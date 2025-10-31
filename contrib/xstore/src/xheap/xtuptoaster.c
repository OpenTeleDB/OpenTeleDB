/* -------------------------------------------------------------------------
 *
 * xtuptoaster.c
 * the tuple toaster for xstore.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xtuptoaster.c
 * -------------------------------------------------------------------------
 */

#include "c.h"
#include "postgres.h"

#include "access/heaptoast.h"
#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/toast_internals.h"
#include "access/transam.h"
#include "xheap/xtuptoaster.h"
#include "xheap/xheap.h"
#include "xheap/xheap_am.h"
#include "xheap/xtuple.h"
#include "xheap/xtup_details.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

static void	 xheap_toast_delete_datum(Relation rel, Datum value, int options);
static Datum xheap_toast_save_datum(Relation rel, Datum value, struct varlena *oldexternal,
								 int options);
static Datum xheap_toast_compress_datum(Datum value, char cmethod);

static Datum
xheap_toast_compress_datum(Datum value, char cmethod)
{
	return toast_compress_datum(value, cmethod);
}

void
xheap_toast_delete(Relation relation, XHeapTuple xtuple)
{
	TupleDesc tuple_desc;
	int		  num_attrs;
	int		  i;
	Datum	  toast_values[MaxHeapAttributeNumber] = {0};
	bool	  toast_is_null[MaxHeapAttributeNumber] = {0};

	Assert(relation->rd_rel->relkind == RELKIND_RELATION);

	tuple_desc = relation->rd_att;
	num_attrs = tuple_desc->natts;

	Assert(num_attrs <= MaxHeapAttributeNumber);
	xheap_deform_tuple(xtuple, tuple_desc, toast_values, toast_is_null);

	/*
     * Check for external stored attributes and delete them from the secondary
     * relation.
     */
	for (i = 0; i < num_attrs; i++)
	{
		if (((TupleDescAttr(tuple_desc, i)))->attlen == -1)
		{
			Datum value = toast_values[i];

			if (toast_is_null[i])
				continue;
			else if (VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(value)))
				xheap_toast_delete_datum(relation, value, 0);
		}
	}
}

