/* -------------------------------------------------------------------------
 *
 * xlock.h
 * Implement the access interfaces of xstore inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * include/xheap/xlog.h
 * -------------------------------------------------------------------------
 */
 #include "postgres.h"

 #include "access/tableam.h"
 #include "xheap/xtuple.h"

typedef struct {
  FullTransactionId disk_tuple_modified_xid;
  FullTransactionId disk_tuple_locker_xid;
  UndoRecPtr 		disk_tuple_urec;
  ItemPointerData 	ctid;
  uint16 			disk_tuple_flag;
} XHeapWaitInfo;


extern void xheap_execute_lock_tuple(Relation relation, Buffer buffer, XHeapTuple xtuple,
						 LockTupleMode mode, RowPtr *rp);
extern bool is_xtuple_locked_by_us(XHeapTuple xtuple, FullTransactionId xid, LockTupleMode mode);
extern bool xheap_wait_helper(Relation relation, Buffer buffer, XHeapWaitInfo *wait_info, LockTupleMode mode,
		   LockWaitPolicy wait_policy, FullTransactionId modified_xid, FullTransactionId locker_xid,
		   FullTransactionId modified_subxid, bool *has_tup_lock, bool *multixid_self);
extern bool validate_tuples_xact(Relation relation, Buffer buffer, Snapshot snapshot, XHeapTuple tuple,
			  FullTransactionId prior_xmax, bool lock_buffer, bool keep_tup);