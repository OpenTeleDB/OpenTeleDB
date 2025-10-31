/*-------------------------------------------------------------------------
 *
 * xbtdesc.c
 *	  rmgr descriptor routines for access/xbtree/xbtxlog.c
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/xbtree/xbtdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/nbtxlog.h"
#include "xbtree/xbtxlog.h"

void
xbtree_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_XBTREE_INSERT_LEAF:
		case XLOG_XBTREE_INSERT_UPPER:
		case XLOG_XBTREE_INSERT_META:
			{
				xl_btree_insert *xlrec = (xl_btree_insert *)rec;

				appendStringInfo(buf, "off %u", xlrec->offnum);
				break;
			}
		case XLOG_XBTREE_SPLIT_L:
		case XLOG_XBTREE_SPLIT_R:
			{
				xl_btree_split *xlrec = (xl_btree_split *)rec;

				appendStringInfo(buf, "level %u, firstrightoff %d, newitemoff %d",
								 xlrec->level, xlrec->firstrightoff,
								 xlrec->newitemoff);
				break;
			}
		case XLOG_XBTREE_VACUUM:
			{
				xl_btree_vacuum *xlrec = (xl_btree_vacuum *)rec;

				appendStringInfo(buf, "ndeleted %u; nupdated %u",
									xlrec->ndeleted, xlrec->nupdated);
				break;
			}
		case XLOG_XBTREE_UNLINK_PAGE:
		case XLOG_XBTREE_UNLINK_PAGE_META:
			{
				xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *)rec;

				appendStringInfo(buf, "left %u; right %u; level %u; safexid %u:%u; ",
									xlrec->leftsib, xlrec->rightsib, xlrec->level,
									EpochFromFullTransactionId(xlrec->safexid),
									XidFromFullTransactionId(xlrec->safexid));
				appendStringInfo(buf, "leafleft %u; leafright %u; leaftopparent %u",
									xlrec->leafleftsib, xlrec->leafrightsib,
									xlrec->leaftopparent);
				break;
			}
		case XLOG_XBTREE_MARK_PAGE_HALFDEAD:
			{
				xl_btree_mark_page_halfdead *xlrec = (xl_btree_mark_page_halfdead *)rec;

				appendStringInfo(buf, "topparent %u; leaf %u; left %u; right %u",
								 xlrec->topparent, xlrec->leafblk, xlrec->leftblk, xlrec->rightblk);
				break;
			}
		case XLOG_XBTREE_NEWROOT:
			{
				xl_btree_newroot *xlrec = (xl_btree_newroot *)rec;

				appendStringInfo(buf, "lev %u", xlrec->level);
				break;
			}
		case XLOG_XBTREE_REUSE_PAGE:
			{
				xl_btree_reuse_page *xlrec = (xl_btree_reuse_page *) rec;
				appendStringInfo(buf, "rel %u/%u/%u; latestRemovedXid %u:%u",
								 xlrec->locator.spcOid, xlrec->locator.dbOid,
								 xlrec->locator.relNumber,
								 EpochFromFullTransactionId(xlrec->snapshotConflictHorizon),
								 XidFromFullTransactionId(xlrec->snapshotConflictHorizon));
				break;
			}
		case XLOG_XBTREE_DELETE:
			{
				xl_xbtree_delete* xlrec = (xl_xbtree_delete*)rec;
				appendStringInfo(buf,
								"mark delete: off %d xmax  %lu ",
						xlrec->offset,
						xlrec->xid.value);
				break;
			}
		case XLOG_XBTREE_PRUNE_PAGE:
			{
				xl_xbtree_prune_page* xlrec = (xl_xbtree_prune_page*)rec;

				appendStringInfo(buf, "count %d, new_prune_xid %lu, latestRemovedXid %lu", xlrec->count,
								xlrec->new_prune_xid.value, xlrec->latestRemovedXid.value);
				break;
			}
		case XLOG_XBTREE_UNDO:
			{
				char  *cur_xlog_ptr = NULL;
				OffsetNumber  *xlogLPOffset = NULL;
				uint8 *flags = (uint8 *) rec;

				FullTransactionId *modified_xid = NULL;
				UndoRecPtr		  *undo_rec_ptr = NULL;
				uint16		      *pd_flags = NULL;
				FullTransactionId *pd_prune_xid = NULL;
				FullTransactionId *last_delete_xid = NULL;
				int16		      *active_tuple_count = NULL;
				
				appendStringInfo(buf, "xbtree undo ::  ");
				appendStringInfo(buf, "flags: %d ", *flags);
				cur_xlog_ptr = (char *) ((char *) flags + sizeof(uint8));
				
				xlogLPOffset = (OffsetNumber *) cur_xlog_ptr;
				cur_xlog_ptr += sizeof(OffsetNumber);

				appendStringInfo(buf, "xlogLPOffset: %d, ", *xlogLPOffset);

				/* Restore indextuple sub data */
				modified_xid = (FullTransactionId *) cur_xlog_ptr;
				cur_xlog_ptr += sizeof(FullTransactionId);
				undo_rec_ptr = (UndoRecPtr *) cur_xlog_ptr;
				cur_xlog_ptr += sizeof(UndoRecPtr);

				appendStringInfo(buf, "modifiedXid: %ld, ", modified_xid->value);
				appendStringInfo(buf, "undoRecPtr: %ld, ", *undo_rec_ptr);

				/* Restore updated page headers and tail */
				pd_flags = (uint16 *) cur_xlog_ptr;
				cur_xlog_ptr += sizeof(uint16);

				pd_prune_xid = (FullTransactionId *) cur_xlog_ptr;
				cur_xlog_ptr += sizeof(FullTransactionId);
				
				last_delete_xid = (FullTransactionId *) cur_xlog_ptr;
				cur_xlog_ptr += sizeof(FullTransactionId);

				active_tuple_count = (int16 *) cur_xlog_ptr;
				cur_xlog_ptr += sizeof(int16);

				appendStringInfo(buf, "pdPruneXid: %lu, ", pd_prune_xid->value);
				appendStringInfo(buf, "pdFlags: %d, ", *pd_flags);
				appendStringInfo(buf, "lastDeleteXid: %lu, ", last_delete_xid->value);
				appendStringInfo(buf, "activeTupleCout: %d, ", *active_tuple_count);
			
				break;
			}
	}
}

