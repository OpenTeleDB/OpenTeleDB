/* -------------------------------------------------------------------------
 *
 * prunexheap.c
 *	  xheap page pruning
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/xheap/prunexheap.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xloginsert.h"
#include "xheap/xtuple.h"
#include "xstore.h"
#include "c.h"
#include "pgstat.h"
#include "xheap/xheap.h"
#include "xheap/xheapam_visibility.h"
#include "xheap/xpage.h"
#include "xheap/xtup_details.h"
#include "xheap/xrel.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "storage/bufpage.h"
#include "util/xxact.h"
#include "util/xrmgr.h"

static int	xheap_prune_item(const RelationBuffer *relbuf, OffsetNumber rootoffnum,
						   FullTransactionId oldest_xmin, XPruneState *prstate,
						   Size *space_freed, bool is_target);
static void xheap_prune_record_prunable(XPruneState *prstate, FullTransactionId xid);
static void xheap_prune_record_unused(XPruneState *prstate, OffsetNumber offnum, Relation relation);
static void xheap_prune_record_fixed(XPruneState *prstate, OffsetNumber offnum,
								  Relation relation, uint16 tup_size);

 /*
 * Optionally prune and repair fragmentation in the specified page.
 *
 * Caller must have exclusive lock on the page.
 *
 * This is an opportunistic function.  It will perform housekeeping only if
 * the page has effect of transaction that has modified data which can be
 * pruned.
 *
 * Note: This is called only when we need some space in page to perform the
 * action which otherwise would need a different page.  It is called when an
 * update statement has to update the existing tuple such that new tuple is
 * bigger than old tuple and the same can't fit on page.
 *
 * Returns true, if we are able to free up the space such that the new tuple
 * can fit into same page, otherwise, false.
*/
bool
xheap_page_prune_opt(Relation relation, Buffer buffer, OffsetNumber offnum,
				  Size space_required)
{
	Page		page;
	FullTransactionId oldest_xmin;
	FullTransactionId ignore = InvalidFullTransactionId;
	Size		pagefree;
	bool		force_prune = false;
	bool		pruned = false;
	RelationBuffer relbuf = {relation, buffer};

	page = BufferGetPage(buffer);

	/*
	 * We can't write WAL in recovery mode, so there's no point trying to
	 * clean the page. The master will likely issue a cleaning WAL record soon
	 * anyway, so this is no particular loss.
	 */
	if (RecoveryInProgress())
		return false;

	oldest_xmin = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
	Assert(TransactionIdIsValid(oldest_xmin.value));

	if (OffsetNumberIsValid(offnum))
	{
		pagefree = page_get_exact_xheap_free_space(page);
		/*
		 * We want to forcefully prune the page if we are sure that the
		 * required space is available.  This will help in rearranging the
		 * page such that we will be able to make space adjacent to required
		 * offset number.
		 */
		if (space_required < pagefree)
			force_prune = true;
	}

	/*
	 * Let's see if we really need pruning.
	 *
	 * Forget it if page is not hinted to contain something prunable that's
	 * committed and we don't want to forcefully prune the page.
	 */
	if (!XPageIsPrunableWithOldestXmin(page, oldest_xmin) && !force_prune)
		return false;

	xheap_page_prune_guts(relation, &relbuf, oldest_xmin, offnum, space_required, true,
					   force_prune, &ignore, &pruned, NULL);
	if (pruned)
		return true;

	return false;
}

bool
xheap_page_prune_opt_page(Relation relation, Buffer buffer, FullTransactionId xid,
					  bool acquireContionalLock)
{
	Page		  page;
	FullTransactionId oldest_xmin = InvalidFullTransactionId;
	FullTransactionId ignore = InvalidFullTransactionId;
	Size		  minfree;
	bool		  pruned = false;

	page = BufferGetPage(buffer);

	/*
	 * We can't write WAL in recovery mode, so there's no point trying to
	 * clean the page. The master will likely issue a cleaning WAL record soon
	 * anyway, so this is no particular loss.
	 */
	if (RecoveryInProgress())
		return false;

	oldest_xmin = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
	Assert(TransactionIdIsValid(oldest_xmin.value));

	if ((IsPostmasterEnvironment))
		return false;

	/*
	 * Let's see if we really need pruning.
	 *
	 * Forget it if page is not hinted to contain something prunable that's committed
	 * and we don't want to forcefully prune the page. Furthermore, even if the page
	 * contains prunable items, we do not want to do pruning every so often and hamper
	 * server performance. The current heuristic to delay pruning is the difference
	 * between pd_prune_xid and oldestxmin must be atleast 3 times or more the difference
	 * of xid and oldestxmin.
	 */
	if (!XPageIsPrunableWithOldestXmin(page, oldest_xmin) ||
		(FullTransactionIdIsValid(xid) &&
		 (oldest_xmin.value - ((XHeapPageHeaderData *) (page))->pd_prune_xid.value) <
			 (3 * (xid.value - oldest_xmin.value))))
		return false;

	minfree = RelationGetTargetPageFreeSpacePrune(relation, HEAP_DEFAULT_FILLFACTOR);
	minfree = Max(minfree, BLCKSZ / 10);
	if (PageIsFull(page) || page_get_exact_xheap_free_space(page) < minfree)
	{
		RelationBuffer relbuf = {relation, buffer};
		if (!acquireContionalLock)
			/* Exclusive lock is acquired, OK to prune */
			(void) xheap_page_prune(relation, &relbuf, oldest_xmin, true, &ignore, &pruned);
		else
		{
			if (!ConditionalLockBuffer(buffer))
				return false;

			if (PageIsFull(page) || page_get_exact_xheap_free_space(page) < minfree)
				(void) xheap_page_prune(relation, &relbuf, oldest_xmin, true, &ignore,
									  &pruned);

			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		}
	}

	if (pruned)
		return true;

	return false;
}


