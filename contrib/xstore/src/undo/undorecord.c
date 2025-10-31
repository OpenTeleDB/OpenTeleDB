/* -------------------------------------------------------------------------
 *
 * undorecord.c
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    src/undo/undorecord.c
 *
 * -------------------------------------------------------------------------
 */

#include "undo/undorecord.h"
#include "storage/block.h"
#include "storage/off.h"
#include "undo/undolog.h"
#include "undo/undotype.h"
#include "xstore.h"

#include "access/xlog.h"
#include "utils/elog.h"
#include "storage/buf_internals.h"


/* Workspace for insert_undo_record and unpack_undo_record. */
static UndoRecordHeader	  work_hdr;
static UndoRecordBlock    work_blk;

/* Prototypes for static functions. */
static bool insert_undo_bytes(char *sourceptr, int sourcelen,
							char **writeptr, char *endptr,
							int *my_bytes_written, int *total_bytes_written);
static bool read_undo_bytes(char *destptr, int readlen,
						  char **readptr, char *endptr,
						  int *my_bytes_read, int *total_bytes_read);


bool unpack_undo_record(UnpackedUndoRecord *urec, Page page, int starting_byte, int *already_read,
					bool copy_data);

UnpackedUndoRecord *
new_undo_record()
{
	UnpackedUndoRecord *urec = (UnpackedUndoRecord *) palloc0(sizeof(UnpackedUndoRecord));
	
	urec->uur_xid.value = InvalidTransactionId;
	urec->uur_cid = InvalidCommandId;
	urec->uur_reloid = InvalidOid;
	urec->uur_relfilenode = InvalidOid;
	urec->uur_type = UNDO_UNKNOWN;
	urec->uur_info = UNDO_UREC_INFO_UNKNOWN;

	urec->uur_tpprevurp = INVALID_UNDO_REC_PTR;
	urec->uur_blkno = InvalidBlockNumber;
	urec->uur_offset = InvalidOffsetNumber;

	urec->uur_txnprevurp = INVALID_UNDO_REC_PTR;
	urec->uur_payloadlen = 0;
	urec->uur_prevxid = InvalidFullTransactionId;
	urec->uur_tablespace = InvalidOid;
	urec->uur_subxid = InvalidFullTransactionId;
	urec->uur_dataheaderflag = 0;

	urec->uur_payload.data = NULL;
	urec->uur_payload.len = 0;
	urec->uur_urp = INVALID_UNDO_REC_PTR;
	urec->uur_buffer = InvalidBuffer;
	urec->buffer_idx = -1;
	urec->is_update = false;
	urec->is_copy = true;
	urec->mem_ctx = CurrentMemoryContext;
	return urec;
}

void
destroy_undo_record(UnpackedUndoRecord *urec)
{
	reset_undo_record(urec, INVALID_UNDO_REC_PTR);
	urec->mem_ctx = NULL;
	pfree(urec);
}


