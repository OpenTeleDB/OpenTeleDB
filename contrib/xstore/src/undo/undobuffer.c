/* -------------------------------------------------------------------------
 *
 * undobuffer.c
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    src/undo/undobuffer.c
 *
 * -------------------------------------------------------------------------
 */

#include "undo/undobuffer.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "undo/undotype.h"
#include "xstore.h"
#include "undo/undolog.h"
#include "access/xlogutils.h"
#include "access/xloginsert.h"

#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/palloc.h"
#include "utils/resowner.h"
#include "utils/elog.h"
#include "storage/buf_internals.h"


int load_prepare_buffers(UndoPrepareBuffers *upbuffers, RelFileLocator rlocator,
					 UndoLogControl *ulog, BlockNumber blk, ReadBufferMode rbm);
int load_prepare_buffers_by_xlog(UndoPrepareBuffers *upbuffers, RelFileLocator rnode,
							 UndoLogControl *ulog, BlockNumber blk,
						     XLogReaderState *xlog_record, ReadBufferMode rbm);

#define MAX_BUFFER_PER_UNDO 2

// cache undobuffer in undo_cache_ctx->undo_buffers
static void
cache_undo_buffers_in_ctx(UndoBuffer *buffer)
{
	int			bufidx = undo_cache_ctx->undo_buffer_idx;
	UndoBuffer *undobuf = &undo_cache_ctx->undo_buffers[bufidx];

	undobuf->buf = buffer->buf;
	undobuf->blk = buffer->blk;
	undobuf->logno = buffer->logno;
	undobuf->zero = buffer->zero;
	undobuf->used = buffer->used;

	undo_cache_ctx->undo_buffer_idx++;

	ResourceOwnerEnlarge(TopTransactionResourceOwner);
	ResourceOwnerRememberBuffer(TopTransactionResourceOwner, undobuf->buf);
	ResourceOwnerForgetBuffer(CurrentResourceOwner, undobuf->buf);
}

static bool
check_or_set_last_record_size(UndoRecordSize lastRecordSize, xl_undo_meta *const xlundometa)
{
	if (InRecovery && (lastRecordSize != xlundometa->lastRecordSize))
	{
		ereport(PANIC,
				(errmsg("last record size %u != xlog last record size %u.",
						lastRecordSize, xlundometa->lastRecordSize)));
		return false;
	}
	else
	{
		xlundometa->lastRecordSize = lastRecordSize;
	}
	return true;
}


static UndoBuffer *
prepare_buffers_get_buffer(UndoPrepareBuffers *upbuffers, int bufidx)
{
	return upbuffers->ubuffers + bufidx;
}

UnpackedUndoRecord *
prepare_buffers_get_undorecord(UndoPrepareBuffers *upbuffers, int idx)
{
	Assert(idx >= 0 && idx < upbuffers->uurec_size);

	return *(upbuffers->uurecs + idx);
}


static void
prepare_undo_record_info(UndoPrepareBuffers *upbuffers)
{
	int size = upbuffers->uurec_size;
	int i = 0;
	for (i = 0; i < size; i++)
	{
		UnpackedUndoRecord *urec = prepare_buffers_get_undorecord(upbuffers, i);
		if (!urec->is_update)
			continue;
		if (FullTransactionIdIsValid(GetUndoRecordOldXactId(urec)))
			SetUndoRecordUinfo(urec, UREC_INFO_PREXID);
		if (GetUndoRecordTablespace(urec) != InvalidOid)
			SetUndoRecordUinfo(urec, UREC_INFO_TABLESPACE);
	}
}

static UndoRecPtr
prepare_undo_record(UnpackedUndoRecord *urec, UndoPersistence upersistence, UndoRecPtr *undoPtr)
{
	UndoRecordSize urec_size = undo_record_expected_size(urec);
	urec->uur_urp = *undoPtr;
	*undoPtr = get_next_undoptr(*undoPtr, urec_size);
	return urec->uur_urp; 
}

