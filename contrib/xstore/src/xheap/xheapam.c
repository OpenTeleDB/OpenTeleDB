/* -------------------------------------------------------------------------
 *
 * xheap.c
 * Implement the access interfaces of xstore inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xheap.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "c.h"
#include "catalog/catalog.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "undo/undotype.h"
#include "pgstat.h"
#include "nodes/pg_list.h"
#include "undo/undobuffer.h"
#include "utils/datum.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "storage/procarray.h"
#include "storage/predicate.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/freespace.h"
#include "storage/itemptr.h"
#include "storage/buf_internals.h"
#include "storage/bufpage.h"
#include "storage/bulk_write.h"
#include "access/detoast.h"
#include "access/rewriteheap.h"
#include "access/sysattr.h"
#include "access/rmgr.h"
#include "access/xloginsert.h"
#include "access/xlog.h"
#include "xheap/xredo.h"
#include "xheap/xheapundo.h"
#include "xheap/xtuptoaster.h"
#include "access/xact.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/detoast.h"
#include "xheap/xheap.h"
#include "xheap/xtuple.h"
#include "xheap/xhio.h"
#include "xheap/xpage.h"
#include "undo/undorequest.h"
#include "xheap/xheapam_visibility.h"
#include "xheap/xrel.h"
#include "util/xxact.h"
#include "xstore.h"
#include "undo/undolog.h"
#include "undo/undorecord.h"
#include "undo/undofetch.h"
#include "undo/undotxn.h"
#include "xheap/xlock.h"
#include "xheap/xtup_details.h"
#include "xheap/xtupleslot.h"
#include "undo/undorequest.h"
#include "nodes/execnodes.h"
#include "xheap/xtuple.h"
#include "access/tableam.h"
#include "utils/syscache.h"
#include "xheap/xtupleslot.h"
#include "xheap/xmulti.h"
#include "util/xrmgr.h"

#include <stdlib.h>

static Bitmapset *xheap_determing_modified_columns(Relation   relation,
												Bitmapset *interesting_cols,
												XHeapTuple oldtup, XHeapTuple newtup);
static void populate_xl_xheap_header(xl_xheap_header *xlhdr, const XHeapDiskTuple disk_tuple);
static void xheap_fill_header(xl_xheap_header *xlhdr, XHeapDiskTuple disk_tup);

static void xheap_keep_subtxn_lock(XHeapDiskTuple tuple, FullTransactionId subxid);

/*
 * xheap_determing_modified_columns - Check which columns are being updated.
 * This is same as HeapDetermineModifiedColumns except that it takes
 * XHeapTuple as input.
 */
static Bitmapset *
xheap_determing_modified_columns(Relation relation, Bitmapset *interesting_cols,
							  XHeapTuple oldtup, XHeapTuple newtup)
{
	return xheap_tuple_attr_equals(RelationGetDescr(relation), interesting_cols, oldtup,
								   newtup);
}


Datum
xheap_fast_get_attr(XHeapTuple tup, int attnum, TupleDesc tuple_desc, bool *isnull)
{

	char *tp = (char *) (tup)->disk_tuple + (tup)->disk_tuple->t_hoff;
	char *dp = ((tuple_desc)->attrs[0].attlen >= 0)
				   ? tp
				   : (char *) att_align_pointer(
						 tp, (tuple_desc)->attrs[(attnum) -1].attalign, -1, tp);
	if (attnum > 0)
	{
		*isnull = false;
		if (XHeapDiskTupNoNulls(tup->disk_tuple))
		{
			/* we need align this type */
			bool no_use_cache = (attnum == 1) &&
							  (TupleDescAttr(tuple_desc, 0)->attlen >= 0) &&
							  !(TupleDescAttr(tuple_desc, 0)->attbyval) &&
							  (TupleDescAttr(tuple_desc, 0)->attcacheoff == 0);
			return TupleDescAttr(tuple_desc, attnum - 1)->attcacheoff >= 0 && !no_use_cache
					   ? (fetchatt(
							 TupleDescAttr(tuple_desc, attnum - 1),
							 (dp + TupleDescAttr(tuple_desc, attnum - 1)->attcacheoff)))
					   : (xheap_no_cache_get_attr((tup), (attnum), (tuple_desc)));
		}
		else
			return att_isnull((attnum) -1, (tup)->disk_tuple->data)
					   ? ((*(isnull) = true), (Datum) NULL)
					   : (xheap_no_cache_get_attr((tup), (attnum), (tuple_desc)));
	}
	else
		return (Datum) NULL;
}

void
xheap_page_prune_fsm(Relation relation, Buffer buffer, FullTransactionId fxid, Page page,
				  BlockNumber blkno)
{
	bool has_pruned = xheap_page_prune_opt_page(relation, buffer, fxid, false);

	if (has_pruned)
	{
		Size   freespace = page_get_xheap_free_space(page);
		double thres =
			RelationGetTargetPageFreeSpacePrune(relation, XHEAP_DEFAULT_FILLFACTOR);
		double prob = FSM_UPDATE_HEURISTI_PROBABILITY * freespace / thres;
		RecordPageWithFreeSpace(relation, blkno, freespace);
		if (rand() % 100 >= 100.0 - prob * 100.0)
		{
			FreeSpaceMapVacuumRange(relation, blkno, blkno + 1);
		}
	}
}

static FullTransactionId
xheap_tuple_set_modified_xid(XHeapTuple xtuple, FullTransactionId xid)
{
	xtuple->disk_tuple->modified_xid = xid;
	return xid;
}

Oid
xheap_insert(Relation rel, XHeapTupleData *tup, CommandId cid, 
			 int options, BulkInsertState bistate, bool isToast)
{
	FullTransactionId    fxid = InvalidFullTransactionId;
	XHeapTuple			 xheaptup;
	Buffer				 buffer = InvalidBuffer;
	Page				 page;
	UndoRecPtr			 prev_urecptr = INVALID_UNDO_REC_PTR,
						 urec_ptr = INVALID_UNDO_REC_PTR;
	xl_undo_meta		 undometa;
	XHeapPageHeaderData *phdr = NULL;
	BlockNumber			 blkno = 0;

	UndoPersistence		 persistence;
	Oid					 relOid;
	UnpackedUndoRecord	 *undorec;
	UndoRecPtr			 old_prev_urp;

	init_xlog_undo_meta(&undometa);

	if (tup == NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("The insert tuple is NULL")));

	if (undo_cache_ctx->undo_prepare_buffers)
		reset_undo_prepare_buffers(undo_cache_ctx->undo_prepare_buffers, false);

	fxid = GetTopFullTransactionId();

	/* Prepare the tuple for insertion */
	xheaptup = xheap_prepare_insert(rel, tup, 0);
	tup->xmin = XidFromFullTransactionId(fxid);

	/* Get buffer page from buffer pool */
	buffer = relation_get_buffer_for_xtuple(rel, xheaptup->disk_tuple_size, InvalidBuffer,
										options, bistate);

	Assert(buffer != InvalidBuffer);

	page = BufferGetPage(buffer);
	Assert(page);
	phdr = (XHeapPageHeaderData *) page;
	ereport(DEBUG5, (errmsg("xheap ins1: xheap rel: %s, buf: %d, space: %d, tuplen: %d",
							RelationGetRelationName(rel), buffer,
							phdr->pd_upper - phdr->pd_lower, xheaptup->disk_tuple_size)));

	blkno = BufferGetBlockNumber(buffer);
	xheap_page_prune_fsm(rel, buffer, fxid, page, blkno);

	/* Prepare Undo record before buffer lock since undo record length is fixed */
	persistence = UndoPersistenceForRelation(rel);
	relOid = RelationGetRelid(rel);

	urec_ptr = xheap_prepare_undo_insert(
		relOid, rel->rd_rel->relfilenode, rel->rd_locator.spcOid, persistence, fxid, cid,
		prev_urecptr, INVALID_UNDO_REC_PTR, 
		BufferGetBlockNumber(buffer), NULL, NULL, &undometa);

#ifdef USE_ASSERT_CHECKING
	check_tuple_validity(rel, xheaptup);
#endif

	/* No ereport(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	XHeapTupleHeaderSetUndoRecPtr(xheaptup->disk_tuple, urec_ptr);
	xheap_tuple_set_modified_xid(xheaptup, fxid);

	/* Put xtuple into buffer page */
	relation_put_xtuple(rel, buffer, xheaptup);

	xheap_record_potential_free_space(buffer, -1 * SHORTALIGN(xheaptup->disk_tuple_size));

	/* Update the UnpackedUndoRecord now that we know where the tuple is located on the Page */
	undorec = prepare_buffers_get_undorecord((undo_cache_ctx->undo_prepare_buffers), 0);
	Assert(GetUndoRecordBlkno(undorec) == ItemPointerGetBlockNumber(&(xheaptup->ctid)));
	SetUndoRecordOffset(undorec, ItemPointerGetOffsetNumber(&(xheaptup->ctid)));

	/* Insert the Undo record into the undo store */
	insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, 0);

	old_prev_urp = get_current_tansaction_undorec_ptr(persistence);
	set_current_tansaction_undorec_ptr(urec_ptr, persistence);

	MarkBufferDirty(buffer);

	update_undolog_meta(fxid, prepare_buffers_get_first_undoptr(undo_cache_ctx->undo_prepare_buffers),
					&undometa, persistence, prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
					prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));

	/* Generate xlog, insert xlog and set page lsn */
	if (RelationNeedsWAL(rel))
	{
		uint8		  xl_undo_header_flag = 0;
		FullTransactionId current_xid = InvalidFullTransactionId;
		xl_undo_header  xlundohdr;
		xl_xheap_insert xlrec;
		xl_xheap_header xlhdr;
		XLogRecPtr	  recptr;
		uint8		  info = XLOG_XHEAP_INSERT;
		int			  bufflags = 0;
		UndoLogControl	 *ulog;
		XHeapTuple tuple = xheaptup;

		if (prev_urecptr != INVALID_UNDO_REC_PTR)
			xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_BLK_PREV;
		if ((GetUndoRecordUinfo(undorec) & UREC_INFO_PREURP) != 0)
			xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_PREV_URP;

		if (IsSubTransaction()) 
		{
			xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_TOP_XID;
			current_xid = GetTopFullTransactionIdIfAny();
		}

		page = BufferGetPage(buffer);
		ulog = (UndoLogControl *) undo_sys_ctx->ulogs[undo_log_ctx->logs[persistence]];

		/*
		 * If this is the single and first tuple on page, we can reinit the
		 * page instead of restoring the whole thing.  Set flag, and hide
		 * buffer references from XLogInsert.
		 */
		if (ItemPointerGetOffsetNumber(&(tuple->ctid)) == FirstOffsetNumber &&
			xheap_page_get_max_offset_number(page) == FirstOffsetNumber)
		{
			info |= XLOG_XHEAP_INIT_PAGE;
			bufflags |= REGBUF_WILL_INIT;
		}

		xlrec.offnum = ItemPointerGetOffsetNumber(&tuple->ctid);
		Assert(ItemPointerGetBlockNumber(&tuple->ctid) == BufferGetBlockNumber(buffer));

		/*
		 * For logical decoding, we need the tuple even if we're doing a full
		 * page write, so make sure it's included even if we take a full-page
		 * image. (XXX We could alternatively store a pointer into the FPW).
		 */
		if (RelationIsLogicallyLogged(rel))
		{
			xlrec.flags |= XLOG_XHEAP_CONTAINS_NEW_TUPLE;
			bufflags |= REGBUF_KEEP_DATA;
			if (IsToastRelation(rel))
					xlrec.flags |= XLOG_XHEAP_INSERT_ON_TOAST_RELATION;
		}

		xlundohdr.relOid = RelationGetRelid(rel);
		xlundohdr.urecptr = urec_ptr;
		xlundohdr.flag = xl_undo_header_flag;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfXHeapInsert);

		XLogRegisterData((char *) &xlundohdr, SizeOfXLUndoHeader);
		if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
		{
			ereport(DEBUG5, (errcode(ERRCODE_DATA_EXCEPTION),
							errmsg("blkprev=%lu", prev_urecptr)));
			Assert(prev_urecptr != INVALID_UNDO_REC_PTR);
			XLogRegisterData((char *) &(prev_urecptr), sizeof(UndoRecPtr));
		}

		if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
			XLogRegisterData((char *) &(old_prev_urp), sizeof(UndoRecPtr));

		if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
		{
			XLogRegisterData((char *) &(current_xid), sizeof(FullTransactionId));
		}

		xlog_undo_meta_write(&undometa);

		populate_xl_xheap_header(&xlhdr, tuple->disk_tuple);


		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);
		XLogRegisterBufData(0, (char *) &xlhdr, SizeOfXHeapHeader);

		XLogRegisterBufData(0,
							(char *) tuple->disk_tuple + offsetof(XHeapDiskTupleData, data),
							tuple->disk_tuple_size - offsetof(XHeapDiskTupleData, data));
		
		xlog_register_undo_buffers(undo_cache_ctx->undo_prepare_buffers, 1, ulog);

		/* filtering by origin on a row level is much more efficient */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		recptr = XLogInsert(RM_XHEAP_ID, info);

		PageSetLSN(page, recptr);
		prepare_buffers_set_page_lsn(undo_cache_ctx->undo_prepare_buffers, recptr);
		set_undolog_meta_lsn(recptr);
	}
	release_undolog_meta(persistence);

	END_CRIT_SECTION();
	/* Clean up */
	Assert(FullTransactionIdIsValid(xheaptup->disk_tuple->modified_xid));

	reset_prepared_buffers_in_ctx();
	UnlockReleaseBuffer(buffer);
	pgstat_count_heap_insert(rel, 1);

	if (xheaptup != tup)
	{
		tup->ctid = xheaptup->ctid;
		XHeapFreeTuple(xheaptup);
	}

	return InvalidOid;
}

void
relation_put_xtuple(Relation relation, Buffer buffer, XHeapTupleData *tuple)
{
	OffsetNumber	offNum = InvalidOffsetNumber;
	XHeapBufferPage bufpage = {buffer, NULL};

	offNum = xpage_add_item(relation, &bufpage, (Item) tuple->disk_tuple,
						  tuple->disk_tuple_size, InvalidOffsetNumber, false);
	if (offNum == InvalidOffsetNumber)
	{
		ereport(PANIC, (errmsg("xid %lu, oid %u, blockno %u. failed to add tuple to page.",
							   GetTopFullTransactionId().value, RelationGetRelid(relation),
							   BufferGetBlockNumber(buffer))));
	}
	ereport(DEBUG5,
			(errmsg("xheap put xtuple: xheap rel: %s, buf: %d, ctid: block %u offset %u, "
					"tuplen: %d",
					RelationGetRelationName(relation), buffer,
					BufferGetBlockNumber(buffer), offNum, tuple->disk_tuple_size)));
	ItemPointerSet(&(tuple->ctid), BufferGetBlockNumber(buffer), offNum);
}

