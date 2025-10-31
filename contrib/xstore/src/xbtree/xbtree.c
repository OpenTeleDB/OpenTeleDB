/* -------------------------------------------------------------------------
 *
 * xbtree.c
 * Implementation of Lehman and Yao's btree management algorithm for
 * teledb.
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * src/xbtree/xbtree.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/htup.h"
#include "access/nbtree.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opfamily.h"
#include "postgres_ext.h"
#include "utils/relcache.h"
#include "xbtree/xbtree.h"
#include "xbtree/xbttup.h"
#include "xstore.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/table.h"
#include "nodes/nodes.h"
#include "access/xloginsert.h"
#include "nodes/execnodes.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "storage/block.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/backend_progress.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/index_selfuncs.h"
#include <string.h>

static void xbtvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state,
			 BTCycleId cycleid);
static void xbtvacuumpage(BTVacState *vstate, BlockNumber scanblkno);

#define FREE_POINTER(ptr)      \
	do {                       \
		if ((ptr) != NULL) {   \
			pfree((void*)ptr); \
			(ptr) = NULL;	   \
		}                      \
	} while (0)

/*
 *	xbtbuildempty() -- build an empty xbtree index in the initialization fork
 */
void
xbtbuildempty(Relation index)
{
	BulkWriteState *bulkstate;
	BulkWriteBuffer metabuf;

	bulkstate = smgr_bulk_start_rel(index, INIT_FORKNUM);

	/* Construct metapage. */
	metabuf = smgr_bulk_get_buf(bulkstate);
	_xbt_initmetapage((Page)metabuf, P_NONE, 0);
	smgr_bulk_write(bulkstate, BTREE_METAPAGE, metabuf, true);

	smgr_bulk_finish(bulkstate);
}

bool
xbtdelete(Relation rel, Datum *values, bool *isnull, ItemPointer heap_tid, bool is_dead)
{
	bool	   ret;
	IndexTuple itup;
	Assert(relation_is_xstore_index(rel));
	itup = index_form_tuple(RelationGetDescr(rel), values, isnull);
	itup->t_tid = *heap_tid;
	ret = _xbt_dodelete(rel, itup, is_dead);
	pfree(itup);
	return ret;
}

/*
 *	btinsert() -- insert an index tuple into a btree.
 *
 *		Descend the tree recursively, find the appropriate location for our
 *		new tuple, and put it there.
 */
bool
xbtinsert(Relation rel, Datum *values, bool *isnull, ItemPointer ht_ctid,
		  Relation heapRel, IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	bool	   result = false;
	IndexTuple itup;
	Size	   newsize;

	/* generate an index tuple */
	itup = index_form_tuple(RelationGetDescr(rel), values, isnull);
	itup->t_tid = *ht_ctid;

	/* reserve space for xbtree undo */
	newsize = IndexTupleSize(itup) + sizeof(FullTransactionId) + sizeof(UndoRecPtr);
	newsize = MAXALIGN(newsize);
	IndexTupleSetSize(itup, newsize);

	result = _xbt_doinsert(rel, itup, checkUnique, heapRel);

	pfree(itup);

	return result;
}

/*
 *	xbtgettuple() -- Get the next tuple in the scan.
 */
bool
xbtgettuple(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		 res = false;

	/* btree indexes are never lossy */
	scan->xs_recheck = false;

	/*
	 * If we have any array keys, initialize them during first call for a
	 * scan.  We can't do this in btrescan because we don't know the scan
	 * direction at that time.
	 */
	if (so->numArrayKeys && !BTScanPosIsValid(so->currPos))
	{
		/* punt if we have any unsatisfiable array keys */
		if (so->numArrayKeys < 0)
			return false;

		_bt_start_array_keys(scan, dir);
	}

	/* This loop handles advancing to the next array elements, if any */
	do
	{
		/*
		 * If we've already initialized this scan, we can just advance it in
		 * the appropriate direction.  If we haven't done so yet, we call
		 * _xbt_first() to get the first item in the scan.
		 */
		if (!BTScanPosIsValid(so->currPos))
			res = _xbt_first(scan, dir);
		else
		{
			/*
			 * Check to see if we should kill the previously-fetched tuple.
			 */
			if (scan->kill_prior_tuple)
			{
				/*
				 * Yes, remember it for later. (We'll deal with all such
				 * tuples at once right before leaving the index page.)  The
				 * test for numKilled overrun is not just paranoia: if the
				 * caller reverses direction in the indexscan then the same
				 * item might get entered multiple times. It's not worth
				 * trying to optimize that, so we don't detect it, but instead
				 * just forget any excess entries.
				 */
				if (so->killedItems == NULL)
					so->killedItems = (int *) palloc(MaxIndexTuplesPerPage * sizeof(int));
				if (so->numKilled < MaxIndexTuplesPerPage)
					so->killedItems[so->numKilled++] = so->currPos.itemIndex;
			}

			/*
			 * Now continue the scan.
			 */
			res = _xbt_next(scan, dir);
		}

		/* If we have a tuple, return it ... */
		if (res)
			break;
		/* ... otherwise see if we have more array keys to deal with */
	} while (so->numArrayKeys && _bt_start_prim_scan(scan, dir));

	return res;
}

