/* -------------------------------------------------------------------------
 *
 * xstat.c
 * some extra state for xheap.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xstat.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgstat.h"
#include "miscadmin.h"
#include "access/hio.h"
#include "postmaster/autovacuum.h"
#include "storage/lmgr.h"
#include "storage/freespace.h"
#include "storage/shmem.h"
#include "utils/rel.h"
#include "xstore.h"
#include "xheap/xheap.h"
#include "xheap/xstat.h"
#include "xheap/xpage.h"
        
#define STARTBLOCK_INIT_XHEAP_REL_NUM 100
#define STARTBLOCK_ESTMATE_MAX_XHEAP_REL_NUM 10000

int xstore_max_search_length_for_prune = 10;
static XstorePruneState *xstore_prune_state = NULL;
const char *xstore_prune_state_tranche = "XstorePruneStateTranche";

static inline int
calc_max_blocks_to_scan(Relation relation)
{
	int max_blocks;
	Assert(relation->pgstat_info);
	max_blocks = relation->pgstat_info->t_freeRatio * relation->pgstat_info->t_freeRatio *
         relation->pgstat_info->t_pruneSuccessRatio * relation->pgstat_info->t_pruneSuccessRatio *
         xstore_max_search_length_for_prune;

	return Max(1, max_blocks);
}

void
xstore_prune_stat_shmem_init(Pointer ptr, bool found)
{
	XstorePruneState *state;


	state = (XstorePruneState *)ptr;
	if (!found)
	{
		int i;
		HASHCTL		info;

        LWLockPadded *locks = GetNamedLWLockTranche(xstore_prune_state_tranche);
		/* Initialize the partition locks*/
		for (i = 0; i < NUM_STARTBLOCK_PARTITIONS; i++)
            state->start_block_locks[i] = &(locks[i].lock);

		/* Init the block hash */
		MemSet(&info, 0, sizeof(info));
		info.keysize = sizeof(PgStat_StartBlockTableKey);
		info.entrysize = sizeof(PgStat_StartBlockTableEntry);
		info.num_partitions = NUM_LOCK_PARTITIONS;

		state->start_block_hash = ShmemInitHash("Start Block hash",
											  STARTBLOCK_INIT_XHEAP_REL_NUM,
										   STARTBLOCK_ESTMATE_MAX_XHEAP_REL_NUM,
										   &info,
										   HASH_ELEM | HASH_BLOBS | HASH_PARTITION);
	}

	xstore_prune_state = state;
}

Size
xstore_prune_shmem_size(void)
{
	return sizeof(XstorePruneState);
}

static LWLock *
lock_start_block_hash_table_partition(PgStat_StartBlockTableKey *tabkey, LWLockMode mode)
{
	uint32 hash_value;
	uint32 partition;
	LWLockId lock;

	Assert(xstore_prune_state);
	hash_value = get_hash_value(xstore_prune_state->start_block_hash, tabkey);
	partition = hash_value % (NUM_STARTBLOCK_PARTITIONS);
	lock = xstore_prune_state->start_block_locks[partition];

	LWLockAcquire(lock, mode);

	return lock;
}

static PgStat_StartBlockTableEntry *
start_block_hash_table_lookup(PgStat_StartBlockTableKey *tabkey)
{
	PgStat_StartBlockTableEntry *result = NULL;
	bool found = false;

	LWLockId lock = lock_start_block_hash_table_partition(tabkey, LW_SHARED);
	result = (PgStat_StartBlockTableEntry *) hash_search(xstore_prune_state->start_block_hash,
														 tabkey, HASH_FIND, &found);
	LWLockRelease(lock);

	return result;
}

static PgStat_StartBlockTableEntry *
start_block_hash_table_add(PgStat_StartBlockTableKey *tabkey)
{
	bool found = true;
	PgStat_StartBlockTableEntry *result = NULL;

	LWLockId lock = lock_start_block_hash_table_partition(tabkey, LW_EXCLUSIVE);

	result = (PgStat_StartBlockTableEntry *)hash_search(xstore_prune_state->start_block_hash,
														 tabkey, HASH_ENTER, &found);

	if (!found)
	{
		int i = 0;

		for (i = 0; i < START_BLOCK_ARRAY_SIZE; i++)
			pg_atomic_write_u32(&(result->starting_blocks[i]), (uint32)i);
	}

	LWLockRelease(lock);

	return result;
}

