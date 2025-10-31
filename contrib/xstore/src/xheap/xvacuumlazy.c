/* -------------------------------------------------------------------------
 *
 * xvacuumlazy.c
 * Implement the vacuum of xstore inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * src/xheap/xvacuumlazy.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/vacuum.h"
#include "postgres.h"
#include "access/xloginsert.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "xstore.h"
#include "xheap/xpage.h"
#include "access/visibilitymap.h"
#include "xheap/xheap.h"
#include "storage/buf.h"
#include "storage/block.h"
#include "storage/freespace.h"
#include "nodes/parsenodes.h"
#include "utils/relcache.h"
#include "utils/pg_rusage.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "executor/instrument.h"
#include "commands/dbcommands.h"
#include "commands/progress.h"
#include "postmaster/autovacuum.h"
#include "xstore.h"
#include "miscadmin.h"
#include "pgstat.h"

static int elevel = -1;

static BufferAccessStrategy vac_strategy;

static void lazy_scan_xheap(LVRelState *vacrel);

void
xheap_lazy_vacuum(Relation onerel, VacuumParams *params, BufferAccessStrategy bstrategy)
{
	LVRelState	 *vacrel;
	PGRUsage	  ru0;
	TimestampTz	  starttime = 0;
	FullTransactionId oldest_xmin;
	TransactionId new_frozen_xid;
	BlockNumber   orig_rel_pages;
	BlockNumber	  new_rel_pages;
	double		  new_rel_tuples;
	bool          verbose;
	bool          instrument;
	bool		  frozenxid_updated;
	bool		  minmulti_updated;
	PgStat_Counter          startreadtime = 0,
				            startwritetime = 0;
	ErrorContextCallback    errcallback;
	char	                **indnames = NULL;
	WalUsage	startwalusage = pgWalUsage;
	int64		startpagehit = VacuumPageHit,
				startpagemiss = VacuumPageMiss,
				startpagedirty = VacuumPageDirty;

	verbose = (params->options & VACOPT_VERBOSE) != 0;
	instrument = (verbose || (AmAutoVacuumWorkerProcess() &&
							  params->log_min_duration >= 0));
	if (instrument)
	{
		pg_rusage_init(&ru0);
		starttime = GetCurrentTimestamp();
		if (track_io_timing)
		{
			startreadtime = pgStatBlockReadTime;
			startwritetime = pgStatBlockWriteTime;
		}
	}

	if (params->options & VACOPT_VERBOSE)
		elevel = INFO;
	else
		elevel = DEBUG2;

	vac_strategy = bstrategy;

	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM, RelationGetRelid(onerel));

	oldest_xmin = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
	if (!FullTransactionIdIsNormal(oldest_xmin))
	{
		ereport(WARNING, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						  errmsg("globalRecycleXid(%lu) is abnormal.", oldest_xmin.value),
						  errdetail("N/A %s",
						  "There is a high probability that the DML operation "
						  "has not been performed on any xstore table. "
						  "Check the value of globalFrozenXid in undo_cxt.")));
		return;
	}

	vacrel = (LVRelState *) palloc0(sizeof(LVRelState));

	vacrel->relnamespace = get_namespace_name(RelationGetNamespace(onerel));
	vacrel->relname = pstrdup(RelationGetRelationName(onerel));
	vacrel->indname = NULL;
	vacrel->phase = VACUUM_ERRCB_PHASE_UNKNOWN;
	vacrel->verbose = verbose;
	errcallback.callback = vacuum_error_callback;
	errcallback.arg = vacrel;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	if (verbose)
	{
		Assert(!AmAutoVacuumWorkerProcess());
		ereport(INFO,
				(errmsg("vacuuming \"%s.%s.%s\"",
						get_database_name(MyDatabaseId),
						vacrel->relnamespace, vacrel->relname)));
	}

	vacrel->rel = onerel;
	/* Open all indexes of the relation */
	vac_open_indexes(vacrel->rel, RowExclusiveLock, &vacrel->nindexes, &vacrel->indrels);

	if (instrument && vacrel->nindexes > 0)
	{
		indnames = palloc(sizeof(char *) * vacrel->nindexes);
		for (int i = 0; i < vacrel->nindexes; i++)
			indnames[i] = pstrdup(RelationGetRelationName(vacrel->indrels[i]));
	}

	/*
	 * The index_cleanup param either disables index vacuuming and cleanup or
	 * forces it to go ahead when we would otherwise apply the index bypass
	 * optimization.  The default is 'auto', which leaves the final decision
	 * up to lazy_vacuum().
	 *
	 * The truncate param allows user to avoid attempting relation truncation,
	 * though it can't force truncation to happen.
	 */
	Assert(params->index_cleanup != VACOPTVALUE_UNSPECIFIED);
	Assert(params->truncate != VACOPTVALUE_UNSPECIFIED &&
		   params->truncate != VACOPTVALUE_AUTO);

	/* XStore do not use visibility map and scan all pages now */
	VacuumFailsafeActive = false;
	vacrel->aggressive = true;
	vacrel->skipwithvm = false;
	vacrel->consider_bypass_optimization = true;
	vacrel->do_index_vacuuming = true;
	vacrel->do_index_cleanup = true;
	vacrel->do_rel_truncate = false;

	/*
	 * VACOPTVALUE_ENABLED is no taken into consideration in XStore because we don't need
	 * to collect dead tuples before vauum index and heap.
	 */
	if (params->index_cleanup == VACOPTVALUE_DISABLED)
	{
		/* Force disable index vacuuming up-front */
		vacrel->do_index_vacuuming = false;
		vacrel->do_index_cleanup = false;
	}

	vacrel->bstrategy = bstrategy;
	vacrel->cutoffs.relfrozenxid = onerel->rd_rel->relfrozenxid;
	vacrel->cutoffs.relminmxid = onerel->rd_rel->relminmxid;

	/* Initialize page counters explicitly (be tidy) */
	vacrel->scanned_pages = 0;
	vacrel->removed_pages = 0;
	vacrel->lpdead_item_pages = 0;
	vacrel->missed_dead_pages = 0;
	vacrel->nonempty_pages = 0;
	/* dead_items_alloc allocates vacrel->dead_items later on */

	/* Allocate/initialize output statistics state */
	vacrel->new_rel_tuples = 0;
	vacrel->new_live_tuples = 0;
	vacrel->indstats = (IndexBulkDeleteResult **)
		palloc0(vacrel->nindexes * sizeof(IndexBulkDeleteResult *));

	/* Initialize remaining counters (be tidy) */
	vacrel->num_index_scans = 0;
	vacrel->tuples_deleted = 0;
	vacrel->lpdead_items = 0;
	vacrel->live_tuples = 0;
	vacrel->recently_dead_tuples = 0;
	vacrel->missed_dead_tuples = 0;

	vacrel->rel_pages = orig_rel_pages = RelationGetNumberOfBlocks(onerel);
	vacrel->cutoffs.OldestXmin = XidFromFullTransactionId(oldest_xmin);
	vacrel->cutoffs.epoch = EpochFromFullTransactionId(oldest_xmin);
	dead_items_alloc(vacrel, params->nworkers);

	/* Do the vacuuming for xheap */
	lazy_scan_xheap(vacrel);

	/*
	 * Xbtree index doesn't need to know the dead tuple, because it has its own xmin and
	 * xmax. We just need to call the bulk delete and clean up interfaces in xbtree AM.
	 */
	if (vacrel->do_index_vacuuming && vacrel->nindexes > 0)
	{
		lazy_vacuum_all_indexes(vacrel);
		lazy_cleanup_all_indexes(vacrel);
		update_relstats_all_indexes(vacrel);
	}

	/* Done with rel's indexes */
	vac_close_indexes(vacrel->nindexes, vacrel->indrels, NoLock);

	/* Report that we are now doing final cleanup */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_FINAL_CLEANUP);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	/* Report that we are now doing final cleanup */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_FINAL_CLEANUP);

	new_rel_pages = vacrel->rel_pages;
	new_rel_tuples = vacrel->new_rel_tuples;
	new_frozen_xid = XidFromFullTransactionId(oldest_xmin);

	/* Vacuum the Free Space Map */
	FreeSpaceMapVacuum(onerel);

	vac_update_relstats(onerel, new_rel_pages, new_rel_tuples, 0,
						vacrel->nindexes > 0, new_frozen_xid, vacrel->NewRelminMxid,
						&frozenxid_updated, &minmulti_updated, false);
	/*
	 * Report results to the cumulative stats system, too.
	 *
	 * Deliberately avoid telling the stats system about LP_DEAD items that
	 * remain in the table due to VACUUM bypassing index and heap vacuuming.
	 * ANALYZE will consider the remaining LP_DEAD items to be dead "tuples".
	 * It seems like a good idea to err on the side of not vacuuming again too
	 * soon in cases where the failsafe prevented significant amounts of heap
	 * vacuuming.
	 */
	pgstat_report_vacuum(RelationGetRelid(onerel),
						 onerel->rd_rel->relisshared,
						 Max(vacrel->new_live_tuples, 0),
						 vacrel->recently_dead_tuples +
						 vacrel->missed_dead_tuples);
	pgstat_progress_end_command();

	if (instrument)
	{
		TimestampTz endtime = GetCurrentTimestamp();

		if (verbose || params->log_min_duration == 0 ||
			TimestampDifferenceExceeds(starttime, endtime,
									   params->log_min_duration))
		{
			long		secs_dur;
			int			usecs_dur;
			WalUsage	walusage;
			StringInfoData buf;
			char	   *msgfmt;
			int32		diff;
			int64		pagehitop = VacuumPageHit - startpagehit,
						pagemissop = VacuumPageMiss - startpagemiss,
						pagedirtyop = VacuumPageDirty - startpagedirty;
			double		read_rate = 0,
						write_rate = 0;

			TimestampDifference(starttime, endtime, &secs_dur, &usecs_dur);
			memset(&walusage, 0, sizeof(WalUsage));
			WalUsageAccumDiff(&walusage, &pgWalUsage, &startwalusage);

			initStringInfo(&buf);
			if (verbose)
			{
				/*
				 * Aggressiveness already reported earlier, in dedicated
				 * VACUUM VERBOSE ereport
				 */
				Assert(!params->is_wraparound);
				msgfmt = _("finished vacuuming \"%s.%s.%s\": index scans: %d\n");
			}
			/* XStore vacuum scan all the pages in xheap now, so we can treat it as always aggressive*/
			else if (params->is_wraparound)
			{
				/*
				 * While it's possible for a VACUUM to be both is_wraparound
				 * and !aggressive, that's just a corner-case -- is_wraparound
				 * implies aggressive.  Produce distinct output for the corner
				 * case all the same, just in case.
				 */
				msgfmt = _("automatic aggressive vacuum to prevent wraparound of table \"%s.%s.%s\": index scans: %d\n");
			}
			else
			{
				msgfmt = _("automatic aggressive vacuum of table \"%s.%s.%s\": index scans: %d\n");
			}

			appendStringInfo(&buf, msgfmt,
							 get_database_name(MyDatabaseId),
							 vacrel->relnamespace,
							 vacrel->relname,
							 vacrel->num_index_scans);
			appendStringInfo(&buf, _("pages: %u removed, %u remain, %u scanned (%.2f%% of total)\n"),
							 vacrel->removed_pages,
							 new_rel_pages,
							 vacrel->scanned_pages,
							 orig_rel_pages == 0 ? 100.0 :
							 100.0 * vacrel->scanned_pages / orig_rel_pages);
			appendStringInfo(&buf,
							 _("tuples: %lld removed, %lld remain, %lld are dead but not yet removable\n"),
							 (long long) vacrel->tuples_deleted,
							 (long long) vacrel->new_rel_tuples,
							 (long long) vacrel->recently_dead_tuples);
			if (vacrel->missed_dead_tuples > 0)
				appendStringInfo(&buf,
								 _("tuples missed: %lld dead from %u pages not removed due to cleanup lock contention\n"),
								 (long long) vacrel->missed_dead_tuples,
								 vacrel->missed_dead_pages);
			diff = (int32) (ReadNextTransactionId() - XidFromFullTransactionId(oldest_xmin));
			appendStringInfo(&buf,
							 _("removable cutoff: %lu, which was %d XIDs old when operation ended\n"),
							 oldest_xmin.value, diff);
			if (frozenxid_updated)
			{
				diff = (int32) (vacrel->NewRelfrozenXid - vacrel->cutoffs.relfrozenxid);
				appendStringInfo(&buf,
								 _("new relfrozenxid: %u, which is %d XIDs ahead of previous value\n"),
								 vacrel->NewRelfrozenXid, diff);
			}
			if (vacrel->do_index_vacuuming)
			{
				if (vacrel->nindexes == 0 || vacrel->num_index_scans == 0)
					appendStringInfoString(&buf, _("index scan not needed: "));
				else
					appendStringInfoString(&buf, _("index scan needed: "));

				msgfmt = _("%u pages from table (%.2f%% of total) had %lld dead item identifiers removed\n");
			}

			appendStringInfo(&buf, msgfmt,
							 vacrel->lpdead_item_pages,
							 orig_rel_pages == 0 ? 100.0 :
							 100.0 * vacrel->lpdead_item_pages / orig_rel_pages,
							 (long long) vacrel->lpdead_items);
			for (int i = 0; i < vacrel->nindexes; i++)
			{
				IndexBulkDeleteResult *istat = vacrel->indstats[i];

				if (!istat)
					continue;

				appendStringInfo(&buf,
								 _("index \"%s\": pages: %u in total, %u newly deleted, %u currently deleted, %u reusable\n"),
								 indnames[i],
								 istat->num_pages,
								 istat->pages_newly_deleted,
								 istat->pages_deleted,
								 istat->pages_free);
			}
			if (track_io_timing)
			{
				double		read_ms = (double) (pgStatBlockReadTime - startreadtime) / 1000;
				double		write_ms = (double) (pgStatBlockWriteTime - startwritetime) / 1000;

				appendStringInfo(&buf, _("I/O timings: read: %.3f ms, write: %.3f ms\n"),
								 read_ms, write_ms);
			}
			if (secs_dur > 0 || usecs_dur > 0)
			{
				read_rate = (double) BLCKSZ * pagemissop / (1024 * 1024) /
					(secs_dur + usecs_dur / 1000000.0);
				write_rate = (double) BLCKSZ * pagedirtyop / (1024 * 1024) /
					(secs_dur + usecs_dur / 1000000.0);
			}
			appendStringInfo(&buf, _("avg read rate: %.3f MB/s, avg write rate: %.3f MB/s\n"),
							 read_rate, write_rate);
			appendStringInfo(&buf,
							 _("buffer usage: %lld hits, %lld misses, %lld dirtied\n"),
							 (long long) pagehitop,
							 (long long) pagemissop,
							 (long long) pagedirtyop);
			appendStringInfo(&buf,
							 _("WAL usage: %lld records, %lld full page images, %llu bytes\n"),
							 (long long) walusage.wal_records,
							 (long long) walusage.wal_fpi,
							 (unsigned long long) walusage.wal_bytes);
			appendStringInfo(&buf, _("system usage: %s"), pg_rusage_show(&ru0));

			ereport(verbose ? INFO : LOG,
					(errmsg_internal("%s", buf.data)));
			pfree(buf.data);
		}
	}

	/* Cleanup index statistics and index names */
	for (int i = 0; i < vacrel->nindexes; i++)
	{
		if (vacrel->indstats[i])
			pfree(vacrel->indstats[i]);

		if (instrument)
			pfree(indnames[i]);
	}
}

