/* -------------------------------------------------------------------------
 *
 * xstorehook.c
 * the row format of inplace update engine.
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 * src/include/access/xstore/xstorehook.c
 * -------------------------------------------------------------------------
 *
 */

#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_inherits.h"
#include "access/xstore/xstorehook.h"
#include "port/atomics.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "nodes/execnodes.h"
#include "optimizer/optimizer.h"

static XStoreTransHook XStoreTransHookFuncs = {
    .SetHotStandbyFrozenXid = NULL,
    .SetMyFrozenXmin = NULL,
    .GetGlobalFrozenXmin = NULL,
    .AllocateUndoLog = NULL,
    .CheckPoint = NULL,
    .RecoveryMeta =NULL,
    .InitLocalUndoCtx =NULL,
    .IsXstoreRM = NULL,
    .ResolveRecoveryConflictWithGlobalFrozenXmin = NULL
};

static XStoreAMHook XStoreAMHookFuncs = {
    .RelationIsXstoreFormat = NULL,
    .RelationIsXstoreIndex = NULL,
    .SizeOfXBTPageOpaqueData = NULL,
    .GetXBtreeOid = NULL,
    .GetXStoreOid = NULL,
    .GetXBtreeBoolOpFamily = NULL,
    .SameOpfamilyForBtreeAndXBtree = NULL,
    .TTSIsXStore = NULL,
    .TTSOpsXHeapTupleAddr = NULL,
    .XHeapTupleToHeapTuple = NULL,
    .ExecDeleteIndexTuples = NULL,
    .ExecInsertIndexTuplesGuts = NULL,
    .ExecDeleteIndexTuplesGuts = NULL
};

static XStoreXMultiHook XStoreXMultiHookFuncs = {
    .xmultixact_twophase_recover = NULL,
	.xmultixact_twophase_postcommit = NULL,
	.xmultixact_twophase_postabort = NULL,
	.AtPrepare_XMultiXact = NULL,
	.PostPrepare_XMultiXact = NULL,
	.AtEOXact_XMultiXact = NULL,
    .XMultiXactSetNextXMXact = NULL,
    .XMultiXactGetCheckptXMulti = NULL,
    .XMultiXactAdvanceNextXMXact = NULL,
    .CheckPointXMultiXact = NULL,
    .InitXMultiFiles = NULL,
    .TrimXMultiXact = NULL,
    .TruncateXMultiXact = NULL,
};

XStoreHook GlobalXStoreHook = {
    .transHook = &XStoreTransHookFuncs,
	.amHook = &XStoreAMHookFuncs,
    .xmultiHook = &XStoreXMultiHookFuncs
};

bool OidIsXBTree(Oid amoid)
{
    if (GlobalXStoreHook.amHook->GetXBtreeOid == NULL)
        return false;

    return amoid == GlobalXStoreHook.amHook->GetXBtreeOid();
}

bool IndexIsXBTree(void *relation)
{
    return (GlobalXStoreHook.amHook->RelationIsXstoreIndex != NULL) && 
           (GlobalXStoreHook.amHook->RelationIsXstoreIndex(relation));
}

bool RelationIsXstoreTable(void* relation)
{
    return (GlobalXStoreHook.amHook->RelationIsXstoreFormat != NULL) &&
        (GlobalXStoreHook.amHook->RelationIsXstoreFormat(relation));
}

bool TTSIsXStore(void *slot)
{
    return (GlobalXStoreHook.amHook->TTSIsXStore != NULL) &&
        (GlobalXStoreHook.amHook->TTSIsXStore(slot));
}

void* GetTTSOpsXHeapTupleAddr(void)
{
    if (GlobalXStoreHook.amHook->TTSOpsXHeapTupleAddr != NULL)
        return GlobalXStoreHook.amHook->TTSOpsXHeapTupleAddr();
    return NULL;
}

