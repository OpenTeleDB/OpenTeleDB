/* -------------------------------------------------------------------------
 *
 * xtuple.c
 * the row format of xstore inplace update engine.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xtuple.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/tupmacs.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/transam.h"
#include "access/tableam.h"
#include "access/sysattr.h"
#include "access/relation.h"
#include "xheap/xpage.h"
#include "xheap/xtuple.h"
#include "xheap/xtup_details.h"
#include "xheap/xheapam_visibility.h"
#include "util/xxact.h"
#include "undo/undolog.h"
#include "utils/typcache.h"
#include "utils/datum.h"

static const int ISNULL_ATTRS_PER_BYTE = 8;

bool enable_reserve_space_for_null_atts = true;

static inline bool
att_is_dropped(Form_pg_attribute attr)
{
	return (attr->attisdropped || strstr(attr->attname.data, "........pg.dropped."));
}

XHeapTuple
xheaptup_alloc(Size size)
{
	XHeapTuple tup = (XHeapTuple) palloc0(size);
	return tup;
}

void
check_tuple_validity(Relation rel, XHeapTuple xtuple)
{
	bool  is_null[MaxHeapAttributeNumber];
	Datum values[MaxHeapAttributeNumber];

	xheap_deform_tuple(xtuple, RelationGetDescr(rel), values, is_null);
}

/* The reason why we pass enableReserve to XHeapCalcTupleDataSize but not read it from GUC(PGC_USERSET level) is:
 * the process of form a tuple is a two-phase operation, firstly calculating the size and then filling the tuple.
 * The value of enableReserve should not be change between the two steps, otherwise it is a bug.
 * So only read enableReserve from GUC at the begining of forming a tuple, and then pass it to XHeapCalcTupleDataSize
 * and XheapFillDiskTuple. In this way we can make sure that they use the same value.
 * The same for toast operations.
 */
uint32
xheap_calc_tuple_data_size(TupleDesc tuple_desc, Datum *values, const bool *is_nulls,
					   uint32 hoff, bool enable_reverse_bitmap, bool enable_reserve)
{
	FormData_pg_attribute *attrs = tuple_desc->attrs;
	int				   attr_num = tuple_desc->natts;
	int				   size = hoff;
	int				   attrLen = 0;
	int				   i = 0;

	Assert(NAttrsReserveSpace(attr_num) == enable_reverse_bitmap);
	Assert(!enable_reserve || (enable_reserve && enable_reverse_bitmap));
	Assert(enable_reverse_bitmap ||
		   (enable_reverse_bitmap == false && enable_reserve == false));

	for (i = 0; i < attr_num; ++i)
	{
		Form_pg_attribute attr = &(attrs[i]);
		Datum			  val  = values[i];

		if (is_nulls[i] && (!enable_reserve || !attr->attbyval || att_is_dropped(attr)))
			continue;

		if (attr->attbyval)
			/* attbyval attributes are stored unaligned in xheap. */
			size += attr->attlen;
		else if (attr->attlen == -1 && attr->attstorage != 'p' &&
				 VARATT_CAN_MAKE_SHORT(DatumGetPointer(val)))
		{
			/*
             * we're anticipating converting to a short varlena header, so
             * adjust length and don't count any alignment
             */
			attrLen = VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(val));
			size += attrLen;
		}
		else
		{
			/*
             * We'll reach this case when storing a varlena that needs a
             * 4-byte header, a variable-width type that requires alignment
             * such as a record type, and for fixed-width types that are not
             * pass-by-value (e.g. aclitem).
             */
			size = att_align_datum(size, attr->attalign, attr->attlen, val);
			attrLen = att_addlength_datum(size, attr->attlen, val) - size;
			size += attrLen;
		}
	}

	return size - hoff;
}

/*  no align for pack tuple data
 * The reason why we pass enableReserve to XHeapCalcTupleDataSize but not read it from GUC(PGC_USERSET level) is:
 * the process of form a tuple is a two-phase operation, firstly calculating the size and then filling the tuple.
 * The value of enableReserve should not be change between the two steps, otherwise it is a bug.
 * So only read enableReserve from GUC at the begining of forming a tuple, and then pass it to XHeapCalcTupleDataSize
 * and XheapFillDiskTuple. In this way we can make sure that they use the same value.
 * The same for toast operations.
 */
