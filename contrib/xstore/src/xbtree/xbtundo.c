/* -------------------------------------------------------------------------
 *
 * xbtundo.c
 * undo rollback action for xbtree
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 * src/xbtree/xbtundo.c
 * -------------------------------------------------------------------------
 */

#include "undo/undotype.h"
#include "undo/undorecord.h"
#include "undo/undorequest.h"
#include "undo/undofetch.h"
#include "xbtree/xbtundo.h"
#include "xbtree/xbtxlog.h"
#include "xbtree/xbtree.h"
#include "xbtree/xbttup.h"
#include "utils/relcache.h"
#include "storage/bufmgr.h"
#include "access/xloginsert.h"
#include "util/xxact.h"
#include "util/xrmgr.h"
#include "xstore.h"

/* This is used to write WAL for xbtree undo actions */
typedef struct XBTreeUndoActionWALInfo
{
    uint8         flags;
	Buffer		  buffer;
	OffsetNumber  xlog_lp_offset;
	FullTransactionId modified_xid;
    UndoRecPtr urec;

	FullTransactionId pd_prune_xid;
	uint16		  pd_flags;
	FullTransactionId last_delete_xid;
    int16 active_tuple_count;
} XBTreeUndoActionWALInfo;

static void  log_xbtree_undo_actions(XBTreeUndoActionWALInfo *walInfo, Relation rel);

static int
execute_undo_insert_xbtree(Relation rel, Buffer buffer,UnpackedUndoRecord *urec,Buffer* tarBuffer, Offset* tarOffset);

static int
execute_undo_delete_xbtree(Relation rel, UnpackedUndoRecord *undorecord, Buffer* tarBuffer, Offset* tarOffset);

static void
log_xbtree_undo_actions(XBTreeUndoActionWALInfo *walInfo, Relation rel)
{
	Page				 page = BufferGetPage(walInfo->buffer);
	XLogRecPtr			 recptr = InvalidXLogRecPtr;

	XLogBeginInsert();
	XLogRegisterData((char *) &walInfo->flags, sizeof(uint8));

	/* Check the fields written to the xlog in advance */
	if (walInfo->xlog_lp_offset <= (OffsetNumber) InvalidOffsetNumber ||
		walInfo->xlog_lp_offset >= (OffsetNumber) MaxOffsetNumber)
		ereport(PANIC, (errmsg("Invalid lp offsetnumber, %u.",
								walInfo->xlog_lp_offset)));

	/* Store updated line pointers data */
	XLogRegisterData((char *) &walInfo->xlog_lp_offset, sizeof(OffsetNumber));

	/* Store indextuple sub data */
	XLogRegisterData((char *) &walInfo->modified_xid, sizeof(FullTransactionId));
	XLogRegisterData((char *) &walInfo->urec, sizeof(UndoRecPtr));

	/* Store updated page headers */
	XLogRegisterData((char *) &walInfo->pd_flags, sizeof(uint16));
	XLogRegisterData((char *) &walInfo->pd_prune_xid, sizeof(FullTransactionId));
	XLogRegisterData((char *) &walInfo->last_delete_xid, sizeof(FullTransactionId));
	XLogRegisterData((char *) &walInfo->active_tuple_count, sizeof(int16));

	XLogRegisterBuffer(0, walInfo->buffer, REGBUF_STANDARD);

	recptr = XLogInsert(RM_XBTREE_ID, XLOG_XBTREE_UNDO);

	PageSetLSN(page, recptr);
}


