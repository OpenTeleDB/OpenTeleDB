/*-------------------------------------------------------------------------
 *
 * xstore.c
 *		Main file: setup shared memory, hooks and other general-purpose
 *		routines.
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 * contrib/xstore/src/xstore.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "postgres.h"
#include "fmgr.h"
#include "storage/procnumber.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/smgr.h"
#include "access/xlog_internal.h"
#include "access/transam.h"
#include "access/xstore/xstorehook.h"
#include "storage/sync.h"
#include "undo/undoxlog.h"
#include "undo/undofile.h"
#include "undo/undolog.h"
#include "undo/undoworker.h"
#include "xheap/xtupleslot.h"
#include "xheap/xheap_am.h"
#include "xheap/xredo.h"
#include "xheap/xtup_details.h"
#include "xbtree/xbtree.h"
#include "xbtree/xbtxlog.h"
#include "util/xxact.h"
#include "xheap/xstat.h"
#include "xheap/xmulti.h"
#include "access/xstore/xstorehook.h"
#include "xstore.h"
#include "postmaster/bgworker.h"
#include "utils/memutils.h"
#include "storage/procarray.h"
#include "util/xrmgr.h"
#include "executor/executor.h"
#include "utils/inval.h"
#include "catalog/index.h"

PG_MODULE_MAGIC;

void		_PG_init(void);


UndoSysContext *undo_sys_ctx = NULL;
UndoCacheContext *undo_cache_ctx = NULL;
UndoLogContext *undo_log_ctx = NULL;

pg_atomic_uint64 *MyFrozenXmins = NULL;


// user define guc
int undo_max_total_size = 33554432;		// unit blocksz 8K
int undo_max_size_per_transaction = 4194304;	// unit blocksz 8k


#define UNDO_HASH_KEY_SIZE sizeof(int)
#define UNDO_HASH_VALUE_SIZE sizeof(UndoLogControl)


/* Previous values of hooks to chain call them */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static void (*prev_shmem_request_hook) (void) = NULL;

static void xstore_shmem_request(void);

typedef struct
{
	Size (*shmem_size) (void);
	void (*shmem_init) (Pointer ptr, bool found);
} ShmemItem;

static ShmemItem shmemItems[] = {
	{undo_worker_shmem_size, undo_worker_shmem_init},
	{xundo_sys_shmem_size, xundo_sys_shmems_init},
	{xstore_prune_shmem_size, xstore_prune_stat_shmem_init},
	{XMultiShmemSize, XMultiXactShmemInit}
};

static RmgrData  undoRmgr = 
{
	.rm_name = "UndoLog",
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_redo = undo_xlog_redo,
	.rm_desc = undo_xlog_desc,
	.rm_identify = undo_xlog_type_name,
	.rm_mask = NULL,
	.rm_decode = NULL
};

static RmgrData  xheapRmgr = 
{
	.rm_name = "XHeap",
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_redo = xheap_redo,
	.rm_desc = xheap_desc,
	.rm_identify = xheap_type_name,
	.rm_mask = NULL,
	.rm_decode = decode_xheap_op
};

static RmgrData  xheapUndoRmgr = 
{
	.rm_name = "XHeapUndo",
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_redo = xheap_undo_redo,
	.rm_desc = xheap_undo_desc,
	.rm_identify = xheap_undo_type_name,
	.rm_mask = NULL,
	.rm_decode = NULL
};


static RmgrData  xbtreeRmgr = 
{
	.rm_name = "XBtree",
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_redo = xbtree_redo,
	.rm_desc = xbtree_desc,
	.rm_identify = xbtree_identify,
	.rm_mask = NULL,
	.rm_decode = NULL 
};

static RmgrData  xbtree2Rmgr = 
{
	.rm_name = "XBtree2",
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_redo = xbtree2_redo,
	.rm_desc = xbtree2_desc,
	.rm_identify = xbtree2_type_name,
	.rm_mask = NULL,
	.rm_decode = NULL 
};

