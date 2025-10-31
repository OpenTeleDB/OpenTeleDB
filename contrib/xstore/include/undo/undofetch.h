/* -------------------------------------------------------------------------
 *
 * undofetch.h
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 * inlcude/undo/undofetch.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef UNDOFETCH_H
#define UNDOFETCH_H

#include "undo/undorecord.h"
#include "undo/undoxlog.h"
#include "undo/undolog.h"

typedef struct UndoList
{
	UnpackedUndoRecord	**uurecs;
	int			        uurec_size;		 
	int			        uurec_cap;      
	MemoryContext       mem_ctx;
} UndoList;

UndoList *new_undo_list(int cap);
void destroy_undo_list(UndoList *ulist);
void push_undo_list(UndoList *ulist, UnpackedUndoRecord *urec);
void qsort_undo_list(UndoList *ulist);

static inline UnpackedUndoRecord *
get_urec_from_list(UndoList *ulist, int i)
{
	Assert(i >= 0 && i < ulist->uurec_size);
	return *(ulist->uurecs + i);
}

/*
 * Fetch multiple undo record
 */
UndoList 
*bulk_fetch_undo_for_range(UndoRecPtr *start_recptr, UndoRecPtr end_recptr, int max_apply_size);

/*
 * Fetch multiple undo record for one tuple
 */
UndoList *
bulk_fetch_undo_for_tuple(UndoRecPtr *start_recptr, int max_apply_size);

typedef bool (*SatisfyUndoRecordCallback)(UnpackedUndoRecord *urec, FullTransactionId xid);

UndoTraversalState 
fetch_undo_record(UnpackedUndoRecord *urec, FullTransactionId xid, bool need_bypass,
								   FullTransactionId *last_xid, SatisfyUndoRecordCallback callback);

/*
 *  satisfied callback function.
 */
bool 
satisfy_undo_record(UnpackedUndoRecord *urec, FullTransactionId xid);


UndoTraversalState 
fetch_transinfo_from_undo(UndoRecPtr urec_ptr, BlockNumber blocknum, OffsetNumber offnum, 
					   FullTransactionId xid, CommandId *cid, ItemPointer new_ctid,
					   bool need_bypass, FullTransactionId *last_xid, UndoRecPtr *urp);

UndoTraversalState 
fetch_subxid_from_undo(UndoRecPtr urecptr, FullTransactionId *subxid);

UndoRecPtr	   
undo_record_get_preurp(UnpackedUndoRecord *urec, UndoRecPtr cur_recptr, Buffer *buffer);

UndoRecordSize 
undo_record_get_prerecordlen(UndoRecPtr cur_recptr, Buffer *input_buffer);

#endif