static int
execute_undo_insert_xbtree(Relation rel, Buffer buffer, UnpackedUndoRecord *urec,
			 Buffer* tar_buffer, Offset* tar_offset)
{
	Page                    page;
	ItemId				   	item;
	IndexTuple			   	itup;
	XBTreeIndexTuple		xbt_tuple;
	XBTPageOpaqueInternal   opaque;
	BTScanInsert            itupKey;
	int                     undo_result = ROLLBACK_ROK;
	bool                    has_match_tid = false;

	StringInfoData *index_tuple_data = GetUndoRecordRawdata(urec);
	// raw data is a origin indexTuple 
	itup = (IndexTuple) index_tuple_data->data;
	Assert(index_tuple_data->len > 0);

	/* need an insertion scan key to do our search */
	itupKey = _xbt_mkscankey(rel, itup);

	/* find the first page may containing this key */
	(void) _xbt_search(rel, itupKey, tar_buffer, BT_WRITE, false);

	/* looking for the first item >= scankey */
	*tar_offset = _xbt_binsrch(rel, itupKey, *tar_buffer);

	for (;;)
	{
		/* looking for itup with the same key, tid */
		*tar_offset = _xbt_findsameindexloc(rel, tar_buffer, *tar_offset, 
	  		itupKey, itup); 
		
			// not more data
		if (*tar_offset == InvalidOffsetNumber)
			break;

		if (!has_match_tid)
			has_match_tid = true;
		else 
			elog(INFO,"get another match with the same tid ");

		page = BufferGetPage(*tar_buffer);
		item = PageGetItemId(page, *tar_offset);

		if (IndexItemIdIsDeleted(item) || !ItemIdHasStorage(item))
		{
			// try next
			*tar_offset += 1; 
			continue;
		}

		itup = (IndexTuple) PageGetItem(page, item);
	    xbt_tuple = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);

		// not the target xid
		if (!FullTransactionIdEquals(xbt_tuple->modified_xid, GetUndoRecordXid(urec)))
		{
			// try next 
			*tar_offset += 1; 
			continue;
		}

		// switch undolog ,wait other undolog do first
		if (xbt_tuple->urec != urec->uur_urp)
		{
			// when switch undolog : new zoneid always > old zoneid.
			if (urec->uur_urp < xbt_tuple->urec)
				undo_result = ROLLBACK_RSWITCH;
			break;
		}
		
	    // set rowPtr info  , next prune will clean up this item.
		ItemIdMarkDead(item);

		elog(DEBUG2,"rollback blkno %d offset %d, xid %ld",BufferGetBlockNumber(*tar_buffer),
		*tar_offset, xbt_tuple->modified_xid.value);

		xbt_tuple->modified_xid = InvalidFullTransactionId;
		xbt_tuple->urec = INVALID_UNDO_REC_PTR;

		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

		/* update active hint */
		opaque->active_count--;
		if (FullTransactionIdPrecedes(opaque->last_delete_xid, GetUndoRecordXid(urec)))
			opaque->last_delete_xid = GetUndoRecordXid(urec);

		if(opaque->active_count == 0)
		{
			xbtree_record_empty_page(rel, BufferGetBlockNumber(*tar_buffer), opaque->last_delete_xid);
		}

		//  write wal.
		if(*tar_buffer != InvalidBuffer)
		{
			MarkBufferDirty(*tar_buffer);
			// new wal for xbtree rollback.
			if (RelationNeedsWAL(rel))
			{
				XBTreeUndoActionWALInfo wal_info;
				PageHeader	  phdr = (PageHeader ) page;

				wal_info.flags = XBTREEUNDO_INSERT;
				wal_info.buffer = *tar_buffer;
				wal_info.xlog_lp_offset = *tar_offset;
				wal_info.modified_xid = xbt_tuple->modified_xid;
				wal_info.urec = xbt_tuple->urec;

				/*
				* NOTE: If more page headers are updated by rollback,
				* they should be added to this list
				*/
				wal_info.pd_flags = phdr->pd_flags;
				wal_info.pd_prune_xid = opaque->pd_prune_xid;
				wal_info.last_delete_xid = opaque->last_delete_xid;
				wal_info.active_tuple_count = opaque->active_count;

				log_xbtree_undo_actions(&wal_info, rel);
			}
		}
		undo_result = ROLLBACK_ROK;
		*tar_offset += 1 ; 
	}
    return undo_result;	
}