void
reset_undo_record(UnpackedUndoRecord *urec, UndoRecPtr urp)
{
	urec->uur_xid.value = InvalidTransactionId;
	urec->uur_cid = InvalidCommandId;
	urec->uur_reloid = InvalidOid;
	urec->uur_relfilenode = InvalidOid;
	urec->uur_type = UNDO_UNKNOWN;
	urec->uur_info = UNDO_UREC_INFO_UNKNOWN;

	urec->uur_tpprevurp = INVALID_UNDO_REC_PTR;
	urec->uur_blkno = InvalidBlockNumber;
	urec->uur_offset = InvalidOffsetNumber;
	urec->uur_txnprevurp = INVALID_UNDO_REC_PTR;
	urec->uur_payloadlen = 0; 
	urec->uur_prevxid = InvalidFullTransactionId;
	urec->uur_tablespace = InvalidOid;
	urec->uur_subxid= InvalidFullTransactionId;
	urec->uur_dataheaderflag = 0;

	if (BufferIsValid(urec->uur_buffer))
	{
		if (!IS_VALID_UNDO_REC_PTR(urp) ||
			(UNDO_PTR_GET_LOG_NO(urp) != UNDO_PTR_GET_LOG_NO(urec->uur_urp)) ||
			(UNDO_PTR_GET_BLOCK_NUM(urp) != BufferGetBlockNumber(urec->uur_buffer)))
		{
			BufferDesc *buf_desc = GetBufferDescriptor(urec->uur_buffer - 1);
			if (LWLockHeldByMe(&buf_desc->content_lock))
			{
				elog(DEBUG2, "Release Buffer %d when Reset UnpackedUndoRecord from %lu to %lu.",
					 urec->uur_buffer, urec->uur_urp, urp);
				LockBuffer(urec->uur_buffer, BUFFER_LOCK_UNLOCK);
			}
			ReleaseBuffer(urec->uur_buffer);
			urec->uur_buffer = InvalidBuffer;
		}
	}

	if (urec->is_copy && urec->uur_payload.data != NULL)
	{
		pfree(urec->uur_payload.data);
	}

	urec->uur_payload.data = NULL;
	urec->uur_payload.len = 0;
	urec->uur_urp = urp;
	urec->buffer_idx = -1;
	urec->is_update = false;
	urec->is_copy = true;
}

void
reset_undo_record2(UnpackedUndoRecord *urec)
{
	reset_undo_record(urec, GetUndoRecordTpprev(urec));
}

UndoRecordSize
unpack_undo_record_size(UnpackedUndoRecord *urec)
{
	return sizeof(UnpackedUndoRecord) + urec->uur_payload.len;
}

UndoRecordSize
undo_record_expected_size(UnpackedUndoRecord *urec)
{
	UndoRecordSize size = SIZE_OF_UNDO_RECORD_HEADER + SIZE_OF_UNDO_RECORD_BLOCK + sizeof(UndoRecordSize);
	if ((urec->uur_info & UREC_INFO_PAYLOAD) != 0)
	{
		size += sizeof(UndoRecordSize);
		size += urec->uur_payload.len;
	}
	if ((urec->uur_info & UREC_INFO_PREURP) != 0)
	{
		size += sizeof(UndoRecPtr);
	}
	if ((urec->uur_info & UREC_INFO_PREXID) != 0)
	{
		size += sizeof(FullTransactionId);
	}
    if ((urec->uur_info & UREC_INFO_DATAHEADER) != 0) 
	{
		size += sizeof(uint16);
	}
	if ((urec->uur_info & UREC_INFO_SUBXACT))
	{
		size += sizeof(FullTransactionId);
	}
	if ((urec->uur_info & UREC_INFO_TABLESPACE) != 0)
	{
		size += sizeof(Oid);
	}

	return size;
}

/*
 * To insert an undo record, call insert_undo_record() repeatedly until it
 * returns true.
 *
 * Insert as much of an undo record as will fit in the given page.
 * starting_byte is the byte within the give page at which to begin writing,
 * while *already_written is the number of bytes written to previous pages.
 *
 * Returns true if the remainder of the record was written and false if more
 * bytes remain to be written; in either case, *already_written is set to the
 * number of bytes written thus far.
 *
 * This function assumes that if *already_written is non-zero on entry, the
 * same UnpackedUndoRecord is passed each time.  It also assumes that
 * unpack_undo_record is not called between successive calls to insert_undo_record
 * for the same UnpackedUndoRecord.
 *
 * If this function is called again to continue writing the record, the
 * previous value for *already_written should be passed again, and
 * starting_byte should be passed as sizeof(PageHeaderData) (since the record
 * will continue immediately following the page header).
 *
 * remaining_bytes number of bytes to be written yet.  This value is only
 * considered when page is NULL and that is required when caller just wanted
 * the local work_hdr and the already_written variable to get updated but
 * don't want to insert actual data in current block and work_hdr should be
 * updated so that we can insert the remaining partial record in the next
 * valid block.
 */
