/* -------------------------------------------------------------------------
 *
 * xlock.c
 * Implement the access interfaces of xstore inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * 
 * IDENTIFICATION
 * src/xheap/xlock.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xloginsert.h"
#include "storage/lmgr.h"
#include "undo/undolog.h"
#include "xheap/xheap.h"
#include "xheap/xpage.h"
#include "xheap/xredo.h"
#include "xheap/xlock.h"
#include "xheap/xheapam_visibility.h"
#include "xheap/xmulti.h"
#include "undo/undofetch.h"
#include "util/xrmgr.h"
#include "utils/elog.h"
#include "xheap/xtup_details.h"

static bool is_parent_hold_share_lock(uint16 flag, FullTransactionId locker_xid);
static XLogRecPtr log_xheap_lock(Relation rel, Buffer buffer, OffsetNumber offnum, FullTransactionId locker_xid, uint16 infomask);
static bool is_lock_mode_conflicting(LockTupleMode mode1, LockTupleMode mode2);

static bool
is_parent_hold_share_lock(uint16 flag, FullTransactionId locker_xid)
{
	if (IsSubTransaction())
	{
		if (XHeapTupleHasMultiLockers(flag))
		{
			if (XMultiXactIdIsCurrent(locker_xid, NULL))
				return true;
		}
		else if (XHEAP_XID_IS_SHR_LOCKED(flag))
		{
			if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(locker_xid)))
				return true;
		}
	}
	return false;
}

bool
validate_tuples_xact(Relation relation, Buffer buffer, Snapshot snapshot, XHeapTuple tuple,
			  FullTransactionId prior_xmax, bool nobuflock, bool keep_tup)
{
	XHeapTuple		visible_tup = NULL;
	XHeapDiskTupleData tup_hdr;
	ItemPointer		tid = &(tuple->ctid);
	OffsetNumber	offnum = ItemPointerGetOffsetNumber(tid);
	BlockNumber		blkno = ItemPointerGetBlockNumber(tid);
	bool			valid = false;
	XHeapTupleTransInfo xinfo;
	FullTransactionId last_xid;
	UndoRecPtr		urp;
	UndoTraversalState state;
	UnpackedUndoRecord *urec;
	UndoTraversalState rc;

	if (nobuflock)
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

	xheap_tuple_fetch(relation, buffer, offnum, snapshot, &visible_tup, NULL, keep_tup,
					  NULL, NULL, NULL, -1, NULL, NULL);

	if (visible_tup == NULL)
	{
		/*
		 * If the tuple is already removed by Rollbacks/pruning, then we don't
		 * need to proceed further.
		 */
		if (nobuflock)
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		return false;
	}

	last_xid = InvalidFullTransactionId;
	urp = INVALID_UNDO_REC_PTR;
	state = xheap_tuple_get_trans_info(buffer, offnum, &xinfo, NULL, &last_xid, &urp);
	if (state == UNDO_TRAVERSAL_ABORT)
	{
		int				ulog = (int) UNDO_PTR_GET_LOG_NO(urp);
		UndoLogControl *ucontrol = get_undo_log(ulog);

		elog(ERROR,
			"snapshot too old! maybe undo record has been discarded. we are doing EPQ. "
			"undo state %d, tuple flag %u. xid %lu, undoptr %lu. "
			"CurrentTransaction xid %lu, table oid %u, tid(%u, %u), lastXid %lu, "
			"globalrecyclexid %lu, globalFrozenXid %lu. "
			"ZoneInfo: urp: %lu, zid %d, insertURecPtr %lu, forceDiscardURecPtr %lu, "
			"discardURecPtr %lu, recycleXid %lu. xmin %u.",
			state, PtrGetVal(PtrGetVal(tuple, disk_tuple), flag), xinfo.xid.value,
			xinfo.urec_add, GetTopFullTransactionIdIfAny().value,
			PtrGetVal(tuple, table_oid), blkno, offnum, last_xid.value,
			pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid),
			pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid), urp, ulog,
			PtrFuncVal(ucontrol, UndoGetInsertURecPtr),
			PtrFuncVal(ucontrol, UndoGetForceDiscardURecPtr),
			PtrFuncVal(ucontrol, UndoGetDiscardURecPtr),
			UndoGetRecycleXid(ucontrol).value, PtrGetVal(snapshot, xmin));
	}

	if (FullTransactionIdEquals(prior_xmax, xinfo.xid))
	{
		valid = true;
		pfree(visible_tup);
		goto cleanup;
	}


	memcpy(&tup_hdr, visible_tup->disk_tuple, SizeOfXHeapDiskTupleData);
	pfree(visible_tup);

	xinfo.xid = InvalidFullTransactionId;

	urec = new_undo_record();
	rc = UNDO_TRAVERSAL_DEFAULT;

	do
	{
		last_xid = InvalidFullTransactionId;

		reset_undo_record(urec, xinfo.urec_add);
		rc = fetch_undo_record(urec, xinfo.xid,
							   false, &last_xid, satisfy_undo_record);
		if (rc == UNDO_TRAVERSAL_ABORT)
		{
			int				ulog = (int) UNDO_PTR_GET_LOG_NO(urec->uur_urp);
			UndoLogControl *ucontrol = get_undo_log(ulog);

			elog(ERROR,"snapshot too old! the undo record has been force discard. we are EPQ Internal. "
						"undo state %d, tuple flag %u. "
						"xid %lu, undoptr %lu. "
						"CurrentTansaction xid %lu, oid %u, tid(%u, %u), lastXid %lu, "
						"globalRecycleXid %lu, globalFrozenXid %lu. "
						"ZoneInfo: urp: %lu, zid %d, insertURecPtr %lu, "
						"forceDiscardURecPtr %lu, "
						"discardURecPtr %ld, recycleXid %lu. "
						"xmin %u.",
						rc, PtrGetVal(PtrGetVal(tuple, disk_tuple), flag),
						xinfo.xid.value, xinfo.urec_add, 
						GetTopFullTransactionIdIfAny().value, PtrGetVal(tuple, table_oid), blkno, offnum, last_xid.value,
						pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid),
						pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid),
						urec->uur_urp, ulog, PtrFuncVal(ucontrol, UndoGetInsertURecPtr),
						PtrFuncVal(ucontrol, UndoGetForceDiscardURecPtr),
						PtrFuncVal(ucontrol, UndoGetDiscardURecPtr),
						UndoGetRecycleXid(ucontrol).value,
						PtrGetVal(snapshot, xmin));
		}

		if (FullTransactionIdEquals(prior_xmax, GetUndoRecordXid(urec)))
		{
			valid = true;
			break;
		}

		xinfo.xid = GetUndoRecordOldXactId(urec);
		xinfo.urec_add = GetUndoRecordTpprev(urec);

	} while (xinfo.urec_add != 0);

	destroy_undo_record(urec);

