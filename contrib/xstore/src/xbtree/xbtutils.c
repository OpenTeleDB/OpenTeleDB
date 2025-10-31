/* -------------------------------------------------------------------------
 *
 * xbtutils.c
 *	  Utility code for teledb xbtree implementation.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/xbtree/xbtutils.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xstore/xstorehook.h"
#include "access/clog.h"
#include "access/nbtree.h"
#include "storage/block.h"
#include "utils/datum.h"
#include "utils/snapshot.h"
#include "xbtree/xbtree.h"
#include "access/xact.h"
#include "access/relscan.h"
#include "access/transam.h"
#include "xstore.h"
#include "catalog/pg_opfamily.h"
#include "storage/procarray.h"
#include "utils/rel.h"
#include "xbtree/xbttup.h"
#include "xbtree/xbtvisbility.h"

static int	_xbt_keep_natts(Relation rel, IndexTuple lastleft, IndexTuple firstright,
							BTScanInsert itup_key);

static IndexTuple _xbt_truncate_tuple(TupleDesc tuple_descriptor, IndexTuple olditup, int leavenatts, bool itup_undo);

static bool
_xbt_check_rowcompare(ScanKey skey, IndexTuple tuple, int tupnatts,
					 TupleDesc tupdesc, ScanDirection dir, bool *continuescan);

#define MAX(A, B) (((B) > (A)) ? (B) : (A))
#define MIN(A, B) (((B) < (A)) ? (B) : (A))

/*
 * _xbt_mkscankey
 *		Build an insertion scan key that contains comparison data from itup
 *		as well as comparator routines appropriate to the key datatypes.
 *
 *		When itup is a non-pivot tuple, the returned insertion scan key is
 *		suitable for finding a place for it to go on the leaf level.  Pivot
 *		tuples can be used to re-find leaf page with matching high key, but
 *		then caller needs to set scan key's pivotsearch field to true.  This
 *		allows caller to search for a leaf page with a matching high key,
 *		which is usually to the left of the first leaf page a non-pivot match
 *		might appear on.
 *
 *		The result is intended for use with _bt_compare() and _bt_truncate().
 *		Callers that don't need to fill out the insertion scankey arguments
 *		(e.g. they use an ad-hoc comparison routine, or only need a scankey
 *		for _bt_truncate()) can pass a NULL index tuple.  The scankey will
 *		be initialized as if an "all truncated" pivot tuple was passed
 *		instead.
 *
 *		Note that we may occasionally have to share lock the metapage to
 *		determine whether or not the keys in the index are expected to be
 *		unique (i.e. if this is a "heapkeyspace" index).  We assume a
 *		heapkeyspace index when caller passes a NULL tuple, allowing index
 *		build callers to avoid accessing the non-existent metapage.
 */
BTScanInsert
_xbt_mkscankey(Relation rel, IndexTuple itup)
{
	BTScanInsert key;
	ScanKey		skey;
	TupleDesc	itupdesc;
	int			indnkeyatts;
	int16	   *indoption;
	int			tupnatts;
	int			i;

	itupdesc = RelationGetDescr(rel);
	indnkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	indoption = rel->rd_indoption;
	tupnatts = itup ? XBTreeTupleGetNAtts(itup, rel) : 0;

	Assert(tupnatts <= IndexRelationGetNumberOfAttributes(rel));

	/*
	 * We'll execute search using scan key constructed on key columns.
	 * Truncated attributes and non-key attributes are omitted from the final
	 * scan key.
	 */
	key = palloc(offsetof(BTScanInsertData, scankeys) +
				 sizeof(ScanKeyData) * indnkeyatts);

	/* xbtree always take TID as tie-breaker */
	key->heapkeyspace = true;
	key->anynullkeys = false;	/* initial assumption */
	key->nextkey = false;
	key->backward = false;
	key->keysz = Min(indnkeyatts, tupnatts);
	key->scantid = key->heapkeyspace && itup ?
		BTreeTupleGetHeapTID(itup) : NULL;
	skey = key->scankeys;
	for (i = 0; i < indnkeyatts; i++)
	{
		FmgrInfo *procinfo = NULL;
		Datum	  arg;
		bool	  null = false;
		uint32	  flags;

		/*
         * We can use the cached (default) support procs since no cross-type
		 * comparison can be needed.
         */
		procinfo = index_getprocinfo(rel, i + 1, BTORDER_PROC);

		/*
         * Key arguments built from truncated attributes (or when caller
		 * provides no tuple) are defensively represented as NULL values. They
		 * should never be used.
         */
		if (i < tupnatts)
			arg = index_getattr(itup, i + 1, itupdesc, &null);
		else
		{
			arg = (Datum) 0;
			null = true;
		}
		flags = (null ? SK_ISNULL : 0) | (((uint16) indoption[i]) << SK_BT_INDOPTION_SHIFT);
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   flags,
									   (AttrNumber) (i + 1),
									   InvalidStrategy,
									   InvalidOid,
									   rel->rd_indcollation[i],
									   procinfo,
									   arg);
		/* Record if any key attribute is NULL (or truncated) */
		if (null)
			key->anynullkeys = true;
	}

	/*
	 * In NULLS NOT DISTINCT mode, we pretend that there are no null keys, so
	 * that full uniqueness check is done.
	 */
	if (rel->rd_index->indnullsnotdistinct)
		key->anynullkeys = false;

	return key;
}