/*
 * xbtgetbitmap() -- gets all matching tuples, and adds them to a bitmap
 */
int64
xbtgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int64		 ntids = 0;
	ItemPointer	 heapTid;

	/* This loop handles advancing to the next array elements, if any */
	do
	{
		/* Fetch the first page & tuple */
		if (_xbt_first(scan, ForwardScanDirection))
		{
			/* Save tuple ID, and continue scanning */
			heapTid = &scan->xs_heaptid;
			tbm_add_tuples(tbm, heapTid, 1, false);
			ntids++;

			for (;;)
			{
				/*
				 * Advance to next tuple within page.  This is the same as the
				 * easy case in _xbt_next().
				 */
				if (++so->currPos.itemIndex > so->currPos.lastItem)
				{
					/* let _bt_next do the heavy lifting */
					if (!_xbt_next(scan, ForwardScanDirection))
						break;
				}

				/* Save tuple ID, and continue scanning */
				heapTid = &so->currPos.items[so->currPos.itemIndex].heapTid;
				tbm_add_tuples(tbm, heapTid, 1, false);
				ntids++;
			}
		}
		/* Now see if we have more array keys to deal with */
	} while (so->numArrayKeys && _bt_start_prim_scan(scan, ForwardScanDirection));

	return ntids;
}

/*
 *	btbeginscan() -- start a scan on a btree index
 */
IndexScanDesc
xbtbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	BTScanOpaque  so;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	/* get the scan */
	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* allocate private workspace */
	so = (BTScanOpaque) palloc(sizeof(BTScanOpaqueData));
	BTScanPosInvalidate(so->currPos);
	BTScanPosInvalidate(so->markPos);
	if (scan->numberOfKeys > 0)
		so->keyData = (ScanKey) palloc(scan->numberOfKeys * sizeof(ScanKeyData));
	else
		so->keyData = NULL;

	so->needPrimScan = false;
	so->scanBehind = false;
	so->arrayKeys = NULL;
	so->orderProcs = NULL;
	so->arrayContext = NULL;

	so->killedItems = NULL;		/* until needed */
	so->numKilled = 0;

	/*
	 * We don't know yet whether the scan will be index-only, so we do not
	 * allocate the tuple workspace arrays until btrescan.  However, we set up
	 * scan->xs_itupdesc whether we'll need it or not, since that's so cheap.
	 */
	so->currTuples = so->markTuples = NULL;

	scan->xs_itupdesc = RelationGetDescr(rel);

	scan->opaque = so;

	return scan;
}

/*
 *	xbtrescan() -- rescan an index relation
 */
void
xbtrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys, ScanKey orderbys,
		  int norderbys)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (BTScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_bt_killitems(scan);
		BTScanPosUnpinIfPinned(so->currPos);
		BTScanPosInvalidate(so->currPos);
	}

	so->markItemIndex = -1;
	so->needPrimScan = false;
	so->scanBehind = false;
	BTScanPosUnpinIfPinned(so->markPos);
	BTScanPosInvalidate(so->markPos);

	/*
	 * Allocate tuple workspace arrays, if needed for an index-only scan and
	 * not already done in a previous rescan call.  To save on palloc
	 * overhead, both workspaces are allocated as one palloc block; only this
	 * function and btendscan know that.
	 *
	 * NOTE: this data structure also makes it safe to return data from a
	 * "name" column, even though btree name_ops uses an underlying storage
	 * datatype of cstring.  The risk there is that "name" is supposed to be
	 * padded to NAMEDATALEN, but the actual index tuple is probably shorter.
	 * However, since we only return data out of tuples sitting in the
	 * currTuples array, a fetch of NAMEDATALEN bytes can at worst pull some
	 * data out of the markTuples array --- running off the end of memory for
	 * a SIGSEGV is not possible.  Yeah, this is ugly as sin, but it beats
	 * adding special-case treatment for name_ops elsewhere.
	 */
	if (scan->xs_want_itup && so->currTuples == NULL)
	{
		so->currTuples = (char *) palloc(BLCKSZ * 2);
		so->markTuples = so->currTuples + BLCKSZ;
	}

	/*
	 * Reset the scan keys
	 */
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	so->numberOfKeys = 0;		/* until _bt_preprocess_keys sets it */
	so->numArrayKeys = 0;		/* ditto */
}

