/* -------------------------------------------------------------------------
 *
 * xhio.c
 * I/O operations of xstore inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xhio.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "xheap/xhio.h"
#include "xheap/xstat.h"
#include "xheap/xheap.h"


Buffer
relation_get_buffer_for_xtuple(Relation relation, Size len, Buffer other_fuffer, int options,
						   BulkInsertState bistate)
{
	bool		use_fsm = !(options & XHEAP_INSERT_SKIP_FSM);
	bool		force_extend = (options & XHEAP_INSERT_EXTEND);
	Buffer		buffer = InvalidBuffer;
	Page		page;
	Size		page_free_space = 0;
	Size		save_free_space = 0;
	BlockNumber target_block;
	BlockNumber other_block;
	bool		need_lock = false;

	/*
     * Blocks that extended one by one are different from bulk-extend blocks, and
     * are not recorded into FSM. As its creator session close this realtion, they
     * can not be used by any other body. It is especially obvious for partition
     * bulk insert. Here, if no available found in FSM, we check the last block to
     * reuse the 'leaked free space' mentioned earlier.
     */
	bool test_last_block = false;

	len = SHORTALIGN(len);
	/*
     * If we're gonna fail for oversize tuple, do it right away
     */
	if (len > MaxXHeapTupleSize(relation))
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("row is too big: size %zu, maximum size %zu", len,
							   MaxXHeapTupleSize(relation))));

	/* Compute desired extra freespace due to fillfactor option */
	save_free_space = RelationGetTargetPageFreeSpace(relation, HEAP_DEFAULT_FILLFACTOR);

	if (other_fuffer != InvalidBuffer)
		other_block = BufferGetBlockNumber(other_fuffer);
	else
		other_block = InvalidBlockNumber;

	if ((NULL != bistate) && BufferIsValid(bistate->current_buf))
	{
		RelFileLocator rlocator;
		ForkNumber	forknum;
		BlockNumber blknum;

		BufferGetTag(bistate->current_buf, &rlocator, &forknum, &blknum);

		if (!RelFileLocatorEquals(relation->rd_locator, rlocator))
		{
			ReleaseBuffer(bistate->current_buf);
			bistate->current_buf = InvalidBuffer;
		}
	}

	/*
     * We first try to put the tuple on the same page we last inserted a tuple
     * on, as cached in the BulkInsertState or relcache entry.  If that
     * doesn't work, we ask the Free Space Map to locate a suitable page.
     * Since the FSM's info might be out of date, we have to be prepared to
     * loop around and retry multiple times. (To insure this isn't an infinite
     * loop, we must update the FSM with the correct amount of free space on
     * each page that proves not to be suitable.)  If the FSM has no record of
     * a page with enough free space, we give up and extend the relation.
     *
     * When useFsm is false, we either put the tuple onto the existing target
     * page or extend the relation.
     */
	if (len + save_free_space > MaxXHeapTupleSize(relation))
	{
		/* can't fit, don't bother asking FSM */
		target_block = InvalidBlockNumber;
		use_fsm = false;
	}
	else if (bistate && bistate->current_buf != InvalidBuffer)
		target_block = BufferGetBlockNumber(bistate->current_buf);
	else
		target_block = RelationGetTargetBlock(relation);

	if (unlikely(force_extend))
		target_block = InvalidBlockNumber;
	else if (target_block == InvalidBlockNumber && use_fsm)
	{
		/*
		 * We have no cached target page, so ask the FSM for an initial
		 * target.
		 */
		target_block = GetPageWithFreeSpace(relation, len + save_free_space);
		/*
		 * If the FSM knows nothing of the rel, try the last page before we
		 * give up and extend.  This avoids one-tuple-per-page syndrome during
		 * bootstrapping or in a recently-started system.
		 */
		if (target_block == InvalidBlockNumber)
		{
			BlockNumber nblocks = RelationGetNumberOfBlocks(relation);
			if (nblocks > 0)
				target_block = nblocks - 1;
		}
	}

