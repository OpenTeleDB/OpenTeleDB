/* -------------------------------------------------------------------------
 *
 * undobuffer.h
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/undo/undobuffer.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef UNDOBUFFER_H
#define UNDOBUFFER_H

#include "undo/undorecord.h"
#include "undo/undoxlog.h"
#include "undo/undolog.h"

/* Undo block number to buffer mapping. */
typedef struct UndoBuffer
{
	int			logno;  /* undo log number */
	BlockNumber blk;	/* block number */
	Buffer		buf;	/* buffer allocated for the block */
	bool		zero;	/* new block full of zeroes */
	bool        used;
} UndoBuffer;

/* Prepare buffers for inserting undo records. */
typedef struct UndoPrepareBuffers
{
	UnpackedUndoRecord	**uurecs;
	int			        uurec_size;
	int			        uurec_cap;
	UndoBuffer	        *ubuffers;
	int			        curr_idx;	 // current index for ubuffers
} UndoPrepareBuffers;


UndoPrepareBuffers*
new_undo_prepare_buffers(int num);

void
release_undo_prepare_buffers(UndoPrepareBuffers *upbuffers);

void
reset_undo_prepare_buffers(UndoPrepareBuffers *upbuffers, bool need_unlock);



bool
prepare_buffers_add_record(UndoPrepareBuffers *upbuffers, UnpackedUndoRecord *urec);

uint64
prepare_buffers_total_size(UndoPrepareBuffers *upbuffers);

UndoRecPtr
prepare_buffers_get_last_undoptr(UndoPrepareBuffers *upbuffers);

UndoRecordSize
prepare_buffers_get_last_recordsize(UndoPrepareBuffers *upbuffers);


/* The first record must be valid. */
UndoRecPtr
prepare_buffers_get_first_undoptr(UndoPrepareBuffers *upbuffers);

UnpackedUndoRecord*
prepare_buffers_get_undorecord(UndoPrepareBuffers *upbuffers, int idx);

/*
 * Prepare 0-n undo record.
 */
int
prepare_undo(UndoPrepareBuffers *upbuffers, UndoPersistence upersistence, 
	XLogReaderState *xlog_record, xl_undo_header *xlundohdr, xl_undo_meta *xlundometa);

/*
 * insert prepared undo record into undo file
 */
void
insert_prepared_undo(UndoPrepareBuffers *upbuffers, XLogRecPtr lsn);

/*
 * Set LSN for used buffer.
 */
void
prepare_buffers_set_page_lsn(UndoPrepareBuffers *upbuffers, XLogRecPtr lsn);

void
xlog_register_undo_buffers(UndoPrepareBuffers *upbuffers, uint8 first_block_id, UndoLogControl *ulog);

/*
* get used undo buffer count
*/
int
prepare_buffers_used_count(UndoPrepareBuffers *upbuffers);

/*
* release undo buffer in ctx cache
*/
void
release_cache_buffers_in_ctx(void);

/*
* reset undo buffer in ctx cache
*/
void
reset_prepared_buffers_in_ctx(void);


#endif