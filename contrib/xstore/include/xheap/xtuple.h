/* -------------------------------------------------------------------------
 *
 * xtuple.h
 * the row format of inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/xheap/xtuple.h
 * -------------------------------------------------------------------------
 *
 */

#ifndef XTUPLE_H
#define XTUPLE_H

#include "postgres.h"

#include "c.h"
#include "access/transam.h"
#include "access/tupdesc.h"
#include "access/rmgr.h"
#include "nodes/bitmapset.h"
#include "storage/buf.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"
#include "undo/undorecord.h"
#include "xheap/xrel.h"

extern bool enable_reserve_space_for_null_atts;

/* 0-3 bits for tuple storage */
#define XHEAP_HAS_NULL 0x0001
#define XHEAP_HASVARWIDTH 0x0002 /* has variable-width attribute(s) */
#define XHEAP_HASEXTERNAL 0x0004 /* has external stored attribute(s) */

#define XHeapTupleHasExternal(tuple) (((tuple)->disk_tuple->flag & XHEAP_HASEXTERNAL) != 0)
#define XHeapDiskTupHasNulls(_xdisk_tuple) (((_xdisk_tuple)->flag & XHEAP_HAS_NULL) != 0)
#define XHeapDiskTupNoNulls(_xdisk_tuple) (((_xdisk_tuple)->flag & XHEAP_HAS_NULL) == 0)
#define XHeapDiskTupSetHasNulls(_xdisk_tuple) ((_xdisk_tuple)->flag |= XHEAP_HAS_NULL)
#define XHeapDiskTupHasVarWidth(_xdisk_tuple) (((_xdisk_tuple)->flag & XHEAP_HASVARWIDTH) != 0)

/* 4-7 bits for tuple status */
#define XHEAP_DELETED 0x0010			 /* tuple deleted */
#define XHEAP_INPLACE_UPDATED 0x0020	 /* tuple is updated inplace */
#define XHEAP_UPDATED 0x0040			 /* tuple is not updated inplace */

#define XHEAP_MOVED (XHEAP_DELETED | XHEAP_UPDATED) /* moved tuple to another partition */
#define XHeapTupleIsInPlaceUpdated(infomask) ((infomask & XHEAP_INPLACE_UPDATED) != 0)
#define XHeapTupleIsUpdated(infomask) ((infomask & XHEAP_UPDATED) != 0)
#define XHeapTupleIsMoved(infomask) ((infomask & XHEAP_MOVED) == XHEAP_MOVED)
#define XHeapDiskTupleDeleted(_xdisk_tuple) \
	(((_xdisk_tuple)->flag & (XHEAP_DELETED | XHEAP_UPDATED)) != 0)
#define XHeapTupleHeaderSetMovedPartitions(xdisk_tuple) \
	((xdisk_tuple)->flag |= XHEAP_MOVED)

/* 8-11 bits for tuple lock */
#define XHEAP_XID_KEYSHR_LOCK 0x0100	 /* xid is a key-shared locker */
#define XHEAP_XID_NOKEY_EXCL_LOCK 0x0200 /* xid is a nokey-exclusive locker */

/* xid is a shared locker */
#define XHEAP_XID_SHR_LOCK (XHEAP_XID_NOKEY_EXCL_LOCK | XHEAP_XID_KEYSHR_LOCK)

#define XHEAP_XID_EXCL_LOCK 0x0400 /* tuple was updated and key cols modified, or tuple deleted */
#define XHEAP_MULTI_LOCKERS 0x0800 /* tuple was locked by multiple lockers */

#define XHEAP_SINGLE_LOCK_MASK (XHEAP_XID_SHR_LOCK | XHEAP_XID_EXCL_LOCK)
#define XHEAP_LOCK_STATUS_MASK                                                 \
	(XHEAP_XID_KEYSHR_LOCK | XHEAP_XID_NOKEY_EXCL_LOCK | XHEAP_XID_EXCL_LOCK | \
	 XHEAP_MULTI_LOCKERS)

/*
 * Use these to test whether a particular lock is applied to a tuple
 */