UndoPrepareBuffers *
new_undo_prepare_buffers(int num)
{
	int i = 0;
	int total_bytes = sizeof(UnpackedUndoRecord *) * num;

	UndoPrepareBuffers *upbuffers = (UndoPrepareBuffers *) palloc0(sizeof(UndoPrepareBuffers));
	upbuffers->ubuffers = NULL;
	upbuffers->uurecs = NULL;
	upbuffers->uurec_size = 0;
	upbuffers->uurec_cap = num;
	upbuffers->curr_idx = 0;

	// init records
	upbuffers->uurecs =
		(UnpackedUndoRecord **) MemoryContextAlloc(CurrentMemoryContext, total_bytes);
	for (i = 0; i < num; i++)
		*(upbuffers->uurecs + i) = NULL;

	// init buffers
	upbuffers->ubuffers = (UndoBuffer *) MemoryContextAllocZero(
		CurrentMemoryContext, num * MAX_BUFFER_PER_UNDO * sizeof(UndoBuffer));

	return upbuffers;
}

void
release_undo_prepare_buffers(UndoPrepareBuffers *upbuffers)
{
	int i = 0;
	for (i = 0; i < upbuffers->uurec_size; i++)
	{
		UnpackedUndoRecord *urec = upbuffers->uurecs[i];
		destroy_undo_record(urec);
	}
	if (upbuffers->uurecs != NULL)
	{
		pfree(upbuffers->uurecs);
		upbuffers->uurecs = NULL;
	}
	for (i = 0; i < upbuffers->curr_idx; i++)
	{
		if (BufferIsValid(upbuffers->ubuffers[i].buf))
			UnlockReleaseBuffer(upbuffers->ubuffers[i].buf);
	}
	upbuffers->curr_idx = 0;
	if (upbuffers->ubuffers != NULL)
	{
		pfree(upbuffers->ubuffers);
		upbuffers->ubuffers = NULL;
	}

	pfree(upbuffers);
}

void
reset_undo_prepare_buffers(UndoPrepareBuffers *upbuffers, bool need_unlock)
{
	int i = 0;
	for (i = 0; i < upbuffers->uurec_size; i++)
	{
		UnpackedUndoRecord *urec = upbuffers->uurecs[i];
		reset_undo_record(urec, INVALID_UNDO_REC_PTR);
	}

	if (need_unlock)
	{
		for (i = 0; i < upbuffers->curr_idx; i++)
		{
			if (BufferIsValid(upbuffers->ubuffers[i].buf))
			{
				if (InRecovery)
					UnlockReleaseBuffer(upbuffers->ubuffers[i].buf);
				else
					LockBuffer(upbuffers->ubuffers[i].buf, BUFFER_LOCK_UNLOCK);	 
			}
		}
	}
	upbuffers->curr_idx = 0;
}

