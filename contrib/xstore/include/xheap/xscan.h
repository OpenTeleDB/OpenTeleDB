/* -------------------------------------------------------------------------
 *
 * xscan.h
 * the row format of inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/xheap/xscan.h
 * -------------------------------------------------------------------------
 *
 */
#ifndef XSCAN_H
#define XSCAN_H

#include "access/relscan.h"
#include "access/skey.h"
#include "access/sdir.h"
#include "xheap/xtuple.h"
#include "xheap/xpage.h"
#include "executor/tuptable.h"

typedef struct RangeScanInRedis{
    uint8 isRangeScanInRedis;
    uint8 sliceTotal;
    uint8 sliceIndex;
} RangeScanInRedis;

typedef struct XHeapScanDescData
{
	TableScanDescData rs_base;
	bool			  rs_allow_strat; /* allow or disallow use of access strategy */
	bool			  rs_allow_sync;  /* allow or disallow use of syncscan */

									  /* state set up at initscan time */
	BlockNumber			 rs_nblocks;	/* total number of blocks in rel */
	BlockNumber			 rs_startblock; /* block # to start at */
	BufferAccessStrategy rs_strategy;	/* access strategy for reads */
	bool				 rs_syncscan;	/* report location to syncscan logic? */
	BlockNumber			 rs_numblocks;	/* max number of blocks to scan */
	/* rs_numblocks is usually InvalidBlockNumber, meaning "scan whole rel" */

	bool rs_pageatatime; /* verify visibility page-at-a-time? */

	/* fields used for xstore partial seq scan */
	AttrNumber lastVar /* = -1*/
		; /* last variable (table column) needed for seq scan (non-inclusive), -1 = all */
	bool *boolArr /* = NULL*/
		; /* false elements indicate columns that must be copied during seq scan */

	/* scan current state */
	bool		rs_inited; /* false = scan not init'd yet */
	BlockNumber rs_cblock; /* current block # in scan, if any */
	Buffer		rs_cbuf;   /* current buffer in scan, if any */

	/* these fields only used in page-at-a-time mode and for bitmap scans */
	int rs_cindex;	/* current tuple's index in vistuples */
	int rs_ntuples; /* number of visible tuples on page */

	/* state set up at initscan time */
	TupleTableSlot	*slot;				  /* For begin scan of CopyTo */

	/* variables for batch mode scan */
	int rs_ctup_rows;
	int rs_max_scan_rows;

	TupleDesc rs_tupdesc; /* heap tuple descriptor for rs_ctup */

	/* these fields only used in page-at-a-time mode and for bitmap scans */
	// XXXTAM
	ItemPointerData rs_mctid; /* marked scan position, if any */

	/* these fields only used in page-at-a-time mode and for bitmap scans */
	int rs_mindex; /* marked tuple's saved index */

	XHeapTuple	rs_visxtuples[MaxPossibleXHeapTuplesPerPage]; /* visible tuples */
	XHeapTuple	rs_cutup;	  /* current tuple in scan, if any */
	XHeapTuple *rs_ctupBatch; /* current tuples in scan */
	/*
	 * For parallel scans to store page allocation data.  NULL when not
	 * performing a parallel scan.
	 */
	ParallelBlockTableScanWorkerData *rs_parallelworkerdata;
} XHeapScanDescData;

typedef struct XHeapScanDescData *XHeapScanDesc;

XHeapTuple	  xheapgettuple_pagemode(XHeapScanDesc scan, ScanDirection dir,
									bool *has_cur_xact_write /* = NULL*/);

TableScanDesc xheap_beginscan(Relation relation, Snapshot snapshot,
							 ParallelTableScanDesc parallel_scan, uint32 flags);

void	   xheap_end_scan(TableScanDesc xscan);
void	   xheap_rescan(TableScanDesc xscan, ScanKey key);
XHeapTuple xheap_index_build_get_next_tuple(XHeapScanDesc scan, TupleTableSlot *slot);
XHeapTuple xheap_search_buffer(ItemPointer tid, Relation relation, Buffer buffer,
							 Snapshot snapshot, bool *all_dead,
							 XHeapTuple freebuf /* = NULL*/,
							 bool	   *has_cur_xact_write /* = NULL*/);
bool	   xheapgetpage(XHeapScanDesc sscan, BlockNumber page,
						bool *has_cur_xact_write /* = NULL*/);

XHeapTuple	xheap_getnext(TableScanDesc sscan, ScanDirection dir,
						 bool *has_cur_xact_write /* = NULL*/);
extern bool xheap_get_tuple_page_batch_mode(XHeapScanDesc scan, ScanDirection dir);
#endif
