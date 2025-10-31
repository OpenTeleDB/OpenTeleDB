/* -------------------------------------------------------------------------
 *
 * xredo.c
 * WAL replay logic for xheap.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * src/xheap/xredo.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "c.h"
#include "miscadmin.h"
#include "access/xlogreader.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "catalog/pg_tablespace.h"
#include "storage/bufpage.h"
#include "storage/off.h"
#include "storage/proc.h"
#include "storage/sinvaladt.h"
#include "storage/standby.h"
#include "storage/procarray.h"
#include "undo/undotype.h"
#include "xheap/xredo.h"
#include "xheap/xpage.h"
#include "undo/undorequest.h"
#include "xheap/xtuple.h"
#include "xstore.h"
#include "access/xlogutils.h"
#include "xheap/xheap.h"
#include "undo/undoxlog.h"
#include "undo/undofetch.h"
#include "storage/freespace.h"
#include "undo/undolog.h"
#include "xheap/xheapundo.h"
#include "utils/wait_event.h"
#include <sys/types.h>

static const int FREESPACE_FRACTION = 5;


typedef union
{
	XHeapDiskTupleData hdr;
	char			   data[MaxPossibleXHeapTupleSize];
} TupleBuffer;

typedef struct
{
	Buffer oldbuffer;
	Buffer newbuffer;
} UpdateRedoBuffers;

typedef struct
{
	XHeapTupleData *oldtup;
	XHeapDiskTuple	newtup;
} UpdateRedoTuples;

typedef struct
{
	uint16 prefixlen;
	uint16 suffixlen;
} UpdateRedoAffixLens;

static XHeapDiskTuple
get_xheap_disk_tuple_from_redo_data(char *data, Size *datalen, TupleBuffer *tbuf,FullTransactionId xid, UndoRecPtr uptr)
{
	XHeapDiskTuple disktup;
	xl_xheap_header  xlhdr;

	memcpy((char *) &xlhdr, data, SizeOfXHeapHeader);
	data += SizeOfXHeapHeader;

	disktup = &tbuf->hdr;
	memset((char *) disktup, 0, SizeOfXHeapDiskTupleData);
	memcpy((char *) disktup + SizeOfXHeapDiskTupleData, data, *datalen);
	*datalen += SizeOfXHeapDiskTupleData;

	disktup->modified_xid = xid; 
	disktup->urec = uptr;
	disktup->flag2 = xlhdr.flag2;
	disktup->flag = xlhdr.flag;
	disktup->t_hoff = xlhdr.t_hoff;
	return disktup;
}

static void
extract_undo_info_for_insert(UndoRecPtr *blkprev, UndoRecPtr *prevurp, xl_undo_meta *undometa, FullTransactionId *xid, char *curr_log_ptr, uint8 undo_flag)
{
	xl_undo_meta *xlundometa;
	FullTransactionId *fullXid;

	*blkprev = INVALID_UNDO_REC_PTR;
	*prevurp = INVALID_UNDO_REC_PTR;
	init_xlog_undo_meta(undometa);

	if ((undo_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
	{
		*blkprev = *((UndoRecPtr *) ((char *) curr_log_ptr));
		curr_log_ptr += sizeof(UndoRecPtr);
	}

	if ((undo_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
	{
		*prevurp = *((UndoRecPtr *) ((char *) curr_log_ptr));
		curr_log_ptr += sizeof(UndoRecPtr);
	}

	// get top xid;
	if ((undo_flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
	{
		fullXid = (FullTransactionId *) curr_log_ptr;
		*xid = *fullXid;
		curr_log_ptr += sizeof(FullTransactionId);
	}

	xlundometa = (xl_undo_meta *) ((char *) curr_log_ptr);

	copy_xlog_undo_meta(xlundometa, undometa);
}

static UndoRecPtr
xheap_xlog_undo_for_insert(XLogReaderState *record, const BlockNumber blkno)
{
	XLogRecPtr		lsn = record->EndRecPtr;
	FullTransactionId xid = XLogRecGetFullXid(record);
	xl_undo_meta	undometa;
	UndoRecPtr		blkprev;
	UndoRecPtr		prevurp;
	UndoRecPtr		urecptr = INVALID_UNDO_REC_PTR;

	xl_xheap_insert *xlrec = (xl_xheap_insert *) XLogRecGetData(record);
	xl_undo_header  *xlundohdr =
		(xl_undo_header *) ((char *) xlrec +
						  SizeOfXHeapInsert); 
	char *curr_log_ptr = ((char *) xlundohdr + SizeOfXLUndoHeader);
	bool		skip_insert;
	RelFileLocator target_locator = {0};
	UnpackedUndoRecord *undorec = NULL;
	
	urecptr = xlundohdr->urecptr;

	extract_undo_info_for_insert(&blkprev, &prevurp, &undometa, &xid, curr_log_ptr, xlundohdr->flag);

	skip_insert = is_skip_insert_undo(urecptr, &undometa);

	if (skip_insert)
		xlog_undo_meta_setinfo(&undometa, XLOG_UNDOMETA_INFO_SKIP);


	XLogRecGetBlockTag(record, 0, &target_locator, NULL, NULL);

	undorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
	undorec->uur_urp = urecptr;
	urecptr = xheap_prepare_undo_insert(
		xlundohdr->relOid, target_locator.relNumber, target_locator.spcOid, 
		UNDO_PERMANENT, xid, 0, blkprev , prevurp, 
		blkno, record, xlundohdr, &undometa);

	ereport(DEBUG2,
			(errmsg("redo:undoptr=%lu, xid %lu, spcoid=%u.",
					urecptr, xid.value, target_locator.spcOid)));

	/* recover undo record */
	Assert(urecptr == xlundohdr->urecptr);
	SetUndoRecordOffset(undorec, xlrec->offnum);
	if (!skip_insert)
		insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, lsn);

	redo_undo_meta(record, &undometa, xlundohdr->urecptr,
					prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
					prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));

	reset_prepared_buffers_in_ctx();

	return urecptr;
}

void
xheap_xlog_insert(XLogReaderState *record)
{
	Buffer			buf;
	RelFileLocator	target_locator;
	BlockNumber		blkno = InvalidBlockNumber;
	XLogRedoAction	action;
	TupleBuffer		tbuf;
	UndoRecPtr		urecptr = INVALID_UNDO_REC_PTR;

	XLogRecGetBlockTag(record, 0, &target_locator, NULL, &blkno);

	urecptr = xheap_xlog_undo_for_insert(record, blkno);

	if (XLogRecGetInfo(record) & XLOG_XHEAP_INIT_PAGE)
	{
		buf = XLogInitBufferForRedo(record, 0);

		xpage_init(XPAGE_HEAP, BufferGetPage(buf), BufferGetPageSize(buf),
					XHEAP_SPECIAL_SIZE);

		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, &buf);

	if (action == BLK_NEEDS_REDO)
	{
		FullTransactionId	xid = XLogRecGetFullXid(record);
		xl_xheap_insert		*xlrec = (xl_xheap_insert *) XLogRecGetData(record);
		Size				datalen;

		XHeapBufferPage		bufpage;
		XHeapDiskTuple		xtup = NULL;
		char			   *data = XLogRecGetBlockData(record, 0, &datalen);
		Size				newlen;
		Page				page;

		if (datalen < SizeOfXHeapHeader)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("Datalen less than SizeOfXHeapHeader")));

		newlen = datalen - SizeOfXHeapHeader;
		page = BufferGetPage(buf);
		if (xheap_page_get_max_offset_number(page) + 1 < xlrec->offnum)
			elog(PANIC, "invalid max offset number");


		Assert(datalen > SizeOfXHeapHeader && newlen <= MaxPossibleXHeapTupleSize);
		xtup = get_xheap_disk_tuple_from_redo_data(data, &newlen, &tbuf, xid, urecptr);

		bufpage.buffer = buf;
		bufpage.page = NULL;
		if (xpage_add_item(NULL, &bufpage, (Item) xtup, newlen, xlrec->offnum, true) ==
			InvalidOffsetNumber)
			elog(PANIC, "failed to add tuple");

		xheap_record_potential_free_space(buf, -1 * SHORTALIGN(newlen));

		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buf);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void 