void
xheap_fill_disk_tuple(TupleDesc tuple_desc, Datum *values, const bool *isnull,
				   XHeapDiskTupleData *disk_tuple, uint32 data_size,
				   bool enable_reverse_bitmap, bool enable_reserve, bool hasnull)
{
	bits8			  *bitP = NULL;
	uint32			   bitmask;
	bits8			  *bit_p_reserve = NULL;
	uint32			   bitmask_reserve = 0;
#ifdef USE_ASSERT_CHECKING
	char			  *begin = NULL;
#endif
	int				   number_of_attributes = tuple_desc->natts;
	char			  *data = NULL;
	Size			   attr_length;
	int				   nullcount, bitmap_bytes, tuple_attrs;
	int				   i = 0;
	FormData_pg_attribute *att = NULL;

	Assert(NAttrsReserveSpace(number_of_attributes) == enable_reverse_bitmap);
	Assert(!enable_reserve || enable_reverse_bitmap);
	Assert(enable_reverse_bitmap ||
		   (enable_reverse_bitmap == false && enable_reserve == false));

	att = tuple_desc->attrs;
	data = (char *) disk_tuple->data;

	if (hasnull)
	{
		nullcount = 0;
		bitmap_bytes = 0;
		tuple_attrs = XHeapTupleHeaderGetNatts(disk_tuple);
		Assert(tuple_attrs == number_of_attributes);
		for (i = 0; i < number_of_attributes; i++)
		{
			if (isnull[i])
				nullcount++;
		}
		bitP = &disk_tuple->data[0] - 1; /* data[-1] doesnt make compiler happy */
		bitmask = HIGHBIT;
		XHeapDiskTupSetHasNulls(disk_tuple);
		if (enable_reverse_bitmap)
			bitmap_bytes = BITMAPLEN(tuple_attrs + nullcount);
		else
			bitmap_bytes = BITMAPLEN(tuple_attrs);

		memset(data, 0, bitmap_bytes);

		bit_p_reserve = (bits8 *) (data + (tuple_attrs / ISNULL_ATTRS_PER_BYTE));
		bitmask_reserve = (1 << (uint32) (tuple_attrs % ISNULL_ATTRS_PER_BYTE));
		data = data + bitmap_bytes;
	}
#ifdef USE_ASSERT_CHECKING
	begin = data;
#endif

	for (i = 0; i < number_of_attributes; i++)
	{
		if (hasnull)
		{
			if (bitmask != HIGHBIT)
				bitmask <<= 1;
			else
			{
				bitP += 1;
				bitmask = 1;
			}

			if (isnull[i])
			{
				if (!enable_reserve || !att[i].attbyval || att_is_dropped(&(att[i])))
					;
				else
				{
					Assert(att[i].attlen > 0);
					data += att[i].attlen;
					*bit_p_reserve |= bitmask_reserve;
				}

				if (bitmask_reserve != HIGHBIT)
					bitmask_reserve <<= 1;
				else
				{
					bit_p_reserve += 1;
					bitmask_reserve = 1;
				}

				continue;
			}

			*bitP |= bitmask;
		}

		if (att[i].attbyval)
		{
			/* pass-by-value
             * Not aligned
             */
			store_att_byval(data, values[i], att[i].attlen);
			attr_length = att[i].attlen;
		}
		else if (att[i].attlen == LEN_VARLENA)
		{
			/* varlena */
			Pointer val = DatumGetPointer(values[i]);

			disk_tuple->flag |= HEAP_HASVARWIDTH;

			if (VARATT_IS_EXTERNAL(val))
			{
				disk_tuple->flag |= HEAP_HASEXTERNAL;
				attr_length = VARSIZE_EXTERNAL(val);
				memcpy(data, val, attr_length);
			}
			else if (VARATT_IS_SHORT(val))
			{
				attr_length = VARSIZE_SHORT(val);
				Assert(attr_length <= MaxPossibleXHeapTupleSize);
				memcpy(data, val, attr_length);
			}
			else if (att[i].attstorage != 'p' && VARATT_CAN_MAKE_SHORT(val))
			{
				attr_length = VARATT_CONVERTED_SHORT_SIZE(val);
				SET_VARSIZE_SHORT(data, attr_length);
				Assert(attr_length <= MaxPossibleXHeapTupleSize);
				memcpy(data + 1, VARDATA(val), attr_length - 1);
			}
			else
			{
				data = (char *) att_align_nominal(data, att[i].attalign);
				attr_length = VARSIZE(val);
				memcpy(data, val, attr_length);
			}
		}
		else if (att[i].attlen == LEN_CSTRING)
		{
			disk_tuple->flag |= HEAP_HASVARWIDTH;
			Assert(att[i].attalign == 'c');
			attr_length = strlen(DatumGetCString(values[i])) + 1;
			Assert(attr_length <= MaxPossibleXHeapTupleSize);
			memcpy(data, DatumGetPointer(values[i]), attr_length);
		}
		else
		{
			data = (char *) att_align_nominal(data, att[i].attalign);
			Assert(att[i].attlen > 0);
			attr_length = att[i].attlen;
			memcpy(data, DatumGetPointer(values[i]), attr_length);
		}

		data += attr_length;
	}
#ifdef USE_ASSERT_CHECKING
	Assert((size_t) (data - begin) == data_size);
#endif
}

/*
 * Copy a varlena column from source disk tuple (val) to destination (data)
 * Param names match XHeapCopyDiskTupleNoNull and XHeapCopyDiskTupleWithNulls
 */
static Pointer
xheap_copy_dtuple_varlena(Pointer val, Pointer data, Size attr_length,
					   const Size remaining_len, const Form_pg_attribute att)
{
	if (VARATT_IS_EXTERNAL(val))
	{
		attr_length = VARSIZE_EXTERNAL(val);
		memcpy(data, val, attr_length);
	}
	else if (VARATT_IS_SHORT(val))
	{
		attr_length = VARSIZE_SHORT(val);
		Assert(attr_length <= MaxPossibleXHeapTupleSize);
		memcpy(data, val, attr_length);
	}
	else if (att->attstorage != 'p' && VARATT_CAN_MAKE_SHORT(val))
	{
		attr_length = VARATT_CONVERTED_SHORT_SIZE(val);
		SET_VARSIZE_SHORT(data, attr_length);
		Assert(attr_length <= MaxPossibleXHeapTupleSize);
		memcpy(data + 1, VARDATA(val), attr_length - 1);
	}
	else
	{
		data = (char *) att_align_nominal(data, att->attalign);
		attr_length = VARSIZE(val);
		memcpy(data, val, attr_length);
	}

	return data;
}

/*
 * Partially copy a src disk tuple (lacking nulls) to a dest disk tuple
 * Only columns marked false in destnull are copied
 * Dest disk tuple has a null bitmap which marks which cols were copied
 */