/*
 *	btendscan() -- close down a scan
 */
void
xbtendscan(IndexScanDesc scan)
{
BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (BTScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_bt_killitems(scan);
		BTScanPosUnpinIfPinned(so->currPos);
	}

	so->markItemIndex = -1;
	BTScanPosUnpinIfPinned(so->markPos);

	/* No need to invalidate positions, the RAM is about to be freed. */

	/* Release storage */
	if (so->keyData != NULL)
		pfree(so->keyData);
	/* so->arrayKeys and so->orderProcs are in arrayContext */
	if (so->arrayContext != NULL)
		MemoryContextDelete(so->arrayContext);
	if (so->killedItems != NULL)
		pfree(so->killedItems);
	if (so->currTuples != NULL)
		pfree(so->currTuples);
	/* so->markTuples should not be pfree'd, see btrescan */
	pfree(so);
}

/*
 *	btmarkpos() -- save current scan position
 */
void
xbtmarkpos(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* There may be an old mark with a pin (but no lock). */
	BTScanPosUnpinIfPinned(so->markPos);

	/*
	 * Just record the current itemIndex.  If we later step to next page
	 * before releasing the marked position, _bt_steppage makes a full copy of
	 * the currPos struct in markPos.  If (as often happens) the mark is moved
	 * before we leave the page, we don't have to do that work.
	 */
	if (BTScanPosIsValid(so->currPos))
		so->markItemIndex = so->currPos.itemIndex;
	else
	{
		BTScanPosInvalidate(so->markPos);
		so->markItemIndex = -1;
	}
}

/*
 *	btrestrpos() -- restore scan to last saved position
 */
void
xbtrestrpos(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	if (so->markItemIndex >= 0)
	{
		/*
		 * The scan has never moved to a new page since the last mark.  Just
		 * restore the itemIndex.
		 *
		 * NB: In this case we can't count on anything in so->markPos to be
		 * accurate.
		 */
		so->currPos.itemIndex = so->markItemIndex;
	}
	else
	{
		/*
		 * The scan moved to a new page after last mark or restore, and we are
		 * now restoring to the marked page.  We aren't holding any read
		 * locks, but if we're still holding the pin for the current position,
		 * we must drop it.
		 */
		if (BTScanPosIsValid(so->currPos))
		{
			/* Before leaving current page, deal with any killed items */
			if (so->numKilled > 0)
				_bt_killitems(scan);
			BTScanPosUnpinIfPinned(so->currPos);
		}

		if (BTScanPosIsValid(so->markPos))
		{
			/* bump pin on mark buffer for assignment to current buffer */
			if (BTScanPosIsPinned(so->markPos))
				IncrBufferRefCount(so->markPos.buf);
			memcpy(&so->currPos, &so->markPos,
				   offsetof(BTScanPosData, items[1]) +
				   so->markPos.lastItem * sizeof(BTScanPosItem));
			if (so->currTuples)
				memcpy(so->currTuples, so->markTuples,
					   so->markPos.nextTupleOffset);
			/* Reset the scan's array keys (see _bt_steppage for why) */
			if (so->numArrayKeys)
			{
				_bt_start_array_keys(scan, so->currPos.dir);
				so->needPrimScan = false;
			}
		}
		else
			BTScanPosInvalidate(so->currPos);
	}
}

/*
 * XBTPARALLEL_NOT_INITIALIZED indicates that the scan has not started.
 *
 * XBTPARALLEL_ADVANCING indicates that some process is advancing the scan to
 * a new page; others must wait.
 *
 * XBTPARALLEL_IDLE indicates that no backend is currently advancing the scan
 * to a new page; some process can start doing that.
 *
 * XBTPARALLEL_DONE indicates that the scan is complete (including error exit).
 * We reach this state once for every distinct combination of array keys.
 */
typedef enum
{
	XBTPARALLEL_NOT_INITIALIZED,
	XBTPARALLEL_ADVANCING,
	XBTPARALLEL_IDLE,
	XBTPARALLEL_DONE
} XBTPS_State;

/*
 * BTParallelScanDescData contains btree specific shared information required
 * for parallel scan.
 */