bool
insert_undo_record(UnpackedUndoRecord *uur, Page page, int starting_byte, int *already_written, int remaining_bytes, 
				 UndoRecordSize undo_len)
{
	char *writeptr = (char *) page + starting_byte;
	char *endptr = (char *) page + BLCKSZ;
	int	  my_bytes_written = *already_written;

	/*
	 * If this is the first call, copy the UnpackedUndoRecord into the
	 * temporary variables of the types that will actually be stored in the
	 * undo pages.  We just initialize everything here, on the assumption that
	 * it's not worth adding branches to save a handful of assignments.
	 */
	if (*already_written == 0)
	{	
		work_hdr.urec_xid = uur->uur_xid;
		work_hdr.urec_cid = uur->uur_cid;
		work_hdr.urec_reloid = uur->uur_reloid;
		work_hdr.urec_relfilenode = uur->uur_relfilenode;
		work_hdr.urec_type = uur->uur_type;
		work_hdr.urec_info = uur->uur_info;
		work_blk.urec_tupprev =	uur->uur_tpprevurp;
		work_blk.urec_block = uur->uur_blkno;
		work_blk.urec_offset = uur->uur_offset;
	}
	else
	{
		/*
		 * We should have been passed the same record descriptor as before, or
		 * caller has messed up.
		 */
		Assert(work_hdr.urec_type == uur->uur_type);
		Assert(work_hdr.urec_info == uur->uur_info);
		Assert(work_hdr.urec_reloid == uur->uur_reloid);
		Assert(work_hdr.urec_xid.value == uur->uur_xid.value);
		Assert(work_hdr.urec_cid == uur->uur_cid);
		Assert(work_blk.urec_tupprev == uur->uur_tpprevurp);
		Assert(work_blk.urec_block == uur->uur_blkno);
		Assert(work_blk.urec_offset == uur->uur_offset);
	}

    /*
	 * Update already_written variable and return, see detailed comment in
	 * function header.
	 */
	if (page == NULL)
	{
		*already_written += (BLCKSZ - starting_byte);
		if (remaining_bytes <= (BLCKSZ - starting_byte))
			return true;
		else
			return false;
	}

    /* Write header (if not already done). */
	if (!insert_undo_bytes((char *) &work_hdr, SIZE_OF_UNDO_RECORD_HEADER, &writeptr,
						 endptr, &my_bytes_written, already_written))
	{
		return false;
	}
	
	/* Write block information (if needed and not already done). */
	if (!insert_undo_bytes((char *) &work_blk, SIZE_OF_UNDO_RECORD_BLOCK, &writeptr,
						 endptr, &my_bytes_written, already_written))
	{
		return false;
	}
	if ((uur->uur_info & UREC_INFO_PREURP) != 0)
	{
		if (!insert_undo_bytes((char *) &uur->uur_txnprevurp, sizeof(UndoRecPtr),
							 &writeptr, endptr, &my_bytes_written, already_written))
		{
			return false;
		}
	}
	if ((uur->uur_info & UREC_INFO_PREXID) != 0)
	{
		if (!insert_undo_bytes((char *) &uur->uur_prevxid, sizeof(FullTransactionId), &writeptr,
							 endptr, &my_bytes_written, already_written))
		{
			return false;
		}
	}

	if ((uur->uur_info & UREC_INFO_TABLESPACE) != 0)
	{
		if (!insert_undo_bytes((char *) &uur->uur_tablespace, sizeof(Oid),
							 &writeptr, endptr, &my_bytes_written, already_written))
		{
			return false;
		}
	}

	if ((uur->uur_info & UREC_INFO_SUBXACT) != 0)
	{
		if (!insert_undo_bytes((char *) &uur->uur_subxid, sizeof(FullTransactionId),
							 &writeptr, endptr, &my_bytes_written, already_written))
		{
			return false;
		}
	}

	if ((uur->uur_info & UREC_INFO_DATAHEADER) != 0)
	{
		if (!insert_undo_bytes((char *) &uur->uur_dataheaderflag, sizeof(uint16),
							 &writeptr, endptr, &my_bytes_written, already_written))
		{
			return false;
		}
	}

	/* Write payload information (if needed and not already done). */
	if ((uur->uur_info & UREC_INFO_PAYLOAD) != 0)
	{
		/* Payload len. */
		uur->uur_payloadlen = uur->uur_payload.len;
		if (!insert_undo_bytes((char *) &uur->uur_payloadlen, sizeof(UndoRecordSize),
							 &writeptr, endptr, &my_bytes_written, already_written))
		{
			return false;
		}
		/* Payload bytes. */
		if (uur->uur_payloadlen > 0 &&
			!insert_undo_bytes((char *) uur->uur_payload.data, uur->uur_payload.len, &writeptr,
							 endptr, &my_bytes_written, already_written))
		{
			return false;
		}
	}

	/* Insert undo record length at the end of the record. */
	if (!insert_undo_bytes((char *) &undo_len, sizeof(UndoRecordSize), &writeptr, endptr,
						 &my_bytes_written, already_written))
	{
		return false;
	}

	elog(DEBUG5, "write undorecord urp %016lX blk no %u undolen %u.",
		 uur->uur_urp, GetUndoRecordBlkno(uur), undo_len);
	/* Hooray! */
	return true;
}


