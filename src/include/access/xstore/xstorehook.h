/* -------------------------------------------------------------------------
 *
 * xstorehook.h
 * the row format of inplace update engine.
 * 
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 * src/include/access/xstore/xstorehook.h
 * -------------------------------------------------------------------------
 *
 */

#ifndef XSTORE_HOOK_H
#define XSTORE_HOOK_H

#include "postgres.h"

#include "access/tableam.h"
#include "access/transam.h"
#include "access/xlogdefs.h"
#include "access/rmgr.h"
#include "access/twophase_rmgr.h"
#include "access/htup.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "postgres_ext.h"
#include "utils/relcache.h"

#define TABLE_ACCESS_METHOD_XSTORE "xstore"
#define DEFAULT_XSTORE_INDEX_TYPE "xbtree"

typedef struct XStoreTransHook {
	void (*SetHotStandbyFrozenXid)(FullTransactionId xid);
	void (*AllocateUndoLog)(void);
	void (*SetMyFrozenXmin)();
	TransactionId (*GetGlobalFrozenXmin)();
	FullTransactionId (*GetGlobalFrozenXid)();
	FullTransactionId (*GetGlobalRecycleXid)();
	void (*SetGlobalFrozenXid)(FullTransactionId xid);
	void (*SetGlobalRecycleXid)(FullTransactionId xid);
	void (*CheckPoint)(XLogRecPtr checkPointRedo);
	void (*RecoveryMeta)();
	void (*InitLocalUndoCtx)();
	bool (*IsXstoreRM)(RmgrId rmId);
    void (*ResolveRecoveryConflictWithGlobalFrozenXmin)(FullTransactionId latestRemovedFullXid);
} XStoreTransHook;

typedef struct XStoreAMHook {
	bool (*RelationIsXstoreFormat)(void* relation);
	bool (*RelationIsXstoreIndex)(void* relation);
	Size (*SizeOfXBTPageOpaqueData)();
	Oid (*GetXBtreeOid)();
	Oid (*GetXStoreOid)();
	Oid (*GetXBtreeBoolOpFamily)();
	bool (*SameOpfamilyForBtreeAndXBtree)(Oid btreeOpf, Oid	xbtreeOpf);
	bool (*TTSIsXStore)(/*TupleTableSlot*/void *slot);
	void* (*TTSOpsXHeapTupleAddr)();
	HeapTuple (*XHeapTupleToHeapTuple)(Relation rel,void *data);

	void (*ExecDeleteIndexTuples)(void *resultRelInfop, void *slot, void *tupleid,
								  void *estate, const void *modifiedIdxAttrs,
								  const bool inplaceUpdated, const bool isRollbackIndex);
	
	List* (*ExecInsertIndexTuplesGuts)(void *resultRelInfop, void *slot, void *index_state, 
									   bool update, bool noDupErr, bool *specConflict, void *arbiterIndexes,
									   void *modifiedIdxAttrs, bool inplaceUpdated);

	void (*ExecDeleteIndexTuplesGuts)(void *resultRelInfop, void* slot, void* index_state,
									  const void *modifiedIdxAttrsp, const bool inplaceUpdated);

	void (*PgStatRemove)(Relation rel);
} XStoreAMHook;

typedef struct XStoreXMultiHook {
	TwoPhaseCallback xmultixact_twophase_recover;
	TwoPhaseCallback xmultixact_twophase_postcommit;
	TwoPhaseCallback xmultixact_twophase_postabort;
	void (*AtPrepare_XMultiXact)(void);
	void (*PostPrepare_XMultiXact)(TransactionId xid);
	void (*AtEOXact_XMultiXact)(void);
	/* XMultiXactId was defined in xstore plugin, we use FullTransactionId here instead */
	void (*XMultiXactSetNextXMXact)(FullTransactionId nextXMulti, uint64 nextXMultiOffset);
	void (*XMultiXactGetCheckptXMulti)(FullTransactionId *nextXMulti, uint64 *nextXMultiOffset);
	void (*XMultiXactAdvanceNextXMXact)(FullTransactionId nextXMulti, uint64 nextXMultiOffset);
	void (*CheckPointXMultiXact)(void);
	void (*InitXMultiFiles)(void);
	void (*TrimXMultiXact)(void);
	void (*TruncateXMultiXact)(void);
} XStoreXMultiHook;

typedef struct XStoreHook {
	XStoreTransHook *transHook;
	XStoreAMHook *amHook;
	XStoreXMultiHook *xmultiHook;
} XStoreHook;


extern XStoreHook GlobalXStoreHook;

extern bool OidIsXBTree(Oid amoid);
extern bool IndexIsXBTree(void *relation);
extern bool RelationIsXstoreTable(void* relation);
extern bool TTSIsXStore(/*TupleTableSlot*/void *slot);
extern void* GetTTSOpsXHeapTupleAddr(void);
extern bool IsXBtreeBooleanOpfamily(Oid opf);
extern bool ParentIsXstoreFormat(Relation relation);

extern void
simple_xtuple_delete(Relation rel, ItemPointer tid,
		 ResultRelInfo *resultRelInfo, EState *estate);
extern void
simple_xtuple_update(ResultRelInfo *resultRelInfo, EState *estate,
                     Relation rel, ItemPointer otid,
					 TupleTableSlot *slot, TU_UpdateIndexes *update_indexes,
					 TM_FailureData* tmfd);
extern HeapTuple XHeapTupleToHeapTuple(Relation rel,void *data);
extern Bitmapset* RelationGetAttrBitmapByIndex(Relation relation, void *index_info);
#endif
