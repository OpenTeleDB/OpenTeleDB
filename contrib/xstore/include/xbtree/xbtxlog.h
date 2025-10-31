/*-------------------------------------------------------------------------
 *
 * xbtxlog.h
 *	  header file for xbtree xlog routines
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * include/access/xbtxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XBTXLOG_H
#define XBTXLOG_H

#include "postgres.h"
#include "access/nbtxlog.h"
#include "access/xlogutils.h"
#include "storage/bufmgr.h"

enum {
    BTREE_INSERT_ORIG_BLOCK_NUM = 0,
    BTREE_INSERT_CHILD_BLOCK_NUM,
    BTREE_INSERT_META_BLOCK_NUM,
};

enum {
    BTREE_SPLIT_LEFT_BLOCK_NUM = 0,
    BTREE_SPLIT_RIGHT_BLOCK_NUM,
    BTREE_SPLIT_RIGHTNEXT_BLOCK_NUM,
    BTREE_SPLIT_CHILD_BLOCK_NUM
};


enum {
    BTREE_VACUUM_ORIG_BLOCK_NUM = 0,
};

enum {
    BTREE_DELETE_ORIG_BLOCK_NUM = 0,
};

enum {
    BTREE_HALF_DEAD_LEAF_PAGE_NUM = 0,
    BTREE_HALF_DEAD_PARENT_PAGE_NUM,
};

enum {
    BTREE_UNLINK_PAGE_CUR_PAGE_NUM = 0,
    BTREE_UNLINK_PAGE_LEFT_NUM,
    BTREE_UNLINK_PAGE_RIGHT_NUM,
    BTREE_UNLINK_PAGE_CHILD_NUM,
    BTREE_UNLINK_PAGE_META_NUM,
};


enum {
    BTREE_NEWROOT_ORIG_BLOCK_NUM = 0,
    BTREE_NEWROOT_LEFT_BLOCK_NUM,
    BTREE_NEWROOT_META_BLOCK_NUM
};


/*
 * XLOG records for XBTree operations
 *
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field
 */
#define XLOG_XBTREE_INSERT_LEAF 0x00      /* add index tuple without split */
#define XLOG_XBTREE_INSERT_UPPER 0x10     /* same, on a non-leaf page */
#define XLOG_XBTREE_INSERT_META 0x20      /* same, plus update metapage */
#define XLOG_XBTREE_SPLIT_L 0x30          /* add index tuple with split */
#define XLOG_XBTREE_SPLIT_R 0x40          /* as above, new item on right */
#define XLOG_XBTREE_UNDO  0x50
#define XLOG_XBTREE_UNLINK_PAGE 0x80      /* delete an entire page */
#define XLOG_XBTREE_UNLINK_PAGE_META 0x90 /* same, and update metapage */
#define XLOG_XBTREE_NEWROOT 0xA0          /* new root page */
#define XLOG_XBTREE_MARK_PAGE_HALFDEAD  0xB0 /* page deletion that makes parent half-dead */
#define XLOG_XBTREE_VACUUM   0xC0         /* delete entries on a page during  \
                                           * vacuum */
#define XLOG_XBTREE_REUSE_PAGE   0xD0     /* old page is about to be reused from \
                                           * FSM */
#define XLOG_XBTREE_DELETE 0xE0           /* delete leaf index tuples for a page */
#define XLOG_XBTREE_PRUNE_PAGE 0xF0

typedef struct xl_xbtree_delete {
    FullTransactionId xid;
    FullTransactionId oldxid;
    OffsetNumber offset;
} xl_xbtree_delete;

#define SizeOfXBTreeDelete (offsetof(xl_xbtree_delete, offset) + sizeof(OffsetNumber))

typedef struct xl_xbtree_prune_page {
    FullTransactionId new_prune_xid;
    FullTransactionId latestRemovedXid;
    int32 count;
} xl_xbtree_prune_page;

#define SizeOfXBTreePrunePage (offsetof(xl_xbtree_prune_page, count) + sizeof(int32))

/*
 * This is what we need to know about deletion of a btree page.  The target
 * identifies the tuple removed from the parent page (note that we remove
 * this tuple's downlink and the *following* tuple's key).	Note we do not
 * store any content for the deleted page --- it is just rewritten as empty
 * during recovery, apart from resetting the btpo.xact.
 *
 * Backup Blk 0: target block being deleted
 * Backup Blk 1: target block's left sibling, if any
 * Backup Blk 2: target block's right sibling
 * Backup Blk 3: target block's parent
 * Backup Blk 4: metapage (if rightsib becomes new fast root)
 */
typedef struct xl_btree_delete_page {
    OffsetNumber poffset;    /* deleted tuple id in parent page */
    BlockNumber leftblk;     /* child block's left sibling, if any */
    BlockNumber rightblk;    /* child block's right sibling */
    TransactionId btpo_xact; /* value of btpo.xact for use in recovery */
                             /* xl_btree_metadata FOLLOWS IF XLOG_BTREE_DELETE_PAGE_META */
} xl_btree_delete_page;

#define SizeOfBtreeDeletePage (offsetof(xl_btree_delete_page, btpo_xact) + sizeof(TransactionId))

#define XBTREEUNDO_DELETE  0x01
#define XBTREEUNDO_INSERT  0x02

/*
 * prototypes for functions in xbtxlog.c
 */
extern void xbtree_redo(XLogReaderState* record);
extern void xbtree_desc(StringInfo buf, XLogReaderState* record);
extern const char* xbtree_identify(uint8 info);

#endif