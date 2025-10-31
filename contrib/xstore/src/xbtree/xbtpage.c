/* -------------------------------------------------------------------------
 *
 * xbtpage.c
 *	  BTree-specific page management code for the xstore btree access
 *	  method.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/xbtree/xbtpage.c
 *
 *	NOTES
 *	   teledb btree pages look like ordinary relation pages.	The opaque
 *	   data at high addresses includes pointers to left and right siblings
 *	   and flag data describing page state.  The first page in a btree, page
 *	   zero, is special -- it stores meta-information describing the tree.
 *	   Pages one and higher store the actual tree data.
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hio.h"
#include "access/nbtree.h"
#include "xbtree/xbtree.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/nbtxlog.h"
#include "access/xloginsert.h"
#include "xstore.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "utils/resowner.h"
#include "storage/buf_internals.h"
#include "xstore.h"
#include "util/xxact.h"
#include "util/xrmgr.h"

static bool _xbt_mark_page_halfdead(Relation rel, Buffer leaf_buf, BTStack stack);
static bool _xbt_unlink_halfdead_page(Relation rel, Buffer leaf_buf, bool *right_sib_empty);
static bool _xbt_lock_branch_parent(Relation rel, BlockNumber child, BTStack stack,
								   Buffer *top_parent, OffsetNumber *top_off,
								   BlockNumber *target, BlockNumber *right_sib);
static void _xbt_log_reuse_page(Relation rel, BlockNumber blk_no,
							   FullTransactionId latest_removed_xid);
static bool _xbt_is_page_halfdead(Relation rel, BlockNumber blk);
static void do_reserve_deletion(Page page);
static bool reserve_for_deletion(Relation rel, Buffer buf);
static void xbtree_add_extra_blocks(Relation relation, BulkInsertState bi_state);

/*
 * _xbt_is_page_halfdead() -- Returns true, if the given block has the half-dead flag set.
 */
static bool
_xbt_is_page_halfdead(Relation rel, BlockNumber blk)
{
	Buffer				  buf;
	Page				  page;
	XBTPageOpaqueInternal opaque;
	bool				  result;

	buf = _bt_getbuf(rel, blk, BT_READ);
	page = BufferGetPage(buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	result = P_ISHALFDEAD(opaque);
	_bt_relbuf(rel, buf);

	return result;
}

/*
 * Subroutine to find the parent of the branch we're deleting.  This climbs
 * up the tree until it finds a page with more than one child, i.e. a page
 * that will not be totally emptied by the deletion.  The chain of pages below
 * it, with one downlink each, will form the branch that we need to delete.
 *
 * If we cannot remove the downlink from the parent, because it's the
 * rightmost entry, returns false.  On success, *topparent and *topoff are set
 * to the buffer holding the parent, and the offset of the downlink in it.
 * *topparent is write-locked, the caller is responsible for releasing it when
 * done.  *target is set to the topmost page in the branch to-be-deleted, i.e.
 * the page whose downlink *topparent / *topoff point to, and *rightsib to its
 * right sibling.
 *
 * "child" is the leaf page we wish to delete, and "stack" is a search stack
 * leading to it (it actually leads to the leftmost leaf page with a high key
 * matching that of the page to be deleted in !heapkeyspace indexes).  Note
 * that we will update the stack entry(s) to reflect current downlink
 * positions --- this is essentially the same as the corresponding step of
 * splitting, and is not expected to affect caller.  The caller should
 * initialize *target and *rightsib to the leaf page and its right sibling.
 *
 * Note: it's OK to release page locks on any internal pages between the leaf
 * and *topparent, because a safe deletion can't become unsafe due to
 * concurrent activity.  An internal page can only acquire an entry if the
 * child is split, but that cannot happen as long as we hold a lock on the
 * leaf.
 */
static bool
_xbt_lock_branch_parent(Relation rel, BlockNumber child, BTStack stack, Buffer *top_parent,
					   OffsetNumber *top_off, BlockNumber *target, BlockNumber *right_sib)
{
	BlockNumber			  parent;
	OffsetNumber		  p_offset;
	OffsetNumber		  max_off;
	Buffer				  p_buf;
	Page				  page;
	XBTPageOpaqueInternal opaque;
	BlockNumber			  left_sib;

	/*
     * Locate the downlink of "child" in the parent, updating the stack entry
     * if needed.  This is how !heapkeyspace indexes deal with having
     * non-unique high keys in leaf level pages.  Even heapkeyspace indexes
     * can have a stale stack due to insertions into the parent.
     */
	p_buf = _xbt_getstackbuf(rel, stack, *target);
	if (p_buf == InvalidBuffer)
	{
		/*
		 * Failed to "re-find" a pivot tuple whose downlink matched our child
		 * block number on the parent level -- the index must be corrupt.
		 * Don't even try to delete the leafbuf subtree.  Just report the
		 * issue and press on with vacuuming the index.
		 *
		 * Note: _bt_getstackbuf() recovers from concurrent page splits that
		 * take place on the parent level.  Its approach is a near-exhaustive
		 * linear search.  This also gives it a surprisingly good chance of
		 * recovering in the event of a buggy or inconsistent opclass.  But we
		 * don't rely on that here.
		 */
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("failed to re-find parent key in index \"%s\" for deletion target page %u",
								 RelationGetRelationName(rel), child)));
		return false;
	}

	parent = stack->bts_blkno;
	p_offset = stack->bts_offset;

	page = BufferGetPage(p_buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	max_off = PageGetMaxOffsetNumber(page);

	/*
     * If the target is the rightmost child of its parent, then we can't
     * delete, unless it's also the only child.
     */
	if (p_offset >= max_off)
	{
		/* It's rightmost child... */
		if (p_offset == P_FIRSTDATAKEY(opaque))
		{
			/*
             * It's only child, so safe if parent would itself be removable.
             * We have to check the parent itself, and then recurse to test
             * the conditions at the parent's parent.
             */
			if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_INCOMPLETE_SPLIT(opaque))
			{
				_bt_relbuf(rel, p_buf);
				return false;
			}

			*target = parent;
			*right_sib = opaque->btpo_next;
			left_sib = opaque->btpo_prev;

			_bt_relbuf(rel, p_buf);

			/*
             * Like in _bt_pagedel, check that the left sibling is not marked
             * with INCOMPLETE_SPLIT flag.  That would mean that there is no
             * downlink to the page to be deleted, and the page deletion
             * algorithm isn't prepared to handle that.
             */
			if (left_sib != P_NONE)
			{
				Buffer				  l_buf;
				Page				  l_page;
				XBTPageOpaqueInternal l_opaque;

				l_buf = _bt_getbuf(rel, left_sib, BT_READ);
				l_page = BufferGetPage(l_buf);
				l_opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(l_page);
				/*
                 * If the left sibling was concurrently split, so that its
                 * next-pointer doesn't point to the current page anymore, the
                 * split that created the current page must be completed. (We
                 * don't allow splitting an incompletely split page again
                 * until the previous split has been completed)
                 */
				if (l_opaque->btpo_next == parent && P_INCOMPLETE_SPLIT(l_opaque))
				{
					_bt_relbuf(rel, l_buf);
					return false;
				}
				_bt_relbuf(rel, l_buf);
			}

			return _xbt_lock_branch_parent(rel, parent, stack->bts_parent, top_parent,
										  top_off, target, right_sib);
		}
		else
		{
			/* Unsafe to delete */
			_bt_relbuf(rel, p_buf);
			return false;
		}
	}
	else
	{
		/* Not rightmost child, so safe to delete */
		*top_parent = p_buf;
		*top_off = p_offset;
		return true;
	}
}

