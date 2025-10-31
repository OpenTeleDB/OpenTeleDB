/* -------------------------------------------------------------------------
 *
 * xheapam_visibility.h.c
 *	  Tuple visibility interfaces of xstore inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/xheap/xheapam_visibility.h.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "storage/buf.h"
#include "storage/itemptr.h"
#include "storage/off.h"
#include "undo/undorecord.h"
#include "undo/undofetch.h"
#include "utils/snapshot.h"
#include "xstore.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "utils/datum.h"
#include "utils/snapmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/predicate.h"
#include "storage/lmgr.h"
#include "access/clog.h"
#include "access/xact.h"
#include "access/transam.h"
#include "access/commit_ts.h"
#include "xheap/xheap.h"
#include "xheap/xpage.h"
#include "xheap/xtuple.h"
#include "xheap/xhio.h"
#include "xheap/xheapam_visibility.h"
#include "util/xxact.h"
#include "undo/undolog.h"
#include "undo/undotxn.h"
#include "postmaster/autovacuum.h"

#define SetXTupleXminXmax(tupleXmin, tupleXmax, xtuple) \
	do { \
		if ((xtuple) != NULL && (*(xtuple) != NULL)) \
		{ \
			(*(xtuple))->xmin = (tupleXmin); \
			(*(xtuple))->xmax = (tupleXmax); \
		} \
	} while (0)

static XTupleTidOp xheap_tid_op_from_infomask(uint16 infomask);
static XVersionSelector xheap_tuple_satisfies(XHeapTuple xtup, Buffer buffer, XTupleTidOp op, Snapshot snapshot,
											  XHeapTupleTransInfo *xinfo);
static XVersionSelector xheap_select_version_mvcc(XHeapTuple xtup, Buffer buffer, XTupleTidOp op, 
												FullTransactionId xid, Snapshot snapshot);
static XVersionSelector xheap_select_version_self_transaction(XHeapTuple tuple, Buffer buffer, XTupleTidOp op, FullTransactionId xid);
static XVersionSelector xheap_select_version_now(XHeapTuple tuple, Buffer buffer, XTupleTidOp op, FullTransactionId xid);
static XVersionSelector xheap_select_version_not_self(XHeapTuple tuple, Buffer buffer, XTupleTidOp op, FullTransactionId xid);
static XVersionSelector xheap_check_cid(XTupleTidOp op, CommandId tuple_cid,
									  CommandId visibility_cid, bool *has_cur_xact_write);
static XVersionSelector xheap_select_version_self(XHeapTuple tuple, Buffer buffer, XTupleTidOp op, FullTransactionId xid);
static XVersionSelector xheap_select_version_dirty(XHeapTuple tuple, Buffer buffer, XTupleTidOp op,
												const XHeapTupleTransInfo *uinfo, Snapshot snapshot);
static XVersionSelector xheap_select_version_update(XTupleTidOp op, FullTransactionId xid,
													CommandId visibility_cid);

static UndoTraversalState get_tuple_from_undo_record(
	UndoRecPtr urec_ptr, FullTransactionId xid, Buffer buffer, OffsetNumber offnum,
	XHeapDiskTuple hdr, XHeapTuple *tuple, bool *free_tuple, XHeapTupleTransInfo *uinfo,
	ItemPointer ctid, FullTransactionId *last_xid, UndoRecPtr *urp);
static bool get_tuple_from_undo(UndoRecPtr urec_add, XHeapTuple current_tuple,
							 XHeapTuple *visible_tuple, Snapshot snapshot,
							 CommandId curcid, Buffer buffer, OffsetNumber offnum,
							 ItemPointer ctid);

typedef enum
{
	XHEAPTUPLESTATUS_LOCKED,
	XHEAPTUPLESTATUS_MULTI_LOCKED,
	XHEAPTUPLESTATUS_INPLACE_UPDATED,
	XHEAPTUPLESTATUS_DELETED,
	XHEAPTUPLESTATUS_INSERTED
} XHeapTupleStatus;

static XHeapTupleStatus
xheap_tuple_get_status(const XHeapTuple xtup)
{
	XHeapDiskTuple xtuple = xtup->disk_tuple;
	uint16		infomask = xtuple->flag;

	if (XHeapTupleHasMultiLockers(infomask))
		return XHEAPTUPLESTATUS_MULTI_LOCKED;
	else if ((XHEAP_XID_IS_EXCL_LOCKED(infomask) ||
			  XHEAP_XID_IS_SHR_LOCKED(infomask)))
		return XHEAPTUPLESTATUS_LOCKED; /* locked by select-for-update or select-for-share */
	else if (infomask & XHEAP_INPLACE_UPDATED)
		return XHEAPTUPLESTATUS_INPLACE_UPDATED;	/* modified or locked by lock-for-update */
	else if ((infomask & (XHEAP_UPDATED | XHEAP_DELETED)) != 0)
		return XHEAPTUPLESTATUS_DELETED;

	return XHEAPTUPLESTATUS_INSERTED;
}

/*
 * xheap_tuple_satisfies
 *
 * Determine whether (a) the current version of the tuple is visible to the
 * snapshot, (b) no version of the tuple is visible to the snapshot, or
 * (c) the previous version of the tuple should be looked up into the undo
 * log to determine which version, if any, is visible.
 *
 * This function can only handle certain types of snapshots; it is a helper
 * function for xheap_tuple_fetch, not a general-purpose facility.
 */
static XVersionSelector
xheap_tuple_satisfies(XHeapTuple xtup, Buffer buffer, XTupleTidOp op, Snapshot snapshot, XHeapTupleTransInfo *xinfo)
{
	XVersionSelector selector = XVERSION_NONE;

	if (TransactionIdOlderThanAllUndo(xinfo->xid))
	{
		/*
		 * The tuple is not associated with a transaction slot that is new
		 * enough to matter, so all changes previously made to the tuple are
		 * now all-visible.  If the last operation performed was a delete or a
		 * non-inplace update, the tuple is now effectively gone; if it was an
		 * insert or an inplace update, use the current version.
		 */
		selector = (op == XTUPLETID_GONE) ? XVERSION_NONE : XVERSION_CURRENT;
	}
	else if (snapshot->snapshot_type == SNAPSHOT_MVCC || IsMVCCSnapshot(snapshot))
	{
		/*
		 * NOTICE: We distinguish and SNAPSHOT_MVCC
		 * in xheap_select_version_mvcc codes.
		 */
		selector = xheap_select_version_mvcc(xtup, buffer, op, xinfo->xid, snapshot);
	}
	else if (snapshot->snapshot_type == SNAPSHOT_SELF_TRANSACTION)
		selector = xheap_select_version_self_transaction(xtup, buffer, op, xinfo->xid);
	else if (snapshot->snapshot_type == SNAPSHOT_SELF)
		selector = xheap_select_version_self(xtup, buffer, op, xinfo->xid);
	else if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
		selector = xheap_select_version_dirty(xtup, buffer, op, xinfo, snapshot);
	else if (snapshot->snapshot_type == SNAPSHOT_TOAST)
	{
		/* 
		 * a toast tuple is pruned and rp is marked as deleted,
		 */
		selector = XVERSION_CURRENT;
	}
	else if (snapshot->snapshot_type == SNAPSHOT_NOW)
		selector = xheap_select_version_now(xtup, buffer, op, xinfo->xid);
	else if (snapshot->snapshot_type == SNAPSHOT_NOT_SELF)
		selector = xheap_select_version_not_self(xtup, buffer, op, xinfo->xid);
	else
		elog(ERROR, "unsupported snapshot style %d", (int) snapshot->snapshot_type);

	return selector;
}

/*
 * xheap_tuple_satisfies_visibility
 *		True iff xheap tuple satisfies a time qual.
 *
 * Notes: Assumes xheap tuple is valid, and buffer at least share locked.
 *
 */