extract_undo_info_for_delete(UndoRecPtr *blkprev, UndoRecPtr *prevurp,
							xl_undo_meta *undometa, FullTransactionId *xid,
							FullTransactionId *sub_xid, char *curr_log_ptr,
							uint8 undo_flag, uint32 *read_size)
{
	xl_undo_meta *xlundometa;
	uint32       readcnt = 0;

	*blkprev = INVALID_UNDO_REC_PTR;
	*prevurp = INVALID_UNDO_REC_PTR;
	init_xlog_undo_meta(undometa);

	if ((undo_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
	{
		*blkprev = *((UndoRecPtr *) ((char *) curr_log_ptr));
		curr_log_ptr += sizeof(UndoRecPtr);
		readcnt += sizeof(UndoRecPtr);
	}

	if ((undo_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
	{
		*prevurp = *((UndoRecPtr *) ((char *) curr_log_ptr));
		curr_log_ptr += sizeof(UndoRecPtr);
		readcnt += sizeof(UndoRecPtr);
	}

	if ((undo_flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
	{
		FullTransactionId *top_xid = (FullTransactionId *) curr_log_ptr;
		curr_log_ptr += sizeof(FullTransactionId);
		readcnt += sizeof(FullTransactionId);
		*sub_xid = *xid;
		*xid = *top_xid;
	}

	if ((undo_flag & XLOG_UNDO_HEADER_HAS_TOAST) != 0)
	{
		uint32 toastLen = *(uint32 *) curr_log_ptr;
		curr_log_ptr += sizeof(toastLen) + toastLen;
		readcnt += sizeof(uint32) + toastLen;
	}

	xlundometa = (xl_undo_meta *) ((char *) curr_log_ptr);

	copy_xlog_undo_meta(xlundometa, undometa);

	readcnt += xlog_undo_meta_size(undometa);

	*read_size = readcnt;
}

static UndoRecPtr
xheap_xlog_undo_for_delete(XLogReaderState *record, XHeapTupleData *xtup,
						  const BlockNumber blkno, TupleBuffer *tbuf)
{
	XLogRecPtr		lsn = record->EndRecPtr;
	FullTransactionId xid = XLogRecGetFullXid(record);
	FullTransactionId sub_full_xid = InvalidFullTransactionId;
	Size			recordlen = XLogRecGetDataLen(record);
	xl_undo_meta	undometa;

	UndoRecPtr	blkprev;
	UndoRecPtr	prevurp;
	uint32		read_size = 0;
	UndoRecPtr	urecptr = INVALID_UNDO_REC_PTR;
	Size		datalen;
	char	   *data = NULL;
	bool		skip_insert;
	RelFileLocator target_locator = {0};
	UnpackedUndoRecord *undorec = NULL;

	xl_xheap_delete *xlrec = (xl_xheap_delete *) XLogRecGetData(record);
	xl_undo_header  *xlundohdr =
		(xl_undo_header *) ((char *) xlrec +
						  SizeOfXHeapDelete);  
	char *currLogPtr = ((char *) xlundohdr + SizeOfXLUndoHeader);

	urecptr = xlundohdr->urecptr;

	extract_undo_info_for_delete(&blkprev, &prevurp, &undometa, &xid, &sub_full_xid, currLogPtr,
								 xlundohdr->flag, &read_size);

	datalen = recordlen - SizeOfXLUndoHeader - SizeOfXHeapDelete -
			  SizeOfXHeapHeader - read_size;	 
	data = (char *) xlrec + SizeOfXHeapDelete + SizeOfXLUndoHeader + read_size;

	xtup->disk_tuple = get_xheap_disk_tuple_from_redo_data(data, &datalen, tbuf, xid, urecptr);
	xtup->disk_tuple_size = datalen;

	skip_insert = is_skip_insert_undo(urecptr, &undometa);
	if (skip_insert)
		xlog_undo_meta_setinfo(&undometa, XLOG_UNDOMETA_INFO_SKIP);

	undorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
	undorec->uur_urp = urecptr;


	XLogRecGetBlockTag(record, 0, &target_locator, NULL, NULL);

	urecptr = xheap_prepare_undo_delete(
		xlundohdr->relOid,  target_locator.relNumber, target_locator.spcOid, UNDO_PERMANENT, InvalidBuffer, xlrec->offnum, xid,
		sub_full_xid, 0, blkprev,
		prevurp,xlrec->oldxid,  xtup, blkno, 
		record, xlundohdr, &undometa);
	Assert(urecptr == xlundohdr->urecptr);
	SetUndoRecordOffset(undorec, xlrec->offnum);

	if (!skip_insert)
		insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, lsn);

	redo_undo_meta(record, &undometa, xlundohdr->urecptr,
					prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
					prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));

	reset_prepared_buffers_in_ctx();

	return urecptr;
}

static void
xheap_xlog_delete(XLogReaderState *record)
{
	Buffer			buf;
	XHeapTupleData	xtup;
	RelFileLocator	target_locator;
	BlockNumber		blkno = InvalidBlockNumber;
	ItemPointerData target_tid;
	XLogRedoAction	action;
	TupleBuffer		tbuf;
	xl_xheap_delete *xlrec = (xl_xheap_delete *) XLogRecGetData(record);
	xl_undo_header *xlundohdr = (xl_undo_header *) ((char *) xlrec + SizeOfXHeapDelete);
	UndoRecPtr		urecptr = INVALID_UNDO_REC_PTR;

	XLogRecGetBlockTag(record, 0, &target_locator, NULL, &blkno);
	ItemPointerSetBlockNumber(&target_tid, blkno);
	ItemPointerSetOffsetNumber(&target_tid, xlrec->offnum);

	xtup.table_oid = xlundohdr->relOid;
	xtup.ctid = target_tid;

	urecptr = xheap_xlog_undo_for_delete(record, &xtup, blkno, &tbuf);

	action = XLogReadBufferForRedo(record, 0, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		XLogRecPtr		lsn = record->EndRecPtr;
		FullTransactionId xid = XLogRecGetFullXid(record);
		Size			datalen = xtup.disk_tuple_size;
		Page			page = BufferGetPage(buf);
		RowPtr		   *rp;

		xlrec = (xl_xheap_delete *) XLogRecGetData(record);

		if (xheap_page_get_max_offset_number(page) >= xlrec->offnum)
			rp = XPageGetRowPtr(page, xlrec->offnum);
		else
			elog(PANIC, "invalid rp");

		/* increment the potential freespace of this page */
		xheap_record_potential_free_space(buf, SHORTALIGN(datalen));

		xtup.disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);
		xtup.disk_tuple_size = RowPtrGetLen(rp);
		xtup.disk_tuple->modified_xid = xid;
		xtup.disk_tuple->urec = urecptr;
		xtup.disk_tuple->flag = xlrec->flag;

		/* Mark the page as a candidate for pruning */
		XPageSetPrunable(page, xid);

		PageSetLSN(page, lsn);
		MarkBufferDirty(buf);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
xheap_xlog_lock(XLogReaderState *record)
{
	Buffer			  buf;
	XLogRedoAction	  action;
	RelFileLocator		  target_locator;
	BlockNumber		  blkno = InvalidBlockNumber;
	Page              page;
	OffsetNumber      offnum;
	XlXHeapLock       *xlrec;
	RowPtr			  *rp;
	XHeapDiskTuple	  disk_tup;
	XLogRecPtr	  	  lsn = record->EndRecPtr;

	xlrec = (XlXHeapLock *)XLogRecGetData(record);
	XLogRecGetBlockTag(record, 0, &target_locator, NULL, &blkno);

	action = XLogReadBufferForRedo(record, 0, &buf);
	if (action == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buf);
		offnum = xlrec->offnum;
		if (xheap_page_get_max_offset_number(page) >= offnum)
			rp = XPageGetRowPtr(page, offnum);
		else
			elog(PANIC, "invalid rp");

		disk_tup = (XHeapDiskTuple)XPageGetRowData(page, rp);
		disk_tup->flag &= ~XHEAP_LOCK_STATUS_MASK;
		XHeapTupleHeaderClearSingleLocker(disk_tup);
		disk_tup->flag |= xlrec->infomask;
		disk_tup->locker_xid = xlrec->locker_xid;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buf);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
xheap_xlog_clean(XLogReaderState *record)
{
	xl_xheap_clean	 *xlrec = (xl_xheap_clean *) XLogRecGetData(record);
	Buffer			  buf;
	Size			  freespace = 0;
	RelFileLocator	  rlocator = ((RelFileLocator){0, 0, 0});
	BlockNumber		  blkno = InvalidBlockNumber;
	XLogRedoAction	  action;

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &blkno);

	/*
     * We're about to remove tuples. In Hot Standby mode, ensure that there's
     * no queries running for which the removed tuples are still visible.
     *
     * Not all INPLACEHEAP_CLEAN records remove tuples with xids, so we only want to
     * conflict on the records that cause MVCC failures for user queries. If
     * latestRemovedXid is invalid, skip conflict processing.
     */
	if (InHotStandby && TransactionIdIsValid(xlrec->latest_removed_xid.value))
		ResolveRecoveryConflictWithSnapshot(xlrec->latest_removed_xid.value, 
								false, rlocator);

	/*
     * If we have a full-page image, restore it (using a cleanup lock) and
     * we're done.
     */
	action = XLogReadBufferForRedo(record, 0, &buf);

	if (action == BLK_NEEDS_REDO)
	{
		XLogRecPtr	  lsn = record->EndRecPtr;
		Page		  page = BufferGetPage(buf);
		Size		  datalen;
		XPruneState	  prstate;
		int			  nunused = xlrec->nunused;
		int			  nfixed = xlrec->nfixed;
		OffsetNumber *nowunused = (OffsetNumber *) XLogRecGetBlockData(record, 0, &datalen);
		OffsetNumber *nowfixed = (OffsetNumber *) nowunused + nunused;
		OffsetNumber *target_offnum;
		OffsetNumber  tmp_target_off;
		OffsetNumber *offnum;
		Size		 *space_required;
		Size		  tmp_spc_rqd;
		int			  i;

		/* Update all item pointers per the record, and repair fragmentation */
		if (xlrec->flags & XLZ_CLEAN_CONTAINS_OFFSET)
		{
			target_offnum = (OffsetNumber *) ((char *) xlrec + SizeOfXHeapClean);
			space_required = (Size *) ((char *) target_offnum + sizeof(OffsetNumber));
		}
		else
		{
			target_offnum = &tmp_target_off;
			*target_offnum = InvalidOffsetNumber;
			space_required = &tmp_spc_rqd;
			*space_required = 0;
		}

		offnum = nowunused;
		for (i = 0; i < nunused; i++)
			prstate.nowunused[i] = *offnum++;

		offnum = nowfixed;
		for (i = 0; i < nfixed; i++)
			prstate.nowfixed[i] = *offnum++;

		offnum = nowfixed + nfixed;
		for (i = 0; i < nfixed; i++)
			prstate.fixedlen[i] = *offnum++;

		prstate.nunused = nunused;
		prstate.nfixed = nfixed;
		xheap_page_prune_execute(buf, *target_offnum, &prstate);

		if (xlrec->flags & XLZ_CLEAN_ALLOW_PRUNING)
		{
			bool pruned PG_USED_FOR_ASSERTS_ONLY = false;

			xheap_page_repaire_fregmentation(NULL, buf, *target_offnum, *space_required, &pruned);

			/*
			* Pruning must be successful at redo time, otherwise the page
			* contents on master and standby might differ.
			*/
			Assert(pruned);
		}

		freespace = page_get_xheap_free_space(page); /* needed to update FSM
												* below */

		/*
		* Note: we don't worry about updating the page's prunability hints.
		* At worst this will cause an extra prune cycle to occur soon.
		*/

		PageSetLSN(page, lsn);
		MarkBufferDirty(buf);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	/*
     * Update the FSM as well.
     *
     * XXX: Don't do this if the page was restored from full page image. We
     * don't bother to update the FSM in that case, it doesn't need to be
     * totally accurate anyway.
     */
	if (action == BLK_NEEDS_REDO)
		XLogRecordPageWithFreeSpace(rlocator, blkno, freespace);
}

static void
get_affix_lens_from_update_redo(char *curxlogptr, XLogReaderState *record,
						   UpdateRedoAffixLens *affixLens)
{
	xl_xheap_update *xlrec = (xl_xheap_update *) XLogRecGetData(record);

	if (xlrec->flags & XLZ_NON_INPLACE_UPDATE)
		return;
	else
	{
		int	  *undo_xor_delta_size_ptr = (int *) curxlogptr;
		int	   undo_xor_delta_size = *undo_xor_delta_size_ptr;
		char  *xor_curxlogptr = NULL;
		uint8 *t_hoff_ptr = NULL;
		uint8  tHoff;
		uint8 *flags_ptr = NULL;
		uint8  flags;
		curxlogptr += sizeof(int);

		xor_curxlogptr = curxlogptr;
		curxlogptr += undo_xor_delta_size;

		t_hoff_ptr = (uint8 *) xor_curxlogptr;
		tHoff = *t_hoff_ptr;

		xor_curxlogptr += sizeof(uint8) + tHoff - OffsetDataHeader;

		flags_ptr = (uint8 *) xor_curxlogptr;
		flags = *flags_ptr;
		xor_curxlogptr += sizeof(uint8);

		if (flags & UREC_XOR_PREFIX)
		{
			uint16 *prefixlen_ptr = (uint16 *) (xor_curxlogptr);
			xor_curxlogptr += sizeof(uint16);
			affixLens->prefixlen = *prefixlen_ptr;
		}
		if (flags & UREC_XOR_SUFFIX)
		{
			uint16 *suffixlen_ptr = (uint16 *) (xor_curxlogptr);
			xor_curxlogptr += sizeof(uint16);
			affixLens->suffixlen = *suffixlen_ptr;
		}
	}
}

static UndoRecPtr
xheap_xlog_undo_for_update(XLogReaderState *record, XHeapTupleData *oldtup,
						  xl_undo_header **xlnewundohdr, char **undo_xor_delta_size_ptr,
						  TupleBuffer *tbuf)
{
	XLogRecPtr		lsn = record->EndRecPtr;
	Size			recordlen = XLogRecGetDataLen(record);
	FullTransactionId xid = XLogRecGetFullXid(record);
	FullTransactionId sub_full_xid = InvalidFullTransactionId;
	FullTransactionId *full_xid;
	xl_undo_meta	undometa;
	UndoRecPtr		new_urec_ptr = INVALID_UNDO_REC_PTR;

	UndoRecPtr *blkprev;
	UndoRecPtr *prevurp;

	UndoRecPtr	invalid_urp = INVALID_UNDO_REC_PTR;
	uint32		read_size = 0;

	int		undo_xor_delta_size = 0;
	bool	inplace_update = true;
	char   *xlog_xor_delta = NULL;

	xl_xheap_update *xlrec = (xl_xheap_update *) XLogRecGetData(record);
	xl_undo_header  *xlundohdr =
		(xl_undo_header *) ((char *) xlrec +
						  SizeOfXHeapUpdate);
	UndoRecPtr		urecptr = xlundohdr->urecptr;
	char		   *curxlogptr = ((char *) xlundohdr) + SizeOfXLUndoHeader;
	xl_undo_meta   *xlundometa = NULL;
	uint32			undo_meta_size;
	BlockNumber		oldblk = InvalidBlockNumber;
	BlockNumber		newblk = InvalidBlockNumber;
	RelFileLocator	rlocator;
	ItemPointerData oldtid, newtid;
	bool			skip_insert;
	FullTransactionId update_xid;
	RelFileLocator	target_locator;
	UnpackedUndoRecord	   *oldundorec = NULL;
	UnpackedUndoRecord	   *newundorec = NULL;
	bool			res PG_USED_FOR_ASSERTS_ONLY;
	UnpackedUndoRecord	   *undorec = NULL;

	init_xlog_undo_meta(&undometa);
	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
	{
		blkprev = (UndoRecPtr *) ((char *) curxlogptr);
		curxlogptr += sizeof(UndoRecPtr);
		read_size += sizeof(UndoRecPtr);
	}
	else
		blkprev = &invalid_urp;

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
	{
		prevurp = (UndoRecPtr *) ((char *) curxlogptr);
		curxlogptr += sizeof(UndoRecPtr);
		read_size += sizeof(UndoRecPtr);
	}
	else
		prevurp = &invalid_urp;

	/* swap topxid and subxid */
	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
	{
		full_xid = (FullTransactionId *) curxlogptr;
		curxlogptr += sizeof(FullTransactionId);
		read_size += sizeof(FullTransactionId);

		sub_full_xid = xid;
		xid = *full_xid;
	}

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_TOAST) != 0)
	{
		uint32 toast_len = *(uint32 *) curxlogptr;
		curxlogptr += sizeof(toast_len) + toast_len;
		read_size += sizeof(toast_len) + toast_len;
	}

	if (xlrec->flags & XLZ_NON_INPLACE_UPDATE)
	{
		*xlnewundohdr = (xl_undo_header *) curxlogptr;
		curxlogptr += SizeOfXLUndoHeader;
		inplace_update = false;

		if (((*xlnewundohdr)->flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
		{
			curxlogptr += sizeof(UndoRecPtr);
			read_size += sizeof(UndoRecPtr);
		}
	}

	xlundometa = (xl_undo_meta *) curxlogptr;
	copy_xlog_undo_meta(xlundometa, &undometa);
	undo_meta_size = xlog_undo_meta_size(&undometa);
	curxlogptr += undo_meta_size;
	*undo_xor_delta_size_ptr = curxlogptr;

	if (inplace_update)
	{
		int *undo_xor_delta_size_ptr = (int *) curxlogptr;
		undo_xor_delta_size = *undo_xor_delta_size_ptr;
		curxlogptr += sizeof(int);
		xlog_xor_delta = curxlogptr;
	}
	else
	{
		Size  init_page_xtra_info = 0;
		char *data = NULL;
		Size  datalen;

		data = (char *) curxlogptr;
		datalen = recordlen - SizeOfXHeapHeader - SizeOfXLUndoHeader - SizeOfXHeapUpdate -
				  undo_meta_size - SizeOfXLUndoHeader - init_page_xtra_info -
				  read_size;	 

		oldtup->disk_tuple = get_xheap_disk_tuple_from_redo_data(data, &datalen, tbuf, xid, urecptr);
		oldtup->disk_tuple_size = datalen;
	}

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &newblk);
	if (XLogRecGetBlockTagExtended(record, 1, NULL, NULL, &oldblk, NULL))
		Assert(!inplace_update);
	else
		oldblk = newblk;

	ItemPointerSet(&oldtid, oldblk, xlrec->old_offnum);
	ItemPointerSet(&newtid, newblk, xlrec->new_offnum);

	oldtup->table_oid = xlundohdr->relOid;
	oldtup->ctid = oldtid;

	skip_insert = is_skip_insert_undo(urecptr, &undometa);
	if (skip_insert)
		xlog_undo_meta_setinfo(&undometa, XLOG_UNDOMETA_INFO_SKIP);

	oldundorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
	oldundorec->uur_urp = urecptr;

	if (!inplace_update)
	{
		newundorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 1);
		newundorec->uur_urp = (*xlnewundohdr)->urecptr;
	}

	update_xid = xlrec->oldxid;
	res = XLogRecGetBlockTagExtended(record, 0, &target_locator, NULL, NULL, NULL);
	Assert(res == true);
	urecptr = xheap_prepare_undo_update(
		xlundohdr->relOid, target_locator.relNumber, target_locator.spcOid, UNDO_PERMANENT,
		InvalidBuffer, InvalidBuffer, xlrec->old_offnum, xid,
		sub_full_xid, 0, *blkprev,
		*prevurp, update_xid, oldtup, inplace_update,
		&new_urec_ptr, undo_xor_delta_size, oldblk, newblk,
		record, xlundohdr, &undometa);
	Assert(urecptr == xlundohdr->urecptr);

	if (!skip_insert)
	{
		if (!inplace_update)
		{
			SetUndoRecordOffset(newundorec, xlrec->new_offnum);
			appendBinaryStringInfo(GetUndoRecordRawdata(oldundorec), (char *) &newtid,
								   sizeof(ItemPointerData));
		}
		else
		{
			undorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
			appendBinaryStringInfo(GetUndoRecordRawdata(undorec), xlog_xor_delta,
								   undo_xor_delta_size);
		}

		insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, lsn);
	}

	redo_undo_meta(record, &undometa, xlundohdr->urecptr,
				 prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
				 prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));

	undorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);

	if (!inplace_update)
		newundorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 1);

	reset_prepared_buffers_in_ctx();

	return urecptr;
}

