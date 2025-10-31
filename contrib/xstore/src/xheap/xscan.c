/* -------------------------------------------------------------------------
 *
 * xscan.c
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xscan.c
 * -------------------------------------------------------------------------
 */

#include "c.h"
#include "postgres.h"

#include "pgstat.h"
#include "miscadmin.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/lsyscache.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "xheap/xscan.h"
#include "xheap/xtuple.h"
#include "xheap/xheapam_visibility.h"
#include "xheap/xheap.h"
#include "undo/undorequest.h"
#include "xheap/xheap_am.h"
#include "xheap/xrel.h"
#include "access/syncscan.h"
#include "xheap/xtupleslot.h"
#include "util/xxact.h"

static const int	CACHE_LINE_SZ = 64;
static const int	XHEAP_SCAN_FALLBACK = -1;
static const uint32 BULKSCAN_BLOCKS_PER_BUFFER = 4;

static void
xheap_initscan(XHeapScanDesc scan, ScanKey key, bool is_rescan)
{
	bool		allow_start;
	bool		allow_sync;

	scan->rs_allow_sync = scan->rs_base.rs_flags & SO_ALLOW_SYNC;
	scan->rs_allow_strat = scan->rs_base.rs_flags & SO_ALLOW_STRAT;
	
	/* Disable page-at-a-time mode if it's not a MVCC-safe snapshot. */
	scan->rs_pageatatime = scan->rs_base.rs_snapshot &&
						   IsMVCCSnapshot(scan->rs_base.rs_snapshot) &&
						   (scan->rs_base.rs_flags & SO_ALLOW_PAGEMODE);

	/*
	 * Determine the number of blocks we have to scan.
	 *
	 * It is sufficient to do this once at scan start, since any tuples added
	 * while the scan is in progress will be invisible to my snapshot anyway.
	 * (That is not true when using a non-MVCC snapshot.  However, we couldn't
	 * guarantee to return tuples added after scan start anyway, since they
	 * might go into pages we already scanned.  To guarantee consistent
	 * results for a non-MVCC snapshot, the caller must hold some higher-level
	 * lock that ensures the interesting tuple(s) won't change.)
	 */
	scan->rs_nblocks = RELATION_IS_PG_PARTITION(scan->rs_base.rs_rd)
		? 0
		: RelationGetNumberOfBlocks(scan->rs_base.rs_rd);

	/*
	 * If the table is large relative to NBuffers, use a bulk-read access
	 * strategy and enable synchronized scanning (see syncscan.c).  Although
	 * the thresholds for these features could be different, we make them the
	 * same so that there are only two behaviors to tune rather than four.
	 * (However, some callers need to be able to disable one or both of these
	 * behaviors, independently of the size of the table; also there is a GUC
	 * variable that can disable synchronized scanning.)
	 *
	 * Note that HeapParallelscanInitialize has a very similar test; if you
	 * change this, consider changing that one, too.
	 */
	if (!RelationUsesLocalBuffers(scan->rs_base.rs_rd) &&
		scan->rs_nblocks > (uint32) NBuffers / BULKSCAN_BLOCKS_PER_BUFFER)
	{
		allow_start = scan->rs_allow_strat;
		allow_sync = scan->rs_allow_sync;
	}
	else
		allow_start = allow_sync = false;

	if (allow_start)
	{
		/* During a rescan, keep the previous strategy object. */
		if (scan->rs_strategy == NULL)
			scan->rs_strategy = GetAccessStrategy(BAS_BULKREAD);
	}
	else
	{
		if (scan->rs_strategy != NULL)
			FreeAccessStrategy(scan->rs_strategy);
		scan->rs_strategy = NULL;
	}

	if (is_rescan)
	{
		scan->rs_syncscan = (allow_sync && synchronize_seqscans);
	}
	else if (allow_sync && synchronize_seqscans)
	{
		scan->rs_syncscan = true;
		scan->rs_startblock = ss_get_location(scan->rs_base.rs_rd, scan->rs_nblocks);
	}
	else
	{
		scan->rs_syncscan = false;
		scan->rs_startblock = 0;
	}

	scan->rs_inited = false;
	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;

	if (scan->rs_base.rs_rd->rd_tableam == get_xheapam_table_am_routine())
	{
		scan->lastVar = -1;
		scan->boolArr = NULL;
	}

	/* page-at-a-time fields are always invalid when not rs_inited */

	/*
	 * copy the scan key, if appropriate
	 */
	if (key != NULL)
		memcpy(scan->rs_base.rs_key, key, scan->rs_base.rs_nkeys * sizeof(ScanKeyData));

	/*
	 * Currently, we only have a stats counter for sequential heap scans (but
	 * e.g for bitmap scans the underlying bitmap index scans will be counted,
	 * and for sample scans we update stats for tuple fetches).
	 */
	if (!(scan->rs_base.rs_flags & SO_TYPE_BITMAPSCAN) &&
		!(scan->rs_base.rs_flags & SO_TYPE_SAMPLESCAN))
		pgstat_count_heap_scan(scan->rs_base.rs_rd);
}

