
/*
 * ---------------------------------------------------------------------------------------
 *
 * xxact.c
 * Implementation of xstore xact.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of C
 *
 * IDENTIFICATION
 *        src/xheap/xxact.c
 * ---------------------------------------------------------------------------------------
 */

#include "xstore.h"
#include "util/xxact.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/clog.h"
#include "undo/undolog.h"
#include "undo/undorequest.h"
#include "miscadmin.h"
#include "utils/portal.h"
#include "utils/snapmgr.h"
#include "storage/procarray.h"
#include "undo/undotxn.h"

extern pg_atomic_uint64 *MyFrozenXmins;
static bool executeSubxactUndo = false;

static void try_execute_undo_actions(TransactionState s, UndoPersistence pLevel);
static void clear_my_global_frozen_xmin(void);

void
register_xstore_xact_callbacks()
{
	RegisterXactCallback(xstore_xact_callback, NULL);
	RegisterSubXactCallback(xstore_sub_xact_callback, NULL);
}


static void 
release_undo_buffer_in_top_resource_owner(void)
{
    ResourceOwner save = CurrentResourceOwner;
    CurrentResourceOwner = TopTransactionResourceOwner;

    release_cache_buffers_in_ctx();
	release_undo_slot_buffers();

    CurrentResourceOwner = save;
}

/*
 * The entry point for xstore xact callbacks. We call RegisterXactCallback this
 * function to the xact callbacks and it will will be called after the
 * transaction committing or aborting. We will apply the undo actions and clean
 * the undo context in this transaction.
 */
void 
xstore_xact_callback(XactEvent event, void *arg) 
{
	switch (event) 
	{
		case XACT_EVENT_COMMIT:
	    case XACT_EVENT_PARALLEL_COMMIT:
        case XACT_EVENT_PARALLEL_ABORT:
			reset_undo_actions_info();
            clear_my_global_frozen_xmin();
            release_undo_buffer_in_top_resource_owner();
			break;

		case XACT_EVENT_ABORT:
			/* apply undo actions when the transaction abortin */
			apply_undo_actions();
			reset_undo_actions_info();
            clear_my_global_frozen_xmin();
            release_undo_buffer_in_top_resource_owner();
			break;

		default:
			break;	
	}
}


/*
 * The entry point for xstore sub xact callbacks. Just like XstoreXactCallBack but used for sub transaction.
 */
void 
xstore_sub_xact_callback(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	switch (event) 
	{
		case SUBXACT_EVENT_PRE_COMMIT_SUB:
            /*
             * Make parents kown the undo ptr for its sub transactions, so parent can rollback
             * its sub transactions 
             */
            proprgate_undo_info_to_parents();
			break;

		case SUBXACT_EVENT_ABORT_SUB:
			apply_undo_actions();
			break;

        case SUBXACT_EVENT_START_SUB:
            reset_undo_actions_info();
            reset_current_sub_transaction_lock();
            break;

		default:
			break;
	}
}

void 
apply_undo_actions(void)
{
    TransactionState s = GetCurrentTransactionSate();
	int i = 0;

    if (executeSubxactUndo) {
        ereport(WARNING, (errmsg("Failed to execute undo for subxact, rollback of the entire transaction will be done by "
                "asynchronous rollback or page-level rollback. Remark info, firstUrp(%lu,%lu,%lu), "
                "lastestUrp(%lu,%lu,%lu), lastestXactUrp(%lu,%lu,%lu).",
                s->first_urp[0], s->first_urp[1], s->first_urp[UNDO_PERSISTENCE_LEVELS - 1],
                s->latest_urp[0], s->latest_urp[1], s->latest_urp[UNDO_PERSISTENCE_LEVELS - 1],
                s->latest_urp_xact[0], s->latest_urp_xact[1], s->latest_urp_xact[UNDO_PERSISTENCE_LEVELS - 1])));
        return;
    }

    /*
     * State should still be TRANS_ABORT from AbortTransaction().
     */
    if (s->state != TRANS_ABORT)
        elog(FATAL, "ApplyUndoActions: unexpected state %s", TransStateAsString(s->state));

    /*
     * We promote the error level to FATAL if we get an error while applying
     * undo for the subtransaction.  See errstart.  So, we should never reach
     * here for such a case.
     */
    Assert(!executeSubxactUndo);

    /*
     * Do abort cleanup processing before applying the undo actions.  We must
     * do this before applying the undo actions to remove the effects of
     * failed transaction.
     */
    if (IsSubTransaction()) {
        AtSubCleanup_Portals(s->subTransactionId);
        executeSubxactUndo = true;

        /* We can't afford to allow cancel of subtransaction's rollback. */
        HOLD_CANCEL_INTERRUPTS();
    } else {
        AtCleanup_Portals();      /* now safe to release portal memory */
        //AtEOXact_Snapshot(false, true); /* and release the transaction's snapshots */
        s->fullTransactionId = InvalidFullTransactionId;
        s->subTransactionId = TopSubTransactionId;
    }

    s->state = TRANS_UNDO;

    for (i = 0; i < UNDO_PERSISTENCE_LEVELS; i++) {
        if (s->latest_urp[i]) {
            try_execute_undo_actions(s, (UndoPersistence)i);
        }
    }

    /* Reset undo information */
    reset_undo_actions_info();
    executeSubxactUndo = false;

    /* Release the locks after applying undo actions. */
    if (IsSubTransaction()) {
        RESUME_CANCEL_INTERRUPTS();
    }

    /*
     * Here we again put back the transaction in abort state so that callers
     * can proceed with the cleanup work.
     */
    s->state = TRANS_ABORT;
}

