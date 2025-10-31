/* -------------------------------------------------------------------------
 *
 * undodesc.c
 *     rmgr descriptor routines for xstore/undo/undoxlog.c
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 *   src/undo/undodesc.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "undo/undoxlog.h"

const char *
undo_xlog_type_name(uint8 subtype)
{
	uint8 info = subtype & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_UNDO_UNLINK:
			return "undo_unlink";
			break;
		case XLOG_UNDO_EXTEND:
			return "undo_extend";
			break;
		case XLOG_UNDO_DISCARD:
			return "undo_slot_discard";
			break;
		case XLOG_UNDO_SLOT_EXTEND:
			return "undo_slot_extend";
			break;
		case XLOG_UNDO_SLOT_UNLINK:
			return "undo_slot_unlink";
			break;
		case XLOG_UNDO_ROLLBACK_FINISH:
			return "undo_rollback_finish";
			break;
		default:
			break;
	}
	return "unknown_type";
}

void
undo_xlog_desc(StringInfo buf, XLogReaderState *record)
{
	char *rec = XLogRecGetData(record);
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_UNDO_UNLINK:
			{
				xl_undolog_unlink *xlrec = (xl_undolog_unlink *) rec;
				appendStringInfo(buf,
								 "UNLINK_UNDO_LOG: pre head logno/offset=%d/%lu, head "
								 "logno/offset=%d/%lu.",
								 (int) (UNDO_PTR_GET_LOG_NO(xlrec->prevhead)),
								 UNDO_PTR_GET_OFFSET(xlrec->prevhead),
								 (int) (UNDO_PTR_GET_LOG_NO(xlrec->head)),
								 UNDO_PTR_GET_OFFSET(xlrec->head));
				break;
			}
		case XLOG_UNDO_EXTEND:
			{
				xl_undolog_extend *xlrec = (xl_undolog_extend *) rec;
				appendStringInfo(buf,
								 "EXTEND_UNDO_LOG: pre tail logno/offset=%d/%lu, tail "
								 "logno/offset=%d/%lu.",
								 (int) (UNDO_PTR_GET_LOG_NO(xlrec->prevtail)),
								 UNDO_PTR_GET_OFFSET(xlrec->prevtail),
								 (int) (UNDO_PTR_GET_LOG_NO(xlrec->tail)),
								 UNDO_PTR_GET_OFFSET(xlrec->tail));
				break;
			}
		case XLOG_UNDO_DISCARD:
			{
				xl_undolog_discard *xlrec = (xl_undolog_discard *) rec;
				appendStringInfo(buf,
								 "DISCARD_UNDO_LOG: logno %d recycle [%lu, %lu) discard %lu"
								 " loops %lu xid %lu xmin %lu.",
								 (int) UNDO_PTR_GET_LOG_NO(xlrec->startSlot),
								 xlrec->startSlot, xlrec->endSlot, xlrec->endUndoPtr,
								 xlrec->recycleLoops, xlrec->recycledXid.value, xlrec->globalFrozenXid.value);
				break;
			}
		case XLOG_UNDO_SLOT_EXTEND:
			{
				xl_undolog_extend *xlrec = (xl_undolog_extend *) rec;
				appendStringInfo(buf,
								 "EXTEND_UNDO_SLOT_LOG: pre tail logno/offset=%d/%lu, tail "
								 "logno/offset=%d/%lu.",
								 (int) (UNDO_PTR_GET_LOG_NO(xlrec->prevtail)),
								 UNDO_PTR_GET_OFFSET(xlrec->prevtail),
								 (int) (UNDO_PTR_GET_LOG_NO(xlrec->tail)),
								 UNDO_PTR_GET_OFFSET(xlrec->tail));
				break;
			}
		case XLOG_UNDO_SLOT_UNLINK:
			{
				xl_undolog_unlink *xlrec = (xl_undolog_unlink *) rec;
				appendStringInfo(buf,
								 "UNLINK_UNDO_SLOT_LOG: pre head logno/offset=%d/%lu, head "
								 "logno/offset=%d/%lu.",
								 (int) (UNDO_PTR_GET_LOG_NO(xlrec->prevhead)),
								 UNDO_PTR_GET_OFFSET(xlrec->prevhead),
								 (int) (UNDO_PTR_GET_LOG_NO(xlrec->head)),
								 UNDO_PTR_GET_OFFSET(xlrec->head));
				break;
			}
		case XLOG_UNDO_ROLLBACK_FINISH:
			{
				xl_undolog_rollback_finish *xlrec = (xl_undolog_rollback_finish *) rec;
				appendStringInfo(buf, "ROLLBACK FINISH: logno %d slot offset %lu.",
								(int) UNDO_PTR_GET_LOG_NO(xlrec->slotPtr),
								UNDO_PTR_GET_OFFSET(xlrec->slotPtr));
				break;
			}
		default:
			{
				appendStringInfo(buf, "UNKNOWN Log");
				break;
			}
	}
}