/*-------------------------------------------------------------------------
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * 
 * xlru.c
 *		Xstore LRU buffering for transaction status logfiles
 *
 * We use a simple least-recently-used scheme to manage a pool of page
 * buffers.  Under ordinary circumstances we expect that write
 * traffic will occur mostly to the latest page (and to the just-prior
 * page, soon after a page transition).  Read traffic will probably touch
 * a larger span of pages, but in any case a fairly small number of page
 * buffers should be sufficient.  So, we just search the buffers using plain
 * linear search; there's no need for a hashtable or anything fancy.
 * The management algorithm is straight LRU except that we will never swap
 * out the latest page (since we know it's going to be hit again eventually).
 *
 * We use a control LWLock to protect the shared data structures, plus
 * per-buffer LWLocks that synchronize I/O for each buffer.  The control lock
 * must be held to examine or modify any shared state.  A process that is
 * reading in or writing out a page buffer does not hold the control lock,
 * only the per-buffer lock for the buffer it is working on.
 *
 * "Holding the control lock" means exclusive lock in all cases except for
 * XlruReadPage_ReadOnly(); see comments for XlruRecentlyUsed() for
 * the implications of that.
 *
 * When initiating I/O on a buffer, we acquire the per-buffer lock exclusively
 * before releasing the control lock.  The per-buffer lock is released after
 * completing the I/O, re-acquiring the control lock, and updating the shared
 * state.  (Deadlock is not possible here, because we never try to initiate
 * I/O when someone else is already doing I/O on the same buffer.)
 * To wait for I/O to complete, release the control lock, acquire the
 * per-buffer lock in shared mode, immediately release the per-buffer lock,
 * reacquire the control lock, and then recheck state (since arbitrary things
 * could have happened while we didn't have the lock).
 *
 * As with the regular buffer manager, it is possible for another process
 * to re-dirty a page that is currently being written out.  This is handled
 * by re-setting the page's page_dirty flag.
 *
 * 
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xlru.c
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "postgres.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/shmem.h"
#include "xheap/xlru.h"

#define XlruFileName(ctl, path, segno) \
	snprintf(path, MAXPGPATH, "%s/%04X%08X", (ctl)->Dir, \
    (uint32)((uint64)(segno) >> 32), (uint32)((segno) & (int64)0xFFFFFFFF)) 

/*
 * During XlruWriteAll(), we will usually not need to write more than one
 * or two physical files, but we may need to write several pages per file.  We
 * can consolidate the I/O requests by leaving files open until control returns
 * to XlruWriteAll().  This data structure remembers which files are open.
 */
#define MAX_XWRITEALL_BUFFERS	16

typedef struct XstoreLruWriteAllData
{
	int			num_files;		/* # files actually open */
	int			fd[MAX_XWRITEALL_BUFFERS];	/* their FD's */
	int64			segno[MAX_XWRITEALL_BUFFERS];	/* their log seg#s */
} XstoreLruWriteAllData;

typedef struct XstoreLruWriteAllData *XstoreLruWriteAll;

/*
 * Populate a file tag describing a segment file.  We only use the segment
 * number, since we can derive everything else we need by having separate
 * sync handler functions for Xmultixact.
 */
#define INIT_XLRUFILETAG(a,xx_handler,xx_segno) \
( \
	memset(&(a), 0, sizeof(FileTag)), \
	(a).handler = (xx_handler), \
	(a).full_segno = (xx_segno) \
)

/*
 * Macro to mark a buffer slot "most recently used".  Note multiple evaluation
 * of arguments!
 *
 * The reason for the if-test is that there are often many consecutive
 * accesses to the same page (particularly the latest page).  By suppressing
 * useless increments of cur_lru_count, we reduce the probability that old
 * pages' counts will "wrap around" and make them appear recently used.
 *
 * We allow this code to be executed concurrently by multiple processes within
 * XlruReadPage_ReadOnly().  As long as int reads and writes are atomic,
 * this should not cause any completely-bogus values to enter the computation.
 * However, it is possible for either cur_lru_count or individual
 * page_lru_count entries to be "reset" to lower values than they should have,
 * in case a process is delayed while it executes this macro.  With care in
 * XlruSelectLRUPage(), this does little harm, and in any case the absolute
 * worst possible consequence is a nonoptimal choice of page to evict.  The
 * gain from allowing concurrent reads of XLRU pages seems worth it.
 */
#define XlruRecentlyUsed(shared, slotno)	\
	do { \
		int		new_lru_count = (shared)->cur_lru_count; \
		if (new_lru_count != (shared)->page_lru_count[slotno]) { \
			(shared)->cur_lru_count = ++new_lru_count; \
			(shared)->page_lru_count[slotno] = new_lru_count; \
		} \
	} while (0)

/* Saved info for XlruReportIOError */
typedef enum
{
	XLRU_OPEN_FAILED,
	XLRU_SEEK_FAILED,
	XLRU_READ_FAILED,
	XLRU_WRITE_FAILED,
	XLRU_FSYNC_FAILED,
	XLRU_CLOSE_FAILED
} XlruErrorCause;

static XlruErrorCause xlru_errcause;
static int	xlru_errno;


static void XlruZeroLSNs(XlruCtl ctl, int slotno);
static void XlruWaitIO(XlruCtl ctl, int slotno);
static void XlruInternalWritePage(XlruCtl ctl, int slotno, XstoreLruWriteAll fdata);
static bool XlruPhysicalReadPage(XlruCtl ctl, int64 pageno, int slotno);
static bool XlruPhysicalWritePage(XlruCtl ctl, int64 pageno, int slotno,
								  XstoreLruWriteAll fdata);
static void XlruReportIOError(XlruCtl ctl, int64 pageno, FullTransactionId xid);
static int	XlruSelectLRUPage(XlruCtl ctl, int64 pageno);

static bool XlruScanDirCbDeleteCutoff(XlruCtl ctl, char *filename,
									  int64 segpage, void *data);
static void XlruInternalDeleteSegment(XlruCtl ctl, int64 segno);
static int64 strtoint64(const char* str, char** endptr, int base);

