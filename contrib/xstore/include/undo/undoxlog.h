/* -------------------------------------------------------------------------
 *
 * undoxlog.h
 * 
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 * IDENTIFICATION
 * include/undo/undoxlog.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef UNDOXLOG_H_
#define UNDOXLOG_H_

#include "undo/undotype.h"
#include "access/xlogdefs.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "lib/stringinfo.h"


/* xlog type of extend undo */
#define XLOG_UNDO_EXTEND 0x00
/* xlog type of unlink undo */
#define XLOG_UNDO_UNLINK 0x10
/* xlog type of clean undo */
#define XLOG_UNDO_SLOT_EXTEND 0x20
/* xlog type of clean undo */
#define XLOG_UNDO_SLOT_UNLINK 0x30
/* xlog type of discard undo */
#define XLOG_UNDO_DISCARD 0x50
/* xlog type of rollback undo*/
#define XLOG_UNDO_ROLLBACK_FINISH 0x60

/* allocate slot. */
#define XLOG_UNDOMETA_INFO_SLOT_ALLOC 0x01
/* allocate slot page is new */
#define XLOG_UNDOMETA_INFO_SLOT_INIT 0x02
/* switch another undo log*/
#define XLOG_UNDOMETA_INFO_SWITCH 0x04
/* mark skip when redoing */
#define XLOG_UNDOMETA_INFO_SKIP 0x08

/*
 * xl_undo_header -> flage values, 8 bits are available
 */
#define XLOG_UNDO_HEADER_HAS_BLK_PREV (1 << 2)
#define XLOG_UNDO_HEADER_HAS_PREV_URP (1 << 3)
#define XLOG_UNDO_HEADER_HAS_TOP_XID (1 << 5)
#define XLOG_UNDO_HEADER_HAS_TOAST (1 << 6)

typedef struct xl_undo_meta
{
    uint8              info : 4;
    UndoSlotOffset     slotPtr : 46;
    uint16             lastRecordSize : 16;
	Oid			       dbid;
} xl_undo_meta;

typedef struct xl_undo_header
{
	UndoRecPtr urecptr; /* location of undo record */
	Oid		   relOid;	/* relation id */
	uint8	   flag;
} xl_undo_header;

#define SizeOfXLUndoHeader (offsetof(xl_undo_header, flag) + sizeof(uint8))  

static inline bool
xlog_undo_meta_is_translot(xl_undo_meta *meta)
{
	return meta->info & XLOG_UNDOMETA_INFO_SLOT_ALLOC;
}

static inline bool
xlog_undo_meta_is_intslot(xl_undo_meta *meta)
{
	return meta->info & XLOG_UNDOMETA_INFO_SLOT_INIT;
}

static inline bool
xlog_undo_meta_is_switch(xl_undo_meta *meta)
{
	return meta->info & XLOG_UNDOMETA_INFO_SWITCH;
}

static inline bool
xlog_undo_meta_is_skip(xl_undo_meta *meta)
{
	return meta->info & XLOG_UNDOMETA_INFO_SKIP;
}

static inline void
xlog_undo_meta_setinfo(xl_undo_meta *meta, uint8 minfo)
{
	meta->info |= minfo;
}

static inline uint32
xlog_undo_meta_size(xl_undo_meta *meta)
{
	if (xlog_undo_meta_is_translot(meta))
	{
		return offsetof(struct xl_undo_meta, dbid) + sizeof(Oid);
	}
	else
	{
		return offsetof(struct xl_undo_meta, dbid);
	}
}

static inline UndoSlotOffset
xlog_undo_meta_slotptr(xl_undo_meta *meta)
{
	return meta->slotPtr;
}

static inline void
init_xlog_undo_meta(xl_undo_meta *xlMeta)
{
	xlMeta->info = 0;
	xlMeta->slotPtr = INVALID_UNDO_REC_PTR;
	xlMeta->lastRecordSize = 0;
	xlMeta->dbid = InvalidOid;
}

static inline void
copy_xlog_undo_meta(const xl_undo_meta *src, xl_undo_meta *dest)
{
	dest->slotPtr = src->slotPtr;
	dest->info = src->info;
	dest->lastRecordSize = src->lastRecordSize;
	if (src->info & XLOG_UNDOMETA_INFO_SLOT_ALLOC)
	{
		dest->dbid = src->dbid;
	}
}

typedef struct xl_undolog_discard
{
	UndoSlotPtr	  endSlot;
	UndoSlotPtr	  startSlot;
	uint64		  recycleLoops;
	UndoRecPtr	  endUndoPtr;
	FullTransactionId recycledXid;
	FullTransactionId globalFrozenXid;
} xl_undolog_discard;

typedef struct xl_undolog_extend
{
	UndoRecPtr prevtail;
	UndoRecPtr tail;
} xl_undolog_extend;

typedef struct xl_undolog_unlink
{
	UndoRecPtr head;
	UndoRecPtr prevhead;
} xl_undolog_unlink;

typedef struct xl_undolog_rollback_finish
{
	UndoSlotPtr slotPtr;
} xl_undolog_rollback_finish;

extern void		   undo_xlog_redo(XLogReaderState *record);
extern void		   undo_xlog_desc(StringInfo buf, XLogReaderState *record);
extern const char *undo_xlog_type_name(uint8 subtype);

XLogRecPtr		   xlog_undo_write(void *xlrec, uint8 type);
void			   xlog_undo_meta_write(xl_undo_meta *xlum);
XLogRecPtr         xlog_undo_rollback_finish_write(UndoSlotBuffer slot_buff, UndoSlotPtr slot_ptr);

XLogRedoAction
xlog_undo_read_buffer_for_redo(XLogReaderState *record, RelFileLocator rlocator, ForkNumber forknum, 
	BlockNumber blockno, ReadBufferMode mode, bool get_cleanup_lock, Buffer *buf);


#endif	
