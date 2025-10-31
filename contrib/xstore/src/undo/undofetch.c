/* -------------------------------------------------------------------------
 *
 * undofetch.c
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 *    src/undo/undofetch.c
 *
 * -------------------------------------------------------------------------
 */

#include "undo/undofetch.h"
#include "access/transam.h"
#include "undo/undolog.h"
#include "undo/undotype.h"
#include "xstore.h"
#include "utils/elog.h"
#include "storage/buf_internals.h"
#include "access/xlog.h"


static void
extend_undo_list(UndoList *ulist, int new_cap)
{
	int i=0;
	int new_total_bytes = sizeof(UnpackedUndoRecord *) * new_cap;
	Assert(ulist->mem_ctx);

	if (ulist->uurecs == NULL)
		ulist->uurecs =
			(UnpackedUndoRecord **) MemoryContextAlloc(ulist->mem_ctx, new_total_bytes);
	else
		ulist->uurecs = (UnpackedUndoRecord **) repalloc(ulist->uurecs, new_total_bytes);
	
	// set null for new item.
	for (i = ulist->uurec_size; i < new_cap; i++)
		*(ulist->uurecs + i) = NULL;

	ulist->uurec_cap = new_cap;
}

UndoList *
new_undo_list(int capacity)
{
	UndoList *ulist = (UndoList *) palloc0(sizeof(UndoList));
	ulist->mem_ctx = CurrentMemoryContext;
	ulist->uurecs = NULL;
	ulist->uurec_size = 0;
	ulist->uurec_cap = 0;
	extend_undo_list(ulist, capacity);
	return ulist;
}

void
destroy_undo_list(UndoList *ulist)
{
	int i = 0;

	for (i = 0; i < ulist->uurec_size; i++)
	{
		UnpackedUndoRecord *urec = ulist->uurecs[i];
		destroy_undo_record(urec);
	}
	if (ulist->uurecs != NULL)
	{
		pfree(ulist->uurecs);
		ulist->uurecs = NULL;
	}
	ulist->mem_ctx = NULL;
	pfree(ulist);
}

void
push_undo_list(UndoList *ulist, UnpackedUndoRecord *urec)
{
	Assert(urec);

	if (ulist->uurec_size == ulist->uurec_cap)
		extend_undo_list(ulist, ulist->uurec_cap * 2);
	urec->buffer_idx = ulist->uurec_size;
	urec->mem_ctx = ulist->mem_ctx;
	ulist->uurecs[ulist->uurec_size++] = urec;
}

/*
 * undo_record_comparator
 *
 * qsort comparator to handle undo record for applying undo actions of the
 * transaction.
 */
static int
undo_record_comparator(const void *left, const void *right)
{
	UnpackedUndoRecord *luur = *((UnpackedUndoRecord **) left);
	UnpackedUndoRecord *ruur = *((UnpackedUndoRecord **) right);

	if (luur->uur_tablespace < ruur->uur_tablespace)
		return -1;
	else if (luur->uur_tablespace > ruur->uur_tablespace)
		return 1;
	else if (luur->uur_relfilenode < ruur->uur_relfilenode)
		return -1;
	else if (luur->uur_relfilenode > ruur->uur_relfilenode)
		return 1;
	else if (luur->uur_blkno == ruur->uur_blkno)
	{
		if (luur->buffer_idx < ruur->buffer_idx)
			return -1;
		else
			return 1;
	}
	else if (luur->uur_blkno < ruur->uur_blkno)
		return -1;
	else if(luur->uur_subxid.value < ruur->uur_subxid.value)
		return -1;
	else
		return 1;
}

void
qsort_undo_list(UndoList *ulist)
{
	qsort((void *) ulist->uurecs, ulist->uurec_size, sizeof(UnpackedUndoRecord *), undo_record_comparator);
}