/*
 * Test whether an indextuple satisfies all the scankey conditions.
 *
 * Return true if so, false if not.  If the tuple fails to pass the qual,
 * we also determine whether there's any need to continue the scan beyond
 * this tuple, and set *continuescan accordingly.  See comments for
 * _bt_preprocess_keys(), above, about how this is done.
 *
 * Forward scan callers can pass a high key tuple in the hopes of having
 * us set *continuescan to false, and avoiding an unnecessary visit to
 * the page to the right.
 *
 * scan: index scan descriptor (containing a search-type scankey)
 * tuple: index tuple to test
 * tupnatts: number of attributes in tupnatts (high key may be truncated)
 * dir: direction we are scanning in
 * continuescan: output parameter (will be set correctly in all cases)
 */
bool
_xbt_checkkeys(IndexScanDesc scan, IndexTuple tuple, int tupnatts,
			  ScanDirection dir, bool *continuescan)
{
	TupleDesc	tupdesc;
	BTScanOpaque so;
	int			keysz;
	int			ikey;
	ScanKey		key;

	Assert(XBTreeTupleGetNAtts(tuple, scan->indexRelation) == tupnatts);

	*continuescan = true; /* default assumption */

	tupdesc = RelationGetDescr(scan->indexRelation);
	so = (BTScanOpaque) scan->opaque;
	keysz = so->numberOfKeys;

	for (key = so->keyData, ikey = 0; ikey < keysz; key++, ikey++)
	{
		Datum datum;
		bool  isNull = false;
		Datum test;

		if (key->sk_attno > tupnatts)
		{
			/*
			 * This attribute is truncated (must be high key).  The value for
			 * this attribute in the first non-pivot tuple on the page to the
			 * right could be any possible value.  Assume that truncated
			 * attribute passes the qual.
			 */
			Assert(ScanDirectionIsForward(dir));
			continue;
		}

		/* row-comparison keys need special processing */
		if (key->sk_flags & SK_ROW_HEADER)
		{
			int tupnattrs = XBTreeTupleGetNAtts(tuple, scan->indexRelation);
			if (_xbt_check_rowcompare(key, tuple, tupnattrs, tupdesc, dir, continuescan))
				continue;
			return NULL;
		}

		datum = index_getattr(tuple,
							  key->sk_attno,
							  tupdesc,
							  &isNull);

		if (key->sk_flags & SK_ISNULL)
		{
			/* Handle IS NULL/NOT NULL tests */
			if (key->sk_flags & SK_SEARCHNULL)
			{
				if (isNull)
					continue; /* tuple satisfies this qual */
			}
			else
			{
				Assert(key->sk_flags & SK_SEARCHNOTNULL);
				if (!isNull)
					continue; /* tuple satisfies this qual */
			}

			/*
             * Tuple fails this qual.  If it's a required qual for the current
             * scan direction, then we can conclude no further tuples will
             * pass, either.
             */
			if ((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir))
				*continuescan = false;
			else if ((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir))
				*continuescan = false;

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}

		if (isNull)
		{
			if (key->sk_flags & SK_BT_NULLS_FIRST)
			{
				/*
                 * Since NULLs are sorted before non-NULLs, we know we have
                 * reached the lower limit of the range of values for this
                 * index attr.	On a backward scan, we can stop if this qual
                 * is one of the "must match" subset.  We can stop regardless
                 * of whether the qual is > or <, so long as it's required,
                 * because it's not possible for any future tuples to pass. On
                 * a forward scan, however, we must keep going, because we may
                 * have initially positioned to the start of the index.
                 */
				if ((key->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsBackward(dir))
					*continuescan = false;
			}
			else
			{
				/*
                 * Since NULLs are sorted after non-NULLs, we know we have
                 * reached the upper limit of the range of values for this
                 * index attr.	On a forward scan, we can stop if this qual is
                 * one of the "must match" subset.	We can stop regardless of
                 * whether the qual is > or <, so long as it's required,
                 * because it's not possible for any future tuples to pass. On
                 * a backward scan, however, we must keep going, because we
                 * may have initially positioned to the end of the index.
                 */
				if ((key->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsForward(dir))
					*continuescan = false;
			}

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}

		test = FunctionCall2Coll(&key->sk_func, key->sk_collation, 
							datum, key->sk_argument);
		
		if (!DatumGetBool(test))
		{
			/*
             * Tuple fails this qual.  If it's a required qual for the current
             * scan direction, then we can conclude no further tuples will
             * pass, either.
             *
             * Note: because we stop the scan as soon as any required equality
             * qual fails, it is critical that equality quals be used for the
             * initial positioning in _bt_first() when they are available. See
             * comments in _bt_first().
             */
			if ((key->sk_flags & SK_BT_REQFWD) && ScanDirectionIsForward(dir))
				*continuescan = false;
			else if ((key->sk_flags & SK_BT_REQBKWD) && ScanDirectionIsBackward(dir))
				*continuescan = false;

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}
	}
	/* If we get here, the tuple passes all index quals. */
	return true;
}

/*
 *  xbt_xidstatus() -- return the status of the given xid
 */
XidStatus
xbt_xidstatus(TransactionId xid)
{
	XLogRecPtr xidlsn;

	XidStatus ts = TRANSACTION_STATUS_COMMITTED;
	if(!TransactionIdIsValid(xid))
	{
		return TRANSACTION_STATUS_ABORTED;
	}
	if(!TransactionIdIsNormal(xid))
	{
		return TRANSACTION_STATUS_COMMITTED;
	}
	ts = TransactionIdGetStatus(xid, &xidlsn);
	
	/* Please refer to HeapTupleSatisfiesVaccum */
	if (ts == TRANSACTION_STATUS_IN_PROGRESS)
	{
		if (TransactionIdIsInProgress(xid))
			/* Inprogress */;
		else if (TransactionIdDidCommit(xid))
			ts = TRANSACTION_STATUS_COMMITTED;
		else
			ts = TRANSACTION_STATUS_ABORTED;
	}
	Assert(ts != TRANSACTION_STATUS_SUB_COMMITTED);
	return ts;
}

/*
 *	_xbt_truncate() -- create tuple without unneeded suffix attributes.
 *
 * Returns truncated pivot index tuple allocated in caller's memory context,
 * with key attributes copied from caller's firstright argument.  If rel is
 * an INCLUDE index, non-key attributes will definitely be truncated away,
 * since they're not part of the key space.  More aggressive suffix
 * truncation can take place when it's clear that the returned tuple does not
 * need one or more suffix key attributes.  We only need to keep firstright
 * attributes up to and including the first non-lastleft-equal attribute.
 * Caller's insertion scankey is used to compare the tuples; the scankey's
 * argument values are not considered here.
 *
 * Sometimes this routine will return a new pivot tuple that takes up more
 * space than firstright, because a new heap TID attribute had to be added to
 * distinguish lastleft from firstright.  This should only happen when the
 * caller is in the process of splitting a leaf page that has many logical
 * duplicates, where it's unavoidable.
 *
 * Note that returned tuple's t_tid offset will hold the number of attributes
 * present, so the original item pointer offset is not represented.  Caller
 * should only change truncated tuple's downlink.  Note also that truncated
 * key attributes are treated as containing "minus infinity" values by
 * _bt_compare().
 *
 * In the worst case (when a heap TID is appended) the size of the returned
 * tuple is the size of the first right tuple plus an additional MAXALIGN()'d
 * item pointer.  This guarantee is important, since callers need to stay
 * under the 1/3 of a page restriction on tuple size.  If this routine is ever
 * taught to truncate within an attribute/datum, it will need to avoid
 * returning an enlarged tuple to caller when truncation + TOAST compression
 * ends up enlarging the final datum.
 */
IndexTuple
_xbt_truncate(Relation rel, IndexTuple lastleft, IndexTuple firstright,
			   BTScanInsert itup_key, bool itup_undo)
{
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int16		nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	int			keepnatts;
	IndexTuple	pivot;
	IndexTuple	tidpivot;
	ItemPointer pivotheaptid;
	Size		newsize;

	/*
     * We should only ever truncate non-pivot tuples from leaf pages.  It's
     * never okay to truncate when splitting an internal page.
     */
	Assert(!XBTreeTupleIsPivot(lastleft) && !XBTreeTupleIsPivot(firstright));
	Assert(itup_key != NULL);

	/* Determine how many attributes must be kept in truncated tuple */
	keepnatts = _xbt_keep_natts(rel, lastleft, firstright, itup_key);

#ifdef DEBUG_NO_TRUNCATE
	/* Force truncation to be ineffective for testing purposes */
	keepnatts = nkeyatts + 1;
#endif

	pivot = _xbt_truncate_tuple(itupdesc, firstright, Min(keepnatts, nkeyatts),
									 itup_undo);

	/*
     * If there is a distinguishing key attribute within pivot tuple, we're
     * done
     */
	if (keepnatts <= nkeyatts)
	{
		XBTreeTupleSetNAtts(pivot, keepnatts, false);
		return pivot;
	}

	/*
     * We have to store a heap TID in the new pivot tuple, since no non-TID
     * key attribute value in firstright distinguishes the right side of the
     * split from the left side.  nbtree conceptualizes this case as an
     * inability to truncate away any key attributes, since heap TID is
     * treated as just another key attribute (despite lacking a pg_attribute
     * entry).
     *
     * Use enlarged space that holds a copy of pivot.  We need the extra space
     * to store a heap TID at the end (using the special pivot tuple
     * representation).
     */
	newsize = MAXALIGN(IndexTupleSize(pivot)) + MAXALIGN(sizeof(ItemPointerData));
	tidpivot = (IndexTuple) palloc0(newsize);
	memcpy(tidpivot, pivot, MAXALIGN(IndexTupleSize(pivot)));
	/* Cannot leak memory here */
	pfree(pivot);

	/*
     * Store all of firstright's key attribute values plus a tiebreaker heap
     * TID value in enlarged pivot tuple
     */
	tidpivot->t_info &= ~INDEX_SIZE_MASK;
	tidpivot->t_info |= newsize;
	XBTreeTupleSetNAtts(tidpivot, nkeyatts, true);
	pivotheaptid = XBTreeTupleGetHeapTID(tidpivot);

	/*
     * Lehman & Yao use lastleft as the leaf high key in all cases, but don't
     * consider suffix truncation.  It seems like a good idea to follow that
     * example in cases where no truncation takes place -- use lastleft's heap
     * TID.  (This is also the closest value to negative infinity that's
     * legally usable.)
	 *
	 * In index with undo, several index tuples may indicate to the same
     * xheap tuple. In this case, even TID can be equal.
     */
	ItemPointerCopy(XBTreeTupleGetMaxHeapTID(lastleft), pivotheaptid);
	
	/*
     * We're done.  Assert() that heap TID invariants hold before returning.
     *
     * Lehman and Yao require that the downlink to the right page, which is to
     * be inserted into the parent page in the second phase of a page split be
     * a strict lower bound on items on the right page, and a non-strict upper
     * bound for items on the left page.  Assert that heap TIDs follow these
     * invariants, since a heap TID value is apparently needed as a
     * tiebreaker.
     *
     */
#ifndef DEBUG_NO_TRUNCATE
	Assert(ItemPointerCompare(XBTreeTupleGetMaxHeapTID(lastleft),
							  XBTreeTupleGetHeapTID(firstright)) <= 0);
	Assert(ItemPointerCompare(pivotheaptid, XBTreeTupleGetHeapTID(lastleft)) >= 0);
	Assert(ItemPointerCompare(pivotheaptid, XBTreeTupleGetHeapTID(firstright)) <= 0);
#else
	/*
     * Those invariants aren't guaranteed to hold for lastleft + firstright
     * heap TID attribute values when they're considered here only because
     * DEBUG_NO_TRUNCATE is defined (a heap TID is probably not actually
     * needed as a tiebreaker).  DEBUG_NO_TRUNCATE must therefore use a heap
     * TID value that always works as a strict lower bound for items to the
     * right.  In particular, it must avoid using firstright's leading key
     * attribute values along with lastleft's heap TID value when lastleft's
     * TID happens to be greater than firstright's TID.
     */
	ItemPointerCopy(XBTreeTupleGetHeapTID(firstright), pivotheaptid);

	/*
     * Pivot heap TID should never be fully equal to firstright.  Note that
     * the pivot heap TID will still end up equal to lastleft's heap TID when
     * that's the only usable value.
     */
	ItemPointerSetOffsetNumber(
		pivotheaptid, OffsetNumberPrev(ItemPointerGetOffsetNumber(pivotheaptid)));
	Assert(ItemPointerCompare(pivotheaptid, XBTreeTupleGetHeapTID(firstright)) < 0);
#endif

	return tidpivot;
}

/*
 * _bt_keep_natts - how many key attributes to keep when truncating.
 *
 * Caller provides two tuples that enclose a split point.  Caller's insertion
 * scankey is used to compare the tuples; the scankey's argument values are
 * not considered here.
 *
 * This can return a number of attributes that is one greater than the
 * number of key attributes for the index relation.  This indicates that the
 * caller must use a heap TID as a unique-ifier in new pivot tuple.
 */
static int
_xbt_keep_natts(Relation rel, IndexTuple lastleft, IndexTuple firstright,
				BTScanInsert itup_key)
{
	int		  nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);
	TupleDesc itupdesc = RelationGetDescr(rel);
	int		  keepnatts;
	ScanKey	  scankey;

	/*
	 * Be consistent about the representation of BTREE_VERSION 2/3 tuples
	 * across Postgres versions; don't allow new pivot tuples to have
	 * truncated key attributes there.  _bt_compare() treats truncated key
	 * attributes as having the value minus infinity, which would break
	 * searches within !heapkeyspace indexes.
     */
	if (!itup_key->heapkeyspace)
		return nkeyatts;

	scankey = itup_key->scankeys;
	keepnatts = 1;
	for (int attnum = 1; attnum <= nkeyatts; attnum++, scankey++)
	{
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;

		datum1 = index_getattr(lastleft, attnum, itupdesc, &isNull1);
		datum2 = index_getattr(firstright, attnum, itupdesc, &isNull2);

		if (isNull1 != isNull2)
			break;

		if (!isNull1 &&
			DatumGetInt32(FunctionCall2Coll(&scankey->sk_func, 
											scankey->sk_collation, 
												datum1,
												datum2)) != 0)
			break;

		keepnatts++;
	}

	return keepnatts;
}

/*
 * _bt_keep_natts_fast - fast bitwise variant of _bt_keep_natts.
 *
 * This is exported so that a candidate split point can have its effect on
 * suffix truncation inexpensively evaluated ahead of time when finding a
 * split location.  A naive bitwise approach to datum comparisons is used to
 * save cycles.
 *
 * The approach taken here usually provides the same answer as _bt_keep_natts
 * will (for the same pair of tuples from a heapkeyspace index), since the
 * majority of btree opclasses can never indicate that two datums are equal
 * unless they're bitwise equal (once detoasted).  Similarly, result may
 * differ from the _bt_keep_natts result when either tuple has TOASTed datums,
 * though this is barely possible in practice.
 *
 * These issues must be acceptable to callers, typically because they're only
 * concerned about making suffix truncation as effective as possible without
 * leaving excessive amounts of free space on either side of page split.
 * Callers can rely on the fact that attributes considered equal here are
 * definitely also equal according to _bt_keep_natts.
 */
int
_xbt_keep_natts_fast(Relation rel, IndexTuple lastleft, IndexTuple firstright)
{
	TupleDesc itupdesc = RelationGetDescr(rel);
	int		  keysz = (rel->rd_rel->relnatts);
	int		  keepnatts;

	keepnatts = 1;
	for (int attnum = 1; attnum <= keysz; attnum++)
	{
		Datum			  datum1, datum2;
		bool			  isNull1 = false;
		bool			  isNull2 = false;
		Form_pg_attribute att;

		datum1 = index_getattr(lastleft, attnum, itupdesc, &isNull1);
		datum2 = index_getattr(firstright, attnum, itupdesc, &isNull2);
		att = TupleDescAttr(itupdesc, attnum - 1);

		if (isNull1 != isNull2)
		{
			break;
		}

		if (!isNull1 && !datum_image_eq(datum1, datum2, att->attbyval, att->attlen))
			break;

		keepnatts++;
	}

	return keepnatts;
}

/*
 *  _xbt_check_natts() -- Verify tuple has expected number of attributes.
 *
 * Returns value indicating if the expected number of attributes were found
 * for a particular offset on page.  This can be used as a general purpose
 * sanity check.
 *
 * Testing a tuple directly with BTreeTupleGetNAtts() should generally be
 * preferred to calling here.  That's usually more convenient, and is always
 * more explicit.  Call here instead when offnum's tuple may be a negative
 * infinity tuple that uses the pre-v11 on-disk representation, or when a low
 * context check is appropriate.  This routine is as strict as possible about
 * what is expected on each version of btree.
 */
bool
_xbt_check_natts(const Relation index, bool heapkeyspace, Page page, OffsetNumber offnum)
{
	int16				  natts = IndexRelationGetNumberOfAttributes(index);
	int16				  nkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
	XBTPageOpaqueInternal opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	IndexTuple			  itup;
	int					  tupnatts;

	/*
     * We cannot reliably test a deleted or half-dead page, since they have
     * dummy high keys
     */
	if (P_IGNORE(opaque))
	{
		return true;
	}

	Assert(offnum >= FirstOffsetNumber && offnum <= PageGetMaxOffsetNumber(page));

	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

	if (P_ISLEAF(opaque) && offnum >= P_FIRSTDATAKEY(opaque))
	{
		/*
         * Regular leaf tuples have as every index attributes
         */
		return (XBTreeTupleGetNAtts(itup, index) == natts);
	}
	else if (!P_ISLEAF(opaque) && offnum == P_FIRSTDATAKEY(opaque))
	{
		/*
         * Leftmost tuples on non-leaf pages have no attributes, or haven't
         * INDEX_ALT_TID_MASK set in pg_upgraded indexes.
         */
		return (XBTreeTupleGetNAtts(itup, index) == 0 ||
				((itup->t_info & INDEX_ALT_TID_MASK) == 0));
	}
	else
	{
		/*
         * Pivot tuples stored in non-leaf pages and hikeys of leaf pages
         * contain only key attributes
         */
		if (!heapkeyspace)
		{
			return (XBTreeTupleGetNAtts(itup, index) <= nkeyatts);
		}
	}

	/* Handle heapkeyspace pivot tuples (excluding minus infinity items) */
	Assert(heapkeyspace);

	/*
     * Explicit representation of the number of attributes is mandatory with
     * heapkeyspace index pivot tuples, regardless of whether or not there are
     * non-key attributes.
     */
	if (!XBTreeTupleIsPivot(itup))
		return false;

	tupnatts = XBTreeTupleGetNAtts(itup, index);
	/*
     * Heap TID is a tiebreaker key attribute, so it cannot be untruncated
     * when any other key attribute is truncated
     */
	if (XBTreeTupleGetHeapTID(itup) != NULL && tupnatts != nkeyatts)
		return false;

	/*
     * Pivot tuple must have at least one untruncated key attribute (minus
     * infinity pivot tuples are the only exception).  Pivot tuples can never
     * represent that there is a value present for a key attribute that
     * exceeds pg_index.indnkeyatts for the index.
     */
	return tupnatts > 0 && tupnatts <= nkeyatts;
}

/*
 *  BtCheckThirdPage() -- check whether tuple fits on a btree page at all.
 *
 * We actually need to be able to fit three items on every page, so restrict
 * any one item to 1/3 the per-page available space.  Note that itemsz should
 * not include the ItemId overhead.
 *
 * It might be useful to apply TOAST methods rather than throw an error here.
 * Using out of line storage would break assumptions made by suffix truncation
 * and by contrib/amcheck, though.
 */
void
_xbt_check_third_page(Relation rel, Relation heap, bool needheaptidspace, Page page,
					 IndexTuple newtup)
{
	Size				  itemsz;
	XBTPageOpaqueInternal opaque;

	itemsz = MAXALIGN(IndexTupleSize(newtup));
	/* Double check item size against limit */
	if (itemsz <= XBTMaxItemSize(page))
	{
		return;
	}

	/*
     * Internal page insertions cannot fail here, because that would mean that
     * an earlier leaf level insertion that should have failed didn't
     */
	opaque = (XBTPageOpaqueInternal) PageGetSpecialPointer(page);
	if (!P_ISLEAF(opaque))
		elog(ERROR,
			 "cannot insert oversized tuple of size %zu on internal page of index \"%s\"",
			 itemsz, RelationGetRelationName(rel));

	ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("index row size %lu exceeds maximum %lu for index \"%s\"",
						   (unsigned long) itemsz, (unsigned long) (XBTMaxItemSize(page)),
						   RelationGetRelationName(rel)),
					errdetail("Index row references tuple (%u,%u) in relation \"%s\".",
							  ItemPointerGetBlockNumber(&newtup->t_tid),
							  ItemPointerGetOffsetNumber(&newtup->t_tid),
							  heap ? RelationGetRelationName(heap) : "unknown"),
					errhint("Values larger than 1/3 of a buffer page cannot be indexed.\n"
							"Consider a function index of an MD5 hash of the value, "
							"or use full text indexing.")));
}

