/* -------------------------------------------------------------------------
 *
 * xxact.h
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/util/xxact.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef XXACT_H
#define XXACT_H

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "undo/undotype.h"

extern void xstore_xact_callback(XactEvent event, void *arg);
extern void xstore_sub_xact_callback(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg);
extern void register_xstore_xact_callbacks(void);
extern void apply_undo_actions(void);
extern void reset_undo_actions_info(void);
extern void set_current_tansaction_undorec_ptr(UndoRecPtr urecPtr, UndoPersistence upersistence);
extern UndoRecPtr get_current_tansaction_undorec_ptr(UndoPersistence upersistence);
extern bool has_current_sub_transaction_lock(void);
extern void set_current_sub_transaction_locked(void);
extern bool transaction_id_did_commit_with_hint(FullTransactionId transactionId, int* hint_status);
extern bool xstore_transaction_id_did_commit(FullTransactionId transactionId);
extern bool xstore_transaction_id_is_in_progress(FullTransactionId transactionId);
extern void proprgate_undo_info_to_parents(void);
extern void reset_current_sub_transaction_lock(void);
extern bool xid_visible_in_snapshot(FullTransactionId xid, Snapshot snapshot, int* hintstatus);
#endif