TableScanDesc
xheap_beginscan(Relation relation, Snapshot snapshot,
			   ParallelTableScanDesc parallel_scan, uint32 flags)
{
	XHeapScanDesc xscan;

	/*
	 * increment relation ref count while scanning relation
	 *
	 * This is just to make really sure the relcache entry won't go away while
	 * the scan has a pointer to it.  Caller should be holding the rel open
	 * anyway, so this is redundant in all normal scenarios...
	 */
	RelationIncrementReferenceCount(relation);

	xscan = (XHeapScanDesc) palloc0(sizeof(XHeapScanDescData));

	xscan->rs_tupdesc = RelationGetDescr(relation);
	xscan->rs_base.rs_rd = relation;
	xscan->rs_base.rs_snapshot = snapshot;
	xscan->rs_base.rs_nkeys = 0;
	xscan->rs_startblock = 0;
	xscan->rs_ntuples = 0;
	xscan->rs_cutup = NULL;
	xscan->rs_base.rs_parallel = parallel_scan;
	if (xscan->rs_base.rs_parallel != NULL)
		/* For parallel scan, believe whatever ParallelTableScanDesc says. */
		xscan->rs_syncscan = xscan->rs_base.rs_parallel->phs_syncscan;

	xscan->rs_base.rs_key = NULL;
	xscan->rs_base.rs_flags = flags;

	if (parallel_scan != NULL)
		xscan->rs_parallelworkerdata = palloc(sizeof(ParallelBlockTableScanWorkerData));
	else
		xscan->rs_parallelworkerdata = NULL;

	xscan->rs_strategy = NULL;
	xscan->rs_ctupBatch = NULL;

	xheap_initscan(xscan, NULL, false);

	return (TableScanDesc) xscan;
}

