/* -------------------------------------------------------------------------
 *
 * undoxlog.c
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 *    src/undo/undoxlog.c
 *
 * -------------------------------------------------------------------------
 */

#include "c.h"
#include "postgres.h"

#include "access/twophase.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "storage/buf.h"
#include "undo/undolog.h"
#include "undo/undoxlog.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "storage/standby.h"
#include "util/xrmgr.h"
#include "xstore.h"

void xlog_undo_extend_redo(const xl_undolog_extend *xlrec, XLogRecPtr extend_lsn);
void xlog_undo_unlink_redo(const xl_undolog_unlink *xlrec, XLogRecPtr unlink_lsn);
void xlog_undo_extend_txnslot_redo(const xl_undolog_extend *xlrec, XLogRecPtr extend_lsn);
void xlog_undo_unlink_txnslot_redo(const xl_undolog_unlink *xlrec, XLogRecPtr unlink_lsn);
void xlog_undo_discard_redo(const xl_undolog_discard *xlrec, XLogRecPtr lsn);
void xlog_undo_rollback_finish_redo(XLogReaderState *record, UndoSlotPtr slot_ptr, XLogRecPtr lsn);

void
xlog_undo_extend_redo(const xl_undolog_extend *xlrec, XLogRecPtr extend_lsn)
{
	int				logno = 0;
	UndoLogControl *undolog;
	UndoSegment *usp;

	Assert(xlrec != NULL);

	logno = UNDO_PTR_GET_LOG_NO(xlrec->tail);
	Assert(IS_VALID_LOGNO(logno));
	undolog = get_undo_log(logno);
	if (undolog == NULL)
		return;

	Assert(UNDO_PTR_GET_LOG_NO(xlrec->tail) == UNDO_PTR_GET_LOG_NO(xlrec->prevtail));
	usp = &undolog->undo_data_seg;

	if (usp->lsn < extend_lsn)
	{
		UndoLogOffset new_tail = UNDO_PTR_GET_OFFSET(xlrec->tail);

		lock_undo_segment(usp);
		usp->dirty = true;
		extend_undo_segment(usp, logno, new_tail, UNDO_DATA_DB_OID);
		usp->lsn = (extend_lsn);
		unlock_undo_segment(usp);
	}
	return;
}

void
xlog_undo_unlink_redo(const xl_undolog_unlink *xlrec, XLogRecPtr unlink_lsn)
{
	int				logno = 0;
	UndoLogControl *ulog;
	UndoSegment *usp;
	RelFileLocator rlocator;
	Assert(xlrec != NULL);

	logno = UNDO_PTR_GET_LOG_NO(xlrec->head);
	Assert(IS_VALID_LOGNO(logno));
	ulog = get_undo_log(logno);
	if (ulog == NULL)
		return;

	Assert(UNDO_PTR_GET_LOG_NO(xlrec->head) == UNDO_PTR_GET_LOG_NO(xlrec->prevhead));
	usp = &ulog->undo_data_seg;
	if (usp->lsn < unlink_lsn)
	{
		UndoLogOffset new_head = UNDO_PTR_GET_OFFSET(xlrec->head);
		UndoLogOffset head = usp->head;

		Assert(head == UNDO_PTR_GET_OFFSET(xlrec->prevhead));
		forget_undo_log_buffers(ulog, head, new_head, UNDO_DATA_DB_OID);
		lock_undo_segment(usp);
		usp->dirty = true;
		unlink_undo_segment(usp, logno, new_head, UNDO_DATA_DB_OID);
		usp->lsn = (unlink_lsn);
		unlock_undo_segment(usp);
	}

	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, xlrec->prevhead, UNDO_DATA_DB_OID);
	for (UndoRecPtr start = xlrec->prevhead; start < xlrec->head;)
	{
		forget_invalid_page(rlocator, MAIN_FORKNUM, start / BLCKSZ);
		start += UNDOLOG_FILE_SIZE(UNDO_DATA_DB_OID);;
	}
	return;
}

