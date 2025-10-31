/* -------------------------------------------------------------------------
 *
 * xbtrecycle.c
 *	  recycle page for xbtree.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * 
 * IDENTIFICATION
 *	  src/xbtree/xbtrecycle.c
 *
 * -------------------------------------------------------------------------
 */


#include "postgres.h"
#include "access/transam.h"
#include "access/nbtree.h"
#include "access/xloginsert.h"
#include "storage/smgr.h"
#include "xbtree/xbtree.h"
#include "xbtree/xbttup.h"
#include "xheap/xheap.h"
#include "xstore.h"
#include "commands/tablespace.h"
#include "port/atomics.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "utils/palloc.h"
#include "util/xxact.h"
#include "util/xrmgr.h"
#include "xbtree/xbtrecycle.h"

typedef struct
{
	RelFileLocator rnode;
	ForkNumber	fork_num;
	BlockNumber blkno;
} redo_buffer_tag;

typedef struct
{
	Page page;	// pagepointer
	Size page_size;
#ifdef USE_ASSERT_CHECKING
	bool ignore_check;
#endif
} redo_page_info;

typedef struct
{
	XLogRecPtr	  lsn; /* block cur lsn */
	Buffer		  buf;
	redo_buffer_tag block_info;
	redo_page_info  page_info;
	int			  dirty_flag; /* true if the buffer changed */
} redo_buffer_info;

typedef struct xbt_recycle_meta_data {
	uint32 flags; /* reserved */
	pg_atomic_uint32 head_blkno;
	pg_atomic_uint32 tail_blkno;
	BlockNumber nblocks_upper; /* optimistic estimate of used blocks */
	/* not used yet, reserved for possible upgrades */
	BlockNumber nblocks_lower; /* number of blocks that have been used correctly */
} xbt_recycle_meta_data;

typedef xbt_recycle_meta_data* xbt_recycle_meta;

const BlockNumber min_recycle_queue_block_number = 6;
const uint16	  invalid_offset = ((uint16) 0) - 1;

static uint32 block_get_max_items(BlockNumber blkno);
static void	  xbtree_init_recycle_queue_page(Relation rel, Page page, Size size,
										 BlockNumber blkno);
static void	  xbtree_recycle_queue_discard_page(Relation rel, XBTRecycleQueueAddress addr);
static void	  xbtree_recycle_queue_add_page(Relation rel, XBTRecycleForkNumber fork_number,
										BlockNumber blkno, FullTransactionId xid);
static Buffer recycle_queue_get_endpoint_page(Relation rel, XBTRecycleForkNumber fork_number, 
											bool need_head, int access);
static Buffer step_next_page(Relation rel, Buffer buf);
static Buffer get_available_page_on_page(Relation rel, XBTRecycleForkNumber fork_number,
									 Buffer buf, FullTransactionId water_level_xid,
									 XBTRecycleQueueAddress *addr, bool *continue_scan);
static Buffer move_to_endpoint_page(Relation rel, Buffer buf, bool need_head, int access);
static uint16 page_allocate_item(Buffer buf);
static void	  recycle_queue_link_new_page(Relation rel, Buffer left_buf, Buffer new_buf);
static bool	  queue_page_is_empty(Buffer buf);
static Buffer acquire_next_available_queue_page(Relation rel, Buffer buf,
											XBTRecycleForkNumber fork_number);
static void	  insert_on_recycle_queue_page(Relation rel, Buffer buf, uint16 offset,
									   BlockNumber blkno, FullTransactionId xid);
static void	  remove_one_item_from_page(Relation rel, Buffer buf, uint16 offset);
static void xbtree2_xlog_recycle_queue_init_page_operator_curr_page(redo_buffer_info *buffer,
													 void			*record_data);
static void xbtree2_xlog_recycle_queue_init_page_operator_adjacent_page(redo_buffer_info *buffer,
														 void *record_data, bool is_left);
static void xbtree2_xlog_recycle_queue_endpoint_operator_left_page(redo_buffer_info *buffer,
													 void			*record_data);
static void xbtree2_xlog_recycle_queue_endpoint_operator_right_page(redo_buffer_info *buffer,
													  void			 *record_data);
static void xbtree2_xlog_recycle_queue_modify_operator_page(redo_buffer_info *buffer, void *record_data);

const BlockNumber first_block_number = 0;
const BlockNumber first_normal_block_number =
	2; /* 0 and 1 are pages which include meta data */
const uint16 first_normal_offset = 0;
const uint16 other_block_offset =
	((uint16) 0) - 2; /* indicate that previous or next item is in other block */

#define IsMetaPage(blkno) (blkno < first_normal_block_number)
#define IsNormalOffset(offset) (offset < other_block_offset)

#define PageSetPruneXid(page, xid) \
	(((PageHeader)(page))->pd_prune_xid = xid)

#define PageGetPruneXid(page) \
	(((PageHeader)(page))->pd_prune_xid)

#define IndexPageIsPrunable(pd_prune_xid, oldest_xmin)                   \
	(AssertMacro(FullTransactionIdIsNormal(oldest_xmin)),       \
		FullTransactionIdIsValid(pd_prune_xid) && \
			FullTransactionIdPrecedes(pd_prune_xid, oldest_xmin))

static inline void
build_redo_buffer_info(redo_buffer_info *buffer, XLogReaderState *record)
{
	buffer->page_info.page = BufferGetPage(buffer->buf);
	buffer->page_info.page_size = BufferGetPageSize(buffer->buf);
	buffer->lsn = record->EndRecPtr;
}

static inline XLogRedoAction
xlog_read_buffer_for_redo_ext(XLogReaderState *record, uint8 block_id,
							  redo_buffer_info *buffer)
{
	XLogRedoAction action =
		XLogReadBufferForRedoExtended(record, block_id, RBM_NORMAL, false, &buffer->buf);
	build_redo_buffer_info(buffer, record);
	return action;
}

static uint32
block_get_max_items(BlockNumber blkno)
{
	uint32 free_space =
		BLCKSZ - sizeof(PageHeaderData) - offsetof(XBTRecycleQueueHeaderData, items);
	if (IsMetaPage(blkno))
		free_space -= sizeof(xbt_recycle_meta_data);
	return free_space / sizeof(XBTRecycleQueueItemData);
}

XBTRecycleQueueHeader
GetRecycleQueueHeader(Page page, BlockNumber blkno)
{
	if (!IsMetaPage(blkno))
		return (XBTRecycleQueueHeader) PageGetContents(page);
	return (XBTRecycleQueueHeader) (((char *) PageGetContents(page)) +
									sizeof(xbt_recycle_meta_data));
}

static XBTRecycleQueueItem
HeaderGetItem(XBTRecycleQueueHeader header, uint16 offset)
{
	if (offset >= 0 && offset <= XBTRecycleMaxItems)
		return &header->items[offset];
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("trying to fetch invalid recycle queue item with offset %u", offset),
			 errhint("Can fixed by REINDEX this index")));
	pg_unreachable(); /* won't reach here */
}

static void
xbtree_init_recycle_queue_page(Relation rel, Page page, Size size, BlockNumber blkno)
{
	XBTRecycleQueueHeader header;

	PageInit(page, size, 0);

	/* init header */
	header = GetRecycleQueueHeader(page, blkno);
	header->flags = 0;
	header->prev_blkno = InvalidBlockNumber;
	header->next_blkno = InvalidBlockNumber;
	header->head = invalid_offset;
	header->tail = invalid_offset;
	header->free_items = block_get_max_items(blkno);
	header->free_list_head = invalid_offset;

	/* init meta if needed */
	if (IsMetaPage(blkno))
	{
		xbt_recycle_meta meta;

		header->flags = (URQ_HEAD_PAGE | URQ_TAIL_PAGE);

		meta = (xbt_recycle_meta) PageGetContents(page);
		meta->flags = 0;
		pg_atomic_write_u32(&(meta->head_blkno), blkno);
		pg_atomic_write_u32(&(meta->tail_blkno), blkno);
		/* setup nblocks */
		if (rel != NULL)
		{
			RelationGetSmgr(rel);
			meta->nblocks_lower = smgrnblocks(rel->rd_smgr, MAIN_FORKNUM);
			meta->nblocks_upper = meta->nblocks_lower;
		}
		else
		{
			meta->nblocks_lower = 0; /* may update later */
			meta->nblocks_upper = 0; /* may update later */
		}
	}
}

