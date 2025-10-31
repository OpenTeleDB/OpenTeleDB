/* -------------------------------------------------------------------------
 *
 * xbtvisbility.h
 *    index tuple visibility interfaces of xbtree index.
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 * include/xbtree/xbtvisibility.h
 * -------------------------------------------------------------------------
 *
 */

#ifndef XBTVISBILITY_H
#define XBTVISBILITY_H
#include "c.h"
#include "utils/snapshot.h"
#include "access/transam.h"
#include "access/xact.h"
#include "utils/relcache.h"
#include "xbtree/xbttup.h"
#include "storage/bufpage.h"

typedef enum
{
	XBTREETUPLE_NEW,		/* inserted */
	XBTREETUPLE_GONE		/* deleted */
} XBTreeTupleOper;

typedef enum
{
	XBTREEVERSION_NONE,
	XBTREEVERSION_CURRENT,
	XBTREEVERSION_OLDER,
	XBTREEVERSION_CHECK_CID
} XBTreeVersionSelector;

typedef struct XBTreeTupleTransInfo
{
	FullTransactionId xid;
	CommandId cid;
	UndoRecPtr urec;
} XBTreeTupleTransInfo;

extern XBTreeVersionSelector xbtree_tuple_version_select(XBTreeTupleOper op, Snapshot snapshot, FullTransactionId modified_xid);
extern XBTreeTupleOper xbtree_oper_from_lp(ItemId item);
extern XBTreeVersionSelector xbtree_check_cid(XBTreeTupleOper op, CommandId tuple_cid, CommandId visibility_cid);
extern bool xbtree_get_tuple_from_undo(UndoRecPtr urec, XBTreeIndexTuple current_tuple, Snapshot snapshot, 
	CommandId curcid);
extern bool _xbt_tuple_satisfies(Page page, Snapshot snapshot, BlockNumber blk, OffsetNumber offnum);
extern void xbt_tuple_satisfies_update(Relation rel, Buffer buf, Page page, BlockNumber blk, OffsetNumber offnum);
#endif