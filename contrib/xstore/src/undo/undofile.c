/* -------------------------------------------------------------------------
 *
 * undofile.c
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    src/undo/undofile.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/xlog.h"
#include "pgstat.h"

#include "storage/fd.h"
#include "common/file_utils.h"
#include "utils/memutils.h"
#include "xstore.h"
#include "undo/undofile.h"

#include <sys/stat.h>


int undo_sync_handler_pos = -1;

/*
 * While md.c expects random access and has a small number of huge
 * segments, undofile.c manages a potentially very large number of smaller
 * segments and has a less random access pattern.  Therefore, instead of
 * keeping a potentially huge array of vfds we'll just keep the most
 * recently accessed N.
 *
 * For now, N == 1, so we just need to hold onto one 'File' handle.
 */
typedef struct UndoFileState
{
	int	 mru_segno;
	File mru_file;
} UndoFileState;

#define INIT_UNDO_FILETAG(a,xx_rlocator,xx_segno) \
( \
	memset(&(a), 0, sizeof(FileTag)), \
	(a).handler = undo_sync_handler_pos, \
	(a).rlocator = (xx_rlocator), \
	(a).forknum = (MAIN_FORKNUM), \
	(a).segno = (xx_segno) \
)

static MemoryContext UndoFileCxt;
const char *UNDO_FILE_BASE_DIR = "undo";

static void get_undo_segment_file_dir(char *path, int len);
static void get_undo_segment_file_name(int logno, uint32 db_id, int segno, char *path, int len);
static UndoFileState *undofile_get_segment_file(SMgrRelation reln, ForkNumber forknum,
								   BlockNumber block_num, int behavior);
static void register_dirty_undo_segment(SMgrRelation reln, const UndoFileState *state);
static void register_forget_undo_requests(RelFileLocatorBackend rlocator, uint32 segno);
static void register_unlink_undo_request(RelFileLocatorBackend rlocator, uint32 segno);
static BlockNumber get_undofile_blocks(SMgrRelation reln, ForkNumber forknum,
									 const UndoFileState *state);

 
void
undofile_init(void)
{
	UndoFileCxt = AllocSetContextCreate(TopMemoryContext,
										"UndoFileSmgr",
										ALLOCSET_DEFAULT_SIZES);
}