static bool
RecycleQueueInitialized(Relation rel)
{
	BlockNumber nblocks;

	/* open smgr, might have to re-open if a cache flush happened */
	RelationGetSmgr(rel);
	nblocks = rel->rd_smgr->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM];

	if (nblocks == InvalidBlockNumber || nblocks < min_recycle_queue_block_number)
	{
		if (smgrexists(rel->rd_smgr, FSM_FORKNUM))
		{
			rel->rd_smgr->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = smgrnblocks(rel->rd_smgr, FSM_FORKNUM);
			nblocks = rel->rd_smgr->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM];
		}
		else
		{
			rel->rd_smgr->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = 0;
			nblocks = 0;
		}
	}

	return nblocks >= min_recycle_queue_block_number;
}

/* init a new page with given prev and next blkno */
void
xbtree_recycle_queue_init_page(Relation rel, Page page, BlockNumber blkno,
						   BlockNumber prev_blkno, BlockNumber next_blkno)
{
	XBTRecycleQueueHeader header;

	PageInit(page, BLCKSZ, 0);
	xbtree_init_recycle_queue_page(rel, page, BLCKSZ, blkno);
	header = GetRecycleQueueHeader(page, blkno);
	header->prev_blkno = prev_blkno;
	header->next_blkno = next_blkno;
}

/* record the chain changes in prev or next page */
void
xbtree_recycle_queue_change_chain(Buffer buf, BlockNumber new_blkno, bool set_next)
{
	XBTRecycleQueueHeader header =
		GetRecycleQueueHeader(BufferGetPage(buf), BufferGetBlockNumber(buf));
	if (set_next)
		header->next_blkno = new_blkno;
	else
		header->prev_blkno = new_blkno;
}

static void
LogInitRecycleQueuePage(Relation rel, Buffer buf, Buffer left_buf, Buffer right_buf)
{
	xl_xbtree2_recycle_queue_init_page xlrec;
	Page							   page = BufferGetPage(buf);
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));

	xlrec.inserting_new_page = false;
	xlrec.prev_blkno = header->prev_blkno;
	xlrec.curr_blkno = BufferGetBlockNumber(buf);
	xlrec.next_blkno = header->next_blkno;

	if (BufferIsValid(left_buf))
	{
		Page	   left_page;
		Page	   right_page;
		XLogRecPtr recptr;

		xlrec.inserting_new_page = true;
		left_page = BufferGetPage(left_buf);
		right_page = BufferGetPage(right_buf);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfXBTree2RecycleQueueInitPage);
		XLogRegisterBuffer(XBTREE2_RECYCLE_QUEUE_INIT_PAGE_CURR_BLOCK_NUM, buf,
						   REGBUF_WILL_INIT);
		XLogRegisterBuffer(XBTREE2_RECYCLE_QUEUE_INIT_PAGE_LEFT_BLOCK_NUM, left_buf, 0);
		XLogRegisterBuffer(XBTREE2_RECYCLE_QUEUE_INIT_PAGE_RIGHT_BLOCK_NUM, right_buf, 0);

		recptr = XLogInsert(RM_XBTREE2_ID, XLOG_XBTREE2_RECYCLE_QUEUE_INIT_PAGE);

		PageSetLSN(page, recptr);
		PageSetLSN(left_page, recptr);
		PageSetLSN(right_page, recptr);
	}
	else
	{
		XLogRecPtr recptr;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfXBTree2RecycleQueueInitPage);
		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);

		recptr = XLogInsert(RM_XBTREE2_ID, XLOG_XBTREE2_RECYCLE_QUEUE_INIT_PAGE);

		PageSetLSN(page, recptr);
	}
}

/* generate prev and next blkno for this page, and init it */
static void
InitRecycleQueueInitialPage(Relation rel, Buffer buf)
{
	BlockNumber blkno = BufferGetBlockNumber(buf);
	Page		page = BufferGetPage(buf);
	/*
	 * page 0 2 4 form a circle (Potential Empty Page Queue).
	 * page 1 3 5 form another circle (Available Page Queue).
	 */
	const int	adj_block_number_diff = 2;
	BlockNumber prev_blkno = (blkno + min_recycle_queue_block_number - adj_block_number_diff) %
							min_recycle_queue_block_number;
	BlockNumber next_blkno = (blkno + adj_block_number_diff) % min_recycle_queue_block_number;

	/* Do the update.  No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	xbtree_recycle_queue_init_page(rel, page, blkno, prev_blkno, next_blkno);

	MarkBufferDirty(buf);

	/* xlog stuff */
	if (RelationNeedsWAL(rel))
		/* XLOG stuff, and set LSN as well */
		LogInitRecycleQueuePage(rel, buf, InvalidBuffer, InvalidBuffer);

	END_CRIT_SECTION();
}

Buffer
read_recycle_queue_buffer(Relation rel, BlockNumber blkno)
{
	Buffer buf = ReadBufferExtended(rel, FSM_FORKNUM, blkno, RBM_NORMAL, NULL);
	/* initial pages may not initialized correctly, before return it we need to check */
	if (blkno != P_NEW && blkno < min_recycle_queue_block_number &&
		PageIsNew(BufferGetPage(buf)))
	{
		LockBuffer(buf, BT_WRITE);
		InitRecycleQueueInitialPage(rel, buf);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}
	return buf;
}

void
xbt_init_recycle_queue(Relation rel)
{
	BlockNumber nblocks_now;

	LockRelationForExtension(rel, ExclusiveLock);
	if (RecycleQueueInitialized(rel))
	{
		/* initialized by other thread, skip */
		UnlockRelationForExtension(rel, ExclusiveLock);
		return;
	}

	RelationGetSmgr(rel);
	/* create the urq fork if not exists */
	if ((rel->rd_smgr->smgr_cached_nblocks[FSM_FORKNUM] == InvalidBlockNumber ||
		 rel->rd_smgr->smgr_cached_nblocks[FSM_FORKNUM] == 0) &&
		!smgrexists(rel->rd_smgr, FSM_FORKNUM))
		smgrcreate(rel->rd_smgr, FSM_FORKNUM, InRecovery);
	nblocks_now = smgrnblocks(rel->rd_smgr, FSM_FORKNUM);
	Assert(nblocks_now <= min_recycle_queue_block_number);

	/* first step, check existing pages */
	for (BlockNumber blkno = 0; blkno < nblocks_now; blkno++)
	{
		Page   page;
		Buffer buf;

		buf = read_recycle_queue_buffer(rel, blkno);
		LockBuffer(buf, BT_WRITE);
		page = BufferGetPage(buf);

		if (PageIsNew(page) || PageGetLSN(page) == InvalidXLogRecPtr)
		{
			/* page is not initialized correctly, re-init it */
			InitRecycleQueueInitialPage(rel, buf);
		}

		UnlockReleaseBuffer(buf);
	}

	/* second step, create necessary pages */
	for (BlockNumber blkno = nblocks_now; blkno < min_recycle_queue_block_number; blkno++)
	{
		Buffer buf = read_recycle_queue_buffer(rel, P_NEW);
		LockBuffer(buf, BT_WRITE);
		/* check that the blkno is what we expected */
		if (BufferGetBlockNumber(buf) != blkno)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("corrupted index recycle queue of index \"%s\": "
								   "expected blkno is %u, actual blkno is %u",
								   RelationGetRelationName(rel), blkno,
								   BufferGetBlockNumber(buf))));
		InitRecycleQueueInitialPage(rel, buf);
		UnlockReleaseBuffer(buf);
	}

	UnlockRelationForExtension(rel, ExclusiveLock);
}