/*
 * Initialization of shared memory
 */
Size
XlruShmemSize(int nslots, int nlsns)
{
	Size		sz;

	/* we assume nslots isn't so large as to risk overflow */
	sz = MAXALIGN(sizeof(XlruSharedData));
	sz += MAXALIGN(nslots * sizeof(char *));	/* page_buffer[] */
	sz += MAXALIGN(nslots * sizeof(XlruPageStatus));	/* page_status[] */
	sz += MAXALIGN(nslots * sizeof(bool));	/* page_dirty[] */
	sz += MAXALIGN(nslots * sizeof(int64));	/* page_number[] */
	sz += MAXALIGN(nslots * sizeof(int));	/* page_lru_count[] */
	sz += MAXALIGN(nslots * sizeof(LWLockPadded));	/* buffer_locks[] */

	if (nlsns > 0)
		sz += MAXALIGN(nslots * nlsns * sizeof(XLogRecPtr));	/* group_lsn[] */

	return BUFFERALIGN(sz) + BLCKSZ * nslots;
}

/*
 * Initialize, or attach to, a xstore LRU cache in shared memory.
 *
 * ctl: address of local (unshared) control structure.
 * name: name of XLRU.  (This is user-visible, pick with care!)
 * nslots: number of page slots to use.
 * nlsns: number of LSN groups per page (set to zero if not relevant).
 * ctllock: LWLock to use to control access to the shared control structure.
 * subdir: PGDATA-relative subdirectory that will contain the files.
 * tranche_id: LWLock tranche ID to use for the XLRU's per-buffer LWLocks.
 * sync_handler: which set of functions to use to handle sync requests
 */
void
XlruInit(XlruCtl ctl, const char *name, int nslots, int nlsns,
			  LWLock *ctllock, const char *subdir, int tranche_id,
			  SyncRequestHandler sync_handler)
{
	XlruShared	shared;
	bool		found;

	shared = (XlruShared) ShmemInitStruct(name,
										  XlruShmemSize(nslots, nlsns),
										  &found);

	if (!IsUnderPostmaster)
	{
		/* Initialize locks and shared memory area */
		char	   *ptr;
		Size		offset;
		int			slotno;

		Assert(!found);

		memset(shared, 0, sizeof(XlruSharedData));

		shared->ControlLock = ctllock;

		shared->num_slots = nslots;
		shared->lsn_groups_per_page = nlsns;

		shared->cur_lru_count = 0;

		/* shared->latest_page_number will be set later */
		shared->xlru_stats_idx = pgstat_get_slru_index(name);

		ptr = (char *) shared;
		offset = MAXALIGN(sizeof(XlruSharedData));
		shared->page_buffer = (char **) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(char *));
		shared->page_status = (XlruPageStatus *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(XlruPageStatus));
		shared->page_dirty = (bool *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(bool));
		shared->page_number = (int64 *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(int64));
		shared->page_lru_count = (int *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(int));

		/* Initialize LWLocks */
		shared->buffer_locks = (LWLockPadded *) (ptr + offset);
		offset += MAXALIGN(nslots * sizeof(LWLockPadded));

		if (nlsns > 0)
		{
			shared->group_lsn = (XLogRecPtr *) (ptr + offset);
			offset += MAXALIGN(nslots * nlsns * sizeof(XLogRecPtr));
		}

		ptr += BUFFERALIGN(offset);
		for (slotno = 0; slotno < nslots; slotno++)
		{
			LWLockInitialize(&shared->buffer_locks[slotno].lock,
							 tranche_id);

			shared->page_buffer[slotno] = ptr;
			shared->page_status[slotno] = XLRU_PAGE_EMPTY;
			shared->page_dirty[slotno] = false;
			shared->page_lru_count[slotno] = 0;
			ptr += BLCKSZ;
		}

		/* Should fit to estimated shmem size */
		Assert(ptr - (char *) shared <= XlruShmemSize(nslots, nlsns));
	}
	else
		Assert(found);

	/*
	 * Initialize the unshared control struct, including directory path. We
	 * assume caller set PagePrecedes.
	 */
	ctl->shared = shared;
	ctl->sync_handler = sync_handler;
	strlcpy(ctl->Dir, subdir, sizeof(ctl->Dir));
}

/*
 * Initialize (or reinitialize) a page to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
int
XlruZeroPage(XlruCtl ctl, int64 pageno)
{
	XlruShared	shared = ctl->shared;
	int			slotno;

	/* Find a suitable buffer slot for the page */
	slotno = XlruSelectLRUPage(ctl, pageno);
	Assert(shared->page_status[slotno] == XLRU_PAGE_EMPTY ||
		   (shared->page_status[slotno] == XLRU_PAGE_VALID &&
			!shared->page_dirty[slotno]) ||
		   shared->page_number[slotno] == pageno);

	/* Mark the slot as containing this page */
	shared->page_number[slotno] = pageno;
	shared->page_status[slotno] = XLRU_PAGE_VALID;
	shared->page_dirty[slotno] = true;
	XlruRecentlyUsed(shared, slotno);

	/* Set the buffer to zeroes */
	MemSet(shared->page_buffer[slotno], 0, BLCKSZ);

	/* Set the LSNs for this new page to zero */
	XlruZeroLSNs(ctl, slotno);

	/* Assume this page is now the latest active page */
	shared->latest_page_number = pageno;

	/* update the stats counter of zeroed pages */
	pgstat_count_slru_page_zeroed(shared->xlru_stats_idx);

	return slotno;
}

/*
 * Zero all the LSNs we store for this xlru page.
 *
 * This should be called each time we create a new page, and each time we read
 * in a page from disk into an existing buffer.  (Such an old page cannot
 * have any interesting LSNs, since we'd have flushed them before writing
 * the page in the first place.)
 *
 * This assumes that InvalidXLogRecPtr is bitwise-all-0.
 */