int
load_prepare_buffers(UndoPrepareBuffers *upbuffers, RelFileLocator rlocator,
					 UndoLogControl *ulog, BlockNumber blk, ReadBufferMode rbm)
{
	Buffer buffer = InvalidBuffer;
	int	   i = 0;
	bool need_add = false;
	bool need_load = false;

	Assert(!InRecovery);

	// first find the buffer in xstore_cxt
	for (i = 0; i < undo_cache_ctx->undo_buffer_idx; i++)
	{
		if ((blk == undo_cache_ctx->undo_buffers[i].blk) &&
			(rlocator.relNumber == (unsigned int) undo_cache_ctx->undo_buffers[i].logno))
		{
			buffer = undo_cache_ctx->undo_buffers[i].buf;

			if (!undo_cache_ctx->undo_buffers[i].used)
			{
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				undo_cache_ctx->undo_buffers[i].used = true;
				undo_cache_ctx->undo_buffers[i].zero = rbm == RBM_ZERO_AND_LOCK;
			}
			Assert(BufferIsValid(buffer));
			break;
		}
	}
	need_load = (i == undo_cache_ctx->undo_buffer_idx);

	// second check buffer in upbuffers
	for (i = 0; i < upbuffers->curr_idx; i++)
	{
		if (blk == upbuffers->ubuffers[i].blk)
		{
			if (!upbuffers->ubuffers[i].used)
			{
				upbuffers->ubuffers[i].used = true;
				upbuffers->ubuffers[i].zero = rbm == RBM_ZERO_AND_LOCK;
			}
			break;
		}
	}

	need_add = (i == upbuffers->curr_idx);

	if (need_load)
		// load from undo file
		buffer = ReadUndoBufferWithoutRelcache(rlocator, MAIN_FORKNUM, blk, rbm, NULL,
												RELPERSISTENCE_PERMANENT);

	if (need_add)
	{
		upbuffers->ubuffers[upbuffers->curr_idx].buf = buffer;
		upbuffers->ubuffers[upbuffers->curr_idx].blk = blk;
		upbuffers->ubuffers[upbuffers->curr_idx].logno = rlocator.relNumber;
		upbuffers->ubuffers[upbuffers->curr_idx].zero = rbm == RBM_ZERO_AND_LOCK;
		upbuffers->ubuffers[upbuffers->curr_idx].used = true;
	}

	if (need_load)
	{
		Assert(need_add);
		cache_undo_buffers_in_ctx(&upbuffers->ubuffers[upbuffers->curr_idx]);
	}

	if (need_add)
		upbuffers->curr_idx++;

	return i;
}

int 
load_prepare_buffers_by_xlog(UndoPrepareBuffers *upbuffers, RelFileLocator rlocator,
							 UndoLogControl *ulog, BlockNumber blk,
							 XLogReaderState *xlog_record, ReadBufferMode rbm)
{
	Buffer buffer = InvalidBuffer;
	int	   i = 0;

	Assert(InRecovery);
	
	// in recovery ,don't need to cache in ctx
	buffer = InvalidBuffer;
	for (i = 0; i < upbuffers->curr_idx; i++)
	{
		if (blk == upbuffers->ubuffers[i].blk)
		{
			if (!upbuffers->ubuffers[i].used)
			{
				LockBuffer(upbuffers->ubuffers[i].buf, BUFFER_LOCK_EXCLUSIVE);
				upbuffers->ubuffers[i].used = true;
			}
			break;
		}
	}

	// load from file 
	if (i == upbuffers->curr_idx)
	{
		if (blk * BLCKSZ >= ulog->undo_data_seg.head && blk * BLCKSZ < ulog->undo_data_seg.tail)
		{
			XLogRedoAction action = xlog_undo_read_buffer_for_redo(xlog_record, rlocator, MAIN_FORKNUM, blk, 
				rbm, false, &buffer);
			if(action == BLK_NOTFOUND)
			{
				upbuffers->ubuffers[upbuffers->curr_idx].buf = InvalidBuffer;
				upbuffers->ubuffers[upbuffers->curr_idx].blk = InvalidBlockNumber;
				upbuffers->curr_idx++;
			}
			else 
			{
				upbuffers->ubuffers[upbuffers->curr_idx].buf = buffer;
				upbuffers->ubuffers[upbuffers->curr_idx].blk = blk;
				upbuffers->ubuffers[upbuffers->curr_idx].logno = rlocator.relNumber;
				upbuffers->ubuffers[upbuffers->curr_idx].zero = rbm == RBM_ZERO_AND_LOCK;
				upbuffers->ubuffers[upbuffers->curr_idx].used = true;
				upbuffers->curr_idx++;
			}
		}
		else if (blk * BLCKSZ < ulog->undo_data_seg.head)
		{
			upbuffers->ubuffers[upbuffers->curr_idx].buf = InvalidBuffer;
			upbuffers->ubuffers[upbuffers->curr_idx].blk = InvalidBlockNumber;
			upbuffers->curr_idx++;
		}
		else
			ereport(PANIC, (errcode(ERRCODE_DATA_EXCEPTION), errmsg(
						"logno %u Space allocation tail=%lu is slower than blk=%u.",
						ulog->logno, ulog->undo_data_seg.tail, blk)));
	}
	
	return i;
}