static bool
XBTreeTryRecycleEmptyPageInternal(Relation rel)
{
	XBTRecycleQueueAddress addr;
	Page				   page;
	XBTPageOpaqueInternal  opaque;
	Buffer				   buf;

	buf = xbtree_get_available_page(rel, RECYCLE_EMPTY_FORK, &addr);

	if (!BufferIsValid(buf))
		return false; /* no available page to recycle */

	page = BufferGetPage(buf);
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	if (P_ISDELETED(opaque))
	{
		/* deleted by other routine earlier, skip */
		_bt_relbuf(rel, buf);
		xbtree_recycle_queue_discard_page(rel, addr);
		return true;
	}


	if (xbt_page_prune_opt(rel, buf, true))
	{
		/* successfully deleted, move to freed page queue */
		xbtree_recycle_queue_add_page(rel, RECYCLE_FREED_FORK, addr.index_blkno,
								  ReadNextFullTransactionId());
	}
	/* whether the page can be deleted or not, discard from the queue */
	xbtree_recycle_queue_discard_page(rel, addr);
	return true;
}

void
xbtree_try_recycle_empty_page(Relation rel)
{
	bool first_try_succeed = XBTreeTryRecycleEmptyPageInternal(rel);
	if (first_try_succeed)
		/* try to recycle the second page */
		XBTreeTryRecycleEmptyPageInternal(rel);
}

void
xbtree_record_free_page(Relation rel, BlockNumber blkno, FullTransactionId xid)
{
	xbtree_recycle_queue_add_page(rel, RECYCLE_FREED_FORK, blkno, xid);
}

void
xbtree_record_empty_page(Relation rel, BlockNumber blkno, FullTransactionId xid)
{
	xbtree_recycle_queue_add_page(rel, RECYCLE_EMPTY_FORK, blkno, xid);
}

void
xbtree_record_used_page(Relation rel, XBTRecycleQueueAddress addr)
{
	if (addr.queue_fuf != InvalidBuffer)
		xbtree_recycle_queue_discard_page(rel, addr);
}

static Buffer
step_next_page(Relation rel, Buffer buf)
{
	Page				  page = BufferGetPage(buf);
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));

	BlockNumber next_blkno = header->next_blkno;
	Buffer		next_buf = read_recycle_queue_buffer(rel, next_blkno);

	UnlockReleaseBuffer(buf);
	LockBuffer(next_buf, BT_WRITE);

	return next_buf;
}

static Buffer
get_available_page_on_page(Relation rel, XBTRecycleForkNumber fork_number, Buffer buf,
					   FullTransactionId water_level_xid, XBTRecycleQueueAddress *addr,
					   bool *continue_scan)
{
	Page				  page = BufferGetPage(buf);
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));

	uint16 cur_offset = header->head;
	while (IsNormalOffset(cur_offset))
	{
		XBTRecycleQueueItem item = HeaderGetItem(header, cur_offset);
		Buffer				target_buf;

		if (FullTransactionIdFollowsOrEquals(item->xid, water_level_xid))
		{
			*continue_scan = false;
			return InvalidBuffer;
		}
		if (!BlockNumberIsValid(item->blkno))
		{
			cur_offset = item->next;
			continue;
		}
		target_buf = ReadBuffer(rel, item->blkno);
		if (ConditionalLockBuffer(target_buf))
		{
			bool				  page_usable;
			XBTPageOpaqueInternal opaque;

			_bt_checkpage(rel, target_buf);
			page_usable = true;
			if (fork_number == RECYCLE_FREED_FORK)
				page_usable = xbtree_page_recyclable(BufferGetPage(target_buf));
			else if (fork_number == RECYCLE_EMPTY_FORK)
			{
				/* make sure that it's not half-dead or the deletion is not reserved yet */
				Page		  index_page = BufferGetPage(target_buf);
				XBTPageOpaque opaque = (XBTPageOpaque) PageGetSpecialPointer(index_page);
				if (P_ISHALFDEAD((XBTPageOpaqueInternal) opaque))
				{
					FullTransactionId previous_xact = opaque->xact;
					if (FullTransactionIdIsValid(previous_xact) &&
						xstore_transaction_id_is_in_progress(previous_xact))
						page_usable = false;
				}
			}

			if (page_usable)
			{
				*continue_scan = false;
				addr->queue_fuf = buf;
				addr->index_blkno = item->blkno;
				addr->offset = cur_offset;
				return target_buf;
			}

			opaque =
				(XBTPageOpaqueInternal) PageGetSpecialPointer(BufferGetPage(target_buf));

			if (!P_ISDELETED(opaque))
			{
				/* this page is reused by others, we help to mark it as unusable */
				item->blkno = InvalidBlockNumber;
				MarkBufferDirtyHint(buf, false);
			}
			_bt_relbuf(rel, target_buf);
		}
		else
			ReleaseBuffer(target_buf);
		cur_offset = item->next;
	}
	*continue_scan = (cur_offset == other_block_offset);
	return InvalidBuffer;
}

Buffer
xbtree_get_available_page(Relation rel, XBTRecycleForkNumber fork_number,
					   XBTRecycleQueueAddress *addr)
{
	BlockNumber		  nblocks;
	bool			  meta_changed;
	Buffer			  meta_buf;
	xbt_recycle_meta	  meta_data;
	const BlockNumber meta_block_number = fork_number;
	FullTransactionId oldest_xmin;
	Buffer queue_buf = recycle_queue_get_endpoint_page(rel, fork_number, true, BT_READ);
	Buffer index_buf = InvalidBuffer;
	bool   continue_scan = false;
	oldest_xmin.value = pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid);
	for (;;)
	{
		index_buf = get_available_page_on_page(rel, fork_number, queue_buf, oldest_xmin, addr,
										  &continue_scan);
		if (!continue_scan)
			break;
		queue_buf = step_next_page(rel, queue_buf);
	}

	LockBuffer(queue_buf, BUFFER_LOCK_UNLOCK);

	if (index_buf != InvalidBuffer)
		return index_buf;

	/* no useful page, but we will check non-tracked page if we are finding free pages */
	ReleaseBuffer(queue_buf);
	addr->queue_fuf = InvalidBuffer;

	if (fork_number == RECYCLE_EMPTY_FORK)
		return InvalidBuffer;

	/* no available page found, but we can check new created pages */
	nblocks = RelationGetNumberOfBlocks(rel);
	meta_changed = false;

	meta_buf = read_recycle_queue_buffer(rel, meta_block_number);
	LockBuffer(meta_buf, BT_READ);
	meta_data = (xbt_recycle_meta) PageGetContents(BufferGetPage(meta_buf));
	for (BlockNumber cur_blkno = meta_data->nblocks_upper; cur_blkno < nblocks; cur_blkno++)
	{
		if (meta_data->nblocks_upper > cur_blkno)
			continue;
		if (meta_data->nblocks_upper <= cur_blkno)
		{
			meta_data->nblocks_upper = cur_blkno + 1; /* update nblocks in meta */
			meta_changed = true;
		}
		index_buf = ReadBuffer(rel, cur_blkno);
		if (ConditionalLockBuffer(index_buf))
		{
			if (PageIsNew(BufferGetPage(index_buf)))
				break;
			LockBuffer(index_buf, BUFFER_LOCK_UNLOCK);
		}
		ReleaseBuffer(index_buf);
		index_buf = InvalidBuffer;
	}

	if (meta_changed)
		MarkBufferDirtyHint(meta_buf, false);
	UnlockReleaseBuffer(meta_buf);

	addr->queue_fuf = InvalidBuffer; /* it's not allocated from any queue page */
	return index_buf;
}

void
xbtree_recycle_queue_page_change_endpoint_left_page(Buffer buf, bool is_head)
{
	uint32				  endpoint_flag = (is_head ? URQ_HEAD_PAGE : URQ_TAIL_PAGE);
	XBTRecycleQueueHeader header =
		GetRecycleQueueHeader(BufferGetPage(buf), BufferGetBlockNumber(buf));
	if (is_head)
	{
		header->head = invalid_offset;
		header->tail = invalid_offset;
	}
	else
	{
		XBTRecycleQueueItem tail_item;
		Assert(IsNormalOffset(header->tail));
		tail_item = HeaderGetItem(header, header->tail);
		tail_item->next = other_block_offset;
	}
	header->flags &= ~endpoint_flag;
}