void undofile_open(SMgrRelation reln)
{
	reln->fileState = NULL;
	/* mark it not open */
	for (int forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		reln->md_num_open_segs[forknum] = 0;
}

bool undofile_is_own(RelFileLocator rlocator)
{
	if(rlocator.dbOid == UNDO_DATA_DB_OID || rlocator.dbOid == UNDO_TXN_DB_OID)
		return true;
	return false;
}

static void
get_undo_segment_file_dir(char *path, int len)
{
	Assert(len >= UNDOLOG_FILE_DIR_LEN);
	memset(path, '\0', len);
	strncpy(path,UNDO_FILE_BASE_DIR,strlen(UNDO_FILE_BASE_DIR));
	return;
}

void
check_undo_dir(void)
{
	if (mkdir(UNDO_FILE_BASE_DIR, S_IRWXU) < 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", UNDO_FILE_BASE_DIR)));
	return;
}

void
clean_undo_files(UndoPersistence up)
{
	char prefix[UNDOLOG_FILE_PATH_LEN];
	char filename[UNDOLOG_FILE_PATH_LEN];
	size_t pre_len = 0;
	struct dirent *de;
	int clean_count = 0;
	DIR *dir;
	char base_path[UNDOLOG_FILE_DIR_LEN];

	memset(prefix, '\0', UNDOLOG_FILE_PATH_LEN);
	memset(filename, '\0', UNDOLOG_FILE_PATH_LEN);
	snprintf(prefix, UNDOLOG_FILE_PATH_LEN, "%s.", UNDO_PERSISTENCE_STR(up));
	pre_len = strlen(prefix);
	get_undo_segment_file_dir(base_path, UNDOLOG_FILE_DIR_LEN);

	dir = opendir(base_path);
	if (!dir) {
		ereport(WARNING, (errmsg("can't open undo path \"%s\" ", base_path)));
		return ;
	}

	while ((de = readdir(dir))) {
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		if (strncmp(de->d_name, prefix, pre_len) == 0) {
			snprintf(filename, UNDOLOG_FILE_PATH_LEN, "%s/%s", base_path,de->d_name);
			if (unlink(filename) == 0)
				++clean_count;
			else
				ereport(WARNING, (errmsg("unlink undofile %s fail ", filename)));
		}
	}
	closedir(dir);
	ereport(INFO, (errmsg("remove %d undo files for persistence type %d  ",
	      clean_count, up)));

	return;
}

void
get_undo_segment_file_name(int logno, uint32 db_id, int segno, char *path, int len)
{
	char			dir[UNDOLOG_FILE_DIR_LEN];
	UndoPersistence upersistence;
	Assert(len >= UNDOLOG_FILE_PATH_LEN);
	GET_UPERSISTENCE_BY_LOGNO(upersistence, logno);
	get_undo_segment_file_dir(dir, UNDOLOG_FILE_DIR_LEN);
	if (db_id == UNDO_DATA_DB_OID)
		snprintf(path, len, "%s/%s.%05X.%08X.dat", dir, UNDO_PERSISTENCE_STR(upersistence), logno, segno);
	else
		snprintf(path, len, "%s/%s.%05X.%08X.txn", dir, UNDO_PERSISTENCE_STR(upersistence), logno, segno);
	return;
}

BlockNumber
undofile_get_nblocks(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * xlogutils.c likes to call this to decide whether to read or extend; for
	 * now we lie and say the relation is big as possible.
	 */
	return MaxBlockNumber;
}


/* Get number of blocks present in a single disk undofile. */
static BlockNumber
get_undofile_blocks(SMgrRelation reln, ForkNumber forknum, const UndoFileState *state)
{
	char  *file_name;
	off_t  len;
	uint32 undo_file_size;
	Assert(state != NULL);

	file_name = FilePathName(state->mru_file);
	len = FileSize(state->mru_file);  
	undo_file_size = UNDOLOG_FILE_SIZE(reln->smgr_rlocator.locator.dbOid);

	if (len < 0)
	{
		undofile_close(reln, forknum);
		ereport(ERROR, (errmsg("could not seek to end of file \"%s\"",
							   file_name)));
	}

	if (len % BLCKSZ != 0)
	{
		undofile_close(reln, forknum);
		ereport(WARNING, (errmsg("The expected size of file \"%s\" is %d, but the actual size is %ld.",
								 file_name, undo_file_size, len)));
	}

	/* note that this calculation will ignore any partial block at EOF */
	return (BlockNumber) (len / BLCKSZ);
}

void
undofile_create(SMgrRelation reln, ForkNumber forknum, bool is_redo)
{
	/* Undo file creation is managed by undofile_extend. */
	return;
}

/* Create an undo file, expand the file by 8 pages until the file size reaches segment maxsize. */
void
undofile_extend(SMgrRelation reln, ForkNumber forknum,
				BlockNumber blockno, const void *buffer, bool skip_fsync)
{
	UndoFileState  *state;
	uint32			undo_file_blocks;
	uint32			undo_file_size;
	int				segno = -1;
	int             logno = -1;
	char			path[UNDOLOG_FILE_PATH_LEN];
	File			fd;
	volatile uint32 flags = O_RDWR | O_CREAT | PG_BINARY;
	int				nbytes;
	off_t			seekpos;
	struct stat		stat_buffer;
	BlockNumber		block_num;
	char			undo_buffer[BLCKSZ] = {'\0'};

	Assert(reln != NULL);
	state = (UndoFileState *) reln->fileState;
	undo_file_blocks = UNDOLOG_FILE_BLOCKS(reln->smgr_rlocator.locator.dbOid);
	undo_file_size = UNDOLOG_FILE_SIZE(reln->smgr_rlocator.locator.dbOid);

	if (blockno == InvalidBlockNumber)
		ereport(ERROR, (errmsg("cannot extend undo file beyond %u blocks.",
							   InvalidBlockNumber)));
	if (state == NULL)
	{
		state = MemoryContextAllocZero(UndoFileCxt, sizeof(UndoFileState));
		reln->fileState = state;
	}
	if (state->mru_file > 0)
		FileClose(state->mru_file);
	state->mru_segno= -1;
	state->mru_file = -1;

	logno = REL_NUMBER_TO_LOGNO(reln->smgr_rlocator.locator.relNumber);
	segno = (int) (blockno / undo_file_blocks);
	get_undo_segment_file_name(logno, reln->smgr_rlocator.locator.dbOid, segno,
					path, UNDOLOG_FILE_PATH_LEN);

	fd = PathNameOpenFile(path, (int) flags);
	if (fd < 0)
	{
		int				save_errno = errno;

		fd = PathNameOpenFile(path, (int) flags);
		if (fd < 0)
		{
			/* be sure to report the error reported by create, not open */
			errno = save_errno;
			undofile_close(reln, forknum);
			ereport(ERROR,
					(errmsg("could not create file %s : %m", path)));
		}
	}

	if (fstat(FileGetRawDesc(fd), &stat_buffer) < 0)
	{
		undofile_close(reln, forknum);
		ereport(ERROR, (errmsg("could not stat file %s : %m", path)));
	}

	state->mru_segno= segno;
	state->mru_file = fd;
	seekpos = stat_buffer.st_size;


	/* Extend file to undoFileSize. */
	while (seekpos < (off_t) undo_file_size)
	{
		off_t diff_size = (off_t) undo_file_size - seekpos;

		if (diff_size < BLCKSZ)
			nbytes = FileWrite(fd, (char *) undo_buffer, (int) diff_size,seekpos,
							   (uint32) WAIT_EVENT_DATA_FILE_EXTEND);
		else
			nbytes = FileWrite(fd, (char *) undo_buffer, BLCKSZ,seekpos,
							   (uint32) WAIT_EVENT_DATA_FILE_EXTEND);
		if (nbytes < 0)
		{
			undofile_close(reln, forknum);
			if (unlink(path) != 0)
				ereport(ERROR, (errmsg("could not delete undo file during initialization %s : %m",
									   path)));
			ereport(ERROR,
					(errmsg("could not initialize undo log segment file \"%s\" : %m",path)));
		}
		seekpos += (off_t) nbytes;
	}

	if (!skip_fsync && !SmgrIsTemp(reln))
		register_dirty_undo_segment(reln, state);
	elog(DEBUG1, "undo file \"%s\" extended to %u blocks.", path,
		 undo_file_blocks);

	block_num = get_undofile_blocks(reln, forknum, state);
	if (block_num != undo_file_blocks)
		ereport(
			PANIC,
			(errmsg("The undo file \"%s\" size is incorrect, blockNum=%u.",
					path, block_num)));
	return;
}

static UndoFileState *
undofile_get_segment_file(SMgrRelation reln, ForkNumber forknum, BlockNumber blockno, int behavior)
{
	UndoFileState *state;
	uint32		   blocks_per_undo;
	char		   path[UNDOLOG_FILE_PATH_LEN];
	File		   fd;
	uint32		   flags = O_RDWR | PG_BINARY;
	int			   segno;
	BlockNumber	   block_num;
	Assert(reln != NULL);

	state = (UndoFileState *) reln->fileState;
	blocks_per_undo = UNDOLOG_FILE_BLOCKS(reln->smgr_rlocator.locator.dbOid);
	segno = (int) (blockno / blocks_per_undo);

	if (blockno == InvalidBlockNumber)
		ereport(ERROR, (errmsg("cannot open undo file beyond %u blocks.",
							   InvalidBlockNumber)));

	if (state == NULL)
	{
		state = MemoryContextAllocZero(UndoFileCxt, sizeof(UndoFileState));
		reln->fileState = state;
	}

	/* No work if already open */

	if (state->mru_file > 0)
	{
		if (state->mru_segno== segno)
			return state;
		/* This is not the file we're looking for. */
		FileClose(state->mru_file);
	}
	state->mru_segno= -1;
	state->mru_file = -1;

	get_undo_segment_file_name(REL_NUMBER_TO_LOGNO(reln->smgr_rlocator.locator.relNumber), reln->smgr_rlocator.locator.dbOid, segno,
					path, UNDOLOG_FILE_PATH_LEN);
	fd = PathNameOpenFilePerm(path, (int) flags, S_IRUSR | S_IWUSR);

	if (fd < 0)
	{
		int eno = errno;
		if ((behavior & EXTENSION_RETURN_NULL) && FILE_POSSIBLY_DELETED(eno))
		{
			elog(INFO, "could not open undo file \"%s\" errno:%d ", path,
				 eno);
			return NULL;
		}
		if (RecoveryInProgress() && (behavior & EXTENSION_CREATE_RECOVERY))
		{
			elog(INFO, "recover undo file \"%s\" errno:%d ", path,eno);
			undofile_extend(reln, forknum, blockno, NULL, false);
			fd = PathNameOpenFilePerm(path, (int) flags, S_IRUSR | S_IWUSR);
		}
		if (fd < 0)
		{
			undofile_close(reln, forknum);
			ereport(ERROR, (errmsg("could not open undo file \"%s\": %m.",
									   path)));
		}
	}
	state->mru_segno= segno;
	state->mru_file = fd;

	block_num = get_undofile_blocks(reln, forknum, state);
	if (block_num < blocks_per_undo)
	{
		ereport(
			WARNING,
			(errmsg("The undo file \"%s\" blocknum %u is small than per undo file(%u blocks) ",
					path, block_num, blocks_per_undo)));
		undofile_extend(reln, forknum, blockno, NULL, false);
		return state;
	}
	else if (block_num > blocks_per_undo)
	{
		ereport(
			PANIC,
			(errmsg("The undo file \"%s\" size is big than per undo file(%u blocks), "
							   "file blockNum=%u.",
					path, blocks_per_undo, block_num)));
		return NULL;
	} 
	return state;
}

/* Read the specified block from a undo file. */
void undofile_read(SMgrRelation reln, ForkNumber forknum,BlockNumber blockno,
						void **buffers, BlockNumber nblocks)
{
	UndoFileState *state = NULL;
	char		  *fileName = NULL;
	off_t		   seekpos;
	int			   nbytes;
	uint32		   undoFileBlocks;
	void          *buffer = *buffers;

	Assert(buffer != NULL);
	Assert(nblocks == 1);
	undoFileBlocks = UNDOLOG_FILE_BLOCKS(reln->smgr_rlocator.locator.dbOid);

	state = undofile_get_segment_file(reln, forknum, blockno, EXTENSION_FAIL);
	if (state == NULL)
	{
		ereport(ERROR,
					(errmsg("file is truncated could not read block %u in file \"%s\": %m.",
							blockno, fileName)));
		return;
	}

	seekpos = (off_t) BLCKSZ * (blockno % undoFileBlocks);
	fileName = FilePathName(state->mru_file);

	Assert(seekpos < (off_t) (undoFileBlocks * BLCKSZ));

	nbytes = FileRead(state->mru_file, buffer, BLCKSZ,seekpos, WAIT_EVENT_DATA_FILE_READ);
	if (nbytes != BLCKSZ)
	{
		undofile_close(reln, forknum);
		if (nbytes < 0)
			ereport(ERROR,
					(errmsg("could not read block %u in file \"%s\": %m.",
							blockno, fileName)));
		else
			ereport(
				ERROR,
				(errmsg("could not read block %u in file \"%s\": read only %d of %d bytes.",
					blockno, fileName, nbytes, BLCKSZ)));
	}

	if (!PageIsVerifiedExtended((Page) buffer, blockno,
										PIV_LOG_WARNING | PIV_REPORT_STAT))
		ereport(
			ERROR,
			(errmsg("page is not verified %u in file \"%s\": read only %d of %d bytes.",
				blockno, fileName, nbytes, BLCKSZ)));
}


/*
 * Convert an array of buffer address into an array of iovec objects, and
 * return the number that were required.  'iov' must have enough space for up
 * to 'nblocks' elements, but the number used may be less depending on
 * merging.  In the case of a run of fully contiguous buffers, a single iovec
 * will be populated that can be handled as a plain non-vectored I/O.
 */
static int
undo_buffers_to_iovec(struct iovec *iov, void **buffers, int nblocks)
{
	struct iovec *iovp;
	int			iovcnt;

	Assert(nblocks >= 1);

	/* If this build supports direct I/O, buffers must be I/O aligned. */
	for (int i = 0; i < nblocks; ++i)
	{
		if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
			Assert((uintptr_t) buffers[i] ==
				   TYPEALIGN(PG_IO_ALIGN_SIZE, buffers[i]));
	}

	/* Start the first iovec off with the first buffer. */
	iovp = &iov[0];
	iovp->iov_base = buffers[0];
	iovp->iov_len = BLCKSZ;
	iovcnt = 1;

	/* Try to merge the rest. */
	for (int i = 1; i < nblocks; ++i)
	{
		void	   *buffer = buffers[i];

		if (((char *) iovp->iov_base + iovp->iov_len) == buffer)
		{
			/* Contiguous with the last iovec. */
			iovp->iov_len += BLCKSZ;
		}
		else
		{
			/* Need a new iovec. */
			iovp++;
			iovp->iov_base = buffer;
			iovp->iov_len = BLCKSZ;
			iovcnt++;
		}
	}

	return iovcnt;
}

void
undofile_write(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void **buffers, BlockNumber nblocks, bool skipFsync)
{

	uint32		   undoFileBlocks = UNDOLOG_FILE_BLOCKS(reln->smgr_rlocator.locator.dbOid);

	while (nblocks > 0)
	{
		struct iovec iov[PG_IOV_MAX];
		int			iovcnt;
		off_t		seekpos;
		int			nbytes;
		UndoFileState *state;
		BlockNumber nblocks_this_segment;
		size_t		transferred_this_segment;
		size_t		size_this_segment;

		// open file
		state = undofile_get_segment_file(reln, forknum, blocknum, EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);
		if (state == NULL)
			return;

		// start write pos
		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) undoFileBlocks));

		Assert(seekpos < (off_t) BLCKSZ * undoFileBlocks);

		nblocks_this_segment =
			Min(nblocks,
				undoFileBlocks - (blocknum % ((BlockNumber) undoFileBlocks)));
		nblocks_this_segment = Min(nblocks_this_segment, lengthof(iov));

		iovcnt = undo_buffers_to_iovec(iov, (void **) buffers, nblocks_this_segment);
		size_this_segment = nblocks_this_segment * BLCKSZ;
		transferred_this_segment = 0;

		/*
		 * Inner loop to continue after a short write.  If the reason is that
		 * we're out of disk space, a future attempt should get an ENOSPC
		 * error from the kernel.
		 */
		for (;;)
		{
			nbytes = FileWriteV(state->mru_file, iov, iovcnt, seekpos,
								WAIT_EVENT_DATA_FILE_WRITE);

			if (nbytes < 0)
			{
				bool		enospc = errno == ENOSPC;

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write blocks %u..%u in file \"%s\": %m",
								blocknum,
								blocknum + nblocks_this_segment - 1,
								FilePathName(state->mru_file)),
						 enospc ? errhint("Check free disk space.") : 0));
			}

			/* One loop should usually be enough. */
			transferred_this_segment += nbytes;
			Assert(transferred_this_segment <= size_this_segment);
			if (transferred_this_segment == size_this_segment)
				break;

			/* Adjust position and iovecs after a short write. */
			seekpos += nbytes;
			iovcnt = compute_remaining_iovec(iov, iov, iovcnt, nbytes);
		}

		if (!skipFsync && !SmgrIsTemp(reln))
			register_dirty_undo_segment(reln, state);

		nblocks -= nblocks_this_segment;
		buffers += nblocks_this_segment;
		blocknum += nblocks_this_segment;
	}
}


