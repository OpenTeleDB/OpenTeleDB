/* -------------------------------------------------------------------------
 *
 * xheapam_handler.c
 * tableam for xheap.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * src/xheap/xheapam_handler.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tableam.h"
#include "access/transam.h"
#include "access/heaptoast.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "access/rewriteheap.h"
#include "access/syncscan.h"
#include "access/tsmapi.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "storage/buf.h"
#include "storage/itemptr.h"
#include "storage/smgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/backend_progress.h"
#include "utils/elog.h"
#include "utils/fmgroids.h"
#include "utils/palloc.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "xstore.h"
#include "xheap/xcluster.h"
#include "xheap/xheap.h"
#include "xheap/xheap_am.h"
#include "xheap/xpage.h"
#include "xheap/xtuple.h"
#include "xheap/xtuptoaster.h"
#include "xheap/xheapam_visibility.h"
#include "xheap/xscan.h"
#include "xheap/xtup_details.h"
#include "xheap/xtupleslot.h"
#include "xheap/xlock.h"
#include "access/multixact.h"


static void reform_and_rewrite_xtuple(XHeapTuple tuple,
									  TupleDesc oldTupDesc,
									  TupleDesc newTupDesc,
									  Datum *values,
									  bool *isnull,
									  RewriteState rwstate);


/**
 * ------------------------------------------------------------------------
 * XHeap AMs below
 * ------------------------------------------------------------------------
 */

/* ------------------------------------------------------------------------
 * Slot related callbacks for xheap AM
 * ------------------------------------------------------------------------
 */

static const TupleTableSlotOps *
xheapam_slot_callbacks(Relation relation)
{
	return &TTSOpsXHeapTuple;
}

/* ------------------------------------------------------------------------
 * Scan Callbacks for xheap AM
 * ------------------------------------------------------------------------
 */

static TableScanDesc
xheapam_scan_begin(Relation relation, Snapshot snapshot,
				int nkeys, ScanKey key,
				ParallelTableScanDesc parallel_scan,
				uint32 flags)
{
	TableScanDesc scan;

	Assert(nkeys == 0 && key == NULL);

	scan = xheap_beginscan(relation, snapshot,  parallel_scan, flags);
	return scan;
}

static void
xheapam_scan_end(TableScanDesc sscan)
{
	xheap_end_scan(sscan);
}

static void
xheapam_scan_rescan(TableScanDesc sscan, ScanKey key, bool set_params,
			bool allow_strat, bool allow_sync, bool allow_pagemode)
{
	XHeapScanDesc scan = (XHeapScanDesc) sscan;

	Assert(key == NULL);

	if (set_params)
	{
		if (allow_strat)
			scan->rs_base.rs_flags |= SO_ALLOW_STRAT;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_STRAT;

		if (allow_sync)
			scan->rs_base.rs_flags |= SO_ALLOW_SYNC;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_SYNC;

		if (allow_pagemode && scan->rs_base.rs_snapshot &&
			IsMVCCSnapshot(scan->rs_base.rs_snapshot))
			scan->rs_base.rs_flags |= SO_ALLOW_PAGEMODE;
		else
			scan->rs_base.rs_flags &= ~SO_ALLOW_PAGEMODE;
	}

	xheap_rescan(sscan, key);
}

static bool
xheapam_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(slot->tts_mcxt);

	XHeapTuple xtuple = xheap_getnext(sscan, direction, NULL);

	MemoryContextSwitchTo(oldcontext);

	if (xtuple == NULL)
	{
		/* clear the slot to make scanstate->ss_ScanTupleSlot empty, avoid CURRENT OF cursor issue. */
		ExecClearTuple(slot);
		return false;
	}

	xheap_slot_store_xheap_tuple(xtuple, slot, false, false);

	return true;
}

static void
xheapam_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
				  ItemPointer maxtid)
{
	XHeapScanDesc scan = (XHeapScanDesc) sscan;
	BlockNumber start_blk;
	BlockNumber num_blks;
	ItemPointerData highest_item;
	ItemPointerData lowest_item;

	/*
	 * For relations without any pages, we can simply leave the TID range
	 * unset.  There will be no tuples to scan, therefore no tuples outside
	 * the given TID range.
	 */
	if (scan->rs_nblocks == 0)
		return;

	/*
	 * Set up some ItemPointers which point to the first and last possible
	 * tuples in the heap.
	 */
	ItemPointerSet(&highest_item, scan->rs_nblocks - 1, MaxOffsetNumber);
	ItemPointerSet(&lowest_item, 0, FirstOffsetNumber);

	/*
	 * If the given maximum TID is below the highest possible TID in the
	 * relation, then restrict the range to that, otherwise we scan to the end
	 * of the relation.
	 */
	if (ItemPointerCompare(maxtid, &highest_item) < 0)
		ItemPointerCopy(maxtid, &highest_item);

	/*
	 * If the given minimum TID is above the lowest possible TID in the
	 * relation, then restrict the range to only scan for TIDs above that.
	 */
	if (ItemPointerCompare(mintid, &lowest_item) > 0)
		ItemPointerCopy(mintid, &lowest_item);

	/*
	 * Check for an empty range and protect from would be negative results
	 * from the numBlks calculation below.
	 */
	if (ItemPointerCompare(&highest_item, &lowest_item) < 0)
	{
		/* Set an empty range of blocks to scan */
		scan->rs_startblock = 0;
		scan->rs_numblocks = 0;
		return;
	}

	/*
	 * Calculate the first block and the number of blocks we must scan. We
	 * could be more aggressive here and perform some more validation to try
	 * and further narrow the scope of blocks to scan by checking if the
	 * lowerItem has an offset above MaxOffsetNumber.  In this case, we could
	 * advance startBlk by one.  Likewise, if highestItem has an offset of 0
	 * we could scan one fewer blocks.  However, such an optimization does not
	 * seem worth troubling over, currently.
	 */
	start_blk = ItemPointerGetBlockNumberNoCheck(&lowest_item);

	num_blks = ItemPointerGetBlockNumberNoCheck(&highest_item) -
		ItemPointerGetBlockNumberNoCheck(&lowest_item) + 1;

	/* Set the start block and number of blocks to scan */
	scan->rs_startblock = start_blk;
	scan->rs_numblocks = num_blks;

	/* Finally, set the TID range in sscan */
	ItemPointerCopy(&lowest_item, &sscan->rs_mintid);
	ItemPointerCopy(&highest_item, &sscan->rs_maxtid);
}

static bool
xheapam_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
						  TupleTableSlot *slot)
{
	XHeapScanDesc scan = (XHeapScanDesc) sscan;
	ItemPointer mintid = &sscan->rs_mintid;
	ItemPointer maxtid = &sscan->rs_maxtid;
	MemoryContext oldcontext = MemoryContextSwitchTo(slot->tts_mcxt);

	for (;;)
	{
		scan->rs_cutup = xheap_getnext(sscan, direction, NULL);

		if (scan->rs_cutup == NULL)
		{
			ExecClearTuple(slot);
			MemoryContextSwitchTo(oldcontext);
			return false;
		}

		/*
		 * heap_set_tidrange will have used heap_setscanlimits to limit the
		 * range of pages we scan to only ones that can contain the TID range
		 * we're scanning for.  Here we must filter out any tuples from these
		 * pages that are outwith that range.
		 */
		if (ItemPointerCompare(&scan->rs_cutup->ctid, mintid) < 0)
		{
			ExecClearTuple(slot);

			/*
			 * When scanning backwards, the TIDs will be in descending order.
			 * Future tuples in this direction will be lower still, so we can
			 * just return false to indicate there will be no more tuples.
			 */
			if (ScanDirectionIsBackward(direction))
			{
				MemoryContextSwitchTo(oldcontext);
				return false;
			}

			continue;
		}

		/*
		 * Likewise for the final page, we must filter out TIDs greater than
		 * maxtid.
		 */
		if (ItemPointerCompare(&scan->rs_cutup->ctid, maxtid) > 0)
		{
			ExecClearTuple(slot);

			/*
			 * When scanning forward, the TIDs will be in ascending order.
			 * Future tuples in this direction will be higher still, so we can
			 * just return false to indicate there will be no more tuples.
			 */
			if (ScanDirectionIsForward(direction))
			{
				MemoryContextSwitchTo(oldcontext);
				return false;
			}
			continue;
		}

		break;
	}

	xheap_slot_store_xheap_tuple(scan->rs_cutup, slot, false, false);
	MemoryContextSwitchTo(oldcontext);
	return true;
}

