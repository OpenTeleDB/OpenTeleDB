/* -------------------------------------------------------------------------
 *
 * xheapundo.c
 * undo rollback action for xheap
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xheapundo.c
 * -------------------------------------------------------------------------
 */

#include "undo/undotype.h"
#include "undo/undorecord.h"
#include "undo/undorequest.h"
#include "undo/undofetch.h"
#include "xheap/xheapundo.h"
#include "xheap/xredo.h"
#include "xheap/xtuple.h"
#include "xheap/xpage.h"
#include "utils/relcache.h"
#include "storage/bufmgr.h"
#include "access/xloginsert.h"
#include "access/subtrans.h"
#include "util/xrmgr.h"
#include "xstore.h"

static void	 log_xheap_undo_actions(XHeapUndoActionWALInfo *wal_info, Relation rel);

static int execute_undo_for_update(UnpackedUndoRecord *undorecord, Buffer buffer);
static int execute_undo_for_delete(UnpackedUndoRecord *undorecord, Buffer buffer);



int
execute_undo_insert(Relation rel, Buffer buffer, OffsetNumber off, UnpackedUndoRecord *urec)
{
	Page	page = BufferGetPage(buffer);
	RowPtr *rp = XPageGetRowPtr(page, off);
	XHeapDiskTuple disk_tuple ;
	// already dead
	if (!RowPtrHasStorage(rp))
		return ROLLBACK_ROK;

	disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);

	// already done
	if (!FullTransactionIdEquals(disk_tuple->modified_xid, GetUndoRecordXid(urec)))
		return ROLLBACK_ROK;

	// switch undolog ,wait other undolog do first
	if (disk_tuple->urec != urec->uur_urp)
	{
		if (urec->uur_urp < disk_tuple->urec)
			return ROLLBACK_RSWITCH;
		return ROLLBACK_ROK;
	}

	/* Rollback insert - increment the potential space for the Page */
	xheap_record_potential_free_space(buffer, SHORTALIGN(rp->len));

	RowPtrSetUnused(rp);
	PageSetHasFreeLinePointers(page);

	disk_tuple->modified_xid = InvalidFullTransactionId;
	disk_tuple->locker_xid = InvalidFullTransactionId;
	disk_tuple->urec = INVALID_UNDO_REC_PTR;

    XPageSetPrunable(page, GetUndoRecordXid(urec)); 
	return ROLLBACK_ROK;
}


void
execute_undo_insert_in_recovery(Buffer buffer, OffsetNumber off, FullTransactionId xid,
							 bool relhasindex )
{
	Page		   page = BufferGetPage(buffer);
	RowPtr		  *rp = XPageGetRowPtr(page, off);
	XHeapDiskTuple disk_tuple = NULL;

	Assert(RowPtrIsNormal(rp));

	disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);

	// already done
	if (!FullTransactionIdEquals(disk_tuple->modified_xid, xid))
		return;

	xheap_record_potential_free_space(buffer, SHORTALIGN(rp->len));

	disk_tuple->modified_xid = InvalidFullTransactionId;
	disk_tuple->locker_xid = InvalidFullTransactionId;
	disk_tuple->urec = INVALID_UNDO_REC_PTR;

	RowPtrSetUnused(rp);
	PageSetHasFreeLinePointers(page);

	XPageSetPrunable(page, xid);
}

/*
 * undo_keep_subtxn_lock
 * infomask may contains sub transaction lock info before rollback.
 */
static void undo_keep_subtxn_lock(UnpackedUndoRecord *undorecord, XHeapDiskTuple disk_tuple, uint16	infomask)
{
	if (UndoRecordHasSubXact(undorecord))
	{
		if (FullTransactionIdFollowsOrEquals(disk_tuple->locker_xid, FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid))) &&
			TransactionIdEquals(XidFromFullTransactionId(GetUndoRecordXid(undorecord)), SubTransGetTopmostTransaction(XidFromFullTransactionId(disk_tuple->locker_xid))) && 
			!FullTransactionIdEquals(disk_tuple->locker_xid, GetUndoRecordSubXid(undorecord)))
			/* locker transaction is my ancestor's transaction, only keep transaction lock flag */
			disk_tuple->flag |= infomask & XHEAP_LOCK_STATUS_MASK;
	}
}