/*
 * Prune and repair fragmentation in the specified page.
 *
 * Caller must have pin and buffer cleanup lock on the page.
 *
 * oldestXmin is the cutoff XID used to distinguish whether tuples are DEAD
 * or RECENTLY_DEAD (see InplaceHeapTupleSatisfiesOldestXmin).
 *
 * To perform pruning, we make the copy of the page.  We don't scribble on
 * that copy, rather it is only used during repair fragmentation to copy
 * the tuples.  So, we need to ensure that after making the copy, we operate
 * on tuples, otherwise, the temporary copy will become useless.  It is okay
 * scribble on itemid's or special space of page.
 *
 * If reportStats is true then we send the number of reclaimed tuples to
 * pgstats.  (This must be false during vacuum, since vacuum will send its own
 * own new total to pgstats, and we don't want this delta applied on top of
 * that.)
 *
 * Returns the number of tuples deleted from the page and sets
 * latestRemovedXid.  It returns 0, when removed the dead tuples can't free up
 * the space required.
 */
int
xheap_page_prune_guts(Relation relation, const RelationBuffer *relbuf,
				   FullTransactionId oldest_xmin, OffsetNumber target_off_num,
				   Size space_required, bool report_stats, bool force_prune,
				   FullTransactionId *latest_removed_xid, bool *pruned, int *remain)
{
	int			ndeleted = 0;
	Size		space_freed = 0;
	Page		page = BufferGetPage(relbuf->buffer);
	OffsetNumber offnum;
	OffsetNumber maxoff;
	XPruneState prstate;
	bool		execute_pruning = false;
	bool		has_pruned = false;


	if (pruned)
		*pruned = false;

	/* initialize the space_free with already existing free space in page */
	space_freed = page_get_exact_xheap_free_space(page);

	/*
	 * Our strategy is to scan the page and make lists of items to change,
	 * then apply the changes within a critical section.  This keeps as much
	 * logic as possible out of the critical section, and also ensures that
	 * WAL replay will work the same as the normal case.
	 *
	 * First, initialize the new pd_prune_xid value to zero (indicating no
	 * prunable tuples).  If we find any tuples which may soon become
	 * prunable, we will save the lowest relevant XID in new_prune_xid. Also
	 * initialize the rest of our working state.
	 */
	prstate.new_prune_xid = InvalidFullTransactionId;
	prstate.latest_removed_xid = *latest_removed_xid;
	prstate.nunused = 0;
	prstate.remain = 0;
	prstate.nfixed = 0;
	memset(prstate.marked, 0, sizeof(prstate.marked));

	/*
	 * If caller has asked to rearrange the page and page is not marked for
	 * pruning, then skip scanning the page.
	 *
	 * XXX We might want to remove this check once we have some optimal
	 * strategy to rearrange the page where we anyway need to traverse all
	 * rows.
	 */
	if (force_prune && !XPageIsPrunable(page))
	{
		;						/* no need to scan */
	}
	else
	{
		/* Scan the page */
		maxoff = xheap_page_get_max_offset_number(page);
		for (offnum = FirstOffsetNumber; 
			 offnum <= maxoff; 
			 offnum = OffsetNumberNext(offnum))
		{
			RowPtr	   *itemid = NULL;

			/* Ignore items already processed as part of an earlier chain */
			if (prstate.marked[offnum])
				continue;

			/*
			* Nothing to do if slot is empty, already dead or marked as
			* deleted.
			*/
			itemid = XPageGetRowPtr(page, offnum);
			if (!RowPtrIsUsed(itemid))
				continue;

			/* Process this item */
			ndeleted += xheap_prune_item(relbuf, offnum, oldest_xmin, &prstate, &space_freed,
									(offnum == target_off_num));
		}
	}
	/*
	 * There is not much advantage in continuing, if we can't free the space
	 * required by the caller or we are not asked to forcefully prune the
	 * page.
	 *
	 * XXX - In theory, we can still continue and perform pruning in the hope
	 * that some future update in this page will be able to use that space.
	 * However, it will lead to additional writes without any guaranteed
	 * benefit, so we skip the pruning for now.
	 */
	if (space_freed < space_required)
		return 0;

	/* Do we want to prune? */
	if (prstate.nunused > 0 || force_prune || prstate.nfixed > 0)
		execute_pruning = true;

	/* Any error while applying the changes is critical */
	START_CRIT_SECTION();

	if (execute_pruning)
	{
		/*
		 * Apply the planned item changes, then repair page fragmentation, and
		 * update the page's hint bit about whether it has free line pointers.
		 */
		xheap_page_prune_execute(relbuf->buffer, target_off_num, &prstate);

		/*
		 * Finally, repair any fragmentation, and update the page's hint bit
		 * whether it has free pointers.
		 */
		xheap_page_repaire_fregmentation(relation, relbuf->buffer, target_off_num, space_required, &has_pruned);

		/*
		 * Update the page's pd_prune_xid field to either zero, or the lowest
		 * XID of any soon-prunable tuple.
		 */
		((XHeapPageHeaderData *) page)->pd_prune_xid = prstate.new_prune_xid;

		/*
		 * Also clear the "page is full" flag, since there's no point in
		 * repeating the prune/defrag process until something else happens to
		 * the page.
		 */
		XPageClearFull(page);

		MarkBufferDirty(relbuf->buffer);

		/*
		 * Emit a WAL INPLACEHEAP_CLEAN record showing what we did
		 */
		if (RelationNeedsWAL(relbuf->relation))
		{
			XLogRecPtr	recptr;

			recptr = log_xheap_clean(relbuf->relation, relbuf->buffer, target_off_num,
								   space_required, prstate.nowunused,  prstate.nunused, 
								   prstate.nowfixed, prstate.fixedlen,  prstate.nfixed, 
								   prstate.latest_removed_xid, has_pruned);

			PageSetLSN(BufferGetPage(relbuf->buffer), recptr);
		}

		if (pruned)
			*pruned = has_pruned;
	}
	else
	{
		/*
		 * If we didn't prune anything, but have found a new value for the
		 * pd_prune_xid field, update it and mark the buffer dirty. This is
		 * treated as a non-WAL-logged hint.
		 *
		 * Also clear the "page is full" flag if it is set, since there's no
		 * point in repeating the prune/defrag process until something else
		 * happens to the page.
		 */
		if (FullTransactionIdEquals(((XHeapPageHeaderData *) page)->pd_prune_xid, prstate.new_prune_xid) ||
			XPageIsFull(page))
		{
			((XHeapPageHeaderData *) page)->pd_prune_xid = prstate.new_prune_xid;
			XPageClearFull(page);
			MarkBufferDirtyHint(relbuf->buffer, true);
		}
	}

	END_CRIT_SECTION();

	/*
	 * Report the number of tuples reclaimed to pgstats. This is ndeleted
	 * minus ndead, because we don't want to count a now-DEAD item or a
	 * now-DELETED item as a deletion for this purpose.
	 * -- no autovacuum so need to report how many we cleaned up
	 */
	if (report_stats && has_pruned && ndeleted > 0)
		pgstat_update_heap_dead_tuples(relbuf->relation, ndeleted);

	*latest_removed_xid = prstate.latest_removed_xid;

	if (remain)
		*remain = prstate.remain;

	/*
	 * XXX Should we update FSM information for this?  Not doing so will
	 * increase the chances of in-place updates.  See heap_page_prune for a
	 * detailed reason.
	 */

	return ndeleted;
}