void
xheap_end_scan(TableScanDesc scan)
{
	XHeapScanDesc xscan = (XHeapScanDesc) scan;

	/*
     * unpin scan buffers
     */
	if (BufferIsValid(xscan->rs_cbuf))
		ReleaseBuffer(xscan->rs_cbuf);

	RelationDecrementReferenceCount(xscan->rs_base.rs_rd);

	if (xscan->rs_base.rs_key)
	{
		pfree(xscan->rs_base.rs_key);
		xscan->rs_base.rs_key = NULL;
	}

	if (xscan->rs_ctupBatch != NULL)
		pfree(xscan->rs_ctupBatch);

	if (xscan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(xscan->rs_base.rs_snapshot);

	if (xscan->rs_strategy)
		FreeAccessStrategy(xscan->rs_strategy);

	if (xscan->rs_parallelworkerdata)
		pfree(xscan->rs_parallelworkerdata);

	pfree(xscan);
}

void
xheap_rescan(TableScanDesc sscan, ScanKey key)
{
	XHeapScanDesc scan = (XHeapScanDesc) sscan;
	/*
     * unpin scan buffers
     */
	if (BufferIsValid(scan->rs_cbuf))
		ReleaseBuffer(scan->rs_cbuf);

	/*
     * reinitialize scan descriptor
     */
	xheap_initscan(scan, key, true);
}


static void
xheap_get_page_prune(XHeapScanDesc scan, Buffer buffer)
{
	Page				 pg;
	BlockNumber			 blkno;
	bool				 has_pruned = false;
	Size				 freespace = 0;
	XHeapPageHeaderData *xpage = NULL;
	double				 thres = 0;

	pg = BufferGetPage(buffer);
	xpage = (XHeapPageHeaderData *) pg;
	thres = RelationGetTargetPageFreeSpacePrune(scan->rs_base.rs_rd,
													HEAP_DEFAULT_FILLFACTOR);

	if (xpage->potential_freespace >=
			(1.0 - FSM_UPDATE_HEURISTI_PROBABILITY) * (BLCKSZ - thres))
	{
		has_pruned = xheap_page_prune_opt_page(scan->rs_base.rs_rd, buffer,
											  GetTopFullTransactionIdIfAny(), true);
		blkno = BufferGetBlockNumber(buffer);
		freespace = has_pruned ? page_get_xheap_free_space(pg) : 0;

		if (has_pruned)
		{
			RecordPageWithFreeSpace(scan->rs_base.rs_rd, blkno, freespace);
			FreeSpaceMapVacuumRange(scan->rs_base.rs_rd, blkno, blkno + 1);
		}
	}

}

static inline AttrNumber
xheap_check_scandesc(const XHeapScanDesc sscan)
{
	/* lastVar only valid if partial seqscan enabled, scan is initialized, and table is not partition(ed) */
	if ((sscan->lastVar < 0) ||
		(!(sscan->rs_inited)) || (RELATION_IS_PG_PARTITION(sscan->rs_base.rs_rd)))
		return XHEAP_SCAN_FALLBACK;

	return sscan->lastVar;
}


/*
 * xheapgetpage - Same as heapgetpage, but operate on xheap page and
 * in page-at-a-time mode, visible tuples are stored in rs_visibletuples.
 *
 * It returns false, if we can't scan the page, otherwise, return true.
 */
bool
xheapgetpage(XHeapScanDesc scan, BlockNumber page, bool *has_cur_xact_write)
{
	Buffer			buffer = InvalidBuffer;
	AttrNumber		last_var = InvalidAttrNumber;
	bool		   *bool_arr = NULL;
	Page			dp = NULL;
	Snapshot		snapshot = InvalidSnapshot;
	int				lines = 0;
	int				ntup = 0;
	RowPtr		   *next_tup;
	OffsetNumber	lineoff;
	RowPtr		   *lpp;
	XHeapTuple		resulttup;
	ItemPointerData tid;
	bool			valid = false;

	Assert(page < scan->rs_nblocks);

	/* release previous scan buffer, if any */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	/*
     * Be sure to check for interrupts at least once per page.  Checks at
     * higher code levels won't be able to stop a seqscan that encounters many
     * pages' worth of consecutive dead tuples.
     */
	CHECK_FOR_INTERRUPTS();

	/* read page using selected strategy */
	buffer = ReadBufferExtended(scan->rs_base.rs_rd, MAIN_FORKNUM, page, RBM_NORMAL,
								scan->rs_strategy);
	scan->rs_cblock = page;

	xheap_get_page_prune(scan, buffer);

	/*
     * We must hold share lock on the buffer content while examining tuple
     * visibility.  Afterwards, however, the tuples we have found to be
     * visible are guaranteed good as long as we hold the buffer pin.
     */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	last_var = xheap_check_scandesc(scan);
	bool_arr = (last_var > 0) ? scan->boolArr : NULL;

	dp = BufferGetPage(buffer);

	if (!(scan->rs_pageatatime))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		scan->rs_cbuf = buffer;
		return true;
	}

	snapshot = scan->rs_base.rs_snapshot;

	lines = xheap_page_get_max_offset_number(dp);

	for (lineoff = FirstOffsetNumber, lpp = XPageGetRowPtr(dp, lineoff); lineoff <= lines;
		 lineoff++, lpp++)
	{
		if (RowPtrIsNormal(lpp))
		{
			next_tup = lpp + 1;
			__builtin_prefetch(dp + next_tup->offset);
			__builtin_prefetch(dp + next_tup->offset + CACHE_LINE_SZ);
			next_tup++;
			__builtin_prefetch(dp + next_tup->offset);

			ItemPointerSet(&tid, page, lineoff);

			valid = xheap_tuple_fetch(scan->rs_base.rs_rd, buffer, lineoff, snapshot,
									  &resulttup, NULL, false, NULL, NULL, NULL, last_var,
									  bool_arr, has_cur_xact_write);

			if (valid)
				scan->rs_visxtuples[ntup++] = resulttup;
		}
	}

	UnlockReleaseBuffer(buffer);

	Assert(ntup <= CalculatedMaxXHeapTuplesPerPage);

	scan->rs_ntuples = ntup;

	return true;
}