typedef struct XBTParallelScanDescData
{
	BlockNumber btps_scanPage;	/* latest or next page to be scanned */
	XBTPS_State	btps_pageStatus;	/* indicates whether next page is
									 * available for scan. see above for
									 * possible states of parallel scan. */
	slock_t		btps_mutex;		/* protects above variables */
	
	ConditionVariable btps_cv;	/* used to synchronize parallel scan */
	/*
	 * btps_arrElems is used when scans need to schedule another primitive
	 * index scan.  Holds BTArrayKeyInfo.cur_elem offsets for scan keys.
	 */
	int			btps_arrElems[FLEXIBLE_ARRAY_MEMBER];
}XBTParallelScanDescData;

typedef struct XBTParallelScanDescData *XBTParallelScanDesc;

/*
 * xbtestimateparallelscan -- estimate storage for BTParallelScanDescData
 */
Size
xbtestimateparallelscan(int nkeys, int norderbys)
{
	/* Pessimistically assume all input scankeys will be output with arrays */
	return offsetof(XBTParallelScanDescData, btps_arrElems) + sizeof(int) * nkeys;
}

/*
 * xbtinitparallelscan -- initialize XBTParallelScanDesc for parallel btree scan
 */
void
xbtinitparallelscan(void *target)
{
	XBTParallelScanDesc bt_target = (XBTParallelScanDesc) target;

	SpinLockInit(&bt_target->btps_mutex);
	bt_target->btps_scanPage = InvalidBlockNumber;
	bt_target->btps_pageStatus = XBTPARALLEL_NOT_INITIALIZED;
	ConditionVariableInit(&bt_target->btps_cv);
}

/*
 *	xbtparallelrescan() -- reset parallel scan
 */