static XLogRedoAction
get_update_new_redo_action(XLogReaderState *record, UpdateRedoBuffers *buffers,
					   const XLogRedoAction oldaction, const bool same_block)
{
	XLogRedoAction newaction;

	if (same_block)
	{
		buffers->newbuffer = buffers->oldbuffer;
		newaction = oldaction;
	}
	else if (XLogRecGetInfo(record) & XLOG_XHEAP_INIT_PAGE)
	{
		Page newpage;
		Size newpagesize;
		buffers->newbuffer = XLogInitBufferForRedo(record, 0);
		newpage = BufferGetPage(buffers->newbuffer);
		newpagesize = BufferGetPageSize(buffers->newbuffer);
	
		xpage_init(XPAGE_HEAP, newpage, newpagesize, XHEAP_SPECIAL_SIZE);

		newaction = BLK_NEEDS_REDO;
	}
	else
	{
		newaction = XLogReadBufferForRedo(record, 0, &buffers->newbuffer);
	}

	return newaction;
}

static uint32
get_xheap_disk_tuple_from_update_new_redo_data(XLogReaderState *record, UpdateRedoTuples *tuples,
									   UpdateRedoAffixLens *affix_lens, TupleBuffer *tbuf,
									   const bool same_block)
{
	Size		   datalen;
	xl_xheap_header  xlhdr;
	xl_xheap_update *xlrec = (xl_xheap_update *) XLogRecGetData(record);

	char  *recdata = XLogRecGetBlockData(record, 0, &datalen);
	char  *recdata_end = recdata + datalen;
	Size   tuplen;
	char  *newp = NULL;
	uint32 newlen;

	if (xlrec->flags & XLZ_NON_INPLACE_UPDATE)
	{
		if (xlrec->flags & XLZ_UPDATE_PREFIX_FROM_OLD)
		{
			Assert(same_block);
			memcpy(&affix_lens->prefixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}

		if (xlrec->flags & XLZ_UPDATE_SUFFIX_FROM_OLD)
		{
			Assert(same_block);
			memcpy(&affix_lens->suffixlen, recdata, sizeof(uint16));
			recdata += sizeof(uint16);
		}
	}

	memcpy((char *) &xlhdr, recdata, SizeOfXHeapHeader);
	recdata += SizeOfXHeapHeader;

	tuplen = recdata_end - recdata;
	Assert(tuplen <= MaxPossibleXHeapTupleSize);

	tuples->newtup = &tbuf->hdr;
	memset((char *) tuples->newtup, 0, SizeOfXHeapDiskTupleData);

	/*
     * Reconstruct the new tuple using the prefix and/or suffix from the
     * old tuple, and the data stored in the WAL record.
     */
	newp = (char *) tuples->newtup + SizeOfXHeapDiskTupleData;
	if (affix_lens->prefixlen > 0)
	{
		int len;

		/* copy bitmap [+ padding] [+ oid] from WAL record */
		len = xlhdr.t_hoff - SizeOfXHeapDiskTupleData;
		if (len > 0)
		{
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;
		}

		/* copy prefix from old tuple */
		memcpy(newp, (char *) tuples->oldtup->disk_tuple + tuples->oldtup->disk_tuple->t_hoff,
			affix_lens->prefixlen);
		newp += affix_lens->prefixlen;

		/* copy new tuple data from WAL record */
		len = tuplen - (xlhdr.t_hoff - SizeOfXHeapDiskTupleData);
		if (len > 0)
		{
			memcpy(newp, recdata, len);
			recdata += len;
			newp += len;
		}
	}
	else
	{
		memcpy(newp, recdata, tuplen);
		recdata += tuplen;
		newp += tuplen;
	}

	Assert(recdata == recdata_end);

	if (affix_lens->suffixlen > 0)
	{
		memcpy(newp, (char *) tuples->oldtup->disk_tuple +
				tuples->oldtup->disk_tuple_size - affix_lens->suffixlen, affix_lens->suffixlen);
	}

	newlen =
		SizeOfXHeapDiskTupleData + tuplen + affix_lens->prefixlen + affix_lens->suffixlen;
	tuples->newtup->modified_xid = InvalidFullTransactionId;
	tuples->newtup->flag2 = xlhdr.flag2;
	tuples->newtup->flag = xlhdr.flag;
	tuples->newtup->t_hoff = xlhdr.t_hoff;
	return newlen;
}

static void
xheap_xlog_update(XLogReaderState *record)
{
	xl_undo_header	   *xlnewundohdr = NULL;
	UpdateRedoBuffers	buffers;
	RelFileLocator			rlocator = {0};
	BlockNumber			oldblk = InvalidBlockNumber;
	BlockNumber			newblk = InvalidBlockNumber;
	XHeapTupleData		oldtup;
	UpdateRedoTuples	tuples;
	XLogRedoAction		oldaction, newaction;
	TupleBuffer			tbuf;
	UpdateRedoAffixLens affix_lens = {0, 0};
	uint32				newlen = 0;
	Size				freespace = 0;
	bool				same_block = false;

	char			 *undo_xor_delta_size_ptr = NULL;
	xl_xheap_update	 *xlrec = (xl_xheap_update *) XLogRecGetData(record);
	bool			  inplace_update = !(xlrec->flags & XLZ_NON_INPLACE_UPDATE);
	UndoRecPtr		  urecptr = INVALID_UNDO_REC_PTR;

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &newblk);
	if (XLogRecGetBlockTagExtended(record, 1, NULL, NULL, &oldblk,NULL))
		Assert(!inplace_update);
	else
	{
		oldblk = newblk;
		same_block = true;
	}

	urecptr = xheap_xlog_undo_for_update(record, &oldtup, &xlnewundohdr,
													  &undo_xor_delta_size_ptr, &tbuf);
	get_affix_lens_from_update_redo(undo_xor_delta_size_ptr, record, &affix_lens);
	tuples.oldtup = &oldtup;

	/* Read old page */
	oldaction = XLogReadBufferForRedo(record, same_block ? 0 : 1, &buffers.oldbuffer);

	/* read new page */
	newaction = get_update_new_redo_action(record, &buffers, oldaction, same_block);

	/* recover old tuple on data page */
	if (oldaction == BLK_NEEDS_REDO)
	{
		XLogRecPtr	   lsn = record->EndRecPtr;
		FullTransactionId  xid = XLogRecGetFullXid(record);
		Buffer		   oldbuf = buffers.oldbuffer;
		Page		   oldpage = BufferGetPage(buffers.oldbuffer);
		RowPtr		  *rp = NULL;

		if (xheap_page_get_max_offset_number(oldpage) >= xlrec->old_offnum)
			rp = XPageGetRowPtr(oldpage, xlrec->old_offnum);
		else
			elog(PANIC, "invalid rp");

		/* Ensure old tuple points to the tuple in page. */
		oldtup.disk_tuple = (XHeapDiskTuple) XPageGetRowData(oldpage, rp);
		oldtup.disk_tuple_size = RowPtrGetLen(rp);
		oldtup.disk_tuple->flag = xlrec->old_tuple_flag;
		oldtup.disk_tuple->modified_xid = xid; 
		oldtup.disk_tuple->urec = urecptr;

		/* Mark the page as a candidate for pruning  and update the page potential freespace */
		if (xlrec->flags & XLZ_NON_INPLACE_UPDATE)
		{
			XPageSetPrunable(oldpage, xid);
			if (buffers.newbuffer != oldbuf)
				xheap_record_potential_free_space(oldbuf, SHORTALIGN(oldtup.disk_tuple_size));
		}

		PageSetLSN(oldpage, lsn);
		MarkBufferDirty(oldbuf);
	}

	if (newaction == BLK_NEEDS_REDO)
	{
		XLogRecPtr	   lsn = record->EndRecPtr;
		FullTransactionId  xid = XLogRecGetFullXid(record);

		Buffer oldbuf = buffers.oldbuffer;
		Buffer newbuf = buffers.newbuffer;
		Page   oldpage = BufferGetPage(buffers.oldbuffer);
		Page   newpage = BufferGetPage(buffers.newbuffer);

		/* max offset number should be valid */
		Assert(xheap_page_get_max_offset_number(newpage) + 1 >= xlrec->new_offnum);

		newlen = get_xheap_disk_tuple_from_update_new_redo_data(record, &tuples, &affix_lens,
														&tbuf, same_block);

		if (xlrec->flags & XLZ_NON_INPLACE_UPDATE)
		{
			XHeapBufferPage bufpage = {newbuf, NULL};
			tuples.newtup->modified_xid = xid;
			tuples.newtup->urec = xlnewundohdr->urecptr;

			if (xpage_add_item(NULL, &bufpage, (Item) tuples.newtup, newlen, xlrec->new_offnum,
							true) == InvalidOffsetNumber)
				elog(PANIC, "failed to add tuple");

			/* Update the page potential freespace */
			if (newbuf != oldbuf)
				xheap_record_potential_free_space(newbuf, -1 * SHORTALIGN(newlen));
			else
			{
				int delta = newlen - tuples.oldtup->disk_tuple_size;
				xheap_record_potential_free_space(newbuf, -1 * SHORTALIGN(delta));
			}

		}
		else
		{

			RowPtr *rp = XPageGetRowPtr(oldpage, xlrec->old_offnum);
			if (newlen >= RowPtrGetLen(rp) ||
				(xlrec->flags & XLZ_UPDATE_PREFIX_FROM_OLD) != 0 ||
				(xlrec->flags & XLZ_UPDATE_SUFFIX_FROM_OLD) != 0)
				RowPtrChangeLen(rp, newlen);

			tuples.newtup->modified_xid = xid;
			tuples.newtup->urec = urecptr;

			memcpy((char *) tuples.oldtup->disk_tuple,
						(char *) tuples.newtup, newlen);

			if (newlen < tuples.oldtup->disk_tuple_size)
			{
				/* new tuple is smaller, a prunable candidate */
				Assert(oldpage == newpage);
				XPageSetPrunable(newpage, XLogRecGetFullXid(record));
			}

		}

		freespace = page_get_xheap_free_space(newpage); /* needed to update FSM below */

		PageSetLSN(newpage, lsn);
		MarkBufferDirty(newbuf);
	}

	if (BufferIsValid(buffers.newbuffer) && buffers.newbuffer != buffers.oldbuffer)
		UnlockReleaseBuffer(buffers.newbuffer);

	if (BufferIsValid(buffers.oldbuffer))
		UnlockReleaseBuffer(buffers.oldbuffer);

	/* may should free space */
	if (newaction == BLK_NEEDS_REDO && !inplace_update &&
		freespace < BLCKSZ / FREESPACE_FRACTION)
		XLogRecordPageWithFreeSpace(rlocator, newblk, freespace);
}

