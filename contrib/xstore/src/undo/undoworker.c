/* -------------------------------------------------------------------------
 *
 * undoworker.c
 * access interfaces of the async undo worker for the xstore engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * 	src/undo/undoworker.c
 * -------------------------------------------------------------------------
 */

#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#include "c.h"

#include "miscadmin.h"
#include "postgres.h"
#include "postmaster/bgworker.h"

#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/procsignal.h"

#include "undo/undoworker.h"
#include "undo/undorequest.h"
#include "undo/undolog.h"
#include "undo/undofetch.h"
#include "utils/memutils.h"
#include "utils/relfilenumbermap.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"
#include "xbtree/xbtree.h"
#include "xbtree/xbtundo.h"
#include "xheap/xheapundo.h"
#include "xstore.h"


int			undo_max_rollback_worker = 5;

struct UndoRollBackContext undorollback_ctx;
/* Memory context for long-lived data */
static MemoryContext UndoLauncherMemCxt;

static void UndolauncherSighupHandler(SIGNAL_ARGS);
static void UndolauncherSigtermHandler(SIGNAL_ARGS);

static bool UndoLauncherGetRequest(UndoRequestInfo request, int *idx);
static bool IsUndoWorkerAvailable();
static void StartUndoWorkerByIdx(UndoRequestInfo request, int idx);


struct UndoRequestInfoData curr_undowork;

static void UndoworkerSighupHandler(SIGNAL_ARGS);
static void UndoworkerSigtermHandler(SIGNAL_ARGS);

static void UndoWorkerFreeInfo(int code, Datum arg);
static void UndoWorkerGetRequest(UndoRequestInfo request);


static void
UndoworkerSighupHandler(SIGNAL_ARGS)
{
	int			saveErrno = errno;

	SetLatch(MyLatch);

	errno = saveErrno;
}

static void
UndoworkerSigtermHandler(SIGNAL_ARGS)
{
	int			saveErrno = errno;

	SetLatch(MyLatch);

	errno = saveErrno;
}

static void
UndoWorkerFreeInfo(int code, Datum arg)
{
	int			i = 0;
	pid_t		pid = MyProcPid;
	int			actualUndoWorkers = Min(undo_max_rollback_worker, MAX_ROLLBACK_WORKERS);

	for (i = 0; i < actualUndoWorkers; i++)
	{
		if (undorollback_ctx.worker_shmem->undo_workers[i].pid == pid)
		{
			undorollback_ctx.worker_shmem->undo_workers[i].pid = InvalidPid;
			undorollback_ctx.worker_shmem->undo_workers[i].xid =
				InvalidFullTransactionId;
			undorollback_ctx.worker_shmem->undo_workers[i].start_recptr =
				INVALID_UNDO_REC_PTR;
			undorollback_ctx.worker_shmem->undo_workers[i].start_time =
				(TimestampTz) 0;
			break;
		}
	}

	if (TransactionIdIsValid(curr_undowork.full_xid.value))
		mark_rollback_progress(curr_undowork.full_xid, curr_undowork.start_recptr, pid, false);

	ereport(DEBUG1, (errmsg("UndoWorker: cleanup  pid %d, current active workers %d",
							MyProcPid, pg_atomic_read_u32(&undorollback_ctx.worker_shmem->running_undo_workers))));

	pg_atomic_sub_fetch_u32(&undorollback_ctx.worker_shmem->running_undo_workers, 1);
	return;
}

static void
UndoWorkerGetRequest(UndoRequestInfo request)
{
	memcpy(request, undorollback_ctx.worker_shmem->rollback_request,
		   sizeof(UndoRequestInfoData));
}

