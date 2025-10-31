/* -------------------------------------------------------------------------
 * undorequest.c
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/undo/undorequest.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "undo/undorequest.h"
#include "undo/undoworker.h"
#include "undo/undofetch.h"
#include "xbtree/xbtree.h"
#include "xbtree/xbtundo.h"
#include "xheap/xrel.h"
#include "xheap/xheapundo.h"
#include "utils/hsearch.h"
#include "utils/relfilenumbermap.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/shmem.h"


static HTAB *RollbackHT;
static uint32 RollbackNextBucket;

#define ROLLBACK_DELAY 10

static Relation undo_action_open_relation(Oid reloid);

static int	do_undo_actions(UndoList *ulist, int start_idx, int end_idx,
							FullTransactionId xid, Oid reloid, BlockNumber blkno);

static Size
get_rollback_hash_size(void)
{
	return (2 * MaxBackends);
}

Size
rollbackhash_shmem_size(void)
{
	Size		size = hash_estimate_size(get_rollback_hash_size(),
									   sizeof(RollbackHashEntry));

	return size;
}

void
rollbackhash_shmem_init(void)
{
	HASHCTL		info;

	info.keysize = sizeof(RollbackHashKey);
	info.entrysize = sizeof(RollbackHashEntry);
	info.hash = tag_hash;

	RollbackHT =
		ShmemInitHash(" Rollback Requests Hash", get_rollback_hash_size(),
					  get_rollback_hash_size(), &info,
					  HASH_ELEM | HASH_FUNCTION | HASH_FIXED_SIZE);
	RollbackNextBucket = 0;
}


static bool
is_rollback_ht_full()
{
	bool		result = false;
	Size		curr_size =
		(Size) hash_get_num_entries(RollbackHT);

	if (curr_size >= get_rollback_hash_size())
		result = true;

	return result;
}

bool
register_rollback_req(FullTransactionId xid, UndoRecPtr from_recptr, UndoRecPtr to_recptr, Oid dbid,
				   UndoSlotPtr slot_ptr)
{
	bool		found = false;
	bool		rc = true;
	RollbackHashEntry *entry = NULL;
	RollbackHashKey key;

	key.full_xid = xid;
	key.start_urec_ptr = from_recptr;

	LWLockAcquire(&undorollback_ctx.worker_shmem->lock, LW_EXCLUSIVE);

	if (unlikely(is_rollback_ht_full()))
	{
		rc = false;
		goto out;
	}

	entry = (RollbackHashEntry *) hash_search(
		RollbackHT, &key, HASH_ENTER, &found);

	if (!found)
	{
		entry->full_xid = xid;
		entry->start_urec_ptr = from_recptr;
		entry->end_urec_ptr = to_recptr;
		entry->dbid = dbid;
		entry->txn_slot_ptr = slot_ptr;
		entry->in_progress = false;
		entry->retry_delay = 0;

		ereport(DEBUG1, (errmsg("add RollbackRequest in hash "
								"xid %lu, startAddr %lu, toAddr %lu.",
								xid.value, from_recptr, to_recptr)));
	}

	/* Wake up the Undo Launcher */
	SetLatch(&undorollback_ctx.worker_shmem->latch);

out:
	LWLockRelease(&undorollback_ctx.worker_shmem->lock);
	return rc;
}


bool
remove_rollback_entry(FullTransactionId xid, UndoRecPtr start_recptr, pid_t pid)
{
	bool		found						 PG_USED_FOR_ASSERTS_ONLY = false;
	RollbackHashEntry *entry PG_USED_FOR_ASSERTS_ONLY = NULL;
	RollbackHashKey key;

	key.full_xid = xid;
	key.start_urec_ptr = start_recptr;

	LWLockAcquire(&undorollback_ctx.worker_shmem->lock, LW_EXCLUSIVE);
	entry = (RollbackHashEntry *) hash_search(
		RollbackHT, &key, HASH_REMOVE, &found);
	if (!found || !entry->in_progress)
		ereport(LOG, (errmsg("undoworker find rollback request result %d "
							 "maybe start worker more than once. "
							 "xid %lu, startAddr %lu, pid %d.",
							 found, xid.value, start_recptr, pid)));

	ereport(DEBUG1, (errmsg("remove RollbackRequest in hash "
							"xid %lu, startAddr %lu, pid %d. found:%d",
							xid.value, start_recptr, pid, found)));
	LWLockRelease(&undorollback_ctx.worker_shmem->lock);

	return true;
}

