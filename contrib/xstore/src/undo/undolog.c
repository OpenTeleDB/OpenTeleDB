/* -------------------------------------------------------------------------
 *
 * undolog.c
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    src/undo/undolog.c
 *
 * -------------------------------------------------------------------------
 */

#include "undo/undolog.h"
#include "port.h"

#include "postmaster/bgworker.h"
#include "undo/undotype.h"
#include "undo/undoxlog.h"
#include "undo/undofile.h"
#include "undo/undofetch.h"
#include "undo/undotxn.h"
#include "undo/undorequest.h"
#include "util/xxact.h"
#include "xstore.h"
#include "xheap/xpage.h"
#include "access/rmgr.h"
#include "access/xlogutils.h"
#include "access/transam.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/procnumber.h"
#include "storage/procarray.h"
#include "storage/buf_internals.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/block.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/pg_crc.h"
#include "utils/palloc.h"
#include "utils/timeout.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/elog.h"
#include <unistd.h>
#include "access/slru.h"
#include "c.h"
#include "libpq/pqsignal.h"

#define UNDODEBUGINFO , __FUNCTION__, __LINE__
#define UNDODEBUGSTR "[%s:%d]"
#define UNDOFORMAT(f) UNDODEBUGSTR f UNDODEBUGINFO


int	undo_max_segno_per_log = -1;  // max  67108863  2^26 - 1 

void alloc_undo_slot(UndoSlotPtr slotPtr, UndoLogControl *ulog, FullTransactionId xid,
					   UndoPersistence upersistence, bool realloc);

void init_undo_slot(UndoSlot *slot, FullTransactionId xid, Oid dbId);
void update_undo_slot(UndoSlot *slot, UndoRecPtr start, UndoRecPtr end);
bool can_rollback_undo_slot(UndoSlot *slot);

void release_undo_slot_buffer(UndoSlotBuffer *slotBuff);
bool check_undo_slot_buffer_valid(BufferDesc *buf, int logno, UndoSlotPtr slotPtr);

UndoSlot *undolog_alloc_undo_slot(UndoLogControl *ulog, UndoSlotPtr slotPtr,
											  FullTransactionId xid, Oid dbid, bool realloc);

void	   undolog_advance_insert_urecptr(UndoLogControl *ulog, uint64 oldInsert, uint64 size);
void	   undolog_set_allocate_slotptr(UndoLogControl *ulog, UndoSlotPtr allocate);
UndoRecPtr undolog_calc_insert_urecptr(UndoLogControl *ulog, uint64 oldInsert, uint64 size);

bool undolog_attach(UndoLogControl *ulog);
bool undolog_detach(UndoLogControl *ulog);

void init_undo_segemnt(UndoLogControl *ulog, UndoSegmentType type);
void create_non_exists_undo_file(UndoSegment *seg, int logno, uint32 dbId);
void checkpoint_undo_segment(int fd, UndoSegmentType type);
void recovery_undo_segment(int fd, UndoSegmentType type);

bool undolog_check_need_switch(UndoLogControl *ulog, UndoRecordSize size);
bool undolog_check_recycle(UndoLogControl *ulog, UndoRecPtr starturp, UndoRecPtr endurp);
UndoRecordState undolog_check_undo_record_valid(UndoLogControl *ulog, UndoLogOffset offset,
											 bool checkForce, FullTransactionId *lastXid);

UndoRecPtr undolog_alloc_space(UndoLogControl *ulog, uint64 size);
void	   undolog_release_space(UndoLogControl *ulog, UndoRecPtr starturp, UndoRecPtr endurp,
								int *forceRecycleSize);
UndoRecPtr undolog_alloc_slot_space(UndoLogControl *ulog);
void undolog_release_slot_space(UndoLogControl *ulog, UndoRecPtr starturp, UndoRecPtr endurp,
							  int *forceRecycleSize);
void undolog_prepare_switch(UndoLogControl *ulog);

bool undolog_isused(int logno, UndoPersistence upersistence);
void undolog_check_exceeds(FullTransactionId xid, uint64 size);
void undolog_release(int logno, UndoPersistence upersistence);

UndoLogControl *undolog_switch(int logno, UndoPersistence upersistence, uint64 size);

/* Checkpoint meta of undolog. */
void checkpoint_undo_logs(int fd);

/* Recovery undolog info from persistent file. */
void recovery_undo_logs(int fd);

UndoRecPtr undo_rec_get_prevurp(UndoRecPtr currUrp);

void prepare_undo_slot_buffer(UndoSlotBuffer *slotBuff, XLogReaderState *record,
							 void *meta, UndoSlotPtr slotPtr);

void advance_undo_slot_page_lower(Page page, UndoSlotPtr slotPtr);							 

static inline void
undolog_set_recycle_slotptr(UndoLogControl *ulog, UndoSlotPtr recycle)
{
	ulog->recycle_slot_offset = UNDO_PTR_GET_OFFSET(recycle);
}
static inline void
undolog_set_insert_urecptr(UndoLogControl *ulog, UndoRecPtr insert)
{
	ulog->insert_offset = UNDO_PTR_GET_OFFSET(insert);
}
static inline void
undolog_set_discard_urecptr(UndoLogControl *ulog, UndoRecPtr discard)
{
	ulog->discard_offset = UNDO_PTR_GET_OFFSET(discard);
}
static inline void
undolog_set_force_discard_urecptr(UndoLogControl *ulog, UndoRecPtr discard)
{
	ulog->force_discard_offset = UNDO_PTR_GET_OFFSET(discard);
}
static inline void
undolog_set_frozen_slotptr(UndoLogControl *ulog, UndoSlotPtr frozenSlotPtr)
{
	ulog->frozen_slot_ptr = frozenSlotPtr;
}

static inline bool
undolog_is_attached(UndoLogControl *ulog) 
{
	return (pg_atomic_read_u32(&(ulog->attached)) == UNDO_LOG_ATTACHED);
}

static inline bool
undolog_is_detached(UndoLogControl *ulog) 
{
	return (pg_atomic_read_u32(&(ulog->attached)) == UNDO_LOG_DETACHED);
}

static inline bool
undolog_need_recycle(UndoLogControl *ulog) 
{
	return ulog->recycle_slot_offset < ulog->alloc_slot_offset;
}

void
alloc_undo_slot(UndoSlotPtr slotPtr, UndoLogControl *ulog, FullTransactionId xid,
				  UndoPersistence upersistence, bool realloc)
{
	UndoSlot *slot =
		undolog_alloc_undo_slot(ulog, slotPtr, xid, MyDatabaseId, realloc);

	if (slot == NULL)
	{
		ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg(UNDOFORMAT("ulog %d cannot allocate transaction slot."),
							   ulog->logno)));
	}
	undo_log_ctx->slots[upersistence] = (void *) slot;
}

bool
check_need_switch_undolog(UndoPersistence upersistence, uint64 size, UndoRecPtr undoPtr)
{
	int		  logno;
	UndoLogControl *ulog;
	if (InRecovery)
	{
		logno = UNDO_PTR_GET_LOG_NO(undoPtr);
	}
	else
	{
		logno = undo_log_ctx->logs[upersistence];
	}
	Assert(logno != INVALID_UNDOLOG_NO);
	ulog = get_undo_log(logno);
	if (ulog == NULL)
	{
		ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("CheckNeedSwitch: ulog is NULL")));
	}
	return undolog_check_need_switch(ulog, size);
}

void
undolog_check_exceeds(FullTransactionId xid, uint64 size)
{
	uint64 transUndoThresholdSize = 0;
	undo_log_ctx->curr_trans_undo_size += size;
	transUndoThresholdSize = (uint64) undo_max_size_per_transaction * BLCKSZ;
	if ((!InRecovery) && (undo_log_ctx->curr_trans_undo_size > transUndoThresholdSize))
	{
		ereport(ERROR, (errmsg(UNDOFORMAT("xid %lu, the undo size %lu of the transaction "
										  "exceeds the threshold %lu."),
							   xid.value, undo_log_ctx->curr_trans_undo_size, transUndoThresholdSize)));
	}
	return;
}

UndoRecPtr
allocate_undo_space(FullTransactionId xid, UndoPersistence upersistence, uint64 size,
				  bool needSwitch, xl_undo_meta *xlundometa)
{
	int				logno;
	UndoLogControl	   *ulog;
	UndoRecPtr		urecptr;
	UndoSlotBuffer *slotBuf;
	UndoSlotPtr		slotPtr;


	logno = undo_log_ctx->logs[upersistence];
	Assert(logno != INVALID_UNDOLOG_NO);
	ulog = get_undo_log(logno);
	if (ulog == NULL)
	{
		ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("allocate_undo_space: ulog is NULL")));
	}
	Assert(upersistence == UndoGetPersitentLevel(ulog));

	if (unlikely(needSwitch))
	{
		ulog = undolog_switch(logno, upersistence, size);
		xlundometa->info |= XLOG_UNDOMETA_INFO_SWITCH;
	}

	if (xid.value != undo_log_ctx->prev_xid[upersistence].value)
	{
		undo_log_ctx->curr_trans_undo_size = 0;
	}
	undolog_check_exceeds(xid, size);
	urecptr = undolog_alloc_space(ulog, size);
	slotBuf = &ulog->txn_slot_buffer;

	if (xid.value != undo_log_ctx->prev_xid[upersistence].value || needSwitch ||
		BufferIsInvalid(slotBuf->buffer) || slotBuf->blkno == InvalidBlockNumber)
	{
		UndoSlotPtr ptr = undolog_alloc_slot_space(ulog);
		undo_log_ctx->slot_ptr[upersistence] = ptr;
	}
	slotPtr = undo_log_ctx->slot_ptr[upersistence];
	xlundometa->slotPtr = UNDO_PTR_GET_OFFSET(slotPtr);
	elog(DEBUG1,
		 UNDOFORMAT("space %d xid %lu allocate space undo ptr from %lu to %lu size %lu"),
		 ulog->logno, xid.value, urecptr, UndoGetInsertURecPtr(ulog), size);

	return urecptr;
}

UndoRecPtr
get_next_undoptr(UndoRecPtr undoPtr, uint64 size)
{
	UndoLogOffset oldInsert = UNDO_PTR_GET_OFFSET(undoPtr);
	UndoLogOffset newInsert = UNDO_LOG_OFFSET_PLUS_USABLE_BYTES(oldInsert, size);
	int			  logno = UNDO_PTR_GET_LOG_NO(undoPtr);
	Assert(newInsert % BLCKSZ >= UNDO_LOG_BLOCK_HEADER_SIZE);
	return MAKE_UNDO_REC_PTR(logno, newInsert);
}

void update_undolog_meta(FullTransactionId xid, UndoRecPtr startUndoPtr,
					 xl_undo_meta *meta, UndoPersistence upersistence,
					 UndoRecPtr lastRecord, UndoRecPtr lastRecordSize)
{
	int			logno = undo_log_ctx->logs[upersistence];
	BufferDesc *buf_desc;

	UndoSlot *slot;
	bool			 allocateTranslot = false;
	UndoLogControl		*ulog = NULL;


	Assert(logno != INVALID_UNDOLOG_NO);
	ulog = get_undo_log(logno);
	if (ulog == NULL)
	{
		ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("UpdateTransactionSlot: ulog is NULL")));
	}

	lock_undo_log(ulog);

    // advance insert ptr
	if (upersistence == UNDO_PERMANENT)
	{
		ulog->dirty = true;
	}
	undolog_advance_insert_urecptr(ulog, UNDO_PTR_GET_OFFSET(lastRecord), lastRecordSize);
	if (UndoGetForceDiscardURecPtr(ulog) > UndoGetInsertURecPtr(ulog))
	{
		ereport(
			PANIC,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg(UNDOFORMAT("ulog %d forceDiscardURecPtr %lu > insertURecPtr %lu."),
					ulog->logno, UndoGetForceDiscardURecPtr(ulog),
					UndoGetInsertURecPtr(ulog))));
	}
	buf_desc = GetBufferDescriptor(ulog->txn_slot_buffer.buffer - 1);
	if (!LWLockHeldByMe(&buf_desc->content_lock))
		LockBuffer(ulog->txn_slot_buffer.buffer, BUFFER_LOCK_EXCLUSIVE);
	
	if (!check_undo_slot_buffer_valid(buf_desc, logno, meta->slotPtr))
	{
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid cached slot buffer %d slot ptr %lu.",
						ulog->txn_slot_buffer.buffer, xlog_undo_meta_slotptr(meta))));
	}

	if (!FullTransactionIdIsNormal(xid) || startUndoPtr == INVALID_UNDO_REC_PTR)
	{
		ereport(
			PANIC,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("update transaction slot failed: xid %lu, logno %d, startUndoPtr %lu",
					xid.value, logno, startUndoPtr)));
	}

	if (xid.value != undo_log_ctx->prev_xid[upersistence].value || meta->info & XLOG_UNDOMETA_INFO_SWITCH)
	{	// need a new slot
		Page page;
		allocateTranslot = true;
		undo_log_ctx->prev_xid[upersistence] = xid;
		alloc_undo_slot(undo_log_ctx->slot_ptr[upersistence], ulog, xid, upersistence,
						  false);
		page = BufferGetPage(ulog->txn_slot_buffer.buffer);
		advance_undo_slot_page_lower(page, undo_log_ctx->slot_ptr[upersistence]);
		elog(DEBUG4, UNDOFORMAT("ulog %d prevXid %lu current xid %lu"), logno,
			 undo_log_ctx->prev_xid[upersistence].value, xid.value);
	} 

	slot = (UndoSlot *) undo_log_ctx->slots[upersistence];
	if (slot->xid.value != xid.value || slot->dbOid != MyDatabaseId)
	{
		ereport(PANIC, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("slot check invalid: ulog %d slotptr %lu slot xid %lu != xid "
							   "%lu, slot dbid %u != dbid %u.",
							   logno, xlog_undo_meta_slotptr(meta), slot->xid.value, xid.value,
							   slot->dbOid, MyDatabaseId)));
	}

	elog(DEBUG5,
		 "update ulog %d, slotptr %lu xid %lu dbid %u: old start %lu end %lu, new start "
		 "%lu end %lu.",
		 logno, xlog_undo_meta_slotptr(meta), xid.value, slot->dbOid, slot->start_undo_ptr,
		 slot->end_undo_ptr, startUndoPtr, ulog->insert_offset);

    // update transaction slot
	update_undo_slot(slot, startUndoPtr, UndoGetInsertURecPtr(ulog));
	Assert(slot->dbOid == MyDatabaseId);

	MarkBufferDirty(ulog->txn_slot_buffer.buffer);
	if (allocateTranslot)
	{
		meta->dbid = MyDatabaseId;
		meta->info |= XLOG_UNDOMETA_INFO_SLOT_ALLOC;
		if(ulog->txn_slot_buffer.zero)
			meta->info |= XLOG_UNDOMETA_INFO_SLOT_INIT;
		Assert(meta->dbid != INVALID_DB_OID);
	}
}

void
release_undolog_meta(UndoPersistence upersistence)
{
	int		  logno = undo_log_ctx->logs[upersistence];
	UndoLogControl *ulog = get_undo_log(logno);
	if (ulog == NULL)
	{
		ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("ReleaseUndoLogMeta: ulog is NULL")));
	}
	LockBuffer(ulog->txn_slot_buffer.buffer, BUFFER_LOCK_UNLOCK);
	unlock_undo_log(ulog);
	return;
}

void
set_undolog_meta_lsn(XLogRecPtr lsn)
{
	int		  logno = undo_log_ctx->logs[UNDO_PERMANENT];
	UndoLogControl *ulog = get_undo_log(logno);
	if (ulog == NULL)
	{
		ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("SetUndoLogMetaLSN: ulog is NULL")));
	}
	ulog->lsn = lsn;
	PageSetLSN(BufferGetPage(ulog->txn_slot_buffer.buffer), lsn);
}

void
redo_undo_meta(XLogReaderState *record, xl_undo_meta *meta, UndoRecPtr startUndoPtr,
			 UndoRecPtr lastRecord, uint32 lastRecordSize)
{
	FullTransactionId xid = InvalidFullTransactionId;
	XLogRecPtr	  lsn = 0;
	UndoSlotPtr	  slotPtr;
	UndoLogControl	 *ulog = get_undo_log(UNDO_PTR_GET_LOG_NO(startUndoPtr));
	if (ulog == NULL)
	{
		return;
	}
	xid = XLogRecGetFullXid(record);
	Assert(TransactionIdIsValid(xid.value));
	
	lsn = record->EndRecPtr;
	if (ulog->lsn < lsn)
	{
		lock_undo_log(ulog);
		undolog_advance_insert_urecptr(ulog, UNDO_PTR_GET_OFFSET(lastRecord),
									 lastRecordSize);
		if (xlog_undo_meta_is_translot(meta) ||
            xlog_undo_meta_is_switch(meta))
		{
			undolog_set_allocate_slotptr(ulog, get_next_slotptr(meta->slotPtr));
		}
		ulog->dirty = true;
		ulog->lsn = (lsn);
		unlock_undo_log(ulog);
	}
	slotPtr = MAKE_UNDO_REC_PTR(ulog->logno, meta->slotPtr); 
	if (!is_skip_insert_slot(slotPtr))
	{
		UndoSlotBuffer buf;
		Page		   page;

        init_undo_slot_buffer(&buf);
		prepare_undo_slot_buffer(&buf, record, meta, slotPtr);
		if (BufferIsInvalid(buf.buffer))
		{
			return;
		}
		page = BufferGetPage(buf.buffer);
		if (PageGetLSN(page) < lsn)
		{
			UndoRecPtr		 endUndoPtr;
			UndoSlot *slot = get_undo_slot_from_buffer(&buf, slotPtr);
			if (xlog_undo_meta_is_translot(meta) ||
                xlog_undo_meta_is_switch(meta))
			{
				init_undo_slot(slot, xid, meta->dbid);
			}
			endUndoPtr = undolog_calc_insert_urecptr(
				ulog, UNDO_PTR_GET_OFFSET(lastRecord), lastRecordSize);
			elog(DEBUG5,
				 UNDOFORMAT("redometa:ulog %d, slotptr=%lu, lastRecordSize=%u, xid=%lu, "
							"start=%lu, end=%lu."),
				 ulog->logno, slotPtr, lastRecordSize, xid.value, startUndoPtr, endUndoPtr);
			update_undo_slot(slot, startUndoPtr, endUndoPtr);
			// the next of slot ptr must not excess the boundary of block
			if (xlog_undo_meta_is_translot(meta))
				advance_undo_slot_page_lower(page, slotPtr);
			MarkBufferDirty(buf.buffer);
			PageSetLSN(page, lsn);
		}
		UnlockReleaseBuffer(buf.buffer);
	}
	return;
}