static int
execute_undo_for_delete(UnpackedUndoRecord *undorecord, Buffer buffer)
{
	OffsetNumber   offnum;
	Page		   page;
	RowPtr		  *rp = NULL;
	XHeapDiskTuple disk_tuple = NULL;
	uint16	infomask;

	Assert(undorecord != NULL);

	offnum = GetUndoRecordOffset(undorecord);
	Assert(offnum != InvalidOffsetNumber);

	page = BufferGetPage(buffer);
	rp = XPageGetRowPtr(page, offnum);
	if (!RowPtrIsNormal(rp))
		return ROLLBACK_ROK;

	disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);

	// already done
	if (!FullTransactionIdEquals(disk_tuple->modified_xid, GetUndoRecordXid(undorecord)))
		return ROLLBACK_ROK;

	// switch undolog ,wait other undolog do first
	if (disk_tuple->urec != undorecord->uur_urp)
	{
		// when switch undolog : new zoneid always > old zoneid.
		if (undorecord->uur_urp < disk_tuple->urec)
			return ROLLBACK_RSWITCH;
		return ROLLBACK_ROK;
	}

	disk_tuple->modified_xid = GetUndoRecordOldXactId(undorecord);
	disk_tuple->urec = GetUndoRecordTpprev(undorecord);
	infomask = disk_tuple->flag;
	disk_tuple->flag = GetUndoRecordDataHeadFlag(undorecord);
	/* keep sub transaction lock */
	undo_keep_subtxn_lock(undorecord, disk_tuple, infomask);
	
	return ROLLBACK_ROK;
}


