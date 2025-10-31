/* -------------------------------------------------------------------------
 *
 * xtupleslot.h
 * table slot for xheap
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/xheap/xtupleslot.h
 * -------------------------------------------------------------------------
 *
 */

#ifndef XTUPLESLOT_H
#define XTUPLESLOT_H

#include "postgres.h"

#include "executor/tuptable.h"
#include "xheap/xtuple.h"

#define TTS_TABLEAM_IS_HEAP(slot) ((slot)->tts_ops == &TTSOpsBufferHeapTuple || \
									(slot)->tts_ops == &TTSOpsMinimalTuple || \
									(slot)->tts_ops == &TTSOpsHeapTuple)

#define TTS_TABLEAM_IS_XSTORE(slot) ((slot)->tts_ops == &TTSOpsXHeapTuple)

#define TTS_TABLEAM_IS_VIRTUAL(slot) ((slot)->tts_ops == &TTSOpsVirtual)

extern const TupleTableSlotOps TTSOpsXHeapTuple;

typedef struct XHeapTupleTableSlot
{
	TupleTableSlot base;

#define FIELDNO_HEAPTUPLETABLESLOT_TUPLE 1
	XHeapTuple    tuple;            /* physical tuple */
#define FIELDNO_HEAPTUPLETABLESLOT_OFF 2
	uint32        off;            /* saved state for slot_deform_heap_tuple */
	XHeapTupleData tupdata;        /* optional workspace for storing tuple */
	// minimal
	MinimalTuple mintuple;        /* minimal tuple, or NULL if none */
	XHeapTupleData minhdr;        /* workspace for minimal-tuple-only case */
} XHeapTupleTableSlot;

extern XHeapTuple exec_fetch_slot_xheap_tuple(TupleTableSlot *slot,
										  bool *shouldFree);
extern XHeapTuple exec_copy_slot_xheap_tuple(TupleTableSlot *slot);
/*
 * XHeap tuptable
 */
void slot_deform_xtuple(TupleTableSlot *slot, XHeapTuple tuple, uint32 *offp, int natts);
HeapTuple xheap_copy_heap_tuple(TupleTableSlot *slot);
void xheap_slot_store_xheap_tuple(XHeapTuple xtuple, TupleTableSlot *slot, bool should_free, bool batch_mode);

void xheap_slot_clean(TupleTableSlot *slot);

void xheap_slot_get_all_attrs(TupleTableSlot *slot);

bool xheap_slot_att_is_null(const TupleTableSlot *slot, int attnum);

MinimalTuple xheap_slot_copy_minimal_tuple(TupleTableSlot *slot);
void xheap_materialize(TupleTableSlot *slot);

bool table_slot_is_xstore(void *slot);
void* tts_xheap_tuple_addr(void);

#endif /* XTUPLESLOT_H */