cleanup:
	if (nobuflock)
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	return valid;
}

/*
 * log the xheap tuple lock to xlog, we should log it to xlog to advance nextXid, or 
 * an alloced xid may be alloced again after recovery from a crash. (see comment in heap_lock_tuple) 
 */
static XLogRecPtr
log_xheap_lock(Relation rel, Buffer buffer, OffsetNumber offnum, FullTransactionId locker_xid, uint16 infomask)
{
	XlXHeapLock xl_rec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);

	/* Caller should not call me on a non-WAL-logged relation */
	Assert(RelationNeedsWAL(rel));

	XLogBeginInsert();
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	xl_rec.locker_xid = locker_xid;
	xl_rec.infomask = infomask;
	xl_rec.offnum = offnum;
	XLogRegisterData((char *) &xl_rec, SizeOfXHeapLock);

	recptr = XLogInsert(RM_XHEAP_ID, XLOG_XHEAP_LOCK);

	PageSetLSN(page, recptr);
	return recptr;
}

/*
 * Callers: XHeapUpdate, XHeapLockTuple
 * This function will do locking, UNDO and WAL logging part.
 */
void
xheap_execute_lock_tuple(Relation relation, Buffer buffer, XHeapTuple xtuple,
						 LockTupleMode mode, RowPtr *rp)
{
	FullTransactionId locker_xid = InvalidFullTransactionId;
	FullTransactionId xid_on_tup = InvalidFullTransactionId;
	FullTransactionId curxid = InvalidFullTransactionId;

	uint16		oldinfomask = xtuple->disk_tuple->flag;
	uint16		newinfomask = 0;
	Page		page = BufferGetPage(buffer);

	if (mode == LockTupleExclusive)
	{
		if (is_parent_hold_share_lock(oldinfomask, XHeapTupleGetLockerXid(xtuple)))
			locker_xid = GetTopFullTransactionId();
		else
			locker_xid = GetCurrentFullTransactionId();
	}
	else if (mode == LockTupleShare)
	{
		xid_on_tup = XHeapTupleGetLockerXid(xtuple);
		curxid = GetCurrentFullTransactionId();
	}

	if (mode == LockTupleExclusive)
		newinfomask |= XHEAP_XID_EXCL_LOCK;
	else if (mode == LockTupleShare)
	{
		if (XHEAP_XID_IS_SHR_LOCKED(oldinfomask) &&
			xstore_transaction_id_is_in_progress(xid_on_tup))
		{
			XMultiXactId multixid;

			/* create a multixid */
			XMultiXactIdSetOldestMember();
			multixid = XMultiXactIdCreate(xid_on_tup, XMultiXactStatusForShare, curxid,
										  XMultiXactStatusForShare);
			locker_xid = multixid;
			newinfomask |= XHEAP_MULTI_LOCKERS;
			elog(DEBUG5, "locker %ld + locker %ld = multi %ld", curxid.value, xid_on_tup.value, locker_xid.value);
		}
		else if (XHeapTupleHasMultiLockers(oldinfomask))
		{
			XMultiXactId xmultixid;
			

			XMultiXactIdSetOldestMember();
			xmultixid = XMultiXactIdExpand((XMultiXactId) xid_on_tup, curxid,
										   MultiXactStatusForShare);

			locker_xid = xmultixid;
			newinfomask |= XHEAP_MULTI_LOCKERS;
			elog(DEBUG5, "locker %lu + multi %lu = multi %lu", curxid.value, xid_on_tup.value, locker_xid.value);
		}
		else
		{
			XMultiXactIdSetOldestMember();
			locker_xid = curxid;
			newinfomask |= XHEAP_XID_SHR_LOCK;
			elog(DEBUG5, "single shared locker %lu", locker_xid.value);
		}
	}
	else
		Assert(0);

	START_CRIT_SECTION();

	xtuple->disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);
	xtuple->disk_tuple_size = RowPtrGetLen(rp);
	xtuple->disk_tuple->flag &= ~XHEAP_LOCK_STATUS_MASK;
	XHeapTupleHeaderClearSingleLocker(xtuple->disk_tuple);
	xtuple->disk_tuple->flag |= newinfomask;
	xtuple->disk_tuple->locker_xid = locker_xid;
	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(relation))
		log_xheap_lock(relation, buffer, ItemPointerGetOffsetNumber(&xtuple->ctid), locker_xid, newinfomask);

	END_CRIT_SECTION();
}

