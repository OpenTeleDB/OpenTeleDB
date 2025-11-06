/* -------------------------------------------------------------------------
 *
 * xbtxlog.c
 *	  WAL replay logic for xbtrees.
 *
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/xbtree/xbtxlog.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/nbtree.h"
#include "utils/elog.h"
#include "xbtree/xbtree.h"
#include "access/transam.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "access/itup.h"
#include "storage/bufmgr.h"
#include "storage/relfilelocator.h"
#include "storage/buf_internals.h"
#include "storage/standby.h"
#include "xbtree/xbtree.h"
#include "xbtree/xbtxlog.h"
#include "xbtree/xbttup.h"
#include "undo/undorecord.h"
#include "undo/undofetch.h"
#include "undo/undolog.h"
#include "xstore.h"

/*
 * We must keep track of expected insertions due to page splits, and apply
 * them manually if they are not seen in the WAL log during replay.  This
 * makes it safe for page insertion to be a multiple-WAL-action process.
 *
 * Similarly, deletion of an only child page and deletion of its parent page
 * form multiple WAL log entries, and we have to be prepared to follow through
 * with the deletion if the log ends between.
 *
 * The data structure is a simple linked list --- this should be good enough,
 * since we don't expect a page split or multi deletion to remain incomplete
 * for long.  In any case we need to respect the order of operations.
 */

static void _xbt_restore_page(Page page, char *from, int len);

static void _xbt_restore_meta(XLogReaderState *record, uint8 block_id);

static void _xbt_clear_incomplete_split(XLogReaderState *record, uint8 block_id);

static void _xbt_xlog_split_redo_right_page(Buffer rbuf, XLogRecPtr lsn, void *recorddata,
									  BlockNumber leftsib, BlockNumber rnext,
									  void *blkdata, Size datalen, bool hasOpaque);
static void _xbt_xlog_split_redo_next_page(Buffer buf, XLogRecPtr lsn, BlockNumber rightsib);
static void _xbt_xlog_split_redo_left_page(Buffer lbuf, XLogRecPtr lsn, void *recorddata,
									 BlockNumber rightsib, bool onleft, void *blkdata,
									 Size datalen, bool hasOpaque);


static void _xbt_xlog_new_root_redo(Buffer buffer, XLogRecPtr lsn, void *record, void *blkdata,
								   Size len, BlockNumber *downlink);

static void _xbt_xlog_half_dead_redo_parent_page(Buffer pbuf, XLogRecPtr lsn, void *recorddata);
static void _xbt_xlog_half_dead_redo_leaf_page(Buffer lbuf, XLogRecPtr lsn, void *recorddata);

static void _xbt_xlog_unlink_redo_right_page(Buffer rbuf, XLogRecPtr lsn, void *recorddata);
static void _xbt_xlog_unlink_redo_left_page(Buffer lbuf, XLogRecPtr lsn, void *recorddata);
static void _xbt_xlog_unlink_redo_page(Buffer buf, XLogRecPtr lsn, void *recorddata);
static void _xbt_xlog_unlink_redo_children(Buffer cbuf, XLogRecPtr lsn, void *recorddata);

UndoRecPtr
_xbt_redo_undo_insert(XLogReaderState *record, const BlockNumber blkno);

UndoRecPtr
_xbt_redo_undo_split(XLogReaderState *record, Buffer buf, bool insertOnLeft);

UndoRecPtr
_xbt_redo_undo_delete(XLogReaderState *record,const BlockNumber blkno);

static void
_xbt_restore_meta(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	char		  *ptr = NULL;
	Size		   len;
	Buffer		metabuf;
	Page		metapg;
	XBTPageOpaqueInternal pageop;
	xl_btree_metadata *xlrec = NULL;
	BTMetaPageData *md;

	metabuf = XLogInitBufferForRedo(record, block_id);
	ptr = XLogRecGetBlockData(record, block_id, &len);
	
	Assert(len == sizeof(xl_btree_metadata));
	Assert(BufferGetBlockNumber(metabuf) == BTREE_METAPAGE);
	xlrec = (xl_btree_metadata *) ptr;
	metapg = BufferGetPage(metabuf);

	_xbt_pageinit(metapg, BufferGetPageSize(metabuf));

	md = BTPageGetMeta(metapg);
	md->btm_magic = BTREE_MAGIC;
	md->btm_version = BTREE_VERSION;
	md->btm_root = xlrec->root;
	md->btm_level = xlrec->level;
	md->btm_fastroot = xlrec->fastroot;
	md->btm_fastlevel = xlrec->fastlevel;

	pageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(metapg);
	pageop->btpo_flags = BTP_META;

	/*
     * Set pd_lower just past the end of the metadata.	This is not essential
     * but it makes the page look compressible to xlog.c.
     */
	((PageHeader) metapg)->pd_lower =
		((char *) md + sizeof(BTMetaPageData)) - (char *) metapg;

	PageSetLSN(metapg, lsn);
	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);
}