/* ----------------------------------------------------------------------------
 * Helper functions to implement parallel scans for xheap AMs.
 * ----------------------------------------------------------------------------
 */

static Size
xheapam_parallelscan_estimate(Relation rel)
{
	return sizeof(ParallelBlockTableScanDescData);
}

static Size
xheapam_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	ParallelBlockTableScanDesc bpscan = (ParallelBlockTableScanDesc) pscan;

	bpscan->base.phs_relid = RelationGetRelid(rel);
	bpscan->phs_nblocks = RelationGetNumberOfBlocks(rel);
	/* compare phs_syncscan initialization to similar logic in initscan */
	bpscan->base.phs_syncscan = synchronize_seqscans &&
			!RelationUsesLocalBuffers(rel) &&
			bpscan->phs_nblocks > NBuffers / 4;
	SpinLockInit(&bpscan->phs_mutex);
	bpscan->phs_startblock = InvalidBlockNumber;
	pg_atomic_init_u64(&bpscan->phs_nallocated, 0);

	return sizeof(ParallelBlockTableScanDescData);
}

static void
xheapam_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	ParallelBlockTableScanDesc bpscan = (ParallelBlockTableScanDesc) pscan;

	pg_atomic_write_u64(&bpscan->phs_nallocated, 0);
}

/* ----------------------------------------------------------------------------
 *  Functions for manipulations of physical tuples for xheap AM.
 * ----------------------------------------------------------------------------
 */

static void
xheapam_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
					int options, BulkInsertState bistate)
{
	XHeapTuple  xtuple;
	bool		should_free = false;

	xtuple = exec_fetch_slot_xheap_tuple(slot, &should_free);

	xtuple->table_oid = slot->tts_tableOid = RelationGetRelid(relation);

	xheap_insert(relation, xtuple, cid, options, bistate, false);

	ItemPointerCopy(&(xtuple->ctid), &(slot->tts_tid));

	if (should_free)
		pfree(xtuple);
}

static void
xheapam_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
								CommandId cid, int options,
								BulkInsertState bistate, uint32 spec_token)
{
	xheapam_tuple_insert(relation, slot, cid, options, bistate);
}

static void
xheapam_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
								  uint32 spec_token, bool succeeded)
{
	XHeapTuple xtuple = NULL;

	/* Do nothing if succeeded. */
	if (succeeded)
		return;

	Assert(TTS_TABLEAM_IS_XSTORE(slot));
	Assert(!TTS_EMPTY(slot));

	xtuple = ((XHeapTupleTableSlot *)slot)->tuple;
	/* Slot must have been materialized in tuple_insert_speculative. */
	Assert(xtuple && TTS_SHOULDFREE(slot));

	/* Rollback if speculative insertion failed. */
	xheap_abort_speculative(relation, xtuple);
}

static void
xheapam_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
				  CommandId cid, int options, BulkInsertState bistate)
{
	int i = 0;
	XHeapTuple* xtuples = palloc(sizeof(XHeapTuple) * ntuples);
	bool* 		should_frees = palloc(sizeof(bool) * ntuples);

	for (i = 0; i < ntuples; i++)
	{
		xtuples[i] = exec_fetch_slot_xheap_tuple(slots[i], &(should_frees[i]));

		xtuples[i]->table_oid = slots[i]->tts_tableOid = RelationGetRelid(relation);
	}

	xheap_multi_insert(relation, xtuples, ntuples, cid, options, bistate);

	for (i = 0; i < ntuples; i++)
	{
		slots[i]->tts_tid = xtuples[i]->ctid;
		if (should_frees[i])
			pfree(xtuples[i]);
	}

	pfree(xtuples);
	pfree(should_frees);
}

static TM_Result
xheapam_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
					Snapshot snapshot, Snapshot crosscheck, bool wait,
					TM_FailureData *tmfd, bool changing_part)
{
	return xheap_delete(relation, tid, cid, crosscheck,
					snapshot, wait,
					tmfd, changing_part);
}


static TM_Result
xheapam_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
					CommandId cid, Snapshot snapshot, Snapshot crosscheck,
					bool wait, TM_FailureData *tmfd,
					LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	TM_Result res = TM_Ok;
	XHeapTuple 	xtuple;
	bool		should_free = false;

	*update_indexes = TU_None;
	tmfd->should_update_xbtree = true;

	xtuple = exec_fetch_slot_xheap_tuple(slot, &should_free);

	xtuple->table_oid = slot->tts_tableOid = RelationGetRelid(relation);

	res = xheap_update(relation, otid, xtuple, cid, snapshot, crosscheck, wait,
					tmfd, lockmode, &tmfd->modifiedIdxAttrs, &tmfd->inplace_update);

	ItemPointerCopy(&(xtuple->ctid), &(slot->tts_tid));

	if (!XHeapTupleIsInPlaceUpdated(((XHeapTuple)xtuple)->disk_tuple->flag) 
						|| tmfd->modifiedIdxAttrs != NULL)
		*update_indexes = TU_All;

	return res;
}