static inline bool
nextxpage(XHeapScanDesc scan, ScanDirection dir, BlockNumber *page)
{
	bool finished = false;
	/*
     * advance to next/prior page and detect end of scan
     */
	if (BackwardScanDirection == dir)
	{
		finished = (*page == scan->rs_startblock);
		if (*page == 0)
			*page = scan->rs_nblocks;

		(*page)--;
	}
	else if (scan->rs_base.rs_parallel != NULL)
	{
		ParallelBlockTableScanDesc pbscan =
			(ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
		ParallelBlockTableScanWorker pbscanwork = scan->rs_parallelworkerdata;

		*page =
			table_block_parallelscan_nextpage(scan->rs_base.rs_rd, pbscanwork, pbscan);
		finished = (*page == InvalidBlockNumber);
	}
	else
	{
		(*page)++;
		if (*page >= scan->rs_nblocks)
			*page = 0;

		finished = (*page == scan->rs_startblock);

		/*
         * Report our new scan position for synchronization purposes. We
         * don't do that when moving backwards, however. That would just
         * mess up any other forward-moving scanners.
         *
         * Note: we do this before checking for end of scan so that the
         * final state of the position hint is back at the start of the
         * rel.  That's not strictly necessary, but otherwise when you run
         * the same query multiple times the starting position would shift
         * a little bit backwards on every invocation, which is confusing.
         * We don't guarantee any specific ordering in general, though.
         */
		if (scan->rs_allow_sync)
			ss_report_location(scan->rs_base.rs_rd, *page);
	}

	return finished;
}


static XHeapTuple
xheapgettup(XHeapScanDesc scan, ScanDirection dir)
{
	XHeapTuple		tuple = scan->rs_cutup;
	Snapshot		snapshot = scan->rs_base.rs_snapshot;
	bool			backward = ScanDirectionIsBackward(dir);
	BlockNumber		page;
	bool			finished;
	bool			valid;
	Page			dp;
	int				lines;
	OffsetNumber	lineoff = InvalidOffsetNumber;
	int				linesleft;
	RowPtr		   *lpp = NULL;

	/*
	 * calculate next starting lineoff, given scan direction
	 */
	if (ScanDirectionIsForward(dir))
	{
		if (!scan->rs_inited)
		{
			/*
			 * return null immediately if relation is empty
			 */
			if (scan->rs_nblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				return NULL;
			}

			if (scan->rs_base.rs_parallel != NULL)
			{
				ParallelBlockTableScanDesc pbscan =
					(ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
				ParallelBlockTableScanWorker pbscanwork = scan->rs_parallelworkerdata;

				table_block_parallelscan_startblock_init(scan->rs_base.rs_rd, pbscanwork,
														 pbscan);

				page = table_block_parallelscan_nextpage(scan->rs_base.rs_rd, pbscanwork,
														 pbscan);

				/* Other processes might have already finished the scan. */
				if (page == InvalidBlockNumber)
				{
					Assert(!BufferIsValid(scan->rs_cbuf));
					tuple = NULL;
					return tuple;
				}
			}
			else
				page = scan->rs_startblock; /* first page */

			valid = xheapgetpage(scan, page, NULL);
			if (!valid)
				goto get_next_page;

			lineoff = FirstOffsetNumber; /* first offnum */
			scan->rs_inited = true;
		}
		else
		{
			/* continue from previously returned page/tuple */
			page = scan->rs_cblock; /* current page */
			if (tuple != NULL)
			{
				lineoff = OffsetNumberNext(
					ItemPointerGetOffsetNumber(&(tuple->ctid))); /* next offnum */
			}
		}

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

		dp = BufferGetPage(scan->rs_cbuf);
		lines = xheap_page_get_max_offset_number(dp);
		/* page and lineoff now reference the physically next tid */

		linesleft = lines - lineoff + 1;
	}
	else if (backward)
	{
		/* backward parallel scan not supported */
		Assert(scan->rs_base.rs_parallel == NULL);

		/* backward parallel scan not supported */
		if (!scan->rs_inited)
		{
			/*
			 * return null immediately if relation is empty
			 */
			if (scan->rs_nblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				return NULL;
			}

			/*
			 * Disable reporting to syncscan logic in a backwards scan; it's
			 * not very likely anyone else is doing the same thing at the same
			 * time, and much more likely that we'll just bollix things for
			 * forward scanners.
			 */
			scan->rs_allow_sync = false;
			/* start from last page of the scan */
			if (scan->rs_startblock > 0)
				page = scan->rs_startblock - 1;
			else
				page = scan->rs_nblocks - 1;
			valid = xheapgetpage(scan, page, NULL);
			if (!valid)
				goto get_next_page;

		}
		else
		{
			/* continue from previously returned page/tuple */
			page = scan->rs_cblock; /* current page */
		}

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

		dp = BufferGetPage(scan->rs_cbuf);

		lines = xheap_page_get_max_offset_number(dp);

		if (!scan->rs_inited)
		{
			lineoff = lines; /* final offnum */
			scan->rs_inited = true;
		}
		else
		{
			if (tuple != NULL)
				lineoff = OffsetNumberPrev(
					ItemPointerGetOffsetNumber(&(tuple->ctid))); /* previous offnum */

		}
		/* page and lineoff now reference the physically previous tid */

		linesleft = lineoff;
	}
	else
	{
		if (!scan->rs_inited || (tuple == NULL))
		{
			Assert(!BufferIsValid(scan->rs_cbuf));
			tuple = NULL;
			return tuple;
		}

		page = ItemPointerGetBlockNumber(&(tuple->ctid));
		valid = xheapgetpage(scan, page, NULL);
		if (!valid)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("Can not refetch prior page")));
			tuple = NULL;
			return tuple;
		}

		lineoff = ItemPointerGetOffsetNumber(&(tuple->ctid));
		/* Since the tuple was previously fetched, needn't lock page here */
		tuple = scan->rs_visxtuples[lineoff];
		return tuple;
	}

	/*
	 * advance the scan until we find a qualifying tuple or run out of stuff
	 * to scan
	 */
	if (lineoff > 0 && lineoff <= lines)
		lpp = XPageGetRowPtr(dp, lineoff);