bool
xheap_tuple_satisfies_visibility(XHeapTuple xhtup, Snapshot snapshot, Buffer buffer)
{
	XHeapTupleData	xtupledata;
	XHeapTuple		xtuple;
	XVersionSelector xheapselect = XVERSION_NONE;
	FullTransactionId tupXid = InvalidFullTransactionId;
	Page			dp;
	XHeapTupleTransInfo xinfo;
	bool			have_trans_info = false;
	UndoTraversalState state = UNDO_TRAVERSAL_DEFAULT;
	OffsetNumber	offnum;
	RowPtr		   *rp;
	BlockNumber		blockno;
	XTupleTidOp		op;

	Assert(xhtup != NULL);
	if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
		snapshot->xmin = snapshot->xmax = InvalidTransactionId;

	if (snapshot->snapshot_type == SNAPSHOT_ANY ||
		snapshot->snapshot_type == SNAPSHOT_TOAST)
		return true;

	xtuple = &xtupledata;
	ItemPointerCopy(&xhtup->ctid, &xtuple->ctid);
	dp = BufferGetPage(buffer);
	offnum = ItemPointerGetOffsetNumber(&xhtup->ctid);
	if (offnum > xheap_page_get_max_offset_number(dp))
		ereport(PANIC,
				(errmsg("the number of tuples in page is %hu, exceeds the maximum count of page.",
						offnum)));

	rp = XPageGetRowPtr(dp, offnum);
	blockno = BufferGetBlockNumber(buffer);

	/* In new version of xstore, deleted tuple will remain until it becomes surely dead for all backends*/
	if (RowPtrIsNormal(rp))
	{
		xtuple->disk_tuple = (XHeapDiskTuple) XPageGetRowData(dp, rp);
		xtuple->disk_tuple_size = RowPtrGetLen(rp);
		tupXid = XHeapTupleGetModifiedXid(xhtup);
	}
	else
	{
		/*
		 * If this RowPtr is neither normal nor dead, it must be unused.  In
		 * that case, there is no version of the tuple visible here, so we can
		 * exit quickly.
		 */
		Assert(!RowPtrIsUsed(rp));

		return false;
	}

	Assert(xtuple != NULL);

	op = xheap_tid_op_from_infomask(xtuple->disk_tuple->flag);

	/* XHeapTupleTransInfo is just used for compatibility with old code */
	xinfo.cid = InvalidCommandId;
	xinfo.xid = tupXid;
	xinfo.urec_add = xtuple->disk_tuple->urec;

	xheapselect = xheap_tuple_satisfies(xtuple, buffer, op, snapshot, &xinfo);
	if (snapshot->snapshot_type == SNAPSHOT_DIRTY &&
		(TransactionIdIsValid(snapshot->xmin) || TransactionIdIsValid(snapshot->xmax)))
	{
		/*
		 * Get the subxid when caller requests to do so. In Xstore, SnapshotDirty.xmin and SnapshotDirty.xmax are both the top tansaction's xid
		 * aftern returning from xheap_select_version_dirty. We must check the undo whether the tuple was modified by a sub transaction and set 
		 * the sub transaction's xid to SnapshotDirty if it was.
		 */
		FullTransactionId subxid = InvalidFullTransactionId;

		if (TransactionIdIsValid(snapshot->xmin))
		{
			state = fetch_subxid_from_undo(xinfo.urec_add, &subxid);
			if (state == UNDO_TRAVERSAL_COMPLETE && FullTransactionIdIsValid(subxid))
				snapshot->xmin = XidFromFullTransactionId(subxid);
		}
		else
		{
			state = fetch_subxid_from_undo(xinfo.urec_add, &subxid);
			if (state == UNDO_TRAVERSAL_COMPLETE && FullTransactionIdIsValid(subxid))
				snapshot->xmax = XidFromFullTransactionId(subxid);
		}
	}

	if (xheapselect == XVERSION_CHECK_CID)
	{
		if (!have_trans_info)
		{
			state = fetch_transinfo_from_undo(
				xinfo.urec_add, blockno, ItemPointerGetOffsetNumber(&xtuple->ctid), InvalidFullTransactionId,
				&xinfo.cid, NULL, false, NULL, NULL);

			Assert(state != UNDO_TRAVERSAL_ABORT);
			have_trans_info = true;
		}

		if (xinfo.cid == InvalidCommandId)
			ereport(PANIC,
					(errmsg("invalid cid! "
							"LogInfo: undo state %d, tuple flag %u, tupXid %lu. "
							"TransInfo: current xid %lu, oid %u, undo ptr:%lu, tid(%u, %u). globalrecyclexid %lu. "
							"Snapshot: type %d, xmin %u.",
							state, xtuple->disk_tuple->flag, tupXid.value, 
							GetTopFullTransactionIdIfAny().value, xtuple->table_oid, xtuple->disk_tuple->urec,
							blockno, offnum, pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid),
							snapshot->snapshot_type, snapshot->xmin)));

		xheapselect = xheap_check_cid(op, xinfo.cid, snapshot->curcid, NULL);
	}

	if (xheapselect == XVERSION_OLDER || xheapselect == XVERSION_NONE)
		return false;

	return true;
}

static inline bool
xheap_get_hint(uint16 infomask, int *hint_status)
{
	if (XHEAP_MODIFIED_XID_IS_COMMITTED(infomask))
	{
		*hint_status = TRANSACTION_STATUS_COMMITTED;
		return true;
	}
	if (XHEAP_MODIFIED_XID_IS_INVALID(infomask))
	{
		*hint_status = TRANSACTION_STATUS_ABORTED;
		return true;
	}
	*hint_status = TRANSACTION_STATUS_IN_PROGRESS;
	return false;
}

static inline void
xheap_set_hint(int hinted, Buffer buffer, OffsetNumber offset, int hint_status)
{
	if (!hinted)
	{
		Page		dp;
		RowPtr	   *rp;
		XHeapDiskTuple tup;

		dp = BufferGetPage(buffer);
		rp = XPageGetRowPtr(dp, offset);
		tup = (XHeapDiskTuple) XPageGetRowData(dp, rp);
		if (hint_status == TRANSACTION_STATUS_COMMITTED)
		{
			if (!XHEAP_MODIFIED_XID_IS_COMMITTED(tup->flag))
				tup->flag = tup->flag | XHEAP_MODIFIED_XID_COMMITTED;
		}
		if (hint_status == TRANSACTION_STATUS_ABORTED)
		{
			if (!XHEAP_MODIFIED_XID_IS_INVALID(tup->flag))
				tup->flag = tup->flag | XHEAP_MODIFIED_XID_INVALID;
		}
	}
}

/*
 * xheap_select_version_mvcc
 *
 * Decide, for a given MVCC snapshot, whether we should return the current
 * version of a tuple, an older version, or no version at all.  We only have
 * the XID available here, so if the CID turns out to be relevant, we must
 * return XVERSION_CHECK_CID; caller is responsible for calling XHeapCheckCID
 * with the appropriate CID to obtain a final answer.
 * xid must be greater than global fronzed xid.
 */
static XVersionSelector
xheap_select_version_mvcc(XHeapTuple tuple, Buffer buffer, XTupleTidOp op, FullTransactionId xid, Snapshot snapshot)
{
	bool		hinted = false;
	int			hint_status = TRANSACTION_STATUS_IN_PROGRESS;

	if (tuple != NULL)
		hinted = xheap_get_hint(tuple->disk_tuple->flag, &hint_status);

	/* IMPORTANT: Version snapshot is independent of the current transaction. */
	if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
	{
		/*
		 * This transaction is still running and belongs to the current
		 * session.  If the current CID has been used to stamp a tuple or the
		 * snapshot belongs to an older CID, then we need the CID for this
		 * tuple to make a final visibility decision.
		 */
		if (GetCurrentCommandIdUsed() ||
			GetCurrentCommandId(false) != snapshot->curcid)
			return XVERSION_CHECK_CID;

		/* Nothing has changed since our scan started. */
		return ((op == XTUPLETID_GONE) ? XVERSION_NONE : XVERSION_CURRENT);
	}

	if (xid_visible_in_snapshot(xid, snapshot, &hint_status))
	{
		if (tuple != NULL)
			xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
		/* The XID is visible to us. */
		return ((op == XTUPLETID_GONE) ? XVERSION_NONE : XVERSION_CURRENT);
	}
	else
	{
		if (tuple != NULL)
			xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
		/*
		 * The XID is not visible to us, either because it aborted or because
		 * it's in our MVCC snapshot.  If this is a new tuple, that means we
		 * can't see it at all; otherwise, we need to check older versions.
		 */
		return ((op == XTUPLETID_NEW) ? XVERSION_NONE : XVERSION_OLDER);
	}
}

/*
 * xheap_select_version_self_transaction
 *		xid must be greater than global fronzed xid.
 */
static XVersionSelector
xheap_select_version_self_transaction(XHeapTuple tuple, Buffer buffer, XTupleTidOp op, FullTransactionId xid)
{
	bool		hinted = false;
	int			hint_status = TRANSACTION_STATUS_IN_PROGRESS;

	if (tuple != NULL)
		hinted = xheap_get_hint(tuple->disk_tuple->flag, &hint_status);

	if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
	{
		/*
		 * This transaction is still running and belongs to the current
		 * session. SnapshotNow always attached with current Command Id.
		 * Nothing has changed since our scan started.
		 */
		return ((op == XTUPLETID_GONE) ? XVERSION_NONE : XVERSION_CURRENT);
	}

	if (!transaction_id_did_commit_with_hint(xid, &hint_status))
	{
		if (tuple != NULL)
			xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
		/* The XID is not visible to us */
		return ((op == XTUPLETID_NEW) ? XVERSION_NONE : XVERSION_OLDER);
	}
	else
	{
		if (tuple != NULL)
			xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
		/* The XID is visible to us. */
		return ((op == XTUPLETID_GONE) ? XVERSION_NONE : XVERSION_CURRENT);
	}
}

static XVersionSelector
xheap_select_version_now(XHeapTuple tuple, Buffer buffer, XTupleTidOp op, FullTransactionId xid)
{
	bool		hinted = false;
	int			hint_status = TRANSACTION_STATUS_IN_PROGRESS;

	if (tuple != NULL)
		hinted = xheap_get_hint(tuple->disk_tuple->flag, &hint_status);

	if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
		return XVERSION_CHECK_CID;

	if (!transaction_id_did_commit_with_hint(xid, &hint_status))
	{
		if (tuple != NULL)
			xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
		/* The XID is not visible to us */
		return ((op == XTUPLETID_NEW) ? XVERSION_NONE : XVERSION_OLDER);
	}
	else
	{
		if (tuple != NULL)
			xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
		/* The XID is visible to us. */
		return ((op == XTUPLETID_GONE) ? XVERSION_NONE : XVERSION_CURRENT);
	}
}

static XVersionSelector
xheap_select_version_not_self(XHeapTuple tuple, Buffer buffer, XTupleTidOp op, FullTransactionId xid)
{
	bool		hinted = false;
	int			hint_status = TRANSACTION_STATUS_IN_PROGRESS;

	if (tuple != NULL)
		hinted = xheap_get_hint(tuple->disk_tuple->flag, &hint_status);

	if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
		/* We can't see anything of our own transaction. */
		return ((op == XTUPLETID_GONE) ? XVERSION_CURRENT : XVERSION_OLDER);

	if (!transaction_id_did_commit_with_hint(xid, &hint_status))
	{
		if (tuple != NULL)
			xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
		/* The XID is not visible to us */
		return ((op == XTUPLETID_NEW) ? XVERSION_NONE : XVERSION_OLDER);
	}
	else
	{
		if (tuple != NULL)
			xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
		/* The XID is visible to us. */
		return ((op == XTUPLETID_GONE) ? XVERSION_NONE : XVERSION_CURRENT);
	}
}

/*
 * xheap_select_version_self
 *
 * Decide, using SnapshotSelf visibility rules, whether we should return the
 * current version of a tuple, an older version, or no version at all.
 * xid must be greater than global fronzed xid.
 */