static int
execute_undo_delete_xbtree(Relation rel, UnpackedUndoRecord *undorecord,
			 Buffer* tarBuffer, Offset* tarOffset)
{
	// check indexTuple 
	Page                    page ;
	ItemId				   	item ;
	IndexTuple			   	itup ;
    BTScanInsert            itupKey;
	XBTreeIndexTuple		xbt_tuple;
	XBTPageOpaqueInternal   opaque;
	int                     undo_result = ROLLBACK_ROK;

	StringInfoData *index_tuple_data = GetUndoRecordRawdata(undorecord);
	// raw data is a origin indexTuple 
	itup = (IndexTuple) index_tuple_data->data;
	Assert(index_tuple_data->len > 0);

	/* need an insertion scan key to do our search */
	itupKey = _xbt_mkscankey(rel, itup);

	/* find the first page may containing this key */
	(void) _xbt_search(rel, itupKey, tarBuffer, BT_WRITE, false);

	/* looking for the first item >= scankey */
	*tarOffset = _xbt_binsrch(rel, itupKey, *tarBuffer);

	for (;;)
	{
		/* looking for itup with the same key, tid */
		*tarOffset = _xbt_findsameindexloc(rel, tarBuffer, *tarOffset, 
		itupKey, itup);

		// not more data
		if (*tarOffset == InvalidOffsetNumber)
			break;

		page = BufferGetPage(*tarBuffer);
		item = PageGetItemId(page, *tarOffset);
		if (!IndexItemIdIsDeleted(item))
		{
			// try next
			*tarOffset += 1; 
			continue;
		}
		itup = (IndexTuple) PageGetItem(page, item);
		xbt_tuple = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);

		// already done
		if (!FullTransactionIdEquals(xbt_tuple->modified_xid,GetUndoRecordXid(undorecord)))
		{
			// try next
			*tarOffset += 1; 
			continue;
		}

		Assert(xbt_tuple->urec==undorecord->uur_urp);
		// find target
		ItemIdSetNormal(item,item->lp_off,item->lp_len); // lg_off, lp_len not change

		// set indexTuple flag.
		xbt_tuple->modified_xid = GetUndoRecordOldXactId(undorecord);
		xbt_tuple->urec = GetUndoRecordTpprev(undorecord);

		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		/* update active hint */
		opaque->active_count++;

		// write wal.
		if(*tarBuffer != InvalidBuffer)
		{
			MarkBufferDirty(*tarBuffer);
			// new wal for xbtree rollback.
			if (RelationNeedsWAL(rel))
			{
				XBTreeUndoActionWALInfo wal_info;
				PageHeader	  phdr = (PageHeader ) page;

				wal_info.flags = XBTREEUNDO_DELETE;
				wal_info.buffer = *tarBuffer;
				wal_info.xlog_lp_offset = *tarOffset;
				wal_info.modified_xid = xbt_tuple->modified_xid;
				wal_info.urec = xbt_tuple->urec;

				/*
				* NOTE: If more page headers are updated by rollback,
				* they should be added to this list
				*/
				wal_info.pd_flags = phdr->pd_flags;
				wal_info.pd_prune_xid = opaque->pd_prune_xid;
				wal_info.last_delete_xid = opaque->last_delete_xid;
				wal_info.active_tuple_count = opaque->active_count;

				log_xbtree_undo_actions(&wal_info, rel);
			}
		}
		undo_result = ROLLBACK_ROK;
		*tarOffset += 1 ; 
	}
	return undo_result;
}

int
execute_undo_delete_xbtree_page(Relation rel, UnpackedUndoRecord *undorecord, Buffer buf, Page page, 
			 Offset offnum)
{
	XBTPageOpaqueInternal   opaque;
	XBTreeIndexTuple xbt_tuple;
	IndexTuple itup;
	ItemId item = PageGetItemId(page, offnum);
	itup = (IndexTuple) PageGetItem(page, item);
	xbt_tuple = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);

	// already done
	if(!FullTransactionIdEquals(xbt_tuple->modified_xid, GetUndoRecordXid(undorecord)) ) 
		return ROLLBACK_ROK;

	Assert(xbt_tuple->urec==undorecord->uur_urp);

	ItemIdSetNormal(item, item->lp_off, item->lp_len); // lg_off, lp_len not change

	// set indexTuple flag.
	xbt_tuple->modified_xid = GetUndoRecordOldXactId(undorecord);
	xbt_tuple->urec = GetUndoRecordTpprev(undorecord);

	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	/* update active hint */
	opaque->active_count++;

	MarkBufferDirty(buf);
	// new wal for xbtree rollback.
	if (RelationNeedsWAL(rel))
	{
			XBTreeUndoActionWALInfo wal_info;
			PageHeader	  phdr = (PageHeader ) page;

			wal_info.flags = XBTREEUNDO_DELETE;
			wal_info.buffer = buf;
			wal_info.xlog_lp_offset = offnum;
			wal_info.modified_xid = xbt_tuple->modified_xid;
			wal_info.urec = xbt_tuple->urec;

			/*
			* NOTE: If more page headers are updated by rollback,
			* they should be added to this list
			*/
			wal_info.pd_flags = phdr->pd_flags;
			wal_info.pd_prune_xid = opaque->pd_prune_xid;
			wal_info.last_delete_xid = opaque->last_delete_xid;
			wal_info.active_tuple_count = opaque->active_count;

			log_xbtree_undo_actions(&wal_info, rel);
	}

	return ROLLBACK_ROK;
}