static f_smgr undofileSmgr =
{
	.smgr_init = undofile_init,
	.smgr_shutdown = NULL,
	.smgr_open = undofile_open,
	.smgr_close = undofile_close,
	.smgr_create = undofile_create,
	.smgr_exists = undofile_exists,
	.smgr_unlink = NULL, // should direct call undofile_unlink,
	.smgr_extend = undofile_extend,
	.smgr_prefetch = NULL,
	.smgr_readv = undofile_read,
	.smgr_writev = undofile_write,
	.smgr_writeback = undofile_writeback,
	.smgr_nblocks = undofile_get_nblocks,
	.smgr_truncate = NULL,
	.smgr_immedsync = NULL,
	.smgr_is_own = undofile_is_own
};

static RmgrData xmultiMgr = 
{
	.rm_name = "XMulti",
	.rm_startup = NULL,
	.rm_cleanup = NULL,
	.rm_redo = xmultixact_redo,
	.rm_desc = xmultixact_desc,
	.rm_identify = xmultixact_identify,
	.rm_mask = NULL,
	.rm_decode = NULL
};

static SyncOps undofileSyncHandler =
{
	.sync_filetagmatches = NULL,
	.sync_syncfiletag = undofile_syncfiletag,
	.sync_unlinkfiletag = undofile_unlinkfiletag
};

static SyncOps XMultiOffsetFileSyncHandler = {
	.sync_filetagmatches = NULL,
	.sync_syncfiletag = xmultixactoffsetssyncfiletag,
	.sync_unlinkfiletag = NULL
};

static SyncOps XMultiMemberFileSyncHandler = {
	.sync_filetagmatches = NULL,
	.sync_syncfiletag = xmultixactmemberssyncfiletag,
	.sync_unlinkfiletag = NULL
};

/*
 * delete index tuples hook
 */
static void exec_delete_index_tuples(void *resultRelInfop, void *slot, void *tupleid,
								  void *estate, const void *modifiedIdxAttrs,
								  const bool inplaceUpdated, bool is_dead);

static List* exec_insert_index_tuples_guts(void *resultRelInfop, void* slot, void* index_state, 
                                       bool update, bool noDupErr, bool *specConflict,
                                       void *arbiterIndexes, void *modifiedIdxAttrs, 
					                   bool inplaceUpdated);

static void exec_delete_index_tuples_guts(void *resultRelInfop, void* slot, void* index_state,
                                      const void *modifiedIdxAttrsp, const bool inplaceUpdated);

/*
 * Estimate amount of shared memory required by xstore extension.
 */
static Size
xstore_memsize(void)
{
	Size size = 0;
	int	i;
	int count = sizeof(shmemItems) / sizeof(shmemItems[0]);

	for (i = 0; i < count; i++)
		size = add_size(size, CACHELINEALIGN(shmemItems[i].shmem_size()));

	return size;
}

/*
 * Request for shared memory and lwlocks
 */
static void
xstore_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(xstore_memsize());

	RequestNamedLWLockTranche(xstore_prune_state_tranche, NUM_STARTBLOCK_PARTITIONS);
	XMultiRequestNamedLWLockTranche();
}

/*
 * Initialize xstore's shared memory.  Called on database instanse start
 * or restart.
 */
