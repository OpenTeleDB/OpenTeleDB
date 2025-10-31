/* -------------------------------------------------------------------------
 *
 * xbtvisibility.c
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * src/xbtree/xbtvisibility.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/itup.h"
#include "access/transam.h"
#include "access/xact.h"
#include "undo/undorecord.h"
#include "undo/undofetch.h"
#include "xbtree/xbtvisbility.h"
#include "xbtree/xbttup.h"
#include "xbtree/xbtundo.h"
#include "storage/itemid.h"
#include "storage/procarray.h"
#include "undo/undotxn.h"
#include "undo/undolog.h"
#include "undo/undorequest.h"
#include "utils/snapshot.h"
#include "utils/snapmgr.h"
#include "util/xxact.h"
#include "xstore.h"

XBTreeTupleOper
xbtree_oper_from_lp(ItemId item)
{
	Assert(IndexItemIdIsInserted(item) || IndexItemIdIsDeleted(item));
	if (IndexItemIdIsInserted(item))
		return XBTREETUPLE_NEW;
	else
		return XBTREETUPLE_GONE;
}

/*
 * xheap_select_version_mvcc
 *
 * Decide, for a given MVCC snapshot, whether we should return the current
 * version of a tuple, an older version, or no version at all.  We only have
 * the XID available here, so if the CID turns out to be relevant, we must
 * return XVERSION_CHECK_CID; caller is responsible for calling XHeapCheckCID
 * with the appropriate CID to obtain a final answer.
 */
static XBTreeVersionSelector
xbtree_select_version_mvcc(XBTreeTupleOper op, FullTransactionId xid, Snapshot snapshot)
{
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
			return XBTREEVERSION_CHECK_CID;

		/* Nothing has changed since our scan started. */
		return ((op == XBTREETUPLE_GONE) ? XBTREEVERSION_NONE : XBTREEVERSION_CURRENT);
	}
	else if (xid_visible_in_snapshot(xid, snapshot, NULL))
	{
		/* The XID is visible to us. */
		return ((op == XBTREETUPLE_GONE) ? XBTREEVERSION_NONE : XBTREEVERSION_CURRENT);
	}
	else
	{
		/*
         * The XID is not visible to us, either because it aborted or because
         * it's in our MVCC snapshot.  If this is a new tuple, that means we
         * can't see it at all; otherwise, we need to check older versions.
         */
		return ((op == XBTREETUPLE_NEW) ? XBTREEVERSION_NONE : XBTREEVERSION_OLDER);
	}
}

/*
 * xbtree_select_version_self_transaction
 */
static XBTreeVersionSelector
xbtree_select_version_self_transaction(XBTreeTupleOper op, FullTransactionId xid)
{
	if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
		/* This transaction is still running and belongs to the current
		 * session. SnapshotNow always attached with current Command Id.
		 * Nothing has changed since our scan started.
		 */
		return ((op == XBTREETUPLE_GONE) ? XBTREEVERSION_NONE : XBTREEVERSION_CURRENT);
	if (!xstore_transaction_id_did_commit(xid))
		/* The XID is not visible to us */
		return ((op == XBTREETUPLE_NEW) ? XBTREEVERSION_NONE : XBTREEVERSION_OLDER);
	/* The XID is visible to us. */
	return ((op == XBTREETUPLE_GONE) ? XBTREEVERSION_NONE : XBTREEVERSION_CURRENT);
}

static XBTreeVersionSelector
xbtree_select_version_not_self(XBTreeTupleOper op, FullTransactionId xid)
{
	if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
		/* This transaction is still running and belongs to the current
		 * session. SnapshotNow always attached with current Command Id.
		 * Nothing has changed since our scan started.
		 */
		return ((op == XBTREETUPLE_GONE) ? XBTREEVERSION_CURRENT : XBTREEVERSION_NONE);
	if (!xstore_transaction_id_did_commit(xid))
		/* The XID is not visible to us */
		return ((op == XBTREETUPLE_NEW) ? XBTREEVERSION_NONE : XBTREEVERSION_OLDER);
	/* The XID is visible to us. */
	return ((op == XBTREETUPLE_GONE) ? XBTREEVERSION_NONE : XBTREEVERSION_CURRENT);
}