void
xheap_copy_disk_tuple_no_null(TupleDesc tuple_desc, const bool *dest_null, XHeapTuple dest_tup,
						 AttrNumber last_var, const XHeapDiskTupleData *src_dtup)
{
	XHeapDiskTupleData *dest_dtup = dest_tup->disk_tuple;
	long				src_off = src_dtup->t_hoff;
	char			   *src_tup_ptr = (char *) src_dtup;
	char			   *data = (char *) dest_dtup + dest_dtup->t_hoff;
	char			   *begin = data;
	bool enable_reverse_bitmap = NAttrsReserveSpace(XHeapTupleHeaderGetNatts(dest_dtup));
	bool enable_reserve = enable_reserve_space_for_null_atts;

	uint32			   data_size;
	bits8			  *bitP;
	uint32			   bitmask = HIGHBIT;
	bits8			  *bit_p_reserve = NULL;
	uint32			   bitmask_reserve = 0;
	FormData_pg_attribute *att;
	int				   i = 0;
	int				   tuple_attrs;

	enable_reserve = enable_reserve && enable_reverse_bitmap;
	Assert(NAttrsReserveSpace(XHeapTupleHeaderGetNatts(dest_dtup)) == enable_reverse_bitmap);
	Assert(!enable_reserve || (enable_reserve && enable_reverse_bitmap));
	Assert(enable_reverse_bitmap ||
		   (enable_reverse_bitmap == false && enable_reserve == false));

	data_size = dest_tup->disk_tuple_size;
	bitP = &dest_dtup->data[0] - 1;
	att = tuple_desc->attrs;
	tuple_attrs = XHeapTupleHeaderGetNatts(src_dtup);
	bit_p_reserve =
		(bits8 *) ((char *) dest_dtup->data + (tuple_attrs / ISNULL_ATTRS_PER_BYTE));
	bitmask_reserve = (1 << (tuple_attrs % ISNULL_ATTRS_PER_BYTE));

	for (i = 0; i < last_var; i++)
	{
		Size	attrLength;
		Size	remainingLen = data_size - (size_t) (data - begin);

		if (bitmask != HIGHBIT)
			bitmask <<= 1;
		else
		{
			bitP += 1;
			bitmask = 1;
		}

		if (!dest_null[i])
			*bitP |= bitmask;

		if (att[i].attbyval)
		{
			/* attribute passed by value */
			if (dest_null[i])
			{
				src_off += att[i].attlen;
				if (enable_reserve)
				{
					Assert(att[i].attlen > 0);
					data += att[i].attlen;
					*bit_p_reserve |= bitmask_reserve;
				}

				if (bitmask_reserve != HIGHBIT)
					bitmask_reserve <<= 1;
				else
				{
					bit_p_reserve += 1;
					bitmask_reserve = 1;
				}

				continue;
			}
			attrLength = att[i].attlen;
			store_att_byval(data, *((Datum *) ((char *) src_tup_ptr + src_off)),
							att[i].attlen);
		}
		else if (att[i].attlen == LEN_VARLENA)
		{
			/* varlena */
			src_off = att_align_pointer(src_off, att[i].attalign, -1, src_tup_ptr + src_off);
			attrLength = VARSIZE_ANY(src_tup_ptr + src_off);

			if (!dest_null[i])
			{
				Pointer val = (Pointer) (src_tup_ptr + src_off);
				data =
					xheap_copy_dtuple_varlena(val, data, attrLength, remainingLen, &(att[i]));
			}
			else
			{
				if (bitmask_reserve != HIGHBIT)
					bitmask_reserve <<= 1;
				else
				{
					bit_p_reserve += 1;
					bitmask_reserve = 1;
				}
			}
		}
		else if (att[i].attlen == LEN_CSTRING)
		{
			/* null terminated cstring */
			src_off = att_align_nominal(src_off, att[i].attalign);
			Assert(att[i].attalign == 'c');
			attrLength = (uint32) strlen(src_tup_ptr + src_off) + 1;
			Assert(attrLength <= MaxPossibleXHeapTupleSize);
			if (!dest_null[i])
			{
				data = (char *) att_align_nominal(data, att[i].attalign);
				memcpy(data, (src_tup_ptr + src_off), attrLength);
			}
			else
			{
				if (bitmask_reserve != HIGHBIT)
					bitmask_reserve <<= 1;
				else
				{
					bit_p_reserve += 1;
					bitmask_reserve = 1;
				}
			}
		}
		else
		{
			/* fixed length */
			src_off = att_align_nominal(src_off, att[i].attalign);
			Assert(att[i].attlen > 0);
			attrLength = att[i].attlen;

			if (!dest_null[i])
			{
				data = (char *) att_align_nominal(data, att[i].attalign);
				memcpy(data, (src_tup_ptr + src_off), attrLength);
			}
			else
			{
				if (bitmask_reserve != HIGHBIT)
					bitmask_reserve <<= 1;
				else
				{
					bit_p_reserve += 1;
					bitmask_reserve = 1;
				}
			}
		}
		/* Compute the offset of the next attribute */
		src_off += attrLength;

		if (!dest_null[i])
			data += attrLength;
	}
}

/*
 * Partially copy a src disk tuple with nulls to a dest disk tuple
 * Only columns marked false in destNull as false are copied unless they are null in src
 */