bool
prepare_buffers_add_record(UndoPrepareBuffers *upbuffers, UnpackedUndoRecord *urec)
{
	Assert(urec);
	Assert(upbuffers->uurec_size < upbuffers->uurec_cap);
	if (upbuffers->uurec_size == upbuffers->uurec_cap)
		return false;
	upbuffers->uurecs[upbuffers->uurec_size++] = urec;
	return true;
}

uint64
prepare_buffers_total_size(UndoPrepareBuffers *upbuffers)
{
	uint64 total = 0;
	int	   i = 0;
	for (i = 0; i < upbuffers->uurec_size; i++)
	{
		UnpackedUndoRecord *urec = upbuffers->uurecs[i];
		if (!urec->is_update)
			continue;
		total += undo_record_expected_size(urec);
	}
	return total;
}

inline UndoRecPtr
prepare_buffers_get_first_undoptr(UndoPrepareBuffers *upbuffers)
{
	return upbuffers->uurecs[0]->uur_urp;
}

UndoRecPtr
prepare_buffers_get_last_undoptr(UndoPrepareBuffers *upbuffers)
{
	UndoRecPtr last_urecptr = INVALID_UNDO_REC_PTR;
	int		   i = 0;
	for (i = 0; i < upbuffers->uurec_size; i++)
	{
		UnpackedUndoRecord *urec = upbuffers->uurecs[i];
		if (!urec->is_update)
			continue;
		last_urecptr = urec->uur_urp;
	}
	return last_urecptr;
}

UndoRecordSize
prepare_buffers_get_last_recordsize(UndoPrepareBuffers *upbuffers)
{
	UndoRecPtr last_rec_size = 0;
	int		   i = 0;
	for (i = 0; i < upbuffers->uurec_size; i++)
	{
		UnpackedUndoRecord *urec = upbuffers->uurecs[i];
		if (!urec->is_update)
			continue;
		last_rec_size = undo_record_expected_size(urec);
	}
	return last_rec_size;
}


void
release_cache_buffers_in_ctx()
{
	int i = 0;
	if (undo_cache_ctx == NULL)
		return;
	if (undo_cache_ctx->undo_prepare_buffers != NULL)
		reset_undo_prepare_buffers(undo_cache_ctx->undo_prepare_buffers, false);
	for (i = 0; i < undo_cache_ctx->undo_buffer_idx; i++)
	{
		if (BufferIsValid(undo_cache_ctx->undo_buffers[i].buf))
		{
			PG_TRY();
			{
				ReleaseBuffer(undo_cache_ctx->undo_buffers[i].buf);
			}
			PG_CATCH();
			{
				FlushErrorState();
			}
			PG_END_TRY();
		}
	}

	undo_cache_ctx->undo_buffer_idx = 0;
}

void
prepare_buffers_set_page_lsn(UndoPrepareBuffers *upbuffers, XLogRecPtr lsn)
{
	int idx = 0;
	Assert(upbuffers->curr_idx != 0);

	for (idx = 0; idx < upbuffers->curr_idx; idx++)
	{
		UndoBuffer *ub = prepare_buffers_get_buffer(upbuffers, idx);
		if (ub->used)
		{
			Page page = BufferGetPage(ub->buf);
			PageSetLSN(page, lsn);
		}
	}
}