loop:
	while (target_block != InvalidBlockNumber)
	{
		/*
         * Read and exclusive-lock the target block, as well as the other
         * block if one was given, taking suitable care with lock ordering and
         * the possibility they are the same block.
         *
         * If the page-level all-visible flag is set, caller will need to
         * clear both that and the corresponding visibility map bit.  However,
         * by the time we return, we'll have x-locked the buffer, and we don't
         * want to do any I/O while in that state.  So we check the bit here
         * before taking the lock, and pin the page if it appears necessary.
         * Checking without the lock creates a risk of getting the wrong
         * answer, so we'll have to recheck after acquiring the lock.
         */
		if (other_fuffer == InvalidBuffer)
		{
			/* easy case */
			buffer = ReadBufferBI(relation, target_block, RBM_NORMAL, bistate);
			if (!TryLockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE, !test_last_block))
			{
				Assert(test_last_block);
				ReleaseBuffer(buffer);

				/* someone is using this block, give up and extend. */
				break;
			}
		}
		else if (other_block == target_block)
		{
			buffer = other_fuffer;
			/* also easy case */
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (other_block < target_block)
		{
			/* lock other buffer first */
			buffer = ReadBuffer(relation, target_block);
			LockBuffer(other_fuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else
		{
			/* lock target buffer first */
			buffer = ReadBuffer(relation, target_block);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(other_fuffer, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
         * Now we can check to see if there's enough free space here. If
         * so, we're done.
         */
		page = BufferGetPage(buffer);


		page_free_space = page_get_xheap_free_space(page);
		if (len + save_free_space <= page_free_space)
		{
			/* use this page as future insert target, too */
			RelationSetTargetBlock(relation, target_block);
			return buffer;
		}

		/*
         * Not enough space, so we must give up our page locks
         * and pin (if any) and prepare to look elsewhere
         */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		if (other_fuffer == InvalidBuffer)
			ReleaseBuffer(buffer);
		else if (other_block != target_block)
		{
			LockBuffer(other_fuffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
		}

		/* Without FSM, always fall out of the loop and extend */
		if (!use_fsm)
			break;

		/*
         * Update FSM as to condition of this page, and ask for another page
         * to try.
         */
		target_block = RecordAndGetPageWithFreeSpace(relation, target_block, page_free_space,
													len + save_free_space);

		/*
         * If the FSM knows nothing of the rel, try the last page before we
         * give up and extend. This's intend to use pages that are extended
         * one by one and not recorded in FSM as possible.
         *
         * The best is to record all pages into FSM using bulk-extend in later.
         */
		if (target_block == InvalidBlockNumber && !test_last_block &&
			other_fuffer == InvalidBuffer)
		{
			BlockNumber nblocks = RelationGetNumberOfBlocks(relation);
			if (nblocks > 0)
			{
				target_block = nblocks - 1;
			}
			test_last_block = true;
		}
	}

	/*
     * See if we can prune an existing block before extending the relation.
     */
	if (use_fsm && !force_extend)
	{
		target_block = relation_prune_optional(relation, len + save_free_space);
		if (target_block != InvalidBlockNumber)
			goto loop;
	}

	/*
     * Have to extend the relation.
     */
	need_lock = !RELATION_IS_LOCAL(relation);

	if (need_lock)
	{
		if (!use_fsm)
			LockRelationForExtension(relation, ExclusiveLock);
		else if (!ConditionalLockRelationForExtension(relation, ExclusiveLock))
		{
			/* Couldn't get the lock immediately; wait for it. */
			LockRelationForExtension(relation, ExclusiveLock);

			/*
			 * Check if some other backend has extended a block for us while
			 * we were waiting on the lock.
			 */
			target_block = GetPageWithFreeSpace(relation, len + save_free_space);
			/*
			 * If some other waiter has already extended the relation, we
			 * don't need to do so; just use the existing freespace.
			 */
			if (target_block != InvalidBlockNumber)
			{
				UnlockRelationForExtension(relation, ExclusiveLock);
				goto loop;
			}

			/* Time to bulk-extend. */
			xheap_relation_add_extra_blocks(relation, bistate);
		}
	}

	/*
     * In addition to whatever extension we performed above, we always add at
     * least one block to satisfy our own request.
     *
     * XXX This does a rather expensive lseek, but at the moment it is the
     * only way to accurately determine how many blocks are in a relation.  Is
     * it worth keeping an accurate file length in shared memory someplace,
     * rather than relying on the kernel to do it for us?
     */
	buffer = ReadBufferBI(relation, P_NEW, RBM_ZERO_AND_LOCK, bistate);
	/*
     * We need to initialize the empty new page.  Double-check that it really
     * is empty (this should never happen, but if it does we don't want to
     * risk wiping out valid data).
     */
	page = BufferGetPage(buffer);

	if (!PageIsNew(page))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("page %u of relation \"%s\" should be empty but is not",
							   BufferGetBlockNumber(buffer),
							   RelationGetRelationName(relation))));
	/*
     * It is possible that by the time we added new block to the relation,
     * another thread initialized it from RelationPruneBlockAndReturn().
     * We used to treat such condition as error in the earlier code but now
     * since we have two initializers, it seems to be valid that a new page
     * is initialized by the time the block creator thread could get the
     * buffer lock.
     */
	if (relation->rd_rel->relkind == RELKIND_TOASTVALUE)
		xpage_init(XPAGE_TOAST, page, BufferGetPageSize(buffer), XHEAP_SPECIAL_SIZE);
	else
		xpage_init(XPAGE_HEAP, page, BufferGetPageSize(buffer), XHEAP_SPECIAL_SIZE);
		
	MarkBufferDirty(buffer);
	/*
     * Release the file-extension lock; it's now OK for someone else to extend
     * the relation some more.
     */
	if (need_lock)
		UnlockRelationForExtension(relation, ExclusiveLock);

	/*
     * Lock the other buffer. It's guaranteed to be of a lower page number
     * than the new page. To conform with the deadlock prevent rules, we ought
     * to lock otherBuffer first, but that would give other backends a chance
     * to put tuples on our page. To reduce the likelihood of that, attempt to
     * lock the other buffer conditionally, that's very likely to work.
     * Otherwise we need to lock buffers in the correct order, and retry if
     * the space has been used in the mean time.
     *
     * Alternatively, we could acquire the lock on otherBuffer before
     * extending the relation, but that'd require holding the lock while
     * performing IO, which seems worse than an unlikely retry.
     */
	if (other_fuffer != InvalidBuffer)
	{
		Assert(other_fuffer != buffer);

		if (unlikely(!ConditionalLockBuffer(other_fuffer)))
		{
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			LockBuffer(other_fuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			/*
             * Because the buffer was unlocked for a while, it's possible,
             * although unlikely, that the page was filled. If so, just retry
             * from start.
             */
			if (len > page_get_xheap_free_space(page))
			{
				LockBuffer(other_fuffer, BUFFER_LOCK_UNLOCK);
				UnlockReleaseBuffer(buffer);

				goto loop;
			}
		}
	}

	if (len > page_get_xheap_free_space(page))
		/* We should not get here given the test at the top */
		elog(PANIC, "tuple is too big: tuple size %zu, page free size %zu", len,
			 page_get_xheap_free_space(page));

	/*
     * Remember the new page as our target for future insertions.
     *
     * XXX should we enter the new page into the free space map immediately,
     * or just keep it for this backend's exclusive use in the short run
     * (until VACUUM sees it)?	Seems to depend on whether you expect the
     * current backend to make more insertions or not, which is probably a
     * good bet most of the time.  So for now, don't add it to FSM yet.
     */
	RelationSetTargetBlock(relation, BufferGetBlockNumber(buffer));

	return buffer;
}