bool
is_xtuple_locked_by_us(XHeapTuple xtuple, FullTransactionId xid, LockTupleMode mode)
{
	if (!FullTransactionIdIsNormal(xid))
		return false;

	if (XHeapTupleHasMultiLockers(xtuple->disk_tuple->flag))
	{
		if (mode == LockTupleShare &&
			XMultiXactIdIsCurrent(xtuple->disk_tuple->locker_xid, NULL))
			return true;
		else
			return false;
	}
	else if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
	{
		if (mode == LockTupleShare)
		{
			if (XHEAP_XID_IS_EXCL_LOCKED(xtuple->disk_tuple->flag) ||
				XHEAP_XID_IS_SHR_LOCKED(xtuple->disk_tuple->flag))
				return true;
		}
		else
		{
			Assert(mode == LockTupleExclusive);

			if (XHEAP_XID_IS_EXCL_LOCKED(xtuple->disk_tuple->flag))
				return true;
		}
	}

	return false;
}

TM_Result
xheap_lock_tuple(Relation relation, XHeapTuple tuple, CommandId cid, Snapshot snapshot,
				 LockTupleMode mode, LockWaitPolicy wait_policy, bool follow_updates,
				 TM_FailureData *tmfd, bool eval, bool on_conflict_update)
{
	RowPtr			*rp = NULL;
	XHeapTupleData	 xtuple;
	Page			 page;
	BlockNumber		 blkno;
	OffsetNumber	 offnum;
	ItemPointerData	 ctid;
	ItemPointer		 tid = &tuple->ctid;
	TM_Result		 result;
	FullTransactionId update_xid = InvalidFullTransactionId,
					 locker_xid = InvalidFullTransactionId;
	FullTransactionId modified_subxid = InvalidFullTransactionId;
	bool			 inplace_updated_or_locked = false;
	bool			 has_tup_lock = false;
	XHeapTupleTransInfo xinfo;
	bool			 multixid_self = false;
	int				 retry_times = 0;
	Buffer			 buffer;

	/* disable xstore skip locked*/
	if (wait_policy == LockWaitSkip)
		wait_policy = LockWaitBlock;

	Assert(wait_policy == LockWaitBlock || wait_policy == LockWaitError);

	if (mode == LockTupleNoKeyExclusive)
	{
		mode = LockTupleExclusive;
		ereport(
			DEBUG5,
			(errmsg("LockTupleNoKeyExclusive is not support for xstore, use LockTupleExclusive instead.")));
	}

	if (mode == LockTupleKeyShare)
	{
		mode = LockTupleShare;
		ereport(
			DEBUG5,
			(errmsg("LockTupleKeyShare is not support for xstore, use LockTupleShare instead.")));
	}

	blkno = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, blkno);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buffer);

	offnum = ItemPointerGetOffsetNumber(tid);
	rp = XPageGetRowPtr(page, offnum);