int
xheap_page_prune(Relation relation, const RelationBuffer *relbuf, FullTransactionId oldest_xmin,
			   bool report_stats, FullTransactionId *latest_removed_xid, bool *pruned)
{
	int				  ndeleted = 0;
	Size			  spaceFreed = 0;
	OffsetNumber	  offnum;
	OffsetNumber	  maxoff;
	XPruneState		  prstate;
	bool			  execute_pruning = false;
	bool			  has_pruned = false;
	Page			  page = BufferGetPage(relbuf->buffer);

	if (pruned)
		*pruned = false;

	/*
	 * Our strategy is to scan the page and make lists of items to change,
	 * then apply the changes within a critical section.  This keeps as much
	 * logic as possible out of the critical section, and also ensures that
	 * WAL replay will work the same as the normal case.
	 *
	 * First, initialize the new pd_prune_xid value to zero (indicating no
	 * prunable tuples).  If we find any tuples which may soon become
	 * prunable, we will save the lowest relevant XID in new_prune_xid. Also
	 * initialize the rest of our working state.
	 */
	prstate.new_prune_xid = InvalidFullTransactionId;
	prstate.latest_removed_xid = *latest_removed_xid;
	prstate.nunused = 0;
	prstate.remain= 0;
	prstate.nfixed = 0;
	memset(prstate.marked, 0, sizeof(prstate.marked));

	/*
	 * If caller has asked to rearrange the page and page is not marked for
	 * pruning, then skip scanning the page.
	 *
	 * XXX We might want to remove this check once we have some optimal
	 * strategy to rearrange the page where we anyway need to traverse all
	 * rows.
	 */
	if (!XPageIsPrunableWithOldestXmin(page, oldest_xmin))
	{
		; /* no need to scan */
	}
	else
	{
		/* Scan the page */
		maxoff = xheap_page_get_max_offset_number(page);
		for (offnum = FirstOffsetNumber; offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			RowPtr *itemid = NULL;

			/* Ignore items already processed as part of an earlier chain */
			if (prstate.marked[offnum])
				continue;

			/* Nothing to do if slot is empty, already dead or marked as */
			itemid = XPageGetRowPtr(page, offnum);
			if (!RowPtrIsUsed(itemid))
				continue;

			/* Process this item */
			ndeleted +=
				xheap_prune_item(relbuf, offnum, oldest_xmin, &prstate, &spaceFreed, true);
		}
	}

	/* Do we want to prune? */
	if (prstate.nunused > 0 || prstate.nfixed > 0)
		execute_pruning = true;

	/* Any error while applying the changes is critical */
	START_CRIT_SECTION();

	if (execute_pruning)
	{
		/*
		 * Apply the planned item changes, then repair page fragmentation, and
		 * update the page's hint bit about whether it has free line pointers.
		 * first print relation oid
		 */
		xheap_page_prune_execute(relbuf->buffer, InvalidOffsetNumber, &prstate);

		/*
		 * Finally, repair any fragmentation, and update the page's hint bit
		 * whether it has free pointers.
		 */
		xheap_page_repaire_fregmentation(relation, relbuf->buffer, InvalidOffsetNumber, 0, &has_pruned);

		/*
		 * Update the page's pd_prune_xid field to either zero, or the lowest
		 * XID of any soon-prunable tuple.
		 */
		((XHeapPageHeaderData *) page)->pd_prune_xid = prstate.new_prune_xid;

		/*
		 * Also clear the "page is full" flag, since there's no point in
		 * repeating the prune/defrag process until something else happens to
		 * the page.
		 */
		XPageClearFull(page);

		MarkBufferDirty(relbuf->buffer);

		/*
		 * Emit a WAL INPLACEHEAP_CLEAN record showing what we did
		 */
		if (RelationNeedsWAL(relbuf->relation))
		{
			XLogRecPtr recptr;

			recptr = log_xheap_clean(relbuf->relation, relbuf->buffer, InvalidOffsetNumber,
								   0,  prstate.nowunused, prstate.nunused,
								   prstate.nowfixed, prstate.fixedlen, prstate.nfixed,
								   prstate.latest_removed_xid, has_pruned);

			PageSetLSN(BufferGetPage(relbuf->buffer), recptr);
		}

		if (pruned)
			*pruned = has_pruned;
	}
	else
	{

		/*
		 * If we didn't prune anything, but have found a new value for the
		 * pd_prune_xid field, update it and mark the buffer dirty. This is
		 * treated as a non-WAL-logged hint.
		 *
		 * Also clear the "page is full" flag if it is set, since there's no
		 * point in repeating the prune/defrag process until something else
		 * happens to the page.
		 */
		if (FullTransactionIdEquals(((XHeapPageHeaderData *) page)->pd_prune_xid, prstate.new_prune_xid) ||
			XPageIsFull(page))
		{
			((XHeapPageHeaderData *) page)->pd_prune_xid = prstate.new_prune_xid;
			XPageClearFull(page);
			MarkBufferDirtyHint(relbuf->buffer, true);
		}
	}

	END_CRIT_SECTION();

	/*
	 * Report the number of tuples reclaimed to pgstats. This is ndeleted
	 * minus ndead, because we don't want to count a now-DEAD item or a
	 * now-DELETED item as a deletion for this purpose.
	 * -- no autovacuum so need to report how many we cleaned up.
	 */
	if (report_stats && has_pruned && ndeleted > 0)
		pgstat_update_heap_dead_tuples(relbuf->relation, ndeleted);

	*latest_removed_xid = prstate.latest_removed_xid;

	/*
	 * XXX Should we update FSM information for this?  Not doing so will
	 * increase the chances of in-place updates.  See heap_page_prune for a
	 * detailed reason.
	 */

	return ndeleted;
}