XHeapTuple
xheap_prepare_insert(Relation rel, XHeapTuple tuple, int options)
{
	tuple->disk_tuple->flag &= ~XHEAP_VIS_STATUS_MASK;
	tuple->disk_tuple->locker_xid = InvalidFullTransactionId;
	tuple->disk_tuple->urec = INVALID_UNDO_REC_PTR;
	tuple->table_oid = RelationGetRelid(rel);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!XHeapTupleHasExternal(tuple));
		return tuple;
	}
	else if (XHeapTupleHasExternal(tuple) ||
			 tuple->disk_tuple_size > XTOAST_TUPLE_THRESHOLD)
	{
		/* Toast insert or update */
		return xheap_toast_insert_or_update(rel, tuple, NULL, options);
	}
	else
		return tuple;
}

static void
keep_or_release_buffer(const bool keepBuf, Buffer *buf, Buffer buffer)
{
	if (keepBuf)
	{
		*buf = buffer;
	}
	else
	{
		ReleaseBuffer(buffer);
		*buf = InvalidBuffer;
	}
}

static void
xheap_fetch_handle_invalid(XHeapTuple res_tup, const bool keep_tup,
						   const XHeapTuple page_tup, XHeapTuple tuple)
{
	if (res_tup != NULL)
	{
		Assert(keep_tup);
		if (page_tup != res_tup)
		{
			xheap_copy_tuple_with_buffer(res_tup, tuple);
			XHeapFreeTuple(res_tup);
		}
	}
	else
	{
		tuple->disk_tuple = NULL;
	}
}

bool
xheap_fetch(Relation relation, Snapshot snapshot, ItemPointer tid, XHeapTuple tuple,
			Buffer *buf, bool keep_buf, bool keepTup, bool *has_cur_xact_write)
{
	XHeapTuple		res_tup = NULL;
	Buffer			buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	OffsetNumber	offnum = ItemPointerGetOffsetNumber(tid);
	bool			is_valid;
	ItemPointerData ctid = *tid;
	RowPtr		   *rp;
	Page			page;
	XHeapTuple		page_tup;

	/* Caller must provide a pre-allocated buffer */
	Assert(tuple && tuple->disk_tuple);

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);

	// check out-of-range items
	if (offnum < FirstOffsetNumber || offnum > xheap_page_get_max_offset_number(page))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		keep_or_release_buffer(keep_buf, buf, buffer);

		tuple->disk_tuple = NULL;
		return false;
	}

	rp = XPageGetRowPtr(page, offnum);
	page_tup =
		(RowPtrIsNormal(rp)) ? xheap_get_tuple(relation, buffer, offnum, tuple) : NULL;

	is_valid =
		xheap_tuple_fetch(relation, buffer, offnum, snapshot, &res_tup, &ctid, keepTup,
						  NULL, NULL, &page_tup, -1, NULL, has_cur_xact_write);

	if (ItemPointerIsValid(&ctid) && (snapshot == SnapshotAny || !is_valid))
	{
		*tid = ctid;  // the new ctid
	}

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	if (is_valid)
	{
		if (page_tup != res_tup)
		{
			xheap_copy_tuple_with_buffer(res_tup, tuple);
			XHeapFreeTuple(res_tup);
		}
		keep_or_release_buffer(keep_buf, buf, buffer);

		return true;
	}

	// tuple is invalid
	keep_or_release_buffer(keep_buf, buf, buffer);
	xheap_fetch_handle_invalid(res_tup, keepTup, page_tup, tuple);

	return false;
}

bool
xheap_fetch_row(Relation relation, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot,
			  XHeapTuple xtuple)
{
	Buffer buffer;

	ExecClearTuple(slot);

	if (xheap_fetch(relation, snapshot, tid, xtuple, &buffer, false, false, NULL))
	{
		xheap_slot_store_xheap_tuple(xtuple, slot, true, false);

		return true;
	}

	return false;
}


static XHeapTuple
xheap_toast_flatten_tuple(XHeapTuple tup, TupleDesc tup_desc)
{
	int				   num_attrs = tup_desc->natts;
	Datum			   toast_values[MaxTupleAttributeNumber];
	bool			   toast_isnull[MaxTupleAttributeNumber];
	bool			   toast_free[MaxTupleAttributeNumber];
	XHeapTuple		   new_tuple;

	/*
     * Break down the tuple into fields.
     */
	Assert(num_attrs <= MaxTupleAttributeNumber);

	xheap_deform_tuple(tup, tup_desc, toast_values, toast_isnull);

	memset(toast_free, 0, num_attrs * sizeof(bool));
	for (int i = 0; i < num_attrs; i++)
	{
		/*
         * Look at non-null varlena attributes
         */
		if (!toast_isnull[i] && tup_desc->attrs[i].attlen == -1)
		{
			struct varlena *newValue = (struct varlena *) DatumGetPointer(toast_values[i]);

			if (VARATT_IS_EXTERNAL(newValue))
			{
				newValue = toast_fetch_datum(newValue);
				toast_values[i] = PointerGetDatum(newValue);
				toast_free[i] = true;
			}
		}
	}

	/*
     * Form the reconfigured tuple.
     */
	new_tuple = xheap_form_tuple(tup_desc, toast_values, toast_isnull);

	new_tuple->ctid = tup->ctid;
	new_tuple->table_oid = tup->table_oid;
	new_tuple->disk_tuple = (XHeapDiskTuple) ((char *) new_tuple + XHeapTupleDataSize);

	/* Only copy visibility related aspect from original tuple. */
	new_tuple->disk_tuple->modified_xid = tup->disk_tuple->modified_xid;
	new_tuple->disk_tuple->locker_xid = tup->disk_tuple->locker_xid;
	new_tuple->disk_tuple->flag &= ~XHEAP_VIS_STATUS_MASK;
	new_tuple->disk_tuple->flag |= tup->disk_tuple->flag & XHEAP_VIS_STATUS_MASK;


	for (int i = 0; i < num_attrs; i++)
	{
		if (toast_free[i])
		{
			pfree(DatumGetPointer(toast_values[i]));
		}
	}
	return new_tuple;
}

static XHeapTuple
xheap_extract_replica_identity(Relation relation, XHeapTuple tp, bool *copy,
							char *relreplident)
{
	TupleDesc  desc = RelationGetDescr(relation);
	Oid		   replidindex;
	bool	   nulls[MaxHeapAttributeNumber];
	Datum	   values[MaxHeapAttributeNumber];
	Relation   rel;
	Relation   indexRel;
	Oid		   relid;
	bool	   is_null = true;
	HeapTuple  tuple;
	Datum	   replident;
	XHeapTuple key_tuple;

	*copy = false;
	*relreplident = REPLICA_IDENTITY_NOTHING;

	if (!RelationIsLogicallyLogged(relation))
	{
		return NULL;
	}

	rel = table_open(RelationRelationId, AccessShareLock);
	relid = RelationGetRelid(relation);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						errmsg("pg_class entry for relid %u vanished during "
							   "XHeapExtractReplicaIdentity",
							   relid)));
	}
	replident =
		heap_getattr(tuple, Anum_pg_class_relreplident, RelationGetDescr(rel), &is_null);
	table_close(rel, AccessShareLock);
	heap_freetuple(tuple);

	if (is_null)
	{
		*relreplident = REPLICA_IDENTITY_NOTHING;
	}
	else
	{
		*relreplident = CharGetDatum(replident);
	}

	/* find the replica identity index */
	replidindex = RelationGetReplicaIndex(relation);
	if (!OidIsValid(replidindex))
	{
		*relreplident = REPLICA_IDENTITY_FULL;
	}

	if (*relreplident == REPLICA_IDENTITY_NOTHING)
	{
		return NULL;
	}

	if (*relreplident == REPLICA_IDENTITY_FULL)
	{
		XHeapTuple fullTuple = tp;
		if (XHeapTupleHasExternal(tp))
		{
			*copy = true;
			fullTuple = xheap_toast_flatten_tuple(tp, RelationGetDescr(relation));
		}
		return fullTuple;
	}

	indexRel = RelationIdGetRelation(replidindex);

	/* deform tuple, so we have fast access to columns */
	xheap_deform_tuple(tp, desc, values, nulls);

	/* set all columns to NULL, regardless of whether they actually are */
	memset(nulls, 1, sizeof(nulls));

	/*
     * Now set all columns contained in the index to NOT NULL, they cannot currently be NULL.
     */
	for (int natt = 0; natt < IndexRelationGetNumberOfKeyAttributes(indexRel); natt++)
	{
		int attno = indexRel->rd_index->indkey.values[natt];

		if (attno < 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED), errmsg("system column in index")));
		}
		nulls[attno - 1] = false;
	}

	*copy = true;
	key_tuple = xheap_form_tuple(desc, values, nulls);
	RelationClose(indexRel);

	/*
     * If the tuple, which by here only contains indexed columns, still has
     * toasted columns, force them to be inlined. This is somewhat unlikely
     * since there's limits on the size of indexed columns, so we don't
     * duplicate toast_flatten_tuple()s functionality in the above loop over
     * the indexed columns, even if it would be more efficient.
     */
	if (XHeapTupleHasExternal(key_tuple))
	{
		XHeapTuple oldtup = key_tuple;
		key_tuple =
			xheap_toast_flatten_tuple((XHeapTuple) oldtup, RelationGetDescr(relation));
		XHeapFreeTuple(oldtup);
	}

	return key_tuple;
}