check_tup_satisfies_update:
	result = xheap_tuple_satisfies_update(relation, snapshot, tid, &xtuple, cid, buffer,
										  &ctid, &xinfo, &modified_subxid, &locker_xid, eval,
										  multixid_self, &inplace_updated_or_locked);
	update_xid = xinfo.xid;
	multixid_self = false;

	if (result == TM_Invisible)
	{
		if (on_conflict_update)
		{
			xheap_copy_tuple_with_buffer(&xtuple, tuple);
			goto cleanup;
		}
		UnlockReleaseBuffer(buffer);
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("attempted to lock invisible tuple")));
	}
	else if (result == TM_BeingModified || result == TM_Ok)
	{
		bool already_locked = false;

		ereport(DEBUG5, (errmsg("xheap_lock_tuple returned %d", result)));

		already_locked = is_xtuple_locked_by_us(&xtuple, locker_xid, mode);

		if (!already_locked && TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xinfo.xid)))
			already_locked = true;

		if (already_locked)
		{
			xheap_copy_tuple_with_buffer(&xtuple, tuple);
			result = TM_Ok;
			goto cleanup;
		}

		if (result != TM_Ok)
		{
			/* wait for remaining updater/locker to terminate */
			XHeapWaitInfo wait_info = {
				.disk_tuple_modified_xid = xtuple.disk_tuple->modified_xid,
				.disk_tuple_locker_xid = xtuple.disk_tuple->locker_xid,
				.disk_tuple_urec = xtuple.disk_tuple->urec,
				.ctid = xtuple.ctid,
				.disk_tuple_flag = xtuple.disk_tuple->flag
			};

			if (!xheap_wait_helper(relation, buffer, &wait_info, mode, wait_policy, update_xid,
								   locker_xid, modified_subxid, &has_tup_lock, &multixid_self))
			{
				LimitRetryTimes(retry_times++);
				goto check_tup_satisfies_update;
			}
		}

		result = TM_Ok;
	}
	else if ((result == TM_Updated && xtuple.disk_tuple != NULL))
	{
		xheap_copy_tuple_with_buffer(&xtuple, tuple);
		ereport(DEBUG5, (errmsg("xheap_lock_tuple returned %d tuple ctid (%u:%u)", result,
								ItemPointerGetBlockNumber(&tuple->ctid),
								ItemPointerGetOffsetNumber(&tuple->ctid))));
	}
	else
		ereport(DEBUG5, (errmsg("xheap_lock_tuple returned %d", result)));


	rp = XPageGetRowPtr(page, offnum);
	if (result != TM_Ok)
	{
		tmfd->in_place_updated_or_locked = inplace_updated_or_locked;

		if (XHeapTupleIsMoved(xtuple.disk_tuple->flag))
			ItemPointerSetMovedPartitions(&tmfd->ctid);
		else
			tmfd->ctid = ctid;

		tmfd->epoch = EpochFromFullTransactionId(update_xid);
		tmfd->xmax = XidFromFullTransactionId(update_xid);

		if (result == TM_SelfModified)
			tmfd->cmax = xinfo.cid;
		else
			tmfd->cmax = InvalidCommandId;

		goto cleanup;
	}

	Assert(RowPtrIsNormal(rp));
	xtuple.disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);
	xtuple.disk_tuple_size = RowPtrGetLen(rp);
	xtuple.ctid = *tid;

	(void) xheap_execute_lock_tuple(relation, buffer, &xtuple, mode, rp);

	xheap_copy_tuple_with_buffer(&xtuple, tuple);

