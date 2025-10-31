/* -------------------------------------------------------------------------
 *
 * xstore.h
 * 
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 * include/xstore.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef XSTORE_H
#define XSTORE_H

#include "postgres.h"

#include "c.h"
#include "port/atomics.h"
#include "datatype/timestamp.h"
#include "undo/undobuffer.h"
#include "undo/undotype.h"
#include "storage/lwlock.h"
#include "access/twophase.h"
#include "access/transam.h"
#include "miscadmin.h"


#define RelFileLocatorRelCopy(relFileNodeRel, relFileNode) \
    do { \
        (relFileNodeRel).spcOid = (relFileNode).spcOid; \
        (relFileNodeRel).dbOid = (relFileNode).dbOid; \
        (relFileNodeRel).relNumber = (relFileNode).relNumber; \
    } while(0)

#define RelFileNodeCopy(relFileNode, relFileNodeRel, bucketid) \
    do {                                                       \
        (relFileNode).spcOid = (relFileNodeRel).spcOid;      \
        (relFileNode).dbOid = (relFileNodeRel).dbOid;        \
        (relFileNode).relNumber = (relFileNodeRel).relNumber;      \
    } while (0)

#define MAX_FROZEN_XMIN_NUM (MaxBackends + max_prepared_xacts)

extern pg_atomic_uint64 *MyFrozenXmins;

extern int undo_max_total_size;
extern int undo_max_size_per_transaction;

// for test only
extern int undo_max_segno_per_log;

typedef struct UndoLogContext
{
	int			      logs[UNDO_PERSISTENCE_LEVELS];
	void		     *slots[UNDO_PERSISTENCE_LEVELS];
	uint64		      slot_ptr[UNDO_PERSISTENCE_LEVELS];
	FullTransactionId prev_xid[UNDO_PERSISTENCE_LEVELS];
	uint64		      curr_trans_undo_size;  // undo size for current transaction
} UndoLogContext;


#define MAX_UNDORECORDS_PER_OPERATION 2 
#define MAX_UNDO_BUFFERS 16 
/*
 * Caching several undo buffers.
 * max undo buffers per record = 2
 * max undo records per operation = 2
 */
typedef struct UndoCacheContext
{
	UndoPrepareBuffers *undo_prepare_buffers;
	UnpackedUndoRecord *undo_records[MAX_UNDORECORDS_PER_OPERATION];
	struct UndoBuffer  *undo_buffers;
	int				    undo_buffer_idx;
	char		        disk_tuple_buffer[BLCKSZ];
} UndoCacheContext;


typedef struct UndoSysContext
{
	void			*ulogs[UNDOLOG_TOTAL_COUNT];
	pg_atomic_uint32 undo_total_size;	  // undo data block count
	uint32			 undo_meta_size;	  // meta data block count
	pg_atomic_uint32 undo_log_used_cout;  // used undo log count

	uint32			 undo_count_threshold;
	pg_atomic_uint64 global_frozen_xid;
	pg_atomic_uint64 global_frozen_xmin;
	pg_atomic_uint64 global_recycle_xid;
	pg_atomic_uint64 hot_standby_frozen_xid;

	int  undo_lock_tranche_id; // for LwLock;
	LWLock undo_log_lock;
} UndoSysContext;


extern UndoSysContext *undo_sys_ctx;
extern UndoCacheContext *undo_cache_ctx;
extern UndoLogContext *undo_log_ctx;

extern Size xundo_sys_shmem_size(void);
extern void xundo_sys_shmems_init(Pointer ptr, bool found);
extern void xstore_contexts_init(void);

extern FullTransactionId get_full_oldest_xmin(void);

#endif