static XBTreeVersionSelector
xbtree_select_version_self(XBTreeTupleOper op, FullTransactionId xid)
{
	if (op == XBTREETUPLE_GONE)
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
			return XBTREEVERSION_NONE;
		else if (xstore_transaction_id_is_in_progress(xid))
			return XBTREEVERSION_OLDER;
		else if (xstore_transaction_id_did_commit(xid))
			return XBTREEVERSION_NONE;
		else
			return XBTREEVERSION_OLDER; /* transaction is aborted */
	}
	else
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
			return XBTREEVERSION_CURRENT;
		else if (xstore_transaction_id_is_in_progress(xid))
			return XBTREEVERSION_NONE;
		else if (xstore_transaction_id_did_commit(xid))
			return XBTREEVERSION_CURRENT;
		else
			return XBTREEVERSION_NONE; /* transaction is aborted */
	}
}

/*
 * xbtree_select_version_dirty
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
 *                                           (b) Latest operation on tuple is Insert, In-Place update or tuple is
 * locked and the transaction that has performed operation is current
 * transaction or is in-progress or is committed.
 */

static XBTreeVersionSelector
xbtree_select_version_dirty(const XBTreeTupleOper op, Snapshot snapshot, FullTransactionId xid)
{
	snapshot->xmin = snapshot->xmax = InvalidTransactionId;
	snapshot->speculativeToken = 0;
	if (op == XBTREETUPLE_GONE)
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
			return XBTREEVERSION_NONE;
		else if (xstore_transaction_id_is_in_progress(xid))
		{
			snapshot->xmax = XidFromFullTransactionId(xid);
			return XBTREEVERSION_CURRENT;
		}
		else if (xstore_transaction_id_did_commit(xid))
			/* tuple is deleted or non-inplace-updated */
			return XBTREEVERSION_NONE;
		else
			/* transaction is aborted */
			return XBTREEVERSION_OLDER;
	}
	else
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
			return XBTREEVERSION_CURRENT;
		else if (xstore_transaction_id_is_in_progress(xid))
		{
			snapshot->xmin = XidFromFullTransactionId(xid);
			return XBTREEVERSION_CURRENT; /* in insertion by other */
		}
		else if (xstore_transaction_id_did_commit(xid))
			return XBTREEVERSION_CURRENT;
		else
			/* inserting transaction aborted */
			return XBTREEVERSION_NONE;
	}
}

/*
 * xbtree_select_version_non_vacuumable is the xstore version like HeapTupleSatisfiesNonVacuumable.
 * True if tuple might be visible to some transaction; false if it's surely dead to everyone, ie, 
 * vacuumable. For xbtree, it's only used for optmizer to estimate the range for a xbtree index.
 */
static XBTreeVersionSelector 
xbtree_select_version_non_vacuumable(XBTreeTupleOper op, Snapshot snapshot, FullTransactionId xid)
{
	FullTransactionId oldestxmin;
	oldestxmin.value = pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid);
	if (op == XBTREETUPLE_GONE)
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
			return XBTREEVERSION_CURRENT;
		else if (xstore_transaction_id_is_in_progress(xid))
			return XBTREEVERSION_CURRENT;
		else if (xstore_transaction_id_did_commit(xid))
		{
			if (FullTransactionIdPrecedes(xid, oldestxmin))
				return XBTREEVERSION_NONE;
			else
				return XBTREEVERSION_CURRENT;
		}
		else
			/* transaction is aborted */
			return XBTREEVERSION_OLDER;
	}
	else 
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
			return XBTREEVERSION_CURRENT;
		else if (xstore_transaction_id_is_in_progress(xid))
			return XBTREEVERSION_CURRENT;
		else if (xstore_transaction_id_did_commit(xid))
			return XBTREEVERSION_CURRENT;
		else
			/* transaction is aborted */
			return XBTREEVERSION_NONE;
	}
}

