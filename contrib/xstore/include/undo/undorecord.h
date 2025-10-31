/* -------------------------------------------------------------------------
 *
 * undorecord.h
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/undo/undorecord.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef UNDORECORD_H
#define UNDORECORD_H

#include "undo/undotype.h"
#include "undo/undoxlog.h"
#include "access/transam.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/relfilelocator.h"

/*
 * Every undo record begins with an UndoRecordHeader structure, which is
 * followed by the additional structures indicated by the contents of
 * urec_info.  All structures are packed into the alignment without padding
 * bytes, and the undo record itself need not be aligned either, so care
 * must be taken when reading the header.
 */
typedef struct UndoRecordHeader
{
	uint8		  urec_type;
	uint8		  urec_info;
	Oid			  urec_reloid;
	Oid			  urec_relfilenode;

	FullTransactionId urec_xid; /* Transaction id */
	CommandId	  urec_cid; /* command id */
} UndoRecordHeader;

#define SIZE_OF_UNDO_RECORD_HEADER \
	(offsetof(UndoRecordHeader, urec_cid) + sizeof(CommandId))

#define UNDO_UREC_INFO_UNKNOWN 0x00
#define UREC_INFO_PAYLOAD 0x01
#define UREC_INFO_PREURP 0x02
#define UREC_INFO_PREXID 0x08
#define UREC_INFO_SUBXACT 0x10
#define UREC_INFO_DATAHEADER 0x20
#define UREC_INFO_TABLESPACE 0x40

/*
 * Identifying information for a block to which this record pertains, and
 * a pointer to the previous record for the same tuple.
 */
typedef struct UndoRecordBlock
{
	UndoRecPtr	 urec_tupprev; /* previous undo for tuple */
	BlockNumber	 urec_block;	  /* block number */
	OffsetNumber urec_offset;  /* offset number */
} UndoRecordBlock;


#define SIZE_OF_UNDO_RECORD_BLOCK \
	(offsetof(UndoRecordBlock, urec_offset) + sizeof(OffsetNumber))	// 12+2

typedef struct UnpackedUndoRecord
{
	FullTransactionId uur_xid; /* Transaction id */
	CommandId	  uur_cid; /* command id */
	Oid			  uur_reloid;
	Oid			  uur_relfilenode;
	uint8		  uur_type;
	uint8		  uur_info;

	UndoRecPtr	  uur_tpprevurp; /* previous undo for tuple */
	BlockNumber	  uur_blkno;	    /* block number */
	OffsetNumber  uur_offset;    /* offset number */

	UndoRecPtr        uur_txnprevurp;     /* previous undo for transaction for switch log */
	uint16            uur_dataheaderflag;
	FullTransactionId uur_prevxid;
	Oid               uur_tablespace;
	FullTransactionId uur_subxid;
	UndoRecordSize    uur_payloadlen; 
	StringInfoData    uur_payload;

    /* info in memory */
	UndoRecPtr    uur_urp;  
	Buffer	      uur_buffer; 
	int		      buffer_idx;
	bool		  is_update;
	bool		  is_copy;
	MemoryContext mem_ctx;
} UnpackedUndoRecord;

static inline FullTransactionId
GetUndoRecordXid(UnpackedUndoRecord *urec)
{
	return urec->uur_xid;
}
static inline CommandId
GetUndoRecordCid(UnpackedUndoRecord *urec)
{
	return urec->uur_cid;
}
static inline Oid
GetUndoRecordReloid(UnpackedUndoRecord *urec)
{
	return urec->uur_reloid;
}
static inline Oid
GetUndoRecordRelfilenode(UnpackedUndoRecord *urec)
{
	return urec->uur_relfilenode;
}
static inline Oid
GetUndoRecordTablespace(UnpackedUndoRecord *urec)
{
	return urec->uur_tablespace;
}

static inline FullTransactionId
GetUndoRecordSubXid(UnpackedUndoRecord *urec)
{
	return urec->uur_subxid;
}

static inline uint8
GetUndoRecordUtype(UnpackedUndoRecord *urec)
{
	return urec->uur_type;
}
static inline uint8
GetUndoRecordUinfo(UnpackedUndoRecord *urec)
{
	return urec->uur_info;
}

static inline UndoRecPtr
GetUndoRecordTpprev(UnpackedUndoRecord *urec)
{
	return urec->uur_tpprevurp;
}
static inline BlockNumber
GetUndoRecordBlkno(UnpackedUndoRecord *urec)
{
	return urec->uur_blkno;
}
static inline OffsetNumber
GetUndoRecordOffset(UnpackedUndoRecord *urec)
{
	return urec->uur_offset;
}
static inline FullTransactionId
GetUndoRecordOldXactId(UnpackedUndoRecord *urec)
{
	return urec->uur_prevxid;
}