#define XHEAP_XID_IS_SHR_LOCKED(infomask) \
	((infomask & XHEAP_SINGLE_LOCK_MASK) == XHEAP_XID_SHR_LOCK)
#define XHEAP_XID_IS_EXCL_LOCKED(infomask) \
	((infomask & XHEAP_SINGLE_LOCK_MASK) == XHEAP_XID_EXCL_LOCK)
#define XHEAP_XID_IS_KEYSHR_LOCKED(infomask) \
	(((infomask) &XHEAP_SINGLE_LOCK_MASK) == XHEAP_XID_KEYSHR_LOCK)
#define XHEAP_XID_IS_NOKEY_EXCL_LOCKED(infomask) \
	(((infomask) &XHEAP_SINGLE_LOCK_MASK) == XHEAP_XID_NOKEY_EXCL_LOCK)

#define XHeapTupleHasMultiLockers(infomask) (((infomask) &XHEAP_MULTI_LOCKERS) != 0)

#define XHeapTupleHeaderClearSingleLocker(xtuple)                                      \
	do                                                                                 \
	{                                                                                  \
		Assert(!XHeapTupleHasMultiLockers((xtuple)->flag)); 						   \
		(xtuple)->flag &= ~(XHEAP_SINGLE_LOCK_MASK); \
		(xtuple)->locker_xid = InvalidFullTransactionId; 							   \
	} while (0)

#define XHeapTupleHeaderClearAllLocker(xtuple)                                      \
	do                                                                                 \
	{                                                                                  \
		(xtuple)->flag &= ~XHEAP_LOCK_STATUS_MASK; 								   \
		(xtuple)->locker_xid = InvalidFullTransactionId; 							   \
	} while (0)

/* 12-15 bits for tuple transaction infomation */
#define XHEAP_MODIFIED_XID_COMMITTED 0x1000
#define XHEAP_MODIFIED_XID_INVALID 0x2000
#define XHEAP_XID_STATUS_MASK (XHEAP_MODIFIED_XID_COMMITTED | XHEAP_MODIFIED_XID_INVALID)
#define XHEAP_MODIFIED_XID_IS_COMMITTED(infomask) \
	((infomask & XHEAP_XID_STATUS_MASK) == XHEAP_MODIFIED_XID_COMMITTED)
#define XHEAP_MODIFIED_XID_IS_INVALID(infomask) \
	((infomask & XHEAP_XID_STATUS_MASK) == XHEAP_MODIFIED_XID_INVALID)

#define XHEAP_VIS_STATUS_MASK 0xFFF0 /* mask for visibility bits (4 ~ 15 bits) */
#define XHEAP_VIS_STATUS_UNLOCK_MASK 0xF0F0 /* mask for visibility bits (4 ~ 15 bits) except lock bits */

/* Information stored in flag2 */
#define XHEAP_NATTS_MASK 0x07FF /* 11 bits for number of attributes */

#define XHeapTupleHeaderGetNatts(tup) (((tup)->flag2 & XHEAP_NATTS_MASK))

#define XHeapTupleHeaderSetNatts(tup, natts) \
	((tup)->flag2 = ((tup)->flag2 & ~XHEAP_NATTS_MASK) | (natts))

#define XHeapTupleHeaderSetUndoRecPtr(xdisk_tuple, urecPtr) \
	((xdisk_tuple)->urec = urecPtr)

typedef struct RowPtr
{
	unsigned offset : 15, /* offset to row (from start of page) */
		flags : 2,		  /* state of row pointer */
		len : 15;		  /* byte length of row */
} RowPtr;

typedef struct XHeapDiskTupleData
{
	FullTransactionId modified_xid;
	FullTransactionId locker_xid;
	UndoRecPtr urec; 
	uint16 flag;					/* Flag for tuple attributes */
	uint16 flag2;					/* Number of attributes for now(11 bits) */
	uint8  t_hoff;					/* Sizeof header incl. bitmap, padding */
	uint8 data[FLEXIBLE_ARRAY_MEMBER];
	/* Followed by isnull array and col data. */
} XHeapDiskTupleData;