/*
 * xbtree_tuple_version_select
 *
 * Determine whether (a) the current version of the tuple is visible to the
 * snapshot, (b) no version of the tuple is visible to the snapshot, or
 * (c) the previous version of the tuple should be looked up into the undo
 * log to determine which version, if any, is visible.
 *
 * This function can only handle certain types of snapshots; it is a helper
 * function for xheap_tuple_fetch, not a general-purpose facility.
 */
XBTreeVersionSelector
xbtree_tuple_version_select(XBTreeTupleOper op, Snapshot snapshot, FullTransactionId modified_xid)
{
	XBTreeVersionSelector selector = XBTREEVERSION_NONE;
	if (TransactionIdOlderThanAllUndo(modified_xid))
	{
		/*
         * The tuple is not associated with a transaction slot that is new
         * enough to matter, so all changes previously made to the tuple are
         * now all-visible.  If the last operation performed was a delete or a
         * non-inplace update, the tuple is now effectively gone; if it was an
         * insert, use the current version.
         */
		selector = (op == XBTREETUPLE_GONE) ? XBTREEVERSION_NONE : XBTREEVERSION_CURRENT;
	}
	else if (snapshot->snapshot_type == SNAPSHOT_MVCC || IsMVCCSnapshot(snapshot))
	{
		/*
         * NOTICE: We distinguish and SNAPSHOT_MVCC in xheap_select_version_mvcc codes.
         */
		selector = xbtree_select_version_mvcc(op, modified_xid, snapshot);
	}
	else if (snapshot->snapshot_type == SNAPSHOT_SELF_TRANSACTION)
		selector = xbtree_select_version_self_transaction(op, modified_xid);
	else if (snapshot->snapshot_type == SNAPSHOT_SELF)
		selector = xbtree_select_version_self(op, modified_xid);
	else if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
		selector = xbtree_select_version_dirty(op, snapshot, modified_xid);
	else if (snapshot->snapshot_type == SNAPSHOT_TOAST)
	{
		/* 
         * toast table visibility already was checked in main table, always visible
         */
		selector = XBTREEVERSION_CURRENT;
	}
	else if (snapshot->snapshot_type == SNAPSHOT_NON_VACUUMABLE)
		selector = xbtree_select_version_non_vacuumable(op, snapshot, modified_xid);
	else if (snapshot->snapshot_type == SNAPSHOT_NOT_SELF)
		selector = xbtree_select_version_not_self(op, modified_xid);
	else
		elog(ERROR, "unsupported snapshot style %d", (int) snapshot->snapshot_type);
	return selector;
}

/*
 * XBTreeVersionSelector
 *
 * For a tuple whose xid satisfies TransactionIdIsCurrentTransactionId(xid),
 * this function makes a determination about tuple visibility based on CID.
 */
XBTreeVersionSelector
xbtree_check_cid(XBTreeTupleOper op, CommandId tuple_cid, CommandId visibility_cid)
{
	if (op == XBTREETUPLE_GONE)
	{
		if (tuple_cid >= visibility_cid)
			return XBTREEVERSION_OLDER; /* deleted after scan started */

		else
			return XBTREEVERSION_NONE; /* deleted before scan started */
	}
	else
	{
		if (tuple_cid >= visibility_cid)
			return XBTREEVERSION_NONE; /* inserted after scan started */
		else
			return XBTREEVERSION_CURRENT; /* inserted before scan started */
	}
}

static inline XBTreeVersionSelector
xbtree_check_undo_snapshot(XBTreeTupleOper op, Snapshot snapshot, FullTransactionId xid)
{
	if (IsMVCCSnapshot(snapshot))
		return xbtree_select_version_mvcc(op, xid, snapshot);
	else if (snapshot->snapshot_type == SNAPSHOT_SELF_TRANSACTION)
		return xbtree_select_version_self_transaction(op, xid);
	else if (snapshot->snapshot_type == SNAPSHOT_NON_VACUUMABLE)
		return xbtree_select_version_non_vacuumable(op, snapshot, xid);
	else
		return xbtree_select_version_self(op, xid);
}