void
xbtree_recycle_queue_page_change_endpoint_right_page(Buffer buf, bool is_head)
{
	uint32				  endpoint_flag = (is_head ? URQ_HEAD_PAGE : URQ_TAIL_PAGE);
	XBTRecycleQueueHeader header =
		GetRecycleQueueHeader(BufferGetPage(buf), BufferGetBlockNumber(buf));
	if (is_head)
	{
		if (IsNormalOffset(header->head))
		{
			XBTRecycleQueueItem head_item = HeaderGetItem(header, header->head);
			head_item->prev = invalid_offset;
		}
		else
			header->head = invalid_offset;
	}
	else
		/* new created tail page must be empty */
		Assert(header->head == invalid_offset);
	header->flags |= endpoint_flag;
}

static void
recycle_queue_change_endpoint(Relation rel, Buffer buf, Buffer next_buf, bool is_head)
{
	Page page = BufferGetPage(buf);
	Page next_page = BufferGetPage(next_buf);

	/* Do the update.  No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	xbtree_recycle_queue_page_change_endpoint_left_page(buf, is_head);
	xbtree_recycle_queue_page_change_endpoint_right_page(next_buf, is_head);

	MarkBufferDirty(buf);
	MarkBufferDirty(next_buf);

	/* xlog stuff */
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr recptr;

		xl_xbtree2_recycle_queue_endpoint xlrec;
		xlrec.is_head = is_head;
		xlrec.left_blkno = BufferGetBlockNumber(buf);
		xlrec.right_blkno = BufferGetBlockNumber(next_buf);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfXBTree2RecycleQueueEndpoint);
		XLogRegisterBuffer(0, buf, 0);
		XLogRegisterBuffer(1, next_buf, 0);

		recptr = XLogInsert(RM_XBTREE2_ID, XLOG_XBTREE2_RECYCLE_QUEUE_ENDPOINT);

		PageSetLSN(page, recptr);
		PageSetLSN(next_page, recptr);
	}

	END_CRIT_SECTION();
}

static Buffer
move_to_endpoint_page(Relation rel, Buffer buf, bool need_head, int access)
{
	Page				  page;
	XBTRecycleQueueHeader header;
	uint32				  endpoint_flag;
restart:

	page = BufferGetPage(buf);
	header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));

	endpoint_flag = (need_head ? URQ_HEAD_PAGE : URQ_TAIL_PAGE);
	while ((header->flags & endpoint_flag) == 0)
	{
		buf = step_next_page(rel, buf);
		page = BufferGetPage(buf);
		header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));
	}

	if (IsNormalOffset(header->head))
	{
		XBTRecycleQueueItem head_item = HeaderGetItem(header, header->head);
		if (!BlockNumberIsValid(head_item->blkno))
		{
			if (access == BT_READ)
			{
				/* we need to switch the head, drop read lock and acquire write lock */
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				access = BT_WRITE;
				LockBuffer(buf, BT_WRITE);
				goto restart;
			}
			remove_one_item_from_page(rel, buf, header->head);
		}
		header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));
	}

	if (need_head && !IsNormalOffset(header->head) && (header->flags & URQ_TAIL_PAGE) == 0)
	{
		Buffer next_buf;
		/* it's Head page, but header->head is not normal and it not the Tail. Now we have to switch the Head */
		if (access == BT_READ)
		{
			/* we need to switch the head, drop read lock and acquire write lock */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			access = BT_WRITE;
			LockBuffer(buf, BT_WRITE);
			goto restart;
		}
		/* by here, buf is write locked */
		next_buf = read_recycle_queue_buffer(rel, header->next_blkno);
		LockBuffer(next_buf, BT_WRITE);
		/* change endpoint, and insert xlog */
		recycle_queue_change_endpoint(rel, buf, next_buf, true);
		/* release current page, and return the next page */
		UnlockReleaseBuffer(buf);
		buf = next_buf;
	}

	return buf;
}

static uint16
page_allocate_item(Buffer buf)
{
	Page				  page = BufferGetPage(buf);
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));
	if (header->free_items > 0)
		/* allocate from freeItems */
		return header->free_items - 1;
	if (header->free_list_head != invalid_offset)
		/* allocate from freeList */
		return header->free_list_head;
	return invalid_offset;
}

static Buffer
recycle_queue_extend(Relation rel)
{
	Buffer buf;

	LockRelationForExtension(rel, ExclusiveLock);
	buf = read_recycle_queue_buffer(rel, P_NEW);
	LockBuffer(buf, BT_WRITE);
	UnlockRelationForExtension(rel, ExclusiveLock);
	return buf;
}

static void
recycle_queue_link_new_page(Relation rel, Buffer left_buf, Buffer new_buf)
{
	/* new page already allocated, link it into the list */
	BlockNumber			  left_blkno = BufferGetBlockNumber(left_buf);
	Page				  left_page = BufferGetPage(left_buf);
	XBTRecycleQueueHeader left_header = GetRecycleQueueHeader(left_page, left_blkno);

	BlockNumber			  blkno = BufferGetBlockNumber(new_buf);
	Page				  page = BufferGetPage(new_buf);
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(page, blkno);

	BlockNumber right_blkno = left_header->next_blkno;
	Buffer		right_buf = read_recycle_queue_buffer(rel, right_blkno);

	Page				  right_page;
	XBTRecycleQueueHeader right_header;
	LockBuffer(right_buf, BT_WRITE);
	right_page = BufferGetPage(right_buf);
	right_header = GetRecycleQueueHeader(right_page, right_blkno);

	/* Do the update.  No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	xbtree_init_recycle_queue_page(rel, page, BLCKSZ, BufferGetBlockNumber(new_buf));

	left_header->next_blkno = blkno;
	header->prev_blkno = left_blkno;
	header->next_blkno = right_blkno;
	right_header->prev_blkno = blkno;

	MarkBufferDirty(left_buf);
	MarkBufferDirty(new_buf);
	MarkBufferDirty(right_buf);

	/* xlog stuff */
	if (RelationNeedsWAL(rel))
		/* XLOG stuff, and set LSN as well */
		LogInitRecycleQueuePage(rel, new_buf, left_buf, right_buf);

	END_CRIT_SECTION();

	UnlockReleaseBuffer(right_buf);
}

static bool
queue_page_is_empty(Buffer buf)
{
	XBTRecycleQueueHeader header =
		GetRecycleQueueHeader(BufferGetPage(buf), BufferGetBlockNumber(buf));
	return (header->flags & URQ_HEAD_PAGE) == 0 && (header->flags & URQ_TAIL_PAGE) == 0 &&
		   !IsNormalOffset(header->head);
}

/* Acquire a page to be the new tail block */
static Buffer
acquire_next_available_queue_page(Relation rel, Buffer buf, XBTRecycleForkNumber fork_number)
{
	Page				  page = BufferGetPage(buf);
	BlockNumber			  blkno = BufferGetBlockNumber(buf);
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(page, blkno);
	Buffer				  new_buf;

	Buffer target_buf = read_recycle_queue_buffer(rel, header->next_blkno);
	LockBuffer(target_buf, BT_WRITE);
	if (queue_page_is_empty(target_buf))
	{
		/* next page is empty, we can reuse it */

		recycle_queue_change_endpoint(rel, buf, target_buf, false);
		UnlockReleaseBuffer(buf);
		return target_buf;
	}
	/* next block is still in use, we need to get a new page */
	UnlockReleaseBuffer(target_buf);
	/* drop write lock of the tail page before acquire relation extend lock */
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	new_buf = recycle_queue_extend(rel);
	/* relock and move to the current tail */
	LockBuffer(buf, BT_WRITE);
	buf = move_to_endpoint_page(rel, buf, false, BT_WRITE);
	recycle_queue_link_new_page(rel, buf, new_buf);
	if (page_allocate_item(buf) != invalid_offset)
	{
		/* there is still space left in the current tail page, no need to switch tail */
		UnlockReleaseBuffer(new_buf);
		return buf;
	}
	/* by here, Exclusive lock of new_buf is acquired */
	recycle_queue_change_endpoint(rel, buf, new_buf, false);
	UnlockReleaseBuffer(buf);
	return new_buf;
}