void
undofile_writeback(SMgrRelation reln, ForkNumber forknum, BlockNumber blockno,
				  BlockNumber nblocks)
{
	uint32 undoFileBlocks = UNDOLOG_FILE_BLOCKS(reln->smgr_rlocator.locator.dbOid);

	while (nblocks > 0)
	{
		UndoFileState *state = NULL;
		int			   segStart;
		int			   segEnd;
		BlockNumber	   nflush = nblocks;
		off_t		   seekpos;

		state = undofile_get_segment_file(reln, forknum, blockno, EXTENSION_RETURN_NULL);
		segStart = blockno / undoFileBlocks;
		segEnd = (blockno + nblocks - 1) / undoFileBlocks;
		/*
         * We might be flushing buffers of already removed relations, that's
         * ok, just ignore that case.
         */
		if (state == NULL)
			return;

		if (segStart != segEnd)
			nflush = undoFileBlocks - (blockno % undoFileBlocks);

		Assert(nflush >= 1);
		Assert(nflush <= nblocks);

		seekpos = (off_t) BLCKSZ * (blockno % undoFileBlocks);
		FileWriteback(state->mru_file, seekpos, (off_t) BLCKSZ * nflush,
					  WAIT_EVENT_DATA_FILE_WRITE);

		nblocks -= nflush;
		blockno += nflush;
	}
}