static TM_Result
xheapam_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
				  TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				  LockWaitPolicy wait_policy, uint8 flags,
				  TM_FailureData *tmfd)
{
	TM_Result   result;
	bool follow_updates = (flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) != 0;
	bool on_conflict_update = (flags & TUPLE_LOCK_FLAG_UPDATE_ON_CONFLICT) !=0;
	XHeapTuple xtuple = (XHeapTuple)xheaptup_alloc(MaxPossibleXHeapTupleSize);
	xtuple->disk_tuple = (XHeapDiskTuple)((char *)xtuple + XHeapTupleDataSize);
	tmfd->traversed = false;

	Assert(TTS_TABLEAM_IS_XSTORE(slot));

	xtuple->ctid = *tid;
	result = xheap_lock_tuple(relation, xtuple, cid, snapshot, mode, wait_policy,
							 follow_updates, tmfd, false, on_conflict_update);
	if (result == TM_Updated &&
		(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
	{
		SnapshotData	snapshot_dirty;
		bool			have_got_tuple pg_attribute_unused() = false;
		Buffer			buffer = InvalidBuffer;
		bool			eval = false;
		FullTransactionId prior_xmax = InvalidFullTransactionId;

		*tid = tmfd->ctid;
		tmfd->traversed = true;

		prior_xmax = FullTransactionIdFromEpochAndXid(tmfd->epoch, tmfd->xmax);
		InitDirtySnapshot(snapshot_dirty);
		snapshot_dirty.xmin = snapshot_dirty.xmax = InvalidTransactionId;

		for (;;)
		{
			bool	fetched = false;
			Buffer	buffer_from_fetch PG_USED_FOR_ASSERTS_ONLY;

			if (ItemPointerIndicatesMovedPartitions(tid))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						errmsg("tuple to be locked was already moved to another partition due to concurrent update")));

			fetched = xheap_fetch(relation, &snapshot_dirty, tid, xtuple, &buffer, true, true,
								NULL);
			buffer_from_fetch = buffer;
			/* buffer lock is released */
			if (fetched)
			{

				/*
				 * Ensure that the tuple is same as what we are expecting.  If
				 * the current or any prior version of tuple doesn't contain
				 * the effect of priorXmax, then the slot must have been
				 * recycled and reused for an unrelated tuple.  This implies
				 * that the latest version of the row was deleted, so we need
				 * do nothing.
				 */
				if (!validate_tuples_xact(relation, buffer, &snapshot_dirty, xtuple, prior_xmax, true,
								true))
				{
					goto out;
				}

				/*
				* We can only reach here through an updating chain, it's impossible that there is a tuple 
				* that was inserting by a running transaction.
				*/
				Assert(snapshot_dirty.xmin == InvalidTransactionId);

				/* 2. wait the current updater if any to terminate and then refetch the latest one */
				if (TransactionIdIsValid(snapshot_dirty.xmax))
				{
					ReleaseBuffer(buffer);
					XactLockTableWait(snapshot_dirty.xmax, NULL, NULL, XLTW_None);
					continue;
				}

				/*
				* If tuple was inserted by our own transaction, we have to
				* do a CID check. If inserted by our own command ID,
				* then we cannot see the tuple, so we should ignore it.
				* Otherwise XHeapLockTuple() will throw an error, and so
				* would any later attempt to update or delete the tuple.
				*/
				if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(prior_xmax)))
				{
					CommandId	tup_cid;

					LockBuffer(buffer, BUFFER_LOCK_SHARE);
					tup_cid = xheap_tuple_get_cid(xtuple, buffer);
					if (tup_cid >= cid)
					{
						LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
						goto out;
					}
					LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				}

				xtuple->ctid = *tid;

				/* 4. try to lock it */
				result = xheap_lock_tuple(relation, xtuple, cid, snapshot, mode,
										LockWaitBlock, true, tmfd, eval, false);

				/* Make sure we release the correct buffer later */
				Assert(buffer_from_fetch == buffer);

				/* 5. handle locking result */
				switch (result)
				{
					case TM_SelfModified:
						ereport(DEBUG5,
								(errmsg("xheap_lock_updated lock tuple result %d", result)));
						Assert(!have_got_tuple);
						goto out;

					case TM_Ok:
						ereport(DEBUG5,
								(errmsg("xheap_lock_updated lock tuple result %d", result)));
						break;	/* successfully locked, don't release buffer before copy the tuple */

					case TM_Updated:
						if (IsolationUsesXactSnapshot())
						{
							ReleaseBuffer(buffer);
							ereport(
								ERROR,
								(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								errmsg(
									"could not serialize access due to concurrent update")));
						}

						if (ItemPointerEquals(&(tmfd->ctid), &(xtuple->ctid)) &&
							!tmfd->in_place_updated_or_locked)
						{
							Assert(!have_got_tuple);
							goto out;
						}

						ReleaseBuffer(buffer);
						eval = true;

						/* Fetch the next tid */
						*tid = tmfd->ctid;
						prior_xmax = FullTransactionIdFromEpochAndXid(tmfd->epoch, tmfd->xmax);
						ereport(DEBUG5,
								(errmsg("xheap_lock_updated lock tuple result %d tmfd {%u:%u "
										"xmax %u cmax %u}",
										result, ItemPointerGetBlockNumber(&tmfd->ctid),
										ItemPointerGetOffsetNumber(&tmfd->ctid), tmfd->xmax,
										tmfd->cmax)));
						continue;
						
					case TM_Deleted:
						if (IsolationUsesXactSnapshot())
						{
							ReleaseBuffer(buffer);
							ereport(
								ERROR,
								(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								errmsg(
									"could not serialize access due to concurrent update")));
						}
						ereport(DEBUG5,
								(errmsg("xheap_lock_updated lock tuple result %d", result)));
						goto out;

					default:
						Assert(0);
				}

				have_got_tuple = true;
				break; /* exit the for loop */
			}

			/* xtuple is null, tuple is deleted */
			if (xtuple->disk_tuple == NULL)
			{
				Assert(!have_got_tuple);
				goto out;
			}

			/* validate priorXmax */
			if (!validate_tuples_xact(relation, buffer, &snapshot_dirty, xtuple, prior_xmax, true,
							true))
			{
				Assert(!have_got_tuple);
				goto out;
			}

			/* itempointer equals, and didn't pass the snapshotDirty protocol, then it's deleted */
			if (ItemPointerEquals(&(xtuple->ctid), tid))
			{
				Assert(!have_got_tuple);
				goto out;
			}

			/* update priorXmax */
			prior_xmax = XHeapTupleGetModifiedXid(xtuple);
			ReleaseBuffer(buffer);
		}

out:
		ReleaseBuffer(buffer);
	}
	
	slot->tts_tableOid = RelationGetRelid(relation);
	xtuple->table_oid = slot->tts_tableOid;

	/* store in slot*/
	xheap_slot_store_xheap_tuple(xtuple, slot, true, false);

	return result;
}

/* ------------------------------------------------------------------------
 * Index Scan Callbacks for xheap AM
 * ------------------------------------------------------------------------
 */

static IndexFetchTableData *
xheapam_index_fetch_begin(Relation rel)
{
	IndexFetchHeapData *hscan = palloc0(sizeof(IndexFetchHeapData));

	hscan->xs_base.rel = rel;
	hscan->xs_cbuf = InvalidBuffer;

	return &hscan->xs_base;
}

static void
xheapam_index_fetch_reset(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	if (BufferIsValid(hscan->xs_cbuf))
	{
		ReleaseBuffer(hscan->xs_cbuf);
		hscan->xs_cbuf = InvalidBuffer;
	}
}

static void
xheapam_index_fetch_end(IndexFetchTableData *scan)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;

	xheapam_index_fetch_reset(scan);

	pfree(hscan);
}

static bool
xheapam_index_fetch_tuple(struct IndexFetchTableData *scan,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot,
						 bool *call_again, bool *all_dead)
{
	IndexFetchHeapData *hscan = (IndexFetchHeapData *) scan;
	Relation	rel = hscan->xs_base.rel;
	Buffer		xs_cbuf = hscan->xs_cbuf;
	XHeapTuple	xheap_tuple = NULL;

	/* Switch to correct buffer if we don't have it already */
	hscan->xs_cbuf = ReleaseAndReadBuffer(xs_cbuf, rel, ItemPointerGetBlockNumber(tid));

	/*
     	* In single mode and hot standby, we may get a null buffer if index
     	* replayed before the tid replayed. This is acceptable, so we return
     	* null without reporting error.
     	*/
	if (RecoveryInProgress() && !BufferIsValid(hscan->xs_cbuf))
		return false;

	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);

	xheap_tuple =
		xheap_search_buffer(tid, rel, hscan->xs_cbuf, snapshot, all_dead, NULL, NULL);

	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);
	*call_again = false;

	if (xheap_tuple == NULL)
	{
		xheap_slot_clean(slot);
		return false;
	}

	xheap_slot_store_xheap_tuple(xheap_tuple, slot, false, false);

	return true;
}

/* ------------------------------------------------------------------------
 * Callbacks for non-modifying operations on individual tuples for xheap AM
 * ------------------------------------------------------------------------
 */

static bool
xheapam_fetch_row_version(Relation relation,
						 ItemPointer tid,
						 Snapshot snapshot,
						 TupleTableSlot *slot)
{
	Buffer buffer = InvalidBuffer;
	XHeapTupleData tup_data;
	XHeapTuple xtuple = &tup_data;

	/* Must set a private data buffer for TidScan. (same as HeapFetchRowVersion) */
	union {
		XHeapDiskTupleData hdr;
		char data[MaxPossibleXHeapTupleSize];
	} tbuf;
	xtuple->disk_tuple = &tbuf.hdr;

	slot->tts_ops->clear(slot);

	if (xheap_fetch(relation, snapshot, tid, xtuple, &buffer, false, false, NULL))
	{
		xheap_slot_store_xheap_tuple(xheap_copy_tuple(xtuple), slot, true, false);
		return true;
	}

	return false;
}

static void
xheapam_get_latest_tid(TableScanDesc sscan,
					ItemPointer tid)
{
	Relation relation = sscan->rs_rd;
	Snapshot snapshot = sscan->rs_snapshot;
	Buffer buffer = InvalidBuffer;
	XHeapTupleData tupData;
	XHeapTuple xtuple = &tupData;

	union {
		XHeapDiskTupleData hdr;
		char data[MaxPossibleXHeapTupleSize];
	} tbuf;
	xtuple->disk_tuple = &tbuf.hdr;

	/*
	 * table_tuple_get_latest_tid() verified that the passed in tid is valid.
	 */
	Assert(ItemPointerIsValid(tid));

	/* Fetch the latest tuple, and get ctid */
	if (xheap_fetch(relation, snapshot, tid, xtuple, &buffer, false, false, NULL))
	{
		*tid = xtuple->ctid;
	}
}

