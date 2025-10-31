/* -------------------------------------------------------------------------
 *
 * xcluster.c
 *	  Implement the cluster functions for xheap.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/xheap/xcluster.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/rel.h"
#include "xheap/xcluster.h"
#include "xheap/xhio.h"
#include "xheap/xpage.h"
#include "xheap/xtuptoaster.h"

static void raw_xheap_insert(RewriteState state, XHeapTuple tup);

/*
 * Add a Xheap tuple to the new heap.
 *
 * Maintaining previous version's visibility information needs much more work,
 * so for now, we freeze all the tuples.  We only get
 * LIVE versions of the tuple as input.
 *
 * state       opaque state as returned by begin_heap_rewrite
 * oldTuple    original tuple in the old heap
 * newTuple    new, rewritten tuple to be inserted to new heap
 */
void
rewrite_xheap_tuple(RewriteState state, XHeapTuple old_tuple, XHeapTuple new_tuple)
{
	MemoryContext old_cxt;

	old_cxt = MemoryContextSwitchTo(state->rs_cxt);

	/*
	 * As of now, we copy only LIVE tuples in XHeap, so we can mark them as
	 * frozen.
	 */
	new_tuple->disk_tuple->flag &= ~XHEAP_VIS_STATUS_MASK;
	new_tuple->disk_tuple->modified_xid = FullTransactionIdFromEpochAndXid(0, FrozenTransactionId);
	new_tuple->disk_tuple->urec = INVALID_UNDO_REC_PTR;

	/* Insert the tuple and find out where it's put in new_heap */
	raw_xheap_insert(state, new_tuple);

	MemoryContextSwitchTo(old_cxt);
}

/*
 * Insert a xtuple to the new relation.  This has to track XHeapInsert
 * and its subsidiary functions!
 *
 * t_self of the tuple is set to the new TID of the tuple.
 */
static void
raw_xheap_insert(RewriteState state, XHeapTuple tup)
{
	Page		page = (Page) state->rs_buffer;
	Size		page_free_space,
				save_free_space;
	Size		len;
	OffsetNumber newoff;
	XHeapTuple	xheaptup = NULL;
	XHeapBufferPage bufpage;
	bool		use_wal = XLogIsNeeded() &&
		RelationNeedsWAL(state->rs_new_rel);	/* copy from teledbx */

	/*
	 * If the new tuple is too big for storage or contains already toasted
	 * out-of-line attributes from some other relation, invoke the toaster.
	 *
	 * Note: below this point, XHeaptup is the data we actually intend to store
	 * into the relation; tup is the caller's original untoasted data.
	 */
	if (state->rs_new_rel->rd_rel->relkind == RELKIND_TOASTVALUE)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!XHeapTupleHasExternal(tup));
		xheaptup = tup;
	}
	else if (XHeapTupleHasExternal(tup) || tup->disk_tuple_size > XTOAST_TUPLE_THRESHOLD)
		xheaptup = xheap_toast_insert_or_update(state->rs_new_rel, tup, NULL,
											XHEAP_INSERT_SKIP_FSM | (use_wal ? 0 : XHEAP_INSERT_SKIP_WAL));
	else
		xheaptup = tup;

	len = MAXALIGN(xheaptup->disk_tuple_size);	/* be conservative */

	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxXHeapTupleSize(state->rs_new_rel))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %lu, maximum size %lu",
						(unsigned long) len,
						(unsigned long) MaxXHeapTupleSize(state->rs_new_rel))));

	/* Compute desired extra freespace due to fillfactor option */
	save_free_space = RelationGetTargetPageFreeSpace(state->rs_new_rel,
												   HEAP_DEFAULT_FILLFACTOR);

	/* Now we can check to see if there's enough free space already. */
	page = (Page) state->rs_buffer;
	if (page)
	{
		page_free_space = page_get_xheap_free_space(page);
		if (len + save_free_space > page_free_space)
		{
			/*
			 * Doesn't fit, so write out the existing page.  It always
			 * contains a tuple.  Hence, unlike RelationGetBufferForTuple(),
			 * enforce saveFreeSpace unconditionally.
			 */
			smgr_bulk_write(state->rs_bulkstate, state->rs_blockno, state->rs_buffer, true);
			state->rs_buffer = NULL;
			page = NULL;
			state->rs_blockno++;
		}
	}

	if (!page)
	{
		/* Initialize a new empty page */
		state->rs_buffer = smgr_bulk_get_buf(state->rs_bulkstate);
		page = (Page) state->rs_buffer;
		xpage_init(XPAGE_HEAP, page, BLCKSZ, XHEAP_SPECIAL_SIZE);
	}

	bufpage.buffer = InvalidBuffer;
	bufpage.page = page;

	/* And now we can insert the tuple into the page */
	newoff = xpage_add_item(state->rs_new_rel, &bufpage, (Item) xheaptup->disk_tuple, xheaptup->disk_tuple_size,
						  InvalidOffsetNumber, false);
	if (newoff == InvalidOffsetNumber)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("failed to add tuple")));

	/* Update caller's t_self to the actual position where it was stored */
	ItemPointerSet(&(tup->ctid), state->rs_blockno, newoff);

	/* If xheaptup is a private copy, release it. */
	if (xheaptup != tup)
		XHeapFreeTuple(xheaptup);
}
