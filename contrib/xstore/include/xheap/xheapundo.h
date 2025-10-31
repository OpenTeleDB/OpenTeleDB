/*-------------------------------------------------------------------------
 *
 * xheapundo.h
 *	
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * include/xheap/xheapundo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XHEAP_UNDO_H
#define XHEAP_UNDO_H

#include "undo/undotype.h"
#include "undo/undorecord.h"
#include "undo/undorequest.h"
#include "undo/undofetch.h"


void execute_undo_insert_in_recovery(Buffer buffer, OffsetNumber off, FullTransactionId xid,
								  bool relhasindex);

int execute_undo_insert(Relation rel, Buffer buffer, OffsetNumber off,
						  UnpackedUndoRecord *urec);

int	 execute_xheap_undoactions(UndoList *ulist, int start_idx, int end_idx,
					   BlockNumber blkno, Relation relation);

#endif                     