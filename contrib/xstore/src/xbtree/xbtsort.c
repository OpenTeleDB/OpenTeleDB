/*-------------------------------------------------------------------------
 *
 * xbtsort.c
 *		Build a btree from sorted input by loading leaf pages sequentially.
 *
 * NOTES
 *
 * We use tuplesort.c to sort the given index tuples into order.
 * Then we scan the index tuples in order and build the btree pages
 * for each level.  We load source tuples into leaf-level pages.
 * Whenever we fill a page at one level, we add a link to it to its
 * parent level (starting a new parent level if necessary).  When
 * done, we write out each final page on each level, adding it to
 * its parent level.  When we have only one page on a level, it must be
 * the root -- it can be attached to the btree metapage and we are done.
 *
 * It is not wise to pack the pages entirely full, since then *any*
 * insertion would cause a split (and not only of the leaf page; the need
 * for a split would cascade right up the tree).  The steady-state load
 * factor for btrees is usually estimated at 70%.  We choose to pack leaf
 * pages to the user-controllable fill factor (default 90%) while upper pages
 * are always packed to 70%.  This gives us reasonable density (there aren't
 * many upper pages if the keys are reasonable-size) without risking a lot of
 * cascading splits during early insertions.
 *
 * Formerly the index pages being built were kept in shared buffers, but
 * that is of no value (since other backends have no interest in them yet)
 * and it created locking problems for CHECKPOINT, because the upper-level
 * pages were held exclusive-locked for long periods.  Now we just build
 * the pages in local memory and smgrwrite or smgrextend them as we finish
 * them.  They will need to be re-read into shared buffers on first use after
 * the build finishes.
 *
 * Since the index will never be used unless it is completely built,
 * from a crash-recovery point of view there is no need to WAL-log the
 * steps of the build.  After completing the index build, we can just sync
 * the whole file to disk using smgrimmedsync() before exiting this module.
 * This can be seen to be sufficient for crash recovery by considering that
 * it's effectively equivalent to what would happen if a CHECKPOINT occurred
 * just after the index build.  However, it is clearly not sufficient if the
 * DBA is using the WAL log for PITR or replication purposes, since another
 * machine would not be able to reconstruct the index from WAL.  Therefore,
 * we log the completed index pages to WAL if and only if WAL archiving is
 * active.
 *
 * This code isn't concerned about the FSM at all. The caller is responsible
 * for initializing that.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/xbtree/xbtsort.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "xbtree/xbtree.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/smgr.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "access/transam.h"
#include "xbtree/xbttup.h"
#include "nodes/execnodes.h"
#include "commands/progress.h"
#include "pgstat.h"

/*
 * Overall status record for index writing phase.
 */
typedef struct XBTWriteState
{
	Relation	heap;
	Relation	index;
	BulkWriteState *bulkstate;
	BTScanInsert inskey;		/* generic insertion scankey */
	bool		btws_use_wal;	/* dump pages to WAL? */
	BlockNumber btws_pages_alloced; /* # pages allocated */
	BlockNumber btws_pages_written; /* # pages written out */
	Page		btws_zeropage;	/* workspace for filling zeroes */
} XBTWriteState;

/*
 * Status record for a btree page being built.  We have one of these
 * for each active tree level.
 */
typedef struct XBTPageState
{
	BulkWriteBuffer btps_buf;	/* workspace for page building */
	BlockNumber btps_blkno;		/* block # to write this page at */
	IndexTuple	btps_lowkey;	/* page's strict lower bound pivot tuple */
	OffsetNumber btps_lastoff;	/* last item offset loaded */
	Size		btps_lastextra; /* last item's extra posting list space */
	uint32		btps_level;		/* tree level (0 = leaf) */
	Size		btps_full;		/* "full" if less than this much free space */
	struct XBTPageState *btps_next;	/* link to parent level, if any */
} XBTPageState;