static XVersionSelector
xheap_select_version_self(XHeapTuple tuple, Buffer buffer, XTupleTidOp op, FullTransactionId xid)
{
	bool		hinted = false;
	int			hint_status = TRANSACTION_STATUS_IN_PROGRESS;

	if (tuple != NULL)
		hinted = xheap_get_hint(tuple->disk_tuple->flag, &hint_status);
	if (op == XTUPLETID_GONE)
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
			return XVERSION_NONE;
		else if (xstore_transaction_id_is_in_progress(xid))
			return XVERSION_OLDER;
		else if (transaction_id_did_commit_with_hint(xid, &hint_status))
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_NONE;
		}
		else
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_OLDER;	/* transaction is aborted */
		}
	}
	else if (op == XTUPLETID_MODIFIED)
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
			return XVERSION_CURRENT;
		else if (xstore_transaction_id_is_in_progress(xid))
			return XVERSION_OLDER;
		else if (transaction_id_did_commit_with_hint(xid, &hint_status))
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_CURRENT;
		}
		else
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_OLDER;	/* transaction is aborted */
		}
	}
	else
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
			return XVERSION_CURRENT;
		else if (xstore_transaction_id_is_in_progress(xid))
			return XVERSION_NONE;
		else if (transaction_id_did_commit_with_hint(xid, &hint_status))
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_CURRENT;
		}
		else
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_NONE;	/* transaction is aborted */
		}
	}

	/* should never get here */
	pg_unreachable();
}

/*
 * xheap_select_version_dirty
 *
 * Returns the visible version of tuple (including effects of open
 * transactions) if any, NULL otherwise.
 *
 * Here, we consider the effects of: all committed and in-progress transactions (as of the current instant)
 * previous commands of this transaction
 * changes made by the current command
 *
 * This is essentially like InplaceHeapTupleSatisfiesSelf as far as effects of
 * the current transaction and committed/aborted xacts are concerned.
 * However, we also include the effects of other xacts still in progress.
 *
 * The tuple will be considered visible iff: (a) Latest operation on tuple is Delete or non-inplace-update and the
 * current transaction is in progress.
 *										   (b) Latest operation on tuple is Insert, In-Place update or tuple is
 * locked and the transaction that has performed operation is current
 * transaction or is in-progress or is committed.
 */

static XVersionSelector
xheap_select_version_dirty(XHeapTuple tuple, Buffer buffer, const XTupleTidOp op, const XHeapTupleTransInfo *xinfo, Snapshot snapshot)
{
	bool		hinted = false;
	int			hint_status = TRANSACTION_STATUS_IN_PROGRESS;

	if (tuple != NULL)
		hinted = xheap_get_hint(tuple->disk_tuple->flag, &hint_status);
	snapshot->xmin = snapshot->xmax = InvalidTransactionId;
	snapshot->speculativeToken = 0;
	if (op == XTUPLETID_GONE)
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xinfo->xid)))
			return XVERSION_NONE;
		else if (xstore_transaction_id_is_in_progress(xinfo->xid))
		{
			snapshot->xmax = XidFromFullTransactionId(xinfo->xid);
			return XVERSION_CURRENT;
		}
		else if (transaction_id_did_commit_with_hint(xinfo->xid, &hint_status))
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			/* tuple is deleted or non-inplace-updated */
			return XVERSION_NONE;
		}
		else
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			/* transaction is aborted */
			return XVERSION_OLDER;
		}
	}
	else if (op == XTUPLETID_MODIFIED)
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xinfo->xid)))
			return XVERSION_CURRENT;
		else if (xstore_transaction_id_is_in_progress(xinfo->xid))
		{
			snapshot->xmax = XidFromFullTransactionId(xinfo->xid);
			return XVERSION_CURRENT;	/* being updated */
		}
		else if (transaction_id_did_commit_with_hint(xinfo->xid, &hint_status))
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_CURRENT;	/* tuple is updated by someone else */
		}
		else
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_OLDER;	/* transaction is aborted */
		}
	}
	else
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xinfo->xid)))
			return XVERSION_CURRENT;
		else if (xstore_transaction_id_is_in_progress(xinfo->xid))
		{
			snapshot->xmin = XidFromFullTransactionId(xinfo->xid);
			return XVERSION_CURRENT;	/* in insertion by other */
		}
		else if (transaction_id_did_commit_with_hint(xinfo->xid, &hint_status))
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_CURRENT;
		}
		else
		{
			if (tuple != NULL)
				xheap_set_hint(hinted, buffer, tuple->ctid.ip_posid, hint_status);
			return XVERSION_NONE;	/* inserting transaction aborted */
		}

	}
	/* should never get here */
	pg_unreachable();
}

/*
 * xheap_check_cid
 *
 * For a tuple whose xid satisfies TransactionIdIsCurrentTransactionId(xid),
 * this function makes a determination about tuple visibility based on CID.
 */
static XVersionSelector
xheap_check_cid(XTupleTidOp op, CommandId tuple_cid, CommandId visibility_cid,
			  bool *has_cur_xact_write)
{
	if (op == XTUPLETID_GONE)
	{
		if (tuple_cid >= visibility_cid)
			return XVERSION_OLDER;	/* deleted after scan started */
		else
		{
			if (has_cur_xact_write != NULL)
				*has_cur_xact_write = true;
			return XVERSION_NONE;	/* deleted before scan started */
		}
	}
	else if (op == XTUPLETID_MODIFIED)
	{
		if (tuple_cid >= visibility_cid)
			return XVERSION_OLDER;	/* updated/locked after scan started */
		else
		{
			if (has_cur_xact_write != NULL)
				*has_cur_xact_write = true;
			return XVERSION_CURRENT;	/* updated/locked before scan started */
		}
	}
	else
	{
		if (tuple_cid >= visibility_cid)
			return XVERSION_NONE;	/* inserted after scan started */
		else
		{
			if (has_cur_xact_write != NULL)
				*has_cur_xact_write = true;
			return XVERSION_CURRENT;	/* inserted before scan started */
		}
	}

	/* should never get here */
	pg_unreachable();
}

static XVersionSelector
xheap_select_version_update(XTupleTidOp op, FullTransactionId xid, CommandId visibility_cid)
{
	/* Shouldn't be looking at a delete or non-inplace update. */
	Assert(op != XTUPLETID_GONE);

	if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
	{
		/*
		 * This transaction is still running and belongs to the current
		 * session.  If the current CID has been used to stamp a tuple or the
		 * snapshot belongs to an older CID, then we need the CID for this
		 * tuple to make a final visibility decision.
		 */
		if (GetCurrentCommandIdUsed() ||
			GetCurrentCommandId(false) != visibility_cid)
			return XVERSION_CHECK_CID;

		/* Nothing has changed since our scan started. */
		return XVERSION_CURRENT;
	}

	if (xstore_transaction_id_is_in_progress(xid) || !xstore_transaction_id_did_commit(xid))
		/* The XID is still in progress, or aborted; we can't see it. */
		return ((op == XTUPLETID_NEW) ? XVERSION_NONE : XVERSION_OLDER);

	/* The XID is visible to us. */
	return XVERSION_CURRENT;
}

