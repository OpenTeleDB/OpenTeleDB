/*-------------------------------------------------------------------------
 *
 * xbtrecycle.h
 *	  header file for xbtree reycle routines
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * 
 * include/access/xbtrecycle.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XBTRECYCLE_H
#define XBTRECYCLE_H

#include "postgres.h"
#include "access/xlogutils.h"
#include "utils/tuplestore.h"

typedef struct XBTRecycleQueueAddress {
    Buffer queue_fuf;
    BlockNumber index_blkno;
    uint16 offset;
} XBTRecycleQueueAddress;

#define XLOG_XBTREE2_RECYCLE_QUEUE_INIT_PAGE 0x10
#define XLOG_XBTREE2_RECYCLE_QUEUE_ENDPOINT 0x20
#define XLOG_XBTREE2_RECYCLE_QUEUE_MODIFY 0x30

enum {
    XBTREE2_RECYCLE_QUEUE_INIT_PAGE_CURR_BLOCK_NUM,
    XBTREE2_RECYCLE_QUEUE_INIT_PAGE_LEFT_BLOCK_NUM,
    XBTREE2_RECYCLE_QUEUE_INIT_PAGE_RIGHT_BLOCK_NUM,
};

enum {
    XBTREE2_RECYCLE_QUEUE_ENDPOINT_CURR_BLOCK_NUM,
    XBTREE2_RECYCLE_QUEUE_ENDPOINT_NEXT_BLOCK_NUM,
};

enum {
    XBTREE2_RECYCLE_QUEUE_MODIFY_BLOCK_NUM,
};

typedef struct XBTRecycleQueueItemData {
    FullTransactionId xid;
    BlockNumber blkno;
    uint16 prev;
    uint16 next;
} XBTRecycleQueueItemData;

typedef struct XBTRecycleQueueHeaderData {
    uint32 flags;
    uint16 head;            /* head of the ordered list in this page */
    uint16 tail;            /* tail of the ordered list in this page */
    uint16 free_items;
    uint16 free_list_head;
    BlockNumber prev_blkno;
    BlockNumber next_blkno;
    XBTRecycleQueueItemData items[FLEXIBLE_ARRAY_MEMBER];
} XBTRecycleQueueHeaderData;

typedef struct xl_xbtree2_recycle_queue_init_page {
    bool inserting_new_page;
    BlockNumber prev_blkno;
    BlockNumber curr_blkno;
    BlockNumber next_blkno;
} xl_xbtree2_recycle_queue_init_page;

#define SizeOfXBTree2RecycleQueueInitPage (sizeof(xl_xbtree2_recycle_queue_init_page))

typedef struct xl_xbtree2_recycle_queue_endpoint {
    bool is_head;
    BlockNumber left_blkno;
    BlockNumber right_blkno;
} xl_xbtree2_recycle_queue_endpoint;

#define SizeOfXBTree2RecycleQueueEndpoint (sizeof(xl_xbtree2_recycle_queue_endpoint))

typedef struct xl_xbtree2_recycle_queue_modify {
    bool is_insert;
    uint16 offset;
    BlockNumber blkno;
    XBTRecycleQueueItemData item;
    XBTRecycleQueueHeaderData header;
} xl_xbtree2_recycle_queue_modify;

#define SizeOfXBTree2RecycleQueueModify (sizeof(xl_xbtree2_recycle_queue_modify))

/*
 * prototypes for functions in xbtrecycle.c
 */
extern void xbtree2_redo(XLogReaderState* record);
extern void xbtree2_desc(StringInfo buf, XLogReaderState* record);
extern const char* xbtree2_type_name(uint8 s_xb_type);

typedef XBTRecycleQueueItemData* XBTRecycleQueueItem;
typedef XBTRecycleQueueHeaderData* XBTRecycleQueueHeader;

#define URQ_HEAD_PAGE (1 << 0)
#define URQ_TAIL_PAGE (1 << 1)

#define XBTRecycleMaxItems \
    ((BLCKSZ - sizeof(PageHeaderData) - offsetof(XBTRecycleQueueHeaderData, items)) / sizeof(XBTRecycleQueueItemData))

/* structures for xbtrecycle.c */
typedef enum {
    RECYCLE_FREED_FORK = 0,
    RECYCLE_EMPTY_FORK,
    RECYCLE_NONE_FORK,   //last unused item
} XBTRecycleForkNumber;

/*
 * prototypes for functions in xbtrecycle.c
 */
extern bool xbtree_page_recyclable(Page page);
extern const BlockNumber minRecycleQueueBlockNumber;
extern XBTRecycleQueueHeader GetRecycleQueueHeader(Page page, BlockNumber blkno);
extern Buffer read_recycle_queue_buffer(Relation rel, BlockNumber blkno);
extern void xbt_init_recycle_queue(Relation rel);
extern void xbtree_try_recycle_empty_page(Relation rel);
extern void xbtree_record_free_page(Relation rel, BlockNumber blkno, FullTransactionId xid);
extern void xbtree_record_empty_page(Relation rel, BlockNumber blkno, FullTransactionId xid);
extern void xbtree_record_used_page(Relation rel, XBTRecycleQueueAddress addr);
extern Buffer xbtree_get_available_page(Relation rel, XBTRecycleForkNumber fork_number, XBTRecycleQueueAddress* addr);
extern void xbtree_recycle_queue_init_page(Relation rel, Page page, BlockNumber blkno, BlockNumber prev_blkno,
    BlockNumber next_blkno);
extern void xbtree_recycle_queue_change_chain(Buffer buf, BlockNumber new_blkno, bool set_next);
extern void xbtree_recycle_queue_page_change_endpoint_left_page(Buffer buf, bool is_head);
extern void xbtree_recycle_queue_page_change_endpoint_right_page(Buffer buf, bool is_head);
extern void xbtree_xlog_recycle_queue_modify_page(Buffer buf, xl_xbtree2_recycle_queue_modify *xlrec);
extern uint32 xbtree_recycle_queue_page_dump(Relation rel, Buffer buf, bool record_each_item,
    TupleDesc *tuple_desc, Tuplestorestate *tupstore, uint32 cols);
extern bool xbtree_recycle_queue_page_verify(Relation rel, Buffer buf);
extern void xbtree_dump_recycle_queue_fork(Relation rel, XBTRecycleForkNumber fork_num, TupleDesc *tuple_desc,
    Tuplestorestate *tupstore, uint32 cols);
#endif