static void
_xbt_clear_incomplete_split(XLogReaderState *record, uint8 block_id)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buf;

	if (XLogReadBufferForRedo(record, block_id, &buf) == BLK_NEEDS_REDO)
	{
		Page page = (Page) BufferGetPage(buf);
		XBTPageOpaqueInternal pageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

		Assert(P_INCOMPLETE_SPLIT(pageop));
		pageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

UndoRecPtr
_xbt_redo_undo_insert(XLogReaderState *record, const BlockNumber blkno)
{
	XLogRecPtr	  lsn = record->EndRecPtr;
	FullTransactionId xid = XLogRecGetFullXid(record);
	FullTransactionId subFullXid = InvalidFullTransactionId;
	xl_undo_meta  undometa;
	FullTransactionId *fullXid;

	IndexTuple indTuple;
	Size      *indTupleSize;

	UndoRecPtr	 *blkprev;
	UndoRecPtr	 *prevurp;
	UndoRecPtr	  invalidUrp = INVALID_UNDO_REC_PTR;
	xl_undo_meta *xlundometa = NULL;
	UndoRecPtr	  urecptr = INVALID_UNDO_REC_PTR;

	bool		skipInsert = false;
	RelFileLocator targetNode;
	UnpackedUndoRecord *undorec = NULL;

	xl_btree_insert *xlrec = (xl_btree_insert *) XLogRecGetData(record);
	xl_undo_header  *xlundohdr = (xl_undo_header *) ((char *) xlrec + SizeOfBtreeInsert); 
	char *currLogPtr = ((char *) xlundohdr + SizeOfXLUndoHeader);
	init_xlog_undo_meta(&undometa);

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
	{
		blkprev = (UndoRecPtr *) ((char *) currLogPtr);
		currLogPtr += sizeof(UndoRecPtr);
	}
	else
		blkprev = &invalidUrp;

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
	{
		prevurp = (UndoRecPtr *) ((char *) currLogPtr);
		currLogPtr += sizeof(UndoRecPtr);
	}
	else
		prevurp = &invalidUrp;

	// swap subxid and topxid
	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
	{
		fullXid = (FullTransactionId *) currLogPtr;
		currLogPtr += sizeof(FullTransactionId);
		subFullXid = xid;
		xid = *fullXid;
	}

	indTupleSize = (Size *) currLogPtr;
	currLogPtr += sizeof(Size);

	indTuple = (IndexTuple) currLogPtr;
	currLogPtr += *indTupleSize;

	xlundometa = (xl_undo_meta *) ((char *) currLogPtr);
	urecptr = xlundohdr->urecptr;

	/* copy xlundometa to local struct */
	copy_xlog_undo_meta(xlundometa, &undometa);

	skipInsert = is_skip_insert_undo(urecptr, &undometa);
	if (skipInsert)
	{
		xlog_undo_meta_setinfo(&undometa, XLOG_UNDOMETA_INFO_SKIP);
	}

	/* We need to pass in tablespace and relfilenode in prepare_undo but we never explicitly
	 * wrote those information in the xlundohdr because we can grab them from the XLOG record itself.
	 */
	XLogRecGetBlockTag(record, 0, &targetNode, NULL, NULL);

	undorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
	undorec->uur_urp = urecptr;
	urecptr = _xbt_prepare_undo_insert(
		xlundohdr->relOid, targetNode.relNumber, targetNode.spcOid, UNDO_PERMANENT,
		xid, 0,indTuple, *blkprev, *prevurp, blkno, 
		record, xlundohdr, &undometa, subFullXid);

	ereport(DEBUG2,
			(errmsg("redo undorecord for index insert :undoptr=%lu, xid %lu,  spcoid=%u.",
					urecptr, xid.value, targetNode.spcOid)));
	/* recover undo record */
	Assert(urecptr == xlundohdr->urecptr);
	SetUndoRecordOffset(undorec, xlrec->offnum);
	if (!skipInsert)
		/* Insert the Undo record into the undo store */
		insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, lsn);

	redo_undo_meta(record, &undometa, xlundohdr->urecptr,
					prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
					prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));

	reset_prepared_buffers_in_ctx();

	return urecptr;
}

static void
xbtree_xlog_insert(bool isleaf, bool ismeta, XLogReaderState *record)
{
	XLogRecPtr	     lsn = record->EndRecPtr;
	xl_btree_insert *xlrec = (xl_btree_insert *) XLogRecGetData(record);
	RelFileLocator	 targetNode;
	Buffer		     buffer;
	BlockNumber		 blkno = InvalidBlockNumber;
	char		    *datapos = NULL;
	Page			 page;
	XLogRedoAction   action;

	/*
     * Insertion to an internal page finishes an incomplete split at the child
     * level.  Clear the incomplete-split flag in the child.  Note: during
     * normal operation, the child and parent pages are locked at the same
     * time, so that clearing the flag and inserting the downlink appear
     * atomic to other backends.  We don't bother with that during replay,
     * because readers don't care about the incomplete-split flag and there
     * cannot be updates happening.
     */
	if (!isleaf)
		_xbt_clear_incomplete_split(record, BTREE_INSERT_CHILD_BLOCK_NUM);

	XLogRecGetBlockTag(record, 0, &targetNode, NULL, &blkno);

	action = XLogReadBufferForRedo(record, BTREE_INSERT_ORIG_BLOCK_NUM, &buffer);
	if (action == BLK_NEEDS_REDO)
	{
		XBTPageOpaqueInternal opaque;
		Size                  datalen;
		datapos = XLogRecGetBlockData(record, BTREE_INSERT_ORIG_BLOCK_NUM, &datalen);

		page = BufferGetPage(buffer);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		opaque->active_count++;

		if (PageAddItem(page, (Item) datapos, datalen, xlrec->offnum, false, false) ==
			InvalidOffsetNumber)
			ereport(PANIC, (errmsg("btree_insert_redo: failed to add item")));

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	if(isleaf)
	{
		// prepare insert undo ,insert undo, update zone meta and update transaction slot;
		_xbt_redo_undo_insert(record, blkno);
	}

	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	if (ismeta)
		_xbt_restore_meta(record, BTREE_INSERT_META_BLOCK_NUM);
}

UndoRecPtr
_xbt_redo_undo_split(XLogReaderState *record, Buffer buf, bool insert_on_left)
{
	IndexTuple indTuple;
	Size      *indTupleSize;
	XLogRecPtr	  lsn = record->EndRecPtr;
	FullTransactionId xid = XLogRecGetFullXid(record);
	FullTransactionId subFullXid = InvalidFullTransactionId;
	xl_undo_meta  undometa;
	FullTransactionId *fullXid;

	UndoRecPtr	 *blkprev;
	UndoRecPtr	 *prevurp;
	UndoRecPtr	  invalidUrp = INVALID_UNDO_REC_PTR;
	xl_undo_meta *xlundometa = NULL;
	UndoRecPtr	  urecptr = INVALID_UNDO_REC_PTR;
	Offset      offset = InvalidOffsetNumber;

	bool		skipInsert = false;
	RelFileLocator targetNode;
	UnpackedUndoRecord *undorec = NULL;

	BlockNumber blkno;

	xl_xbtree_split *xlrec = (xl_xbtree_split *) XLogRecGetData(record);
	xl_undo_header  *xlundohdr = (xl_undo_header *) ((char *) xlrec +  SizeOfXBTreeSplit); 
	char *currLogPtr = ((char *) xlundohdr + SizeOfXLUndoHeader);
	init_xlog_undo_meta(&undometa);

	blkno = BufferGetBlockNumber(buf);

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
	{
		blkprev = (UndoRecPtr *) ((char *) currLogPtr);
		currLogPtr += sizeof(UndoRecPtr);
	}
	else
		blkprev = &invalidUrp;

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
	{
		prevurp = (UndoRecPtr *) ((char *) currLogPtr);
		currLogPtr += sizeof(UndoRecPtr);
	}
	else
		prevurp = &invalidUrp;

	// swap subxid and topxid
	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
	{
		fullXid = (FullTransactionId *) currLogPtr;
		currLogPtr += sizeof(FullTransactionId);
		subFullXid = xid;
		xid = *fullXid;
	}

	indTupleSize = (Size *) currLogPtr;
	currLogPtr += sizeof(Size);

	indTuple = (IndexTuple) currLogPtr;
	currLogPtr += *indTupleSize;

	xlundometa = (xl_undo_meta *) ((char *) currLogPtr);
	urecptr = xlundohdr->urecptr;

	/* copy xlundometa to local struct */
	copy_xlog_undo_meta(xlundometa, &undometa);

	skipInsert = is_skip_insert_undo(urecptr, &undometa);
	if (skipInsert)
	{
		xlog_undo_meta_setinfo(&undometa, XLOG_UNDOMETA_INFO_SKIP);
	}

	/* We need to pass in tablespace and relfilenode in prepare_undo but we never explicitly
		* wrote those information in the xlundohdr because we can grab them from the XLOG record itself.
		*/
	XLogRecGetBlockTag(record, 0, &targetNode, NULL, NULL);

	undorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
	undorec->uur_urp = urecptr;
	urecptr = _xbt_prepare_undo_insert(
		xlundohdr->relOid, targetNode.relNumber, targetNode.spcOid, UNDO_PERMANENT,
		xid, 0, indTuple, *blkprev, *prevurp, 
		blkno, record, xlundohdr, &undometa, subFullXid);

	ereport(DEBUG2,
			(errmsg("redo undorecord for index insert with split:undoptr=%lu, xid %lu,  spcoid=%u.",
					urecptr, xid.value, targetNode.spcOid)));
	/* recover undo record */
	Assert(urecptr == xlundohdr->urecptr);
	SetUndoRecordOffset(undorec, offset);

	if (!skipInsert)
		/* Insert the Undo record into the undo store */
		insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, lsn);

	redo_undo_meta(record, &undometa, xlundohdr->urecptr,
					prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
					prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));

	reset_prepared_buffers_in_ctx();

	return urecptr;
}