bool
xheap_tuple_fetch(Relation rel, Buffer buffer, OffsetNumber offnum, Snapshot snapshot,
				  XHeapTuple *visible_tuple, ItemPointer new_ctid, bool keep_tup,
				  XHeapTupleTransInfo *saved_xinfo, bool *got_xinfo,
				  const XHeapTuple *saved_tuple, int16 last_var, bool *bool_arr,
				  bool *has_cur_xact_write)
{
	Page			dp = BufferGetPage(buffer);
	RowPtr		   *rp = XPageGetRowPtr(dp, offnum);
	XHeapTuple		xtuple = NULL;
	XTupleTidOp		op;
	bool			have_trans_info = false;
	XVersionSelector xheapselect = XVERSION_NONE;
	XHeapTupleTransInfo xinfo;
	bool			valid = true;
	FullTransactionId tup_xid = InvalidFullTransactionId;
	UndoTraversalState state = UNDO_TRAVERSAL_DEFAULT;
	BlockNumber		blockno = BufferGetBlockNumber(buffer);
	bool			visible;
	uint16			infomask;

	ereport(DEBUG5,
			(errmsg("xheap fetch xtuple: xheap rel: %s, buf: %d, ctid: block %u offset %u",
					RelationGetRelationName(rel), buffer, BufferGetBlockNumber(buffer), offnum)));
	/*
	 * If caller wants SNAPSHOT_DIRTY semantics, certain fields need to be
	 * cleared up front.  We may set them again later to pass back various
	 * bits of information to the caller.
	 */
	if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
		snapshot->xmin = snapshot->xmax = InvalidTransactionId;

	/* In new version of xstore, deleted tuple will remain until it becomes surely dead */
	if (RowPtrIsNormal(rp))
	{
		Assert(last_var >= -1);
		xtuple = saved_tuple ? *saved_tuple
							: xheap_get_tuple_partial(rel, buffer, offnum, last_var, bool_arr);
		tup_xid = XHeapTupleGetModifiedXid(xtuple);
	}
	else
	{
		/*
		 * If this RowPtr is neither normal nor dead, it must be unused.  In
		 * that case, there is no version of the tuple visible here, so we can
		 * exit quickly.
		 */
		Assert(!RowPtrIsUsed(rp));
		if (visible_tuple)
			*visible_tuple = NULL;
		return false;
	}

	/* The xtuple will remain untill being set unused now, so there must be a tuple */
	Assert(xtuple != NULL);

	xinfo.cid = InvalidCommandId;
	xinfo.xid = tup_xid;
	xinfo.urec_add = xtuple->disk_tuple->urec;

	infomask = xtuple->disk_tuple->flag;
	op = xheap_tid_op_from_infomask(infomask);

	if (op == XTUPLETID_GONE)
	{
		xtuple->xmax = XidFromFullTransactionId(XHeapTupleGetModifiedXid(xtuple));
		xtuple->xmin = InvalidTransactionId;
	}
	else
	{
		xtuple->xmin = XidFromFullTransactionId(XHeapTupleGetModifiedXid(xtuple));
		xtuple->xmax = InvalidTransactionId;
	}

	/*
	 * If this is a SNAPSHOT_ANY snapshot, the current version of the tuple is
	 * always the visible one.
	 *
	 * For SNAPSHOT_TOAST, if the tuple has been pruned out or vacuumed, then
	 * we go to undo to fetch the latest version. Otherwise, the current version
	 * is the visible one.
	 */
	if (snapshot->snapshot_type == SNAPSHOT_ANY ||
		(snapshot->snapshot_type == SNAPSHOT_TOAST && xtuple != NULL))
		goto out;

	xheapselect = xheap_tuple_satisfies(xtuple, buffer, op, snapshot, &xinfo);

	if (snapshot->snapshot_type == SNAPSHOT_DIRTY &&
		(TransactionIdIsValid(snapshot->xmin) || TransactionIdIsValid(snapshot->xmax)))
	{

		FullTransactionId subxid = InvalidFullTransactionId;

		if (TransactionIdIsValid(snapshot->xmin))
		{
			state = fetch_subxid_from_undo(xinfo.urec_add, &subxid);
			if (state == UNDO_TRAVERSAL_COMPLETE && FullTransactionIdIsValid(subxid))
				snapshot->xmin = XidFromFullTransactionId(subxid);
		}
		else
		{
			state = fetch_subxid_from_undo(xinfo.urec_add, &subxid);
			if (state == UNDO_TRAVERSAL_COMPLETE && FullTransactionIdIsValid(subxid))
				snapshot->xmax = XidFromFullTransactionId(subxid);
		}
	}

	/* If necessary, check CID against snapshot. */
	if (xheapselect == XVERSION_CHECK_CID)
	{
		/* XHeapUNDO : Fetch the tuple's transaction information from the undo */
		if (!have_trans_info)
		{
			state = fetch_transinfo_from_undo(xinfo.urec_add, blockno, offnum, InvalidFullTransactionId, &xinfo.cid,
										   new_ctid, false, NULL, NULL);

			Assert(state != UNDO_TRAVERSAL_ABORT);
			have_trans_info = true;
		}

		if (xinfo.cid == InvalidCommandId)
			ereport(PANIC,
					(errmsg("invalid cid! "
							"LogInfo: undo state %d, tuple flag %u, tupXid %lu. "
							"TransInfo: current xid %lu, oid %u, undo ptr:%lu, tid(%u, %u). globalrecyclexid %lu. "
							"Snapshot: type %d, xmin %u.",
							state, xtuple->disk_tuple->flag, tup_xid.value, 
							GetTopFullTransactionIdIfAny().value, xtuple->table_oid, xtuple->disk_tuple->urec,
							blockno, offnum, pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid),
							snapshot->snapshot_type, snapshot->xmin)));

		xheapselect = xheap_check_cid(op, xinfo.cid, snapshot->curcid, NULL);
	}

	/*
	 * If we decided that we need to consult the undo log to figure out what
	 * version our snapshot can see, call get_tuple_from_undo to fetch it.
	 */
	if (xheapselect == XVERSION_OLDER || (xheapselect == XVERSION_NONE && keep_tup))
	{
		XHeapTuple	prior_tuple = NULL;

		/* 
		 * Fetch the full tuple from page if xtuple was from a partial seq scan. 
		 * This is important as the undo has the difference between the old and new tuple in the page.
		 */
		if (xtuple != NULL && last_var != -1 && !saved_tuple)
		{
			/* xmin, xmax is not supported rightly now, support it later*/
			TransactionId saved_xmin = xtuple->xmin;
			TransactionId saved_xmax = xtuple->xmax;

			pfree(xtuple);
			xtuple = xheap_get_tuple_partial(rel, buffer, offnum, -1, bool_arr);
			xtuple->xmin = saved_xmin;
			xtuple->xmax = saved_xmax;
		}

		get_tuple_from_undo(xinfo.urec_add, xtuple, &prior_tuple, snapshot, snapshot->curcid,
						 buffer, offnum, new_ctid);

		if (xtuple != NULL && xtuple != prior_tuple && !saved_tuple)
			pfree(xtuple);

		xtuple = prior_tuple;
		if (xtuple != NULL)
			xtuple->table_oid = RelationGetRelid(rel);
	}

	/* If we decide that no tuple is visible, free the tuple we built here. */
	if (xheapselect == XVERSION_NONE && xtuple != NULL)
	{
		/*
		 * Don't free the tuple when we need the latest version even if it's invisible
		 * will be useful when a tuple has been non-inplace updated by other transaction
		 * and then you lock the tuple again
		 * We will retain the not-visible tuple if the caller asked us to do
		 * so, but that won't change the visibility status.
		 */
		if (keep_tup)
			valid = false;
		else
		{
			if (!saved_tuple)
				pfree(xtuple);
			xtuple = NULL;
		}
	}

	/*
	 * If this tuple has been subjected to a non-inplace update, try to
	 * retrieve the new CTID if the caller wants it.  When the tuple has been
	 * moved to a completely different partition, the new CTID is not
	 * meaningful, so we skip trying to find it in that case.
	 */
	if (new_ctid && !have_trans_info &&
		(xheapselect == XVERSION_NONE || snapshot->snapshot_type == SNAPSHOT_ANY) &&
		xtuple != NULL && !XHeapTupleIsMoved(xtuple->disk_tuple->flag) &&
		XHeapTupleIsUpdated(xtuple->disk_tuple->flag))
	{
		FullTransactionId last_xid = InvalidFullTransactionId;
		UndoRecPtr	urp = INVALID_UNDO_REC_PTR;

		state = fetch_transinfo_from_undo(xinfo.urec_add, blockno, offnum, InvalidFullTransactionId, &xinfo.cid,
									   new_ctid, false, &last_xid, &urp);
		if (state == UNDO_TRAVERSAL_ABORT)
		{
			int			logno = (int) UNDO_PTR_GET_LOG_NO(urp);
			UndoLogControl *ulog = get_undo_log(logno);

			elog(ERROR, "snapshot too old! maybe the undo record has been discarded. "
						"undo state %d, tuple flag %u, tupXid %lu. xinfo: xid %lu, undoptr %lu. "
						"CurrentTransaction xid %lu, tuple table oid %u, tid(%u, %u), lastXid %lu, "
						"globalRecycleXid %lu, globalFrozenXid %lu.undoinfo: urp: %lu, logno %d, insertURecPtr %lu, "
						"forceDiscardURecPtr %lu, discardURecPtr %lu, recycleXid %lu.Snapshot: type %d, xmin %u.",
						state, xtuple->disk_tuple->flag, tup_xid.value, xinfo.xid.value, xinfo.urec_add, GetTopFullTransactionIdIfAny().value, 
						xtuple->table_oid, blockno, offnum, last_xid.value, pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid),
						pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid), urp, logno,
						UndoGetInsertURecPtr(ulog), UndoGetForceDiscardURecPtr(ulog),
					    UndoGetDiscardURecPtr(ulog), UndoGetRecycleXid(ulog).value,
						snapshot->snapshot_type, snapshot->xmin);
		}
		have_trans_info = true;
	}

	/*
	 * We're all done. Make sure that either caller gets the tuple, or it gets
	 * freed.
	 */
out:
	if (have_trans_info && saved_xinfo)
	{
		*got_xinfo = true;

		saved_xinfo->xid = xinfo.xid;
		saved_xinfo->cid = xinfo.cid;
		saved_xinfo->urec_add = xinfo.urec_add;
	}

	visible = (xtuple != NULL && valid);
	if (visible_tuple)
		*visible_tuple = xtuple;
	else if (xtuple && !saved_tuple)
		pfree(xtuple);

	if (visible_tuple && *visible_tuple)
		Assert(ItemPointerIsValid(&((*visible_tuple)->ctid)));

	return visible;
}

static XTupleTidOp
xheap_tid_op_from_infomask(uint16 infomask)
{
	if ((infomask & (XHEAP_INPLACE_UPDATED)) != 0)
		return XTUPLETID_MODIFIED;
	if ((infomask & (XHEAP_UPDATED | XHEAP_DELETED)) != 0)
		return XTUPLETID_GONE;
	return XTUPLETID_NEW;
}

/*
 * xheap_tuple_satisfies_update
 *
 * The return value for this API are same as HeapTupleSatisfiesUpdate.
 * However, there is a notable difference in the way to determine visibility
 * of tuples.  We need to traverse undo record chains to determine the
 * visibility of tuple.
 *
 * For multilockers, the visibility can be determined by the information
 * present on tuple.  See XHeapTupleSatisfiesMVCC.  Also, this API returns
 * TM_Ok, if the strongest locker is committed which means
 * the caller need to take care of waiting for other lockers in such a case.
 *
 * ctid - returns the ctid of visible tuple if the tuple is either deleted or
 * updated.  ctid needs to be retrieved from undo tuple.
 * xinfo - returns the transaction slot of the transaction that has
 * modified the visible tuple.
 * cid - returns the cid of visible tuple.
 * locker_xid - returns the xid of a single in-progress locker, if any.
 * lock_allowed - allow caller to lock the tuple if it is in-place updated
 * inplaceUpdated - returns whether the current visible version of tuple is
 * updated in place.
 */
