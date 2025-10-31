/* -------------------------------------------------------------------------
 *
 * undotxn.h
 * undo transaction utils.
 *
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 * include/undo/undotxn.h
 * -------------------------------------------------------------------------
 *
 */
#ifndef UNDOTXN_H
#define UNDOTXN_H

#include "c.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "xstore.h"
#include "port/atomics.h"

static inline bool
TransactionIdOlderThanAllUndo(FullTransactionId xid)
{
	/* to slove standby read consistency problem */
	FullTransactionId cutoff;
	if (FullTransactionIdEquals(xid, FullTransactionIdFromEpochAndXid(0, FrozenTransactionId))) 
		return true;

	if (RecoveryInProgress())
	{
		FullTransactionId standby_recycle_xid =
			FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->hot_standby_frozen_xid));

		return FullTransactionIdPrecedes(xid, standby_recycle_xid);
	}
	cutoff = FullTransactionIdFromU64(pg_atomic_read_u64(&undo_sys_ctx->global_frozen_xid));
	return FullTransactionIdPrecedes(xid, cutoff);
}

#endif