static void
UndoPerformRequest(UndoRequestInfo request)
{
	bool		error = false;

	Assert(request->dbid != InvalidOid);
	StartTransactionCommand();
	PG_TRY();
	{
		elog(DEBUG1, "UndoWorker: Performing Rollback for xid:%lu, undo:(%lu -> %lu)",
			 request->full_xid.value, request->start_recptr, request->end_recptr);
		if (!execute_undo_actions(
							   request->full_xid, request->start_recptr,
							   request->end_recptr, request->slot_ptr, true))
		{
			error = true;
			elog(WARNING,
				 "UndoWorker: undo actions fail "
				 "TransactionId: %lu, latest urp: %lu, first urp: %lu, last urp %lu, dbid: %d",
				 request->full_xid.value, request->start_recptr, request->end_recptr,
				 request->start_recptr, request->dbid);

			/* set to retry */
			mark_rollback_fail(request->full_xid, request->start_recptr,
							   request->end_recptr, request->dbid);
		}
	}
	PG_CATCH();
	{
		StringInfoData errData;

		error = true;
		initStringInfo(&errData);

		elog(WARNING,
			 "UndoWorker: Error occured while executing undo actions "
			 "TransactionId: %lu, latest urp: %lu, first urp: %lu, last urp %lu, dbid: %d, errdata:%s",
			 request->full_xid.value, request->start_recptr, request->end_recptr,
			 request->start_recptr, request->dbid, errData.data);

		mark_rollback_fail(request->full_xid, request->start_recptr,
						   request->end_recptr, request->dbid);

		/* Prevent interrupts while cleaning up. */
		HOLD_INTERRUPTS();

		/* Send the error only to server log. */
		EmitErrorReport();

		/*
		 * Abort the transaction and continue processing pending undo
		 * requests.
		 */
		AbortOutOfAnyTransaction();
		FlushErrorState();

		RESUME_INTERRUPTS();
	}
	PG_END_TRY();

	if (!error)
	{
		CommitTransactionCommand();
		remove_rollback_entry(request->full_xid, request->start_recptr, MyProcPid);
	}
}

void
undo_worker_main(Datum main_arg)
{
	UndoRequestInfoData undowork;
	char		db_name[NAMEDATALEN] = {0};
	bool		databaseExists = false;
	bool		requestExists = false;
	int			actualUndoWorkers = Min(undo_max_rollback_worker, MAX_ROLLBACK_WORKERS);
	int			i = 0;

	/*
	 * Set up signal handlers.  We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 */
	pqsignal(SIGTERM, UndoworkerSigtermHandler);
	pqsignal(SIGHUP, UndoworkerSighupHandler);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	BackgroundWorkerUnblockSignals();

	/* Let the UndoLauncher know we have picked up the job and that we're active. */
	pg_atomic_add_fetch_u32(&undorollback_ctx.worker_shmem->running_undo_workers, 1);

	on_shmem_exit(UndoWorkerFreeInfo, 0);

	/* Get the work from the shared memory */
	UndoWorkerGetRequest(&undowork);
	Assert(undowork.dbid != InvalidOid);

	InitPostgres(NULL, undowork.dbid, NULL, InvalidOid, 0, db_name);
	ereport(DEBUG1, (errmsg("UndoWorker: perform undo work on %s xid %ld pid %d",
							db_name, undowork.full_xid.value, MyProcPid)));

	databaseExists = strlen(db_name) > 0;
	for (i = 0; i < actualUndoWorkers; i++)
	{
		if (FullTransactionIdEquals(
								  undorollback_ctx.worker_shmem->undo_workers[i].xid,
								  undowork.full_xid))
		{
			undorollback_ctx.worker_shmem->undo_workers[i].pid = MyProcPid;
			undorollback_ctx.worker_shmem->undo_workers[i].start_time =
				GetCurrentTimestamp();
			break;
		}
	}

	curr_undowork = undowork;
	requestExists = mark_rollback_progress(undowork.full_xid, undowork.start_recptr, MyProcPid, true);
	if (databaseExists && requestExists)
		UndoPerformRequest(&undowork);
	else
	{
		StartTransactionCommand();	/* need CurrentResourceOwner to  read buffer */
		set_undo_slot_rollback_finish(undowork.slot_ptr);
		ereport(WARNING,
				(errmsg("UndoWorker: db does not exists or request not exists "
						"but we need rollback txn on it, "
						"xid %lu, fromAddr %lu, toAddr %lu, dbid %u, slotptr %lu.",
						undowork.full_xid.value, undowork.start_recptr, undowork.end_recptr,
						undowork.dbid, undowork.slot_ptr)));
		remove_rollback_entry(undowork.full_xid, undowork.start_recptr, MyProcPid);
		CommitTransactionCommand();
	}

	ereport(DEBUG1, (errmsg("UndoWorker: rollback tran %lu from %lu is shutting down",
							undowork.full_xid.value, undowork.start_recptr)));
}