void
xlog_register_undo_buffers(UndoPrepareBuffers *upbuffers, uint8 first_block_id, UndoLogControl *ulog)
{
	int idx = 0;
	int	flags = REGBUF_STANDARD;
	Assert(upbuffers->curr_idx != 0);
	if (ulog->txn_slot_buffer.zero)
	{
		flags |= REGBUF_WILL_INIT;
		ulog->txn_slot_buffer.zero = false;
	}
	XLogRegisterBuffer(first_block_id, ulog->txn_slot_buffer.buffer, flags);
	first_block_id += 1;
	flags = REGBUF_STANDARD;
	for (idx = 0; idx < upbuffers->curr_idx; idx++)
	{
		UndoBuffer *ub = prepare_buffers_get_buffer(upbuffers, idx);
		if (ub->used)
		{
			if(ub->zero)
				flags |= REGBUF_WILL_INIT;
			XLogRegisterBuffer(first_block_id + idx, ub->buf, flags);
		}
	}
}

int 
prepare_buffers_used_count(UndoPrepareBuffers *upbuffers)
{
	int used = 0;
	for (int idx = 0; idx < upbuffers->curr_idx; idx++)
	{
		UndoBuffer *ub = prepare_buffers_get_buffer(upbuffers, idx);
		if (ub->used)
			used++;
	}
	return used;
}

int
prepare_undo(UndoPrepareBuffers *upbuffers, UndoPersistence upersistence, XLogReaderState *xlog_record, 
	xl_undo_header *xlundohdr, xl_undo_meta *xlundometa)
{
	UndoRecPtr	   urec_ptr = 0;
	UnpackedUndoRecord	  *urec = NULL;
	uint32		   total_size = 0;
	UndoRecordSize urec_size = 0;
	bool		   is_switch = false;
	int			   i = 0;
	int			   buf_idx = 0;

	if (upbuffers == NULL)
		return UNDO_PREPARE_FAIL;

	Assert(upbuffers->uurec_size > 0);
	prepare_undo_record_info(upbuffers);

	urec = prepare_buffers_get_undorecord(upbuffers, 0);
	total_size = prepare_buffers_total_size(upbuffers);

	if (InRecovery)
	{
		urec_ptr = xlundohdr->urecptr;
		is_switch = xlog_undo_meta_is_switch(xlundometa);
		if (is_switch)
		{
			SetUndoRecordUinfo(urec, UREC_INFO_PREURP);
			elog(INFO, "recovery need switch %d %ld", total_size, urec_ptr);
		}
	}
	else
	{
		is_switch = check_need_switch_undolog(upersistence, total_size, INVALID_UNDOLOG_NO);
		if (is_switch)
		{
			//recalculate total size because of uinfo change
			SetUndoRecordUinfo(urec, UREC_INFO_PREURP);
			total_size = prepare_buffers_total_size(upbuffers);  
		}
		urec_ptr = allocate_undo_space(GetUndoRecordXid(urec), upersistence, total_size,
									is_switch, xlundometa);
	}

	Assert(urec_ptr != INVALID_UNDO_REC_PTR);
	for (i = 0; i < upbuffers->uurec_size; i++)
	{
		UndoRecPtr	urecptr = INVALID_UNDO_REC_PTR;
		urec = prepare_buffers_get_undorecord(upbuffers, i);
		if (!urec->is_update)
			continue;

		urec_size = undo_record_expected_size(urec);
		if ((urecptr = prepare_undo_record(urec, upersistence, &urec_ptr)) ==
			INVALID_UNDO_REC_PTR)
		{
			ereport(PANIC,
					(errmsg("prepare %d bytes failed on logno %d",
							undo_record_expected_size(urec), undo_log_ctx->logs[upersistence])));
			return UNDO_PREPARE_FAIL;
		}

		if (!xlog_undo_meta_is_skip(xlundometa))
		{
			BlockNumber	   curBlk = UNDO_PTR_GET_BLOCK_NUM(urecptr);
			int			   startingByte = UNDO_PTR_GET_PAGE_OFFSET(urecptr);
			RelFileLocator	   rlocator;
			UndoRecordSize curSize = 0;
			ReadBufferMode rbm = RBM_NORMAL;
			int logno = UNDO_PTR_GET_LOG_NO(urecptr);
			UndoLogControl	*ulog = get_undo_log(logno);
			UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, urecptr, UNDO_DATA_DB_OID);
			
			elog(DEBUG5, "Uinfo = %d; Urp = %lu; Prevurp2 = %lu",
				 GetUndoRecordUinfo(urec), urec->uur_urp, GetUndoRecordTxnPrevurp(urec));

			if (startingByte == UNDO_LOG_BLOCK_HEADER_SIZE)
				rbm = RBM_ZERO_AND_LOCK; 

			do
			{
				if(!InRecovery)
					buf_idx = load_prepare_buffers(upbuffers, rlocator, ulog, curBlk, rbm);
				else
					buf_idx = load_prepare_buffers_by_xlog(upbuffers, rlocator, ulog, curBlk, xlog_record, rbm);
				
				if (urec->buffer_idx == -1)
					urec->buffer_idx = buf_idx;
				if (curSize == 0)
					curSize = BLCKSZ - startingByte;
				else
					curSize += BLCKSZ - UNDO_LOG_BLOCK_HEADER_SIZE;

				/*
				 * If we need more pages they'll be all new so we can definitely skip
				 * reading from disk.
				 */
				curBlk++;
				rbm = RBM_ZERO_AND_LOCK;
			} while (curSize < urec_size);
		}
	}

	check_or_set_last_record_size(urec_size, xlundometa);
	return UNDO_PREPARE_SUCC;
}