TM_Result
xheap_tuple_satisfies_update(Relation rel, Snapshot snapshot, ItemPointer tid,
							 XHeapTuple xtuple, CommandId cid, Buffer buffer,
							 ItemPointer ctid, XHeapTupleTransInfo *xinfo,
							 FullTransactionId *update_subxid, FullTransactionId *locker_xid,
							 bool avoidVisCheck, bool multixid_self,
							 bool *inplaceUpdated)
{
	BlockNumber		blocknum = ItemPointerGetBlockNumber(tid);
	OffsetNumber	offnum = ItemPointerGetOffsetNumber(tid);
	Page			page = BufferGetPage(buffer);
	RowPtr		   *rp = XPageGetRowPtr(page, offnum);
	CommandId		cur_cid = GetCurrentCommandId(false);
	bool			do_fetch_cid = false;
	bool			has_ctid = false;
	bool			fetch_subxid = false;
	TransactionId	tup_xid = InvalidTransactionId;
	TM_Result		result = TM_Invisible;
	UndoTraversalState state = UNDO_TRAVERSAL_DEFAULT;
	XHeapTupleStatus tuple_status;
	XHeapDiskTuple	tuple_data;
	XTupleTidOp		op;

	*locker_xid = InvalidFullTransactionId;
	*update_subxid = InvalidFullTransactionId;

	xtuple->table_oid = RelationGetRelid(rel);
	xtuple->ctid = *tid;

	*inplaceUpdated = false;
	if (ctid != NULL)
		*ctid = *tid;

	Assert(RowPtrIsNormal(rp));

	// read data from page
	xtuple->disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);
	xtuple->disk_tuple_size = RowPtrGetLen(rp);

	tuple_data = xtuple->disk_tuple;

	op = xheap_tid_op_from_infomask(xtuple->disk_tuple->flag);

	if (op == XTUPLETID_GONE)
	{
		xtuple->xmax = XidFromFullTransactionId(XHeapTupleGetModifiedXid(xtuple));
		xtuple->xmin = InvalidTransactionId;
	}
	else
	{
		xtuple->xmin = XidFromFullTransactionId(XHeapTupleGetModifiedXid(xtuple));
		xtuple->xmax = InvalidTransactionId;
	}

	xinfo->xid = XHeapTupleGetModifiedXid(xtuple);
	xinfo->cid = InvalidCommandId;
	xinfo->urec_add = xtuple->disk_tuple->urec;

	do_fetch_cid = GetCurrentCommandIdUsed() || cur_cid != cid;

	if (do_fetch_cid && TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xinfo->xid)))
	{
		state = fetch_transinfo_from_undo(xinfo->urec_add, blocknum, offnum, InvalidFullTransactionId, &xinfo->cid,
									   ctid, false, NULL, NULL);
		Assert(state != UNDO_TRAVERSAL_ABORT);
		has_ctid = true;
	}

	tuple_status = xheap_tuple_get_status(xtuple);

	/* tuple is locked by multiple transactions */
	if (tuple_status == XHEAPTUPLESTATUS_MULTI_LOCKED)
	{
		*inplaceUpdated = true;

		/*
		 * Tuple is locked by multixid and xids other than current transaction has been terminated
		 * See xheap_wait for detail.
		 */
		if (multixid_self)
		{
			elog(DEBUG5,
				 "xheap_tuple_satisfies_update[OK], multixact %lu, multixid_self %d",
				 XHeapTupleGetLockerXid(xtuple).value, multixid_self);
			/* Since we can lock the tuple, tuple must be visible to us */
			return TM_Ok;
		}
		else
		{
			if (TransactionIdOlderThanAllUndo(xinfo->xid))
			{
				elog(DEBUG5,
					 "xheap_tuple_satisfies_update[FROZEN], multixact %lu, "
					 "multixid_self %d",
					 XHeapTupleGetModifiedXid(xtuple).value, multixid_self);
				return TM_BeingModified;
			}
			else
			{
				Assert(xstore_transaction_id_did_commit(xinfo->xid));
				if (avoidVisCheck ||
					committed_xid_visible_in_snapshot(xinfo->xid, snapshot))
					elog(DEBUG5,
						 "xheap_tuple_satisfies_update[BeingUpdated], multixact %lu, "
						 "multixid_self %d",
						 XHeapTupleGetLockerXid(xtuple).value, multixid_self);
				else
					elog(DEBUG5,
						 "xheap_tuple_satisfies_update[UPDATED], multixact %lu, "
						 "multixid_self %d",
						 XHeapTupleGetLockerXid(xtuple).value, multixid_self);
				return TM_BeingModified;
			}
		}
		/* tuple is locked */
	}
	else if (tuple_status == XHEAPTUPLESTATUS_LOCKED)
	{
		// tuple is locked, then the data is either inplace updated or newly-inserted
		FullTransactionId locker;
		bool		checkLocker;
		// 1. get locker info
		Assert(XHEAP_XID_IS_EXCL_LOCKED(tuple_data->flag) ||
			   XHEAP_XID_IS_SHR_LOCKED(tuple_data->flag));

		locker = XHeapTupleGetLockerXid(xtuple);

		*inplaceUpdated = true;

		/*
		 * We only need to check locker if it is larger than frozen xid, or it will be
		 * treated as ended.
		 */
		checkLocker = (FullTransactionIdIsNormal(locker) && !TransactionIdOlderThanAllUndo(locker));

		if (checkLocker &&
			(TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(locker)) || 
			 xstore_transaction_id_is_in_progress(locker)))
		{
			/* let waiter handle this case, possibly already locked if we are the locker */
			*locker_xid = locker;
			result = TM_BeingModified;
		}
		else
		{ 
			/* 
			 * if locker xid is commited/aborted, just consider previous updater 
			 * no active locker on tuple, since we have acquired exclusive lock on buffer, simply clear the locker txid 
			 */
			XHeapTupleHeaderClearSingleLocker(tuple_data);

			if (TransactionIdOlderThanAllUndo(xinfo->xid))
				result = TM_Ok;
			else if (xstore_transaction_id_did_commit(xinfo->xid))
			{
				if (avoidVisCheck ||
					committed_xid_visible_in_snapshot(xinfo->xid, snapshot))
					result = TM_Ok;
				else
					result = TM_Updated;	// caller will do refetch, lock and EPQ
			}
			else
				result = TM_BeingModified;  // aborted
		}
	}
	else if (tuple_status == XHEAPTUPLESTATUS_DELETED)
	{
		// tuple is deleted or non-inplace updated
		// tuple can pass visibility test so DELETE operation on it cannot be all-visible

		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xinfo->xid)))
		{
			if (do_fetch_cid && xinfo->cid >= cid)
				result = TM_SelfModified;
			else
				result = TM_Invisible;
		}
		else if (xstore_transaction_id_is_in_progress(xinfo->xid))
		{
			// deleter is still active, caller should wait it until it commits or aborts
			result = TM_BeingModified;
			fetch_subxid = true;
		}
		else if (xstore_transaction_id_did_commit(xinfo->xid))
			result = TM_Updated;
		else
			result = TM_BeingModified;  // aborted
	}
	else if (tuple_status == XHEAPTUPLESTATUS_INPLACE_UPDATED)
	{
		// tuple is inplace updated
		*inplaceUpdated = true;

		if (TransactionIdOlderThanAllUndo(xinfo->xid))
			result = TM_Ok;
		else if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xinfo->xid)))
		{
			if (do_fetch_cid && xinfo->cid >= cid)
				result = TM_SelfModified;
			else
				result = TM_Ok;
		}
		else if (xstore_transaction_id_is_in_progress(xinfo->xid))
		{
			result = TM_BeingModified;
			fetch_subxid = true;
		}
		else if (xstore_transaction_id_did_commit(xinfo->xid))
		{
			if (avoidVisCheck ||
				committed_xid_visible_in_snapshot(xinfo->xid, snapshot))
				result = TM_Ok;
			else
				result = TM_Updated;
		}
		else
			result = TM_BeingModified;  // aborted
	}
	else
	{
		if (TransactionIdOlderThanAllUndo(xinfo->xid))
			result = TM_Ok;
		else if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xinfo->xid)))
		{
			if (do_fetch_cid && xinfo->cid >= cid)
				result = TM_Invisible;
			else
				result = TM_Ok;
		}
		else if (xstore_transaction_id_is_in_progress(xinfo->xid))
			result = TM_Invisible;
		else if (xstore_transaction_id_did_commit(xinfo->xid))
			result = TM_Ok;
		else
			result = TM_Invisible;  // aborted
	}

	// fetch ctid when tuple is non-inplace updated
	if (ctid && !has_ctid && XHeapTupleIsUpdated(tuple_data->flag) &&
		!XHeapTupleIsMoved(tuple_data->flag))
	{
		FullTransactionId last_xid = InvalidFullTransactionId;
		UndoRecPtr	urp = INVALID_UNDO_REC_PTR;

		state = fetch_transinfo_from_undo(xinfo->urec_add, blocknum, offnum, InvalidFullTransactionId, &xinfo->cid,
									   ctid, false, &last_xid, &urp);
		if (state == UNDO_TRAVERSAL_ABORT)
		{
			int			logno = (int) UNDO_PTR_GET_LOG_NO(urp);
			UndoLogControl *ulog = get_undo_log(logno);

			elog(ERROR,"snapshot too old! maybe undo record has been discarded. we are trying to fetch ctid from undo. "
				"undo state %d, tuple flag %u, tupXid %u. undoptr %lu. CurrentTransaction Xid: %u, table oid %u, tid(%u, %u), lastXid %lu,  "
				"globalRecycleXid %lu, globalFrozenXid %lu.undolog: urp: %lu, logno %d,insertURecPtr %lu, forceDiscardURecPtr %lu,"
				"discardURecPtr %lu, recycleXid %lu. Snapshot: type %d, xmin %u.",
				state, xtuple->disk_tuple->flag, tup_xid,
				xinfo->urec_add, GetTopTransactionIdIfAny(),
				xtuple->table_oid, blocknum, offnum, last_xid.value,
				pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid),
				pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid), urp, logno,
				UndoGetInsertURecPtr(ulog), UndoGetForceDiscardURecPtr(ulog),
				UndoGetDiscardURecPtr(ulog), UndoGetRecycleXid(ulog).value,
				snapshot->snapshot_type, snapshot->xmin);
		}

		if (ctid->ip_posid == 0)
			ereport(PANIC,
					(errmsg("invalid ctid! "
							"LogInfo: undo state %d, tuple flag %u, tupXid %u. "
							"xinfo: xid %lu, undoptr %lu. "
							"TransInfo: current xid %u, oid %u, tid(%u, %u). globalrecyclexid %lu. "
							"Snapshot: type %d, xmin %u.",
							state, xtuple->disk_tuple->flag, tup_xid, xinfo->xid.value,
							xinfo->urec_add, GetTopTransactionIdIfAny(),
							xtuple->table_oid, blocknum, offnum,
							pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid),
							snapshot->snapshot_type, snapshot->xmin)));
	}

	if (fetch_subxid)
	{
		/*
		 * Xstore use TransactionStateData.subtransactionId rather than real assigned xid to lock the tuple
		 * in Delete/Update but use real assigned xid in xheap_execute_lock_tuple. So if we have a valid locker_xid,
		 * it means this tuple was locked by xheap_execute_lock_tuple and there must have no subtransactionId in undo.
		 */
		Assert(!TransactionIdIsValid(locker_xid->value));
		Assert(tuple_status != XHEAPTUPLESTATUS_LOCKED);

		fetch_subxid_from_undo(xinfo->urec_add, update_subxid);
	}

	return result;
}