bool
mark_rollback_progress(FullTransactionId xid, UndoRecPtr start_recptr, pid_t pid, bool in_progress)
{
	bool		found PG_USED_FOR_ASSERTS_ONLY = false;
	RollbackHashEntry *entry PG_USED_FOR_ASSERTS_ONLY = NULL;
	RollbackHashKey key;

	key.full_xid = xid;
	key.start_urec_ptr = start_recptr;

	LWLockAcquire(&undorollback_ctx.worker_shmem->lock, LW_EXCLUSIVE);
	entry = (RollbackHashEntry *) hash_search(
		RollbackHT, &key, HASH_FIND, &found);
	if (!found)
	{
		if (in_progress)
			/* try to mark in progress but not exist */
			ereport(LOG, (errmsg("undowoker cannot find rollback request "
								 "xid %lu, startAddr %lu, pid %d.",
								 xid.value, start_recptr, pid)));
		LWLockRelease(&undorollback_ctx.worker_shmem->lock);
		return false;
	}

	entry->in_progress = in_progress;
	entry->retry_delay = ROLLBACK_DELAY;
	LWLockRelease(&undorollback_ctx.worker_shmem->lock);

	return true;
}

void
mark_rollback_fail(FullTransactionId xid, UndoRecPtr start_recptr, UndoRecPtr to_recptr, Oid dbid)
{
	bool		found = false;
	RollbackHashEntry *entry = NULL;
	RollbackHashKey key;

	key.full_xid = xid;
	key.start_urec_ptr = start_recptr;

	LWLockAcquire(&undorollback_ctx.worker_shmem->lock, LW_EXCLUSIVE);

	entry = (RollbackHashEntry *) hash_search(
		RollbackHT, &key, HASH_FIND, &found);

	Assert(found == true);
	Assert(entry->end_urec_ptr == to_recptr);
	Assert(entry->dbid == dbid);
	Assert(entry->in_progress == true);

	entry->in_progress = false;
	entry->retry_delay = ROLLBACK_DELAY;

	/* Wake up the Undo Launcher */
	SetLatch(&undorollback_ctx.worker_shmem->latch);

	LWLockRelease(&undorollback_ctx.worker_shmem->lock);
	ereport(LOG, (errmsg("reset RollbackRequest in hash for next retry"
						 "xid %lu, startAddr %lu, toAddr %lu. found:%d",
						 xid.value, start_recptr, to_recptr, found)));
}

RollbackHashEntry *
get_next_rollback_request()
{
	RollbackHashEntry *entry = NULL;
	HASH_SEQ_STATUS hash_seq;

	LWLockAcquire(&undorollback_ctx.worker_shmem->lock, LW_SHARED);
	hash_seq_init(&hash_seq, RollbackHT);
	hash_seq.curBucket = RollbackNextBucket;
	hash_seq.curEntry = NULL;

	while ((entry = (RollbackHashEntry *) hash_seq_search(&hash_seq)) != NULL)
	{
		if (!entry->in_progress)
			break;
	}

	if (entry == NULL)
		RollbackNextBucket = 0;
	else
	{
		RollbackNextBucket = hash_seq.curBucket;
		hash_seq_term(&hash_seq);
	}

	LWLockRelease(&undorollback_ctx.worker_shmem->lock);

	return entry;
}


/*
 * execute_undo_actions - Execute the undo actions
 *
 * full_xid - Transaction id that is getting rolled back.
 * from_urecptr - undo record pointer from where to start applying undo action.
 * to_urecptr   - undo record pointer upto which point apply undo action.
 * nopartial    - true if rollback is for complete transaction.
 *
 * return true done rollback ,false error happen.
 */