/*
 * Perform the actual page changes needed by xheap_page_prune_guts.
 * It is expected that the caller has suitable pin and lock on the
 * buffer, and is inside a critical section.
 */
void
xheap_page_prune_execute(Buffer buffer, OffsetNumber target_offnum,
					  const XPruneState *prstate)
{
	Page		page = (Page) BufferGetPage(buffer);
	const OffsetNumber *offnum;
	const uint16 *tupSize;
	int			i;

	/* Update all now-unused line pointers */
	offnum = prstate->nowunused;
	for (i = 0; i < prstate->nunused; i++)
	{
		OffsetNumber off = *offnum++;
		RowPtr	   *lp = NULL;

		/* The target offset must not be unused. */
		Assert(target_offnum != off);

		lp = XPageGetRowPtr(page, off);

		RowPtrSetUnused(lp);
	}

	offnum = prstate->nowfixed;
	tupSize = prstate->fixedlen;
	for (i = 0; i < prstate->nfixed; i++)
	{
		OffsetNumber off = *offnum++;
		uint16		realSize = *tupSize++;
		RowPtr	   *lp = XPageGetRowPtr(page, off);

		/* The target offset must not be fixed. */
		Assert(target_offnum != off);
		RowPtrChangeLen(lp, realSize);
	}
}