static UndoTraversalState
xbtree_get_tuple_from_undo_record(UndoRecPtr urecPtr, FullTransactionId xid, XBTreeTupleTransInfo *xinfo)
{
	UndoTraversalState	state;
	UnpackedUndoRecord 			*urec = new_undo_record();
	FullTransactionId	lastXid;
	urec->uur_urp = urecPtr;
	urec->mem_ctx = CurrentMemoryContext;

	state = fetch_undo_record(urec, xid, false,
							&lastXid,satisfy_undo_record);
	if (state != UNDO_TRAVERSAL_COMPLETE)
	{
		destroy_undo_record(urec);
		return state;
	}

	xinfo->urec = GetUndoRecordTpprev(urec);
	xinfo->cid = InvalidCommandId;
	xinfo->xid = GetUndoRecordOldXactId(urec);
	destroy_undo_record(urec);

	return state;
}

bool
xbtree_get_tuple_from_undo(UndoRecPtr urec, XBTreeIndexTuple current_tuple, Snapshot snapshot, CommandId curcid)
{
	FullTransactionId		prev_undo_xid = InvalidFullTransactionId;
	XBTreeTupleTransInfo 	xinfo;
	UndoTraversalState		state = UNDO_TRAVERSAL_DEFAULT;

	/*
     * tuple is modified after the scan is started, fetch the prior record
     * from undo to see if it is visible. loop until we find the visible
     * version.
     */
	while (1)
	{
		XBTreeVersionSelector 	selector = XBTREEVERSION_NONE;
		XBTreeTupleOper	 		op = XBTREETUPLE_NEW;
		state = xbtree_get_tuple_from_undo_record(urec, prev_undo_xid, &xinfo);
		if (state == UNDO_TRAVERSAL_ABORT || state == UNDO_TRAVERSAL_END)
		{
			elog(ERROR, "snapshot too old! maybe undo record has been discarded. state %d, xid: xid %lu, undoptr %lu. "
					"globalFrozenXid %lu.Snapshot: type %d, xmin %u.", state, U64FromFullTransactionId(xinfo.xid), xinfo.urec, 
					pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid), snapshot->snapshot_type, snapshot->xmin);
		}
		else if (state != UNDO_TRAVERSAL_COMPLETE ||
				 TransactionIdOlderThanAllUndo(xinfo.xid))
		{
			break;
		}

		/*
         * The tuple must be all visible if the transaction slot is cleared or
         * latest xid that has changed the tuple is too old that it is
         * all-visible or it precedes smallest xid that has undo.
         *
         * For snapshot_toast, the first undo tuple is the visible one
         */
		if (snapshot != NULL && snapshot->snapshot_type == SNAPSHOT_TOAST)
		{
			break;
		}

		/* Preliminary visibility check, without relying on the CID. */
		selector = xbtree_check_undo_snapshot(op, snapshot, xinfo.xid);
		/* If necessary, get and check CID. */
		if (selector == XBTREEVERSION_CHECK_CID)
		{
			state = fetch_transinfo_from_undo(xinfo.urec, InvalidBlockNumber, InvalidOffsetNumber, 
											xinfo.xid, &xinfo.cid, NULL,
											   false, NULL, NULL);
			Assert(state != UNDO_TRAVERSAL_ABORT);

			/* OK, now we can make a final visibility decision. */
			selector = xbtree_check_cid(op, xinfo.cid, curcid);
		}

		/* Return the current version, or nothing, if appropriate. */
		if (selector == XBTREEVERSION_CURRENT)
		{
			break;
		}

		if (selector == XBTREEVERSION_NONE)
		{
			return false;
		}

		/* Need to check next older version, so loop around. */
		Assert(selector == XBTREEVERSION_OLDER);
		urec = xinfo.urec;
		prev_undo_xid = xinfo.xid;
	}

	return true;
}