int
execute_xbtree_undoactions(UndoList *ulist, int start_idx, int end_idx,
				   BlockNumber blkno,   Relation relation)
{
	Buffer			  buffer = InvalidBuffer;
	bool              wait_switch_zone =false;
	int 			  i;


	for (i = start_idx; i <= end_idx; i++)
	{
		UnpackedUndoRecord			   *undorecord = get_urec_from_list(ulist, i);
		uint8					undotype = GetUndoRecordUtype(undorecord);
		FullTransactionId oldest_xid PG_USED_FOR_ASSERTS_ONLY;

		oldest_xid.value = pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid);

		/* Either globalRecycleXid is zero or it is always <= than aborting xid */
	Assert(!FullTransactionIdIsValid(oldest_xid) ||
		   FullTransactionIdPrecedesOrEquals(oldest_xid,
			   GetUndoRecordXid(undorecord)));

	switch (undotype)
	{
		case UNDO_XBTREE_INSERT:
		{
			int           undo_res;
			Buffer        target_buf = InvalidBuffer;
			Offset        target_offset = InvalidOffsetNumber;
			undo_res = execute_undo_insert_xbtree(
								 relation, 
								 buffer,
								 undorecord, &target_buf, &target_offset);

			if (target_buf != InvalidBuffer)
				_bt_relbuf(relation,target_buf);

			if (undo_res == ROLLBACK_RSWITCH) 
				wait_switch_zone = true;
			break;
		}
		case UNDO_XBTREE_DELETE:
		{
			int           undo_res;
			Buffer        target_buf = InvalidBuffer;
			Offset        target_offset = InvalidOffsetNumber;

			undo_res = execute_undo_delete_xbtree(relation, undorecord, 
				&target_buf, &target_offset);
			if (target_buf != InvalidBuffer)
				_bt_relbuf(relation,target_buf);

			if (undo_res == ROLLBACK_RSWITCH) 
				wait_switch_zone = true;
			break;
		}
			case UNDO_INSERT:
			case UNDO_MULTI_INSERT:
			case UNDO_DELETE:
			case UNDO_UPDATE:
			case UNDO_INPLACE_UPDATE:
			{
				ereport(PANIC, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								errmsg("Unsupported Rollback Action for xbtree")));
				break;
			}				
			case UNDO_ITEMID_UNUSED:
			default:
				ereport(PANIC, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								errmsg("Unsupported Rollback Action")));
		}
	}


	
	if (wait_switch_zone)
		return ROLLBACK_RSWITCH;

	return ROLLBACK_ROK;
}

UndoRecPtr
_xbt_prepare_undo_insert(Oid relOid, Oid relfilenode, Oid tablespace,
						  UndoPersistence persistence, FullTransactionId xid, CommandId cid, IndexTuple itup,
						  UndoRecPtr prevurpInOneTup, UndoRecPtr prevurpInOneXact,
						  BlockNumber blk, XLogReaderState *xlog_record, xl_undo_header *xlundohdr,
						  xl_undo_meta *xlundometa, FullTransactionId subXid)
{
	int			   status = 0;
	UndoRecPtr	   urecptr;
	UndoPrepareBuffers *xrecvec = undo_cache_ctx->undo_prepare_buffers;
	UnpackedUndoRecord	  *urec = prepare_buffers_get_undorecord(xrecvec, 0);
	Size		   pay_load_len;
	MemoryContext  old_cxt;
	Assert(tablespace != InvalidOid);
	SetUndoRecordUtype(urec, UNDO_XBTREE_INSERT);
	SetUndoRecordUinfo(urec, UREC_INFO_PAYLOAD);
	SetUndoRecordXid(urec, xid);
	SetUndoRecordCid(urec, cid);
	if(TransactionIdIsValid(subXid.value))
	{
		SetUndoRecordUinfo(urec, UREC_INFO_SUBXACT);
		SetUndoRecordSubXid(urec, subXid);
	}
	SetUndoRecordReloid(urec, relOid);
	SetUndoRecordTpprev(urec, prevurpInOneTup);
	SetUndoRecordRelfilenode(urec, relfilenode);
	SetUndoRecordTablespace(urec, tablespace);
	SetUndoRecordBlkno(urec, blk);
	SetUndoRecordOffset(urec, InvalidOffsetNumber);
	SetUndoRecordPrevurp(urec, InRecovery ? prevurpInOneXact
										  : get_current_tansaction_undorec_ptr(persistence));
	urec->is_update = true;
	
	/* Tell Undo chain traversal this record does not have any older version */
	SetUndoRecordOldXactId(urec, InvalidFullTransactionId);
	
	/* Tell the Undo subsystem how much rawdata we need */
	pay_load_len = XstoreIndexTupleSize(itup);
	if (!InRecovery)
		pay_load_len -= (sizeof(FullTransactionId) + sizeof(UndoRecPtr));
	GetUndoRecordRawdata(urec)->len = pay_load_len;

	status = prepare_undo(xrecvec, persistence, xlog_record, xlundohdr, xlundometa);
	/* Do not continue if there was a failure during Undo preparation */
	if (status != UNDO_PREPARE_SUCC)
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("Failed to generate UnpackedUndoRecord")));

	urecptr = urec->uur_urp;
	Assert(IS_VALID_UNDO_REC_PTR(urecptr));

	old_cxt = MemoryContextSwitchTo(urec->mem_ctx);
	initStringInfo(GetUndoRecordRawdata(urec));
	MemoryContextSwitchTo(old_cxt);

	appendBinaryStringInfo(GetUndoRecordRawdata(urec),
							   (char *) itup,pay_load_len);
	
	return urecptr;
}