void
insert_prepared_undo(UndoPrepareBuffers *upbuffers, XLogRecPtr lsn)
{
	int i = 0;
	if (upbuffers == NULL)
		return;
	for (i = 0; i < upbuffers->uurec_size; i++)
	{
		UnpackedUndoRecord	 *urec = prepare_buffers_get_undorecord(upbuffers, i);

		UndoRecordSize undo_len = undo_record_expected_size(urec);
		int			   starting_byte = UNDO_PTR_GET_PAGE_OFFSET(urec->uur_urp);
		int			   already_written = 0;
		UndoRecordSize remaining_bytes = undo_len;
		int			   last_page_written = 0;
		int			   buf_idx = urec->buffer_idx;
		Page		   page = NULL;
		bool		   diffpage = false;
		bool		   newpage = false;
		PageHeader	   phdr;

		if (!urec->is_update)
			continue;

		Assert((urec->uur_payloadlen == 0) ||
		   (urec->uur_payloadlen > 0 && urec->uur_payload.data != NULL));

		do
		{
			UndoBuffer *ubuffer = prepare_buffers_get_buffer(upbuffers, buf_idx);
			Buffer		buffer = ubuffer->buf;
			if (BufferIsValid(buffer))
			{
				BufferDesc *buf_desc = GetBufferDescriptor(buffer - 1);
				if (!LWLockHeldByMe(&buf_desc->content_lock))
					LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				page = BufferGetPage(buffer);
				phdr = (PageHeader) page;
				if (PageIsNew(page))
				{
					int logno = UNDO_PTR_GET_LOG_NO(urec->uur_urp);
					PageInit(page, BLCKSZ, 0);
					phdr->pd_prune_xid = (uint32) logno;
					newpage = true;
				}
				if (already_written == 0 && phdr->pd_lower == SizeOfPageHeaderData)
					diffpage = true;
				if (!InRecovery || PageGetLSN(page) < lsn)
				{
					if (starting_byte != phdr->pd_lower)
					{
						elog(INFO,
							 "undo record discontinuous,logno %u, buffer %d, startingByte "
							 "%u, "
							 "page start %u, page end %u, alreadyWritten %d, "
							 "lastPageWritten %d, diffpage %s, urp %lu, "
							 "newpage %s.",
							 phdr->pd_prune_xid, buffer, starting_byte, phdr->pd_lower,
							 phdr->pd_upper, already_written, last_page_written,
							 diffpage ? "true" : "false", urec->uur_urp,
							 newpage ? "true" : "false");
					}
					if (insert_undo_record(urec, page, starting_byte, &already_written, 0,
										 undo_len))
					{
						MarkBufferDirty(buffer);
						if (InRecovery)
							PageSetLSN(page, lsn);
						Assert(already_written >= last_page_written);
						phdr->pd_lower =
							(uint16) (starting_byte + already_written - last_page_written);
						break;
					}
					MarkBufferDirty(buffer);
					if (InRecovery)
						PageSetLSN(page, lsn);
				}
				else
					insert_undo_record(urec, page, starting_byte, &already_written, 0, undo_len);
				Assert(already_written >= last_page_written);
				phdr->pd_lower =
					(uint16) (starting_byte + already_written - last_page_written);
			}
			else
			{
				page = NULL;
				/*
				* During recovery, there might be some blocks which are already
				* removed by discard process, so we can just skip inserting into
				* those blocks.
				*/
				Assert(InRecovery);

				/*
				* Block is not valid so we can not write to the current block but
				* we might need to insert remaining partial record to the next
				* block so set proper value for already_written variable to jump
				* to the undo record offset from which we want to insert into
				* next block.
				*/
				if (insert_undo_record(urec, page, starting_byte, &already_written,
									  remaining_bytes, undo_len))
					break;
				else
					remaining_bytes -= (BLCKSZ - starting_byte);
			}
			starting_byte = UNDO_LOG_BLOCK_HEADER_SIZE;
			buf_idx++;
			last_page_written = already_written;
		} while (buf_idx < upbuffers->curr_idx);
	}
}