/*
 * Prune specified item pointer.
 *
 * oldestXmin is the cutoff XID used to identify dead tuples.
 *
 * We don't actually change the page here.  We just add entries to the arrays in
 * prstate showing the changes to be made.  Items to be set to RP_DEAD state are
 * added to nowdead[]; items to be set to RP_DELETED are added to nowdeleted[];
 * and items to be set to RP_UNUSED state are added to nowunused[].
 *
 * Returns the number of tuples (to be) deleted from the page.
 */
static int
xheap_prune_item(const RelationBuffer *relbuf, OffsetNumber offnum,
			   FullTransactionId oldest_xmin, XPruneState *prstate, Size *space_freed,
			   bool is_target)
{
	XHeapTupleData tup;
	RowPtr	   *lp;
	Page		dp = (Page) BufferGetPage(relbuf->buffer);
	int			ndeleted = 0;
	FullTransactionId xid = InvalidFullTransactionId;
	bool		tupdead = false;
	bool		inplace_updated = false;

	lp = XPageGetRowPtr(dp, offnum);

	Assert(RowPtrIsNormal(lp));

	tup.disk_tuple = (XHeapDiskTuple) XPageGetRowData(dp, lp);
	tup.disk_tuple_size = RowPtrGetLen(lp);
	ItemPointerSet(&(tup.ctid), BufferGetBlockNumber(relbuf->buffer), offnum);
	tup.table_oid = RelationGetRelid(relbuf->relation);

	/*
	 * Check tuple's visibility status.
	 */
	tupdead = false;

	switch (xheap_tuple_satisfies_oldest_xmin(&tup, oldest_xmin, relbuf->buffer, false, NULL,
										  &xid,relbuf->relation, &inplace_updated,
										  NULL))
	{
		case XHEAPTUPLE_DEAD:
			tupdead = true;
			break;

		case XHEAPTUPLE_RECENTLY_DEAD:
			break;

		case XHEAPTUPLE_DELETE_IN_PROGRESS:
			prstate->remain++;
			/*
			 * This tuple may soon become DEAD.  Update the hint field so that
			 * the page is reconsidered for pruning in future.
			 */
			xheap_prune_record_prunable(prstate, xid);
			break;

		case XHEAPTUPLE_LIVE:
			prstate->remain++;
			break;
		case XHEAPTUPLE_INSERT_IN_PROGRESS:
			/*
			 * If we wanted to optimize for aborts, we might consider marking
			 * the page prunable when we see INSERT_IN_PROGRESS. But we don't.
			 * See related decisions about when to mark the page prunable in
			 * heapam.c.
			 */
			break;

		case XHEAPTUPLE_ABORT_IN_PROGRESS:
			/*
			 * We can simply skip the tuple if it has inserted/operated by
			 * some aborted transaction and its rollback is still pending.
			 * It'll be taken care of by future prune calls.
			 */
			if ((tup.disk_tuple->flag & XHEAP_DELETED) || 
				(tup.disk_tuple->flag & XHEAP_UPDATED) ||
				(tup.disk_tuple->flag & XHEAP_INPLACE_UPDATED))
				prstate->remain++;
			break;
		default:
			elog(ERROR, "unexpected InplaceHeapTupleSatisfiesOldestXmin result");
			break;
	}

	if (inplace_updated && !is_target)
	{
		int			tupSize = xheap_cal_tuple_size(relbuf->relation, tup.disk_tuple, NULL);

		if (tupSize < (int) tup.disk_tuple_size)
			xheap_prune_record_fixed(prstate, offnum, relbuf->relation, (uint16) tupSize);
	}

	if (tupdead)
		xheap_tuple_header_advance_latest_removed_xid(tup.disk_tuple, xid,
												&prstate->latest_removed_xid);
		
	/* 
	 * In Xstore, we don't have transaction slot like TD in zheap. So the recentDead tuple will not be freed here,
	 * it will remain in xheap untill becoming surely dead .
	 */
	if (tupdead)
	{
		/*
		 * Count dead or recently dead tuple in result and update the space
		 * that can be freed.
		 */
		ndeleted++;
		Assert(!FullTransactionIdIsValid(xid) || !xstore_transaction_id_is_in_progress(xid));
		if (FullTransactionIdIsValid(xid) && xstore_transaction_id_is_in_progress(xid))
			ereport(PANIC, (errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("Tuple will be pruned but xid is inprogress, xid=%lu, "
								   "oldestxmin=%lu, globalRecycleXid=%lu.",
								   xid.value, oldest_xmin.value,
								   pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid))));
		/* short aligned */
		*space_freed += SHORTALIGN(tup.disk_tuple_size);
	}

	/* Record dead item as unused */
	if (tupdead)
		xheap_prune_record_unused(prstate, offnum, relbuf->relation);

	return ndeleted;
}

