/*-------------------------------------------------------------------------
 *
 * xbtinsert.c
 *	  Item insertion in Lehman and Yao btrees for Postgres.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/xbtree/xbtinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "storage/block.h"
#include "undo/undolog.h"
#include "undo/undotype.h"
#include "undo/undobuffer.h"
#include "xbtree/xbtree.h"
#include "xbtree/xbtvisbility.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/itup.h"
#include "undo/undorecord.h"
#include "util/xxact.h"
#include "xstore.h"
#include "xbtree/xbttup.h"
#include "access/xloginsert.h"
#include "access/genam.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "pgstat.h"
#include "catalog/catalog.h"
#include "access/xact.h"
#include "xstore.h"
#include "undo/undorequest.h"
#include "util/xrmgr.h"

static TransactionId _xbt_check_unique(Relation rel, BTInsertState insertstate,
									  Relation heapRel,
									  IndexUniqueCheck checkUnique, bool *is_unique);
static OffsetNumber _xbt_findinsertloc(Relation rel,
									  BTInsertState insertstate,
									  bool checkingunique,
									  BTStack stack,
									  Relation heapRel);
static OffsetNumber	 _xbt_finddeleteloc(Relation rel, Buffer *bufP, OffsetNumber offset,
										 BTScanInsert itup_key, IndexTuple itup);
static void			 _xbt_insertonpg(Relation rel, BTScanInsert itup_key, Buffer buf,
 										Buffer cbuf, BTStack stack, IndexTuple itup,
 										OffsetNumber newitemoff, bool split_only_page);
static Buffer _xbt_split(Relation rel, BTScanInsert itup_key, Buffer buf,
						Buffer cbuf, OffsetNumber newitemoff, Size newitemsz,
						IndexTuple newitem);
static void _xbt_insert_parent(Relation rel, Buffer buf, Buffer rbuf, BTStack stack, bool is_root, bool is_only);
static bool	  _xbt_pgaddtup(Page page, Size itemsize, IndexTuple itup,
								 OffsetNumber itup_off, FullTransactionId xid, bool isnew, 
								 UndoRecPtr urec, ItemId orgitem);
static void	  _xbt_deleteonpage(Relation rel, Buffer buf, OffsetNumber offset, bool is_dead);
static Buffer _xbt_newroot(Relation rel, Buffer lbuf, Buffer rbuf);

static void _xbt_stepright(Relation rel, BTInsertState insertstate, BTStack stack);

/*
 *	_xbt_doinsert() -- Handle insertion of a single index tuple in the tree.
 *
 *		This routine is called by the public interface routine, xbtinsert.
 *		By here, itup is filled in, including the TID.
 *
 *		If checkUnique is UNIQUE_CHECK_NO or UNIQUE_CHECK_PARTIAL, this
 *		will allow duplicates.  Otherwise (UNIQUE_CHECK_YES or
 *		UNIQUE_CHECK_EXISTING) it will throw error for a duplicate.
 *		For UNIQUE_CHECK_EXISTING we merely run the duplicate check, and
 *		don't actually insert.
 *
 *		The result value is only significant for UNIQUE_CHECK_PARTIAL:
 *		it must be true if the entry is known unique, else false.
 *		(In the current implementation we'll also return true after a
 *		successful UNIQUE_CHECK_YES or UNIQUE_CHECK_EXISTING call, but
 *		that's just a coding artifact.)
 */
bool
_xbt_doinsert(Relation rel, IndexTuple itup,
			 IndexUniqueCheck checkUnique, Relation heapRel)
{
	bool		 is_unique = false;
	BTInsertStateData insertstate;
	BTScanInsert itup_key;
	BTStack		stack = NULL;
	Buffer		buf;
	bool		fastpath;
	bool		checkingunique = (checkUnique != UNIQUE_CHECK_NO);

	OffsetNumber offset;

	/* we need an insertion scan key to do our search, so build one */
	itup_key = _xbt_mkscankey(rel, itup);

	if (checkingunique)
	{
		if (!itup_key->anynullkeys)
		{
			/* No (heapkeyspace) scantid until uniqueness established */
			itup_key->scantid = NULL;
		}
		else
		{
			/*
			 * Scan key for new tuple contains NULL key values.  Bypass
			 * checkingunique steps.  They are unnecessary because core code
			 * considers NULL unequal to every value, including NULL.
			 *
			 * This optimization avoids O(N^2) behavior within the
			 * _bt_findinsertloc() heapkeyspace path when a unique index has a
			 * large number of "duplicates" with NULL key values.
			 */
			checkingunique = false;
			/* Tuple is unique in the sense that core code cares about */
			Assert(checkUnique != UNIQUE_CHECK_EXISTING);
			is_unique = true;
		}
	}

	/*
	 * Fill in the BTInsertState working area, to track the current page and
	 * position within the page to insert on
	 */
	insertstate.itup = itup;
	/* PageAddItem will MAXALIGN(), but be consistent */
	insertstate.itemsz = MAXALIGN(IndexTupleSize(itup));
	insertstate.itup_key = itup_key;
	insertstate.bounds_valid = false;
	insertstate.buf = InvalidBuffer;

	/*
	 * It's very common to have an index on an auto-incremented or
	 * monotonically increasing value. In such cases, every insertion happens
	 * towards the end of the index. We try to optimize that case by caching
	 * the right-most leaf of the index. If our cached block is still the
	 * rightmost leaf, has enough free space to accommodate a new entry and
	 * the insertion key is strictly greater than the first key in this page,
	 * then we can safely conclude that the new key will be inserted in the
	 * cached block. So we simply search within the cached block and insert
	 * the key at the appropriate location. We call it a fastpath.
	 *
	 * Testing has revealed, though, that the fastpath can result in increased
	 * contention on the exclusive-lock on the rightmost leaf page. So we
	 * conditionally check if the lock is available. If it's not available
	 * then we simply abandon the fastpath and take the regular path. This
	 * makes sense because unavailability of the lock also signals that some
	 * other backend might be concurrently inserting into the page, thus
	 * reducing our chances to finding an insertion place in this page.
	 */
top:
	fastpath = false;
	offset = InvalidOffsetNumber;
	if (RelationGetTargetBlock(rel) != InvalidBlockNumber)
	{
		Page		page;
		XBTPageOpaqueInternal lpageop;

		/*
         * Conditionally acquire exclusive lock on the buffer before doing any
		 * checks. If we don't get the lock, we simply follow slowpath. If we
		 * do get the lock, this ensures that the index state cannot change,
		 * as far as the rightmost part of the index is concerned.
         */
		buf = ReadBuffer(rel, RelationGetTargetBlock(rel));

		if (ConditionalLockBuffer(buf))
		{
			_xbt_checkpage(rel, buf);

			page = BufferGetPage(buf);

			lpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
			
			/*
			 * Check if the page is still the rightmost leaf page, has enough
			 * free space to accommodate the new tuple, and the insertion scan
			 * key is strictly greater than the first key on the page.
			 */
			if (P_ISLEAF(lpageop) && P_RIGHTMOST(lpageop) &&
				!P_IGNORE(lpageop) &&
				(PageGetFreeSpace(page) > insertstate.itemsz) &&
				PageGetMaxOffsetNumber(page) >= P_FIRSTDATAKEY(lpageop) &&
				_xbt_compare(rel, itup_key, page, P_FIRSTDATAKEY(lpageop)) > 0)
			{
				/*
				 * The right-most block should never have an incomplete split.
				 * But be paranoid and check for it anyway.
				 */
				Assert(!P_INCOMPLETE_SPLIT(lpageop));
				fastpath = true;
			}
			else
			{
				_bt_relbuf(rel, buf);

				/*
				 * Something did not work out. Just forget about the cached
				 * block and follow the normal path. It might be set again if
				 * the conditions are favourable.
				 */
				RelationSetTargetBlock(rel, InvalidBlockNumber);
			}
		}
		else
		{
			ReleaseBuffer(buf);

			/*
			 * If someone's holding a lock, it's likely to change anyway, so
			 * don't try again until we get an updated rightmost leaf.
			 */
			RelationSetTargetBlock(rel, InvalidBlockNumber);
		}
	}

	if (!fastpath)
	{
		/*
		 * Find the first page containing this key.  Buffer returned by
		 * _xbt_search() is locked in exclusive mode.
		 */
		stack = _xbt_search(rel, itup_key, &buf, BT_WRITE, true);
	}

	insertstate.buf = buf;
	buf = InvalidBuffer;		/* insertstate.buf now owns the buffer */

	/*
	 * If we're not allowing duplicates, make sure the key isn't already in
	 * the index.
	 *
	 * NOTE: obviously, _bt_check_unique can only detect keys that are already
	 * in the index; so it cannot defend against concurrent insertions of the
	 * same key.  We protect against that by means of holding a write lock on
	 * the first page the value could be on, with omitted/-inf value for the
	 * implicit heap TID tiebreaker attribute.  Any other would-be inserter of
	 * the same key must acquire a write lock on the same page, so only one
	 * would-be inserter can be making the check at one time.  Furthermore,
	 * once we are past the check we hold write locks continuously until we
	 * have performed our insertion, so no later inserter can fail to see our
	 * insertion.  (This requires some care in _bt_findinsertloc.)
	 *
	 * If we must wait for another xact, we release the lock while waiting,
	 * and then must start over completely.
	 *
	 * For a partial uniqueness check, we don't wait for the other xact. Just
	 * let the tuple in and return false for possibly non-unique, or true for
	 * definitely unique.
	 */
	if (checkingunique)
	{
		TransactionId xwait;

		xwait = _xbt_check_unique(rel, &insertstate, heapRel, checkUnique,
								  &is_unique);
		if (unlikely(TransactionIdIsValid(xwait)))
		{
			/* Have to wait for the other guy ... */
			_bt_relbuf(rel, insertstate.buf);
			insertstate.buf = InvalidBuffer;

			XactLockTableWait(xwait, rel, &itup->t_tid, XLTW_InsertIndex);
			/* start over... */
			if (stack)
				_bt_freestack(stack);
			goto top;
		}

		/* Uniqueness is established -- restore heap tid as scantid */
		if (itup_key->heapkeyspace)
			itup_key->scantid = &itup->t_tid;
	}

	if (checkUnique != UNIQUE_CHECK_EXISTING)
	{
		/*
		 * The only conflict predicate locking cares about for indexes is when
		 * an index tuple insert conflicts with an existing lock.  We don't
		 * know the actual page we're going to insert on for sure just yet in
		 * checkingunique and !heapkeyspace cases, but it's okay to use the
		 * first page the value could be on (with scantid omitted) instead.
		 */
		CheckForSerializableConflictIn(rel, NULL, insertstate.buf);
		/*
		 * Do the insertion.  Note that insertstate contains cached binary
		 * search bounds established within _bt_check_unique when insertion is
		 * checkingunique.
		 */
		offset = _xbt_findinsertloc(rel, &insertstate, checkingunique,
									 stack, heapRel);
		_xbt_insertonpg(rel, itup_key, insertstate.buf, InvalidBuffer, stack, itup, offset, false);
	}
	else
	{
		/* just release the buffer */
		_bt_relbuf(rel, insertstate.buf);
	}

	/* be tidy */
	if (stack)
		_bt_freestack(stack);
	pfree(itup_key);

	return is_unique;
}

/*
 *	_xbt_check_unique() -- Check for violation of unique index constraint
 *
 * Returns InvalidTransactionId if there is no conflict, else an xact ID
 * we must wait for to see if it commits a conflicting tuple.   If an actual
 * conflict is detected, no return --- just ereport().  If an xact ID is
 * returned, and the conflicting tuple still has a speculative insertion in
 * progress, *speculativeToken is set to non-zero, and the caller can wait for
 * the verdict on the insertion using SpeculativeInsertionWait().
 *
 * However, if checkUnique == UNIQUE_CHECK_PARTIAL, we always return
 * InvalidTransactionId because we don't want to wait.  In this case we
 * set *is_unique to false if there is a potential conflict, and the
 * core code must redo the uniqueness check later.
 *
 * As a side-effect, sets state in insertstate that can later be used by
 * _bt_findinsertloc() to reuse most of the binary search work we do
 * here.
 *
 * Do not call here when there are NULL values in scan key.  NULL should be
 * considered unequal to NULL when checking for duplicates, but we are not
 * prepared to handle that correctly.
 */