static bool
xheapam_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	XHeapScanDesc xscan = (XHeapScanDesc)scan;
	return ItemPointerIsValid(tid) &&
		ItemPointerGetBlockNumber(tid) < xscan->rs_nblocks;
}

static bool
xheapam_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
								Snapshot snapshot)
{
	ItemPointer tid = NULL;
	XHeapTupleTableSlot *xslot = NULL;
	Buffer buffer = InvalidBuffer;
	bool   res = false;

	Assert(TTS_TABLEAM_IS_XSTORE(slot));
	Assert(!TTS_EMPTY(slot));
	xslot = (XHeapTupleTableSlot *) slot;
	Assert(xslot->tuple);

	tid = &(xslot->tuple->ctid);

	buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	res = xheap_tuple_satisfies_visibility(xslot->tuple, snapshot, buffer);

	UnlockReleaseBuffer(buffer);

	return res;
}

static TransactionId
xheap_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	elog(ERROR, "xheap_index_delete_tuples is not needed for xheap");
	return InvalidTransactionId;
}

/* ------------------------------------------------------------------------
 * DDL related callbacks for xheap AM.
 * ------------------------------------------------------------------------
 */

static void
xheapam_relation_set_new_filelocator(Relation rel,
								 const RelFileLocator *newrlocator,
								 char persistence,
								 TransactionId *freeze_xid,
								 MultiXactId *minmulti)
{
	SMgrRelation srel;

	/*
	 * Initialize to the minimum XID that could put tuples in the table. We
	 * know that no xacts older than RecentXmin are still running, so that
	 * will do.
	 */
	*freeze_xid = RecentXmin;

	/*
	 * Similarly, initialize the minimum Multixact to the first value that
	 * could possibly be stored in tuples in the table.  Running transactions
	 * could reuse values from their local cache, so we are careful to
	 * consider all currently running multis.
	 *
	 * XXX this could be refined further, but is it worth the hassle?
	 */
	*minmulti = GetOldestMultiXactId();

	srel = RelationCreateStorage(*newrlocator, persistence, true);

	/*
	 * If required, set up an init fork for an unlogged table so that it can
	 * be correctly reinitialized on restart.  An immediate sync is required
	 * even if the page has been logged, because the write did not go through
	 * shared_buffers and therefore a concurrent checkpoint may have moved the
	 * redo pointer past our xlog record.  Recovery may as well remove it
	 * while replaying, for example, XLOG_DBASE_CREATE* or XLOG_TBLSPC_CREATE
	 * record. Therefore, logging is necessary even if wal_level=minimal.
	 */
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_MATVIEW ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
		smgrimmedsync(srel, INIT_FORKNUM);
	}

	smgrclose(srel);
}

static void
xheapam_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation    dstrel;
	ForkNumber      fork_num;

	dstrel = smgropen(*newrlocator, rel->rd_backend);
	RelationGetSmgr(rel);

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume no new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	/*
	 * Create and copy all forks of the relation, and schedule unlinking of
	 * old physical files.
	 *
	 * NOTE: any conflict in relfilenode value will be caught in
	 * RelationCreateStorage().
	 */
	RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence, false);

	/* copy main fork */
	RelationCopyStorage(rel->rd_smgr, dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* copy those extra forks that exist */
	for (fork_num = MAIN_FORKNUM + 1; fork_num <= MAX_FORKNUM; fork_num++)
	{
		if (smgrexists(rel->rd_smgr, fork_num))
		{
			smgrcreate(dstrel, fork_num, false);

			/*
			 * WAL log creation if the relation is persistent, or this is the
			 * init fork of an unlogged relation.
			 */
			if (rel->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 fork_num == INIT_FORKNUM))
				log_smgrcreate(newrlocator, fork_num);
			RelationCopyStorage(rel->rd_smgr, dstrel, fork_num,
								rel->rd_rel->relpersistence);
		}
	}

	/* drop old relation, and close new one */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

static void
xheapam_relation_nontransactional_truncate(Relation rel)
{
	// use heap method
	RelationTruncate(rel, 0);
}

