/* -------------------------------------------------------------------------
 *
 * xheap_am.h
 * 
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 *
 *
 * IDENTIFICATION
 * include/xheap/xheap_am.h
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tableam.h"

extern const TableAmRoutine *get_xheapam_table_am_routine(void);
extern bool relation_is_xstore_format(void *relation);
extern Oid get_xstore_oid(void);