static TransactionId
_xbt_check_unique(Relation rel, BTInsertState insertstate, Relation heapRel,
				  IndexUniqueCheck checkUnique, bool *is_unique)
{
	IndexTuple	itup = insertstate->itup;
	BTScanInsert itup_key = insertstate->itup_key;
	OffsetNumber offset;
	OffsetNumber maxoff;
	Page		page;
	XBTPageOpaqueInternal opaque;
	Buffer		nbuf = InvalidBuffer;
	bool		found = false;

	/* Assume unique until we find a duplicate */
	*is_unique = true;

	page = BufferGetPage(insertstate->buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);
	
	/*
	 * Find the first tuple with the same key.
	 *
	 * This also saves the binary search bounds in insertstate.  We use them
	 * in the fastpath below, but also in the _xbt_findinsertloc() call later.
	 */
	Assert(!insertstate->bounds_valid);
	offset = _xbt_binsrch_insert(rel, insertstate);

	/*
	 * Scan over all equal tuples, looking for live conflicts.
	 */
	Assert(!insertstate->bounds_valid || insertstate->low == offset);
	Assert(!itup_key->anynullkeys);
	Assert(itup_key->scantid == NULL);
	for (;;)
	{
		ItemId		curitemid;
		IndexTuple	curitup;
		BlockNumber nblkno;

		/*
		 * make sure the offset points to an actual item before trying to
		 * examine it...
		 */
		if (offset <= maxoff)
		{
			/*
			 * Fastpath: In most cases, we can use cached search bounds to
			 * limit our consideration to items that are definitely
			 * duplicates.  This fastpath doesn't apply when the original page
			 * is empty, or when initial offset is past the end of the
			 * original page, which may indicate that we need to examine a
			 * second or subsequent page.
			 *
			 * Note that this optimization allows us to avoid calling
			 * _xbt_compare() directly when there are no duplicates, as long
			 * as the offset where the key will go is not at the end of the page.
			 */
			if (nbuf == InvalidBuffer && offset == insertstate->stricthigh)
			{
				Assert(insertstate->bounds_valid);
				Assert(insertstate->low >= P_FIRSTDATAKEY(opaque));
				Assert(insertstate->low <= insertstate->stricthigh);
				Assert(_xbt_compare(rel, itup_key, page, offset) < 0);
				break;
			}

			curitemid = PageGetItemId(page, offset);

			/*
			 * We can skip items that are marked killed.
			 *
			 * In the presence of heavy update activity an index may contain
			 * many killed items with the same key; running _bt_compare() on
			 * each killed item gets expensive.  Just advance over killed
			 * items as quickly as we can.  We only apply _bt_compare() when
			 * we get to a non-killed item.  Even those comparisons could be
			 * avoided (in the common case where there is only one page to
			 * visit) by reusing bounds, but just skipping dead items is fast
			 * enough.
			 */
			if (!ItemIdIsDead(curitemid))
			{
				ItemPointerData htid;

				if (_xbt_compare(rel, itup_key, page, offset) != 0)
					break;		/* we're past all the equal tuples */

				/* okay, we gotta fetch the heap tuple ... */
				curitup = (IndexTuple) PageGetItem(page, curitemid);
				htid = curitup->t_tid;

				/*
				 * If we are doing a recheck, we expect to find the tuple we
				 * are rechecking.  It's not a duplicate, but we have to keep
				 * scanning.
                 */
				if (checkUnique == UNIQUE_CHECK_EXISTING &&
					ItemPointerCompare(&htid, &itup->t_tid) == 0)
				{
					found = true;
				}
				else
				{
					SnapshotData snapshot_dirty;
					bool is_visible = false;
					TransactionId xwait;
					InitDirtySnapshot(snapshot_dirty);
					snapshot_dirty.xmin = snapshot_dirty.xmax = InvalidTransactionId;

					/*
                     * we have to return the xid in order to tell the caller to wait for the xid.
                     */
					is_visible = _xbt_tuple_satisfies(page, &snapshot_dirty, InvalidBlockNumber, offset);
					/*
					 * If this tuple is being updated by other transaction
					 * then we have to wait for its commit/abort.
					 */
					xwait = (TransactionIdIsValid(snapshot_dirty.xmin)) ?
						snapshot_dirty.xmin : snapshot_dirty.xmax;

					/* here we guarantee the same semantics as UNIQUE_CHECK_PARTIAL */
					if (checkUnique == UNIQUE_CHECK_PARTIAL)
					{
						if (is_visible)
						{
							if (nbuf != InvalidBuffer)
								_bt_relbuf(rel, nbuf);
							*is_unique = false;
							return InvalidTransactionId;
						}
					}

					if (is_visible && !TransactionIdIsValid(xwait))
					{
						Datum values[INDEX_MAX_KEYS];
						bool  isnull[INDEX_MAX_KEYS];
						char *key_desc = NULL;

						index_deform_tuple(itup, RelationGetDescr(rel), values, isnull);

						key_desc = BuildIndexValueDescription(rel, values, isnull);

						/* save the primary scene once index relation of CU Descriptor
                         * violates unique constraint. this means wrong max cu id happens.
                         */
						Assert(memcmp(NameStr(rel->rd_rel->relname), "pg_cudesc_",
									  strlen("pg_cudesc_")) != 0);
						
						ereport(
							ERROR,
							(errcode(ERRCODE_UNIQUE_VIOLATION),
							 errmsg(
								 "duplicate key value violates unique constraint \"%s\"",
								 RelationGetRelationName(rel)),
							 key_desc ? errdetail("Key %s already exists.", key_desc)
									  : 0));
					}

					if (!is_visible)
						goto next;

					if (nbuf != InvalidBuffer)
						_bt_relbuf(rel, nbuf);
					insertstate->bounds_valid = false;
					return xwait;
				}
			}
		}

	next:
		/*
         * Advance to next tuple to continue checking.
         */
		if (offset < maxoff)
			offset = OffsetNumberNext(offset);
		else
		{
			int highkeycmp;
			/* If scankey == hikey we gotta check the next page too */
			if (P_RIGHTMOST(opaque))
				break;
			highkeycmp = _xbt_compare(rel, itup_key, page, P_HIKEY);
			Assert(highkeycmp <= 0);
			if (highkeycmp != 0)
				break;
			/* Advance to next non-dead page --- there must be one */
			for (;;)
			{
				nblkno = opaque->btpo_next;
				nbuf = _bt_relandgetbuf(rel, nbuf, nblkno, BT_READ);
				page = BufferGetPage(nbuf);
				opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
				if (!P_IGNORE(opaque))
					break;
				if (P_RIGHTMOST(opaque))
					ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
									errmsg("fell off the end of index \"%s\" at blkno %u",
										   RelationGetRelationName(rel), nblkno)));
			}
			maxoff = PageGetMaxOffsetNumber(page);
			offset = P_FIRSTDATAKEY(opaque);
		}
	}

	/*
     * If we are doing a recheck then we should have found the tuple we are
     * checking.  Otherwise there's something very wrong --- probably, the
     * index is on a non-immutable expression.
     */
	if (checkUnique == UNIQUE_CHECK_EXISTING && !found)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("failed to re-find tuple within index \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("This may be because of a non-immutable index expression.")));

	if (nbuf != InvalidBuffer)
		_bt_relbuf(rel, nbuf);

	return InvalidTransactionId;
}

/*
 *	_xbt_findinsertloc() -- Finds an insert location for a tuple
 *
 *		On entry, insertstate buffer contains the page the new tuple belongs
 *		on.  It is exclusive-locked and pinned by the caller.
 *
 *		If 'checkingunique' is true, the buffer on entry is the first page
 *		that contains duplicates of the new key.  If there are duplicates on
 *		multiple pages, the correct insertion position might be some page to
 *		the right, rather than the first page.  In that case, this function
 *		moves right to the correct target page.
 *
 *		(In a !heapkeyspace index, there can be multiple pages with the same
 *		high key, where the new tuple could legitimately be placed on.  In
 *		that case, the caller passes the first page containing duplicates,
 *		just like when checkingunique=true.  If that page doesn't have enough
 *		room for the new tuple, this function moves right, trying to find a
 *		legal page that does.)
 *
 *		On exit, insertstate buffer contains the chosen insertion page, and
 *		the offset within that page is returned.  If _xbt_findinsertloc needed
 *		to move right, the lock and pin on the original page are released, and
 *		the new buffer is exclusively locked and pinned instead.
 *
 *		If insertstate contains cached binary search bounds, we will take
 *		advantage of them.  This avoids repeating comparisons that we made in
 *		_xbt_check_unique() already.
 *
 *		If there is not enough room on the page for the new tuple, we try to
 *		make room by removing any LP_DEAD tuples.
 */
static OffsetNumber
_xbt_findinsertloc(Relation rel,
				  BTInsertState insertstate,
				  bool checkingunique,
				  BTStack stack,
				  Relation heapRel)
{
	BTScanInsert itup_key = insertstate->itup_key;
	Page		page = BufferGetPage(insertstate->buf);
	XBTPageOpaqueInternal  lpageop;

	lpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);


	/* Check 1/3 of a page restriction */
	if (unlikely(insertstate->itemsz > XBTMaxItemSize(page)))
		_xbt_check_third_page(rel, heapRel, itup_key->heapkeyspace, page,
							 insertstate->itup);

	Assert(P_ISLEAF(lpageop) && !P_INCOMPLETE_SPLIT(lpageop));
	Assert(!insertstate->bounds_valid || checkingunique);
	Assert(!itup_key->heapkeyspace || itup_key->scantid != NULL);
	Assert(itup_key->heapkeyspace || itup_key->scantid == NULL);

	/* heapkeyspace always set in xbtree */
	Assert(itup_key->heapkeyspace);
	
	/*
	 * If we're inserting into a unique index, we may have to walk right
	 * through leaf pages to find the one leaf page that we must insert on
	 * to.
	 *
	 * This is needed for checkingunique callers because a scantid was not
	 * used when we called _xbt_search().  scantid can only be set after
	 * _xbt_check_unique() has checked for duplicates.  The buffer
	 * initially stored in insertstate->buf has the page where the first
	 * duplicate key might be found, which isn't always the page that new
	 * tuple belongs on.  The heap TID attribute for new tuple (scantid)
	 * could force us to insert on a sibling page, though that should be
	 * very rare in practice.
	 */
	if (checkingunique)
	{
		for (;;)
		{
			/*
				* Does the new tuple belong on this page?
				*
				* The earlier _bt_check_unique() call may well have
				* established a strict upper bound on the offset for the new
				* item.  If it's not the last item of the page (i.e. if there
				* is at least one tuple on the page that goes after the tuple
				* we're inserting) then we know that the tuple belongs on
				* this page.  We can skip the high key check.
				*/
			if (insertstate->bounds_valid &&
				insertstate->low <= insertstate->stricthigh &&
				insertstate->stricthigh <= PageGetMaxOffsetNumber(page))
				break;

			/* Test '<=', not '!=', since scantid is set now */
			if (P_RIGHTMOST(lpageop) ||
				_xbt_compare(rel, itup_key, page, P_HIKEY) <= 0)
				break;

			_xbt_stepright(rel, insertstate, stack);
			/* Update local state after stepping right */
			page = BufferGetPage(insertstate->buf);
			lpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		}
	}

	/*
     * If the target page is full, see if we can obtain enough space by:
     *      (a) pruning this page if we have multi-version info.
     *      (b) erasing LP_DEAD items.
     */
	if (PageGetFreeSpace(page) < insertstate->itemsz)
	{
		if (P_ISLEAF(lpageop))
		{
			if (xbt_page_prune_opt(rel, insertstate->buf, false))
				insertstate->bounds_valid = false;
		}
	}

	/*
	 * We should now be on the correct page.  Find the offset within the page
	 * for the new tuple. (Possibly reusing earlier search bounds.)
	 */
	Assert(P_RIGHTMOST(lpageop) ||
		   _bt_compare(rel, itup_key, page, P_HIKEY) <= 0);

	/* otherwise, we need to call _bt_binsrch() again with TID to find the insertion location */
	return _xbt_binsrch_insert(rel, insertstate);
}

/*----------
 *	_xbt_insertonpg() -- Insert a tuple on a particular page in the index.
 *
 *		This recursive procedure does the following things:
 *
 *			+  if necessary, splits the target page, using 'itup_key' for
 *			   suffix truncation on leaf pages (caller passes NULL for
 *			   non-leaf pages).
 *			+  inserts the tuple.
 *			+  if the page was split, pops the parent stack, and finds the
 *			   right place to insert the new child pointer (by walking
 *			   right using information stored in the parent stack).
 *			+  invokes itself with the appropriate tuple for the right
 *			   child page on the parent.
 *			+  updates the metapage if a true root or fast root is split.
 *
 *		On entry, we must have the correct buffer in which to do the
 *		insertion, and the buffer must be pinned and write-locked.  On return,
 *		we will have dropped both the pin and the lock on the buffer.
 *
 *		This routine only performs retail tuple insertions.  'itup' should
 *		always be either a non-highkey leaf item, or a downlink (new high
 *		key items are created indirectly, when a page is split).  When
 *		inserting to a non-leaf page, 'cbuf' is the left-sibling of the page
 *		we're inserting the downlink for.  This function will clear the
 *		INCOMPLETE_SPLIT flag on it, and release the buffer.
 *----------
 */
