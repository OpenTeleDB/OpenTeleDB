/* -------------------------------------------------------------------------
 *
 * xpage.c
 * the page format of inplace update engine
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xpage.c
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "utils/rel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "xheap/xheap.h"
#include "xheap/xpage.h"
#include "util/xxact.h"
#include "xheap/xheapam_visibility.h"
#include "xheap/xtup_details.h"
#include "access/htup_details.h"
#include "storage/freespace.h"
#include "utils/timestamp.h"


#define ISNULL_BITMAP_NUMBER 2
#define HIGH_BITS_LENGTH_OF_LSN 32
#define PG_XHEAP_PAGE_LAYOUT_VERSION 7
void
xpage_init(XPageType page_type, Page page, Size page_size, Size special_size)
{
	char				*ptr = (char *) page;
	XHeapPageHeaderData *xheap_page;

	Assert(page_size == BLCKSZ);

	memset(ptr, 0, page_size);

	if (page_type == XPAGE_INDEX)
		return;


	Assert(page_size > (SizeOfXHeapPageHeaderData + special_size));

	PageInit(page, page_size, special_size);
	ptr += SizeOfXHeapPageHeaderData;
	xheap_page = (XHeapPageHeaderData *) page;
	xheap_page->pd_lower = SizeOfXHeapPageHeaderData;
	xheap_page->pd_upper = page_size - special_size;
	xheap_page->potential_freespace = xheap_page->pd_upper - xheap_page->pd_lower;
	PageSetPageSizeAndVersion(page, page_size, PG_XHEAP_PAGE_LAYOUT_VERSION);
}


static bool
check_offset_validity(Page page, OffsetNumber offset_number, bool *needshuffle,
					bool overwrite)
{
	RowPtr				*item_id = NULL;
	OffsetNumber		 limit;
	XHeapPageHeaderData *phdr = (XHeapPageHeaderData *) page;

	limit = OffsetNumberNext(xheap_page_get_max_offset_number(page));
	if (overwrite)
	{
		if (offset_number < limit)
		{
			item_id = XPageGetRowPtr(phdr, offset_number);

			if (InRecovery)
			{
				if (RowPtrHasStorage(item_id))
				{
					elog(WARNING, "will not overwrite a used ItemId");
					return false;
				}
			}
			else if (RowPtrIsUsed(item_id) || RowPtrHasStorage(item_id))
			{
				elog(WARNING, "will not overwrite a used ItemId");
				return false;
			}
		}
	}
	else
	{
		if (offset_number < limit)
			*needshuffle = true; /* need to move existing linp's */
	}

	return true;
}

static void
find_next_free_slot(const XHeapBufferPage *bufpage, Page input_page,
				 OffsetNumber *offset_number)
{
	RowPtr				*itemId = NULL;
	Page				 page = bufpage->page;
	XHeapPageHeaderData *uphdr = (XHeapPageHeaderData *) page;
	OffsetNumber		 limit;

	limit = OffsetNumberNext(xheap_page_get_max_offset_number(page));
	if (XPageHasFreeLinePointers(uphdr))
	{
		bool hasPendingXact = false;

		/*
		 * Look for "recyclable" (unused) ItemId.  We check for no storage
		 * as well, just to be paranoid --- unused items should never have
		 * storage.
		 */
		for (*offset_number = FirstOffsetNumber; *offset_number < limit; (*offset_number)++)
		{
			itemId = XPageGetRowPtr(uphdr, *offset_number);
			if (!RowPtrIsUsed(itemId) && !RowPtrHasStorage(itemId))
			{
				break;
			}
		}

		if (*offset_number >= limit && !hasPendingXact)
		{
			/* the hint is wrong, so reset it */
			XPageClearHasFreeLinePointers(uphdr);
		}
	}
	else
	{
		/* don't bother searching if hint says there's no free slot */
		*offset_number = limit;
	}
}