/*
 * Call unpack_undo_record() one or more times to unpack an undo record.  For
 * the first call, starting_byte should be set to the beginning of the undo
 * record within the specified page, and *already_decoded should be set to 0;
 * the function will update it based on the number of bytes decoded.  The
 * return value is true if the entire record was unpacked and false if the
 * record continues on the next page.  In the latter case, the function
 * should be called again with the next page, passing starting_byte as the
 * sizeof(PageHeaderData).
 */
bool
unpack_undo_record(UnpackedUndoRecord *uur, Page page, int starting_byte, 
				int *already_decoded, bool copy_data)
{
	char *readptr = (char *) page + starting_byte;
	char *endptr = (char *) page + BLCKSZ;
	int	  my_bytes_decoded = *already_decoded;
	bool  is_undo_splited = my_bytes_decoded > 0 ? true : false;
	Assert(page);


	if (!read_undo_bytes((char *) &work_hdr, SIZE_OF_UNDO_RECORD_HEADER, &readptr,
					   endptr, &my_bytes_decoded, already_decoded))
	{
		return false;
	}
	uur->uur_xid = work_hdr.urec_xid;
	uur->uur_cid = work_hdr.urec_cid;
	uur->uur_reloid = work_hdr.urec_reloid;
	uur->uur_relfilenode = work_hdr.urec_relfilenode;
	uur->uur_type = work_hdr.urec_type;
	uur->uur_info = work_hdr.urec_info;
	if (!read_undo_bytes((char *) &work_blk, SIZE_OF_UNDO_RECORD_BLOCK, &readptr, endptr,
					   &my_bytes_decoded, already_decoded))
	{
		return false;
	}
	uur->uur_tpprevurp = work_blk.urec_tupprev;
	uur->uur_blkno = work_blk.urec_block;
	uur->uur_offset = work_blk.urec_offset;

	if ((uur->uur_info & UREC_INFO_PREURP) != 0)
	{
		if (!read_undo_bytes((char *) &uur->uur_txnprevurp, sizeof(UndoRecPtr),
						   &readptr, endptr, &my_bytes_decoded, already_decoded))
		{
			return false;
		}
	}
	if ((uur->uur_info & UREC_INFO_PREXID) != 0)
	{
		if (!read_undo_bytes((char *) &uur->uur_prevxid, sizeof(FullTransactionId), &readptr,
						   endptr, &my_bytes_decoded, already_decoded))
		{
			return false;
		}
	}

	if ((uur->uur_info & UREC_INFO_TABLESPACE) != 0)
	{
		if (!read_undo_bytes((char *) &uur->uur_tablespace, sizeof(Oid),
						   &readptr, endptr, &my_bytes_decoded, already_decoded))
		{
			return false;
		}
	}

	if ((uur->uur_info & UREC_INFO_SUBXACT) !=0)
	{
		if (!read_undo_bytes((char *) &uur->uur_subxid, sizeof(FullTransactionId),
						   &readptr, endptr, &my_bytes_decoded, already_decoded))
		{
			return false;
		}
	}

	if(( uur->uur_info & UREC_INFO_DATAHEADER))
	{
		if (!read_undo_bytes((char *) &uur->uur_dataheaderflag, sizeof(uint16),
						   &readptr, endptr, &my_bytes_decoded, already_decoded))
		{
			return false;
		}
	}

	if ((uur->uur_info & UREC_INFO_PAYLOAD) != 0)
	{
		if (!read_undo_bytes((char *) &uur->uur_payloadlen, sizeof(UndoRecordSize), &readptr,
						   endptr, &my_bytes_decoded, already_decoded))
		{
			return false;
		}

		uur->uur_payload.len = uur->uur_payloadlen;
		if (uur->uur_payload.len > 0)
		{
			if (!copy_data && !is_undo_splited && uur->uur_payload.len <= (endptr - readptr))
			{
				uur->uur_payload.data = readptr;
				uur->is_copy = false;
			}
			else
			{
				if (uur->uur_payload.len > 0 && uur->uur_payload.data == NULL)
				{
					uur->uur_payload.data = (char *) MemoryContextAllocZero(
						CurrentMemoryContext, uur->uur_payload.len);
				}
				if (!read_undo_bytes((char *) uur->uur_payload.data, uur->uur_payload.len,
								   &readptr, endptr, &my_bytes_decoded, already_decoded))
				{
					return false;
				}
			}
		}
	}

	return true;
}


