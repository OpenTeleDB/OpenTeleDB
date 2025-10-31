/* -------------------------------------------------------------------------
 *
 * xtup_details.h
 * POSTGRES xheap tuple header definitions..
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * include/xheap/xtup_details.h
 * -------------------------------------------------------------------------
 *
 */

#ifndef XTUP_DETAILS_H
#define XTUP_DETAILS_H

#include "xheap/xtuple.h"
#include "access/htup.h"
#include "access/htup_details.h"


void xheap_deform_tuple(XHeapTuple xtuple, TupleDesc row_desc, Datum *values, bool *isNulls);
void xheap_deform_tuple_guts(XHeapTuple xtuple, TupleDesc row_desc, Datum *values,
						  bool *isNulls, int unatts);
XHeapTuple heap_to_xheap(Relation rel, HeapTuple heaptuple);
HeapTuple  xheap_to_heap(Relation rel, XHeapTuple xheaptuple);
Datum	   xheap_get_sys_attr(XHeapTuple uhtup, Buffer buf, int attnum, TupleDesc tuple_desc,
						   bool *isnull);

XHeapTuple xheap_copy_tuple(XHeapTuple uhtup);

XHeapTuple xheap_form_tuple_shard(TupleDesc tuple_desc, Datum *values, bool *is_nulls);

#define xheap_form_tuple(tuple_descriptor, values, isnull)                    \
	xheap_form_tuple_shard(tuple_descriptor, values, isnull)

XHeapTuple xheap_modify_tuple(XHeapTuple tuple, TupleDesc tuple_desc, Datum *repl_values,
							const bool *repl_isnull, const bool *do_replace);
void   xheap_tuple_header_advance_latest_removed_xid(XHeapDiskTuple tuple, FullTransactionId xid,
											   FullTransactionId *latest_removed_xid);
bool   xheap_att_is_null(XHeapTuple tup, int attnum, TupleDesc tuple_desc);
Datum  xheap_no_cache_get_attr(XHeapTuple tuple, uint32 attnum, TupleDesc tupleDesc);
uint32 xheap_calc_tuple_data_size(TupleDesc tuple_desc, Datum *values, const bool *is_nulls,
							  uint32 hoff, bool enable_reverse_bitmap, bool enable_reserve);
void   xheap_copy_disk_tuple_no_null(TupleDesc tuple_desc, const bool *destNull,
								XHeapTuple dest_tuple, AttrNumber fillatts,
								const XHeapDiskTupleData *src_tuple);
void   xheap_copy_disk_tuple_with_nulls(TupleDesc tuple_desc, const bool *dest_null,
								   XHeapTuple dest_tuple, AttrNumber fillatts,
								   XHeapDiskTupleData *src_tuple);

void	   xheap_fill_disk_tuple(TupleDesc tuple_desc, Datum *values, const bool *isnull,
							  XHeapDiskTupleData *disk_tuple, uint32 data_size,
							  bool enable_reverse_bitmap, bool enable_reserve, bool hasnull);
void	   check_tuple_validity(Relation rel, XHeapTuple xtuple);
Bitmapset *xheap_tuple_attr_equals(TupleDesc tupdesc, Bitmapset *att_list,
								   XHeapTuple tup1, XHeapTuple tup2);
void	   xheap_copy_tuple_with_buffer(XHeapTuple src_tup, XHeapTuple dest_tup);

#endif	//TELEDB_XTUP_DETAILS_H