static inline bool
UndoRecordHasDataHeadFlag(UnpackedUndoRecord *urec)
{
	return urec->uur_info & UREC_INFO_DATAHEADER;
}

static inline uint16 
GetUndoRecordDataHeadFlag(UnpackedUndoRecord *urec)
{
	return urec->uur_dataheaderflag;
}

static inline StringInfoData *
GetUndoRecordRawdata(UnpackedUndoRecord *urec)
{
	return &(urec->uur_payload);
}

static inline int
GetUndoRecordRawdataLen(UnpackedUndoRecord *urec)
{
	return urec->uur_payload.len;
}

static inline UndoRecPtr
GetUndoRecordTxnPrevurp(UnpackedUndoRecord *urec)
{
	return urec->uur_txnprevurp;
}

static inline UndoRecordSize
GetUndoRecordPayLoadLen(UnpackedUndoRecord *urec)
{
	return urec->uur_payloadlen;
}

static inline void
SetUndoRecordXid(UnpackedUndoRecord *urec, FullTransactionId xid)
{
	urec->uur_xid = xid;
}

static inline void
SetUndoRecordCid(UnpackedUndoRecord *urec, CommandId cid)
{
	urec->uur_cid = cid;
}
static inline void
SetUndoRecordReloid(UnpackedUndoRecord *urec, Oid reloid)
{
	urec->uur_reloid = reloid;
}
static inline void
SetUndoRecordRelfilenode(UnpackedUndoRecord *urec, Oid relfilenode)
{
	urec->uur_relfilenode = relfilenode;
}

static inline void
SetUndoRecordSubXid(UnpackedUndoRecord *urec, FullTransactionId subxid)
{
	urec->uur_subxid = subxid;
}

static inline void
SetUndoRecordTablespace(UnpackedUndoRecord *urec, Oid tablespace)
{
	urec->uur_tablespace = tablespace;
}

static inline void
SetUndoRecordUtype(UnpackedUndoRecord *urec, uint8 utype)
{
	urec->uur_type = utype;
}
static inline void
SetUndoRecordUinfo(UnpackedUndoRecord *urec, uint8 uinfo)
{
	urec->uur_info |= uinfo;
}
static inline void
SetUndoRecordTpprev(UnpackedUndoRecord *urec, UndoRecPtr tpprev)
{
	urec->uur_tpprevurp = tpprev;
}
static inline void
SetUndoRecordBlkno(UnpackedUndoRecord *urec, BlockNumber blk)
{
	urec->uur_blkno = blk;
}
static inline void
SetUndoRecordOffset(UnpackedUndoRecord *urec, OffsetNumber offset)
{
	urec->uur_offset = offset;
}

static inline void
SetUndoRecordPrevurp(UnpackedUndoRecord *urec, UndoRecPtr prevurp)
{
	urec->uur_txnprevurp = prevurp;
}

static inline void
SetUndoRecordPayLoadLen(UnpackedUndoRecord *urec, UndoRecordSize len)
{
	urec->uur_payloadlen = len;
}

static inline bool
UndoRecordHasSubXact(UnpackedUndoRecord *urec)
{
	if ((urec->uur_info & UREC_INFO_SUBXACT) != 0)
	{
		return true;
	}
	return false;
}

static inline void
SetUndoRecordOldXactId(UnpackedUndoRecord *urec, FullTransactionId xid)
{
	urec->uur_prevxid = xid;
}

static inline void
SetUndoRecordDataHeaderFlag(UnpackedUndoRecord *urec, uint16 flag)
{
	urec->uur_dataheaderflag = flag;
}

/* interfaces for undo record*/
UnpackedUndoRecord *new_undo_record(void);
void destroy_undo_record(UnpackedUndoRecord *urec);

// Reset befor reuse it.
void reset_undo_record(UnpackedUndoRecord *urec, UndoRecPtr urp);
void reset_undo_record2(UnpackedUndoRecord *urec);

UndoRecordSize unpack_undo_record_size(UnpackedUndoRecord *urec);
UndoRecordSize undo_record_expected_size(UnpackedUndoRecord *urec);


bool insert_undo_record(UnpackedUndoRecord *urec, Page page, 
						int starting_byte, int *already_written, 
						int remaining_bytes, UndoRecordSize undo_len);



void undo_get_one_record(UnpackedUndoRecord *urec, bool copy_data);					



#endif