static void
UndolauncherSighupHandler(SIGNAL_ARGS)
{
	int			saveErrno = errno;

	if (undorollback_ctx.worker_shmem)
		SetLatch(&undorollback_ctx.worker_shmem->latch);

	errno = saveErrno;
}

static void
UndolauncherSigtermHandler(SIGNAL_ARGS)
{
	int			saveErrno = errno;

	undorollback_ctx.got_sigterm = true;
	if (undorollback_ctx.worker_shmem)
		SetLatch(&undorollback_ctx.worker_shmem->latch);

	errno = saveErrno;
}

static void
CleanupUndoWorkers()
{
	int			i = 0;
	int			actualUndoWorkers = Min(undo_max_rollback_worker, MAX_ROLLBACK_WORKERS);

	/* clean up not running worker */
	for (i = 0; i < actualUndoWorkers; i++)
	{
		if (undorollback_ctx.worker_shmem->undo_workers[i].pid == InvalidPid)
		{
			undorollback_ctx.worker_shmem->undo_workers[i].xid =
				InvalidFullTransactionId;
			undorollback_ctx.worker_shmem->undo_workers[i].start_recptr =
				INVALID_UNDO_REC_PTR;
			undorollback_ctx.worker_shmem->undo_workers[i].start_time =
				(TimestampTz) 0;
			break;
		}
	}
}

static bool
UndoLauncherGetRequest(UndoRequestInfo request, int *idx)
{
	int			i;
	RollbackHashEntry *entry = get_next_rollback_request();
	int			actualUndoWorkers = Min(undo_max_rollback_worker, MAX_ROLLBACK_WORKERS);

	if (entry == NULL)
		return false;

	for (i = 0; i < actualUndoWorkers; i++)
	{
		if (*idx == -1 &&
			!TransactionIdIsValid(
								  undorollback_ctx.worker_shmem->undo_workers[i].xid.value))
		{
			*idx = i;
		}
		if (undorollback_ctx.worker_shmem->undo_workers[i].xid.value == entry->full_xid.value)
			return false;
	}

	if (entry->retry_delay > 0)
	{
		entry->retry_delay--;
		return false;
	}

	if (*idx == -1)
	{
		ereport(INFO, (errmsg("cannot get idle worker, current active workers %d", pg_atomic_read_u32(
																					&undorollback_ctx.worker_shmem->running_undo_workers))));
		return false;
	}

	request->full_xid = entry->full_xid;
	request->start_recptr = entry->start_urec_ptr;
	request->end_recptr = entry->end_urec_ptr;
	request->dbid = entry->dbid;
	request->slot_ptr = entry->txn_slot_ptr;

	return true;
}

static bool
IsUndoWorkerAvailable()
{
	uint32		activeWorkers =
		pg_atomic_read_u32(&undorollback_ctx.worker_shmem->running_undo_workers);

	return (activeWorkers < (uint32) undo_max_rollback_worker);
}