void
xlog_undo_extend_txnslot_redo(const xl_undolog_extend *xlrec, XLogRecPtr extend_lsn)
{
	int				logno = 0;
	UndoLogControl *ulog;
	UndoSegment *usp;

	Assert(xlrec != NULL);

	logno = UNDO_PTR_GET_LOG_NO(xlrec->tail);
	Assert(IS_VALID_LOGNO(logno));
	ulog = get_undo_log(logno);
	if (ulog == NULL)
		return;

	Assert(UNDO_PTR_GET_LOG_NO(xlrec->tail) == UNDO_PTR_GET_LOG_NO(xlrec->prevtail));
	usp = &ulog->undo_txn_seg;

	if (usp->lsn < extend_lsn)
	{
		UndoLogOffset new_tail = UNDO_PTR_GET_OFFSET(xlrec->tail);

		lock_undo_segment(usp);
		usp->dirty = true;
		extend_undo_segment(usp, logno, new_tail, UNDO_TXN_DB_OID);
		usp->lsn = (extend_lsn);
		unlock_undo_segment(usp);
	}
	return;
}

void
xlog_undo_unlink_txnslot_redo(const xl_undolog_unlink *xlrec, XLogRecPtr unlink_lsn)
{
	int				logno = 0;
	UndoLogControl *ulog;
	UndoSegment *usp;
	RelFileLocator rlocator;

	Assert(xlrec != NULL);

	logno = UNDO_PTR_GET_LOG_NO(xlrec->head);
	Assert(IS_VALID_LOGNO(logno));
	ulog = get_undo_log(logno);
	if (ulog == NULL)
		return;

	Assert(UNDO_PTR_GET_LOG_NO(xlrec->head) == UNDO_PTR_GET_LOG_NO(xlrec->prevhead));
	usp = &ulog->undo_txn_seg;

	if (usp->lsn < unlink_lsn)
	{
		UndoLogOffset new_head = UNDO_PTR_GET_OFFSET(xlrec->head);
		UndoLogOffset head = usp->head;

		Assert(head == UNDO_PTR_GET_OFFSET(xlrec->prevhead));
		forget_undo_log_buffers(ulog, head, new_head, UNDO_TXN_DB_OID);
		lock_undo_segment(usp);
		usp->dirty = true;
		unlink_undo_segment(usp, logno, new_head, UNDO_TXN_DB_OID);
		usp->lsn = (unlink_lsn);
		unlock_undo_segment(usp);
	}

	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, xlrec->prevhead, UNDO_TXN_DB_OID);
	for (UndoRecPtr start = xlrec->prevhead; start < xlrec->head;)
	{
		forget_invalid_page(rlocator, MAIN_FORKNUM, start / BLCKSZ);
		start += UNDOLOG_FILE_SIZE(UNDO_TXN_DB_OID);
	}
	return;
}

void
xlog_undo_discard_redo(const xl_undolog_discard *xlrec, XLogRecPtr lsn)
{
	UndoLogControl *ulog;

	Assert(xlrec != NULL);
	ulog = get_undo_log(UNDO_PTR_GET_LOG_NO(xlrec->startSlot));
	if (ulog == NULL)
		return;

	if (InHotStandby)
	{
		if (!is_skip_insert_slot(xlrec->startSlot))
		{
			UndoSlotBuffer buf;
			Page		page;
			FullTransactionId global_fronzen_xid;

			init_undo_slot_buffer(&buf);
			load_undo_slot_buffer(&buf, xlrec->startSlot);
			LockBuffer(buf.buffer, BUFFER_LOCK_EXCLUSIVE);
			if (BufferIsValid(buf.buffer))
			{
				page = BufferGetPage(buf.buffer);
				if (PageGetLSN(page) < lsn)
				{
					UndoSlotPtr recycle;
					UndoSlotPtr next;
					UndoSlot *slot;
					RelFileLocator rlocator;

					recycle = INVALID_UNDO_SLOT_PTR;

					/*
					 * We compare the transaction id with every proc.xmin to check whether this redo may conflict
					 * with any snapshot. So we only need the max transaction id because if someone conflict with
					 * the max one, it will conflict with all others.
					 */
					next = xlrec->startSlot;
					while (next < xlrec->endSlot)
					{
						recycle = next;
						next = get_next_slotptr(next);
					}

					slot = get_undo_slot_from_buffer(&buf, recycle);
					rlocator.dbOid = slot->dbOid;

					ResolveRecoveryConflictWithSnapshotFullXid(slot->xid, false, rlocator);
				}
				UnlockReleaseBuffer(buf.buffer);
			}

			global_fronzen_xid = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->hot_standby_frozen_xid));
			if (FullTransactionIdFollows(xlrec->globalFrozenXid, global_fronzen_xid))
				pg_atomic_write_u64(&undo_sys_ctx->hot_standby_frozen_xid, U64FromFullTransactionId(xlrec->globalFrozenXid));
		}
	}

	if (ulog->lsn < lsn)
	{
		lock_undo_log(ulog);
		Assert(xlrec->startSlot == UndoGetRecycleTSlotPtr(ulog));
		ulog->recycle_slot_offset = UNDO_PTR_GET_OFFSET(xlrec->endSlot);
		ulog->discard_offset = UNDO_PTR_GET_OFFSET(xlrec->endUndoPtr);
		ulog->force_discard_offset = UNDO_PTR_GET_OFFSET(xlrec->endUndoPtr);
		ulog->recycle_xid = xlrec->recycledXid;
		ulog->dirty = true;
		ulog->lsn = (lsn);
		unlock_undo_log(ulog);
	}
	return;
}