get_next_tuple:
	while (linesleft > 0)
	{
		if (RowPtrIsNormal(lpp))
		{
			tuple = NULL;

			valid =
				xheap_tuple_fetch(scan->rs_base.rs_rd, scan->rs_cbuf, lineoff, snapshot,
								  &tuple, NULL, false, NULL, NULL, NULL, -1, NULL, NULL);

			if (valid)
			{
				LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
				return tuple;
			}
		}

		/*
		 * otherwise move to the next item on the page
		 */
		--linesleft;
		if (backward)
		{
			--lpp; /* move back in this page's ItemId array */
			--lineoff;
		}
		else
		{
			++lpp; /* move forward in this page's ItemId array */
			++lineoff;
		}
	}

	/*
	 * if we get here, it means we've exhausted the items on this page and
	 * it's time to move to the next.
	 */
	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

get_next_page:
	for (;;)
	{
		finished = nextxpage(scan, dir, &page);
		/* return NULL if we've exhausted all the pages */
		if (finished)
		{
			if (BufferIsValid(scan->rs_cbuf))
				ReleaseBuffer(scan->rs_cbuf);
			scan->rs_cbuf = InvalidBuffer;
			scan->rs_cblock = InvalidBlockNumber;
			scan->rs_inited = false;
			return NULL;
		}

		valid = xheapgetpage(scan, page, NULL);
		if (!valid)
			continue;

		if (!scan->rs_inited)
			scan->rs_inited = true;

		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

		dp = BufferGetPage(scan->rs_cbuf);
		lines = xheap_page_get_max_offset_number((Page) dp);
		linesleft = lines;
		if (backward)
		{
			lineoff = lines;
			lpp = XPageGetRowPtr(dp, lines);
		}
		else
		{
			lineoff = FirstOffsetNumber;
			lpp = XPageGetRowPtr(dp, FirstOffsetNumber);
		}

		goto get_next_tuple;
	}
}


/* ----------------
 * xheapgettup_pagemode - fetch next xheap tuple in page-at-a-time mode
 * ----------------
 */
XHeapTuple
xheapgettuple_pagemode(XHeapScanDesc scan, ScanDirection dir, bool *has_cur_xact_write)
{
	XHeapTuple	tuple = scan->rs_cutup;
	bool		backward = ScanDirectionIsBackward(dir);
	BlockNumber page;
	bool		finished;
	bool		valid;
	int			lines;
	int			lineindex;
	int			linesleft;
	int			i = 0;

	/*
     * calculate next starting lineindex, given scan direction
     */
	if (ScanDirectionIsForward(dir))
	{
		if (!scan->rs_inited)
		{
			/*
             * return null immediately if relation is empty
             */
			if (scan->rs_nblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				tuple = NULL;
				return tuple;
			}
			if (scan->rs_base.rs_parallel != NULL)
			{
				ParallelBlockTableScanDesc pbscan =
					(ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;
				ParallelBlockTableScanWorker pbscanwork = scan->rs_parallelworkerdata;
				table_block_parallelscan_startblock_init(scan->rs_base.rs_rd, pbscanwork,
														 pbscan);
				page = table_block_parallelscan_nextpage(scan->rs_base.rs_rd, pbscanwork,
														 pbscan);

				/* Other processes might have already finished the scan. */
				if (page == InvalidBlockNumber)
				{
					Assert(!BufferIsValid(scan->rs_cbuf));
					tuple = NULL;
					return tuple;
				}
			}
			else
			{
				page = scan->rs_startblock; /* first page */
			}

			valid = xheapgetpage(scan, page, has_cur_xact_write);
			if (!valid)
			{
				goto get_next_page;
			}

			lineindex = 0;
			scan->rs_inited = true;
		}
		else
		{
			/* continue from previously returned page/tuple */
			page = scan->rs_cblock; /* current page */
			lineindex = scan->rs_cindex + 1;
		}

		lines = scan->rs_ntuples;
		linesleft = lines - lineindex;
	}
	else if (backward)
	{
		/* backward parallel scan not supported */
		Assert(scan->rs_base.rs_parallel == NULL);

		if (!scan->rs_inited)
		{
			/*
             * return null immediately if relation is empty
             */
			if (scan->rs_nblocks == 0)
			{
				Assert(!BufferIsValid(scan->rs_cbuf));
				tuple = NULL;
				return tuple;
			}

			/*
             * Disable reporting to syncscan logic in a backwards scan; it's
             * not very likely anyone else is doing the same thing at the same
             * time, and much more likely that we'll just bollix things for
             * forward scanners.
             */
			scan->rs_allow_sync = false;
			/* start from last page of the scan */
			if (scan->rs_startblock > 0)
			{
				page = scan->rs_startblock - 1;
			}
			else
			{
				page = scan->rs_nblocks - 1;
			}
			valid = xheapgetpage(scan, page, has_cur_xact_write);
			if (!valid)
			{
				goto get_next_page;
			}
		}
		else
		{
			/* continue from previously returned page/tuple */
			page = scan->rs_cblock; /* current page */
		}

		lines = scan->rs_ntuples;

		if (!scan->rs_inited)
		{
			lineindex = lines - 1;
			scan->rs_inited = true;
		}
		else
		{
			lineindex = scan->rs_cindex - 1;
		}
		/* page and lineindex now reference the previous visible tid */

		linesleft = lineindex + 1;
	}
	else
	{
		if (!scan->rs_inited || (tuple == NULL))
		{
			Assert(!BufferIsValid(scan->rs_cbuf));
			tuple = NULL;
			return tuple;
		}

		page = ItemPointerGetBlockNumber(&(tuple->ctid));
		valid = xheapgetpage(scan, page, NULL);
		if (!valid)
		{
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("Can not refetch prior page")));
			tuple = NULL;
			return tuple;
		}

		tuple = scan->rs_visxtuples[scan->rs_cindex];
		return tuple;
	}