static void
xbtree_xlog_split(bool newitemonleft, XLogReaderState *record, bool hasOpaque)
{
	XLogRecPtr lsn = record->EndRecPtr;
	xl_xbtree_split *xlrec = (xl_xbtree_split *) XLogRecGetData(record);
	bool isleaf = (xlrec->level == 0);
	Size			 datalen;
	char			*datapos = NULL;
	RelFileLocator		 rnode;
	BlockNumber		 leftsib;
	BlockNumber		 rightsib;
	BlockNumber		 rnext = P_NONE;
	Buffer	 rbuf;
	Buffer	 lbuf;
	
	XLogRedoAction   action;

	XLogRecGetBlockTag(record, BTREE_SPLIT_LEFT_BLOCK_NUM, &rnode, NULL, &leftsib);
	XLogRecGetBlockTag(record, BTREE_SPLIT_RIGHT_BLOCK_NUM, NULL, NULL, &rightsib);
	if (!XLogRecGetBlockTagExtended(record, BTREE_SPLIT_RIGHTNEXT_BLOCK_NUM, NULL, NULL, &rnext,NULL))
		rnext = P_NONE;

	if (!isleaf)
		_xbt_clear_incomplete_split(record, BTREE_SPLIT_CHILD_BLOCK_NUM);

	/* Reconstruct right (new) sibling page from scratch */
	rbuf = XLogInitBufferForRedo(record, BTREE_SPLIT_RIGHT_BLOCK_NUM);
	datapos = XLogRecGetBlockData(record, BTREE_SPLIT_RIGHT_BLOCK_NUM, &datalen);
	_xbt_xlog_split_redo_right_page(rbuf, lsn, (void *) xlrec, leftsib, rnext,
									(void *) datapos, datalen, hasOpaque);

	MarkBufferDirty(rbuf);

	action =
		XLogReadBufferForRedoExtended(record, BTREE_SPLIT_LEFT_BLOCK_NUM, RBM_NORMAL, false, &lbuf);

	if (action == BLK_NEEDS_REDO)
	{
		datapos = XLogRecGetBlockData(record, BTREE_SPLIT_LEFT_BLOCK_NUM, &datalen);
		_xbt_xlog_split_redo_left_page(lbuf, lsn, (void *) xlrec, rightsib, newitemonleft,
										(void *) datapos, datalen, hasOpaque);
		MarkBufferDirty(lbuf);
	}

	if(isleaf)
	{
		if(newitemonleft)
			_xbt_redo_undo_split(record,lbuf,true);
		else
			_xbt_redo_undo_split(record,rbuf,false);
	}

	if (BufferIsValid(lbuf))
		UnlockReleaseBuffer(lbuf);

	UnlockReleaseBuffer(rbuf);

	if (rnext != P_NONE)
	{
		Buffer buf;

		action =
		XLogReadBufferForRedoExtended(record, BTREE_SPLIT_RIGHTNEXT_BLOCK_NUM, RBM_NORMAL, false, &buf);
      
		if (action == BLK_NEEDS_REDO)
		{
			_xbt_xlog_split_redo_next_page(buf,lsn, rightsib);
			MarkBufferDirty(buf);
		}
		if (BufferIsValid(buf))
			UnlockReleaseBuffer(buf);
	}
}