bool IsXBtreeBooleanOpfamily(Oid opf)
{
    if (GlobalXStoreHook.amHook->GetXBtreeBoolOpFamily == NULL)
        return false;

    return opf == GlobalXStoreHook.amHook->GetXBtreeBoolOpFamily();
}

/*
 * ParentIsXstoreFormat
 *   relation: parent relationData
 *
 * Parent relation has the same storage engine type with child partition,
 * which means parent is xstore if any child partition relation is xstore.
 */
bool
ParentIsXstoreFormat(Relation relation)
{
	List		*partitionIdList = NIL;
	ListCell	*partitionCell = NULL;
	Oid			partitionId;
	Relation	heapPartRel = NULL;
	bool		isXStore = false;

	partitionIdList = find_all_inheritors(RelationGetRelid(relation), NoLock, NULL);
	if (list_length(partitionIdList) == 0)
		return false;

	foreach (partitionCell, partitionIdList)
	{
		partitionId = lfirst_oid(partitionCell);
		heapPartRel = table_open(partitionId, ShareLock);
		if (heapPartRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			table_close(heapPartRel, ShareLock);
			continue;
		}
		isXStore = RelationIsXstoreTable(heapPartRel);
		table_close(heapPartRel, ShareLock);
		if (isXStore)
			return true;
	}
	return false;
}

#ifdef USE_XSTORE
// same function as simple_table_tuple_delete ,adding delete indexs 
void
simple_xtuple_delete(Relation rel, ItemPointer tid, 
                 ResultRelInfo *resultRelInfo, EState *estate)
{
    TM_Result	result;
	TM_FailureData tmfd;
    Snapshot snapshot = estate->es_snapshot;

    if (!(GetTTSOpsXHeapTupleAddr() && RelationIsXstoreTable(rel) ))
    {
        // should not reach here
        elog(ERROR, "relation is not xstore table, should use simple_table_tuple_delete ...");
    }

    if ( rel->rd_indexlist != NIL)
    { // now need a oldslot to store oldtuple.
        tmfd.oldslot = MakeSingleTupleTableSlot(rel->rd_att, GetTTSOpsXHeapTupleAddr());
    } else
    {
        tmfd.oldslot = NULL;
    }

	result = table_tuple_delete(rel, tid,
								GetCurrentCommandId(true),
								snapshot, InvalidSnapshot,
								true /* wait for commit */ ,
								&tmfd, false /* changingPart */ );

	switch (result)
	{
		case TM_SelfModified:
			/* Tuple was already updated in current command? */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* done successfully */

            if (tmfd.oldslot != NULL && TTSIsXStore(tmfd.oldslot)) {
                Assert(GlobalXStoreHook.amHook->ExecDeleteIndexTuples != NULL);
                GlobalXStoreHook.amHook->ExecDeleteIndexTuples(resultRelInfo, tmfd.oldslot, &tmfd.oldslot->tts_tid, estate, NULL, false, false);
                ExecDropSingleTupleTableSlot(tmfd.oldslot);
                tmfd.oldslot = NULL;
            }

			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized table_tuple_delete status: %u", result);
			break;
	}
}
#endif

#ifdef USE_XSTORE
/*
 * same as simple_table_tuple_update - to handle xtuple update
 */