/*
 * First stage of page deletion.  Remove the downlink to the top of the
 * branch being deleted, and mark the leaf page as half-dead.
 */
static bool
_xbt_mark_page_halfdead(Relation rel, Buffer leaf_buf, BTStack stack)
{
	BlockNumber			  leaf_blkno;
	BlockNumber			  leaf_right_sib;
	BlockNumber			  target;
	BlockNumber			  right_sib;
	ItemId				  item_id;
	Page				  page;
	XBTPageOpaqueInternal opaque;
	Buffer				  top_parent;
	OffsetNumber		  top_off;
	OffsetNumber		  next_offset;
	IndexTuple			  it_up;
	IndexTupleData		  trunc_tuple;

	page = BufferGetPage(leaf_buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	Assert(!P_RIGHTMOST(opaque) && !P_ISROOT(opaque) && !P_ISDELETED(opaque) &&
		   !P_ISHALFDEAD(opaque) && P_ISLEAF(opaque) &&
		   P_FIRSTDATAKEY(opaque) > PageGetMaxOffsetNumber(page));

	/*
     * Save info about the leaf page.
     */
	leaf_blkno = BufferGetBlockNumber(leaf_buf);
	leaf_right_sib = opaque->btpo_next;

	/*
     * Before attempting to lock the parent page, check that the right sibling
     * is not in half-dead state.  A half-dead right sibling would have no
     * downlink in the parent, which would be highly confusing later when we
     * delete the downlink that follows the current page's downlink. (I
     * believe the deletion would work correctly, but it would fail the
     * cross-check we make that the following downlink points to the right
     * sibling of the delete page.)
     */
	if (_xbt_is_page_halfdead(rel, leaf_right_sib))
	{
		elog(DEBUG1, "could not delete page %u because its right sibling %u is half-dead",
			 leaf_blkno, leaf_right_sib);
		return false;
	}

	/*
     * We cannot delete a page that is the rightmost child of its immediate
     * parent, unless it is the only child --- in which case the parent has to
     * be deleted too, and the same condition applies recursively to it. We
     * have to check this condition all the way up before trying to delete,
     * and lock the final parent of the to-be-deleted subtree.
     *
     * However, we won't need to repeat the above _bt_is_page_halfdead() check
     * for parent/ancestor pages because of the rightmost restriction. The
     * leaf check will apply to a right "cousin" leaf page rather than a
     * simple right sibling leaf page in cases where we actually go on to
     * perform internal page deletion. The right cousin leaf page is
     * representative of the left edge of the subtree to the right of the
     * to-be-deleted subtree as a whole.  (Besides, internal pages are never
     * marked half-dead, so it isn't even possible to directly assess if an
     * internal page is part of some other to-be-deleted subtree.)
     */
	right_sib = leaf_right_sib;
	target = leaf_blkno;
	if (!_xbt_lock_branch_parent(rel, leaf_blkno, stack, 
						&top_parent, &top_off, &target, &right_sib))
		return false;

	/*
     * Check that the parent-page index items we're about to delete/overwrite
     * contain what we expect.  This can fail if the index has become corrupt
     * for some reason.  We want to throw any error before entering the
     * critical section --- otherwise it'd be a PANIC.
     *
     * The test on the target item is just an Assert because
     * _bt_lock_branch_parent should have guaranteed it has the expected
     * contents.  The test on the next-child downlink is known to sometimes
     * fail in the field, though.
     */
	page = BufferGetPage(top_parent);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

#ifdef USE_ASSERT_CHECKING
	item_id = PageGetItemId(page, top_off);
	it_up = (IndexTuple) PageGetItem(page, item_id);
	Assert(XBTreeTupleGetDownLink(it_up) == target);
#endif

	next_offset = OffsetNumberNext(top_off);
	item_id = PageGetItemId(page, next_offset);
	it_up = (IndexTuple) PageGetItem(page, item_id);
	if (XBTreeTupleGetDownLink(it_up) != right_sib)
	{
		Buffer				  r_buf = _bt_getbuf(rel, right_sib, BT_READ);
		Page				  r_page = BufferGetPage(r_buf);
		XBTPageOpaqueInternal r_opaque =
			(XBTPageOpaqueInternal) PageGetSpecialPointer(r_page);
		if (P_ISHALFDEAD(r_opaque))
		{
			/* right page already deleted by concurrent worker, do not continue */
			_bt_relbuf(rel, r_buf);
			_bt_relbuf(rel, top_parent);
			return false;
		}
		elog(ERROR,
			 "right sibling %u of block %u is not next child %u of block %u in index "
			 "\"%s\"",
			 right_sib, target, XBTreeTupleGetDownLink(it_up) != right_sib,
			 BufferGetBlockNumber(top_parent), RelationGetRelationName(rel));
	}

	/*
     * Any insert which would have gone on the leaf block will now go to its
     * right sibling.
     */
	PredicateLockPageCombine(rel, leaf_blkno, leaf_right_sib);

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/*
     * Update parent.  The normal case is a tad tricky because we want to
     * delete the target's downlink and the *following* key.  Easiest way is
     * to copy the right sibling's downlink over the target downlink, and then
     * delete the following item.
     */
	page = BufferGetPage(top_parent);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	item_id = PageGetItemId(page, top_off);
	it_up = (IndexTuple) PageGetItem(page, item_id);
	XBTreeTupleSetDownLink(it_up, right_sib);

	next_offset = OffsetNumberNext(top_off);
	PageIndexTupleDelete(page, next_offset);

	/*
     * Mark the leaf page as half-dead, and stamp it with a pointer to the
     * highest internal page in the branch we're deleting.  We use the tid of
     * the high key to store it.
     */
	page = BufferGetPage(leaf_buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	opaque->btpo_flags |= BTP_HALF_DEAD;

	PageIndexTupleDelete(page, P_HIKEY);
	Assert(PageGetMaxOffsetNumber(page) == 0);
	memset(&trunc_tuple, 0, sizeof(IndexTupleData));
	trunc_tuple.t_info = sizeof(IndexTupleData);
	if (target != leaf_blkno)
		XBTreeTupleSetTopParent(&trunc_tuple, target);
	else
		XBTreeTupleSetTopParent(&trunc_tuple, InvalidBlockNumber);

	if (PageAddItem(page, (Item) &trunc_tuple, sizeof(IndexTupleData), P_HIKEY, 
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "could not add dummy high key to half-dead page");

	/* Must mark buffers dirty before XLogInsert */
	MarkBufferDirty(top_parent);
	MarkBufferDirty(leaf_buf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_mark_page_halfdead xlrec;
		XLogRecPtr					rec_ptr;

		xlrec.poffset = top_off;
		xlrec.leafblk = leaf_blkno;
		if (target != leaf_blkno)
			xlrec.topparent = target;
		else
			xlrec.topparent = InvalidBlockNumber;

		XLogBeginInsert();
		XLogRegisterBuffer(0, leaf_buf, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, top_parent, REGBUF_STANDARD);

		page = BufferGetPage(leaf_buf);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		xlrec.leftblk = opaque->btpo_prev;
		xlrec.rightblk = opaque->btpo_next;

		XLogRegisterData((char *) &xlrec, SizeOfBtreeMarkPageHalfDead);

		rec_ptr = XLogInsert(RM_XBTREE_ID, XLOG_XBTREE_MARK_PAGE_HALFDEAD);

		page = BufferGetPage(top_parent);
		PageSetLSN(page, rec_ptr);
		page = BufferGetPage(leaf_buf);
		PageSetLSN(page, rec_ptr);
	}

	END_CRIT_SECTION();

	_bt_relbuf(rel, top_parent);
	return true;
}

static void
do_reserve_deletion(Page page)
{
	XBTPageOpaque opaque = (XBTPageOpaque) PageGetSpecialPointer(page);

	bool is_vacuum_process =
		( (MyProc->statusFlags & PROC_IN_VACUUM) || AmAutoVacuumWorkerProcess());
	if (is_vacuum_process && !TransactionIdIsValid(GetTopTransactionIdIfAny()))
	{
		((XBTPageOpaqueInternal) opaque)->btpo_flags |= BTP_VACUUM_DELETING;
		opaque->xact = ReadNextFullTransactionId();
	}
	else
	{
		opaque->xact = GetTopFullTransactionId();
	}
}


static bool
reserve_for_deletion(Relation rel, Buffer buf)
{
	FullTransactionId previous_xact;
	Page		  page = BufferGetPage(buf);
	XBTPageOpaque opaque = (XBTPageOpaque) PageGetSpecialPointer(page);

	const int MAX_RETRY_TIMES = 1000;
	const int WAIT_TIME = 500;
	int		  times = MAX_RETRY_TIMES;
	while (times--)
	{
		if (P_ISDELETED((XBTPageOpaqueInternal) opaque))
			return false;

		if (P_VACUUM_DELETING((XBTPageOpaqueInternal) opaque))
		{
			FullTransactionId oldest_xmin = get_full_oldest_xmin();
			if (FullTransactionIdPrecedes(opaque->xact, oldest_xmin))
			{
				opaque->xact = InvalidFullTransactionId;
				((XBTPageOpaqueInternal) opaque)->btpo_flags &= ~BTP_VACUUM_DELETING;
			}
		}

		/* try to reserve for the deleteion */
		if (!FullTransactionIdIsValid(opaque->xact))
		{
			do_reserve_deletion(page);
			return true;
		}
		/* someone else is deleting, need wait for it */
		previous_xact = opaque->xact;
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(previous_xact)))
			/* already reserved by self, continue */
			return true;
		if (!xstore_transaction_id_is_in_progress(previous_xact))
		{
			/* previous worker abort, we can still reserve for deletion */
			do_reserve_deletion(page);
			return true;
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		pg_usleep(WAIT_TIME); /* 0.5 ms */
		LockBuffer(buf, BT_WRITE);
	}
	return false;
}

/*
 * Unlink a page in a branch of half-dead pages from its siblings.
 *
 * If the leaf page still has a downlink pointing to it, unlinks the highest
 * parent in the to-be-deleted branch instead of the leaf page.  To get rid
 * of the whole branch, including the leaf page itself, iterate until the
 * leaf page is deleted.
 *
 * Returns 'false' if the page could not be unlinked (shouldn't happen).  If
 * the right sibling of the current target page is empty, *rightsib_empty is
 * set to true, allowing caller to delete the target's right sibling page in
 * passing.  Note that *rightsib_empty is only actually used by caller when
 * target page is leafbuf, following last call here for leafbuf/the subtree
 * containing leafbuf.  (We always set *rightsib_empty for caller, just to be
 * consistent.)
 *
 * We maintain *oldestBtpoXact for pages that are deleted by the current
 * VACUUM operation here.  This must be handled here because we conservatively
 * assume that there needs to be a new call to ReadNewTransactionId() each
 * time a page gets deleted.  See comments about the underlying assumption
 * below.
 *
 * Must hold pin and lock on leafbuf at entry (read or write doesn't matter).
 * On success exit, we'll be holding pin and write lock.  On failure exit,
 * we'll release both pin and lock before returning (we define it that way
 * to avoid having to reacquire a lock we already released).
 */
static bool
_xbt_unlink_halfdead_page(Relation rel, Buffer leaf_buf, bool *right_sib_empty)
{
	BlockNumber			  leaf_blkno = BufferGetBlockNumber(leaf_buf);
	BlockNumber			  leaf_left_sib;
	BlockNumber			  leaf_right_sib;
	BlockNumber			  target;
	BlockNumber			  left_sib;
	BlockNumber			  right_sib;
	Buffer				  l_buf = InvalidBuffer;
	Buffer				  buf;
	Buffer				  r_buf;
	Buffer				  meta_buf = InvalidBuffer;
	Page				  meta_pg = NULL;
	BTMetaPageData		 *meta_d = NULL;
	ItemId				  item_id;
	Page				  page;
	XBTPageOpaqueInternal opaque;
	bool				  right_sib_is_rightmost = false;
	int					  target_level;
	IndexTuple			  leaf_hi_key;
	BlockNumber			  next_child;

	if (!reserve_for_deletion(rel, leaf_buf))
	{
		_bt_relbuf(rel, leaf_buf);
		return false; /* concurrent worker finished this deletion already */
	}

	page = BufferGetPage(leaf_buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	Assert(P_ISLEAF(opaque) && P_ISHALFDEAD(opaque));

	/*
     * Remember some information about the leaf page.
     */
	item_id = PageGetItemId(page, P_HIKEY);
	leaf_hi_key = (IndexTuple) PageGetItem(page, item_id);
	leaf_left_sib = opaque->btpo_prev;
	leaf_right_sib = opaque->btpo_next;

	LockBuffer(leaf_buf, BUFFER_LOCK_UNLOCK);

	/*
     * Check here, as calling loops will have locks held, preventing
     * interrupts from being processed.
     */
	CHECK_FOR_INTERRUPTS();

	/*
     * If the leaf page still has a parent pointing to it (or a chain of
     * parents), we don't unlink the leaf page yet, but the topmost remaining
     * parent in the branch.  Set 'target' and 'buf' to reference the page
     * actually being unlinked.
     */
	target = XBTreeTupleGetTopParent(leaf_hi_key);
	if (target != InvalidBlockNumber)
	{
		Assert(target != leaf_blkno);

		/* fetch the block number of the topmost parent's left sibling */
		buf = _bt_getbuf(rel, target, BT_READ);
		page = BufferGetPage(buf);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		left_sib = opaque->btpo_prev;
		target_level = opaque->btpo.level;

		/*
         * To avoid deadlocks, we'd better drop the target page lock before
         * going further.
         */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}
	else
	{
		target = leaf_blkno;

		buf = leaf_buf;
		left_sib = leaf_left_sib;
		target_level = 0;
	}

	/*
	 * We have to lock the pages we need to modify in the standard order:
	 * moving right, then up.  Else we will deadlock against other writers.
	 *
	 * So, first lock the leaf page, if it's not the target.  Then find and
	 * write-lock the current left sibling of the target page.  The sibling
	 * that was current a moment ago could have split, so we may have to move
	 * right.  This search could fail if either the sibling or the target page
	 * was deleted by someone else meanwhile; if so, give up.  (Right now,
	 * that should never happen, since page deletion is only done in VACUUM
	 * and there shouldn't be multiple VACUUMs concurrently on the same
	 * table.)
     */
	if (target != leaf_blkno)
		LockBuffer(leaf_buf, BT_WRITE);
	if (left_sib != P_NONE)
	{
		l_buf = _bt_getbuf(rel, left_sib, BT_WRITE);
		page = BufferGetPage(l_buf);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		while (P_ISDELETED(opaque) || opaque->btpo_next != target)
		{
			/*
             * Before we follow the link from the page that was the left
             * sibling mere moments ago, validate its right link.  This
             * reduces the opportunities for loop to fail to ever make any
             * progress in the presence of index corruption.
             *
             * Note: we rely on the assumption that there can only be one
             * vacuum process running at a time (against the same index).
             */
			bool leftSibValid = true;
			if (P_RIGHTMOST(opaque) || P_ISDELETED(opaque) ||
				left_sib == opaque->btpo_next)
			{
				leftSibValid = false;
			}

			left_sib = opaque->btpo_next;
			_bt_relbuf(rel, l_buf);

			if (!leftSibValid)
			{
				if (target != leaf_blkno)
				{
					/* we have only a pin on target, but pin+lock on leafbuf */
					ReleaseBuffer(buf);
					_bt_relbuf(rel, leaf_buf);
				}
				else
				{
					/* we have only a pin on leafbuf */
					ReleaseBuffer(leaf_buf);
				}

				ereport(
					LOG,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal(
						 "valid left sibling for deletion target could not be located: "
						 "left sibling %u of target %u with leafblkno %u in index \"%s\"",
						 left_sib, target, leaf_blkno, RelationGetRelationName(rel))));
				return false;
			}

			CHECK_FOR_INTERRUPTS();

			/* step right one page */
			l_buf = _bt_getbuf(rel, left_sib, BT_WRITE);
			page = BufferGetPage(l_buf);
			opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		}
	}
	else
		l_buf = InvalidBuffer;

	/*
     * Next write-lock the target page itself.  It should be okay to take just
     * a write lock not a superexclusive lock, since no scans would stop on an
     * empty page.
     */
	LockBuffer(buf, BT_WRITE);
	page = BufferGetPage(buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	/*
     * Check page is still empty etc, else abandon deletion.  This is just for
     * paranoia's sake; a half-dead page cannot resurrect because there can be
     * only one vacuum process running at a time.
     */
	if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque))
	{
		if (BufferIsValid(l_buf))
		{
			_bt_relbuf(rel, l_buf);
		}
		_bt_relbuf(rel, buf);
		return false;
	}
	if (opaque->btpo_prev != left_sib)
	{
		if (BufferIsValid(l_buf))
		{
			_bt_relbuf(rel, l_buf);
		}
		_bt_relbuf(rel, buf);
		return false;
	}

	if (target == leaf_blkno)
	{
		if (P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page) || !P_ISLEAF(opaque) ||
			!P_ISHALFDEAD(opaque))
		{
			if (BufferIsValid(l_buf))
				_bt_relbuf(rel, l_buf);
			_bt_relbuf(rel, buf);
			return false;
		}
		next_child = InvalidBlockNumber;
	}
	else
	{
		if (P_FIRSTDATAKEY(opaque) != PageGetMaxOffsetNumber(page) || P_ISLEAF(opaque))
		{
			if (BufferIsValid(l_buf))
			{
				_bt_relbuf(rel, l_buf);
			}
			_bt_relbuf(rel, buf);
			return false;
		}

		/* remember the next non-leaf child down in the branch. */
		item_id = PageGetItemId(page, P_FIRSTDATAKEY(opaque));
		next_child = XBTreeTupleGetDownLink((IndexTuple) PageGetItem(page, item_id));
		if (next_child == leaf_blkno)
		{
			next_child = InvalidBlockNumber;
		}
	}

	/*
     * And next write-lock the (current) right sibling.
     */
	right_sib = opaque->btpo_next;
	r_buf = _bt_getbuf(rel, right_sib, BT_WRITE);
	page = BufferGetPage(r_buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	if (opaque->btpo_prev != target)
	{
		elog(ERROR,
			 "right sibling's left-link doesn't match: "
			 "block %u links to %u instead of expected %u in index \"%s\"",
			 right_sib, opaque->btpo_prev, target, RelationGetRelationName(rel));
	}
	right_sib_is_rightmost = P_RIGHTMOST(opaque);
	*right_sib_empty = (P_FIRSTDATAKEY(opaque) > PageGetMaxOffsetNumber(page));

	/*
     * If we are deleting the next-to-last page on the target's level, then
     * the rightsib is a candidate to become the new fast root. (In theory, it
     * might be possible to push the fast root even further down, but the odds
     * of doing so are slim, and the locking considerations daunting.)
     *
     * We don't support handling this in the case where the parent is becoming
     * half-dead, even though it theoretically could occur.
     *
     * We can safely acquire a lock on the metapage here --- see comments for
     * _bt_newroot().
     */
	if (left_sib == P_NONE && right_sib_is_rightmost)
	{
		page = BufferGetPage(r_buf);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		if (P_RIGHTMOST(opaque))
		{
			/* rightsib will be the only one left on the level */
			meta_buf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
			meta_pg = BufferGetPage(meta_buf);
			meta_d = BTPageGetMeta(meta_pg);
			/*
             * The expected case here is btm_fastlevel == targetlevel+1; if
             * the fastlevel is <= targetlevel, something is wrong, and we
             * choose to overwrite it to fix it.
             */
			if (meta_d->btm_fastlevel > (uint32) target_level + 1)
			{
				/* no update wanted */
				_bt_relbuf(rel, meta_buf);
				meta_buf = InvalidBuffer;
			}
		}
	}

	/*
     * Here we begin doing the deletion.
     */

	/* No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/*
     * Update siblings' side-links.  Note the target page's side-links will
     * continue to point to the siblings.  Asserts here are just rechecking
     * things we already verified above.
     */
	if (BufferIsValid(l_buf))
	{
		page = BufferGetPage(l_buf);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		Assert(opaque->btpo_next == target);
		opaque->btpo_next = right_sib;
	}
	page = BufferGetPage(r_buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	Assert(opaque->btpo_prev == target);
	opaque->btpo_prev = left_sib;

	/*
     * If we deleted a parent of the targeted leaf page, instead of the leaf
     * itself, update the leaf to point to the next remaining child in the
     * branch.
     */
	if (target != leaf_blkno)
	{
		if (next_child == leaf_blkno)
		{
			XBTreeTupleSetTopParent(leaf_hi_key, InvalidBlockNumber);
		}
		else
		{
			XBTreeTupleSetTopParent(leaf_hi_key, next_child);
		}
	}

	/*
     * Mark the page itself deleted.  It can be recycled when all current
     * transactions are gone.  Storing GetTopTransactionId() would work, but
     * we're in VACUUM and would not otherwise have an XID.  Having already
     * updated links to the target, ReadNextTransactionId() suffices as an
     * upper bound.  Any scan having retained a now-stale link is advertising
     * in its PGXACT an xmin less than or equal to the value we read here.  It
     * will continue to do so, holding back RecentGlobalXmin, for the duration
     * of that scan.
     */
	page = BufferGetPage(buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	opaque->btpo_flags &= ~BTP_HALF_DEAD;
	opaque->btpo_flags |= BTP_DELETED;
	((XBTPageOpaque) opaque)->xact = ReadNextFullTransactionId();

	/* And update the metapage, if needed */
	if (BufferIsValid(meta_buf))
	{
		meta_d->btm_fastroot = right_sib;
		meta_d->btm_fastlevel = target_level;
		MarkBufferDirty(meta_buf);
	}

	/* Must mark buffers dirty before XLogInsert */
	MarkBufferDirty(r_buf);
	MarkBufferDirty(buf);
	if (BufferIsValid(l_buf))
	{
		MarkBufferDirty(l_buf);
	}
	if (target != leaf_blkno)
	{
		MarkBufferDirty(leaf_buf);
	}

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_unlink_page xlrec;
		xl_btree_metadata	 xlmeta;
		uint8				 xlinfo;
		XLogRecPtr			 recptr;

		XLogBeginInsert();

		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);
		if (BufferIsValid(l_buf))
			XLogRegisterBuffer(1, l_buf, REGBUF_STANDARD);
		XLogRegisterBuffer(2, r_buf, REGBUF_STANDARD);
		if (target != leaf_blkno)
			XLogRegisterBuffer(3, leaf_buf, REGBUF_WILL_INIT);

		/* information on the unlinked block */
		xlrec.leftsib = left_sib;
		xlrec.rightsib = right_sib;
		xlrec.safexid =  FullTransactionIdFromEpochAndXid(0,opaque->btpo.xact_old);  

		/* information needed to recreate the leaf block (if not the target) */
		xlrec.leafleftsib = leaf_left_sib;
		xlrec.leafrightsib = leaf_right_sib;
		xlrec.leaftopparent = next_child;

		XLogRegisterData((char *) &xlrec, SizeOfBtreeUnlinkPage);

		if (BufferIsValid(meta_buf))
		{
			XLogRegisterBuffer(4, meta_buf, REGBUF_WILL_INIT | REGBUF_STANDARD);

			xlmeta.root = meta_d->btm_root;
			xlmeta.level = meta_d->btm_level;
			xlmeta.fastroot = meta_d->btm_fastroot;
			xlmeta.fastlevel = meta_d->btm_fastlevel;

			XLogRegisterBufData(4, (char *) &xlmeta, sizeof(xl_btree_metadata));
			xlinfo = XLOG_XBTREE_UNLINK_PAGE_META;
		}
		else
		{
			xlinfo = XLOG_XBTREE_UNLINK_PAGE;
		}

		recptr = XLogInsert(RM_XBTREE_ID, xlinfo);

		if (BufferIsValid(meta_buf))
		{
			PageSetLSN(meta_pg, recptr);
		}
		page = BufferGetPage(r_buf);
		PageSetLSN(page, recptr);
		page = BufferGetPage(buf);
		PageSetLSN(page, recptr);
		if (BufferIsValid(l_buf))
		{
			page = BufferGetPage(l_buf);
			PageSetLSN(page, recptr);
		}
		if (target != leaf_blkno)
		{
			page = BufferGetPage(leaf_buf);
			PageSetLSN(page, recptr);
		}
	}

	END_CRIT_SECTION();

	/* release metapage */
	if (BufferIsValid(meta_buf))
		_bt_relbuf(rel, meta_buf);

	/* release siblings */
	if (BufferIsValid(l_buf))
		_bt_relbuf(rel, l_buf);
	_bt_relbuf(rel, r_buf);

	/*
     * Release the target, if it was not the leaf block.  The leaf is always
     * kept locked.
     */
	if (target != leaf_blkno)
		_bt_relbuf(rel, buf);

	return true;
}

/*
 * Log the reuse of a page from the recycle queue.
 */
static void
_xbt_log_reuse_page(Relation rel, BlockNumber blkno, FullTransactionId latestRemovedXid)
{
	xl_btree_reuse_page xlrec;

	if (!RelationNeedsWAL(rel))
		return;

	/*
     * Note that we don't register the buffer with the record, because this
     * operation doesn't modify the page. This record only exists to provide a
     * conflict point for Hot Standby.
     *
     * XLOG stuff
     */
	xlrec.locator = rel->rd_locator;

	xlrec.block = blkno;
	xlrec.snapshotConflictHorizon = latestRemovedXid; 

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfBtreeReusePage);

	(void) XLogInsert(RM_XBTREE_ID, XLOG_XBTREE_REUSE_PAGE);  // rel->rd_node.bucketNode
}


