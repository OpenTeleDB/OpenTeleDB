/* -------------------------------------------------------------------------
 *
 * xheapfun.c
 *   Functions to investigate xheap pages.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * We check the input to these functions for corrupt pointers etc. that
 * might cause crashes, but at the same time we try to print out as much
 * information as possible, even if it's nonsense. That's because if a
 * page is corrupt, we don't know why and how exactly it is corrupt, so we
 * let the user judge it.
 *
 * These functions are restricted to superusers for the fear of introducing
 * security holes if the input checking isn't as water-tight as it should be.
 * You'd need to be superuser to obtain a raw page image anyway, so
 * there's hardly any use case for using these without superuser-rights
 * anyway.
 *
 * IDENTIFICATION
 * src/xheap/xheapfun.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/xlog_internal.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_control.h"
#include "catalog/pg_type.h"
#include "common/controldata_utils.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/varlena.h"
#include "xheap/xpage.h"
#include "xheap/xtuple.h"

static bytea *get_raw_page_internal(text *relname, ForkNumber forknum,
					  BlockNumber blkno);

                      
/*
 * get_raw_page
 *
 * Returns a copy of a page from shared buffers as a bytea
 */
PG_FUNCTION_INFO_V1(get_raw_page);

Datum
get_raw_page(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	uint32		blkno = PG_GETARG_UINT32(1);
	bytea	   *raw_page;

	/*
	 * We don't normally bother to check the number of arguments to a C
	 * function, but here it's needed for safety because early 8.4 beta
	 * releases mistakenly redefined get_raw_page() as taking three arguments.
	 */
	if (PG_NARGS() != 2)
		ereport(ERROR,
				(errmsg("wrong number of arguments to get_raw_page()"),
				 errhint("Run the updated xstore.sql script.")));

	raw_page = get_raw_page_internal(relname, MAIN_FORKNUM, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * get_raw_page_fork
 *
 * Same, for any fork
 */
PG_FUNCTION_INFO_V1(get_raw_page_fork);

Datum
get_raw_page_fork(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	text	   *forkname = PG_GETARG_TEXT_PP(1);
	uint32		blkno = PG_GETARG_UINT32(2);
	bytea	   *raw_page;
	ForkNumber	forknum;

	forknum = forkname_to_number(text_to_cstring(forkname));

	raw_page = get_raw_page_internal(relname, forknum, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * workhorse
 */
static bytea *
get_raw_page_internal(text *relname, ForkNumber forknum, BlockNumber blkno)
{
	bytea	   *raw_page;
	RangeVar   *relrv;
	Relation	rel;
	char	   *raw_page_data;
	Buffer		buf;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use raw functions"))));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	/* Check that this relation has storage */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from view \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from composite type \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from foreign table \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from partitioned table \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from partitioned index \"%s\"",
						RelationGetRelationName(rel))));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	if (blkno >= RelationGetNumberOfBlocksInFork(rel, forknum))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number %u is out of range for relation \"%s\"",
						blkno, RelationGetRelationName(rel))));

	/* Initialize buffer to copy to */
	raw_page = (bytea *) palloc(BLCKSZ + VARHDRSZ);
	SET_VARSIZE(raw_page, BLCKSZ + VARHDRSZ);
	raw_page_data = VARDATA(raw_page);

	/* Take a verbatim copy of the page */

	buf = ReadBufferExtended(rel, forknum, blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	memcpy(raw_page_data, BufferGetPage(buf), BLCKSZ);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	relation_close(rel, AccessShareLock);

	return raw_page;
}

/*
 * xpage_header
 *
 * Allows inspection of page header fields of a raw page
 */

PG_FUNCTION_INFO_V1(xheap_page_header);