static int
Itemoffcompare(const void *itemidp1, const void *itemidp2)
{
	/* Sort in decreasing itemoff order */
	return ((itemIdCompact) itemidp2)->itemoff - ((itemIdCompact) itemidp1)->itemoff;
}

int
xheap_cal_tuple_size(Relation relation, XHeapDiskTuple diskTuple, TupleDesc scanTupDesc)
{
	TupleDesc	row_desc = (scanTupDesc == NULL) ? RelationGetDescr(relation) : scanTupDesc;
	bool		hasnulls = XHeapDiskTupHasNulls(diskTuple);
	Form_pg_attribute att = row_desc->attrs;
	int			natts;			/* number of atts to extract */
	int			attnum;
	bits8	   *bp = diskTuple->data;
	int			off = diskTuple->t_hoff;
	char	   *tupPtr = (char *) diskTuple;
	int			nullcount = 0;
	int			tuple_attrs = XHeapTupleHeaderGetNatts(diskTuple);
	bool		enable_reverse_bitmap = NAttrsReserveSpace(tuple_attrs);

	natts = Min(tuple_attrs, row_desc->natts);
	for (attnum = 0; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = &(att[attnum]);

		if (hasnulls && att_isnull(attnum, bp))
		{
			/* Skip attribute length in case the tuple was stored with
			 * space reserved for null attributes */
			if (enable_reverse_bitmap)
				if (!att_isnull(tuple_attrs + nullcount, bp))
					off += thisatt->attlen;

			nullcount++;

			continue;
		}

		/*
		 * If this is a varlena, there might be alignment padding, if it has a
		 * 4-byte header.  Otherwise, there will only be padding if it's not
		 * pass-by-value.
		 */
		if (thisatt->attlen == -1)
			off = att_align_pointer(off, thisatt->attalign, -1, tupPtr + off);
		else if (!thisatt->attbyval)
			off = att_align_nominal(off, thisatt->attalign);

		off = att_addlength_pointer(off, thisatt->attlen, tupPtr + off);
	}

	return off;
}

/* Record lowest soon-prunable XID */
static void
xheap_prune_record_prunable(XPruneState *prstate, FullTransactionId xid)
{
	/*
	 * This should exactly match the PageSetPrunable macro.  We can't store
	 * directly into the page header yet, so we update working state.
	 */
	Assert(FullTransactionIdIsNormal(xid));
	if (!FullTransactionIdIsValid(prstate->new_prune_xid) ||
		FullTransactionIdPrecedes(xid, prstate->new_prune_xid))
		prstate->new_prune_xid = xid;
}


/* Record item pointer to be marked dead */
static void
xheap_prune_record_unused(XPruneState *prstate, OffsetNumber offnum, Relation relation)
{
	Assert(prstate->nunused < MaxXHeapTuplesPerPage(relation));
	prstate->nowunused[prstate->nunused] = offnum;
	prstate->nunused++;
	Assert(offnum < MaxXHeapTuplesPerPage(relation) + 1);
	Assert(!prstate->marked[offnum]);
	prstate->marked[offnum] = true;
}