/*
 *	_xbt_pageinit() -- Initialize a new page.
 *
 * On return, the page header is initialized; data space is empty;
 * special space is zeroed out.
 */
void
_xbt_pageinit(Page page, Size size)
{
	PageInit(page, size, sizeof(XBTPageOpaqueData));
	((XBTPageOpaque) PageGetSpecialPointer(page))->xact = InvalidFullTransactionId;
}

/*
 *	_xbt_initmetapage() -- Fill a page buffer with a correct metapage image
 */
void
_xbt_initmetapage(Page page, BlockNumber rootbknum, uint32 level)
{
	BTMetaPageData		 *metad = NULL;
	XBTPageOpaqueInternal metaopaque;

	_xbt_pageinit(page, BLCKSZ);

	metad = BTPageGetMeta(page);
	metad->btm_magic = BTREE_MAGIC;
	metad->btm_version = BTREE_VERSION;
	metad->btm_root = rootbknum;
	metad->btm_level = level;
	metad->btm_fastroot = rootbknum;
	metad->btm_fastlevel = level;

	metaopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	metaopaque->btpo_flags = BTP_META;

	/*
     * Set pd_lower just past the end of the metadata.	This is not essential
     * but it makes the page look compressible to xlog.c.
     */
	((PageHeader) page)->pd_lower =
		(uint16) (((char *) metad + sizeof(BTMetaPageData)) - (char *) page);
}