/*
 * xheap_tuple_get_trans_info - Retrieve transaction information of transaction
 *			that has modified the tuple.
 *
 * nobuflock indicates whether caller has lock on the buffer 'buf'. If nobuflock
 * is false, we rely on the supplied tuple uhtup to fetch the slot and undo
 * information. Otherwise, we take buffer lock and fetch the actual tuple.
 *
 * snapshot will be used to avoid fetching tuple transaction id from the
 * undo if the transaction slot is reused.  So caller should pass a valid
 * snapshot where it's just fetching the xid for the visibility purpose.
 * InvalidSnapshot indicates that we need the xid of reused transaction
 * slot even if it is not in the snapshot, this is required to store its
 * value in undo record, otherwise, that can break the visibility for
 * other concurrent session holding old snapshot.
 */
UndoTraversalState
xheap_tuple_get_trans_info(Buffer buf, OffsetNumber offnum, XHeapTupleTransInfo *txactinfo,
					   bool *has_cur_xact_write, FullTransactionId *lastXid, UndoRecPtr *urp)
{
	RowPtr	   *rp;
	Page		page;
	UndoTraversalState state = UNDO_TRAVERSAL_DEFAULT;
	XHeapDiskTuple hdr;

	page = BufferGetPage(buf);
	rp = XPageGetRowPtr(page, offnum);
	Assert(RowPtrIsNormal(rp));

	txactinfo->cid = InvalidCommandId;

	hdr = (XHeapDiskTuple) XPageGetRowData(page, rp);

	txactinfo->xid = hdr->modified_xid;
	txactinfo->urec_add = hdr->urec;

	if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(txactinfo->xid)))
	{
		state = fetch_transinfo_from_undo(txactinfo->urec_add, BufferGetBlockNumber(buf), offnum,
										InvalidFullTransactionId, &txactinfo->cid, NULL, false,
										lastXid, urp);
	}

	return state;
}

/*
 * xheap_tuple_satisfies_oldest_xmin
 *		The tuple will be considered visible if it is visible to any open
 *		transaction.
 *
 * xtuple is an input/output parameter.  The caller must send the palloc'ed
 * data.  This function can get a tuple from undo to return in which case it
 * will free the memory passed by the caller.
 *
 * xid is an output parameter. It is set to the latest committed/in-progress
 * xid that inserted/modified the tuple.
 * If the latest transaction for the tuple aborted, we fetch a prior committed
 * version of the tuple and return the prior committed xid and status as
 * HEAPTUPLE_LIVE.
 * If the latest transaction for the tuple aborted and it also inserted
 * the tuple, we return the aborted transaction id and status as
 * HEAPTUPLE_DEAD. In this case, the caller *should* never mark the
 * corresponding item id as dead. Because, when undo action for the same will
 * be performed, we need the item pointer.
 */
XHTSVResult
xheap_tuple_satisfies_oldest_xmin(XHeapTuple xhtup, FullTransactionId oldestXmin, Buffer buffer,
							  bool resolve_abort_in_progress, XHeapTuple *preabort_tuple,
							  FullTransactionId *xid, Relation rel,
							  bool *inplaceUpdated, FullTransactionId *lastXid)
{
	XHeapDiskTuple	tuple = xhtup->disk_tuple;
	XHeapTupleTransInfo xinfo;
	OffsetNumber	offnum = ItemPointerGetOffsetNumber(&xhtup->ctid);
	UndoTraversalState state;

	Assert(ItemPointerIsValid(&xhtup->ctid));
	Assert(xhtup->table_oid != InvalidOid);

	/* Get transaction id */
	state = xheap_tuple_get_trans_info(buffer, offnum, &xinfo, NULL, NULL, NULL);
	*xid = xinfo.xid;

	if ((tuple->flag & XHEAP_DELETED) || (tuple->flag & XHEAP_UPDATED))
	{
		/*
		 * The tuple is deleted and must be all visible if the transaction
		 * slot is cleared or latest xid that has changed the tuple precedes
		 * smallest xid that has undo.
		 */
		if (state == UNDO_TRAVERSAL_ABORT)
		{
			*xid = xinfo.xid;
			return XHEAPTUPLE_RECENTLY_DEAD;
		}

		if (TransactionIdOlderThanAllUndo(xinfo.xid))
			return XHEAPTUPLE_DEAD;

		if (TransactionIdIsInProgress(XidFromFullTransactionId(xinfo.xid)))
			return XHEAPTUPLE_DELETE_IN_PROGRESS;

		if (TransactionIdDidCommit(XidFromFullTransactionId(xinfo.xid)))
		{
			/*
			 * Deleter committed, but perhaps it was recent enough that some
			 * open transactions could still see the tuple.
			 */
			if (!FullTransactionIdPrecedes(xinfo.xid, oldestXmin))
				return XHEAPTUPLE_RECENTLY_DEAD;

			/* Otherwise, it's dead and removable */
			return XHEAPTUPLE_DEAD;
		}
		else
		{						/* transaction is aborted */
			XHeapTuple	undoTuple;

			if (!resolve_abort_in_progress)
				return XHEAPTUPLE_ABORT_IN_PROGRESS;

			/*
			 * For aborted transactions, we need to fetch the tuple from undo
			 * chain.  It should be OK to use SnapshotSelf semantics because
			 * we know that the latest transaction is aborted; the previous
			 * transaction therefore can't be current or in-progress or for
			 * that matter aborted.  It seems like even SnapshotAny semantics
			 * would be OK here, but get_tuple_from_undo doesn't know about
			 * those.
			 *
			 * ZBORKED: This code path needs tests.  I was not able to hit it
			 * in either automated or manual testing.
			 */

			get_tuple_from_undo(xinfo.urec_add, xhtup, &undoTuple, SnapshotSelf,
							 InvalidCommandId, buffer, offnum, NULL);

			if (preabort_tuple)
				*preabort_tuple = undoTuple;
			else if (undoTuple != NULL && undoTuple != xhtup)
				pfree(undoTuple);

			if (undoTuple != NULL)
				return XHEAPTUPLE_LIVE;
			else
				// If the transaction that inserted the tuple got aborted, we
				// should return the aborted transaction id.
				return XHEAPTUPLE_DEAD;
		}
	}

	if (state == UNDO_TRAVERSAL_ABORT)
	{
		if (inplaceUpdated && (tuple->flag & XHEAP_INPLACE_UPDATED))
			*inplaceUpdated = true;
		*xid = InvalidFullTransactionId;
		return XHEAPTUPLE_LIVE;
	}

	/*
	 * The tuple must be all visible if the transaction slot is cleared or
	 * latest xid that has changed the tuple precedes smallest xid that has
	 * undo.
	 */
	if (TransactionIdOlderThanAllUndo(xinfo.xid))
	{
		if (inplaceUpdated && (tuple->flag & XHEAP_INPLACE_UPDATED))
			*inplaceUpdated = true;
		return XHEAPTUPLE_LIVE;
	}

	if (TransactionIdIsInProgress(XidFromFullTransactionId(xinfo.xid)))
		return XHEAPTUPLE_INSERT_IN_PROGRESS;

	if (TransactionIdDidCommit(XidFromFullTransactionId(xinfo.xid)))
	{
		if (inplaceUpdated && (tuple->flag & XHEAP_INPLACE_UPDATED))
			*inplaceUpdated = true;
		return XHEAPTUPLE_LIVE;
	}
	else
	{						/* transaction is aborted */
		if (!resolve_abort_in_progress)
			return XHEAPTUPLE_ABORT_IN_PROGRESS;

		if (tuple->flag & XHEAP_INPLACE_UPDATED)
		{
			/*
			 * For aborted transactions, we need to fetch the tuple from undo
			 * chain.  It should be OK to use SnapshotSelf semantics because
			 * we know that the latest transaction is aborted; the previous
			 * transaction therefore can't be current or in-progress or for
			 * that matter aborted.  It seems like even SnapshotAny semantics
			 * would be OK here, but get_tuple_from_undo doesn't know about
			 * those.
			 *
			 * ZBORKED: This code path needs tests.  I was not able to hit it
			 * in either automated or manual testing.
			 */
			XHeapTuple	undoTuple;

			get_tuple_from_undo(xinfo.urec_add, xhtup, &undoTuple, SnapshotSelf,
							 InvalidCommandId, buffer, offnum, NULL);

			if (preabort_tuple)
				*preabort_tuple = undoTuple;
			else if (undoTuple != NULL && undoTuple != xhtup)
				pfree(undoTuple);

			if (undoTuple != NULL)
				return XHEAPTUPLE_LIVE;
		}

		/*
		 * If the transaction that inserted the tuple got aborted, we should
		 * return the aborted transaction id.
		 */
		return XHEAPTUPLE_DEAD;
	}

	return XHEAPTUPLE_LIVE;
}

/*
 * xheap_tuple_is_surely_dead
 *
 * Similar to HeapTupleIsSurelyDead, but for XHeap tuples.
 */