static BulkWriteBuffer _xbt_blnewpage(XBTWriteState *wstate, uint32 level);
static void _xbt_slideleft(Page page);
static void _xbt_sortaddtup(Page page, Size itemsize, IndexTuple itup, OffsetNumber itup_off, bool is_xbtree_tuple);
static void _xbt_load(XBTWriteState *wstate, BTSpool *btspool, BTSpool *btspool2);
static void _xbt_leafbuild(BTSpool *btspool, BTSpool *btspool2);
static XBTPageState* _xbt_pagestate(XBTWriteState* wstate, uint32 level);
static void _xbt_buildadd(XBTWriteState *wstate, XBTPageState *state, IndexTuple itup);
static void _xbt_uppershutdown(XBTWriteState* wstate, XBTPageState* state);

/*
 *	xbtbuild() -- build a new btree index.
 */
IndexBuildResult *
xbtbuild(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	IndexBuildResult *result = NULL;
	BTBuildState	  buildstate;
	double			  reltuples = 0;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
		ResetUsage();
#endif /* BTREE_BUILD_STATS */

	buildstate.isunique = indexInfo->ii_Unique;
	buildstate.nulls_not_distinct = indexInfo->ii_NullsNotDistinct;
	buildstate.havedead = false;
	buildstate.heap = heap;
	buildstate.spool = NULL;
	buildstate.spool2 = NULL;
	buildstate.indtuples = 0;
	buildstate.btleader = NULL;

	/*
	 * We expect to be called exactly once for any index relation. If that's
	 * not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	reltuples = _bt_spools_heapscan(heap, index, &buildstate, indexInfo);

	/*
	 * Finish the build by (1) completing the sort of the spool file, (2)
	 * inserting the sorted tuples into btree pages and (3) building the upper
	 * levels.  Finally, it may also be necessary to end use of parallelism.
	 */
	_xbt_leafbuild(buildstate.spool, buildstate.spool2);
	_bt_spooldestroy(buildstate.spool);
	if (buildstate.spool2)
		_bt_spooldestroy(buildstate.spool2);
	if (buildstate.btleader)
		_bt_end_parallel(buildstate.btleader);
	
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD STATS");
		ResetUsage();
	}
#endif /* BTREE_BUILD_STATS */
	
	return result;
}

/*
 * given a spool loaded by successive calls to _bt_spool,
 * create an entire btree.
 */
static void
_xbt_leafbuild(BTSpool *btspool, BTSpool *btspool2)
{
	XBTWriteState wstate;

#ifdef BTREE_BUILD_STATS
	if (log_btree_build_stats)
	{
		ShowUsage("BTREE BUILD (Spool) STATISTICS");
		ResetUsage();
	}
#endif /* BTREE_BUILD_STATS */

	/* Execute the sort */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_PERFORMSORT_1);
	tuplesort_performsort(btspool->sortstate);
	if (btspool2 != NULL)
	{
		pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
									 PROGRESS_BTREE_PHASE_PERFORMSORT_2);
		tuplesort_performsort(btspool2->sortstate);
	}

	wstate.heap = btspool->heap;
	wstate.index = btspool->index;
	wstate.inskey = _xbt_mkscankey(wstate.index, NULL);

	/* reserve the metapage */
	wstate.btws_pages_alloced = BTREE_METAPAGE + 1;

	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE,
								 PROGRESS_BTREE_PHASE_LEAF_LOAD);
	_xbt_load(&wstate, btspool, btspool2);
}

/*
 * allocate workspace for a new, clean btree page, not linked to any siblings.
 */
static BulkWriteBuffer 
_xbt_blnewpage(XBTWriteState *wstate, uint32 level)
{
	BulkWriteBuffer buf;
	Page		page;
	XBTPageOpaqueInternal opaque;
	
	buf = smgr_bulk_get_buf(wstate->bulkstate);
	page = (Page) buf;

	/* Zero the page and set up standard page header info */
	_xbt_pageinit(page, BLCKSZ);

	/* Initialize BT opaque state */
	opaque = (XBTPageOpaqueInternal)PageGetSpecialPointer(page);
	opaque->btpo_prev = opaque->btpo_next = P_NONE;
	opaque->btpo.level = level;
	opaque->btpo_flags = (level > 0) ? 0 : BTP_LEAF;
	opaque->btpo_cycleid = 0;

	/* Make the P_HIKEY line pointer appear allocated */
	((PageHeader)page)->pd_lower += sizeof(ItemIdData);

	return buf;
}