static void
xheap_prune_record_fixed(XPruneState *prstate, OffsetNumber offnum, Relation relation,
					  uint16 tupSize)
{
	Assert(prstate->nfixed < MaxXHeapTuplesPerPage(relation));
	Assert(offnum < MaxXHeapTuplesPerPage(relation) + 1);
	prstate->nowfixed[prstate->nfixed] = offnum;
	prstate->fixedlen[prstate->nfixed] = tupSize;
	prstate->nfixed++;
}

/*
 * log_xheap_clean - Perform XLogInsert for a xheap-clean operation.
 *
 * Caller must already have modified the buffer and marked it dirty.
 *
 * We also include latestRemovedXid, which is the greatest XID present in
 * the removed tuples. That allows recovery processing to cancel or wait
 * for long standby queries that can still see these tuples.
 */
XLogRecPtr
log_xheap_clean(Relation reln, Buffer buffer, OffsetNumber target_offnum,
			  Size space_required, OffsetNumber *nowunused, int nunused,
			  OffsetNumber *nowfixed, uint16 *fixedlen, uint16 nfixed,
			  FullTransactionId latest_removed_xid, bool pruned)
{
	XLogRecPtr	 recptr;
	xl_xheap_clean xl_rec;

	/* Caller should not call me on a non-WAL-logged relation */
	Assert(RelationNeedsWAL(reln));

	xl_rec.latest_removed_xid = latest_removed_xid;
	xl_rec.nunused = nunused;
	xl_rec.nfixed = nfixed;
	xl_rec.flags = 0;
	XLogBeginInsert();

	if (pruned)
		xl_rec.flags |= XLZ_CLEAN_ALLOW_PRUNING;
	XLogRegisterData((char *) &xl_rec, SizeOfXHeapClean);

	/* Register the offset information. */
	if (target_offnum != InvalidOffsetNumber)
	{
		xl_rec.flags |= XLZ_CLEAN_CONTAINS_OFFSET;
		XLogRegisterData((char *) &target_offnum, sizeof(OffsetNumber));
		XLogRegisterData((char *) &space_required, sizeof(space_required));
	}

	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	/*
     * The OffsetNumber arrays are not actually in the buffer, but we pretend
     * that they are.  When XLogInsert stores the whole buffer, the offset
     * arrays need not be stored too.  Note that even if all three arrays are
     * empty, we want to expose the buffer as a candidate for whole-page
     * storage, since this record type implies a defragmentation operation
     * even if no item pointers changed state.
     */
	if (nunused > 0)
		XLogRegisterBufData(0, (char *) nowunused, nunused * sizeof(OffsetNumber));

	if (nfixed > 0)
	{
		xl_rec.flags |= XLZ_CLEAN_CONTAINS_TUPLEN;
		XLogRegisterBufData(0, (char *) nowfixed, nfixed * sizeof(OffsetNumber));
		XLogRegisterBufData(0, (char *) fixedlen, nfixed * sizeof(OffsetNumber));
	}

	recptr = XLogInsert(RM_XHEAP_ID, XLOG_XHEAP_CLEAN);

	return recptr;
}

/*
 * After removing or marking some line pointers unused, move the tuples to
 * remove the gaps caused by the removed items.  Here, we are rearranging
 * the page such that tuples will be placed in itemid order.  It will help
 * in the speedup of future sequential scans.
 */
static void
compactify_xtuples(itemIdCompact itemidbase, int nitems, Page page,
				 OffsetNumber target_offnum)
{
	XHeapPageHeaderData *phdr = (XHeapPageHeaderData *) page;

	/* We copy over the targetOffnum last */
	union
	{
		XHeapDiskTupleData hdr;
		char		data[MaxPossibleXHeapTupleSize];
	} tbuf;

	XHeapTupleData tuple;
	uint16		new_tuple_len = 0;
	Offset		curr;

	memset(&tbuf, 0, sizeof(tbuf));
	tuple.disk_tuple = &(tbuf.hdr);
	tuple.disk_tuple_size = 0;

	if (target_offnum != InvalidOffsetNumber)
	{
		RowPtr	   *rp = XPageGetRowPtr(page, target_offnum);
		XHeapDiskTuple item;

		Assert(RowPtrIsNormal(rp));
		Assert(sizeof(tbuf) >= rp->len);

		tuple.disk_tuple_size = rp->len;
		item = (XHeapDiskTuple) XPageGetRowData(page, rp);
		memcpy((char *) tuple.disk_tuple, (char *) item, tuple.disk_tuple_size);
	}

	/* sort itemIdSortData array into decreasing itemoff order */
	qsort((char *) itemidbase, nitems, sizeof(itemIdCompactData), Itemoffcompare);

	curr = phdr->pd_special;
	for (int i = 0; i < nitems; i++)
	{
		itemIdCompact itemidptr = &itemidbase[i];
		RowPtr	   *lp;

		if (target_offnum == itemidptr->offsetindex + 1)
		{
			new_tuple_len = itemidptr->alignedlen;
			continue;
		}

		lp = XPageGetRowPtr(page, itemidptr->offsetindex + 1);
		curr -= itemidptr->alignedlen;

		memmove((char *) page + curr, (char *) page + itemidptr->itemoff, itemidptr->alignedlen);

		lp->offset = curr;
	}

	if (target_offnum != InvalidOffsetNumber)
	{
		RowPtr	   *rp;

		curr -= new_tuple_len;
		memcpy((char *) page + curr, (char *) tuple.disk_tuple, tuple.disk_tuple_size);

		rp = XPageGetRowPtr(page, target_offnum);
		rp->offset = curr;
	}

	phdr->pd_upper = curr;
}