static bool
load_undo_record_for_bulk(UnpackedUndoRecord *urec, Buffer *buffer)
{
	UndoRecordState state = check_undo_record_valid(urec->uur_urp, false, NULL);
	if (state != UNDO_RECORD_NORMAL)
		return false;

	PG_TRY();
	{
		undo_get_one_record(urec, true);
		state = check_undo_record_valid(urec->uur_urp, true, NULL);
	}
	PG_CATCH();
	{  // err or fail
		MemoryContext old_ctx = MemoryContextSwitchTo(CurrentMemoryContext);
		if (BufferIsValid(urec->uur_buffer))
		{
			if (urec->uur_buffer == *buffer)
				*buffer = InvalidBuffer;
			if (LWLockHeldByMeInMode(
					BufferDescriptorGetContentLock(GetBufferDescriptor(urec->uur_buffer - 1)),
					LW_SHARED))
				LockBuffer(urec->uur_buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(urec->uur_buffer);
			urec->uur_buffer = InvalidBuffer;
		}
		state = check_undo_record_valid(urec->uur_urp, false, NULL);
		if (state == UNDO_RECORD_DISCARD || state == UNDO_RECORD_FORCE_DISCARD)
		{
			FlushErrorState();
			return false;
		}
		else
		{
			(void) MemoryContextSwitchTo(old_ctx);
			PG_RE_THROW();
		}
	}
	PG_END_TRY();
	*buffer = urec->uur_buffer;
	urec->uur_buffer = InvalidBuffer;
	return (state == UNDO_RECORD_NORMAL);
}


static UndoRecordState
load_undo_record(UnpackedUndoRecord *urec, FullTransactionId *last_xid)
{
	UndoRecordState state = check_undo_record_valid(urec->uur_urp, true, last_xid);
	if (state != UNDO_RECORD_NORMAL)
		return state;

	PG_TRY();
	{
		undo_get_one_record(urec, false);
		state = check_undo_record_valid(urec->uur_urp, true, NULL);
	}
	PG_CATCH();
	{
		MemoryContext oldContext = MemoryContextSwitchTo(CurrentMemoryContext);
		state = check_undo_record_valid(urec->uur_urp, true, last_xid);
		if (state == UNDO_RECORD_DISCARD || state == UNDO_RECORD_FORCE_DISCARD)
		{
			if (BufferIsValid(urec->uur_buffer))
			{
				if (LWLockHeldByMeInMode(BufferDescriptorGetContentLock(
											 GetBufferDescriptor(urec->uur_buffer - 1)),
										 LW_SHARED))
					LockBuffer(urec->uur_buffer, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(urec->uur_buffer);
				urec->uur_buffer = (InvalidBuffer);
			}
			FlushErrorState();
			return state;
		}
		else
		{
			(void) MemoryContextSwitchTo(oldContext);
			PG_RE_THROW();
		}
	}
	PG_END_TRY();
	return state;
}

bool
satisfy_undo_record(UnpackedUndoRecord *urec, FullTransactionId xid)
{
	Assert(urec != NULL);
	Assert(urec->uur_blkno != InvalidBlockNumber);

	if ((TransactionIdIsValid(xid.value) && !FullTransactionIdEquals(xid, urec->uur_xid)))
		return false;
	return true;
}


UndoList *
bulk_fetch_undo_for_range(UndoRecPtr *start_recptr, UndoRecPtr end_recptr, int max_apply_size)
{
	static const int   init_recsize = 1024;
	int				   used_size = 0;

	Buffer	   buffer = InvalidBuffer;
	UndoRecPtr cur_recptr = *start_recptr;
	UndoRecPtr pre_recptr = INVALID_UNDO_REC_PTR;

	UndoList *undolist = new_undo_list(init_recsize);
	*start_recptr = INVALID_UNDO_REC_PTR;
	do
	{
		UnpackedUndoRecord *urec = new_undo_record();
		urec->uur_urp = cur_recptr;

		//  release the prev undo buffer.
		if (!IS_VALID_UNDO_REC_PTR(pre_recptr) ||
			UNDO_PTR_GET_LOG_NO(pre_recptr) != UNDO_PTR_GET_LOG_NO(cur_recptr) ||
			UNDO_PTR_GET_BLOCK_NUM(pre_recptr) != UNDO_PTR_GET_BLOCK_NUM(cur_recptr))
		{
			if (BufferIsValid(buffer))
			{
				if (LWLockHeldByMeInMode(
						BufferDescriptorGetContentLock(GetBufferDescriptor((buffer) -1)),
						LW_SHARED))
					LockBuffer((buffer), BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buffer);
				buffer = InvalidBuffer;
			}
		}
		else
			urec->uur_buffer = (buffer);

		if (!load_undo_record_for_bulk(urec, &buffer))
			break;

		pre_recptr = cur_recptr;
		if (pre_recptr == end_recptr)
			// reach the end
			cur_recptr = INVALID_UNDO_REC_PTR;	 
		else
		{
			cur_recptr = undo_record_get_preurp(urec, cur_recptr, &buffer);  
			elog(DEBUG5, "cur urp %lu blk no %u prev urp %lu.", urec->uur_urp,
				 GetUndoRecordBlkno(urec), cur_recptr);
		}

		push_undo_list(undolist, urec);

		if (!IS_VALID_UNDO_REC_PTR(cur_recptr))
			break;

		used_size += unpack_undo_record_size(urec);
		if (used_size >= max_apply_size)
		{
			*start_recptr = cur_recptr;
			break;
		}

	} while (true);

	// finally release the undo buffer
	if (BufferIsValid(buffer))
	{
		if (LWLockHeldByMeInMode(
				BufferDescriptorGetContentLock(GetBufferDescriptor((buffer) -1)),
				LW_SHARED))
			LockBuffer((buffer), BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}

	return undolist;
}

UndoList *
bulk_fetch_undo_for_tuple(UndoRecPtr *start_recptr, int max_apply_size)
{
	static const int init_size = 128;
	int				 used_size = 0;
	FullTransactionId	 xid = InvalidFullTransactionId;

	Buffer	   buffer = InvalidBuffer;
	UndoRecPtr cur_recptr = *start_recptr;
	UndoRecPtr pre_recptr = INVALID_UNDO_REC_PTR;

	UndoList *undolist = new_undo_list(init_size);
	*start_recptr = INVALID_UNDO_REC_PTR;
	do
	{
		UnpackedUndoRecord *urec = new_undo_record();
		urec->uur_urp = cur_recptr;

		// release the prev undo buffer.
		if (!IS_VALID_UNDO_REC_PTR(pre_recptr) ||
			UNDO_PTR_GET_LOG_NO(pre_recptr) != UNDO_PTR_GET_LOG_NO(cur_recptr) ||
			UNDO_PTR_GET_BLOCK_NUM(pre_recptr) != UNDO_PTR_GET_BLOCK_NUM(cur_recptr))
		{
			if (BufferIsValid(buffer))
			{
				if (LWLockHeldByMeInMode(
						BufferDescriptorGetContentLock(GetBufferDescriptor((buffer) -1)),
						LW_SHARED))
					LockBuffer((buffer), BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buffer);
				buffer = InvalidBuffer;
			}
		}
		else
			urec->uur_buffer = (buffer);

		if (!load_undo_record_for_bulk(urec, &buffer))
			break;

		if (!TransactionIdIsValid(xid.value))
			xid = GetUndoRecordXid(urec);
		else if (xid.value != GetUndoRecordXid(urec).value)
			break;
		
		pre_recptr = cur_recptr;
		
		// get next undorecord for the same tuple
		cur_recptr = GetUndoRecordTpprev(urec);  
		elog(DEBUG5, "cur urp %lu blk no %u blk prev %lu.", urec->uur_urp,
				GetUndoRecordBlkno(urec), cur_recptr);
		
		push_undo_list(undolist, urec);

		// not more undo record
		if (!IS_VALID_UNDO_REC_PTR(cur_recptr))
			break;

		used_size += unpack_undo_record_size(urec);
		if (used_size >= max_apply_size)
		{
			// over the max undo apply size
			*start_recptr = cur_recptr;
			break;
		}

	} while (true);

	// finally release the undo buffer
	if (BufferIsValid(buffer))
	{
		if (LWLockHeldByMeInMode(
				BufferDescriptorGetContentLock(GetBufferDescriptor((buffer) -1)),
				LW_SHARED))
			LockBuffer((buffer), BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}

	return undolist;
}

UndoTraversalState
fetch_undo_record(UnpackedUndoRecord *urec, FullTransactionId xid, bool need_bypass,
				FullTransactionId *last_xid, SatisfyUndoRecordCallback callback)
{
	Assert(urec);
	if (RecoveryInProgress())
	{
		uint64 blockcnt = 0;

		while (check_undo_record_valid(urec->uur_urp, false, NULL) == UNDO_RECORD_NOT_INSERT)
		{
			elog(INFO, "urp: %ld is not replayed yet.  waiting for replay.",
				 urec->uur_urp);

			pg_usleep(1000L);
			if (blockcnt % 1000 == 0)
				CHECK_FOR_INTERRUPTS();
		}

		if (check_undo_record_valid(urec->uur_urp, false, NULL) == UNDO_RECORD_DISCARD)
			return UNDO_TRAVERSAL_END;
	}

	do
	{
		UndoRecordState state = load_undo_record(urec, last_xid);
		FullTransactionId	frozenXid = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
		if (state == UNDO_RECORD_DISCARD)
			return UNDO_TRAVERSAL_END;
		else if (state == UNDO_RECORD_INVALID)
			return UNDO_TRAVERSAL_END;
		else if (state == UNDO_RECORD_FORCE_DISCARD)
			return UNDO_TRAVERSAL_ABORT;

		if (need_bypass && FullTransactionIdPrecedes(urec->uur_xid, frozenXid))
			return UNDO_TRAVERSAL_STOP;

		if (!callback)
			break;

		if (callback(urec, xid))
			break;

		reset_undo_record2(urec);
	} while (true);

	return UNDO_TRAVERSAL_COMPLETE;
}

/*
 * Fetch the transaction information for the given tuple. In new version of xsore, we just need only to
 * get cid and new ctid (when meet a non-inplace update) from undo log.
 */
UndoTraversalState
fetch_transinfo_from_undo(UndoRecPtr urec_ptr, BlockNumber blocknum, OffsetNumber offnum, 
					   FullTransactionId xid, CommandId *cid, ItemPointer new_ctid,
					   bool need_bypass, FullTransactionId *last_xid, UndoRecPtr *urp)
{
 	UnpackedUndoRecord	*urec = new_undo_record();
 	UndoTraversalState  rc;

 	if (new_ctid)
 		ItemPointerSet(new_ctid, blocknum, offnum);

 	urec->uur_urp = urec_ptr;
 	urec->mem_ctx = CurrentMemoryContext;
 	rc = fetch_undo_record(urec, xid,
 						 need_bypass, last_xid, satisfy_undo_record);
 	if (urp != NULL)
 		*urp = urec->uur_urp;
 	/* The undo record has been discarded. It should be all-visible. */
 	if (rc == UNDO_TRAVERSAL_END || rc == UNDO_TRAVERSAL_STOP)
 		goto out;
 	else if (rc != UNDO_TRAVERSAL_COMPLETE)
 		goto out;

 	*cid = GetUndoRecordCid(urec);

 	/* If this is a non-in-place update, update ctid if requested. */
 	if (new_ctid && GetUndoRecordUtype(urec) == UNDO_UPDATE)
 	{
 		char *end;
 		Assert(GetUndoRecordRawdata(urec) != NULL);

 		end = (char *) (GetUndoRecordRawdata(urec)->data) +
 			  GetUndoRecordRawdata(urec)->len ;
 		ItemPointerCopy((ItemPointer) (end - sizeof(ItemPointerData)), new_ctid);
 	}

 out:
 	destroy_undo_record(urec);
 	return rc;
}

UndoTraversalState
fetch_subxid_from_undo(UndoRecPtr urecptr, FullTransactionId *subxid)
{
	UnpackedUndoRecord *urec = new_undo_record();
	UndoTraversalState rc;

	*subxid = InvalidFullTransactionId;
	urec->uur_urp = urecptr;
	urec->mem_ctx = CurrentMemoryContext;

	rc = fetch_undo_record(urec, InvalidFullTransactionId, false, NULL, satisfy_undo_record);
	if (rc != UNDO_TRAVERSAL_COMPLETE)
		goto out;

	if (UndoRecordHasSubXact(urec))
		*subxid = GetUndoRecordSubXid(urec);

out:
	destroy_undo_record(urec);
	return rc;
}


UndoRecPtr
undo_record_get_preurp(UnpackedUndoRecord *urec, UndoRecPtr cur_recptr, Buffer *buffer)
{
	int			   logno = UNDO_PTR_GET_LOG_NO(cur_recptr);
	UndoLogOffset  offset = UNDO_PTR_GET_OFFSET(cur_recptr);
	UndoRecordSize prevLen = 0;
	if (IS_VALID_UNDO_REC_PTR(urec->uur_txnprevurp))
		return urec->uur_txnprevurp;
	prevLen = undo_record_get_prerecordlen(cur_recptr, buffer);
	elog(DEBUG5, "Prevurp logno=%d, offset=%lu, prevLen=%u", logno, offset, prevLen);

	return MAKE_UNDO_REC_PTR(logno, offset - prevLen);
}

UndoRecordSize
undo_record_get_prerecordlen(UndoRecPtr cur_recptr, Buffer *input_buffer)
{
	Buffer		   buffer = InvalidBuffer;
	bool		   need_release = false;
	BlockNumber	   blk = UNDO_PTR_GET_BLOCK_NUM(cur_recptr);
	RelFileLocator rlocator;
	UndoRecordSize pre_reclen = 0;
	UndoLogOffset  page_offset = UNDO_PTR_GET_PAGE_OFFSET(cur_recptr);
	char		  *page = NULL;
	UndoRecordSize byte_to_read = sizeof(UndoRecordSize);
	char		   prev_len[2];
	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, cur_recptr, UNDO_DATA_DB_OID);

	Assert(page_offset != 0);
	if (input_buffer == NULL || !BufferIsValid(*input_buffer))
	{
		buffer = ReadUndoBufferWithoutRelcache(rlocator, MAIN_FORKNUM, blk, RBM_NORMAL, NULL,
											   RELPERSISTENCE_PERMANENT);
		need_release = true;
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
	}
	else
		buffer = *input_buffer;

	page = (char *) BufferGetPage(buffer);

	while (byte_to_read > 0)
	{
		page_offset -= 1;
		if (page_offset >= UNDO_LOG_BLOCK_HEADER_SIZE)
		{
			prev_len[byte_to_read - 1] = page[page_offset];
			byte_to_read -= 1;
		}
		else
		{
			if (need_release)
			{
				if (LWLockHeldByMeInMode(
						BufferDescriptorGetContentLock(GetBufferDescriptor(buffer - 1)),
						LW_SHARED))
					LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buffer);
			}
			need_release = true;
			blk -= 1;
			buffer = ReadUndoBufferWithoutRelcache(rlocator, MAIN_FORKNUM, blk, RBM_NORMAL,
												   NULL, RELPERSISTENCE_PERMANENT);
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			page_offset = BLCKSZ;
			page = (char *) BufferGetPage(buffer);
		}
	}

	pre_reclen = *(UndoRecordSize *) (prev_len);

	if (UNDO_PTR_GET_PAGE_OFFSET(cur_recptr) - UNDO_LOG_BLOCK_HEADER_SIZE < pre_reclen)
		pre_reclen += UNDO_LOG_BLOCK_HEADER_SIZE;
	if (need_release)
	{
		if (LWLockHeldByMeInMode(
				BufferDescriptorGetContentLock(GetBufferDescriptor(buffer - 1)),
				LW_SHARED))
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}
	if (pre_reclen == 0)
		ereport(PANIC,
				(errmsg("Currurp %lu, prevLen=%u", cur_recptr, pre_reclen)));
	return pre_reclen;
}