static void
XlruZeroLSNs(XlruCtl ctl, int slotno)
{
	XlruShared	shared = ctl->shared;

	if (shared->lsn_groups_per_page > 0)
		MemSet(&shared->group_lsn[slotno * shared->lsn_groups_per_page], 0,
			   shared->lsn_groups_per_page * sizeof(XLogRecPtr));
}

/*
 * Wait for any active I/O on a page slot to finish.  (This does not
 * guarantee that new I/O hasn't been started before we return, though.
 * In fact the slot might not even contain the same page anymore.)
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static void
XlruWaitIO(XlruCtl ctl, int slotno)
{
	XlruShared	shared = ctl->shared;

	/* See notes at top of file */
	LWLockRelease(shared->ControlLock);
	LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_SHARED);
	LWLockRelease(&shared->buffer_locks[slotno].lock);
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	/*
	 * If the slot is still in an io-in-progress state, then either someone
	 * already started a new I/O on the slot, or a previous I/O failed and
	 * neglected to reset the page state.  That shouldn't happen, really, but
	 * it seems worth a few extra cycles to check and recover from it. We can
	 * cheaply test for failure by seeing if the buffer lock is still held (we
	 * assume that transaction abort would release the lock).
	 */
	if (shared->page_status[slotno] == XLRU_PAGE_READ_IN_PROGRESS ||
		shared->page_status[slotno] == XLRU_PAGE_WRITE_IN_PROGRESS)
	{
		if (LWLockConditionalAcquire(&shared->buffer_locks[slotno].lock, LW_SHARED))
		{
			/* indeed, the I/O must have failed */
			if (shared->page_status[slotno] == XLRU_PAGE_READ_IN_PROGRESS)
				shared->page_status[slotno] = XLRU_PAGE_EMPTY;
			else				/* write_in_progress */
			{
				shared->page_status[slotno] = XLRU_PAGE_VALID;
				shared->page_dirty[slotno] = true;
			}
			LWLockRelease(&shared->buffer_locks[slotno].lock);
		}
	}
}

/*
 * Find a page in a shared buffer, reading it in if necessary.
 * The page number must correspond to an already-initialized page.
 *
 * If write_ok is true then it is OK to return a page that is in
 * WRITE_IN_PROGRESS state; it is the caller's responsibility to be sure
 * that modification of the page is safe.  If write_ok is false then we
 * will not return the page until it is not undergoing active I/O.
 *
 * The passed-in xid is used only for error reporting, and may be
 * InvalidTransactionId if no specific xid is associated with the action.
 *
 * Return value is the shared-buffer slot number now holding the page.
 * The buffer's LRU access info is updated.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
int
XlruReadPage(XlruCtl ctl, int64 pageno, bool write_ok,
				  FullTransactionId xid)
{
	XlruShared	shared = ctl->shared;

	/* Outer loop handles restart if we must wait for someone else's I/O */
	for (;;)
	{
		int			slotno;
		bool		ok;

		/* See if page already is in memory; if not, pick victim slot */
		slotno = XlruSelectLRUPage(ctl, pageno);

		/* Did we find the page in memory? */
		if (shared->page_number[slotno] == pageno &&
			shared->page_status[slotno] != XLRU_PAGE_EMPTY)
		{
			/*
			 * If page is still being read in, we must wait for I/O.  Likewise
			 * if the page is being written and the caller said that's not OK.
			 */
			if (shared->page_status[slotno] == XLRU_PAGE_READ_IN_PROGRESS ||
				(shared->page_status[slotno] == XLRU_PAGE_WRITE_IN_PROGRESS &&
				 !write_ok))
			{
				XlruWaitIO(ctl, slotno);
				/* Now we must recheck state from the top */
				continue;
			}
			/* Otherwise, it's ready to use */
			XlruRecentlyUsed(shared, slotno);

			/* update the stats counter of pages found in the XLRU */
			pgstat_count_slru_page_hit(shared->xlru_stats_idx);

			return slotno;
		}

		/* We found no match; assert we selected a freeable slot */
		Assert(shared->page_status[slotno] == XLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == XLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));

		/* Mark the slot read-busy */
		shared->page_number[slotno] = pageno;
		shared->page_status[slotno] = XLRU_PAGE_READ_IN_PROGRESS;
		shared->page_dirty[slotno] = false;

		/* Acquire per-buffer lock (cannot deadlock, see notes at top) */
		LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_EXCLUSIVE);

		/* Release control lock while doing I/O */
		LWLockRelease(shared->ControlLock);

		/* Do the read */
		ok = XlruPhysicalReadPage(ctl, pageno, slotno);

		/* Set the LSNs for this newly read-in page to zero */
		XlruZeroLSNs(ctl, slotno);

		/* Re-acquire control lock and update page state */
		LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

		Assert(shared->page_number[slotno] == pageno &&
			   shared->page_status[slotno] == XLRU_PAGE_READ_IN_PROGRESS &&
			   !shared->page_dirty[slotno]);

		shared->page_status[slotno] = ok ? XLRU_PAGE_VALID : XLRU_PAGE_EMPTY;

		LWLockRelease(&shared->buffer_locks[slotno].lock);

		/* Now it's okay to ereport if we failed */
		if (!ok)
			XlruReportIOError(ctl, pageno, xid);

		XlruRecentlyUsed(shared, slotno);

		/* update the stats counter of pages not found in XLRU */
		pgstat_count_slru_page_read(shared->xlru_stats_idx);

		return slotno;
	}
}

/*
 * Find a page in a shared buffer, reading it in if necessary.
 * The page number must correspond to an already-initialized page.
 * The caller must intend only read-only access to the page.
 *
 * The passed-in xid is used only for error reporting, and may be
 * InvalidTransactionId if no specific xid is associated with the action.
 *
 * Return value is the shared-buffer slot number now holding the page.
 * The buffer's LRU access info is updated.
 *
 * Control lock must NOT be held at entry, but will be held at exit.
 * It is unspecified whether the lock will be shared or exclusive.
 */
