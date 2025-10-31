/*-------------------------------------------------------------------------
 *
 * xlru.h
 *		Xstore LRU buffering for transaction status logfiles
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * include/xheap/xlru.h
 *-------------------------------------------------------------------------
 */
#ifndef XLRU_H
#define XLRU_H

#include "postgres.h"
#include "access/xlogdefs.h"
#include "storage/lwlock.h"
#include "storage/sync.h"
#include "access/transam.h"

/*
 * Define XLRU segment size.  A page is the same BLCKSZ as is used everywhere
 * else in Postgres.  The segment size can be chosen somewhat arbitrarily;
 * we make it 2048 pages by default, or 16Mb.
 *
 * Note: xlru.c currently assumes that segment file names will be twelve hex
 * digits.	This sets a lower bound on the segment size ( 4M transactions
 * for 64-bit TransactionIds).
 */
#define XLRU_PAGES_PER_SEGMENT	2048

/*
 * Page status codes.  Note that these do not include the "dirty" bit.
 * page_dirty can be true only in the VALID or WRITE_IN_PROGRESS states;
 * in the latter case it implies that the page has been re-dirtied since
 * the write started.
 */
typedef enum
{
	XLRU_PAGE_EMPTY,			/* buffer is not in use */
	XLRU_PAGE_READ_IN_PROGRESS, /* page is being read in */
	XLRU_PAGE_VALID,			/* page is valid and not being written */
	XLRU_PAGE_WRITE_IN_PROGRESS /* page is being written out */
} XlruPageStatus;

/*
 * Shared-memory state
 */
typedef struct XlruSharedData
{
	LWLock	   *ControlLock;

	/* Number of buffers managed by this Xlru structure */
	int			num_slots;

	/*
	 * Arrays holding info for each buffer slot.  Page number is undefined
	 * when status is EMPTY, as is page_lru_count.
	 */
	char	    **page_buffer;
	XlruPageStatus *page_status;
	bool	   *page_dirty;
	int64		   *page_number;
	int		   *page_lru_count;
	LWLockPadded *buffer_locks;

	/*
	 * Optional array of WAL flush LSNs associated with entries in the Xlru
	 * pages.  If not zero/NULL, we must flush WAL before writing pages (false
	 * for multixact).  group_lsn[] has lsn_groups_per_page entries per buffer 
	 * slot, each containing the highest LSN known for a contiguous group of
	 * Xlru entries on that slot's page.
	 */
	XLogRecPtr *group_lsn;
	int			lsn_groups_per_page;

	/*----------
	 * We mark a page "most recently used" by setting
	 *		page_lru_count[slotno] = ++cur_lru_count;
	 * The oldest page is therefore the one with the highest value of
	 *		cur_lru_count - page_lru_count[slotno]
	 * The counts will eventually wrap around, but this calculation still
	 * works as long as no page's age exceeds INT_MAX counts.
	 *----------
	 */
	int			cur_lru_count;

	/*
	 * latest_page_number is the page number of the current end of the log;
	 * this is not critical data, since we use it only to avoid swapping out
	 * the latest page.
	 */
	int64			latest_page_number;

	/* XLRU's index for statistics purposes (might not be unique) */
	int			xlru_stats_idx;
} XlruSharedData;

typedef XlruSharedData *XlruShared;

/*
 * XlruCtlData is an unshared structure that points to the active information
 * in shared memory.
 */
typedef struct XlruCtlData
{
	XlruShared	shared;

	/*
	 * Which sync handler function to use when handing sync requests over to
	 * the checkpointer.  SYNC_HANDLER_NONE to disable fsync (eg pg_notify).
	 */
	SyncRequestHandler sync_handler;


	/*
	 * Dir is set during XLruInit and does not change thereafter. Since
	 * it's always the same, it doesn't need to be in shared memory.
	 */
	char		Dir[64];
} XlruCtlData;

typedef XlruCtlData *XlruCtl;


extern Size XlruShmemSize(int nslots, int nlsns);
extern void XlruInit(XlruCtl ctl, const char *name, int nslots, int nlsns,
						  LWLock *ctllock, const char *subdir, int tranche_id,
						  SyncRequestHandler sync_handler);
extern int	XlruZeroPage(XlruCtl ctl, int64 pageno);
extern int	XlruReadPage(XlruCtl ctl, int64 pageno, bool write_ok,
							  FullTransactionId xid);
extern int	XlruReadPage_ReadOnly(XlruCtl ctl, int64 pageno,
									   FullTransactionId xid);
extern void XlruWritePage(XlruCtl ctl, int slotno);
extern void XlruWriteAll(XlruCtl ctl, bool allow_redirtied);
extern void XlruTruncate(XlruCtl ctl, int64 cutoffPage);
extern bool XlruDoesPhysicalPageExist(XlruCtl ctl, int64 pageno);

typedef bool (*XlruScanCallback) (XlruCtl ctl, char *filename, int64 segpage,
								  void *data);
extern bool XlruScanDirectory(XlruCtl ctl, XlruScanCallback callback, void *data);

extern int	XlruSyncFileTag(XlruCtl ctl, const FileTag *ftag, char *path);


#endif							/* XLRU_H */