XHeapTuple
xheap_toast_insert_or_update(Relation relation, XHeapTuple newtup, XHeapTuple oldtup,
						 int options)
{
	XHeapTuple res_tup;
	TupleDesc  tuple_desc;
	int		   num_attrs;
	int		   i;

	bool need_change = false;
	bool need_free = false;
	bool need_del_old = false;
	bool has_nulls = false;

	Size max_data_len;
	Size hoff;

	char			toast_action[MaxHeapAttributeNumber] = {0};
	bool			toast_is_null[MaxHeapAttributeNumber] = {0};
	bool			toast_old_is_null[MaxHeapAttributeNumber] = {0};
	Datum			toast_values[MaxHeapAttributeNumber] = {0};
	Datum			toast_old_values[MaxHeapAttributeNumber] = {0};
	struct varlena *toast_old_external[MaxHeapAttributeNumber] = {0};
	uint32			toast_sizes[MaxHeapAttributeNumber] = {0};
	bool			toast_free[MaxHeapAttributeNumber] = {0};
	bool			toast_del_old[MaxHeapAttributeNumber] = {0};

	bool enable_reserve = enable_reserve_space_for_null_atts;
	bool enable_reverse_bitmap;

	/*
     * We should only ever be called for tuples of plain relations or
     * materialized views --- recursing on a toast rel is bad news.
     */
	Assert(relation->rd_rel->relkind == RELKIND_RELATION);

	/*
     * Get the tuple descriptor and break down the tuple(s) into fields.
     */
	tuple_desc = relation->rd_att;
	num_attrs = tuple_desc->natts;

	enable_reverse_bitmap = NAttrsReserveSpace(num_attrs);
	enable_reserve = enable_reserve && enable_reverse_bitmap;
	Assert(!enable_reserve || (enable_reserve && enable_reverse_bitmap));
	Assert(enable_reverse_bitmap ||
		   (enable_reverse_bitmap == false && enable_reserve == false));
	Assert(num_attrs <= MaxHeapAttributeNumber);

	xheap_deform_tuple(newtup, tuple_desc, toast_values, toast_is_null);

	if (oldtup != NULL)
		xheap_deform_tuple(oldtup, tuple_desc, toast_old_values, toast_old_is_null);

	memset(toast_action, ' ', num_attrs * sizeof(char));
	memset(toast_old_external, 0, num_attrs * sizeof(struct varlena *));
	memset(toast_free, 0, num_attrs * sizeof(bool));
	memset(toast_del_old, 0, num_attrs * sizeof(bool));

	for (i = 0; i < num_attrs; i++)
	{
		Form_pg_attribute att = (TupleDescAttr(tuple_desc, i));
		struct varlena	 *oldValue = NULL;
		struct varlena	 *newValue = NULL;

		if (oldtup != NULL)
		{
			/*
             * For UPDATE get the old and new values of this attribute
             */
			oldValue = (struct varlena *) DatumGetPointer(toast_old_values[i]);
			newValue = (struct varlena *) DatumGetPointer(toast_values[i]);

			/*
             * If the old value is stored on disk, check if it has changed so
             * we have to delete it later.
             */
			if (att->attlen == -1 && !toast_old_is_null[i] &&
				VARATT_IS_EXTERNAL_ONDISK(oldValue))
			{
				if (toast_is_null[i] || !VARATT_IS_EXTERNAL_ONDISK(newValue) ||
					RelationIsLogicallyLogged(relation) ||
					memcmp((char *) oldValue, (char *) newValue,
						   VARSIZE_EXTERNAL(oldValue)) != 0)
				{
					/*
                     * The old external stored value isn't needed any more
                     * after the update
                     */
					toast_del_old[i] = true;
					need_del_old = true;
				}
				else
				{
					/*
                     * This attribute isn't changed by this update so we reuse
                     * the original reference to the old value in the new
                     * tuple.
                     */
					toast_action[i] = 'p';
					continue;
				}
			}
		}
		else
		{
			/*
             * For INSERT simply get the new value
             */
			newValue = (struct varlena *) DatumGetPointer(toast_values[i]);
		}

		if (toast_is_null[i])
		{
			toast_action[i] = 'p';
			has_nulls = true;
			continue;
		}

		/*
         * Now look at varlena attributes
         */
		if (att->attlen == -1)
		{
			/*
             * If the table's attribute says PLAIN always, force it so.
             */
			if (att->attstorage == 'p')
				toast_action[i] = 'p';

			/*
             * We took care of UPDATE above, so any external value we find
             * still in the tuple must be someone else's that we cannot reuse
             * (this includes the case of an out-of-line in-memory datum).
             * Fetch it back (without decompression, unless we are forcing
             * PLAIN storage).  If necessary, we'll push it out as a new
             * external value below.
             */
			if (VARATT_IS_EXTERNAL(newValue))
			{
				toast_old_external[i] = newValue;
				if (att->attstorage == 'p')
					newValue = detoast_attr(newValue);
				else
					newValue = detoast_external_attr(newValue);
				toast_values[i] = PointerGetDatum(newValue);
				toast_free[i] = true;
				need_change = true;
				need_free = true;
			}

			/*
             * Remember the size of this attribute
             */
			toast_sizes[i] = VARSIZE_ANY(newValue);
		}
		else
		{
			/*
             * Not a varlena attribute, plain storage always
             */
			toast_action[i] = 'p';
		}
	}

	/* ----------
     * Compress and/or save external until data fits into target length
     *
     * 1: Inline compress attributes with attstorage 'x', and store very
     * large attributes with attstorage 'x' or 'e' external immediately
     * 2: Store attributes with attstorage 'x' or 'e' external
     * 3: Inline compress attributes with attstorage 'm'
     * 4: Store attributes with attstorage 'm' external
     * ----------
     */

	/* compute header overhead --- this should match heap_form_tuple() */
	hoff = SizeOfXHeapDiskTupleData;

	if (has_nulls)
	{
		int nullcount = 0;
		for (i = 0; i < num_attrs; i++)
		{
			if (toast_is_null[i])
				nullcount++;
		}
		if (enable_reverse_bitmap)
			hoff += BITMAPLEN(num_attrs + nullcount);
		else
			hoff += BITMAPLEN(num_attrs);
	}

	/* now convert to a limit on the tuple data size */
	max_data_len = XTOAST_TUPLE_TARGET - hoff;

	/*
     * Look for attributes with attstorage 'x' to compress.  Also find large
     * attributes with attstorage 'x' or 'e', and store them external.
     */
	while (xheap_calc_tuple_data_size(tuple_desc, toast_values, toast_is_null, hoff,
								  enable_reverse_bitmap, enable_reserve) > max_data_len)
	{
		int	   biggest_attno = -1;
		uint32 biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum  old_value;
		Datum  new_value;


		for (i = 0; i < num_attrs; i++)
		{
			Form_pg_attribute att = (TupleDescAttr(tuple_desc, i));

			if (toast_action[i] != ' ')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue; /* can't happen, toast_action would be PLAIN */
			if (VARATT_IS_COMPRESSED(DatumGetPointer(toast_values[i])))
				continue;
			if (att->attstorage != 'x' && att->attstorage != 'e')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
         * Attempt to compress it inline, if it has attstorage 'x'
         */
		i = biggest_attno;
		if (((TupleDescAttr(tuple_desc, i)))->attstorage == 'x')
		{
			old_value = toast_values[i];
			new_value = xheap_toast_compress_datum(old_value, (TupleDescAttr(tuple_desc, i))->attcompression);
			if (DatumGetPointer(new_value) != NULL)
			{
				/* successful compression */
				if (toast_free[i])
					pfree(DatumGetPointer(old_value));
				toast_values[i] = new_value;
				toast_free[i] = true;
				toast_sizes[i] = VARSIZE(DatumGetPointer(toast_values[i]));
				need_change = true;
				need_free = true;
			}
			else
			{
				/* incompressible, ignore on subsequent compression passes */
				toast_action[i] = 'x';
			}
		}
		else
		{
			/* has attstorage 'e', ignore on subsequent compression passes */
			toast_action[i] = 'x';
		}

		/*
         * If this value is by itself more than maxDataLen (after compression
         * if any), push it out to the toast table immediately, if possible.
         * This avoids uselessly compressing other fields in the common case
         * where we have one long field and several short ones.
         *
         * XXX maybe the threshold should be less than maxDataLen?
         */
		if (toast_sizes[i] > max_data_len && relation->rd_rel->reltoastrelid != InvalidOid)
		{
			old_value = toast_values[i];
			toast_action[i] = 'p';
			toast_values[i] = xheap_toast_save_datum(relation, toast_values[i],
												 toast_old_external[i], options);
			if (toast_free[i])
				pfree(DatumGetPointer(old_value));
			toast_free[i] = true;
			need_change = true;
			need_free = true;
		}
	}

	/*
     * Second we look for attributes of attstorage 'x' or 'e' that are still
     * inline.  But skip this if there's no toast table to push them to.
     */
	while (xheap_calc_tuple_data_size(tuple_desc, toast_values, toast_is_null, hoff,
								  enable_reverse_bitmap, enable_reserve) > max_data_len &&
		   relation->rd_rel->reltoastrelid != InvalidOid)
	{
		int	   biggest_attno = -1;
		uint32 biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum  old_value;

		/* ------
         * Search for the biggest yet inlined attribute with
         * attstorage equals 'x' or 'e'
         * ------
         */
		for (i = 0; i < num_attrs; i++)
		{
			Form_pg_attribute att = (TupleDescAttr(tuple_desc, i));

			if (toast_action[i] == 'p')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue; /* can't happen, toast_action would be PLAIN */
			if (att->attstorage != 'x' && att->attstorage != 'e')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
         * Store this external
         */
		i = biggest_attno;
		old_value = toast_values[i];
		toast_action[i] = 'p';
		toast_values[i] =
			xheap_toast_save_datum(relation, toast_values[i], toast_old_external[i], options);
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));
		toast_free[i] = true;

		need_change = true;
		need_free = true;
	}

	/*
     * Round 3 - this time we take attributes with storage 'm' into
     * compression
     */
	while (xheap_calc_tuple_data_size(tuple_desc, toast_values, toast_is_null, hoff,
								  enable_reverse_bitmap, enable_reserve) > max_data_len)
	{
		int	   biggest_attno = -1;
		uint32 biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum  old_value;
		Datum  new_value;

		for (i = 0; i < num_attrs; i++)
		{
			if (toast_action[i] != ' ')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue; /* can't happen, toast_action would be PLAIN */
			if (VARATT_IS_COMPRESSED(DatumGetPointer(toast_values[i])))
				continue;
			if (((TupleDescAttr(tuple_desc, i)))->attstorage != 'm')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
         * Attempt to compress it inline
         */
		i = biggest_attno;
		old_value = toast_values[i];
		new_value = xheap_toast_compress_datum(old_value, (TupleDescAttr(tuple_desc, i))->attcompression);
		if (DatumGetPointer(new_value) != NULL)
		{
			/* successful compression */
			if (toast_free[i])
				pfree(DatumGetPointer(old_value));
			toast_values[i] = new_value;
			toast_free[i] = true;
			toast_sizes[i] = VARSIZE(DatumGetPointer(toast_values[i]));
			need_change = true;
			need_free = true;
		}
		else
		{
			/* incompressible, ignore on subsequent compression passes */
			toast_action[i] = 'x';
		}
	}

	/*
     * Finally we store attributes of type 'm' externally.  At this point we
     * increase the target tuple size, so that 'm' attributes aren't stored
     * externally unless really necessary.
     */
	max_data_len = MaxXHeapTupleSize(relation) - hoff;

	while (xheap_calc_tuple_data_size(tuple_desc, toast_values, toast_is_null, hoff,
								  enable_reverse_bitmap, enable_reserve) > max_data_len &&
		   relation->rd_rel->reltoastrelid != InvalidOid)
	{
		int	   biggest_attno = -1;
		uint32 biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum  old_value;


		for (i = 0; i < num_attrs; i++)
		{
			if (toast_action[i] == 'p')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue; /* can't happen, toast_action would be PLAIN */
			if (((TupleDescAttr(tuple_desc, i)))->attstorage != 'm')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
         * Store this external
         */
		i = biggest_attno;
		old_value = toast_values[i];
		toast_action[i] = 'p';
		toast_values[i] =
			xheap_toast_save_datum(relation, toast_values[i], toast_old_external[i], options);

		if (toast_free[i])
			pfree(DatumGetPointer(old_value));
		toast_free[i] = true;

		need_change = true;
		need_free = true;
	}

	/*
     * In the case we toasted any values, we need to build a new heap tuple
     * with the changed values.
     */
	if (need_change)
	{
		XHeapDiskTuple old_data = newtup->disk_tuple;
		XHeapDiskTuple new_data;
		int32		   new_header_len;
		int32		   new_data_len;
		int32		   new_tuple_len;

		/*
         * Calculate the new size of the tuple.
         *
         * Note: we used to assume here that the old tuple's t_hoff must equal
         * the new_header_len value, but that was incorrect.  The old tuple
         * might have a smaller-than-current natts, if there's been an ALTER
         * TABLE ADD COLUMN since it was stored; and that would lead to a
         * different conclusion about the size of the null bitmap, or even
         * whether there needs to be one at all.
         */
		new_header_len = SizeOfXHeapDiskTupleData;
		if (has_nulls)
		{
			int nullcount = 0;
			for (i = 0; i < num_attrs; i++)
			{
				if (toast_is_null[i])
					nullcount++;
			}
			if (enable_reverse_bitmap)
				new_header_len += BITMAPLEN(num_attrs + nullcount);
			else
				new_header_len += BITMAPLEN(num_attrs);
		}
		new_data_len =
			xheap_calc_tuple_data_size(tuple_desc, toast_values, toast_is_null, new_header_len,
								   enable_reverse_bitmap, enable_reserve);
		new_tuple_len = new_header_len + new_data_len;

		res_tup = (XHeapTuple) xheaptup_alloc(XHeapTupleDataSize + new_tuple_len);
		res_tup->disk_tuple_size = new_tuple_len;
		res_tup->ctid = newtup->ctid;
		res_tup->table_oid = newtup->table_oid;
		new_data = (XHeapDiskTuple) ((char *) res_tup + XHeapTupleDataSize);
		res_tup->disk_tuple = new_data;


		memcpy(new_data, old_data, SizeOfXHeapDiskTupleData);

		XHeapTupleHeaderSetNatts(new_data, num_attrs);
		new_data->t_hoff = new_header_len;
		new_data->flag &= XHEAP_VIS_STATUS_MASK;

		if (has_nulls)
			xheap_fill_disk_tuple(tuple_desc, toast_values, toast_is_null, new_data, new_data_len,
							   enable_reverse_bitmap, enable_reserve, true);
		else
			xheap_fill_disk_tuple(tuple_desc, toast_values, toast_is_null, new_data, new_data_len,
							   enable_reverse_bitmap, enable_reserve, false);
	}
	else
		res_tup = newtup;

	/*
     * Free allocated temp values
     */
	if (need_free)
		for (i = 0; i < num_attrs; i++)
			if (toast_free[i])
				pfree(DatumGetPointer(toast_values[i]));

	/*
     * Delete external values from the old tuple
     */
	if (need_del_old)
		for (i = 0; i < num_attrs; i++)
			if (toast_del_old[i])
				xheap_toast_delete_datum(relation, toast_old_values[i], 0);

	return res_tup;
}

/* ----------
 * toast_save_datum -
 *
 * 	Save one single datum into the secondary relation and return
 * 	a Datum reference for it.
 *
 * rel: the main relation we're working with (not the toast rel!)
 * value: datum to be pushed to toast storage
 * oldexternal: if not NULL, toast pointer previously representing the datum
 * options: options to be passed to heap_insert() for toast rows
 * ----------
 */
static Datum
xheap_toast_save_datum(Relation rel, Datum value, struct varlena *oldexternal, int options)
{
	Relation			   toastrel;
	Relation			  *toastidxs;
	XHeapTuple			   toasttup;
	TupleDesc			   toast_tupDesc;
	Datum				   t_values[3] = {0};
	bool				   t_isnull[3] = {0};
	CommandId			   mycid = GetCurrentCommandId(true);
	struct varlena		  *result = NULL;
	struct varatt_external toast_pointer;
	struct
	{
		struct varlena hdr;
		char		   data[XTOAST_MAX_CHUNK_SIZE + VARHDRSZ];
		int32		   align_it;
	} chunkData;
	int32	chunkSize;
	int32	chunkSeq = 0;
	char   *dataP = NULL;
	int32	dataTodo;
	Pointer dval = DatumGetPointer(value);
	int		num_indexes;
	int		validIndex;
	Size	resultSize = TOAST_POINTER_SIZE;

	Assert(!VARATT_IS_EXTERNAL(value));
	memset(&chunkData, 0, sizeof(chunkData));


	toastrel = table_open(rel->rd_rel->reltoastrelid, RowExclusiveLock);
	toast_tupDesc = toastrel->rd_att;

	/* Open all the toast indexes and look for the valid one */
	validIndex = toast_open_indexes(toastrel, RowExclusiveLock, &toastidxs, &num_indexes);

	/*
     * Get the data pointer and length, and compute va_rawsize and va_extsize.
     *
     * va_rawsize is the size of the equivalent fully uncompressed datum, so
     * we have to adjust for short headers.
     *
     * va_extsize is the actual size of the data payload in the toast records.
     */
	if (VARATT_IS_SHORT(dval))
	{
		dataP = VARDATA_SHORT(dval);
		dataTodo = VARSIZE_SHORT(dval) - VARHDRSZ_SHORT;
		toast_pointer.va_rawsize = dataTodo + VARHDRSZ; /* as if not short */
		toast_pointer.va_extinfo = dataTodo;
	}
	else if (VARATT_IS_COMPRESSED(dval))
	{
		dataP = VARDATA(dval);
		dataTodo = VARSIZE(dval) - VARHDRSZ;
		/* rawsize in a compressed datum is just the size of the payload */
		toast_pointer.va_rawsize = VARDATA_COMPRESSED_GET_EXTSIZE(dval) + VARHDRSZ;

		/* set external size and compression method */
		VARATT_EXTERNAL_SET_SIZE_AND_COMPRESS_METHOD(toast_pointer, dataTodo,
													 VARDATA_COMPRESSED_GET_COMPRESS_METHOD(dval));
		/* Assert that the numbers look like it's compressed */
		Assert(VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer));
	}
	else
	{
		dataP = VARDATA(dval);
		dataTodo = VARSIZE(dval) - VARHDRSZ;
		toast_pointer.va_rawsize = VARSIZE(dval);
		toast_pointer.va_extinfo = dataTodo;
	}

	/*
     * Insert the correct table OID into the result TOAST pointer.
     *
     * Normally this is the actual OID of the target toast table, but during
     * table-rewriting operations such as CLUSTER, we have to insert the OID
     * of the table's real permanent toast table instead.  rd_toastoid is set
     * if we have to substitute such an OID.
     */
	if (OidIsValid(rel->rd_toastoid))
		toast_pointer.va_toastrelid = rel->rd_toastoid;
	else
		toast_pointer.va_toastrelid = RelationGetRelid(toastrel);

	/*
     * Choose an OID to use as the value ID for this toast value.
     *
     * Normally we just choose an unused OID within the toast table.  But
     * during table-rewriting operations where we are preserving an existing
     * toast table OID, we want to preserve toast value OIDs too.  So, if
     * rd_toastoid is set and we had a prior external value from that same
     * toast table, re-use its value ID.  If we didn't have a prior external
     * value (which is a corner case, but possible if the table's attstorage
     * options have been changed), we have to pick a value ID that doesn't
     * conflict with either new or existing toast value OIDs.
     */
	if (!OidIsValid(rel->rd_toastoid))
	{
		/* normal case: just choose an unused OID */
		toast_pointer.va_valueid = GetNewOidWithIndex(
			toastrel, RelationGetRelid(toastidxs[validIndex]), (AttrNumber) 1);
	}
	else
	{
		/* rewrite case: check to see if value was in old toast table */
		toast_pointer.va_valueid = InvalidOid;
		if (oldexternal != NULL)
		{
			struct varatt_external oldToastPointer;
			Assert(VARATT_IS_EXTERNAL_ONDISK(oldexternal));
			/* Must copy to access aligned fields */
			VARATT_EXTERNAL_GET_POINTER(oldToastPointer, oldexternal);
			if (oldToastPointer.va_toastrelid == rel->rd_toastoid)
			{
				/* This value came from the old toast table; reuse its OID */
				toast_pointer.va_valueid = oldToastPointer.va_valueid;

				/*
                 * There is a corner case here: the table rewrite might have
                 * to copy both live and recently-dead versions of a row, and
                 * those versions could easily reference the same toast value.
                 * When we copy the second or later version of such a row,
                 * reusing the OID will mean we select an OID that's already
                 * in the new toast table.	Check for that, and if so, just
                 * fall through without writing the data again.
                 *
                 * While annoying and ugly-looking, this is a good thing
                 * because it ensures that we wind up with only one copy of
                 * the toast value when there is only one copy in the old
                 * toast table.  Before we detected this case, we'd have made
                 * multiple copies, wasting space; and what's worse, the
                 * multiple copies belonging to already-deleted heap tuples would not
                 * be reclaimed by VACUUM.
                 */
				if (toastid_valueid_exists(RelationGetRelid(toastrel),
										   toast_pointer.va_valueid))
				{
					/* Match, so short-circuit the data storage loop below */
					dataTodo = 0;
				}
			}
		}
		if (toast_pointer.va_valueid == InvalidOid)
		{
			/*
             * new value; must choose an OID that doesn't conflict in either
             * old or new toast table
             */
			do
			{
				toast_pointer.va_valueid = GetNewOidWithIndex(
					toastrel, RelationGetRelid(toastidxs[validIndex]), (AttrNumber) 1);
			} while (toastid_valueid_exists(rel->rd_toastoid, toast_pointer.va_valueid));
		}
	}

	/*
     * Initialize constant parts of the tuple data
     */
	t_values[ATTR_FIRST - 1] = ObjectIdGetDatum(toast_pointer.va_valueid);
	t_values[ATTR_SECOND] = PointerGetDatum(&chunkData);
	t_isnull[ATTR_FIRST - 1] = false;
	t_isnull[ATTR_SECOND - 1] = false;
	t_isnull[ATTR_THIRD - 1] = false;

	/*
     * Split up the item into chunks
     */
	while (dataTodo > 0)
	{
		int i;
		/*
         * Calculate the size of this chunk
         */
		chunkSize = Min(XTOAST_MAX_CHUNK_SIZE, (uint32) dataTodo);

		/*
         * Build a tuple and store it
         */
		t_values[1] = Int32GetDatum(chunkSeq++);
		SET_VARSIZE(&chunkData, chunkSize + VARHDRSZ);
		memcpy(VARDATA(&chunkData), dataP, chunkSize);
		toasttup = xheap_form_tuple(toast_tupDesc, t_values, t_isnull);

		(void) xheap_insert(toastrel, toasttup, mycid, options, NULL, true);

		/*
         * Create the index entry.	We cheat a little here by not using
         * FormIndexDatum: this relies on the knowledge that the index columns
         * are the same as the initial columns of the table.
         *
         * Note also that there had better not be any user-created index on
         * the TOAST table, since we don't bother to update anything else.
         */
		for (i = 0; i < num_indexes; i++)
		{
			/* Only index relations marked as ready can be updated */
			if (toastidxs[i]->rd_index->indisready)
				index_insert(toastidxs[i], t_values, t_isnull, 
							 &(toasttup->ctid), toastrel,
							 toastidxs[i]->rd_index->indisunique ? UNIQUE_CHECK_YES
																 : UNIQUE_CHECK_NO,
							 false,NULL);
		}

		/*
         * Free memory
         */
		XHeapFreeTuple(toasttup);

		/*
         * Move on to next chunk
         */
		dataTodo -= chunkSize;
		dataP += chunkSize;
	}

	/*
     * Done - close toast relation
     */
	toast_close_indexes(toastidxs, num_indexes, RowExclusiveLock);
	table_close(toastrel, RowExclusiveLock);

	/*
     * Create the TOAST pointer value that we'll return
     */
	result = (struct varlena *) palloc(resultSize);
	SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(result), &toast_pointer, sizeof(toast_pointer));
	return PointerGetDatum(result);
}