static void
_xbt_insertonpg(Relation rel,
			   BTScanInsert itup_key,
			   Buffer buf,
			   Buffer cbuf,
			   BTStack stack,
			   IndexTuple itup,
			   OffsetNumber newitemoff,
			   bool split_only_page)
{
	Page				  page;
	XBTPageOpaqueInternal lpageop;
	Size				  itemsz;

	page = BufferGetPage(buf);
	lpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	/* child buffer must be given iff inserting on an internal page */
	Assert(P_ISLEAF(lpageop) == !BufferIsValid(cbuf));
	/* tuple must have appropriate number of attributes */
	Assert(!P_ISLEAF(lpageop) ||
		   XBTreeTupleGetNAtts(itup, rel) ==
		   IndexRelationGetNumberOfAttributes(rel));
	Assert(P_ISLEAF(lpageop) ||
		   XBTreeTupleGetNAtts(itup, rel) <=
		   IndexRelationGetNumberOfKeyAttributes(rel));

	/* The caller should've finished any incomplete splits already. */
	if (P_INCOMPLETE_SPLIT(lpageop))
		elog(ERROR, "cannot insert to incompletely split page %u",
			 BufferGetBlockNumber(buf));

	itemsz = IndexTupleSize(itup);
	itemsz = MAXALIGN(itemsz); /* be safe, PageAddItem will do this but we
								* need to be consistent */

	/*
	 * Do we need to split the page to fit the item on it?
	 *
	 * Note: PageGetFreeSpace() subtracts sizeof(ItemIdData) from its result,
	 * so this comparison is correct even though we appear to be accounting
	 * only for the item and not for its line pointer.
     */
	if (PageGetFreeSpace(page) < itemsz)
	{
		bool   is_root = P_ISROOT(lpageop);
		bool   is_only = P_LEFTMOST(lpageop) && P_RIGHTMOST(lpageop);
		Buffer rbuf;

		/*
		 * If we're here then a pagesplit is needed. We should never reach
		 * here if we're using the fastpath since we should have checked for
		 * all the required conditions, including the fact that this page has
		 * enough freespace. Note that this routine can in theory deal with
		 * the situation where a NULL stack pointer is passed (that's what
		 * would happen if the fastpath is taken). But that path is much
		 * slower, defeating the very purpose of the optimization.  The
		 * following assertion should protect us from any future code changes
		 * that invalidate those assumptions.
		 *
		 * Note that whenever we fail to take the fastpath, we clear the
		 * cached block. Checking for a valid cached block at this point is
		 * enough to decide whether we're in a fastpath or not.
		 */
		Assert(!(P_ISLEAF(lpageop) &&
				 BlockNumberIsValid(RelationGetTargetBlock(rel))));

		/* split the buffer into left and right halves */
		rbuf = _xbt_split(rel, itup_key, buf, cbuf, newitemoff, itemsz, itup);
		PredicateLockPageSplit(rel,
							   BufferGetBlockNumber(buf),
							   BufferGetBlockNumber(rbuf));

		/*----------
		 * By here,
		 *
		 *		+  our target page has been split;
		 *		+  the original tuple has been inserted;
		 *		+  we have write locks on both the old (left half)
		 *		   and new (right half) buffers, after the split; and
		 *		+  we know the key we want to insert into the parent
		 *		   (it's the "high key" on the left child page).
		 *
		 * We're ready to do the parent insertion.  We need to hold onto the
		 * locks for the child pages until we locate the parent, but we can
		 * at least release the lock on the right child before doing the
		 * actual insertion.  The lock on the left child will be released
		 * last of all by parent insertion, where it is the 'cbuf' of parent
		 * page.
		 *----------
		 */
		_xbt_insert_parent(rel, buf, rbuf, stack, is_root, is_only);

		/* xbtree will try to recycle one page after allocate one page */
		xbtree_try_recycle_empty_page(rel);
	}
	else
	{
		Buffer				   metabuf = InvalidBuffer;
		Page				   metapg = NULL;
		BTMetaPageData		  *metad = NULL;
		OffsetNumber		   itup_off;
		BlockNumber			   itup_blkno;
		FullTransactionId	   xid;
		xl_undo_meta		   xlum;
		FullTransactionId	   sub_xid = InvalidFullTransactionId;
		UndoRecPtr			   prev_urecptr = INVALID_UNDO_REC_PTR;
		UndoRecPtr			   urec_ptr = INVALID_UNDO_REC_PTR;
		UndoPersistence		   persistence;
		Oid					   reloid; 
		UnpackedUndoRecord	  *undorec;
		UndoRecPtr			   old_prev_urp;

		init_xlog_undo_meta(&xlum);
		reloid = InvalidOid;
		persistence = -1;
		undorec = NULL;

		if (undo_cache_ctx->undo_prepare_buffers)
			reset_undo_prepare_buffers(undo_cache_ctx->undo_prepare_buffers, false);

		itup_off = newitemoff;
		itup_blkno = BufferGetBlockNumber(buf);

		/*
         * If we are doing this insert because we split a page that was the
         * only one on its tree level, but was not the root, it may have been
         * the "fast root".  We need to ensure that the fast root link points
         * at or above the current page.  We can safely acquire a lock on the
         * metapage here --- see comments for _bt_newroot().
         */
		if (split_only_page)
		{
			Assert(!P_ISLEAF(lpageop));

			metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
			metapg = BufferGetPage(metabuf);
			metad = BTPageGetMeta(metapg);
			if (metad->btm_fastlevel >= lpageop->btpo.level)
			{
				/* no update wanted */
				_bt_relbuf(rel, metabuf);
				metabuf = InvalidBuffer;
			}
		}
		xid = GetTopFullTransactionId();

		if (IsSubTransaction())
			sub_xid = GetCurrentFullTransactionId();

		if(P_ISLEAF(lpageop))
		{
			/* Prepare Undo record before buffer lock since undo record length is fixed */
			persistence = UndoPersistenceForRelation(rel);
			reloid = RelationGetRelid(rel);
			urec_ptr = _xbt_prepare_undo_insert(
						reloid, rel->rd_rel->relfilenode, rel->rd_locator.spcOid, 
						persistence, xid, GetCurrentCommandId(true), itup, 
						prev_urecptr, INVALID_UNDO_REC_PTR, 
						BufferGetBlockNumber(buf), NULL, NULL, &xlum, sub_xid);
			
			/* XLOG stuff */
			if (RelationNeedsWAL(rel))
			{
				/* 3 xbtree buffer + 1 undo slot buffer */
				int	used_buffers = prepare_buffers_used_count(undo_cache_ctx->undo_prepare_buffers) + 4;
				if (used_buffers - 1 > XLR_NORMAL_MAX_BLOCK_ID)
					XLogEnsureRecordSpace(used_buffers - 1, 0);
			}
		}
		
		/* Do the update.  No ereport(ERROR) until changes are logged */
		START_CRIT_SECTION();
		if (!_xbt_pgaddtup(page, itemsz, itup, newitemoff, xid, true, urec_ptr, NULL))
			ereport(PANIC, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("failed to add new item to block %u in index \"%s\"",
								   itup_blkno, RelationGetRelationName(rel))));

		if(P_ISLEAF(lpageop))
		{
			/* Update the UnpackedUndoRecord now that we know where the tuple is located on the Page */
			undorec = prepare_buffers_get_undorecord((undo_cache_ctx->undo_prepare_buffers), 0);
			SetUndoRecordOffset(undorec, newitemoff);

			/* Insert the Undo record into the undo store */
			insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, 0);

			old_prev_urp = get_current_tansaction_undorec_ptr(persistence);
			set_current_tansaction_undorec_ptr(urec_ptr, persistence);
		}

		/* update active hint */
		lpageop->active_count++;
		MarkBufferDirty(buf);

		if (BufferIsValid(metabuf))
		{
			metad->btm_fastroot = itup_blkno;
			metad->btm_fastlevel = lpageop->btpo.level;
			MarkBufferDirty(metabuf);
		}

		/* clear INCOMPLETE_SPLIT flag on child if inserting a downlink */
		if (BufferIsValid(cbuf))
		{
			Page				  cpage = BufferGetPage(cbuf);
			XBTPageOpaqueInternal cpageop =
				(XBTPageOpaqueInternal) PageGetSpecialPointer(cpage);
			Assert(P_INCOMPLETE_SPLIT(cpageop));
			cpageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
			MarkBufferDirty(cbuf);
		}
		
		if(P_ISLEAF(lpageop))
		{
			update_undolog_meta(xid, prepare_buffers_get_first_undoptr(undo_cache_ctx->undo_prepare_buffers),
						 &xlum, persistence, 
						 prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
						 prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));
		}
		
		/* XLOG stuff */
		if (RelationNeedsWAL(rel))
		{
			xl_btree_insert	  xlrec;
			xl_btree_metadata xlmeta;
			uint8			  xlinfo;
			XLogRecPtr		  recptr;
			IndexTupleData	  trunctuple;
			/*wal undo*/
			uint8 xlundoheader_flag = 0; 
			FullTransactionId top_xid = InvalidFullTransactionId;
			xl_undo_header  xlundohdr;

			if (P_ISLEAF(lpageop))
			{
				if (prev_urecptr != INVALID_UNDO_REC_PTR)
					xlundoheader_flag |= XLOG_UNDO_HEADER_HAS_BLK_PREV;
				if ((GetUndoRecordUinfo(undorec) & UREC_INFO_PREURP) != 0)
					xlundoheader_flag |= XLOG_UNDO_HEADER_HAS_PREV_URP;

				if (IsSubTransaction())
				{
					xlundoheader_flag |= XLOG_UNDO_HEADER_HAS_TOP_XID;
					top_xid = GetTopFullTransactionIdIfAny();
				}
				/* undo meta information */
				Assert((UndoLogControl *) undo_sys_ctx->ulogs[undo_log_ctx->logs[persistence]] != NULL);

				xlundohdr.relOid = reloid;
				xlundohdr.urecptr = urec_ptr;
				xlundohdr.flag = xlundoheader_flag;
			}

			xlrec.offnum = itup_off;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, SizeOfBtreeInsert);

			if (P_ISLEAF(lpageop))
			{
				xlinfo = XLOG_XBTREE_INSERT_LEAF;

				/*
                 * Cache the block information if we just inserted into the
                 * rightmost leaf page of the index.
                */
				if (P_RIGHTMOST(lpageop))
					RelationSetTargetBlock(rel, BufferGetBlockNumber(buf));
			}
			else
			{
				/*
                 * Register the left child whose INCOMPLETE_SPLIT flag was
                 * cleared.
                 */
				XLogRegisterBuffer(1, cbuf, REGBUF_STANDARD);
				xlinfo = XLOG_XBTREE_INSERT_UPPER;
			}

			if (BufferIsValid(metabuf))
			{
				xlmeta.root = metad->btm_root;
				xlmeta.level = metad->btm_level;
				xlmeta.fastroot = metad->btm_fastroot;
				xlmeta.fastlevel = metad->btm_fastlevel;

				XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);
				XLogRegisterBufData(2, (char *) &xlmeta, sizeof(xl_btree_metadata));
				xlinfo = XLOG_XBTREE_INSERT_META;
			}

			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
			if (!P_ISLEAF(lpageop) && newitemoff == P_FIRSTDATAKEY(lpageop))
			{
				trunctuple = *itup;
				trunctuple.t_info = sizeof(IndexTupleData);
				XLogRegisterBufData(0, (char *) &trunctuple, sizeof(IndexTupleData));
			}
			else
			{
				ItemId iid = PageGetItemId(page, itup_off);
				XLogRegisterBufData(0, (char *) PageGetItem(page, iid), itemsz);
			}

			if(P_ISLEAF(lpageop))
			{
				Size itupSize = XstoreIndexTupleSize(itup);
				XLogRegisterData((char *) &xlundohdr, SizeOfXLUndoHeader);
				if ((xlundoheader_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
				{
					ereport(DEBUG5, (errcode(ERRCODE_DATA_EXCEPTION),
									errmsg("blkprev=%lu", INVALID_UNDO_REC_PTR)));
					Assert(prev_urecptr != INVALID_UNDO_REC_PTR);
					XLogRegisterData((char *) &(prev_urecptr), sizeof(UndoRecPtr));
				}

				if ((xlundoheader_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
					XLogRegisterData((char *) &(old_prev_urp), sizeof(UndoRecPtr));

				if ((xlundoheader_flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
					XLogRegisterData((char *) &(top_xid), sizeof(FullTransactionId));

				// add indextupe size and data
				XLogRegisterData((char *) &(itupSize), sizeof(Size));
				XLogRegisterData((char *) itup, itupSize);
				xlog_undo_meta_write(&xlum);

				xlog_register_undo_buffers(undo_cache_ctx->undo_prepare_buffers, 3,
					(UndoLogControl *) undo_sys_ctx->ulogs[undo_log_ctx->logs[persistence]]);
			}
			
			recptr = XLogInsert(RM_XBTREE_ID, xlinfo);

			if (BufferIsValid(metabuf))
				PageSetLSN(metapg, recptr);
			if (BufferIsValid(cbuf))
				PageSetLSN(BufferGetPage(cbuf), recptr);
			PageSetLSN(page, recptr);
			if(P_ISLEAF(lpageop))
			{
				prepare_buffers_set_page_lsn(undo_cache_ctx->undo_prepare_buffers, recptr);
				set_undolog_meta_lsn(recptr);
			}
		}
		if(P_ISLEAF(lpageop))
		{
			release_undolog_meta(persistence);
			reset_prepared_buffers_in_ctx();
		}
		
		END_CRIT_SECTION();
		/* release buffers */
		if (BufferIsValid(metabuf))
			_bt_relbuf(rel, metabuf);
		if (BufferIsValid(cbuf))
			_bt_relbuf(rel, cbuf);
		_bt_relbuf(rel, buf);
	}
}

/*
 * Step right to next non-dead page, during insertion.
 *
 * This is a bit more complicated than moving right in a search.  We must
 * write-lock the target page before releasing write lock on current page;
 * else someone else's _bt_check_unique scan could fail to see our insertion.
 * Write locks on intermediate dead pages won't do because we don't know when
 * they will get de-linked from the tree.
 *
 * This is more aggressive than it needs to be for non-unique !heapkeyspace
 * indexes.
 */
static void
_xbt_stepright(Relation rel, BTInsertState insertstate, BTStack stack)
{
	Page		page;
	XBTPageOpaqueInternal lpageop;
	Buffer		rbuf;
	BlockNumber rblkno;

	page = BufferGetPage(insertstate->buf);
	lpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	rbuf = InvalidBuffer;
	rblkno = lpageop->btpo_next;
	for (;;)
	{
		rbuf = _bt_relandgetbuf(rel, rbuf, rblkno, BT_WRITE);
		page = BufferGetPage(rbuf);
		lpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

		/*
		 * If this page was incompletely split, finish the split now.  We do
		 * this while holding a lock on the left sibling, which is not good
		 * because finishing the split could be a fairly lengthy operation.
		 * But this should happen very seldom.
		 */
		if (P_INCOMPLETE_SPLIT(lpageop))
		{
			_xbt_finish_split(rel, rbuf, stack);
			rbuf = InvalidBuffer;
			continue;
		}

		if (!P_IGNORE(lpageop))
			break;
		if (P_RIGHTMOST(lpageop))
			elog(ERROR, "fell off the end of index \"%s\"",
				 RelationGetRelationName(rel));

		rblkno = lpageop->btpo_next;
	}
	/* rbuf locked; unlock buf, update state for caller */
	_bt_relbuf(rel, insertstate->buf);
	insertstate->buf = rbuf;
	insertstate->bounds_valid = false;
}

/*
 *	_xbt_split() -- split a page in the btree.
 *
 *		On entry, buf is the page to split, and is pinned and write-locked.
 *		newitemoff etc. tell us about the new item that must be inserted
 *		along with the data from the original page.
 *
 *		itup_key is used for suffix truncation on leaf pages (internal
 *		page callers pass NULL).  When splitting a non-leaf page, 'cbuf'
 *		is the left-sibling of the page we're inserting the downlink for.
 *		This function will clear the INCOMPLETE_SPLIT flag on it, and
 *		release the buffer.
 *
 *		Returns the new right sibling of buf, pinned and write-locked.
 *		The pin and lock on buf are maintained.
 */
static Buffer
_xbt_split(Relation rel, BTScanInsert itup_key, Buffer buf, Buffer cbuf,
			OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem)
{
	Buffer				  rbuf;
	Page				  origpage;
	Page				  leftpage, rightpage;
	BlockNumber			  origpagenumber, rightpagenumber;
	XBTPageOpaqueInternal ropaque, lopaque, oopaque;
	Buffer				  sbuf = InvalidBuffer;
	Page				  spage = NULL;
	XBTPageOpaqueInternal sopaque = NULL;
	Size				  itemsz;
	ItemId				  itemid;
	IndexTuple			  item;
	OffsetNumber		  leftoff, rightoff;
	OffsetNumber newitemleftoff = InvalidOffsetNumber;
	OffsetNumber newitemrightoff = InvalidOffsetNumber;
	OffsetNumber firstright;
	OffsetNumber		  maxoff;
	OffsetNumber		  i;
	bool		newitemonleft;
	bool				  isleaf = false;
	IndexTuple			  lefthikey;
	bool				  itup_undo = false;
	FullTransactionId	  xid;
	UndoRecPtr			  urec_ptr = INVALID_UNDO_REC_PTR;
	UndoPersistence		  persistence;
	Oid					  reloid; 
	xl_undo_meta		  xlum;
	UndoRecPtr			  prev_urec_ptr = INVALID_UNDO_REC_PTR;
	UnpackedUndoRecord			 *undorec = NULL;
	UndoRecPtr			  oldPrevUrp;
	FullTransactionId     sub_xid = InvalidFullTransactionId;
	int			indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	XBTRecycleQueueAddress addr;

	/*
	 * origpage is the original page to be split.  leftpage is a temporary
	 * buffer that receives the left-sibling data, which will be copied back
	 * into origpage on success.  rightpage is the new page that will receive
	 * the right-sibling data.
	 *
	 * leftpage is allocated after choosing a split point.  rightpage's new
	 * buffer isn't acquired until after leftpage is initialized and has new
	 * high key, the last point where splitting the page may fail (barring
	 * corruption).  Failing before acquiring new buffer won't have lasting
	 * consequences, since origpage won't have been modified and leftpage is
	 * only workspace.
	 */
	origpage = BufferGetPage(buf);
	oopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(origpage);
	origpagenumber = BufferGetBlockNumber(buf);

	/*
	 * Choose a point to split origpage at.
	 *
	 * A split point can be thought of as a point _between_ two existing
	 * tuples on origpage (lastleft and firstright tuples), provided you
	 * pretend that the new item that didn't fit is already on origpage.
	 *
	 * Since origpage does not actually contain newitem, the representation of
	 * split points needs to work with two boundary cases: splits where
	 * newitem is lastleft, and splits where newitem is firstright.
	 * newitemonleft resolves the ambiguity that would otherwise exist when
	 * newitemoff == firstright.  In all other cases it's clear which side of
	 * the split every tuple goes on from context.  newitemonleft is usually
	 * (but not always) redundant information.
	 */
	firstright = _xbt_findsplitloc(rel, origpage, newitemoff, newitemsz,
									newitem, &newitemonleft);
	
	/* Allocate temp buffer for leftpage */	
	leftpage = PageGetTempPage(origpage);
	_xbt_pageinit(leftpage, BufferGetPageSize(buf));
	lopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(leftpage);

	/*
	 * leftpage won't be the root when we're done.  Also, clear the SPLIT_END
	 * and HAS_GARBAGE flags.
	 */
	lopaque->btpo_flags = oopaque->btpo_flags;
	lopaque->btpo_flags &= ~(BTP_ROOT | BTP_SPLIT_END | BTP_HAS_GARBAGE);
	/* set flag in left page indicating that the right page has no downlink */
	lopaque->btpo_flags |= BTP_INCOMPLETE_SPLIT;
	lopaque->btpo_prev = oopaque->btpo_prev;
	lopaque->btpo.level = oopaque->btpo.level;

	/*
	 * Copy the original page's LSN into leftpage, which will become the
	 * updated version of the page.  We need this because XLogInsert will
	 * examine the LSN and possibly dump it in a page image.
	 */
	PageSetLSN(leftpage, PageGetLSN(origpage));
	isleaf = P_ISLEAF(oopaque);

	/*
	 * The "high key" for the new left page will be the first key that's going
	 * to go into the new right page, or a truncated version if this is a leaf
	 * page split.
	 *
	 * The high key for the left page is formed using the first item on the
	 * right page, which may seem to be contrary to Lehman & Yao's approach of
	 * using the left page's last item as its new high key when splitting on
	 * the leaf level.  It isn't, though: suffix truncation will leave the
	 * left page's high key fully equal to the last item on the left page when
	 * two tuples with equal key values (excluding heap TID) enclose the split
	 * point.  It isn't actually necessary for a new leaf high key to be equal
	 * to the last item on the left for the L&Y "subtree" invariant to hold.
	 * It's sufficient to make sure that the new leaf high key is strictly
	 * less than the first item on the right leaf page, and greater than or
	 * equal to (not necessarily equal to) the last item on the left leaf
	 * page.
	 *
	 * In other words, when suffix truncation isn't possible, L&Y's exact
	 * approach to leaf splits is taken.  (Actually, even that is slightly
	 * inaccurate.  A tuple with all the keys from firstright but the heap TID
	 * from lastleft will be used as the new high key, since the last left
	 * tuple could be physically larger despite being opclass-equal in respect
	 * of all attributes prior to the heap TID attribute.)
	 */
	if (!newitemonleft && newitemoff == firstright)
	{
		/* incoming tuple will become first on right page */
		itemsz = newitemsz;
		item = newitem;
		/*
         * we have expanded the itup size by 8B in xbtree index to represent the
         * space it actually occupies. When the itup is written to the page, the 8B is
         * restored.
         *
         * if we choose newitem as firstright here, it may be copied as HIKEY. So we need
         * to remind the 8B for this case.
         */
		itup_undo = isleaf; /* itup with undo only in xbtree */
	}
	else
	{
		/* existing item at firstright will become first on right page */
		itemid = PageGetItemId(origpage, firstright);
		item = (IndexTuple) PageGetItem(origpage, itemid);
		itemsz = IndexTupleSize(item);
	}

	/*
	 * Truncate unneeded key and non-key attributes of the high key item
	 * before inserting it on the left page.  This can only happen at the leaf
	 * level, since in general all pivot tuple values originate from leaf
	 * level high keys.  A pivot tuple in a grandparent page must guide a
	 * search not only to the correct parent page, but also to the correct
	 * leaf page.
	 */
	if (isleaf)
	{
		IndexTuple lastleft;

		/*
		 * Determine which tuple will become the last on the left page.  This
		 * is needed to decide how many attributes from the first item on the
		 * right page must remain in new high key for left page.
         */
		if (newitemonleft && newitemoff == firstright)
		{
			/* incoming tuple will become last on left page */
			lastleft = newitem;
		}
		else
		{
			OffsetNumber lastleftoff;

			/* item just before firstright will become last on left page */
			lastleftoff = OffsetNumberPrev(firstright);
			Assert(lastleftoff >= P_FIRSTDATAKEY(oopaque));
			itemid = PageGetItemId(origpage, lastleftoff);
			lastleft = (IndexTuple) PageGetItem(origpage, itemid);
		}

		Assert(lastleft != item);
		lefthikey = _xbt_truncate(rel, lastleft, item, itup_key, itup_undo);
		itemsz = IndexTupleSize(lefthikey);
		itemsz = MAXALIGN(itemsz);
	}
	else
		lefthikey = item;

	/*
     * Add new high key to leftpage
     */
	leftoff = P_HIKEY;
	Assert(XBTreeTupleGetNAtts(lefthikey, rel) > 0);
	Assert(XBTreeTupleGetNAtts(lefthikey, rel) <= indnkeyatts);

	if (PageAddItem(leftpage, (Item) lefthikey, itemsz, leftoff, 
		false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add hikey to the left sibling"
			 " while splitting block %u of index \"%s\"",
			 origpagenumber, RelationGetRelationName(rel));
	leftoff = OffsetNumberNext(leftoff);
	/* be tidy */
	if (lefthikey != item)
		pfree(lefthikey);

	/*
	 * Acquire a new right page to split into, now that left page has a new
	 * high key.  From here on, it's not okay to throw an error without
	 * zeroing rightpage first.  This coding rule ensures that we won't
	 * confuse future VACUUM operations, which might otherwise try to re-find
	 * a downlink to a leftover junk page as the page undergoes deletion.
	 *
	 * It would be reasonable to start the critical section just after the new
	 * rightpage buffer is acquired instead; that would allow us to avoid
	 * leftover junk pages without bothering to zero rightpage.  We do it this
	 * way because it avoids an unnecessary PANIC when either origpage or its
	 * existing sibling page are corrupt.
	 */
	
	rbuf = _xbt_getnewbuf(rel, &addr);
	rightpage = BufferGetPage(rbuf);
	rightpagenumber = BufferGetBlockNumber(rbuf);
	/* rightpage was initialized by _xbt_getnewbuf */
	ropaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(rightpage);
	
	/*
	 * Finish off remaining leftpage special area fields.  They cannot be set
	 * before both origpage (leftpage) and rightpage buffers are acquired and
	 * locked.
	 */
	lopaque->btpo_next = rightpagenumber;
	lopaque->btpo_cycleid = _bt_vacuum_cycleid(rel);
	lopaque->last_delete_xid = oopaque->last_delete_xid;
	lopaque->active_count = 0;

	/*
	 * rightpage won't be the root when we're done.  Also, clear the SPLIT_END
	 * and HAS_GARBAGE flags.
	 */
	ropaque->btpo_flags = oopaque->btpo_flags;
	ropaque->btpo_flags &= ~(BTP_ROOT | BTP_SPLIT_END | BTP_HAS_GARBAGE);
	ropaque->btpo_prev = origpagenumber;
	ropaque->btpo_next = oopaque->btpo_next;
	ropaque->btpo.level = oopaque->btpo.level;
	ropaque->btpo_cycleid = lopaque->btpo_cycleid;
	ropaque->last_delete_xid = oopaque->last_delete_xid;
	ropaque->active_count = 0;

	/*
	 * Add new high key to rightpage where necessary.
	 *
	 * If the page we're splitting is not the rightmost page at its level in
	 * the tree, then the first entry on the page is the high key from
	 * origpage.
	 */
	rightoff = P_HIKEY;

	if (!P_RIGHTMOST(oopaque))
	{
		itemid = PageGetItemId(origpage, P_HIKEY);
		itemsz = ItemIdGetLength(itemid);
		item = (IndexTuple) PageGetItem(origpage, itemid);
		Assert(XBTreeTupleGetNAtts(item, rel) > 0);
		Assert(XBTreeTupleGetNAtts(item, rel) <= indnkeyatts);
		if (PageAddItem(rightpage, (Item) item, itemsz, rightoff, 
						false, false) == InvalidOffsetNumber)
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			elog(ERROR, "failed to add hikey to the right sibling"
				 " while splitting block %u of index \"%s\"",
				 origpagenumber, RelationGetRelationName(rel));
		}
		rightoff = OffsetNumberNext(rightoff);
	}

	init_xlog_undo_meta(&xlum);

	if (undo_cache_ctx->undo_prepare_buffers)
		reset_undo_prepare_buffers(undo_cache_ctx->undo_prepare_buffers, false);

	if (IsSubTransaction())
	{
		sub_xid = GetCurrentFullTransactionId();
	}

	/* Prepare Undo record before buffer lock since undo record length is fixed */
	persistence = UndoPersistenceForRelation(rel);
	reloid = RelationGetRelid(rel);
	xid = GetTopFullTransactionId();
	
	/*
	 * Now transfer all the data items (non-pivot tuples in isleaf case, or
	 * additional pivot tuples in !isleaf case) to the appropriate page.
	 *
	 * Note: we *must* insert at least the right page's items in item-number
	 * order, for the benefit of _bt_restore_page().
	 */
	maxoff = PageGetMaxOffsetNumber(origpage);

	for (i = P_FIRSTDATAKEY(oopaque); i <= maxoff; i = OffsetNumberNext(i))
	{
		itemid = PageGetItemId(origpage, i);
		itemsz = ItemIdGetLength(itemid);
		item = (IndexTuple) PageGetItem(origpage, itemid);

		/* does new item belong before this one? */
		if (i == newitemoff)
		{
			if(isleaf)
				urec_ptr = _xbt_prepare_undo_insert(
						reloid, rel->rd_rel->relfilenode, rel->rd_locator.spcOid, 
						persistence, xid, GetCurrentCommandId(true), newitem, 
						prev_urec_ptr, INVALID_UNDO_REC_PTR, 
						BufferGetBlockNumber(newitemonleft ? buf : rbuf),
						NULL, NULL, &xlum, sub_xid);
			
			if (newitemonleft)
			{
				Assert(newitemoff <= firstright);
				newitemleftoff = leftoff;
				if (!_xbt_pgaddtup(leftpage, newitemsz, newitem, leftoff, xid, 
							true, urec_ptr, NULL))
				{
					memset(rightpage, 0, BufferGetPageSize(rbuf));
					elog(ERROR, "failed to add new item to the left sibling"
						 " while splitting block %u of index \"%s\"",
						 origpagenumber, RelationGetRelationName(rel));
				}
				leftoff = OffsetNumberNext(leftoff);
				/* update active hint */
				lopaque->active_count++;
			}
			else
			{
				newitemrightoff = rightoff;
				if (!_xbt_pgaddtup(rightpage, newitemsz, newitem, rightoff, xid, 
							true, urec_ptr, NULL))
				{
					memset(rightpage, 0, BufferGetPageSize(rbuf));
					elog(ERROR, "failed to add new item to the right sibling"
						 " while splitting block %u of index \"%s\"",
						 origpagenumber, RelationGetRelationName(rel));
				}
				rightoff = OffsetNumberNext(rightoff);
				/* update active hint */
				ropaque->active_count++;
			}
			if(isleaf)
			{
				undorec = prepare_buffers_get_undorecord((undo_cache_ctx->undo_prepare_buffers), 0);
				SetUndoRecordOffset(undorec, newitemonleft ? newitemleftoff : newitemrightoff);
			}
		}

		/* decide which page to put it on */
		if (i < firstright)
		{
			if (!_xbt_pgaddtup(leftpage, itemsz, item, leftoff, xid, 
						false, urec_ptr, itemid))
			{
				memset(rightpage, 0, BufferGetPageSize(rbuf));
				elog(ERROR, "failed to add old item to the left sibling"
					 " while splitting block %u of index \"%s\"",
					 origpagenumber, RelationGetRelationName(rel));
			}
			leftoff = OffsetNumberNext(leftoff);
			if (isleaf && !IndexItemIdIsDeleted(itemid))
				lopaque->active_count++; /* not deleted, count as active */
		}
		else
		{
			if (!_xbt_pgaddtup(rightpage, itemsz, item, rightoff, xid, 
						false, urec_ptr, itemid))
			{
				memset(rightpage, 0, BufferGetPageSize(rbuf));
				elog(ERROR, "failed to add old item to the right sibling"
					 " while splitting block %u of index \"%s\"",
					 origpagenumber, RelationGetRelationName(rel));
			}
			rightoff = OffsetNumberNext(rightoff);
			if (isleaf && !IndexItemIdIsDeleted(itemid))
				ropaque->active_count++; /* not deleted, count as active */
		}
	}
	
	/* cope with possibility that newitem goes at the end */
	if (i <= newitemoff)
	{
		/*
		 * Can't have newitemonleft here; that would imply we were told to put
		 * *everything* on the left page, which cannot fit (if it could, we'd
		 * not be splitting the page).
		 */
		Assert(!newitemonleft);
		if(isleaf)
			urec_ptr = _xbt_prepare_undo_insert(
						reloid, rel->rd_rel->relfilenode, rel->rd_locator.spcOid, 
						persistence, xid, GetCurrentCommandId(true), newitem, 
						prev_urec_ptr, INVALID_UNDO_REC_PTR, 
						BufferGetBlockNumber(rbuf), 
						NULL, NULL, &xlum, sub_xid);

		if (!_xbt_pgaddtup(rightpage, newitemsz, newitem, rightoff, xid, 
					true, urec_ptr, NULL))
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			elog(ERROR, "failed to add new item to the right sibling"
				 " while splitting block %u of index \"%s\"",
				 origpagenumber, RelationGetRelationName(rel));
		}
		newitemrightoff = rightoff;
		rightoff = OffsetNumberNext(rightoff);
		/* update active hint */
		ropaque->active_count++;

		if(isleaf)
		{
			undorec = prepare_buffers_get_undorecord((undo_cache_ctx->undo_prepare_buffers), 0);
			SetUndoRecordOffset(undorec,  newitemrightoff);
		}
	}

	/*
	 * We have to grab the right sibling (if any) and fix the prev pointer
	 * there. We are guaranteed that this is deadlock-free since no other
	 * writer will be holding a lock on that page and trying to move left, and
	 * all readers release locks on a page before trying to fetch its
	 * neighbors.
	 */
	if (!P_RIGHTMOST(oopaque))
	{
		sbuf = _bt_getbuf(rel, oopaque->btpo_next, BT_WRITE);
		spage = BufferGetPage(sbuf);
		sopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(spage);
		if (sopaque->btpo_prev != origpagenumber)
		{
			memset(rightpage, 0, BufferGetPageSize(rbuf));
			elog(ERROR, "right sibling's left-link doesn't match: "
				 "block %u links to %u instead of expected %u in index \"%s\"",
				 oopaque->btpo_next, sopaque->btpo_prev, origpagenumber,
				 RelationGetRelationName(rel));
		}

		/*
		 * Check to see if we can set the SPLIT_END flag in the right-hand
		 * split page; this can save some I/O for vacuum since it need not
		 * proceed to the right sibling.  We can set the flag if the right
		 * sibling has a different cycleid: that means it could not be part
		 * of a group of pages that were all split off from the same ancestor
		 * page.  If you're confused, imagine that page A splits to A B and
		 * then again, yielding A C B, while vacuum is in progress.  Tuples
		 * originally in A could now be in either B or C, hence vacuum must
		 * examine both pages.  But if D, our right sibling, has a different
		 * cycleid then it could not contain any tuples that were in A when
		 * the vacuum started.
		 */
		if (sopaque->btpo_cycleid != ropaque->btpo_cycleid)
			ropaque->btpo_flags |= BTP_SPLIT_END;
	}

	if (RelationNeedsWAL(rel))
	{
		if(isleaf)
		{
			/* 4 xbtree buffer + 1 undo slot buffer */
			int	used_buffers = prepare_buffers_used_count(undo_cache_ctx->undo_prepare_buffers) + 5;
			XLogEnsureRecordSpace(used_buffers - 1, 0);
		}
	}

	/*
     * Right sibling is locked, new siblings are prepared, but original page
     * is not updated yet.
     *
     * NO EREPORT(ERROR) till right sibling is updated.  We can get away with
     * not starting the critical section till here because we haven't been
     * scribbling on the original page yet; see comments above.
     */
	START_CRIT_SECTION();

	/*
	 * By here, the original data page has been split into two new halves, and
	 * these are correct.  The algorithm requires that the left page never
	 * move during a split, so we copy the new left page back on top of the
	 * original.  Note that this is not a waste of time, since we also require
	 * (in the page management code) that the center of a page always be
	 * clean, and the most efficient way to guarantee this is just to compact
	 * the data by reinserting it into a new left page.  (XXX the latter
	 * comment is probably obsolete; but in any case it's good to not scribble
	 * on the original page until we enter the critical section.)
	 *
	 * We need to do this before writing the WAL record, so that XLogInsert
	 * can WAL log an image of the page if necessary.
	 */
	PageRestoreTempPage(leftpage, origpage);
	/* leftpage, lopaque must not be used below here */

	MarkBufferDirty(buf);
	MarkBufferDirty(rbuf);

	if (!P_RIGHTMOST(ropaque))
	{
		sopaque->btpo_prev = rightpagenumber;
		MarkBufferDirty(sbuf);
	}

	/*
	 * Clear INCOMPLETE_SPLIT flag on child if inserting the new item finishes
	 * a split.
     */
	if (!isleaf)
	{
		Page cpage = BufferGetPage(cbuf);
		XBTPageOpaqueInternal cpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(cpage);

		cpageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
		MarkBufferDirty(cbuf);
	}

	if(isleaf)
	{
		insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, 0);
		oldPrevUrp = get_current_tansaction_undorec_ptr(persistence);
		set_current_tansaction_undorec_ptr(urec_ptr, persistence);
		update_undolog_meta(xid, prepare_buffers_get_first_undoptr(undo_cache_ctx->undo_prepare_buffers),
						 &xlum, persistence, prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
						 prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));
	}

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_xbtree_split xlrec;
		uint8			xlinfo;
		XLogRecPtr		recptr;
		uint8 xlundohdr_flag = 0; 
		FullTransactionId current_xid = InvalidFullTransactionId;
		xl_undo_header  xlundohdr;

		xlrec.level = ropaque->btpo.level;
		xlrec.firstright = firstright;
		xlrec.newitemoff = newitemoff;
		xlrec.newitemrightoff = newitemrightoff;
		xlrec.rightitemcount = PageGetMaxOffsetNumber(rightpage);
		xlrec.opaqueversion = XBTREE_OPAQUE_VERSION;
		xlrec.lopaque = *(XBTPageOpaqueInternal) PageGetSpecialPointer(origpage);
		xlrec.ropaque = *ropaque;
		
		if(isleaf)
		{
			if (prev_urec_ptr != INVALID_UNDO_REC_PTR)
				xlundohdr_flag |= XLOG_UNDO_HEADER_HAS_BLK_PREV;
			if ((GetUndoRecordUinfo(undorec) & UREC_INFO_PREURP) != 0)
				xlundohdr_flag |= XLOG_UNDO_HEADER_HAS_PREV_URP;
			if (IsSubTransaction())
			{
				xlundohdr_flag |= XLOG_UNDO_HEADER_HAS_TOP_XID;
				current_xid = GetTopFullTransactionIdIfAny();
			}
			/* undo meta information */
			Assert((UndoLogControl *) undo_sys_ctx->ulogs[undo_log_ctx->logs[persistence]] != NULL);

			xlundohdr.relOid = reloid;
			xlundohdr.urecptr = urec_ptr;
			xlundohdr.flag = xlundohdr_flag;
		}

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfXBTreeSplit);

		if(isleaf)
		{
			Size  itupSize = XstoreIndexTupleSize(newitem);
			/*wal undo*/
			XLogRegisterData((char *) &xlundohdr, SizeOfXLUndoHeader);
			if ((xlundohdr_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
			{
				Assert(prev_urec_ptr != INVALID_UNDO_REC_PTR);
				XLogRegisterData((char *) &(prev_urec_ptr), sizeof(UndoRecPtr));
			}
			if ((xlundohdr_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
				XLogRegisterData((char *) &(oldPrevUrp), sizeof(UndoRecPtr));
			if ((xlundohdr_flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
			{
				XLogRegisterData((char *) &(current_xid), sizeof(FullTransactionId));
			}
			// add indextupe size and data
			XLogRegisterData((char *) &(itupSize), sizeof(Size));
			XLogRegisterData((char *) newitem, itupSize);
			xlog_undo_meta_write(&xlum);
		}
		
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBuffer(1, rbuf, REGBUF_WILL_INIT);
		/* Log the right sibling, because we've changed its' prev-pointer. */
		if (!P_RIGHTMOST(ropaque))
			XLogRegisterBuffer(2, sbuf, REGBUF_STANDARD);
		if (BufferIsValid(cbuf))
			XLogRegisterBuffer(3, cbuf, REGBUF_STANDARD);

		/*
		 * Log the new item, if it was inserted on the left page. (If it was
		 * put on the right page, we don't need to explicitly WAL log it
		 * because it's included with all the other items on the right page.)
		 * Show the new item as belonging to the left page buffer, so that it
		 * is not stored if XLogInsert decides it needs a full-page image of
		 * the left page.  We store the offset anyway, though, to support
		 * archive compression of these records.
		 */
		if (newitemonleft)
		{
			ItemId	   iid;
			IndexTuple itup;

			Assert(newitemleftoff != InvalidOffsetNumber);
			iid = PageGetItemId(origpage, newitemleftoff);
			itup = (IndexTuple) PageGetItem(origpage, iid);
			XLogRegisterBufData(0, (char *) itup, ItemIdGetLength(iid));
		}

		/* Log the left page's new high key */
		itemid = PageGetItemId(origpage, P_HIKEY);
		item = (IndexTuple) PageGetItem(origpage, itemid);
		XLogRegisterBufData(0, (char *) item, MAXALIGN(IndexTupleSize(item)));

		/*
		 * Log the contents of the right page in the format understood by
		 * _bt_restore_page().  The whole right page will be recreated.
		 *
		 * Direct access to page is not good but faster - we should implement
		 * some new func in page API.  Note we only store the tuples
		 * themselves, knowing that they were inserted in item-number order
		 * and so the line pointers can be reconstructed.  See comments for
		 * _bt_restore_page().
		 */
		XLogRegisterBufData(
			1, (char *) rightpage + SizeOfPageHeaderData,
			xlrec.rightitemcount * sizeof(ItemIdData));	
		XLogRegisterBufData(
			1, (char *) rightpage + ((PageHeader) rightpage)->pd_upper,
			((PageHeader) rightpage)->pd_special - ((PageHeader) rightpage)->pd_upper);

		xlinfo = newitemonleft ? XLOG_XBTREE_SPLIT_L : XLOG_XBTREE_SPLIT_R;
		
		if(isleaf)
			/* 4 xbtree buffer */
			xlog_register_undo_buffers(undo_cache_ctx->undo_prepare_buffers, 4, 
				(UndoLogControl *) undo_sys_ctx->ulogs[undo_log_ctx->logs[persistence]]);
		
		recptr = XLogInsert(RM_XBTREE_ID, xlinfo);

		PageSetLSN(origpage, recptr);
		PageSetLSN(rightpage, recptr);
		if (!P_RIGHTMOST(ropaque))
		{
			PageSetLSN(spage, recptr);
		}
		if (!isleaf)
		{
			PageSetLSN(BufferGetPage(cbuf), recptr);
		}
		if(isleaf)
		{
			prepare_buffers_set_page_lsn(undo_cache_ctx->undo_prepare_buffers, recptr);
			set_undolog_meta_lsn(recptr);
		}
		
	}
	if(isleaf)
	{
		release_undolog_meta(persistence);
		reset_prepared_buffers_in_ctx();
	}

	END_CRIT_SECTION();

	/* discard this page from the Recycle Queue */
	xbtree_record_used_page(rel, addr);

	/* release the old right sibling */
	if (!P_RIGHTMOST(ropaque))
		_bt_relbuf(rel, sbuf);
	/* release the child */
	if (!isleaf)
		_bt_relbuf(rel, cbuf);

	/* split's done */
	return rbuf;
}

/*
 *	_xbt_dodelete() -- Handle insertion of a single index tuple in the tree.
 *
 *		This routine is called by the public interface routine, xbtdelete.
 *		By here, itup is filled in, including the TID.
 */
bool
_xbt_dodelete(Relation rel, IndexTuple itup, bool is_dead)
{
	bool		 found = true;
	BTScanInsert itup_key;
	Buffer		 buf;
	OffsetNumber offset;
	OffsetNumber loc;

	/* we need an insertion scan key to do our search, so build one */
	itup_key = _xbt_mkscankey(rel, itup);

	/*
	 * Find the first page containing this key.  Buffer returned by
	 * _xbt_search() is locked in exclusive mode.
	 */
	(void) _xbt_search(rel, itup_key, &buf, BT_WRITE, false);

	/* looking for the first item >= scankey */
	offset = _xbt_binsrch(rel, itup_key, buf);

	/* looking for itup with tid */
	loc = _xbt_finddeleteloc(rel, &buf, offset, itup_key, itup);

	if (loc == InvalidOffsetNumber)
		elog(ERROR, "xid %u failed to delete index %s tuple with TID (%u,%u)",
					GetCurrentTransactionId(), RelationGetRelationName(rel),
					ItemPointerGetBlockNumber(&itup->t_tid),
					ItemPointerGetOffsetNumber(&itup->t_tid));
	
	_xbt_deleteonpage(rel, buf, loc, is_dead);
	pfree(itup_key);
	return found;
}

/*
 *	xbt_finddeleteloc() -- Finds the location of the given tuple
 *
 *      We consider the location is right when we found a tuple whose t_tid equals
 *      the target, and the tuple is visible in non-vacuum snapshot.
 *
 *      Currently, we find the first tuple with the same key by _xbt_binsrch(),
 *      but we need to go through all the equal tuples to find the tuple with
 *      same t_tid. This can be optimized by sort by key and t_tid when inserting
 *      and comparing.
 */
static OffsetNumber
_xbt_finddeleteloc(Relation rel, Buffer *bufP, OffsetNumber offset,
					BTScanInsert itup_key, IndexTuple itup)
{
	OffsetNumber		   maxoff;
	Page				   page;
	XBTPageOpaqueInternal  opaque;

	page = BufferGetPage(*bufP);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
     * Scan over all equal tuples, looking for itup.
     */
	for (;;)
	{
		ItemId		item;
		IndexTuple	cur_itup;
		BlockNumber nblkno;

		if (offset <= maxoff)
		{
			item = PageGetItemId(page, offset);
			if (!ItemIdIsDead(item))
			{
				cur_itup = (IndexTuple) PageGetItem(page, item);

				if (ItemPointerEquals(&itup->t_tid, &cur_itup->t_tid))
				{
					SnapshotData snapshotData;
					nblkno = BufferGetBlockNumber(*bufP);
					InitNonVacuumableSnapshot(snapshotData, NULL);
					if (_xbt_tuple_satisfies(page, &snapshotData, nblkno, offset))
						return offset;
				}
			}
		}

		/*
         * Advance to next tuple to continue checking.
         */
		if (offset < maxoff)
			offset = OffsetNumberNext(offset);
		else
		{
			int highkeycmp;

			/* If scankey == hikey we gotta check the next page too */
			if (P_RIGHTMOST(opaque))
				break;

			highkeycmp = _xbt_compare(rel, itup_key, page, P_HIKEY);
			Assert(highkeycmp <= 0);
			if (highkeycmp != 0)
				break;

			/* Advance to next non-dead page --- there must be one */
			for (;;)
			{
				nblkno = opaque->btpo_next;
				*bufP = _bt_relandgetbuf(rel, *bufP, nblkno, BT_WRITE);
				page = BufferGetPage(*bufP);
				opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
				if (!P_IGNORE(opaque))
					break;
				if (P_RIGHTMOST(opaque))
					elog(ERROR, "fell off the end of index \"%s\" at blkno %u",
										   RelationGetRelationName(rel), nblkno);
			}
			maxoff = PageGetMaxOffsetNumber(page);
			offset = P_FIRSTDATAKEY(opaque);
		}
	}
	/* No corresponding itup was found, release buffer and return InvalidOffsetNumber */
	return InvalidOffsetNumber;
}

/*
 *	xbt_findsameindexloc() -- Finds the location of the given tuple
 *
 *      We consider the location is right when we found a tuple whose t_tid equals
 *      the target, find index match key and same tid, ignore visibility
 */
OffsetNumber
_xbt_findsameindexloc(Relation rel, Buffer *bufP, OffsetNumber offset,
					BTScanInsert itup_key, IndexTuple itup)
{
	OffsetNumber		   maxoff;
	Page				   page;
	XBTPageOpaqueInternal  opaque;

	page = BufferGetPage(*bufP);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
     * Scan over all equal tuples, looking for itup.
     */
	for (;;)
	{
		ItemId		curitemid;
		IndexTuple	cur_itup;
		BlockNumber nblkno;

		if (offset <= maxoff)
		{
			curitemid = PageGetItemId(page, offset);
			if (!ItemIdIsDead(curitemid))
			{
				cur_itup = (IndexTuple) PageGetItem(page, curitemid);
				if (ItemPointerEquals(&itup->t_tid, &cur_itup->t_tid))
					/* t_tid is correct, we found it */
					return offset;
			}
		}

		/*
         * Advance to next tuple to continue checking.
         */
		if (offset < maxoff)
			offset = OffsetNumberNext(offset);
		else
		{
			int highkeycmp;

			/* If scankey == hikey we gotta check the next page too */
			if (P_RIGHTMOST(opaque))
				break;

			highkeycmp = _xbt_compare(rel, itup_key, page, P_HIKEY);
			Assert(highkeycmp <= 0);
			if (highkeycmp != 0)
			{
				break;
			}
			/* Advance to next non-dead page --- there must be one */
			for (;;)
			{
				nblkno = opaque->btpo_next;
				*bufP = _bt_relandgetbuf(rel, *bufP, nblkno, BT_WRITE);
				page = BufferGetPage(*bufP);
				opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
				if (!P_IGNORE(opaque))
					break;
				if (P_RIGHTMOST(opaque))
				{
					elog(WARNING, "fell off the end of index \"%s\" at blkno %u",
										   RelationGetRelationName(rel), nblkno);
					return InvalidOffsetNumber;			   
				}
			}
			maxoff = PageGetMaxOffsetNumber(page);
			offset = P_FIRSTDATAKEY(opaque);
		}
	}
	/* No corresponding itup was found, release buffer and return InvalidOffsetNumber */
	return InvalidOffsetNumber;
}

/*
 *	_xbt_pgaddtup() -- add a tuple to a particular page in the index.
 *
 *		This routine adds the tuple to the page as requested.  It does
 *		not affect pin/lock status, but you'd better have a write lock
 *		and pin on the target buffer!  Don't forget to write and release
 *		the buffer afterwards, either.
 *
 *		The main difference between this routine and a bare PageAddItem call
 *		is that this code knows that the leftmost index tuple on a non-leaf
 *		btree page doesn't need to have a key.  Therefore, it strips such
 *		tuples down to just the tuple header.  CAUTION: this works ONLY if
 *		we insert the tuples in order, so that the given itup_off does
 *		represent the final position of the tuple!
 */
static bool
_xbt_pgaddtup(Page page, Size itemsize, IndexTuple itup, OffsetNumber itup_off, FullTransactionId xid,
				   bool isnew, UndoRecPtr urec, ItemId orgitem)
{
	XBTPageOpaqueInternal opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	IndexTupleData		  trunctuple;

	if (!P_ISLEAF(opaque) && itup_off == P_FIRSTDATAKEY(opaque))
	{
		trunctuple = *itup;
		trunctuple.t_info = sizeof(IndexTupleData);
		XBTreeTupleSetNAtts(&trunctuple, 0, false);
		itup = &trunctuple;
		itemsize = sizeof(IndexTupleData);
	}

	if (!(isnew && P_ISLEAF(opaque)))
	{
		// if isnew it must inserting downlink to parent and itup is normal
		if (PageAddItem(page, (Item) itup, itemsize, itup_off, false, false) ==
			InvalidOffsetNumber)
			return false;
		
		if(P_ISLEAF(opaque))
		{
			// copy the orig item flags
		   ItemId iid = PageGetItemId(page, itup_off);
		   iid->lp_flags = orgitem->lp_flags;
		}
	}
	else
	{
		Size		   newsize = itemsize - (sizeof(FullTransactionId) + sizeof(UndoRecPtr));
		bool		   needXid;
		XBTreeIndexTuple uxid;
		ItemId		   iid;

		IndexTupleSetSize(itup, newsize);

		/*
         * New insert tuple with undo, except HIKEY.
         */
		needXid = (itup_off >= P_FIRSTDATAKEY(opaque));

		((PageHeader) page)->pd_upper -= (sizeof(FullTransactionId) + sizeof(UndoRecPtr));
		uxid = (XBTreeIndexTuple) (((char *) page) + ((PageHeader) page)->pd_upper);

		uxid->modified_xid = needXid ? xid : InvalidFullTransactionId;
		uxid->urec = urec;

		if (PageAddItem(page, (Item) itup, newsize, itup_off, false, false) ==
			InvalidOffsetNumber)
		{
			/* restore page->upper before we return false */
			((PageHeader) page)->pd_upper += (sizeof(FullTransactionId) + sizeof(UndoRecPtr));
			return false;
		}

		iid = PageGetItemId(page, itup_off);
		ItemIdSetNormal(iid, ((PageHeader) page)->pd_upper, itemsize);
	}

	return true;
}

/*
 *  xbt_deleteonpage() -- Delete a tuple on a particular page in the index.
 *
 *      If the deletion deleted the last tuple in the page, the page may
 *      become empty after the last transaction committed. So we record it as
 *      empty page for further page recycle logic.
 */
static void
_xbt_deleteonpage(Relation rel, Buffer buf, OffsetNumber offset, bool is_dead)
{
	Page				   	page = BufferGetPage(buf);
	BlockNumber				blk = BufferGetBlockNumber(buf);
	XBTPageOpaqueInternal  	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	ItemId				   	item = PageGetItemId(page, offset);
	IndexTuple			   	itup = (IndexTuple) PageGetItem(page, item);
	XBTreeIndexTuple		xbt_tuple = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);
	xl_undo_meta		   	xlum;
	UndoRecPtr			   	urec_ptr = INVALID_UNDO_REC_PTR;
	UndoRecPtr				prev_tuple_uptr = INVALID_UNDO_REC_PTR;
	UndoRecPtr				prev_txn_uptr = INVALID_UNDO_REC_PTR;
	FullTransactionId		xid = GetTopFullTransactionId();
	bool				   	needRecordEmpty;
	UndoPersistence		   	persistence;
	Oid					   	relOid;
	FullTransactionId       oldXid = xbt_tuple->modified_xid;
	FullTransactionId       sub_xid = InvalidFullTransactionId;

	init_xlog_undo_meta(&xlum);

	if (undo_cache_ctx->undo_prepare_buffers)
		reset_undo_prepare_buffers(undo_cache_ctx->undo_prepare_buffers, false);

	if (IsSubTransaction())
		sub_xid = GetCurrentFullTransactionId();

	xbt_tuple_satisfies_update(rel, buf, page, blk, offset);

	/* Prepare Undo */
	persistence = UndoPersistenceForRelation(rel);
	relOid = RelationGetRelid(rel);
	prev_tuple_uptr = xbt_tuple->urec;

	urec_ptr = _xbt_prepare_undo_delete(
			relOid, rel->rd_rel->relfilenode, rel->rd_locator.spcOid, persistence, offset, xid, GetCurrentCommandId(true),
			itup, prev_tuple_uptr, INVALID_UNDO_REC_PTR, xbt_tuple->modified_xid, BufferGetBlockNumber(buf), NULL,
			NULL, &xlum, sub_xid);

	/* Do the update.  No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, 0);
	prev_txn_uptr = get_current_tansaction_undorec_ptr(persistence);
	set_current_tansaction_undorec_ptr(urec_ptr, persistence);

	xbt_tuple->modified_xid = xid;
	xbt_tuple->urec = urec_ptr;
	IndexItemIdSetDeleted(item);

	/* 
	 * avoid possible deadlock in the future unique check.
	 */
	if (is_dead)
		ItemIdMarkDead(item);

	/* update active hint */
	opaque->active_count--;
	if (FullTransactionIdPrecedes(opaque->last_delete_xid, xid))
		opaque->last_delete_xid = xid;

	MarkBufferDirty(buf);
	update_undolog_meta(xid, prepare_buffers_get_first_undoptr(undo_cache_ctx->undo_prepare_buffers),
					 &xlum, persistence, prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
					 prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));
	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_xbtree_delete	xlrec;
		XLogRecPtr			recptr;
		xl_undo_header   	xlundohdr;
		uint8		  		xlUndoHeaderFlag = 0;
		Size                itupSize = XstoreIndexTupleSize(itup);
		UnpackedUndoRecord	*urec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
		xlrec.xid = xid;
		xlrec.oldxid = oldXid;
		xlrec.offset = offset;
		
		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfXBTreeDelete);
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);

		if (prev_tuple_uptr != INVALID_UNDO_REC_PTR)
			xlUndoHeaderFlag |= XLOG_UNDO_HEADER_HAS_BLK_PREV;
		if (GetUndoRecordUinfo(urec) & UREC_INFO_PREURP)
			xlUndoHeaderFlag |= XLOG_UNDO_HEADER_HAS_PREV_URP;
		if (IsSubTransaction())
			xlUndoHeaderFlag |= XLOG_UNDO_HEADER_HAS_TOP_XID;

		// undo xlog stuff
		xlundohdr.relOid = relOid;
		xlundohdr.urecptr = urec_ptr;
		xlundohdr.flag = xlUndoHeaderFlag;
		XLogRegisterData((char *) &xlundohdr, SizeOfXLUndoHeader);
		if ((xlUndoHeaderFlag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
			XLogRegisterData((char *) &(prev_tuple_uptr), sizeof(UndoRecPtr));
		if ((xlUndoHeaderFlag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
			XLogRegisterData((char *) &(prev_txn_uptr), sizeof(UndoRecPtr));
		if ((xlUndoHeaderFlag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
		{
			FullTransactionId curxid = GetTopFullTransactionIdIfAny();
			XLogRegisterData((char *) &(curxid), sizeof(FullTransactionId));
		}
		// add indextupe size and data
		XLogRegisterData((char *) &(itupSize), sizeof(Size));
		XLogRegisterData((char *) itup, itupSize);
		xlog_undo_meta_write(&xlum);
		xlog_register_undo_buffers(undo_cache_ctx->undo_prepare_buffers, 1, 
			(UndoLogControl *) undo_sys_ctx->ulogs[undo_log_ctx->logs[persistence]]);
		recptr = XLogInsert(RM_XBTREE_ID, XLOG_XBTREE_DELETE);
		PageSetLSN(page, recptr);
		prepare_buffers_set_page_lsn(undo_cache_ctx->undo_prepare_buffers, recptr);
		set_undolog_meta_lsn(recptr);
	}
	release_undolog_meta(persistence);
	reset_prepared_buffers_in_ctx();
	END_CRIT_SECTION();
	needRecordEmpty = (opaque->active_count == 0);
	if (needRecordEmpty)
	{
		/*
         * This delete operation deleted the last tuple in the page, This page is likely
         * to be empty later, so record this empty hint into a queue.
         */
		xbtree_record_empty_page(rel, BufferGetBlockNumber(buf), opaque->last_delete_xid);
		/* release buffer */
		_bt_relbuf(rel, buf);
		/* at the same time, try to recycle an empty page already exists */
		xbtree_try_recycle_empty_page(rel);
	}
	else
	{
		/* just release buffer */
		_bt_relbuf(rel, buf);
	}
}

/*
 * _xbt_insert_parent() -- Insert downlink into parent, completing split.
 *
 * On entry, buf and rbuf are the left and right split pages, which we
 * still hold write locks on.  Both locks will be released here.  We
 * release the rbuf lock once we have a write lock on the page that we
 * intend to insert a downlink to rbuf on (i.e. buf's current parent page).
 * The lock on buf is released at the same point as the lock on the parent
 * page, since buf's INCOMPLETE_SPLIT flag must be cleared by the same
 * atomic operation that completes the split by inserting a new downlink.
 *
 * stack - stack showing how we got here.  Will be NULL when splitting true
 *			root, or during concurrent root split, where we can be inefficient
 * is_root - we split the true root
 * is_only - we split a page alone on its level (might have been fast root)
 */
static void
_xbt_insert_parent(Relation rel, Buffer buf, Buffer rbuf, BTStack stack, bool is_root,
				   bool is_only)
{
	/*
	 * Here we have to do something Lehman and Yao don't talk about: deal with
	 * a root split and construction of a new root.  If our stack is empty
	 * then we have just split a node on what had been the root level when we
	 * descended the tree.  If it was still the root then we perform a
	 * new-root construction.  If it *wasn't* the root anymore, search to find
	 * the next higher level that someone constructed meanwhile, and find the
	 * right place to insert as for the normal case.
	 *
	 * If we have to search for the parent level, we do so by re-descending
	 * from the root.  This is not super-efficient, but it's rare enough not
	 * to matter.
	 */
	if (is_root)
	{
		Buffer rootbuf;

		Assert(stack == NULL);
		Assert(is_only);
		/* create a new root node and update the metapage */
		rootbuf = _xbt_newroot(rel, buf, rbuf);
		/* release the split buffers */
		_bt_relbuf(rel, rootbuf);
		_bt_relbuf(rel, rbuf);
		_bt_relbuf(rel, buf);
	}
	else
	{
		BlockNumber bknum = BufferGetBlockNumber(buf);
		BlockNumber rbknum = BufferGetBlockNumber(rbuf);
		Page		page = BufferGetPage(buf);
		IndexTuple	new_item;
		BTStackData fakestack;
		IndexTuple	ritem;
		Buffer		pbuf;

		if (stack == NULL)
		{
			XBTPageOpaqueInternal lpageop;
			ereport(DEBUG2, (errmsg("concurrent ROOT page split")));
			lpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
			/* Find the leftmost page at the next level up */
			pbuf = _xbt_get_endpoint(rel, lpageop->btpo.level + 1, false);
			/* Set up a phony nstack entry poiting there */
			stack = &fakestack;
			stack->bts_blkno = BufferGetBlockNumber(pbuf);
			stack->bts_offset = InvalidOffsetNumber;
			stack->bts_parent = NULL;
			_bt_relbuf(rel, pbuf);
		}

		/* get high key from left, a strict lower bound for new right page */
		ritem = (IndexTuple) PageGetItem(page, PageGetItemId(page, P_HIKEY));

		/* form an index tuple that points at the new right page */
		new_item = CopyIndexTuple(ritem);
		XBTreeTupleSetDownLink(new_item, rbknum);

		/*
		 * Re-find and write lock the parent of buf.
		 *
		 * It's possible that the location of buf's downlink has changed since
		 * our initial _bt_search() descent.  _bt_getstackbuf() will detect
		 * and recover from this, updating the stack, which ensures that the
		 * new downlink will be inserted at the correct offset. Even buf's
		 * parent may have changed.
         */
		pbuf = _xbt_getstackbuf(rel, stack, bknum);

		/* Now we can unlock the right child. The left child will be unlocked
         * by _bt_insertonpg()
         */
		_bt_relbuf(rel, rbuf);

		/* Check for error only after writing children */
		if (pbuf == InvalidBuffer)
			elog(ERROR, "failed to re-find parent key in index \"%s\" for split pages %u/%u",
				 RelationGetRelationName(rel), bknum, rbknum);
		
		/* Recursively update the parent */
		_xbt_insertonpg(rel, NULL, pbuf, buf, stack->bts_parent, new_item,
						   stack->bts_offset + 1, is_only);

		/* be tidy */
		pfree(new_item);
	}
}

/*
 *	_xbt_newroot() -- Create a new root page for the index.
 *
 *		We've just split the old root page and need to create a new one.
 *		In order to do this, we add a new root page to the file, then lock
 *		the metadata page and update it.  This is guaranteed to be deadlock-
 *		free, because all readers release their locks on the metadata page
 *		before trying to lock the root, and all writers lock the root before
 *		trying to lock the metadata page.  We have a write lock on the old
 *		root page, so we have not introduced any cycles into the waits-for
 *		graph.
 *
 *		On entry, lbuf (the old root) and rbuf (its new peer) are write-
 *		locked. On exit, a new root page exists with entries for the
 *		two new children, metapage is updated and unlocked/unpinned.
 *		The new root buffer is returned to caller which has to unlock/unpin
 *		lbuf, rbuf & rootbuf.
 */
static Buffer
_xbt_newroot(Relation rel, Buffer lbuf, Buffer rbuf)
{
	Buffer				   rootbuf;
	Page				   lpage, rootpage;
	BlockNumber			   lbkno, rbkno;
	BlockNumber			   rootblknum;
	XBTPageOpaqueInternal  rootopaque;
	XBTPageOpaqueInternal  lopaque;
	ItemId				   itemid;
	IndexTuple			   item;
	IndexTuple			   left_item;
	Size				   left_item_sz;
	IndexTuple			   right_item;
	Size				   right_item_sz;
	Buffer				   metabuf;
	Page				   metapg;
	BTMetaPageData		  *metad = NULL;
	XBTRecycleQueueAddress addr;

	lbkno = BufferGetBlockNumber(lbuf);
	rbkno = BufferGetBlockNumber(rbuf);
	lpage = BufferGetPage(lbuf);
	lopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(lpage);

	/* get a new root page */
	rootbuf = _xbt_getnewbuf(rel, &addr);
	rootpage = BufferGetPage(rootbuf);
	rootblknum = BufferGetBlockNumber(rootbuf);

	/* acquire lock on the metapage */
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);

	/*
	 * Create downlink item for left page (old root).  Since this will be the
	 * first item in a non-leaf page, it implicitly has minus-infinity key
	 * value, so we need not store any actual key in it.
     */
	left_item_sz = sizeof(IndexTupleData);
	left_item = (IndexTuple) palloc(left_item_sz);
	left_item->t_info = (unsigned short) left_item_sz;
	XBTreeTupleSetDownLink(left_item, lbkno);
	XBTreeTupleSetNAtts(left_item, 0, false);

	/*
     * Create downlink item for right page.  The key for it is obtained from
     * the "high key" position in the left page.
     */
	itemid = PageGetItemId(lpage, P_HIKEY);
	item = (IndexTuple) PageGetItem(lpage, itemid);
	right_item = CopyIndexTuple(item);
	XBTreeTupleSetDownLink(right_item, rbkno);
	right_item_sz = IndexTupleSize(right_item);

	/* NO EREPORT(ERROR) from here till newroot op is logged */
	START_CRIT_SECTION();

	/* set btree special data */
	rootopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(rootpage);
	rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
	rootopaque->btpo_flags = BTP_ROOT;
	rootopaque->btpo.level =
		((XBTPageOpaqueInternal) PageGetSpecialPointer(lpage))->btpo.level + 1;
	rootopaque->btpo_cycleid = 0;

	/* update metapage data */
	metad->btm_root = rootblknum;
	metad->btm_level = rootopaque->btpo.level;
	metad->btm_fastroot = rootblknum;
	metad->btm_fastlevel = rootopaque->btpo.level;

	/*
     * Insert the left page pointer into the new root page.  The root page is
     * the rightmost page on its level so there is no "high key" in it; the
     * two items will go into positions P_HIKEY and P_FIRSTKEY.
     *
     * Note: we *must* insert the two items in item-number order, for the
     * benefit of _bt_restore_page().
     */
	if (PageAddItem(rootpage, (Item) left_item, left_item_sz, P_HIKEY, false, false) ==
		InvalidOffsetNumber)
		elog(PANIC, "failed to add leftkey to new root page"
			 " while splitting block %u of index \"%s\"",
			 BufferGetBlockNumber(lbuf), RelationGetRelationName(rel));

	/*
     * insert the right page pointer into the new root page.
     */
	if (PageAddItem(rootpage, (Item) right_item, right_item_sz, P_FIRSTKEY, false,
					false) == InvalidOffsetNumber)
		elog(PANIC, "failed to add rightkey to new root page"
			 " while splitting block %u of index \"%s\"",
			 BufferGetBlockNumber(lbuf), RelationGetRelationName(rel));

	/* Clear the incomplete-split flag in the left child */
	Assert(P_INCOMPLETE_SPLIT(lopaque));
	lopaque->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
	MarkBufferDirty(lbuf);

	MarkBufferDirty(rootbuf);
	MarkBufferDirty(metabuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_newroot  xlrec;
		XLogRecPtr		  recptr;
		xl_btree_metadata md;

		xlrec.rootblk = rootblknum;
		xlrec.level = metad->btm_level;

		Assert(xlrec.level > 0);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfBtreeNewroot);

		XLogRegisterBuffer(0, rootbuf, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, lbuf, REGBUF_STANDARD);
		XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		md.root = rootblknum;
		md.level = metad->btm_level;
		md.fastroot = rootblknum;
		md.fastlevel = metad->btm_level;

		XLogRegisterBufData(2, (char *) &md, sizeof(xl_btree_metadata));

		/*
         * Direct access to page is not good but faster - we should implement
         * some new func in page API.
         */
		XLogRegisterBufData(
			0, (char *) rootpage + ((PageHeader) rootpage)->pd_upper,
			((PageHeader) rootpage)->pd_special - ((PageHeader) rootpage)->pd_upper);

		recptr = XLogInsert(RM_XBTREE_ID, XLOG_XBTREE_NEWROOT);

		PageSetLSN(lpage, recptr);
		PageSetLSN(rootpage, recptr);
		PageSetLSN(metapg, recptr);
	}

	END_CRIT_SECTION();

	/* done with metapage */
	_bt_relbuf(rel, metabuf);

	/* discard this page from the Recycle Queue */
	xbtree_record_used_page(rel, addr);

	pfree(left_item);
	pfree(right_item);

	return rootbuf;
}

/*
 * _xbt_finish_split() -- Finish an incomplete split
 *
 * A crash or other failure can leave a split incomplete.  The insertion
 * routines won't allow to insert on a page that is incompletely split.
 * Before inserting on such a page, call _xbt_finish_split().
 *
 * On entry, 'lbuf' must be locked in write-mode. On exit, it is unlocked
 * and unpinned.
 */
void
_xbt_finish_split(Relation rel, Buffer lbuf, BTStack stack)
{
	Page				  lpage = BufferGetPage(lbuf);
	XBTPageOpaqueInternal lpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(lpage);
	Buffer				  rbuf;
	Page				  rpage;
	XBTPageOpaqueInternal rpageop;
	bool				  wasRoot = false;
	bool				  wasOnly;

	Assert(P_INCOMPLETE_SPLIT(lpageop));

	/* Lock right sibling, the one missing the downlink */
	rbuf = _bt_getbuf(rel, lpageop->btpo_next, BT_WRITE);
	rpage = BufferGetPage(rbuf);
	rpageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(rpage);

	/* Could this be a root split? */
	if (!stack)
	{
		Buffer			metabuf;
		Page			metapg;
		BTMetaPageData *metad;

		/* acquire lock on the metapage */
		metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
		metapg = BufferGetPage(metabuf);
		metad = BTPageGetMeta(metapg);

		wasRoot = (metad->btm_root == BufferGetBlockNumber(lbuf));

		_bt_relbuf(rel, metabuf);
	}
	else
	{
		wasRoot = false;
	}

	/* Was this the only page on the level before split? */
	wasOnly = (P_LEFTMOST(lpageop) && P_RIGHTMOST(rpageop));

	elog(DEBUG1, "finishing incomplete split of %u/%u", BufferGetBlockNumber(lbuf),
		 BufferGetBlockNumber(rbuf));

	_xbt_insert_parent(rel, lbuf, rbuf, stack, wasRoot, wasOnly);
}

/*
 *	_xbt_getstackbuf() -- Walk back up the tree one step, and find the item
 *						 we last looked at in the parent.
 *
 *		This is possible because we save the downlink from the parent item,
 *		which is enough to uniquely identify it.  Insertions into the parent
 *		level could cause the item to move right; deletions could cause it
 *		to move left, but not left of the page we previously found it in.
 *
 *		Adjusts bts_blkno & bts_offset if changed.
 *
 *		Returns write-locked buffer, or InvalidBuffer if item not found
 *		(should not happen).
 */
Buffer
_xbt_getstackbuf(Relation rel, BTStack stack, BlockNumber child)
{
	BlockNumber	 blkno;
	OffsetNumber start;

	blkno = stack->bts_blkno;
	start = stack->bts_offset;

	for (;;)
	{
		Buffer				  buf;
		Page				  page;
		XBTPageOpaqueInternal opaque;

		buf = _bt_getbuf(rel, blkno, BT_WRITE);
		page = BufferGetPage(buf);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

		if (P_INCOMPLETE_SPLIT(opaque))
		{
			_xbt_finish_split(rel, buf, stack->bts_parent);
			continue;
		}

		if (!P_IGNORE(opaque))
		{
			OffsetNumber offnum, minoff, maxoff;
			ItemId		 itemid;
			IndexTuple	 item;

			minoff = P_FIRSTDATAKEY(opaque);
			maxoff = PageGetMaxOffsetNumber(page);

			/*
             * start = InvalidOffsetNumber means "search the whole page". We
             * need this test anyway due to possibility that page has a high
             * key now when it didn't before.
             */
			if (start < minoff)
				start = minoff;

			/*
             * Need this check too, to guard against possibility that page
             * split since we visited it originally.
             */
			if (start > maxoff)
				start = OffsetNumberNext(maxoff);

			/*
             * These loops will check every item on the page --- but in an
             * order that's attuned to the probability of where it actually
             * is.	Scan to the right first, then to the left.
             */
			for (offnum = start; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				item = (IndexTuple) PageGetItem(page, itemid);

				if (BTreeTupleGetDownLink(item) == child)
				{
					/* Return accurate pointer to where link is now */
					stack->bts_blkno = blkno;
					stack->bts_offset = offnum;
					return buf;
				}
			}

			for (offnum = OffsetNumberPrev(start); offnum >= minoff;
				 offnum = OffsetNumberPrev(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				item = (IndexTuple) PageGetItem(page, itemid);

				if (BTreeTupleGetDownLink(item) == child)
				{
					/* Return accurate pointer to where link is now */
					stack->bts_blkno = blkno;
					stack->bts_offset = offnum;
					return buf;
				}
			}
		}

		/*
         * The item we're looking for moved right at least one page.
         */
		if (P_RIGHTMOST(opaque))
		{
			_bt_relbuf(rel, buf);
			return InvalidBuffer;
		}
		blkno = opaque->btpo_next;
		start = InvalidOffsetNumber;
		_bt_relbuf(rel, buf);
	}
}