void
undofile_unlink(RelFileLocatorBackend rlocator, ForkNumber forkNum, bool isRedo,
			   BlockNumber blockNum)
{
	char   path[UNDOLOG_FILE_PATH_LEN];
	uint32 undoFileBlocks = UNDOLOG_FILE_BLOCKS(rlocator.locator.dbOid);
	int	   logno = REL_NUMBER_TO_LOGNO(rlocator.locator.relNumber);
	int	   segno = blockNum / undoFileBlocks;
	bool   remove = false;

	Assert(blockNum != InvalidBlockNumber);

	elog(DEBUG5,
		 "undofile_unlink : undofile (db %u spc %u node %u ,"
		 "forknum %d,blocknum %u, isRedo %d,segno %u)",
		 rlocator.locator.dbOid, rlocator.locator.spcOid, rlocator.locator.relNumber, forkNum, blockNum,
		 isRedo, segno);

	get_undo_segment_file_name(logno, rlocator.locator.dbOid, segno, path, UNDOLOG_FILE_PATH_LEN); 
	if (isRedo || forkNum != MAIN_FORKNUM)
	{
		register_forget_undo_requests(rlocator, segno);
		if (unlink(path) < 0 && errno != ENOENT)
		{
			/* try again */
			if ((unlink(path) < 0) && (errno != ENOENT))
			{
				ereport(WARNING,
					(errmsg("could not remove file \"%s\": %m.", path)));
			}
			else
			{
				remove = true;
			}
		}
		else
		{
			remove = true;
		}

		if (remove)
		{
			elog(DEBUG1, "unlink undo file \"%s\" ", path);
		}
	}
	else
	{
		int fd;
		int ret;
		fd = BasicOpenFile(path, O_RDWR | PG_BINARY);
		if (fd >= 0)
		{
			int save_errno;
			ret = ftruncate(fd, 0);
			save_errno = errno;
			(void) close(fd);
			errno = save_errno;
		}
		else
		{
			ret = -1;
		}
		if (ret < 0 && errno != ENOENT)
		{
			ereport(WARNING, (errcode_for_file_access(),
							  errmsg("could not truncate file \"%s\": %m", path)));
		}
		register_unlink_undo_request(rlocator, segno);
	}
	return;
}