UndoRecPtr
_xbt_prepare_undo_delete(Oid relOid, Oid relfilenode, Oid tablespace,
						  UndoPersistence persistence, OffsetNumber offnum, FullTransactionId xid, 
						  CommandId cid, IndexTuple itup, UndoRecPtr prevurpInOneTup, 
						  UndoRecPtr prevurpInOneXact, FullTransactionId xactid,
						  BlockNumber blk, XLogReaderState *xlog_record, xl_undo_header *xlundohdr, 
						  xl_undo_meta *xlundometa, FullTransactionId subXid)
{
	int			   status = 0;
	Size           payload_len = 0;
	UndoRecPtr	   urecptr;
	UndoPrepareBuffers *xrecvec = undo_cache_ctx->undo_prepare_buffers;
	UnpackedUndoRecord	  *urec = prepare_buffers_get_undorecord(xrecvec, 0);
	MemoryContext  old_cxt;

	Assert(tablespace != InvalidOid);
	SetUndoRecordUtype(urec, UNDO_XBTREE_DELETE);
	SetUndoRecordUinfo(urec, UREC_INFO_PAYLOAD);
	SetUndoRecordXid(urec, xid);
	SetUndoRecordCid(urec, cid);
	if(TransactionIdIsValid(subXid.value))
	{
		SetUndoRecordUinfo(urec, UREC_INFO_SUBXACT);
		SetUndoRecordSubXid(urec, subXid);
	}
	SetUndoRecordReloid(urec, relOid);
	SetUndoRecordTpprev(urec, prevurpInOneTup);
	SetUndoRecordRelfilenode(urec, relfilenode);
	SetUndoRecordTablespace(urec, tablespace);
	SetUndoRecordBlkno(urec, blk);
	SetUndoRecordOffset(urec, offnum);
	SetUndoRecordPrevurp(urec, InRecovery ? prevurpInOneXact
										  : get_current_tansaction_undorec_ptr(persistence));
	urec->is_update = true;
	SetUndoRecordOldXactId(urec, xactid);

	/* Tell the Undo subsystem how much rawdata we need */
	payload_len = XstoreIndexTupleSize(itup);
	GetUndoRecordRawdata(urec)->len = payload_len;

	status = prepare_undo(xrecvec, persistence, xlog_record, xlundohdr, xlundometa);
	if (status != UNDO_PREPARE_SUCC)
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("Failed to generate UnpackedUndoRecord")));

	urecptr = urec->uur_urp;
	Assert(IS_VALID_UNDO_REC_PTR(urecptr));

	old_cxt = MemoryContextSwitchTo(urec->mem_ctx);
	initStringInfo(GetUndoRecordRawdata(urec));
	MemoryContextSwitchTo(old_cxt);

	appendBinaryStringInfo(GetUndoRecordRawdata(urec),
							   (char *) itup,payload_len);

	return urecptr;
}