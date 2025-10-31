/* -------------------------------------------------------------------------
 *
 * xredo.h
 * the access interfaces of xheap recovery.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * include/xheap/xredo.h
 * -------------------------------------------------------------------------
 */

#ifndef XREDO_H
#define XREDO_H


#include "postgres.h"

#include "miscadmin.h"

#include "access/xlog.h"
#include "access/xlogutils.h"
#include "undo/undotype.h"
#include "undo/undorecord.h"
#include "catalog/pg_tablespace.h"
#include "access/xlog_internal.h"

/*
 * WAL record definitions for xheap WAL operations
 *
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field.  We use 3 for opcode and one for init bit.
 */
#define XLOG_XHEAP_INSERT 0x00
#define XLOG_XHEAP_DELETE 0x10
#define XLOG_XHEAP_UPDATE 0x20
#define XLOG_XHEAP_LOCK 0x30
#define XLOG_XHEAP_CLEAN 0x50
#define XLOG_XHEAP_MULTI_INSERT 0x60
#define XLOG_XHEAP_OPMASK 0x70
/*
 * When we insert 1st item on new page in INSERT, UPDATE, HOT_UPDATE,
 * or MULTI_INSERT, we can (and we do) restore entire page in redo
 */
#define XLOG_XHEAP_INIT_PAGE 0x80

/*
 * XlXHeap* ->flag values, 8 bits are available
 */
#define XLOG_XHEAP_INSERT_ON_TOAST_RELATION (1 << 3)
#define XLOG_XHEAP_CONTAINS_NEW_TUPLE (1 << 4)
#define XLZ_INSERT_IS_FROZEN (1 << 5)
#define XLOG_XHEAP_CONTAINS_OLD_HEADER (1 << 6)
#define XLOG_XHEAP_INSERT_LAST_IN_MULTI (1 << 7)

/*
 * XlXHeapDelete flag values, 8 bits are available.
 */
#define XLZ_HAS_DELETE_UNDOTUPLE (1 << 1)

/* all fields in XHeapDiskTuple */
/* size=8 alignment=2 */
typedef struct xl_xheap_header
{
	uint16 flag;
	uint16 flag2;
	uint8  t_hoff;
	char   padding;
} xl_xheap_header;

#define SizeOfXHeapHeader (offsetof(xl_xheap_header, padding))


/* size=24 alignment=8 */
typedef struct xl_xheap_delete
{
	FullTransactionId oldxid; /* xid in oldTD, i.e. the xid of last operation on the tuple */
	OffsetNumber  offnum;
	uint8		  flag;
	char		  padding[3];
} xl_xheap_delete;

#define SizeOfXHeapDelete (offsetof(xl_xheap_delete, padding))

/* size=4 alignment=2 */
typedef struct xl_xheap_insert
{
	OffsetNumber offnum;
	uint8		 flags;
	char		 padding;
} xl_xheap_insert;

#define SizeOfXHeapInsert (offsetof(xl_xheap_insert, padding))

#define XLZ_UPDATE_PREFIX_FROM_OLD (1 << 0)
#define XLZ_UPDATE_SUFFIX_FROM_OLD (1 << 1)
#define XLZ_NON_INPLACE_UPDATE (1 << 2)
#define XLZ_HAS_UPDATE_UNDOTUPLE (1 << 3)

/* size=24 alignment=8 */
typedef struct xl_xheap_update
{
	/* XHeap related info */
	FullTransactionId oldxid; /* xid in oldTD, i.e. the xid of last operation on the tuple */
	OffsetNumber  old_offnum; /* old tuple's offset */
	uint16		  old_tuple_flag;
	OffsetNumber  new_offnum; /* new tuple's offset */
	uint8		  flags;	  /* reduced from uint16 */
} xl_xheap_update;

#define SizeOfXHeapUpdate (offsetof(xl_xheap_update, flags) + sizeof(uint8))

/*
 * This is what we need to know about vacuum page cleanup/redirect
 *
 * The array of OffsetNumbers following the fixed part of the record contains:
 * for each redirected item: the item offset, then the offset redirected to
 * for each now-dead item: the item offset for each now-unused item: the item offset
 * The total number of OffsetNumbers is therefore 2*nredirected+ndead+nunused.
 * Note that nunused is not explicitly stored, but may be found by reference to the
 * total record length.
 */