/* Check undo record valid.. */
UndoRecordState
check_undo_record_valid(UndoRecPtr urp, bool checkForce, FullTransactionId *lastXid)
{
	int		  logno = 0;
	UndoLogControl *ulog = NULL;
	if (!IS_VALID_UNDO_REC_PTR(urp))
	{
		return UNDO_RECORD_INVALID;
	}
	logno = UNDO_PTR_GET_LOG_NO(urp);
	Assert(IS_VALID_LOGNO(logno));
	ulog = get_undo_log(logno);
	if (ulog == NULL)
	{
		return UNDO_RECORD_INVALID;
	}
	else
	{
		return undolog_check_undo_record_valid(ulog, UNDO_PTR_GET_OFFSET(urp),
											checkForce, lastXid);
	}
}

/*
 * skip prepare undo record when undo record was invalid.
 */
bool
is_skip_insert_undo(UndoRecPtr urp, xl_undo_meta *xlundometa)
{
	int			  logno = 0;
	UndoLogControl	 *ulog = NULL;
	UndoSegment	 *space = NULL;
	UndoLogOffset offset = 0;

	Assert(IS_VALID_UNDO_REC_PTR(urp));
	logno = UNDO_PTR_GET_LOG_NO(urp);
	ulog = get_undo_log(logno);

	if (ulog == NULL)
	{
		return true;
	}
	space = &ulog->undo_data_seg;
	offset = UNDO_LOG_OFFSET_PLUS_USABLE_BYTES(UNDO_PTR_GET_OFFSET(urp), 
		xlundometa->lastRecordSize);
	if (offset > space->head && offset <= space->tail)
	{
		return false;
	}
	else if (offset > space->tail)
	{
		ereport(
			PANIC,
			(errcode(ERRCODE_DATA_EXCEPTION),
			 errmsg(
				 UNDOFORMAT(
					 "Space allocation tail=%lu is slower than undo insert offset=%lu."),
				 space->tail, offset)));
	}
	return true;
}

bool
is_skip_insert_slot(UndoSlotPtr slotPtr)
{
	int			  logno;
	UndoLogControl	 *ulog;
	UndoSegment	 *space;
	UndoLogOffset offset;

	Assert(IS_VALID_UNDO_REC_PTR(slotPtr));
	logno = UNDO_PTR_GET_LOG_NO(slotPtr);
	ulog = get_undo_log(logno);
	if (ulog == NULL)
	{
		return true;
	}
	space = &ulog->undo_txn_seg;
	offset = UNDO_PTR_GET_OFFSET(slotPtr);
	if (offset > space->head && offset < space->tail)
	{
		return false;
	}
	else if (offset >= space->tail)
	{
		ereport(
			PANIC,
			(errcode(ERRCODE_DATA_EXCEPTION),
			 errmsg(
				 UNDOFORMAT(
					 "Space allocation tail=%lu is faster than slot insert offset=%lu."),
				 space->tail, offset)));
	}
	return true;
}


void
checkpoint_undo_meta(XLogRecPtr checkPointRedo)
{
	int fd;

	if (undo_sys_ctx == NULL || pg_atomic_read_u32(&undo_sys_ctx->undo_log_used_cout) == 0)
	{
		return;
	}
	{

		fd = BasicOpenFilePerm(UNDO_META_FILE, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
							errmsg("could not open file %s", UNDO_META_FILE)));
			return;
		}

		/* Checkpoint undospace meta first. */
		checkpoint_undo_logs(fd);
		checkpoint_undo_segment(fd, UNDO_LOG_SEGMENT);
		checkpoint_undo_segment(fd, UNDO_SLOT_SEGMENT);

		/* Flush buffer data and close fd. */
		fsync(fd);
		close(fd);
	}
}


void
set_undo_threshold()
{
	uint32 undoMemFactor = 4;
	uint32 undoCountThreshold = 0;
	uint32 maxConn = MaxConnections;
	uint32 maxThreadNum = MaxBackends;

	undoCountThreshold = (maxConn >= maxThreadNum) ? undoMemFactor * maxConn
												   : undoMemFactor * maxThreadNum;
	undo_sys_ctx->undo_count_threshold =
		(pg_atomic_read_u32(&undo_sys_ctx->undo_log_used_cout) >= undoCountThreshold)
			? undoMemFactor * pg_atomic_read_u32(&undo_sys_ctx->undo_log_used_cout)
			: undoCountThreshold;

	undo_sys_ctx->undo_count_threshold =
		(undo_sys_ctx->undo_count_threshold > UNDOLOG_TOTAL_COUNT)
			? undo_sys_ctx->undo_count_threshold
			: UNDOLOG_TOTAL_COUNT;

}

static bool
InitUndoLogMeta(int fd)
{
	uint64	 writeSize = 0;
	uint32	 ret = 0;
	uint32	 loop;
	uint32	 totalLogPageCnt = 0;
	char	 metaPageBuffer[UNDO_META_PAGE_SIZE] = {'\0'};
	pg_crc32 logMetaPageCrc = 0;

	/* Init undospace meta, persist meta info into disk. */
	UNDO_LOG_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOLOG_COUNT_PER_PAGE,
							 totalLogPageCnt);
	for (loop = 0; loop < PERSIST_UNDOLOG_COUNT; loop++)
	{
		uint32		  logno = loop;
		uint32		  offset = logno % UNDOLOG_COUNT_PER_PAGE;
		UndoLogMeta *ulogMetaPoint = NULL;

		if (logno % UNDOLOG_COUNT_PER_PAGE == 0)
		{
			memset(metaPageBuffer,  0, UNDO_META_PAGE_SIZE);

			/* On last page, count of undospace meta maybe less than UNDOSEGMENT_COUNT_PER_PAGE. */
			if ((uint32) (logno / UNDOLOG_COUNT_PER_PAGE) + 1 == totalLogPageCnt)
			{
				writeSize = (PERSIST_UNDOLOG_COUNT -
							 (totalLogPageCnt - 1) * UNDOLOG_COUNT_PER_PAGE) *
							sizeof(UndoLogMeta);
			}
			else
			{
				writeSize = sizeof(UndoLogMeta) * UNDOLOG_COUNT_PER_PAGE;
			}
		}

		ulogMetaPoint =
			(UndoLogMeta *) (metaPageBuffer + offset * sizeof(UndoLogMeta));
		ulogMetaPoint->version = XSTORE_UNDO_VERSION;
		ulogMetaPoint->lsn = 0;
		ulogMetaPoint->insert_rec_ptr = UNDO_LOG_BLOCK_HEADER_SIZE;
		ulogMetaPoint->discard_rec_ptr = UNDO_LOG_BLOCK_HEADER_SIZE;
		ulogMetaPoint->forece_discard_rec_ptr = UNDO_LOG_BLOCK_HEADER_SIZE;
		ulogMetaPoint->recycle_xid = InvalidFullTransactionId;
		ulogMetaPoint->allocate_slot_offset = UNDO_LOG_BLOCK_HEADER_SIZE;
		ulogMetaPoint->recycle_slot_offset = UNDO_LOG_BLOCK_HEADER_SIZE;

		if ((logno + 1) % UNDOLOG_COUNT_PER_PAGE == 0 ||
			(logno == PERSIST_UNDOLOG_COUNT - 1 &&
			 ((uint32) (logno / UNDOLOG_COUNT_PER_PAGE) + 1 == totalLogPageCnt)))
		{
			INIT_CRC32C(logMetaPageCrc);
			COMP_CRC32C(logMetaPageCrc, (void *) metaPageBuffer, writeSize);
			FIN_CRC32C(logMetaPageCrc);
			*(pg_crc32 *) (metaPageBuffer + writeSize) = logMetaPageCrc;

			ret = write(fd, (void *) metaPageBuffer, UNDO_META_PAGE_SIZE);
			if (ret != UNDO_META_PAGE_SIZE)
			{
				ereport(WARNING, (errcode(ERRCODE_DATA_EXCEPTION),
								  errmsg("[INIT UNDO] Write undolog meta info fail, "
										 "expect size(%u), real size(%u).",
										 UNDO_META_PAGE_SIZE, ret)));
				return false;
			}
		}
	}
	return true;
}

static bool
InitSegmentMeta(int fd)
{
	uint64	 writeSize = 0;
	uint32	 ret = 0;
	uint32	 loop;
	uint32	 totalUspPageCnt = 0;
	char	 metaPageBuffer[UNDO_META_PAGE_SIZE] = {'\0'};
	pg_crc32 spaceMetaPageCrc = 0;

	/* Init undospace meta, persist meta info into disk. */
	UNDO_LOG_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOSEGMENT_COUNT_PER_PAGE,
							 totalUspPageCnt);

	for (loop = 0; loop < PERSIST_UNDOLOG_COUNT; loop++)
	{
		uint32			 logno = loop;
		uint32			 offset = logno % UNDOSEGMENT_COUNT_PER_PAGE;
		UndoSegmentMeta *uspMetaPoint = NULL;

		if (logno % UNDOSEGMENT_COUNT_PER_PAGE == 0)
		{
			memset(metaPageBuffer,  0, UNDO_META_PAGE_SIZE);

			/* On last page, count of undospace meta maybe less than UNDOSEGMENT_COUNT_PER_PAGE. */
			if ((uint32) (logno / UNDOSEGMENT_COUNT_PER_PAGE) + 1 == totalUspPageCnt)
			{
				writeSize = (PERSIST_UNDOLOG_COUNT -
							 (totalUspPageCnt - 1) * UNDOSEGMENT_COUNT_PER_PAGE) *
							sizeof(UndoSegmentMeta);
			}
			else
			{
				writeSize = sizeof(UndoSegmentMeta) * UNDOSEGMENT_COUNT_PER_PAGE;
			}
		}

		uspMetaPoint =
			(UndoSegmentMeta *) (metaPageBuffer + offset * sizeof(UndoSegmentMeta));
		uspMetaPoint->version = XSTORE_UNDO_VERSION;
		uspMetaPoint->lsn = 0;
		uspMetaPoint->head = 0;
		uspMetaPoint->tail = 0;

		if ((logno + 1) % UNDOSEGMENT_COUNT_PER_PAGE == 0 ||
			(logno == PERSIST_UNDOLOG_COUNT - 1 &&
			 ((uint32) (logno / UNDOSEGMENT_COUNT_PER_PAGE) + 1 == totalUspPageCnt)))
		{
			INIT_CRC32C(spaceMetaPageCrc);
			COMP_CRC32C(spaceMetaPageCrc, (void *) metaPageBuffer, writeSize);
			FIN_CRC32C(spaceMetaPageCrc);
			*(pg_crc32 *) (metaPageBuffer + writeSize) = spaceMetaPageCrc;

			ret = write(fd, (void *) metaPageBuffer, UNDO_META_PAGE_SIZE);
			if (ret != UNDO_META_PAGE_SIZE)
			{
				ereport(WARNING, (errcode(ERRCODE_DATA_EXCEPTION),
								  errmsg("[INIT UNDO] Write undospace meta info fail, "
										 "expect size(%u), real size(%u).",
										 UNDO_META_PAGE_SIZE, ret)));
				return false;
			}
		}
	}
	return true;
}

void
init_undo_meta(void)
{
	int	 fd;
	int errorno;
	char undoFilePath[MAXPGPATH] = {'\0'};
	char tmpUndoFile[MAXPGPATH] = {'\0'};

	elog(LOG,"Begin init undo subsystem meta.");

	check_undo_dir();
	sprintf(undoFilePath, "%s", UNDO_META_FILE);
	sprintf(tmpUndoFile,  "%s_%s", UNDO_META_FILE, "tmp");

	if (access(undoFilePath, F_OK) != 0)
	{
		/* First, delete tmpUndoFile. */

		unlink(tmpUndoFile);
		fd = open(tmpUndoFile, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			errorno = errno;
			ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
							errmsg("[INIT UNDO] Open %s file failed, error (%s).",
								   tmpUndoFile, strerror(errorno))));
			return;
		}

		/* init undo ulog meta */
		if (!InitUndoLogMeta(fd))
		{
			goto ERROR_PROC;
		}
		/* init undo segment meta */
		if (!InitSegmentMeta(fd))
		{
			goto ERROR_PROC;
		}
		/* init slot segment meta */
		if (!InitSegmentMeta((fd)))
		{
			goto ERROR_PROC;
		}

		/* Flush buffer to disk and close fd. */
		fsync(fd);
		close(fd);

		/* Rename tmpUndoFile to real undoFile. */
		if (rename(tmpUndoFile, undoFilePath) != 0)
		{
			errorno = errno;
			ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
							errmsg("[INIT UNDO] Rename tmp undo meta file failed. error (%s).",
							strerror(errorno))));
		}
		ereport(LOG, (errcode(ERRCODE_DATA_EXCEPTION),
					  errmsg("[INIT UNDO] Init undo subsystem meta successfully.")));

	}

	return;

	ERROR_PROC:
	close(fd);
	unlink(tmpUndoFile);
	elog(PANIC, "[INIT UNDO] Init undo subsystem failed, exit.");
}

void
recovery_undo_meta()
{
	int fd;


	/* Ensure that the undometa file exists. */
	if (access(UNDO_META_FILE, F_OK) != 0)
	{
		ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
							errmsg("Undo meta file does't exists.")));
		return;
	}

	fd = BasicOpenFilePerm(UNDO_META_FILE, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		int errorno = errno;
		ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("Open file(%s), return code desc(%s)", UNDO_META_FILE,
							   strerror(errorno))));
		return;
	}
	pg_atomic_write_u32(&undo_sys_ctx->undo_total_size, 0);
	undo_sys_ctx->undo_meta_size = 0;
	recovery_undo_logs(fd);
	recovery_undo_segment(fd, UNDO_LOG_SEGMENT);
	recovery_undo_segment(fd, UNDO_SLOT_SEGMENT);

	/* Close fd. */
	close(fd);
}


void
set_undo_slot_rollback_finish(UndoSlotPtr slotPtr)
{
	int				 logno = (int) UNDO_PTR_GET_LOG_NO(slotPtr);
	XLogRecPtr		 lsn;
	UndoSlotBuffer	 buf;
	Page			 page;
	UndoSlot *slot;
	FullTransactionId globalRecycleXid ;

	globalRecycleXid.value = pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid);
    
    init_undo_slot_buffer(&buf);
	load_undo_slot_buffer(&buf, slotPtr);
	LockBuffer(buf.buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf.buffer);
	slot = get_undo_slot_from_buffer(&buf, slotPtr);
	Assert(slot->xid.value != InvalidTransactionId);
	Assert(slot->dbOid != InvalidOid);

	if (!can_rollback_undo_slot(slot))
	{
		ereport(
			WARNING,
			(errmsg(
				UNDOFORMAT(
					"double register rollback request, update ulog %d slot %lu xid %lu "
					"dbid %u "
					"rollback progress from start %lu to end %lu, globalRecycleXid %lu."),
				logno, slotPtr, slot->xid.value, slot->dbOid, slot->start_undo_ptr,
				slot->end_undo_ptr, globalRecycleXid.value)));
		UnlockReleaseBuffer(buf.buffer);
		return;
	}

	if (FullTransactionIdPrecedes(slot->xid, globalRecycleXid))
	{
		ereport(PANIC,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg(UNDOFORMAT("curr xid having undo %lu < globalRecycleXid %lu."),
						slot->xid.value, globalRecycleXid.value)));
	}

	/* only persist level space need update transaction slot. */
	START_CRIT_SECTION();
	slot->info |= UNDOSLOT_ROLLBACK;
	ereport(DEBUG1,
			(errmsg(UNDOFORMAT("update ulog %d slot %lu xid %lu dbid %u rollback progress "
							   "from start %lu to end %lu, globalRecycleXid %lu."),
					logno, slotPtr, slot->xid.value, slot->dbOid, slot->start_undo_ptr,
					slot->end_undo_ptr, globalRecycleXid.value)));

	MarkBufferDirty(buf.buffer);
	/* WAL log the rollback progress so it can be replayed */
	if (IS_PERSIST_LEVEL(logno))
	{
		lsn = xlog_undo_rollback_finish_write(buf, slotPtr);
		PageSetLSN(page, lsn);
	}
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf.buffer);
	return;
}

void
cleanup_undo_log(int code, Datum arg)
{
	int i;
	if (undo_log_ctx == NULL)
	{
		return;
	}
	if (MyProc != NULL)
	{
		elog(DEBUG1, UNDOFORMAT("on undo exit, pid: %d"), MyProc->pid);
	}

	for (i = 0; i < UNDO_PERSISTENCE_LEVELS; i++)
	{
		UndoPersistence upersistence = (UndoPersistence) (i);
		int				logno = undo_log_ctx->logs[upersistence];
		if (!IS_VALID_LOGNO(logno))
		{
			continue;
		}

		undo_log_ctx->logs[upersistence] = INVALID_UNDOLOG_NO;
		undolog_release(logno, upersistence);
	}
}


UndoRecPtr
undo_rec_get_prevurp(UndoRecPtr currUrp)
{
	int			   logno = UNDO_PTR_GET_LOG_NO(currUrp);
	UndoLogOffset  offset = UNDO_PTR_GET_OFFSET(currUrp);
	UndoRecordSize prevLen = undo_record_get_prerecordlen(currUrp, NULL);
	return MAKE_UNDO_REC_PTR(logno, offset - prevLen);
}

