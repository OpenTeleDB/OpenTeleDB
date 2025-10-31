/* -------------------------------------------------------------------------
 *
 * xpage.h
 * the row format of inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California	
 *
 *
 * IDENTIFICATION
 * include/xheap/xpage.h
 * -------------------------------------------------------------------------
 *
 */

#ifndef XPAGE_H
#define XPAGE_H

#include "postgres.h"

#include "storage/bufpage.h"
#include "access/genam.h"
#include "xheap/xrel.h"
#include "xheap/xtuple.h"
#include "utils/rel.h"

#define FORCE_EXTEND_THRESHOLD 3
#define DML_MAX_RETRY_TIMES 100000

#define XHEAP_HAS_FREE_LINES 0x0001	 /* are there any unused line pointers? */
#define XHEAP_PAGE_FULL \
	0x0002							 /* not enough free space for new \
                                    * tuple? */
#define XHP_ALL_VISIBLE \
	0x0004							 /* all tuples on page are visible to \
                                    * everyone */
#define XHEAP_VALID_FLAG_BITS 0xFFFF /* OR of all valid flag bits */


#define XHeapPageGetLSN(page) (PageXLogRecPtrGet(((XHeapPageHeader) (page))->pd_lsn))
#define XHeapPageGetPageSize(page) \
	(Size)(((XHeapPageHeader) (page))->pd_pagesize_version & (uint16) 0xFF00)
#define XHeapPageGetPageLayoutVersion(page) \
	(((XHeapPageHeader) (page))->pd_pagesize_version & 0x00FF)


#define XPageHasFreeLinePointers(_page) \
	(((XHeapPageHeaderData *) (_page))->pd_flags & XHEAP_HAS_FREE_LINES)
#define XPageSetHasFreeLinePointers(_page) \
	(((XHeapPageHeaderData *) (_page))->pd_flags |= XHEAP_HAS_FREE_LINES)
#define XPageClearHasFreeLinePointers(_page) \
	(((XHeapPageHeaderData *) (_page))->pd_flags &= ~XHEAP_HAS_FREE_LINES)

#define XPageIsFull(_page) (((XHeapPageHeaderData *) (_page))->pd_flags & XHEAP_PAGE_FULL)
#define XPageSetFull(_page) \
	(((XHeapPageHeaderData *) (_page))->pd_flags |= XHEAP_PAGE_FULL)
#define XPageClearFull(_page) \
	(((XHeapPageHeaderData *) (_page))->pd_flags &= ~XHEAP_PAGE_FULL)

#define SizeOfXHeapPageHeaderData (sizeof(XHeapPageHeaderData))

#define XPageGetRowPtrOffset (SizeOfXHeapPageHeaderData)

#define XPageGetRowPtr(_xpage, _offsetNumber)                         \
	XPageGenerateRowPtr(_xpage, _offsetNumber)

#define XPageGenerateRowPtr(_xpage, _offsetNumber)                   \
	((RowPtr *) (((char *) _xpage) + SizeOfXHeapPageHeaderData +     \
				 ((_offsetNumber - 1) * sizeof(RowPtr))))

#define SetNormalRowPointer(_rowptr, _off, _size) \
	((_rowptr)->flags = RP_NORMAL, (_rowptr)->offset = (_off), (_rowptr)->len = (_size))

#define XPageGetRowData(_xpage, _rowptr)                                       \
	(AssertMacro(_xpage), AssertMacro(RowPtrHasStorage(_rowptr)), \
	 (Item) (((char *) (_xpage)) + RowPtrGetOffset(_rowptr)))

#define XPageIsPrunable(_page)                                                \
	(FullTransactionIdIsValid(((XHeapPageHeaderData *) (_page))->pd_prune_xid) && \
	 !TransactionIdIsInProgress(XidFromFullTransactionId(((XHeapPageHeaderData *) (_page))->pd_prune_xid)))

#define XPageIsPrunableWithOldestXmin(_page, _oldestxmin)                      \
	(AssertMacro(FullTransactionIdIsNormal(_oldestxmin)),                           \
	 FullTransactionIdIsValid(((XHeapPageHeaderData *) (_page))->pd_prune_xid) &&   \
		 FullTransactionIdPrecedes(((XHeapPageHeaderData *) (_page))->pd_prune_xid, \
							   _oldestxmin))