static int
execute_undo_for_update(UnpackedUndoRecord *undorecord, Buffer buffer)
{
	OffsetNumber   offnum;
	Page		   page;
	RowPtr		  *rp = NULL;
	XHeapDiskTuple disk_tuple = NULL;
	uint8		   undo_type;
	Assert(undorecord != NULL);

	offnum = GetUndoRecordOffset(undorecord);
	Assert(offnum != InvalidOffsetNumber);

	page = BufferGetPage(buffer);
	rp = XPageGetRowPtr(page, offnum);

	if (offnum == InvalidOffsetNumber)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("undorecord %lu xid %lu undotype %u need rollback but rp invalid: "
						"reloid %u, blk %u, offnum %u, rpflag %u, rpoffset %u, rplen %u.",
						undorecord->uur_urp, GetUndoRecordXid(undorecord).value,
						GetUndoRecordUtype(undorecord), GetUndoRecordReloid(undorecord),
						GetUndoRecordBlkno(undorecord), offnum, rp->flags, rp->offset,
						rp->len)));

	if (!RowPtrIsNormal(rp))
		return ROLLBACK_ROK;

	disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);

	// already done
	if (!FullTransactionIdEquals(disk_tuple->modified_xid, GetUndoRecordXid(undorecord)))
		return ROLLBACK_ROK;

	// switch undolog ,wait other undolog do first
	if (disk_tuple->urec != undorecord->uur_urp)
	{
		if (undorecord->uur_urp < disk_tuple->urec)
			return ROLLBACK_RSWITCH;
		return ROLLBACK_ROK;
	}

	undo_type = GetUndoRecordUtype(undorecord);
	switch (undo_type)
	{
		case UNDO_UPDATE:
		{
			uint16			infomask = 0;
#ifdef USE_ASSERT_CHECKING
			StringInfoData *undo_data = GetUndoRecordRawdata(undorecord);

			Assert(undo_data != NULL);
			Assert(undo_data->len > 0);

			Assert(undo_data->len == sizeof(ItemPointerData));
#endif
			infomask = disk_tuple->flag;
			disk_tuple->flag = GetUndoRecordDataHeadFlag(undorecord);
			disk_tuple->modified_xid = GetUndoRecordOldXactId(undorecord);  
			/* keep sub transaction lock */
			undo_keep_subtxn_lock(undorecord, disk_tuple, infomask);
			disk_tuple->urec = GetUndoRecordTpprev(undorecord);
      
			xheap_record_potential_free_space(buffer, -1 * rp->len);

			break;
		}
		case UNDO_INPLACE_UPDATE:
		{
			StringInfoData *undo_data = GetUndoRecordRawdata(undorecord);
			uint16		   *prefixlen_ptr;
			uint16		   *suffixlen_ptr;
			uint8		   *t_hoff_ptr;
			uint8		   *flags_ptr;
			uint16			prefixlen = 0;
			uint16			suffixlen = 0;
			uint16			infomask = 0;
			uint8			t_hoff = 0;
			uint8			flags = 0;
			char		   *cur_undodata_ptr = NULL;
			int				read_size = 0;
			int				subxid_size = 0;
			char   *cur_old_disktuple_ptr = NULL;
			char   *old_disktuple = NULL;
			char   *newp = NULL;
			char   *cur_undo_data_p = NULL;
			int		oldlen;
			int		newlen;
			int		old_data_len;
			uint32	new_tuple_length;
			Assert(undo_data != NULL);
			Assert(undo_data->len > 0);

			t_hoff_ptr = (uint8 *) undo_data->data;
			t_hoff = *t_hoff_ptr;
			cur_undodata_ptr = undo_data->data + sizeof(uint8) + t_hoff - OffsetDataHeader;
			read_size = sizeof(uint8) + t_hoff - OffsetDataHeader;

			flags_ptr = (uint8 *) cur_undodata_ptr;
			flags = *flags_ptr;
			cur_undodata_ptr += sizeof(uint8);
			read_size += sizeof(uint8);

			if (flags & UREC_XOR_PREFIX)
			{
				prefixlen_ptr = (uint16 *) (cur_undodata_ptr);
				cur_undodata_ptr += sizeof(uint16);
				read_size += sizeof(uint16);
				prefixlen = *prefixlen_ptr;
			}

			if (flags & UREC_XOR_SUFFIX)
			{
				suffixlen_ptr = (uint16 *) (cur_undodata_ptr);
				cur_undodata_ptr += sizeof(uint16);
				read_size += sizeof(uint16);
				suffixlen = *suffixlen_ptr;
			}


			old_disktuple = undo_cache_ctx->disk_tuple_buffer;
			memcpy(old_disktuple + OffsetDataHeader, undo_data->data + sizeof(uint8), t_hoff - OffsetDataHeader);
			cur_old_disktuple_ptr = old_disktuple + t_hoff;

			/* copy the perfix to old_disktuple */
			if (flags & UREC_XOR_PREFIX)
			{
				memcpy(cur_old_disktuple_ptr, (char *) disk_tuple + disk_tuple->t_hoff, prefixlen);
				cur_old_disktuple_ptr += prefixlen;
			}

			newp = (char *) disk_tuple + disk_tuple->t_hoff + prefixlen;
			cur_undo_data_p = undo_data->data + read_size;
			oldlen = undo_data->len - read_size - subxid_size + prefixlen + suffixlen;
			newlen = rp->len - disk_tuple->t_hoff;
			old_data_len = oldlen - prefixlen - suffixlen;

			if (old_data_len > 0)
			{
				memcpy(cur_old_disktuple_ptr, cur_undo_data_p, old_data_len);
				cur_old_disktuple_ptr += (old_data_len);
			}

			if (flags & UREC_XOR_SUFFIX)
			{
				memcpy(cur_old_disktuple_ptr, newp + newlen - prefixlen - suffixlen, suffixlen);
			}
			new_tuple_length = oldlen + t_hoff;
			RowPtrChangeLen(rp, new_tuple_length);
			
			/* retain disk tuple flag */
			infomask = disk_tuple->flag;
			memcpy((char*)disk_tuple + OffsetDataHeader, old_disktuple + OffsetDataHeader, new_tuple_length - OffsetDataHeader);

			disk_tuple->modified_xid = GetUndoRecordOldXactId(undorecord);
			disk_tuple->urec = GetUndoRecordTpprev(undorecord);
			/* keep sub transaction lock */
			undo_keep_subtxn_lock(undorecord, disk_tuple, infomask);

			break;
		}
		case UNDO_DELETE:
		case UNDO_ITEMID_UNUSED:
		case UNDO_INSERT:
		case UNDO_MULTI_INSERT:
		{
			ereport(PANIC, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							errmsg("invalid undo record type for restoring tuple.")));
			break;
		}
		default:
			ereport(PANIC, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							errmsg("Unsupported Rollback Action")));
	}

	return ROLLBACK_ROK;
}


