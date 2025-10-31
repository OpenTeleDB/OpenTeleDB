/* -------------------------------------------------------------------------
 *
 * xtupleslot.c
 * table slot for xheap.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xtupleslot.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "postgres_ext.h"
#include "executor/tuptable.h"
#include "storage/buf.h"
#include "utils/palloc.h"
#include "xheap/xtupleslot.h"
#include "xheap/xtup_details.h"
#include "xheap/xtuple.h"

static void tts_xheap_init(TupleTableSlot *slot);
static void tts_xheap_release(TupleTableSlot *slot);
static void tts_xheap_clear(TupleTableSlot *slot);
static void tts_xheap_getsomeattrs(TupleTableSlot *slot, int natts);
static Datum tts_xheap_getsysattr(TupleTableSlot *slot, int attnum,
								  bool *isnull);
static void tts_xheap_materialize(TupleTableSlot *slot);
static void tts_xheap_copyslot(TupleTableSlot *dstslot,
							   TupleTableSlot *srcslot);
static HeapTuple tts_xheap_copy_heap_tuple(TupleTableSlot *slot);
static MinimalTuple tts_xheap_copy_minimal_tuple(TupleTableSlot *slot);
static bool tts_xheap_is_current_xact_tuple(TupleTableSlot *slot);

const TupleTableSlotOps TTSOpsXHeapTuple = {
	.base_slot_size = sizeof(XHeapTupleTableSlot),
	.init = tts_xheap_init,
	.release = tts_xheap_release,
	.clear = tts_xheap_clear,
	.getsomeattrs = tts_xheap_getsomeattrs,
	.getsysattr = tts_xheap_getsysattr,
	.materialize = tts_xheap_materialize,
	.copyslot = tts_xheap_copyslot,
	.get_heap_tuple = NULL,		/* cannot directly get heap tuple from xheap slot, always use copy */

	/* A heap tuple table slot can not "own" a minimal tuple. */
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_xheap_copy_heap_tuple,
	.copy_minimal_tuple = tts_xheap_copy_minimal_tuple,
	.is_current_xact_tuple = tts_xheap_is_current_xact_tuple
};

/*
 * TupleTableSlotOps implementation for XHeapTupleTableSlot.
 */
static void
tts_xheap_init(TupleTableSlot *slot)
{
}

static void
tts_xheap_release(TupleTableSlot *slot)
{
}

static void
tts_xheap_clear(TupleTableSlot *slot)
{
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(TTS_TABLEAM_IS_XSTORE(slot));

	/* Free the memory for the heap tuple if it's allowed. */
	if (TTS_SHOULDFREE(slot))
	{
		XHeapFreeTuple(xslot->tuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	xslot->off = 0;
	xslot->tuple = NULL;
}

static void
tts_xheap_getsomeattrs(TupleTableSlot *slot, int natts)
{
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));
	Assert(TTS_TABLEAM_IS_XSTORE(slot));

	/* Quick out if we have 'em all already */
	if (slot->tts_nvalid >= natts)
		return;

	slot_deform_xtuple(slot, xslot->tuple, &xslot->off, natts);
}

static Datum
tts_xheap_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	/*
	 * In some code paths it's possible to get here with a non-materialized
	 * slot, in which case we can't retrieve system columns.
	 */
	if (!xslot->tuple)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot retrieve a system column in this context")));

	return xheap_get_sys_attr(xslot->tuple, InvalidBuffer, attnum, slot->tts_tupleDescriptor, isnull);
}

static void
tts_xheap_materialize(TupleTableSlot *slot)
{
	xheap_materialize(slot);
}

static void
tts_xheap_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	XHeapTuple	xtuple;
	MemoryContext oldcontext;

	/* Get XHeapTuple from srcslot */
	oldcontext = MemoryContextSwitchTo(dstslot->tts_mcxt);
	xtuple = exec_copy_slot_xheap_tuple(srcslot);
	MemoryContextSwitchTo(oldcontext);

	/* Store XHeapTuple to dstslot */
	xheap_slot_store_xheap_tuple(xtuple, dstslot, true, false);
}