TM_Result
xheap_delete(Relation relation, ItemPointer tid, 
			 CommandId cid, Snapshot crosscheck, Snapshot snapshot, bool wait, 
			 TM_FailureData *tmfd, bool changing_part)
{
	TM_Result			result;
	FullTransactionId	fxid = GetTopFullTransactionId(),
						update_xid = InvalidFullTransactionId,
						locker_xid = InvalidFullTransactionId,
						modified_subxid = InvalidFullTransactionId,
						subxid = InvalidFullTransactionId;
	XHeapTupleData		xtuple;
	Buffer				buffer;
	UndoRecPtr			prev_urecptr = INVALID_UNDO_REC_PTR,
						urecptr = INVALID_UNDO_REC_PTR;
	xl_undo_meta		xlum;
	ItemPointerData		ctid;
	bool				inplace_updatedOrLocked = false,
						has_tup_lock = false;

	XHeapTupleTransInfo xinfo;
	StringInfoData		undotup;
	bool				multixid_self = false;
	int					retryTimes = 0;
	BlockNumber			blkno;
	Page				page;
	OffsetNumber		offnum;
	RowPtr			   *rp;
	bool				is_old_tuple_copied = false;
	char				identity;
	XHeapTuple			old_key_tuple;
	UndoPersistence		persistence;
	Oid					relOid;
	UndoRecPtr			old_prev_urp;
	uint16 				infomask;
	bool 				is_subxact;

	Assert(ItemPointerIsValid(tid));

	/*
	 * Forbid this during a parallel operation, lest it allocate a combocid.
	 * Other workers might need that combocid for visibility checks, and we
	 * have no provision for broadcasting it to them.
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot delete tuples during a parallel operation")));

	blkno = ItemPointerGetBlockNumber(tid);
	buffer = ReadBuffer(relation, blkno);
	page = BufferGetPage(buffer);

	init_xlog_undo_meta(&xlum);
	if (undo_cache_ctx->undo_prepare_buffers)
		reset_undo_prepare_buffers(undo_cache_ctx->undo_prepare_buffers, false);

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	offnum = ItemPointerGetOffsetNumber(tid);
	rp = XPageGetRowPtr(page, offnum);
	Assert(RowPtrIsNormal(rp));

	xheap_page_prune_fsm(relation, buffer, fxid, page, blkno); 

check_tup_satisfies_update:
	result = xheap_tuple_satisfies_update(
		relation, snapshot, tid, &xtuple, cid, buffer, &ctid, &xinfo, &modified_subxid,
		&locker_xid, false, multixid_self, &inplace_updatedOrLocked);
	update_xid = xinfo.xid;
	multixid_self = false;

	if (result == TM_Invisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("XHeapDelete: attempted to delete invisible tuple")));
	}
	else if ((result == TM_BeingModified) && wait)
	{
		XHeapWaitInfo wait_info = {
			.disk_tuple_modified_xid = xtuple.disk_tuple->modified_xid,
			.disk_tuple_locker_xid = xtuple.disk_tuple->locker_xid,
			.disk_tuple_urec = xtuple.disk_tuple->urec,
			.ctid = xtuple.ctid,
			.disk_tuple_flag = xtuple.disk_tuple->flag
		};

		if (!xheap_wait_helper(relation, buffer, &wait_info, LockTupleExclusive, LockWaitBlock,
						update_xid, locker_xid, modified_subxid, &has_tup_lock,
						&multixid_self)) 
		{
			LimitRetryTimes(retryTimes++);
			goto check_tup_satisfies_update;
		}

		result = TM_Ok;
	}
	else if (result == TM_Updated && xtuple.disk_tuple != NULL &&
			 XHeapTupleHasMultiLockers(xtuple.disk_tuple->flag))
		ereport(PANIC, (errmsg("xheap_delete error. tuple flag %hu, xid %lu, oid %u, "
							   "blockno %u offsetnum %hu.",
							   xtuple.disk_tuple->flag, GetTopFullTransactionId().value,
							   RelationGetRelid(relation), BufferGetBlockNumber(buffer),
							   offnum)));

	if (crosscheck != InvalidSnapshot && result == TM_Ok)
	{
		/* Perform additional check for transaction-snapshot mode RI updates */
		if (!xheap_tuple_fetch(relation, buffer, offnum, crosscheck, NULL, NULL, false,
							   NULL, NULL, NULL, -1, NULL, NULL))
			result = TM_Updated;
	}

	rp = XPageGetRowPtr(page, offnum);
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified || result == TM_Updated ||
			   result == TM_Deleted || result == TM_BeingModified);

		/* Fill in the tmfd that the caller could use */
		/* If item id is deleted, tuple can't be marked as moved. */
		if (XHeapTupleIsMoved(xtuple.disk_tuple->flag))
			ItemPointerSetMovedPartitions(&tmfd->ctid);
		else
			tmfd->ctid = ctid;

		tmfd->xmax = XidFromFullTransactionId(update_xid);  
		tmfd->in_place_updated_or_locked = inplace_updatedOrLocked;
		tmfd->cmax = (result == TM_SelfModified) ? xinfo.cid : InvalidCommandId;

		if (result == TM_Updated && !inplace_updatedOrLocked &&
			ItemPointerEquals(&tmfd->ctid, tid))
			result = TM_Deleted;

		UnlockReleaseBuffer(buffer);

		if (has_tup_lock)
			UnlockTuple(relation, &(xtuple.ctid), ExclusiveLock);

		return result;
	}

	is_subxact = IsSubTransaction();
	if (is_subxact)
		subxid = GetCurrentFullTransactionId();

	/*
     * It's possible that tuple slot is now marked as frozen. Hence, we
     * refetch the tuple here.
     */
	Assert(RowPtrIsNormal(rp));
	xtuple.disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);
	xtuple.disk_tuple_size = RowPtrGetLen(rp);

	/* create the old tuple for caller */
	if (relation->rd_indexlist != NIL && tmfd->oldslot)
	{
		xheap_slot_store_xheap_tuple(xheap_copy_tuple(&xtuple), tmfd->oldslot, true, true);
		slot_getallattrs(tmfd->oldslot);
	}

	/*
     * The latest modifier of the tuple must be frozen.
     */
	if (FullTransactionIdPrecedes(((XHeapDiskTuple)(xtuple.disk_tuple))->modified_xid,
		FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid))))
		xinfo.xid = InvalidFullTransactionId;

	if (TransactionIdOlderThanAllUndo(xinfo.xid))
		xinfo.xid = FullTransactionIdFromEpochAndXid(0, FrozenTransactionId);

	xtuple.table_oid = RelationGetRelid(relation);

	old_key_tuple =
		xheap_extract_replica_identity(relation, &xtuple, &is_old_tuple_copied, &identity);

	/* Prepare Undo */
	persistence = UndoPersistenceForRelation(relation);
	relOid = RelationGetRelid(relation);

	prev_urecptr = xtuple.disk_tuple->urec;

	/* 
	 * Clear the lock status and locker_xid. When we reach here, there must be no locker in the
	 * old tuple, because we will update it soon. So we can clear the lock status and locker xid 
	 * and the undo record for the old tuple will not have lock flag. Tuple in undo record must 
	 * have no lock flag because it has no locker xid in undo, or we will get a tuple with lock flag
	 * but have no valid locker xid after rollbacking. This will make xheap_tuple_satisfies_update 
	 * and xheap_wait confused.
	 */
	infomask = xtuple.disk_tuple->flag;
	xtuple.disk_tuple->flag &= ~XHEAP_LOCK_STATUS_MASK;

	/* We can't alloc memory in critical section */
	urecptr = xheap_prepare_undo_delete(
		relOid,  relation->rd_rel->relfilenode, relation->rd_locator.spcOid,
		persistence, buffer, offnum, fxid, subxid, cid, prev_urecptr, INVALID_UNDO_REC_PTR,
		xinfo.xid, &xtuple, InvalidBlockNumber, NULL, NULL, &xlum);
	/* recover flag from infomask */
	xtuple.disk_tuple->flag = infomask;
	/* undo tup is only for xheap wal */
	initStringInfo(&undotup);
	appendBinaryStringInfo(&undotup, (char *) xtuple.disk_tuple, xtuple.disk_tuple_size);

	START_CRIT_SECTION();
	insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, 0);
	old_prev_urp = get_current_tansaction_undorec_ptr(persistence);
	set_current_tansaction_undorec_ptr(urecptr, persistence);
	XHeapTupleHeaderSetUndoRecPtr(xtuple.disk_tuple, urecptr);

	/*
     * If this transaction commits, the tuple will become DEAD sooner or
     * later.  If the transaction finally aborts, the subsequent page pruning
     * will be a no-op and the hint will be cleared.
     */
	XPageSetPrunable(page, fxid);
	xheap_record_potential_free_space(buffer, SHORTALIGN(xtuple.disk_tuple_size));
	if (is_subxact)
		xheap_keep_subtxn_lock(xtuple.disk_tuple, subxid);
	else
		XHeapTupleHeaderClearAllLocker(xtuple.disk_tuple);
	xtuple.disk_tuple->flag &= ~XHEAP_VIS_STATUS_UNLOCK_MASK;
	xtuple.disk_tuple->flag |= XHEAP_DELETED;
	xheap_tuple_set_modified_xid(&xtuple, fxid);

	/* Signal that this is actually a move into another partition */
	if (changing_part)
		XHeapTupleHeaderSetMovedPartitions(xtuple.disk_tuple);

	MarkBufferDirty(buffer);
	update_undolog_meta(fxid, prepare_buffers_get_first_undoptr(undo_cache_ctx->undo_prepare_buffers),
					 &xlum, persistence, prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
					 prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));


	if (RelationNeedsWAL(relation))
	{
		uint8		  xl_undo_header_flag = 0;
		UnpackedUndoRecord	 *urec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
		FullTransactionId current_xid = InvalidFullTransactionId;
		xl_xheap_delete  xlrec;
		XLogRecPtr	   recptr;
		xl_xheap_header  xlhdr;
		xl_undo_header   xlundohdr;
		XHeapDiskTuple oldtup;
		uint32		   toastlen = 0;
		XHeapDiskTuple toasttup = NULL;
		xl_xheap_header  toastxlhdr;
		UndoLogControl *ulog;

		if (prev_urecptr != INVALID_UNDO_REC_PTR)
			xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_BLK_PREV;

		if (GetUndoRecordUinfo(urec) & UREC_INFO_PREURP)
			xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_PREV_URP;

		if (TransactionIdIsValid(subxid.value))
		{
			xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_TOP_XID;
			current_xid = GetTopFullTransactionIdIfAny();
		}

		if (RelationIsLogicallyLogged(relation) && XHeapTupleHasExternal(&xtuple))
			xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_TOAST;

		xlundohdr.relOid = relOid;
		xlundohdr.urecptr = urecptr;
		xlundohdr.flag = xl_undo_header_flag;
		page = BufferGetPage(buffer);

		ulog =  (UndoLogControl *) undo_sys_ctx->ulogs[undo_log_ctx->logs[persistence]];
		
		xlrec.offnum = ItemPointerGetOffsetNumber(&(xtuple.ctid));
		xlrec.flag = xtuple.disk_tuple->flag;
		xlrec.oldxid = xinfo.xid;

		oldtup = (XHeapDiskTuple) undotup.data;
		xheap_fill_header(&xlhdr, oldtup);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfXHeapDelete);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

		XLogRegisterData((char *) &xlundohdr, SizeOfXLUndoHeader);

		if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
		{
			Assert(prev_urecptr != INVALID_UNDO_REC_PTR);
			XLogRegisterData((char *) &(prev_urecptr), sizeof(UndoRecPtr));
		}
		if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
			XLogRegisterData((char *) &(old_prev_urp), sizeof(UndoRecPtr));
		if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
		{
			XLogRegisterData((char *) &(current_xid), sizeof(FullTransactionId));
		}

		if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_TOAST) != 0)
		{
			toastlen = old_key_tuple->disk_tuple_size;
			toasttup = (XHeapDiskTuple) old_key_tuple->disk_tuple;
			xheap_fill_header(&toastxlhdr, toasttup);
			toastlen -= SizeOfXHeapDiskTupleData - SizeOfXHeapHeader;
			XLogRegisterData((char *) &toastlen, sizeof(uint32));
			XLogRegisterData((char *) &toastxlhdr, SizeOfXHeapHeader);
			XLogRegisterData(
				(char *) old_key_tuple->disk_tuple + SizeOfXHeapDiskTupleData,
				toastlen - SizeOfXHeapHeader);
		}

		xlog_undo_meta_write(&xlum);

		XLogRegisterData((char *) &xlhdr, SizeOfXHeapHeader);
		XLogRegisterData((char *) xtuple.disk_tuple + offsetof(XHeapDiskTupleData, data),
						xtuple.disk_tuple_size - offsetof(XHeapDiskTupleData, data));
		
		xlog_register_undo_buffers(undo_cache_ctx->undo_prepare_buffers, 1, ulog);

		/* filtering by origin on a row level is much more efficient */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		recptr = XLogInsert(RM_XHEAP_ID, XLOG_XHEAP_DELETE);
		PageSetLSN(page, recptr);
		prepare_buffers_set_page_lsn(undo_cache_ctx->undo_prepare_buffers, recptr);
		set_undolog_meta_lsn(recptr);
	}

	release_undolog_meta(persistence);

	END_CRIT_SECTION();

	if (old_key_tuple != NULL && is_old_tuple_copied)
		XHeapFreeTuple(old_key_tuple);
	
	pfree(undotup.data);
	Assert(FullTransactionIdIsValid(xtuple.disk_tuple->modified_xid));
	
	reset_prepared_buffers_in_ctx();

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	pgstat_count_heap_delete(relation);

	if (XHeapTupleHasExternal(&xtuple))
		xheap_toast_delete(relation, &xtuple);

	ReleaseBuffer(buffer);

	if (has_tup_lock)
		UnlockTuple(relation, &(xtuple.ctid), ExclusiveLock);

	return TM_Ok;
}

static void
put_inplace_update_xtuple(XHeapTuple old_tup, XHeapTuple new_tup, RowPtr *lp)
{

	/*
     * For inplace updates, we copy the entire data portion including null
     * bitmap of new tuple.
     */
	if (new_tup->disk_tuple_size > RowPtrGetLen(lp))
	{
		RowPtrChangeLen(lp, new_tup->disk_tuple_size);
	}
	memcpy((char *) old_tup->disk_tuple + SizeOfXHeapDiskTupleData,
				  (char *) new_tup->disk_tuple + SizeOfXHeapDiskTupleData,
				  new_tup->disk_tuple_size - SizeOfXHeapDiskTupleData);

	old_tup->disk_tuple->flag = old_tup->disk_tuple->flag & XHEAP_VIS_STATUS_MASK;
	old_tup->disk_tuple->flag |= (new_tup->disk_tuple->flag & ~XHEAP_VIS_STATUS_MASK);

	XHeapTupleHeaderSetNatts(old_tup->disk_tuple,
							 XHeapTupleHeaderGetNatts(new_tup->disk_tuple));

	old_tup->disk_tuple_size = new_tup->disk_tuple_size;
	old_tup->disk_tuple->t_hoff = new_tup->disk_tuple->t_hoff;
	return;
}

static void 
xheap_keep_subtxn_lock(XHeapDiskTuple tuple, FullTransactionId subxid)
{
	if (XHeapTupleHasMultiLockers(tuple->flag))
	{
		FullTransactionId member_xid;
		if(XMultiXactIdIsCurrent(tuple->locker_xid, &member_xid))
		{
			/* need to keep the lock info of sub transaction on the header */
			tuple->locker_xid = member_xid;
			/* promote to exclusive lock */
			tuple->flag &= ~XHEAP_LOCK_STATUS_MASK;
			tuple->flag |= XHEAP_XID_EXCL_LOCK;
		} 
		else 
		{
			tuple->locker_xid = InvalidFullTransactionId;
			tuple->flag &= ~XHEAP_LOCK_STATUS_MASK;
		}
	}
	else if (FullTransactionIdFollowsOrEquals(tuple->locker_xid, FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid))) &&
		!FullTransactionIdEquals(tuple->locker_xid, subxid) &&
		TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(tuple->locker_xid)))
	{
		/* locker transaction is my ancestor's transaction, promote to exclusive lock */
		tuple->flag &= ~XHEAP_LOCK_STATUS_MASK;
		tuple->flag |= XHEAP_XID_EXCL_LOCK;
	} else {
		/* don't keep the lock */
		tuple->flag &= ~XHEAP_LOCK_STATUS_MASK;
		tuple->locker_xid = InvalidFullTransactionId;
	}
}

/*
 * xheap_update - update a tuple
 *
 * This function either updates the tuple in-place or it deletes the old
 * tuple and new tuple for non-in-place updates.  Additionally this function
 * inserts an undo record and updates the undo pointer in page header.
 *
 * For input and output values, see heap_update.
 */
TM_Result
xheap_update(Relation relation, ItemPointer otid, XHeapTuple newtup, CommandId cid,
			 Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
			 LockTupleMode *lockmode, Bitmapset **modified_idx_attrs, bool *inplace_update)
{
	TM_Result	  result = TM_Ok;
	FullTransactionId fxid = GetTopFullTransactionId();
	FullTransactionId	old_updater_xid;
	Bitmapset	   *inplace_upd_attrs = NULL;
	Bitmapset	   *key_attrs = NULL;
	Bitmapset	   *interesting_attrs = NULL;
	Bitmapset	   *modified_attrs = NULL;
	RowPtr		   *lp;
	StringInfoData	undotup;
	XHeapTupleData	oldtup;
	XHeapTuple		xheaptup;
	UndoRecPtr		urecptr;
	UndoRecPtr		new_urecptr;
	UndoRecPtr		prev_urecptr = INVALID_UNDO_REC_PTR;
	Page			page;
	BlockNumber		block;
	ItemPointerData ctid;
	Buffer			buffer;
	Buffer			newbuf;
	Size			newtupsize = 0;
	Size			oldtupsize = 0;
	Size			pagefree = 0;
	OffsetNumber	old_offnum = 0;
	bool			have_tuple_lock = false;
	bool			is_index_updated = false;
	bool			use_inplace_update = true;
	bool				need_toast = false;
	bool				is_subxact = false;
	bool				inplace_updated = false;
	XHeapTupleTransInfo txactinfo;
	uint16				infomask_old_tuple = 0;
	uint16				infomask_new_tuple = 0;
	FullTransactionId	locker_xid = InvalidFullTransactionId;
	FullTransactionId	modified_subxid = InvalidFullTransactionId;

	FullTransactionId	subxid = InvalidFullTransactionId;
	char			   *xlog_xor_delta = NULL;
	xl_undo_meta		xlum;
	bool				has_pruned = false;
	bool				already_locked = false;
	bool				multixid_self = false;
	BlockNumber			blkno = 0;
	bool				is_old_tuple_copied = false;
	char				identity;
	XHeapTuple			old_key_tuple;
	UndoPersistence		persistence;
	Oid					rel_oid;
	int					undo_xor_delta_size = 0;
	uint16				prefixlen = 0;
	uint16				suffixlen = 0;
	uint8				xor_delta_flags = 0;
	char			   *oldp;
	char			   *newp;
	int					oldlen;
	int					newlen;
	int					minlen;
	UnpackedUndoRecord *undorec;
	UndoRecPtr			old_prev_urp;
	UndoRecPtr			old_prev_urp_insert;
	UndoPrepareBuffers *upbuffers;


	Assert(ItemPointerIsValid(otid));

	/*
	 * Forbid this during a parallel operation, lest it allocate a combocid.
	 * Other workers might need that combocid for visibility checks, and we
	 * have no provision for broadcasting it to them.
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot update tuples during a parallel operation")));


	init_xlog_undo_meta(&xlum);
	if (undo_cache_ctx->undo_prepare_buffers)
		reset_undo_prepare_buffers(undo_cache_ctx->undo_prepare_buffers, false);

	/*
	 * Fetch the list of attributes to be checked for various operations.
	 *
	 * For in-place update considerations, this is wasted effort if we fail to
	 * update or have to put the new tuple on a different page.  But we must
	 * compute the list before obtaining buffer lock --- in the worst case, if
	 * we are doing an update on one of the relevant system catalogs, we could
	 * deadlock if we try to fetch the list later.  Note, that as of now
	 * system catalogs are always stored in heap, so we might not hit the
	 * deadlock case, but it can be supported in future.  In any case, the
	 * relcache caches the data so this is usually pretty cheap.
	 *
	 * Note that we get a copy here, so we need not worry about relcache flush
	 * happening midway through.
	 */
	inplace_upd_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_HOT_BLOCKING);
	key_attrs = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_IDENTITY_KEY);
	block = ItemPointerGetBlockNumber(otid);
	buffer = ReadBuffer(relation, block);
	page = BufferGetPage(buffer);

	interesting_attrs = NULL;
	interesting_attrs = bms_add_members(interesting_attrs, inplace_upd_attrs);
	interesting_attrs = bms_add_members(interesting_attrs, key_attrs);


	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	old_offnum = ItemPointerGetOffsetNumber(otid);
	lp = XPageGetRowPtr(page, old_offnum);
	Assert(RowPtrIsNormal(lp));