static int
get_offset_ranges_for_multi_insert(XHeapFreeOffsetRanges **ufree_offset_ranges, char *data,
							  UndoRecPtr **urpvec)
{
	/* allocate the information related to offset ranges */
	char *ranges_data = data;

	int		nranges = *(int *) ranges_data;
	ranges_data += sizeof(int);
	*urpvec = (UndoRecPtr *) ranges_data;
	ranges_data += nranges * sizeof(UndoRecPtr);

	*ufree_offset_ranges = (XHeapFreeOffsetRanges *) palloc0(sizeof(XHeapFreeOffsetRanges));
	Assert(nranges > 0);
	memcpy(&(*ufree_offset_ranges)->startOffset[0],
				  (char *) ranges_data, sizeof(OffsetNumber) * nranges);
	ranges_data += sizeof(OffsetNumber) * nranges;
	memcpy(&(*ufree_offset_ranges)->endOffset[0],
				  (char *) ranges_data, sizeof(OffsetNumber) * nranges);
	ranges_data += sizeof(OffsetNumber) * nranges;

	return nranges;
}

static void 
extract_undo_info_for_multi_insert(UndoRecPtr *blkprev, UndoRecPtr *prevurp,
							xl_undo_meta *undometa, FullTransactionId *xid,
							char *cur_log_ptr, uint8 undo_flag, UndoRecPtr *urecptr,
							XlXHeapMultiInsert **xlrec, XHeapFreeOffsetRanges **ufreeOffsetRanges,
							int *nranges, UndoRecPtr *urpvec)
{
	UndoRecPtr *last_urecptr;
	xl_undo_meta *xlundometa;
	uint32		  undo_meta_size;

	init_xlog_undo_meta(undometa);
	*blkprev = INVALID_UNDO_REC_PTR;
	*prevurp = INVALID_UNDO_REC_PTR;

	if ((undo_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
	{
		*blkprev = *((UndoRecPtr *) ((char *) cur_log_ptr));
		Assert(*blkprev != INVALID_UNDO_REC_PTR);
		cur_log_ptr += sizeof(UndoRecPtr);
	}

	if ((undo_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
	{
		*prevurp = *((UndoRecPtr *) ((char *) cur_log_ptr));
		cur_log_ptr += sizeof(UndoRecPtr);
	}

	if ((undo_flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
	{
		FullTransactionId *fullXid = (FullTransactionId *) cur_log_ptr;
		*xid = *fullXid;
		cur_log_ptr += sizeof(FullTransactionId);
	}

	last_urecptr = (UndoRecPtr *) cur_log_ptr;
	*urecptr = *last_urecptr;
	cur_log_ptr = (char *) last_urecptr + sizeof(*last_urecptr);
	xlundometa = (xl_undo_meta *) cur_log_ptr;

	/* copy xlundometa to local struct */
	copy_xlog_undo_meta(xlundometa, undometa);
	undo_meta_size = xlog_undo_meta_size(undometa);
	cur_log_ptr += undo_meta_size;

	(*xlrec) = (XlXHeapMultiInsert *) cur_log_ptr;
	cur_log_ptr = (char *) *xlrec + SizeOfXHeapMultiInsert;

	/* fetch number of distinct ranges */
	*nranges = get_offset_ranges_for_multi_insert(ufreeOffsetRanges, cur_log_ptr, &urpvec);
}

static UndoRecPtr
xheap_xlog_undo_for_multi_insert(XLogReaderState		*record,
											 const BlockNumber		 blkno,
											 XlXHeapMultiInsert	   **xlrec,
											 XHeapFreeOffsetRanges **ufree_offset_ranges)
{
	UndoPrepareBuffers *upbuffers = NULL;
	UndoRecPtr	  *urpvec = NULL;
	xl_undo_meta   undometa;
	UndoRecPtr	  blkprev;
	UndoRecPtr	  prevurp;

	XLogRecPtr	  lsn = record->EndRecPtr;
	FullTransactionId xid = XLogRecGetFullXid(record);

	xl_undo_header *xlundohdr = (xl_undo_header *) XLogRecGetData(record);
	char		 *curxlogptr = (char *) xlundohdr + SizeOfXLUndoHeader;
	UndoRecPtr	  urecptr = INVALID_UNDO_REC_PTR;
	int			  nranges;
	bool		  skip_insert;
	bool		  skip_undo;
	RelFileLocator	  target_locator = {0};
	bool res	  PG_USED_FOR_ASSERTS_ONLY;

	extract_undo_info_for_multi_insert(&blkprev, &prevurp, &undometa, &xid,
									   curxlogptr, xlundohdr->flag, 
									   &urecptr, xlrec, ufree_offset_ranges, &nranges, urpvec);

	skip_insert = is_skip_insert_undo(urecptr, &undometa);
	if (skip_insert)
		xlog_undo_meta_setinfo(&undometa, XLOG_UNDOMETA_INFO_SKIP);

	skip_undo = ((*xlrec)->flags & XLZ_INSERT_IS_FROZEN);


	res = XLogRecGetBlockTagExtended(record, 0, &target_locator, NULL, NULL,NULL);
	Assert(res == true);

	urecptr = xheap_prepare_undo_multi_insert(
		xlundohdr->relOid, target_locator.relNumber, target_locator.spcOid, UNDO_PERMANENT,
		InvalidBuffer, nranges, xid, InvalidCommandId, blkprev, prevurp, &upbuffers, NULL,
		urpvec, blkno, record, xlundohdr, &undometa);

	elog(LOG, "Undo record prepared: %d for Block Number: %d", nranges, blkno);
	if (!skip_undo && !skip_insert)
	{
		for (int i = 0; i < nranges; i++)
		{
			MemoryContext old_cxt =
				MemoryContextSwitchTo(prepare_buffers_get_undorecord(upbuffers, i)->mem_ctx);
			initStringInfo(GetUndoRecordRawdata(prepare_buffers_get_undorecord(upbuffers, i)));
			MemoryContextSwitchTo(old_cxt);
			appendBinaryStringInfo(GetUndoRecordRawdata(prepare_buffers_get_undorecord(upbuffers, i)),
								   (char *) &(*ufree_offset_ranges)->startOffset[i],
								   sizeof(OffsetNumber));
			appendBinaryStringInfo(GetUndoRecordRawdata(prepare_buffers_get_undorecord(upbuffers, i)),
								   (char *) &(*ufree_offset_ranges)->endOffset[i],
								   sizeof(OffsetNumber));
		}


		Assert(prepare_buffers_get_undorecord(upbuffers, 0)->uur_urp == xlundohdr->urecptr);
		insert_prepared_undo(upbuffers, lsn);
	}

	redo_undo_meta(record, &undometa, xlundohdr->urecptr, prepare_buffers_get_last_undoptr(upbuffers),
				 prepare_buffers_get_last_recordsize(upbuffers));
	reset_prepared_buffers_in_ctx();
	release_undo_prepare_buffers(upbuffers);

	return urecptr;
}

static XLogRedoAction
get_multi_insert_redo_action(XLogReaderState *record, Buffer *buf)
{
	XLogRedoAction action;
	bool		   isinit = (XLogRecGetInfo(record) & XLOG_XHEAP_INIT_PAGE) != 0;
	if (isinit)
	{
		Page page;
		Size pagesize;

		*buf = XLogInitBufferForRedo(record, 0);
		page = BufferGetPage(*buf);
		pagesize = BufferGetPageSize(*buf);

		xpage_init(XPAGE_HEAP, page, pagesize, XHEAP_SPECIAL_SIZE);

		action = BLK_NEEDS_REDO;
	}
	else
		action = XLogReadBufferForRedo(record, 0, buf);

	return action;
}

static XHeapDiskTuple
get_xheap_disk_tuple_from_multi_insert_redo_data(char **data, int *datalen, TupleBuffer *tbuf, UndoRecPtr undo_ptr)
{
	XHeapDiskTuple		 disktup;
	XlMultiInsertXTuple *xlhdr;

	xlhdr = (XlMultiInsertXTuple *) (*data);
	*data = ((char *) xlhdr) + SizeOfMultiInsertXTuple;

	*datalen = xlhdr->datalen;
	Assert(*datalen <= (int) MaxPossibleXHeapTupleSize);
	disktup = &tbuf->hdr;

	memset((char *) disktup, 0, SizeOfXHeapDiskTupleData);
	memcpy((char *) disktup + SizeOfXHeapDiskTupleData, (char *) *data, *datalen);
	*data += *datalen;

	*datalen += SizeOfXHeapDiskTupleData;
	disktup->modified_xid = xlhdr->xid; 
	disktup->urec = undo_ptr;
	disktup->flag2 = xlhdr->flag2;
	disktup->flag = xlhdr->flag;
	disktup->t_hoff = xlhdr->t_hoff;
	disktup->locker_xid = InvalidFullTransactionId;

	return disktup;
}

static void
xheap_xlog_multi_insert(XLogReaderState *record)
{
	RelFileLocator		   rlocator;
	BlockNumber			   blkno = InvalidBlockNumber;
	Buffer				   buf;
	XlXHeapMultiInsert	  *xlrec = NULL;
	XLogRedoAction		   action = BLK_NOTFOUND;
	XHeapFreeOffsetRanges *ufree_offset_ranges = NULL;
	UndoRecPtr			   urecptr = INVALID_UNDO_REC_PTR;

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &blkno);

	urecptr = xheap_xlog_undo_for_multi_insert(record, blkno, &xlrec,
														   &ufree_offset_ranges);

	action = get_multi_insert_redo_action(record, &buf);

	/* Apply the wal for data */
	if (action == BLK_NEEDS_REDO)
	{
		TupleBuffer		tbuf;
		int				newlen;
		Size			len;
		bool			isinit = (XLogRecGetInfo(record) & XLOG_XHEAP_INIT_PAGE) != 0;
		Page			page = BufferGetPage(buf);
		XHeapBufferPage bufpage;
		OffsetNumber	offnum = ufree_offset_ranges->startOffset[0];

		/* Tuples are stored as block data */
		char *tupdata = XLogRecGetBlockData(record, 0, &len);
		char *endptr = tupdata + len;

		for (int i = 0, j = 0; i < xlrec->ntuples; i++, offnum++)
		{
			XHeapDiskTuple uhtup = NULL;
			/*
			* If we're reinitializing the page, the tuples are stored in
			* order from FirstOffsetNumber. Otherwise there's an array of
			* offsets in the WAL record, and the tuples come after that.
			*/
			if (isinit)
				offnum = FirstOffsetNumber + i;
			else
			{
				/*
				* Change the offset range if we've reached the end of current
				* range.
				*/
				if (offnum > ufree_offset_ranges->endOffset[j])
				{
					j++;
					offnum = ufree_offset_ranges->startOffset[j];
				}
			}

			/* max offset should be valid */
			Assert(xheap_page_get_max_offset_number(page) + 1 >= offnum);

			uhtup = get_xheap_disk_tuple_from_multi_insert_redo_data(&tupdata, &newlen, &tbuf, urecptr);
			bufpage.buffer = buf;
			bufpage.page = NULL;
			if (xpage_add_item(NULL, &bufpage, (Item) uhtup, newlen, offnum, true) ==
				InvalidOffsetNumber)
				elog(PANIC, "failed to add tuple");

			/* decrement the potential freespace of this page */
			xheap_record_potential_free_space(buf, SHORTALIGN(newlen));
		}

		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buf);

		if (tupdata != endptr)
			elog(PANIC, "total tuple length mismatch");
	}

	pfree(ufree_offset_ranges);

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

void
xheap_redo(XLogReaderState *record)
{
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/*
     * These operations don't overwrite MVCC data so no conflict processing is
     * required. The ones in heap2 rmgr do.
     */
	switch (info & XLOG_XHEAP_OPMASK)
	{
		case XLOG_XHEAP_INSERT:
			xheap_xlog_insert(record);
			break;
		case XLOG_XHEAP_DELETE:
			xheap_xlog_delete(record);
			break;
		case XLOG_XHEAP_UPDATE:
			xheap_xlog_update(record);
			break;
		case XLOG_XHEAP_CLEAN:
			xheap_xlog_clean(record);
			break;
		case XLOG_XHEAP_MULTI_INSERT:
			xheap_xlog_multi_insert(record);
			break;
		case XLOG_XHEAP_LOCK :
			xheap_xlog_lock(record);
			break;
		default:
			ereport(PANIC, (errmsg("XHeapRedo: unknown op code %u", (uint8) info)));
	}
	elog(DEBUG2, "XHeapRedo called lsn %016lx", record->EndRecPtr);
}


static void
xheap_undo_xlog_page_restore(char *curxlogptr, Buffer buffer, Page page)
{
	/* Restore updated line pointers */
	OffsetNumber  *xlog_min_lp_offset = NULL;
	OffsetNumber  *xlog_max_lp_offset = NULL;

	xlog_min_lp_offset = (OffsetNumber *) curxlogptr;
	curxlogptr += sizeof(OffsetNumber);
	xlog_max_lp_offset = (OffsetNumber *) curxlogptr;
	curxlogptr += sizeof(OffsetNumber);

	Assert(*xlog_min_lp_offset > InvalidOffsetNumber);
	Assert(*xlog_max_lp_offset <= MaxOffsetNumber);

	if (*xlog_max_lp_offset >= *xlog_min_lp_offset)
	{
		Offset				*xlog_copy_start_offset = NULL;
		Offset				*xlog_copy_end_offset = NULL;
		FullTransactionId		*pd_prune_xid = NULL;
		uint16				*pd_flags = NULL;
		uint16				*potential_freespace = NULL;
		XHeapPageHeaderData *phdr = NULL;
		size_t	lpSize = (*xlog_max_lp_offset - *xlog_min_lp_offset + 1) * sizeof(RowPtr);
		memcpy((char *) XPageGetRowPtr(page, *xlog_min_lp_offset), curxlogptr, lpSize);
		curxlogptr += lpSize;

		/* Restore updated tuples data */
		xlog_copy_start_offset = (Offset *) curxlogptr;
		curxlogptr += sizeof(Offset);
		xlog_copy_end_offset = (Offset *) curxlogptr;
		curxlogptr += sizeof(Offset);

		Assert(*xlog_copy_start_offset > (Offset) SizeOfXHeapPageHeaderData);
		Assert(*xlog_copy_end_offset <= BLCKSZ);

		if (*xlog_copy_end_offset > *xlog_copy_start_offset)
		{
			size_t dataSize = *xlog_copy_end_offset - *xlog_copy_start_offset;
			memcpy((char *) page + *xlog_copy_start_offset, curxlogptr, dataSize);
			curxlogptr += dataSize;
		}

		/* Restore updated page headers */
		pd_prune_xid = (FullTransactionId *) curxlogptr;
		curxlogptr += sizeof(FullTransactionId);
		pd_flags = (uint16 *) curxlogptr;
		curxlogptr += sizeof(uint16);
		potential_freespace = (uint16 *) curxlogptr;
		curxlogptr += sizeof(uint16);

		phdr = (XHeapPageHeaderData *) page;
		phdr->pd_flags = *pd_flags;
		phdr->pd_prune_xid = *pd_prune_xid;
		phdr->potential_freespace = *potential_freespace;
	}

}


static void
xheap_undo_xlog_page(XLogReaderState *record)
{
	Buffer			  buf;
	uint8			 *flags = (uint8 *) XLogRecGetData(record);
	char			 *curxlogptr = (char *) ((char *) flags + sizeof(uint8));
	XLogRedoAction	  action = XLogReadBufferForRedo(record, 0, &buf);
	BlockNumber		  blkno = InvalidBlockNumber;

	XLogRecGetBlockTag(record, 0, NULL, NULL, &blkno);
	if (action == BLK_NEEDS_REDO)
	{
		Page page = BufferGetPage(buf);

		if (*flags & XLU_INIT_PAGE)
			xpage_init(XPAGE_HEAP, page, BufferGetPageSize(buf), XHEAP_SPECIAL_SIZE);
		else
			xheap_undo_xlog_page_restore(curxlogptr, buf, page);

		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buf);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
xheap_undo_xlog_abort_specinsert(XLogReaderState *record)
{
	Buffer						buf;
	uint8					   *flags = (uint8 *) XLogRecGetData(record);
	XLogRecPtr					lsn = record->EndRecPtr;
	XlXHeapUndoAbortSpecInsert *xlrec =
		(XlXHeapUndoAbortSpecInsert *) ((char *) flags + sizeof(uint8));
	XLogRedoAction	  action = XLogReadBufferForRedo(record, 0, &buf);
	BlockNumber		  blkno = InvalidBlockNumber;

	(void) XLogRecGetBlockTag(record, 0, NULL, NULL, &blkno);

	if (action == BLK_NEEDS_REDO)
	{
		bool		   relhasindex = *flags & XLU_ABORT_SPECINSERT_REL_HAS_INDEX;
		bool		   is_page_init = *flags & XLU_ABORT_SPECINSERT_INIT_PAGE;

		execute_undo_insert_in_recovery(buf, xlrec->offset, XLogRecGetFullXid(record),
									 relhasindex);

		if (is_page_init)
		{
			Page page = BufferGetPage(buf);
			xpage_init(XPAGE_HEAP, page, BufferGetPageSize(buf), XHEAP_SPECIAL_SIZE);
		}

		PageSetLSN(BufferGetPage(buf), lsn);
		MarkBufferDirty(buf);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

void
xheap_undo_redo(XLogReaderState *record)
{
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_XHEAPUNDO_PAGE:
			xheap_undo_xlog_page(record);
			break;
		case XLOG_XHEAPUNDO_ABORT_SPECINSERT:
			xheap_undo_xlog_abort_specinsert(record);
			break;
		default:
			elog(PANIC, "XHeapUndoRedo: unknown op code %u", info);
	}
}

static TransactionId
xheap_xlog_get_current_xid_insert(XLogReaderState *record)
{
	return XLogRecGetXid(record);
}

static TransactionId
xheap_xlog_get_current_xid_delete(XLogReaderState *record)
{
	return XLogRecGetXid(record);
}

static TransactionId
xheap_xlog_get_current_xid_update(XLogReaderState *record)
{
	return XLogRecGetXid(record);
}

static TransactionId
xheap_xlog_get_current_xid_multi_insert(XLogReaderState *record)
{
	return XLogRecGetXid(record);
}


static VirtualTransactionId *
get_conflicting_virtual_xids_with_frozen_xmin(FullTransactionId limit_xmin)
{
	static VirtualTransactionId *vxids;
	FullTransactionId 			backend_frozen_xmin;
	int 			  			i;
	int							count = 0;

	/*
	 * If first time through, get workspace to remember main XIDs in. We
	 * malloc it permanently to avoid repeated palloc/pfree overhead. Allow
	 * result space, remembering room for a terminator.
	 */
	if (vxids == NULL)
	{
		vxids = (VirtualTransactionId *)
			malloc(sizeof(VirtualTransactionId) * (MAX_FROZEN_XMIN_NUM + 1));
		if (vxids == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					errmsg("out of memory")));
	}

	for (i = 0; i < MAX_FROZEN_XMIN_NUM; i++) 
	{
		backend_frozen_xmin = FullTransactionIdFromU64(pg_atomic_read_u64(&(MyFrozenXmins[i])));
		if (FullTransactionIdIsNormal(backend_frozen_xmin) && FullTransactionIdPrecedes(backend_frozen_xmin, limit_xmin))
		{
			PGPROC *proc;
			VirtualTransactionId vxid;

			/* The index of MyFrozenXmins is BackendId */
			proc = ProcNumberGetProc(i);
			GET_VXID_FROM_PGPROC(vxid, *proc);
			if (VirtualTransactionIdIsValid(vxid))
				vxids[count++] = vxid;
		}
	}

	/* add the terminator */
	vxids[count].procNumber = INVALID_PROC_NUMBER;
	vxids[count].localTransactionId = InvalidLocalTransactionId;

	return vxids;
}

void
resolve_recovery_conflict_with_global_frozen_xmin(FullTransactionId latest_removed_full_xid)
{
	VirtualTransactionId *backends;

	if (!FullTransactionIdIsValid(latest_removed_full_xid))
		return;

	backends = get_conflicting_virtual_xids_with_frozen_xmin(latest_removed_full_xid);
	
	ResolveRecoveryConflictWithVirtualXIDs(backends,
										   PROCSIG_RECOVERY_CONFLICT_SNAPSHOT,
										   WAIT_EVENT_RECOVERY_CONFLICT_SNAPSHOT,
										   true);
}

TransactionId
xheap_xlog_get_current_xid(XLogReaderState *record)
{
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info & XLOG_XHEAP_OPMASK)
	{
		case XLOG_XHEAP_INSERT:
			return xheap_xlog_get_current_xid_insert(record);
		case XLOG_XHEAP_DELETE:
			return xheap_xlog_get_current_xid_delete(record);
		case XLOG_XHEAP_UPDATE:
			return xheap_xlog_get_current_xid_update(record);
		case XLOG_XHEAP_CLEAN:
			break;
		case XLOG_XHEAP_MULTI_INSERT:
			/* The way we get current xid in MULTI_INSERT is not affected */
			return xheap_xlog_get_current_xid_multi_insert(record);
		default:
			ereport(PANIC, (errmsg("XHeapRedo: unknown op code %u", (uint8) info)));
	}

	return XLogRecGetXid(record);
}