static void
lazy_scan_xheap(LVRelState *vacrel)
{
	BlockNumber	   nblocks = RelationGetNumberOfBlocks(vacrel->rel);
	BlockNumber	   blkno = InvalidBlockNumber;
	BlockNumber	   scan_starting_block = 0;
	int			   remain_tuples = 0;
	double		   num_tuples = 0;
	double		   tups_vacuumed = 0;
	RelationBuffer relbuf = {vacrel->rel, InvalidBuffer};
	char		  *relname = RelationGetRelationName(vacrel->rel);
	FullTransactionId  tmp;

	ereport(elevel, (errmsg("vacuuming xheap \"%s.%s\" oldestXmin:%u, epoch: %u",
							get_namespace_name(RelationGetNamespace(vacrel->rel)), relname,
							vacrel->cutoffs.OldestXmin, vacrel->cutoffs.epoch)));

	/*
	 * For xheap, we don't need to collect dead tuple to reclaim xbtree, so there iss no need to alloc
	 * space for vacrelstats->dead_tuples
	 */
	vacrel->rel_pages = nblocks;

	for (blkno = scan_starting_block; blkno < nblocks; blkno++)
	{
		Buffer buf;
		Page   page;
		Size   freespace = 0;

		vacuum_delay_point();

		buf = ReadBufferExtended(vacrel->rel, MAIN_FORKNUM, blkno, RBM_NORMAL, vac_strategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		vacrel->scanned_pages++;

		page = BufferGetPage(buf);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buf);

			if (GetRecordedFreeSpace(vacrel->rel, blkno) == 0)
				freespace = BufferGetPageSize(buf) - SizeOfXHeapPageHeaderData;

			if (freespace > 0)
			{
				RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
				elog(DEBUG1,
					 "relation \"%s\" page %u is uninitialized and not in fsm, fixing",
					 relname, blkno);
			}

			continue;
		}

		vacrel->nonempty_pages++;

		if (xpage_is_empty((XHeapPageHeaderData *) page))
		{
			freespace = page_get_xheap_free_space(page);
			UnlockReleaseBuffer(buf);
			RecordPageWithFreeSpace(vacrel->rel, blkno, freespace);
			continue;
		}

		relbuf.buffer = buf;

		tups_vacuumed += xheap_page_prune_guts(
			vacrel->rel, &relbuf, FullTransactionIdFromEpochAndXid(vacrel->cutoffs.epoch, vacrel->cutoffs.OldestXmin), InvalidOffsetNumber, 0, false, false,
			&tmp, NULL, &remain_tuples);
		UnlockReleaseBuffer(buf);
		num_tuples += remain_tuples;
	}

	vacrel->tuples_deleted = tups_vacuumed;

	vacrel->new_rel_tuples =
		vac_estimate_reltuples(vacrel->rel, nblocks, vacrel->scanned_pages, num_tuples);
}