static HeapTuple
tts_xheap_copy_heap_tuple(TupleTableSlot *slot)
{
	return xheap_copy_heap_tuple(slot);
}

static MinimalTuple
tts_xheap_copy_minimal_tuple(TupleTableSlot *slot)
{
	return xheap_slot_copy_minimal_tuple(slot);
}

static bool
tts_xheap_is_current_xact_tuple(TupleTableSlot *slot)
{
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;
	TransactionId xmin;

	Assert(!TTS_EMPTY(slot));

	/*
	 * In some code paths it's possible to get here with a non-materialized
	 * slot, in which case we can't check if tuple is created by the current
	 * transaction.
	 */
	if (!xslot->tuple)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("don't have a storage tuple in this context")));

	xmin = xslot->tuple->xmin;

	return TransactionIdIsCurrentTransactionId(xmin);
}

/*
 * ExecFetchSlotXHeapTuple - fetch xheaptuple from tableslot.
 *
 * Form a xheaptuple if this is not a xheap tableslot, and set shouldFree to true.
 * Return xheaptuple directly if this is a xheap tableslot, set shouldFree to false.
 */
XHeapTuple
exec_fetch_slot_xheap_tuple(TupleTableSlot *slot, bool *should_free)
{
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

	*should_free = false;

	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(!TTS_EMPTY(slot));

	if (!TTS_TABLEAM_IS_XSTORE(slot))
	{
		/* Slot Maybe BufferHeapTuple for test case:
		 * CREATE TABLE as_select1_x using xstore AS SELECT * FROM pg_class WHERE relkind = 'r' and relpartbound is NULL;
		 * which select from a heap table, and then insert into xstore table.
		 */

		/* Assert(TTS_TABLEAM_IS_VIRTUAL(slot)); */
		*should_free = true;
		slot_getallattrs(slot);
		return xheap_form_tuple(slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);
	}

	if (!xslot->tuple)
		slot->tts_ops->materialize(slot);

	return xslot->tuple;
}

/*
 * ExecCopySlotXHeapTuple - copy xheaptuple from tableslot.
 */
XHeapTuple
exec_copy_slot_xheap_tuple(TupleTableSlot *slot)
{
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(!TTS_EMPTY(slot));

	if (TTS_TABLEAM_IS_VIRTUAL(slot))
		return xheap_form_tuple(slot->tts_tupleDescriptor,
							   slot->tts_values,
							   slot->tts_isnull);
	else if (TTS_TABLEAM_IS_HEAP(slot))
	{
		slot_getallattrs(slot);
		return xheap_form_tuple(slot->tts_tupleDescriptor,
							   slot->tts_values,
							   slot->tts_isnull);
	}
	else
	{
		XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

		Assert(TTS_TABLEAM_IS_XSTORE(slot));
		if (!xslot->tuple)
			tts_xheap_materialize(slot);

		return xheap_copy_tuple(xslot->tuple);
	}

	Assert(0);
}