void 
reset_undo_actions_info(void)
{
	int i = 0;

	TransactionState s = GetCurrentTransactionSate();

    for (i = 0; i < UNDO_PERSISTENCE_LEVELS; i++) 
    {
        s->first_urp[i] = INVALID_UNDO_REC_PTR;
        s->latest_urp[i] = INVALID_UNDO_REC_PTR;
        s->latest_urp_xact[i] = INVALID_UNDO_REC_PTR;
    }
}

static void 
try_execute_undo_actions(TransactionState s, UndoPersistence pLevel)
{
    uint32 saveHoldoff;
    bool error;
    UndoSlot *slot;
	MemoryContext currentContext;

    if (!(IsSubTransaction() || pLevel == UNDO_TEMP || pLevel == UNDO_UNLOGGED)) 
    {
        return;
    }

    saveHoldoff = InterruptHoldoffCount;
    error = false;
    slot = (UndoSlot *)undo_log_ctx->slots[pLevel];

    Assert(slot->xid.value != InvalidTransactionId);
    Assert(slot->dbOid == MyDatabaseId);

    currentContext = CurrentMemoryContext;

    PG_TRY();
    {
        UndoSlotPtr slotPtr = undo_log_ctx->slot_ptr[pLevel];
        execute_undo_actions(slot->xid, s->latest_urp[pLevel], s->first_urp[pLevel],
            slotPtr, !IsSubTransaction());
    }
    PG_CATCH();
    {
        (void)MemoryContextSwitchTo(currentContext);
        if (pLevel == UNDO_TEMP || pLevel == UNDO_UNLOGGED) {
            //PgRethrowAsFatal();
			PG_RE_THROW();
        }
        elog(LOG,
            "[ApplyUndoActions:] Error occured while executing undo actions "
            "TransactionId: %lu, latest_urp: %ld, dbid: %d",
            slot->xid.value, s->latest_urp[pLevel], MyDatabaseId);

        /*
         * Errors can reset holdoff count, so restore back.  This is
         * required because this function can be called after holding
         * interrupts.
         */
        InterruptHoldoffCount = saveHoldoff;

        /* Send the error only to server log. */
        EmitErrorReport();

        error = true;

        /*
         * We promote the error level to FATAL if we get an error
         * while applying undo for the subtransaction.  See errstart.
         * So, we should never reach here for such a case.
         */
        Assert(!executeSubxactUndo);
        FlushErrorState();
    }
    PG_END_TRY();

    if (error) {
        /*
         * This should take care of releasing the locks held under
         * TopTransactionResourceOwner.
         */
        AbortTransaction();
    }
}

void
set_current_tansaction_undorec_ptr(UndoRecPtr urecPtr, UndoPersistence upersistence)
{
	TransactionState s = GetCurrentTransactionSate();

	Assert(IS_VALID_UNDO_REC_PTR(urecPtr));
	if (s->first_urp[upersistence] == INVALID_UNDO_REC_PTR) {
		s->first_urp[upersistence] = urecPtr;
	}

	s->latest_urp[upersistence] = urecPtr;
	s->latest_urp_xact[upersistence] = urecPtr;
}

