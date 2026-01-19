/* -------------------------------------------------------------------------
 *
 * undotype.h
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * include/undo/undotype.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef UNDOTYPE_H
#define UNDOTYPE_H

#include "postgres.h"
#include "catalog/pg_class.h"
#include "catalog/pg_tablespace.h"
#include "storage/bufpage.h"
#include "access/xact.h"

#define INVALID_DB_OID (0)
#define UNDO_DATA_DB_OID (9)
#define UNDO_TXN_DB_OID (10)

/* The type used for undo record lengths. */
typedef uint16 UndoRecordSize;

/* Type for offsets within undo logs */
typedef uint64 UndoLogOffset;

/* Type for offsets within undo transaction slot */
typedef uint64 UndoSlotOffset;

typedef uint64 UndoSlotPtr;


/* The maximum number of undo logs. 2^18*/
#define UNDOLOG_TOTAL_COUNT (256 * 1024)   

#define PAGES_READ_NUM (1024 * 16)

#define INVALID_UNDOLOG_NO -1

#define UNDOLOG_DAT_FILE_MAXSIZE (1024 * 1024)   /* 1MB  128 page*/
#define UNDOLOG_TXN_FILE_MAXSIZE (32 * 1024)      /* 32KB  4 page*/

#define UNDOLOG_FILE_DIR_LEN 15
#define UNDOLOG_FILE_PATH_LEN 64

/* Number of blocks of BLCKSZ in an undo dat segment file. */
#define UNDOLOG_DAT_FILE_BLOCKS (UNDOLOG_DAT_FILE_MAXSIZE / BLCKSZ)

/* Number of blocks of BLCKSZ in an undo txn segment file. */
#define UNDOLOG_TXN_FILE_BLOCKS (UNDOLOG_TXN_FILE_MAXSIZE / BLCKSZ)

#define UNDOLOG_FILE_SIZE(dbOid) (dbOid == UNDO_DATA_DB_OID ? UNDOLOG_DAT_FILE_MAXSIZE : UNDOLOG_TXN_FILE_MAXSIZE)

#define UNDOLOG_FILE_BLOCKS(dbOid) (dbOid == UNDO_DATA_DB_OID ? UNDOLOG_DAT_FILE_BLOCKS : UNDOLOG_TXN_FILE_BLOCKS)	

/* up to 64TB per log*/
#define UNDO_LOG_MAX_SIZE                                                             \
	((undo_max_segno_per_log >= 1) ? (UndoLogOffset) undo_max_segno_per_log * UNDOLOG_DAT_FILE_MAXSIZE \
									: (UndoLogOffset) 1L << 46)

/* Special value for undo record pointer which indicates that it is invalid. */
#define INVALID_UNDO_REC_PTR ((UndoRecPtr) 0)

/* Special value for undo record pointer which indicates that it is invalid. */
#define INVALID_UNDO_SLOT_PTR ((UndoSlotPtr) 0)

#define IS_VALID_UNDO_REC_PTR(urecptr) \
	((bool) ((UndoRecPtr) (urecptr) != INVALID_UNDO_REC_PTR))


/* The width of an undo log number in bits.  */
#define UNDO_LOG_NUMBER_BITS 18

/* The width of an undo log offset in bits.  */
#define UNDO_LOG_OFFSET_BITS (64 - UNDO_LOG_NUMBER_BITS)

/* Extract the undo log number from an Undo Ptr. */
#define UNDO_PTR_GET_LOG_NO(urp) ((urp) >> UNDO_LOG_OFFSET_BITS)

/* Make an UndoRecPtr from an log number and offset. */
#define MAKE_UNDO_REC_PTR(logno, offset) (((uint64) (logno) << UNDO_LOG_OFFSET_BITS) | (offset))

/* Extract the offset from an UndoRecPtr. */
#define UNDO_PTR_GET_OFFSET(urp) ((urp) & ((UINT64CONST(1) << UNDO_LOG_OFFSET_BITS) - 1))

/* Compute the offset of a given UndoRecPtr in the page that holds it. */
#define UNDO_PTR_GET_PAGE_OFFSET(urp) (UNDO_PTR_GET_OFFSET(urp) % BLCKSZ)

/* The number of unusable bytes in the header of each block. */
#define UNDO_LOG_BLOCK_HEADER_SIZE SizeOfPageHeaderData

/* The number of usable bytes we can store per block. */
#define UNDO_LOG_USABLE_BYTES_PER_PAGE (BLCKSZ - UNDO_LOG_BLOCK_HEADER_SIZE)