static void
try_fix_meta_data(Buffer meta_buf, uint32 old_val, uint32 new_val, bool is_head)
{
	xbt_recycle_meta	  meta_data = (xbt_recycle_meta) PageGetContents(BufferGetPage(meta_buf));
	pg_atomic_uint32 *addr = (is_head ? &(meta_data->head_blkno) : &(meta_data->tail_blkno));
	if (pg_atomic_compare_exchange_u32(addr, &old_val, new_val))
	{
		/* update succeed, mark buffer dirty */
		if (ConditionalLockBuffer(meta_buf))
		{
			MarkBufferDirty(meta_buf);
			LockBuffer(meta_buf, BUFFER_LOCK_UNLOCK);
		}
	}
}

static Buffer
recycle_queue_get_endpoint_page(Relation rel, XBTRecycleForkNumber fork_number, bool need_head,
							int access)
{
	const BlockNumber meta_block_number = fork_number;
	Buffer			  meta_buf;
	xbt_recycle_meta	  meta_data;
	pg_atomic_uint32 *addr;
	BlockNumber		  given_blkno;
	Buffer			  buf;
	BlockNumber		  true_blkno;

	if (!RecycleQueueInitialized(rel))
		xbt_init_recycle_queue(rel);

	meta_buf = read_recycle_queue_buffer(rel, meta_block_number);
	/* read out tail block number without lock */
	meta_data = (xbt_recycle_meta) PageGetContents(BufferGetPage(meta_buf));
	addr = (need_head ? &(meta_data->head_blkno) : &(meta_data->tail_blkno));
	given_blkno = pg_atomic_read_u32(addr);

	/* get the advised block, and move to the true endpoint if necessary */
	buf = read_recycle_queue_buffer(rel, given_blkno);
	LockBuffer(buf, access);
	buf = move_to_endpoint_page(rel, buf, need_head, access);

	/* try to fix the information in the meta if necessary */
	true_blkno = BufferGetBlockNumber(buf);
	if (true_blkno != given_blkno)
		try_fix_meta_data(meta_buf, given_blkno, true_blkno, need_head);
	ReleaseBuffer(meta_buf);

	/* okay, we got the target page */
	return buf;
}

/* we insert into the tail page, ensure that the items in the page are arranged by XID incrementally */
static void
xbtree_recycle_queue_add_page(Relation rel, XBTRecycleForkNumber fork_number,
						  BlockNumber blkno, FullTransactionId xid)
{
	/* get the tail page */
	Buffer buf = recycle_queue_get_endpoint_page(rel, fork_number, false, BT_WRITE);

	uint16 offset = page_allocate_item(buf);
	if (offset == invalid_offset)
	{
		/* tail page is full, allocate in next page */
		buf = acquire_next_available_queue_page(rel, buf, fork_number);
		offset = page_allocate_item(buf); /* new page must be empty yet */
	}

	Assert(offset != invalid_offset);
	insert_on_recycle_queue_page(rel, buf, offset, blkno, xid);
}

static void
log_modify_page(Buffer buf, bool is_insert, uint16 offset, XBTRecycleQueueItem item,
			  XBTRecycleQueueHeader header, uint16 free_list_offset)
{
	Page							page = BufferGetPage(buf);
	xl_xbtree2_recycle_queue_modify xlrec;
	XLogRecPtr						recptr;

	xlrec.is_insert = is_insert;
	xlrec.offset = offset;
	xlrec.blkno = BufferGetBlockNumber(buf);
	xlrec.item = *item;
	xlrec.header = *header;
	xlrec.header.free_list_head = free_list_offset;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfXBTree2RecycleQueueModify);
	XLogRegisterBuffer(0, buf, 0);

	recptr = XLogInsert(RM_XBTREE2_ID, XLOG_XBTREE2_RECYCLE_QUEUE_MODIFY);

	PageSetLSN(page, recptr);
}

static void
insert_on_recycle_queue_page(Relation rel, Buffer buf, uint16 offset, BlockNumber blkno,
						 FullTransactionId xid)
{
	Page				  page = BufferGetPage(buf);
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));

	XBTRecycleQueueItem item = HeaderGetItem(header, offset);

	/* Do the update.  No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	item->blkno = blkno;
	item->xid = xid;
	if (header->free_list_head == offset)
		/* allocated from freeList */
		header->free_list_head = item->next;
	if (header->free_items == offset + 1)
		/* allocated from freeItems */
		header->free_items--;

	if (!IsNormalOffset(header->head))
	{
		/* empty page, previous item of a new empty page is in other page */
		item->prev = header->head; /* invalid_offset or other_block_offset */
		item->next = invalid_offset;
		header->head = offset;
		header->tail = offset;
	}
	else
	{
		XBTRecycleQueueItem head_item = HeaderGetItem(header, header->head);
		if (FullTransactionIdPrecedesOrEquals(xid, head_item->xid))
		{
			/* insert in the front */
			item->prev = head_item->prev;
			item->next = header->head; /* old head offset */
			head_item->prev = offset;
			header->head = offset;
		}
		else
		{
			uint16				cur_offset = header->tail;
			XBTRecycleQueueItem cur_item = HeaderGetItem(header, cur_offset);
			/* find the insert loc */
			while (FullTransactionIdPrecedes(xid, cur_item->xid))
			{
				cur_offset = cur_item->prev;
				cur_item = HeaderGetItem(header, cur_offset);
			}
			item->prev = cur_offset;
			item->next = cur_item->next;
			if (IsNormalOffset(cur_item->next))
			{
				XBTRecycleQueueItem next_item = HeaderGetItem(header, cur_item->next);
				next_item->prev = offset;
			}
			else
				/* cur_item is tail, update the tail of this page */
				header->tail = offset;
			cur_item->next = offset;
		}
	}

	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
		log_modify_page(buf, true, offset, item, header, header->free_list_head);

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
}

void
xbtree_xlog_recycle_queue_modify_page(Buffer buf, xl_xbtree2_recycle_queue_modify *xlrec)
{
	XBTRecycleQueueItem item;
	Page				page = BufferGetPage(buf);
	BlockNumber			blkno = BufferGetBlockNumber(buf);
	/* restore header and the modified item */
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(page, blkno);
	*header = xlrec->header;
	item = HeaderGetItem(header, xlrec->offset);
	*item = xlrec->item;

	if (xlrec->is_insert)
	{
		/* restore adjacent items */
		if (IsNormalOffset(item->prev))
		{
			XBTRecycleQueueItem prev_item = HeaderGetItem(header, item->prev);
			prev_item->next = xlrec->offset;
		}
		if (IsNormalOffset(item->next))
		{
			XBTRecycleQueueItem next_item = HeaderGetItem(header, item->next);
			next_item->prev = xlrec->offset;
		}
	}
	else
	{
		/* restore adjacent items */
		if (IsNormalOffset(item->prev))
		{
			XBTRecycleQueueItem prev_item = HeaderGetItem(header, item->prev);
			prev_item->next = item->next;
		}
		if (IsNormalOffset(item->next))
		{
			XBTRecycleQueueItem next_item = HeaderGetItem(header, item->next);
			next_item->prev = item->prev;
		}
		item->blkno = InvalidBlockNumber;
		item->xid = InvalidFullTransactionId;
		item->prev = invalid_offset;
		item->next = header->free_list_head;
		header->free_list_head = xlrec->offset;
	}
}