int
XlruReadPage_ReadOnly(XlruCtl ctl, int64 pageno, FullTransactionId xid)
{
	XlruShared	shared = ctl->shared;
	int			slotno;

	/* Try to find the page while holding only shared lock */
	LWLockAcquire(shared->ControlLock, LW_SHARED);

	/* See if page is already in a buffer */
	for (slotno = 0; slotno < shared->num_slots; slotno++)
	{
		if (shared->page_number[slotno] == pageno &&
			shared->page_status[slotno] != XLRU_PAGE_EMPTY &&
			shared->page_status[slotno] != XLRU_PAGE_READ_IN_PROGRESS)
		{
			/* See comments for XlruRecentlyUsed macro */
			XlruRecentlyUsed(shared, slotno);

			/* update the stats counter of pages found in the XLRU */
			pgstat_count_slru_page_hit(shared->xlru_stats_idx);

			return slotno;
		}
	}

	/* No luck, so switch to normal exclusive lock and do regular read */
	LWLockRelease(shared->ControlLock);
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	return XlruReadPage(ctl, pageno, true, xid);
}

/*
 * Write a page from a shared buffer, if necessary.
 * Does nothing if the specified slot is not dirty.
 *
 * NOTE: only one write attempt is made here.  Hence, it is possible that
 * the page is still dirty at exit (if someone else re-dirtied it during
 * the write).  However, we *do* attempt a fresh write even if the page
 * is already being written; this is for checkpoints.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static void
XlruInternalWritePage(XlruCtl ctl, int slotno, XstoreLruWriteAll fdata)
{
	XlruShared	shared = ctl->shared;
	int64			pageno = shared->page_number[slotno];
	bool		ok;

	/* If a write is in progress, wait for it to finish */
	while (shared->page_status[slotno] == XLRU_PAGE_WRITE_IN_PROGRESS &&
		   shared->page_number[slotno] == pageno)
	{
		XlruWaitIO(ctl, slotno);
	}

	/*
	 * Do nothing if page is not dirty, or if buffer no longer contains the
	 * same page we were called for.
	 */
	if (!shared->page_dirty[slotno] ||
		shared->page_status[slotno] != XLRU_PAGE_VALID ||
		shared->page_number[slotno] != pageno)
		return;

	/*
	 * Mark the slot write-busy, and clear the dirtybit.  After this point, a
	 * transaction status update on this page will mark it dirty again.
	 */
	shared->page_status[slotno] = XLRU_PAGE_WRITE_IN_PROGRESS;
	shared->page_dirty[slotno] = false;

	/* Acquire per-buffer lock (cannot deadlock, see notes at top) */
	LWLockAcquire(&shared->buffer_locks[slotno].lock, LW_EXCLUSIVE);

	/* Release control lock while doing I/O */
	LWLockRelease(shared->ControlLock);

	/* Do the write */
	ok = XlruPhysicalWritePage(ctl, pageno, slotno, fdata);

	/* If we failed, and we're in a flush, better close the files */
	if (!ok && fdata)
	{
		int			i;

		for (i = 0; i < fdata->num_files; i++)
			CloseTransientFile(fdata->fd[i]);
	}

	/* Re-acquire control lock and update page state */
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	Assert(shared->page_number[slotno] == pageno &&
		   shared->page_status[slotno] == XLRU_PAGE_WRITE_IN_PROGRESS);

	/* If we failed to write, mark the page dirty again */
	if (!ok)
		shared->page_dirty[slotno] = true;

	shared->page_status[slotno] = XLRU_PAGE_VALID;

	LWLockRelease(&shared->buffer_locks[slotno].lock);

	/* Now it's okay to ereport if we failed */
	if (!ok)
		XlruReportIOError(ctl, pageno, InvalidFullTransactionId);

	/* If part of a checkpoint, count this as a buffer written. */
	if (fdata)
		CheckpointStats.ckpt_bufs_written++;
}

/*
 * Wrapper of XlruInternalWritePage, for external callers.
 * fdata is always passed a NULL here.
 */
void
XlruWritePage(XlruCtl ctl, int slotno)
{
	XlruInternalWritePage(ctl, slotno, NULL);
}

/*
 * Return whether the given page exists on disk.
 *
 * A false return means that either the file does not exist, or that it's not
 * large enough to contain the given page.
 */
bool
XlruDoesPhysicalPageExist(XlruCtl ctl, int64 pageno)
{
	int64			segno = pageno / XLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % XLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;
	bool		result;
	off_t		endpos;

	/* update the stats counter of checked pages */
	pgstat_count_slru_page_exists(ctl->shared->xlru_stats_idx);

	XlruFileName(ctl, path, segno);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		/* expected: file doesn't exist */
		if (errno == ENOENT)
			return false;

		/* report error normally */
		xlru_errcause = XLRU_OPEN_FAILED;
		xlru_errno = errno;
		XlruReportIOError(ctl, pageno, InvalidFullTransactionId);
	}

	if ((endpos = lseek(fd, 0, SEEK_END)) < 0)
	{
		xlru_errcause = XLRU_SEEK_FAILED;
		xlru_errno = errno;
		XlruReportIOError(ctl, pageno, InvalidFullTransactionId);
	}

	result = endpos >= (off_t) (offset + BLCKSZ);

	if (CloseTransientFile(fd) != 0)
	{
		xlru_errcause = XLRU_CLOSE_FAILED;
		xlru_errno = errno;
		return false;
	}

	return result;
}

/*
 * Physical read of a (previously existing) page into a buffer slot
 *
 * On failure, we cannot just ereport(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return false and save enough
 * info in static variables to let XlruReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * read/write operations.  We could cache one virtual file pointer ...
 */