bool
xheap_tuple_is_surely_dead(XHeapTuple xhtup, Buffer buffer, OffsetNumber offnum,
					   const XHeapTupleTransInfo *cachedTdInfo,
					   const bool				  useCachedTdInfo)
{
	XHeapTupleTransInfo xinfo;
	UndoTraversalState	state = UNDO_TRAVERSAL_DEFAULT;

	if (xhtup != NULL &&
		xheap_tid_op_from_infomask(xhtup->disk_tuple->flag) != XTUPLETID_GONE)
		return false;

	/*
	 * Get transaction information.
	 * Here we use a cached transaction information if there is any.
	 */
	if (useCachedTdInfo)
		xinfo = *cachedTdInfo;
	else
		state = xheap_tuple_get_trans_info(buffer, offnum, &xinfo, NULL, NULL, NULL);

	if (state == UNDO_TRAVERSAL_ABORT)
		return false;
	/*
	 * The tuple is deleted and must be all visible if the transaction slot is
	 * cleared or latest xid that has changed the tuple precedes smallest xid
	 * that has undo.
	 */
	if (TransactionIdOlderThanAllUndo(xinfo.xid))
		return true;

	return false;				/* Tuple is still alive */
}

void
update_tuple_header_from_undo_record(UnpackedUndoRecord *urec, XHeapDiskTuple diskTuple)
{
	Assert(urec != NULL);

	if (GetUndoRecordUtype(urec) == UNDO_INSERT ||
		GetUndoRecordUtype(urec) == UNDO_MULTI_INSERT)
	{
		diskTuple->flag &= ~XHEAP_VIS_STATUS_MASK;
		diskTuple->modified_xid = InvalidFullTransactionId;
	}
	else if (GetUndoRecordUtype(urec) == UNDO_INPLACE_UPDATE)
	{
		Assert(GetUndoRecordRawdata(urec) != NULL);
		Assert(GetUndoRecordRawdata(urec)->len >= (int) SizeOfXHeapDiskTupleHeaderExceptXid);
		memcpy((char *) diskTuple + OffsetDataHeader, GetUndoRecordRawdata(urec)->data + sizeof(uint8),
					 SizeOfXHeapDiskTupleHeaderExceptXid);
		diskTuple->modified_xid = GetUndoRecordOldXactId(urec);
	}
	else if (GetUndoRecordUtype(urec) == UNDO_UPDATE)
	{
		Assert(GetUndoRecordRawdata(urec) != NULL);
		Assert(GetUndoRecordRawdata(urec)->len == sizeof(ItemPointerData));
		Assert(UndoRecordHasDataHeadFlag(urec));
		diskTuple->flag = GetUndoRecordDataHeadFlag(urec);
		diskTuple->modified_xid = GetUndoRecordOldXactId(urec);
	}
	else
	{
		Assert(UndoRecordHasDataHeadFlag(urec));
		diskTuple->flag = GetUndoRecordDataHeadFlag(urec);
		diskTuple->modified_xid = GetUndoRecordOldXactId(urec);
	}

	diskTuple->urec = GetUndoRecordTpprev(urec);
}

static UndoTraversalState
get_tuple_from_undo_record(UndoRecPtr urec_ptr, FullTransactionId xid, Buffer buffer,
					   OffsetNumber offnum, XHeapDiskTuple hdr, XHeapTuple *tuple,
					   bool *freeTuple, XHeapTupleTransInfo *uinfo, ItemPointer ctid,
					   FullTransactionId *lastXid, UndoRecPtr *urp)
{
	int			undotype;
	UndoTraversalState state;

	UnpackedUndoRecord *urec = new_undo_record();

	urec->uur_urp = urec_ptr;
	urec->mem_ctx = CurrentMemoryContext;

	state = fetch_undo_record(urec, xid, false,
							lastXid, satisfy_undo_record);
	if (state != UNDO_TRAVERSAL_COMPLETE)
	{
		destroy_undo_record(urec);
		return state;
	}

	update_tuple_header_from_undo_record(urec, hdr);

	/*
	 * If the tuple is being updated or deleted, the payload contains a whole
	 * new tuple.  If the caller wants it, extract it.
	 */
	undotype = GetUndoRecordUtype(urec);

	Assert(tuple != NULL);

	if (undotype == UNDO_DELETE)
	{
		XHeapDiskTuple disk_tuple;

		disk_tuple = (*tuple)->disk_tuple;
		disk_tuple->modified_xid = hdr->modified_xid;
		disk_tuple->locker_xid = hdr->locker_xid;
		disk_tuple->urec = hdr->urec;
	}
	else if (undotype == UNDO_UPDATE)
	{
		// just new tuple ctid in rawdata
		XHeapDiskTuple disk_tuple;
#ifdef USE_ASSERT_CHECKING
		StringInfo	urecPayload = GetUndoRecordRawdata(urec);

		Assert(urecPayload);
		Assert(urecPayload->len == sizeof(ItemPointerData));
#endif
		disk_tuple = (*tuple)->disk_tuple;
		disk_tuple->modified_xid = hdr->modified_xid;
		disk_tuple->locker_xid = hdr->locker_xid;
		disk_tuple->urec = hdr->urec;
		disk_tuple->flag = GetUndoRecordDataHeadFlag(urec);

	}
	else if (undotype == UNDO_INPLACE_UPDATE)
	{
		uint16	   *prefixlenPtr = NULL;
		uint16	   *suffixlenPtr = NULL;
		uint16		prefixlen = 0;
		uint16		suffixlen = 0;
		int			subxid_size;
		StringInfo	urec_pay_load = GetUndoRecordRawdata(urec);
		XHeapDiskTuple disk_tuple;
		uint8	   *t_hoff_ptr;
		uint8		t_hoff;
		char	   *cur_undodata_ptr;
		int			read_size;
		uint8	   *flags_ptr;
		uint8		flags;
		char	   *old_disk_tuple;
		char	   *cur_old_disktuple_ptr;
		char	   *newp;
		char	   *cur_undo_data_p;
		int			oldlen;
		int			newlen;
		int			old_data_len;
		uint32		new_tuple_length;
		XHeapTuple	htup;

		Assert(urec_pay_load);
		Assert(urec_pay_load->len > 0);

		subxid_size = 0;
		disk_tuple = (*tuple)->disk_tuple;
		t_hoff_ptr = (uint8 *) urec_pay_load->data;
		t_hoff = *t_hoff_ptr;
		cur_undodata_ptr = urec_pay_load->data + sizeof(uint8) + t_hoff - OffsetDataHeader;
		read_size = sizeof(uint8) + t_hoff - OffsetDataHeader;
		flags_ptr = (uint8 *) cur_undodata_ptr;
		flags = *flags_ptr;
		cur_undodata_ptr += sizeof(uint8);
		read_size += sizeof(uint8);

		if (flags & UREC_XOR_PREFIX)
		{
			prefixlenPtr = (uint16 *) (cur_undodata_ptr);
			cur_undodata_ptr += sizeof(uint16);
			read_size += sizeof(uint16);
			prefixlen = *prefixlenPtr;
		}
		if (flags & UREC_XOR_SUFFIX)
		{
			suffixlenPtr = (uint16 *) (cur_undodata_ptr);
			cur_undodata_ptr += sizeof(uint16);
			read_size += sizeof(uint16);
			suffixlen = *suffixlenPtr;
		}

		old_disk_tuple = (char *) palloc0(urec_pay_load->len - read_size - subxid_size +
										prefixlen + suffixlen + t_hoff);
		memcpy(old_disk_tuple + OffsetDataHeader, urec_pay_load->data + sizeof(uint8), t_hoff - OffsetDataHeader);
		((XHeapDiskTupleData *) old_disk_tuple)->modified_xid = InvalidFullTransactionId;
		cur_old_disktuple_ptr = old_disk_tuple + t_hoff;

		/* copy the perfix to oldDisktuple */
		if (flags & UREC_XOR_PREFIX)
		{
			memcpy(cur_old_disktuple_ptr, (char *) disk_tuple + disk_tuple->t_hoff, prefixlen);
			cur_old_disktuple_ptr += prefixlen;
		}

		newp = (char *) disk_tuple + disk_tuple->t_hoff + prefixlen;
		cur_undo_data_p = urec_pay_load->data + read_size;
		oldlen = urec_pay_load->len - read_size - subxid_size + prefixlen + suffixlen;
		newlen = (*tuple)->disk_tuple_size - disk_tuple->t_hoff;
		old_data_len = oldlen - prefixlen - suffixlen;

		if (old_data_len > 0)
		{
			memcpy(cur_old_disktuple_ptr, cur_undo_data_p, old_data_len);
			cur_old_disktuple_ptr += (old_data_len);
		}

		if (flags & UREC_XOR_SUFFIX)
			memcpy(cur_old_disktuple_ptr, newp + newlen - prefixlen - suffixlen, suffixlen);

		new_tuple_length = oldlen + t_hoff;
		htup = (XHeapTuple) xheaptup_alloc(XHeapTupleDataSize + new_tuple_length);
		htup->disk_tuple_size = new_tuple_length;
		ItemPointerSet(&htup->ctid, GetUndoRecordBlkno(urec), GetUndoRecordOffset(urec));
		htup->disk_tuple = (XHeapDiskTuple) ((char *) htup + XHeapTupleDataSize);
		htup->table_oid = GetUndoRecordReloid(urec);
		memcpy(htup->disk_tuple, old_disk_tuple, new_tuple_length);

		if (*freeTuple)
			pfree(*tuple);
		*tuple = htup;
		*freeTuple = true;
		pfree(old_disk_tuple);
	}

	uinfo->urec_add = GetUndoRecordTpprev(urec);
	uinfo->cid = InvalidCommandId;
	uinfo->xid = GetUndoRecordOldXactId(urec);

	/* If this is a non-in-place update, update ctid if requested. */
	if (ctid && GetUndoRecordUtype(urec) == UNDO_UPDATE)
	{
		char	   *end;

		Assert(GetUndoRecordRawdata(urec) != NULL);

		/* ItemPointerData is at the end of RawData() */
		end = (char *) GetUndoRecordRawdata(urec)->data +
			  GetUndoRecordRawdata(urec)->len;
		ItemPointerCopy((ItemPointer) (end - sizeof(ItemPointerData)), ctid);
	}
	destroy_undo_record(urec);

	return state;
}