check_tup_satisfies_update:
	result = xheap_tuple_satisfies_update(relation, snapshot, otid, &oldtup, cid, buffer,
										  &ctid, &txactinfo, &modified_subxid, &locker_xid,
										  false, multixid_self, &inplace_updated);

	multixid_self = false;

	/*
	 * The oldUpdaterXid is either the inserting xid or the previous updater xid.
	 */
	old_updater_xid =
		FullTransactionIdIsValid(txactinfo.xid) ? txactinfo.xid : InvalidFullTransactionId;

	/* Determine columns modified by the update. Should be recomputed after we re-fetch the old tuple */
	if (oldtup.disk_tuple != NULL)
	{
		if (modified_attrs != NULL)
		{
			bms_free(modified_attrs);
		}
		modified_attrs =
			xheap_determing_modified_columns(relation, interesting_attrs, &oldtup, newtup);
	}

	*lockmode = LockTupleExclusive;

	if (result == TM_Invisible)
	{
		UnlockReleaseBuffer(buffer);
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("attempted to update invisible tuple")));
	}
	else if ((result == TM_BeingModified) && wait)
	{
		FullTransactionId xwait = InvalidFullTransactionId;
		XHeapWaitInfo wait_info = {
			.disk_tuple_modified_xid = oldtup.disk_tuple->modified_xid,
			.disk_tuple_locker_xid = oldtup.disk_tuple->locker_xid,
			.disk_tuple_urec = oldtup.disk_tuple->urec,
			.ctid = oldtup.ctid,
			.disk_tuple_flag = oldtup.disk_tuple->flag
		};

		if (FullTransactionIdIsValid(locker_xid))
			xwait = locker_xid;
		else
			xwait = txactinfo.xid;

		ereport(DEBUG5, (errmsg("xheap_update xid %lu wait %d", xwait.value, wait)));

		// Check if tuple has already been locked by us in the required mode
		already_locked = is_xtuple_locked_by_us(&oldtup, xwait, *lockmode);

		if (!xheap_wait_helper(relation, buffer, &wait_info, *lockmode, LockWaitBlock,
						txactinfo.xid, locker_xid, modified_subxid, &have_tuple_lock,
						&multixid_self))
		{
			goto check_tup_satisfies_update;
		}

		result = TM_Ok;
	}
	else if (result == TM_Ok)
	{
		/*
         * There is no active locker on the tuple, so we avoid grabbing the
         * lock on new tuple.
         */
		ereport(DEBUG5,
				(errmsg("xheap_update result %d success to satisfies update.", result)));
	}
	else
	{
		ereport(DEBUG5,
				(errmsg("xheap_update result %d after satisfies update.", result)));
	}

	if (crosscheck != InvalidSnapshot && result == TM_Ok)
	{
		/* Perform additional check for transaction-snapshot mode RI updates */
		if (!xheap_tuple_fetch(relation, buffer, old_offnum, crosscheck, NULL, NULL, false,
							   NULL, NULL, NULL, -1, NULL, NULL))
			result = TM_Updated;
	}

	lp = XPageGetRowPtr(page, old_offnum);
	if (result != TM_Ok)
	{
		Assert(result == TM_SelfModified || result == TM_Updated ||
			   result == TM_Deleted || result == TM_BeingModified);
		if (oldtup.disk_tuple == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR), errmsg("delete tuple is NULL.")));
		}
		
		if (XHeapTupleIsMoved(oldtup.disk_tuple->flag))
			ItemPointerSetMovedPartitions(&tmfd->ctid);
		else
			tmfd->ctid = ctid;

		tmfd->xmax = XidFromFullTransactionId(txactinfo.xid);
		tmfd->epoch = EpochFromFullTransactionId(txactinfo.xid);

		if (result == TM_SelfModified)
			tmfd->cmax = txactinfo.cid;
		else
			tmfd->cmax = InvalidCommandId;
		tmfd->in_place_updated_or_locked = inplace_updated;
		if (result == TM_Updated && ItemPointerEquals(&tmfd->ctid, otid) &&
			!inplace_updated)
		{
			result = TM_Deleted;
		}

		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock)
		{
			UnlockTuple(relation, &(oldtup.ctid), ExclusiveLock);
		}

		bms_free(inplace_upd_attrs);
		bms_free(key_attrs);

		ereport(
			DEBUG5,
			(errmsg("xheap_update returned %d tmfd {block %u offset %u xmax %u cmax %u}",
					result, ItemPointerGetBlockNumber(&tmfd->ctid),
					ItemPointerGetOffsetNumber(&tmfd->ctid), tmfd->xmax, tmfd->cmax)));

		return result;
	}

	/* the new tuple is ready, except for this: */
	newtup->table_oid = RelationGetRelid(relation);

	is_index_updated = bms_overlap(modified_attrs, inplace_upd_attrs);
	if (modified_idx_attrs != NULL) {
        *modified_idx_attrs = is_index_updated ? bms_intersect(modified_attrs, inplace_upd_attrs) : NULL;
    }

	if (relation->rd_rel->relkind != RELKIND_RELATION)
	{
		/* toast table entries should never be recursively toasted */
		Assert(!XHeapTupleHasExternal(&oldtup));
		Assert(!XHeapTupleHasExternal(newtup));
		need_toast = false;
	}
	else
	{
		need_toast = (newtup->disk_tuple_size >= XTOAST_TUPLE_THRESHOLD ||
					 XHeapTupleHasExternal(&oldtup) || XHeapTupleHasExternal(newtup));
	}

	prev_urecptr = oldtup.disk_tuple->urec;
	oldtupsize = SHORTALIGN(oldtup.disk_tuple_size);
	newtupsize = SHORTALIGN(newtup->disk_tuple_size);

	/*
     * An in-place update is only possible if no attribute that have been moved to
     * an external TOAST table.If the new tuple is no larger than the old one, that's enough;
     * otherwise, we also need sufficient free space to be available in the page.
     */
	if (need_toast)
	{
		use_inplace_update = false;
		has_pruned = xheap_page_prune_opt_page(relation, buffer, fxid, false);
		blkno = BufferGetBlockNumber(buffer);

		/* Now that we are done with the page, get its available space */
		if (has_pruned)
		{
			Size   freespace = page_get_xheap_free_space(page);
			double thres =
				RelationGetTargetPageFreeSpacePrune(relation, XHEAP_DEFAULT_FILLFACTOR);
			double prob = FSM_UPDATE_HEURISTI_PROBABILITY * freespace / thres;
			RecordPageWithFreeSpace(relation, blkno, freespace);
			if (rand() % 100 >= 100.0 - prob * 100.0)
			{
				FreeSpaceMapVacuumRange(relation, blkno, blkno + 1);
			}
		}
		ereport(DEBUG5, (errmsg("xheap_update choose non in place because of toast")));
	}
	else if (newtupsize > oldtupsize)
	{
		use_inplace_update = false;
		ereport(DEBUG5,
				(errmsg("xheap_update choose non in place update new tup %lu old tup %lu",
						newtupsize, oldtupsize)));
	}
	else
	{
		use_inplace_update = true;
		ereport(DEBUG5,
				(errmsg("xheap_update choose in place update new tup %lu old tup %lu",
						newtupsize, oldtupsize)));
	}

	is_subxact = IsSubTransaction();
	if (is_subxact)
		subxid = GetCurrentFullTransactionId();

	lp = XPageGetRowPtr(page, old_offnum);
	pagefree = page_get_xheap_free_space(page);

	/*
     * It's possible that tuple slot is now marked as frozen. Hence, we
     * refetch the tuple here.
     */
	Assert(RowPtrIsNormal(lp));
	oldtup.disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, lp);
	oldtup.disk_tuple_size = RowPtrGetLen(lp);

	/* create the old tuple for caller */
	if (relation->rd_indexlist != NIL && tmfd->oldslot)
	{
		xheap_slot_store_xheap_tuple(xheap_copy_tuple(&oldtup), tmfd->oldslot, true, true);
		slot_getallattrs(tmfd->oldslot);
	}

	/*
     * If the slot is marked as frozen, the latest modifier of the tuple must
     * be frozen.
     */
	if (FullTransactionIdPrecedes(((XHeapDiskTuple)(oldtup.disk_tuple))->modified_xid,
		FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid))))
	{
		txactinfo.xid = InvalidFullTransactionId;
	}

	/*
     * If the last transaction that has updated the tuple is already too old,
     * then consider it as frozen which means it is all-visible.  This ensures
     * that we don't need to store epoch in the undo record to check if the
     * undo tuple belongs to previous epoch and hence all-visible.  See
     * comments atop of file inplaceheapam_visibility.c.
     */
	if (FullTransactionIdPrecedes(txactinfo.xid, 
		FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_recycle_xid))))
	{
		txactinfo.xid = FullTransactionIdFromEpochAndXid(0, FrozenTransactionId);
		old_updater_xid = FullTransactionIdFromEpochAndXid(0, FrozenTransactionId);
	}

	Assert(!XHeapTupleIsUpdated(oldtup.disk_tuple->flag));

	if (need_toast)
	{
		use_inplace_update = false;
	}
	else if (!use_inplace_update)
	{
		/* try prune xheap page to avoid non in place update in newtup exceeding oldtup case */
		use_inplace_update =
			xheap_page_prune_opt(relation, buffer, old_offnum, newtupsize - oldtupsize);
		/* The page might have been modified, so refresh disk_tuple */
		oldtup.disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, lp);
	}
	else
	{
		ereport(DEBUG5, (errmsg("xheap_update is in place update %d index update %d",
								use_inplace_update, is_index_updated)));
	}

	/*
     * updated tuple doesn't fit on current page or the toaster needs to be
     * activated or transaction slot has been reused.  To prevent concurrent
     * sessions from updating the tuple, we have to temporarily mark it
     * locked, while we release the page lock.
     */
	if (!use_inplace_update)
	{
		XHeapTuple oldtupletemp;

		if (!already_locked)
		{
			(void) xheap_execute_lock_tuple(relation, buffer, &oldtup, LockTupleExclusive,
											lp);
		}
		oldtupletemp = xheap_copy_tuple(&oldtup);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

		/*
         * Let the toaster do its thing, if needed.
         *
         * Note: below this point, XHeaptup is the data we actually intend to
         * store into the relation; newtup is the caller's original untoasted
         * data.
         */
		if (need_toast)
		{
			xheaptup = xheap_toast_insert_or_update(relation, newtup, oldtupletemp, 0);
			newtupsize = SHORTALIGN(xheaptup->disk_tuple_size);
		}
		else
		{
			xheaptup = newtup;
		}
		XHeapFreeTuple(oldtupletemp);

		if (!need_toast)
		{
			newbuf = relation_get_buffer_for_xtuple(relation, xheaptup->disk_tuple_size,
												buffer, 0, NULL);
		}
		else
		{
			/* Re-acquire the lock on the old tuple's page. */
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			/* Re-check using the up-to-date free space */
			pagefree = page_get_xheap_free_space(page);
			if (newtupsize > pagefree)
			{
				/*
                 * Rats, it doesn't fit anymore.  We must now unlock and
                 * relock to avoid deadlock.  Fortunately, this path should
                 * seldom be taken.
                 */
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				newbuf = relation_get_buffer_for_xtuple(relation, xheaptup->disk_tuple_size,
													buffer, 0, NULL);
			}
			else
			{
				/* OK, it fits here, so we're done. */
				newbuf = buffer;
			}
		}
	}
	else
	{
		/* No TOAST work needed, and it'll fit on same page */
		newbuf = buffer;
		xheaptup = newtup;
	}

	*inplace_update = use_inplace_update;

	old_key_tuple =
		xheap_extract_replica_identity(relation, &oldtup, &is_old_tuple_copied, &identity);

	/* Save the previous updated information in the undo record */
	persistence = UndoPersistenceForRelation(relation);
	rel_oid = RelationGetRelid(relation);

	/* calculate xor delta for inplaceupdate, to allocate correct undo size */
	oldp = (char *) oldtup.disk_tuple + oldtup.disk_tuple->t_hoff;
	newp = (char *) xheaptup->disk_tuple + xheaptup->disk_tuple->t_hoff;
	oldlen = oldtup.disk_tuple_size - oldtup.disk_tuple->t_hoff;
	newlen = xheaptup->disk_tuple_size - xheaptup->disk_tuple->t_hoff;
	minlen = Min(oldlen, newlen);

	if (use_inplace_update && (xheaptup->disk_tuple_size == oldtup.disk_tuple_size) &&
		!RelationIsLogicallyLogged(relation))
	{
		char *oldp_tmp = NULL;
		char *newp_tmp = NULL;
		int	  minlen_with_no_prefixlen;

		oldp_tmp = oldp;
		newp_tmp = newp;
		for (prefixlen = 0; prefixlen < minlen; prefixlen++, oldp_tmp++, newp_tmp++)
		{
			if (*oldp_tmp != *newp_tmp)
			{
				break;
			}
		}

		if (prefixlen < NUM_BLOCKS_FOR_NON_INPLACE_UPDATES)
		{
			prefixlen = 0;
		}
		else
		{
			xor_delta_flags |= UREC_XOR_PREFIX;
		}

		minlen_with_no_prefixlen = minlen - prefixlen;
		oldp_tmp = &(oldp[oldlen - 1]);
		newp_tmp = &(newp[newlen - 1]);
		for (suffixlen = 0; suffixlen < minlen_with_no_prefixlen;
			 suffixlen++, oldp_tmp--, newp_tmp--)
		{
			if (*oldp_tmp != *newp_tmp)
				break;
		}

		if (suffixlen < NUM_BLOCKS_FOR_NON_INPLACE_UPDATES)
		{
			suffixlen = 0;
		}
		else
		{
			xor_delta_flags |= UREC_XOR_SUFFIX;
		}

		if (prefixlen > 0)
			undo_xor_delta_size += sizeof(uint16);
		if (suffixlen > 0)
			undo_xor_delta_size += sizeof(uint16);
	}

	/* The first sizeof(uint8) is space for t_hoff and the second sizeof(uint8) is space for prefix and suffix flag */
	undo_xor_delta_size +=
		sizeof(uint8) + oldtup.disk_tuple->t_hoff - OffsetDataHeader + sizeof(uint8);
	undo_xor_delta_size += oldlen - prefixlen - suffixlen;

	urecptr = xheap_prepare_undo_update(
		rel_oid, RelationGetRelFileLocator(relation), RelationGetRnodeSpace(relation),
		persistence, buffer, newbuf, ItemPointerGetOffsetNumber(&oldtup.ctid), fxid,
		subxid, cid, prev_urecptr, INVALID_UNDO_REC_PTR, old_updater_xid, &oldtup,
		use_inplace_update, &new_urecptr, undo_xor_delta_size, InvalidBlockNumber,
		InvalidBlockNumber, NULL, NULL, &xlum);
	/* undo tup is only for xheap redo wal */
	initStringInfo(&undotup);
	appendBinaryStringInfo(&undotup, (char *) oldtup.disk_tuple, oldtup.disk_tuple_size);

	old_prev_urp = get_current_tansaction_undorec_ptr(persistence);
	old_prev_urp_insert = INVALID_UNDO_REC_PTR;

	oldtup.disk_tuple->urec = urecptr;
	set_current_tansaction_undorec_ptr(urecptr, persistence);

	if (!use_inplace_update)
	{
		old_prev_urp_insert = get_current_tansaction_undorec_ptr(persistence);
		set_current_tansaction_undorec_ptr(new_urecptr, persistence);
		xheaptup->disk_tuple->urec = new_urecptr;
	}
	
	undorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);

	/* 
	 * Tuple in undo record must have no lock flag because it has no locker xid in undo. 
	 * Clear the lock status and locker_xid of old data before insert tuple into undo log 
	 * in in-place update case. For non in place update, we will not write old tuple 
	 * into undo. 
	 */
	infomask_old_tuple = oldtup.disk_tuple->flag;
	oldtup.disk_tuple->flag &= ~XHEAP_LOCK_STATUS_MASK;
	if (use_inplace_update)
	{
		appendBinaryStringInfo(GetUndoRecordRawdata(undorec), (char *) &(oldtup.disk_tuple->t_hoff),
							   sizeof(uint8));
		appendBinaryStringInfo(GetUndoRecordRawdata(undorec),
							   (char *) oldtup.disk_tuple + OffsetDataHeader,
							   oldtup.disk_tuple->t_hoff - OffsetDataHeader);
		/* write XOR detla into the undo raw data */
		appendBinaryStringInfo(GetUndoRecordRawdata(undorec), (char *) &xor_delta_flags,
							   sizeof(uint8));
	}
	
	/* recover the disk tuple flag */
	oldtup.disk_tuple->flag = infomask_old_tuple;

	if (use_inplace_update)
	{
		infomask_old_tuple = infomask_new_tuple = XHEAP_INPLACE_UPDATED;
	}
	else
	{
		infomask_old_tuple = XHEAP_UPDATED;
		infomask_new_tuple = 0;
	}

	if (use_inplace_update)
	{
		xlog_xor_delta = (char *) palloc(undo_xor_delta_size);
	}

	/* No ereport(ERROR) from here till changes are logged */
	START_CRIT_SECTION();
	/*
     * A page can be pruned for non-inplace updates or inplace updates that
     * results in shorter tuples.  If this transaction commits, the tuple will
     * become DEAD sooner or later.  If the transaction finally aborts, the
     * subsequent page pruning will be a no-op and the hint will be cleared.
     */
	if (!use_inplace_update || (xheaptup->disk_tuple_size < oldtup.disk_tuple_size))
	{
		XPageSetPrunable(page, fxid);
	}

	/* oldtup should be pointing to right place in page */
	Assert(oldtup.disk_tuple == (XHeapDiskTuple) XPageGetRowData(page, lp));

	if (is_subxact)
		xheap_keep_subtxn_lock(oldtup.disk_tuple, subxid);
	else
		XHeapTupleHeaderClearAllLocker(oldtup.disk_tuple);
	
	oldtup.disk_tuple->flag &= ~XHEAP_VIS_STATUS_UNLOCK_MASK;
	oldtup.disk_tuple->flag |= infomask_old_tuple;
	xheap_tuple_set_modified_xid(&oldtup, fxid);

	/* keep the new tuple copy updated for the caller */
	xheaptup->disk_tuple->flag &= ~XHEAP_VIS_STATUS_MASK;
	xheaptup->disk_tuple->flag |= infomask_new_tuple;
	xheap_tuple_set_modified_xid(xheaptup, fxid);

	if (use_inplace_update)
	{

		Assert(buffer == newbuf);
		if (prefixlen > 0)
		{
			appendBinaryStringInfo(GetUndoRecordRawdata(undorec), (char *) &prefixlen,
								   sizeof(uint16));
		}

		if (suffixlen > 0)
		{
			appendBinaryStringInfo(GetUndoRecordRawdata(undorec), (char *) &suffixlen,
								   sizeof(uint16));
		}

		/* Do a XOR delta between the end of prefixlen and start of suffixlen */
		appendBinaryStringInfo(GetUndoRecordRawdata(undorec), oldp + prefixlen,
							   oldlen - prefixlen - suffixlen);
		Assert(xlog_xor_delta != NULL);
		memcpy(xlog_xor_delta, GetUndoRecordRawdata(undorec)->data, undo_xor_delta_size);

		if (undo_xor_delta_size != GetUndoRecordRawdataLen(undorec))
		{
			ereport(
				PANIC,
				(errmsg("xid %lu, oid %u, ctid(%u, %u). "
						"xor data mismatch in undo and xlog, undo size %d, xlog size %d.",
						fxid.value, oldtup.table_oid, ItemPointerGetOffsetNumber(otid),
						ItemPointerGetBlockNumber(otid), GetUndoRecordRawdataLen(undorec),
						undo_xor_delta_size)));
		}

		put_inplace_update_xtuple(&oldtup, xheaptup, lp);
		ItemPointerCopy(&oldtup.ctid, &xheaptup->ctid);
	}
	else
	{
		UnpackedUndoRecord *oldUndoRec;
		UnpackedUndoRecord *newundorec;
#ifdef USE_ASSERT_CHECKING
		check_tuple_validity(relation, xheaptup);
#endif

		/* insert tuple at new location */
		relation_put_xtuple(relation, newbuf, xheaptup);

		/* Update the UnpackedUndoRecord now that we know where the tuple is located on the Page */
		newundorec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 1);
		Assert(GetUndoRecordBlkno(newundorec) ==
			   ItemPointerGetBlockNumber(&(xheaptup->ctid)));
		SetUndoRecordOffset(newundorec, ItemPointerGetOffsetNumber(&(xheaptup->ctid)));


		oldUndoRec = prepare_buffers_get_undorecord(undo_cache_ctx->undo_prepare_buffers, 0);
		appendBinaryStringInfo(GetUndoRecordRawdata(oldUndoRec), (char *) &(xheaptup->ctid),
							   sizeof(ItemPointerData));

		/* update the potential freespace */
		xheap_record_potential_free_space(buffer, SHORTALIGN(oldtupsize));
		xheap_record_potential_free_space(newbuf, -1 * SHORTALIGN(newtupsize));
	}

	insert_prepared_undo(undo_cache_ctx->undo_prepare_buffers, 0);

	if (!use_inplace_update && newbuf != buffer)
		MarkBufferDirty(newbuf);

	MarkBufferDirty(buffer);
	update_undolog_meta(fxid, prepare_buffers_get_first_undoptr(undo_cache_ctx->undo_prepare_buffers),
					 &xlum, persistence, prepare_buffers_get_last_undoptr(undo_cache_ctx->undo_prepare_buffers),
					 prepare_buffers_get_last_recordsize(undo_cache_ctx->undo_prepare_buffers));
	/* XLOG stuff */
	if (RelationNeedsWAL(relation))
	{
		uint8		   old_xl_undo_header_flag = 0;
		uint8		   new_xl_undo_header_flag = 0;
		UnpackedUndoRecord	  *oldurec;
		UnpackedUndoRecord	  *newurec;
		FullTransactionId  current_xid = InvalidFullTransactionId;
		char		  *logoldp = NULL;
		char		  *lognewp = NULL;
		int			   logoldlen = 0;
		int			   lognewlen = 0;
		int			   bufflags = REGBUF_STANDARD;
		int            obufflags = REGBUF_STANDARD;
		uint32		   old_tup_len = 0;
		Page		   logpage = NULL;
		uint8		   info = XLOG_XHEAP_UPDATE;
		uint16		   logprefixlen = 0;
		uint16		   logsuffixlen = 0;
		XHeapTuple	   difftup = NULL;
		XHeapDiskTuple logoldtup = NULL;
		XHeapTuple	   inplacetup = NULL;
		XHeapTuple	   noninplacenewtup = NULL;
		xl_xheap_update  xlrec;
		XLogRecPtr	   recptr = InvalidXLogRecPtr;
		xl_xheap_header  oldXlhdr;
		xl_xheap_header  newXlhdr;
		xl_undo_header   xlundohdr;
		xl_undo_header   xlnewundohdr;
		uint32		   toastLen = 0;
		XHeapDiskTuple toastTup = NULL;
		xl_xheap_header  toastxlhdr;
		UndoLogControl *ulog;

		upbuffers = undo_cache_ctx->undo_prepare_buffers;
		oldurec = prepare_buffers_get_undorecord(upbuffers, 0);
		newurec = prepare_buffers_get_undorecord(upbuffers, 1);
		ulog = (UndoLogControl *) undo_sys_ctx->ulogs[undo_log_ctx->logs[persistence]];

		if ((GetUndoRecordUinfo(oldurec) & UREC_INFO_PREURP) != 0)
		{
			old_xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_PREV_URP;
		}
		if ((GetUndoRecordUinfo(newurec) & UREC_INFO_PREURP) != 0)
		{
			new_xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_PREV_URP;
		}
		if (prev_urecptr != INVALID_UNDO_REC_PTR)
		{
			old_xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_BLK_PREV;
		}
		if ((GetUndoRecordUinfo(oldurec) & UREC_INFO_SUBXACT) != 0)
		{
			old_xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_TOP_XID;
			current_xid = GetTopFullTransactionIdIfAny();
		}
		if (RelationIsLogicallyLogged(relation) && XHeapTupleHasExternal(&oldtup))
		{
			old_xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_TOAST;
		}

		Assert(undotup.data);
		logoldtup = (XHeapDiskTupleData *) undotup.data;
		old_tup_len = undotup.len;
		inplacetup = &oldtup;
		noninplacenewtup = xheaptup;
		if (use_inplace_update)
		{
			/*
			* For inplace updates the old tuple is in undo record and the new
			* tuple is replaced in page where old tuple was present.
			*/
			logoldp = (char *) logoldtup + logoldtup->t_hoff;
			logoldlen = old_tup_len - logoldtup->t_hoff;
			lognewp = (char *) inplacetup->disk_tuple + inplacetup->disk_tuple->t_hoff;
			lognewlen = inplacetup->disk_tuple_size - inplacetup->disk_tuple->t_hoff;

			difftup = inplacetup;
		}
		else if (buffer == newbuf)
		{
			logoldp = (char *) inplacetup->disk_tuple + inplacetup->disk_tuple->t_hoff;
			logoldlen = inplacetup->disk_tuple_size - inplacetup->disk_tuple->t_hoff;
			lognewp =
				(char *) noninplacenewtup->disk_tuple + noninplacenewtup->disk_tuple->t_hoff;
			lognewlen = noninplacenewtup->disk_tuple_size - noninplacenewtup->disk_tuple->t_hoff;

			difftup = noninplacenewtup;
		}
		else
		{
			difftup = noninplacenewtup;
		}

		/*
		* If the old and new tuple are on the same page, we only need to log the
		* parts of the new tuple that were changed.  That saves on the amount of
		* WAL we need to write.  Currently, we just count any unchanged bytes in
		* the beginning and end of the tuple.  That's quick to check, and
		* perfectly covers the common case that only one field is updated.
		*
		* We could do this even if the old and new tuple are on different pages,
		* but only if we don't make a full-page image of the old page, which is
		* difficult to know in advance.  Also, if the old tuple is corrupt for
		* some reason, it would allow the corruption to propagate the new page,
		* so it seems best to avoid.  Under the general assumption that most
		* updates tend to create the new tuple version on the same page, there
		* isn't much to be gained by doing this across pages anyway.
		*
		* Skip this if we're taking a full-page image of the new page, as we
		* don't include the new tuple in the WAL record in that case.  Also
		* disable if wal_level='logical', as logical decoding needs to be able to
		* read the new tuple in whole from the WAL record alone.
		*/
		if (buffer == newbuf &&
			!XLogCheckBufferNeedsBackup(newbuf) &&
			!RelationIsLogicallyLogged(relation))
		{
			if (use_inplace_update)
			{
				logprefixlen = prefixlen;
				logsuffixlen = suffixlen;
			}
			else
			{
				int logminlen = Min(logoldlen, lognewlen);

				Assert(logoldp != NULL && lognewp != NULL);

				/* Check for common prefix between undo and old tuple */
				for (logprefixlen = 0; logprefixlen < logminlen; logprefixlen++)
				{
					if (logoldp[logprefixlen] != lognewp[logprefixlen])
					{
						break;
					}
				}

				/*
				* Storing the length of the prefix takes 2 bytes, so we need to save
				* at least 3 bytes or there's no point.
				*/
				if (logprefixlen < NUM_BLOCKS_FOR_NON_INPLACE_UPDATES)
				{
					logprefixlen = 0;
				}

				/* Same for suffix */
				for (logsuffixlen = 0; logsuffixlen < logminlen - logprefixlen; logsuffixlen++)
				{
					if (logoldp[logoldlen - logsuffixlen - 1] != lognewp[lognewlen - logsuffixlen - 1])
						break;
				}

				if (logsuffixlen < NUM_BLOCKS_FOR_NON_INPLACE_UPDATES)
				{
					logsuffixlen = 0;
				}
			}
		}

		/*
		* Store the information required to generate undo record during replay.
		*/
		xlundohdr.relOid = rel_oid;
		xlundohdr.urecptr = urecptr;
		xlundohdr.flag = old_xl_undo_header_flag;

		xlrec.old_offnum = ItemPointerGetOffsetNumber(&inplacetup->ctid);
		xlrec.new_offnum = ItemPointerGetOffsetNumber(&difftup->ctid);
		xlrec.old_tuple_flag = inplacetup->disk_tuple->flag;
		xlrec.flags = 0;
		xlrec.oldxid = txactinfo.xid;

		if (logprefixlen > 0)
			xlrec.flags |= XLZ_UPDATE_PREFIX_FROM_OLD;

		if (logsuffixlen > 0)
			xlrec.flags |= XLZ_UPDATE_SUFFIX_FROM_OLD;

		if (RelationIsLogicallyLogged(relation))
		{
			xlrec.flags |= XLOG_XHEAP_CONTAINS_OLD_HEADER;
			bufflags |= REGBUF_KEEP_DATA;
			obufflags |= REGBUF_KEEP_DATA;
		}

		if (!use_inplace_update)
		{
			logpage = BufferGetPage(newbuf);

			xlrec.flags |= XLZ_NON_INPLACE_UPDATE;

			xlnewundohdr.relOid = rel_oid;
			xlnewundohdr.urecptr = new_urecptr;
			xlnewundohdr.flag = new_xl_undo_header_flag;

			Assert(xheaptup);

			/* If new tuple is the single and first tuple on page... */
			if (ItemPointerGetOffsetNumber(&(xheaptup->ctid)) ==
					FirstOffsetNumber &&
				xheap_page_get_max_offset_number(logpage) == FirstOffsetNumber)
			{
				info |= XLOG_XHEAP_INIT_PAGE;
				bufflags |= REGBUF_WILL_INIT;
			}
		}

		xlrec.flags |= XLZ_HAS_UPDATE_UNDOTUPLE;
		xheap_fill_header(&oldXlhdr, logoldtup);

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, SizeOfXHeapUpdate);

		XLogRegisterData((char *) &xlundohdr, SizeOfXLUndoHeader);
		if ((old_xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
		{
			Assert(prev_urecptr != INVALID_UNDO_REC_PTR);
			XLogRegisterData((char *) &(prev_urecptr), sizeof(UndoRecPtr));
		}
		if ((old_xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
		{
			XLogRegisterData((char *) &(old_prev_urp), sizeof(UndoRecPtr));
		}
		if ((old_xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
		{
			XLogRegisterData((char *) &(current_xid), sizeof(FullTransactionId));
		}

		if ((old_xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_TOAST) != 0)
		{
			toastLen = old_key_tuple->disk_tuple_size;
			toastTup = (XHeapDiskTuple) old_key_tuple->disk_tuple;
			xheap_fill_header(&toastxlhdr, toastTup);
			toastLen -= SizeOfXHeapDiskTupleData - SizeOfXHeapHeader;
			XLogRegisterData((char *) &toastLen, sizeof(uint32));
			XLogRegisterData((char *) &toastxlhdr, SizeOfXHeapHeader);
			XLogRegisterData(
				(char *) old_key_tuple->disk_tuple + SizeOfXHeapDiskTupleData,
				toastLen - SizeOfXHeapHeader);
		}

		if (!use_inplace_update)
		{
			XLogRegisterData((char *) &xlnewundohdr, SizeOfXLUndoHeader);

			Assert(INVALID_UNDO_REC_PTR == INVALID_UNDO_REC_PTR);
			Assert(!(new_xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV));

			if ((new_xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
				XLogRegisterData((char *) &(old_prev_urp_insert), sizeof(UndoRecPtr));
		}

		xlog_undo_meta_write(&xlum);

		XLogRegisterBuffer(0, newbuf, bufflags);

		if (buffer != newbuf)
		{
			Assert(!use_inplace_update);
			XLogRegisterBuffer(1, buffer, obufflags);
		}

		if (xlrec.flags & XLZ_HAS_UPDATE_UNDOTUPLE)
		{
			if (!use_inplace_update)
			{
				XLogRegisterData((char *) &oldXlhdr, SizeOfXHeapHeader);
				XLogRegisterData((char *) logoldtup + SizeOfXHeapDiskTupleData,
								old_tup_len - SizeOfXHeapDiskTupleData);
			}
			else
			{
				XLogRegisterData((char *) &undo_xor_delta_size, sizeof(int));
				XLogRegisterData(xlog_xor_delta, undo_xor_delta_size);
				if ((xlrec.flags & XLOG_XHEAP_CONTAINS_OLD_HEADER) != 0)
				{
					XLogRegisterData((char *) &oldXlhdr, SizeOfXHeapHeader);
					XLogRegisterData((char *) logoldtup + SizeOfXHeapDiskTupleData,
									old_tup_len - SizeOfXHeapDiskTupleData);
				}
			}
		}

		/*
		* Prepare WAL data for the new tuple.
		*/
		if (!use_inplace_update)
		{
			if (logprefixlen > 0)
				XLogRegisterBufData(0, (char *) &logprefixlen, sizeof(uint16));

			if (logsuffixlen > 0)
				XLogRegisterBufData(0, (char *) &logsuffixlen, sizeof(uint16));
		}

		newXlhdr.flag2 = difftup->disk_tuple->flag2;
		newXlhdr.flag = difftup->disk_tuple->flag;
		newXlhdr.t_hoff = difftup->disk_tuple->t_hoff;
		Assert(SizeOfXHeapDiskTupleData + logprefixlen + logsuffixlen <= difftup->disk_tuple_size);

		XLogRegisterBufData(0, (char *) &newXlhdr, SizeOfXHeapHeader);
		if (logprefixlen == 0)
		{
			XLogRegisterBufData(
				0, ((char *) difftup->disk_tuple) + SizeOfXHeapDiskTupleData,
				difftup->disk_tuple_size - SizeOfXHeapDiskTupleData - logsuffixlen);
		}
		else
		{
			if (difftup->disk_tuple->t_hoff - SizeOfXHeapDiskTupleData > 0)
			{
				XLogRegisterBufData(0,
									((char *) difftup->disk_tuple) + SizeOfXHeapDiskTupleData,
									difftup->disk_tuple->t_hoff - SizeOfXHeapDiskTupleData);
			}

			/* data after common prefix */
			XLogRegisterBufData(
				0, ((char *) difftup->disk_tuple) + difftup->disk_tuple->t_hoff + logprefixlen,
				difftup->disk_tuple_size - difftup->disk_tuple->t_hoff - logprefixlen -
					logsuffixlen);
		}

		/* filtering by origin on a row level is much more efficient */
		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		xlog_register_undo_buffers(undo_cache_ctx->undo_prepare_buffers, 2, ulog);

		recptr = XLogInsert(RM_XHEAP_ID, info);

		if (newbuf != buffer)
			PageSetLSN(BufferGetPage(newbuf), recptr);

		PageSetLSN(BufferGetPage(buffer), recptr);
		prepare_buffers_set_page_lsn(undo_cache_ctx->undo_prepare_buffers, recptr);
		set_undolog_meta_lsn(recptr);
	}

	release_undolog_meta(persistence);
	if (use_inplace_update)
		pfree(xlog_xor_delta);

	END_CRIT_SECTION();
	/* be tidy */
	pfree(undotup.data);
	Assert(FullTransactionIdIsValid(xheaptup->disk_tuple->modified_xid));

	upbuffers = undo_cache_ctx->undo_prepare_buffers;


	newtup->xmin = XidFromFullTransactionId(fxid);

	reset_prepared_buffers_in_ctx();

	if (newbuf != buffer)
		UnlockReleaseBuffer(newbuf);

	UnlockReleaseBuffer(buffer);
	/*
	 * As of now, we only count non-inplace updates as hot update
	 */
	pgstat_count_heap_update(relation, !use_inplace_update, buffer != newbuf);

	if (have_tuple_lock)
		UnlockTuple(relation, &(oldtup.ctid), ExclusiveLock);

	if (xheaptup != newtup)
	{
		newtup->ctid = xheaptup->ctid;
		XHeapFreeTuple(xheaptup);
	}

	if (old_key_tuple != NULL && is_old_tuple_copied)
		XHeapFreeTuple(old_key_tuple);

	bms_free(inplace_upd_attrs);
	bms_free(interesting_attrs);
	bms_free(modified_attrs);
	bms_free(key_attrs);

	return TM_Ok;
}

/*
 * xheap_multi_insert        - insert multiple tuples into a xheap
 *
 * Similar to heap_multi_insert(), but inserts xheap tuples
 */
void
xheap_multi_insert(Relation relation, XHeapTuple *tuples, int ntuples, CommandId cid,
				   int options, BulkInsertState bistate)
{
	XHeapTuple			*xheaptuples = NULL;
	int					 i;
	int					 ndone;
	char				*scratch = NULL;
	Page				 page;
	Size				 save_free_space;
	UndoPersistence		 persistence = UndoPersistenceForRelation(relation);
	FullTransactionId		 fxid = GetTopFullTransactionId();
	UndoPrepareBuffers		*upbuffers = NULL;

	/* needwal can also be passed in by options */
	bool			  needwal = RelationNeedsWAL(relation);
	bool			  skip_undo = false;


	save_free_space = RelationGetTargetPageFreeSpace(relation, XHEAP_DEFAULT_FILLFACTOR);

	/* Toast and set header data in all the tuples */
	xheaptuples = (XHeapTupleData **) palloc(ntuples * sizeof(XHeapTuple));
	for (i = 0; i < ntuples; i++)
	{
		tuples[i] = xheap_prepare_insert(relation, tuples[i], 0);
		tuples[i]->xmin = XidFromFullTransactionId(fxid);
		xheaptuples[i] = tuples[i];
	}

	if (needwal)
	{
		scratch = (char *) palloc(BLCKSZ);
		memset(scratch, 0, BLCKSZ);
	}

	ndone = 0;

	while (ndone < ntuples)
	{
		xl_undo_meta xlum;
		Buffer		 buffer = InvalidBuffer;
		int			 nthispage = 0;
		UndoRecPtr	 urec_ptr = INVALID_UNDO_REC_PTR, prev_urecptr = INVALID_UNDO_REC_PTR,
				   first_urecptr = INVALID_UNDO_REC_PTR;
		XHeapFreeOffsetRanges *xfree_offset_ranges = NULL;
		UndoRecPtr			   old_prev_urp;
		UndoRecPtr			   *urpvec = NULL;

		CHECK_FOR_INTERRUPTS();

		init_xlog_undo_meta(&xlum);

		buffer = relation_get_buffer_for_xtuple(relation, xheaptuples[ndone]->disk_tuple_size,
											InvalidBuffer, options, bistate);
		page = BufferGetPage(buffer);

		/*
         * Get the unused offset ranges in the page. This is required for
         * deciding the number of undo records to be prepared later.
         */
		xfree_offset_ranges = xheap_get_usable_offset_ranges(buffer, &xheaptuples[ndone],
													   ntuples - ndone, save_free_space);

		/*
         * We've ensured at least one tuple fits in the page. So, there'll be
         * at least one offset range.
         */
		Assert(xfree_offset_ranges->nranges > 0);

		if (!skip_undo)
		{
			Oid relOid;
			relOid = RelationGetRelid(relation);

			urec_ptr = xheap_prepare_undo_multi_insert(
				relOid, RelationGetRelFileLocator(relation), RelationGetRnodeSpace(relation),
				persistence, buffer, xfree_offset_ranges->nranges, fxid, cid, prev_urecptr,
				INVALID_UNDO_REC_PTR, &upbuffers, &first_urecptr, NULL, InvalidBlockNumber,
				NULL, NULL, &xlum);

			if (needwal)
			{
				/* 1 xheap buffer + 1 undo slot buffer */
				int	used_buffers = prepare_buffers_used_count(upbuffers) + 2;
				if(used_buffers - 1 > XLR_NORMAL_MAX_BLOCK_ID)
					XLogEnsureRecordSpace(used_buffers - 1, 0);
			}
		}

		/* init string info before critical section */
		for (i = 0; i < xfree_offset_ranges->nranges; i++)
		{
			if (!skip_undo)
			{
				MemoryContext old_cxt =
					MemoryContextSwitchTo(prepare_buffers_get_undorecord(upbuffers, i)->mem_ctx);
				initStringInfo(GetUndoRecordRawdata(prepare_buffers_get_undorecord(upbuffers, i)));
				MemoryContextSwitchTo(old_cxt);
			}
		}

		urpvec = (UndoRecPtr *) palloc(xfree_offset_ranges->nranges * sizeof(UndoRecPtr));

		/* No ereport(ERROR) from here till changes are logged */
		START_CRIT_SECTION();

		nthispage = 0;
		for (i = 0; i < xfree_offset_ranges->nranges; i++)
		{
			OffsetNumber offnum;

			if (!skip_undo)		
				urpvec[i] = prepare_buffers_get_undorecord(upbuffers, i)->uur_urp;

			for (offnum = xfree_offset_ranges->startOffset[i];
				 offnum <= xfree_offset_ranges->endOffset[i]; offnum++)
			{
				XHeapTuple xheaptup;
				Size	   pagefreespace;
				bool	   isFirstInsert;

				if (ndone + nthispage == ntuples)
					break;

				xheaptup = xheaptuples[ndone + nthispage];

				/* Make sure that the tuple fits in the page. */
				pagefreespace = page_get_xheap_free_space(page);
				isFirstInsert = (offnum == xfree_offset_ranges->startOffset[0]);
				if ((isFirstInsert && pagefreespace < xheaptup->disk_tuple_size) ||
					(!isFirstInsert &&
					 (pagefreespace < xheaptup->disk_tuple_size + save_free_space)))
				{
					break;
				}

				if (!skip_undo)
				{
					XHeapTupleHeaderSetUndoRecPtr(xheaptup->disk_tuple, urpvec[i]);
				}

				xheap_tuple_set_modified_xid(xheaptup, fxid);

#ifdef USE_ASSERT_CHECKING
				check_tuple_validity(relation, xheaptup);
#endif
				relation_put_xtuple(relation, buffer, xheaptup);

				xheap_record_potential_free_space(buffer,
											  -1 * SHORTALIGN(xheaptup->disk_tuple_size));

				/*
                 * Let's make sure that we've decided the offset ranges
                 * correctly.
                 */
				Assert(offnum == ItemPointerGetOffsetNumber(&(xheaptup->ctid)));

				nthispage++;
			}

			/*
             * Store the offset ranges in undo payload. We've not calculated
             * the end offset for the last range previously. Hence, we set it
             * to offnum - 1. There is no harm in doing the same for previous
             * undo records as well.
             */
			xfree_offset_ranges->endOffset[i] = offnum - 1;
			if (!skip_undo)
			{
				StringInfoData *  rdata =  GetUndoRecordRawdata(prepare_buffers_get_undorecord(upbuffers, i));
				appendBinaryStringInfo(rdata,
									   (char *) &xfree_offset_ranges->startOffset[i],
									   sizeof(OffsetNumber));
				appendBinaryStringInfo(rdata,
									   (char *) &xfree_offset_ranges->endOffset[i],
									   sizeof(OffsetNumber));
			}

			elog(DEBUG1, "start offset: %d, end offset: %d",
				 xfree_offset_ranges->startOffset[i], xfree_offset_ranges->endOffset[i]);
		}

		old_prev_urp = get_current_tansaction_undorec_ptr(persistence);

		if (!skip_undo)
		{
			/* Insert the undo */
			insert_prepared_undo(upbuffers, 0);

			for (i = 0; i < xfree_offset_ranges->nranges; i++)
			{
				UndoRecPtr urecptr = prepare_buffers_get_undorecord(upbuffers, i)->uur_urp;
				Assert(IS_VALID_UNDO_REC_PTR(urecptr));
				set_current_tansaction_undorec_ptr(urecptr, persistence);
			}

			/*
             * We're sending the undo record for debugging purpose. So, just
             * send the last one.
             */
			update_undolog_meta(fxid, prepare_buffers_get_first_undoptr(upbuffers), &xlum, persistence, prepare_buffers_get_last_undoptr(upbuffers),
							prepare_buffers_get_last_recordsize(upbuffers));
		}

		MarkBufferDirty(buffer);

		/* XLOG stuff */
		if (needwal)
		{
			uint8					xl_undo_header_flag = 0;
			UnpackedUndoRecord		*urec = prepare_buffers_get_undorecord(upbuffers, 0);
			xl_undo_header		xlundohdr = {0};
			XLogRecPtr			recptr;
			XlXHeapMultiInsert *xlrec;
			uint8				info = XLOG_XHEAP_MULTI_INSERT;
			char			   *tupledata;
			char			   *scratchptr = scratch;
			int					nranges = xfree_offset_ranges->nranges;
			int					bufflags = 0, 
								totaldatalen;
			bool				init;
			Page				logpage = BufferGetPage(buffer);
			UndoLogControl     *ulog;


			if ((GetUndoRecordUinfo(urec) & UREC_INFO_PREURP) != 0)
			{
				xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_PREV_URP;
			}
			if (prev_urecptr != INVALID_UNDO_REC_PTR)
			{
				xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_BLK_PREV;
			}

			if (IsSubTransaction())
			{
				xl_undo_header_flag |= XLOG_UNDO_HEADER_HAS_TOP_XID;
			}

			ulog = (UndoLogControl *) undo_sys_ctx->ulogs[undo_log_ctx->logs[persistence]];

			/*
			* Store the information required to generate undo record during replay.
			* All undo records have same information apart from the payload data.
			* Hence, we can copy the same from the last record.
			*/
			xlundohdr
				.relOid = RelationGetRelid(relation);
			xlundohdr.urecptr = first_urecptr;
			xlundohdr.flag = xl_undo_header_flag;

			/* allocate XlXHeapMultiInsert struct from the scratch area */
			xlrec = (XlXHeapMultiInsert *) scratchptr;
			if (skip_undo)
				xlrec->flags |= XLZ_INSERT_IS_FROZEN;
			xlrec->ntuples = nthispage;
			scratchptr += SizeOfXHeapMultiInsert;

			/* copy the offset ranges as well */
			memcpy((char *) scratchptr, (char *) &nranges, sizeof(int));
			scratchptr += sizeof(int);

			memcpy((char *) scratchptr, (char *) urpvec, sizeof(UndoRecPtr) * nranges);
			scratchptr += sizeof(UndoRecPtr) * nranges;

			memcpy((char *) scratchptr, (char *) &xfree_offset_ranges->startOffset[0],
				(sizeof(OffsetNumber) * nranges));
			scratchptr += (sizeof(OffsetNumber) * nranges);

			memcpy((char *) scratchptr, (char *) &xfree_offset_ranges->endOffset[0],
				(sizeof(OffsetNumber) * nranges));
			scratchptr += (sizeof(OffsetNumber) * nranges);

			/* the rest of the scratch space is used for tuple data */
			tupledata = scratchptr;

			if (RelationIsLogicallyLogged(relation))
			{
				xlrec->flags |= XLOG_XHEAP_CONTAINS_NEW_TUPLE;
				bufflags |= REGBUF_KEEP_DATA;
			}

			/*
			* Write out an xl_multi_insert_tuple and the tuple data itself for each
			* tuple.
			*/
			for (i = 0; i < nthispage; i++)
			{
				XHeapTuple			 xheaptup = xheaptuples[ndone + i];
				XlMultiInsertXTuple *tuphdr;
				int					 datalen;

				/* xl_multi_insert_tuple needs two-byte alignment. */
				tuphdr = (XlMultiInsertXTuple *) (scratchptr);
				scratchptr = ((char *) tuphdr) + SizeOfMultiInsertXTuple;
				tuphdr->xid = xheaptup->disk_tuple->modified_xid;
				tuphdr->flag = xheaptup->disk_tuple->flag;
				tuphdr->flag2 = xheaptup->disk_tuple->flag2;
				tuphdr->t_hoff = xheaptup->disk_tuple->t_hoff;

				/* write bitmap [+ padding] [+ oid] + data */
				datalen = xheaptup->disk_tuple_size - SizeOfXHeapDiskTupleData;
				memcpy(scratchptr, (char *) xheaptup->disk_tuple + SizeOfXHeapDiskTupleData, datalen);
				tuphdr->datalen = datalen;
				scratchptr += datalen;
			}
			totaldatalen = scratchptr - tupledata;
			Assert((scratchptr - scratch) < BLCKSZ);

			if (RelationIsLogicallyLogged(relation) &&
				ndone + nthispage == ntuples)
			{
				xlrec->flags |= XLOG_XHEAP_INSERT_LAST_IN_MULTI;
			}

			init = (ItemPointerGetOffsetNumber(&(
						xheaptuples[ndone]->ctid)) == FirstOffsetNumber &&
					xheap_page_get_max_offset_number(logpage) ==
						FirstOffsetNumber + nthispage - 1);
			if (init)
			{
				info |= XLOG_XHEAP_INIT_PAGE;
				bufflags |= REGBUF_WILL_INIT;
			}

			XLogBeginInsert();

			XLogRegisterData((char *) &xlundohdr, SizeOfXLUndoHeader);
			if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_BLK_PREV) != 0)
			{
				Assert(prev_urecptr != INVALID_UNDO_REC_PTR);
				XLogRegisterData((char *) &(prev_urecptr),
								sizeof(UndoRecPtr));
			}
			if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_PREV_URP) != 0)
			{
				XLogRegisterData((char *) &(old_prev_urp),
								sizeof(UndoRecPtr));
			}
			if ((xl_undo_header_flag & XLOG_UNDO_HEADER_HAS_TOP_XID) != 0)
			{
				XLogRegisterData((char *) &(fxid), sizeof(FullTransactionId));
			}

			XLogRegisterData((char *) &(urec_ptr),
							sizeof(urec_ptr));
			xlog_undo_meta_write(&xlum);


			/* copy xl_multi_insert_tuple in maindata */
			XLogRegisterData((char *) xlrec, tupledata - scratch);

			XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | bufflags);

			/* copy tuples in block data */
			XLogRegisterBufData(0, tupledata, totaldatalen);

			xlog_register_undo_buffers(upbuffers, 1, ulog);

			/* filtering by origin on a row level is much more efficient */
			XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

			recptr = XLogInsert(RM_XHEAP_ID, info);

			PageSetLSN(logpage, recptr);
			if (!skip_undo)
			{
				prepare_buffers_set_page_lsn(upbuffers, recptr);
				set_undolog_meta_lsn(recptr);
			}
		}

		if (!skip_undo)
		{
			release_undolog_meta(persistence);
		}

		END_CRIT_SECTION();

		pfree(xfree_offset_ranges);
		pfree(urpvec);
		UnlockReleaseBuffer(buffer);
		if (!skip_undo)
		{
			reset_undo_prepare_buffers(upbuffers, true);
			reset_prepared_buffers_in_ctx();
			release_undo_prepare_buffers(upbuffers);
		}

		ndone += nthispage;
		options &= ~XHEAP_INSERT_EXTEND;
	}

	for (i = 0; i < ntuples; i++)
		tuples[i]->ctid = xheaptuples[i]->ctid;

	pgstat_count_heap_insert(relation, ntuples);
}

CommandId
xheap_tuple_get_cid(XHeapTuple xtuple, Buffer buffer)
{
	UnpackedUndoRecord		  *urec;
	UndoTraversalState rc;
	CommandId		   current_cid;

	if (TransactionIdOlderThanAllUndo(xtuple->disk_tuple->modified_xid))
		return InvalidCommandId;

	Assert(IS_VALID_UNDO_REC_PTR(xtuple->disk_tuple->urec));
	urec = new_undo_record();
	reset_undo_record(urec, xtuple->disk_tuple->urec);

	rc = fetch_undo_record(
		urec, InvalidFullTransactionId, false, NULL,satisfy_undo_record);

	if (rc != UNDO_TRAVERSAL_COMPLETE)
	{
		destroy_undo_record(urec);
		return InvalidCommandId;
	}

	current_cid = GetUndoRecordCid(urec);

	destroy_undo_record(urec);

	return current_cid;
}



UndoRecPtr
xheap_prepare_undo_insert(Oid relOid, Oid relfilenode, Oid tablespace,
						  UndoPersistence persistence, FullTransactionId xid, CommandId cid,
						  UndoRecPtr prevurp_in_one_tup, UndoRecPtr prevurp_in_one_xact,
						  BlockNumber blk, XLogReaderState *xlog_record, xl_undo_header *xlundohdr,
						  xl_undo_meta *xlundometa)
{
	int			   status = 0;
	UndoRecPtr	   urecptr;
	UndoPrepareBuffers *xrecvec = undo_cache_ctx->undo_prepare_buffers;
	UnpackedUndoRecord	  *urec = prepare_buffers_get_undorecord(xrecvec, 0);
	Assert(tablespace != InvalidOid);
	SetUndoRecordUtype(urec, UNDO_INSERT);
	SetUndoRecordXid(urec, xid);
	SetUndoRecordCid(urec, cid);
	SetUndoRecordReloid(urec, relOid);
	SetUndoRecordTpprev(urec, prevurp_in_one_tup);
	SetUndoRecordRelfilenode(urec, relfilenode);
	SetUndoRecordTablespace(urec, tablespace);
	SetUndoRecordBlkno(urec, blk);
	SetUndoRecordOffset(urec, InvalidOffsetNumber);
	SetUndoRecordPrevurp(urec, InRecovery ? prevurp_in_one_xact
										  : get_current_tansaction_undorec_ptr(persistence));
	urec->is_update = true;
	
	SetUndoRecordOldXactId(urec, InvalidFullTransactionId);
	status = prepare_undo(xrecvec, persistence, xlog_record, xlundohdr, xlundometa);

	if (status != UNDO_PREPARE_SUCC)
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("Failed to generate UnpackedUndoRecord")));
	}

	urecptr = urec->uur_urp;
	Assert(IS_VALID_UNDO_REC_PTR(urecptr));

	return urecptr;
}