void
undo_xlog_redo(XLogReaderState *record)
{
	uint8		info;
	void	   *xlrec;

	Assert(record != NULL);

	info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	xlrec = (void *) XLogRecGetData(record);

	elog(DEBUG5, "undo_xlog_redo: info = %u, lsn = %X/%X", info,
		 (uint32) (record->EndRecPtr >> 32), (uint32) record->EndRecPtr);

	switch (info)
	{
		case XLOG_UNDO_UNLINK:
			xlog_undo_unlink_redo((xl_undolog_unlink *) xlrec, record->EndRecPtr);
			break;
		case XLOG_UNDO_EXTEND:
			xlog_undo_extend_redo((xl_undolog_extend *) xlrec, record->EndRecPtr);
			break;
		case XLOG_UNDO_SLOT_UNLINK:
			xlog_undo_unlink_txnslot_redo((xl_undolog_unlink *) xlrec, record->EndRecPtr);
			break;
		case XLOG_UNDO_SLOT_EXTEND:
			xlog_undo_extend_txnslot_redo((xl_undolog_extend *) xlrec, record->EndRecPtr);
			break;
		case XLOG_UNDO_DISCARD:
			xlog_undo_discard_redo((xl_undolog_discard *) xlrec, record->EndRecPtr);
			break;
		case XLOG_UNDO_ROLLBACK_FINISH:
			{
				xl_undolog_rollback_finish *xlrf = (xl_undolog_rollback_finish *) xlrec;

				xlog_undo_rollback_finish_redo(record, xlrf->slotPtr, record->EndRecPtr);
				break;
			}
		default:
			ereport(PANIC, (errmsg("Unknown op code %u", info)));
	}
}

XLogRecPtr
xlog_undo_write(void *xlrec, uint8 type)
{
	XLogRecPtr	xlog_rec_ptr;

	Assert(xlrec != NULL);
	xlog_rec_ptr = InvalidXLogRecPtr;

	switch (type)
	{
		case XLOG_UNDO_EXTEND:
			XLogBeginInsert();
			XLogRegisterData((char *) xlrec, sizeof(xl_undolog_extend));
			xlog_rec_ptr = XLogInsert(RM_XUNDOLOG_ID, XLOG_UNDO_EXTEND);
			break;
		case XLOG_UNDO_UNLINK:
			XLogBeginInsert();
			XLogRegisterData((char *) xlrec, sizeof(xl_undolog_unlink));
			xlog_rec_ptr = XLogInsert(RM_XUNDOLOG_ID, XLOG_UNDO_UNLINK);
			break;
		case XLOG_UNDO_SLOT_EXTEND:
			XLogBeginInsert();
			XLogRegisterData((char *) xlrec, sizeof(xl_undolog_extend));
			xlog_rec_ptr = XLogInsert(RM_XUNDOLOG_ID, XLOG_UNDO_SLOT_EXTEND);
			break;
		case XLOG_UNDO_SLOT_UNLINK:
			XLogBeginInsert();
			XLogRegisterData((char *) xlrec, sizeof(xl_undolog_unlink));
			xlog_rec_ptr = XLogInsert(RM_XUNDOLOG_ID, XLOG_UNDO_SLOT_UNLINK);
			break;
		case XLOG_UNDO_DISCARD:
			XLogBeginInsert();
			XLogRegisterData((char *) xlrec, sizeof(xl_undolog_discard));
			xlog_rec_ptr = XLogInsert(RM_XUNDOLOG_ID, XLOG_UNDO_DISCARD);
			break;
		default:
			ereport(PANIC, (errmsg("Unknown type %u", type)));
	}
	elog(DEBUG5, "xlog_undo_write: type = %u, lsn = %X/%X", type,
		 (uint32) (xlog_rec_ptr >> 32), (uint32) xlog_rec_ptr);

	return xlog_rec_ptr;
}