bool
execute_undo_actions(FullTransactionId full_xid, UndoRecPtr from_urecptr, UndoRecPtr to_urecptr,
				   UndoSlotPtr slot_ptr, bool nopartial)
{
	int			undo_apply_size = maintenance_work_mem * 1024L;
	UndoRecPtr	urec_ptr = from_urecptr;
	int			rb_result = ROLLBACK_ROK;
	bool		has_switch = false;

	Assert(to_urecptr != INVALID_UNDO_REC_PTR && from_urecptr != INVALID_UNDO_REC_PTR);
	Assert(slot_ptr != INVALID_UNDO_REC_PTR);
	Assert(FullTransactionIdIsValid(full_xid));

	if (nopartial)
	{
		UndoTraversalState rc;
		UnpackedUndoRecord *urec = new_undo_record();

		urec->uur_urp = to_urecptr;
		/*
		 * It is important here to fetch the latest undo record and validate
		 * if the actions are already executed.  The reason is that it is
		 * possible that discard worker or backend might try to execute the
		 * rollback request which is already executed.  For ex., after discard
		 * worker fetches the record and found that this transaction need to
		 * be rolledback, backend might concurrently execute the actions and
		 * remove the request from rollback hash table. The similar problem
		 * can happen if the discard worker first pushes the request, the undo
		 * worker processed it and backend tries to process it some later
		 * point.
		 */
		rc = fetch_undo_record(urec,
							InvalidFullTransactionId, false, NULL, NULL);
		/* already processed. */
		if (rc != UNDO_TRAVERSAL_COMPLETE)
		{
			destroy_undo_record(urec);
			return true;
		}
		destroy_undo_record(urec);
	}
	/*
	 * Fetch the multiple undo records which can fit into uur_segment; sort
	 * them in order of reloid and block number then apply them together
	 * page-wise. Repeat this until we get invalid undo record pointer.
	 */
	do
	{
		int			start_index = 0;
		Oid			pre_reloid = InvalidOid;
		Oid			pre_relfilenode = InvalidOid;
		Oid			pre_tablespace = InvalidOid;
		BlockNumber pre_blk = InvalidBlockNumber;
		int			i = 0;
		UndoList   *ulist = NULL;

		/*
		 * If urec_ptr is not valid means we have complete all undo actions
		 * for this transaction, otherwise we need to fetch the next batch of
		 * the undo records.
		 */
		if (!IS_VALID_UNDO_REC_PTR(urec_ptr))
			break;

		/*
		 * Fetch multiple undo record in bulk.  This will return the array of
		 * undo record which will holds undo record pointers and the pointers
		 * to the actual unpacked undo record.
		 */
		ulist = bulk_fetch_undo_for_range(&urec_ptr, to_urecptr, undo_apply_size);
		if (ulist->uurec_size == 0)
			break;

		qsort_undo_list(ulist);
		for (i = 0; i < ulist->uurec_size; i++)
		{
			UnpackedUndoRecord *uur = get_urec_from_list(ulist, i);

			if (pre_relfilenode != InvalidOid &&
				(pre_tablespace != GetUndoRecordTablespace(uur) ||
				 pre_relfilenode != GetUndoRecordRelfilenode(uur) ||
				 pre_blk != GetUndoRecordBlkno(uur)))
			{
				rb_result = do_undo_actions(
					ulist, start_index, i - 1, full_xid, pre_reloid,
					pre_blk);
				start_index = i;
				if (ROLLBACK_RSWITCH == rb_result)
					has_switch = true;
			}
			pre_reloid = GetUndoRecordReloid(uur);
			pre_blk = GetUndoRecordBlkno(uur);
			pre_relfilenode = GetUndoRecordRelfilenode(uur);
			pre_tablespace = GetUndoRecordTablespace(uur);
		}

		/* Apply the remaining undo records */
		rb_result = do_undo_actions(
			ulist, start_index, i - 1, full_xid, pre_reloid, pre_blk);

		if (ROLLBACK_RSWITCH == rb_result)
			has_switch = true;

		destroy_undo_list(ulist);
		if (has_switch)
			break;
	} while (true);

	if (nopartial)
	{
		if (!has_switch)
			set_undo_slot_rollback_finish(slot_ptr);
	}

	if (has_switch)
		return false;

	return true;
}

/*
 * execute_undo_actions_tuple
 *
 * This is similar to execute_undo_actions but wont rollback the entire the
 * transaction but will only rollback all changes made by fxid on the given
 * buffer.
 *
 * from_recptr  - UndoRecPtr from where to start applying the undo.
 * rel      - relation descriptor for which undo to be applied.
 * buffer   - buffer for which undo to be processed.
 * xid     - aborted transaction id whose effects needs to be reverted.
 */