static void
xheap_toast_delete_datum(Relation rel, Datum value, int options)
{
	struct varlena		  *attr = (struct varlena *) DatumGetPointer(value);
	struct varatt_external toast_pointer;
	Relation			   toastrel;
	Relation			  *toastidxs;
	ScanKeyData			   toastkey;
	SysScanDesc			   toastscan;
	HeapTuple			   toasttup;
	int					   num_indexes;
	int					   valid_index;
	SnapshotData		   snapshot_toast;
	Datum				   t_values[3] = {0};
	bool				   t_isnull[3] = {0};
	TupleDesc			   toast_tup_desc;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		return;

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/*
     * Open the toast relation and its index
     */
	toastrel = table_open(toast_pointer.va_toastrelid, RowExclusiveLock);
	valid_index = toast_open_indexes(toastrel, RowExclusiveLock, &toastidxs, &num_indexes);
	toast_tup_desc = toastrel->rd_att;

	/* The toast table of xstore table should also be of xstore type */
	Assert(relation_is_xstore_format(toastrel));

	/*
     * Setup a scan key to find chunks with matching va_valueid
     */
	ScanKeyInit(&toastkey, (AttrNumber) 1, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

	/*
     * Find all the chunks.  (We don't actually care whether we see them in
     * sequence or not, but since we've already locked the index we might as
     * well use systable_beginscan_ordered.)
     */
	init_toast_snapshot(&snapshot_toast);
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[valid_index],
										   &snapshot_toast, 1, &toastkey);
	while ((toasttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		int i;
		/*
         * Have a chunk, delete it
         */
		simple_xheap_delete(toastrel, &toasttup->t_self, &snapshot_toast);
		heap_deform_tuple(toasttup, toast_tup_desc, t_values, t_isnull);
		for (i = 0; i < num_indexes; i++)
		{
			/* Only index relations marked as ready can be updated */
			if (toastidxs[i]->rd_index->indisready)
				index_delete(toastidxs[i], t_values, t_isnull, &toasttup->t_self, false);
		}
	}

	/*
     * End scan and close relations
     */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, RowExclusiveLock);
	table_close(toastrel, RowExclusiveLock);
}