static bool
XlruPhysicalReadPage(XlruCtl ctl, int64 pageno, int slotno)
{
	XlruShared	shared = ctl->shared;
	int64			segno = pageno / XLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % XLRU_PAGES_PER_SEGMENT;
	off_t		offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd;

	XlruFileName(ctl, path, segno);

	/*
	 * In a crash-and-restart situation, it's possible for us to receive
	 * commands to set the commit status of transactions whose bits are in
	 * already-truncated segments of the commit log (see notes in
	 * XlruPhysicalWritePage).  Hence, if we are InRecovery, allow the case
	 * where the file doesn't exist, and return zeroes instead.
	 */
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno != ENOENT || !InRecovery)
		{
			xlru_errcause = XLRU_OPEN_FAILED;
			xlru_errno = errno;
			return false;
		}

		ereport(LOG,
				(errmsg("file \"%s\" doesn't exist, reading as zeroes",
						path)));
		MemSet(shared->page_buffer[slotno], 0, BLCKSZ);
		return true;
	}

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_SLRU_READ);
	if (pg_pread(fd, shared->page_buffer[slotno], BLCKSZ, offset) != BLCKSZ)
	{
		pgstat_report_wait_end();
		xlru_errcause = XLRU_READ_FAILED;
		xlru_errno = errno;
		CloseTransientFile(fd);
		return false;
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
	{
		xlru_errcause = XLRU_CLOSE_FAILED;
		xlru_errno = errno;
		return false;
	}

	return true;
}

/*
 * Physical write of a page from a buffer slot
 *
 * On failure, we cannot just ereport(ERROR) since caller has put state in
 * shared memory that must be undone.  So, we return false and save enough
 * info in static variables to let XlruReportIOError make the report.
 *
 * For now, assume it's not worth keeping a file pointer open across
 * independent read/write operations.  We do batch operations during
 * XlruWriteAll, though.
 *
 * fdata is NULL for a standalone write, pointer to open-file info during
 * XlruWriteAll.
 */
static bool
XlruPhysicalWritePage(XlruCtl ctl, int64 pageno, int slotno, XstoreLruWriteAll fdata)
{
	XlruShared	shared = ctl->shared;
	int64			segno = pageno / XLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % XLRU_PAGES_PER_SEGMENT;
	off_t		offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];
	int			fd = -1;

	/* update the stats counter of written pages */
	pgstat_count_slru_page_written(shared->xlru_stats_idx);

	/*
	 * Honor the write-WAL-before-data rule, if appropriate, so that we do not
	 * write out data before associated WAL records.  This is the same action
	 * performed during FlushBuffer() in the main buffer manager.
	 */
	if (shared->group_lsn != NULL)
	{
		/*
		 * We must determine the largest async-commit LSN for the page. This
		 * is a bit tedious, but since this entire function is a slow path
		 * anyway, it seems better to do this here than to maintain a per-page
		 * LSN variable (which'd need an extra comparison in the
		 * transaction-commit path).
		 */
		XLogRecPtr	max_lsn;
		int			lsnindex,
					lsnoff;

		lsnindex = slotno * shared->lsn_groups_per_page;
		max_lsn = shared->group_lsn[lsnindex++];
		for (lsnoff = 1; lsnoff < shared->lsn_groups_per_page; lsnoff++)
		{
			XLogRecPtr	this_lsn = shared->group_lsn[lsnindex++];

			if (max_lsn < this_lsn)
				max_lsn = this_lsn;
		}

		if (!XLogRecPtrIsInvalid(max_lsn))
		{
			/*
			 * As noted above, elog(ERROR) is not acceptable here, so if
			 * XLogFlush were to fail, we must PANIC.  This isn't much of a
			 * restriction because XLogFlush is just about all critical
			 * section anyway, but let's make sure.
			 */
			START_CRIT_SECTION();
			XLogFlush(max_lsn);
			END_CRIT_SECTION();
		}
	}

	/*
	 * During a WriteAll, we may already have the desired file open.
	 */
	if (fdata)
	{
		int			i;

		for (i = 0; i < fdata->num_files; i++)
		{
			if (fdata->segno[i] == segno)
			{
				fd = fdata->fd[i];
				break;
			}
		}
	}

	if (fd < 0)
	{
		/*
		 * If the file doesn't already exist, we should create it.  It is
		 * possible for this to need to happen when writing a page that's not
		 * first in its segment; we assume the OS can cope with that. (Note:
		 * it might seem that it'd be okay to create files only when
		 * XlruZeroPage is called for the first page of a segment.
		 * However, if after a crash and restart the REDO logic elects to
		 * replay the log from a checkpoint before the latest one, then it's
		 * possible that we will get commands to set transaction status of
		 * transactions that have already been truncated from the commit log.
		 * Easiest way to deal with that is to accept references to
		 * nonexistent files here and in XlruPhysicalReadPage.)
		 *
		 * Note: it is possible for more than one backend to be executing this
		 * code simultaneously for different pages of the same file. Hence,
		 * don't use O_EXCL or O_TRUNC or anything like that.
		 */
		XlruFileName(ctl, path, segno);
		fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
		if (fd < 0)
		{
			xlru_errcause = XLRU_OPEN_FAILED;
			xlru_errno = errno;
			return false;
		}

		if (fdata)
		{
			if (fdata->num_files < MAX_XWRITEALL_BUFFERS)
			{
				fdata->fd[fdata->num_files] = fd;
				fdata->segno[fdata->num_files] = segno;
				fdata->num_files++;
			}
			else
			{
				/*
				 * In the unlikely event that we exceed MAX_FLUSH_BUFFERS,
				 * fall back to treating it as a standalone write.
				 */
				fdata = NULL;
			}
		}
	}

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_SLRU_WRITE);
	if (pg_pwrite(fd, shared->page_buffer[slotno], BLCKSZ, offset) != BLCKSZ)
	{
		pgstat_report_wait_end();
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		xlru_errcause = XLRU_WRITE_FAILED;
		xlru_errno = errno;
		if (!fdata)
			CloseTransientFile(fd);
		return false;
	}
	pgstat_report_wait_end();

	/* Queue up a sync request for the checkpointer. */
	if (ctl->sync_handler != SYNC_HANDLER_NONE)
	{
		FileTag		tag;

		INIT_XLRUFILETAG(tag, ctl->sync_handler, segno);
		if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false))
		{
			/* No space to enqueue sync request.  Do it synchronously. */
			pgstat_report_wait_start(WAIT_EVENT_SLRU_SYNC);
			if (pg_fsync(fd) != 0)
			{
				pgstat_report_wait_end();
				xlru_errcause = XLRU_FSYNC_FAILED;
				xlru_errno = errno;
				CloseTransientFile(fd);
				return false;
			}
			pgstat_report_wait_end();
		}
	}

	/* Close file, unless part of flush request. */
	if (!fdata)
	{
		if (CloseTransientFile(fd) != 0)
		{
			xlru_errcause = XLRU_CLOSE_FAILED;
			xlru_errno = errno;
			return false;
		}
	}

	return true;
}