static void
RegisterUndoWorker(UndoRequestInfo work, int idx)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	BgwHandleStatus status;
	pid_t		pid;

	/* Register the new dynamic worker. */
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "xstore");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "undo_worker_main");
	snprintf(bgw.bgw_name, BGW_MAXLEN, " undo workers xid %lu roll back from %lu",
			 work->full_xid.value, work->start_recptr);
	snprintf(bgw.bgw_type, BGW_MAXLEN, "undo worker");

	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = 0;
	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		/* warning and retry */
		ereport(WARNING,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register undo worker for xid %lu", work->full_xid.value),
				 errhint("You may need to increase max_worker_processes.")));
		return;
	}

	status = WaitForBackgroundWorkerStartup(bgw_handle, &pid);
	if (status != BGWH_STARTED && status != BGWH_STOPPED)
	{
		/* BGWH_STOPED is OK */
		undorollback_ctx.worker_shmem->undo_workers[idx].xid = InvalidFullTransactionId;
		undorollback_ctx.worker_shmem->undo_workers[idx].start_recptr = INVALID_UNDO_REC_PTR;
		/* error to exit launcher */
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start undo worker for xid %lu process status %d", work->full_xid.value, status),
				 errhint("More details may be available in the server log.")));
	}
	elog(DEBUG1, " started undo worker for xid %lu, process status %d", work->full_xid.value, status);
}


static void
StartUndoWorkerByIdx(UndoRequestInfo request, int idx)
{
	int			actualUndoWorkers = Min(undo_max_rollback_worker, MAX_ROLLBACK_WORKERS);
	const TimestampTz waitTime = 10 * 1000;
	const int	maxRetryTimes = 1000;
	int			retryTimes = 0;

	memcpy(undorollback_ctx.worker_shmem->rollback_request, request, sizeof(UndoRequestInfoData));

	if (idx < 0 || idx >= actualUndoWorkers)
	{
		ereport(PANIC,
				(errmsg("Can't find a slot in undo_worker_status, undo_max_rollback_worker %d, "
						"active_undo_workers %u",
						undo_max_rollback_worker,
						pg_atomic_read_u32(
										   &undorollback_ctx.worker_shmem->running_undo_workers))));
	}

	undorollback_ctx.worker_shmem->undo_workers[idx].xid = request->full_xid;
	undorollback_ctx.worker_shmem->undo_workers[idx].start_recptr =
		request->start_recptr;

	do
	{
		bool		to_start = (retryTimes % maxRetryTimes == 0);

		if (to_start)
		{
			/* start 0s or 10s */
			RegisterUndoWorker(request, idx);
		}
		/* worker is done or worker is running. */
		if (!TransactionIdIsValid(
								  undorollback_ctx.worker_shmem->undo_workers[idx].xid.value) ||
			undorollback_ctx.worker_shmem->undo_workers[idx].pid != InvalidPid)
			break;

		pg_usleep(waitTime);
		retryTimes++;
		if (retryTimes > maxRetryTimes)
		{
			/* do not retry anymore */
			undorollback_ctx.worker_shmem->undo_workers[idx].xid = InvalidFullTransactionId;
			undorollback_ctx.worker_shmem->undo_workers[idx].start_recptr = INVALID_UNDO_REC_PTR;
			break;
		}

	} while (true);
}

Size
undo_worker_shmem_size(void)
{
	Size		size = MAXALIGN(sizeof(UndoWorkerShmem));

	size = add_size(size, sizeof(UndoRequestInfoData));
	size = add_size(size, rollbackhash_shmem_size());
	return size;
}

void
undo_worker_shmem_init(Pointer ptr, bool found)
{
	int			i = 0;

	undorollback_ctx.worker_shmem = (UndoWorkerShmem *) ptr;

	if (!found)
	{
		undorollback_ctx.worker_shmem->undo_launcher_pid = 0;
		pg_atomic_write_u32(&undorollback_ctx.worker_shmem->running_undo_workers, 0);

		InitSharedLatch(&undorollback_ctx.worker_shmem->latch);
		LWLockInitialize(&undorollback_ctx.worker_shmem->lock, LWLockNewTrancheId());

		undorollback_ctx.worker_shmem->rollback_request =
			(UndoRequestInfo) ((char *) undorollback_ctx.worker_shmem +
							   MAXALIGN(sizeof(UndoWorkerShmem)));

		for (i = 0; i < MAX_ROLLBACK_WORKERS; i++)
		{
			undorollback_ctx.worker_shmem->undo_workers[i].xid =
				InvalidFullTransactionId;
			undorollback_ctx.worker_shmem->undo_workers[i].pid = InvalidPid;
			undorollback_ctx.worker_shmem->undo_workers[i].start_recptr =
				INVALID_UNDO_REC_PTR;
			undorollback_ctx.worker_shmem->undo_workers[i].start_time =
				(TimestampTz) 0;
		}
	}
	rollbackhash_shmem_init();
	LWLockRegisterTranche(undorollback_ctx.worker_shmem->lock.tranche, "xstore_rollback");
}