/*
 *	_xbt_getroot() -- Get the root page of the btree.
 *
 *		Since the root page can move around the btree file, we have to read
 *		its location from the metadata page, and then read the root page
 *		itself.  If no root page exists yet, we have to create one.  The
 *		standard class of race conditions exists here; I think I covered
 *		them all in the Hopi Indian rain dance of lock requests below.
 *
 *		The access type parameter (BT_READ or BT_WRITE) controls whether
 *		a new root page will be created or not.  If access = BT_READ,
 *		and no root page exists, we just return InvalidBuffer.	For
 *		BT_WRITE, we try to create the root page if it doesn't exist.
 *		NOTE that the returned root page will have only a read lock set
 *		on it even if access = BT_WRITE!
 *
 *		The returned page is not necessarily the true root --- it could be
 *		a "fast root" (a page that is alone in its level due to deletions).
 *		Also, if the root page is split while we are "in flight" to it,
 *		what we will return is the old root, which is now just the leftmost
 *		page on a probably-not-very-wide level.  For most purposes this is
 *		as good as or better than the true root, so we do not bother to
 *		insist on finding the true root.  We do, however, guarantee to
 *		return a live (not deleted or half-dead) page.
 *
 *		On successful return, the root page is pinned and read-locked.
 *		The metadata page is not locked or pinned on exit.
 */