static bool
calculate_lower_upper_pointers(Page page, OffsetNumber offset_number, Item item, Size size,
							bool needshuffle)
{
	int					 lower;
	int					 upper;
	Size				 alignedSize;
	RowPtr				*itemId = NULL;
	OffsetNumber		 limit;
	XHeapPageHeaderData *xphdr = (XHeapPageHeaderData *) page;

	limit = OffsetNumberNext(xheap_page_get_max_offset_number(page));
	/* Reject placing items beyond the first unused line pointer */
	if (offset_number > limit)
	{
		elog(WARNING, "specified item offset is too large");
		return false;
	}

	/*
	 * Compute new lower and upper pointers for page, see if it'll fit.
	 *
	 * Note: do arithmetic as signed ints, to avoid mistakes if, say, size >
	 * pd_upper.
	 */
	if (offset_number == limit || needshuffle)
		lower = xphdr->pd_lower + sizeof(ItemIdData);
	else
		lower = xphdr->pd_lower;
	alignedSize = SHORTALIGN(size);

	upper = (int) xphdr->pd_upper - (int) alignedSize;

	if (lower > upper)
		return false;

	/*
	 * OK to insert the item.  First, shuffle the existing pointers if needed.
	 */
	itemId = XPageGenerateRowPtr(xphdr, offset_number);

	if (needshuffle)
		memmove(itemId + 1, itemId, (limit - offset_number) * sizeof(ItemIdData));

	/* set the item pointer */
	SetNormalRowPointer(itemId, upper, size);

	/* copy the item's data onto the page */
	memcpy((char *) page + upper, item, size);

	/* adjust page header */
	xphdr->pd_lower = (uint16) lower;
	xphdr->pd_upper = (uint16) upper;

	return true;
}

/*
 * XPageAddItem: this function is similar to PageAddItem, but we
 * removed the flag is_heap since XPageAddItem is only called on
 * a XHeap data page
 */
OffsetNumber
xpage_add_item(Relation rel, XHeapBufferPage *bufpage, Item item, Size size,
			 OffsetNumber offset_number, bool overwrite)
{
	Page				 page, input_page = NULL;
	bool				 needshuffle = false;
	bool				 offsetvalid = false;
	XHeapPageHeaderData *xphdr;

	/* Either one of buffer or page could be valid. */
	if (BufferIsValid(bufpage->buffer))
	{
		Assert(!bufpage->page);
		page = BufferGetPage(bufpage->buffer);
		bufpage->page = page;
	}
	else
	{
		Assert(bufpage->page);
		page = bufpage->page;
		input_page = bufpage->page;
	}

	Assert(page);

	xphdr = (XHeapPageHeaderData *) page;

	/*
	 * Be wary about corrupted page pointers
	 * Fixme: Add more sanity checks for page
	 */
	if (xphdr->pd_lower > xphdr->pd_upper)
		ereport(PANIC, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("corrupted page pointers: pd_lower = %u, pd_upper = %u",
							   xphdr->pd_lower, xphdr->pd_upper)));

	/* was offsetNumber passed in? */
	if (OffsetNumberIsValid(offset_number))
	{
		offsetvalid = check_offset_validity(page, offset_number, &needshuffle, overwrite);
		if (!offsetvalid)
			return InvalidOffsetNumber;
	}
	else
		find_next_free_slot(bufpage, input_page, &offset_number);

	/* Reject placing items beyond heap boundary, if heap */
	if (!InRecovery && offset_number > CalculatedMaxXHeapTuplesPerPage)
	{
		elog(WARNING, "can't put (%d)th item in a Xstore page with max %d items",
			 offset_number, CalculatedMaxXHeapTuplesPerPage);
		return InvalidOffsetNumber;
	}

	if (!calculate_lower_upper_pointers(page, offset_number, item, size, needshuffle))
		return InvalidOffsetNumber;

	return offset_number;
}

/*
 * XHeapGetTuple
 *
 * Copy a raw tuple from a xheap page, forming a XHeapTuple.
 */
XHeapTuple
xheap_get_tuple(Relation relation, Buffer buffer, OffsetNumber offnum, XHeapTuple freebuf)
{
	Page		   dp;
	RowPtr		  *rp;
	Size		   tupleLen;
	XHeapDiskTuple item;
	XHeapTuple	   tuple;

	dp = BufferGetPage(buffer);
	rp = XPageGetRowPtr(dp, offnum);

	Assert(offnum >= FirstOffsetNumber && offnum <= xheap_page_get_max_offset_number(dp));
	Assert(RowPtrIsNormal(rp));

	tupleLen = RowPtrGetLen(rp);

	if (freebuf)
	{
		Assert(freebuf->disk_tuple != NULL);
		tuple = freebuf;
	}
	else
	{
		tuple = (XHeapTuple) xheaptup_alloc(XHeapTupleDataSize + tupleLen);
		tuple->disk_tuple = (XHeapDiskTuple) ((char *) tuple + XHeapTupleDataSize);
	}

	tuple->table_oid = RelationGetRelid(relation);
	tuple->disk_tuple_size = tupleLen;
	ItemPointerSet(&tuple->ctid, BufferGetBlockNumber(buffer), offnum);
	item = (XHeapDiskTuple) XPageGetRowData(dp, rp);
	memcpy(tuple->disk_tuple, item, tupleLen);

	return tuple;
}