const char *
xbtree_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_XBTREE_INSERT_LEAF:
			id = "XBT_INSERT_LEAF";
			break;
		case XLOG_XBTREE_INSERT_UPPER:
			id = "XBT_INSERT_UPPER";
			break;
		case XLOG_XBTREE_INSERT_META:
			id = "XBT_INSERT_META";
			break;
		case XLOG_XBTREE_SPLIT_L:
			id = "XBT_SPLIT_L";
			break;
		case XLOG_XBTREE_SPLIT_R:
			id = "XBT_SPLIT_R";
			break;
		case XLOG_XBTREE_VACUUM:
			id = "XBT_VACUUM";
			break;
		case XLOG_XBTREE_UNLINK_PAGE:
			id = "XBT_UNLINK_PAGE";
			break;
		case XLOG_XBTREE_UNLINK_PAGE_META:
			id = "XBT_UNLINK_PAGE_META";
			break;
		case XLOG_XBTREE_MARK_PAGE_HALFDEAD:
			id = "XBT_MARK_PAGE_HALFDEAD";
			break;
		case XLOG_XBTREE_NEWROOT:
			id = "XBT_NEWROOT";
			break;
		case XLOG_XBTREE_REUSE_PAGE:
			id = "XBT_REUSE_PAGE";
			break;
		case XLOG_XBTREE_DELETE:
			id = "XBT_DELETE";
			break;
		case XLOG_XBTREE_PRUNE_PAGE:
			id = "XBT_PRUNE_PAGE";
			break;
		case XLOG_XBTREE_UNDO:
			return "XBT_UNDO";
	}

	return id;
}