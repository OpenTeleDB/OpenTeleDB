/* -------------------------------------------------------------------------
 *
 * xheap.h
 * the access interfaces of inplace update engine
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * include/xheap/xheap.h
 * -------------------------------------------------------------------------
 *
 */
#ifndef XHEAP_H
#define XHEAP_H

#include "access/transam.h"
#include "storage/itemptr.h"
#include "utils/snapshot.h"
#include "nodes/parsenodes.h"
#include "xheap/xtuple.h"
#include "xheap/xpage.h"
#include "xheap/xredo.h"
#include "undo/undobuffer.h"
#include "undo/undoxlog.h"
#include "undo/undolog.h"
#include "undo/undorecord.h"
#include "access/heapam.h"
#include "access/rewriteheap.h"
#include "commands/vacuum.h"

/*
 * Threshold for the number of blocks till which non-inplace updates due to
 * reuse of transaction slot. The performance testing on various sizes of tables
 * indicate that threshold of 200 is good enough to keep the contention on
 * transaction slots under control.
 */
#define NUM_BLOCKS_FOR_NON_INPLACE_UPDATES 3

typedef enum XHeapDMLType
{
	XHEAP_INSERT = 0,
	XHEAP_UPDATE,
	XHEAP_DELETE,
} XHeapDMLType;

/* Working data for XHeapPagePrune and subroutines */
typedef struct XPruneState
{
	FullTransactionId new_prune_xid;	/* new prune hint value for page */
	FullTransactionId latest_removed_xid; /* latest xid to be removed by this prune */
	int			  nunused;
	int			  remain; /* the number of tuples that will survive after prune */
	uint16		  nfixed;
	/* arrays that accumulate indexes of items to be changed */

	OffsetNumber nowunused[MaxPossibleXHeapTuplesPerPage];
	OffsetNumber nowfixed[MaxPossibleXHeapTuplesPerPage];
	uint16		 fixedlen[MaxPossibleXHeapTuplesPerPage];
	/* marked[i] is TRUE if item i is entered in one of the above arrays */
	bool marked[MaxPossibleXHeapTuplesPerPage + 1];
} XPruneState;

/* Controls the overall probability of conducting FSM update during page pruning */
#define FSM_UPDATE_HEURISTI_PROBABILITY 0.3

typedef struct
{
	Relation relation;
	Buffer	 buffer;
} RelationBuffer;


typedef struct itemIdCompactData
{
	uint16		offsetindex;	/* linp array index */
	int16		itemoff;		/* page offset of item data */
	uint16		alignedlen;		/* MAXALIGN(item data len) */
} itemIdCompactData;
typedef itemIdCompactData *itemIdCompact;

typedef struct {
    int offsetindex;      /* linp array index */
    int itemoff;          /* page offset of item data */
    Size alignedlen;      /* MAXALIGN(item data len) */
    ItemIdData olditemid; /* used only in PageIndexMultiDelete */
} itemIdSortData;
typedef itemIdSortData* itemIdSort;

Datum xheap_fast_get_attr(XHeapTuple tup, int attnum, TupleDesc tuple_desc, bool *isnull);
Oid xheap_insert(Relation rel, XHeapTupleData *tup, CommandId cid, 
			 int options, BulkInsertState bistate, bool isToast);
XHeapTuple			 xheap_prepare_insert(Relation rel, XHeapTuple tuple, int options);
void relation_put_xtuple(Relation relation, Buffer buffer, XHeapTupleData *tuple);
typedef void *Tuple;


TM_Result	xheap_update(Relation relation, ItemPointer otid, XHeapTuple newtup,
						 CommandId cid, Snapshot snapshot, Snapshot crosscheck, bool wait,
						 TM_FailureData *tmfd, LockTupleMode *lockmode, 
						 Bitmapset **modified_idx_attrs, bool *inplace_update);

TM_Result xheap_lock_tuple(Relation relation, XHeapTuple tuple, CommandId cid,
						   Snapshot snapshot, LockTupleMode mode,
						   LockWaitPolicy wait_policy, bool follow_updates,
						   TM_FailureData *tmfd, bool eval, bool on_conflict_update);

CommandId xheap_tuple_get_cid(XHeapTuple uhtup, Buffer buf);
bool	  xheap_fetch_row(Relation relation, ItemPointer tid, Snapshot snapshot,
						TupleTableSlot *slot, XHeapTuple xtuple);
bool xheap_fetch(Relation relation, Snapshot snapshot, ItemPointer tid, XHeapTuple tuple,
				 Buffer *buf, bool keep_buf, bool keep_tup, bool *has_cur_xact_write);
TM_Result xheap_delete(Relation relation, ItemPointer tid, 
			 CommandId cid, Snapshot crosscheck, Snapshot snapshot, bool wait, 
			 TM_FailureData *tmfd, bool changing_part);