UndoRecPtr
get_current_tansaction_undorec_ptr(UndoPersistence upersistence)
{
	TransactionState s = GetCurrentTransactionSate();
	return s->latest_urp_xact[upersistence];
}

bool 
has_current_sub_transaction_lock()
{
    TransactionState s = GetCurrentTransactionSate();
    return s->subXactLock;
}

void 
set_current_sub_transaction_locked()
{
    TransactionState s = GetCurrentTransactionSate();
    s->subXactLock = true;
}

void
reset_current_sub_transaction_lock()
{
    TransactionState s = GetCurrentTransactionSate();
    s->subXactLock = false;
}

/*
 * For xstore, clog is truncated based on globalFrozenXid. Therefore, we need to perform a quick check
 * first. If the transaction is smaller than globalFrozenXid, the transaction must be committed.
 *
 *      true iff given transaction committed
 */
bool 
transaction_id_did_commit_with_hint(FullTransactionId transactionId, int* hint_status)
{
    if (hint_status != NULL)
    {
        if (*hint_status == TRANSACTION_STATUS_COMMITTED || *hint_status == TRANSACTION_STATUS_ABORTED)
            return *hint_status == TRANSACTION_STATUS_COMMITTED;
    }
    if (TransactionIdDidCommit(XidFromFullTransactionId(transactionId)))
    {
        if (hint_status != NULL)
            *hint_status = TRANSACTION_STATUS_COMMITTED;
        return true;
    } else {
        if (hint_status != NULL)
            *hint_status = TRANSACTION_STATUS_ABORTED;
        return false;
    }
}

/*
 * For xstore, clog is truncated based on globalFrozenXid. Therefore, we need to perform a quick check
 * first. If the transaction is smaller than globalFrozenXid, the transaction must be committed.
 *
 *      true iff given transaction committed
 */
bool 
xstore_transaction_id_did_commit(FullTransactionId transactionId)
{
    if (TransactionIdOlderThanAllUndo(transactionId))
    {
        return true;
    }
    return TransactionIdDidCommit(XidFromFullTransactionId(transactionId));
}

bool 
xstore_transaction_id_is_in_progress(FullTransactionId transactionId)
{
    if (TransactionIdOlderThanAllUndo(transactionId))
    {
        return false;
    }
    return TransactionIdIsInProgress(XidFromFullTransactionId(transactionId));
}

void 
proprgate_undo_info_to_parents()
{
    TransactionState s = GetCurrentTransactionSate();

    for (int i = 0; i < UNDO_PERSISTENCE_LEVELS; i++)
    {
        if (IS_VALID_UNDO_REC_PTR(s->latest_urp[i]))
        {
            s->parent->latest_urp[i] = s->latest_urp[i];
            s->parent->latest_urp_xact[i] = s->latest_urp[i];
        }

        if (!IS_VALID_UNDO_REC_PTR(s->parent->first_urp[i]))
            s->parent->first_urp[i] = s->first_urp[i];
    }
}

/*
 * XidVisibleInSnapshot
 *		Is the given XID(Top Transaction) visible according to the snapshot?
 *
 * On return, *hintstatus is set to indicate if the transaction had committed,
 * or aborted, whether or not it's not visible to us.
 * xid must be less than global fronzed xid.
 */
bool
xid_visible_in_snapshot(FullTransactionId xid, Snapshot snapshot, int* hint_status)
{
	if (XidInMVCCSnapshot(XidFromFullTransactionId(xid), snapshot))
    {
        if (hint_status != NULL)
            *hint_status = TRANSACTION_STATUS_IN_PROGRESS;
        return false;
    }

    if (hint_status != NULL)
    {
        if (*hint_status == TRANSACTION_STATUS_COMMITTED || *hint_status == TRANSACTION_STATUS_ABORTED)
            return *hint_status == TRANSACTION_STATUS_COMMITTED;
    }
	if (TransactionIdDidCommit(XidFromFullTransactionId(xid)))
    {
        if (hint_status != NULL)
            *hint_status = TRANSACTION_STATUS_COMMITTED;
        return true;
    } else {
        if (hint_status != NULL)
            *hint_status = TRANSACTION_STATUS_ABORTED;
        return false;
    }
}

static void
clear_my_global_frozen_xmin(void)
{
	pg_atomic_write_u64(&MyFrozenXmins[MyProcNumber], 0);
}