void
slot_deform_xtuple(TupleTableSlot *slot, XHeapTuple tuple, uint32 *offp, int natts)
{
	uint32		off;
	int			attnum;
	TupleDesc	tuple_desc = slot->tts_tupleDescriptor;
	Datum	   *values = slot->tts_values;
	bool	   *is_nulls = slot->tts_isnull;
	XHeapDiskTuple tup = tuple->disk_tuple;
	FormData_pg_attribute *att = tuple_desc->attrs;
	bool		hasnulls = XHeapDiskTupHasNulls(tup);
	char	   *tp = (char *) tup;
	bits8	   *bp = tup->data;		/* ptr to null bitmap in tuple */
	int			nullcount = 0;
	int			tuple_attrs = XHeapTupleHeaderGetNatts(tuple->disk_tuple);
	bool		enable_reverse_bitmap = NAttrsReserveSpace(tuple_attrs);

	/* We can only fetch as many attributes as the tuple has. */
	int			natts_available = Min(tuple_attrs, natts);

	Assert(*offp >= 0);

	/*
	 * Check whether the first call for this tuple, and initialize or restore
	 * loop state.
	 */
	off = tup->t_hoff;
	attnum = slot->tts_nvalid;
	/* if we're not starting from the first attribute */
	if (attnum != 0)
	{
		for (int i = 0; i < attnum; i++)
			if (is_nulls[i])
				nullcount++;

		off = *offp;			/* Restore state from previous execution */
	}

	for (; attnum < natts_available; attnum++)
	{
		Form_pg_attribute thisatt = &(att[attnum]);

		if (hasnulls && att_isnull(attnum, bp))
		{
			/* Skip attribute length in case the tuple was stored with
			 * space reserved for null attributes */
			if (enable_reverse_bitmap)
				if (!att_isnull(tuple_attrs + nullcount, bp))
					off += thisatt->attlen;

			nullcount++;

			values[attnum] = (Datum) 0;
			is_nulls[attnum] = true;
			continue;
		}

		is_nulls[attnum] = false;

		if (thisatt->attlen == LEN_VARLENA)
			off = att_align_pointer(off, thisatt->attalign, -1, tp + off);
		else if (!thisatt->attbyval)
			/* not varlena, so safe to use att_align_nominal */
			off = att_align_nominal(off, thisatt->attalign);

		values[attnum] = fetchatt(thisatt, tp + off);
		off = att_addlength_pointer(off, thisatt->attlen, tp + off);
	}

	for (; attnum < natts; attnum++)
		values[attnum] = getmissingattr(tuple_desc, attnum + 1, &is_nulls[attnum]);

	slot->tts_nvalid = attnum;
	*offp = off;
}

/*
 * XHeapCopyHeapTuple
 * Return a heap tuple constructed from the contents of the slot.
 */
HeapTuple
xheap_copy_heap_tuple(TupleTableSlot *slot)
{
	HeapTuple	tuple;
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));
	Assert(TTS_TABLEAM_IS_XSTORE(slot));

	xheap_slot_get_all_attrs(slot);

	tuple = heap_form_tuple(slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);

	/* Some code like analyze, needs
	 * the t_self item pointer in the
	 * tuple for sorting purposes,
	 * so we set it here */
	if (xslot->tuple != NULL)
	{
		tuple->t_self = ((XHeapTuple) xslot->tuple)->ctid;
		tuple->t_tableOid = ((XHeapTuple) xslot->tuple)->table_oid;
	}

	return tuple;
}

/*
 * Clears the contents of the table slot that contains xheap table tuple data.
 */
void
xheap_slot_clean(TupleTableSlot *slot)
{
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

	/*
	 * sanity checks
	 */
	Assert(slot != NULL);
	Assert(TTS_TABLEAM_IS_XSTORE(slot));

	/*
	 * Free any old physical tuple belonging to the slot.
	 */
	if (TTS_SHOULDFREE(slot))
	{
		XHeapFreeTuple(xslot->tuple);
		XHeapFreeTuple(xslot->mintuple);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}
}

/*
 * xheap_slot_att_is_null
 * Detect whether an attribute of the slot is null, without
 * actually fetching it.
 *
 * @param slot: Tabletuple slot
 * @para attnum: attribute index that should be checked for null value.
 */
