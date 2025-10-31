/*-------------------------------------------------------------------------
 *
 * xbtundo.h
 *	  head file for xbtree roll back routines
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * 
 * include/xbtree/xbtundo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XBTUNDO_H
#define XBTUNDO_H

#include "undo/undotype.h"
#include "undo/undorecord.h"
#include "undo/undorequest.h"
#include "undo/undofetch.h"

int execute_undo_delete_xbtree_page(Relation rel, UnpackedUndoRecord *undorecord,
						 Buffer buf, Page page, Offset offnum);

int execute_xbtree_undoactions(UndoList *ulist, int start_idx, int end_idx,
					   BlockNumber blkno, Relation relation);

#endif                     