static void
log_xheap_undo_actions(XHeapUndoActionWALInfo *wal_info, Relation rel)
{
	uint8				 flags = 0;
	Page				 page = BufferGetPage(wal_info->buffer);

	XLogRecPtr			 recptr = InvalidXLogRecPtr;

	if (wal_info->need_init)
		flags |= XLU_INIT_PAGE;

	if (wal_info->potential_freespace > BLCKSZ)
		ereport(PANIC, (errmsg("Invalid potential freespace(%u)",
							   wal_info->potential_freespace)));

	XLogBeginInsert();
	XLogRegisterData((char *) &flags, sizeof(uint8));

	if (!wal_info->need_init)
	{
		/* Check the fields written to the xlog in advance */
		if (wal_info->xlog_min_lp_offset <= (OffsetNumber) InvalidOffsetNumber ||
			wal_info->xlog_max_lp_offset >= (OffsetNumber) MaxOffsetNumber)
			ereport(PANIC, (errmsg("Invalid lp offsetnumber, min(%u), max(%u).",
								   wal_info->xlog_min_lp_offset, wal_info->xlog_max_lp_offset)));

		if (wal_info->xlog_copy_start_offset <=
				(Offset) (SizeOfXHeapPageHeaderData) ||
			wal_info->xlog_copy_end_offset < (Offset) 0 ||
			wal_info->xlog_copy_start_offset > (Offset) BLCKSZ ||
			wal_info->xlog_copy_end_offset > (Offset) BLCKSZ)
			ereport(PANIC,
					(errmsg("Invalid rollback start and end offset in page, (start, "
							"end)=(%d, %d).",
							wal_info->xlog_copy_start_offset, wal_info->xlog_copy_end_offset)));

		/* Store updated line pointers data */
		XLogRegisterData((char *) &wal_info->xlog_min_lp_offset, sizeof(OffsetNumber));
		XLogRegisterData((char *) &wal_info->xlog_max_lp_offset, sizeof(OffsetNumber));

		if (wal_info->xlog_max_lp_offset >= wal_info->xlog_min_lp_offset)
		{
			size_t lpSize = (wal_info->xlog_max_lp_offset - wal_info->xlog_min_lp_offset + 1) *
							sizeof(RowPtr);
			XLogRegisterData((char *) XPageGetRowPtr(page, wal_info->xlog_min_lp_offset),
							 lpSize);

			/* Store updated tuples data */
			XLogRegisterData((char *) &wal_info->xlog_copy_start_offset, sizeof(Offset));
			XLogRegisterData((char *) &wal_info->xlog_copy_end_offset, sizeof(Offset));

			if (wal_info->xlog_copy_end_offset > wal_info->xlog_copy_start_offset)
			{
				size_t dataSize =
					wal_info->xlog_copy_end_offset - wal_info->xlog_copy_start_offset;
				XLogRegisterData((char *) page + wal_info->xlog_copy_start_offset, dataSize);
			}

			/* Store updated page headers */
			XLogRegisterData((char *) &wal_info->pd_prune_xid, sizeof(FullTransactionId));
			XLogRegisterData((char *) &wal_info->pd_flags, sizeof(uint16));
			XLogRegisterData((char *) &wal_info->potential_freespace, sizeof(uint16));
		}

	}

	XLogRegisterBuffer(0, wal_info->buffer, REGBUF_STANDARD);

	recptr = XLogInsert(RM_XHEAPUNDO_ID, XLOG_XHEAPUNDO_PAGE);

	PageSetLSN(page, recptr);
}