/*
 * emit a completed btree page, and release the working storage.
 */
static void _xbt_blwritepage(XBTWriteState *wstate, BulkWriteBuffer buf, BlockNumber blkno)
{
	smgr_bulk_write(wstate->bulkstate, blkno, buf, true);
	/* smgr_bulk_write took ownership of 'buf' */
}

/*
 * allocate and initialize a new BTPageState.  the returned structure
 * is suitable for immediate use by _bt_buildadd.
 */
static XBTPageState *
_xbt_pagestate(XBTWriteState *wstate, uint32 level)
{
	XBTPageState *state = (XBTPageState *)palloc0(sizeof(XBTPageState));

	/* create initial page for level */
	state->btps_buf = _xbt_blnewpage(wstate, level);

	/* and assign it a page position */
	state->btps_blkno = wstate->btws_pages_alloced++;

	state->btps_lowkey = NULL;
	/* initialize lastoff so first item goes into P_FIRSTKEY */
	state->btps_lastoff = P_HIKEY;
	state->btps_level = level;
	/* set "full" threshold based on level.  See notes at head of file. */
	if (level > 0)
		state->btps_full = (BLCKSZ * (100 - BTREE_NONLEAF_FILLFACTOR) / 100);
	else
		state->btps_full = (Size)RelationGetTargetPageFreeSpace(wstate->index, BTREE_DEFAULT_FILLFACTOR);
	/* no parent level, yet */
	state->btps_next = NULL;

	return state;
}

/*
 * slide an array of ItemIds back one slot (from P_FIRSTKEY to
 * P_HIKEY, overwriting P_HIKEY).  we need to do this when we discover
 * that we have built an ItemId array in what has turned out to be a
 * P_RIGHTMOST page.
 */
static void _xbt_slideleft(Page page)
{
	OffsetNumber off;
	OffsetNumber maxoff;
	ItemId previi;
	ItemId thisii;

	if (!PageIsEmpty(page))
	{
		maxoff = PageGetMaxOffsetNumber(page);
		previi = PageGetItemId(page, P_HIKEY);
		for (off = P_FIRSTKEY; off <= maxoff; off = OffsetNumberNext(off))
		{
			thisii = PageGetItemId(page, off);
			*previi = *thisii;
			previi = thisii;
		}
		((PageHeader)page)->pd_lower -= sizeof(ItemIdData);
	}
}

/*
 * Add an item to a page being built.
 *
 * The main difference between this routine and a bare PageAddItem call
 * is that this code knows that the leftmost data item on a non-leaf
 * btree page doesn't need to have a key.  Therefore, it strips such
 * items down to just the item header.
 *
 * This is almost like xbtinsert.c's _xbt_pgaddtup(), but we can't use
 * that because it assumes that P_RIGHTMOST() will return the correct
 * answer for the page.  Here, we don't know yet if the page will be
 * rightmost.  Offset P_FIRSTKEY is always the first data key.
 */