void
reset_prepared_buffers_in_ctx()
{
	/* Reset undo records and undo prepare buffers */
	reset_undo_prepare_buffers(undo_cache_ctx->undo_prepare_buffers, true);

	if (undo_cache_ctx->undo_buffer_idx >= ((MAX_UNDO_BUFFERS / 2) - 1))
	{
		for (int i = 0; i < undo_cache_ctx->undo_buffer_idx; i++)
		{
			ResourceOwnerForgetBuffer(TopTransactionResourceOwner,
									  undo_cache_ctx->undo_buffers[i].buf);

			ResourceOwnerRememberBuffer(CurrentResourceOwner,
										undo_cache_ctx->undo_buffers[i].buf);
			ReleaseBuffer(undo_cache_ctx->undo_buffers[i].buf); 
			undo_cache_ctx->undo_buffers[i].used = false;
			undo_cache_ctx->undo_buffers[i].zero = false;
		}
		// all cache buffer is released and unlock.
		undo_cache_ctx->undo_buffer_idx = 0;
	}
	else
	{
		for (int i = 0; i < undo_cache_ctx->undo_buffer_idx; i++)
		{
			if (BufferIsValid(undo_cache_ctx->undo_buffers[i].buf))
			{
				// should be unlock already. but not release
				BufferDesc *bufdesc =
					GetBufferDescriptor(undo_cache_ctx->undo_buffers[i].buf - 1);
				if (LWLockHeldByMeInMode(BufferDescriptorGetContentLock(bufdesc),
										 LW_EXCLUSIVE))
				{
					LWLock *lock = BufferDescriptorGetContentLock(bufdesc);
					ereport(PANIC,
							(errmsg("xid %u, oid %u, blockno %u. buffer %d is not "
									"unlocked, lock state %u.",
									GetTopTransactionId(), bufdesc->tag.relNumber,
									BufferGetBlockNumber(undo_cache_ctx->undo_buffers[i].buf),
									undo_cache_ctx->undo_buffers[i].buf, lock->state.value)));
					undo_cache_ctx->undo_buffers[i].used = false;
					undo_cache_ctx->undo_buffers[i].zero = false;
				}
			}
		}
		//all cache buffer is unlocked ,but not released.
	}
}