static void
xbtree_xlog_mark_page_halfdead(uint8 info, XLogReaderState *record)
{
	xl_btree_mark_page_halfdead *xlrec =
		(xl_btree_mark_page_halfdead *) XLogRecGetData(record);
	Buffer pbuffer;
	Buffer lbuffer;
	XLogRecPtr lsn;

	/*
     * In normal operation, we would lock all the pages this WAL record
     * touches before changing any of them.  In WAL replay, it should be okay
     * to lock just one page at a time, since no concurrent index updates can
     * be happening, and readers should not care whether they arrive at the
     * target page or not (since it's surely empty).
     */

    XLogRedoAction action =
		XLogReadBufferForRedoExtended(record, BTREE_HALF_DEAD_PARENT_PAGE_NUM, RBM_NORMAL, false, &pbuffer);
    lsn = record->EndRecPtr;

	if (action == BLK_NEEDS_REDO)
	{
		_xbt_xlog_half_dead_redo_parent_page(pbuffer, lsn, xlrec);
		MarkBufferDirty(pbuffer);
	}

	if (BufferIsValid(pbuffer))
		UnlockReleaseBuffer(pbuffer);

	lbuffer = XLogInitBufferForRedo(record, BTREE_HALF_DEAD_LEAF_PAGE_NUM);

	_xbt_xlog_half_dead_redo_leaf_page(lbuffer, lsn, xlrec);

	MarkBufferDirty(lbuffer);
	UnlockReleaseBuffer(lbuffer);
}

static void
xbtree_xlog_unlink_page(uint8 info, XLogReaderState *record)
{
	xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *) XLogRecGetData(record);
	BlockNumber			  leftsib;
	Buffer rbuffer;
	Buffer buffer;
	XLogRecPtr lsn = record->EndRecPtr;
	XLogRedoAction action;

	leftsib = xlrec->leftsib;

	/*
     * In normal operation, we would lock all the pages this WAL record
     * touches before changing any of them.  In WAL replay, it should be okay
     * to lock just one page at a time, since no concurrent index updates can
     * be happening, and readers should not care whether they arrive at the
     * target page or not (since it's surely empty).
     */

	/* Fix left-link of right sibling */
	action = XLogReadBufferForRedoExtended(record, BTREE_UNLINK_PAGE_RIGHT_NUM, RBM_NORMAL, false, &rbuffer);
	if (action == BLK_NEEDS_REDO)
	{
		_xbt_xlog_unlink_redo_right_page(rbuffer, lsn, xlrec);
		MarkBufferDirty(rbuffer);
	}
	if (BufferIsValid(rbuffer))
		UnlockReleaseBuffer(rbuffer);

	/* Fix right-link of left sibling, if any */
	if (leftsib != P_NONE)
	{
		Buffer lbuffer;
		action = XLogReadBufferForRedoExtended(record, BTREE_UNLINK_PAGE_LEFT_NUM, RBM_NORMAL, false, &lbuffer);
		if (action == BLK_NEEDS_REDO)
		{
			_xbt_xlog_unlink_redo_left_page(lbuffer, lsn, xlrec);
			MarkBufferDirty(lbuffer);
		}
		if (BufferIsValid(lbuffer))
			UnlockReleaseBuffer(lbuffer);
	}

	/* Rewrite target page as empty deleted page */
	buffer = XLogInitBufferForRedo(record, BTREE_UNLINK_PAGE_CUR_PAGE_NUM);
	_xbt_xlog_unlink_redo_page(buffer, lsn, xlrec);
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	/*
     * If we deleted a parent of the targeted leaf page, instead of the leaf
     * itself, update the leaf to point to the next remaining child in the
     * branch.
     */
	if (XLogRecHasBlockRef(record, BTREE_UNLINK_PAGE_CHILD_NUM))
	{
		/*
         * There is no real data on the page, so we just re-create it from
         * scratch using the information from the WAL record.
         */
		Buffer cbuffer;
		cbuffer = XLogInitBufferForRedo(record, BTREE_UNLINK_PAGE_CHILD_NUM);

		_xbt_xlog_unlink_redo_children(cbuffer, lsn, xlrec);
		MarkBufferDirty(cbuffer);
		UnlockReleaseBuffer(cbuffer);
	}

	if (info == XLOG_XBTREE_UNLINK_PAGE_META)
		_xbt_restore_meta(record, BTREE_UNLINK_PAGE_META_NUM);
}

static void
xbtree_xlog_new_root_update(XLogReaderState *record)
{
	xl_btree_newroot *xlrec = (xl_btree_newroot *) XLogRecGetData(record);
	BlockNumber		  downlink = 0;
	Buffer	          buffer;
	Buffer	          lbuffer;
	char			 *ptr = NULL;
	Size			  len;
	XLogRecPtr        lsn = record->EndRecPtr;
	XLogRedoAction action;

	buffer = XLogInitBufferForRedo(record, BTREE_NEWROOT_ORIG_BLOCK_NUM);

	ptr = XLogRecGetBlockData(record, BTREE_NEWROOT_ORIG_BLOCK_NUM, &len);
	_xbt_xlog_new_root_redo(buffer, lsn, (void *) xlrec, (void *) ptr, len, &downlink);

	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	lbuffer = InvalidBuffer;
	if (xlrec->level > 0)
	{
		action = XLogReadBufferForRedoExtended(record, BTREE_NEWROOT_LEFT_BLOCK_NUM, RBM_NORMAL, false, &lbuffer);
		if (action == BLK_NEEDS_REDO)
		{
			Page				  page = BufferGetPage(lbuffer);
			XBTPageOpaqueInternal pageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
			Assert(P_INCOMPLETE_SPLIT(pageop));
			pageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
			PageSetLSN(page, lsn);
			MarkBufferDirty(lbuffer);
		}
	}
	
	if (BufferIsValid(lbuffer))
	{
		UnlockReleaseBuffer(lbuffer);
	}
	_xbt_restore_meta(record, BTREE_NEWROOT_META_BLOCK_NUM);
}


static void
xbtree_xlog_reuse_page(XLogReaderState *record)
{
	xl_btree_reuse_page *xlrec = (xl_btree_reuse_page *)XLogRecGetData(record);

	/*
     * Btree reuse_page records exist to provide a conflict point when we
     * reuse pages in the index via the FSM.  That's all they do though.
     *
     * latestRemovedXid was the page's btpo.xact.  The btpo.xact <
     * RecentGlobalXmin test in _bt_page_recyclable() conceptually mirrors the
     * pgxact->xmin > limitXmin test in GetConflictingVirtualXIDs().
     * Consequently, one XID value achieves the same exclusion effect on
     * master and standby.
     */
	RelFileLocator tmp_node;
	tmp_node = xlrec->locator;
	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshotFullXid(xlrec->snapshotConflictHorizon, false, tmp_node);
}