cleanup:
	if (result == TM_Updated && !inplace_updated_or_locked &&
		ItemPointerEquals(&tmfd->ctid, &tuple->ctid))
		result = TM_Deleted;


	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (has_tup_lock)
		UnlockTuple(relation, tid, (mode == LockTupleShare) ? ShareLock : ExclusiveLock);
	ReleaseBuffer(buffer);
	return result;
}

static bool
is_lock_mode_conflicting(LockTupleMode mode1, LockTupleMode mode2)
{
	if (mode1 == LockTupleShare)
		return mode2 == LockTupleExclusive;
	else
		return true;
}

/*
 * xheap_wait
 * helper function called by XHeapUpdate, XHeapDelete, XHeapLockTuple
 * to check write-write  conflict
 * - if needs recheck, return false
 * - otherwise, return true
 *
 * mode - delete/update/select-for-update => LockTupleExclusive
 * select-for-share                => LockTupleShared
 */
bool
xheap_wait_helper(Relation relation, Buffer buffer, XHeapWaitInfo *wait_info, LockTupleMode mode,
				  LockWaitPolicy wait_policy, FullTransactionId modified_xid, FullTransactionId locker_xid,
				  FullTransactionId modified_subxid, bool *has_tup_lock, bool *multixid_self)
{
	uint16		flag = wait_info->disk_tuple_flag;
	bool		modifie_xid_running = (!xstore_transaction_id_did_commit(modified_xid));
	LockTupleMode cur_mode =
		(XHEAP_XID_IS_EXCL_LOCKED(flag) || modifie_xid_running) ? LockTupleExclusive : LockTupleShare;
	LOCKMODE	tuple_lock_type = (mode == LockTupleShare) ? ShareLock : ExclusiveLock;
	bool		is_lock_single_locker =
		XHEAP_XID_IS_EXCL_LOCKED(wait_info->disk_tuple_flag) ||
		XHEAP_XID_IS_SHR_LOCKED(wait_info->disk_tuple_flag);

	Assert(wait_policy == LockWaitBlock || wait_policy == LockWaitError);

	if (XHeapTupleHasMultiLockers(wait_info->disk_tuple_flag))
	{
		XMultiXactId xwait;
		Page		page;
		OffsetNumber offnum;
		RowPtr	   *rp;
		FullTransactionId previous_xid;
		XHeapDiskTuple disk_tuple;

		Assert(cur_mode == LockTupleShare);

		if (!is_lock_mode_conflicting(cur_mode, mode))
			return true;

		xwait = wait_info->disk_tuple_locker_xid;
		previous_xid = wait_info->disk_tuple_modified_xid;

		elog(DEBUG5,
			 "curxid %lu, xheaptuple(flag=%d, tid(%d,%d)), wait xmultixid = %ld, xid is %ld",
			 GetTopFullTransactionId().value, wait_info->disk_tuple_flag,
			 ItemPointerGetBlockNumber(&(wait_info->ctid)),
			 ItemPointerGetOffsetNumber(&(wait_info->ctid)), xwait.value, wait_info->disk_tuple_modified_xid.value);

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		if (!(*has_tup_lock))
		{
			if (wait_policy == LockWaitError)
			{
				if (!ConditionalLockTuple(relation, &(wait_info->ctid), tuple_lock_type))
					ereport(ERROR,
							(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
							 errmsg("could not obtain lock on row in relation \"%s\"",
									RelationGetRelationName(relation))));
			}
			else
				LockTuple(relation, &(wait_info->ctid), tuple_lock_type);

			*has_tup_lock = true;
		}

		/* wait multixid */
		if (wait_policy == LockWaitError)
		{
			if (!ConditionalXMultiXactIdWait(xwait,
											 get_xmxact_status_for_lock(mode, false)))
				ereport(ERROR, (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
								errmsg("could not obtain lock on row in relation \"%s\"",
									   RelationGetRelationName(relation))));
		}
		else
			XMultiXactIdWait(xwait, get_xmxact_status_for_lock(mode, false));

		/* reacquire lock */
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

		page = BufferGetPage(buffer);
		offnum = ItemPointerGetOffsetNumber(&wait_info->ctid);
		rp = XPageGetRowPtr(page, offnum);

		disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);

		if (XHeapTupleHasMultiLockers(disk_tuple->flag) &&
			FullTransactionIdEquals(previous_xid, disk_tuple->modified_xid) &&
			FullTransactionIdEquals(xwait, disk_tuple->locker_xid))
		{
			if (XMultiXactIdIsCurrent(xwait, NULL))
				*multixid_self = true;
			else
			{
				disk_tuple->flag &= ~XHEAP_MULTI_LOCKERS;
				disk_tuple->locker_xid = InvalidFullTransactionId;
			}
		}

		return false;
	}
	else if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(modified_xid)) ||
			 TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(locker_xid)))
		return true;
	else
	{
		bool			is_sub_xact = false;
		FullTransactionId top_xid;

		Assert(FullTransactionIdIsValid(modified_xid) || FullTransactionIdIsValid(locker_xid));
		if (is_lock_mode_conflicting(cur_mode, mode))
		{
			uint16 infomask = wait_info->disk_tuple_flag;

			/*
			 * Wait for the transaction to end. But obtain the tuple lock so we can maintain our priority.
			 * release the buffer lock while waiting to avoid deadlock.
			 */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

			if (!(*has_tup_lock))
			{
				if (wait_policy == LockWaitError)
				{
					if (!ConditionalLockTuple(relation, &(wait_info->ctid), tuple_lock_type))
						ereport(ERROR,
								(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
								 errmsg("could not obtain lock on row in relation \"%s\"",
										RelationGetRelationName(relation))));
				}
				else
					LockTuple(relation, &(wait_info->ctid), tuple_lock_type);

				*has_tup_lock = true;
			}

			/* Figure out which xid to wait for and wait for it to finish */
			top_xid = FullTransactionIdIsValid(locker_xid) ? locker_xid : modified_xid;
			elog(DEBUG5,
				 "cur top xid %lu, xheaptuple(flag=%u, tid(%u,%u)), xheap_wait (xid = %lu, "
				 "subxid = %lu)",
				 GetTopFullTransactionId().value, infomask,
				 ItemPointerGetBlockNumber(&wait_info->ctid),
				 ItemPointerGetOffsetNumber(&wait_info->ctid), top_xid.value, modified_subxid.value);

			if (wait_policy == LockWaitError)
			{
				if (TransactionIdIsValid(modified_subxid.value))
				{
					is_sub_xact = true;
					if (!ConditionalXactLockTableWait(XidFromFullTransactionId(modified_subxid)))
						ereport(ERROR,
								(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
								 errmsg("could not obtain lock on row in relation \"%s\"",
										RelationGetRelationName(relation))));
				}
				else
				{
					if (!ConditionalXactLockTableWait(XidFromFullTransactionId(top_xid)))
						ereport(ERROR,
								(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
								 errmsg("could not obtain lock on row in relation \"%s\"",
										RelationGetRelationName(relation))));
				}
			}
			else
			{
				if (TransactionIdIsValid(modified_subxid.value))
				{
					is_sub_xact = true;
					XactLockTableWait(XidFromFullTransactionId(modified_subxid), NULL, NULL, XLTW_None);
				}
				else
					XactLockTableWait(XidFromFullTransactionId(top_xid), NULL, NULL, XLTW_None);
			}

			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/* a single locker only transaction shouldn't exec pending undo */
			if (!is_sub_xact && !is_lock_single_locker && FullTransactionIdIsValid(top_xid) &&
				!xstore_transaction_id_did_commit(top_xid))
				xheap_exec_pending_undo_actions(relation, buffer, top_xid, wait_info->disk_tuple_urec);

			return false;
		}
		else
			return true;
	}

	return false;	/* shouldn't reach here */
}