void
xbtparallelrescan(IndexScanDesc scan)
{
	XBTParallelScanDesc btscan;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;

	Assert(parallel_scan);

	btscan = (XBTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	/*
	 * In theory, we don't need to acquire the spinlock here, because there
	 * shouldn't be any other workers running at this point, but we do so for
	 * consistency.
	 */
	SpinLockAcquire(&btscan->btps_mutex);
	btscan->btps_scanPage = InvalidBlockNumber;
	btscan->btps_pageStatus = XBTPARALLEL_NOT_INITIALIZED;
	SpinLockRelease(&btscan->btps_mutex);
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
xbtbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			  IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation  rel = info->index;
	BTCycleId cycleid;

	/* allocate stats if first time through, else re-use existing struct */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Establish the vacuum cycle ID to use for this scan */
	/* The ENSURE stuff ensures we clean up shared memory on failure */
	PG_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
	{
		cycleid = _bt_start_vacuum(rel);

		xbtvacuumscan(info, stats, callback, callback_state, cycleid);
	}
	PG_END_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
	_bt_end_vacuum(rel);

	return stats;
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
xbtvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	/* No-op in ANALYZE ONLY mode */
	if (info->analyze_only)
		return stats;

	/*
	 * If btbulkdelete was called, we need not do anything, just return the
	 * stats from the latest btbulkdelete call.  If it wasn't called, we must
	 * still do a pass over the index, to recycle any newly-recyclable pages
	 * and to obtain index statistics.
	 *
	 * Since we aren't going to actually delete any leaf items, there's no
	 * need to go through all the vacuum-cycle-ID pushups.
	 */
	if (stats == NULL)
	{
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		xbtvacuumscan(info, stats, NULL, NULL, 0);
	}

	/* xbtree don't use Free Space Map */

	/*
	 * It's quite possible for us to be fooled by concurrent page splits into
	 * double-counting some index tuples, so disbelieve any total that exceeds
	 * the underlying heap's count ... if we know that accurately.  Otherwise
	 * this might just make matters worse.
	 */
	if (!info->estimated_count && stats->num_index_tuples > info->num_heap_tuples &&
		info->num_heap_tuples >= 0)
		stats->num_index_tuples = info->num_heap_tuples;

	return stats;
}

/*
 * btvacuumscan --- scan the index for VACUUMing purposes
 *
 * This combines the functions of looking for leaf tuples that are deletable
 * according to the vacuum callback, looking for empty pages that can be
 * deleted, and looking for old deleted pages that can be recycled.  Both
 * btbulkdelete and btvacuumcleanup invoke this (the latter only if no
 * btbulkdelete call occurred).
 *
 * The caller is responsible for initially allocating/zeroing a stats struct
 * and for obtaining a vacuum cycle ID if necessary.
 */
static void
xbtvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state,
			 BTCycleId cycleid)
{
	Relation	rel = info->index;
	BTVacState	vstate;
	BlockNumber num_pages;
	BlockNumber scanblkno;
	bool		need_lock = false;

	/*
	 * Reset fields that track information about the entire index now.  This
	 * avoids double-counting in the case where a single VACUUM command
	 * requires multiple scans of the index.
	 *
	 * Avoid resetting the tuples_removed and pages_newly_deleted fields here,
	 * since they track information about the VACUUM command, and so must last
	 * across each call to btvacuumscan().
	 *
	 * (Note that pages_free is treated as state about the whole index, not
	 * the current VACUUM.  This is appropriate because RecordFreeIndexPage()
	 * calls are idempotent, and get repeated for the same deleted pages in
	 * some scenarios.  The point for us is to track the number of recyclable
	 * pages in the index at the end of the VACUUM command.)
	 */
	stats->estimated_count = false;
	stats->num_pages = 0;
	stats->num_index_tuples = 0;
	stats->pages_deleted = 0;
	stats->pages_free = 0;

	/* Set up info to pass down to xbtvacuumpage */
	vstate.info = info;
	vstate.stats = stats;
	vstate.callback = callback;
	vstate.callback_state = callback_state;
	vstate.cycleid = cycleid;

	/* Create a temporary memory context to run _bt_pagedel in */
	vstate.pagedelcontext = AllocSetContextCreate(
		CurrentMemoryContext, "_bt_pagedel", ALLOCSET_DEFAULT_MINSIZE,
		ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
	
	/* Initialize vstate fields used by _bt_pendingfsm_finalize */
	vstate.bufsize = 0;
	vstate.maxbufsize = 0;
	vstate.pendingpages = NULL;
	vstate.npendingpages = 0;
	/* Consider applying _bt_pendingfsm_finalize optimization */
	_bt_pendingfsm_init(rel, &vstate, (callback == NULL));

	/*
	 * The outer loop iterates over all index pages except the metapage, in
	 * physical order (we hope the kernel will cooperate in providing
	 * read-ahead for speed).  It is critical that we visit all leaf pages,
	 * including ones added after we start the scan, else we might fail to
	 * delete some deletable tuples.  Hence, we must repeatedly check the
	 * relation length.  We must acquire the relation-extension lock while
	 * doing so to avoid a race condition: if someone else is extending the
	 * relation, there is a window where bufmgr/smgr have created a new
	 * all-zero page but it hasn't yet been write-locked by _bt_getbuf(). If
	 * we manage to scan such a page here, we'll improperly assume it can be
	 * recycled.  Taking the lock synchronizes things enough to prevent a
	 * problem: either numPages won't include the new page, or _bt_getbuf
	 * already has write lock on the buffer and it will be fully initialized
	 * before we can examine it.  (See also vacuumlazy.c, which has the same
	 * issue.)	Also, we need not worry if a page is added immediately after
	 * we look; the page splitting code already has write-lock on the left
	 * page before it adds a right page, so we must already have processed any
	 * tuples due to be moved into such a page.
	 *
	 * We can skip locking for new or temp relations, however, since no one
	 * else could be accessing them.
	 */
	need_lock = !RELATION_IS_LOCAL(rel);

	scanblkno = BTREE_METAPAGE + 1;
	for (;;)
	{
		/* Get the current relation length */
		if (need_lock)
			LockRelationForExtension(rel, ExclusiveLock);
		num_pages = RelationGetNumberOfBlocks(rel);
		if (need_lock)
			UnlockRelationForExtension(rel, ExclusiveLock);

		if (info->report_progress)
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
										 num_pages);
		
		/* Quit if we've scanned the whole relation */
		if (scanblkno >= num_pages)
			break;
		/* Iterate over pages, then loop back to recheck length */
		for (; scanblkno < num_pages; scanblkno++)
		{
			xbtvacuumpage(&vstate, scanblkno);
			if (info->report_progress)
				pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
											 scanblkno);
		}
	}

	MemoryContextDelete(vstate.pagedelcontext);

	/*
	 * If there were any calls to _bt_pagedel() during scan of the index then
	 * see if any of the resulting pages can be placed in the FSM now.  When
	 * it's not safe we'll have to leave it up to a future VACUUM operation.
	 *
	 * Finally, if we placed any pages in the FSM (either just now or during
	 * the scan), forcibly update the upper-level FSM pages to ensure that
	 * searchers can find them.
	 */
	_bt_pendingfsm_finalize(rel, &vstate);
	//if (stats->pages_free > 0)
		//IndexFreeSpaceMapVacuum(rel);
}

/*
 * btvacuumpage --- VACUUM one page
 *
 * This processes a single page for xbtvacuumscan().  In some cases we
 * must go back and re-examine previously-scanned pages; this routine
 * recurses when necessary to handle that case.
 *
 * blkno is the page to process.  origBlkno is the highest block number
 * reached by the outer btvacuumscan loop (the same as blkno, unless we
 * are recursing to re-examine a previous page).
 */
