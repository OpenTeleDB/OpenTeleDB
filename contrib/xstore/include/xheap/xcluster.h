/* -------------------------------------------------------------------------
 *
 * xcluster.h
 *	  the cluster functions for xheap for xstore.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  include/xheap/xcluster.h
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/rewriteheap.h"
#include "storage/bulk_write.h"
#include "utils/hsearch.h"
#include "xheap/xtuple.h"

/*
 * a copy of RewriteStateData typedef in rewriteheap.c
 */
typedef struct RewriteStateData
{
	Relation		rs_old_rel;		/* source heap */
	Relation		rs_new_rel;		/* destination heap */
	BulkWriteState *rs_bulkstate;	/* writer for the destination */
	BulkWriteBuffer rs_buffer;		/* page currently being built */
	BlockNumber		rs_blockno;		/* block where page will go */
	bool			rs_logical_rewrite; /* do we need to do logical rewriting */
	TransactionId	rs_oldest_xmin; /* oldest xmin used by caller to determine
									 * tuple visibility */
	TransactionId	rs_freeze_xid;	/* Xid that will be used as freeze cutoff
									 * point */
	TransactionId	rs_logical_xmin; /* Xid that will be used as cutoff point
									 * for logical rewrites */
	MultiXactId		rs_cutoff_multi; /* MultiXactId that will be used as cutoff
									 * point for multixacts */
	MemoryContext	rs_cxt;			/* for hash tables and entries and tuples in
									 * them */
	XLogRecPtr		rs_begin_lsn;	/* XLogInsertLsn when starting the rewrite */
	HTAB		   *rs_unresolved_tups; /* unmatched A tuples */
	HTAB		   *rs_old_new_tid_map; /* unmatched B tuples */
	HTAB		   *rs_logical_mappings; /* logical remapping files */
	uint32			rs_num_rewrite_mappings; /* # in memory mappings */
} RewriteStateData;

extern void rewrite_xheap_tuple(RewriteState state, XHeapTuple oldTuple, XHeapTuple newTuple);