Buffer
_xbt_getroot(Relation rel, int access)
{
	Buffer				  metabuf;
	Page				  metapg;
	XBTPageOpaqueInternal metaopaque;
	Buffer				  rootbuf;
	Page				  rootpage;
	XBTPageOpaqueInternal rootopaque;
	BlockNumber			  rootblkno;
	uint32				  rootlevel;
	BTMetaPageData		 *metad = NULL;

	/*
     * Try to use previously-cached metapage data to find the root.  This
     * normally saves one buffer access per index search, which is a very
     * helpful savings in bufmgr traffic and hence contention.
     */
	if (rel->rd_amcache != NULL)
	{
		metad = (BTMetaPageData *) rel->rd_amcache;
		/* We shouldn't have cached it if any of these fail */
		Assert(metad->btm_magic == BTREE_MAGIC);
		Assert(metad->btm_version == BTREE_VERSION);
		Assert(metad->btm_root != P_NONE);

		rootblkno = metad->btm_fastroot;
		Assert(rootblkno != P_NONE);
		rootlevel = metad->btm_fastlevel;

		rootbuf = _bt_getbuf(rel, rootblkno, BT_READ);
		rootpage = BufferGetPage(rootbuf);
		rootopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(rootpage);

		/*
         * Since the cache might be stale, we check the page more carefully
         * here than normal.  We *must* check that it's not deleted. If it's
         * not alone on its level, then we reject too --- this may be overly
         * paranoid but better safe than sorry.  Note we don't check P_ISROOT,
         * because that's not set in a "fast root".
         */
		if (!P_IGNORE(rootopaque) && rootopaque->btpo.level == rootlevel &&
			P_LEFTMOST(rootopaque) && P_RIGHTMOST(rootopaque))
		{
			/* OK, accept cached page as the root */
			return rootbuf;
		}
		_bt_relbuf(rel, rootbuf);
		/* Cache is stale, throw it away */
		if (rel->rd_amcache)
			pfree(rel->rd_amcache);
		rel->rd_amcache = NULL;
	}

	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metaopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(metapg);
	metad = BTPageGetMeta(metapg);
	/* sanity-check the metapage */
	if (!(metaopaque->btpo_flags & BTP_META) || metad->btm_magic != BTREE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a btree", RelationGetRelationName(rel))));

	if (metad->btm_version != BTREE_VERSION)
		ereport(
			ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("version mismatch in index \"%s\": file version %u, code version %d",
					RelationGetRelationName(rel), metad->btm_version, BTREE_VERSION)));

	/* if no root page initialized yet, do it */
	if (metad->btm_root == P_NONE)
	{
		XBTRecycleQueueAddress addr;
		/* If access = BT_READ, caller doesn't want us to create root yet */
		if (access == BT_READ)
		{
			_bt_relbuf(rel, metabuf);
			return InvalidBuffer;
		}

		/* trade in our read lock for a write lock */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(metabuf, BT_WRITE);

		/*
         * Race condition:	if someone else initialized the metadata between
         * the time we released the read lock and acquired the write lock, we
         * must avoid doing it again.
         */
		if (metad->btm_root != P_NONE)
		{
			/*
             * Metadata initialized by someone else.  In order to guarantee no
             * deadlocks, we have to release the metadata page and start all
             * over again.	(Is that really true? But it's hardly worth trying
             * to optimize this case.)
             */
			_bt_relbuf(rel, metabuf);
			return _xbt_getroot(rel, access);
		}

		/*
         * Get, initialize, write, and leave a lock of the appropriate type on
         * the new root page.  Since this is the first page in the tree, it's
         * a leaf as well as the root.
         *      NOTE: after the page is absolutely used, call XBTreeRecordUsedPage()
         *            before we release the Exclusive lock.
         */

		rootbuf = _xbt_getnewbuf(rel, &addr);
		rootblkno = BufferGetBlockNumber(rootbuf);
		rootpage = BufferGetPage(rootbuf);
		rootopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(rootpage);
		rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
		rootopaque->btpo_flags = (BTP_LEAF | BTP_ROOT);
		rootopaque->btpo.level = 0;
		rootopaque->btpo_cycleid = 0;

		/* NO ELOG(ERROR) till meta is updated */
		START_CRIT_SECTION();

		metad->btm_root = rootblkno;
		metad->btm_level = 0;
		metad->btm_fastroot = rootblkno;
		metad->btm_fastlevel = 0;

		MarkBufferDirty(rootbuf);
		MarkBufferDirty(metabuf);

		/* XLOG stuff */
		if (RelationNeedsWAL(rel))
		{
			xl_btree_newroot  xlrec;
			xl_btree_metadata md;
			XLogRecPtr		  recptr;

			XLogBeginInsert();
			XLogRegisterBuffer(0, rootbuf, REGBUF_WILL_INIT);
			XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT);

			md.root = rootblkno;
			md.level = 0;
			md.fastroot = rootblkno;
			md.fastlevel = 0;

			XLogRegisterBufData(2, (char *) &md, sizeof(xl_btree_metadata));

			xlrec.rootblk = rootblkno;
			xlrec.level = 0;
			XLogRegisterData((char *) &xlrec, SizeOfBtreeNewroot);

			recptr = XLogInsert(RM_XBTREE_ID, XLOG_XBTREE_NEWROOT);

			PageSetLSN(rootpage, recptr);
			PageSetLSN(metapg, recptr);
		}

		END_CRIT_SECTION();

		/* discard this page from the Recycle Queue */
		xbtree_record_used_page(rel, addr);

		/*
         * swap root write lock for read lock.	There is no danger of anyone
         * else accessing the new root page while it's unlocked, since no one
         * else knows where it is yet.
         */
		LockBuffer(rootbuf, BUFFER_LOCK_UNLOCK);
		LockBuffer(rootbuf, BT_READ);

		/* okay, metadata is correct, release lock on it */
		_bt_relbuf(rel, metabuf);
	}
	else
	{
		rootblkno = metad->btm_fastroot;
		Assert(rootblkno != P_NONE);
		rootlevel = metad->btm_fastlevel;

		/*
         * Cache the metapage data for next time
         */
		rel->rd_amcache = MemoryContextAlloc(rel->rd_indexcxt, sizeof(BTMetaPageData));
		memcpy(rel->rd_amcache, metad,sizeof(BTMetaPageData));

		/*
         * We are done with the metapage; arrange to release it via first
         * _bt_relandgetbuf call
         */
		rootbuf = metabuf;

		for (;;)
		{
			rootbuf = _bt_relandgetbuf(rel, rootbuf, rootblkno, BT_READ);
			rootpage = BufferGetPage(rootbuf);
			rootopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(rootpage);
			if (!P_IGNORE(rootopaque))
				break;

			/* it's dead, Jim.  step right one page */
			if (P_RIGHTMOST(rootopaque))
				ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
								errmsg("no live root page found in index \"%s\"",
									   RelationGetRelationName(rel))));
			rootblkno = rootopaque->btpo_next;
		}

		/* Note: can't check btpo.level on deleted pages */
		if (rootopaque->btpo.level != rootlevel)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("root page %u of index \"%s\" has level %u, expected %u",
							rootblkno, RelationGetRelationName(rel),
							rootopaque->btpo.level, rootlevel)));
	}

	/*
     * By here, we have a pin and read lock on the root page, and no lock set
     * on the metadata page.  Return the root page's buffer.
     */
	return rootbuf;
}