static void
xbtvacuumpage(BTVacState *vstate, BlockNumber scanblkno)
{
	IndexVacuumInfo		  *info = vstate->info;
	IndexBulkDeleteResult *stats = vstate->stats;
	Relation			   rel = info->index;
	bool				   attempt_pagedel;
	BlockNumber			   blkno, backtrack_to;
	Buffer				   buf;
	Page				   page;
	XBTPageOpaqueInternal  opaque;
	blkno = scanblkno;

backtrack:
	attempt_pagedel = false;
	backtrack_to = P_NONE;

	/* call vacuum_delay_point while not holding any buffer lock */
	vacuum_delay_point();

	/*
	 * We can't use _bt_getbuf() here because it always applies
	 * xbt_checkpage(), which will barf on an all-zero page. We want to
	 * recycle all-zero pages, not fail.  Also, we want to use a nondefault
	 * buffer access strategy.
	 */
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, info->strategy);
	_bt_lockbuf(rel, buf, BT_READ);
	page = BufferGetPage(buf);
	opaque = NULL;
	if (!PageIsNew(page))
	{
		_xbt_checkpage(rel, buf);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	}

	Assert(blkno <= scanblkno);
	if (blkno != scanblkno)
	{
		/*
		 * We're backtracking.
		 *
		 * We followed a right link to a sibling leaf page (a page that
		 * happens to be from a block located before scanblkno).  The only
		 * case we want to do anything with is a live leaf page having the
		 * current vacuum cycle ID.
		 *
		 * The page had better be in a state that's consistent with what we
		 * expect.  Check for conditions that imply corruption in passing.  It
		 * can't be half-dead because only an interrupted VACUUM process can
		 * leave pages in that state, so we'd definitely have dealt with it
		 * back when the page was the scanblkno page (half-dead pages are
		 * always marked fully deleted by _bt_pagedel(), barring corruption).
		 */
		if (!opaque || !P_ISLEAF(opaque) || P_ISHALFDEAD(opaque))
		{
			Assert(false);
			ereport(LOG,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("right sibling %u of scanblkno %u unexpectedly in an inconsistent state in index \"%s\"",
									 blkno, scanblkno, RelationGetRelationName(rel))));
			_bt_relbuf(rel, buf);
			return;
		}
		
		if (xbtree_page_recyclable(page) || opaque->btpo_cycleid != vstate->cycleid || P_ISDELETED(opaque))
		{
			/* Done with current scanblkno (and all lower split pages) */
			_bt_relbuf(rel, buf);
			return;
		}
	}

	/* Page is valid, see what to do with it */
	if (xbtree_page_recyclable(page))
		/* Okay to recycle this page */
		stats->pages_deleted++;
	else if (P_ISDELETED(opaque))
		/* Already deleted, but can't recycle yet */
		stats->pages_deleted++;
	else if (P_ISHALFDEAD(opaque))
		/* Half-dead leaf page (from interrupted VACUUM) -- finish deleting */
		attempt_pagedel = true;
	else if (P_ISLEAF(opaque))
	{
		OffsetNumber minoff;
		OffsetNumber maxoff;
		FullTransactionId oldest_xmin;
		oldest_xmin.value = pg_atomic_read_u64(&(undo_sys_ctx->global_frozen_xid));
		/*
		 * vacuum logic for xbtree. We use XBTreePagePruneOpt() instead
		 * of original vacuum.
		 *
		 * Note: we only prune leaf pages and never delete right most page.
		 */

		/* acquire the write lock for clean up */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		/* prune this index page */
		xbt_page_prune(rel, buf, oldest_xmin);

		if (!P_RIGHTMOST(opaque) && PageGetMaxOffsetNumber(page) == 1)
			/* already empty (only HIKEY left), ok to delete */
			attempt_pagedel = true;

		minoff = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);
		/*
		 * If it's now empty, try to delete; else count the live tuples. We
		 * don't delete when recursing, though, to avoid putting entries into
		 * freePages out-of-order (doesn't seem worth any extra code to handle
		 * the case).
		 */
		if (minoff <= maxoff)
			stats->num_index_tuples += maxoff - minoff + 1;
	}

	if (attempt_pagedel)
	{
		MemoryContext oldcontext;
		int			  ndel;
		/* Run pagedel in a temp context to avoid memory leakage */
		MemoryContextReset(vstate->pagedelcontext);
		oldcontext = MemoryContextSwitchTo(vstate->pagedelcontext);

		ndel = _xbt_pagedel(rel, buf);
		if (ndel)
		{
			/* successfully deleted, move to freed page queue */
			xbtree_record_free_page(rel, blkno, ReadNextFullTransactionId());
			/* count only this page, else may double-count parent */
			stats->pages_deleted++;
		}

		MemoryContextSwitchTo(oldcontext);
		/* pagedel released buffer, so we shouldn't */
	}
	else
		_bt_relbuf(rel, buf);

	/*
	 * This is really tail recursion, but if the compiler is too stupid to
	 * optimize it as such, we'd eat an uncomfortably large amount of stack
	 * space per recursion level (due to the deletable[] array). A failure is
	 * improbable since the number of levels isn't likely to be large ... but
	 * just in case, let's hand-optimize into a loop.
	 */
	if (backtrack_to != P_NONE)
	{
		blkno = backtrack_to;
		goto backtrack;
	}
}

