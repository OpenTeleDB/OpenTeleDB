/* -------------------------------------------------------------------------
 *
 * xstat.h
 *
 * Some extra stat for xstore
 * 
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/xheap/xstat.h
 * -------------------------------------------------------------------------
 *
 */

#include "postgres.h"

#include "access/heapam.h"
#include "storage/lwlock.h"
#include "storage/block.h"
#include "utils/hsearch.h"
#include "utils/pgstat_internal.h"

#define START_BLOCK_ARRAY_SIZE 20
#define NUM_STARTBLOCK_PARTITIONS 128

extern const char *xstore_prune_state_tranche;

typedef struct XstorePruneState {
	LWLockId start_block_locks[NUM_STARTBLOCK_PARTITIONS];
	HTAB   *start_block_hash;
} XstorePruneState;

typedef struct PgStat_StartBlockTableKey {
    Oid dbid;
    Oid relid;
} PgStat_StartBlockTableKey;

typedef struct PgStat_StartBlockTableEntry {
    PgStat_StartBlockTableKey tabkey;
    pg_atomic_uint32 starting_blocks[START_BLOCK_ARRAY_SIZE];
} PgStat_StartBlockTableEntry;

extern Size xstore_prune_shmem_size(void);
extern void xstore_prune_stat_shmem_init(Pointer ptr, bool found);
extern PgStat_StartBlockTableEntry *get_start_block_hash_entry(PgStat_StartBlockTableKey *tabkey);
extern void start_block_hash_table_remove(Relation rel);
extern void pgstat_report_prune_stats(Oid tableoid, uint32 statFlag,
					  bool shared, PgStat_Counter scanned,
					  PgStat_Counter pruned);
extern BlockNumber relation_prune_optional(Relation relation, Size required_size);
extern BlockNumber relation_prune_block_and_return(Relation relation, BlockNumber start_block,
											   BlockNumber	max_blocks_to_scan,
											   Size			required_size,
											   BlockNumber *next_block);

extern void xheap_relation_add_extra_blocks(Relation relation, BulkInsertState bistate);