void
undofile_close(SMgrRelation reln, ForkNumber forkNum)
{
	UndoFileState *state;
	Assert(reln != NULL);

	state = (UndoFileState *) reln->fileState;

	/* No work if already closed */
	if (state == NULL)
		return;
	reln->fileState = NULL; /* prevent dangling pointer after error */

	/* if not closed already */
	if (state->mru_file >= 0)
		FileClose(state->mru_file);
	pfree(state);
}

static void
register_forget_undo_requests(RelFileLocatorBackend rlocator, uint32 segno)
{
	FileTag tag;
	INIT_UNDO_FILETAG(tag, rlocator.locator, segno);

	Assert(undo_sync_handler_pos>0);

	elog(DEBUG1, "forget undo request : undofile (db %u spc %u node %u ,segno %u)",
		 rlocator.locator.dbOid, rlocator.locator.spcOid, rlocator.locator.relNumber, segno);
	(void) RegisterSyncRequest(&tag, SYNC_FORGET_REQUEST,true);
}

static void
register_unlink_undo_request(RelFileLocatorBackend rlocator, uint32 segno)
{
	FileTag tag;
	INIT_UNDO_FILETAG(tag, rlocator.locator, segno);
	register_forget_undo_requests(rlocator, segno);

	elog(DEBUG1, "register unlink : undofile (db %u spc %u node %u ,segno %u)",
		rlocator.locator.dbOid, rlocator.locator.spcOid, rlocator.locator.relNumber, segno);
	
	(void) RegisterSyncRequest(&tag, SYNC_UNLINK_REQUEST,true);
}