get_next_tuple:

	/*
     * advance the scan until we find a qualifying tuple or run out of stuff
     * to scan
     */
	if (linesleft > 0)
	{
		tuple = scan->rs_visxtuples[lineindex];
		scan->rs_cindex = lineindex;
		return tuple;
	}

	/*
     * if we get here, it means we've exhausted the items on this page and
     * it's time to move to the next. 
     */
	for (i = 0; i < scan->rs_ntuples; i++)
		pfree(scan->rs_visxtuples[i]);
	scan->rs_ntuples = 0;
	scan->rs_cutup = NULL;

get_next_page:
	for (;;)
	{
		finished = nextxpage(scan, dir, &page);
		if (finished)
		{
			if (BufferIsValid(scan->rs_cbuf))
				ReleaseBuffer(scan->rs_cbuf);
			scan->rs_cbuf = InvalidBuffer;
			scan->rs_cblock = InvalidBlockNumber;
			tuple = NULL;
			scan->rs_inited = false;
			return tuple;
		}

		valid = xheapgetpage(scan, page, has_cur_xact_write);
		if (!valid)
		{
			continue;
		}

		if (!scan->rs_inited)
			scan->rs_inited = true;
		lines = scan->rs_ntuples;
		linesleft = lines;
		if (backward)
		{
			lineindex = lines - 1;
		}
		else
		{
			lineindex = 0;
		}

		goto get_next_tuple;
	}
}

XHeapTuple
xheap_getnext(TableScanDesc sscan, ScanDirection dir, bool *has_cur_xact_write)
{
	XHeapScanDesc scan = (XHeapScanDesc) sscan;
	XHeapTuple	  xhtup = NULL;

	Assert(scan->rs_base.rs_key == NULL);

	if (scan->rs_pageatatime)
	{
		xhtup = xheapgettuple_pagemode(scan, dir, has_cur_xact_write);
	}
	else
	{
		xhtup = xheapgettup(scan, dir);
	}

	if (xhtup == NULL)
	{
		return NULL;
	}

	scan->rs_cutup = xhtup;

	pgstat_count_heap_getnext(scan->rs_base.rs_rd);

	return scan->rs_cutup;
}

/*
 * xheap_search_buffer - search tuple satisfying snapshot
 *
 * On entry, *tid is the TID of a tuple, and buffer is the buffer holding
 * this tuple.  We search for the first visible member satisfying the given
 * snapshot. If one is found, we return the tuple, in addition to updating
 * *tid. Return NULL otherwise.
 *
 * The caller must already have pin and (at least) share lock on the buffer;
 * it is still pinned/locked at exit.  Also, We do not report any pgstats
 * count; caller may do so if wanted.
 */