Datum
xheap_page_header(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	int			raw_page_size;

	TupleDesc	tupdesc;

	Datum		result;
	HeapTuple	tuple;
	Datum		values[13];
	bool		nulls[13];

	XHeapPageHeader	page;
	XLogRecPtr	lsn;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use raw page functions"))));

	raw_page_size = VARSIZE(raw_page) - VARHDRSZ;

	/*
	 * Check that enough data was supplied, so that we don't try to access
	 * fields outside the supplied buffer.
	 */
	if (raw_page_size < SizeOfXHeapPageHeaderData)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input page too small (%d bytes)", raw_page_size)));

	page = (XHeapPageHeader) VARDATA(raw_page);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Extract information from the page header */

	lsn = XHeapPageGetLSN(page);

	
    values[0] = LSNGetDatum(lsn);
	values[1] = UInt16GetDatum(page->pd_checksum);
	values[2] = UInt16GetDatum(page->pd_flags);
	values[4] = UInt16GetDatum(page->pd_lower);
	values[5] = UInt16GetDatum(page->pd_upper);
	values[6] = UInt16GetDatum(page->pd_special);
	values[7] = UInt16GetDatum(XHeapPageGetPageSize(page));
	values[8] = UInt16GetDatum(XHeapPageGetPageLayoutVersion(page));
	values[11] = UInt16GetDatum(page->potential_freespace);
	values[12] = UInt64GetDatum(page->pd_prune_xid.value);

	/* Build and return the tuple. */

	memset(nulls, 0, sizeof(nulls));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * bits_to_text
 *
 * Converts a bits8-array of 'len' bits to a human-readable
 * c-string representation.
 */
static char *
bits_to_text(bits8 *bits, int len)
{
	int			i;
	char	   *str;

	str = palloc(len + 1);

	for (i = 0; i < len; i++)
		str[i] = (bits[(i / 8)] & (1 << (i % 8))) ? '1' : '0';

	str[i] = '\0';

	return str;
}


/*
 * xheap_page_items
 *
 * Allows inspection of line pointers and tuple headers of a heap page.
 */
PG_FUNCTION_INFO_V1(xheap_page_items);

typedef struct heap_page_items_state
{
	TupleDesc	tupd;
	Page		page;
	uint16		offset;
} heap_page_items_state;

Datum
xheap_page_items(PG_FUNCTION_ARGS)
{
	bytea *raw_page = PG_GETARG_BYTEA_P(0);
	heap_page_items_state *inter_call_data = NULL;
	FuncCallContext *fctx;
	int			raw_page_size;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use raw page functions"))));

	raw_page_size = VARSIZE(raw_page) - VARHDRSZ;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext mctx;

		if (raw_page_size < SizeOfXHeapPageHeaderData)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("input page too small (%d bytes)", raw_page_size)));

		fctx = SRF_FIRSTCALL_INIT();
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		inter_call_data = palloc(sizeof(heap_page_items_state));

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		inter_call_data->tupd = tupdesc;

		inter_call_data->offset = FirstOffsetNumber;
		inter_call_data->page = VARDATA(raw_page);

		fctx->max_calls = xheap_page_get_max_offset_number(inter_call_data->page);
		fctx->user_fctx = inter_call_data;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	inter_call_data = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		Page		page = inter_call_data->page;
		HeapTuple	resultTuple;
		Datum		result;
		RowPtr		*id;
		Datum		values[16];
		bool		nulls[16];
		uint16		lp_offset;
		uint16		lp_flags;
		uint16		lp_len;

		memset(nulls, 0, sizeof(nulls));

		/* Extract information from the line pointer */

		id = XPageGetRowPtr(page, inter_call_data->offset);
		lp_offset = RowPtrGetOffset(id);
		lp_len = RowPtrGetLen(id);
		lp_flags = RowPtrGetFlags(id);

		values[0] = UInt16GetDatum(inter_call_data->offset);
		values[1] = UInt16GetDatum(lp_offset);
		values[2] = UInt16GetDatum(lp_flags);
		values[3] = UInt16GetDatum(lp_len);

		/*
		 * We do just enough validity checking to make sure we don't reference
		 * data outside the page passed to us. The page could be corrupt in
		 * many other ways, but at least we won't crash.
		 */
		if (RowPtrHasStorage(id) &&
			lp_len >= MINXHeapTupleSize &&
			lp_offset + lp_len <= raw_page_size)
		{
			XHeapDiskTuple tuphdr;
			bytea	   *tuple_data_bytea;
			// char * tuple_data = NULL;
			int			tuple_data_len;
			UndoRecPtr undo_record_ptr = INVALID_UNDO_REC_PTR;


			/* Extract information from the tuple header */
			tuphdr = (XHeapDiskTuple) XPageGetRowData(page, id);
			undo_record_ptr = tuphdr->urec;
			values[4] = UInt64GetDatum(tuphdr->modified_xid.value);
			values[5] = UInt64GetDatum(tuphdr->locker_xid.value);
			values[6] = UInt64GetDatum(undo_record_ptr);
			values[7] = UInt16GetDatum(tuphdr->flag);
			values[8] = UInt16GetDatum(tuphdr->flag2);
			values[9] = UInt8GetDatum(tuphdr->t_hoff);
			
			/* Copy raw tuple data into bytea attribute */
			tuple_data_len = lp_len - tuphdr->t_hoff;
			tuple_data_bytea = (bytea *) palloc(tuple_data_len + VARHDRSZ);
			SET_VARSIZE(tuple_data_bytea, tuple_data_len + VARHDRSZ);
			memcpy(VARDATA(tuple_data_bytea), (char *) tuphdr + tuphdr->t_hoff,
				   tuple_data_len);
			values[15] = PointerGetDatum(tuple_data_bytea);

			/*
			 * We already checked that the item is completely within the raw
			 * page passed to us, with the length given in the line pointer.
			 * Let's check that t_hoff doesn't point over lp_len, before using
			 * it to access t_bits and oid.
			 */
			 
			if (tuphdr->t_hoff >= SizeOfXHeapDiskTupleData &&
				tuphdr->t_hoff <= lp_len)
			{
				if (tuphdr->flag & XHEAP_HAS_NULL)
				{
					int			bits_len;

					bits_len =
						BITMAPLEN(XHeapTupleHeaderGetNatts(tuphdr)) * BITS_PER_BYTE;
					values[11] = CStringGetTextDatum(bits_to_text(tuphdr->data, bits_len));
				}
				else
					nulls[11] = true;
			}
			else
				nulls[11] = true;

			//fetch undo info
			values[12] = UInt32GetDatum(UNDO_PTR_GET_LOG_NO(undo_record_ptr));
			values[13] = UInt32GetDatum(UNDO_PTR_GET_BLOCK_NUM(undo_record_ptr));
			values[14] = UInt16GetDatum(UNDO_PTR_GET_PAGE_OFFSET(undo_record_ptr));
	
		}
		else
		{
			/*
			 * The line pointer is not used, or it's invalid. Set the rest of
			 * the fields to NULL
			 */
			int			i;

			for (i = 4; i <= 15; i++)
				nulls[i] = true;
		}

		/* Build and return the result tuple. */
		resultTuple = heap_form_tuple(inter_call_data->tupd, values, nulls);
		result = HeapTupleGetDatum(resultTuple);

		inter_call_data->offset++;

		SRF_RETURN_NEXT(fctx, result);
	}
	else
		SRF_RETURN_DONE(fctx);
}