UndoRecPtr
xheap_prepare_undo_multi_insert(Oid relOid, Oid relfilenode, Oid tablespace,
								UndoPersistence persistence, Buffer buffer, int nranges,
								FullTransactionId xid, CommandId cid,
								UndoRecPtr prevurp_in_one_tup, UndoRecPtr prevurp_in_one_xact,
								UndoPrepareBuffers **upbuffers_ptr, UndoRecPtr *first_urecptr,
								UndoRecPtr *urpvec, BlockNumber blk, XLogReaderState *xlog_record,
								xl_undo_header *xlundohdr, xl_undo_meta *xlundometa)
{
	UnpackedUndoRecord	  *undo_record = NULL;
	int			   i = 0;
	int			   status;
	UndoRecPtr	   urecptr;
	UndoRecPtr	   prevurp;
	UndoPrepareBuffers *upbuffers = new_undo_prepare_buffers(nranges);

	Assert(tablespace != InvalidOid);
	for (i = 0; i < nranges; i++)
	{
		undo_record = new_undo_record();
		SetUndoRecordUtype(undo_record, UNDO_MULTI_INSERT);
		SetUndoRecordUinfo(undo_record, UREC_INFO_PAYLOAD);
		SetUndoRecordXid(undo_record, xid);
		SetUndoRecordCid(undo_record, cid);
		SetUndoRecordReloid(undo_record, relOid);
		SetUndoRecordTpprev(undo_record, prevurp_in_one_tup);
		SetUndoRecordRelfilenode(undo_record, relfilenode);
		SetUndoRecordTablespace(undo_record, tablespace);

		if (InRecovery)
		{
			SetUndoRecordBlkno(undo_record, blk);
		}
		else
		{
			if (BufferIsValid(buffer))
			{
				SetUndoRecordBlkno(undo_record, BufferGetBlockNumber(buffer));
			}
			else
			{
				SetUndoRecordBlkno(undo_record, InvalidBlockNumber);
			}
		}
		SetUndoRecordOffset(undo_record, InvalidOffsetNumber);
		SetUndoRecordPrevurp(
			undo_record,
			InRecovery ? prevurp_in_one_xact : get_current_tansaction_undorec_ptr(persistence));
		SetUndoRecordOldXactId(undo_record, InvalidFullTransactionId);
		GetUndoRecordRawdata(undo_record)->len = 2 * sizeof(OffsetNumber);
		undo_record->uur_urp = INVALID_UNDO_REC_PTR;
		undo_record->is_update = true;

		if (InRecovery)
		{
			Assert(urpvec && IS_VALID_UNDO_REC_PTR(urpvec[i]));
			undo_record->uur_urp = urpvec[i];
		}
		prepare_buffers_add_record(upbuffers, undo_record);
	}

	status = prepare_undo(upbuffers, persistence, xlog_record, xlundohdr, xlundometa);
	if (status != UNDO_PREPARE_SUCC)
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("Failed to generate UnpackedUndoRecord")));
	}

	urecptr = prevurp_in_one_tup;
	prevurp =
		InRecovery ? prevurp_in_one_xact : get_current_tansaction_undorec_ptr(persistence);
	for (i = 0; i < nranges; i++)
	{
		undo_record = prepare_buffers_get_undorecord(upbuffers, i);
		SetUndoRecordTpprev(undo_record, INVALID_UNDO_REC_PTR);
		SetUndoRecordPrevurp(undo_record, prevurp);

		urecptr = undo_record->uur_urp;
		prevurp = undo_record->uur_urp;

		Assert(IS_VALID_UNDO_REC_PTR(urecptr));
	}

	if (!InRecovery)
	{
		Assert(first_urecptr && !IS_VALID_UNDO_REC_PTR(*first_urecptr));
		undo_record = prepare_buffers_get_undorecord(upbuffers, 0);
		*first_urecptr = undo_record->uur_urp;
	}
	*upbuffers_ptr = upbuffers;
	return urecptr;
}