/*
 * XHeapGetTuplePartial
 *
 * Copy part of a raw tuple from a xheap page, forming a XHeapTuple that excludes some columns
 * Columns that aren't copied are marked as NULL
 */
XHeapTuple
xheap_get_tuple_partial(Relation relation, Buffer buffer, OffsetNumber offnum,
					 AttrNumber last_var, bool *bool_arr)
{
	Page		   dp = BufferGetPage(buffer);
	RowPtr		  *rp = XPageGetRowPtr(dp, offnum);
	bool		   enable_reverse_bitmap = false;
	XHeapDiskTuple item = (XHeapDiskTuple) XPageGetRowData(dp, rp);
	Size		   tuple_len;
	TupleDesc	   row_desc;
	int			   disk_tuple_n_attrs;
	XHeapTuple	   tuple;
	uint8		   nulls_len;

	Assert(offnum >= FirstOffsetNumber && offnum <= xheap_page_get_max_offset_number(dp));

	item = (XHeapDiskTuple) XPageGetRowData(dp, rp);
	/* length of source tuple */
	/* length of deleted tuple will no longer be 0 */
	tuple_len = RowPtrGetLen(rp);

	/* tuple_desc */
	row_desc = RelationGetDescr(relation);

	disk_tuple_n_attrs = XHeapTupleHeaderGetNatts(item);
	enable_reverse_bitmap = NAttrsReserveSpace(disk_tuple_n_attrs);
	nulls_len = BITMAPLEN(disk_tuple_n_attrs);
	if (enable_reverse_bitmap)
	{
		nulls_len += BITMAPLEN(disk_tuple_n_attrs);
	}

	/* make room for null bitmap and reserve bitmap in dest tuple */
	if (last_var > 0)
	{
		tuple_len += (nulls_len + XHEAP_MAX_ATTR_PAD);
		if (tuple_len > MaxXHeapTupleSize(relation))
		{
			/* if bitmaps cannot fit within tuple length limit, fallback */
			last_var = -1;
		}
	}

	tuple = (XHeapTuple) xheaptup_alloc(XHeapTupleDataSize + tuple_len);
	tuple->disk_tuple = (XHeapDiskTuple) ((char *) tuple + XHeapTupleDataSize);

	tuple->table_oid = RelationGetRelid(relation);
	tuple->disk_tuple_size = tuple_len;

	ItemPointerSet(&tuple->ctid, BufferGetBlockNumber(buffer), offnum);

	if (last_var <= 0)
		memcpy(tuple->disk_tuple, item, tuple_len);
	else
	{
		Assert(last_var <= row_desc->natts);
		memcpy(tuple->disk_tuple, item, SizeOfXHeapDiskTupleTillThoff);
		XHeapDiskTupSetHasNulls(tuple->disk_tuple);	 // dest tuple will always have nulls
		tuple->disk_tuple->t_hoff = SizeOfXHeapDiskTupleData + nulls_len;

		memset(tuple->disk_tuple->data, 0, nulls_len);

		if (XHeapDiskTupNoNulls(item))
			xheap_copy_disk_tuple_no_null(row_desc, bool_arr, tuple, last_var, item);
		else
			xheap_copy_disk_tuple_with_nulls(row_desc, bool_arr, tuple, last_var, item);
	}

	Assert(XHeapTupleHeaderGetNatts(tuple->disk_tuple) == disk_tuple_n_attrs);
	return tuple;
}

static Size
xpage_get_free_space(Page page)
{
	int space = (int) ((XHeapPageHeaderData *) page)->pd_upper -
				(int) ((XHeapPageHeaderData *) page)->pd_lower;
	if (space < (int) sizeof(RowPtr))
	{
		return 0;
	}
	space -= sizeof(RowPtr);

	return (Size) space;
}