// if not in buffer, read from file
// caller should use destroy_undo_record to release buffer.
void
undo_get_one_record(UnpackedUndoRecord *urec, bool copyData)
{
	
	Page		page;
	Buffer		buffer = urec->uur_buffer;
	int			starting_byte = UNDO_PTR_GET_PAGE_OFFSET(urec->uur_urp);
	int			already_decoded = 0;
	BlockNumber cur_blk = UNDO_PTR_GET_BLOCK_NUM(urec->uur_urp);
	RelFileLocator rlocator;
	bool		is_undo_rec_split = false;
	int			logno = UNDO_PTR_GET_LOG_NO(urec->uur_urp);

	Assert(urec->uur_urp != INVALID_UNDO_REC_PTR);
	UNDO_PTR_ASSIGN_REL_FILE_LOCALTOR(rlocator, urec->uur_urp, UNDO_DATA_DB_OID);

	if (!BufferIsValid(buffer))
	{
		buffer = ReadUndoBufferWithoutRelcache(rlocator, MAIN_FORKNUM, cur_blk, RBM_NORMAL, NULL,
											   RELPERSISTENCE_PERMANENT);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		urec->uur_buffer = buffer;
	}
	else
	{
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
	}

	do
	{
		BufferDesc *bufDesc = GetBufferDescriptor(buffer - 1);
		page = BufferGetPage(buffer);
		if (bufDesc->tag.blockNum != cur_blk || bufDesc->tag.dbOid != UNDO_DATA_DB_OID ||
			bufDesc->tag.relNumber != (Oid)LOGNO_TO_REL_NUMBER(logno))
		{
			ereport(PANIC, (errmsg("undo buffer desc invalid, bufdesc: "
											  "dbid=%u, relid=%u, blockno=%u. "
											  "expect: dbid=%u, logno=%u, blockno=%u.",
								   bufDesc->tag.dbOid, bufDesc->tag.relNumber,
								   bufDesc->tag.blockNum, (Oid) UNDO_DATA_DB_OID, (Oid) logno,
								   cur_blk)));
		}
		if (already_decoded > BLCKSZ)
		{
			ereport(PANIC,
					(errmsg("undo record exceeds max size, readSize %d.",
							already_decoded)));
		}
		if (unpack_undo_record(urec, page, starting_byte, &already_decoded, copyData))
		{
			break;
		}

		/* Go to next block. */
		starting_byte = UNDO_LOG_BLOCK_HEADER_SIZE;
		cur_blk++;
		is_undo_rec_split = true;

		buffer = ReadUndoBufferWithoutRelcache(rlocator, MAIN_FORKNUM, cur_blk, RBM_NORMAL, NULL,
											   RELPERSISTENCE_PERMANENT);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
	} while (true);

    // always unlock and release the second buffer
	if (is_undo_rec_split)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}
	// unlock the first buffer
	buffer = urec->uur_buffer;
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
}