static void
register_dirty_undo_segment(SMgrRelation reln, const UndoFileState *state)
{
	FileTag tag;
	INIT_UNDO_FILETAG(tag, reln->smgr_rlocator.locator, state->mru_segno);

	Assert(undo_sync_handler_pos>0);

	if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false))
	{
		elog(
			DEBUG5,
			"could not forward fsync request because request queue is full.");

		if (FileSync(state->mru_file, WAIT_EVENT_DATA_FILE_SYNC) < 0)
			ereport(ERROR, (errmsg("could not fsync file \"%s\": %m.",
								   FilePathName(state->mru_file))));
	}
}

bool
undofile_exists(SMgrRelation reln, ForkNumber forkNum)
{
	BlockNumber block_num = reln->smgr_targblock;
	bool		is_exist = false;
	if(block_num == InvalidBlockNumber)
		return is_exist;

	/*
     * Close it first, to ensure that we notice if the fork has been unlinked
     * since we opened it.
     */
	undofile_close(reln, forkNum);

	if (undofile_get_segment_file(reln, forkNum, block_num, EXTENSION_CREATE_RECOVERY|EXTENSION_RETURN_NULL) != NULL)
		is_exist = true;
	undofile_close(reln, forkNum);
	return is_exist;
}


/*
 *	undo_unlink() -- close and unlink.
 */