/*
 * Fetch a TOAST slice from a xheap table.
 *
 * toastrel is the relation from which chunks are to be fetched.
 * valueid identifies the TOAST value from which chunks are being fetched.
 * attrsize is the total size of the TOAST value.
 * sliceoffset is the byte offset within the TOAST value from which to fetch.
 * slicelength is the number of bytes to be fetched from the TOAST value.
 * result is the varlena into which the results should be written.
 */
void
xheap_fetch_toast_slice(Relation toastrel, Oid valueid, int32 attrsize, int32 sliceoffset,
						int32 slicelength, struct varlena *result)
{
	Relation	*toastidxs;
	ScanKeyData	 toastkey[3];
	TupleDesc	 toasttupDesc = toastrel->rd_att;
	int			 nscankeys;
	SysScanDesc	 toastscan;
	HeapTuple	 ttup;
	int32		 expectedchunk;
	int32		 totalchunks = ((attrsize - 1) / XTOAST_MAX_CHUNK_SIZE) + 1;
	int			 startchunk;
	int			 endchunk;
	int			 num_indexes;
	int			 valid_index;
	SnapshotData snapshot_toast;

	/* Look for the valid index of toast relation */
	valid_index = toast_open_indexes(toastrel, AccessShareLock, &toastidxs, &num_indexes);

	startchunk = sliceoffset / XTOAST_MAX_CHUNK_SIZE;
	endchunk = (sliceoffset + slicelength - 1) / XTOAST_MAX_CHUNK_SIZE;
	Assert(endchunk <= totalchunks);

	/* Set up a scan key to fetch from the index. */
	ScanKeyInit(&toastkey[0], (AttrNumber) 1, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(valueid));

	/*
	 * No additional condition if fetching all chunks. Otherwise, use an
	 * equality condition for one chunk, and a range condition otherwise.
	 */
	if (startchunk == 0 && endchunk == totalchunks - 1)
		nscankeys = 1;
	else if (startchunk == endchunk)
	{
		ScanKeyInit(&toastkey[1], (AttrNumber) 2, BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(startchunk));
		nscankeys = 2;
	}
	else
	{
		ScanKeyInit(&toastkey[1], (AttrNumber) 2, BTGreaterEqualStrategyNumber, F_INT4GE,
					Int32GetDatum(startchunk));
		ScanKeyInit(&toastkey[2], (AttrNumber) 2, BTLessEqualStrategyNumber, F_INT4LE,
					Int32GetDatum(endchunk));
		nscankeys = 3;
	}

	/* Prepare for scan */
	init_toast_snapshot(&snapshot_toast);
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[valid_index],
										   &snapshot_toast, nscankeys, toastkey);

	/*
	 * Read the chunks by index
	 *
	 * The index is on (valueid, chunkidx) so they will come in order
	 */
	expectedchunk = startchunk;
	while ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		int32	curchunk;
		Pointer chunk;
		bool	isnull;
		char   *chunkdata;
		int32	chunksize;
		int32	expected_size;
		int32	chcpystrt;
		int32	chcpyend;

		/*
		 * Have a chunk, extract the sequence number and the data
		 */
		curchunk = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
		Assert(!isnull);
		chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
		Assert(!isnull);
		if (!VARATT_IS_EXTENDED(chunk))
		{
			chunksize = VARSIZE(chunk) - VARHDRSZ;
			chunkdata = VARDATA(chunk);
		}
		else if (VARATT_IS_SHORT(chunk))
		{
			/* could happen due to heap_form_tuple doing its thing */
			chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
			chunkdata = VARDATA_SHORT(chunk);
		}
		else
		{
			/* should never happen */
			elog(ERROR, "found toasted toast chunk for toast value %u in %s", valueid,
				 RelationGetRelationName(toastrel));
			chunksize = 0; /* keep compiler quiet */
			chunkdata = NULL;
		}

		/*
		 * Some checks on the data we've found
		 */
		if (curchunk != expectedchunk)
			ereport(
				PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal(
					 "unexpected chunk number %d (expected %d) for toast value %u in %s",
					 curchunk, expectedchunk, valueid,
					 RelationGetRelationName(toastrel))));
		if (curchunk > endchunk)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
							errmsg_internal("unexpected chunk number %d (out of range "
											"%d..%d) for toast value %u in %s",
											curchunk, startchunk, endchunk, valueid,
											RelationGetRelationName(toastrel))));
		expected_size = curchunk < totalchunks - 1
							? XTOAST_MAX_CHUNK_SIZE
							: attrsize - ((totalchunks - 1) * XTOAST_MAX_CHUNK_SIZE);
		if (chunksize != expected_size)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk size %d (expected %d) in chunk %d "
									 "of %d for toast value %u in %s",
									 chunksize, expected_size, curchunk, totalchunks,
									 valueid, RelationGetRelationName(toastrel))));

		/*
		 * Copy the data into proper place in our result
		 */
		chcpystrt = 0;
		chcpyend = chunksize - 1;
		if (curchunk == startchunk)
			chcpystrt = sliceoffset % XTOAST_MAX_CHUNK_SIZE;
		if (curchunk == endchunk)
			chcpyend = (sliceoffset + slicelength - 1) % XTOAST_MAX_CHUNK_SIZE;

		memcpy(VARDATA(result) + (curchunk * XTOAST_MAX_CHUNK_SIZE - sliceoffset) +
				   chcpystrt,
			   chunkdata + chcpystrt, (chcpyend - chcpystrt) + 1);

		expectedchunk++;
	}

	/*
	 * Final checks that we successfully fetched the datum
	 */
	if (expectedchunk != (endchunk + 1))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg_internal(
							"missing chunk number %d for toast value %u in %s",
							expectedchunk, valueid, RelationGetRelationName(toastrel))));

	/* End scan and close indexes. */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
}