static void
xheapam_relation_copy_for_cluster(Relation old_heap, Relation new_heap,
								 Relation old_index, bool use_sort,
								 TransactionId oldest_xmin,
								 TransactionId *xid_cutoff,
								 MultiXactId *multi_cutoff,
								 double *num_tuples,
								 double *tups_vacuumed,
								 double *tups_recently_dead)
{
	RewriteState rwstate;
	IndexScanDesc index_scan;
	TableScanDesc table_scan;
	XHeapScanDesc heap_scan;
	Tuplesortstate *tuplesort;
	TupleDesc	old_tup_desc = RelationGetDescr(old_heap);
	TupleDesc	new_tup_desc = RelationGetDescr(new_heap);
	TupleTableSlot *slot;
	int			natts;
	Datum	   *values;
	bool	   *isnull;
	XHeapTupleTableSlot *xslot;
	BlockNumber prev_cblock = InvalidBlockNumber;
	FullTransactionId global_frozen_xid = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));

	/*
	 * Valid smgr_targblock implies something already wrote to the relation.
	 * This may be harmless, but this function hasn't planned for it.
	 */
	Assert(RelationGetTargetBlock(new_heap) == InvalidBlockNumber);

	oldest_xmin = XidFromFullTransactionId(global_frozen_xid);
	/* Preallocate values/isnull arrays */
	natts = new_tup_desc->natts;
	values = (Datum *) palloc(natts * sizeof(Datum));
	isnull = (bool *) palloc(natts * sizeof(bool));

	/* Initialize the rewrite operation */
	rwstate = begin_heap_rewrite(old_heap, new_heap, oldest_xmin, *xid_cutoff,
								 *multi_cutoff);


	/* Set up sorting if wanted */
	if (use_sort)
	{
		tuplesort = tuplesort_begin_cluster(old_tup_desc, old_index,
											maintenance_work_mem,
											NULL, TUPLESORT_NONE);
	}
	else
		tuplesort = NULL;

	/*
	 * Prepare to scan the OldHeap.  To ensure we see recently-dead tuples
	 * that still need to be copied, we scan with SnapshotAny and use
	 * xheap_tuple_satisfies_oldest_xmin for the visibility test.
	 */
	if (old_index != NULL && !use_sort)
	{
		const int   ci_index[] = {
			PROGRESS_CLUSTER_PHASE,
			PROGRESS_CLUSTER_INDEX_RELID
		};
		int64       ci_val[2];

		/* Set phase and OIDOldIndex to columns */
		ci_val[0] = PROGRESS_CLUSTER_PHASE_INDEX_SCAN_HEAP;
		ci_val[1] = RelationGetRelid(old_index);
		pgstat_progress_update_multi_param(2, ci_index, ci_val);

		table_scan = NULL;
		heap_scan = NULL;
		index_scan = index_beginscan(old_heap, old_index, SnapshotAny, 0, 0);
		index_rescan(index_scan, NULL, 0, NULL, 0);
	}
	else
	{
		/* In scan-and-sort mode and also VACUUM FULL, set phase */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_SEQ_SCAN_HEAP);

		table_scan = table_beginscan(old_heap, SnapshotAny, 0, (ScanKey) NULL);
		heap_scan = (XHeapScanDesc) table_scan;
		index_scan = NULL;

		/* Set total heap blocks */
		pgstat_progress_update_param(PROGRESS_CLUSTER_TOTAL_HEAP_BLKS,
									 heap_scan->rs_nblocks);
	}

	slot = table_slot_create(old_heap, NULL);
	xslot = (XHeapTupleTableSlot *) slot;

	/*
	 * Scan through the OldHeap, either in OldIndex order or sequentially;
	 * copy each tuple into the NewHeap, or transiently to the tuplesort
	 * module.  Note that we don't bother sorting dead tuples (they won't get
	 * to the new table anyway).
	 */
	for (;;)
	{
		XHeapTuple   		tuple;
		Buffer      		buf;
		bool        		isdead;
		FullTransactionId 	xwait;

		CHECK_FOR_INTERRUPTS();

		if (index_scan != NULL)
		{
			IndexFetchHeapData *hscan = NULL;
			if (!index_getnext_slot(index_scan, ForwardScanDirection, slot))
				break;

			/* Since we used no scan keys, should never need to recheck */
			if (index_scan->xs_recheck)
				elog(ERROR, "CLUSTER does not support lossy index conditions");

			hscan = (IndexFetchHeapData *)(index_scan->xs_heapfetch);
			buf = hscan->xs_cbuf;
		}
		else
		{
			if (!table_scan_getnextslot(table_scan, ForwardScanDirection, slot))
			{
				/*
				 * If the last pages of the scan were empty, we would go to
				 * the next phase while heap_blks_scanned != heap_blks_total.
				 * Instead, to ensure that heap_blks_scanned is equivalent to
				 * total_heap_blocks after the table scan phase, this parameter
				 * is manually updated to the correct value when the table
				 * scan finishes.
				 */
				pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_BLKS_SCANNED,
											 heap_scan->rs_nblocks);
				break;
			}

			/*
			 * In scan-and-sort mode and also VACUUM FULL, set heap blocks
			 * scanned
			 *
			 * Note that heapScan may start at an offset and wrap around, i.e.
			 * rs_startblock may be >0, and rs_cblock may end with a number
			 * below rs_startblock. To prevent showing this wraparound to the
			 * user, we offset rs_cblock by rs_startblock (modulo rs_nblocks).
			 */
			if (prev_cblock != heap_scan->rs_cblock)
			{
				pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_BLKS_SCANNED,
											 (heap_scan->rs_cblock +
											  heap_scan->rs_nblocks -
											  heap_scan->rs_startblock
											  ) % heap_scan->rs_nblocks + 1);
				prev_cblock = heap_scan->rs_cblock;
			}

			buf = heap_scan->rs_cbuf;
		}

		Assert(BufferIsValid(buf));
		Assert(xslot->tuple);
		tuple = xslot->tuple;

		LockBuffer(buf, BUFFER_LOCK_SHARE);

		switch (xheap_tuple_satisfies_oldest_xmin(tuple, global_frozen_xid, buf, true,
        &tuple, &xwait, old_heap, NULL, NULL))
		{
			case XHEAPTUPLE_DEAD:
				/* Definitely dead */
				isdead = true;
				break;
			case XHEAPTUPLE_RECENTLY_DEAD:
				*tups_recently_dead += 1;
				isdead = true;
				break;
			case XHEAPTUPLE_LIVE:
				/* Live or recently dead, must copy it */
				isdead = false;
				break;
			case XHEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * Since we hold exclusive lock on the relation, normally the
				 * only way to see this is if it was inserted earlier in our
				 * own transaction.  However, it can happen in system
				 * catalogs, since we tend to release write lock before commit
				 * there.  Give a warning if neither case applies; but in any
				 * case we had better copy it.
				 */
				if (!TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xwait)))
					elog(WARNING, "concurrent insert in progress within table \"%s\"",
						 RelationGetRelationName(old_heap));
				/* treat as live */
				isdead = false;
				break;
			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * Similar situation to INSERT_IN_PROGRESS case.
				 */
				if (!TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xwait)))
					elog(WARNING, "concurrent delete in progress within table \"%s\"",
						 RelationGetRelationName(old_heap));
				/* treat as recently dead */
				*tups_recently_dead += 1;
				isdead = true;
				break;
			default:
				elog(ERROR, "unexpected xheap_tuple_satisfies_oldest_xmin result");
				isdead = false; /* keep compiler quiet */
				break;
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		if (isdead)
		{
			*tups_vacuumed += 1;
			continue;
		}

		*num_tuples += 1;
		if (tuplesort != NULL)
		{
			HeapTuple		htuple = xheap_to_heap(new_heap, tuple);
			tuplesort_putheaptuple(tuplesort, htuple);
			pfree(htuple);

			/*
			 * In scan-and-sort mode, report increase in number of tuples
			 * scanned
			 */
			pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_TUPLES_SCANNED,
										 *num_tuples);
		}
		else
		{
			const int   ct_index[] = {
				PROGRESS_CLUSTER_HEAP_TUPLES_SCANNED,
				PROGRESS_CLUSTER_HEAP_TUPLES_WRITTEN
			};
			int64       ct_val[2];

			reform_and_rewrite_xtuple(tuple, RelationGetDescr(old_heap), RelationGetDescr(new_heap),
									 values, isnull, rwstate);

			/*
			 * In indexscan mode and also VACUUM FULL, report increase in
			 * number of tuples scanned and written
			 */
			ct_val[0] = *num_tuples;
			ct_val[1] = *num_tuples;
			pgstat_progress_update_multi_param(2, ct_index, ct_val);
		}
	}

	if (index_scan != NULL)
		index_endscan(index_scan);
	if (table_scan != NULL)
		table_endscan(table_scan);
	if (slot)
		ExecDropSingleTupleTableSlot(slot);

	/*
	 * In scan-and-sort mode, complete the sort, then read out all live tuples
	 * from the tuplestore and write them to the new relation.
	 */
	if (tuplesort != NULL)
	{
		double      n_tuples = 0;

		/* Report that we are now sorting tuples */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_SORT_TUPLES);

		tuplesort_performsort(tuplesort);

		/* Report that we are now writing new heap */
		pgstat_progress_update_param(PROGRESS_CLUSTER_PHASE,
									 PROGRESS_CLUSTER_PHASE_WRITE_NEW_HEAP);

		for (;;)
		{
			HeapTuple   htuple;
			XHeapTuple  xtuple;

			CHECK_FOR_INTERRUPTS();

			htuple = tuplesort_getheaptuple(tuplesort, true);
			if (htuple == NULL)
				break;

			n_tuples += 1;
			xtuple = heap_to_xheap(new_heap, htuple);
			reform_and_rewrite_xtuple(xtuple,
									 RelationGetDescr(old_heap), RelationGetDescr(new_heap),
									 values, isnull, rwstate);
			pfree(xtuple);
			/* Report n_tuples */
			pgstat_progress_update_param(PROGRESS_CLUSTER_HEAP_TUPLES_WRITTEN,
										 n_tuples);
		}

		tuplesort_end(tuplesort);
	}

	/* Write out any remaining tuples, and fsync if needed */
	end_heap_rewrite(rwstate);

	/* Log what we did*/
	ereport(LOG, (errmsg("found %.0f tuples vacuumed, %.0f live tuples copied",
						*tups_vacuumed, *num_tuples)));

	/* Clean up */
	pfree(values);
	pfree(isnull);
}

/*
 * Reconstruct and rewrite the given xtuple
 *
 * We cannot simply copy the tuple as-is, for several reasons:
 *
 * 1. We'd like to squeeze out the values of any dropped columns, both
 * to save space and to ensure we have no corner-case failures. (It's
 * possible for example that the new table hasn't got a TOAST table
 * and so is unable to store any large values of dropped cols.)
 *
 * 2. The tuple might not even be legal for the new table; this is
 * currently only known to happen as an after-effect of ALTER TABLE
 * SET WITHOUT OIDS.
 *
 * So, we must reconstruct the tuple from component Datums.
 */
static void
reform_and_rewrite_xtuple(XHeapTuple tuple,
						  TupleDesc old_tup_desc,
						  TupleDesc new_tup_desc,
						  Datum *values, bool *isnull,
						  RewriteState rwstate)
{
	XHeapTuple		copiedTuple;
	int				i;

	xheap_deform_tuple(tuple, old_tup_desc, values, isnull);

	/* Be sure to null out any dropped columns */
	for (i = 0; i < new_tup_desc->natts; i++)
	{
		if (new_tup_desc->attrs[i].attisdropped)
			isnull[i] = true;
	}

	copiedTuple = xheap_form_tuple(new_tup_desc, values, isnull);

	rewrite_xheap_tuple(rwstate, tuple, copiedTuple);
	XHeapFreeTuple(copiedTuple);
}