void
xheap_copy_disk_tuple_with_nulls(TupleDesc tuple_desc, const bool *dest_null, XHeapTuple dest_tup,
							AttrNumber last_var, XHeapDiskTupleData *src_dtup)
{
	XHeapDiskTupleData *dest_dtup = dest_tup->disk_tuple;
	long				src_off = src_dtup->t_hoff;
	char			   *src_tup_ptr = (char *) src_dtup;
	char			   *data = (char *) dest_dtup + dest_dtup->t_hoff;
	char			   *begin = data;
	bool enable_reverse_bitmap = NAttrsReserveSpace(XHeapTupleHeaderGetNatts(dest_dtup));
	bool enable_reserve = enable_reserve_space_for_null_atts;

	uint32			   data_size;
	bits8			  *src_bp;
	bits8			  *bit_p;
	uint32			   bitmask = HIGHBIT;
	bits8			  *bit_p_reserve = NULL;
	uint32			   bitmask_reserve = 0;
	FormData_pg_attribute *att;
	int				   srcnullcount = 0;
	int				   i = 0;
	int				   tupleAttrs;

	enable_reserve = enable_reserve && enable_reverse_bitmap;
	Assert(NAttrsReserveSpace(XHeapTupleHeaderGetNatts(dest_dtup)) == enable_reverse_bitmap);
	Assert(!enable_reserve || (enable_reserve && enable_reverse_bitmap));
	Assert(enable_reverse_bitmap ||
		   (enable_reverse_bitmap == false && enable_reserve == false));

	data_size = dest_tup->disk_tuple_size;
	src_bp = src_dtup->data;
	bit_p = &dest_dtup->data[0] - 1;
	att = tuple_desc->attrs;
	tupleAttrs = XHeapTupleHeaderGetNatts(src_dtup);
	bit_p_reserve =
		(bits8 *) ((char *) dest_dtup->data + (tupleAttrs / ISNULL_ATTRS_PER_BYTE));
	bitmask_reserve = (1 << (tupleAttrs % ISNULL_ATTRS_PER_BYTE));

	for (i = 0; i < last_var; i++)
	{
		Size	attrLength;
		Size	remainingLen = data_size - (size_t) (data - begin);

		if (bitmask != HIGHBIT)
			bitmask <<= 1;
		else
		{
			bit_p += 1;
			bitmask = 1;
		}

		if (att_isnull(i, src_bp))
		{
			if (enable_reverse_bitmap)
			{
				if (!att_isnull(tupleAttrs + srcnullcount, src_bp))
				{
					Assert(att[i].attlen > 0);
					data += att[i].attlen;
					src_off += att[i].attlen;
					*bit_p_reserve |= bitmask_reserve;
				}
			}

			if (bitmask_reserve != HIGHBIT)
				bitmask_reserve <<= 1;
			else
			{
				bit_p_reserve += 1;
				bitmask_reserve = 1;
			}

			srcnullcount++;
			continue;
		}

		if (!dest_null[i])
			*bit_p |= bitmask;

		if (att[i].attbyval)
		{
			/* attribute is passed by value */
			if (dest_null[i])
			{
				src_off += att[i].attlen;
				if (enable_reserve)
				{
					Assert(att[i].attlen > 0);
					data += att[i].attlen;
					*bit_p_reserve |= bitmask_reserve;
				}

				if (bitmask_reserve != HIGHBIT)
					bitmask_reserve <<= 1;
				else
				{
					bit_p_reserve += 1;
					bitmask_reserve = 1;
				}

				continue;
			}
			attrLength = att[i].attlen;
			store_att_byval(data, *((Datum *) ((char *) src_tup_ptr + src_off)),
							att[i].attlen);
		}
		else if (att[i].attlen == LEN_VARLENA)
		{
			/* varlena */
			src_off = att_align_pointer(src_off, att[i].attalign, -1, src_tup_ptr + src_off);
			attrLength = VARSIZE_ANY(src_tup_ptr + src_off);

			if (!dest_null[i])
			{
				Pointer val = (Pointer) (src_tup_ptr + src_off);
				data =
					xheap_copy_dtuple_varlena(val, data, attrLength, remainingLen, &(att[i]));
			}
			else
			{
				if (bitmask_reserve != HIGHBIT)
					bitmask_reserve <<= 1;
				else
				{
					bit_p_reserve += 1;
					bitmask_reserve = 1;
				}
			}
		}
		else if (att[i].attlen == LEN_CSTRING)
		{
			/* null terminated cstring */
			src_off = att_align_nominal(src_off, att[i].attalign);
			Assert(att[i].attalign == 'c');
			attrLength = (uint32) strlen(src_tup_ptr + src_off) + 1;
			Assert(attrLength <= MaxPossibleXHeapTupleSize);
			if (!dest_null[i])
			{
				data = (char *) att_align_nominal(data, att[i].attalign);
				memcpy(data, (src_tup_ptr + src_off), attrLength);
			}
			else
			{
				if (bitmask_reserve != HIGHBIT)
					bitmask_reserve <<= 1;
				else
				{
					bit_p_reserve += 1;
					bitmask_reserve = 1;
				}
			}
		}
		else
		{
			/* fixed length */
			src_off = att_align_nominal(src_off, att[i].attalign);
			Assert(att[i].attlen > 0);
			attrLength = att[i].attlen;
			if (!dest_null[i])
			{
				data = (char *) att_align_nominal(data, att[i].attalign);
				memcpy(data, (src_tup_ptr + src_off), attrLength);
			}
			else
			{
				if (bitmask_reserve != HIGHBIT)
					bitmask_reserve <<= 1;
				else
				{
					bit_p_reserve += 1;
					bitmask_reserve = 1;
				}
			}
		}
		/* Compute the offset of the next attribute */
		src_off += attrLength;

		if (!dest_null[i])
			data += attrLength;
	}
}