UndoRecPtr
_xbt_redo_undo_delete(XLogReaderState *record,const BlockNumber blkno)
{
	XLogRecPtr	  lsn = record->EndRecPtr;
	FullTransactionId xid = XLogRecGetFullXid(record);
	FullTransactionId subFullXid = InvalidFullTransactionId;
	xl_undo_meta  undometa;
	FullTransactionId *fullxid;
	Size         *indTupleSize = 0;
	IndexTuple    indTuple;

	UndoRecPtr	 *blkprev;
	UndoRecPtr	 *prevurp;

	UndoRecPtr	  invalidUrp = INVALID_UNDO_REC_PTR;
	xl_undo_meta *xlundometa = NULL;
	UndoRecPtr	  urecptr = INVALID_UNDO_REC_PTR;

	bool		skipInsert = false;
	RelFileLocator targetNode ;
	UnpackedUndoRecord *undorec = NULL;

	xl_xbtree_delete *xlrec = (xl_xbtree_delete *) XLogRecGetData(record);
	xl_undo_header  *xlundohdr = (xl_undo_header *) ((char *) xlrec + SizeOfXBTreeDelete); 

	char *currLogPtr = ((char *) xlundohdr + SizeOfXLUndoHeader);
	init_xlog_undo_meta(&undometa);

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
	{
		blkprev = (UndoRecPtr *) ((char *) currLogPtr);
		currLogPtr += sizeof(UndoRecPtr);
	}
	else
		blkprev = &invalidUrp;

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
	{
		prevurp = (UndoRecPtr *) ((char *) currLogPtr);
		currLogPtr += sizeof(UndoRecPtr);
	}
	else
		prevurp = &invalidUrp;

	// swap subxid and topxid
	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
	{
		fullxid = (FullTransactionId *) currLogPtr;
		currLogPtr += sizeof(FullTransactionId);
		subFullXid = xid;
		xid = *fullxid;
	}

	indTupleSize = (Size *) currLogPtr;
	currLogPtr += sizeof(Size);

	indTuple = (IndexTuple) currLogPtr;
	currLogPtr += *indTupleSize;

	xlundometa = (xl_undo_meta *) ((char *) currLogPtr);
	urecptr = xlundohdr->urecptr;

	/* copy xlundometa to local struct */
	copy_xlog_undo_meta(xlundometa, &undometa);

	skipInsert = is_skip_insert_undo(urecptr, &undometa);
	if (skipInsert)
	{
		xlog_undo_meta_setinfo(&undometa, XLOG_UNDOMETA_INFO_SKIP);
	}

	/* recover undo record */
	undorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
	undorec->uur_urp = urecptr;

	XLogRecGetBlockTag(record, 0, &targetNode, NULL, NULL);
	urecptr = _xbt_prepare_undo_delete(
		xlundohdr->relOid, targetNode.relNumber, targetNode.spcOid,
		UNDO_PERMANENT, xlrec->offset, xid, 0, indTuple, *blkprev,
		*prevurp,  xlrec->oldxid,  blkno, record, 
		xlundohdr, &undometa, subFullXid);

	ereport(DEBUG2,
			(errmsg("redo undorecord for index delete :undoptr=%lu, xid %lu,  spcoid=%u.",
					urecptr, xid.value, targetNode.spcOid)));

	Assert(urecptr == xlundohdr->urecptr);
	SetUndoRecordOffset(undorec, xlrec->offset);
	if (!skipInsert)
		/* Insert the Undo record into the undo store */
		insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, lsn);
	redo_undo_meta(record, &undometa, xlundohdr->urecptr,
					prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
					prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));

	reset_prepared_buffers_in_ctx();

	return urecptr;
}

static void
xbtree_xlog_delete(XLogReaderState *record)
{
	XLogRecPtr lsn = record->EndRecPtr;
	Buffer buffer;
	xl_xbtree_delete *xlrec = (xl_xbtree_delete *) XLogRecGetData(record);
	RelFileLocator relationNode;
	FullTransactionId xid = xlrec->xid;
	OffsetNumber offset = xlrec->offset;
    BlockNumber	blkno = InvalidBlockNumber;
	UndoRecPtr urecptr = INVALID_UNDO_REC_PTR;

    XLogRecGetBlockTag(record, 0, &relationNode, NULL, &blkno);
    // preare undo, insert prepare undo, update undolog meta, and update transaction slot
    urecptr = _xbt_redo_undo_delete(record, blkno);

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		XBTPageOpaqueInternal opaque;
		IndexTuple	itup;
		XBTreeIndexTuple uxid;
		ItemId	item;
		Page page = BufferGetPage(buffer);
		item = PageGetItemId(page, offset);
		itup = (IndexTuple) PageGetItem(page, item);
		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	    uxid = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);
		IndexItemIdSetDeleted(item);
		uxid->modified_xid = XLogRecGetFullXid(record);
		uxid->urec = urecptr; 
		/* update active hint */
		opaque->active_count--;

		if (FullTransactionIdPrecedes(opaque->last_delete_xid, xid))
			opaque->last_delete_xid = xid;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
xbtree_xlog_prune(XLogReaderState *record)
{
	Buffer buffer = InvalidBuffer;
	RelFileLocator rnode;
	xl_xbtree_prune_page *xlrec = (xl_xbtree_prune_page*)XLogRecGetData(record);
	XLogRecPtr lsn = record->EndRecPtr;

	XLogRecGetBlockTag(record, 0, &rnode, NULL, NULL);
	
	if (InHotStandby && TransactionIdIsValid(xlrec->latestRemovedXid.value))
		ResolveRecoveryConflictWithSnapshot(xlrec->latestRemovedXid.value, false, rnode);

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page page = BufferGetPage(buffer);
		XBTPageOpaqueInternal opaque;
		/* Set up flags and try to repair page  fragmentation */
		xbt_page_prune_execute(page,
						(OffsetNumber *) (((char *) xlrec) + SizeOfXBTreePrunePage),
						xlrec->count, NULL, InvalidFullTransactionId);
		xbt_page_repair_fragmentation(NULL, BufferGetBlockNumber(buffer), page);

		opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
		opaque->pd_prune_xid = xlrec->new_prune_xid;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}

	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