/*
 * xbt_pagedel() -- Delete a page from the b-tree, if legal to do so.
 *
 * This action unlinks the page from the b-tree structure, removing all
 * pointers leading to it --- but not touching its own left and right links.
 * The page cannot be physically reclaimed right away, since other processes
 * may currently be trying to follow links leading to the page; they have to
 * be allowed to use its right-link to recover.  See nbtree/README.
 *
 * On entry, the target buffer must be pinned and locked (either read or write
 * lock is OK).  This lock and pin will be dropped before exiting.
 *
 * Returns the number of pages successfully deleted (zero if page cannot
 * be deleted now; could be more than one if parent or sibling pages were
 * deleted too).
 *
 * NOTE: this leaks memory.  Rather than trying to clean up everything
 * carefully, it's better to run it in a temp context that can be reset
 * frequently.
 */
int
_xbt_pagedel(Relation rel, Buffer buf)
{
	int					  ndeleted = 0;
	BlockNumber			  rightsib;
	bool				  rightsib_empty = false;
	Page				  page;
	XBTPageOpaqueInternal opaque;

	/*
     * "stack" is a search stack leading (approximately) to the target page.
     * It is initially NULL, but when iterating, we keep it to avoid
     * duplicated search effort.
     *
     * Also, when "stack" is not NULL, we have already checked that the
     * current page is not the right half of an incomplete split, i.e. the
     * left sibling does not have its INCOMPLETE_SPLIT flag set.
     */
	BTStack stack = NULL;

	for (;;)
	{
		page = BufferGetPage(buf);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		/*
         * Internal pages are never deleted directly, only as part of deleting
         * the whole branch all the way down to leaf level.
         */
		if (!P_ISLEAF(opaque))
		{
			/*
             * Pre-9.4 page deletion only marked internal pages as half-dead,
             * but now we only use that flag on leaf pages. The old algorithm
             * was never supposed to leave half-dead pages in the tree, it was
             * just a transient state, but it was nevertheless possible in
             * error scenarios. We don't know how to deal with them here. They
             * are harmless as far as searches are considered, but inserts
             * into the deleted keyspace could add out-of-order downlinks in
             * the upper levels. Log a notice, hopefully the admin will notice
             * and reindex.
             */
			if (P_ISHALFDEAD(opaque))
			{
				ereport(LOG, (errcode(ERRCODE_INDEX_CORRUPTED),
							  errmsg("index \"%s\" contains a half-dead internal page",
									 RelationGetRelationName(rel)),
							  errhint("This can be caused by an interrupted VACUUM in "
									  "version 9.3 or older, before upgrade. "
									  "Please REINDEX it.")));
			}
			_bt_relbuf(rel, buf);
			return ndeleted;
		}

		/*
         * We can never delete rightmost pages nor root pages.  While at it,
         * check that page is not already deleted and is empty.
         *
         * To keep the algorithm simple, we also never delete an incompletely
         * split page (they should be rare enough that this doesn't make any
         * meaningful difference to disk usage):
         *
         * The INCOMPLETE_SPLIT flag on the page tells us if the page is the
         * left half of an incomplete split, but ensuring that it's not the
         * right half is more complicated.  For that, we have to check that
         * the left sibling doesn't have its INCOMPLETE_SPLIT flag set.  On
         * the first iteration, we temporarily release the lock on the current
         * page, and check the left sibling and also construct a search stack
         * to.  On subsequent iterations, we know we stepped right from a page
         * that passed these tests, so it's OK.
         */
		if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque) ||
			P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page) ||
			P_INCOMPLETE_SPLIT(opaque))
		{
			/* Should never fail to delete a half-dead page */
			Assert(!P_ISHALFDEAD(opaque));

			_bt_relbuf(rel, buf);
			return ndeleted;
		}

		/*
         * First, remove downlink pointing to the page (or a parent of the
         * page, if we are going to delete a taller branch), and mark the page
         * as half-dead.
         */
		if (!P_ISHALFDEAD(opaque))
		{
			/*
             * We need an approximate pointer to the page's parent page.  We
             * use a variant of the standard search mechanism to search for
             * the page's high key; this will give us a link to either the
             * current parent or someplace to its left (if there are multiple
             * equal high keys, which is possible with !heapkeyspace indexes).
             *
             * Also check if this is the right-half of an incomplete split
             * (see comment above).
             */
			if (!stack)
			{
				BTScanInsert itup_key;
				ItemId		 itemid;
				IndexTuple	 targetkey;
				Buffer		 lbuf;
				BlockNumber	 leftsib;

				itemid = PageGetItemId(page, P_HIKEY);
				targetkey = CopyIndexTuple((IndexTuple) PageGetItem(page, itemid));

				leftsib = opaque->btpo_prev;

				/*
                 * To avoid deadlocks, we'd better drop the leaf page lock
                 * before going further.
                 */
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);

				/*
                 * Fetch the left sibling, to check that it's not marked with
                 * INCOMPLETE_SPLIT flag.  That would mean that the page
                 * to-be-deleted doesn't have a downlink, and the page
                 * deletion algorithm isn't prepared to handle that.
                 */
				if (!P_LEFTMOST(opaque))
				{
					XBTPageOpaqueInternal lopaque;
					Page				  lpage;

					lbuf = _bt_getbuf(rel, leftsib, BT_READ);
					lpage = BufferGetPage(lbuf);
					lopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(lpage);
					/*
                     * If the left sibling is split again by another backend,
                     * after we released the lock, we know that the first
                     * split must have finished, because we don't allow an
                     * incompletely-split page to be split again.  So we don't
                     * need to walk right here.
                     */
					if (lopaque->btpo_next == BufferGetBlockNumber(buf) &&
						P_INCOMPLETE_SPLIT(lopaque))
					{
						ReleaseBuffer(buf);
						_bt_relbuf(rel, lbuf);
						return ndeleted;
					}
					_bt_relbuf(rel, lbuf);
				}

				/* we need an insertion scan key for the search, so build one */
				itup_key = _xbt_mkscankey(rel, targetkey);
				/* find the leftmost leaf page with matching pivot/high key */
				itup_key->backward = true;
				stack = _xbt_search(rel, itup_key, &lbuf, BT_READ, true);
				/* don't need a lock or second pin on the page */
				_bt_relbuf(rel, lbuf);

				/*
                 * Re-lock the leaf page, and start over, to re-check that the
                 * page can still be deleted.
                 */
				LockBuffer(buf, BT_WRITE);
				continue;
			}

			if (!_xbt_mark_page_halfdead(rel, buf, stack))
			{
				_bt_relbuf(rel, buf);
				return ndeleted;
			}
		}

		/*
         * Then unlink it from its siblings.  Each call to
         * _bt_unlink_halfdead_page unlinks the topmost page from the branch,
         * making it shallower.  Iterate until the leaf page is gone.
         */
		rightsib_empty = false;
		while (P_ISHALFDEAD(opaque))
		{
			/* will check for interrupts, once lock is released */
			if (!_xbt_unlink_halfdead_page(rel, buf, &rightsib_empty))
			{
				/* _bt_unlink_halfdead_page already released buffer */
				return ndeleted;
			}
			ndeleted++;
		}

		rightsib = opaque->btpo_next;

		_bt_relbuf(rel, buf);

		/*
         * Check here, as calling loops will have locks held, preventing
         * interrupts from being processed.
         */
		CHECK_FOR_INTERRUPTS();

		/*
         * The page has now been deleted. If its right sibling is completely
         * empty, it's possible that the reason we haven't deleted it earlier
         * is that it was the rightmost child of the parent. Now that we
         * removed the downlink for this page, the right sibling might now be
         * the only child of the parent, and could be removed. It would be
         * picked up by the next vacuum anyway, but might as well try to
         * remove it now, so loop back to process the right sibling.
         */
		if (!rightsib_empty)
		{
			break;
		}

		buf = _bt_getbuf(rel, rightsib, BT_WRITE);
	}

	return ndeleted;
}