XHeapTuple
xheap_form_tuple_shard(TupleDesc tuple_desc, Datum *values, bool *is_nulls)
{
	uint8				hoff = 0;
	uint32				disk_tuple_size = 0;
	uint32				i = 0;
	uint32				attr_num = tuple_desc->natts;
	bool				enable_reverse_bitmap = NAttrsReserveSpace(attr_num);
	bool				enable_reserve = enable_reserve_space_for_null_atts;
	XHeapDiskTupleData *disk_tuple = NULL;
	FormData_pg_attribute  *att = tuple_desc->attrs;
	int					nullcount = 0;
	uint32				data_size = 0;
	XHeapTuple			xheap_tuple_ptr;

	enable_reserve = enable_reserve && enable_reverse_bitmap;

	if (attr_num > MaxTupleAttributeNumber)
		ereport(
			ERROR,
			(errcode(ERRCODE_TOO_MANY_COLUMNS),
			 errmsg(
				 "number of columns (%d) exceeds limit (%d), type id (%u)",
				 attr_num, MaxTupleAttributeNumber, tuple_desc->tdtypeid)));

	/* Step 1: caculate xheap_tuple size for allocate memory */
	for (i = 0; i < attr_num; i++)
	{
		if (is_nulls[i])
			nullcount++;
		else
		{
			ereport(DEBUG5,
					(errmsg("attr name %s attlen %d attalign %c", att[i].attname.data,
							att[i].attlen, att[i].attalign)));
			if (att[i].attlen == -1)
				ereport(DEBUG5, (errmsg("attr %s size %lu", att[i].attname.data,
										VARSIZE_ANY(DatumGetPointer(values[i])))));
		}
	}
	disk_tuple_size = SizeOfXHeapDiskTupleData;

	if (nullcount > 0)
	{
		if (enable_reverse_bitmap)
			disk_tuple_size += BITMAPLEN(attr_num + nullcount);
		else
			disk_tuple_size += BITMAPLEN(attr_num);
			
		data_size = xheap_calc_tuple_data_size(tuple_desc, values, is_nulls, disk_tuple_size,
										  enable_reverse_bitmap, enable_reserve);
	}
	else
		data_size = xheap_calc_tuple_data_size(tuple_desc, values, is_nulls, disk_tuple_size,
										  enable_reverse_bitmap, enable_reserve);

	hoff = disk_tuple_size;
	disk_tuple_size += data_size;

	xheap_tuple_ptr = (XHeapTuple) xheaptup_alloc(disk_tuple_size + sizeof(XHeapTupleData));
	xheap_tuple_ptr->disk_tuple =
		(XHeapDiskTupleData *) ((char *) xheap_tuple_ptr + sizeof(XHeapTupleData));
	disk_tuple = xheap_tuple_ptr->disk_tuple;

	/* Step 2: fill disk tuple header and data */
	XHeapTupleHeaderSetNatts(disk_tuple, attr_num);
	disk_tuple->t_hoff = hoff;

	if (nullcount > 0)
		xheap_fill_disk_tuple(tuple_desc, values, is_nulls, disk_tuple, data_size,
						   enable_reverse_bitmap, enable_reserve, true);
	else
		xheap_fill_disk_tuple(tuple_desc, values, is_nulls, disk_tuple, data_size,
						   enable_reverse_bitmap, enable_reserve, false);

	xheap_tuple_ptr->disk_tuple_size = disk_tuple_size;

	return xheap_tuple_ptr;
}

XHeapTuple
xheap_modify_tuple(XHeapTuple tuple, TupleDesc tuple_desc, Datum *repl_values,
				 const bool *repl_isnull, const bool *do_replace)
{
	int		   number_of_attributes = tuple_desc->natts;
	int		   attoff;
	Datum	  *values = NULL;
	bool	  *isnull = NULL;
	XHeapTuple new_tuple;

	/*
     * allocate and fill values and isnull arrays from either the tuple or the
     * repl information, as appropriate.
     *
     * NOTE: it's debatable whether to use heap_deform_tuple() here or just
     * heap_getattr() only the non-replaced columns.  The latter could win if
     * there are many replaced columns and few non-replaced ones. However,
     * heap_deform_tuple costs only O(N) while the heap_getattr way would cost
     * O(N^2) if there are many non-replaced columns, so it seems better to
     * err on the side of linear cost.
     */
	values = (Datum *) palloc(number_of_attributes * sizeof(Datum));
	isnull = (bool *) palloc(number_of_attributes * sizeof(bool));

	xheap_deform_tuple(tuple, tuple_desc, values, isnull);

	for (attoff = 0; attoff < number_of_attributes; attoff++)
	{
		if (do_replace[attoff])
		{
			values[attoff] = repl_values[attoff];
			isnull[attoff] = repl_isnull[attoff];
		}
	}

	/*
     * create a new tuple from the values and isnull arrays
     */
	new_tuple = xheap_form_tuple(tuple_desc, values, isnull);

	pfree(values);
	pfree(isnull);

	/*
     * copy the identification info of the old tuple: t_ctid, t_self, and OID
     * (if any)
     */
	new_tuple->ctid = tuple->ctid;
	new_tuple->table_oid = tuple->table_oid;

	return new_tuple;
}

void
xheap_deform_tuple(XHeapTuple xtuple, TupleDesc row_desc, Datum *values, bool *is_nulls)
{
	xheap_deform_tuple_guts(xtuple, row_desc, values, is_nulls, row_desc->natts);
}

