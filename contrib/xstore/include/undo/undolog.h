/* -------------------------------------------------------------------------
 *
 * undolog.h
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * include/undo/undolog.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef UNDOLOG_H
#define UNDOLOG_H

#include "postgres.h"
#include "catalog/pg_class.h"
#include "catalog/pg_tablespace.h"
#include "storage/bufpage.h"
#include "access/xact.h"
#include "storage/lwlock.h"
#include "undo/undotype.h"
#include "undo/undoxlog.h"

/**************undo segment ******************* */
typedef struct UndoSegment
{
	/* next insertion point (head), this backend is the only one that can modify insert. */
	UndoLogOffset head;
	/* one past end of highest segment, need lock befor modify end. */
	UndoLogOffset tail;
	LWLock lock;
	bool dirty;
	XLogRecPtr lsn;
} UndoSegment;

/* segment lock/unlock. */
void lock_undo_segment(UndoSegment *seg);
void unlock_undo_segment(UndoSegment *seg);

/* Extend the end of this undo log to cover newInsert */
void extend_undo_segment(UndoSegment *seg, int logno, UndoLogOffset offset, uint32 db_id);
/* Unlink unused undo segment file. */
extern void unlink_undo_segment(UndoSegment *seg, int logno, UndoLogOffset start_off, UndoLogOffset end_off, uint32 dbId);

/****************undo log ************ */

typedef struct UndoLogControl
{
	int	              logno;
	pg_atomic_uint32  attached;
	UndoSlotBuffer	  txn_slot_buffer;
	UndoSlotOffset	  alloc_slot_offset;
	UndoSlotOffset	  recycle_slot_offset;
	UndoSlotPtr		  frozen_slot_ptr;
	UndoLogOffset	  insert_offset;
	UndoLogOffset	  discard_offset;
	UndoLogOffset	  force_discard_offset;
	UndoPersistence	  persistence;
	FullTransactionId recycle_xid;
	FullTransactionId frozen_xid;
	int				  attach_pid;
	UndoSegment       undo_data_seg;
	UndoSegment       undo_txn_seg;

	LWLock lock;
	XLogRecPtr lsn;
	bool dirty;
	bool frozen;
} UndoLogControl;

#define UndoGetInsertURecPtr(ulog) (MAKE_UNDO_REC_PTR(ulog->logno, ulog->insert_offset))
#define UndoGetDiscardURecPtr(ulog) (MAKE_UNDO_REC_PTR(ulog->logno, ulog->discard_offset))
#define UndoGetForceDiscardURecPtr(ulog) (MAKE_UNDO_REC_PTR(ulog->logno, ulog->force_discard_offset))
#define UndoGetRecycleXid(ulog) (ulog->recycle_xid)

#define UndoGetLogNo(ulog) (ulog->logno)
#define UndoGetFrozenSlotPtr(ulog) (ulog->frozen_slot_ptr)
#define UndoGetPersitentLevel(ulog) (ulog->persistence)
#define UndoGetUndoSegment(ulog, segType) \
	(segType == UNDO_LOG_SEGMENT ? &ulog->undo_data_seg : &ulog->undo_txn_seg)

#define UndoGetAllocateTSlotPtr(ulog) (MAKE_UNDO_REC_PTR(ulog->logno, ulog->alloc_slot_offset))
#define UndoGetRecycleTSlotPtr(ulog) (MAKE_UNDO_REC_PTR(ulog->logno, ulog->recycle_slot_offset))


void init_undo_log(UndoLogControl *ulog, const int logno, UndoPersistence upersistence);
void lock_undo_log(UndoLogControl *ulog);
void unlock_undo_log(UndoLogControl *ulog);

/* forget all buffers for the the range in undo log. */
void forget_undo_log_buffers(UndoLogControl *ulog, UndoLogOffset start, UndoLogOffset end,
							 uint32 dbId);

/************* undo init and alloc ************ */

void init_undo_meta(void);
void checkpoint_undo_meta(XLogRecPtr checkPointRedo);
void recovery_undo_meta(void);

void alloc_undo_log(void);
void set_undo_threshold(void);
void cleanup_undo_log(int code, Datum arg);

/************ undo interfaces ************* */

UndoLogControl *get_undo_log(int logno);

bool check_need_switch_undolog(UndoPersistence upersistence, uint64 size, UndoRecPtr undo_ptr);

/* Check undo record valid.. */
UndoRecordState check_undo_record_valid(UndoRecPtr urp, bool check_force_recycle,
									 FullTransactionId *last_xid);

UndoRecPtr allocate_undo_space(FullTransactionId xid, UndoPersistence upersistence, uint64 size,
							 bool need_switch, xl_undo_meta *xlundometa);

UndoRecPtr get_next_undoptr(UndoRecPtr undoPtr, uint64 size);

void update_undolog_meta(FullTransactionId xid, UndoRecPtr start_undo_ptr,
					 xl_undo_meta *meta, UndoPersistence upersistence,
					 UndoRecPtr last_record, UndoRecPtr last_record_size);
void release_undolog_meta(UndoPersistence upersistence);


void set_undolog_meta_lsn(XLogRecPtr lsn);
void redo_undo_meta(XLogReaderState *record, xl_undo_meta *meta, UndoRecPtr start_undo_ptr,
				  UndoRecPtr last_record, uint32 last_record_size);

void release_undo_slot_buffers(void);

bool is_skip_insert_undo(UndoRecPtr urp, xl_undo_meta *xlundometa);
bool is_skip_insert_slot(UndoSlotPtr urp);

void set_undo_slot_rollback_finish(UndoSlotPtr slot_ptr);

/**************undo slot buffer******************* */

void init_undo_slot_buffer(UndoSlotBuffer *slot_buff);
void load_undo_slot_buffer(UndoSlotBuffer *slot_buff, UndoSlotPtr slot_ptr);
void xlog_load_undo_slot_buffer(UndoSlotBuffer *slot_buff, XLogReaderState *record, UndoSlotPtr slot_ptr);
UndoSlot *get_undo_slot_from_buffer(UndoSlotBuffer *slot_buff, UndoSlotPtr slot_ptr);
UndoSlotPtr get_next_slotptr(UndoSlotPtr slot_ptr);

/************ undo discard ************* */
PGDLLEXPORT void discard_worker_register(void);
PGDLLEXPORT void discard_worker_main(Datum);

#endif