static void _xbt_sortaddtup(Page page,
							Size itemsize,
							IndexTuple itup,
							OffsetNumber itup_off,
							bool is_copied)
{
	XBTPageOpaqueInternal opaque = (XBTPageOpaqueInternal)PageGetSpecialPointer(page);
	IndexTupleData trunctuple;

	if (!P_ISLEAF(opaque) && itup_off == P_FIRSTKEY)
	{
		trunctuple = *itup;
		trunctuple.t_info = sizeof(IndexTupleData);
		XBTreeTupleSetNAtts(&trunctuple, 0, false);
		itup = &trunctuple;
		itemsize = sizeof(IndexTupleData);
	}

	if (XBTreeTupleIsPivot(itup) || is_copied)
	{
		/* 
		 * pivot tuple or copied xbtree tuple already contains modified_xid、undo pointer. 
		 * Don't need to handle modified_xid and undo pointer, normal insert 
		 */
		if (PageAddItem(page, (Item)itup, itemsize, itup_off, 
						false, false) == InvalidOffsetNumber)
			ereport(PANIC, (errcode(ERRCODE_INDEX_CORRUPTED),
					errmsg("Index tuple cant fit in the page when creating index.")));
	}
	else
	{
		Size real_size = IndexTupleSize(itup);
		Size item_size = real_size - TXNINFOSIZE;
		XBTreeIndexTuple index_tuple;
		ItemId ii;
		IndexTupleSetSize(itup, item_size);

		/* reserve space for modified_xid、undo pointer and set into Frozen and Invalid */
		((PageHeader)page)->pd_upper -= TXNINFOSIZE;
		index_tuple = (XBTreeIndexTuple)(((char*)page) + ((PageHeader)page)->pd_upper);
		index_tuple->modified_xid = FullTransactionIdFromEpochAndXid(0, FrozenTransactionId);
		index_tuple->urec = INVALID_UNDO_REC_PTR;
		/* item pointer to be inserted don't contain modified_xid、undo pointer  */
		if (PageAddItem(page, (Item)itup, item_size, itup_off, 
						false, false) == InvalidOffsetNumber)
			ereport(PANIC, (errcode(ERRCODE_INDEX_CORRUPTED),
					errmsg("Index tuple cant fit in the page when creating index.")));
		ii = PageGetItemId(page, itup_off);
		ItemIdSetNormal(ii, ((PageHeader)page)->pd_upper, real_size);
		opaque->active_count++;
	}
}

/*----------
 * Add an item to a disk page from the sort output.
 *
 * We must be careful to observe the page layout conventions of nbtsearch.c:
 * - rightmost pages start data items at P_HIKEY instead of at P_FIRSTKEY.
 * - on non-leaf pages, the key portion of the first item need not be
 *	 stored, we should store only the link.
 *
 * A leaf page being built looks like:
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp0 linp1 linp2 ...           |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |	 ^ last										  |
 * |												  |
 * +-------------+------------------------------------+
 * |			 | itemN ...                          |
 * +-------------+------------------+-----------------+
 * |		  ... item3 item2 item1 | "special space" |
 * +--------------------------------+-----------------+
 *
 * Contrast this with the diagram in bufpage.h; note the mismatch
 * between linps and items.  This is because we reserve linp0 as a
 * placeholder for the pointer to the "high key" item; when we have
 * filled up the page, we will set linp0 to point to itemN and clear
 * linpN.  On the other hand, if we find this is the last (rightmost)
 * page, we leave the items alone and slide the linp array over.  If
 * the high key is to be truncated, offset 1 is deleted, and we insert
 * the truncated high key at offset 1.
 *
 * 'last' pointer indicates the last offset added to the page.
 *----------
 */