/*
 * Write undo bytes from a particular source, but only to the extent that
 * they weren't written previously and will fit.
 *
 * 'sourceptr' points to the source data, and 'sourcelen' is the length of
 * that data in bytes.
 *
 * 'writeptr' points to the insertion point for these bytes, and is updated
 * for whatever we write.  The insertion point must not pass 'endptr', which
 * represents the end of the buffer into which we are writing.
 *
 * 'my_bytes_written' is a pointer to the count of previous-written bytes
 * from this and following structures in this undo record; that is, any
 * bytes that are part of previous structures in the record have already
 * been subtracted out.
 *
 * 'total_bytes_written' points to the count of all previously-written bytes,
 * and must it must be updated for the bytes we write.
 *
 * The return value is false if we ran out of space before writing all
 * the bytes, and otherwise true.
 */
static bool
insert_undo_bytes(char *sourceptr, int sourcelen,
				char **writeptr, char *endptr,
				int *my_bytes_written, int *total_bytes_written)
{

	int			can_write;
	int			remaining;

	/*
	 * If we've previously written all of these bytes, there's nothing to do
	 * except update *my_bytes_written, which we must do to ensure that the
	 * next call to this function gets the right starting value.
	 */
	if (*my_bytes_written >= sourcelen)
	{
		*my_bytes_written -= sourcelen;
		return true;
	}

	/* Compute number of bytes we can write. */
	remaining = sourcelen - *my_bytes_written;
	can_write = Min(remaining, endptr - *writeptr);

	/* Bail out if no bytes can be written. */
	if (can_write == 0)
		return false;

	/* Copy the bytes we can write. */
	memcpy(*writeptr, sourceptr + *my_bytes_written, can_write);

	/* Update bookkeeeping infrormation. */
	*writeptr += can_write;
	*total_bytes_written += can_write;
	*my_bytes_written = 0;

	/* Return true only if we wrote the whole thing. */
	return (can_write == remaining);
}


/*
 * Read undo bytes into a particular destination,
 *
 * 'destptr' points to the source data, and 'readlen' is the length of
 * that data to be read in bytes.
 *
 * 'readptr' points to the read point for these bytes, and is updated
 * for how much we read.  The read point must not pass 'endptr', which
 * represents the end of the buffer from which we are reading.
 *
 * 'my_bytes_read' is a pointer to the count of previous-read bytes
 * from this and following structures in this undo record; that is, any
 * bytes that are part of previous structures in the record have already
 * been subtracted out.
 *
 * 'total_bytes_read' points to the count of all previously-read bytes,
 * and must likewise be updated for the bytes we read.
 *
 * nocopy if this flag is set true then it will just skip the readlen
 * size in undo but it will not copy into the buffer.
 *
 * The return value is false if we ran out of space before read all
 * the bytes, and otherwise true.
 */
static bool
read_undo_bytes(char *destptr, int readlen, char **readptr, char *endptr,
			  int *my_bytes_read, int *total_bytes_read)
{
	int			can_read;
	int			remaining;

	if (*my_bytes_read >= readlen)
	{
		*my_bytes_read -= readlen;
		return true;
	}

	/* Compute number of bytes we can read. */
	remaining = readlen - *my_bytes_read;
	can_read = Min(remaining, endptr - *readptr);

	/* Bail out if no bytes can be read. */
	if (can_read == 0)
		return false;

	memcpy(destptr + *my_bytes_read, *readptr, can_read);

	/* Update bookkeeping information. */
	*readptr += can_read;
	*total_bytes_read += can_read;
	*my_bytes_read = 0;

	return (can_read == remaining);
}