void
release_undo_slot_buffers()
{
	int i = 0;

	if (undo_sys_ctx == NULL || undo_log_ctx == NULL)
	{
		return;
	}

	for (i = 0; i < UNDO_PERSISTENCE_LEVELS; i++)
	{
		UndoPersistence upersistence = (UndoPersistence) (i);
		int				logno = undo_log_ctx->logs[upersistence];
		UndoLogControl	*ulog;
		if (!IS_VALID_LOGNO(logno))
		{
			continue;
		}
		ulog = (UndoLogControl *) undo_sys_ctx->ulogs[logno];
		if (ulog == NULL)
		{
			continue;
		}

		PG_TRY();
		{
			release_undo_slot_buffer(&ulog->txn_slot_buffer);
		}
		PG_CATCH();
		{
			FlushErrorState();
		}
		PG_END_TRY();
	}
}

static uint32
undo_segment_isused(UndoSegment *seg)
{
	if (seg->tail < seg->head)
	{
		ereport(PANIC, (errmsg(UNDOFORMAT("segment tail %lu < head %lu."), seg->tail,
							   seg->head)));
	}
	return (uint32) ((seg->tail - seg->head) / BLCKSZ);
}

void
lock_undo_segment(UndoSegment *seg)
{
	LWLockAcquire(&seg->lock, LW_EXCLUSIVE);
}
void
unlock_undo_segment(UndoSegment *seg)
{
	LWLockRelease(&seg->lock);
}


/* Create segments needed to increase end_ to newEnd. */
void
extend_undo_segment(UndoSegment *seg, int logno, UndoLogOffset offset, uint32 dbId)
{
	RelFileLocator	  rlocator;
	UndoLogOffset tail = seg->tail;
	BlockNumber	  blockno;
	SMgrRelation  reln;
	uint64		  segSize = UNDOLOG_FILE_SIZE(dbId);
	uint32		  segBlocks = UNDOLOG_FILE_BLOCKS(dbId);
	Assert(tail < offset && seg->head <= seg->tail);

	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, MAKE_UNDO_REC_PTR(logno, offset), dbId);
	reln = smgropen(rlocator, INVALID_PROC_NUMBER);
	Assert(offset % segSize == 0);

	Assert(offset <= UNDO_LOG_MAX_SIZE);

	while (tail < offset)
	{
		if ((!InRecovery) && ((int) (pg_atomic_read_u32(&undo_sys_ctx->undo_total_size)) +
								  (int) (undo_sys_ctx->undo_meta_size) >=
							  undo_max_total_size))
		{
			uint64 undoSize = (pg_atomic_read_u32(&undo_sys_ctx->undo_total_size) +
							   undo_sys_ctx->undo_meta_size) *
							  BLCKSZ / (1024 * 1024);
			uint64 limitSize = undo_max_total_size * BLCKSZ / (1024 * 1024);
			ereport(ERROR,
					(errmsg(UNDOFORMAT("undo space size %luM > limit size %luM. Please "
									   "increase the undo_max_total_size."),
							undoSize, limitSize)));
		}
		blockno = (BlockNumber) (tail / BLCKSZ + 1);
		/* Create a new undo segment. */
		smgrextend(reln, MAIN_FORKNUM, blockno, NULL, false);
		pg_atomic_fetch_add_u32(&undo_sys_ctx->undo_total_size, segBlocks);
		tail += segSize;
	}

	if (dbId == UNDO_DATA_DB_OID && seg->tail == 0)
	{
		// first extend ulog
		pg_atomic_fetch_add_u32(&undo_sys_ctx->undo_log_used_cout, 1);
		elog(DEBUG1, "first extend ulog %d add undo ulog count %d %lu %lu", logno,
			 pg_atomic_read_u32(&undo_sys_ctx->undo_log_used_cout), seg->head, seg->tail);
	}

	elog(DEBUG1,
		 UNDOFORMAT("entxend undo log, total blocks=%u, logno=%d, dbid=%u, head=%lu."),
		 pg_atomic_read_u32(&undo_sys_ctx->undo_total_size), logno, dbId, offset);
	seg->tail = offset;
	return;
}

/* Unlink undo segment file from startOffset to endOffset. */
void
unlink_undo_segment(UndoSegment *seg, int logno, UndoLogOffset offset, uint32 dbId)
{
	RelFileLocator	  rlocator;
	UndoLogOffset head = seg->head;
	SMgrRelation  reln;
	uint64		  segSize = UNDOLOG_FILE_SIZE(dbId);
	uint32		  segBlocks = UNDOLOG_FILE_BLOCKS(dbId);
	uint32		  releaseBlocks = 0;
	Assert(head < offset && seg->head <= seg->tail);
	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, MAKE_UNDO_REC_PTR(logno, offset), dbId);
	reln = smgropen(rlocator, INVALID_PROC_NUMBER);

	Assert(offset % segSize == 0);
	seg->head = offset;

	while (head < offset)
	{
		/* delete a new undo segment. */
		reln->smgr_targblock = (head / BLCKSZ);
		undo_unlink(reln, InRecovery) ;
		if (pg_atomic_read_u32(&undo_sys_ctx->undo_total_size) < segBlocks)
		{
			ereport(
				PANIC,
				(errmsg(UNDOFORMAT("unlink undo log, total blocks=%u < segment size."),
						pg_atomic_read_u32(&undo_sys_ctx->undo_total_size))));
		}
		pg_atomic_fetch_sub_u32(&undo_sys_ctx->undo_total_size, segBlocks);
		releaseBlocks += segBlocks;
		head += segSize;
	}
	smgrclose(reln);
	elog(DEBUG1,
		 UNDOFORMAT("unlink undo log, total blocks=%u, logno=%d, dbid=%u, new head=%lu "
					",release blocks=%u."),
		 pg_atomic_read_u32(&undo_sys_ctx->undo_total_size), logno, dbId, offset,
		 releaseBlocks);
	return;
}

void
create_non_exists_undo_file(UndoSegment *seg, int logno, uint32 dbId)
{
	UndoLogOffset offset = seg->head;
	RelFileLocator	  rlocator;
	BlockNumber	  blockno;
	uint64		  segSize = UNDOLOG_FILE_SIZE(dbId);
	uint32		  segBlocks = UNDOLOG_FILE_BLOCKS(dbId);
	SMgrRelation  reln;
	if (seg->head == seg->tail)
	{
		return;
	}
	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, MAKE_UNDO_REC_PTR(logno, offset), dbId);
	reln = smgropen(rlocator, INVALID_PROC_NUMBER);

	while (offset < seg->tail)
	{
		blockno = (BlockNumber) (offset / BLCKSZ + 1);
		reln->smgr_targblock = blockno;
		if (!smgrexists(reln, MAIN_FORKNUM))
		{
			smgrextend(reln, MAIN_FORKNUM, blockno, NULL, false);
			elog(DEBUG1, UNDOFORMAT("undo file not exists, logno %d, blockno=%u."), logno,
				 blockno);
			pg_atomic_fetch_add_u32(&undo_sys_ctx->undo_total_size, segBlocks);
		}
		offset += segSize;
	}
	smgrclose(reln);
	return;
}

/*
 * Persist undosegment metadata to disk. The fomart as follows
 * ----------|--------|---------|---------|--------------|
 * undoMeta |undoMeta|undoMeta |undoMeta | pageCRC(32bit)
 * |->--------------------512---------------------------<-|
 */
void
checkpoint_undo_segment(int fd, UndoSegmentType stype)
{
	bool	   retry = false;
	bool	   needFlushMetaPage = false;
	uint32	   ret = 0;
	uint32	   totalPageCnt = 0;
	uint32	   uspOffset = 0;
	uint64	   writeSize = 0;
	pg_crc32   metaPageCrc = 0;
	XLogRecPtr flushLsn = InvalidXLogRecPtr;
	char	   uspMetaPagebuffer[UNDO_WRITE_SIZE] = {'\0'};	 // 8page = 4k /per writer
	uint64	   currWritePos = 0;
	uint32	   cycle = 0;
	int		   segPageBegin = 0;
	uint32	   loop;

	Assert(fd > 0);
	if (stype == UNDO_LOG_SEGMENT)
	{
		UNDO_LOG_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOLOG_COUNT_PER_PAGE,
								 totalPageCnt);
		lseek(fd, totalPageCnt * UNDO_META_PAGE_SIZE, SEEK_SET);
	}
	else
	{
		uint32 seek = 0;
		UNDO_LOG_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOLOG_COUNT_PER_PAGE,
								 totalPageCnt);
		seek = totalPageCnt * UNDO_META_PAGE_SIZE;
		UNDO_LOG_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOSEGMENT_COUNT_PER_PAGE,
								 totalPageCnt);
		seek += totalPageCnt * UNDO_META_PAGE_SIZE;
		lseek(fd, seek, SEEK_SET);
	}
	segPageBegin = lseek(fd, 0, SEEK_CUR);
	if (segPageBegin < 0)
	{
		ereport(
			ERROR,
			(errcode_for_file_access(),
			 errmsg(UNDOFORMAT("could not seek start position in undo metadata: %m"))));
		return;
	}
	/* Get total page count of storing all undosegments. */
	UNDOSEGMENT_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOSEGMENT_COUNT_PER_PAGE,
								totalPageCnt);

	for (loop = 0; loop < PERSIST_UNDOLOG_COUNT; loop++)
	{
		UndoSegment		*usp = NULL;
		UndoSegmentMeta *uspMetaPointer = NULL;
		if (loop % UNDOSEGMENT_COUNT_PER_WRITE == 0)
		{
			cycle = loop;  // record start ulog.
			memset(uspMetaPagebuffer, 0, UNDO_WRITE_SIZE);

			if ((uint32) (PERSIST_UNDOLOG_COUNT - loop) < UNDOSEGMENT_COUNT_PER_WRITE)
			{
				writeSize =
					((PERSIST_UNDOLOG_COUNT - loop) / UNDOSEGMENT_COUNT_PER_WRITE + 1) *
					UNDO_META_PAGE_SIZE;
			}
			else
			{
				writeSize = UNDO_WRITE_SIZE;
			}
		}
		// check if need flush this 4k buffer
		if (undo_sys_ctx->ulogs[loop] != NULL)
		{
			if (stype == UNDO_LOG_SEGMENT)
			{
				usp = &((UndoLogControl *) undo_sys_ctx->ulogs[loop])->undo_data_seg;
			}
			else
			{
				usp = &((UndoLogControl *) undo_sys_ctx->ulogs[loop])->undo_txn_seg;
			}
			/* If one segment on a meta page is dirty, write total segments of this 4K page. */
			if (usp->dirty)
			{
				elog(DEBUG1,
					 UNDOFORMAT("undo seg metadata find dirty ulog %u. segtype %d ,"
								"head %lu, tail %lu, lsn %lu "),
					 loop, stype, usp->head, usp->tail, usp->lsn);
				needFlushMetaPage = true;
			}
		}

		if (needFlushMetaPage && ((loop + 1) % UNDOSEGMENT_COUNT_PER_WRITE == 0 ||
								  (loop + 1) == PERSIST_UNDOLOG_COUNT))
		{
			// do the meta page update.
			while (cycle <= loop)
			{
				/* Locate the undosegemnt on the undo page, then refresh. */
				char *pageOffset =
					uspMetaPagebuffer +
					((cycle % UNDOSEGMENT_COUNT_PER_WRITE) / UNDOSEGMENT_COUNT_PER_PAGE) *
						UNDO_META_PAGE_SIZE;
				/* Locate the offset in meta page */
				uspOffset = cycle % UNDOSEGMENT_COUNT_PER_PAGE;

				uspMetaPointer =
					(UndoSegmentMeta *) (pageOffset +
										 uspOffset * sizeof(UndoSegmentMeta));

				if (undo_sys_ctx->ulogs[cycle] != NULL)
				{
					if (stype == UNDO_LOG_SEGMENT)
					{
						usp = &((UndoLogControl *) undo_sys_ctx->ulogs[cycle])->undo_data_seg;
					}
					else
					{
						usp = &((UndoLogControl *) undo_sys_ctx->ulogs[cycle])->undo_txn_seg;
					}
					lock_undo_segment(usp);
					/* Set the initial value of flushLsn  */
					if (cycle % UNDOSEGMENT_COUNT_PER_WRITE == 0)
					{
						flushLsn = usp->lsn;
					}
					else
					{
						/* Pick out max lsn of total undosegments on one meta page. */
						if (usp->lsn > flushLsn)
						{
							flushLsn = usp->lsn;
						}
					}

					uspMetaPointer->version = XSTORE_UNDO_VERSION;
					uspMetaPointer->lsn = usp->lsn;
					uspMetaPointer->head = usp->head;
					uspMetaPointer->tail = usp->tail;
					
					usp->dirty = false;
					unlock_undo_segment(usp);
				}
				else
				{
					uspMetaPointer->version = XSTORE_UNDO_VERSION;
					uspMetaPointer->lsn = 0;
					uspMetaPointer->head = 0;
					uspMetaPointer->tail = 0;
				}

				/* compute the crc for this page */
				if ((cycle + 1) % UNDOSEGMENT_COUNT_PER_PAGE == 0 ||
					cycle == PERSIST_UNDOLOG_COUNT - 1)
				{
					uint64 crcSize;
					if (cycle == PERSIST_UNDOLOG_COUNT - 1)
					{
						crcSize = (PERSIST_UNDOLOG_COUNT % UNDOSEGMENT_COUNT_PER_PAGE) *
								  sizeof(UndoSegmentMeta);
					}
					else
					{
						crcSize = UNDOSEGMENT_COUNT_PER_PAGE * sizeof(UndoSegmentMeta);
					}
					/* Flush wal buffer of undo xlog first. */
					INIT_CRC32C(metaPageCrc);
					COMP_CRC32C(metaPageCrc, (void *) pageOffset, crcSize);
					FIN_CRC32C(metaPageCrc);

					/* Store CRC behind the last undosegment meta on the page. */
					*(pg_crc32 *) (pageOffset + crcSize) = metaPageCrc;
				}

				if ((cycle + 1) % UNDOSEGMENT_COUNT_PER_WRITE == 0 ||
					cycle == PERSIST_UNDOLOG_COUNT - 1)
				{
					XLogFlush(flushLsn);  // wait flush lsn done
					currWritePos =
						lseek(fd,
							  segPageBegin +
								  (cycle / UNDOSEGMENT_COUNT_PER_WRITE) * UNDO_WRITE_SIZE,
							  SEEK_SET);
				RE_WRITE:
					lseek(fd, currWritePos, SEEK_SET);
					ret = write(fd, uspMetaPagebuffer, writeSize);
					if (ret != writeSize && !retry)
					{
						retry = true;
						goto RE_WRITE;
					}
					else if (ret != writeSize && retry)
					{
						ereport(ERROR,
								(errmsg(UNDOFORMAT("Write undo meta page failed expect "
												   "size(%lu) real size(%u)."),
										writeSize, ret)));
						return;
					}
					elog(DEBUG1,
						 UNDOFORMAT("undo seg metadata write loop %u. segtype %d"), cycle,
						 stype);
					needFlushMetaPage = false;
				}
				cycle++;
			}
		}
	}
}