static void
remove_one_item_from_page(Relation rel, Buffer buf, uint16 offset)
{
	Page				  page = BufferGetPage(buf);
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));
	XBTRecycleQueueItem	  item = HeaderGetItem(header, offset);
	XBTRecycleQueueItemData old_item = *item;

	/* Do the update.  No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	if (header->head == offset)
		header->head = item->next;
	if (header->tail == offset)
		header->tail = item->prev;
	if (IsNormalOffset(item->prev))
	{
		XBTRecycleQueueItem prev_item = HeaderGetItem(header, item->prev);
		prev_item->next = item->next;
	}
	if (IsNormalOffset(item->next))
	{
		XBTRecycleQueueItem next_item = HeaderGetItem(header, item->next);
		next_item->prev = item->prev;
	}

	item->blkno = InvalidBlockNumber;
	item->xid = InvalidFullTransactionId;
	item->prev = invalid_offset;
	item->next = header->free_list_head;
	header->free_list_head = offset;

	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
		/* we record the old free_list_head, new free_list_head is known as xlrec.offset */
		log_modify_page(buf, false, offset, &old_item, header, item->next);

	END_CRIT_SECTION();

	if (!(IsNormalOffset(header->head)))
	{
		/* deleting the only item on this page */
		if ((header->flags & URQ_HEAD_PAGE) != 0 && (header->flags & URQ_TAIL_PAGE) == 0)
		{
			/* it's head page, and it's not tail page, we need to change the head page flag */
			Buffer next_buf = read_recycle_queue_buffer(rel, header->next_blkno);
			LockBuffer(next_buf, BT_WRITE);
			/* change endpoint, and insert xlog */
			recycle_queue_change_endpoint(rel, buf, next_buf, true);
			UnlockReleaseBuffer(next_buf);
		}
	}
}

static void
xbtree_recycle_queue_discard_page(Relation rel, XBTRecycleQueueAddress addr)
{
	Buffer				  buf = addr.queue_fuf;
	Page				  page;
	XBTRecycleQueueHeader header;
	XBTRecycleQueueItem	  item;
	LockBuffer(buf, BT_WRITE);

	page = BufferGetPage(buf);
	header = GetRecycleQueueHeader(page, BufferGetBlockNumber(buf));
	item = HeaderGetItem(header, addr.offset);
	if (item->blkno != addr.index_blkno && BlockNumberIsValid(item->blkno))
	{
		/* already discarded, skip */
		UnlockReleaseBuffer(buf);
		return;
	}
	if (!BlockNumberIsValid(item->blkno) && !FullTransactionIdIsValid(item->xid))
	{
		/* already discarded, skip */
		UnlockReleaseBuffer(buf);
		return;
	}

	/* try remove invalid item in left side */
	for (;;)
	{
		XBTRecycleQueueItem cur_item = HeaderGetItem(header, addr.offset);
		bool				removed = false;
		if (IsNormalOffset(cur_item->prev))
		{
			XBTRecycleQueueItem prev_item = HeaderGetItem(header, cur_item->prev);
			if (!BlockNumberIsValid(prev_item->blkno))
			{
				remove_one_item_from_page(rel, buf, cur_item->prev);
				removed = true;
			}
		}
		if (!removed)
			break;
	}
	/* try remove invalid item in right side */
	for (;;)
	{
		XBTRecycleQueueItem cur_item = HeaderGetItem(header, addr.offset);
		bool				removed = false;
		if (IsNormalOffset(cur_item->next))
		{
			XBTRecycleQueueItem next_item = HeaderGetItem(header, cur_item->next);
			if (!BlockNumberIsValid(next_item->blkno))
			{
				remove_one_item_from_page(rel, buf, cur_item->next);
				removed = true;
			}
		}
		if (!removed)
			break;
	}
	/* remove self */
	remove_one_item_from_page(rel, buf, addr.offset);

	UnlockReleaseBuffer(buf);
}

/*
 *  xbt_page_prune_opt() -- Optional prune a index page
 *
 * This function trying to prune the page if possible, and always do BtPageRepairFragmentation()
 * if we deleted any dead tuple.
 *
 * The passed tryDelete indicate whether we just try to delete the whole page. When this value is
 * true, we will do nothing if there is an alive tuple in the page, and delete the whole page if all
 * tuples are dead.
 *
 * IMPORTANT: Make sure we've got the write lock on the buf before call this function.
 *
 *      Note: The page must be a leaf page.
 */
bool
xbt_page_prune_opt(Relation rel, Buffer buf, bool try_delete)
{
	FullTransactionId		oldest_xmin;
	Page					page = BufferGetPage(buf);
	XBTPageOpaqueInternal	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	/*
	 * We can't write WAL in recovery mode, so there's no point trying to
	 * clean the page. The master will likely issue a cleaning WAL record soon
	 * anyway, so this is no particular loss.
	 */
	if (RecoveryInProgress())
	{
		if (try_delete)
			_bt_relbuf(rel, buf);
		return false;
	}

	if (!P_ISLEAF(opaque))
	{
		/* we can't prune internal pages */
		if (try_delete)
			_bt_relbuf(rel, buf);
		return false;
	}

	if (try_delete && P_RIGHTMOST(opaque))
	{
		/* can't delete the right most page */
		_bt_relbuf(rel, buf);
		return false;
	}

	oldest_xmin.value = pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid);

	Assert(FullTransactionIdIsValid(oldest_xmin));

	/*
	 * Let's see if we really need pruning.
	 *
	 * Forget it if page is not hinted to contain something prunable that's
	 * older than oldest_xmin.
	 */
	if (FullTransactionIdIsValid(opaque->pd_prune_xid) &&
		!IndexPageIsPrunable(opaque->pd_prune_xid, oldest_xmin))
	{
		if (try_delete)
			_bt_relbuf(rel, buf);
		return false;
	}

	/* Ok to prune */
	if (try_delete)
	{
		/* if we are trying to delete a page, prune it first (we need WAL), then check empty and delete */
		xbt_page_prune(rel, buf, oldest_xmin);

		Assert(!P_RIGHTMOST(opaque));
		if (PageGetMaxOffsetNumber(page) == 1)
			/* already empty (only HIKEY left), ok to delete */
			return _xbt_pagedel(rel, buf) > 0;
		else
		{
			_bt_relbuf(rel, buf);
			return false;
		}
	}

	return xbt_page_prune(rel, buf, oldest_xmin);
}

/*
 *  xbt_page_prune() -- Apply page pruning to the page.
 *
 * If passed tryDelete parameter is false, delete all dead tuples in the page, then repair the fragmentation.
 * If passed tryDelete is true, only delete the whole page when all tuples are dead.
 *
 *      Note: we won't delete the high key (if any).
 */