UndoRecPtr
xheap_prepare_undo_delete(Oid rel_oid, Oid relfilenode, Oid tablespace,
					   UndoPersistence persistence, Buffer buffer, OffsetNumber offnum,
					   FullTransactionId xid, FullTransactionId subxid, CommandId cid,
					   UndoRecPtr prevurp_in_one_blk, UndoRecPtr prevurp_in_one_xact, FullTransactionId xactid,
					   XHeapTuple oldtuple, BlockNumber blk, XLogReaderState *xlog_record, xl_undo_header *xlundohdr,
					   xl_undo_meta *xlundometa)
{
	UndoPrepareBuffers *upbuffers = undo_cache_ctx->undo_prepare_buffers;
	UnpackedUndoRecord	  *urec = prepare_buffers_get_undorecord(upbuffers, 0);
	MemoryContext  old_cxt;
	int			   status;
	UndoRecPtr	   urecptr;

	Assert(tablespace != InvalidOid);

	SetUndoRecordUtype(urec, UNDO_DELETE);
	SetUndoRecordXid(urec, xid);
	SetUndoRecordCid(urec, cid);
	SetUndoRecordReloid(urec, rel_oid);

	SetUndoRecordTpprev(urec, prevurp_in_one_blk);
	SetUndoRecordRelfilenode(urec, relfilenode);

	SetUndoRecordTablespace(urec, tablespace);
	SetUndoRecordUinfo(urec, UREC_INFO_DATAHEADER);
	SetUndoRecordDataHeaderFlag(urec,oldtuple->disk_tuple->flag);

	if (InRecovery)
	{
		SetUndoRecordBlkno(urec, blk);
	}
	else
	{
		if (BufferIsValid(buffer))
		{
			SetUndoRecordBlkno(urec, BufferGetBlockNumber(buffer));
		}
		else
		{
			SetUndoRecordBlkno(urec, InvalidBlockNumber);
		}
	}

	SetUndoRecordOffset(urec, offnum);
	SetUndoRecordPrevurp(urec, InRecovery ? prevurp_in_one_xact
										  : get_current_tansaction_undorec_ptr(persistence));
	SetUndoRecordOldXactId(urec, xactid);
	urec->is_update = true;

	/* Copy over the entire tuple data to the undorecord */
	old_cxt = MemoryContextSwitchTo(urec->mem_ctx);
	initStringInfo(GetUndoRecordRawdata(urec));
	MemoryContextSwitchTo(old_cxt);

	if (TransactionIdIsValid(subxid.value))
	{
		SetUndoRecordUinfo(urec, UREC_INFO_SUBXACT);
		SetUndoRecordSubXid(urec,subxid);
	}

	status = prepare_undo(upbuffers, persistence, xlog_record, xlundohdr, xlundometa);
	if (status != UNDO_PREPARE_SUCC)
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("Failed to generate UnpackedUndoRecord")));
	}

	urecptr = urec->uur_urp;
	Assert(IS_VALID_UNDO_REC_PTR(urecptr));

	return urecptr;
}