#define XLZ_CLEAN_CONTAINS_OFFSET (1 << 0)
#define XLZ_CLEAN_ALLOW_PRUNING (1 << 1)
#define XLZ_CLEAN_CONTAINS_TUPLEN (1 << 2)

/* size=16 alignment=8 */
typedef struct XlXHeapClean
{
	FullTransactionId latest_removed_xid;
	uint16		  nunused;
	uint16		  nfixed;
	uint8		  flags;
	char		  padding[3];
	/* OFFSET NUMBERS are in the block reference 0 */
} xl_xheap_clean;

#define SizeOfXHeapClean (offsetof(xl_xheap_clean, padding))

typedef struct XlXHeapMultiInsert
{
	int	  ntuples;
	uint8 flags;
} XlXHeapMultiInsert;

#define SizeOfXHeapMultiInsert (offsetof(XlXHeapMultiInsert, flags) + sizeof(uint8))

typedef struct XlMultiInsertXTuple
{
	int			  datalen;
	FullTransactionId xid;
	uint16		  flag;
	uint16		  flag2;
	uint8		  t_hoff;
} XlMultiInsertXTuple;

#define SizeOfMultiInsertXTuple (offsetof(XlMultiInsertXTuple, t_hoff) + sizeof(uint8))

typedef struct XlXHeapLock 
{
	FullTransactionId locker_xid;
	OffsetNumber offnum;		/* locked tuple's offset on page */
	uint16		 infomask;      /* lock related mask for the tuple */   
} XlXHeapLock;

#define SizeOfXHeapLock (sizeof(XlXHeapLock))

/*
 * WAL record definitions for rollback WAL operations
 */
#define XLOG_XHEAPUNDO_PAGE 0x00
#define XLOG_XHEAPUNDO_ABORT_SPECINSERT 0x20

/*
 * xl_undoaction_page flag values, 8 bits are available.
 */
#define XLU_INIT_PAGE (1 << 0)

/* This is used to write WAL for undo actions */
typedef struct XHeapUndoActionWALInfo
{
	Buffer		  buffer;
	OffsetNumber  xlog_min_lp_offset;
	OffsetNumber  xlog_max_lp_offset;
	Offset		  xlog_copy_start_offset;
	Offset		  xlog_copy_end_offset;

	FullTransactionId pd_prune_xid;
	uint16		  pd_flags;
	uint16		  potential_freespace;
	bool		  need_init;
} XHeapUndoActionWALInfo;

#define SizeOfXHeapUndoActionWALInfo \
	(offsetof(XHeapUndoActionWALInfo, needInit) + sizeof(bool))

/*
 * XlXHeapUndoAbortSpecInsert flag values, 8 bits are available
 */
#define XLU_ABORT_SPECINSERT_INIT_PAGE (1 << 0)
#define XLU_ABORT_SPECINSERT_REL_HAS_INDEX (1 << 1)

typedef struct XlXHeapUndoAbortSpecInsert
{
	OffsetNumber offset;
	int			 logno;
} XlXHeapUndoAbortSpecInsert;

#define SizeOfXHeapUndoAbortSpecInsert \
	(offsetof(XlXHeapUndoAbortSpecInsert, logno) + sizeof(int))

/*
 * Hint bit for whether xlog contains CSN info, which is stored in xl_term.
 */
#define XLOG_CONTAIN_CSN 0x80000000


extern void			 xheap_redo(XLogReaderState *record);
extern void			 xheap_desc(StringInfo buf, XLogReaderState *record);
extern const char	*xheap_type_name(uint8 subtype);
extern char			*parse_undo_header(xl_undo_header *xlundohdr, Oid *partition_oid,
								   UndoRecPtr *blkprev, UndoRecPtr *prev_urp,
								   FullTransactionId *full_xid, uint32 *toast_len);

extern void			 xheap_undo_redo(XLogReaderState *record);
extern void			 xheap_undo_desc(StringInfo buf, XLogReaderState *record);
extern const char	*xheap_undo_type_name(uint8 subtype);
extern TransactionId xheap_xlog_get_current_xid(XLogReaderState *record);
extern void			 xheap_xlog_insert(XLogReaderState *record);
extern void resolve_recovery_conflict_with_global_frozen_xmin(FullTransactionId latest_removed_full_xid);

#ifndef FRONTEND
#include "replication/decode.h"
void decode_xheap_op(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
#endif

#endif