void
recovery_undo_segment(int fd, UndoSegmentType stype)
{
	uint32	 logno = 0;
	uint32	 spaceMetaSize = 0;
	uint32	 totalPageCnt = 0;
	pg_crc32 pageCrcVal = 0; /* CRC store in undo meta page */
	pg_crc32 comCrcVal = 0;	 /* calculating CRC current */
	char	*uspMetaBuffer = NULL;

	char *persistBlock = (char *) palloc0(UNDO_META_PAGE_SIZE * PAGES_READ_NUM);

	Assert(fd > 0);
	if (stype == UNDO_LOG_SEGMENT)
	{
		UNDOSEGMENT_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOLOG_COUNT_PER_PAGE,
									totalPageCnt);
		lseek(fd, totalPageCnt * UNDO_META_PAGE_SIZE, SEEK_SET);
	}
	else
	{
		uint32 seek = 0;
		UNDOSEGMENT_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOLOG_COUNT_PER_PAGE,
									totalPageCnt);
		seek = totalPageCnt * UNDO_META_PAGE_SIZE;
		UNDOSEGMENT_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOSEGMENT_COUNT_PER_PAGE,
									totalPageCnt);
		seek += totalPageCnt * UNDO_META_PAGE_SIZE;
		lseek(fd, seek, SEEK_SET);
	}

	UNDOSEGMENT_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOSEGMENT_COUNT_PER_PAGE,
								totalPageCnt);
	spaceMetaSize = totalPageCnt * UNDO_META_PAGE_SIZE / BLCKSZ;
	undo_sys_ctx->undo_meta_size += spaceMetaSize;

	for (logno = 0; logno < PERSIST_UNDOLOG_COUNT; logno++)
	{
		UndoSegmentMeta *uspMetaInfo = NULL;
		UndoLogControl		*ulog = NULL;
		UndoSegment		*usp = NULL;
		int				 offset = 0;
		uint32			 ret = 0;
		uint32			 count = 0;
		if (logno % (UNDOSEGMENT_COUNT_PER_PAGE * PAGES_READ_NUM) == 0)
		{
			Size readSize;
			if ((uint32) (PERSIST_UNDOLOG_COUNT - logno) <
				UNDOSEGMENT_COUNT_PER_PAGE * PAGES_READ_NUM)
			{
				readSize =
					((uint32) (PERSIST_UNDOLOG_COUNT - logno) / UNDOSEGMENT_COUNT_PER_PAGE +
					 1) *
					UNDO_META_PAGE_SIZE;
			}
			else
			{
				readSize = UNDO_META_PAGE_SIZE * PAGES_READ_NUM;
			}
			memset(persistBlock, 0,
						  UNDO_META_PAGE_SIZE * PAGES_READ_NUM);
			ret = read(fd, persistBlock, readSize);
			if (ret != readSize)
			{
				ereport(
					ERROR,
					(errmsg(UNDOFORMAT(
								"Read undo meta page, expect size(%lu), real size(%u)."),
							readSize, ret)));
				return;
			}
		}

		if (logno % UNDOSEGMENT_COUNT_PER_PAGE == 0)
		{
			uspMetaBuffer =
				persistBlock + ((logno % (UNDOSEGMENT_COUNT_PER_PAGE * PAGES_READ_NUM)) /
								UNDOSEGMENT_COUNT_PER_PAGE) *
								   UNDO_META_PAGE_SIZE;
			count = UNDOSEGMENT_COUNT_PER_PAGE;
			if ((uint32) (PERSIST_UNDOLOG_COUNT - logno) < UNDOSEGMENT_COUNT_PER_PAGE)
			{
				count = PERSIST_UNDOLOG_COUNT - logno;
			}
			/* Get page CRC from uspMetaBuffer. */
			pageCrcVal = *(pg_crc32 *) (uspMetaBuffer + sizeof(UndoSegmentMeta) * count);
			/* 
             * Calculate the CRC value based on all undospace meta information stored on the page. 
             * Then compare with pageCrcVal.
             */
			INIT_CRC32C(comCrcVal);
			COMP_CRC32C(comCrcVal, (void *) uspMetaBuffer,
						sizeof(UndoSegmentMeta) * count);
			FIN_CRC32C(comCrcVal);
			if (!EQ_CRC32C(pageCrcVal, comCrcVal))
			{
				ereport(ERROR,
						(errmsg(UNDOFORMAT("Undo meta CRC calculated(%u) is different "
										   "from CRC recorded(%u) in page."),
								comCrcVal, pageCrcVal)));
				return;
			}
		}

		offset = logno % UNDOSEGMENT_COUNT_PER_PAGE;
		uspMetaInfo =
			(UndoSegmentMeta *) (uspMetaBuffer + offset * sizeof(UndoSegmentMeta));
		if (uspMetaInfo->tail == 0 &&
			(undo_sys_ctx == NULL || undo_sys_ctx->ulogs[logno] == NULL))
		{
			continue;
		}
		ulog = get_undo_log(logno);
		usp = UndoGetUndoSegment(ulog, stype);
		usp->dirty = false;
		usp->lsn = uspMetaInfo->lsn;
		usp->head = uspMetaInfo->head;
		usp->tail = (uspMetaInfo->tail);
		if (usp->head < usp->tail)
		{
			elog(DEBUG1,
				 UNDOFORMAT("undo segment metadata recovery ulog %u. "
							"  segtype %d head %lu tail %lu lsn %lu"),
				 logno, stype, usp->head, usp->tail, usp->lsn);
		}
		if (stype == UNDO_LOG_SEGMENT)
		{
			create_non_exists_undo_file(usp, logno, UNDO_DATA_DB_OID);
			if (usp->tail > 0)
			{
				// recover one ulog segment
				if (UNDO_PTR_GET_OFFSET(UndoGetInsertURecPtr(ulog)) >=
						UNDO_LOG_MAX_SIZE &&
					UndoGetAllocateTSlotPtr(ulog) == UndoGetRecycleTSlotPtr(ulog))
				{
					ulog->frozen = true;
				}
				else
				{
					pg_atomic_fetch_add_u32(&undo_sys_ctx->undo_log_used_cout, 1);
				}
			}
		}
		else
		{
			create_non_exists_undo_file(usp, logno, UNDO_TXN_DB_OID);
		}
		pg_atomic_fetch_add_u32(&undo_sys_ctx->undo_total_size, undo_segment_isused(usp));
	}
	pfree(persistBlock);
}



void
init_undo_slot(UndoSlot *slot, FullTransactionId xid, Oid dbId)
{
	Assert(TransactionIdIsValid(xid.value));
	slot->xid = xid;
	slot->start_undo_ptr = INVALID_UNDO_REC_PTR;
	slot->end_undo_ptr = INVALID_UNDO_REC_PTR;
	slot->info |= UNDOSLOT_INIT;
	slot->dbOid = dbId;
}

void
update_undo_slot(UndoSlot *slot, UndoRecPtr start, UndoRecPtr end)
{
    Assert(UNDO_PTR_GET_LOG_NO(start) == UNDO_PTR_GET_LOG_NO(end));
	Assert(start <= end);
	slot->end_undo_ptr = end;
	if (slot->start_undo_ptr == INVALID_UNDO_REC_PTR)
	{
		pg_write_barrier();
		slot->start_undo_ptr = start;
	}
	return;
}

bool
can_rollback_undo_slot(UndoSlot *slot)
{
	return !(slot->info & UNDOSLOT_ROLLBACK);
}

static inline void
undo_slot_buffer_setinfo(UndoSlotBuffer *slotBuff, uint8 info)
{
	slotBuff->info |= info;
}

static inline bool
undo_slot_buffer_is_loaded(UndoSlotBuffer *slotBuff)
{
	return slotBuff->info & UNDOSLOT_BUFFER_LOAD;
}

static inline void
undo_slot_buffer_unsetinfo(UndoSlotBuffer *slotBuff, uint8 info)
{
	slotBuff->info &= ~info;
}

void
xlog_load_undo_slot_buffer(UndoSlotBuffer *slot_buff, XLogReaderState *record, UndoSlotPtr slot_ptr)
{
	RelFileLocator	   rlocator;
	ReadBufferMode rbm = RBM_NORMAL;
	Buffer buff;
	XLogRedoAction action;
	if (!BufferIsInvalid(slot_buff->buffer))
	{
		if (undo_slot_buffer_is_loaded(slot_buff))
		{
			release_undo_slot_buffer(slot_buff);
			elog(DEBUG2,
					UNDOFORMAT("ulog %d release pre buffer %d by blk %u info %u."),
					(int) (UNDO_PTR_GET_LOG_NO(slot_ptr)), slot_buff->buffer,
					slot_buff->blkno, slot_buff->info);
		}
	}
	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, slot_ptr, UNDO_TXN_DB_OID);
	slot_buff->blkno = UNDO_PTR_GET_BLOCK_NUM(slot_ptr);

	action = xlog_undo_read_buffer_for_redo(record, rlocator, MAIN_FORKNUM, slot_buff->blkno, 
		rbm, false, &buff);
	/* buffer must be in lock */
	if (action == BLK_NOTFOUND)
		slot_buff->buffer = InvalidBuffer;
	else
		slot_buff->buffer = buff;
	
	if (BufferIsValid(slot_buff->buffer))
	{
		BufferDesc *buf = GetBufferDescriptor(slot_buff->buffer - 1);
		if (!check_undo_slot_buffer_valid(buf, UNDO_PTR_GET_LOG_NO(slot_ptr), slot_ptr))
		{
			ereport(PANIC,
					(errmsg(UNDOFORMAT("invalid cached slot buffer %d slot ptr %lu."),
							slot_buff->buffer, slot_ptr)));
		}
		elog(DEBUG1,
			UNDOFORMAT("ulog %d prepare buffer %d by slot ptr %lu blk %u info %u."),
			(int) (UNDO_PTR_GET_LOG_NO(slot_ptr)), slot_buff->buffer, slot_ptr,
			slot_buff->blkno, slot_buff->info);
	}
	undo_slot_buffer_setinfo(slot_buff, UNDOSLOT_BUFFER_LOAD);
}

void
load_undo_slot_buffer(UndoSlotBuffer *slot_buff, UndoSlotPtr slot_ptr)
{
	RelFileLocator	   rlocator;
	ReadBufferMode rbm = RBM_NORMAL;
	if (!BufferIsInvalid(slot_buff->buffer))
	{
		if (undo_slot_buffer_is_loaded(slot_buff))
		{
			release_undo_slot_buffer(slot_buff);
			elog(DEBUG2,
					UNDOFORMAT("ulog %d release pre buffer %d by blk %u info %u."),
					(int) (UNDO_PTR_GET_LOG_NO(slot_ptr)), slot_buff->buffer,
					slot_buff->blkno, slot_buff->info);
		}
	}
	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, slot_ptr, UNDO_TXN_DB_OID);
	slot_buff->blkno = UNDO_PTR_GET_BLOCK_NUM(slot_ptr);
	slot_buff->buffer = ReadUndoBufferWithoutRelcache(
		rlocator, MAIN_FORKNUM, slot_buff->blkno, rbm, NULL,
		RELPERSISTENCE_PERMANENT);
	if (BufferIsValid(slot_buff->buffer))
	{
		BufferDesc *buf = GetBufferDescriptor(slot_buff->buffer - 1);
		if (!check_undo_slot_buffer_valid(buf, UNDO_PTR_GET_LOG_NO(slot_ptr), slot_ptr))
		{
			ereport(PANIC,
					(errmsg(UNDOFORMAT("invalid cached slot buffer %d slot ptr %lu."),
					slot_buff->buffer, slot_ptr)));
		}
		elog(DEBUG1,
			UNDOFORMAT("ulog %d prepare buffer %d by slot ptr %lu blk %u info %u."),
			(int) (UNDO_PTR_GET_LOG_NO(slot_ptr)), slot_buff->buffer, slot_ptr,
			slot_buff->blkno, slot_buff->info);
	}
	undo_slot_buffer_setinfo(slot_buff, UNDOSLOT_BUFFER_LOAD);
	if(!InRecovery && TopTransactionResourceOwner) {
		// keep slot buffer in TopTransactionResourceOwner
		ResourceOwnerEnlarge(TopTransactionResourceOwner);
		ResourceOwnerRememberBuffer(TopTransactionResourceOwner, slot_buff->buffer);
		ResourceOwnerForgetBuffer(CurrentResourceOwner, slot_buff->buffer);
	}
}

void
prepare_undo_slot_buffer(UndoSlotBuffer *slotBuff, XLogReaderState *record, void *meta, UndoSlotPtr slotPtr)
{
	{
		RelFileLocator	   rlocator;
		ReadBufferMode rbm = RBM_NORMAL;
		if (!BufferIsInvalid(slotBuff->buffer))
		{
			if (undo_slot_buffer_is_loaded(slotBuff))
			{
				release_undo_slot_buffer(slotBuff);
				elog(DEBUG2,
					 UNDOFORMAT("ulog %d release pre buffer %d by blk %u info %u."),
					 (int) (UNDO_PTR_GET_LOG_NO(slotPtr)), slotBuff->buffer,
					 slotBuff->blkno, slotBuff->info);
			}
		}
		UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, slotPtr, UNDO_TXN_DB_OID);
		slotBuff->blkno = UNDO_PTR_GET_BLOCK_NUM(slotPtr);

		if (!InRecovery)
		{
			if (UNDO_PTR_GET_PAGE_OFFSET(slotPtr) == UNDO_LOG_BLOCK_HEADER_SIZE)
				rbm = RBM_ZERO_AND_LOCK;
			/* no lock buffer */
			slotBuff->buffer = ReadUndoBufferWithoutRelcache(
				rlocator, MAIN_FORKNUM, slotBuff->blkno, rbm, NULL,
				RELPERSISTENCE_PERMANENT);
			
		} else {
			Buffer buff;
			XLogRedoAction action;
			if (xlog_undo_meta_is_intslot((xl_undo_meta*)meta))
				rbm = RBM_ZERO_AND_LOCK;
			action = xlog_undo_read_buffer_for_redo(record, rlocator, MAIN_FORKNUM, slotBuff->blkno, 
				rbm, false, &buff);
			/* buffer must be in lock */
			if (action == BLK_NOTFOUND)
				slotBuff->buffer = InvalidBuffer;
			else
				slotBuff->buffer = buff;
		}
		
		if (BufferIsValid(slotBuff->buffer))
		{
			BufferDesc *buf = GetBufferDescriptor(slotBuff->buffer - 1);
			if (!check_undo_slot_buffer_valid(buf, UNDO_PTR_GET_LOG_NO(slotPtr), slotPtr))
			{
				ereport(PANIC,
						(errmsg(UNDOFORMAT("invalid cached slot buffer %d slot ptr %lu."),
								slotBuff->buffer, slotPtr)));
			}
			elog(DEBUG1,
				UNDOFORMAT("ulog %d prepare buffer %d by slot ptr %lu blk %u info %u."),
				(int) (UNDO_PTR_GET_LOG_NO(slotPtr)), slotBuff->buffer, slotPtr,
				slotBuff->blkno, slotBuff->info);
		}
		undo_slot_buffer_setinfo(slotBuff, UNDOSLOT_BUFFER_LOAD);
		if(!InRecovery && TopTransactionResourceOwner) {
			// keep slot buffer in TopTransactionResourceOwner
			ResourceOwnerEnlarge(TopTransactionResourceOwner);
			ResourceOwnerRememberBuffer(TopTransactionResourceOwner, slotBuff->buffer);
			ResourceOwnerForgetBuffer(CurrentResourceOwner, slotBuff->buffer);
		}
	}
}

void advance_undo_slot_page_lower(Page page, UndoSlotPtr slotPtr)
{
	PageHeader phdr = (PageHeader) page;
	UndoSlotOffset slotOffset = (UNDO_PTR_GET_OFFSET(slotPtr)) % BLCKSZ;
	if (slotOffset + sizeof(UndoSlot) > BLCKSZ)
		ereport(PANIC,
					(errmsg(UNDOFORMAT("slotptr=%lu excess block limit"), slotPtr)));
	phdr->pd_lower = (LocationIndex)slotOffset + sizeof(UndoSlot);
}

UndoSlot *
get_undo_slot_from_buffer(UndoSlotBuffer *slotBuff, UndoSlotPtr slotPtr)
{
	UndoSlotOffset slotOffset = (UNDO_PTR_GET_OFFSET(slotPtr)) % BLCKSZ;
	Page		   page = BufferGetPage(slotBuff->buffer);
	if (PageIsNew(page))
	{
		if ((slotOffset != UNDO_LOG_BLOCK_HEADER_SIZE) && !InRecovery)
		{
			ereport(
				WARNING,
				(errmsg(UNDOFORMAT("INIT UNDO PAGE: slotptr=%lu, blockno=%u"), slotPtr,
						GetBufferDescriptor(slotBuff->buffer - 1)->tag.blockNum)));
		}
		PageInit(page, BLCKSZ, 0);
		if (!InRecovery)
			slotBuff->zero = true;
	}

	return (UndoSlot *) ((char *) page + slotOffset);
}

void
release_undo_slot_buffer(UndoSlotBuffer *slotBuff)
{
	if (!BufferIsInvalid(slotBuff->buffer))
	{
		if (undo_slot_buffer_is_loaded(slotBuff))
		{
			if(!InRecovery && TopTransactionResourceOwner) {
				ResourceOwnerEnlarge(CurrentResourceOwner);
				ResourceOwnerRememberBuffer(CurrentResourceOwner, slotBuff->buffer);
				ResourceOwnerForgetBuffer(TopTransactionResourceOwner, slotBuff->buffer);
			}
			

			elog(DEBUG1, UNDOFORMAT("release buffer %d by blk %u info %u."),
				 slotBuff->buffer, slotBuff->blkno, slotBuff->info);
			ReleaseBuffer(slotBuff->buffer);
			undo_slot_buffer_unsetinfo(slotBuff, UNDOSLOT_BUFFER_LOAD);
		}
	}
}

bool
check_undo_slot_buffer_valid(BufferDesc *buf, int logno, UndoSlotPtr slotPtr)
{
	if (buf->tag.dbOid == UNDO_TXN_DB_OID &&
		buf->tag.spcOid == DEFAULTTABLESPACE_OID &&
		(int) (buf->tag.relNumber) == LOGNO_TO_REL_NUMBER(logno) && buf->tag.forkNum == MAIN_FORKNUM &&
		buf->tag.blockNum == UNDO_PTR_GET_BLOCK_NUM(slotPtr))
	{
		return true;
	}
	else
	{
		return false;
	}
}

void
init_undo_slot_buffer(UndoSlotBuffer *slotBuff)
{
	slotBuff->blkno = InvalidBlockNumber;
	slotBuff->buffer = InvalidBuffer;
	slotBuff->info = UNDOSLOT_BUFFER_INIT;
}

UndoSlotPtr
get_next_slotptr(UndoSlotPtr slotPtr)
{
	UndoSlotOffset slotOffset = UNDO_PTR_GET_OFFSET(slotPtr);
	BlockNumber	   block = slotOffset / BLCKSZ;
	UndoSlotOffset blkOffset = slotOffset % BLCKSZ;
	UndoSlotOffset offset = blkOffset + MAXALIGN(sizeof(UndoSlot));
	if (BLCKSZ - offset < MAXALIGN(sizeof(UndoSlot)))
	{
		offset = (block + 1) * BLCKSZ + UNDO_LOG_BLOCK_HEADER_SIZE;
	}
	else
	{
		offset += block * BLCKSZ;
	}
	Assert(offset <= UNDO_LOG_MAX_SIZE);
	return MAKE_UNDO_REC_PTR(UNDO_PTR_GET_LOG_NO(slotPtr), offset);
}

bool
undolog_attach(UndoLogControl *ulog)
{
	uint32 expected = UNDO_LOG_DETACHED;
	while (
		!pg_atomic_compare_exchange_u32(&ulog->attached, &expected, UNDO_LOG_ATTACHED))
	{
		expected = UNDO_LOG_DETACHED;
	}
	return true;
}
bool
undolog_detach(UndoLogControl *ulog)
{
	uint32 expected = UNDO_LOG_ATTACHED;
	while (
		!pg_atomic_compare_exchange_u32(&ulog->attached, &expected, UNDO_LOG_DETACHED))
	{
		expected = UNDO_LOG_ATTACHED;
	}
	return true;
}

void
undolog_advance_insert_urecptr(UndoLogControl *ulog, uint64 oldInsert, uint64 size)
{
	ulog->insert_offset = UNDO_LOG_OFFSET_PLUS_USABLE_BYTES(oldInsert, size);
}

void
undolog_set_allocate_slotptr(UndoLogControl *ulog, UndoSlotPtr allocate)
{
	ulog->alloc_slot_offset = UNDO_PTR_GET_OFFSET(allocate);
}