#define XPageSetPrunable(_page, _xid)                                                 \
	do                                                                                \
	{                                                                                 \
		Assert(FullTransactionIdIsNormal(_xid));                                          \
		if (!TransactionIdIsValid(((XHeapPageHeaderData *) (_page))->pd_prune_xid.value) || \
			FullTransactionIdPrecedes(_xid,                                               \
								  ((XHeapPageHeaderData *) (_page))->pd_prune_xid))   \
			((XHeapPageHeaderData *) (_page))->pd_prune_xid = (_xid);                 \
	} while (0)

#define XPageClearPrunable(_page) \
	(((XHeapPageHeaderData *) (_page))->pd_prune_xid = InvalidTransactionId)

#define LimitRetryTimes(retryTimes)                                      \
	do                                                                   \
	{                                                                    \
		if ((retryTimes) > DML_MAX_RETRY_TIMES)                          \
		{                                                                \
			elog(ERROR, "Transaction aborted due to too many retries."); \
		}                                                                \
	} while (0)

/*
 * RowPtr "flags" has these possible states.  An UNUSED row pointer is available
 * for immediate re-use, the other states are not.
 */
#define RP_UNUSED 0	  /* unused (should always have len=0) */
#define RP_NORMAL 1	  /* used (should always have len>0) */
#define RP_REDIRECT 2 /* HOT redirect (should have len=0) */
#define RP_DEAD 3	  /* dead, may or may not have storage */

/*
 * Flags used in XHeap.  These flags are used in a row pointer of a deleted
 * row that has no actual storage.  These help in fetching the tuple from
 * undo when required.
 */
#define ROWPTR_DELETED 0x0001	   /* Row is deleted */
#define ROWPTR_XACT_INVALID 0x0002 /* TD slot on tuple got reused */
#define VISIBILTY_MASK 0x007F	   /* 7 bits (1..7) for visibility mask */
#define XACT_SLOT \
	0x7F80						   /* 8 bits (8..15) of offset for transaction \
                                    * slot */
#define XACT_SLOT_MASK 0x0007	   /* 7 - mask to retrieve transaction slot */

/*
 * RowPtrIsUsed
 * True iff row pointer is in use.
 */
#define RowPtrIsUsed(_rowptr) ((_rowptr)->flags != RP_UNUSED)

#define RowPtrHasStorage(_rowptr) ((_rowptr)->len > 0)

#define RowPtrIsNormal(_rowptr) ((_rowptr)->flags == RP_NORMAL)

#define RowPtrGetFlags(_rowptr) ((_rowptr)->flags)

#define RowPtrGetLen(_rowptr) ((_rowptr)->len)

#define RowPtrGetOffset(_rowptr) ((_rowptr)->offset)
/*
 * RowPtrSetUnused
 * Set the row pointer to be UNUSED, with no storage.
 * Beware of multiple evaluations of itemId!
 */
#define RowPtrSetUnused(_rowptr) \
	((_rowptr)->flags = RP_UNUSED, (_rowptr)->offset = 0, (_rowptr)->len = 0)

/*
 * ItemIdChangeLen
 * Change the length of itemid.
 */
#define RowPtrChangeLen(_rowptr, _length)                                 \
	do                                                                    \
	{                                                                     \
		if (RowPtrGetOffset(_rowptr) + _length > BLCKSZ)                  \
		{                                                                 \
			elog(PANIC, "row pointer error, offset:%u, flags:%u, len:%u", \
				 RowPtrGetOffset(_rowptr), (_rowptr)->flags, (_length));  \
		}                                                                 \
		(_rowptr)->len = (_length);                                       \
	} while (0)


#define RowPtrGetVisibilityInfo(_rowptr) ((_rowptr)->offset & VISIBILTY_MASK)

#define RowPtrSetInvalidXact(_rowptr) \
	((_rowptr)->offset = ((_rowptr)->offset & ~VISIBILTY_MASK) | ROWPTR_XACT_INVALID)

#define RowPtrResetInvalidXact(_rowptr) \
	((_rowptr)->offset = ((_rowptr)->offset & ~VISIBILTY_MASK) & ~(ROWPTR_XACT_INVALID))

/*
 * MaxXHeapTupFixedSize - Fixed size for tuple, this is computed based
 * on data alignment.
 */
#define MaxXHeapTupFixedSize (SizeOfXHeapDiskTupleData + sizeof(ItemIdData))

/* MaxXHeapPageFixedSpace - Maximum fixed size for page */
#define MaxXHeapPageFixedSpace(relation)                                               \
	(BLCKSZ - SizeOfXHeapPageHeaderData - XHEAP_SPECIAL_SIZE)

#define MaxPossibleXHeapPageFixedSpace \
	(BLCKSZ - SizeOfXHeapPageHeaderData - XHEAP_SPECIAL_SIZE)