static void
populate_xl_xheap_header(xl_xheap_header *xlhdr, const XHeapDiskTuple diskTuple)
{
	xlhdr->flag2 = diskTuple->flag2;
	xlhdr->flag = diskTuple->flag;
	xlhdr->t_hoff = diskTuple->t_hoff;
}

static void
xheap_fill_header(xl_xheap_header *xlhdr, XHeapDiskTuple diskTup)
{
	xlhdr->flag = diskTup->flag;
	xlhdr->flag2 = diskTup->flag2;
	xlhdr->t_hoff = diskTup->t_hoff;
}

bool
xheap_exec_pending_undo_actions(Relation relation, Buffer buffer, FullTransactionId xid,  UndoRecPtr urp)
{
	UndoRecPtr slot_urec_ptr = urp;

	/* It's either we're the one applying our own undo actions
     * or we're trying to apply undo action of a completed transaction.
     */
	Assert(TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(xid)) || !xstore_transaction_id_is_in_progress(xid));
	/*
     * Apply Undo Actions if the transaction is aborted. 
     */
	if (FullTransactionIdIsValid(xid) && !xstore_transaction_id_is_in_progress(xid) &&
		!xstore_transaction_id_did_commit(xid))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		execute_undo_actions_tuple(slot_urec_ptr, relation, buffer, xid);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	}

	return true;
}