/*
 * Issue the error message after failure of XlruPhysicalReadPage or
 * XlruPhysicalWritePage.  Call this after cleaning up shared-memory state.
 */
static void
XlruReportIOError(XlruCtl ctl, int64 pageno, FullTransactionId xid)
{
	int64			segno = pageno / XLRU_PAGES_PER_SEGMENT;
	int			rpageno = pageno % XLRU_PAGES_PER_SEGMENT;
	int			offset = rpageno * BLCKSZ;
	char		path[MAXPGPATH];

	XlruFileName(ctl, path, segno);
	errno = xlru_errno;
	switch (xlru_errcause)
	{
		case XLRU_OPEN_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %lu", xid.value),
					 errdetail("Could not open file \"%s\": %m.", path)));
			break;
		case XLRU_SEEK_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %lu", xid.value),
					 errdetail("Could not seek in file \"%s\" to offset %d: %m.",
							   path, offset)));
			break;
		case XLRU_READ_FAILED:
			if (errno)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not access status of transaction %lu", xid.value),
						 errdetail("Could not read from file \"%s\" at offset %d: %m.",
								   path, offset)));
			else
				ereport(ERROR,
						(errmsg("could not access status of transaction %lu", xid.value),
						 errdetail("Could not read from file \"%s\" at offset %d: read too few bytes.", path, offset)));
			break;
		case XLRU_WRITE_FAILED:
			if (errno)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not access status of transaction %lu", xid.value),
						 errdetail("Could not write to file \"%s\" at offset %d: %m.",
								   path, offset)));
			else
				ereport(ERROR,
						(errmsg("could not access status of transaction %lu", xid.value),
						 errdetail("Could not write to file \"%s\" at offset %d: wrote too few bytes.",
								   path, offset)));
			break;
		case XLRU_FSYNC_FAILED:
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %lu", xid.value),
					 errdetail("Could not fsync file \"%s\": %m.",
							   path)));
			break;
		case XLRU_CLOSE_FAILED:
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access status of transaction %lu", xid.value),
					 errdetail("Could not close file \"%s\": %m.",
							   path)));
			break;
		default:
			/* can't get here, we trust */
			elog(ERROR, "unrecognized XLRU error cause: %d",
				 (int) xlru_errcause);
			break;
	}
}

/*
 * Select the slot to re-use when we need a free slot.
 *
 * The target page number is passed because we need to consider the
 * possibility that some other process reads in the target page while
 * we are doing I/O to free a slot.  Hence, check or recheck to see if
 * any slot already holds the target page, and return that slot if so.
 * Thus, the returned slot is *either* a slot already holding the pageno
 * (could be any state except EMPTY), *or* a freeable slot (state EMPTY
 * or CLEAN).
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
XlruSelectLRUPage(XlruCtl ctl, int64 pageno)
{
	XlruShared	shared = ctl->shared;

	/* Outer loop handles restart after I/O */
	for (;;)
	{
		int			slotno;
		int			cur_count;
		int			bestvalidslot = 0;	/* keep compiler quiet */
		int			best_valid_delta = -1;
		int64		best_valid_page_number = 0; /* keep compiler quiet */
		int			bestinvalidslot = 0;	/* keep compiler quiet */
		int			best_invalid_delta = -1;
		int64		best_invalid_page_number = 0;	/* keep compiler quiet */

		/* See if page already has a buffer assigned */
		for (slotno = 0; slotno < shared->num_slots; slotno++)
		{
			if (shared->page_number[slotno] == pageno &&
				shared->page_status[slotno] != XLRU_PAGE_EMPTY)
				return slotno;
		}

		/*
		 * If we find any EMPTY slot, just select that one. Else choose a
		 * victim page to replace.  We normally take the least recently used
		 * valid page, but we will never take the slot containing
		 * latest_page_number, even if it appears least recently used.  We
		 * will select a slot that is already I/O busy only if there is no
		 * other choice: a read-busy slot will not be least recently used once
		 * the read finishes, and waiting for an I/O on a write-busy slot is
		 * inferior to just picking some other slot.  Testing shows the slot
		 * we pick instead will often be clean, allowing us to begin a read at
		 * once.
		 *
		 * Normally the page_lru_count values will all be different and so
		 * there will be a well-defined LRU page.  But since we allow
		 * concurrent execution of XlruRecentlyUsed() within
		 * XlruReadPage_ReadOnly(), it is possible that multiple pages
		 * acquire the same lru_count values.  In that case we break ties by
		 * choosing the furthest-back page.
		 *
		 * Notice that this next line forcibly advances cur_lru_count to a
		 * value that is certainly beyond any value that will be in the
		 * page_lru_count array after the loop finishes.  This ensures that
		 * the next execution of XlruRecentlyUsed will mark the page newly
		 * used, even if it's for a page that has the current counter value.
		 * That gets us back on the path to having good data when there are
		 * multiple pages with the same lru_count.
		 */
		cur_count = (shared->cur_lru_count)++;
		for (slotno = 0; slotno < shared->num_slots; slotno++)
		{
			int			this_delta;
			int64			this_page_number;

			if (shared->page_status[slotno] == XLRU_PAGE_EMPTY)
				return slotno;
			this_delta = cur_count - shared->page_lru_count[slotno];
			if (this_delta < 0)
			{
				/*
				 * Clean up in case shared updates have caused cur_count
				 * increments to get "lost".  We back off the page counts,
				 * rather than trying to increase cur_count, to avoid any
				 * question of infinite loops or failure in the presence of
				 * wrapped-around counts.
				 */
				shared->page_lru_count[slotno] = cur_count;
				this_delta = 0;
			}
			this_page_number = shared->page_number[slotno];
			if (this_page_number == shared->latest_page_number)
				continue;
			if (shared->page_status[slotno] == XLRU_PAGE_VALID)
			{
				if (this_delta > best_valid_delta ||
					(this_delta == best_valid_delta &&
					 this_page_number < best_valid_page_number))
				{
					bestvalidslot = slotno;
					best_valid_delta = this_delta;
					best_valid_page_number = this_page_number;
				}
			}
			else
			{
				if (this_delta > best_invalid_delta ||
					(this_delta == best_invalid_delta &&
					 this_page_number < best_invalid_page_number))
				{
					bestinvalidslot = slotno;
					best_invalid_delta = this_delta;
					best_invalid_page_number = this_page_number;
				}
			}
		}

		/*
		 * If all pages (except possibly the latest one) are I/O busy, we'll
		 * have to wait for an I/O to complete and then retry.  In that
		 * unhappy case, we choose to wait for the I/O on the least recently
		 * used slot, on the assumption that it was likely initiated first of
		 * all the I/Os in progress and may therefore finish first.
		 */
		if (best_valid_delta < 0)
		{
			XlruWaitIO(ctl, bestinvalidslot);
			continue;
		}

		/*
		 * If the selected page is clean, we're set.
		 */
		if (!shared->page_dirty[bestvalidslot])
			return bestvalidslot;

		/*
		 * Write the page.
		 */
		XlruInternalWritePage(ctl, bestvalidslot, NULL);

		/*
		 * Now loop back and try again.  This is the easiest way of dealing
		 * with corner cases such as the victim page being re-dirtied while we
		 * wrote it.
		 */
	}
}