PgStat_StartBlockTableEntry *
get_start_block_hash_entry(PgStat_StartBlockTableKey *tabkey)
{
	PgStat_StartBlockTableEntry *result = NULL;
	result = start_block_hash_table_lookup(tabkey);
	/* not found, add it */
	if (result == NULL)
		result = start_block_hash_table_add(tabkey);

	Assert(result);

	return result;
}

void
start_block_hash_table_remove(Relation rel)
{
	LWLock *lock;
	PgStat_StartBlockTableKey tab_key;
	tab_key.dbid = MyDatabaseId;
	tab_key.relid = RelationGetRelid(rel);
	if (!start_block_hash_table_lookup(&tab_key))
		return;

	lock = lock_start_block_hash_table_partition(&tab_key, LW_EXCLUSIVE);
	(void)hash_search(xstore_prune_state->start_block_hash, &tab_key, HASH_REMOVE, NULL);
	LWLockRelease(lock);
}

void
pgstat_report_prune_stats(Oid tableoid, uint32 statFlag,
					  bool shared, PgStat_Counter scanned,
					  PgStat_Counter pruned)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_Relation *shtabentry;
	PgStat_StatTabEntry *tabentry;
	Oid			dboid = MyDatabaseId;
	/* block acquiring lock for the same reason as pgstat_report_autovac() */
	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_RELATION,
											dboid, tableoid, false);

	shtabentry = (PgStatShared_Relation *) entry_ref->shared_stats;
	tabentry = &shtabentry->stats;

    tabentry->success_prune_cnt += pruned;
    tabentry->total_prune_cnt += scanned;

	pgstat_unlock_entry(entry_ref);
}

/*
 * Check the in-memory global statistics if relation has some dead tuples to prune.
 * If so, get a block to prune from a global hash.
 */
BlockNumber
relation_prune_optional(Relation relation, Size required_size)
{
	BlockNumber					 result = InvalidBlockNumber;
	PgStat_StartBlockTableEntry *start_block_entry = NULL;
	int64						 reltuples = 0;
	float4						 threshold = 0;
	int64						 deadtuples = 0;
	int64						 livetuples = 0;
	int							 max_search_length = 0;
	BlockNumber					 next_block = InvalidBlockNumber;
	PgStat_TableStatus			*pg_stat_info = relation->pgstat_info;

	if (pg_stat_info == NULL)
	{
		goto done;
	}

start:
	/*
     * Check if we have a starting block already. If so, read the latest
     * value and start pruning there.
     */
	if (pg_stat_info->startBlockArray != NULL)
	{
		/* Temporarily tell other backends we are working on this subset.
         * pg_atomic_exchange_u32 should return the old value.
        */
		uint32		index = pg_stat_info->startBlockIndex;
		BlockNumber startBlkno = pg_atomic_exchange_u32(
			&pg_stat_info->startBlockArray[index], InvalidBlockNumber);
		/* Someone else is working on this subset. Avoid contention and move on. */
		if (startBlkno == InvalidBlockNumber)
			goto done;

		max_search_length = calc_max_blocks_to_scan(relation);

		result = relation_prune_block_and_return(relation, startBlkno, max_search_length,
											 required_size, &next_block);
		/* failed to prune a block, check another subset next time */
		if (result == InvalidBlockNumber)
			pg_stat_info->startBlockIndex =
				(pg_stat_info->startBlockIndex + 1) % START_BLOCK_ARRAY_SIZE;

		Assert(next_block != InvalidBlockNumber);
		pg_atomic_exchange_u32(&pg_stat_info->startBlockArray[index], next_block);
		goto done;
	}

	/*
     * Now load the in-memory statistics for this table if not yet done
    */
	if (!pg_stat_info->t_globalStatChecked)
	{
		PgStat_StatTabEntry *tabentry;
		PgStat_StartBlockTableKey tabkey;

		tabentry = pgstat_fetch_stat_tabentry(RelationGetRelid(relation));

		pg_stat_info->t_globalStatChecked = true;

		if (tabentry == NULL)
		{
			goto done;
		}

		/*
         * Okay we found this table in the in-memory statistics hash.
         * Now check if there are dead tuples to prune.
         * Nice to have: More flexible to use reloptions values not the autovacuum one
         */
		livetuples = tabentry->live_tuples;
		threshold = (float4) autovacuum_vac_thresh + (autovacuum_vac_scale * livetuples);
		deadtuples = tabentry->dead_tuples;
		reltuples = livetuples + deadtuples;

		/* Not enough dead tuples to prune */
		if ((float4) deadtuples < threshold)
		{
			Assert(pg_stat_info->startBlockArray == NULL);
			goto done;
		}

		if (reltuples > 0)
			/* just in case */
			pg_stat_info->t_freeRatio = (float4) deadtuples / (float4) reltuples;

        pg_stat_info->t_pruneSuccessRatio = (tabentry->total_prune_cnt < 100) ?
            1 :
            (float4)tabentry->success_prune_cnt / (float4)tabentry->total_prune_cnt;
		/*
         * Grab the starting block for pruning.
         */
		tabkey.dbid = MyDatabaseId;
		tabkey.relid = RelationGetRelid(relation);
		start_block_entry = get_start_block_hash_entry(&tabkey);

		Assert(start_block_entry);

		pg_stat_info->startBlockIndex = GetTopTransactionId() % START_BLOCK_ARRAY_SIZE;
		pg_stat_info->startBlockArray = start_block_entry->starting_blocks;

		goto start;
	}

done:
	return result;
}