static inline XVersionSelector
xheap_check_undo_snapshot(Snapshot snapshot, XHeapTupleTransInfo uinfo, CommandId curcid,
					   XTupleTidOp op, Buffer buffer)
{
	if (snapshot == NULL)
		return xheap_select_version_update(op, uinfo.xid, curcid);
	else if (IsMVCCSnapshot(snapshot))
		return xheap_select_version_mvcc(NULL, InvalidBuffer, op, uinfo.xid, snapshot);
	else if (snapshot->snapshot_type == SNAPSHOT_NOW)
		return xheap_select_version_now(NULL, InvalidBuffer, op, uinfo.xid);
	else if (snapshot->snapshot_type == SNAPSHOT_SELF_TRANSACTION)
		return xheap_select_version_self_transaction(NULL, InvalidBuffer, op, uinfo.xid);
	else if (snapshot->snapshot_type == SNAPSHOT_SELF)
		return xheap_select_version_self(NULL, InvalidBuffer, op, uinfo.xid);
	else if (snapshot->snapshot_type == SNAPSHOT_NOT_SELF)
		return xheap_select_version_not_self(NULL, InvalidBuffer, op, uinfo.xid);
	else if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
		return xheap_select_version_self(NULL, InvalidBuffer, op, uinfo.xid);
	else
		elog(ERROR, "unsupported snapshot style %d", (int) snapshot->snapshot_type);
}

static bool
get_tuple_from_undo(UndoRecPtr urec_add, XHeapTuple current_tuple, XHeapTuple *visible_tuple,
				 Snapshot snapshot, CommandId curcid, Buffer buffer, OffsetNumber offnum,
				 ItemPointer ctid)
{
	FullTransactionId prev_undo_xid = InvalidFullTransactionId;
	XHeapTupleTransInfo xinfo;
	bool			free_tuple = false;
	BlockNumber		blkno = BufferGetBlockNumber(buffer);
	XHeapDiskTupleData hdr;
	UndoTraversalState state = UNDO_TRAVERSAL_DEFAULT;
	TransactionId	save_xmax = InvalidTransactionId;

	if (current_tuple != NULL)
	{
		/* Sanity check. */
		Assert(ItemPointerGetOffsetNumber(&current_tuple->ctid) == offnum);

		/*
		 * We must set up 'hdr' to point to be a copy of the header bytes from
		 * the most recent version of the tuple.  This is because in the
		 * special case where the undo record we find is an UNDO_INSERT
		 * record, we modify the existing bytes rather than overwriting them
		 * completely.  If currentTuple == NULL, then the current version of
		 * the tuple has been deleted or subjected to a non-in-place update,
		 * so the first record we find won't be UNDO_INSERT.
		 *
		 * ZBORKED: We should really change this to get rid of the special
		 * case for UNDO_INSERT, either by making it so that this function
		 * doesn't get called in that case, or by making it so that it doesn't
		 * need the newer tuple header bytes, or some other clever trick. That
		 * would eliminate a substantial amount of complexity and ugliness
		 * here.
		 */
		memcpy(&hdr, current_tuple->disk_tuple, SizeOfXHeapDiskTupleData);

		/* Initially, result tuple is same as input tuple. */
		if (visible_tuple != NULL)
			*visible_tuple = current_tuple;

		save_xmax = current_tuple->xmin;
	}

	/*
	 * If caller wants the CTID of the latest version of the tuple, set it to
	 * that of the tuple we're looking up for starters.  If it's been the
	 * subject of a non-in-place update, get_tuple_from_undo_record will adjust
	 * the value later.
	 */
	if (ctid)
		ItemPointerSet(ctid, blkno, offnum);

	/*
	 * tuple is modified after the scan is started, fetch the prior record
	 * from undo to see if it is visible. loop until we find the visible
	 * version.
	 */
	while (1)
	{
		FullTransactionId last_xid = InvalidFullTransactionId;
		UndoRecPtr	urp = INVALID_UNDO_REC_PTR;
		XVersionSelector selector = XVERSION_NONE;
		bool		have_cid = false;
		XTupleTidOp	op;
		int			logno;
		UndoLogControl *ulog;

		state = get_tuple_from_undo_record(urec_add, prev_undo_xid, buffer, offnum, &hdr,
									   visible_tuple, &free_tuple, &xinfo, ctid, &last_xid,
									   &urp);
		logno = (int) UNDO_PTR_GET_LOG_NO(urp);
		ulog = get_undo_log(logno);

		if (state == UNDO_TRAVERSAL_ABORT)
			elog(ERROR,"snapshot too old! the undo record has been force discard. we are fetching undo record. "
						"undo state %d, VisibleTuple flag %u, currentTuple flag %u. "
						"xInfo: xid %lu, undoptr %lu. "
						"TransInfo: xid %u, oid %u, tid(%u, %u), lastXid %lu,  "
						"globalRecycleXid %lu, globalFrozenXid %lu. "
						"undolog: urp: %lu, logno %d, insertURecPtr %lu, forceDiscardURecPtr "
						"%lu, discardURecPtr %lu, recycleXid %lu. Snapshot: type %d, xmin %u.",
						state, (*visible_tuple)->disk_tuple->flag,
						current_tuple->disk_tuple->flag, xinfo.xid.value,
						xinfo.urec_add, GetTopTransactionIdIfAny(),
						(*visible_tuple)->table_oid, blkno, offnum, last_xid.value,
						pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid),
						pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid), urp, logno,
						UndoGetInsertURecPtr(ulog), UndoGetForceDiscardURecPtr(ulog),
						UndoGetDiscardURecPtr(ulog), UndoGetRecycleXid(ulog).value,
						snapshot->snapshot_type, snapshot->xmin);
		else if (state == UNDO_TRAVERSAL_END)
		{
			FullTransactionId tup_xid = InvalidFullTransactionId;
			Oid			table_oid = InvalidOid;

			if (current_tuple != NULL)
			{
				tup_xid = XHeapTupleGetModifiedXid(current_tuple);
				table_oid = current_tuple->table_oid;
			}
			elog(ERROR, "snapshot too old! we are fetching undo record. "
				"urp %lu, undo state %d, tuple flag %u, tupXid %lu. "
				"xid %lu, undoptr %lu. TransInfo: xid %lu, oid %u, tid(%u, %u), "
				"globalRecycleXid %lu, globalFrozenXid %lu. "
				"undolog: urp: %lu, logno %d, insertURecPtr %lu, "
				"forceDiscardURecPtr %lu, discardURecPtr %lu, recycleXid %lu. "
				"Snapshot: type %d, xmin %u.",
				urec_add, state, hdr.flag, tup_xid.value, xinfo.xid.value,
				xinfo.urec_add, GetTopFullTransactionIdIfAny().value, table_oid, blkno, offnum,
				pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid),
				pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid),
				urp, logno, UndoGetInsertURecPtr(ulog),
				UndoGetForceDiscardURecPtr(ulog),
				UndoGetDiscardURecPtr(ulog), UndoGetRecycleXid(ulog).value,
				snapshot->snapshot_type, snapshot->xmin);
		}
		else if (state != UNDO_TRAVERSAL_COMPLETE ||
				 TransactionIdOlderThanAllUndo(xinfo.xid))
		{
			SetXTupleXminXmax(FrozenTransactionId, save_xmax, visible_tuple);
			break;
		}

		op = xheap_tid_op_from_infomask(hdr.flag);

		/* can't further operate on deleted or non-inplace-updated tuple */
		Assert(op != XTUPLETID_GONE);

		if (xinfo.cid != InvalidCommandId)
			have_cid = true;

		/*
		 * The tuple must be all visible if the transaction slot is cleared or
		 * latest xid that has changed the tuple is too old that it is
		 * all-visible or it precedes smallest xid that has undo.
		 *
		 * For snapshot_toast, the first undo tuple is the visible one
		 */
		if (TransactionIdOlderThanAllUndo(xinfo.xid) ||
			(snapshot != NULL && snapshot->snapshot_type == SNAPSHOT_TOAST))
		{
			SetXTupleXminXmax(FrozenTransactionId, save_xmax, visible_tuple);
			break;
		}

		/* Preliminary visibility check, without relying on the CID. */
		selector = xheap_check_undo_snapshot(snapshot, xinfo, curcid, op, buffer);
		/* If necessary, get and check CID. */
		if (selector == XVERSION_CHECK_CID)
		{
			if (!have_cid)
			{
				state = fetch_transinfo_from_undo(xinfo.urec_add, blkno, offnum, xinfo.xid, &xinfo.cid, NULL,
											   false, NULL, NULL);
				Assert(state != UNDO_TRAVERSAL_ABORT);
				have_cid = true;
			}

			/* OK, now we can make a final visibility decision. */
			selector = xheap_check_cid(op, xinfo.cid, curcid, NULL);
		}

		/* Return the current version, or nothing, if appropriate. */
		if (selector == XVERSION_CURRENT)
		{
			SetXTupleXminXmax(XidFromFullTransactionId(xinfo.xid), save_xmax, visible_tuple);
			break;
		}

		if (selector == XVERSION_NONE)
		{
			if (visible_tuple != NULL)
			{
				if (free_tuple)
					pfree(*visible_tuple);
				*visible_tuple = NULL;
			}
			return false;
		}

		/* Need to check next older version, so loop around. */
		Assert(selector == XVERSION_OLDER);
		urec_add = xinfo.urec_add;
		prev_undo_xid = xinfo.xid;
		save_xmax = XidFromFullTransactionId(xinfo.xid);
	}

	/* Copy latest header reconstructed from undo back into tuple. */
	if (visible_tuple != NULL && *visible_tuple != NULL)
		memcpy((*visible_tuple)->disk_tuple, &hdr, SizeOfXHeapDiskTupleData);

	return true;
}

/*
 * committed_xid_visible_in_snapshot
 *		Is the given XID visible according to the snapshot?
 *
 * This is the same as XidVisibleInSnapshot, but the caller knows that the
 * given XID committed. The only question is whether it's visible to our
 * snapshot or not.
 */
bool
committed_xid_visible_in_snapshot(FullTransactionId xid, Snapshot snapshot)
{
	Assert(xstore_transaction_id_did_commit(xid));

	return !XidInMVCCSnapshot(XidFromFullTransactionId(xid), snapshot);
}