/* How many non-header bytes are there before a given offset? */
#define UNDO_LOG_OFFSET_TO_USABLE_BYTE_NO(offset)       \
	(((offset) % BLCKSZ - UNDO_LOG_BLOCK_HEADER_SIZE) + \
	 ((offset) / BLCKSZ) * UNDO_LOG_USABLE_BYTES_PER_PAGE)

/* What is the offset of the i'th non-header byte? */
#define UNDO_LOG_OFFSET_FROM_USABLE_BYTE_NO(i)                                      \
	(((i) / UNDO_LOG_USABLE_BYTES_PER_PAGE) * BLCKSZ + UNDO_LOG_BLOCK_HEADER_SIZE + \
	 ((i) % UNDO_LOG_USABLE_BYTES_PER_PAGE))

/* Add 'n' usable bytes to offset stepping over headers to find new offset. */
#define UNDO_LOG_OFFSET_PLUS_USABLE_BYTES(offset, n) \
	UNDO_LOG_OFFSET_FROM_USABLE_BYTE_NO(UNDO_LOG_OFFSET_TO_USABLE_BYTE_NO(offset) + (n))

/* Compute the block number that holds a given UndoRecPtr. */
#define UNDO_PTR_GET_BLOCK_NUM(urp) (UNDO_PTR_GET_OFFSET(urp) / BLCKSZ)

/* Extract the relnode for an undo log. */
#define UNDO_PTR_GET_REL_NUMBER(urp) (UNDO_PTR_GET_LOG_NO(urp) +1)

#define LOGNO_TO_REL_NUMBER(logno) (logno+1)

#define REL_NUMBER_TO_LOGNO(relNumber) (relNumber-1)

#define IS_VALID_LOGNO(logno) ((logno) >= 0 && (logno) < UNDOLOG_TOTAL_COUNT)

/* The location of undo meta persistence . */
#define UNDO_META_FILE "undo/undolog.meta"
#define UNDO_META_PAGE_SIZE 512
#define UNDO_WRITE_SIZE (UNDO_META_PAGE_SIZE * 8)
#define UNDO_META_PAGE_CRC_LENGTH 4

#define UNDOLOG_COUNT_PER_PAGE \
	((UNDO_META_PAGE_SIZE - UNDO_META_PAGE_CRC_LENGTH) / sizeof(UndoLogMeta))
#define UNDOLOG_COUNT_PER_WRITE (UNDOLOG_COUNT_PER_PAGE * 8)


#define UNDO_LOG_ATTACHED  1
#define UNDO_LOG_DETACHED  0

/*
 * Undo log persistence levels.  These have a one-to-one correspondence with
 * relpersistence values, but are small integers so that we can use them as an
 * index into the "logs" and "lognos" arrays.
 */
typedef enum
{
	UNDO_PERMANENT = 0,
	UNDO_UNLOGGED = 1,
	UNDO_TEMP = 2
} UndoPersistence;

typedef enum
{
	UNDO_LOG_SEGMENT = 0,
	UNDO_SLOT_SEGMENT = 1	
} UndoSegmentType;


typedef enum
{
	UNDO_TRAVERSAL_DEFAULT = 0,
	UNDO_TRAVERSAL_COMPLETE,
	UNDO_TRAVERSAL_STOP,
	UNDO_TRAVERSAL_ABORT,
	UNDO_TRAVERSAL_END
} UndoTraversalState;

typedef enum
{
	UNDO_RECORD_NORMAL = 0,
	UNDO_RECORD_DISCARD,
	UNDO_RECORD_FORCE_DISCARD,
	UNDO_RECORD_NOT_INSERT,
	UNDO_RECORD_INVALID
} UndoRecordState;

typedef enum
{
	UNDO_UNKNOWN = 0,
	UNDO_INSERT,
	UNDO_MULTI_INSERT,
	UNDO_DELETE,
	UNDO_INPLACE_UPDATE,
	UNDO_UPDATE,
	UNDO_XBTREE_INSERT,
	UNDO_XBTREE_DELETE,
	UNDO_ITEMID_UNUSED
} UndoRecType;


#define UNDO_PERSISTENCE_STR(upersistence)    \
	((upersistence) == UNDO_PERMANENT  ? "p"  \
	 : (upersistence) == UNDO_UNLOGGED ? "u"  \
									   : "t")


#define UndoPersistenceForRelPersistence(rp)            \
	((rp) == RELPERSISTENCE_PERMANENT  ? UNDO_PERMANENT \
	 : (rp) == RELPERSISTENCE_UNLOGGED ? UNDO_UNLOGGED  \
									   : UNDO_TEMP)