/*
 * PageGetXHeapFreeSpace
 * Returns the size of the free (allocatable) space on a Xheap page,
 * reduced by the space needed for a new line pointer.
 *
 * This is same as PageGetHeapFreeSpace except for max tuples that can
 * be accommodated on a page or the way unused items are dealt.
 */
Size
page_get_xheap_free_space(Page page)
{
	Size space;

	space = xpage_get_free_space(page);
	if (space > 0)
	{
		OffsetNumber offnum, nline;

		nline = xheap_page_get_max_offset_number(page);
		if (nline >= CalculatedMaxXHeapTuplesPerPage)
		{
			if (PageHasFreeLinePointers(page))
			{
				/*
				 * Since this is just a hint, we must confirm that there is
				 * indeed a free line pointer
				 */
				for (offnum = FirstOffsetNumber; offnum <= nline;
					 offnum = OffsetNumberNext(offnum))
				{
					RowPtr *lp = XPageGetRowPtr(page, offnum);

					/*
					 * The unused items that have pending xact information
					 * can't be reused.
					 */
					if (!RowPtrIsUsed(lp))
						break;
				}

				if (offnum > nline)
				{
					/*
					 * The hint is wrong, but we can't clear it here since we
					 * don't have the ability to mark the page dirty.
					 */
					space = 0;
				}
			}
			else
			{
				/*
				 * Although the hint might be wrong, PageAddItem will believe
				 * it anyway, so we must believe it too.
				 */
				space = 0;
			}
		}
	}

	return space;
}

/*
 * PageGetExactXHeapFreeSpace
 * Returns the size of the free (allocatable) space on a page,
 * without any consideration for adding/removing line pointers.
 */
Size
page_get_exact_xheap_free_space(Page page)
{
	int space;

	/*
	 * Use signed arithmetic here so that we behave sensibly if pd_lower >
	 * pd_upper.
	 */
	space = (int) ((XHeapPageHeaderData *) page)->pd_upper -
			(int) ((XHeapPageHeaderData *) page)->pd_lower;

	if (space < 0)
		return 0;

	return (Size) space;
}

static XHeapFreeOffsetRanges *
locate_usable_item_ids(Page page, XHeapTuple *tuples, int ntuples, Size save_free_space,
					int *nthispage, Size *used_space)
{
	bool				   in_range = false;
	OffsetNumber		   offset_number;
	OffsetNumber		   limit;
	Size				   avail_space;
	XHeapFreeOffsetRanges *xfree_offset_ranges;
	bool				   is_first_insert = true;
	xfree_offset_ranges = (XHeapFreeOffsetRanges *) palloc0(sizeof(XHeapFreeOffsetRanges));
	xfree_offset_ranges->nranges = 0;

	avail_space = page_get_exact_xheap_free_space(page);
	limit = OffsetNumberNext(xheap_page_get_max_offset_number(page));

	/*
	 * Look for "recyclable" (unused) ItemId.  We check for no storage as
	 * well, just to be paranoid --- unused items should never have
	 * storage.
	 */
	for (offset_number = 1; offset_number < limit; offset_number++)
	{
		RowPtr *rp = XPageGetRowPtr(page, offset_number);

		if (*nthispage >= ntuples)
		{
			/* No more tuples to insert */
			break;
		}

		if (!RowPtrIsUsed(rp) && !RowPtrHasStorage(rp))
		{
			XHeapTuple xheaptup = tuples[*nthispage];
			Size	   needed_space;
			if (is_first_insert)
			{
				needed_space = *used_space + xheaptup->disk_tuple_size;
				is_first_insert = false;
			}
			else
			{
				needed_space = *used_space + xheaptup->disk_tuple_size + save_free_space;
			}
			/* Check if we can fit this tuple in the page */
			if (avail_space < needed_space)
			{
				/* No more space to insert tuples in this page */
				break;
			}

			(*used_space) += xheaptup->disk_tuple_size;
			(*nthispage)++;

			if (!in_range)
			{
				/* Start of a new range */
				xfree_offset_ranges->nranges++;
				xfree_offset_ranges->startOffset[xfree_offset_ranges->nranges - 1] =
					offset_number;
				in_range = true;
			}
			xfree_offset_ranges->endOffset[xfree_offset_ranges->nranges - 1] = offset_number;
		}
		else
		{
			in_range = false;
		}
	}

	return xfree_offset_ranges;
}

