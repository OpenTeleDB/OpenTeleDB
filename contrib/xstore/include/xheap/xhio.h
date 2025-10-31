/* -------------------------------------------------------------------------
 *
 * xhio.h
 * the I/O interfaces of xstore inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * include/xheap/xio.h
 * -------------------------------------------------------------------------
 */

#ifndef XHIO_H
#define XHIO_H

#include "postgres.h"

#include "access/hio.h"
#include "access/tableam.h"


#define XHEAP_INSERT_SKIP_WAL 0x0001
#define XHEAP_INSERT_SKIP_FSM TABLE_INSERT_SKIP_FSM
#define XHEAP_INSERT_FROZEN TABLE_INSERT_FROZEN
#define XHEAP_INSERT_EXTEND 0x0020

#define GET_BUF_FOR_XTUPLE_LOOP_LIMIT 2

extern int xstore_max_search_length_for_prune;

extern Buffer relation_get_buffer_for_xtuple(Relation relation, Size len, Buffer other_buffer,
										 int options, BulkInsertState bistate);
#endif