/*
 * Get the appropriate UndoPersistence value from a Relation.
 */
#define UndoPersistenceForRelation(rel) \
	(UndoPersistenceForRelPersistence((rel)->rd_rel->relpersistence))

#define UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rfn, urp, dbId) \
	do                                                    \
	{                                                     \
		(rfn).spcOid = DEFAULTTABLESPACE_OID;             \
		(rfn).dbOid = dbId;                               \
		(rfn).relNumber = UNDO_PTR_GET_REL_NUMBER(urp);   \
	} while (false);


#define PERSIST_UNDOLOG_COUNT (UNDOLOG_TOTAL_COUNT / 2)
#define UNLOGED_UNDOLOG_COUNT (UNDOLOG_TOTAL_COUNT / 4)

#define IS_PERSIST_LEVEL(logno) (logno < PERSIST_UNDOLOG_COUNT)

#define GET_UPERSISTENCE_BY_LOGNO(up,logno)             \
	if (logno < (int) PERSIST_UNDOLOG_COUNT)            \
	{                                                   \
		up = UNDO_PERMANENT;                            \
	}                                                   \
	else if (logno < (int) PERSIST_UNDOLOG_COUNT + UNLOGED_UNDOLOG_COUNT ) \
	{                                                   \
		up = UNDO_UNLOGGED;                             \
	}                                                   \
	else                                                \
	{                                                   \
		up = UNDO_TEMP;                                 \
	}

#define GET_START_LOGNO_BY_UPERSISTENCE(up,logno)                  \
	if (up == UNDO_PERMANENT)                      \
	{                                              \
		logno = 0;                                 \
	}                                              \
	else if ( up == UNDO_UNLOGGED )                \
	{                                              \
		logno = (int) PERSIST_UNDOLOG_COUNT;       \
	}                                              \
	else                                           \
	{                                              \
		logno = (int) PERSIST_UNDOLOG_COUNT + UNLOGED_UNDOLOG_COUNT; \
	}

#define UNDO_PREPARE_FAIL -1
#define UNDO_PREPARE_SUCC 0


#define UREC_XOR_PREFIX 0x01
#define UREC_XOR_SUFFIX 0x02


#define XSTORE_UNDO_VERSION 1

typedef struct UndoSegmentMeta
{
	uint32	   version;
	XLogRecPtr lsn;
	UndoRecPtr head;
	UndoRecPtr tail;
} UndoSegmentMeta;

#define UNDOSEGMENT_COUNT_PER_PAGE \
	((UNDO_META_PAGE_SIZE - UNDO_META_PAGE_CRC_LENGTH) / sizeof(UndoSegmentMeta))

#define UNDOSEGMENT_COUNT_PER_WRITE (UNDOSEGMENT_COUNT_PER_PAGE * 8)

#define UNDOSEGMENT_META_PAGE_COUNT(total, unit, count)                    \
	do                                                                     \
	{                                                                      \
		count = (total % unit == 0) ? (total / unit) : (total / unit) + 1; \
	} while (0)

typedef struct UndoLogMeta
{
	uint32		   version;
	XLogRecPtr	   lsn;
	UndoSlotOffset allocate_slot_offset;
	UndoSlotOffset recycle_slot_offset;
	UndoRecPtr	   insert_rec_ptr;
	UndoRecPtr	   discard_rec_ptr;
	UndoRecPtr	   forece_discard_rec_ptr;
	FullTransactionId  recycle_xid;
} UndoLogMeta;


#define UNDO_LOG_META_PAGE_COUNT(total, unit, count)                       \
	do                                                                     \
	{                                                                      \
		count = (total % unit == 0) ? (total / unit) : (total / unit) + 1; \
	} while (0)


#define UNDOSLOT_INIT 0x00
#define UNDOSLOT_ROLLBACK 0x01

#define UNDOSLOT_BUFFER_INIT 0x00
#define UNDOSLOT_BUFFER_LOAD 0x01

typedef struct UndoSlot
{
	FullTransactionId	xid;
	volatile UndoRecPtr start_undo_ptr;
	UndoRecPtr			end_undo_ptr;
	uint32				info : 8;
	uint32				pad : 24;
	Oid					dbOid;
} UndoSlot;

typedef struct UndoSlotBuffer
{
	BlockNumber blkno;
	Buffer		buffer;
	uint8		info;
	bool		zero;
} UndoSlotBuffer;

#endif	// UNDOTYPE_H