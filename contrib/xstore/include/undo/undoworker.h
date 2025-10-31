/* -------------------------------------------------------------------------
 *
 * undoworker.h
 * access interfaces of the undo worker for the xstore engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/undo/undoworker.h
 * -------------------------------------------------------------------------
 */

#ifndef UNDOWORKER_H
#define UNDOWORKER_H

#include "undo/undotype.h"
#include "undo/undorequest.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "port/atomics.h"


extern int undo_max_rollback_worker;

#define MAX_ROLLBACK_WORKERS 128

typedef struct UndoRequestInfoData
{
	FullTransactionId full_xid;
	UndoRecPtr	  start_recptr;
	UndoRecPtr	  end_recptr;
	Oid			  dbid;
	UndoSlotPtr	  slot_ptr;
} UndoRequestInfoData;

typedef UndoRequestInfoData *UndoRequestInfo;

typedef struct UndoApplyWorker
{
	pid_t		  pid;
	FullTransactionId xid;
	UndoRecPtr	  start_recptr;
	TimestampTz	  start_time;
} UndoApplyWorker;


typedef struct UndoWorkerShmem
{
	/* Latch used by backends to wake the undo launcher when it has work to do */
	Latch latch;
	/* LWLock used by backends to concurrent access rollback request hash table */
	LWLock lock;
	pid_t undo_launcher_pid;
	UndoRequestInfo rollback_request;
	pg_atomic_uint32 running_undo_workers;
	UndoApplyWorker	 undo_workers[MAX_ROLLBACK_WORKERS];
} UndoWorkerShmem;

typedef struct UndoRollBackContext
{
	/* Flags set by signal handlers */
	volatile sig_atomic_t got_sigterm;
	struct UndoWorkerShmem *worker_shmem;
} UndoRollBackContext;

extern struct UndoRollBackContext undorollback_ctx;

/* shared memory specific */
extern Size undo_worker_shmem_size(void);
extern void undo_worker_shmem_init(Pointer ptr, bool found);

PGDLLEXPORT void undo_launcher_register(void);
PGDLLEXPORT void undo_launcher_main(Datum main_arg);
PGDLLEXPORT void undo_worker_main(Datum main_arg);

#endif
