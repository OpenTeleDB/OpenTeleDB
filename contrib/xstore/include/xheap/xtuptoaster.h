/* -------------------------------------------------------------------------
 *
 * xtuptoaster.h
 * the access interfaces of xstore tuple toaster.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * include/xheap/xtuptoaster.h
 * -------------------------------------------------------------------------
 */

#ifndef XTUPTOASTER_H
#define XTUPTOASTER_H

#include "xheap/xtuple.h"

#define EXTERN_XHEAP_TUPLES_PER_PAGE (4)
#define XTOAST_TUPLES_PER_PAGE (4)

/*
 * Find the maximum size of a tuple if there are to be N xtuples per page.
 */
#define XHeapMaximumBytesPerTuple(tuplesPerPage)                                         \
	MAXALIGN_DOWN(                                                                       \
		(BLCKSZ - MAXALIGN(SizeOfXHeapPageHeaderData +                                   \
						   XHEAP_SPECIAL_SIZE + (tuplesPerPage) * sizeof(ItemIdData))) / \
		(tuplesPerPage))

#define XTOAST_TUPLE_THRESHOLD XHeapMaximumBytesPerTuple(XTOAST_TUPLES_PER_PAGE)

#define XTOAST_TUPLE_TARGET XTOAST_TUPLE_THRESHOLD

#define EXTERN_XHEAP_TUPLE_MAX_SIZE \
	XHeapMaximumBytesPerTuple(EXTERN_XHEAP_TUPLES_PER_PAGE)

#define SizeofXHeapTupleHeader offsetof(XHeapDiskTupleData, data)

#define XTOAST_MAX_CHUNK_SIZE    \
	(EXTERN_XHEAP_TUPLE_MAX_SIZE - \
	 MAXALIGN(SizeofXHeapTupleHeader) - \
	 sizeof(Oid) - \
	 sizeof(int32) - \
	 VARHDRSZ)

#define CHUNK_ID_ATTR 2
#define CHUNK_DATA_ATTR 3

#define ATTR_FIRST 1
#define ATTR_SECOND 2
#define ATTR_THIRD 3

void	   xheap_toast_delete(Relation relation, XHeapTuple xtuple);
XHeapTuple xheap_toast_insert_or_update(Relation relation, XHeapTuple newtup,
									XHeapTuple oldtup, int options);
void	   xheap_fetch_toast_slice(Relation toastrel, Oid valueid, int32 attrsize,
								   int32 sliceoffset, int32 slicelength,
								   struct varlena *result);

#endif