XHeapTuple
xheap_search_buffer(ItemPointer tid, Relation relation, Buffer buffer, Snapshot snapshot,
				  bool *all_dead, XHeapTuple freebuf, bool *has_cur_xact_write)
{
	Page				dp = (Page) BufferGetPage(buffer);
	XHeapTuple			pagetup = NULL;
	XHeapTuple			resulttup = NULL;
	OffsetNumber		offnum = InvalidOffsetNumber;
	RowPtr			   *lp = NULL;
	XHeapTupleTransInfo tdinfo;
	bool				got_xinfo = false;

	if (all_dead)
		*all_dead = false;

	Assert(ItemPointerGetBlockNumber(tid) == BufferGetBlockNumber(buffer));
	offnum = ItemPointerGetOffsetNumber(tid);
	/* check for bogus TID */
	if (offnum < FirstOffsetNumber || offnum > xheap_page_get_max_offset_number(dp))
		return NULL;

	lp = XPageGetRowPtr(dp, offnum);

	/* check for unused items */
	if (!RowPtrIsNormal(lp) )
	{
		if (all_dead)
			*all_dead = true;

		return NULL;
	}

	pagetup = xheap_get_tuple(relation, buffer, offnum, freebuf);

	/*
     * If the record is deleted, its place in the page might have been taken
     * by another of its kind. Try to get it from the UNDO if it is still
     * visible.
     */
	xheap_tuple_fetch(relation, buffer, offnum, snapshot, &resulttup, NULL, false,
						  &tdinfo, &got_xinfo, &pagetup, -1, NULL, has_cur_xact_write);

	if (resulttup)
		/* set the tid */
		*tid = resulttup->ctid;
	else
	{
		/*
         * If we can't see it, maybe no one else can either.  At caller
         * request, check whether tuple is dead to all transactions.
         * This should be a quick check because we grabbed the TD information
         * from xheap_tuple_fetch already.
         */
		if (all_dead)
			*all_dead =
				xheap_tuple_is_surely_dead(pagetup, buffer, offnum, &tdinfo, got_xinfo);
	}

	/*
     * pagetup can be reused as resulttup, we cannot easily free pagetup here.
     */
	if (!freebuf && pagetup && resulttup != pagetup)
		pfree(pagetup);
	return resulttup;
}

static Buffer
xheap_index_build_next_block(XHeapScanDesc scan)
{
	BlockNumber blkno = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;

	/* xstore don't support parallel index build now */
	Assert(scan->rs_base.rs_parallel == NULL);

	if (scan->rs_cblock == InvalidBlockNumber)
	{
		/* first page, init rs_visxtuples array and other information */
		blkno = 0;
		scan->rs_ntuples = 0;
	}
	else
		blkno = scan->rs_cblock + 1;

	for (int i = 0; i < scan->rs_ntuples; i++)
		pfree(scan->rs_visxtuples[i]);

	scan->rs_ntuples = 0;
	scan->rs_cutup = NULL;

	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	if (blkno >= scan->rs_nblocks)
		return InvalidBuffer; /* we are done */

	scan->rs_cblock = blkno;

	/* read the next page, and lock with exclusive mode */
	buf = ReadBufferExtended(scan->rs_base.rs_rd, MAIN_FORKNUM, blkno, RBM_NORMAL,
							 scan->rs_strategy);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	scan->rs_cbuf = buf;
	return buf;
}

