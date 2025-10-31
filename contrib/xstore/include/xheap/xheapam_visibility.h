/* -------------------------------------------------------------------------
 *
 * xheapam_visibility.h
 * Tuple visibility interfaces of inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/xheap/xheapam_visibility.h
 * -------------------------------------------------------------------------
 *
 */

#ifndef XHEAPAM_VISIBILITY_H
#define XHEAPAM_VISIBILITY_H

#include "access/transam.h"
#include "access/tableam.h"
#include "xheap/xtuple.h"
#include "xstore.h"
#include "undo/undotype.h"
#include "port/atomics.h"
#include "utils/snapshot.h"

typedef int XidStatus;

typedef struct XHeapTupleTransInfo
{
	FullTransactionId xid;
	CommandId			cid;
	UndoRecPtr			urec_add;
} XHeapTupleTransInfo;

typedef enum
{
	XTUPLETID_NEW,		/* inserted */
	XTUPLETID_MODIFIED, /* in-place update or lock */
	XTUPLETID_GONE		/* non-in-place update or delete */
} XTupleTidOp;

typedef enum
{
	XVERSION_NONE,
	XVERSION_CURRENT,
	XVERSION_OLDER,
	XVERSION_CHECK_CID
} XVersionSelector;

/* Result codes for xheap_tuple_satisfies_oldest_xmin */
typedef enum
{
	XHEAPTUPLE_DEAD,					/* tuple is dead and deletable */
	XHEAPTUPLE_LIVE,					/* tuple is live (committed, no deleter) */
	XHEAPTUPLE_RECENTLY_DEAD,			/* tuple is dead, but not deletable yet */
	XHEAPTUPLE_INSERT_IN_PROGRESS,		/* inserting xact is still in progress */
	XHEAPTUPLE_DELETE_IN_PROGRESS,		/* deleting xact is still in progress */
	XHEAPTUPLE_ABORT_IN_PROGRESS		/* rollback is still pending */
} XHTSVResult;

typedef struct XstoreUndoScanDescData
{
	XHeapTupleTransInfo uinfo;
	TransactionId		prev_undo_xid;
	XHeapTuple			current_xheap_tuple;
} XstoreUndoScanDescData;
typedef XstoreUndoScanDescData *XstoreUndoScanDesc;

extern bool xheap_tuple_fetch(Relation rel, Buffer buffer, OffsetNumber offnum,
							  Snapshot snapshot, XHeapTuple *visible_tuple, ItemPointer new_ctid,
							  bool keep_tup, XHeapTupleTransInfo *saved_xinfo, bool *got_xinfo,
							  const XHeapTuple *saved_tuple, int16 last_var, bool *bool_arr,
							  bool *has_cur_xact_write);

extern bool xheap_tuple_satisfies_visibility(XHeapTuple xhtup, Snapshot snapshot, Buffer buffer);

extern TM_Result xheap_tuple_satisfies_update(Relation rel, Snapshot snapshot, ItemPointer tid,
											  XHeapTuple xtuple, CommandId cid, Buffer buffer,
											  ItemPointer ctid, XHeapTupleTransInfo *xinfo,
											  FullTransactionId *update_sub_xid,
											  FullTransactionId *locker_xid, bool avoid_vis_check,
											  bool multixid_is_myself, bool *inplace_updated);

extern UndoTraversalState xheap_tuple_get_trans_info(Buffer buf, OffsetNumber offnum,
													 XHeapTupleTransInfo *txactinfo,
													 bool *has_cur_xact_write,
													 FullTransactionId *lastXid, UndoRecPtr *urp);

extern XHTSVResult xheap_tuple_satisfies_oldest_xmin(XHeapTuple inplacehtup,
													 FullTransactionId oldest_xmin, Buffer buffer,
													 bool resolve_abort_in_progress,
													 XHeapTuple *preabort_tuple, FullTransactionId *xid, Relation rel,
													 bool *inplace_updated, FullTransactionId *last_xid);

extern bool xheap_tuple_is_surely_dead(XHeapTuple uhtup, Buffer buffer, OffsetNumber offnum,
									   const XHeapTupleTransInfo *cached_xinfo,
									   const bool use_cached_xnfo);


extern bool committed_xid_visible_in_snapshot(FullTransactionId xid, Snapshot snapshot);
extern void update_tuple_header_from_undo_record(UnpackedUndoRecord *urec, XHeapDiskTuple disk_tuple);

#endif	/* XHEAPAM_VISIBILITY_H */