UndoRecPtr
undolog_calc_insert_urecptr(UndoLogControl *ulog, uint64 oldInsert, uint64 size)
{
	return MAKE_UNDO_REC_PTR(ulog->logno, UNDO_LOG_OFFSET_PLUS_USABLE_BYTES(oldInsert, size));
}

void
lock_undo_log(UndoLogControl *ulog)
{
	(void) LWLockAcquire(&ulog->lock, LW_EXCLUSIVE);
}
void
unlock_undo_log(UndoLogControl *ulog)
{
	(void) LWLockRelease(&ulog->lock);
}

bool
undolog_check_need_switch(UndoLogControl *ulog, UndoRecordSize size)
{
	UndoLogOffset newInsert =
		UNDO_LOG_OFFSET_PLUS_USABLE_BYTES(ulog->insert_offset, size);

	if (unlikely(newInsert > UNDO_LOG_MAX_SIZE))
	{
		return true;
	}
	return false;
}

bool
undolog_check_recycle(UndoLogControl *ulog, UndoRecPtr starturp, UndoRecPtr endurp)
{
	int start_logno = UNDO_PTR_GET_LOG_NO(starturp);
	int end_logno = UNDO_PTR_GET_LOG_NO(endurp);
	UndoLogOffset start = UNDO_PTR_GET_OFFSET(starturp);
    UndoLogOffset end = UNDO_PTR_GET_OFFSET(endurp);
	Assert(start == ulog->force_discard_offset);

	if ((start_logno == end_logno) && (ulog->force_discard_offset <= ulog->insert_offset) &&
               (end <= ulog->insert_offset) && (start < end))
	{
		return true;
	}
	return false;
}

/* 
 * Check whether the undo record is discarded or not. If it's already discarded
 * return false otherwise return true.
 */
UndoRecordState
undolog_check_undo_record_valid(UndoLogControl *ulog, UndoLogOffset offset, bool checkForce,
							 FullTransactionId *lastXid)
{
	Assert((offset < UNDO_LOG_MAX_SIZE) && (offset >= UNDO_LOG_BLOCK_HEADER_SIZE));
	Assert(ulog->force_discard_offset <= ulog->insert_offset);

	if (offset >= ulog->insert_offset)
	{
		elog(
			DEBUG1,
			UNDOFORMAT("The undo record not insert yet: logno=%d, insert=%lu, offset=%lu."),
			ulog->logno, ulog->insert_offset, offset);
		return UNDO_RECORD_NOT_INSERT;
	}
	if (offset >= ulog->force_discard_offset)
	{
		return UNDO_RECORD_NORMAL;
	}
	if (lastXid != NULL)
	{
		*lastXid = ulog->recycle_xid;
	}
	if (offset >= ulog->discard_offset && checkForce)
	{
		FullTransactionId recycleXmin;
		FullTransactionId oldestXmin = get_full_oldest_xmin();
		recycleXmin = oldestXmin;
		if (FullTransactionIdPrecedes(ulog->recycle_xid, recycleXmin))
		{
			elog(DEBUG2,
				 UNDOFORMAT("oldestxmin %lu, recycleXmin %lu > recyclexid %lu: logno=%d,"
							"forceDiscardURecPtr=%lu, discardURecPtr=%lu, offset=%lu."),
				 oldestXmin.value, recycleXmin.value, ulog->recycle_xid.value, ulog->logno,
				 ulog->force_discard_offset, ulog->discard_offset, offset);
			return UNDO_RECORD_DISCARD;
		}
		elog(DEBUG1,
			 UNDOFORMAT(
				 "The record has been force recycled: logno=%d, forceDiscardURecPtr=%lu, "
				 "discardURecPtr=%lu, offset=%lu."),
			 ulog->logno, ulog->force_discard_offset, ulog->discard_offset, offset);
		return UNDO_RECORD_FORCE_DISCARD;
	}
	return UNDO_RECORD_DISCARD;
}

/*
 * Drop all buffers for the given undo log, from the start to end.
 */
void
forget_undo_log_buffers(UndoLogControl *ulog, UndoLogOffset start, UndoLogOffset end,
						 uint32 dbId)
{
	BlockNumber startBlock;
	BlockNumber endBlock;
	RelFileLocator rlocator;

	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, MAKE_UNDO_REC_PTR(ulog->logno, start), dbId);
	startBlock = start / BLCKSZ;
	endBlock = end / BLCKSZ;

	while (startBlock < endBlock)
	{
		ForgetBuffer(rlocator, MAIN_FORKNUM, startBlock);
		ForgetLocalBuffer(rlocator, MAIN_FORKNUM, startBlock++);
	}
	return;
}


UndoSlot *
undolog_alloc_undo_slot(UndoLogControl *ulog, UndoSlotPtr slotPtr, FullTransactionId xid,
							 Oid dbid, bool realloc)
{
	UndoSlot *slot = NULL;
	if (!FullTransactionIdEquals(xid, GetTopFullTransactionId()))
	{
		ereport(PANIC, (errmsg(UNDOFORMAT("init slot %lu xid %lu but not topxid %lu."),
							   slotPtr, xid.value, GetTopFullTransactionId().value)));
	}
	slot = get_undo_slot_from_buffer(&ulog->txn_slot_buffer, slotPtr);
	if (!realloc)
	{
		init_undo_slot(slot, xid, dbid);
		ulog->alloc_slot_offset = UNDO_PTR_GET_OFFSET(get_next_slotptr(slotPtr));
	}
	Assert(slot->dbOid == MyDatabaseId && FullTransactionIdIsValid(slot->xid));
	Assert(ulog->alloc_slot_offset >= ulog->recycle_slot_offset);
	return slot;
}

UndoRecPtr
undolog_alloc_space(UndoLogControl *ulog, uint64 size)
{
	UndoLogOffset oldInsert = ulog->insert_offset;
	UndoLogOffset newInsert =
		UNDO_LOG_OFFSET_PLUS_USABLE_BYTES(oldInsert, size);	 // 这里计算包含了新block的头.
	Assert(newInsert % UNDOLOG_DAT_FILE_MAXSIZE != 0);
	if (unlikely(newInsert > ulog->undo_data_seg.tail))
	{
		UndoRecPtr	   prevTail;
		XLogRecPtr	   lsn;
		xl_undolog_extend undoExtend;

		lock_undo_segment(&ulog->undo_data_seg);
		prevTail = MAKE_UNDO_REC_PTR(ulog->logno, ulog->undo_data_seg.tail);
		extend_undo_segment(
			&ulog->undo_data_seg, ulog->logno,
			newInsert + UNDOLOG_DAT_FILE_MAXSIZE - newInsert % UNDOLOG_DAT_FILE_MAXSIZE,
			UNDO_DATA_DB_OID);
		if (ulog->persistence == UNDO_PERMANENT)
		{
			START_CRIT_SECTION();
			ulog->undo_data_seg.dirty = true;

			undoExtend.prevtail = prevTail;
			undoExtend.tail = MAKE_UNDO_REC_PTR(ulog->logno, ulog->undo_data_seg.tail);
			lsn = xlog_undo_write(&undoExtend, XLOG_UNDO_EXTEND);
			ulog->undo_data_seg.lsn = (lsn);
			END_CRIT_SECTION();
		}
		unlock_undo_segment(&ulog->undo_data_seg);
	}
	return MAKE_UNDO_REC_PTR(ulog->logno, oldInsert);
}

UndoSlotPtr
undolog_alloc_slot_space(UndoLogControl *ulog)
{
	UndoSlotPtr	   slotPtr;
	UndoRecPtr	   prevTail;
	xl_undolog_extend undoExtend;
	XLogRecPtr	   lsn;
	UndoSlotOffset oldInsert = ulog->alloc_slot_offset;
	UndoSlotOffset newInsert =
		UNDO_LOG_OFFSET_PLUS_USABLE_BYTES(oldInsert, sizeof(UndoSlot));
	Assert(newInsert % UNDOLOG_TXN_FILE_MAXSIZE != 0);

	if (unlikely(newInsert > ulog->undo_txn_seg.tail))
	{
		lock_undo_segment(&ulog->undo_txn_seg);
		prevTail = MAKE_UNDO_REC_PTR(ulog->logno, ulog->undo_txn_seg.tail);
		extend_undo_segment(
			&ulog->undo_txn_seg, ulog->logno,
			newInsert + UNDOLOG_TXN_FILE_MAXSIZE - newInsert % UNDOLOG_TXN_FILE_MAXSIZE,
			UNDO_TXN_DB_OID);
		if (ulog->persistence == UNDO_PERMANENT)
		{
			START_CRIT_SECTION();
			ulog->undo_txn_seg.dirty = true;

			undoExtend.prevtail = prevTail;
			undoExtend.tail = MAKE_UNDO_REC_PTR(ulog->logno, ulog->undo_txn_seg.tail);
			lsn = xlog_undo_write(&undoExtend, XLOG_UNDO_SLOT_EXTEND);
			ulog->undo_txn_seg.lsn = lsn;
			END_CRIT_SECTION();
		}
		unlock_undo_segment(&ulog->undo_txn_seg);
	}
	slotPtr = MAKE_UNDO_REC_PTR(ulog->logno, oldInsert);
	prepare_undo_slot_buffer(&ulog->txn_slot_buffer, NULL, NULL, slotPtr);
	return slotPtr;
}

/* Release undo space from starturp to endurp and advance discard. */
void
undolog_release_space(UndoLogControl *ulog, UndoRecPtr starturp, UndoRecPtr endurp,
					 int *forceRecycleSize)
{
	UndoLogOffset end = UNDO_PTR_GET_OFFSET(endurp);
	int			  startSegno = (int) (ulog->undo_data_seg.head / UNDOLOG_DAT_FILE_MAXSIZE);
	int			  endSegno = (int) (end / UNDOLOG_DAT_FILE_MAXSIZE);

	if (unlikely(startSegno < endSegno))
	{
		UndoRecPtr prevHead;
		if (unlikely(*forceRecycleSize > 0))
		{
			*forceRecycleSize -= (int) (endSegno - startSegno) * UNDOLOG_DAT_FILE_BLOCKS;
		}
		forget_undo_log_buffers(ulog, startSegno * UNDOLOG_DAT_FILE_MAXSIZE,
								 endSegno * UNDOLOG_DAT_FILE_MAXSIZE, UNDO_DATA_DB_OID);
		lock_undo_segment(&ulog->undo_data_seg);
		prevHead = MAKE_UNDO_REC_PTR(ulog->logno, ulog->undo_data_seg.head);
		unlink_undo_segment(&ulog->undo_data_seg, ulog->logno, endSegno * UNDOLOG_DAT_FILE_MAXSIZE,
					  UNDO_DATA_DB_OID);
		Assert(ulog->undo_data_seg.head <= ulog->insert_offset);
		if (ulog->persistence == UNDO_PERMANENT)
		{
			XLogRecPtr	   lsn;
			xl_undolog_unlink undoUnlink;

			START_CRIT_SECTION();
			ulog->undo_data_seg.dirty = true;

			undoUnlink.head = MAKE_UNDO_REC_PTR(ulog->logno, ulog->undo_data_seg.head);
			undoUnlink.prevhead = prevHead;
			lsn = xlog_undo_write(&undoUnlink, XLOG_UNDO_UNLINK);
			ulog->undo_data_seg.lsn = lsn;
			END_CRIT_SECTION();
		}
		unlock_undo_segment(&ulog->undo_data_seg);
	}
	return;
}

/* Release slot space from starturp to endurp and advance discard. */
void
undolog_release_slot_space(UndoLogControl *ulog, UndoRecPtr startSlotPtr, UndoRecPtr endSlotPtr,
						 int *forceRecycleSize)
{
	UndoLogOffset end = UNDO_PTR_GET_OFFSET(endSlotPtr);
	int			  startSegno = (int) (ulog->undo_txn_seg.head / UNDOLOG_TXN_FILE_MAXSIZE);
	int			  endSegno = (int) (end / UNDOLOG_TXN_FILE_MAXSIZE);
	UndoRecPtr	  prevHead;
	if (unlikely(startSegno < endSegno))
	{
		if (unlikely(*forceRecycleSize > 0))
		{
			*forceRecycleSize -= (int) (endSegno - startSegno) * UNDOLOG_TXN_FILE_BLOCKS;
		}
		forget_undo_log_buffers(ulog, startSegno * UNDOLOG_TXN_FILE_MAXSIZE,
								 endSegno * UNDOLOG_TXN_FILE_MAXSIZE, UNDO_TXN_DB_OID);
		lock_undo_segment(&ulog->undo_txn_seg);
		prevHead = MAKE_UNDO_REC_PTR(ulog->logno, ulog->undo_txn_seg.head);
		unlink_undo_segment(&ulog->undo_txn_seg, ulog->logno, endSegno * UNDOLOG_TXN_FILE_MAXSIZE,
					  UNDO_TXN_DB_OID);
		Assert(ulog->undo_txn_seg.head <= ulog->alloc_slot_offset);
		if (ulog->persistence == UNDO_PERMANENT)
		{
			XLogRecPtr	   lsn;
			xl_undolog_unlink undoUnlink;

			START_CRIT_SECTION();
			ulog->undo_txn_seg.dirty = true;

			undoUnlink.head = MAKE_UNDO_REC_PTR(ulog->logno, ulog->undo_txn_seg.head);
			undoUnlink.prevhead = prevHead;
			lsn = xlog_undo_write(&undoUnlink, XLOG_UNDO_SLOT_UNLINK);
			ulog->undo_txn_seg.lsn = lsn;
			END_CRIT_SECTION();
		}
		unlock_undo_segment(&ulog->undo_txn_seg);
	}
	return;
}

void
undolog_prepare_switch(UndoLogControl *ulog)
{
	if (ulog->undo_data_seg.tail != UNDO_LOG_MAX_SIZE)
	{
		ereport(PANIC, (errmsg(UNDOFORMAT("Undo space switch fail ,logno(%d) expect "
										  "tail(%lu), real tail(%lu)."),
							   ulog->logno, UNDO_LOG_MAX_SIZE, ulog->undo_data_seg.tail)));
	}
	if (ulog->persistence == UNDO_PERMANENT)
	{
		lock_undo_log(ulog);
		ulog->dirty = true;
		ulog->insert_offset = UNDO_LOG_MAX_SIZE;  // space not use anymore
		unlock_undo_log(ulog);
	}
	return;
}


static void
reset_undo_meta(UndoLogMeta *uspMetaPointer)
{
	uspMetaPointer->version = XSTORE_UNDO_VERSION;
	uspMetaPointer->lsn = 0;
	uspMetaPointer->insert_rec_ptr = UNDO_LOG_BLOCK_HEADER_SIZE;
	uspMetaPointer->discard_rec_ptr = UNDO_LOG_BLOCK_HEADER_SIZE;
	uspMetaPointer->forece_discard_rec_ptr = UNDO_LOG_BLOCK_HEADER_SIZE;
	uspMetaPointer->recycle_xid = InvalidFullTransactionId;
	uspMetaPointer->allocate_slot_offset = UNDO_LOG_BLOCK_HEADER_SIZE;
	uspMetaPointer->recycle_slot_offset = UNDO_LOG_BLOCK_HEADER_SIZE;
}


static void
get_undo_meta_from_undolog(UndoLogControl *ulog, UndoLogMeta *uspMetaPointer)
{
	uspMetaPointer->version = XSTORE_UNDO_VERSION;
	uspMetaPointer->lsn = ulog->lsn;
	uspMetaPointer->insert_rec_ptr = UNDO_PTR_GET_OFFSET(ulog->insert_offset);
	uspMetaPointer->discard_rec_ptr = UNDO_PTR_GET_OFFSET(ulog->discard_offset);
	uspMetaPointer->forece_discard_rec_ptr =
		UNDO_PTR_GET_OFFSET(ulog->force_discard_offset);
	uspMetaPointer->recycle_xid = ulog->recycle_xid;
	uspMetaPointer->allocate_slot_offset = UNDO_PTR_GET_OFFSET(ulog->alloc_slot_offset);
	uspMetaPointer->recycle_slot_offset = UNDO_PTR_GET_OFFSET(ulog->recycle_slot_offset);
}

/*
 * Persist undospace metadata to disk. The fomart as follows
 * ----------|--------|---------|---------|--------------|
 *  undoMeta |undoMeta|undoMeta |undoMeta | pageCRC(32bit)
 * |->--------------------512---------------------------<-|
 */