void
xheap_deform_tuple_guts(XHeapTuple xtuple, TupleDesc row_desc, Datum *values,
						bool *is_nulls, int unatts)
{
	XHeapDiskTuple	   disk_tuple = xtuple->disk_tuple;
	bool			   hasnulls = XHeapDiskTupHasNulls(disk_tuple);
	FormData_pg_attribute *att = row_desc->attrs;
	int				   natts; /* number of atts to extract */
	int				   attnum;
	bits8			  *bp = disk_tuple->data;
	long			   off = disk_tuple->t_hoff;
	char			  *tup_ptr = (char *) disk_tuple;
	int				   nullcount = 0;
	int				   tuple_attrs = XHeapTupleHeaderGetNatts(disk_tuple);
	bool			   enable_reverse_bitmap = NAttrsReserveSpace(tuple_attrs);

	natts = Min(tuple_attrs, unatts);
	for (attnum = 0; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = &(att[attnum]);

		if (hasnulls && att_isnull(attnum, bp))
		{
			/* Skip attribute length in case the tuple was stored with
               space reserved for null attributes */
			if (enable_reverse_bitmap)
			{
				if (!att_isnull(tuple_attrs + nullcount, bp))
					off += thisatt->attlen;
			}

			nullcount++;

			values[attnum] = (Datum) 0;
			is_nulls[attnum] = true;
			continue;
		}

		is_nulls[attnum] = false;

		/*
         * If this is a varlena, there might be alignment padding, if it has a
         * 4-byte header.  Otherwise, there will only be padding if it's not
         * pass-by-value.
         */
		if (thisatt->attlen == -1)
			off = att_align_pointer(off, thisatt->attalign, -1, tup_ptr + off);
		else if (!thisatt->attbyval)
			off = att_align_nominal(off, thisatt->attalign);

		values[attnum] = fetchatt(thisatt, tup_ptr + off);
		off = att_addlength_pointer(off, thisatt->attlen, tup_ptr + off);
	}

	/*
     * If tuple doesn't have all the atts indicated by tupleDesc, read the
     * rest as null
     */
	for (; attnum < unatts; attnum++)
		/* get init default value from tupleDesc. */
		values[attnum] = getmissingattr(row_desc, attnum + 1, &is_nulls[attnum]);
}

/*
 * This function is used to cast heap type tuple to xheap type tuple
 */
XHeapTuple
heap_to_xheap(Relation rel, HeapTuple heaptuple)
{
	Datum	  *values;
	bool	  *isnull;
	XHeapTuple xheaptuple;

	TupleDesc tuple_desc = RelationGetDescr(rel);

	values = (Datum *) palloc(sizeof(Datum) * tuple_desc->natts);
	isnull = (bool *) palloc(sizeof(bool) * tuple_desc->natts);
	heap_deform_tuple(heaptuple, tuple_desc, values, isnull);

	xheaptuple = xheap_form_tuple(tuple_desc, values, isnull);
	xheaptuple->ctid = heaptuple->t_self;
	xheaptuple->table_oid = heaptuple->t_tableOid;

	pfree(values);
	pfree(isnull);

	return xheaptuple;
}

/*
 * This function is used to cast xheap type tuple to heap type tuple
 */
HeapTuple
xheap_to_heap(Relation rel, XHeapTuple xheaptuple)
{
	HeapTuple heaptuple;
	TupleDesc tuple_desc 	= RelationGetDescr(rel);
	Datum	 *values 		= (Datum *) palloc(sizeof(Datum) * tuple_desc->natts);
	bool	 *isnull 		= (bool *) palloc(sizeof(bool) * tuple_desc->natts);

	xheap_deform_tuple(xheaptuple, tuple_desc, values, isnull);

	heaptuple = heap_form_tuple(tuple_desc, values, isnull);
	heaptuple->t_self = xheaptuple->ctid;
	heaptuple->t_tableOid = xheaptuple->table_oid;

	pfree(values);
	pfree(isnull);

	return heaptuple;
}

/*
 * XHeapGetSysAttr
 * Fetch the value of a system attribute for a tuple.
 */
Datum
xheap_get_sys_attr(XHeapTuple xhtup, Buffer buf, int attnum, TupleDesc tuple_desc,
				bool *isnull)
{
	Datum result = (Datum) 0;

	Assert(xhtup);

	/* Currently, no sys attribute ever reads as NULL. */
	*isnull = false;

	switch (attnum)
	{
		case SelfItemPointerAttributeNumber:
			/* pass-by-reference datatype */
			result = PointerGetDatum(&(xhtup->ctid));
			break;
		case MinTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(xhtup->xmin);
			break;
		case MaxTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(xhtup->xmax);
			break;
		case MinCommandIdAttributeNumber:
		case MaxCommandIdAttributeNumber:
			ereport(
				ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg(
					 "xmin, xmax, cmin, and cmax are not supported for xstore tuples")));
			break;
		case TableOidAttributeNumber:
			result = ObjectIdGetDatum(xhtup->table_oid);
			break;
		default:
			elog(ERROR, "invalid attnum: %d", attnum);
			result = 0; /* keep compiler quiet */
			break;
	}

	return result;
}

/*
 * xheap_tuple_attr_equals
 * Subroutine for XHeapDetermineModifiedColumns which returns the set of
 * attributes from the given att_list that are different in tup1 and tup2.
 */