static void xheap_vacuum_rel(Relation onerel, VacuumParams *params,
				BufferAccessStrategy bstrategy)
{
	xheap_lazy_vacuum(onerel, params, bstrategy);
}

static bool
xheapam_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	XHeapScanDesc xscan = (XHeapScanDesc) scan;

	/*
	 * We must maintain a pin on the target page's buffer to ensure that
	 * concurrent activity - e.g. HOT pruning - doesn't delete tuples out from
	 * under us.  It comes from the stream already pinned.   We also choose to
	 * hold sharelock on the buffer throughout --- we could release and
	 * re-acquire sharelock for each tuple, but since we aren't doing much
	 * work per tuple, the extra lock traffic is probably better avoided.
	 */
	xscan->rs_cbuf = read_stream_next_buffer(stream, NULL);
	if (!BufferIsValid(xscan->rs_cbuf))
		return false;

	LockBuffer(xscan->rs_cbuf, BUFFER_LOCK_SHARE);

	xscan->rs_cblock = BufferGetBlockNumber(xscan->rs_cbuf);
	xscan->rs_cindex = FirstOffsetNumber;
	return true;
}

static bool
xheapam_scan_analyze_next_tuple(TableScanDesc scan, TransactionId oldest_xmin,
							   double *liverows, double *deadrows,
							   TupleTableSlot *slot)
{
	XHeapScanDesc hscan = (XHeapScanDesc) scan;
	Page        targpage;
	OffsetNumber maxoffset;
	XHeapTupleTableSlot *hslot;

	FullTransactionId globalFrozenXid = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
	oldest_xmin = XidFromFullTransactionId(globalFrozenXid);

	Assert(TTS_TABLEAM_IS_XSTORE(slot));

	hslot = (XHeapTupleTableSlot *) slot;
	targpage = BufferGetPage(hscan->rs_cbuf);
	maxoffset = xheap_page_get_max_offset_number(targpage);

	/* Inner loop over all tuples on the selected page */
	for (; hscan->rs_cindex <= maxoffset; hscan->rs_cindex++)
	{
		XHeapTuple  targtuple = &hslot->tupdata;
		RowPtr *rp = NULL;
		bool sample_it = false;
		FullTransactionId xid;

		ItemPointerSet(&targtuple->ctid, hscan->rs_cblock, hscan->rs_cindex);

		targtuple->table_oid = RelationGetRelid(scan->rs_rd);

		rp = XPageGetRowPtr(targpage, hscan->rs_cindex);
		if (!RowPtrIsNormal(rp))
			continue;

		targtuple->disk_tuple = (XHeapDiskTuple)XPageGetRowData(targpage, rp);
		targtuple->disk_tuple_size = RowPtrGetLen(rp);

		xid = XHeapTupleGetModifiedXid(targtuple);

		switch (xheap_tuple_satisfies_oldest_xmin(targtuple, globalFrozenXid, hscan->rs_cbuf, true,
        NULL, &xid, hscan->rs_base.rs_rd, NULL, NULL))
		{
			case XHEAPTUPLE_LIVE:
				sample_it = true;
				*liverows += 1;
				break;
			case XHEAPTUPLE_DEAD:
			case XHEAPTUPLE_RECENTLY_DEAD:
				*deadrows += 1;
				break;
			case XHEAPTUPLE_INSERT_IN_PROGRESS:
				if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
				{
					sample_it = true;
					*liverows += 1;
				}
				break;
			case XHEAPTUPLE_DELETE_IN_PROGRESS:
				if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)))
					*deadrows += 1;
				else
				{
					sample_it = true;
					*liverows += 1;
				}
				break;

			default:
				elog(ERROR, "unexpected xheap_tuple_satisfies_oldest_xmin result");
				break;
		}

		if (sample_it)
		{
			// Do not set shouldFree, because the buffer is locked for share,
			// and the targtuple did not copy anything
			xheap_slot_store_xheap_tuple(targtuple, slot, false, false);
			hscan->rs_cindex++;

			/* note that we leave the buffer locked here! */
			return true;
		}
	}

	/* Now release the lock and pin on the page */
	UnlockReleaseBuffer(hscan->rs_cbuf);
	hscan->rs_cbuf = InvalidBuffer;

	/* also prevent old slot contents from having pin on page */
	ExecClearTuple(slot);

	return false;
}

static double
xheapam_index_build_range_scan(Relation heap_relation,
							  Relation index_relation,
							  IndexInfo *index_info,
							  bool allow_sync,
							  bool anyvisible,
							  bool progress,
							  BlockNumber start_blockno,
							  BlockNumber numblocks,
							  IndexBuildCallback callback,
							  void *callback_state,
							  TableScanDesc scan)
{
	XHeapScanDesc xscan;
#ifdef USE_ASSERT_CHECKING
	bool        checking_uniqueness;
#endif
	HeapTupleData heap_tuple;
	XHeapTuple  xheap_tuple;
	Datum       values[INDEX_MAX_KEYS];
	bool        isnull[INDEX_MAX_KEYS];
	double      reltuples;
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState     *estate;
	ExprContext *econtext;
	Snapshot    snapshot;

	/*
	 * sanity checks
	 */
	Assert(OidIsValid(index_relation->rd_rel->relam));
	/* anyvisible is only used for brin index, which is not supported in xstore */
	Assert(!anyvisible);
	/* start_blockno and numblocks are only used for brin index, which is not supported in xstore */
	Assert(start_blockno == 0 && numblocks == InvalidBlockNumber);

#ifdef USE_ASSERT_CHECKING
	/* See whether we're verifying uniqueness/exclusion properties */
	checking_uniqueness = (index_info->ii_Unique ||
						   index_info->ii_ExclusionOps != NULL);

	/*
	 * "Any visible" mode is not compatible with uniqueness checks; make sure
	 * only one of those is requested.
	 */
	Assert(!(anyvisible && checking_uniqueness));
#endif

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heap_relation, NULL);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any. */
	predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

	if (index_info->ii_Concurrent)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("concurrent index create is not supported in xstore")));
	/*
	 * Prepare for scan of the base relation.  In a normal index build, we use
	 * SnapshotAny because we must retrieve all tuples and do our own time
	 * qual checks (because we have to index RECENTLY_DEAD tuples). In a
	 * concurrent build, or during bootstrap, we take a regular MVCC snapshot
	 * and index whatever's live according to that.
	 */
	snapshot = SnapshotSelfTransaction;
	index_info->ii_BrokenHotChain = true;
	if (scan == NULL)
		scan = xheap_beginscan(heap_relation, snapshot, 0, allow_sync ? SO_ALLOW_SYNC : 0); /* number of keys is 0 */

	xscan = (XHeapScanDesc)scan;
	reltuples = 0;

	/*
	 * Scan all tuples in the base relation.
	 */
	while ((xheap_index_build_get_next_tuple(xscan, slot)) != NULL)
	{
		xheap_tuple = ((XHeapTupleTableSlot *)slot)->tuple;

		CHECK_FOR_INTERRUPTS();

		reltuples += 1;

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		/*
		 * In a partial index, discard tuples that don't satisfy the
		 * predicate.
		 */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * For the current heap tuple, extract all the attributes we use in
		 * this index, and note which are null.  This also performs evaluation
		 * of any expressions needed.
		 *
		 * NOTE: We can't free the xheap tuple fetched by the scan method
		 * before next iteration since this tuple is also referenced by
		 * scan->rs_cutup. which is used by xheap scan API's to fetch the next
		 * tuple. But, for forming and creating the index, we've to store the
		 * correct version of the tuple in the slot. Hence, after forming the
		 * index and calling the callback function, we restore the xheap tuple
		 * fetched by the scan method in the slot.
		 */
		FormIndexDatum(index_info, slot, estate, values, isnull);

		/*
		 * But, it needs only the tid. So, we set t_self for the xheap tuple
		 * and call the AM's callback.
		 */
		heap_tuple.t_self = xheap_tuple->ctid;

		/* Call the AM's callback routine to process the tuple */
		callback(index_relation, &heap_tuple.t_self, values, isnull, true, callback_state);
	}

	xheap_end_scan(scan);

	/* we can now forget our snapshot, if set */
	if (index_info->ii_Concurrent)
		UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);

	FreeExecutorState(estate);


	/* These may have been pointing to the now-gone estate */
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;

	return reltuples;
}