void
execute_undo_actions_tuple(UndoRecPtr from_recptr, Relation rel, Buffer buffer, FullTransactionId xid)
{
	UndoRecPtr	urp = from_recptr;
	int			undo_apply_size = maintenance_work_mem * 1024L;

	do
	{
		UndoList   *ulist = NULL;
		Oid			rel_oid;

		if (!IS_VALID_UNDO_REC_PTR(urp))
			break;

		ulist = bulk_fetch_undo_for_tuple(&urp, undo_apply_size);

		if (ulist->uurec_size == 0)
			break;

		rel_oid = RelationGetRelid(rel);

		do_undo_actions(
			ulist, 0, ulist->uurec_size - 1, xid, rel_oid,
			BufferGetBlockNumber(buffer));

		destroy_undo_list(ulist);
	} while (true);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
}

int
do_undo_actions(UndoList *ulist, int startIdx, int endIdx, FullTransactionId xid,
				 Oid reloid, BlockNumber blkno)
{
	int			res;
	Relation	relation;
	UnpackedUndoRecord *firstUndoRecord = NULL;
#ifdef USE_ASSERT_CHECKING
	UnpackedUndoRecord *lastUndoRecord = NULL;
#endif
	Oid			relfilenode;
	Oid			tablespace;

	firstUndoRecord = get_urec_from_list(ulist, startIdx);
	relfilenode = GetUndoRecordRelfilenode(firstUndoRecord);
	tablespace = GetUndoRecordTablespace(firstUndoRecord);

#ifdef USE_ASSERT_CHECKING
	lastUndoRecord = get_urec_from_list(ulist, endIdx);
	Assert(relfilenode == GetUndoRecordRelfilenode(lastUndoRecord));
	Assert(tablespace == GetUndoRecordTablespace(lastUndoRecord));
#endif

	/*
	 * We always try to lock the relation.  If the relation is already gone,
	 * then we can skip processing the undo actions.
	 */
	relation = undo_action_open_relation(reloid);
	if (!relation)
	{
		elog(LOG, "Either the relation is dropped or temporary relation.");
		return ROLLBACK_ROK;
	}

	/*
	 * When a transaction does a DDL, it is possible that relid maps to the
	 * old relfilenode during rollback and hence it does not match the
	 * relfilenode in the undorecord anymore. But we know we need to do the rollback
	 * on the relfilenode pointed to by the undorecord because that's where we did the operation.
	 * In this case, we need to find the correct relid that maps to this relfilenode.
	 */
	if (RelationGetRelFileLocator(relation) != relfilenode ||
		RelationGetRnodeSpace(relation) != tablespace)
	{
		elog(LOG,
			 "The open relation does not match undorecord. expected:(%d/%d) "
			 "actual:(%d/%d) for block: %d",
			 RelationGetRnodeSpace(relation),
			 RelationGetRelFileLocator(relation), tablespace, relfilenode,
			 blkno);

		/* close the recently opened relation before opening a new one */
		relation_close(relation, RowExclusiveLock);

		/* Find the relid mapped to the relfilenode data. */
		reloid = RelidByRelfilenumber(tablespace, relfilenode);
		if (reloid == InvalidOid)
		{
			elog(LOG, "The relation is dropped");
			return ROLLBACK_ROK;
		}

		/* Now try opening the relation using the "correct" relid */
		relation = undo_action_open_relation(reloid);
		if (!relation)
		{
			elog(LOG, "Either the relation is dropped after retry.");
			return ROLLBACK_ROK;
		}
	}

	/*
	 * This is possible if the underlying relation is truncated just
	 * before taking the relation lock above.
	 */
	if (RelationGetNumberOfBlocks(relation) <= blkno)
	{
		relation_close(relation, RowExclusiveLock);
		elog(LOG, "relation is already truncated.");
		return ROLLBACK_ROK;
	}

	if (relation_is_xstore_index(relation))
		res = execute_xbtree_undoactions(ulist, startIdx, endIdx, blkno, relation);
	else
		res = execute_xheap_undoactions(ulist, startIdx, endIdx, blkno, relation);

	relation_close(relation, RowExclusiveLock);
	return res;
}

static Relation
undo_action_open_relation(Oid reloid)
{
	Relation	rel = try_relation_open(reloid, RowExclusiveLock);

	if (rel == NULL)
		return rel;

	if (RELATION_IS_OTHER_TEMP(rel))
	{
		relation_close(rel, RowExclusiveLock);
		ereport(DEBUG1, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("temporary tables do not need to be "
								"rolled back after the session exits.")));
		return NULL;
	}
	return rel;
}