/*
 *	xbtcanreturn() -- Check whether btree indexes support index-only scans.
 *
 * xbtrees always do, so this is trivial.
 */
bool
xbtcanreturn(Relation index, int attno)
{
	return true;
}

bytea *
xbtoptions(Datum reloptions, bool validate)
{
	return btoptions(reloptions, validate);
}

/*
 *	xbtproperty() -- Check boolean properties of indexes.
 *
 * This is optional, but handling AMPROP_RETURNABLE here saves opening the rel
 * to call btcanreturn.
 */
bool
xbtproperty(Oid index_oid, int attno,
		   IndexAMProperty prop, const char *propname,
		   bool *res, bool *isnull)
{
	switch (prop)
	{
		case AMPROP_RETURNABLE:
			/* answer only for columns, not AM or whole index */
			if (attno == 0)
				return false;
			/* otherwise, btree can always return data */
			*res = true;
			return true;

		default:
			return false;		/* punt to generic code */
	}
}

/*
 *	xbtbuildphasename() -- Return name of index build phase.
 */
char *
xbtbuildphasename(int64 phasenum)
{
	switch (phasenum)
	{
		case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
			return "initializing";
		case PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN:
			return "scanning table";
		case PROGRESS_BTREE_PHASE_PERFORMSORT_1:
			return "sorting live tuples";
		case PROGRESS_BTREE_PHASE_PERFORMSORT_2:
			return "sorting dead tuples";
		case PROGRESS_BTREE_PHASE_LEAF_LOAD:
			return "loading tuples in tree";
		default:
			return NULL;
	}
}

/* ------------------------------------------------------------------------
 * Definition of the xbtree index access method.
 * ------------------------------------------------------------------------
 */

