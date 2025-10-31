/*-------------------------------------------------------------------------
 *
 * xbttup.h
 *	  xbtree index tuple definitions.
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 * include/xbtree/xbttup.h
 *-------------------------------------------------------------------------
 */
#ifndef XBTTUP_H
#define XBTTUP_H

#include "c.h"
#include "access/transam.h"
#include "access/xact.h"

#define XstoreIndexTupleSize(itup)		((Size) (((IndexTuple) (itup))->t_info & INDEX_SIZE_MASK))
#define IndexTupleSetSize(itup, newsz) (itup->t_info = ((itup->t_info) & (~INDEX_SIZE_MASK)) | (newsz))
#define XbtreeIndexGetTuple(itup) (((char*)(itup)) + (IndexTupleSize(itup)))

typedef struct XBTreeIndexTupleData {
	FullTransactionId modified_xid;
    UndoRecPtr urec;
} XBTreeIndexTupleData;

typedef XBTreeIndexTupleData* XBTreeIndexTuple;

/* xbtree index tuple is deleted (used for xbtree index only) */
#define LP_INDEX_DELETED 2

/*
 * IndexItemIdIsDeleted
 *		True iff item identifier is in state LP_INDEX_DELETED.
 */
#define IndexItemIdIsDeleted(itemId) \
	((itemId)->lp_flags == LP_INDEX_DELETED)

/*
 * IndexItemIdSetDeleted
 *		Set the item identifier to be LP_INDEX_DELETED.
 */
#define IndexItemIdSetDeleted(itemId) ((itemId)->lp_flags = LP_INDEX_DELETED)

#define LP_INDEX_INSERTED LP_NORMAL

/*
 * IndexItemIdIsInserted
 *		True iff item identifier is in state NORMAL.
 */
#define IndexItemIdIsInserted(itemId) \
	((itemId)->lp_flags == LP_INDEX_INSERTED)

#endif