/*
 * Create a palloc'd copy of an index tuple, leaving only the first
 * leavenatts attributes remaining.
 *
 * Truncation is guaranteed to result in an index tuple that is no
 * larger than the original.  It is safe to use the IndexTuple with
 * the original tuple descriptor, but caller must avoid actually
 * accessing truncated attributes from returned tuple!  In practice
 * this means that index_getattr() must be called with special care,
 * and that the truncated tuple should only ever be accessed by code
 * under caller's direct control.
 *
 * It's safe to call this function with a buffer lock held, since it
 * never performs external table access.  If it ever became possible
 * for index tuples to contain EXTERNAL TOAST values, then this would
 * have to be revisited.
 */
static IndexTuple 
_xbt_truncate_tuple(TupleDesc sourceDescriptor, IndexTuple source, int leavenatts, bool itup_undo)
{
	TupleDesc	truncdesc;
	Datum values[INDEX_MAX_KEYS];
    bool isnull[INDEX_MAX_KEYS];
    IndexTuple truncated;

    Assert(leavenatts <= sourceDescriptor->natts);

    /* Easy case: no truncation actually required */
    if (leavenatts == sourceDescriptor->natts)
	{
        if (itup_undo) 
		{
            /* itup_undo indicates that the itup's size is not accurate */
            IndexTuple copied_itup;
            Size with_undo = IndexTupleSize(source);
            Size without_undo = with_undo - TXNINFOSIZE;

            /* subtract 8B before copy, see _bt_split() for more details */
            IndexTupleSetSize(source, without_undo);
            copied_itup = CopyIndexTuple(source);
			/* recover size after copy */
            IndexTupleSetSize(source, with_undo);
            return copied_itup;
        }
        return CopyIndexTuple(source);
    }

	/* Create temporary descriptor to scribble on */
	truncdesc = palloc(TupleDescSize(sourceDescriptor));
	TupleDescCopy(truncdesc, sourceDescriptor);
	truncdesc->natts = leavenatts;

    /* Deform, form copy of tuple with fewer attributes */
    index_deform_tuple(source, sourceDescriptor, values, isnull);
    truncated = index_form_tuple(truncdesc, values, isnull);
    truncated->t_tid = source->t_tid;
    Assert(IndexTupleSize(truncated) <= IndexTupleSize(source));

	/*
	 * Cannot leak memory here, TupleDescCopy() doesn't allocate any inner
	 * structure, so, plain pfree() should clean all allocated memory
	 */
    pfree(truncdesc);

    return truncated;
}