#define MaxXHeapToastPageFixedSpace                                                     \
	(BLCKSZ - SizeOfXHeapPageHeaderData - \
	 XHEAP_SPECIAL_SIZE)

/*
 * MaxXHeapTuplesPerPage is an upper bound on the number of tuples that can
 * fit on one xheap page.
 */
#define MaxXHeapTuplesPerPage(relation) \
	((int) ((MaxXHeapPageFixedSpace(relation)) / (MaxXHeapTupFixedSize)))

/*
 * Only used for bitmap scan.
 * MaxXHeapTuplesPerPageUpperBound > MaxXHeapTuplesPerPage
*/
#define MaxXHeapTuplesPerPageUpperBound                                 \
	((int) ((BLCKSZ - SizeOfXHeapPageHeaderData - XHEAP_SPECIAL_SIZE) / \
			(MaxXHeapTupFixedSize)))

#define MaxPossibleXHeapTuplesPerPage \
	((int) ((MaxPossibleXHeapPageFixedSpace) / (MaxXHeapTupFixedSize)))

#define CalculatedMaxXHeapTuplesPerPage                           \
	((int) ((BLCKSZ - SizeOfXHeapPageHeaderData - \
			 XHEAP_SPECIAL_SIZE) /                                         \
			(MaxXHeapTupFixedSize)))

#define XPageGetPruneXID(_page) ((XHeapPageHeaderData*) (_page))->pd_prune_xid


#define XHEAP_MAX_ATTR_PAD  3

typedef enum XPageType
{
	XPAGE_HEAP = 0,
	XPAGE_INDEX,
	XPAGE_TOAST
} XPageType;

typedef struct XHeapPageHeaderData
{
	PageXLogRecPtr pd_lsn;
	uint16		   pd_checksum;
	uint16		   pd_flags; /* Various page attribute flags e.g. free rowptrs */
	LocationIndex pd_lower;   /* Start of the free space between row pointers and row data */
	LocationIndex pd_upper;
	LocationIndex pd_special; /* Pointer to AM-specific per-page data */
	uint16 pd_pagesize_version; /* Page version identifier */
	FullTransactionId pd_prune_xid; /* oldest prunable XID, or zero if none */
	uint32		  reserved;
	uint16 potential_freespace; /* Potential space from deleted and updated tuples */
} XHeapPageHeaderData;

typedef XHeapPageHeaderData *XHeapPageHeader;

static inline bool
xpage_is_empty(XHeapPageHeaderData *phdr)
{
	uint16 start = (uint16) SizeOfXHeapPageHeaderData;
	return phdr->pd_lower <= start;
}


typedef struct XHeapFreeOffsetRanges
{
	OffsetNumber startOffset[MaxOffsetNumber];
	OffsetNumber endOffset[MaxOffsetNumber];
	int			 nranges;
} XHeapFreeOffsetRanges;

typedef struct XHeapBufferPage
{
	Buffer buffer;
	Page   page;
} XHeapBufferPage;

void xpage_init(XPageType, Page page, Size pageSize, Size specialSize);

OffsetNumber xpage_add_item(Relation relation, XHeapBufferPage *bufpage, Item item,
						  Size size, OffsetNumber offsetNumber, bool overwrite);
XHeapTuple	 xheap_get_tuple(Relation relation, Buffer buffer, OffsetNumber offnum,
						   XHeapTuple freebuf);
XHeapTuple	 xheap_get_tuple_partial(Relation relation, Buffer buffer, OffsetNumber offnum,
								  AttrNumber lastVar, bool *boolArr);
Size		 page_get_xheap_free_space(Page page);
Size		 page_get_exact_xheap_free_space(Page page);
extern XHeapFreeOffsetRanges *xheap_get_usable_offset_ranges(Buffer		 buffer,
														 XHeapTuple *tuples, int ntuples,
														 Size saveFreeSpace);

void						  xheap_record_potential_free_space(Buffer buffer, int delta);


static inline OffsetNumber
xheap_page_get_max_offset_number(char *xpage)
{
	OffsetNumber		 maxoff = InvalidOffsetNumber;
	XHeapPageHeaderData *xpghdr = (XHeapPageHeaderData *) xpage;

	if (xpghdr->pd_lower <= SizeOfXHeapPageHeaderData)
		maxoff = 0;
	else
		maxoff =
			(xpghdr->pd_lower - (SizeOfXHeapPageHeaderData )) /
			sizeof(RowPtr);

	return maxoff;
}

#endif	//XPAGE_H