Bitmapset *
xheap_tuple_attr_equals(TupleDesc tupdesc, Bitmapset *att_list, XHeapTuple tup1,
						XHeapTuple tup2)
{
	int		   lAttno = 0, col = -1;
	bool	   old_isnull[MaxHeapAttributeNumber];
	bool	   new_isnull[MaxHeapAttributeNumber];
	Datum	   old_values[MaxHeapAttributeNumber];
	Datum	   new_values[MaxHeapAttributeNumber];
	Bitmapset *modified = NULL;

	/* Find the largest attno in the given Bitmapset. */
	while ((col = bms_next_member(att_list, col)) >= 0)
	{
		/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
		AttrNumber attno = col + FirstLowInvalidHeapAttributeNumber;

		/*
         * Donot allow update on any system attribute other than tableOID,
         * which is stored in index tuples of Global Partition Indexes
         * and might not have been set correctly yet in the new tuple.
         */
		if (attno <= InvalidAttrNumber && attno != TableOidAttributeNumber)
			elog(ERROR, "system-column update is not supported");

		if (lAttno < attno)
			lAttno = attno;
	}

	Assert(lAttno <= tupdesc->natts);

	/* Deform both the old and new tuple. */
	xheap_deform_tuple_guts(tup1, tupdesc, old_values, old_isnull, lAttno);
	xheap_deform_tuple_guts(tup2, tupdesc, new_values, new_isnull, lAttno);

	/* Loop through atts and add every non-equal attno to modified. */
	col = -1;
	while ((col = bms_next_member(att_list, col)) >= 0)
	{
		/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
		AttrNumber attno = col + FirstLowInvalidHeapAttributeNumber;

		/* possible for Global Partition Indexes */
		if (attno == TableOidAttributeNumber)
			continue;

		/* If one value is NULL and other is not, they are not equal. */
		if (old_isnull[attno - 1] != new_isnull[attno - 1])
		{
			modified =
				bms_add_member(modified, attno - FirstLowInvalidHeapAttributeNumber);
			/* If both are NULL, they can be considered equal. */
		}
		else if (old_isnull[attno - 1])
			continue;
		else
		{
			Form_pg_attribute att = (TupleDescAttr(tupdesc, attno - 1));
			/*
             * We do simple binary comparison of the two datums.  This may be
             * overly strict because there can be multiple binary
             * representations for the same logical value.  But we should be
             * OK as long as there are no false positives.  Using a
             * type-specific equality operator is messy because there could be
             * multiple notions of equality in different operator classes;
             * furthermore, we cannot safely invoke user-defined functions
             * while holding exclusive buffer lock.
             */
			if (!datumIsEqual(old_values[attno - 1], new_values[attno - 1], att->attbyval,
							  att->attlen))
				modified =
					bms_add_member(modified, attno - FirstLowInvalidHeapAttributeNumber);
		}
	}

	return modified;
}

/*
 * XHeapCopyTuple
 * Returns a copy of an entire tuple.
 *
 * The XHeapTuple struct, tuple header, and tuple data are all allocated
 * as a single palloc() block.
 */
XHeapTuple
xheap_copy_tuple(XHeapTuple xhtup)
{
	XHeapTuple new_tuple;

	if (!XHeapTupleIsValid(xhtup) || xhtup->disk_tuple == NULL)
		return NULL;

	new_tuple = (XHeapTuple) xheaptup_alloc(XHeapTupleDataSize + xhtup->disk_tuple_size);
	new_tuple->ctid = xhtup->ctid;
	new_tuple->table_oid = xhtup->table_oid;
	new_tuple->disk_tuple_size = xhtup->disk_tuple_size;
	new_tuple->xmin = xhtup->xmin;
	new_tuple->xmax = xhtup->xmax;
	new_tuple->disk_tuple = (XHeapDiskTuple) ((char *) new_tuple + XHeapTupleDataSize);
	memcpy((char *) new_tuple->disk_tuple, (char *) xhtup->disk_tuple, xhtup->disk_tuple_size);
	return new_tuple;
}

void
xheap_copy_tuple_with_buffer(XHeapTuple src_tup, XHeapTuple dest_tup)
{
	Assert(XHeapTupleIsValid(src_tup));
	Assert(src_tup->disk_tuple != NULL);
	Assert(XHeapTupleIsValid(dest_tup));
	Assert(dest_tup->disk_tuple != NULL);

	dest_tup->ctid = src_tup->ctid;
	dest_tup->table_oid = src_tup->table_oid;
	dest_tup->disk_tuple_size = src_tup->disk_tuple_size;
	dest_tup->xmin = src_tup->xmin;
	dest_tup->xmax = src_tup->xmax;
	memcpy((char *) dest_tup->disk_tuple, (char *) src_tup->disk_tuple, src_tup->disk_tuple_size);
}

/*
 * XHeapTupleHeaderAdvanceLatestRemovedXid - Advance the latestRemovedXid, if
 * tuple is deleted by a transaction greater than latestRemovedXid.  This is
 * required to generate conflicts on hot standby.
 *
 * If we change this function then we need a similar change in
 * *_xlog_vacuum_get_latestRemovedXid functions as well.
 *
 * This is quite similar to HeapTupleHeaderAdvanceLatestRemovedXid.
 */
void
xheap_tuple_header_advance_latest_removed_xid(XHeapDiskTuple tuple, FullTransactionId xid,
										FullTransactionId *latest_removed_xid)
{
	/*
     * Ignore tuples inserted by an aborted transaction.
     *
     * XXX we can ignore the tuple if it was non-in-place updated/deleted by
     * the inserting transaction, but for that we need to traverse the
     * complete undo chain to find the root tuple, is it really worth?
     */
	if (xstore_transaction_id_did_commit(xid))
	{
		Assert((tuple->flag & XHEAP_DELETED) || (tuple->flag & XHEAP_UPDATED));
		if (FullTransactionIdFollows(xid, *latest_removed_xid))
			*latest_removed_xid = xid;
	}

	/* latestRemovedXid may still be invalid at end */
}

/*
 * XHeapAttIsNull
 * Returns TRUE if xheap tuple attribute is not present.
 */
bool
xheap_att_is_null(XHeapTuple tup, int attnum, TupleDesc tuple_desc)
{
	/*
     * We allow a NULL tupledesc for relations not expected to have missing
     * values, such as catalog relations and indexes.
     */
	Assert(!tuple_desc || attnum <= tuple_desc->natts);
	if (attnum > (int) XHeapTupleHeaderGetNatts(tup->disk_tuple))
		return true;

	if (attnum > 0)
	{
		if (XHeapDiskTupNoNulls(tup->disk_tuple))
			return false;

		return att_isnull(attnum - 1, tup->disk_tuple->data);
	}

	switch (attnum)
	{
		case TableOidAttributeNumber:
		case SelfItemPointerAttributeNumber:
		case MinTransactionIdAttributeNumber:
		case MinCommandIdAttributeNumber:
		case MaxTransactionIdAttributeNumber:
		case MaxCommandIdAttributeNumber:
			/* these are never null */
			break;
		default:
			elog(ERROR, "invalid attnum: %d", attnum);
	}

	return false;
}