void _xbt_buildadd(XBTWriteState *wstate, XBTPageState *state, IndexTuple itup)
{
	BulkWriteBuffer nbuf;
	Page npage;
	BlockNumber nblkno;
	OffsetNumber last_off;
	bool isleaf;
	bool ispivot;
	Size pgspc;
	Size itupsz;

	/*
	 * This is a handy place to check for cancel interrupts during the btree
	 * load phase of index creation.
	 */
	CHECK_FOR_INTERRUPTS();

	nbuf = state->btps_buf;
	npage = (Page) nbuf;
	nblkno = state->btps_blkno;
	last_off = state->btps_lastoff;

	pgspc = PageGetFreeSpace(npage);
	/* Leaf case has slightly different rules due to suffix truncation */
	isleaf = (state->btps_level == 0);
	ispivot = XBTreeTupleIsPivot(itup);

	if (!ispivot)
	{
		/* normal index tuple, need reserve space for modified_xid and undo pointer */
		itupsz = IndexTupleSize(itup);
		IndexTupleSetSize(itup, itupsz + TXNINFOSIZE); /* size += 16B */
	}
	/* xbtree normal index tuple include reserved space */
	itupsz = IndexTupleSize(itup);
	itupsz = MAXALIGN(itupsz);

	/*
	 * Check whether the new item can fit on a btree page on current level at
	 * all.
	 *
	 * Every newly built index will treat heap TID as part of the keyspace,
	 * which imposes the requirement that new high keys must occasionally have
	 * a heap TID appended within _bt_truncate().  That may leave a new pivot
	 * tuple one or two MAXALIGN() quantums larger than the original first
	 * right tuple it's derived from.  v4 deals with the problem by decreasing
	 * the limit on the size of tuples inserted on the leaf level by the same
	 * small amount.  Enforce the new v4+ limit on the leaf level, and the old
	 * limit on internal levels, since pivot tuples may need to make use of
	 * the reserved space.  This should never fail on internal pages.
	 */
	if (unlikely(itupsz > XBTMaxItemSize(npage)))
		_xbt_check_third_page(wstate->index, wstate->heap, isleaf, npage, 
					itup);

	/*
	 * Check to see if current page will fit new item, with space left over to
	 * append a heap TID during suffix truncation when page is a leaf page.
	 *
	 * It is guaranteed that we can fit at least 2 non-pivot tuples plus a
	 * high key with heap TID when finishing off a leaf page, since we rely on
	 * _bt_check_third_page() rejecting oversized non-pivot tuples.  On
	 * internal pages we can always fit 3 pivot tuples with larger internal
	 * page tuple limit (includes page high key).
	 *
	 * Most of the time, a page is only "full" in the sense that the soft
	 * fillfactor-wise limit has been exceeded.  However, we must always leave
	 * at least two items plus a high key on each page before starting a new
	 * page.  Disregard fillfactor and insert on "full" current page if we
	 * don't have the minimum number of items yet.  (Note that we deliberately
	 * assume that suffix truncation neither enlarges nor shrinks new high key
	 * when applying soft limit.)
	 */
	if (pgspc < itupsz + (isleaf ? MAXALIGN(sizeof(ItemPointerData)) : 0) ||
		(pgspc < state->btps_full && last_off > P_FIRSTKEY))
	{
		/*
		 * Finish off the page and write it out.
		 */
		BulkWriteBuffer obuf = nbuf;
		Page		opage = npage;
		BlockNumber oblkno = nblkno;
		ItemId		ii;
		ItemId		hii;
		IndexTuple	oitup;

		/* Create new page of same level */
		nbuf = _xbt_blnewpage(wstate, state->btps_level);
		npage = (Page) nbuf;

		/* and assign it a page position */
		nblkno = wstate->btws_pages_alloced++;

		/*
		 * We copy the last item on the page into the new page, and then
		 * rearrange the old page so that the 'last item' becomes its high key
		 * rather than a true data item.  There had better be at least two
		 * items on the page already, else the page would be empty of useful
		 * data.
		 */
		Assert(last_off > P_FIRSTKEY);
		ii = PageGetItemId(opage, last_off);
		oitup = (IndexTuple)PageGetItem(opage, ii);
		_xbt_sortaddtup(npage, ItemIdGetLength(ii), oitup, P_FIRSTKEY, true);

		/*
		 * Move 'last' into the high key position on opage.  _bt_blnewpage()
		 * allocated empty space for a line pointer when opage was first
		 * created, so this is a matter of rearranging already-allocated space
		 * on page, and initializing high key line pointer. (Actually, leaf
		 * pages must also swap oitup with a truncated version of oitup, which
		 * is sometimes larger than oitup, though never by more than the space
		 * needed to append a heap TID.)
		 */
		hii = PageGetItemId(opage, P_HIKEY);
		*hii = *ii;
		ItemIdSetUnused(ii); /* redundant */
		((PageHeader)opage)->pd_lower -= sizeof(ItemIdData);

		if (isleaf)
		{
			IndexTuple	lastleft;
			IndexTuple	truncated;

			/*
			 * Truncate away any unneeded attributes from high key on leaf
			 * level.  This is only done at the leaf level because downlinks
			 * in internal pages are either negative infinity items, or get
			 * their contents from copying from one level down.  See also:
			 * _bt_split().
			 *
			 * We don't try to bias our choice of split point to make it more
			 * likely that _bt_truncate() can truncate away more attributes,
			 * whereas the split point passed to _bt_split() is chosen much
			 * more delicately.  Suffix truncation is mostly useful because it
			 * improves space utilization for workloads with random
			 * insertions.  It doesn't seem worthwhile to add logic for
			 * choosing a split point here for a benefit that is bound to be
			 * much smaller.
			 *
			 * Since the truncated tuple is often smaller than the original
			 * tuple, it cannot just be copied in place (besides, we want to
			 * actually save space on the leaf page).  We delete the original
			 * high key, and add our own truncated high key at the same
			 * offset.
			 *
			 * Note that the page layout won't be changed very much.  oitup is
			 * already located at the physical beginning of tuple space, so we
			 * only shift the line pointer array back and forth, and overwrite
			 * the tuple space previously occupied by oitup.  This is fairly
			 * cheap.
			 */
			ii = PageGetItemId(opage, OffsetNumberPrev(last_off));
			lastleft = (IndexTuple) PageGetItem(opage, ii);

			truncated = _xbt_truncate(wstate->index, lastleft, oitup, 
							wstate->inskey, false);
			/* delete "wrong" high key, insert truncated as P_HIKEY. */
			PageIndexTupleDelete(opage, P_HIKEY);
			_xbt_sortaddtup(opage, IndexTupleSize(truncated), truncated, P_HIKEY, true);
			pfree(truncated);

			/* oitup should continue to point to the page's high key */
			hii = PageGetItemId(opage, P_HIKEY);
			oitup = (IndexTuple) PageGetItem(opage, hii);
		}

		/*
		 * Link the old page into its parent, using its minimum key. If we
		 * don't have a parent, we have to create one; this adds a new btree
		 * level.
		 */
		if (state->btps_next == NULL)
			state->btps_next = _xbt_pagestate((XBTWriteState*)wstate, state->btps_level + 1);

		Assert(state->btps_lowkey != NULL);
		Assert((XBTreeTupleGetNAtts(state->btps_lowkey, wstate->index) <=
				 IndexRelationGetNumberOfKeyAttributes(wstate->index) &&
				 XBTreeTupleGetNAtts(state->btps_lowkey, wstate->index) > 0) ||
				P_LEFTMOST((XBTPageOpaqueInternal) PageGetSpecialPointer(opage)));
		Assert(XBTreeTupleGetNAtts(state->btps_lowkey, wstate->index) == 0 ||
				!P_LEFTMOST((XBTPageOpaqueInternal) PageGetSpecialPointer(opage)));
		
		XBTreeTupleSetDownLink(state->btps_lowkey, oblkno);
		_xbt_buildadd(wstate, state->btps_next, state->btps_lowkey);
		pfree(state->btps_lowkey);
		
		/*
		 * Save a copy of the minimum key for the new page.  We have to copy
		 * it off the old page, not the new one, in case we are not at leaf
		 * level.  Despite oitup is already initialized, it's important to get
		 * high key from the page, since we could have replaced it with
		 * truncated copy.	See comment above.
		 */
		state->btps_lowkey = CopyIndexTuple(oitup);

		/*
		 * Set the sibling links for both pages.
		 */
		{
			XBTPageOpaqueInternal oopaque = (XBTPageOpaqueInternal)PageGetSpecialPointer(opage);
			XBTPageOpaqueInternal nopaque = (XBTPageOpaqueInternal)PageGetSpecialPointer(npage);

			oopaque->btpo_next = nblkno;
			nopaque->btpo_prev = oblkno;
			nopaque->btpo_next = P_NONE; /* redundant */

			/* The last tuple's data have been moved into the next page */
			oopaque->active_count--;
			nopaque->active_count++;
		}

		/*
		 * Write out the old page.	We never need to touch it again, so we can
		 * free the opage workspace too.
		 */
		_xbt_blwritepage((XBTWriteState*)wstate, obuf, oblkno);

		/*
		 * Reset last_off to point to new page
		 */
		last_off = P_FIRSTKEY;
	}

	/*
	 * By here, either original page is still the current page, or a new page
	 * was created that became the current page.  Either way, the current page
	 * definitely has space for new item.
	 *
	 * If the new item is the first for its page, stash a copy for later. Note
	 * this will only happen for the first item on a level; on later pages,
	 * the first item for a page is copied from the prior page in the code
	 * above.  The minimum key for an entire level is nothing more than a
	 * minus infinity (downlink only) pivot tuple placeholder.
	 */
	if (last_off == P_HIKEY)
	{
		Assert(state->btps_lowkey == NULL);
		state->btps_lowkey = CopyIndexTuple(itup);
		/* _xbt_sortaddtup() will perform full truncation later */
		XBTreeTupleSetNAtts(state->btps_lowkey, 0, false); 
	}

	/*
	 * Add the new item into the current page.
	 */
	last_off = OffsetNumberNext(last_off);
	_xbt_sortaddtup(npage, itupsz, itup, last_off, false);

	state->btps_buf = nbuf;
	state->btps_blkno = nblkno;
	state->btps_lastoff = last_off;
}