static void
xheapam_index_validate_scan(Relation heap_relation,
						   Relation index_relation,
						   IndexInfo *index_info,
						   Snapshot snapshot,
						   ValidateIndexState *state)
{
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("xheapam_index_validate_scan not supported in xstore yet")));
}

/* ------------------------------------------------------------------------
* Miscellaneous helper functions for xheap AM.
* ------------------------------------------------------------------------
*/

static uint64
xheapam_relation_size(Relation rel, ForkNumber fork_number)
{
	return table_block_relation_size(rel, fork_number);
}

static bool
xheapam_relation_needs_toast_table(Relation rel)
{
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc = rel->rd_att;
	int32		tuple_length;
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		data_length = att_align_nominal(data_length, att->attalign);
		if (att->attlen > 0)
		{
			/* Fixed-length types are never toastable */
			data_length += att->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att->atttypid,
												   att->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att->attstorage != TYPSTORAGE_PLAIN)
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;			/* nothing to toast? */
	if (maxlength_unknown)
		return true;			/* any unlimited-length attrs? */
	tuple_length = MAXALIGN(SizeofHeapTupleHeader +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}

static Oid
xheapam_relation_toast_am(Relation rel)
{
	return rel->rd_rel->relam;
}

/* ------------------------------------------------------------------------
* Planner related helper functions for xheap AM.
* ------------------------------------------------------------------------
*/
#define XHEAP_OVERHEAD_BYTES_PER_TUPLE \
	(MAXALIGN(XHeapTupleDataSize) + sizeof(ItemIdData))
#define XHEAP_USABLE_BYTES_PER_PAGE \
	(BLCKSZ - SizeOfXHeapPageHeaderData)

static void
xheapam_estimate_rel_size(Relation rel, int32 *attr_widths,
						 BlockNumber *pages, double *tuples,
						 double *allvisfrac)
{
	table_block_relation_estimate_size(rel, attr_widths, pages, tuples, allvisfrac,
								   XHEAP_OVERHEAD_BYTES_PER_TUPLE,
								   XHEAP_USABLE_BYTES_PER_PAGE);
	*allvisfrac = 1.0;
}

/* ------------------------------------------------------------------------
* Executor related helper functions for xheap AM.
* ------------------------------------------------------------------------
*/
static bool
xheapam_scan_bitmap_next_block(TableScanDesc scan,
							  TBMIterateResult *tbmres)
{
	XHeapScanDesc hscan = (XHeapScanDesc) scan;
	BlockNumber page = tbmres->blockno;
	Buffer      buffer;
	bool		old_page_at_a_time;

	hscan->rs_cindex = 0;
	hscan->rs_ntuples = 0;

	/*
	 * Ignore any claimed entries past what we think is the end of the
	 * relation. It may have been extended after the start of our scan (we
	 * only hold an AccessShareLock, and it could be inserts from this
	 * backend).
	 */
	if (page >= hscan->rs_nblocks)
		return false;

	/*
	 * Acquire pin on the target heap page, trading in any pin we held before.
	 */
	hscan->rs_cbuf = ReleaseAndReadBuffer(hscan->rs_cbuf,
										  scan->rs_rd,
										  page);
	hscan->rs_cblock = page;
	buffer = hscan->rs_cbuf;

	/*
	 * We need two separate strategies for lossy and non-lossy cases.
	 */
	if (tbmres->ntuples >= 0)
	{
		/*
		 * Bitmap is non-lossy, so we just look through the offsets listed in
		 * tbmres; but we have to follow any HOT chain starting at each such
		 * offset.
		 */
		int         curslot;

		/*
		* We must hold share lock on the buffer content while examining tuple
		* visibility.  Afterwards, however, the tuples we have found to be
		* visible are guaranteed good as long as we hold the buffer pin.
		*/
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		for (curslot = 0; curslot < tbmres->ntuples; curslot++)
		{
			OffsetNumber 	offnum = tbmres->offsets[curslot];
			ItemPointerData tid;
			XHeapTuple		page_tup;
			Page  			dp = BufferGetPage(buffer);
			RowPtr		   *rp = XPageGetRowPtr(dp, offnum);
			Size 			tuple_len = RowPtrGetLen(rp);
			XHeapTuple 		xtuple = NULL;
			XHeapTuple		resulttup = NULL;

			if (!RowPtrIsNormal(rp))
				continue;

			xtuple = (XHeapTuple) xheaptup_alloc(XHeapTupleDataSize + tuple_len);
			xtuple->disk_tuple = (XHeapDiskTuple) ((char *) xtuple + XHeapTupleDataSize);

			ItemPointerSet(&tid, page, offnum);
			page_tup = xheap_get_tuple(scan->rs_rd, buffer, offnum, xtuple);
			if (xheap_tuple_fetch(scan->rs_rd, buffer, offnum, scan->rs_snapshot, &resulttup, &tid, false,
						  NULL, NULL, &page_tup, -1, NULL, NULL))
			{
				if (xtuple && resulttup != xtuple)
					pfree(xtuple);
				hscan->rs_visxtuples[(hscan->rs_ntuples)++] = resulttup;
			}
			else
			{
				pfree(xtuple);
			}
		}

		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	}
	else
	{
		/*
		 * Bitmap is lossy, so we must examine each line pointer on the page.
		 */
		old_page_at_a_time = hscan->rs_pageatatime;
		hscan->rs_pageatatime = true;
		xheapgetpage(hscan, page, NULL);
		hscan->rs_pageatatime = old_page_at_a_time;
	}

	Assert(hscan->rs_ntuples <= MaxXHeapTuplesPerPage(scan->rs_rd));

	return hscan->rs_ntuples > 0;
}

static bool
xheapam_scan_bitmap_next_tuple(TableScanDesc scan,
							  TBMIterateResult *tbmres,
							  TupleTableSlot *slot)
{
	XHeapScanDesc hscan = (XHeapScanDesc) scan;

	/* Clear the slot */
	ExecClearTuple(slot);

	/*
	 * Out of range?  If so, nothing more to look at on this page
	 */
	if (hscan->rs_cindex < 0 || hscan->rs_cindex >= hscan->rs_ntuples)
		return false;

	hscan->rs_cutup = hscan->rs_visxtuples[hscan->rs_cindex];

	pgstat_count_heap_fetch(scan->rs_rd);

	/*
	 * Set up the result slot to point to this tuple.  Note that the slot
	 * acquires a pin on the buffer.
	 */
	xheap_slot_store_xheap_tuple(hscan->rs_cutup, slot, true, false);

	hscan->rs_cindex++;

	return true;
}

static bool
xheapam_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	XHeapScanDesc hscan = (XHeapScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno;

	/* return false immediately if relation is empty */
	if (hscan->rs_nblocks == 0)
		return false;

	if (tsm->NextSampleBlock)
	{
		blockno = tsm->NextSampleBlock(scanstate, hscan->rs_nblocks);
		hscan->rs_cblock = blockno;
	}
	else
	{
		/* scanning table sequentially */

		if (hscan->rs_cblock == InvalidBlockNumber)
		{
			Assert(!hscan->rs_inited);
			blockno = hscan->rs_startblock;
		}
		else
		{
			Assert(hscan->rs_inited);

			blockno = hscan->rs_cblock + 1;

			if (blockno >= hscan->rs_nblocks)
			{
				/* wrap to beginning of rel, might not have started at 0 */
				blockno = 0;
			}

			/*
			 * Report our new scan position for synchronization purposes.
			 *
			 * Note: we do this before checking for end of scan so that the
			 * final state of the position hint is back at the start of the
			 * rel.  That's not strictly necessary, but otherwise when you run
			 * the same query multiple times the starting position would shift
			 * a little bit backwards on every invocation, which is confusing.
			 * We don't guarantee any specific ordering in general, though.
			 */
			if (scan->rs_flags & SO_ALLOW_SYNC)
				ss_report_location(scan->rs_rd, blockno);

			if (blockno == hscan->rs_startblock)
				blockno = InvalidBlockNumber;
		}
	}

	if (!BlockNumberIsValid(blockno))
	{
		if (BufferIsValid(hscan->rs_cbuf))
			ReleaseBuffer(hscan->rs_cbuf);
		hscan->rs_cbuf = InvalidBuffer;
		hscan->rs_cblock = InvalidBlockNumber;
		hscan->rs_inited = false;

		return false;
	}

	hscan->rs_cbuf = ReadBufferExtended(scan->rs_rd, MAIN_FORKNUM,
										blockno, RBM_NORMAL, GetAccessStrategy(BAS_NORMAL));
	hscan->rs_inited = true;
	hscan->rs_cblock = blockno;

	return true;
}

static bool
xheapam_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
							  TupleTableSlot *slot)
{
	XHeapScanDesc hscan = (XHeapScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno = hscan->rs_cblock;
	XHeapTupleTableSlot *hslot = (XHeapTupleTableSlot *)slot;
	Page        page;
	bool        all_visible;
	OffsetNumber maxoffset;

	LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

	page = (Page) BufferGetPage(hscan->rs_cbuf);
	all_visible = PageIsAllVisible(page) &&
		!scan->rs_snapshot->takenDuringRecovery;
	maxoffset = xheap_page_get_max_offset_number(page);

	for (;;)
	{
		OffsetNumber tupoffset;

		CHECK_FOR_INTERRUPTS();

		/* Ask the tablesample method which tuples to check on this page. */
		tupoffset = tsm->NextSampleTuple(scanstate,
										 blockno,
										 maxoffset);

		if (OffsetNumberIsValid(tupoffset))
		{
			bool        visible;
			RowPtr 		*rp = NULL;
			XHeapTuple   tuple = &hslot->tupdata;

			ItemPointerSet(&tuple->ctid, blockno, tupoffset);

			tuple->table_oid = RelationGetRelid(scan->rs_rd);

			rp = XPageGetRowPtr(page, tupoffset);
			if (!RowPtrIsNormal(rp))
				continue;

			tuple->disk_tuple = (XHeapDiskTuple)XPageGetRowData(page, rp);
			tuple->disk_tuple_size = RowPtrGetLen(rp);

			if (all_visible)
				visible = true;
			else
				visible = xheap_tuple_satisfies_visibility(tuple, scan->rs_snapshot, hscan->rs_cbuf);

			/* Try next tuple from same page. */
			if (!visible)
				continue;

			/* Found visible tuple, return it. */
			xheap_slot_store_xheap_tuple(xheap_copy_tuple(tuple), slot, true, false);

			/* Count successfully-fetched tuples as heap fetches */
			pgstat_count_heap_getnext(scan->rs_rd);
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			return true;
		}
		else
		{
			/* Now release the lock and pin on the page */
			UnlockReleaseBuffer(hscan->rs_cbuf);
			hscan->rs_cbuf = InvalidBuffer;

			ExecClearTuple(slot);
			return false;
		}
	}

	Assert(0);
}

/* ------------------------------------------------------------------------
 * Definition of the heap table access method.
 * ------------------------------------------------------------------------
 */

static const TableAmRoutine xheapam_methods = {
	.type = T_TableAmRoutine,

	.slot_callbacks = xheapam_slot_callbacks,

	.scan_begin = xheapam_scan_begin,
	.scan_end = xheapam_scan_end,
	.scan_rescan = xheapam_scan_rescan,
	.scan_getnextslot = xheapam_scan_getnextslot,

	.scan_set_tidrange = xheapam_scan_set_tidrange,
	.scan_getnextslot_tidrange = xheapam_scan_getnextslot_tidrange,

	.parallelscan_estimate = xheapam_parallelscan_estimate,
	.parallelscan_initialize = xheapam_parallelscan_initialize,
	.parallelscan_reinitialize = xheapam_parallelscan_reinitialize,

	.index_fetch_begin = xheapam_index_fetch_begin,
	.index_fetch_reset = xheapam_index_fetch_reset,
	.index_fetch_end = xheapam_index_fetch_end,
	.index_fetch_tuple = xheapam_index_fetch_tuple,

	.tuple_insert = xheapam_tuple_insert,
	.tuple_insert_speculative = xheapam_tuple_insert_speculative,
	.tuple_complete_speculative = xheapam_tuple_complete_speculative,
	.multi_insert = xheapam_multi_insert,
	.tuple_delete = xheapam_tuple_delete,
	.tuple_update = xheapam_tuple_update,
	.tuple_lock = xheapam_tuple_lock,

	.tuple_fetch_row_version = xheapam_fetch_row_version,
	.tuple_get_latest_tid = xheapam_get_latest_tid,
	.tuple_tid_valid = xheapam_tuple_tid_valid,
	.tuple_satisfies_snapshot = xheapam_tuple_satisfies_snapshot,
	.index_delete_tuples = xheap_index_delete_tuples,

	.relation_set_new_filelocator = xheapam_relation_set_new_filelocator,
	.relation_nontransactional_truncate = xheapam_relation_nontransactional_truncate,
	.relation_copy_data = xheapam_relation_copy_data,
	.relation_copy_for_cluster = xheapam_relation_copy_for_cluster,
	.relation_vacuum = xheap_vacuum_rel,
	.scan_analyze_next_block = xheapam_scan_analyze_next_block,
	.scan_analyze_next_tuple = xheapam_scan_analyze_next_tuple,
	.index_build_range_scan = xheapam_index_build_range_scan,
	.index_validate_scan = xheapam_index_validate_scan,

	.relation_size = xheapam_relation_size,
	.relation_needs_toast_table = xheapam_relation_needs_toast_table,
	.relation_toast_am = xheapam_relation_toast_am,
	.relation_fetch_toast_slice = xheap_fetch_toast_slice,

	.relation_estimate_size = xheapam_estimate_rel_size,

	.scan_bitmap_next_block = xheapam_scan_bitmap_next_block,
	.scan_bitmap_next_tuple = xheapam_scan_bitmap_next_tuple,

	.scan_sample_next_block = xheapam_scan_sample_next_block,
	.scan_sample_next_tuple = xheapam_scan_sample_next_tuple
};

const TableAmRoutine *
get_xheapam_table_am_routine(void)
{
	return &xheapam_methods;
}

PG_FUNCTION_INFO_V1(xheap_tableam_handler);
Datum
xheap_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&xheapam_methods);
}

bool 
relation_is_xstore_format(void *relation)
{
	Relation rel = (Relation)relation;
	return ((RELKIND_RELATION == rel->rd_rel->relkind ||
	  		RELKIND_TOASTVALUE == rel->rd_rel->relkind) &&
	 		rel->rd_tableam == get_xheapam_table_am_routine());
}

Oid 
get_xstore_oid(void)
{
	Relation pg_am_rel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple tuple;
	Oid		xstoreOid = InvalidOid;

	ScanKeyInit(key,
				Anum_pg_am_amname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum("xstore"));

	pg_am_rel = table_open(AccessMethodRelationId, AccessShareLock);

	scan = systable_beginscan(pg_am_rel, AmNameIndexId, true, NULL, 1, key);

	tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_am form = (Form_pg_am) GETSTRUCT(tuple);
		xstoreOid = form->oid;
	}

	systable_endscan(scan);

	table_close(pg_am_rel, AccessShareLock);

	return xstoreOid;
}