/*
 * xheap_page_repaire_fregmentation
 *
 * Frees fragmented space on a page.
 *
 * The basic idea is same as PageRepairFragmentation, but here we additionally
 * deal with unused items that can't be immediately reclaimed.  We don't allow
 * page to be pruned, if there is an inplace update from an open transaction.
 * The reason is that we don't know the size of previous row in undo which
 * could be bigger in which case we might not be able to perform rollback once
 * the page is repaired.  Now, we can always traverse the undo chain to find
 * the size of largest tuple in the chain, but we don't do that for now as it
 * can take time especially if there are many such tuples on the page.
 */
void
xheap_page_repaire_fregmentation(Relation rel, Buffer buffer, OffsetNumber targetOffnum,
						 Size spaceRequired, bool *pruned)
{
	Page		page = BufferGetPage(buffer);
	uint16		pd_lower = ((XHeapPageHeaderData *) page)->pd_lower;
	uint16		pd_upper = ((XHeapPageHeaderData *) page)->pd_upper;
	uint16		pd_special = ((XHeapPageHeaderData *) page)->pd_special;
	itemIdCompactData itemidbase[MaxPossibleXHeapTuplesPerPage];
	itemIdCompact itemidptr;
	RowPtr	   *lp = NULL;
	int			nline;
	int			nstorage;
	int			nunused;
	int			i;
	Size		totallen;

	/*
	 * It's worth the trouble to be more paranoid here than in most places,
	 * because we are about to reshuffle data in (what is usually) a shared
	 * disk buffer.  If we aren't careful then corrupted pointers, lengths,
	 * etc. could cause us to clobber adjacent disk buffers, spreading the
	 * data loss further.  So, check everything.
	 */
	if (pd_lower < (SizeOfXHeapPageHeaderData) ||
		pd_lower > pd_upper || pd_upper > pd_special || pd_special > BLCKSZ ||
		pd_special != MAXALIGN(pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						pd_lower, pd_upper, pd_special)));

	nline = xheap_page_get_max_offset_number(page);

	/*
	 * Run through the line pointer array and collect data about live items.
	 */
	itemidptr = itemidbase;
	nunused = totallen = 0;
	for (i = FirstOffsetNumber; i <= nline; i++)
	{
		lp = XPageGetRowPtr(page, i);
		if (RowPtrIsUsed(lp))
		{
			Assert(RowPtrHasStorage(lp));

			itemidptr->offsetindex = i - 1;
			itemidptr->itemoff = RowPtrGetOffset(lp);
			if (unlikely(itemidptr->itemoff < (int) pd_upper ||
							itemidptr->itemoff >= (int) pd_special))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("corrupted item pointer: %u", itemidptr->itemoff)));
		    /*
			 * We need to save additional space for the target offset, so
			 * that we can save the space for new tuple.
			 */
			if (i == targetOffnum)
				itemidptr->alignedlen = SHORTALIGN(RowPtrGetLen(lp) + spaceRequired);
			else
				itemidptr->alignedlen = SHORTALIGN(RowPtrGetLen(lp));

			totallen += itemidptr->alignedlen;
			itemidptr++;
		}
		else
		{
			nunused++;
			/* Unused entries should have lp_len = 0, but make sure */
			RowPtrSetUnused(lp);
		}
	}

	nstorage = itemidptr - itemidbase;
	if (nstorage == 0)
		/* Page is completely empty, so just reset it quickly */
		((XHeapPageHeaderData *) page)->pd_upper = pd_special;
	else
	{
		/* Need to compact the page the hard way */
		if (totallen > (Size) (pd_special - pd_lower))
		{
			/* The future length is too big, so no need to prune*/
			if (pruned)
				*pruned = false;
			return;
		}

		compactify_xtuples(itemidbase, nstorage, page, targetOffnum);
	}

	/* Set hint bit for PageAddItem */
	if (nunused > 0)
		XPageSetHasFreeLinePointers(page);
	else
		XPageClearHasFreeLinePointers(page);

	/* indicate that the page has been pruned */
	if (pruned)
		*pruned = true;
}