bool
xbt_page_prune(Relation rel, Buffer buf, FullTransactionId oldest_xmin)
{
	int					   nprevious_dead = 0;
	int16				   active_tuple_count = 0;
	bool				   has_pruned = false;
	Page				   page = BufferGetPage(buf);
	XBTPageOpaqueInternal  opaque;
	OffsetNumber		   offnum, maxoff;
	IndexPruneState		   prstate;

#ifdef XLOG_BTREE_PRUNE_DEBUG
	memcpy(xlrec.pagebak, page, BLCKSZ);
#endif

	prstate.new_prune_xid = InvalidFullTransactionId;
	prstate.latestRemovedXid = InvalidFullTransactionId;
	prstate.ndead = 0;
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	/* Scan the page */
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = P_FIRSTDATAKEY(opaque); offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId itemid;

		itemid = PageGetItemId(page, offnum);
		/* Nothing to do if slot is already dead, just count nprevious_dead */
		if (ItemIdIsDead(itemid))
		{
			prstate.previousdead[nprevious_dead] = offnum;
			nprevious_dead++;
			continue;
		}
		/* check visibility and record dead tuples */
		if (xbt_prune_item(page, offnum, oldest_xmin, &prstate))
			active_tuple_count++;
	}

	/* Any error while applying the changes is critical */
	START_CRIT_SECTION();

	/* Have we found any prunable items? */
	if (nprevious_dead > 0 || prstate.ndead > 0)
	{
		/* Set up flags and try to repair page  fragmentation */
		// WaitState oldStatus = pgstat_report_waitstatus(STATE_PRUNE_INDEX);
		xbt_page_prune_execute(page, prstate.previousdead, nprevious_dead, &prstate,
							   oldest_xmin);
		xbt_page_prune_execute(page, prstate.nowdead, prstate.ndead, &prstate,
							   oldest_xmin);
		xbt_page_repair_fragmentation(rel, BufferGetBlockNumber(buf), page);

		has_pruned = true;

		/*
		 * Update the page's pd_prune_xid field to either zero, or the lowest
		 * XID of any soon-prunable tuple.
		 */
		opaque->pd_prune_xid = prstate.new_prune_xid;
		opaque->active_count = active_tuple_count;

		MarkBufferDirty(buf);

		/* xlog stuff */
		if (RelationNeedsWAL(rel))
		{
			XLogRecPtr			 recptr;
			xl_xbtree_prune_page xlrec;
			xlrec.count = nprevious_dead + prstate.ndead;
			xlrec.new_prune_xid = prstate.new_prune_xid;
			xlrec.latestRemovedXid = prstate.latestRemovedXid;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, SizeOfXBTreePrunePage);
			XLogRegisterData((char *) &prstate.previousdead,
							 nprevious_dead * sizeof(OffsetNumber));
			XLogRegisterData((char *) &prstate.nowdead,
							 prstate.ndead * sizeof(OffsetNumber));

			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);

			recptr = XLogInsert(RM_XBTREE_ID, XLOG_XBTREE_PRUNE_PAGE);

			PageSetLSN(page, recptr);
		}
	}
	else
	{
		/*
		 * If we didn't prune anything, but have found a new value for the
		 * pd_prune_xid field, update it and mark the buffer dirty. This is
		 * treated as a non-WAL-logged hint.
		 *
		 * Also clear the "page is full" flag if it is set, since there's no
		 * point in repeating the prune/defrag process until something else
		 * happens to the page.
		 */
		if ((opaque->pd_prune_xid.value != prstate.new_prune_xid.value) ||
			(opaque->active_count != active_tuple_count))
		{
			opaque->pd_prune_xid = prstate.new_prune_xid;
			opaque->active_count = active_tuple_count;
			MarkBufferDirtyHint(buf, true);
		}
	}

	END_CRIT_SECTION();

	return has_pruned;
}

/*
 *  xbt_prune_item() -- Check a item's status, and update prune state if found any dead tuple or new_prune_xid.
 *
 * The return value indicates whether the item is active.
 */
bool
xbt_prune_item(Page page, OffsetNumber offnum, FullTransactionId oldest_xmin,
				IndexPruneState *prstate)
{
	ItemId				item = PageGetItemId(page, offnum);
	IndexTuple	   		itup = (IndexTuple) PageGetItem(page, item);
	XBTreeIndexTuple	xbt_tuple = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);
	/*
	 * INDEXTUPLE_DEAD: xmin invalid || xmax frozen
	 * INDEXTUPLE_DELETED: xmax valid && xmax committed
	 * INDEXTUPLE_ACTIVE: else
	 */
	if(IndexItemIdIsDeleted(item))
	{
		if(FullTransactionIdPrecedes(xbt_tuple->modified_xid, oldest_xmin))
		{
			prstate->nowdead[prstate->ndead] = offnum;
			prstate->ndead++;
			return false;
		}
		else 
		{
			FullTransactionId xmax = xbt_tuple->modified_xid;
			
			if (FullTransactionIdIsValid(xmax) && xstore_transaction_id_did_commit(xmax))
			{
				/* update xmax as new_prune_xid */
				if (!FullTransactionIdIsValid(prstate->new_prune_xid) ||
					FullTransactionIdPrecedes(xmax, prstate->new_prune_xid))
					prstate->new_prune_xid = xmax;
			}

			/* when xmax is invalid, we treat the tuple as active */
			return !FullTransactionIdIsValid(xmax);
		}
	}
	else 
		return true;
}

/*
 *  xbt_page_prune_execute() -- Traverse the nowdead array, set the corresponding tuples as dead.
 */
void
xbt_page_prune_execute(Page page, OffsetNumber *nowdead, int ndead,
					   IndexPruneState *prstate, FullTransactionId oldest_xmin)
{
	OffsetNumber *offnum = NULL;
	int			  i;

	/* Update all now-dead line pointers */
	offnum = nowdead;
	for (i = 0; i < ndead; i++)
	{
		OffsetNumber off = *offnum++;
		ItemId		 lp = PageGetItemId(page, off);
		/* NOTE: prstate is NULL iff in redo */
		if (prstate != NULL && ItemIdHasStorage(lp))
		{
			IndexTuple	   itup = (IndexTuple) PageGetItem(page, lp);
			XBTreeIndexTuple uxid = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);

			FullTransactionId xmax = uxid->modified_xid;
			if (FullTransactionIdIsNormal(xmax))
			{
				if ((!FullTransactionIdIsValid(prstate->latestRemovedXid) ||
					 FullTransactionIdPrecedes(prstate->latestRemovedXid, xmax)) &&
					FullTransactionIdPrecedes(xmax, oldest_xmin))
					/* update latestRemovedXid */
					prstate->latestRemovedXid = xmax;
			}
		}
		ItemIdSetDead(lp);
	}
}

static int
item_off_compare(const void *itemIdp1, const void *itemIdp2)
{
	/* Sort in decreasing itemoff order */
	return ((itemIdSort) itemIdp2)->itemoff - ((itemIdSort) itemIdp1)->itemoff;
}

/*
 *  xbt_page_repair_fragmentation() -- Repair the fragmentation in the page.
 *
 * On the basis of PageRepairFragmentation(Page page), we ignore redirected/unused, compact both
 * line pointers and index tuples.
 */
void
xbt_page_repair_fragmentation(Relation rel, BlockNumber blkno, Page page)
{
	Offset				  pdLower = ((PageHeader) page)->pd_lower;
	Offset				  pdUpper = ((PageHeader) page)->pd_upper;
	Offset				  pdSpecial = ((PageHeader) page)->pd_special;
	XBTPageOpaqueInternal opaque;
	int					  nstorage = 0;
	int					  nline;
	Size				  totallen = 0;
	Offset				  upper;
	int					  i = 0;

	itemIdSortData itemidbase[MaxIndexTuplesPerPage];
	itemIdSort	   itemidptr = itemidbase;

	const char *relName = (rel ? RelationGetRelationName(rel) : "Unknown During Redo");
	if ((unsigned int) (pdLower) < SizeOfPageHeaderData || pdLower > pdUpper ||
		pdUpper > pdSpecial || pdSpecial > BLCKSZ ||
		(unsigned int) (pdSpecial) != MAXALIGN(pdSpecial))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("corrupted page pointers: lower = %d, upper = %d, special "
							   "= %d. rel \"%s\", blkno %u",
							   pdLower, pdUpper, pdSpecial, relName, blkno)));

	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	nline = PageGetMaxOffsetNumber(page);

	for (i = FirstOffsetNumber; i <= nline; i++)
	{
		ItemId lp = PageGetItemId(page, i);
		/* Include active and frozen tuples */
		if (ItemIdHasStorage(lp))
		{
			nstorage++;
			itemidptr->offsetindex = nstorage;
			itemidptr->itemoff = ItemIdGetOffset(lp);
			itemidptr->olditemid = *lp;
			if (itemidptr->itemoff < (int) pdUpper ||
				itemidptr->itemoff >= (int) pdSpecial)
				ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
								errmsg("corrupted item pointer: %d. rel \"%s\", blkno %u",
									   itemidptr->itemoff, relName, blkno)));
			itemidptr->alignedlen = MAXALIGN(ItemIdGetLength(lp));
			totallen += itemidptr->alignedlen;
			itemidptr++;
		}
	}

	if (totallen > (Size) (pdSpecial - pdLower))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted item lengths: total %u, available space %d. rel "
						"\"%s\", blkno %u",
						(unsigned int) totallen, pdSpecial - pdLower, relName, blkno)));

	/* sort itemIdSortData array into decreasing itemoff order */
	qsort((char *) itemidbase, nstorage, sizeof(itemIdSortData), item_off_compare);

	/* compactify page and install new itemids */
	upper = pdSpecial;

	itemidptr = itemidbase;
	for (i = 0; i < nstorage; i++, itemidptr++)
	{
		ItemId lp = PageGetItemId(page, itemidptr->offsetindex);
		upper -= itemidptr->alignedlen;
		memmove((char *) page + upper, (char *) page + itemidptr->itemoff, itemidptr->alignedlen);
		*lp = itemidptr->olditemid;
		lp->lp_off = upper;
	}

	/* Mark the page as not containing any LP_DEAD items, which means vacuum is not needed */
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	((PageHeader) page)->pd_lower = SizeOfPageHeaderData + nstorage * sizeof(ItemIdData);
	((PageHeader) page)->pd_upper = upper;
}