static void
xstore_shmem_startup(void)
{
    Pointer	ptr;
	bool found;
	int	i;
	int count = sizeof(shmemItems) / sizeof(shmemItems[0]);

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
    
    /*
	 * We must hold AddinShmemInitLock while initilization of our shared
	 * memory.
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	ptr = ShmemInitStruct("xstore_enigne", xstore_memsize(), &found);
    for (i = 0; i < count; i++)
	{
		shmemItems[i].shmem_init(ptr, found);
		ptr += CACHELINEALIGN(shmemItems[i].shmem_size());
	}
    LWLockRelease(AddinShmemInitLock);
}

/* set xstore hook and call in CreateCheckPoint */
static void 
checkpoint_xstore(XLogRecPtr checkPointRedo)
{
	/* backup undo sys meta*/
	checkpoint_undo_meta(checkPointRedo);
}

/* set xstore hook and call in StartupXLOG */
static void 
recover_xstore()
{
	/*
	* initial contexts for xstore
	*/
	xstore_contexts_init();

	/* recovery undo sys meta*/
	recovery_undo_meta();
}

static void 
init_local_undo_ctx()
{
	/*
	* initial contexts for xstore
	*/
	xstore_contexts_init();

	/*
	* register exit clean up.
	*/
	on_shmem_exit(cleanup_undo_log, 0);
}



static bool 
is_xstore_rm(RmgrId rid)
{
	return rid>=RM_XHEAP_ID && rid<= RM_XUNDOLOG_ID;
}

static TransactionId
get_global_frozen_xmin(void)
{
	return XidFromFullTransactionId(FullTransactionIdFromU64(pg_atomic_read_u64(&(undo_sys_ctx->global_frozen_xmin))));
}

static void
set_my_frozen_xmin(void)
{
	uint64 globalFronzenXid;
	if (RecoveryInProgress())
		globalFronzenXid = pg_atomic_read_u64(&(undo_sys_ctx->hot_standby_frozen_xid));
	else
		globalFronzenXid = pg_atomic_read_u64(&(undo_sys_ctx->global_frozen_xid));

	pg_atomic_write_u64(&MyFrozenXmins[MyProcNumber], globalFronzenXid);
}

static FullTransactionId 
get_global_frozenXid(void)
{
	return FullTransactionIdFromU64(pg_atomic_read_u64(&(undo_sys_ctx->global_frozen_xid)));
} 

static FullTransactionId
get_global_recycle_xid(void)
{
	return FullTransactionIdFromU64(pg_atomic_read_u64(&(undo_sys_ctx->global_recycle_xid)));
}

static void
set_global_frozen_xid(FullTransactionId xid)
{
	pg_atomic_write_u64(&undo_sys_ctx->global_frozen_xid,xid.value);
}

static void
set_global_recycle_xid(FullTransactionId xid)
{
	pg_atomic_write_u64(&undo_sys_ctx->global_recycle_xid,xid.value);
}

static void
set_hot_standby_frozen_xid(FullTransactionId xid)
{
	pg_atomic_write_u64(&undo_sys_ctx->hot_standby_frozen_xid,xid.value);
}

static HeapTuple force_xheap_to_heap(Relation rel,void *data)
{
	XHeapTupleData xtuple;
	HeapTuple tuple;

	xtuple.disk_tuple = (XHeapDiskTupleData *)data;
	// copy from xheaptuple
    tuple = xheap_to_heap(rel,&xtuple);
	return tuple;
}



void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
		return;
    
    /* Verify that the given directory exists. If it does not exist, create it.*/
    /* Consider half init case. */

	/*init undo meta file*/
	init_undo_meta();

    /* Register guc. */
    DefineCustomBoolVariable("xstore.enable_reserve_space_for_null_atts", 
	                         "Enable reserve space for nullable attributes.",
                             NULL,
			                 &enable_reserve_space_for_null_atts,
			                 true,
			                 PGC_USERSET,
                             0,
                             NULL, 
                             NULL,
                             NULL);

	DefineCustomIntVariable("xstore.undo_max_total_size",
							"Undo total limit size for force discard ",
							NULL,
							&undo_max_total_size,
							33554432,
							1024,
							INT_MAX,
							PGC_POSTMASTER,
							GUC_UNIT_BLOCKS,
							NULL,
							NULL,
							NULL);	

	DefineCustomIntVariable("xstore.undo_max_size_per_transaction",
							"Set the max undo size for per transaction",
							NULL,
							&undo_max_size_per_transaction,
							4194304,
							256,
							INT_MAX,
							PGC_POSTMASTER,
							GUC_UNIT_BLOCKS,
							NULL,
							NULL,
							NULL);	

	DefineCustomIntVariable("xstore.undo_max_segno_per_log",
							"Set the max undo seg no for per undo log ,only for test",
							NULL,
							&undo_max_segno_per_log,
							-1,
							-1,
							67108863,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("xstore.undo_max_rollback_worker",
							"Sets the maximum number of undo rollback worker processes.",
							NULL,
							&undo_max_rollback_worker,
							5,
							1,
							MAX_ROLLBACK_WORKERS,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);											
    
    /* Setup the required hooks. */
    prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = xstore_shmem_request;

    prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = xstore_shmem_startup;

	/* setup trans hooks*/
	GlobalXStoreHook.transHook->CheckPoint = checkpoint_xstore;
	GlobalXStoreHook.transHook->RecoveryMeta = recover_xstore;
	GlobalXStoreHook.transHook->InitLocalUndoCtx = init_local_undo_ctx;
	GlobalXStoreHook.transHook->AllocateUndoLog = alloc_undo_log;
	GlobalXStoreHook.transHook->IsXstoreRM = is_xstore_rm;
	GlobalXStoreHook.transHook->GetGlobalFrozenXmin = get_global_frozen_xmin;
	GlobalXStoreHook.transHook->SetMyFrozenXmin = set_my_frozen_xmin;
	GlobalXStoreHook.transHook->GetGlobalFrozenXid = get_global_frozenXid;
	GlobalXStoreHook.transHook->GetGlobalRecycleXid = get_global_recycle_xid;
	GlobalXStoreHook.transHook->SetGlobalFrozenXid = set_global_frozen_xid;
	GlobalXStoreHook.transHook->SetGlobalRecycleXid = set_global_recycle_xid;
	GlobalXStoreHook.transHook->SetHotStandbyFrozenXid = set_hot_standby_frozen_xid;
	GlobalXStoreHook.transHook->ResolveRecoveryConflictWithGlobalFrozenXmin = resolve_recovery_conflict_with_global_frozen_xmin;

	/* setup am hooks*/
	GlobalXStoreHook.amHook->RelationIsXstoreFormat = relation_is_xstore_format;
	GlobalXStoreHook.amHook->RelationIsXstoreIndex = relation_is_xstore_index;
	GlobalXStoreHook.amHook->SizeOfXBTPageOpaqueData = size_of_xbtpage_opaque_data;
	GlobalXStoreHook.amHook->GetXBtreeOid = xbt_oid;
	GlobalXStoreHook.amHook->GetXBtreeBoolOpFamily = xbt_boolopsfamily;
	GlobalXStoreHook.amHook->SameOpfamilyForBtreeAndXBtree = same_opfamily_for_btree_and_xbtree;
	GlobalXStoreHook.amHook->GetXStoreOid = get_xstore_oid;
	GlobalXStoreHook.amHook->TTSIsXStore = table_slot_is_xstore;
	GlobalXStoreHook.amHook->TTSOpsXHeapTupleAddr = tts_xheap_tuple_addr;
	GlobalXStoreHook.amHook->ExecDeleteIndexTuples = exec_delete_index_tuples;
	GlobalXStoreHook.amHook->ExecDeleteIndexTuplesGuts = exec_delete_index_tuples_guts;
	GlobalXStoreHook.amHook->ExecInsertIndexTuplesGuts = exec_insert_index_tuples_guts;
	GlobalXStoreHook.amHook->XHeapTupleToHeapTuple = force_xheap_to_heap;
	GlobalXStoreHook.amHook->PgStatRemove = start_block_hash_table_remove;

	/* setup xmulti hooks */
	GlobalXStoreHook.xmultiHook->xmultixact_twophase_recover = xmultixact_twophase_recover;
	GlobalXStoreHook.xmultiHook->xmultixact_twophase_postcommit = xmultixact_twophase_postcommit;
	GlobalXStoreHook.xmultiHook->xmultixact_twophase_postabort = xmultixact_twophase_postabort;
	GlobalXStoreHook.xmultiHook->AtPrepare_XMultiXact = AtPrepare_XMultiXact;
	GlobalXStoreHook.xmultiHook->PostPrepare_XMultiXact = PostPrepare_XMultiXact;
	GlobalXStoreHook.xmultiHook->AtEOXact_XMultiXact = AtEOXact_XMultiXact;
	GlobalXStoreHook.xmultiHook->XMultiXactSetNextXMXact = XMultiXactSetNextXMXact;
	GlobalXStoreHook.xmultiHook->XMultiXactGetCheckptXMulti = XMultiXactGetCheckptXMulti;
	GlobalXStoreHook.xmultiHook->XMultiXactAdvanceNextXMXact = XMultiXactAdvanceNextXMXactWrapper;
	GlobalXStoreHook.xmultiHook->CheckPointXMultiXact = CheckPointXMultiXact;
	GlobalXStoreHook.xmultiHook->TrimXMultiXact = TrimXMultiXact;
	GlobalXStoreHook.xmultiHook->InitXMultiFiles = InitXMultiFiles;
	GlobalXStoreHook.xmultiHook->TruncateXMultiXact = TruncateXMultiXact;

    /* register rmgr*/
	RegisterCustomRmgr(RM_XHEAP_ID,&xheapRmgr);
	RegisterCustomRmgr(RM_XHEAPUNDO_ID,&xheapUndoRmgr); 
	RegisterCustomRmgr(RM_XBTREE_ID,&xbtreeRmgr);
	RegisterCustomRmgr(RM_XBTREE2_ID,&xbtree2Rmgr);
	RegisterCustomRmgr(RM_XUNDOLOG_ID, &undoRmgr);
	RegisterCustomRmgr(RM_XMULTIXACT_ID, &xmultiMgr);

	/* register sync handler*/
	undo_sync_handler_pos = RegisterCustomSyncHandler(&undofileSyncHandler);

	XMultiOffsetSyncHandlerPos = RegisterCustomSyncHandler(&XMultiOffsetFileSyncHandler);
	XMultiMemberSyncHandlerPos = RegisterCustomSyncHandler(&XMultiMemberFileSyncHandler);

	/* register smgr for undofile*/
	RegisterCustomSmgr(&undofileSmgr);

	/* register xact callbacks */
	register_xstore_xact_callbacks();

	/* launch background worker */
	undo_launcher_register();

	/* register undo discard worker */
	discard_worker_register();
}



Size
xundo_sys_shmem_size(void)
{
	Size size = 0;

	size = MAXALIGN64(sizeof(UndoSysContext));
	size += mul_size(MAXALIGN64(sizeof(UndoLogControl)), UNDOLOG_TOTAL_COUNT);
	size += mul_size(MAXALIGN64(sizeof(pg_atomic_uint64)), MAX_FROZEN_XMIN_NUM);

	return size;
}

void
xundo_sys_shmems_init(Pointer ptr, bool found)
{
	void *base;

	base = ptr;

	undo_sys_ctx = (UndoSysContext *) base;

	if (!found)
	{
		UndoPersistence up;
		undo_sys_ctx->undo_lock_tranche_id = LWLockNewTrancheId();
		LWLockInitialize(&undo_sys_ctx->undo_log_lock, undo_sys_ctx->undo_lock_tranche_id);
		for (int i = 0; i < UNDOLOG_TOTAL_COUNT; i++)
		{
			undo_sys_ctx->ulogs[i] =
				((char *) base + MAXALIGN64(sizeof(UndoSysContext)) +
				 MAXALIGN64(sizeof(UndoLogControl)) * i);
			GET_UPERSISTENCE_BY_LOGNO(up,i)
			init_undo_log((UndoLogControl *) undo_sys_ctx->ulogs[i], i,up);
		}

		pg_atomic_write_u32(&undo_sys_ctx->undo_total_size, 0);
		pg_atomic_write_u64(&undo_sys_ctx->global_frozen_xid, InvalidTransactionId);
		pg_atomic_write_u64(&undo_sys_ctx->global_recycle_xid, InvalidTransactionId);
		pg_atomic_write_u64(&undo_sys_ctx->hot_standby_frozen_xid, InvalidTransactionId);

		undo_sys_ctx->undo_meta_size = 0;

		MyFrozenXmins = (pg_atomic_uint64 *)((char *) base + MAXALIGN64(sizeof(UndoSysContext)) + 
					   MAXALIGN64(sizeof(UndoLogControl)) * UNDOLOG_TOTAL_COUNT);
		MemSet(MyFrozenXmins, 0, mul_size(MAXALIGN64(sizeof(pg_atomic_uint64)), MAX_FROZEN_XMIN_NUM));
	}
	set_undo_threshold();
}

void
xstore_contexts_init(void)
{
	MemoryContext oldctx;

	if (undo_cache_ctx != NULL)
	{
		return;
	}


	oldctx = MemoryContextSwitchTo(TopMemoryContext);
	undo_cache_ctx = (UndoCacheContext *) palloc0(sizeof(UndoCacheContext));

	undo_cache_ctx->undo_prepare_buffers = new_undo_prepare_buffers(MAX_UNDORECORDS_PER_OPERATION);

	for (int i = 0; i < MAX_UNDORECORDS_PER_OPERATION; i++)
	{
		undo_cache_ctx->undo_records[i] = new_undo_record();
		prepare_buffers_add_record(undo_cache_ctx->undo_prepare_buffers, undo_cache_ctx->undo_records[i]);
	}
	undo_cache_ctx->undo_buffer_idx = 0;
	undo_cache_ctx->undo_buffers =
		(UndoBuffer *) palloc0(MAX_UNDO_BUFFERS * sizeof(UndoBuffer));

	undo_log_ctx = (UndoLogContext *) palloc0(sizeof(UndoLogContext));

	for (int i = 0; i < UNDO_PERSISTENCE_LEVELS; i++)
	{
		UndoPersistence upersistence = (UndoPersistence) (i);
		undo_log_ctx->logs[upersistence] = INVALID_UNDOLOG_NO;
		undo_log_ctx->prev_xid[upersistence] = InvalidFullTransactionId;
		undo_log_ctx->slots[upersistence] = NULL;
		undo_log_ctx->slot_ptr[upersistence] = INVALID_UNDO_REC_PTR;
	}
	undo_log_ctx->curr_trans_undo_size = 0;

	MemoryContextSwitchTo(oldctx);
}

FullTransactionId get_full_oldest_xmin(void)
{
	TransactionId oldestXmin = GetOldestNonRemovableTransactionId(NULL);
	TransactionId	next_xid;
	uint32		epoch;


	Assert(AmStartupProcess() || IsUnderPostmaster);

	LWLockAcquire(XidGenLock, LW_SHARED);
	next_xid = XidFromFullTransactionId(TransamVariables->nextXid);
	epoch = EpochFromFullTransactionId(TransamVariables->nextXid);
	LWLockRelease(XidGenLock);

	/*
	 * If xid is numerically greater than next_xid, it has to be from the last
	 * epoch.
	 */
	if (unlikely(oldestXmin > next_xid))
		--epoch;

	return FullTransactionIdFromEpochAndXid(epoch, oldestXmin);
}

static List *
exec_insert_index_tuples_guts(void *resultRelInfop, void* slotp, void* index_statep, 
	                            bool update, bool noDupErr, bool *specConflict,
                                void *arbiterIndexesp, void *modifiedIdxAttrs, 
					            bool inplaceUpdated)
{
    ResultRelInfo* resultRelInfo = (ResultRelInfo*) resultRelInfop;
    TupleTableSlot* slot = (TupleTableSlot*) slotp;
    ExecIndexTuplesState* index_state = (ExecIndexTuplesState*) index_statep;
    List *arbiterIndexes = (List*) arbiterIndexesp;
	return ExecInsertIndexTuplesXbtree(resultRelInfo, slot, index_state->estate, update, noDupErr, specConflict, arbiterIndexes,
							(Bitmapset*)modifiedIdxAttrs, inplaceUpdated);
}

/*
 * Copied from ExecInsertIndexTuples
 */
static void 
exec_delete_index_tuples(void *resultRelInfop, void *slotp, void *tupleidp, void *estatep,
    const void *modifiedIdxAttrsp,
    const bool inplaceUpdated, bool is_dead)
{
    ResultRelInfo *resultRelInfo = (ResultRelInfo*) resultRelInfop;
    TupleTableSlot* slot = (TupleTableSlot*) slotp;
    ItemPointer tupleid = (ItemPointer) tupleidp;
    EState* estate = (EState*) estatep;
    const Bitmapset *modifiedIdxAttrs = (const Bitmapset*) modifiedIdxAttrsp;
    int numIndices;
    RelationPtr relationDescs;
    Relation heapRelation;
    IndexInfo** indexInfoArray;
    ExprContext* econtext = NULL;
    Datum values[INDEX_MAX_KEYS];
    bool isnull[INDEX_MAX_KEYS];

    numIndices = resultRelInfo->ri_NumIndices;
    if (numIndices == 0) {
        return;
    }

    if (slot->tts_nvalid == 0) {
        slot_getallattrs(slot);
    }

    if (slot->tts_nvalid == 0) {
        elog(ERROR, "no values in slot when trying to delete index tuple");
    }

    /*
     * Get information from the result relation info structure.
     */
    relationDescs = resultRelInfo->ri_IndexRelationDescs;
    indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
    heapRelation = resultRelInfo->ri_RelationDesc;

    /*
     * We will use the EState's per-tuple context for evaluating predicates
     * and index expressions (creating it if it's not already there).
     */
    econtext = GetPerTupleExprContext(estate);

    /* Arrange for econtext's scan tuple to be the tuple under test */
    econtext->ecxt_scantuple = slot;
    // actualheap = heapRelation;

    if (!relation_is_xstore_format(heapRelation))
        return;

    AcceptInvalidationMessages();

    /*
     * for each index, form and insert the index tuple
     */
    for (int i = 0; i < numIndices; i++) {
        Relation indexRelation = relationDescs[i];
        IndexInfo* indexInfo = NULL;
        Relation actualindex = NULL;

        if (indexRelation == NULL) {
            continue;
        }

        indexInfo = indexInfoArray[i];

        /* If the index is marked as read-only, ignore it */
        if (!indexInfo->ii_ReadyForInserts) {
            continue;
        }

        /* modifiedIdxAttrs != NULL means updating, not every index are affected */
        if (inplaceUpdated && modifiedIdxAttrs != NULL) {
            /* Collect attribute Bitmapset of this index, and compare with modifiedIdxAttrs */
            Bitmapset *indexattrs = RelationGetAttrBitmapByIndex(indexRelation, indexInfo);
            bool overlap = bms_overlap(indexattrs, modifiedIdxAttrs);

            bms_free(indexattrs);
            if (!overlap) {
                continue; /* related columns are not modified */
            }
        }

        actualindex = indexRelation;

		/* Check for partial index */
        if (indexInfo->ii_Predicate != NIL) {
            ExprState *predicate = NULL;

            /*
             * If predicate state not set up yet, create it (in the estate's
             * per-query context)
             */
            predicate = indexInfo->ii_PredicateState;
            if (predicate == NULL) {
                predicate = ExecPrepareExpr((Expr*)indexInfo->ii_Predicate, estate);
                indexInfo->ii_PredicateState = predicate;
            }

            /* Skip this index-update if the predicate isn't satisfied */
            if (!ExecQual(predicate, econtext)) {
                continue;
            }
        }

        /*
         * FormIndexDatum fills in its values and isnull parameters with the
         * appropriate values for the column(s) of the index.
         */
        FormIndexDatum(indexInfo, slot, estate, values, isnull);
        index_delete(actualindex, values, isnull, tupleid, is_dead);
    }
}

static 
void exec_delete_index_tuples_guts(void *resultRelInfop, void* slotp, void* index_statep,
                               const void *modifiedIdxAttrsp, const bool inplaceUpdated)
{
    ResultRelInfo *resultRelInfo = (ResultRelInfo*) resultRelInfop;
    ExecIndexTuplesState* index_state = (ExecIndexTuplesState*) index_statep;
    XHeapTupleTableSlot *oldSlot = (XHeapTupleTableSlot *)(resultRelInfo->ri_oldTupleSlot);

	exec_delete_index_tuples(resultRelInfo, oldSlot, &oldSlot->base.tts_tid, index_state->estate, modifiedIdxAttrsp, 
							inplaceUpdated, false);
}