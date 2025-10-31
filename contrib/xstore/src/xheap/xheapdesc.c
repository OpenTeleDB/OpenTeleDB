/* -------------------------------------------------------------------------
 *
 * xheapdesc.c
 *     rmgr descriptor routines for xredo.c
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California	
 *
 *
 * IDENTIFICATION
 * src/xheap/xheapdesc.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "storage/off.h"
#include "xheap/xtuple.h"
#include "xheap/xredo.h"
#include "undo/undoxlog.h"
#include "undo/undotype.h"

char *
parse_undo_header(xl_undo_header *xlundohdr, Oid *partition_oid, UndoRecPtr *blkprev,
			  UndoRecPtr *prev_urp, FullTransactionId *full_xid, uint32 *toast_len)
{
	char *currLogPtr = NULL;

	currLogPtr = ((char *) xlundohdr + SizeOfXLUndoHeader);

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
	{
		*blkprev = *(UndoRecPtr *) ((char *) currLogPtr);
		currLogPtr += sizeof(UndoRecPtr);
	}

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
	{
		*prev_urp = *(UndoRecPtr *) ((char *) currLogPtr);
		currLogPtr += sizeof(UndoRecPtr);
	}

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
	{
		*full_xid = *(FullTransactionId *) ((char *) currLogPtr);
		currLogPtr += sizeof(FullTransactionId);
	}

	if ((xlundohdr->flag & XLOG_UNDO_HEADER_HAS_TOAST) != 0)
	{
		*toast_len = *(uint32 *) ((char *) currLogPtr);
		currLogPtr += sizeof(uint32) + *toast_len;
	}
	return currLogPtr;
}

const char *
xheap_type_name(uint8 subtype)
{
	uint8 info = subtype & ~XLR_INFO_MASK;
	info &= XLOG_XHEAP_OPMASK;
	switch (info)
	{
		case XLOG_XHEAP_INSERT:
			return "xheap_insert";
			break;
		case XLOG_XHEAP_DELETE:
			return "xheap_delete";
			break;
		case XLOG_XHEAP_UPDATE:
			return "xheap_update";
			break;
		case XLOG_XHEAP_CLEAN:
			return "xheap_clean";
			break;
		case XLOG_XHEAP_MULTI_INSERT:
			return "xheap_multi_insert";
			break;
		case XLOG_XHEAP_LOCK:
			return "xheap_lock";
			break;
		default:
			return "unknown_type";
			break;
	}
}


void
xheap_desc(StringInfo buf, XLogReaderState *record)
{
	char		 *rec = XLogRecGetData(record);
	uint8		  info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	TransactionId xid = XLogRecGetXid(record);
	FullTransactionId full_xid = InvalidFullTransactionId;
	Oid			  partition_oid = InvalidOid;
	UndoRecPtr	  blkprev = INVALID_UNDO_REC_PTR;
	UndoRecPtr	  prev_urp = INVALID_UNDO_REC_PTR;
	uint32		  toast_len = 0;
	char		 *curr_log_ptr;
	xl_undo_meta  undometa;

	if (XLogRecHasBlockImage(record, 0))
		return;

	info &= XLOG_XHEAP_OPMASK;
	init_xlog_undo_meta(&undometa);

	switch (info)
	{
		case XLOG_XHEAP_INSERT:
		{
			Size		   blk_data_len = 0;
			xl_undo_header  *xlundohdr = NULL;
			xl_undo_meta  *xlundometa = NULL;
			xl_xheap_insert *xlrec = (xl_xheap_insert *) rec;
			xl_xheap_header *xheap_header =
				(xl_xheap_header *) XLogRecGetBlockData(record, 0, &blk_data_len);
			bool isInit = (XLogRecGetInfo(record) & XLOG_XHEAP_INIT_PAGE) != 0;
			if (isInit)
				appendStringInfo(buf, "XLOG_XHEAP_INSERT insert(init): ");
			else
				appendStringInfo(buf, "XLOG_XHEAP_INSERT insert: ");

			appendStringInfo(
				buf, "TupHeader: flag %d, flag2 %d, t_hoff %d ",
				 xheap_header->flag,
				xheap_header->flag2, xheap_header->t_hoff);
			appendStringInfo(buf, "TupInfo: ");
			appendStringInfo(buf, "tupoffset %u, flag %u. ", (uint16) xlrec->offnum,
							 (uint8) xlrec->flags);
			xlundohdr =
				(xl_undo_header
					 *) ((char *) rec +
						 SizeOfXHeapInsert);
			curr_log_ptr = parse_undo_header(xlundohdr, &partition_oid, &blkprev, &prev_urp,
									   &full_xid, &toast_len);
			appendStringInfo(buf, "UndoInfo: ");
			appendStringInfo(buf,
							 "urecptr %lu, blkprev %lu, txnprevurp %lu, relOid %u, "
							 "partitionOid %u, flag %u, fullXid %lu, "
							 "toastLen %u. ",
							 xlundohdr->urecptr, blkprev, prev_urp, xlundohdr->relOid,
							 partition_oid, xlundohdr->flag, full_xid.value, toast_len);
			xlundometa = (xl_undo_meta *) ((char *) curr_log_ptr);
			copy_xlog_undo_meta(xlundometa, &undometa);

			appendStringInfo(buf, "UndoMetaInfo: ");
			appendStringInfo(buf,
							 "undolog %d slot offset %lu: dbid %u, xid %u, lastrecsize %u, "
							 "allocate %d init slot buffer %d switch undolog %d.",
							 (int) UNDO_PTR_GET_LOG_NO(xlundohdr->urecptr),
							 xlog_undo_meta_slotptr(xlundometa), xlundometa->dbid, xid,
							 xlundometa->lastRecordSize,
							 xlog_undo_meta_is_translot(xlundometa),
							 xlog_undo_meta_is_intslot(xlundometa),
							 xlog_undo_meta_is_switch(xlundometa));
			break;
		}
		case XLOG_XHEAP_MULTI_INSERT:
		{
			UndoRecPtr		   *last_urecptr = NULL;
			xl_undo_meta	   *xlundometa = NULL;
			bool				isinit = false;
			int					nranges = 0;
			XlXHeapMultiInsert *xlrec = NULL;
			xl_undo_header	   *xlundohdr = (xl_undo_header *) rec;
			char			   *curxlogptr = (char *) xlundohdr + SizeOfXLUndoHeader;
			curxlogptr = parse_undo_header(xlundohdr, &partition_oid, &blkprev, &prev_urp,
									   &full_xid, &toast_len);

			last_urecptr = (UndoRecPtr *) curxlogptr;
			curxlogptr = (char *) last_urecptr + sizeof(*last_urecptr);

			xlundometa = (xl_undo_meta *) curxlogptr;
			curxlogptr = (char *) xlundometa + xlog_undo_meta_size(xlundometa);
			copy_xlog_undo_meta(xlundometa, &undometa);

			isinit = (XLogRecGetInfo(record) & XLOG_XHEAP_INIT_PAGE) != 0;

			xlrec = (XlXHeapMultiInsert *) ((char *) curxlogptr);
			curxlogptr = (char *) xlrec + SizeOfXHeapMultiInsert;
			nranges = *(int *) curxlogptr;

			if (isinit)
				appendStringInfo(buf, "XLOG_XHEAP_MULTI_INSERT (init): ");
			else
				appendStringInfo(buf, "XLOG_XHEAP_MULTI_INSERT : ");

			appendStringInfo(buf, "TupInfo: ");
			appendStringInfo(buf, "ntuples %u, flag %u, nranges %d. ",
							 (int) xlrec->ntuples, (uint8) xlrec->flags, nranges);
			appendStringInfo(buf, "UndoInfo: ");
			appendStringInfo(
				buf,
				"urecptr %lu, blkprev %lu, txnprevurp %lu, last_urecptr %lu, fullXid %lu, "
				"toastLen %u. ",
				xlundohdr->urecptr, blkprev, prev_urp, *last_urecptr, full_xid.value, toast_len);
			appendStringInfo(buf, "relOid %u, partitionOid %u, flag %u. ",
							 xlundohdr->relOid, partition_oid, xlundohdr->flag);

			appendStringInfo(buf, "UndoMetaInfo: ");
			appendStringInfo(buf,
							 "undolog %d slot offset %lu: dbid %u, xid %u, lastrecsize %u, "
							 "allocate %d init slot buffer %d switch undolog %d.",
							 (int) UNDO_PTR_GET_LOG_NO(xlundohdr->urecptr),
							 xlog_undo_meta_slotptr(xlundometa), xlundometa->dbid, xid,
							 xlundometa->lastRecordSize,
							 xlog_undo_meta_is_translot(xlundometa),
							 xlog_undo_meta_is_intslot(xlundometa),
							 xlog_undo_meta_is_switch(xlundometa));
			break;
		}
		case XLOG_XHEAP_DELETE:
		{
			xl_undo_header  *xlundohdr = NULL;
			xl_undo_meta  *xlundometa = NULL;
			xl_xheap_delete *xlrec = (xl_xheap_delete *) rec;
			appendStringInfo(buf, "XLOG_XHEAP_DELETE: ");
			appendStringInfo(buf, "TupInfo: ");
			appendStringInfo(buf, "oldxid %lu, tupoffset %u,  flag %u. ",
							 xlrec->oldxid.value, (uint16) xlrec->offnum, 
							 (uint8) xlrec->flag);

			xlundohdr =
				(xl_undo_header
					 *) ((char *) rec +
						 SizeOfXHeapDelete);
			curr_log_ptr = parse_undo_header(xlundohdr, &partition_oid, &blkprev, &prev_urp,
									   &full_xid, &toast_len);
			appendStringInfo(buf, "UndoInfo: ");
			appendStringInfo(buf,
							 "urecptr %lu, blkprev %lu, txnprevurp %lu, relOid %u, "
							 "partitionOid %u, fullXid %lu, "
							 "toastLen %u. ",
							 xlundohdr->urecptr, blkprev, prev_urp, xlundohdr->relOid,
							 partition_oid, full_xid.value, toast_len);
			xlundometa = (xl_undo_meta *) curr_log_ptr;
			copy_xlog_undo_meta(xlundometa, &undometa);
			appendStringInfo(buf, "UndoMetaInfo: ");
			appendStringInfo(buf,
							 "undolog %d slot offset %lu: dbid %u, xid %u, lastrecsize %u, "
							 "allocate %d init slot buffer %d switch undolog %d.",
							 (int) UNDO_PTR_GET_LOG_NO(xlundohdr->urecptr),
							 xlog_undo_meta_slotptr(xlundometa), xlundometa->dbid, xid,
							 xlundometa->lastRecordSize,
							 xlog_undo_meta_is_translot(xlundometa),
							 xlog_undo_meta_is_intslot(xlundometa),
							 xlog_undo_meta_is_switch(xlundometa));
			break;
		}
		case XLOG_XHEAP_UPDATE:
		{
			Size		   datalen;
			Size		   len;
			xl_xheap_header  xlhdr;
			char		  *recdata = XLogRecGetBlockData(record, 0, &datalen);
			char		  *recdata_end = recdata + datalen;
			xl_undo_header  *xlundohdr = NULL;
			xl_undo_meta  *xlundometa = NULL;
			xl_xheap_update *xlrec = (xl_xheap_update *) rec;
			appendStringInfo(buf, "XLOG_XHEAP_UPDATE: ");
			appendStringInfo(buf, "TupInfo: ");
			appendStringInfo(buf,
							 "oldxid %lu, old tupoffset %u, new tupoffset %u, "
							 "  old_tuple_flag %u. ",
							 xlrec->oldxid.value, (uint16) xlrec->old_offnum,
							 (uint16) xlrec->new_offnum, 
							 (uint16) xlrec->old_tuple_flag);
			xlundohdr =
				(xl_undo_header
					 *) ((char *) rec +
						 SizeOfXHeapUpdate);
			curr_log_ptr = parse_undo_header(xlundohdr, &partition_oid, &blkprev, &prev_urp,
									   &full_xid, &toast_len);
			appendStringInfo(buf, "UndoInfo(oldpage): ");
			appendStringInfo(buf,
							 "urecptr %lu, blkprev %lu, txnprevurp %lu, relOid %u, "
							 "partitionOid %u, flag %u, fullXid %lu, "
							 "toastLen %u. ",
							 xlundohdr->urecptr, blkprev, prev_urp, xlundohdr->relOid,
							 partition_oid, xlundohdr->flag, full_xid.value, toast_len);

			if (xlrec->flags & XLZ_NON_INPLACE_UPDATE)
			{
				appendStringInfo(buf, "NON_INPLACE_UPDATE. ");
				appendStringInfo(buf, "UndoInfo(newpage): ");
				xlundohdr = (xl_undo_header *) ((char *) curr_log_ptr);
				curr_log_ptr = parse_undo_header(xlundohdr, &partition_oid, &blkprev, &prev_urp,
										   &full_xid, &toast_len);
				appendStringInfo(buf,
								 "relOid %u, urecptr %lu, blkprev %lu, txnprevurp %lu, "
								 "newflag %u, fullXid %lu, "
								 "toastLen %u. ",
								 xlundohdr->relOid, xlundohdr->urecptr, blkprev, prev_urp,
								 xlundohdr->flag, full_xid.value, toast_len);
			}
			else
			{
				appendStringInfo(buf, "INPLACE_UPDATE. ");
			}

			xlundometa = (xl_undo_meta *) curr_log_ptr;
			copy_xlog_undo_meta(xlundometa, &undometa);
			appendStringInfo(buf, "UndoMetaInfo: ");
			appendStringInfo(buf,
							 "undolog %d slot offset %lu: dbid %u, xid %u, lastrecsize %u, "
							 "allocate %d init slot buffer %d switch undolog %d.",
							 (int) UNDO_PTR_GET_LOG_NO(xlundohdr->urecptr),
							 xlog_undo_meta_slotptr(xlundometa), xlundometa->dbid, xid,
							 xlundometa->lastRecordSize,
							 xlog_undo_meta_is_translot(xlundometa),
							 xlog_undo_meta_is_intslot(xlundometa),
							 xlog_undo_meta_is_switch(xlundometa));
			curr_log_ptr = curr_log_ptr + xlog_undo_meta_size(xlundometa);

			if (!(xlrec->flags & XLZ_NON_INPLACE_UPDATE))
			{
				uint8 *flags_ptr = NULL;
				uint8  flags = 0;
				uint8 *t_hoff_ptr = NULL;
				uint8  t_hoff = 0;
				char  *xor_cur_xlog_ptr = NULL;
				int	  *undo_xor_delta_size_ptr = (int *) curr_log_ptr;
				int	   undo_xor_delta_size = *undo_xor_delta_size_ptr;
				curr_log_ptr += sizeof(int);
				xor_cur_xlog_ptr = curr_log_ptr;
				curr_log_ptr += undo_xor_delta_size;
				t_hoff_ptr = (uint8 *) xor_cur_xlog_ptr;
				t_hoff = *t_hoff_ptr;

				xor_cur_xlog_ptr += sizeof(uint8) + t_hoff - OffsetDataHeader;

				flags_ptr = (uint8 *) xor_cur_xlog_ptr;
				flags = *flags_ptr;
				xor_cur_xlog_ptr += sizeof(uint8);

				if (flags & UREC_XOR_PREFIX)
				{
					uint16 *prefix_len_ptr = (uint16 *) (xor_cur_xlog_ptr);
					xor_cur_xlog_ptr += sizeof(uint16);
					appendStringInfo(buf, "prefixlen %u ", *prefix_len_ptr);
				}
				if (flags & UREC_XOR_SUFFIX)
				{
					uint16 *suffix_len_ptr = (uint16 *) (xor_cur_xlog_ptr);
					xor_cur_xlog_ptr += sizeof(uint16);
					appendStringInfo(buf, "suffixlen %u ", *suffix_len_ptr);
				}
			}
			else
			{
				if (xlrec->flags & XLZ_UPDATE_PREFIX_FROM_OLD)
				{
					uint16 prefixlen = 0;
					memcpy(&prefixlen, recdata, sizeof(uint16));
					recdata += sizeof(uint16);
					appendStringInfo(buf, "prefixlen %u ", prefixlen);
				}

				if (xlrec->flags & XLZ_UPDATE_SUFFIX_FROM_OLD)
				{
					uint16 suffixlen = 0;
					memcpy(&suffixlen, recdata, sizeof(uint16));
					recdata += sizeof(uint16);
					appendStringInfo(buf, "suffixlen %u ", suffixlen);
				}
			}

			memcpy((char *) &xlhdr, recdata, SizeOfXHeapHeader);
			recdata += SizeOfXHeapHeader;

			len = (recdata_end - recdata) - (xlhdr.t_hoff - SizeOfXHeapDiskTupleData);
			appendStringInfo(buf, "difflen %lu tHoff %u.", len,
							 xlhdr.t_hoff);
			break;
		}
		case XLOG_XHEAP_CLEAN:
		{
			xl_xheap_clean *xlrec = (xl_xheap_clean *) rec;
			Size		  datalen;
			int			  nunused = xlrec->nunused;
			uint16		  nfixed = xlrec->nfixed;
			OffsetNumber *nowunused = (OffsetNumber *) XLogRecGetBlockData(record, 0, &datalen);
			OffsetNumber *nowfixed = (OffsetNumber *) nowunused + nunused;
			OffsetNumber *fixedlen;
			OffsetNumber *target_off_num;
			OffsetNumber  tmp_target_off;
			Size		 *space_required;
			Size		  tmp_spc_rqd;
			int			  i = 0;

			appendStringInfo(buf, "XLOG_XHEAP_CLEAN: ");
			appendStringInfo(buf, "remxid %lu. ", xlrec->latest_removed_xid.value);

			if (xlrec->flags & XLZ_CLEAN_CONTAINS_OFFSET)
			{
				target_off_num = (OffsetNumber *) ((char *) xlrec + SizeOfXHeapClean);
				space_required = (Size *) ((char *) target_off_num + sizeof(OffsetNumber));
			}
			else
			{
				target_off_num = &tmp_target_off;
				*target_off_num = InvalidOffsetNumber;
				space_required = &tmp_spc_rqd;
				*space_required = 0;
			}

			if (xlrec->flags & XLZ_CLEAN_CONTAINS_TUPLEN)
			{
				fixedlen = nowfixed + nfixed;
				appendStringInfo(buf, "XLZ_CLEAN_CONTAINS_TUPLEN. nfixed: %u. ", nfixed);
			}
			else
			{
				nfixed = 0;
				nowfixed = NULL;
				fixedlen = NULL;
			}

			appendStringInfo(buf, "nunused: %d, flags: %d. ",
							 nunused, xlrec->flags);

			if (nunused > 0)
			{
				i = 0;
				appendStringInfo(buf, " unused: [");
				while (i < nunused)
				{
					appendStringInfo(buf, " %d ", nowunused[i]);
					i++;
				}
				appendStringInfo(buf, "]");
			}

			if (nfixed > 0)
			{
				i = 0;
				appendStringInfo(buf, " fixed: [");
				while (i < nfixed)
				{
					appendStringInfo(buf, " %d ", nowfixed[i]);
					i++;
				}
				appendStringInfo(buf, "]");
				i = 0;
				appendStringInfo(buf, " fixlen: [");
				while (i < nfixed)
				{
					appendStringInfo(buf, " %d ", fixedlen[i]);
					i++;
				}
				appendStringInfo(buf, "]");
			}
			break;
		}
		case XLOG_XHEAP_LOCK:
		{
			XlXHeapLock *xlrec = (XlXHeapLock *) rec;

			appendStringInfo(buf, "XLOG_XHEAP_LOCK: ");
			appendStringInfo(buf, "off %u: xid %lu: infomask 0x%02X ",
						 xlrec->offnum, xlrec->locker_xid.value, xlrec->infomask);
			break;
		}
		default:
			appendStringInfo(buf, "UNKNOWN");
	}
}

const char *
xheap_undo_type_name(uint8 subtype)
{
	uint8 info = subtype & ~XLR_INFO_MASK;
	if (info == XLOG_XHEAPUNDO_PAGE)
	{
		return "xheap_undo_page";
	}
	else if (info == XLOG_XHEAPUNDO_ABORT_SPECINSERT)
	{
		return "xheap_undo_abort";
	}
	else
	{
		return "unknown_type";
	}
}


void
xheap_undo_desc(StringInfo buf, XLogReaderState *record)
{
	char *rec = XLogRecGetData(record);
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	if (info == XLOG_XHEAPUNDO_PAGE)
	{
		char  *curxlogptr = NULL;
		uint8 *flags = (uint8 *) rec;
		appendStringInfo(buf, "is_page_initialized: %c ",
						 (*flags & XLU_INIT_PAGE) ? 'T' : 'F');
		curxlogptr = (char *) ((char *) flags + sizeof(uint8));
		if (*flags & XLU_INIT_PAGE)
		{

		}
		else
		{

			OffsetNumber  *xlog_max_lp_offset = NULL;
			OffsetNumber  *xlog_min_lp_offset = (OffsetNumber *) curxlogptr;
			curxlogptr += sizeof(OffsetNumber);
			xlog_max_lp_offset = (OffsetNumber *) curxlogptr;
			curxlogptr += sizeof(OffsetNumber);

			appendStringInfo(buf, "xlogMinLPOffset: %d, ", *xlog_min_lp_offset);
			appendStringInfo(buf, "xlogMaxLPOffset: %d, ", *xlog_max_lp_offset);

			if (*xlog_max_lp_offset >= *xlog_min_lp_offset)
			{
				Offset		  *xlog_copy_start_offset = NULL;
				Offset		  *xlog_copy_end_offset = NULL;
				uint16		  *pd_flags = NULL;
				uint16		  *potential_freespace = NULL;
				FullTransactionId *pd_pruneXid = NULL;
				size_t		   lp_size =
					(*xlog_max_lp_offset - *xlog_min_lp_offset + 1) * sizeof(RowPtr);
				curxlogptr += lp_size;

				xlog_copy_start_offset = (Offset *) curxlogptr;
				curxlogptr += sizeof(Offset);
				xlog_copy_end_offset = (Offset *) curxlogptr;
				curxlogptr += sizeof(Offset);

				appendStringInfo(buf, "xlogCopyStartOffset: %d, ", *xlog_copy_start_offset);
				appendStringInfo(buf, "xlogCopyEndOffset: %d, ", *xlog_copy_end_offset);

				if (*xlog_copy_end_offset > *xlog_copy_start_offset)
				{
					size_t dataSize = *xlog_copy_end_offset - *xlog_copy_start_offset;
					curxlogptr += dataSize;
				}

				pd_pruneXid = (FullTransactionId *) curxlogptr;
				curxlogptr += sizeof(FullTransactionId);
				pd_flags = (uint16 *) curxlogptr;
				curxlogptr += sizeof(uint16);
				potential_freespace = (uint16 *) curxlogptr;
				curxlogptr += sizeof(uint16);

				appendStringInfo(buf, "pdPruneXid: %lu, ", pd_pruneXid->value);
				appendStringInfo(buf, "pdFlags: %d, ", *pd_flags);
				appendStringInfo(buf, "potentialFreespace: %d, ", *potential_freespace);
			}

		}
	}
	else if (info == XLOG_XHEAPUNDO_ABORT_SPECINSERT)
	{
		uint8					   *flags = (uint8 *) XLogRecGetData(record);
		XlXHeapUndoAbortSpecInsert *xlrec =
			(XlXHeapUndoAbortSpecInsert *) ((char *) flags + sizeof(uint8));
		appendStringInfo(buf, "offset %d ", xlrec->offset);
		appendStringInfo(buf, "flags %d ", *flags);
		if (*flags & XLU_ABORT_SPECINSERT_REL_HAS_INDEX)
		{
			appendStringInfo(buf, "hasIndex true");
		}
	}
	else
	{
		appendStringInfo(buf, "UNKNOWN");
	}
}