/*
 * Finish writing out the completed btree.
 */
static void 
_xbt_uppershutdown(XBTWriteState *wstate, XBTPageState *state)
{
	XBTPageState *s = NULL;
	BlockNumber rootblkno = P_NONE;
	uint32 rootlevel = 0;
	BulkWriteBuffer metabuf;

	/*
	 * Each iteration of this loop completes one more level of the tree.
	 */
	for (s = state; s != NULL; s = s->btps_next)
	{
		BlockNumber blkno;
		XBTPageOpaqueInternal opaque;

		blkno = s->btps_blkno;
		opaque = (XBTPageOpaqueInternal)PageGetSpecialPointer((Page)s->btps_buf);

		/*
		 * We have to link the last page on this level to somewhere.
		 *
		 * If we're at the top, it's the root, so attach it to the metapage.
		 * Otherwise, add an entry for it to its parent using its minimum key.
		 * This may cause the last page of the parent level to split, but
		 * that's not a problem -- we haven't gotten to it yet.
		 */
		if (s->btps_next == NULL)
		{
			opaque->btpo_flags |= BTP_ROOT;
			rootblkno = blkno;
			rootlevel = s->btps_level;
		}
		else
		{
			Assert((BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) <=
					IndexRelationGetNumberOfKeyAttributes(wstate->index) &&
					BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) > 0) ||
				   P_LEFTMOST(opaque));
			Assert(BTreeTupleGetNAtts(s->btps_lowkey, wstate->index) == 0 ||
				   !P_LEFTMOST(opaque));
			XBTreeTupleSetDownLink(s->btps_lowkey, blkno);
			_xbt_buildadd(wstate, s->btps_next, s->btps_lowkey);
			pfree(s->btps_lowkey);
			s->btps_lowkey = NULL;
		}

		/*
		 * This is the rightmost page, so the ItemId array needs to be slid
		 * back one slot.  Then we can dump out the page.
		 */
		_xbt_slideleft((Page) s->btps_buf);
		_xbt_blwritepage(wstate, s->btps_buf, s->btps_blkno);
		s->btps_buf = NULL; /* writepage freed the workspace */
	}

	/*
	 * As the last step in the process, construct the metapage and make it
	 * point to the new root (unless we had no data at all, in which case it's
	 * set to point to "P_NONE").  This changes the index to the "valid" state
	 * by filling in a valid magic number in the metapage.
	 */
	// free in function _bt_blwritepage()
	metabuf = smgr_bulk_get_buf(wstate->bulkstate);
	_xbt_initmetapage((Page)metabuf, rootblkno, rootlevel);
	_xbt_blwritepage(wstate, metabuf, BTREE_METAPAGE);
}