void
checkpoint_undo_logs(int fd)
{
	bool	   retry = false;
	bool	   needFlushMetaPage = false;
	uint32	   ret = 0;
	uint32	   uspOffset = 0;
	uint64	   writeSize = 0;
	uint64	   currWritePos = 0;
	pg_crc32   metaPageCrc = 0;
	XLogRecPtr flushLsn = InvalidXLogRecPtr;
	uint32	   cycle = 0;
	char	   uspMetaPagebuffer[UNDO_WRITE_SIZE] = {'\0'};
	UndoLogControl  *ulog = NULL;
	uint32	   loop = 0;
	char	  *pageOffset;

	Assert(fd > 0);
	for (loop = 0; loop < PERSIST_UNDOLOG_COUNT; loop++)
	{
		UndoLogMeta *uspMetaPointer = NULL;
		ulog = (UndoLogControl *) undo_sys_ctx->ulogs[loop];
		if (loop % UNDOLOG_COUNT_PER_WRITE == 0)
		{
			cycle = loop;
			if ((uint32) (PERSIST_UNDOLOG_COUNT - loop) < UNDOLOG_COUNT_PER_WRITE)
			{
				writeSize = ((PERSIST_UNDOLOG_COUNT - loop) / UNDOLOG_COUNT_PER_PAGE + 1) *
							UNDO_META_PAGE_SIZE;
			}
			else
			{
				writeSize = UNDO_WRITE_SIZE;
			}
			needFlushMetaPage = false;
			memset(uspMetaPagebuffer,  0, UNDO_WRITE_SIZE);
		}
		if (ulog != NULL && ulog->dirty)
		{
			elog(
				DEBUG1,
				UNDOFORMAT(
					"undo metadata find dirty ulog %u. ,"
					" insertPtr %lu, discardPtr %lu, forceDiscardPtr %lu,"
					" recycleXid %lu, allocateTSlotPtr %lu, recycleTSlotPtr %lu lsn %lu "),
				ulog->logno, UNDO_PTR_GET_OFFSET(ulog->insert_offset),
				UNDO_PTR_GET_OFFSET(ulog->discard_offset),
				UNDO_PTR_GET_OFFSET(ulog->force_discard_offset), ulog->recycle_xid.value,
				UNDO_PTR_GET_OFFSET(ulog->alloc_slot_offset),
				UNDO_PTR_GET_OFFSET(ulog->recycle_slot_offset), ulog->lsn);
			needFlushMetaPage = true;
		}

		if (needFlushMetaPage && ((loop + 1) % UNDOLOG_COUNT_PER_WRITE == 0 ||
								  (loop + 1) == PERSIST_UNDOLOG_COUNT))
		{
			while (cycle <= loop)
			{
				ulog = (UndoLogControl *) undo_sys_ctx->ulogs[cycle];
				uspOffset = cycle % UNDOLOG_COUNT_PER_PAGE;
				pageOffset = uspMetaPagebuffer + ((cycle % UNDOLOG_COUNT_PER_WRITE) /
												  UNDOLOG_COUNT_PER_PAGE) *
													 UNDO_META_PAGE_SIZE;
				uspMetaPointer =
					(UndoLogMeta *) (pageOffset + uspOffset * sizeof(UndoLogMeta));
				Assert(uspMetaPointer != NULL);

				if (ulog != NULL)
				{
					lock_undo_log(ulog);
					/* Set the initial value of flushLsn when the first undospace of each meta page is traversed. */
					if (cycle % UNDOLOG_COUNT_PER_WRITE == 0)
					{
						flushLsn = ulog->lsn;
					}
					else
					{
						/* Pick out max lsn of total undospaces on one meta page. */
						if (ulog->lsn > flushLsn)
						{
							flushLsn = ulog->lsn;
						}
					}
					/* Locate the undospace on the undo page, then refresh. */
					get_undo_meta_from_undolog(ulog, uspMetaPointer);
					
					ulog->dirty = false;
					if (UNDO_PTR_GET_OFFSET(UndoGetInsertURecPtr(ulog)) >=
							UNDO_LOG_MAX_SIZE &&
						UndoGetAllocateTSlotPtr(ulog) == UndoGetRecycleTSlotPtr(ulog) &&
						!(ulog->frozen))
					{
						get_undo_meta_from_undolog(ulog, uspMetaPointer);
						ulog->frozen = true;
						unlock_undo_log(ulog);
						pg_atomic_fetch_sub_u32(&undo_sys_ctx->undo_log_used_cout, 1);
						ereport(LOG, (errmsg(UNDOFORMAT("frozen ulog %d."), cycle)));
					}
					else
					{
						unlock_undo_log(ulog);
					}
				}
				else
				{
					reset_undo_meta(uspMetaPointer);
				}

				/* all undospaces on the meta page are written. */
				if ((cycle + 1) % UNDOLOG_COUNT_PER_PAGE == 0 ||
					cycle == PERSIST_UNDOLOG_COUNT - 1)
				{
					/* Flush wal buffer of undo xlog first. */
					uint64 crcSize;
					if (cycle == PERSIST_UNDOLOG_COUNT - 1)
					{
						crcSize = (PERSIST_UNDOLOG_COUNT % UNDOLOG_COUNT_PER_PAGE) *
								  sizeof(UndoLogMeta);
					}
					else
					{
						crcSize = UNDOLOG_COUNT_PER_PAGE * sizeof(UndoLogMeta);
					}

					INIT_CRC32C(metaPageCrc);
					COMP_CRC32C(metaPageCrc, (void *) pageOffset, crcSize);
					FIN_CRC32C(metaPageCrc);
					/* Store CRC behind the last undospace meta on the page. */
					*(pg_crc32 *) (pageOffset + crcSize) = metaPageCrc;
				}
				if ((cycle + 1) % UNDOLOG_COUNT_PER_WRITE == 0 ||
					cycle == PERSIST_UNDOLOG_COUNT - 1)
				{
					XLogFlush(flushLsn);
					needFlushMetaPage = false;
					currWritePos =
						lseek(fd, (cycle / UNDOLOG_COUNT_PER_WRITE) * UNDO_WRITE_SIZE,
							  SEEK_SET);
				RE_WRITE:
					lseek(fd, currWritePos, SEEK_SET);
					ret = write(fd, uspMetaPagebuffer, writeSize);
					if (ret != writeSize && !retry)
					{
						retry = true;
						goto RE_WRITE;
					}
					else if (ret != writeSize && retry)
					{
						ereport(ERROR, (errmsg(UNDOFORMAT("Write undo meta failed expect "
														  "size(%lu) real size(%u)."),
											   writeSize, ret)));
						return;
					}
					elog(DEBUG1, UNDOFORMAT("undo metadata write loop %u."), cycle);
				}
				cycle++;
			}
		}
	}
}


bool
undolog_isused(int logno, UndoPersistence upersistence)
{
	UndoLogControl *ulog;
	if (!IS_VALID_LOGNO(logno))
	{
		ereport(PANIC, (errmsg(UNDOFORMAT("logno %d invalid."), logno)));
	}
	ulog = (UndoLogControl *) undo_sys_ctx->ulogs[logno];
	if (undolog_is_attached(ulog))
	{
		return true;
	}

	return false;
}

static void
recovery_undo_log(UndoLogControl *ulog, const UndoLogMeta *uspMetaInfo, const int logno)
{
	ulog->logno = logno;
	ulog->persistence = (UNDO_PERMANENT);
	ulog->lsn = (uspMetaInfo->lsn);
	ulog->insert_offset = UNDO_PTR_GET_OFFSET(uspMetaInfo->insert_rec_ptr);
	ulog->discard_offset = UNDO_PTR_GET_OFFSET(uspMetaInfo->discard_rec_ptr);
	ulog->force_discard_offset = UNDO_PTR_GET_OFFSET(uspMetaInfo->forece_discard_rec_ptr);
	ulog->alloc_slot_offset = UNDO_PTR_GET_OFFSET(uspMetaInfo->allocate_slot_offset);
	ulog->recycle_slot_offset = UNDO_PTR_GET_OFFSET(uspMetaInfo->recycle_slot_offset);
	ulog->recycle_xid = (uspMetaInfo->recycle_xid);
}

/* Initialize parameters in the undo ulog. */
void
init_undo_log(UndoLogControl *ulog, const int logno, UndoPersistence upersistence)
{
	ulog->dirty = false;
	LWLockInitialize(&ulog->lock, undo_sys_ctx->undo_lock_tranche_id);
	pg_atomic_write_u32(&ulog->attached, UNDO_LOG_DETACHED);
	ulog->persistence = (0);
	ulog->lsn = (0);
	ulog->insert_offset = (UNDO_LOG_BLOCK_HEADER_SIZE);
	ulog->discard_offset = (UNDO_LOG_BLOCK_HEADER_SIZE);
	ulog->force_discard_offset = (UNDO_LOG_BLOCK_HEADER_SIZE);
	ulog->alloc_slot_offset = (UNDO_LOG_BLOCK_HEADER_SIZE);
	ulog->recycle_slot_offset = (UNDO_LOG_BLOCK_HEADER_SIZE);
	ulog->frozen_slot_ptr = (INVALID_UNDO_SLOT_PTR);
	ulog->recycle_xid = (InvalidFullTransactionId);
	ulog->frozen_xid = (InvalidFullTransactionId);
	init_undo_slot_buffer(&ulog->txn_slot_buffer);
	ulog->attach_pid = (0);

	ulog->undo_data_seg.dirty = false;
	LWLockInitialize(&ulog->undo_data_seg.lock, undo_sys_ctx->undo_lock_tranche_id);
	ulog->undo_data_seg.lsn = 0;
	ulog->undo_data_seg.head = 0;
	ulog->undo_data_seg.tail = 0;

	ulog->undo_txn_seg.dirty = false;
	LWLockInitialize(&ulog->undo_txn_seg.lock, undo_sys_ctx->undo_lock_tranche_id);
	ulog->undo_txn_seg.lsn = 0;
	ulog->undo_txn_seg.head = 0;
	ulog->undo_txn_seg.tail = 0;

	ulog->logno = logno;
	pg_atomic_write_u32(&ulog->attached, UNDO_LOG_DETACHED);
	ulog->persistence = (upersistence);
}

/* Initialize parameters in the undo space. */
void
init_undo_segemnt(UndoLogControl *ulog, UndoSegmentType type)
{
	UndoSegment *usp = UndoGetUndoSegment(ulog, type);
	usp->dirty = false;
	usp->lsn = 0;
	usp->head = 0;
	usp->tail = 0;
}

void
recovery_undo_logs(int fd)
{
	uint32	 logno = 0;
	uint32	 logMetaSize = 0;
	uint32	 totalPageCnt = 0;
	pg_crc32 pageCrcVal = 0; /* CRC store in undo meta page */
	pg_crc32 comCrcVal = 0;	 /* calculating CRC current */
	char	*uspMetaBuffer = NULL;
	uint32	 idx;
	int		 i;

	// use currentMemoryContext
	char *persistBlock = (char *) palloc0(UNDO_META_PAGE_SIZE * PAGES_READ_NUM);
	Assert(fd > 0);

	UNDO_LOG_META_PAGE_COUNT(PERSIST_UNDOLOG_COUNT, UNDOLOG_COUNT_PER_PAGE, totalPageCnt);
	logMetaSize = totalPageCnt * UNDO_META_PAGE_SIZE / BLCKSZ;
	undo_sys_ctx->undo_meta_size += logMetaSize;

	/* Ensure read at start posistion of file. */
	lseek(fd, 0, SEEK_SET);

	if (undo_sys_ctx != NULL)
	{
		// do clean up
		for (idx = PERSIST_UNDOLOG_COUNT; idx < UNDOLOG_TOTAL_COUNT; idx++)
		{
			if (undo_sys_ctx->ulogs[idx] != NULL)
			{
				UndoLogControl	   *ulog = (UndoLogControl *) undo_sys_ctx->ulogs[idx];
				UndoPersistence upersistence = UNDO_PERMANENT;
				forget_undo_log_buffers(ulog, ulog->undo_data_seg.head,
										 ulog->undo_data_seg.tail, UNDO_DATA_DB_OID);
				forget_undo_log_buffers(ulog, ulog->undo_txn_seg.head,
										 ulog->undo_txn_seg.tail, UNDO_TXN_DB_OID);
				GET_UPERSISTENCE_BY_LOGNO(upersistence,idx);
				init_undo_log(ulog, idx, upersistence);
				init_undo_segemnt(ulog, UNDO_LOG_SEGMENT);
				init_undo_segemnt(ulog, UNDO_SLOT_SEGMENT);
			}
		}
	}

	for (i = 0; i < UNDO_PERSISTENCE_LEVELS; i++)
	{
		UndoPersistence persistence = (UndoPersistence) i;
		if (persistence != UNDO_PERMANENT)
		{
			clean_undo_files(persistence);
		}
	}

	for (logno = 0; logno < PERSIST_UNDOLOG_COUNT; logno++)
	{
		UndoLogMeta *uspMetaInfo = NULL;
		uint32		  ret = 0;
		int			  offset = 0;
		uint32		  count = 0;
		if (logno % (UNDOLOG_COUNT_PER_PAGE * PAGES_READ_NUM) == 0)
		{
			Size readSize;
			if ((uint32) (PERSIST_UNDOLOG_COUNT - logno) <
				UNDOLOG_COUNT_PER_PAGE * PAGES_READ_NUM)
			{
				readSize =
					((uint32) (PERSIST_UNDOLOG_COUNT - logno) / UNDOLOG_COUNT_PER_PAGE +
					 1) *
					UNDO_META_PAGE_SIZE;
			}
			else
			{
				readSize = UNDO_META_PAGE_SIZE * PAGES_READ_NUM;
			}
			memset(persistBlock, 0,
						  UNDO_META_PAGE_SIZE * PAGES_READ_NUM);
			ret = read(fd, persistBlock, readSize);
			if (ret != readSize)
			{
				ereport(
					ERROR,
					(errmsg(UNDOFORMAT(
								"Read undo meta page, expect size(%lu), real size(%u)."),
							readSize, ret)));
				return;
			}
		}
		if (logno % UNDOLOG_COUNT_PER_PAGE == 0)
		{
			uspMetaBuffer =
				persistBlock + ((logno % (UNDOLOG_COUNT_PER_PAGE * PAGES_READ_NUM)) /
								UNDOLOG_COUNT_PER_PAGE) *
								   UNDO_META_PAGE_SIZE;
			count = UNDOLOG_COUNT_PER_PAGE;
			if ((uint32) (PERSIST_UNDOLOG_COUNT - logno) < UNDOLOG_COUNT_PER_PAGE)
			{
				count = PERSIST_UNDOLOG_COUNT - logno;
			}
			/* Get page CRC from uspMetaBuffer. */
			pageCrcVal = *(pg_crc32 *) (uspMetaBuffer + sizeof(UndoLogMeta) * count);
			/* 
             * Calculate the CRC value based on all undospace meta information stored on the page. 
             * Then compare with pageCrcVal.
             */
			INIT_CRC32C(comCrcVal);
			COMP_CRC32C(comCrcVal, (void *) uspMetaBuffer, sizeof(UndoLogMeta) * count);
			FIN_CRC32C(comCrcVal);
			if (!EQ_CRC32C(pageCrcVal, comCrcVal))
			{
				ereport(ERROR,
						(errmsg(UNDOFORMAT("Undo meta CRC calculated(%u) is different "
										   "from CRC recorded(%u) in page."),
								comCrcVal, pageCrcVal)));
				return;
			}
		}
		offset = logno % UNDOLOG_COUNT_PER_PAGE;
		uspMetaInfo = (UndoLogMeta *) (uspMetaBuffer + offset * sizeof(UndoLogMeta));
		Assert(uspMetaInfo != NULL);
		if (uspMetaInfo->insert_rec_ptr != UNDO_LOG_BLOCK_HEADER_SIZE)
		{
			UndoLogControl *ulog = get_undo_log(logno);
			recovery_undo_log(ulog, uspMetaInfo, logno);
			elog(
				DEBUG1,
				UNDOFORMAT(
					"undo metadata recover ulog %u. ,"
					" insertPtr %lu, discardPtr %lu, forceDiscardPtr %lu,"
					" recycleXid %lu, allocateTSlotPtr %lu, recycleTSlotPtr %lu lsn %lu "),
				ulog->logno, UNDO_PTR_GET_OFFSET(ulog->insert_offset),
				UNDO_PTR_GET_OFFSET(ulog->discard_offset),
				UNDO_PTR_GET_OFFSET(ulog->force_discard_offset), ulog->recycle_xid.value,
				UNDO_PTR_GET_OFFSET(ulog->alloc_slot_offset),
				UNDO_PTR_GET_OFFSET(ulog->recycle_slot_offset), ulog->lsn);
		}
	}
	pfree(persistBlock);
}

static int
allocate_undo_logno(UndoPersistence upersistence,int oldLogno, uint64 size,bool isSwitch)
{
	int			  retLogno = INVALID_UNDOLOG_NO;
	UndoLogControl	 *ulog;
	UndoLogOffset newInsert;
	int			  i;
	GET_START_LOGNO_BY_UPERSISTENCE(upersistence,i);
	for (; i < UNDOLOG_TOTAL_COUNT; i++)
	{
		ulog = (UndoLogControl *) (undo_sys_ctx->ulogs[i]);

		if (undolog_is_attached(ulog) || ulog->persistence != upersistence)
		{
			continue;
		}

		if(isSwitch) {
			if(i<=oldLogno) {
				// new ulog id must bigger than lodLogno ,to fit endUndoPtr > startUndoPtr.
				continue;
			}
			if(ulog->insert_offset !=UNDO_LOG_BLOCK_HEADER_SIZE) {
				continue;
			} else {
				retLogno = i;
				break;
			}
		}

		newInsert = UNDO_LOG_OFFSET_PLUS_USABLE_BYTES(ulog->insert_offset, size);
		if (unlikely(newInsert > UNDO_LOG_MAX_SIZE))
		{
			continue;
		}

		retLogno = i;
		break;
	}

	return retLogno;
}

void
undolog_release(int logno, UndoPersistence upersistence)
{
	UndoLogControl *ulog;
	Assert(IS_VALID_LOGNO(logno));

	if (undo_sys_ctx == NULL)
	{
		return;
	}
	ulog = (UndoLogControl *) undo_sys_ctx->ulogs[logno];

	Assert(ulog != NULL);
	// logno in UNDO_UNLOGGED or UNDO_TEMP maybe reset in 
	if (!undolog_isused(logno, upersistence))
	{
		if(upersistence == UNDO_PERMANENT)
		{
			ereport(PANIC,
				(errmsg(UNDOFORMAT("used ulog %d already detached "), logno)));
		}
	}
	if (ulog != NULL && undolog_is_attached(ulog))
	{
		undolog_detach(ulog);
		init_undo_slot_buffer(&ulog->txn_slot_buffer);
		ulog->attach_pid = 0;
	}
}