_xbt_xlog_restore_undo_page(char *curxlogptr, Buffer buffer, Page page)
{
	uint8 *flags = NULL;
	OffsetNumber  *xlogLPOffset = NULL;

	FullTransactionId   *modifiedXid = NULL;
    UndoRecPtr		    *undoRecPtr = NULL;
	FullTransactionId	*pdPruneXid = NULL;
	FullTransactionId   *lastDeleteXid = NULL;
	uint16				*pdFlags = NULL;
	uint16				*activeTupleCount = NULL;

	XBTPageOpaqueInternal opaque = NULL;
	PageHeader	          phdr = NULL;
	ItemId				  item;
	IndexTuple            itup;
	XBTreeIndexTuple      xbt_tuple;

	flags = (uint8 *) curxlogptr;
	curxlogptr += sizeof(uint8);

	xlogLPOffset = (OffsetNumber *) curxlogptr;
	curxlogptr += sizeof(OffsetNumber);

	Assert(*xlogLPOffset > InvalidOffsetNumber);
	Assert(*xlogLPOffset <= MaxOffsetNumber);

	modifiedXid = (FullTransactionId *) curxlogptr;
	curxlogptr += sizeof(FullTransactionId);
	undoRecPtr = (UndoRecPtr *) curxlogptr;
	curxlogptr += sizeof(UndoRecPtr);

	pdFlags = (uint16 *) curxlogptr;
	curxlogptr += sizeof(uint16);

	pdPruneXid = (FullTransactionId *) curxlogptr;
	curxlogptr += sizeof(FullTransactionId);

	lastDeleteXid = (FullTransactionId *) curxlogptr;
	curxlogptr += sizeof(FullTransactionId);

	activeTupleCount = (uint16 *) curxlogptr;
	curxlogptr += sizeof(uint16);

	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	phdr = (PageHeader ) page;

	phdr->pd_flags = *pdFlags;
	opaque->pd_prune_xid = *pdPruneXid;
	opaque->last_delete_xid = *lastDeleteXid;
	opaque->active_count = *activeTupleCount;

	item = PageGetItemId(page, *xlogLPOffset);
	itup = (IndexTuple) PageGetItem(page, item);
	xbt_tuple = (XBTreeIndexTuple) XbtreeIndexGetTuple(itup);
	if(*flags == XBTREEUNDO_INSERT)
	{
		ItemIdMarkDead(item);
	}
	else
	{
		ItemIdSetNormal(item, item->lp_off, item->lp_len);
	}
	xbt_tuple->modified_xid = *modifiedXid;
	xbt_tuple->urec = *undoRecPtr;

	elog(DEBUG2,"xbtree undo restore blkno %d flags %d startrowptr %d modifiedXid %ld undoPtr %ld,"
	" page header pd_flag %d ,page tail prune_xid %lu, last_delete_xid %lu, activeTupleCount %d",
	BufferGetBlockNumber(buffer), *flags, *xlogLPOffset,  modifiedXid->value, *undoRecPtr, 
	 *pdFlags, pdPruneXid->value, lastDeleteXid->value, *activeTupleCount);

}

static void
xbtree_xlog_undo(XLogReaderState *record)
{
	Buffer			  buf;
	char			 *curxlogptr = (char *) (XLogRecGetData(record));
	XLogRedoAction	  action = XLogReadBufferForRedo(record, 0, &buf);
	BlockNumber		  blkno = InvalidBlockNumber;

	XLogRecGetBlockTag(record, 0, NULL, NULL, &blkno);
	if (action == BLK_NEEDS_REDO)
	{
		Page page = BufferGetPage(buf);

		/* Restore Rolledback Page */
		_xbt_xlog_restore_undo_page(curxlogptr, buf, page);
		
		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buf);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

void
xbtree_redo(XLogReaderState *record)
{
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_XBTREE_INSERT_LEAF:
			xbtree_xlog_insert(true, false, record);
			break;
		case XLOG_XBTREE_INSERT_UPPER:
			xbtree_xlog_insert(false, false, record);
			break;
		case XLOG_XBTREE_INSERT_META:
			xbtree_xlog_insert(false, true, record);
			break;
		case XLOG_XBTREE_SPLIT_L:
			xbtree_xlog_split(true, record, true);
			break;
		case XLOG_XBTREE_SPLIT_R:
			xbtree_xlog_split(false, record, true);
			break;
		case XLOG_XBTREE_UNLINK_PAGE:
		case XLOG_XBTREE_UNLINK_PAGE_META:
			xbtree_xlog_unlink_page(info, record);
			break;
		case XLOG_XBTREE_MARK_PAGE_HALFDEAD:
			xbtree_xlog_mark_page_halfdead(info, record);
			break;
		case XLOG_XBTREE_NEWROOT:
			xbtree_xlog_new_root_update(record);
			break;
		case XLOG_XBTREE_REUSE_PAGE:
			xbtree_xlog_reuse_page(record);
			break;
		case XLOG_XBTREE_DELETE:
			xbtree_xlog_delete(record);
			break;
		case XLOG_XBTREE_PRUNE_PAGE:
			xbtree_xlog_prune(record);
			break;
		case XLOG_XBTREE_UNDO:
			xbtree_xlog_undo(record);
			break;
		default:
			ereport(PANIC, (errmsg("xbtree_redo: unknown op code %hhu", info)));
	}
}

/*
 * _xbt_restore_page -- re-enter all the index tuples on a page
 *
 * The page is freshly init'd, and *from (length len) is a copy of what
 * had been its upper part (pd_upper to pd_special).  We assume that the
 * tuples had been added to the page in item-number order, and therefore
 * the one with highest item number appears first (lowest on the page).
 *
 * NOTE: the way this routine is coded, the rebuilt page will have the items
 * in correct itemno sequence, but physically the opposite order from the
 * original, because we insert them in the opposite of itemno order.  This
 * does not matter in any current btree code, but it's something to keep an
 * eye on.	Is it worth changing just on general principles?  See also the
 * notes in xbtree_xlog_split().
 */