BlockNumber
relation_prune_block_and_return(Relation relation, BlockNumber start_block,
							BlockNumber max_blocks_to_scan, Size required_size,
							BlockNumber *next_block)
{
	bool				pruned = false;
	Size				freespace = 0;
	Page				page;
	TransactionId		fxid = GetTopTransactionId();
	Buffer				buffer;
	BlockNumber			result = InvalidBlockNumber;
	BlockNumber			nblocks = RelationGetNumberOfBlocks(relation);
	BlockNumber			blkno = start_block;
	BlockNumber			scanned_blocks = 0;
	BlockNumber			prune_try_cnt = 0;
	PgStat_TableStatus *pg_stat_info = relation->pgstat_info;
	BlockNumber			init_block = InvalidBlockNumber;
	*next_block = start_block;

	/* Handle the case of relation truncation */
	if (nblocks == 0)
		return InvalidBlockNumber;

	/* Handle the case when nblocks < START_BLOCK_ARRAY_SIZE */
	if (START_BLOCK_ARRAY_SIZE >= nblocks)
		pg_stat_info->startBlockIndex = fxid % nblocks;
	init_block = pg_stat_info->startBlockIndex;

	while (scanned_blocks < max_blocks_to_scan)
	{
		blkno = (blkno >= nblocks) ? init_block : blkno;

		buffer = ReadBuffer(relation, blkno);
		/*
         * We call page pruning in Insert-Update-Delete (and Scan).
         * So if we can't grab the buffer lock and this page is prunable then
         * the backend currently holding the lock could potentially prune it,
         * hence we do a conditional lock here.
         */
		if (!ConditionalLockBuffer(buffer))
		{
			ReleaseBuffer(buffer);
			goto next;
		}

		prune_try_cnt++;
		*next_block = blkno;
		page = BufferGetPage(buffer);
		/* This logic is similar to LazyScanXHeap() */
		if (PageIsNew(page))
		{
			/*
             * An all-zeros page could be left over if a backend extends the
             * relation but crashes before initializing the page, or when
             * bulk-extending the relation (which creates a number of empty
             * pages at the tail end of the relation, but enters them into the
             * FSM)Reclaim such pages for use.
             */

			/*
             * Perform checking of FSM after releasing lock, the fsm is
             * approximate, after all.
             */
			if (relation->rd_rel->relkind == RELKIND_TOASTVALUE)
				xpage_init(XPAGE_TOAST, page, BufferGetPageSize(buffer),
						  XHEAP_SPECIAL_SIZE);
			else
				xpage_init(XPAGE_HEAP, page, BufferGetPageSize(buffer), XHEAP_SPECIAL_SIZE);
			MarkBufferDirty(buffer);
			UnlockReleaseBuffer(buffer);

			if (GetRecordedFreeSpace(relation, blkno) == 0)
				freespace = BufferGetPageSize(buffer) - SizeOfXHeapPageHeaderData;
			if (freespace > 0)
				RecordPageWithFreeSpace(relation, blkno, freespace);

			result = blkno;
			break;
		}

		freespace = page_get_xheap_free_space(page);
		if (xpage_is_empty((XHeapPageHeaderData *) page) || required_size <= freespace)
		{
			UnlockReleaseBuffer(buffer);
			RecordPageWithFreeSpace(relation, blkno, freespace);
			result = blkno;
			break;
		}

		/* Done with the easy cases, we try to prune it now */
		pruned = xheap_page_prune_opt(relation, buffer, InvalidOffsetNumber, 0);
		freespace = page_get_xheap_free_space(page);
		UnlockReleaseBuffer(buffer);

		if (pruned)
		{
			RecordPageWithFreeSpace(relation, blkno, freespace);
			if (required_size <= freespace)
			{
				result = blkno;
				break;
			}
		}
	next:
		scanned_blocks++;
		blkno += START_BLOCK_ARRAY_SIZE;
	}

	/* Move the next_block pointer */
	if (result != InvalidBlockNumber)
		*next_block = ((*next_block + START_BLOCK_ARRAY_SIZE) >= nblocks)
						  ? init_block
						  : (*next_block + START_BLOCK_ARRAY_SIZE);

	if (prune_try_cnt)
        pgstat_report_prune_stats(RelationGetRelid(relation), InvalidOid, relation->rd_rel->relisshared,
            prune_try_cnt, (result != InvalidBlockNumber) ? 1 : 0);

	return result;
}