UndoLogControl *
undolog_switch(int logno, UndoPersistence upersistence, uint64 size)
{
	UndoLogControl *new_ulog;
	UndoLogControl *ulog;
	int		  ret_logno = -1;
	if (pg_atomic_read_u32(&undo_sys_ctx->undo_log_used_cout) >
		undo_sys_ctx->undo_count_threshold)
	{
		ereport(
			ERROR,
			(errmsg(UNDOFORMAT(
						"Too many undo logs are requested, max count is %d, now is %d"),
					undo_sys_ctx->undo_count_threshold,
					pg_atomic_read_u32(&undo_sys_ctx->undo_log_used_cout))));
	}

	ulog = get_undo_log(logno);
	undolog_prepare_switch(ulog);					  // set old ulog
	LWLockAcquire(&undo_sys_ctx->undo_log_lock, LW_EXCLUSIVE);
	ret_logno = allocate_undo_logno(upersistence,logno, size,true);  // get new ulog
	if (!IS_VALID_LOGNO(ret_logno))
	{
		ereport(
			ERROR,
			(errmsg(
				"SwitchUndoLog: logno is invalid, there're too many working threads.")));
	}

	new_ulog = get_undo_log(ret_logno);
	if (new_ulog == NULL)
	{
		ereport(PANIC,
				(errmsg(UNDOFORMAT("can not palloc undo log memory, logno = %d."),
						ret_logno)));
	}
	undolog_attach(new_ulog);
	LWLockRelease(&undo_sys_ctx->undo_log_lock);
	init_undo_slot_buffer(&new_ulog->txn_slot_buffer);
	Assert(new_ulog->attach_pid == 0);
	new_ulog->attach_pid = MyProcPid;
	pg_write_barrier();
	undo_log_ctx->logs[upersistence] = ret_logno;

	// release old slot buff
	release_undo_slot_buffer(&ulog->txn_slot_buffer);

	ereport(LOG, (errmsg(UNDOFORMAT("old ulog %d switch to new ulog %d."), logno, ret_logno)));
	return new_ulog;
}

UndoLogControl *
get_undo_log(int logno)
{
	UndoLogControl *ulog;
	if (!IS_VALID_LOGNO(logno))
	{
		ereport(PANIC, (errmsg(UNDOFORMAT("logno %d invalid."), logno)));
	}

	ulog = (UndoLogControl *) undo_sys_ctx->ulogs[logno];
	Assert(ulog != NULL);
	return ulog;
}

void
alloc_undo_log()
{
	int		  i = 0;

	if (RecoveryInProgress() && !EnableHotStandby)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_TRANSACTION_INITIATION),
						errmsg("cannot assign undo ulog during recovery %d",standbyState)));
	}

	for (i = 0; i < UNDO_PERSISTENCE_LEVELS; i++)
	{
		UndoPersistence upersistence = (UndoPersistence) (i);
		int				logno = -1;
		UndoLogControl	   *ulog;
		if (IS_VALID_LOGNO(undo_log_ctx->logs[upersistence]))
		{
			continue;
		}

		LWLockAcquire(&undo_sys_ctx->undo_log_lock, LW_EXCLUSIVE);

		logno = allocate_undo_logno(upersistence,logno, BLCKSZ,false);
		if (!IS_VALID_LOGNO(logno))
		{
			ereport(WARNING,
					(errmsg(UNDOFORMAT("failed to allocate a undo ulog, bitmap num %d."),
							1)));
		}

		ulog = get_undo_log(logno);
		if (ulog == NULL)
		{
			ereport(PANIC, (errmsg(UNDOFORMAT("can not palloc undo ulog memory."))));
		}
		if (ulog->persistence != upersistence)
		{
			ereport(
				PANIC,
				(errmsg(
					UNDOFORMAT(
						"ulog %d Persistence %d, upersistance %d"),
					logno, ulog->persistence, upersistence)));
		}
		if (!undolog_is_detached(ulog))
		{
			ereport(
				WARNING,
				(errmsg(UNDOFORMAT("ulog %d attached pid %u, cur pid %u"),
						logno, ulog->attach_pid, MyProcPid)));
		}
		elog(DEBUG1,"backend %d using ulog %d for level %d",MyProcNumber,logno,i);
		undolog_attach(ulog);
		LWLockRelease(&undo_sys_ctx->undo_log_lock);
		init_undo_slot_buffer(&ulog->txn_slot_buffer);
		Assert(ulog->attach_pid == 0);
		ulog->attach_pid = MyProcPid;
		pg_write_barrier();
		undo_log_ctx->logs[upersistence] = logno;
	}
	return;
}

/*************** undo discard worker ********* */

/* max sleep time between cycles (100 milliseconds) */
#define MIN_NAPTIME_PER_CYCLE 100L
#define DELAYED_NAPTIME 10 * MIN_NAPTIME_PER_CYCLE
#define MAX_NAPTIME_PER_CYCLE 100 * MIN_NAPTIME_PER_CYCLE

static long wait_nptime = MIN_NAPTIME_PER_CYCLE;
static bool got_SIGTERM = false;

extern pg_atomic_uint64 *MyFrozenXmins;

const float FORCE_RECYCLE_PERCENT = 0.8;
const int	FORCE_RECYCLE_RETRY_TIMES = 5;
const float FORCE_RECYCLE_PUSH_PERCENT = 0.2;

static uint64	g_recycleLoops = 0;
static int		g_forceRecycleSize = 0;

void calculate_global_frozen_xmin(void);

void advance_frozen_xid(UndoLogControl *ulog, FullTransactionId *oldestFozenXid,
					  FullTransactionId oldestXmin);

void undo_discard_quick_die(SIGNAL_ARGS);
void undo_discard_shutdown_handler(SIGNAL_ARGS);
bool request_async_rollback(UndoLogControl *ulog, UndoSlotPtr recycle, UndoSlot *slot);

bool recycle_undo_space(UndoLogControl *ulog, FullTransactionId recycleXmin, FullTransactionId frozenXid,
					  FullTransactionId *oldestRecycleXid, FullTransactionId forceRecycleXid);

void
undo_discard_quick_die(SIGNAL_ARGS)
{
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);
	on_exit_reset();
	exit(1);
}

void
undo_discard_shutdown_handler(SIGNAL_ARGS)
{
	int save_errno = errno;
	got_SIGTERM= true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
	errno = save_errno;
}

bool
request_async_rollback(UndoLogControl *ulog, UndoSlotPtr recycle, UndoSlot *slot)
{
	if (can_rollback_undo_slot(slot))
	{
		UndoRecPtr prev;
		if (UndoGetPersitentLevel(ulog) == UNDO_TEMP ||
			UndoGetPersitentLevel(ulog) == UNDO_UNLOGGED)
		{
			return true;
		}

		prev = undo_rec_get_prevurp(slot->end_undo_ptr);
		register_rollback_req(slot->xid, prev, slot->start_undo_ptr, slot->dbOid,
						   recycle);
		elog(DEBUG1,
			 UNDOFORMAT("add async rollback requet:  slotxid=%lu, "
						"db_id=%u, ulog=%u, startUndoPtr=%lu, endUndoPtr=%lu"),
			 slot->xid.value, slot->dbOid, ulog->logno,
			 UNDO_PTR_GET_OFFSET(slot->start_undo_ptr),
			 UNDO_PTR_GET_OFFSET(slot->end_undo_ptr));

		return true;
	}
	return false;
}

static void
recheck_undo_recycle_xid(UndoLogControl *ulog, UndoSlot *slot, UndoSlotPtr slotPtr)
{
	FullTransactionId globalFronzenXid = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
	FullTransactionId globalRecycleXid = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid));
	FullTransactionId slotXid = slot->xid;
	FullTransactionId recycleXmin = FullTransactionIdFromU64(0);
	FullTransactionId oldestXmin = get_full_oldest_xmin();// GetOldestActiveTransactionId();
	int			  elogLevel = WARNING;
	recycleXmin = oldestXmin;

	if (TransactionIdOlderThanAllUndo(globalFronzenXid) ||
		(TransactionIdIsValid(slotXid.value) && TransactionIdOlderThanAllUndo(slotXid) &&
		 !xstore_transaction_id_did_commit(slotXid) && can_rollback_undo_slot(slot)))
	{
		elogLevel = PANIC;
	}
	ereport(
		elogLevel,
		(errmsg(
			UNDOFORMAT("recycle slot xid preceeds than globalFrozenXid: "
					   "ulog %d frozenXid %lu, frozenSlotPtr %lu, recycleXid %lu, "
					   "recycleSlotPtr %lu, "
					   "slot %lu xid %lu, needRollback %d, oldestXmin %lu, recycleXmin %lu, "
					   "globalFrozenXid %lu, globalRecycleXid %lu."),
			UndoGetLogNo(ulog), ulog->frozen_xid.value, UndoGetFrozenSlotPtr(ulog),
			ulog->recycle_xid.value, UndoGetRecycleTSlotPtr(ulog), slotPtr, slotXid.value,
			can_rollback_undo_slot(slot), oldestXmin.value, recycleXmin.value, globalFronzenXid.value,
			globalRecycleXid.value)));
	return;
}

void
advance_frozen_xid(UndoLogControl *ulog, FullTransactionId *oldestFozenXid, FullTransactionId oldestXmin)
{
	UndoSlot *slot = NULL;
	UndoSlotPtr		 frozenSlotPtr = UndoGetFrozenSlotPtr(ulog);
	UndoSlotPtr		 recycle = UndoGetRecycleTSlotPtr(ulog);
	UndoSlotPtr		 allocate = UndoGetAllocateTSlotPtr(ulog);
	UndoSlotPtr		 currentSlotPtr = frozenSlotPtr > recycle ? frozenSlotPtr : recycle;
	UndoSlotBuffer   undoSlotBuf;
	while (currentSlotPtr < allocate)
	{
		bool			finishAdvanceXid = false;
		UndoSlotBuffer *slotBuf = &undoSlotBuf;
		init_undo_slot_buffer(slotBuf);
		load_undo_slot_buffer(slotBuf, currentSlotPtr);

		while (slotBuf->blkno == UNDO_PTR_GET_BLOCK_NUM(currentSlotPtr) &&
			   (currentSlotPtr < allocate))
		{
			slot = get_undo_slot_from_buffer(slotBuf, currentSlotPtr);
			pg_read_barrier();
			if (!TransactionIdIsValid(slot->xid.value))
			{
				oldestFozenXid->value = pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid);
				recheck_undo_recycle_xid(ulog, slot, currentSlotPtr);
				finishAdvanceXid = true;
				break;
			}

			if (FullTransactionIdPrecedes(slot->xid,
									FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid))))
			{
				recheck_undo_recycle_xid(ulog, slot, currentSlotPtr);
			}
			
			if (slot->start_undo_ptr == INVALID_UNDO_REC_PTR)
			{
				ulog->frozen_xid = slot->xid;
				finishAdvanceXid = true;
				break;
			}

			elog(DEBUG4,
				 UNDOFORMAT(
					 "logno %d, currentSlotPtr %lu. slot xactid %lu, oldestXmin %lu"),
				 ulog->logno, currentSlotPtr, slot->xid.value, oldestXmin.value);
			frozenSlotPtr = currentSlotPtr;

			if(FullTransactionIdPrecedes(slot->xid, oldestXmin))
			{
				if (!xstore_transaction_id_did_commit(slot->xid))
				{
					if (request_async_rollback(ulog, currentSlotPtr, slot))
					{
						*oldestFozenXid = slot->xid;
						ulog->frozen_xid = (slot->xid);
						elog(DEBUG1,
							UNDOFORMAT("Transaction add to async rollback queue, logno: %d, "
										"oldestFozenXid: %lu,"
										"frozenSlotPtr %lu. oldestXmin %lu"),
							ulog->logno, oldestFozenXid->value, frozenSlotPtr, oldestXmin.value);
						finishAdvanceXid = true;
						break;
					}
				}
			}
			
			if (FullTransactionIdFollowsOrEquals(slot->xid, oldestXmin))
			{
				elog(DEBUG4,
					 UNDOFORMAT("logno %d, slotxid %lu, oldestXmin %lu, oldestFozenXid %lu, "
								"frozenSlotPtr %lu."),
					 ulog->logno, slot->xid.value, oldestXmin.value, oldestFozenXid->value,
					 frozenSlotPtr);
				finishAdvanceXid = true;
				ulog->frozen_xid = slot->xid;
				break;
			}
			currentSlotPtr = get_next_slotptr(currentSlotPtr);
			frozenSlotPtr = currentSlotPtr;
			if (slotBuf->blkno != UNDO_PTR_GET_BLOCK_NUM(currentSlotPtr))
			{
				release_undo_slot_buffer(slotBuf);
			}
		}
		release_undo_slot_buffer(slotBuf);
		if (finishAdvanceXid)
		{
			break;
		}
	}
	undolog_set_frozen_slotptr(ulog, frozenSlotPtr);
}

bool
recycle_undo_space(UndoLogControl *ulog, FullTransactionId recycleXmin, FullTransactionId frozenXid,
				 FullTransactionId *oldestRecycleXid, FullTransactionId forceRecycleXid)
{
	UndoSlotPtr		 recycle = UndoGetRecycleTSlotPtr(ulog);
	UndoSlotPtr		 allocate = UndoGetAllocateTSlotPtr(ulog);
	UndoSlot *slot = NULL;
	UndoRecPtr		 endUndoPtr = INVALID_UNDO_REC_PTR;
	UndoRecPtr		 oldestEndUndoPtr = INVALID_UNDO_REC_PTR;
	bool			 forceRecycle = false;
	bool			 needWal = false;
	FullTransactionId	 recycleXid = InvalidFullTransactionId;
	bool			 undoRecycled = false;
	bool			 result = false;
	UndoSlotPtr		 start = INVALID_UNDO_SLOT_PTR;
	FullTransactionId globalRecycleXid = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid));
	UndoSlotBuffer   undoSlotBuf;

	*oldestRecycleXid = ulog->recycle_xid;
	if(!TransactionIdIsValid(oldestRecycleXid->value))
	{
		*oldestRecycleXid = FullTransactionIdPrecedes(recycleXmin, frozenXid) ? recycleXmin : frozenXid;
	}
	if (ulog->persistence == UNDO_PERMANENT)
	{
		needWal = true;
	}
	Assert(recycle <= allocate);
	while (recycle < allocate)
	{
		UndoSlotBuffer *slotBuf = &undoSlotBuf;
		UndoRecPtr startUndoPtr = INVALID_UNDO_REC_PTR;
		start = recycle;
		init_undo_slot_buffer(slotBuf);
		load_undo_slot_buffer(slotBuf, recycle);
		undoRecycled = false;
		Assert(slotBuf->blkno == UNDO_PTR_GET_BLOCK_NUM(recycle));
		while (slotBuf->blkno == UNDO_PTR_GET_BLOCK_NUM(recycle) && (recycle < allocate))
		{
			slot = get_undo_slot_from_buffer(slotBuf, recycle);

			pg_read_barrier();
			if (!TransactionIdIsValid(slot->xid.value))
			{
				recheck_undo_recycle_xid(ulog, slot, recycle);
				break;
			}
			if (slot->start_undo_ptr == INVALID_UNDO_REC_PTR)
			{
				break;
			}
			if (FullTransactionIdPrecedes(slot->xid, globalRecycleXid))
			{
				recheck_undo_recycle_xid(ulog, slot, recycle);
			}

			if (FullTransactionIdPrecedes(slot->xid, recycleXmin))
			{
				Assert(forceRecycle == false);

				if (FullTransactionIdFollowsOrEquals(slot->xid, frozenXid))
				{
					break;
				}
			}
			else
			{
				bool forceRecycleXidCheck = false;
				bool isInProgress = false;
				bool slotTranactionStateCheck = false;
				forceRecycleXidCheck =
					(TransactionIdIsNormal(forceRecycleXid.value) &&
					 FullTransactionIdPrecedes(slot->xid, forceRecycleXid));
				if (!forceRecycleXidCheck)
				{
					break;
				}
				isInProgress = xstore_transaction_id_is_in_progress(slot->xid);
				if (isInProgress)
				{
					elog(DEBUG1,
						 UNDOFORMAT(
							 "try ForceRecycle fail at (slot=%lu, slotxid=%lu),because this txn still inprogress , "
							 "forceRecycleXid=%lu, recycleXmin=%lu"),
						 recycle, slot->xid.value, forceRecycleXid.value, recycleXmin.value);
					break;
				}
				slotTranactionStateCheck = (!xstore_transaction_id_did_commit(slot->xid) &&
											!isInProgress && can_rollback_undo_slot(slot));
				if (slotTranactionStateCheck)
				{
					elog(DEBUG1,
						 UNDOFORMAT(
							 "try ForceRecycle fail at (slot=%lu, slotxid=%lu ), because this txn needs to rollback  , "
							 "forceRecycleXid=%lu, recycleXmin=%lu"),
						 recycle, slot->xid.value, forceRecycleXid.value, recycleXmin.value);
					request_async_rollback(ulog, recycle, slot);
					break;
				}
				elog(LOG,
					 UNDOFORMAT(
						 "ForceRecycle succ at (slot=%lu, slotxid=%lu ), "
						 "forceRecycleXid=%lu, recycleXmin=%lu, startptr=%lu, endptr=%lu."),
					 recycle, slot->xid.value, forceRecycleXid.value, recycleXmin.value,
					 UNDO_PTR_GET_OFFSET(slot->start_undo_ptr),
					 UNDO_PTR_GET_OFFSET(slot->end_undo_ptr));
				forceRecycle = true;
			}

			elog(DEBUG2,
				 UNDOFORMAT("recycle logno %d, transaction slot %lu xid %lu start ptr %lu "
							"end ptr %lu."),
				 ulog->logno, recycle, slot->xid.value, slot->start_undo_ptr,
				 slot->end_undo_ptr);
			if (!startUndoPtr)
			{
				startUndoPtr = slot->start_undo_ptr;	 // first mark startUndoPtr
			}
			if (!forceRecycle)
			{
				oldestEndUndoPtr = slot->end_undo_ptr;
			}
			endUndoPtr = slot->end_undo_ptr;	 // mark endUndoPtr
			recycleXid = slot->xid;
			undoRecycled = true;
			recycle = get_next_slotptr(recycle);
			if (slotBuf->blkno != UNDO_PTR_GET_BLOCK_NUM(recycle))
			{
				release_undo_slot_buffer(slotBuf);
			}
		}

		release_undo_slot_buffer(slotBuf);
		if (undoRecycled)
		{
			Assert(TransactionIdIsValid(recycleXid.value) &&
				   (FullTransactionIdPrecedes(UndoGetRecycleXid(ulog), recycleXid)));
			lock_undo_log(ulog);
			if (!undolog_check_recycle(ulog, startUndoPtr, endUndoPtr))
			{
				ereport(
					PANIC,
					(errmsg(UNDOFORMAT("logno %d recycle start %lu >= recycle end %lu."),
							ulog->logno, startUndoPtr, endUndoPtr)));
			}
			if (IS_VALID_UNDO_REC_PTR(oldestEndUndoPtr))
			{
				int start_logno = UNDO_PTR_GET_LOG_NO(startUndoPtr);
				int end_logno = UNDO_PTR_GET_LOG_NO(oldestEndUndoPtr);
				if (start_logno != end_logno)
				{
					ereport(PANIC,
					     (errmsg(UNDOFORMAT(
                            "logno %d recycle start %lu >= recycle end %lu."),
                            ulog->logno, startUndoPtr, oldestEndUndoPtr)));
				}
				undolog_set_discard_urecptr(ulog, oldestEndUndoPtr);
			}

			START_CRIT_SECTION();
			ulog->recycle_xid = (recycleXid);
			*oldestRecycleXid = recycleXid;
			undolog_set_force_discard_urecptr(ulog, endUndoPtr);
			undolog_set_recycle_slotptr(ulog, recycle);
			Assert(UndoGetForceDiscardURecPtr(ulog) <= UndoGetInsertURecPtr(ulog));
			result = true;

			if (needWal)
			{
				xl_undolog_discard xlrec;
				XLogRecPtr		lsn;
				ulog->dirty = true;
				xlrec.endSlot = recycle;
				xlrec.startSlot = start;
				xlrec.recycleLoops = g_recycleLoops;
				xlrec.recycledXid = recycleXid;
				xlrec.globalFrozenXid = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
				xlrec.endUndoPtr = endUndoPtr;
				lsn = xlog_undo_write(&xlrec, XLOG_UNDO_DISCARD);
				ulog->lsn = lsn;
				elog(DEBUG2,
					 UNDOFORMAT(
						 "logno %d recycle seg start %lu end %lu from slot %lu "
						 "to slot %lu lsn %lu recyclexid %lu loops %lu oldestXmin %lu."),
					 ulog->logno, startUndoPtr, endUndoPtr, start, recycle, lsn,
					 recycleXid.value, g_recycleLoops, recycleXmin.value);
			}
			END_CRIT_SECTION();

			unlock_undo_log(ulog);
			undolog_release_space(
				ulog, startUndoPtr, endUndoPtr,
				&g_forceRecycleSize);  // release undo buffer and clean up undofile
			undolog_release_slot_space(
				ulog, start, recycle,
				&g_forceRecycleSize);  // release slot buffer and clean up undo slot file
		}
		else
		{
			/* ulog has nothing to recycle. */
			break;
		}
	}
	return result;
}