/*
 *	XBTreeGetNewPage() -- Allocate a new page.
 *
 *		This routine will allocate a new page from Recycle Queue or extend the
 *		relation.
 *
 *		We will try to found a free page from the freed fork of Recycle Queue, and
 *		extend the relation when there is no free page in Recycle Queue.
 *
 *		addr is a output parameter, it will be set to a valid value if the page
 *		is found from Recycle Queue. This output tells where is the corresponding
 *		page in the Recycle Queue, and we need to call XBTreeRecordUsedPage()
 *		with this addr when the returned page is used correctly.
 */
Buffer
_xbt_getnewbuf(Relation rel, XBTRecycleQueueAddress *addr)
{
	Buffer buf;
	Page   page;
restart:
	buf = xbtree_get_available_page(rel, RECYCLE_FREED_FORK, addr);
	if (buf == InvalidBuffer)
	{
		/*
         * No free page left, need to extend the relation
         *
         * Extend the relation by one page.
         *
         * We have to use a lock to ensure no one else is extending the rel at
         * the same time, else we will both try to initialize the same new
         * page.  We can skip locking for new or temp relations, however,
         * since no one else could be accessing them.
         */
		bool need_lock = !RELATION_IS_LOCAL(rel);
		if (need_lock)
		{
			if (!ConditionalLockRelationForExtension(rel, ExclusiveLock))
			{
				/* couldn't get the lock immediately; wait for it. */
				LockRelationForExtension(rel, ExclusiveLock);
				/* check again, relation may extended by other backends */
				buf = xbtree_get_available_page(rel, RECYCLE_FREED_FORK, addr);
				if (buf != InvalidBuffer)
				{
					UnlockRelationForExtension(rel, ExclusiveLock);
					goto out;
				}
				/* Time to bulk-extend. */
				xbtree_add_extra_blocks(rel, NULL);
			}
		}
		/* extend by one page */
		buf = ReadBuffer(rel, P_NEW);
		// WHITEBOX_TEST_STUB("XBTreeGetNewPage-extend", WhiteboxDefaultErrorEmit);
		if (!ConditionalLockBuffer(buf))
		{
			/* lock failed. To avoid dead lock, we need to retry */
			if (need_lock)
			{
				UnlockRelationForExtension(rel, ExclusiveLock);
			}
			ReleaseBuffer(buf);
			goto restart;
		}
		/*
         * Release the file-extension lock; it's now OK for someone else to
         * extend the relation some more.
         */
		if (need_lock)
			UnlockRelationForExtension(rel, ExclusiveLock);

		/* we have successfully extended the space, get the new page and write lock */
		addr->queue_fuf = InvalidBuffer; /* not allocated from recycle */
	}
out:
	/* buffer is valid, exclusive lock already acquired */
	Assert(BufferIsValid(buf));

	page = BufferGetPage(buf);
	if (!xbtree_page_recyclable(page))
	{
		/* oops, failure due to concurrency, retry. */
		UnlockReleaseBuffer(buf);
		if (BufferIsValid(addr->queue_fuf))
		{
			ReleaseBuffer(addr->queue_fuf);
			addr->queue_fuf = InvalidBuffer;
		}
		goto restart;
	}

	if (addr->queue_fuf != InvalidBuffer)
	{
		/*
         * If we are generating WAL for Hot Standby then create a
         * WAL record that will allow us to conflict with queries
         * running on standby.
         */
		if (XLogStandbyInfoActive() && RelationNeedsWAL(rel))
		{
			XBTPageOpaque opaque = (XBTPageOpaque) PageGetSpecialPointer(page);
			_xbt_log_reuse_page(rel, BufferGetBlockNumber(buf), opaque->xact);
		}
	}
	_xbt_pageinit(page, BufferGetPageSize(buf));
	return buf;
}

