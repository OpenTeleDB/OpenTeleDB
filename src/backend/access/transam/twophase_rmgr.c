/*-------------------------------------------------------------------------
 *
 * twophase_rmgr.c
 *	  Two-phase-commit resource managers tables
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/twophase_rmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/twophase_rmgr.h"
#ifdef USE_XSTORE
#include "access/xstore/xstorehook.h"
#endif
#include "pgstat.h"
#include "storage/lock.h"
#include "storage/predicate.h"

#ifdef USE_XSTORE
static void xmultixact_twophase_recover_hook(TransactionId xid, uint16 info, void *recdata, uint32 len);
static void xmultixact_twophase_postcommit_hook(TransactionId xid, uint16 info, void *recdata, uint32 len);
static void xmultixact_twophase_postabort_hook(TransactionId xid, uint16 info, void *recdata, uint32 len);
#endif

const TwoPhaseCallback twophase_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_recover,		/* Lock */
	NULL,						/* pgstat */
	multixact_twophase_recover, /* MultiXact */
#ifndef USE_XSTORE
	predicatelock_twophase_recover	/* PredicateLock */
#else
	predicatelock_twophase_recover,	/* PredicateLock */
	xmultixact_twophase_recover_hook
#endif
};

const TwoPhaseCallback twophase_postcommit_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postcommit,	/* Lock */
	pgstat_twophase_postcommit, /* pgstat */
	multixact_twophase_postcommit,	/* MultiXact */
#ifndef USE_XSTORE
	NULL						/* PredicateLock */
#else
	NULL,
	xmultixact_twophase_postcommit_hook
#endif
};

const TwoPhaseCallback twophase_postabort_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_postabort,	/* Lock */
	pgstat_twophase_postabort,	/* pgstat */
	multixact_twophase_postabort,	/* MultiXact */
#ifndef USE_XSTORE
	NULL						/* PredicateLock */
#else
	NULL,
	xmultixact_twophase_postabort_hook
#endif
};

const TwoPhaseCallback twophase_standby_recover_callbacks[TWOPHASE_RM_MAX_ID + 1] =
{
	NULL,						/* END ID */
	lock_twophase_standby_recover,	/* Lock */
	NULL,						/* pgstat */
	NULL,						/* MultiXact */
#ifndef USE_XSTORE
	NULL						/* PredicateLock */
#else
	NULL,						/* PredicateLock */
	NULL
#endif
};

#ifdef USE_XSTORE

/*
 * xmultixact twophase hooks, if will be called it xstore plugin registered the callback
 * We use thoes hooks because callback arraies for twophase rmgrs are const and can't be
 * registered when running. 
 */

/*
 * xmultixact_twophase_recover hook, if will be called it xstore plugin registered the callback
 * Recover the state of a prepared transaction at startup
 */
static void
xmultixact_twophase_recover_hook(TransactionId xid, uint16 info,
						   void *recdata, uint32 len)
{
	if (GlobalXStoreHook.xmultiHook && GlobalXStoreHook.xmultiHook->xmultixact_twophase_recover)
		return GlobalXStoreHook.xmultiHook->xmultixact_twophase_recover(xid, info, recdata, len);
}

/*
 * xmultixact_twophase_postcommit
 *		Similar to AtEOXact_XMultiXact but for COMMIT PREPARED
 */
static void
xmultixact_twophase_postcommit_hook(TransactionId xid, uint16 info,
						   void *recdata, uint32 len)
{
	if (GlobalXStoreHook.xmultiHook && GlobalXStoreHook.xmultiHook->xmultixact_twophase_postcommit)
		return GlobalXStoreHook.xmultiHook->xmultixact_twophase_postcommit(xid, info, recdata, len);
}

/*
 * xmultixact_twophase_postabort
 *		This is actually just the same as the COMMIT case.
 */
static void
xmultixact_twophase_postabort_hook(TransactionId xid, uint16 info,
							 void *recdata, uint32 len)
{
	if (GlobalXStoreHook.xmultiHook && GlobalXStoreHook.xmultiHook->xmultixact_twophase_postabort)
		return GlobalXStoreHook.xmultiHook->xmultixact_twophase_postabort(xid, info, recdata, len);
}

#endif