void
xlog_undo_meta_write(xl_undo_meta *xlum)
{
	XLogRegisterData((char *) xlum, xlog_undo_meta_size(xlum));
}

void
xlog_undo_rollback_finish_redo(XLogReaderState *record, UndoSlotPtr slot_ptr, XLogRecPtr lsn)
{
	if (!is_skip_insert_slot(slot_ptr))
	{
		UndoSlotBuffer buf;
		Page		page;

		init_undo_slot_buffer(&buf);
		xlog_load_undo_slot_buffer(&buf, record, slot_ptr);
		if (BufferIsInvalid(buf.buffer))
			return;

		/* slot buffer must be in lock */
		page = BufferGetPage(buf.buffer);
		if (PageGetLSN(page) < lsn)
		{
			UndoSlot *slot = get_undo_slot_from_buffer(&buf, slot_ptr);

			slot->info |= UNDOSLOT_ROLLBACK;
			PageSetLSN(page, lsn);
			MarkBufferDirty(buf.buffer);
			elog(DEBUG5, "xlog_undo_rollback_finish_redo: slotPtr = %lX, lsn = %X/%X", slot_ptr,
				 (uint32) (lsn >> 32), (uint32) lsn);
		}
		UnlockReleaseBuffer(buf.buffer);
	}
	return;
}


XLogRecPtr
xlog_undo_rollback_finish_write(UndoSlotBuffer slot_buff, UndoSlotPtr slot_ptr)
{
	xl_undolog_rollback_finish xlrec;
	XLogRecPtr	lsn;

	xlrec.slotPtr = slot_ptr;
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, slot_buff.buffer, REGBUF_STANDARD);
	lsn = XLogInsert(RM_XUNDOLOG_ID, XLOG_UNDO_ROLLBACK_FINISH);
	elog(DEBUG5, "xlog_undo_rollback_finish_write: slotPtr = %lX, lsn = %X/%X", slot_ptr,
		 (uint32) (lsn >> 32), (uint32) lsn);
	return lsn;
}

/*
 * If the caller doesn't know the the block ID, but does know the RelFileLocator,
 * forknum and block number, then we search all registered blocks.  This is
 * expected to be a small number.
 */
XLogRedoAction
xlog_undo_read_buffer_for_redo(XLogReaderState *record,
							   RelFileLocator rlocator,
							   ForkNumber forknum,
							   BlockNumber blockno,
							   ReadBufferMode mode,
							   bool get_cleanup_lock,
							   Buffer *buf)
{
	int			i;

	for (i = 0; i <= XLogRecMaxBlockId(record); ++i)
	{
		DecodedBkpBlock *block = XLogRecGetBlock(record, i);

		if (block->in_use &&
			RelFileLocatorEquals(block->rlocator, rlocator) &&
			block->forknum == forknum &&
			block->blkno == blockno)
			return XLogReadBufferForRedoExtended(record,
												 i,
												 mode,
												 get_cleanup_lock,
												 buf);
	}

	elog(ERROR,
		 "could not find block ref rel %u/%u/%u, forknum = %u, block = %u",
		 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber, forknum, blockno);

	return 0;					/* silence compiler warnings */
}