/*
 * Extend a xheap relation by multiple blocks to avoid future contention on the
 * relation extension lock.  Our goal is to pre-extend the relation by an
 * amount which ramps up as the degree of contention ramps up, but limiting
 * the result to some sane overall value.
 */
void
xheap_relation_add_extra_blocks(Relation relation, BulkInsertState bistate)
{
	Page		page;
	BlockNumber block_num = InvalidBlockNumber,
				first_block = InvalidBlockNumber;
	int			extra_blocks = 0;
	int			lock_waiters = 0;
	Size		freespace = 0;
	Buffer		buffer;

	/* Use the length of the lock wait queue to judge how much to extend. */
	lock_waiters = RelationExtensionLockWaiterCount(relation);
	if (lock_waiters <= 0)
		return;

	/*
	 * It might seem like multiplying the number of lock waiters by as much as
	 * 20 is too aggressive, but benchmarking revealed that smaller numbers
	 * were insufficient.  512 is just an arbitrary cap to prevent
	 * pathological results.
	 */
	extra_blocks = Min(512, lock_waiters * 20);

	while (extra_blocks-- >= 0)
	{
		/* Ouch - an unnecessary lseek() each time through the loop! */
		buffer = ReadBufferBI(relation, P_NEW, RBM_NORMAL, bistate);

		/* Extend by one page. */
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buffer);
		xpage_init(XPAGE_HEAP, page, BufferGetPageSize(buffer), XHEAP_SPECIAL_SIZE);
		MarkBufferDirty(buffer);
		block_num = BufferGetBlockNumber(buffer);
		freespace = PageGetHeapFreeSpace(page);
		UnlockReleaseBuffer(buffer);

		/* Remember first block number thus added. */
		if (first_block == InvalidBlockNumber)
			first_block = block_num;

		/*
		 * Immediately update the bottom level of the FSM.  This has a good
		 * chance of making this page visible to other concurrently inserting
		 * backends, and we want that to happen without delay.
		 */
		RecordPageWithFreeSpace(relation, block_num, freespace);
	}

	/*
	 * Updating the upper levels of the free space map is too expensive to do
	 * for every block, but it's worth doing once at the end to make sure that
	 * subsequent insertion activity sees all of those nifty free pages we
	 * just inserted.
	 *
	 * Note that we're using the freespace value that was reported for the
	 * last block we added as if it were the freespace value for every block
	 * we added.  That's actually true, because they're all equally empty.
	 */
	FreeSpaceMapVacuumRange(relation, first_block, block_num + 1);
}