/*
 * Xbtree handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
PG_FUNCTION_INFO_V1(xbthandler);
Datum
xbthandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = BTMaxStrategyNumber;
	amroutine->amsupport = BTNProcs;
	amroutine->amcanorder = true;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = true;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = true;
	amroutine->amsearchnulls = true;
	// set amstorage to true, otherwise cannot set name_ops's opckeytype to (cstring,2275)
	// which is differ from name_ops's opcinttype(name,19)
	amroutine->amstorage = true;
	amroutine->amclusterable = true;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = true;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = true;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = xbtbuild;
	amroutine->ambuildempty = xbtbuildempty;
	amroutine->aminsert = xbtinsert;
    amroutine->amdelete = xbtdelete;
	amroutine->ambulkdelete = xbtbulkdelete;
	amroutine->amvacuumcleanup = xbtvacuumcleanup;
	amroutine->amcanreturn = xbtcanreturn;
	amroutine->amcostestimate = btcostestimate;
	amroutine->amoptions = xbtoptions;
	amroutine->amproperty = xbtproperty;
	amroutine->ambuildphasename = xbtbuildphasename;
	amroutine->amvalidate = btvalidate;
	amroutine->ambeginscan = xbtbeginscan;
	amroutine->amrescan = xbtrescan;
	amroutine->amgettuple = xbtgettuple;
	amroutine->amgetbitmap = xbtgetbitmap;
	amroutine->amendscan = xbtendscan;
	amroutine->ammarkpos = xbtmarkpos;
	amroutine->amrestrpos = xbtrestrpos;
	amroutine->amestimateparallelscan = xbtestimateparallelscan;
	amroutine->aminitparallelscan = xbtinitparallelscan;
	amroutine->amparallelrescan = xbtparallelrescan;

	PG_RETURN_POINTER(amroutine);
}

Oid 
xbt_oid(void)
{
	Relation pg_am_rel;
    ScanKeyData key[1];
    SysScanDesc scan;
    HeapTuple tuple;
	Oid		xbtreeOid = InvalidOid;

    ScanKeyInit(key,
                Anum_pg_am_amname,
                BTEqualStrategyNumber, F_NAMEEQ,
                PointerGetDatum("xbtree"));

    pg_am_rel = table_open(AccessMethodRelationId, AccessShareLock);

    scan = systable_beginscan(pg_am_rel, AmNameIndexId, true, NULL, 1, key);

    tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_am form = (Form_pg_am) GETSTRUCT(tuple);
		xbtreeOid = form->oid;
	}

    systable_endscan(scan);

    table_close(pg_am_rel, AccessShareLock);

    return xbtreeOid;
}

Oid 
xbt_boolopsfamily(void)
{
	Relation 	pg_opfamily_rel;
    ScanKeyData key[2];
    SysScanDesc scan;
    HeapTuple 	tuple;
	Oid 		xbtreeOid = xbt_oid();
	Oid 		xbtreeBoolOpf = InvalidOid;

	ScanKeyInit(&key[0],
                Anum_pg_opfamily_opfmethod,
                BTEqualStrategyNumber, F_OIDEQ,
                xbtreeOid);

	ScanKeyInit(&key[1],
                Anum_pg_opfamily_opfname,
                BTEqualStrategyNumber, F_NAMEEQ,
                PointerGetDatum("bool_ops"));

	pg_opfamily_rel = table_open(OperatorFamilyRelationId, AccessShareLock);

	scan = systable_beginscan(pg_opfamily_rel, OpfamilyAmNameNspIndexId, true, NULL, 2, key);

    tuple = systable_getnext(scan);

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_opfamily form = (Form_pg_opfamily) GETSTRUCT(tuple);
		xbtreeBoolOpf = form->oid;
	}

    systable_endscan(scan);

    table_close(pg_opfamily_rel, AccessShareLock);

    return xbtreeBoolOpf;
}

bool 
same_opfamily_for_btree_and_xbtree(Oid btreeOpf, Oid xbtreeOpf)
{
	Relation 	pg_opfamily_rel;
    ScanKeyData btreeKey[1], xbtreeKey[1];
    SysScanDesc btreeScan, xbtreeScan;
    HeapTuple 	btreeTuple, xbtreeTuple;
	char*		btreeOpfname;
	char*		xbtreeOpfname;
	bool 		isSame = false;
	Oid			xbtreeOid = xbt_oid();

	pg_opfamily_rel = table_open(OperatorFamilyRelationId, AccessShareLock);

	// Get opfname for btree
	ScanKeyInit(&btreeKey[0],
				Anum_pg_opfamily_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(btreeOpf));
	btreeScan = systable_beginscan(pg_opfamily_rel, OpfamilyOidIndexId, true, NULL, 1, btreeKey);
	btreeTuple = systable_getnext(btreeScan);
	if (!HeapTupleIsValid(btreeTuple))
		goto out2;
	else
	{
		Form_pg_opfamily form = (Form_pg_opfamily) GETSTRUCT(btreeTuple);
		btreeOpfname = NameStr(form->opfname);
		if (form->opfmethod != xbtreeOid && form->opfmethod != BTREE_AM_OID)
		{
			// neither xbtree nor btree
			goto out2;
		}
	}

	// Get opfname for xbtree
	ScanKeyInit(&xbtreeKey[0],
				Anum_pg_opfamily_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(xbtreeOpf));
	xbtreeScan = systable_beginscan(pg_opfamily_rel, OpfamilyOidIndexId, true, NULL, 1, xbtreeKey);
	xbtreeTuple = systable_getnext(xbtreeScan);
	if (!HeapTupleIsValid(xbtreeTuple))
		goto out1;
	else
	{
		Form_pg_opfamily form = (Form_pg_opfamily) GETSTRUCT(xbtreeTuple);
		xbtreeOpfname = NameStr(form->opfname);
		if (form->opfmethod != xbtreeOid && form->opfmethod != BTREE_AM_OID)
		{
			// neither xbtree nor btree
			goto out1;
		}
	}

	// Compare opfname
	if (strncmp(btreeOpfname, xbtreeOpfname, NAMEDATALEN) == 0)
		isSame = true;

	// Close scan and table
out1:
	systable_endscan(xbtreeScan);
out2:
	systable_endscan(btreeScan);
    table_close(pg_opfamily_rel, AccessShareLock);

	return isSame;
}

bool 
relation_is_xstore_index(void* relation)
{
	Relation rel = (Relation) relation;
	return (rel->rd_indam != NULL) && (rel->rd_indam->ambuild == xbtbuild);
}

Size 
size_of_xbtpage_opaque_data(void)
{
	return sizeof(XBTPageOpaqueData);
}