void undo_unlink(SMgrRelation reln, bool isRedo)
{
	RelFileLocatorBackend rlocator = reln->smgr_rlocator;

	ForkNumber	forknum;

	/* Close the forks at smgr level */
	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		undofile_close(reln, forknum);

	/*
	 * Delete the physical file(s).
	 */
	undofile_unlink(rlocator, MAIN_FORKNUM, isRedo, reln->smgr_targblock);
}

/*
 * Sync a file to disk, given a file tag.  Write the path into an output
 * buffer so the caller can use it in error messages.
 *
 * Return 0 on success, -1 on failure, with errno set.
 */
int	undofile_syncfiletag(const FileTag *ftag, char *path)
{
	UndoFileState *state;
	SMgrRelation   reln = smgropen(ftag->rlocator, INVALID_PROC_NUMBER);
	uint32		   undoFileBlocks = UNDOLOG_FILE_BLOCKS(ftag->rlocator.dbOid);

	get_undo_segment_file_name(REL_NUMBER_TO_LOGNO(ftag->rlocator.relNumber), ftag->rlocator.dbOid, ftag->segno, path, UNDOLOG_FILE_PATH_LEN);
	state = undofile_get_segment_file(reln, ftag->forknum, ftag->segno * undoFileBlocks, EXTENSION_RETURN_NULL);
	if (state == NULL)
		return 0;

	if (FileSync(state->mru_file, WAIT_EVENT_DATA_FILE_SYNC) >= 0)
	{
		// success
		elog(DEBUG1, "checkpoint sync : undofile=%s  ", path);
		return 0;
	}
	elog(LOG, "could not fsync undofile \"%s\" but retrying: %m", path);

	return -1;
}

/*
 * Unlink a file, given a file tag.  Write the path into an output
 * buffer so the caller can use it in error messages.
 *
 * Return 0 on success, -1 on failure, with errno set.
 */
int	undofile_unlinkfiletag(const FileTag *ftag, char *path)
{
	get_undo_segment_file_name(REL_NUMBER_TO_LOGNO(ftag->rlocator.relNumber), ftag->rlocator.dbOid, ftag->segno, path, UNDOLOG_FILE_PATH_LEN);
	/* Try to unlink the file. */
	if (unlink(path) < 0)
	{
		if (errno != ENOENT)
			ereport(WARNING, (errcode_for_file_access(),
							  errmsg("could not remove undofile \"%s\": %m", path)));
		return -1;
	}
	elog(DEBUG1, "checkpoint unlink : undofile=%s ", path);
	return 0;
}