bool
xbtree_page_recyclable(Page page)
{
	XBTPageOpaqueInternal opaque;
	/*
	 * It's possible to find an all-zeroes page in an index --- for example, a
	 * backend might successfully extend the relation one page and then crash
	 * before it is able to make a WAL entry for adding the page. If we find a
	 * zeroed page then reclaim it.
	 */
	FullTransactionId frozenXmin = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
	if (PageIsNew(page))
		return true;

	/*
	 * Otherwise, recycle if deleted and too old to have any processes
	 * interested in it.
	 */
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	return P_ISDELETED(opaque) &&
		   FullTransactionIdPrecedes(((XBTPageOpaque) opaque)->xact, frozenXmin);
}


static void
xbtree2_xlog_recycle_queue_init_page_operator_curr_page(redo_buffer_info *buffer, void *recorddata)
{
	xl_xbtree2_recycle_queue_init_page *xlrec =
		(xl_xbtree2_recycle_queue_init_page *) recorddata;
	Page page = BufferGetPage(buffer->buf);
	xbtree_recycle_queue_init_page(NULL, page, xlrec->curr_blkno,
							   xlrec->prev_blkno,
							   xlrec->next_blkno);
	PageSetLSN(page, buffer->lsn);
}

static void
xbtree2_xlog_recycle_queue_init_page_operator_adjacent_page(redo_buffer_info *buffer,
														 void *record_data, bool is_left)
{
	xl_xbtree2_recycle_queue_init_page *xlrec = (xl_xbtree2_recycle_queue_init_page *) record_data;
	XBTRecycleQueueHeader header = GetRecycleQueueHeader(buffer->page_info.page, buffer->block_info.blkno);

	if (is_left)
		header->next_blkno = xlrec->curr_blkno;
	else
		header->prev_blkno = xlrec->curr_blkno;
	PageSetLSN(buffer->page_info.page, buffer->lsn);
}

static void
xbtree2_xlog_recycle_queue_endpoint_operator_left_page(redo_buffer_info *buffer,
													 void			*record_data)
{
	xl_xbtree2_recycle_queue_endpoint *xlrec = (xl_xbtree2_recycle_queue_endpoint *) record_data;
	xbtree_recycle_queue_page_change_endpoint_left_page(buffer->buf, xlrec->is_head);
	PageSetLSN(BufferGetPage(buffer->buf), buffer->lsn);
}

static void
xbtree2_xlog_recycle_queue_endpoint_operator_right_page(redo_buffer_info *buffer,
													  void			 *record_data)
{
	xl_xbtree2_recycle_queue_endpoint *xlrec = (xl_xbtree2_recycle_queue_endpoint *) record_data;
	xbtree_recycle_queue_page_change_endpoint_right_page(buffer->buf, xlrec->is_head);
	PageSetLSN(BufferGetPage(buffer->buf), buffer->lsn);
}

static void
xbtree2_xlog_recycle_queue_modify_operator_page(redo_buffer_info *buffer, void *record_data)
{
	xbtree_xlog_recycle_queue_modify_page(buffer->buf,
									 (xl_xbtree2_recycle_queue_modify *) record_data);
	PageSetLSN(BufferGetPage(buffer->buf), buffer->lsn);
}

static void
xbtree2_xlog_recycle_queue_init_page(XLogReaderState *record)
{
	xl_xbtree2_recycle_queue_init_page *xlrec =
		(xl_xbtree2_recycle_queue_init_page *) XLogRecGetData(record);

	redo_buffer_info buf;
	buf.buf =
		XLogInitBufferForRedo(record, XBTREE2_RECYCLE_QUEUE_INIT_PAGE_CURR_BLOCK_NUM);
	build_redo_buffer_info(&buf, record);
	xbtree2_xlog_recycle_queue_init_page_operator_curr_page(&buf, (void *) xlrec);
	MarkBufferDirty(buf.buf);

	if (xlrec->inserting_new_page)
	{
		redo_buffer_info lbuf;
		redo_buffer_info rbuf;
		if (xlog_read_buffer_for_redo_ext(record,
										  XBTREE2_RECYCLE_QUEUE_INIT_PAGE_LEFT_BLOCK_NUM,
										  &lbuf) == BLK_NEEDS_REDO)
		{
			xbtree2_xlog_recycle_queue_init_page_operator_adjacent_page(&lbuf, (void *) xlrec,
																true);
			MarkBufferDirty(lbuf.buf);
		}
		if (xlog_read_buffer_for_redo_ext(record,
										  XBTREE2_RECYCLE_QUEUE_INIT_PAGE_RIGHT_BLOCK_NUM,
										  &rbuf) == BLK_NEEDS_REDO)
		{
			xbtree2_xlog_recycle_queue_init_page_operator_adjacent_page(&rbuf, (void *) xlrec,
																false);
			MarkBufferDirty(rbuf.buf);
		}
		if (BufferIsValid(lbuf.buf))
			UnlockReleaseBuffer(lbuf.buf);
		if (BufferIsValid(rbuf.buf))
			UnlockReleaseBuffer(rbuf.buf);
	}
	UnlockReleaseBuffer(buf.buf);
}

static void
xbtree2_xlog_recycle_queue_endpoint(XLogReaderState *record)
{
	redo_buffer_info lbuf;
	redo_buffer_info rbuf;

	if (xlog_read_buffer_for_redo_ext(record,
									  XBTREE2_RECYCLE_QUEUE_ENDPOINT_CURR_BLOCK_NUM,
									  &lbuf) == BLK_NEEDS_REDO)
	{
		xbtree2_xlog_recycle_queue_endpoint_operator_left_page(&lbuf,
														(void *) XLogRecGetData(record));
		MarkBufferDirty(lbuf.buf);
	}

	if (xlog_read_buffer_for_redo_ext(record,
									  XBTREE2_RECYCLE_QUEUE_ENDPOINT_NEXT_BLOCK_NUM,
									  &rbuf) == BLK_NEEDS_REDO)
	{
		xbtree2_xlog_recycle_queue_endpoint_operator_right_page(&rbuf,
														 (void *) XLogRecGetData(record));
		MarkBufferDirty(rbuf.buf);
	}
	if (BufferIsValid(lbuf.buf))
		UnlockReleaseBuffer(lbuf.buf);
	if (BufferIsValid(rbuf.buf))
		UnlockReleaseBuffer(rbuf.buf);
}

static void
xbtree2_xlog_recycle_queue_modify(XLogReaderState *record)
{
	redo_buffer_info buf;
	if (xlog_read_buffer_for_redo_ext(record, XBTREE2_RECYCLE_QUEUE_MODIFY_BLOCK_NUM,
									  &buf) == BLK_NEEDS_REDO)
	{
		xbtree2_xlog_recycle_queue_modify_operator_page(&buf, (void *) XLogRecGetData(record));
		MarkBufferDirty(buf.buf);
	}
	if (BufferIsValid(buf.buf))
		UnlockReleaseBuffer(buf.buf);
}

void
xbtree2_redo(XLogReaderState *record)
{
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_XBTREE2_RECYCLE_QUEUE_INIT_PAGE:
			xbtree2_xlog_recycle_queue_init_page(record);
			break;
		case XLOG_XBTREE2_RECYCLE_QUEUE_ENDPOINT:
			xbtree2_xlog_recycle_queue_endpoint(record);
			break;
		case XLOG_XBTREE2_RECYCLE_QUEUE_MODIFY:
			xbtree2_xlog_recycle_queue_modify(record);
			break;
		
		default:
			ereport(PANIC, (errmsg("XBTree2Redo: unknown op code %hhu", info)));
	}
}