static void
_xbt_restore_page(Page page, char *from, int len)
{
	IndexTupleData itupdata;
	Size		   itemsz;
	char		  *end = from + len;

	XBTPageOpaqueInternal opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	for (; from < end;)
	{
		memcpy(&itupdata, from, sizeof(IndexTupleData));
		itemsz = IndexTupleSize(&itupdata);

		/* every non-pivot tuple in pages which include undo info */
		if (P_ISLEAF(opaque) && !XBTreeTupleIsPivot((IndexTuple) from))
		{
			itemsz += TXNINFOSIZE;
		}

		itemsz = MAXALIGN(itemsz);
		if (PageAddItem(page, (Item) from, itemsz, FirstOffsetNumber, false, false) ==
			InvalidOffsetNumber)
			ereport(PANIC, (errmsg("XBTreeRestorePage: cannot add item to page")));
		from += itemsz;
	}
}


static void
_xbt_xlog_split_redo_right_page(Buffer rbuf, XLogRecPtr lsn, void *recorddata,
								 BlockNumber leftsib, BlockNumber rnext, void *blkdata,
								 Size datalen, bool hasOpaque)
{
	xl_xbtree_split		 *xlrec = (xl_xbtree_split *) recorddata;
	bool				  isleaf = (xlrec->level == 0);
	Page				  rpage = BufferGetPage(rbuf);
	char				 *datapos = (char *) blkdata;
	char                 *itempos  = datapos;
	OffsetNumber          maxoff = 0;
	OffsetNumber          i = 0;
	ItemId                itemid;
	XBTPageOpaqueInternal ropaque;

	_xbt_pageinit(rpage, BufferGetPageSize(rbuf));
	ropaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(rpage);

	if (hasOpaque)
	{
		if (xlrec->opaqueversion != XBTREE_OPAQUE_VERSION)
			ereport(ERROR, (errmsg("unknown btree opaque version")));
		*ropaque = xlrec->ropaque;
	}
	else
	{
		ropaque->btpo_prev = leftsib;
		ropaque->btpo_next = rnext;
		ropaque->btpo.level = xlrec->level;
		ropaque->btpo_flags = isleaf ? BTP_LEAF : 0;
		ropaque->btpo_cycleid = 0;
	}
	datapos = datapos + (xlrec->rightitemcount * sizeof(ItemIdData));
	datalen -= xlrec->rightitemcount * sizeof(ItemIdData);
	_xbt_restore_page(rpage, datapos, (int) datalen);

	// update items flags
	maxoff = PageGetMaxOffsetNumber(rpage);
	for (i = P_FIRSTDATAKEY(ropaque); i <= maxoff; i = OffsetNumberNext(i))
	{
		itemid = PageGetItemId(rpage, i);
		itemid->lp_flags = ((ItemId)(itempos + (i-1)*sizeof(ItemIdData)))->lp_flags;
	}

	PageSetLSN(rpage, lsn);
}

static void
_xbt_xlog_split_redo_next_page(Buffer buf, XLogRecPtr lsn, BlockNumber rightsib)
{
	Page page = BufferGetPage(buf);

	XBTPageOpaqueInternal pageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	pageop->btpo_prev = rightsib;
	PageSetLSN(page, lsn);
}

static void
_xbt_xlog_split_redo_left_page(Buffer lbuf, XLogRecPtr lsn, void *recorddata,
								BlockNumber rightsib, bool onleft, void *blkdata,
								Size datalen, bool hasOpaque)
{
	xl_xbtree_split *xlrec = (xl_xbtree_split *) recorddata;
	bool			 isleaf = (xlrec->level == 0);
	Page			 lpage = BufferGetPage(lbuf);
	char			*datapos = (char *) blkdata;
	Item			 left_hikey = NULL;
	Size			 left_hikeysz = 0;


	XBTPageOpaqueInternal lopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(lpage);
	OffsetNumber		  off;
	Item				  newitem = NULL;
	Size				  newitemsz = 0;
	Page				  newlpage;
	OffsetNumber		  leftoff;

	if (onleft)
	{
		newitem = (Item) datapos;
		newitemsz = MAXALIGN(XstoreIndexTupleSize(newitem));
		if (P_ISLEAF(lopaque))
		{
			newitemsz += TXNINFOSIZE;
		}
		datapos += newitemsz;
		datalen -= newitemsz;
	}

	left_hikey = (Item) datapos;
	left_hikeysz = MAXALIGN(XstoreIndexTupleSize(left_hikey));
	datapos += left_hikeysz;
	datalen -= left_hikeysz;
	Assert(datalen == 0);

	newlpage = PageGetTempPageCopySpecial(lpage);

	leftoff = P_HIKEY;
	if (PageAddItem(newlpage, left_hikey, left_hikeysz, P_HIKEY, false, false) ==
		InvalidOffsetNumber)
		ereport(PANIC, (errmsg("failed to add high key to left page after split")));
	leftoff = OffsetNumberNext(leftoff);

	for (off = P_FIRSTDATAKEY(lopaque); off < xlrec->firstright; off++)
	{
		ItemId itemid;
		Size   itemsz;
		Item   item;
		ItemId nitemid;

		if (onleft && off == xlrec->newitemoff)
		{
			if (PageAddItem(newlpage, newitem, newitemsz, leftoff, false, false) ==
				InvalidOffsetNumber)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("failed to add new item to left page after split")));
			leftoff = OffsetNumberNext(leftoff);
		}

		itemid = PageGetItemId(lpage, off);
		itemsz = ItemIdGetLength(itemid);
		item = PageGetItem(lpage, itemid);
		if (PageAddItem(newlpage, item, itemsz, leftoff, false, false) ==
			InvalidOffsetNumber)
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("failed to add old item to left page after split")));
		nitemid = PageGetItemId(newlpage, leftoff);
		nitemid->lp_flags = itemid->lp_flags;
		leftoff = OffsetNumberNext(leftoff);
	}

	if (onleft && off == xlrec->newitemoff)
	{
		if (PageAddItem(newlpage, newitem, newitemsz, leftoff, false, false) ==
			InvalidOffsetNumber)
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("failed to add new item to left page after split")));
		leftoff = OffsetNumberNext(leftoff);
	}

	PageRestoreTempPage(newlpage, lpage);

	if (hasOpaque)
	{
		if (xlrec->opaqueversion != XBTREE_OPAQUE_VERSION)
			ereport(ERROR, (errmsg("unknown btree opaque version")));
		*lopaque = xlrec->lopaque;
	}
	else
	{
		lopaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(lpage);
		lopaque->btpo_flags = BTP_INCOMPLETE_SPLIT;
		if (isleaf)
			lopaque->btpo_flags |= BTP_LEAF;

		lopaque->btpo_next = rightsib;
		lopaque->btpo_cycleid = 0;
	}

	PageSetLSN(lpage, lsn);
}