void xheap_multi_insert(Relation relation, XHeapTuple *tuples, int ntuples, CommandId cid,
						int options, BulkInsertState bistate);

extern void xheap_lazy_vacuum(Relation onerel, VacuumParams *params,
							   BufferAccessStrategy bstrategy);


UndoRecPtr xheap_prepare_undo_insert(Oid relOid, Oid relfilenode, Oid tablespace,
									 UndoPersistence persistence, FullTransactionId xid,
									 CommandId cid, UndoRecPtr prevurp_in_one_blk,
									 UndoRecPtr prevurp_in_one_xact, BlockNumber blk, XLogReaderState *xlog_record,
									 xl_undo_header *xlundohdr, xl_undo_meta *xlundometa);
UndoRecPtr xheap_prepare_undo_multi_insert(
	Oid relOid, Oid relfilenode, Oid tablespace, UndoPersistence persistence,
	Buffer buffer, int nranges, FullTransactionId xid, CommandId cid,
	UndoRecPtr prevurp_in_one_blk, UndoRecPtr prevurp_in_one_xact, UndoPrepareBuffers **upbuffers_ptr,
	UndoRecPtr *first_urecptr, UndoRecPtr *urpvec, BlockNumber blk, XLogReaderState *xlog_record,
	xl_undo_header *xlundohdr, xl_undo_meta *xlundometa);
UndoRecPtr xheap_prepare_undo_delete(Oid relOid, Oid relfilenode,
								  Oid tablespace, UndoPersistence persistence,
								  Buffer buffer, OffsetNumber offnum, FullTransactionId xid,
								  FullTransactionId subxid, CommandId cid,
								  UndoRecPtr prevurp_in_one_blk, UndoRecPtr prevurp_in_one_xact, FullTransactionId xactid,
								  XHeapTuple oldtuple, BlockNumber blk,
								  XLogReaderState *xlog_record, xl_undo_header *xlundohdr, xl_undo_meta *xlundometa);
UndoRecPtr xheap_prepare_undo_update(
	Oid relOid, Oid relfilenode, Oid tablespace, UndoPersistence persistence,
	Buffer buffer, Buffer newbuffer, OffsetNumber offnum, FullTransactionId xid,
	FullTransactionId subxid, CommandId cid, UndoRecPtr prevurp_in_one_blk,
	UndoRecPtr prevurp_in_one_xact, FullTransactionId old_updater_xid,
	XHeapTuple oldtuple, bool is_inplace_update, UndoRecPtr *new_urec,
	int undo_xor_delta_size, BlockNumber oldblk, BlockNumber newblk,
	XLogReaderState *xlog_record, xl_undo_header *xlundohdr, xl_undo_meta *xlundometa);
extern int	xheap_cal_tuple_size(Relation relation, XHeapDiskTuple tup, TupleDesc tupDesc);
extern bool xheap_page_prune_opt_page(Relation relation, Buffer buffer, FullTransactionId xid,
								  bool acquire_conditional_lock);
extern bool xheap_page_prune_opt(Relation relation, Buffer buffer, OffsetNumber offnum,
							  Size space_required);
extern int	xheap_page_prune(Relation rel, const RelationBuffer *relbuf,
						   FullTransactionId oldest_xmin, bool report_stats,
						   FullTransactionId *latest_removed_xid, bool *pruned);
extern int	xheap_page_prune_guts(Relation relation, const RelationBuffer *relbuf,
							   FullTransactionId OldestXmin, OffsetNumber target_offnum,
							   Size space_required, bool report_stats, bool forcePrune,
							   FullTransactionId *latest_removed_xid, bool *pruned, int *remain);
extern void xheap_page_prune_execute(Buffer buffer, OffsetNumber target_offnum,
								  const XPruneState *prstate);
extern void xheap_page_repaire_fregmentation(Relation rel, Buffer buffer,
									 OffsetNumber target_offnum, Size space_required,
									 bool *pruned);
extern void xheap_page_prune_fsm(Relation relation, Buffer buffer, FullTransactionId fxid,
							  Page page, BlockNumber blkno);
bool xheap_exec_pending_undo_actions(Relation rel, Buffer buffer, FullTransactionId xwait, UndoRecPtr urp);

extern XLogRecPtr log_xheap_clean(Relation reln, Buffer buffer, OffsetNumber target_offnum,
								Size space_required, OffsetNumber *nowunused, int nunused,
								OffsetNumber *nowfixed, uint16 *fixedlen, uint16 nfixed,
								FullTransactionId latest_removed_xid, bool pruned);

extern void simple_xheap_delete(Relation relation, ItemPointer tid, Snapshot snapshot);

void xheap_abort_speculative(Relation relation, XHeapTuple tuple);

#endif