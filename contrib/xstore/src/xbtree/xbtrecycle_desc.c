/*-------------------------------------------------------------------------
 *
 * xbtrecycle_desc.c
 *	  rmgr descriptor routines for access/xbtree/xbtrecycle.c
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * IDENTIFICATION
 *	  src/xbtree/xbtrecycle_desc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/nbtxlog.h"
#include "xbtree/xbtrecycle.h"

void xbtree2_desc(StringInfo buf, XLogReaderState* record)
{
	char* rec = XLogRecGetData(record);
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info) {
		case XLOG_XBTREE2_RECYCLE_QUEUE_INIT_PAGE:
			{
				xl_xbtree2_recycle_queue_init_page *xlrec = (xl_xbtree2_recycle_queue_init_page *)rec;
				appendStringInfo(buf, "recycle queue init page: inserting %s, prev blkno %u, curr blkno %u next blkno %u",
					(xlrec->inserting_new_page ? "yes" : "no"), xlrec->prev_blkno, xlrec->curr_blkno, xlrec->next_blkno);
				break;
			}
		case XLOG_XBTREE2_RECYCLE_QUEUE_ENDPOINT:
			{
				xl_xbtree2_recycle_queue_endpoint *xlrec = (xl_xbtree2_recycle_queue_endpoint *)rec;
				appendStringInfo(buf, "recycle queue change endpoint: isHead %s, left blkno %u, right blkno %u",
					(xlrec->is_head ? "yes" : "no"), xlrec->left_blkno, xlrec->right_blkno);
				break;
			}
		case XLOG_XBTREE2_RECYCLE_QUEUE_MODIFY:
			{
				xl_xbtree2_recycle_queue_modify *xlrec = (xl_xbtree2_recycle_queue_modify *)rec;
				appendStringInfo(buf, "recycle queue modify: isInsert %s, blkno %u, offset %u, item_xid %ld item_blkno %d ",
					(xlrec->is_insert ? "yes" : "no"), xlrec->blkno, xlrec->offset, xlrec->item.xid.value, xlrec->item.blkno);
				break;
			}
		default:
			appendStringInfo(buf, "UNKNOWN");
			break;
	}
}

const char* xbtree2_type_name(uint8 subtype)
{
	uint8 info = subtype & ~XLR_INFO_MASK;

	switch (info) {
		case XLOG_XBTREE2_RECYCLE_QUEUE_INIT_PAGE:
			return "xbt2_recycle_init_page";
		case XLOG_XBTREE2_RECYCLE_QUEUE_ENDPOINT:
			return "xbt2_recycle_endpoint";
		case XLOG_XBTREE2_RECYCLE_QUEUE_MODIFY:
			return "xbt2_recycle_modify";
		default:
			return "unknown_type";
	}
}