UndoRecPtr
xheap_prepare_undo_update(Oid rel_oid, Oid relfilenode, Oid tablespace,
						  UndoPersistence persistence, Buffer buffer, Buffer newbuffer,
						  OffsetNumber offnum, FullTransactionId xid, FullTransactionId subxid,
						  CommandId cid, UndoRecPtr prevurp_in_one_blk, UndoRecPtr prevurp_in_one_xact,
						  FullTransactionId old_updater_xid, XHeapTuple oldtuple, bool isInplaceUpdate,
						  UndoRecPtr *new_urec, int undo_xor_delta_size, BlockNumber oldblk,
						  BlockNumber newblk, XLogReaderState *xlog_record, xl_undo_header *xlundohdr, 
						  xl_undo_meta *xlundometa)
{
	UndoRecPtr	   urecptr = INVALID_UNDO_REC_PTR;
	UndoPrepareBuffers *upbuffers = undo_cache_ctx->undo_prepare_buffers;
	UnpackedUndoRecord	  *urec = prepare_buffers_get_undorecord(upbuffers, 0);
	UnpackedUndoRecord	  *urec_new = prepare_buffers_get_undorecord(upbuffers, 1);
	Size		   payload_len = 0;
	int			   status;
	MemoryContext  old_cxt;

	Assert(tablespace != InvalidOid);
	Assert(!InRecovery || (oldblk != InvalidBlockNumber && newblk != InvalidBlockNumber));

	SetUndoRecordXid(urec, xid);
	SetUndoRecordCid(urec, cid);
	SetUndoRecordReloid(urec, rel_oid);
	SetUndoRecordRelfilenode(urec, relfilenode);
	SetUndoRecordTablespace(urec, tablespace);
	SetUndoRecordTpprev(urec, prevurp_in_one_blk);
	SetUndoRecordBlkno(urec, InRecovery ? oldblk : BufferGetBlockNumber(buffer));
	SetUndoRecordOffset(urec, offnum);
	SetUndoRecordPrevurp(urec, InRecovery ? prevurp_in_one_xact
										  : get_current_tansaction_undorec_ptr(persistence));
	urec->is_update = true;
	SetUndoRecordOldXactId(urec, old_updater_xid);

	if (isInplaceUpdate)
	{
		SetUndoRecordUtype(urec, UNDO_INPLACE_UPDATE);
		SetUndoRecordUinfo(urec, UREC_INFO_PAYLOAD);
	}
	else
	{
		SetUndoRecordUtype(urec, UNDO_UPDATE);
		SetUndoRecordUinfo(urec, UREC_INFO_PAYLOAD);

		// record the old tuple flag
		SetUndoRecordUinfo(urec, UREC_INFO_DATAHEADER);
	    SetUndoRecordDataHeaderFlag(urec,oldtuple->disk_tuple->flag);

		/* Prepare undo record for the new tuple */
		SetUndoRecordXid(urec_new, xid);
		SetUndoRecordCid(urec_new, cid);
		SetUndoRecordReloid(urec_new, rel_oid);
		SetUndoRecordRelfilenode(urec_new, relfilenode);
		SetUndoRecordTablespace(urec_new, tablespace);
		SetUndoRecordTpprev(urec_new, INVALID_UNDO_REC_PTR);
		SetUndoRecordBlkno(urec_new,
						   InRecovery ? newblk : BufferGetBlockNumber(newbuffer));
		SetUndoRecordOffset(urec_new, offnum);
		urec_new->is_update = true;
		SetUndoRecordUtype(urec_new, UNDO_INSERT);
		SetUndoRecordOldXactId(urec_new, xid);

		/* Non-inplace updates contains the ctid after the tuple data */
		payload_len += sizeof(ItemPointerData);
	}

	if (isInplaceUpdate)
		GetUndoRecordRawdata(urec)->len = undo_xor_delta_size;
	else
		GetUndoRecordRawdata(urec)->len = payload_len;

	if (FullTransactionIdIsValid(subxid))
	{ 
		SetUndoRecordUinfo(urec, UREC_INFO_SUBXACT);
		SetUndoRecordSubXid(urec, subxid);
	}

	/* Now allocate the Undo record with the correct size */
	status = prepare_undo(upbuffers, persistence, xlog_record, xlundohdr, xlundometa);

	if (status != UNDO_PREPARE_SUCC)
	{
		ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
						errmsg("Failed to generate UnpackedUndoRecord")));
	}

	urecptr = urec->uur_urp;
	Assert(IS_VALID_UNDO_REC_PTR(urecptr));

	old_cxt = MemoryContextSwitchTo(urec->mem_ctx);
	initStringInfo(GetUndoRecordRawdata(urec));
	if (isInplaceUpdate)
		enlargeStringInfo(GetUndoRecordRawdata(urec),undo_xor_delta_size + sizeof(uint16) * 2); //sizeof(uint16)*2 is space for prifixlen and suffixlen
	else
		enlargeStringInfo(GetUndoRecordRawdata(urec),payload_len);
	MemoryContextSwitchTo(old_cxt);

	/* Set the undo record for the new tuple in case of non-inplace update */
	if (!isInplaceUpdate)
	{
		if (new_urec != NULL)
		{
			*new_urec = urec_new->uur_urp;
			Assert(IS_VALID_UNDO_REC_PTR(*new_urec));
		}

		SetUndoRecordPrevurp(urec_new, urecptr);
	}

	return urecptr;
}

/*
 * xheap_abort_speculative
 *
 * This function is used to abort an insert right away.
 *
 */
void
xheap_abort_speculative(Relation relation, XHeapTuple xtuple)
{
	ItemPointer			tid = &xtuple->ctid;
	BlockNumber			blkno = ItemPointerGetBlockNumber(tid);
	OffsetNumber		offnum = ItemPointerGetOffsetNumber(tid);
	Buffer				buffer = InvalidBuffer;
	RowPtr			   *rp = NULL;
	XHeapDiskTuple		disk_tuple = NULL;
	UnpackedUndoRecord		   *urec = NULL;
	UndoTraversalState	rc = UNDO_TRAVERSAL_DEFAULT;
	Page				page = NULL;
	int					logno;
	int					nline;
	bool				need_page_init = true;

	buffer = ReadBuffer(relation, blkno);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buffer);
	rp = XPageGetRowPtr(page, offnum);
	disk_tuple = (XHeapDiskTuple) XPageGetRowData(page, rp);

	/* Fetch Undo record */
	urec = new_undo_record();
	urec->uur_urp = disk_tuple->urec;
	rc = fetch_undo_record(urec, disk_tuple->modified_xid, false,
						 NULL, satisfy_undo_record);
	if (rc != UNDO_TRAVERSAL_COMPLETE)
	{
		ereport(WARNING, (errmsg("fetch undo record failed, got undo state: %d", rc)));
	}

	/* the tuple cannot be all-visible because it's inserted by current transaction */
	Assert(rc != UNDO_TRAVERSAL_DEFAULT);
	Assert(GetUndoRecordUtype(urec) == UNDO_INSERT &&
		   GetUndoRecordOffset(urec) == offnum &&
		   FullTransactionIdEquals(GetUndoRecordXid(urec), disk_tuple->modified_xid));

	START_CRIT_SECTION();

	/* Apply undo action for an INSERT */
	execute_undo_insert(relation, buffer, GetUndoRecordOffset(urec), urec);

	nline = xheap_page_get_max_offset_number(page);

	for (int offset = FirstOffsetNumber; offset <= nline; offset++)
	{
		RowPtr *localRp = XPageGetRowPtr(page, offset);
		if (RowPtrIsUsed(localRp))
		{
			need_page_init = false;
			break;
		}
	}

	logno = (int) UNDO_PTR_GET_LOG_NO(urec->uur_urp);

	destroy_undo_record(urec);

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(relation))
	{
		uint8	   flags = 0;
		XLogRecPtr recptr;

		XlXHeapUndoAbortSpecInsert walinfo;

		walinfo.offset = ItemPointerGetOffsetNumber(&xtuple->ctid);
		walinfo.logno = logno;

		if (need_page_init)
		{
			flags |= XLU_ABORT_SPECINSERT_INIT_PAGE;
		}

		if (RelationGetForm(relation)->relhasindex)
		{
			flags |= XLU_ABORT_SPECINSERT_REL_HAS_INDEX;
		}

		XLogBeginInsert();

		XLogRegisterData((char *) &flags, sizeof(uint8));
		XLogRegisterData((char *) &walinfo, SizeOfXHeapUndoAbortSpecInsert);

		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
		recptr = XLogInsert(RM_XHEAPUNDO_ID, XLOG_XHEAPUNDO_ABORT_SPECINSERT);
		PageSetLSN(page, recptr);
	}

	if (need_page_init)
	{
		XLogRecPtr lsn = PageGetLSN(page);
		xpage_init(XPAGE_HEAP, page, BufferGetPageSize(buffer), XHEAP_SPECIAL_SIZE);
		PageSetLSN(page, lsn);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);

	return;
}

void
simple_xheap_delete(Relation relation, ItemPointer tid, Snapshot snapshot)
{
	TM_Result	   result;
	TM_FailureData tmfd;
	
	tmfd.oldslot = NULL;

	result = xheap_delete(relation, tid, GetCurrentCommandId(true), InvalidSnapshot,
						  snapshot, true, /* wait for commit */
						  &tmfd, false);
	switch (result)
	{
		case TM_SelfModified:
			/* Tuple was already updated in current command? */
			ereport(
				ERROR,
				(errmsg("xid %lu, oid %u, ctid(%u, %u). tuple already updated by self.",
						GetTopFullTransactionId().value, RelationGetRelid(relation),
						ItemPointerGetOffsetNumber(tid),
						ItemPointerGetBlockNumber(tid))));
			break;

		case TM_Ok:
			/* done successfully */
			break;

		case TM_Updated:
			ereport(ERROR,
					(errmsg("xid %lu, oid %u, ctid(%u, %u). tuple concurrently updated.",
							GetTopFullTransactionId().value, RelationGetRelid(relation),
							ItemPointerGetOffsetNumber(tid),
							ItemPointerGetBlockNumber(tid))));
			break;

		case TM_Deleted:
			ereport(ERROR,
					(errmsg("xid %lu, oid %u, ctid(%u, %u). tuple concurrently deleted.",
							GetTopFullTransactionId().value, RelationGetRelid(relation),
							ItemPointerGetOffsetNumber(tid),
							ItemPointerGetBlockNumber(tid))));
			break;

		default:
			ereport(
				ERROR,
				(errmsg(
					"xid %lu, oid %u, ctid(%u, %u). unrecognized XHeapDelete status: %u.",
					GetTopFullTransactionId().value, RelationGetRelid(relation),
					ItemPointerGetOffsetNumber(tid), ItemPointerGetBlockNumber(tid),
					result)));
			break;
	}
}
