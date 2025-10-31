/* -------------------------------------------------------------------------
 *
 * undorequest.h
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/undo/undorequest.h
 * -------------------------------------------------------------------------
 */


#ifndef UNDOREQUEST_H
#define UNDOREQUEST_H

#include "undo/undotype.h"
#include "undo/undorecord.h"
#include "c.h"
#include "utils/rel.h"


typedef struct RollbackHashKey
{
	FullTransactionId full_xid;
	UndoRecPtr	  start_urec_ptr;
} RollbackHashKey;

typedef struct RollbackHashEntry
{
	FullTransactionId full_xid;
	UndoRecPtr	  start_urec_ptr;
	UndoRecPtr	  end_urec_ptr;
	Oid			  dbid;
	UndoSlotPtr	  txn_slot_ptr;
	bool		  in_progress;
	int           retry_delay;
} RollbackHashEntry;

typedef enum RollbackResult
{
	ROLLBACK_ROK = 0,
	ROLLBACK_RSWITCH = 1
} RollbackResult;


Size rollbackhash_shmem_size(void);
void rollbackhash_shmem_init(void);

bool register_rollback_req(FullTransactionId xid, UndoRecPtr from_recptr, UndoRecPtr to_recptr,
						Oid dbid, UndoSlotPtr slot_ptr);
bool mark_rollback_progress(FullTransactionId xid, UndoRecPtr start_recptr, pid_t pid, bool in_progress);						
bool remove_rollback_entry(FullTransactionId xid, UndoRecPtr start_recptr, pid_t pid);
void mark_rollback_fail(FullTransactionId xid, UndoRecPtr start_recptr, UndoRecPtr to_recptr, Oid dbid);
RollbackHashEntry *get_next_rollback_request(void);


bool execute_undo_actions(FullTransactionId full_xid, UndoRecPtr from_urecptr,
						UndoRecPtr to_urecptr, UndoSlotPtr slotPtr, bool isTopTxn);
void execute_undo_actions_tuple(UndoRecPtr urp, Relation relation, Buffer buf,
							FullTransactionId xid);


#endif