bool
xheap_slot_att_is_null(const TupleTableSlot *slot, int attnum)
{
	TupleDesc	tuple_desc = slot->tts_tupleDescriptor;
	XHeapTuple	xhtup;
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

	Assert(TTS_TABLEAM_IS_XSTORE(slot));

	xhtup = xslot->tuple;

	/*
	 * system attributes are handled by heap_attisnull
	 */

	if (attnum <= 0)
	{
		/* internal error */
		if (xhtup == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot extract system attribute from virtual tuple")));

		return xheap_att_is_null(xhtup, attnum, tuple_desc);
	}

	/*
	 * fast path if desired attribute already cached
	 */
	if (attnum <= slot->tts_nvalid)
		return slot->tts_isnull[attnum - 1];

	/*
	 * return NULL if attnum is out of range according to the tupdesc
	 */
	if (attnum > tuple_desc->natts)
		return true;

	/* internal error */
	if (xhtup == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot extract attribute from empty tuple slot")));


	return xheap_att_is_null(xhtup, attnum, tuple_desc);
}

/*
 * xheap_slot_get_all_attrs
 * This function forces all the entries of the slot's Datum/isnull
 * arrays to be valid.  The caller may then extract data directly
 * from those arrays instead of using heap_slot_getattr.
 *
 */
void
xheap_slot_get_all_attrs(TupleTableSlot *slot)
{
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

	Assert(TTS_TABLEAM_IS_XSTORE(slot));

	/* Quick out if we have 'em all already */
	if (slot->tts_nvalid == slot->tts_tupleDescriptor->natts)
		return;

	slot_deform_xtuple(slot, (XHeapTuple) xslot->tuple, &xslot->off, slot->tts_tupleDescriptor->natts);
}


MinimalTuple
xheap_slot_copy_minimal_tuple(TupleTableSlot *slot)
{
	/*
	 * sanity checks.
	 */
	Assert(slot != NULL);
	Assert(!TTS_EMPTY(slot));
	Assert(slot->tts_tupleDescriptor != NULL);
	Assert(TTS_TABLEAM_IS_XSTORE(slot));

	xheap_slot_get_all_attrs(slot);

	return heap_form_minimal_tuple(slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);
}

/*
 * Stores XHeapTuple in the slot.
 *
 * @param xtuple: xheap to be stored.
 * @param slot: slot inwhich tuple needs to be stored.
 * @param should_free: whether slot assumes responsibility of freeing up the tuple.
 */
void
xheap_slot_store_xheap_tuple(XHeapTuple xtuple, TupleTableSlot *slot, bool should_free, bool batch_mode)
{
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;

	/*
	 * sanity checks
	 */
	Assert(xtuple != NULL);
	Assert(slot != NULL && TTS_TABLEAM_IS_XSTORE(slot));
	Assert(slot->tts_tupleDescriptor != NULL);

	xheap_slot_clean(slot);

	/*
	 * Store the new tuple into the specified slot.
	 */
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	if (should_free)
		slot->tts_flags |= TTS_FLAG_SHOULDFREE;
	else
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;

	/* slot->tts_flags &= ~TTS_FLAG_SHOULDFREEMIN; */
	xslot->tuple = xtuple;
	xslot->mintuple = NULL;
	slot->tts_tid = xtuple->ctid;
	slot->tts_tableOid = xtuple->table_oid;

	/* Mark extracted state invalid */
	slot->tts_nvalid = 0;
}

/*
 * Make the contents of the xheap table's slot contents solely depend on the slot(make them a local copy),
 * and not on underlying external resources like another memory context, buffers etc.
 *
 * @pram slot: slot to be materialized.
 */
void
xheap_materialize(TupleTableSlot *slot)
{
	XHeapTupleTableSlot *xslot = (XHeapTupleTableSlot *) slot;
	MemoryContext old_context;

	Assert(!TTS_EMPTY(slot));
	Assert(TTS_TABLEAM_IS_XSTORE(slot));
	Assert(slot->tts_tupleDescriptor != NULL);

	if (xslot->tuple && TTS_SHOULDFREE(slot))
		return;

	old_context = MemoryContextSwitchTo(slot->tts_mcxt);
	if (xslot->tuple != NULL)
		xslot->tuple = xheap_copy_tuple((XHeapTuple) xslot->tuple);
	else
		xslot->tuple = xheap_form_tuple(slot->tts_tupleDescriptor,
										slot->tts_values,
										slot->tts_isnull);

	slot->tts_flags |= TTS_FLAG_SHOULDFREE;
	MemoryContextSwitchTo(old_context);

	slot->tts_nvalid = 0;
}

bool
table_slot_is_xstore(void *slot)
{
	return ((TupleTableSlot *) slot)->tts_ops == &TTSOpsXHeapTuple;
}

void *
tts_xheap_tuple_addr(void)
{
	return (void *) &TTSOpsXHeapTuple;
}