static void
_xbt_xlog_half_dead_redo_parent_page(Buffer pbuf, XLogRecPtr lsn, void *recorddata)
{
	xl_btree_mark_page_halfdead *xlrec = (xl_btree_mark_page_halfdead *) recorddata;
	OffsetNumber				 poffset;
	ItemId						 itemid;
	IndexTuple					 itup;
	OffsetNumber				 nextoffset;
	BlockNumber					 rightsib;
	Page                         ppage = BufferGetPage(pbuf);

	poffset = xlrec->poffset;

	nextoffset = OffsetNumberNext(poffset);
	itemid = PageGetItemId(ppage, nextoffset);
	itup = (IndexTuple) PageGetItem(ppage, itemid);
	rightsib = ItemPointerGetBlockNumber(&(itup->t_tid));

	itemid = PageGetItemId(ppage, poffset);
	itup = (IndexTuple) PageGetItem(ppage, itemid);
	ItemPointerSetBlockNumber(&(itup->t_tid), rightsib);
	nextoffset = OffsetNumberNext(poffset);
	PageIndexTupleDelete(ppage, nextoffset);

	PageSetLSN(ppage, lsn);
}

static void
_xbt_xlog_half_dead_redo_leaf_page(Buffer lbuf, XLogRecPtr lsn, void *recorddata)
{
	IndexTupleData				 trunctuple;
	XBTPageOpaqueInternal		 pageop;
	Page                         lpage = BufferGetPage(lbuf);

	xl_btree_mark_page_halfdead *xlrec = (xl_btree_mark_page_halfdead *) recorddata;

	_xbt_pageinit(lpage, BufferGetPageSize(lbuf));
	pageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(lpage);

	pageop->btpo_prev = xlrec->leftblk;
	pageop->btpo_next = xlrec->rightblk;

	pageop->btpo.level = 0;
	pageop->btpo_flags = BTP_HALF_DEAD | BTP_LEAF;
	pageop->btpo_cycleid = 0;


	memset(&trunctuple, 0, sizeof(IndexTupleData));
	trunctuple.t_info = sizeof(IndexTupleData);
	ItemPointerSet(&(trunctuple.t_tid), xlrec->topparent, 0);

	if (PageAddItem(lpage, (Item) &trunctuple, sizeof(IndexTupleData),
					P_HIKEY, false, false) == InvalidOffsetNumber)
	{
		ereport(ERROR, (errmsg("could not add dummy high key to half-dead page")));
	}

	PageSetLSN(lpage, lsn);
}

static void
_xbt_xlog_unlink_redo_right_page(Buffer rbuf, XLogRecPtr lsn, void *recorddata)
{
	xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *) recorddata;
	Page rpage = BufferGetPage(rbuf);
	XBTPageOpaqueInternal pageop =
		(XBTPageOpaqueInternal) PageGetSpecialPointer(rpage);
	pageop->btpo_prev = xlrec->leftsib;

	PageSetLSN(rpage, lsn);
}

static void
_xbt_xlog_unlink_redo_left_page(Buffer lbuf, XLogRecPtr lsn, void *recorddata)
{
	xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *) recorddata;
	Page  page = BufferGetPage(lbuf);
	XBTPageOpaqueInternal pageop =
		(XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	pageop->btpo_next = xlrec->rightsib;

	PageSetLSN(page, lsn);
}

static void
_xbt_xlog_unlink_redo_page(Buffer buf, XLogRecPtr lsn, void *recorddata)
{
	XBTPageOpaqueInternal pageop;
	Page  page = BufferGetPage(buf);
	xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *) recorddata;

	_xbt_pageinit(page, BufferGetPageSize(buf));
	pageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	pageop->btpo_prev = xlrec->leftsib;
	pageop->btpo_next = xlrec->rightsib;
	pageop->btpo.xact_old =  XidFromFullTransactionId( xlrec->safexid);
	pageop->btpo_flags = BTP_DELETED;
	pageop->btpo_cycleid = 0;

	PageSetLSN(page, lsn);
}

static void
_xbt_xlog_unlink_redo_children(Buffer cbuf, XLogRecPtr lsn, void *recorddata)
{
	IndexTupleData		  trunctuple;
	XBTPageOpaqueInternal pageop;
	Page                  cpage = BufferGetPage(cbuf);
	xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *) recorddata;

	_xbt_pageinit(cpage, BufferGetPageSize(cbuf));

	pageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(cpage);

	pageop->btpo_flags = BTP_HALF_DEAD | BTP_LEAF;
	pageop->btpo_prev = xlrec->leafleftsib;
	pageop->btpo_next = xlrec->leafrightsib;
	pageop->btpo.level = 0;
	pageop->btpo_cycleid = 0;

	memset(&trunctuple, 0, sizeof(IndexTupleData));
	trunctuple.t_info = sizeof(IndexTupleData);
	ItemPointerSet(&(trunctuple.t_tid), xlrec->leaftopparent, 0);

	if (PageAddItem(cpage, (Item) &trunctuple, sizeof(IndexTupleData),
					P_HIKEY, false, false) == InvalidOffsetNumber)
	{
		ereport(ERROR, (errmsg("could not add dummy high key to half-dead page")));
	}

	PageSetLSN(cpage, lsn);
}

static void
_xbt_xlog_new_root_redo(Buffer buffer, XLogRecPtr lsn, void *record, void *blkdata,
							  Size len, BlockNumber *downlink)
{
	xl_btree_newroot	 *xlrec = (xl_btree_newroot *) record;
	Page				  page = BufferGetPage(buffer);
	char				 *ptr = (char *) blkdata;
	XBTPageOpaqueInternal pageop;

	_xbt_pageinit(page, BufferGetPageSize(buffer));
	pageop = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);

	pageop->btpo_flags = BTP_ROOT;
	pageop->btpo_prev = pageop->btpo_next = P_NONE;
	pageop->btpo.level = xlrec->level;
	if (xlrec->level == 0)
		pageop->btpo_flags |= BTP_LEAF;

	pageop->btpo_cycleid = 0;

	if (xlrec->level > 0)
		_xbt_restore_page(page, ptr, len);

	PageSetLSN(page, lsn);
}