/*
 * Write dirty pages to disk during checkpoint or database shutdown.  Flushing
 * is deferred until the next call to ProcessSyncRequests(), though we do fsync
 * the containing directory here to make sure that newly created directory
 * entries are on disk.
 */
void
XlruWriteAll(XlruCtl ctl, bool allow_redirtied)
{
	XlruShared	shared = ctl->shared;
	XstoreLruWriteAllData fdata;
	int			slotno;
	int64			pageno = 0;
	int			i;
	bool		ok;

	/* update the stats counter of flushes */
	pgstat_count_slru_flush(shared->xlru_stats_idx);

	/*
	 * Find and write dirty pages
	 */
	fdata.num_files = 0;

	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

	for (slotno = 0; slotno < shared->num_slots; slotno++)
	{
		XlruInternalWritePage(ctl, slotno, &fdata);

		/*
		 * In some places (e.g. checkpoints), we cannot assert that the slot
		 * is clean now, since another process might have re-dirtied it
		 * already.  That's okay.
		 */
		Assert(allow_redirtied ||
			   shared->page_status[slotno] == XLRU_PAGE_EMPTY ||
			   (shared->page_status[slotno] == XLRU_PAGE_VALID &&
				!shared->page_dirty[slotno]));
	}

	LWLockRelease(shared->ControlLock);

	/*
	 * Now close any files that were open
	 */
	ok = true;
	for (i = 0; i < fdata.num_files; i++)
	{
		if (CloseTransientFile(fdata.fd[i]) != 0)
		{
			xlru_errcause = XLRU_CLOSE_FAILED;
			xlru_errno = errno;
			pageno = fdata.segno[i] * XLRU_PAGES_PER_SEGMENT;
			ok = false;
		}
	}
	if (!ok)
		XlruReportIOError(ctl, pageno, InvalidFullTransactionId);

	/* Ensure that directory entries for new files are on disk. */
	if (ctl->sync_handler != SYNC_HANDLER_NONE)
		fsync_fname(ctl->Dir, true);
}

/*
 * Remove all segments before the one holding the passed page number
 *
 * All XLRUs prevent concurrent calls to this function, either with an LWLock
 * or by calling it only as part of a checkpoint.  Mutual exclusion must begin
 * before computing cutoffPage.  Mutual exclusion must end after any limit
 * update that would permit other backends to write fresh data into the
 * segment immediately preceding the one containing cutoffPage.  Otherwise,
 * when the XLRU is quite full, XlruTruncate() might delete that segment
 * after it has accrued freshly-written data.
 */
void
XlruTruncate(XlruCtl ctl, int64 cutoffPage)
{
	XlruShared	shared = ctl->shared;
	int			slotno;

	/* update the stats counter of truncates */
	pgstat_count_slru_truncate(shared->xlru_stats_idx);

	/*
	 * Scan shared memory and remove any pages preceding the cutoff page, to
	 * ensure we won't rewrite them later.  (Since this is normally called in
	 * or just after a checkpoint, any dirty pages should have been flushed
	 * already ... we're just being extra careful here.)
	 */
	LWLockAcquire(shared->ControlLock, LW_EXCLUSIVE);

restart:;

	/*
    * While we are holding the lock, make an important safety check: the
    * planned cutoff point must be <= the current endpoint page. Otherwise we
    * have already wrapped around, and proceeding with the truncation would
    * risk removing the current segment. 
	 */
	if (shared->latest_page_number < cutoffPage)
	{
		int64 page_page_num = shared->latest_page_number;
		LWLockRelease(shared->ControlLock);
		ereport(LOG,
				(errmsg("could not truncate directory \"%s\": apparent wraparound, latest_page_number: %lu, cutoffPage: %lu",
						ctl->Dir, page_page_num, cutoffPage)));
		return;
	}

	for (slotno = 0; slotno < shared->num_slots; slotno++)
	{
		if (shared->page_status[slotno] == XLRU_PAGE_EMPTY)
			continue;
		if (shared->page_number[slotno] >= cutoffPage)
			continue;

		/*
		 * If page is clean, just change state to EMPTY (expected case).
		 */
		if (shared->page_status[slotno] == XLRU_PAGE_VALID &&
			!shared->page_dirty[slotno])
		{
			shared->page_status[slotno] = XLRU_PAGE_EMPTY;
			continue;
		}

		/*
		 * Hmm, we have (or may have) I/O operations acting on the page, so
		 * we've got to wait for them to finish and then start again. This is
		 * the same logic as in XlruSelectLRUPage.  (XXX if page is dirty,
		 * wouldn't it be OK to just discard it without writing it?
		 * XlruMayDeleteSegment() uses a stricter qualification, so we might
		 * not delete this page in the end; even if we don't delete it, we
		 * won't have cause to read its data again.  For now, keep the logic
		 * the same as it was.)
		 */
		if (shared->page_status[slotno] == XLRU_PAGE_VALID)
			XlruInternalWritePage(ctl, slotno, NULL);
		else
			XlruWaitIO(ctl, slotno);
		goto restart;
	}

	LWLockRelease(shared->ControlLock);

	/* Now we can remove the old segment(s) */
	(void) XlruScanDirectory(ctl, XlruScanDirCbDeleteCutoff, &cutoffPage);
}