static void 
xbtree_add_extra_blocks(Relation relation, BulkInsertState bistate)
{
	int extra_blocks = 0;
	int lock_waiters = RelationExtensionLockWaiterCount(relation);
	if (lock_waiters <= 0)
		return;

	extra_blocks = Min(8, lock_waiters);

	while (extra_blocks-- >= 0)
	{
		/* Ouch - an unnecessary lseek() each time through the loop! */
		Buffer buffer = ReadBufferBI(relation, P_NEW, RBM_NORMAL, bistate);
		/* xbtree don't need to read or write here, and don't use FSM */
		ReleaseBuffer(buffer); /* just release the buffer */
	}
}

/*
 *	xbt_checkpage() -- Verify that a freshly-read page looks sane.
 */
void
_xbt_checkpage(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);
	Size 		_bt_specialsize;

	/*
	 * ReadBuffer verifies that every newly-read page passes
	 * PageHeaderIsValid, which means it either contains a reasonably sane
	 * page header or is all-zero.  We have to defend against the all-zero
	 * case, however.
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains unexpected zero page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	/*
	 * Additionally check that the special area looks sane.
	 */
	_bt_specialsize = MAXALIGN(sizeof(XBTPageOpaqueData));
	if (PageGetSpecialSize(page) != _bt_specialsize)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));
}