void
undo_launcher_main(Datum main_arg)
{
	sigjmp_buf	local_sigjmp_buf;
	long int	defaultSleepTime = 1000L;	/* 1 s */
	long int	currSleepTime = defaultSleepTime;

	ereport(LOG, (errmsg("undo launcher started")));

	CleanupUndoWorkers();

	/*
	 * Set up signal handlers.  We operate on databases much like a regular
	 * backend, so we use the same signal handling.  See equivalent code in
	 * tcop/postgres.c.
	 */
	pqsignal(SIGTERM, UndolauncherSigtermHandler);
	pqsignal(SIGHUP, UndolauncherSighupHandler);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGQUIT, UndolauncherSigtermHandler);
	BackgroundWorkerUnblockSignals();

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * We just need to clean up, report the error, and go away.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/*
		 * sigsetjmp will have blocked all signals, but we may need to accept
		 * signals while communicating with our parallel leader.  Once we've
		 * done HOLD_INTERRUPTS() it should be safe to unblock signals.
		 */
		BackgroundWorkerUnblockSignals();

		/* Report the error to the parallel leader and the server log */
		EmitErrorReport();

		/* should disown for next restart. important! */
		DisownLatch(&undorollback_ctx.worker_shmem->latch);

		/*
		 * Do we need more cleanup here?  For shmem-connected bgworkers, we
		 * will call InitProcess below, which will install ProcKill as exit
		 * callback.  That will take care of releasing locks, etc.
		 */

		/* and go away */
		proc_exit(1);
	}
	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	UndoLauncherMemCxt =
		AllocSetContextCreate(TopMemoryContext, "Undo Launcher", ALLOCSET_DEFAULT_SIZES);

	MemoryContextSwitchTo(UndoLauncherMemCxt);

	undorollback_ctx.worker_shmem->undo_launcher_pid = MyProcPid;
	OwnLatch(&undorollback_ctx.worker_shmem->latch);

	ereport(DEBUG1, (errmsg("undo launcher pid %d has started", MyProcPid)));

	while (!undorollback_ctx.got_sigterm)
	{
		UndoRequestInfoData request;
		int			idx = -1;

		if (IsUndoWorkerAvailable() && UndoLauncherGetRequest(&request, &idx))
		{
			StartUndoWorkerByIdx(&request, idx);
			currSleepTime = defaultSleepTime;
		}
		else
		{
			/* Wait until sleep time expires or we get some type of signal */
			int			rc = WaitLatch(&undorollback_ctx.worker_shmem->latch,
									   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, currSleepTime,
									   PG_WAIT_EXTENSION);

			ResetLatch(&undorollback_ctx.worker_shmem->latch);
			if (((unsigned int) rc) & WL_POSTMASTER_DEATH)
			{
				ereport(DEBUG1, (errmsg("undo launcher pid %d has stoped when postmaster death", MyProcPid)));
				DisownLatch(&undorollback_ctx.worker_shmem->latch);
				proc_exit(1);
			}

			currSleepTime = Min(defaultSleepTime * 180, 2 * currSleepTime);
		}
	}

	ereport(LOG, (errmsg("undo launcher shutting down")));
	undorollback_ctx.worker_shmem->undo_launcher_pid = 0;
	DisownLatch(&undorollback_ctx.worker_shmem->latch);
	proc_exit(1);
}

void
undo_launcher_register(void)
{
	BackgroundWorker bgw;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "xstore");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "undo_launcher_main");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "undo launcher");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "undo launcher");
	bgw.bgw_restart_time = 5;
	bgw.bgw_notify_pid = 0;
	bgw.bgw_main_arg = (Datum) 0;
	RegisterBackgroundWorker(&bgw);
}