bool
_xbt_tuple_satisfies(Page page, Snapshot snapshot, BlockNumber blk, OffsetNumber offnum)
{
	XBTreeTupleOper op;
	XBTreeVersionSelector selector;
	bool is_visible;
	IndexTuple itup;
	CommandId cid;
	XBTreeIndexTuple xbt_tuple;
	UndoTraversalState state = UNDO_TRAVERSAL_DEFAULT;
	ItemId item = PageGetItemId(page, offnum);
	if (!IndexItemIdIsInserted(item) && !IndexItemIdIsDeleted(item))
		return false;
	op = xbtree_oper_from_lp(item);
	itup = (IndexTuple) PageGetItem(page, item);
	
	xbt_tuple = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);
	
	/*
	 * If this IndexTuple is not visible to the current Snapshot, try to get the next one.
	 * We're not going to tell heap to skip visibility check, because it doesn't cost a lot and we need heap
	 * to check the visibility with CID when snapshot's xid equals to xmin or xmax.
	 */
	selector = xbtree_tuple_version_select(op, snapshot, xbt_tuple->modified_xid);

	/* try to fetch sub transaction xid from undo record */
	if (snapshot->snapshot_type == SNAPSHOT_DIRTY && 
		(TransactionIdIsValid(snapshot->xmin) || TransactionIdIsValid(snapshot->xmax)))
	{
		FullTransactionId subxid = InvalidFullTransactionId;
		if(TransactionIdIsValid(snapshot->xmin))
		{
			state = fetch_subxid_from_undo(xbt_tuple->urec, &subxid);
			if (state == UNDO_TRAVERSAL_COMPLETE && FullTransactionIdIsValid(subxid))
				snapshot->xmin = XidFromFullTransactionId(subxid);
		} else {
			state = fetch_subxid_from_undo(xbt_tuple->urec, &subxid);
			if (state == UNDO_TRAVERSAL_COMPLETE && FullTransactionIdIsValid(subxid))
				snapshot->xmax = XidFromFullTransactionId(subxid);
		}
	}

	if (selector == XBTREEVERSION_CHECK_CID)
	{
		ItemPointerData ctid;
		state = fetch_transinfo_from_undo(xbt_tuple->urec, blk, offnum, 
									InvalidFullTransactionId, &cid,
									&ctid, false, NULL, NULL);
		Assert(state != UNDO_TRAVERSAL_ABORT);
		selector = xbtree_check_cid(op, cid, snapshot->curcid);
	}
	/*
     * If we decided that we need to consult the undo log to figure out what
     * version our snapshot can see, call GetTupleFromUndo to fetch it.
     */
	if (selector == XBTREEVERSION_OLDER)
		is_visible = xbtree_get_tuple_from_undo(xbt_tuple->urec, xbt_tuple, 
			snapshot, cid);
	else if (selector == XBTREEVERSION_CURRENT)
		is_visible = true;
	else
		is_visible = false;
	return is_visible;
}

void
xbt_tuple_satisfies_update(Relation rel, Buffer buf, Page page, BlockNumber blk, OffsetNumber offnum)
{
	XBTreeTupleOper op;
	XBTreeIndexTuple xbt_tuple;
	IndexTuple itup;
	ItemId item = PageGetItemId(page, offnum);
	if (!IndexItemIdIsInserted(item) && !IndexItemIdIsDeleted(item))
		return;
	op = xbtree_oper_from_lp(item);
	itup = (IndexTuple) PageGetItem(page, item);
	
	xbt_tuple = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);

	if(op == XBTREETUPLE_GONE)
	{
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xbt_tuple->modified_xid))) 
		{
			
		} 
		else if (xstore_transaction_id_is_in_progress(xbt_tuple->modified_xid))
		{

		}
		else if(xstore_transaction_id_did_commit(xbt_tuple->modified_xid))
		{

		}
		else 
		{
			UndoTraversalState	state;
			UnpackedUndoRecord 			*urec = new_undo_record();
			FullTransactionId	lastXid;
			urec->uur_urp = xbt_tuple->urec;
			urec->mem_ctx = CurrentMemoryContext;

			state = fetch_undo_record(urec, xbt_tuple->modified_xid, false,
									&lastXid, satisfy_undo_record);
			if (state != UNDO_TRAVERSAL_COMPLETE)
			{
				destroy_undo_record(urec);
				return;
			}
			execute_undo_delete_xbtree_page(rel, urec, buf, page, offnum);
			destroy_undo_record(urec);
		}
	}
	return;
}