/*
 * Read tuples in correct sort order from tuplesort, and load them into
 * btree leaves.
 */
static void _xbt_load(XBTWriteState *wstate, BTSpool *btspool, BTSpool *btspool2)
{
	XBTPageState *state = NULL;
	bool merge = (btspool2 != NULL);
	IndexTuple	itup,
				itup2 = NULL;
	bool load1 = false;
	TupleDesc tupdes = RelationGetDescr(wstate->index);
	int			i,
				keysz = IndexRelationGetNumberOfKeyAttributes(wstate->index);
	SortSupport sort_keys;
	int64		tuples_done = 0;

	wstate->bulkstate = smgr_bulk_start_rel(wstate->index, MAIN_FORKNUM);

	if (merge)
	{
		/*
		 * Another BTSpool for dead tuples exists. Now we have to merge
		 * btspool and btspool2.
		 */
		
		/* the preparation of merge */
		itup = tuplesort_getindextuple(btspool->sortstate, true);
		itup2 = tuplesort_getindextuple(btspool2->sortstate, true);

		/* Prepare SortSupport data for each column */
		sort_keys = (SortSupport) palloc0(keysz * sizeof(SortSupportData));

		for (i = 0; i < keysz; i++)
		{
			SortSupport sort_key = sort_keys + i;
			ScanKey		scan_key = wstate->inskey->scankeys + i;
			int16		strategy;

			sort_key->ssup_cxt = CurrentMemoryContext;
			sort_key->ssup_collation = scan_key->sk_collation;
			sort_key->ssup_nulls_first =
				(scan_key->sk_flags & SK_BT_NULLS_FIRST) != 0;
			sort_key->ssup_attno = scan_key->sk_attno;
			/* Abbreviation is not supported here */
			sort_key->abbreviate = false;

			Assert(sort_key->ssup_attno != 0);

			strategy = (scan_key->sk_flags & SK_BT_DESC) != 0 ?
				BTGreaterStrategyNumber : BTLessStrategyNumber;

			PrepareSortSupportFromIndexRel(wstate->index, strategy, sort_key);
		}

		for (;;)
		{
			load1 = true;		/* load BTSpool next ? */
			if (itup2 == NULL)
			{
				if (itup == NULL)
					break;
			}
			else if (itup != NULL)
			{
				int32		compare = 0;

				for (i = 1; i <= keysz; i++)
				{
					SortSupport entry;
					Datum		attrDatum1,
								attrDatum2;
					bool		isNull1,
								isNull2;

					entry = sort_keys + i - 1;
					attrDatum1 = index_getattr(itup, i, tupdes, &isNull1);
					attrDatum2 = index_getattr(itup2, i, tupdes, &isNull2);

					compare = ApplySortComparator(attrDatum1, isNull1,
												  attrDatum2, isNull2,
												  entry);
					if (compare > 0)
					{
						load1 = false;
						break;
					}
					else if (compare < 0)
						break;
				}

				/*
				 * If key values are equal, we sort on ItemPointer.  This is
				 * required for btree indexes, since heap TID is treated as an
				 * implicit last key attribute in order to ensure that all
				 * keys in the index are physically unique.
				 */
				if (compare == 0)
				{
					compare = ItemPointerCompare(&itup->t_tid, &itup2->t_tid);
					Assert(compare != 0);
					if (compare > 0)
						load1 = false;
				}
			}
			else
				load1 = false;

			/* When we see first tuple, create first index page */
			if (state == NULL)
				state = _xbt_pagestate(wstate, 0);

			if (load1)
			{
				_xbt_buildadd(wstate, state, itup);
				itup = tuplesort_getindextuple(btspool->sortstate, true);
			}
			else
			{
				_xbt_buildadd(wstate, state, itup2);
				itup2 = tuplesort_getindextuple(btspool2->sortstate, true);
			}

			/* Report progress */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}
		pfree(sort_keys);
	}
	else
	{
		/* merge is unnecessary */
		while ((itup = tuplesort_getindextuple(btspool->sortstate,
											   true)) != NULL)
		{
			/* When we see first tuple, create first index page */
			if (state == NULL)
				state = _xbt_pagestate(wstate, 0);

			_xbt_buildadd(wstate, state, itup);

			/* Report progress */
			pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE,
										 ++tuples_done);
		}
	}

	/* Close down final pages and write the metapage */
	_xbt_uppershutdown(wstate, state);
	smgr_bulk_finish(wstate->bulkstate);

	/* initialize recycle queue for xbtree */
	xbt_init_recycle_queue(wstate->index);
}