static bool
_xbt_check_rowcompare(ScanKey skey, IndexTuple tuple, int tupnatts,
					 TupleDesc tupdesc, ScanDirection dir, bool *continuescan)
{
	ScanKey		subkey = (ScanKey) DatumGetPointer(skey->sk_argument);
	int32		cmpresult = 0;
	bool		result;

	/* First subkey should be same as the header says */
	Assert(subkey->sk_attno == skey->sk_attno);

	/* Loop over columns of the row condition */
	for (;;)
	{
		Datum		datum;
		bool		isNull;

		Assert(subkey->sk_flags & SK_ROW_MEMBER);

		if (subkey->sk_attno > tupnatts)
		{
			/*
			 * This attribute is truncated (must be high key).  The value for
			 * this attribute in the first non-pivot tuple on the page to the
			 * right could be any possible value.  Assume that truncated
			 * attribute passes the qual.
			 */
			Assert(ScanDirectionIsForward(dir));
			Assert(BTreeTupleIsPivot(tuple));
			cmpresult = 0;
			if (subkey->sk_flags & SK_ROW_END)
				break;
			subkey++;
			continue;
		}

		datum = index_getattr(tuple,
							  subkey->sk_attno,
							  tupdesc,
							  &isNull);

		if (isNull)
		{
			if (subkey->sk_flags & SK_BT_NULLS_FIRST)
			{
				/*
				 * Since NULLs are sorted before non-NULLs, we know we have
				 * reached the lower limit of the range of values for this
				 * index attr.  On a backward scan, we can stop if this qual
				 * is one of the "must match" subset.  We can stop regardless
				 * of whether the qual is > or <, so long as it's required,
				 * because it's not possible for any future tuples to pass. On
				 * a forward scan, however, we must keep going, because we may
				 * have initially positioned to the start of the index.
				 */
				if ((subkey->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsBackward(dir))
					*continuescan = false;
			}
			else
			{
				/*
				 * Since NULLs are sorted after non-NULLs, we know we have
				 * reached the upper limit of the range of values for this
				 * index attr.  On a forward scan, we can stop if this qual is
				 * one of the "must match" subset.  We can stop regardless of
				 * whether the qual is > or <, so long as it's required,
				 * because it's not possible for any future tuples to pass. On
				 * a backward scan, however, we must keep going, because we
				 * may have initially positioned to the end of the index.
				 */
				if ((subkey->sk_flags & (SK_BT_REQFWD | SK_BT_REQBKWD)) &&
					ScanDirectionIsForward(dir))
					*continuescan = false;
			}

			/*
			 * In any case, this indextuple doesn't match the qual.
			 */
			return false;
		}

		if (subkey->sk_flags & SK_ISNULL)
		{
			/*
			 * Unlike the simple-scankey case, this isn't a disallowed case.
			 * But it can never match.  If all the earlier row comparison
			 * columns are required for the scan direction, we can stop the
			 * scan, because there can't be another tuple that will succeed.
			 */
			if (subkey != (ScanKey) DatumGetPointer(skey->sk_argument))
				subkey--;
			if ((subkey->sk_flags & SK_BT_REQFWD) &&
				ScanDirectionIsForward(dir))
				*continuescan = false;
			else if ((subkey->sk_flags & SK_BT_REQBKWD) &&
					 ScanDirectionIsBackward(dir))
				*continuescan = false;
			return false;
		}

		/* Perform the test --- three-way comparison not bool operator */
		cmpresult = DatumGetInt32(FunctionCall2Coll(&subkey->sk_func,
													subkey->sk_collation,
													datum,
													subkey->sk_argument));

		if (subkey->sk_flags & SK_BT_DESC)
			INVERT_COMPARE_RESULT(cmpresult);

		/* Done comparing if unequal, else advance to next column */
		if (cmpresult != 0)
			break;

		if (subkey->sk_flags & SK_ROW_END)
			break;
		subkey++;
	}

	/*
	 * At this point cmpresult indicates the overall result of the row
	 * comparison, and subkey points to the deciding column (or the last
	 * column if the result is "=").
	 */
	switch (subkey->sk_strategy)
	{
			/* EQ and NE cases aren't allowed here */
		case BTLessStrategyNumber:
			result = (cmpresult < 0);
			break;
		case BTLessEqualStrategyNumber:
			result = (cmpresult <= 0);
			break;
		case BTGreaterEqualStrategyNumber:
			result = (cmpresult >= 0);
			break;
		case BTGreaterStrategyNumber:
			result = (cmpresult > 0);
			break;
		default:
			elog(ERROR, "unrecognized RowCompareType: %d",
				 (int) subkey->sk_strategy);
			result = 0;			/* keep compiler quiet */
			break;
	}

	if (!result)
	{
		/*
		 * Tuple fails this qual.  If it's a required qual for the current
		 * scan direction, then we can conclude no further tuples will pass,
		 * either.  Note we have to look at the deciding column, not
		 * necessarily the first or last column of the row condition.
		 */
		if ((subkey->sk_flags & SK_BT_REQFWD) &&
			ScanDirectionIsForward(dir))
			*continuescan = false;
		else if ((subkey->sk_flags & SK_BT_REQBKWD) &&
				 ScanDirectionIsBackward(dir))
			*continuescan = false;
	}

	return result;
}