int
execute_xheap_undoactions(UndoList *ulist, int start_idx, int end_idx, BlockNumber blkno,  Relation relation)
{
	Buffer			  buffer;
	Page			  page;
	bool              wait_switch_zone =false;

	bool			  need_page_init = false;
	OffsetNumber	  xlog_min_lp_offset = MaxOffsetNumber;
	OffsetNumber	  xlog_max_lp_offset = InvalidOffsetNumber;
	Offset			  xlog_copy_start_offset = BLCKSZ;
	Offset			  xlog_copy_end_offset = 0;

	int 			  i;
	OffsetNumber 	  offset;

	buffer = ReadBuffer(relation, blkno);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);



	START_CRIT_SECTION();

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

		/* Store the minimum and maximum LP offsets of all undorecords */
		if (GetUndoRecordOffset(undorecord) > InvalidOffsetNumber &&
			GetUndoRecordOffset(undorecord) < xlog_min_lp_offset)
			xlog_min_lp_offset = GetUndoRecordOffset(undorecord);
			
		if (GetUndoRecordOffset(undorecord) <= MaxOffsetNumber &&
			GetUndoRecordOffset(undorecord) > xlog_max_lp_offset)
			xlog_max_lp_offset = GetUndoRecordOffset(undorecord);

		switch (undotype)
		{
			case UNDO_INSERT:
			{
				int nline;
				int undo_result = execute_undo_insert(relation, buffer,
									 GetUndoRecordOffset(undorecord),
									 undorecord);
				if (undo_result == ROLLBACK_RSWITCH)
					wait_switch_zone = true;

				nline = xheap_page_get_max_offset_number(page);
				need_page_init = true;
				for (offset = FirstOffsetNumber; offset <= nline; offset++)
				{
					RowPtr *rp = XPageGetRowPtr(page, offset);
					if (RowPtrIsUsed(rp))
					{
						need_page_init = false;
						break;
					}
				}

				break;
			}
			case UNDO_MULTI_INSERT:
			{
				OffsetNumber start_offset;
				OffsetNumber end_offset;
				OffsetNumber iter_offset;
				int			 nline;

				Assert(GetUndoRecordRawdata(undorecord) != NULL);
				start_offset =
					((OffsetNumber *) GetUndoRecordRawdata(undorecord)->data)[0];
				end_offset = ((OffsetNumber *) GetUndoRecordRawdata(undorecord)->data)[1];

				/* Store the minimum and maximum LP offsets */
				if (start_offset < xlog_min_lp_offset)
				{
					Assert(start_offset > InvalidOffsetNumber);
					xlog_min_lp_offset = start_offset;
				}
				if (end_offset > xlog_max_lp_offset)
				{
					Assert(end_offset <= MaxOffsetNumber);
					xlog_max_lp_offset = end_offset;
				}

				for (iter_offset = start_offset; iter_offset <= end_offset; iter_offset++)
				{
					int undoRes = execute_undo_insert(relation, buffer, iter_offset,
										 undorecord);
					if (undoRes == ROLLBACK_RSWITCH)
						wait_switch_zone = true;
				}

				nline = xheap_page_get_max_offset_number(page);
				need_page_init = true;
				for (i = FirstOffsetNumber; i <= nline; i++)
				{
					RowPtr *rp = XPageGetRowPtr(page, i);
					if (RowPtrIsUsed(rp))
					{
						need_page_init = false;
						break;
					}
				}

				break;
			}
			
			case UNDO_DELETE:
			{
				RowPtr		  *rp = NULL;

				int undo_res = execute_undo_for_delete(undorecord, buffer);
				if (undo_res == ROLLBACK_RSWITCH)
				{
					wait_switch_zone = true;
					break;
				}

				/* Store the page offsets where we start and end updating tuples */
				rp = XPageGetRowPtr(page, GetUndoRecordOffset(undorecord));
				if (rp->offset < xlog_copy_start_offset)
				{
					Assert(rp->offset > SizeOfXHeapPageHeaderData);
					xlog_copy_start_offset = rp->offset;
				}
				if (rp->offset + rp->len > xlog_copy_end_offset)
				{
					Assert(rp->offset + rp->len <= BLCKSZ);
					xlog_copy_end_offset = rp->offset + rp->len;
				}

				break;
			}
			case UNDO_UPDATE:
			case UNDO_INPLACE_UPDATE:
			{
				RowPtr		  *rp = NULL;

				int undo_res = execute_undo_for_update(undorecord, buffer);
				if (undo_res == ROLLBACK_RSWITCH)
				{
					wait_switch_zone = true;
					break;
				}

				/* Store the page offsets where we start and end updating tuples */
				rp = XPageGetRowPtr(page, GetUndoRecordOffset(undorecord));
				if (rp->offset < xlog_copy_start_offset)
				{
					Assert(rp->offset > SizeOfXHeapPageHeaderData);
					xlog_copy_start_offset = rp->offset;
				}
				if (rp->offset + rp->len > xlog_copy_end_offset)
				{
					Assert(rp->offset + rp->len <= BLCKSZ);
					xlog_copy_end_offset = rp->offset + rp->len;
				}

				break;
			}
			case UNDO_XBTREE_INSERT:
			case UNDO_XBTREE_DELETE:
			{
				ereport(PANIC, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								errmsg("Unsupported Rollback Action for xheap table")));
				break;
			}
			case UNDO_ITEMID_UNUSED:
			default:
				ereport(PANIC, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								errmsg("Unsupported Rollback Action")));
		}
	}


	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(relation))
	{
		XHeapUndoActionWALInfo wal_info;
		XHeapPageHeaderData	  *phdr = (XHeapPageHeaderData *) page;

		wal_info.buffer = buffer;
		wal_info.xlog_min_lp_offset = xlog_min_lp_offset;
		wal_info.xlog_max_lp_offset = xlog_max_lp_offset;
		wal_info.xlog_copy_start_offset = xlog_copy_start_offset;
		wal_info.xlog_copy_end_offset = xlog_copy_end_offset;
		wal_info.need_init = need_page_init;

		/*
         * NOTE: If more page headers are updated by rollback,
         * they should be added to this list
         */
		wal_info.pd_flags = phdr->pd_flags;
		wal_info.potential_freespace = phdr->potential_freespace;
		wal_info.pd_prune_xid = phdr->pd_prune_xid;

		log_xheap_undo_actions(&wal_info, relation);
	}

	if (need_page_init)
	{
		XLogRecPtr lsn = PageGetLSN(page);

		if (relation->rd_rel->relkind == RELKIND_TOASTVALUE)
		{
			xpage_init(XPAGE_TOAST, page, BufferGetPageSize(buffer), XHEAP_SPECIAL_SIZE);
		}
		else
		{
			xpage_init(XPAGE_HEAP, page, BufferGetPageSize(buffer), XHEAP_SPECIAL_SIZE);
		}
		PageSetLSN(page, lsn);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);

	if (wait_switch_zone)
		return ROLLBACK_RSWITCH;

	return ROLLBACK_ROK;
}