/* XHeapNoCacheGetAttr
 * xheap equivalent to nocachegetattr
 */
Datum
xheap_no_cache_get_attr(XHeapTuple tuple, uint32 attnum, TupleDesc tuple_desc)
{
	XHeapDiskTuple	   tup = tuple->disk_tuple;
	FormData_pg_attribute *att = tuple_desc->attrs;
	char			  *tp = (char *) tup;  /* ptr to data part of tuple */
	bits8			  *bp = tup->data;	   /* ptr to null bitmap in tuple */
	bool			   slow = false;	   /* do we have to walk attrs? */
	int				   off;				   /* current offset within data */
	int				   hoff = tup->t_hoff; /* header length on tuple data */
	bool			   hasnulls = XHeapDiskTupHasNulls(tup);

	/* ----------------
     * Three cases:
     *
     * 1: No nulls and no variable-width attributes.
     * 2: Has a null or a var-width AFTER att.
     * 3: Has nulls or var-widths BEFORE att.
     * ----------------
     */

	/*
     * important:
     * maybe this function is not safe for accessing some attribute, which is different
     * from methods slot_getattr(), slot_getallattrs(), slot_getsomeattrs(). those three
     * always make sure that attnum is always valid between 1 and InplaceHeapTupleHeaderGetNatts(),
     * because the caller has guarantee that.
     * we find that the caller fastgetattr() doesn't guarantee the validition, and top callers
     * are almost in system table level, for example pg_class and so on. so that it's NOT
     * recommended that users' table functions call fastgetattr() and nocachegetattr();
     */
	Assert(attnum <= XHeapTupleHeaderGetNatts(tup));
	attnum--;

	if (!XHeapDiskTupNoNulls(tup))
	{
		/*
         * there's a null somewhere in the tuple
         *
         * check to see if any preceding bits are null...
         */
		int byte = attnum >> ATTNUM_BMP_SHIFT;
		int finalbit = attnum & 0x07;

		/* check for nulls "before" final bit of last byte */
		if ((~bp[byte]) & ((1 << finalbit) - 1))
			slow = true;
		else
		{
			/* check for nulls in any "earlier" bytes */
			int i = 0;
			for (i = 0; i < byte; i++)
			{
				if (bp[i] != 0xFF)
				{
					slow = true;
					break;
				}
			}
		}
	}

	if (!slow)
	{
		/*
         * If we get here, there are no nulls up to and including the target
         * attribute. Check for non-fixed-length attrs up to and including
         * target. If there aren't any, it's safe to cheaply initialize the
         * cached offsets for these attrs.
         */
		if (XHeapDiskTupHasVarWidth(tup))
		{
			uint32 j = 0;
			for (j = 0; j <= attnum; j++)
			{
				if (att[j].attlen <= 0)
				{
					slow = true;
					break;
				}
			}
		}
	}

	if (!slow)
	{
		uint32 natts = tuple_desc->natts;
		uint32 j = 1;

		/*
         * If we get here, we have a tuple with no nulls or var-widths up to
         * and including the target attribute, so we can use the cached offset
         * ... only we don't have it yet, or we'd not have got here.  Since
         * it's cheap to compute offsets for fixed-width columns, we take the
         * opportunity to initialize the cached offsets for *all* the leading
         * fixed-width columns, in hope of avoiding future visits to this
         * routine.
         */
		att[0].attcacheoff = 0;

		/* we might have set some offsets in the slow path previously */
		while (j < natts && att[j].attcacheoff > 0)
			j++;

		off = att[j - 1].attcacheoff + att[j - 1].attlen;

		for (; j < natts; j++)
		{
			if (att[j].attlen <= 0)
				break;

			att[j].attcacheoff = off;

			off += att[j].attlen;
		}

		Assert(j > attnum);

		off = att[attnum].attcacheoff + hoff;
	}
	else
	{
		bool usecache = true;

		/*
         * Now we know that we have to walk the tuple CAREFULLY.  But we still
         * might be able to cache some offsets for next time.
         *
         * Note - This loop is a little tricky.  For each non-null attribute,
         * we have to first account for alignment padding before the attr,
         * then advance over the attr based on its length.      Nulls have no
         * storage and no alignment padding either.  We can use/set
         * attcacheoff until we reach either a null or a var-width attribute.
         */

		int	   nullcount = 0;
		uint32 i = 0;
		int	   tupleAttrs = XHeapTupleHeaderGetNatts(tup);
		bool   enableReverseBitmap = NAttrsReserveSpace(tupleAttrs);
		off = hoff;
		for (i = 0; i <= attnum; i++)
		{ /* loop exit is at "break" */
			Assert(i < (uint32) tuple_desc->natts);

			if (hasnulls && att_isnull(i, bp))
			{
				if (enableReverseBitmap && !att_isnull(tupleAttrs + nullcount, bp))
					off += att[i].attlen;

				nullcount++;

				usecache = false;
				continue; /* this cannot be the target att */
			}

			if (att[i].attlen == LEN_VARLENA)
			{
				off = att_align_pointer(off, att[i].attalign, -1, tp + off);
			}
			else if (!att[i].attbyval)
				off = att_align_nominal(off, att[i].attalign);
			else if (usecache)
				att[i].attcacheoff = off - hoff;

			if (i == (uint32) attnum)
				break;

			off = att_addlength_pointer(off, att[i].attlen, tp + off);

			if (usecache && att[i].attlen <= 0)
				usecache = false;
		}
	}

	return fetchatt(&(att[attnum]), tp + off);
}