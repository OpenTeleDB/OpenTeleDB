/* -------------------------------------------------------------------------
 *
 * xdecode.c
 * WAL logic decode for xheap.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California	
 *
 *
 * IDENTIFICATION
 * src/xheap/xdecode.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/relation.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "xheap/xredo.h"
#include "xheap/xtup_details.h"
#include "undo/undoxlog.h"
#include "catalog/pg_control.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/message.h"
#include "replication/reorderbuffer.h"
#include "replication/origin.h"
#include "replication/snapbuild.h"
#include "storage/standby.h"
#include "utils/memutils.h"


void decode_xheap_op(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);

static void decode_xheap_insert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void decode_xheap_update(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void decode_xheap_delete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void decode_xheap_multi_insert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);

static void decode_xlog_xtuple(const char *data, Size len, HeapTuple tuple);

static inline bool
filter_by_origin(LogicalDecodingContext *ctx, RepOriginId origin_id)
{
	if (ctx->callbacks.filter_by_origin_cb == NULL)
		return false;

	return filter_by_origin_cb_wrapper(ctx, origin_id);
}

static Pointer xlog_get_xlrec(XLogReaderState *record)
{
	Pointer		rec_data = (Pointer) XLogRecGetData(record);

	return rec_data;
}

static size_t decode_undo_meta(const char *data)
{
	uint64		info = (*(uint64 *) data) & XLOG_UNDOMETA_INFO_SLOT_ALLOC;

	if (info == 0)
		return sizeof(uint64);
	else
		return sizeof(uint64) + sizeof(Oid);
}

static Pointer xlog_get_multi_insert_xlrec(XLogReaderState *record)
{
	Size		meta_len;
	Pointer		rec_data = (Pointer) XLogRecGetData(record);
	xl_undo_header *xlundohdr = (xl_undo_header *) (rec_data);
	Size		header_len = SizeOfXLUndoHeader + sizeof(UndoRecPtr);

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
		header_len += sizeof(UndoRecPtr);
	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
		header_len += sizeof(UndoRecPtr);
	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
		header_len += sizeof(FullTransactionId);
	rec_data += header_len;
	meta_len = decode_undo_meta((char *) rec_data);
	rec_data += meta_len;

	return rec_data;
}

void
decode_xheap_op(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_XHEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	/*
	 * If we don't have snapshot or we are just fast-forwarding, there is no
	 * point in decoding data changes.
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_XHEAP_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				decode_xheap_insert(ctx, buf);
			break;

		case XLOG_XHEAP_UPDATE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				decode_xheap_update(ctx, buf);
			break;

		case XLOG_XHEAP_DELETE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				decode_xheap_delete(ctx, buf);
			break;

		case XLOG_XHEAP_CLEAN:
		case XLOG_XHEAP_LOCK:
			break;

		case XLOG_XHEAP_MULTI_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				decode_xheap_multi_insert(ctx, buf);
			break;
		default:
			elog(WARNING, "unexpected RM_XHEAP_ID record type: %u", info);
			break;
	}
}

static void
decode_xheap_insert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	RelFileLocator target_locator;
	ReorderBufferChange *change;
	xl_xheap_insert *xlrec = (xl_xheap_insert *) xlog_get_xlrec(r);

	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;
	/* output plugin doesn't look for this origin, no need to queue */
	if (filter_by_origin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_XINSERT;
	change->origin_id = XLogRecGetOrigin(r);
	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	if (xlrec->flags & XLOG_XHEAP_CONTAINS_NEW_TUPLE)
	{
		Size		tuplelen = 0;
		char	   *tupledata = XLogRecGetBlockData(r, 0, &tuplelen);

		change->data.tp.newtuple = ReorderBufferGetTupleBuf(ctx->reorder, tuplelen);
		decode_xlog_xtuple(tupledata, tuplelen, change->data.tp.newtuple);
	}

	change->data.tp.clear_toast_afterwards = true;
	ReorderBufferQueueChange(ctx->reorder, xheap_xlog_get_current_xid(r), buf->origptr, change, xlrec->flags & XLOG_XHEAP_INSERT_ON_TOAST_RELATION);
}

/*
 * Filter out records that we don't need to decode.
 */