typedef XHeapDiskTupleData *XHeapDiskTuple;

#define SizeOfXHeapDiskTupleData (offsetof(XHeapDiskTupleData, data))
#define SizeOfXHeapDiskTupleTillThoff (offsetof(XHeapDiskTupleData, t_hoff))
#define OffsetDataHeader (sizeof(FullTransactionId) * 2 + sizeof(UndoRecPtr))
#define SizeOfXHeapDiskTupleHeaderExceptXid (SizeOfXHeapDiskTupleData - OffsetDataHeader)

static inline bool
NAttrsReserveSpace(int nattrs)
{
	return ((nattrs) <= (int) ((255 - SizeOfXHeapDiskTupleData) * 8 / 2));
}

typedef struct XHeapTupleData
{
	uint32 disk_tuple_size;
	ItemPointerData ctid;
	Oid				table_oid;
	TransactionId xmin;
	TransactionId xmax;
	XHeapDiskTupleData *disk_tuple;
} XHeapTupleData;

typedef void *TupData;

typedef XHeapTupleData *XHeapTuple;


XHeapTuple xheaptup_alloc(Size size);

/*
 * Possible lock modes for a tuple.
 */
typedef enum LockOper
{
	/* SELECT FOR 'KEY SHARE/SHARE/NO KEY UPDATE/UPDATE' */
	LockOnly,
	/* Via EvalPlanQual where after locking we will update it */
	LockForUpdate,
	/* Update/Delete */
	ForUpdate
} LockOper;

#define XHEAP_SPECIAL_SIZE (0)
#define XHeapDiskTupleDataHeaderSize (offsetof(XHeapDiskTupleData, data))
#define XHeapTupleDataSize (sizeof(XHeapTupleData))
#define MINXHeapTupleSize (MAXALIGN(XHeapDiskTupleDataHeaderSize))
#define DefaultTdMaxXHeapTupleSize                                                 \
	(BLCKSZ - MAXALIGN(SizeOfXHeapPageHeaderData + \
					   sizeof(ItemIdData) + XHEAP_SPECIAL_SIZE))
#define MaxPossibleXHeapTupleSize                                              \
	(BLCKSZ - MAXALIGN(SizeOfXHeapPageHeaderData + \
					   sizeof(ItemIdData) + XHEAP_SPECIAL_SIZE))
#define MaxXHeapTupleSize(relation)                                            \
	(BLCKSZ - MAXALIGN(SizeOfXHeapPageHeaderData + \
					   sizeof(ItemIdData) + XHEAP_SPECIAL_SIZE))
#define XHeapTupleIsValid(tuple) PointerIsValid(tuple)
#define xheap_getattr(tup, attnum, tupleDesc, isnull)                         \
	(((attnum) > 0)                                                           \
		 ? (((attnum) > (int) XHeapTupleHeaderGetNatts((tup)->disk_tuple))    \
				? (                                                           \
                                                                              \
					  heapGetInitDefVal((attnum), (tupleDesc), (isnull)))     \
				: (XHeapFastGetAttr((tup), (attnum), (tupleDesc), (isnull)))) \
		 : XHeapGetSysAttr((tup), (InvalidBuffer), (attnum), (tupleDesc), (isnull)))


#define XHeapTupleGetModifiedXid(tup) ((tup)->disk_tuple->modified_xid)

#define XHeapTupleGetLockerXid(tup) ((tup)->disk_tuple->locker_xid)

#define PtrGetVal(ptr, val) (((ptr) == NULL) ? 0 : ((ptr)->val))

#define PtrFuncVal(ptr, func) (((ptr) == NULL) ? 0 : (func(ptr)))

#define LEN_VARLENA (-1)
#define LEN_CSTRING (-2)
#define ATTNUM_BMP_SHIFT (3)

#define XHeapFreeTuple(xhtup) \
	do                        \
	{                         \
		if ((xhtup) != NULL)  \
		{                     \
			pfree(xhtup);     \
			xhtup = NULL;     \
		}                     \
	} while (0)

#endif