/*
 * XHeapGetUsableOffsetRanges
 *
 * Given a page and a set of tuples, it calculates how many tuples can fit in
 * the page and the contiguous ranges of free offsets that can be used/reused
 * in the same page to store those tuples.
 */
XHeapFreeOffsetRanges *
xheap_get_usable_offset_ranges(Buffer buffer, XHeapTuple *tuples, int ntuples,
						   Size save_free_space)
{
	Page				   page;
	int					   nthispage = 0;
	Size				   used_space = 0;
	Size				   avail_space;
	OffsetNumber		   limit;
	XHeapFreeOffsetRanges *xfree_offset_ranges = NULL;
	bool				   is_first_insert;
	XHeapPageHeaderData	  *xphdr;
	page = BufferGetPage(buffer);

	limit = OffsetNumberNext(xheap_page_get_max_offset_number(page));
	avail_space = page_get_exact_xheap_free_space(page);

	xphdr = (XHeapPageHeaderData *) page;
	if (XPageHasFreeLinePointers(xphdr))
	{
		xfree_offset_ranges = locate_usable_item_ids(page, tuples, ntuples, save_free_space,
												&nthispage, &used_space);
	}
	else
	{
		xfree_offset_ranges =
			(XHeapFreeOffsetRanges *) palloc0(sizeof(XHeapFreeOffsetRanges));
		xfree_offset_ranges->nranges = 0;
	}
	is_first_insert = (xfree_offset_ranges->nranges == 0);

	/*
	 * Now, there are no free line pointers. Check whether we can insert
	 * another tuple in the page, then we'll insert another range starting
	 * from limit to max required offset number. We can decide the actual end
	 * offset for this range while inserting tuples in the buffer.
	 */
	if ((limit <= MaxXHeapTuplesPerPageUpperBound) && (nthispage < ntuples))
	{
		XHeapTuple xheaptup = tuples[nthispage];
		Size	   needed_space;
		if (is_first_insert)
		{
			needed_space = used_space + sizeof(ItemIdData) + xheaptup->disk_tuple_size;
			is_first_insert = false;
		}
		else
		{
			needed_space = used_space + sizeof(ItemIdData) + xheaptup->disk_tuple_size +
						  save_free_space;
		}
		/* Check if we can fit this tuple + a new offset in the page */
		if (avail_space >= needed_space)
		{
			OffsetNumber max_required_offset;
			int			 required_tuples = ntuples - nthispage;

			/*
			 * Choose minimum among MaxOffsetNumber and the maximum offsets
			 * required for tuples.
			 */
			max_required_offset = Min(MaxOffsetNumber, (limit + required_tuples));

			xfree_offset_ranges->nranges++;
			xfree_offset_ranges->startOffset[xfree_offset_ranges->nranges - 1] = limit;
			xfree_offset_ranges->endOffset[xfree_offset_ranges->nranges - 1] =
				max_required_offset;
		}
		else if (xfree_offset_ranges->nranges == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("tuple is too big, please check the table fillfactor: "
							"needed_size = %lu, free_space = %lu, save_free_space = %lu.",
							(unsigned long) needed_space, (unsigned long) avail_space,
							(unsigned long) save_free_space)));
		}
	}

	if (xfree_offset_ranges->nranges == 0)
	{
		ereport(PANIC, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("please check free space: limit = %u, num_this_page = %d, "
							   "free_space = %lu.",
							   limit, nthispage, (unsigned long) avail_space)));
	}

	return xfree_offset_ranges;
}

void
xheap_record_potential_free_space(Buffer buffer, int delta)
{
	Size				 max_writable_spc;
	Size				 old_potential_freespace;
	Size				 new_potential_freespace;
	XHeapPageHeaderData *page = (XHeapPageHeaderData *) BufferGetPage(buffer);
	Assert(page->potential_freespace >= 0);

	max_writable_spc =
		page->pd_special - (SizeOfXHeapPageHeaderData);
	old_potential_freespace = page->potential_freespace;
	new_potential_freespace = Min(old_potential_freespace + delta, max_writable_spc);

	page->potential_freespace = new_potential_freespace;
}