/*
 * Delete an individual XLRU segment.
 *
 * NB: This does not touch the XLRU buffers themselves, callers have to ensure
 * they either can't yet contain anything, or have already been cleaned out.
 */
static void
XlruInternalDeleteSegment(XlruCtl ctl, int64 segno)
{
	char		path[MAXPGPATH];

	/* Forget any fsync requests queued for this segment. */
	if (ctl->sync_handler != SYNC_HANDLER_NONE)
	{
		FileTag		tag;

		INIT_XLRUFILETAG(tag, ctl->sync_handler, segno);
		RegisterSyncRequest(&tag, SYNC_FORGET_REQUEST, true);
	}

	/* Unlink the file. */
	XlruFileName(ctl, path, segno);
	ereport(DEBUG2, (errmsg_internal("removing file \"%s\"", path)));
	unlink(path);
}

/*
 * Determine whether a segment is okay to delete.
 *
 * segpage is the first page of the segment, and cutoffPage is the oldest (in
 * PagePrecedes order) page in the XLRU containing still-useful data.  Since
 * every core PagePrecedes callback implements "wrap around", check the
 * segment's first and last pages:
 *
 * first<cutoff  && last<cutoff:  yes
 * first<cutoff  && last>=cutoff: no; cutoff falls inside this segment
 * first>=cutoff && last<cutoff:  no; wrap point falls inside this segment
 * first>=cutoff && last>=cutoff: no; every page of this segment is too young
 */
static bool
XlruMayDeleteSegment(XlruCtl ctl, int64 segpage, int64 cutoffPage)
{
	int64			seg_last_page = segpage + XLRU_PAGES_PER_SEGMENT - 1;

	Assert(segpage % XLRU_PAGES_PER_SEGMENT == 0);

	return (segpage < cutoffPage && seg_last_page < cutoffPage);
}

/*
 * XlruScanDirectory callback.
 *		This callback deletes segments prior to the one passed in as "data".
 */
static bool
XlruScanDirCbDeleteCutoff(XlruCtl ctl, char *filename, int64 segpage, void *data)
{
	int64			cutoffPage = *(int64 *) data;

	if (XlruMayDeleteSegment(ctl, segpage, cutoffPage))
		XlruInternalDeleteSegment(ctl, segpage / XLRU_PAGES_PER_SEGMENT);

	return false;				/* keep going */
}

/*
 * Scan the Xlru directory and apply a callback to each file found in it.
 *
 * If the callback returns true, the scan is stopped.  The last return value
 * from the callback is returned.
 *
 * The callback receives the following arguments: 1. the XlruCtl struct for the
 * xlru being truncated; 2. the filename being considered; 3. the page number
 * for the first page of that file; 4. a pointer to the opaque data given to us
 * by the caller.
 *
 * Note that the ordering in which the directory is scanned is not guaranteed.
 *
 * Note that no locking is applied.
 */
bool
XlruScanDirectory(XlruCtl ctl, XlruScanCallback callback, void *data)
{
	bool		retval = false;
	DIR		   *cldir;
	struct dirent *clde;
	int64			segno;
	int64			segpage;

	cldir = AllocateDir(ctl->Dir);
	while ((clde = ReadDir(cldir, ctl->Dir)) != NULL)
	{
		size_t		len;

		len = strlen(clde->d_name);

		if ((len == 12) && strspn(clde->d_name, "0123456789ABCDEF") == len)
		{
			segno = strtoint64(clde->d_name, NULL, 16);
			segpage = segno * XLRU_PAGES_PER_SEGMENT;

			elog(DEBUG2, "XlruScanDirectory invoking callback on %s/%s",
				 ctl->Dir, clde->d_name);
			retval = callback(ctl, clde->d_name, segpage, data);
			if (retval)
				break;
		}
	}
	FreeDir(cldir);

	return retval;
}

/*
 * strtoint64
 *		Converts 'str' into an 64-bit integer.
 *
 * This has the identical API to strtol(3), except that it will handle
 * 64-bit ints even where "long" is narrower than that.
 *
 * For the moment it seems sufficient to assume that the platform has
 * such a function somewhere; let's not roll our own.
 */
static int64
strtoint64(const char* str, char** endptr, int base)
{
#if defined(HAVE_STRTOLL) && SIZEOF_LONG < 8
    return strtoll(str, endptr, base);
#else
    return strtol(str, endptr, base);
#endif
}

/*
 * Individual XLRUs (clog, ...) have to provide a sync.c handler function so
 * that they can provide the correct "XlruCtl" (otherwise we don't know how to
 * build the path), but they just forward to this common implementation that
 * performs the fsync.
 */
int
XlruSyncFileTag(XlruCtl ctl, const FileTag *ftag, char *path)
{
	int			fd;
	int			save_errno;
	int			result;


	XlruFileName(ctl, path, ftag->full_segno);

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		return -1;

	pgstat_report_wait_start(WAIT_EVENT_SLRU_FLUSH_SYNC);
	result = pg_fsync(fd);
	pgstat_report_wait_end();
	save_errno = errno;

	CloseTransientFile(fd);

	errno = save_errno;
	return result;
}
