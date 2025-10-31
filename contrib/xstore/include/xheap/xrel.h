
/* -------------------------------------------------------------------------
 *
 * xrel.h
 * the ralation utils for xstore.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/xheap/xrel.h
 * -------------------------------------------------------------------------
 *
 */

#define HEAP_MIN_FILLFACTOR			10
#define HEAP_DEFAULT_FILLFACTOR		100
#define XHEAP_DEFAULT_FILLFACTOR 92


#define RelationGetTargetPageFreeSpacePrune(relation, defaultff) \
    (BLCKSZ * (100 - 0.9 * RelationGetFillFactor(relation, defaultff)) / 100)


#define RelationGetRelFileLocator(relation) \
    ((relation)->rd_rel->relfilenode)


#define RelationGetRnodeSpace(relation) \
    ((relation)->rd_locator.spcOid)

#define RELATION_IS_PG_PARTITION(relation) \
	((relation)->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
    