/* -------------------------------------------------------------------------
 *
 * undofile.h
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/undo/undofile.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef UNDOFILE_H
#define UNDOFILE_H
#include "storage/smgr.h"
#include "storage/sync.h"


/*** behavior for open or get ***/
/* ereport if segment not present */
#define EXTENSION_FAIL				(1 << 0)
/* return NULL if segment not present */
#define EXTENSION_RETURN_NULL		(1 << 1)
/* create new segments as needed */
#define EXTENSION_CREATE			(1 << 2)
/* create new segments if needed during recovery */
#define EXTENSION_CREATE_RECOVERY	(1 << 3)

extern int undo_sync_handler_pos;

/* Prototypes of functions exposed to SMgr. */
void undofile_init(void);
void undofile_open(SMgrRelation reln);
bool undofile_is_own(RelFileLocator rlocator);

void undofile_close(SMgrRelation reln, ForkNumber forknum); 
void undofile_create(SMgrRelation reln, ForkNumber forknum, bool is_redo);
bool undofile_exists(SMgrRelation reln, ForkNumber forknum);
void undofile_unlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool is_redo, BlockNumber block_num);
void undofile_extend(SMgrRelation reln, ForkNumber forknum,
				BlockNumber blockno, const void *buffer, bool skip_fsync);

void undofile_read(SMgrRelation reln, ForkNumber forknum,BlockNumber blockno,
						void **buffers, BlockNumber nblocks);
void undofile_write(SMgrRelation reln, ForkNumber forknum,BlockNumber blocknum,
						const void **buffers, BlockNumber nblocks,bool skip_fsync);

void undofile_writeback(SMgrRelation reln, ForkNumber forknum, BlockNumber blockNum, BlockNumber nblocks);
BlockNumber undofile_get_nblocks(SMgrRelation reln, ForkNumber forknum);

/* Prototypes of functions exposed to SyncOps. */
int	undofile_syncfiletag(const FileTag *ftag, char *path);
int	undofile_unlinkfiletag(const FileTag *ftag, char *path);


/* direct call functions */
void check_undo_dir(void);
void clean_undo_files(UndoPersistence);
void undo_unlink(SMgrRelation reln, bool isRedo);

#endif