void
simple_xtuple_update( ResultRelInfo *resultRelInfo, EState *estate,
                      Relation rel, ItemPointer otid,
					  TupleTableSlot *slot, TU_UpdateIndexes *update_indexes,
					  TM_FailureData* tmfd)
{
	TM_Result	result;
	LockTupleMode lockmode;
    Snapshot snapshot = estate->es_snapshot;

    if(!(GetTTSOpsXHeapTupleAddr() &&
		RelationIsXstoreTable(rel) ))
    {
        elog(ERROR, "relation is not xstore table, should use simple_table_tuple_update");
    }

    // If resultRelInfo->ri_oldTupleSlot is invalid, but we need old tuples to delete xbtree index.
	// Make a xheap slot to resultRelInfo->ri_oldTupleSlot, and set to tmfd.oldslot.
	// The tmfd.oldslot will get the old tuple from xheap_update.
	if (resultRelInfo->ri_RelationDesc->rd_indexlist != NIL &&
		(resultRelInfo->ri_oldTupleSlot == NULL || TTS_EMPTY(resultRelInfo->ri_oldTupleSlot)))
	{
		if (resultRelInfo->ri_oldTupleSlot == NULL)
		{
			resultRelInfo->ri_oldTupleSlot = table_slot_create(resultRelInfo->ri_RelationDesc,
						  &estate->es_tupleTable);
		}
		tmfd->oldslot = resultRelInfo->ri_oldTupleSlot;
	}
	else
	{
		tmfd->oldslot = NULL;
	}

	result = table_tuple_update(rel, otid, slot,
								GetCurrentCommandId(true),
								snapshot, InvalidSnapshot,
								true /* wait for commit */ ,
								tmfd, &lockmode, update_indexes);

	switch (result)
	{
		case TM_SelfModified:
			/* Tuple was already updated in current command? */
			elog(ERROR, "tuple already updated by self");
			break;

		case TM_Ok:
			/* done successfully */
			break;

		case TM_Updated:
			elog(ERROR, "tuple concurrently updated");
			break;

		case TM_Deleted:
			elog(ERROR, "tuple concurrently deleted");
			break;

		default:
			elog(ERROR, "unrecognized table_tuple_update status: %u", result);
			break;
	}
}
#endif
HeapTuple XHeapTupleToHeapTuple(Relation rel,void *data)
{
    if (GlobalXStoreHook.amHook->XHeapTupleToHeapTuple == NULL)
        return NULL;
    return GlobalXStoreHook.amHook->XHeapTupleToHeapTuple(rel, data); 
}
/*
 * IndexGetAttrBitmap -- get a bitmap of the given index's attribute columns
 *
 * The result has a bit set for each attribute used in the index.
 *
 * Caller had better hold at least RowShareLock on the index to ensure that
 * the index columns won't be changed by DDL statements.
 *
 * The returned result is palloc'd in the caller's memory context and should
 * be bms_free'd when not needed anymore.
 */
Bitmapset* RelationGetAttrBitmapByIndex(Relation relation, void *index_info)
{
	IndexInfo *indexInfo = (IndexInfo *)index_info;
	Bitmapset* indexattrs = NULL;
	MemoryContext oldcxt;

	/* make sure relation is a index relation */
	Assert(relation->rd_rel->relam != 0);

	/*
	 * Quick exit if we already computed the result.
	 *      Note: this field is shared with heap relation.
	 */
	if (relation->rd_hotblockingattr != NULL) {
		return bms_copy(relation->rd_hotblockingattr);
	}

	/* Collect simple attribute references */
	for (int i = 0; i < indexInfo->ii_NumIndexAttrs; i++) {
		int attrnum = indexInfo->ii_IndexAttrNumbers[i];
		/*
		 * Since we have covering indexes with non-key columns, we must
		 * handle them accurately here. non-key columns must be added into
		 * indexattrs, since they are in index, and HOT-update shouldn't
		 * miss them.
		 */
		if (attrnum != 0) {
			indexattrs = bms_add_member(indexattrs, attrnum - FirstLowInvalidHeapAttributeNumber);
		}
	}

	/* Collect all attributes used in expressions, too */
	pull_varattnos((Node*)indexInfo->ii_Expressions, 1, &indexattrs);

	/* Collect all attributes in the index predicate, too */
	pull_varattnos((Node*)indexInfo->ii_Predicate, 1, &indexattrs);

	/* Now save copies of the bitmaps in the relcache entry */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_hotblockingattr = bms_copy(indexattrs);
	(void)MemoryContextSwitchTo(oldcxt);

	return indexattrs;
}