static bool
xheap_index_build_next_page(XHeapScanDesc scan)
{
	Buffer		   buf = xheap_index_build_next_block(scan);
	Page		   page = NULL;
	FullTransactionId  xid = InvalidFullTransactionId;
	RowPtr		  *rp = NULL;
	XHeapTuple	   tuple = NULL;
	uint16		   infomask = 0;

	OffsetNumber offnum = InvalidOffsetNumber;

	int			 ntup = 0;
	OffsetNumber maxoff = InvalidOffsetNumber;

	if (!BufferIsValid(buf))
	{
		return false;
	}

	page = BufferGetPage(buf);

	maxoff = xheap_page_get_max_offset_number(page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
	{
		XHeapDiskTuple disk_tuple;
		FullTransactionId tuple_xid;

		rp = XPageGetRowPtr(page, offnum);
		if (!RowPtrIsNormal(rp))
		{
			continue; /* deleted or unused is not visible here */
		}

		disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);
		tuple_xid = disk_tuple->modified_xid;

		if (!FullTransactionIdIsValid(tuple_xid) || TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(tuple_xid)) ||
			xstore_transaction_id_did_commit(tuple_xid))
		{
			continue; /* xid visible in SnapshotSelfTransaction */
		}

		if (FullTransactionIdIsValid(tuple_xid) &&
			!IS_VALID_UNDO_REC_PTR(disk_tuple->urec))
		{
			ereport(
				PANIC,
				(errcode(/*MOD_XSTORE*/ ERRCODE_DATA_CORRUPTED),
				 errmsg("Xid %lu on page %u is valid ,but urp %lu is invalid",
						tuple_xid.value, BufferGetBlockNumber(buf),
						disk_tuple->urec)));
		}

		/* This tuple is aborted */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		execute_undo_actions_tuple(disk_tuple->urec, scan->rs_base.rs_rd, buf, tuple_xid);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	}

	xid = GetCurrentFullTransactionId();
	xheap_page_prune_fsm(scan->rs_base.rs_rd, buf, xid, page, BufferGetBlockNumber(buf));

	maxoff = xheap_page_get_max_offset_number(page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
	{
		rp = XPageGetRowPtr(page, offnum);
		if (!RowPtrIsNormal(rp))
		{
			continue;
		}

		tuple = (XHeapTuple) xheaptup_alloc(XHeapTupleDataSize);
		tuple->disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);

		infomask = tuple->disk_tuple->flag;
		if ((infomask & (XHEAP_UPDATED | XHEAP_DELETED)) != 0)
		{
			pfree(tuple);
			continue;
		}

		tuple->table_oid = RelationGetRelid(scan->rs_base.rs_rd);
		tuple->disk_tuple_size = RowPtrGetLen(rp);
		ItemPointerSet(&tuple->ctid, BufferGetBlockNumber(buf), offnum);

		scan->rs_visxtuples[ntup++] = tuple;
	}
	scan->rs_ntuples = ntup;
	scan->rs_cindex = 0;

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	return true;
}

XHeapTuple
xheap_index_build_get_next_tuple(XHeapScanDesc scan, TupleTableSlot *slot)
{
	int		   lineindex = 0;
	XHeapTuple tuple = NULL;

	if (scan->rs_base.rs_snapshot->snapshot_type /*satisfies*/ != SNAPSHOT_SELF_TRANSACTION)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("We must use SnapshotSelfTransaction to build a xstore index.")));

	while (scan->rs_cblock == InvalidBlockNumber || scan->rs_cindex >= scan->rs_ntuples)
	{
		if (!xheap_index_build_next_page(scan))
		{
			ExecClearTuple(slot);
			return NULL;
		}
	}
	lineindex = scan->rs_cindex;
	tuple = scan->rs_visxtuples[lineindex];
	scan->rs_cindex++; /* now rs_cindex indicate the next tuple's index */
	scan->rs_cutup = tuple;

	/* Let scan->rs_visxtuples to release the tuple */
	xheap_slot_store_xheap_tuple(scan->rs_cutup, slot, false, false);
	return tuple;
}


static void
xheap_scan_pages_for_batch_mode(XHeapScanDesc scan, int lineIndex)
{
	int			  lines, rows = 0;
	XHeapScanDesc uscan = (XHeapScanDesc) scan;
	lines = scan->rs_ntuples;

	while (lineIndex < lines)
	{
		scan->rs_ctupBatch[rows++] = uscan->rs_visxtuples[lineIndex];
		if (rows == scan->rs_max_scan_rows)
			break;

		lineIndex++;
	}

	scan->rs_cindex = lineIndex;
	scan->rs_ctup_rows = rows;
}

bool
xheap_get_tuple_page_batch_mode(XHeapScanDesc scan, ScanDirection dir)
{
	BlockNumber page;

	int	 line_index;
	bool finished = false;

	scan->rs_ctup_rows = 0;

	/* calculate next starting lineindex, given scan direction */
	if (!scan->rs_inited)
	{
		/* return null immediately if relation is empty */
		if (scan->rs_nblocks == 0)
		{
			Assert(!BufferIsValid(scan->rs_cbuf));
			scan->rs_ctup_rows = 0;
			return true;
		}
		page = scan->rs_startblock;
		xheapgetpage(scan, page, NULL);
		line_index = 0;
		scan->rs_inited = true;
	}
	else
	{
		/* continue from previously returned page/tuple */
		page = scan->rs_cblock;
		line_index = scan->rs_cindex + 1;
	}

	for (;;)
	{
		if (line_index < scan->rs_ntuples)
		{
			xheap_scan_pages_for_batch_mode(scan, line_index);
			break;
		}

		finished = nextxpage(scan, dir, &page);
		/* return NULL if we've exhausted all the pages */
		if (finished)
		{
			scan->rs_cbuf = InvalidBuffer;
			scan->rs_cblock = InvalidBlockNumber;
			scan->rs_inited = false;
			scan->rs_ctup_rows = 0;
			return true;
		}
		else
		{
			if (unlikely(!xheapgetpage(scan, page, NULL)))
			{
				continue;
			}
			line_index = 0;
		}
	}

	return false;
}