static bool
xlog_filter_record(LogicalDecodingContext *ctx, XLogReaderState *r, uint8 flags, RelFileLocator *rlocator)
{
	if (filter_by_origin(ctx, XLogRecGetOrigin(r)))
		return true;
	if (((flags & XLZ_UPDATE_PREFIX_FROM_OLD) != 0) || ((flags & XLZ_UPDATE_SUFFIX_FROM_OLD) != 0))
	{
		elog(LOG, "update tuple has affix, don't decode it");
		return true;
	}
	XLogRecGetBlockTag(r, 0, rlocator, NULL, NULL);
	if (rlocator->dbOid != ctx->slot->data.database)
		return true;
	return false;
}

static void
update_undo_body(Size *add_len_ptr, char *data, uint8 flag, uint32 *toast_len)
{
	if ((flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
		*add_len_ptr += sizeof(UndoRecPtr);
	if ((flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
		*add_len_ptr += sizeof(UndoRecPtr);
	if ((flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
		*add_len_ptr += sizeof(FullTransactionId);
	if ((flag & XLOG_UNDO_HEADER_HAS_TOAST) != 0)
	{
		*toast_len = *(uint32 *) (data + *add_len_ptr);
		elog(DEBUG2, "update_undo_body toastLen = %u", *toast_len);
		*add_len_ptr += sizeof(uint32);
	}
}

static char *
calc_update_old_tuple(bool is_inplace_update, XLogReaderState *r, char **tuple_old, Size *tuplelen_old, uint32 *toast_len)
{
	Size		meta_len;
	char	   *toast_data;
	xl_undo_header *xlundohdr = (xl_undo_header *) (*tuple_old);
	Size		add_len = SizeOfXLUndoHeader;

	update_undo_body(&add_len, *tuple_old, xlundohdr->flag, toast_len);
	*tuple_old += add_len;
	*tuplelen_old -= add_len + *toast_len;

	toast_data = *tuple_old;
	*tuple_old += *toast_len;
	add_len = 0;
	if (!is_inplace_update)
	{
		xl_undo_header *xlnewundohdr = (xl_undo_header *) (*tuple_old);

		add_len += SizeOfXLUndoHeader;
		if ((xlnewundohdr->flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
			add_len += sizeof(UndoRecPtr);
		if ((xlnewundohdr->flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
			add_len += sizeof(UndoRecPtr);
	}

	meta_len = decode_undo_meta(*tuple_old + add_len);
	add_len += meta_len;
	*tuple_old += add_len;
	*tuplelen_old -= add_len;
	return toast_data;
}


static void
decode_xheap_update(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	RelFileLocator target_locator;
	xl_xheap_update *xlrec = (xl_xheap_update *) xlog_get_xlrec(r);
	ReorderBufferChange *change;
	Size		datalen_new = 0;
	char	   *data_new;
	Size		tuplelen_old;
	char	   *data_old;
	uint32		toast_len = 0;
	bool		has_toast = false;
	char	   *toast_ptr;
	char	   *toast_data = NULL;

	bool		is_inplace_update = (xlrec->flags & XLZ_NON_INPLACE_UPDATE) == 0;

	if (xlog_filter_record(ctx, r, xlrec->flags, &target_locator))
		return;

	data_new = XLogRecGetBlockData(r, 0, &datalen_new);
	tuplelen_old = XLogRecGetDataLen(r) - SizeOfXHeapUpdate;
	data_old = (char *) xlrec + SizeOfXHeapUpdate;
	toast_ptr = calc_update_old_tuple(is_inplace_update, r, &data_old, &tuplelen_old, &toast_len);

	if (toast_len > 0)
	{
		toast_data = (char *) palloc0(toast_len);
		memcpy(toast_data, toast_ptr, toast_len);

		has_toast = true;
	}

	if (toast_len == 0 && (tuplelen_old == 0 || !AllocSizeIsValid(tuplelen_old)))
	{
		elog(WARNING, "tuplelen is invalid(%lu), don't decode it", tuplelen_old);
		return;
	}

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_XUPDATE;
	change->origin_id = XLogRecGetOrigin(r);
	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	change->data.tp.newtuple = ReorderBufferGetTupleBuf(ctx->reorder, datalen_new);

	decode_xlog_xtuple(data_new, datalen_new, change->data.tp.newtuple);
	if (xlrec->flags & XLZ_HAS_UPDATE_UNDOTUPLE)
	{
		if (!has_toast)
		{
			change->data.tp.oldtuple = ReorderBufferGetTupleBuf(ctx->reorder, tuplelen_old);
			if (!is_inplace_update)
				decode_xlog_xtuple(data_old, tuplelen_old, change->data.tp.oldtuple);
			else if ((xlrec->flags & XLOG_XHEAP_CONTAINS_OLD_HEADER) != 0)
			{
				int			undoXorDeltaSize = *(int *) data_old;

				data_old += sizeof(int) + undoXorDeltaSize;
				tuplelen_old -= sizeof(int) + undoXorDeltaSize;
				decode_xlog_xtuple(data_old, tuplelen_old, change->data.tp.oldtuple);
			}
			else
			{
				elog(LOG, "current tuple is not fully logged, don't decode it");
				return;
			}
		}
		else
		{
			change->data.tp.oldtuple = ReorderBufferGetTupleBuf(ctx->reorder, toast_len);
			decode_xlog_xtuple(toast_data, toast_len, change->data.tp.oldtuple);
		}
	}

	change->data.tp.clear_toast_afterwards = true;
	ReorderBufferQueueChange(ctx->reorder, xheap_xlog_get_current_xid(r), buf->origptr, change, false);
	if (toast_data != NULL)
		pfree(toast_data);
}


static void
decode_xheap_delete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_xheap_delete *xlrec = NULL;
	RelFileLocator target_locator;

	xl_undo_header *xlundohdr;
	bool		has_toast = false;
	Size		datalen;
	Size		add_len;
	uint32		toast_len;
	char	   *toast_data = NULL;
	Size		meta_len;
	ReorderBufferChange *change;
	char	   *dataold;

	xlrec = (xl_xheap_delete *) xlog_get_xlrec(r);

	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;
	/* output plugin doesn't look for this origin, no need to queue */
	if (filter_by_origin(ctx, XLogRecGetOrigin(r)))
		return;
	xlundohdr = (xl_undo_header *) ((char *) xlrec + SizeOfXHeapDelete);
	has_toast = (xlundohdr->flag & XLOG_UNDO_HEADER_HAS_TOAST) != 0;
	datalen = XLogRecGetDataLen(r) - SizeOfXHeapDelete - SizeOfXLUndoHeader;
	add_len = 0;
	toast_len = 0;
	update_undo_body(&add_len, (char *) xlundohdr + SizeOfXLUndoHeader, xlundohdr->flag, &toast_len);

	if (toast_len > 0)
	{
		toast_data = (char *) palloc0(toast_len);
		memcpy(toast_data,
			   (char *) xlrec + SizeOfXHeapDelete + SizeOfXLUndoHeader + add_len,
			   toast_len);
	}
	add_len += toast_len;

	meta_len = decode_undo_meta((char *) xlrec + SizeOfXHeapDelete +
							 SizeOfXLUndoHeader + add_len);
	add_len += meta_len;
	if (toast_len == 0 && (datalen == 0 || !AllocSizeIsValid(datalen)))
	{
		elog(WARNING, "tuplelen is invalid(%lu), don't decode it", datalen);
		return;
	}
	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_XDELETE;
	change->origin_id = XLogRecGetOrigin(r);
	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	dataold =
		(char *) xlrec + SizeOfXHeapDelete + SizeOfXLUndoHeader + add_len;
	if (!has_toast)
	{
		change->data.tp.oldtuple = ReorderBufferGetTupleBuf(ctx->reorder, datalen - add_len);
		decode_xlog_xtuple(dataold, datalen - add_len, change->data.tp.oldtuple);
	}
	else
	{
		change->data.tp.oldtuple = ReorderBufferGetTupleBuf(ctx->reorder, toast_len);
		decode_xlog_xtuple(toast_data, toast_len, change->data.tp.oldtuple);
	}
	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, xheap_xlog_get_current_xid(r), buf->origptr, change, false);
	if (toast_data != NULL)
		pfree(toast_data);
}

static ReorderBufferChange *
xlog_get_xheap_change(LogicalDecodingContext *ctx, XLogReaderState *r, RelFileLocator *node)
{
	ReorderBufferChange *change = ReorderBufferGetChange(ctx->reorder);

	change->action = REORDER_BUFFER_CHANGE_XINSERT;
	change->origin_id = XLogRecGetOrigin(r);
	memcpy(&change->data.tp.rlocator, node, sizeof(RelFileLocator));

	return change;
}

static void
decode_xheap_multi_insert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	char	   *data;
	XLogReaderState *r = buf->record;
	Size		tuplelen = 0;
	RelFileLocator rlocator = {0, 0, 0};
	XlXHeapMultiInsert *xlrec = (XlXHeapMultiInsert *) xlog_get_multi_insert_xlrec(r);

	XLogRecGetBlockTag(r, 0, &rlocator, NULL, NULL);
	if (rlocator.dbOid != ctx->slot->data.database)
		return;
	/* output plugin doesn't look for this origin, no need to queue */
	if (filter_by_origin(ctx, XLogRecGetOrigin(r)))
		return;

	data = XLogRecGetBlockData(r, 0, &tuplelen);
	for (int i = 0; i < xlrec->ntuples; i++)
	{
		ReorderBufferChange *change = xlog_get_xheap_change(ctx, r, &rlocator);

		if (xlrec->flags & XLOG_XHEAP_CONTAINS_NEW_TUPLE)
		{
			int			len;
			XlMultiInsertXTuple *xlhdr = (XlMultiInsertXTuple *) data;

			data = ((char *) xlhdr) + SizeOfMultiInsertXTuple;
			len = xlhdr->datalen;
			if (len != 0 && AllocSizeIsValid((uint) len))
			{
				XHeapDiskTuple header;
				XHeapTupleData temp_tuple;
				HeapTuple	tuple;
				XHeapTupleData *xtuple = &temp_tuple;

				change->data.tp.newtuple = ReorderBufferGetTupleBuf(ctx->reorder, len + SizeOfXHeapDiskTupleData);
				/* borrow the space from tupleBuf (avoid realloc) */
				tuple = change->data.tp.newtuple;
				xtuple->disk_tuple = (XHeapDiskTupleData *) tuple->t_data;
				header = xtuple->disk_tuple;

				/* not a disk based tuple */
				ItemPointerSetInvalid(&xtuple->ctid);
				xtuple->table_oid = InvalidOid;
				xtuple->disk_tuple_size = len + SizeOfXHeapDiskTupleData;

				memset(header, 0, SizeOfXHeapDiskTupleData);
				memcpy((char *) xtuple->disk_tuple + SizeOfXHeapDiskTupleData, (char *) data, len);

				// update header by xlheader
				header->flag = xlhdr->flag;
				header->flag2 = xlhdr->flag2;
				header->t_hoff = xlhdr->t_hoff;

				tuple->t_len = xtuple->disk_tuple_size;
			}
			else
			{
				elog(WARNING, "tuplelen is invalid(%d), don't decode it", len);
				return;
			}
			data += len;
		}

		if ((xlrec->flags & XLOG_XHEAP_INSERT_LAST_IN_MULTI) && (i + 1) == xlrec->ntuples)
			change->data.tp.clear_toast_afterwards = true;
		else
			change->data.tp.clear_toast_afterwards = false;

		ReorderBufferQueueChange(ctx->reorder, xheap_xlog_get_current_xid(r), buf->origptr, change, false);
	}
}


static void
decode_xlog_xtuple(const char *data, Size len, HeapTuple tuple)
{
	int			datalen = 0;

	XHeapTupleData temp_tuple;
	XHeapDiskTuple header;
	xl_xheap_header xlhdr;

	XHeapTupleData *xtuple = &temp_tuple;

	/* borrow the space from tupleBuf (avoid realloc) */
	xtuple->disk_tuple = (XHeapDiskTupleData *) tuple->t_data;
	header = xtuple->disk_tuple;

	datalen = len - SizeOfXHeapHeader;
	Assert(datalen >= 0);

	/* we can only figure this out after reassembling the transactions */
	xtuple->table_oid = InvalidOid;
	/* not a disk based tuple */
	ItemPointerSetInvalid(&xtuple->ctid);

	/* data is not stored aligned, copy to aligned storage */
	memcpy((char *) &xlhdr, data, SizeOfXHeapHeader);
	memset(header, 0, SizeOfXHeapDiskTupleData);
	memcpy(((char *) xtuple->disk_tuple) + SizeOfXHeapDiskTupleData, data + SizeOfXHeapHeader,
		   datalen);

	// update header from xlhdr.
	header->flag = xlhdr.flag;
	header->flag2 = xlhdr.flag2;
	header->t_hoff = xlhdr.t_hoff;

	tuple->t_len = datalen + SizeOfXHeapDiskTupleData;
}