static void
update_recycle_xid(FullTransactionId *recycleMaxXIDs, uint32 *count, FullTransactionId recycleXid)
{
	if (recycleXid.value != InvalidTransactionId)
	{
		uint32 idx = *count;
		recycleMaxXIDs[idx] = recycleXid;
		*count = idx + 1;
	}
}

static void
update_havingundo_xid(FullTransactionId *recycleMaxXIDs, uint32 count,
					FullTransactionId *oldestXidHavingUndo)
{
	uint32 idx = 0;
	if (count > 0)
	{
		for (idx = 0; idx < count; idx++)
		{
			if (oldestXidHavingUndo->value == InvalidTransactionId)
			{
				*oldestXidHavingUndo = recycleMaxXIDs[idx];
			}
			else
			{
				/* get oldest xid in recycleMaxXIDs */
				if (FullTransactionIdFollows(*oldestXidHavingUndo, recycleMaxXIDs[idx]))
				{
					*oldestXidHavingUndo = recycleMaxXIDs[idx];
				}
			}
		}
	}
	else
	{
		*oldestXidHavingUndo = InvalidFullTransactionId;
	}
}

// if untoTotalSize > 0.8 undo_max_total_size ,need to forceRecycle.
static bool
check_need_force_recycle(void)
{
	int totalSize = (int) pg_atomic_read_u32(&undo_sys_ctx->undo_total_size);
	int limitSize = (int) (undo_max_total_size * FORCE_RECYCLE_PERCENT);
	int metaSize = (int) undo_sys_ctx->undo_meta_size;
	Assert(totalSize >= 0 && limitSize >= 0 && metaSize >= 0);
	g_forceRecycleSize = totalSize + metaSize - limitSize;
	if (g_forceRecycleSize >= 0)
	{
		elog(DEBUG1,
			 UNDOFORMAT("Need ForceRecycle: undoTotalSize=%d blocks, metaSize=%d blocks, limitSize=%d blocks, "
						"forceRecycleSize=%d blocks."),
			 totalSize, metaSize, limitSize, g_forceRecycleSize);
		return true;
	}
	return false;
}

// get new forceRecyleXid = (NextXid-oldestXmin)*retry*0.2 +oldestXmin
static FullTransactionId
get_force_recycle_xid(FullTransactionId oldestXmin, int retry)
{
	FullTransactionId forceRecycleXid = InvalidFullTransactionId;
	FullTransactionId cxid = ReadNextFullTransactionId();
	Assert(FullTransactionIdFollowsOrEquals(cxid, oldestXmin));
	if (oldestXmin.value == cxid.value || retry > FORCE_RECYCLE_RETRY_TIMES)
	{
		return InvalidFullTransactionId;
	}
	forceRecycleXid.value =
		(cxid.value - oldestXmin.value) * retry * FORCE_RECYCLE_PUSH_PERCENT + oldestXmin.value;
	return forceRecycleXid;
}


static void
shutdown_undo_recycle(FullTransactionId *recycleMaxXIDs)
{
	ereport(LOG, (errmsg(UNDOFORMAT("UndoRecycler: shutting down"))));
	pfree(recycleMaxXIDs);
	ResourceOwnerRelease(CurrentResourceOwner, RESOURCE_RELEASE_BEFORE_LOCKS, false,
						 true);
	proc_exit(1);
}

static void
wait_time(uint64 waitms)
{
	int rc = 0;
	rc = WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
				   waitms, PG_WAIT_EXTENSION);
	/* Clear any already-pending wakeups */
	ResetLatch(&MyProc->procLatch);
	if (((unsigned int) rc) & WL_POSTMASTER_DEATH)
	{
		ResourceOwnerRelease(CurrentResourceOwner, RESOURCE_RELEASE_BEFORE_LOCKS, false,
						 true);
		proc_exit(1);
	}
}

void
discard_worker_register(void)
{
	BackgroundWorker worker;

	/* Set up background worker parameters */
	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS ;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 5;
	worker.bgw_notify_pid = 0;
	worker.bgw_main_arg = Int32GetDatum(0);
	strcpy(worker.bgw_library_name, "xstore");
	strcpy(worker.bgw_function_name, "discard_worker_main");
	pg_snprintf(worker.bgw_name, sizeof(worker.bgw_name),
				"undo discard process");
	strcpy(worker.bgw_type, "undo discard process");
	RegisterBackgroundWorker(&worker);
}


void discard_worker_main(Datum main_arg)
{
	bool		   recycled = false;
	FullTransactionId  oldestXidHavingUndo = InvalidFullTransactionId;
	FullTransactionId  lastOldestXidHavingUndo = InvalidFullTransactionId;
	FullTransactionId  oldestFrozenXidInUndo = InvalidFullTransactionId;
	FullTransactionId lastForceAdvanceXid = InvalidFullTransactionId;
	FullTransactionId *recycleMaxXIDs = NULL;
	MemoryContext  undoRecycleContext;

	init_ps_display("undo discard process");

	/*
     * Properly accept or ignore signals the postmaster might send us.
     */
	pqsignal(SIGHUP, SIG_IGN);
	pqsignal(SIGINT, SIG_IGN);				/* request shutdown */
	pqsignal(SIGTERM, undo_discard_shutdown_handler); /* request shutdown */
	pqsignal(SIGQUIT, undo_discard_quick_die);        /* hard crash time */
	InitializeTimeouts(); /* establishes SIGALRM handler */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);
    BackgroundWorkerUnblockSignals();

	// should release
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "undo discard");
	undoRecycleContext =
		AllocSetContextCreate(TopMemoryContext, "undo discard", ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE * 4);
	(void) MemoryContextSwitchTo(undoRecycleContext);

	SetProcessingMode(NormalProcessing);

	recycleMaxXIDs = (FullTransactionId *) palloc0(sizeof(FullTransactionId) * UNDOLOG_TOTAL_COUNT);
	if (recycleMaxXIDs == NULL)
	{
		ereport(PANIC, (errmsg(UNDOFORMAT("undo cannot alloc xids memory."))));
	}

	if (!TransactionIdIsValid(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid)))
	{
		pg_atomic_write_u64(&undo_sys_ctx->global_frozen_xid,
							pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid));
	}

	while (!got_SIGTERM)
	{
		if (!RecoveryInProgress())
		{
			FullTransactionId oldestXmin = get_full_oldest_xmin(); 
			FullTransactionId recycleXmin = oldestXmin;
			uint32		  idx = 0;
			uint32		  retry = 1;
			FullTransactionId recycleXid;
			FullTransactionId forceRecycleXid = InvalidFullTransactionId;
			uint32		  recycleMaxXIDCount = 0;

			bool		  isAnyLogUsed = false;
			bool          endOneLoop = false;

			/* 
			 * Calculate the globalFrozenXmin, we use globalFrozenXmin as water mark to decide whether
			 * we can allocate new xid and truncate the clog.
			 */
			calculate_global_frozen_xmin();

			if (!TransactionIdIsValid(oldestXmin.value) ||
				FullTransactionIdPrecedes(oldestXmin,
									 FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid))))
			{
				wait_time(wait_nptime);
				continue;
			}
			recycled = false;
			oldestXidHavingUndo = InvalidFullTransactionId;
			oldestFrozenXidInUndo = oldestXmin;

			if (pg_atomic_read_u32(&undo_sys_ctx->undo_log_used_cout) != 0)
			{
				if (check_need_force_recycle())
				{
					forceRecycleXid = get_force_recycle_xid(recycleXmin, retry);
				}
			retry:
				memset(recycleMaxXIDs, 0,sizeof(FullTransactionId) * UNDOLOG_TOTAL_COUNT);
				recycleMaxXIDCount = 0;
				isAnyLogUsed = false;
				endOneLoop = false;
				for (idx = 0;
					 idx < UNDOLOG_TOTAL_COUNT && !got_SIGTERM;
					 idx++)
				{
					UndoLogControl	 *ulog = get_undo_log(idx);
					FullTransactionId frozenXid = oldestXmin;
					if (ulog == NULL)
					{
						continue;
					}
					recycleXid = InvalidFullTransactionId;
					if (undolog_need_recycle(ulog))
					{
						if (!TransactionIdIsValid(ulog->frozen_xid.value))
						{
							ulog->frozen_xid =
								FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
						}
						// advance frozenXid to  oldestXmin
						advance_frozen_xid(ulog, &frozenXid, oldestXmin);
						// recycle space and return recycleXid 	 
						if (recycle_undo_space(ulog, recycleXmin, frozenXid, &recycleXid,
											 forceRecycleXid))
						{
							recycled = true;
						}
						isAnyLogUsed = true;

						elog(DEBUG4,
							 UNDOFORMAT("oldestFrozenXidInUndo: "
										"oldestFrozenXidInUndo=%lu, frozenXid=%lu"
										" after RecycleUndoSpace recycled:%d loops:%lu"),
							 oldestFrozenXidInUndo.value, frozenXid.value, recycled, g_recycleLoops);
						oldestFrozenXidInUndo = FullTransactionIdPrecedes(frozenXid,oldestFrozenXidInUndo)
													? frozenXid
													: oldestFrozenXidInUndo;
						update_recycle_xid(recycleMaxXIDs, &recycleMaxXIDCount, recycleXid);
					}
				}
				if (idx == UNDOLOG_TOTAL_COUNT)
				{
					endOneLoop = true;
				}
			}
			smgrreleaseall();

			// update globalFrozenXid;
			if (isAnyLogUsed)
			{
				if(endOneLoop)
				{
					elog(DEBUG4,
						UNDOFORMAT(
							"oldestFrozenXidInUndo for update: oldestFrozenXidInUndo=%lu, loops %lu"),
						oldestFrozenXidInUndo.value, g_recycleLoops);
					pg_atomic_write_u64(&undo_sys_ctx->global_frozen_xid,
										U64FromFullTransactionId(oldestFrozenXidInUndo));
				}
			}
			else if (!got_SIGTERM)
			{
				if (!FullTransactionIdIsNormal(lastForceAdvanceXid) )
				{
					lastForceAdvanceXid = oldestXmin;
				}
				if (TransactionIdOlderThanAllUndo(lastForceAdvanceXid))
				{
					lastForceAdvanceXid = InvalidFullTransactionId;
				}
				else if (!TransactionIdIsInProgress(XidFromFullTransactionId(lastForceAdvanceXid)))
				{
					if(TransactionIdDidAbort(XidFromFullTransactionId(lastForceAdvanceXid)))
					{
						// aborted
						lastForceAdvanceXid = InvalidFullTransactionId;
					}
					else if (TransactionIdDidCommit(XidFromFullTransactionId(lastForceAdvanceXid)))
					{
						if (FullTransactionIdPrecedes(FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid)), lastForceAdvanceXid))
						{
							//  advance globalFrozenXid directly 1) all logs are done 2) xstore not used yet 
							pg_atomic_write_u64(&undo_sys_ctx->global_frozen_xid, lastForceAdvanceXid.value);
							elog(DEBUG1,
								UNDOFORMAT("no undo log used,just advance globalFrozenXid to xid %lu"),
								lastForceAdvanceXid.value);
						}
						lastForceAdvanceXid = InvalidFullTransactionId;
					}
				}
			}

			if (got_SIGTERM)
			{
				shutdown_undo_recycle(recycleMaxXIDs);
			}
			// update globalRecycleXid;
			if (pg_atomic_read_u32(&undo_sys_ctx->undo_log_used_cout) != 0)
			{
				FullTransactionId globalRecycleXid =
						FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid));

				update_havingundo_xid(recycleMaxXIDs, recycleMaxXIDCount,
									&oldestXidHavingUndo);
				if (g_forceRecycleSize > 0)
				{
					retry++;
					forceRecycleXid = get_force_recycle_xid(recycleXmin, retry);
					if (TransactionIdIsValid(forceRecycleXid.value))
					{
						elog(DEBUG1,
							 UNDOFORMAT("try next ForceRecycle to forceRecycleXid=%lu, oldestXmin=%lu, "
										"forceRecycleSize=%d blocks, retry=%u."),
							 forceRecycleXid.value, recycleXmin.value, g_forceRecycleSize, retry);
						goto retry;
					}
				}

				if (TransactionIdIsValid(oldestXidHavingUndo.value) && 
					oldestXidHavingUndo.value != lastOldestXidHavingUndo.value)
				{
					lastOldestXidHavingUndo = oldestXidHavingUndo;
					elog(DEBUG1, UNDOFORMAT("find new oldestXidHavingUndo = %lu. loops:%lu"),
						 oldestXidHavingUndo.value,g_recycleLoops);

					
					if (FullTransactionIdPrecedes(oldestXidHavingUndo, globalRecycleXid))
					{
						ereport(
							WARNING,
							(errmsg(
								UNDOFORMAT(
									"undorecycle loop having undo %lu < global globalRecycleXid %lu.loops:%lu"),
								oldestXidHavingUndo.value, globalRecycleXid.value, g_recycleLoops)));
					}
					if (FullTransactionIdPrecedes(recycleXmin, oldestXidHavingUndo))
					{
						oldestXidHavingUndo = recycleXmin;
					}

					if (FullTransactionIdFollows(oldestXidHavingUndo, globalRecycleXid))
					{
						ereport(
							LOG,
							(errmsg(
								UNDOFORMAT(
									"update globalRecycleXid: oldestXmin=%lu, recycleXmin=%lu, "
									"globalFrozenXid=%lu, globalRecycleXid=%lu, "
									"newRecycleXid=%lu. loops=%lu"),
								oldestXmin.value, recycleXmin.value,
								pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid),
								globalRecycleXid.value, oldestXidHavingUndo.value,g_recycleLoops)));

						pg_atomic_write_u64(&undo_sys_ctx->global_recycle_xid,
											U64FromFullTransactionId( oldestXidHavingUndo));
					}
				}
				
				g_recycleLoops++; 
			}
			if (!recycled)
			{
				/*
				* Increase the wait_time based on the length of inactivity. If
				* wait_time is within one second, then increment it by 100 ms at a
				* time. Henceforth, increment it one second at a time, till it
				* reaches ten seconds. Never increase the wait_time more than ten
				* seconds, it will be too much of waiting otherwise.
				*/
				wait_nptime += (wait_nptime < DELAYED_NAPTIME ?
						  MIN_NAPTIME_PER_CYCLE : DELAYED_NAPTIME);
				if (wait_nptime > MAX_NAPTIME_PER_CYCLE)
					wait_nptime = MAX_NAPTIME_PER_CYCLE;

				wait_time(wait_nptime);
			}
			else
			{
				wait_nptime = MIN_NAPTIME_PER_CYCLE;
			}
		}
		else
		{
			wait_time(DELAYED_NAPTIME);
		}
	}
	shutdown_undo_recycle(recycleMaxXIDs);
}

void
calculate_global_frozen_xmin(void)
{
	int i;
	FullTransactionId oldestFrozenXmin = FullTransactionIdFromU64(pg_atomic_read_u64(&(undo_sys_ctx->global_frozen_xid)));

	for (i = 0; i < MAX_FROZEN_XMIN_NUM; i++) 
	{
		FullTransactionId xmin = FullTransactionIdFromU64(pg_atomic_read_u64(&(MyFrozenXmins[i])));

		if (FullTransactionIdIsNormal(xmin) && FullTransactionIdPrecedes(xmin, oldestFrozenXmin))
			oldestFrozenXmin = xmin;		
	}

	pg_atomic_write_u64(&(undo_sys_ctx->global_frozen_xmin), oldestFrozenXmin.value);
}