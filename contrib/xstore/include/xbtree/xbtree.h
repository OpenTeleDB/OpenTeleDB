/* -------------------------------------------------------------------------
 *
 * xbtree.h
 *    header file for xbtree access method implementation.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * 
 * include/xbtree/xbtree.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef XBTREE_H
#define XBTREE_H

#include "access/clog.h"
#include "access/nbtree.h"
#include "access/genam.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "access/transam.h"
#include "access/xlogreader.h"
#include "c.h"
#include "lib/stringinfo.h"
#include "storage/block.h"
#include "undo/undoxlog.h"
#include "xbtree/xbtxlog.h"
#include "xbtree/xbtrecycle.h"

/*
 * prototypes for functions in xbtree.c (external entry points for XBTree)
 */
extern void xbtbuildempty(Relation index);
extern bool xbtinsert(Relation rel, Datum *values, bool *isnull, ItemPointer heap_tid,
		  Relation heapRel, IndexUniqueCheck checkUnique, bool indexUnchanged, struct IndexInfo *indexInfo);
extern bool xbtdelete(Relation rel, Datum *values, bool *isnull, ItemPointer heap_tid, bool is_dead);
extern IndexScanDesc xbtbeginscan(Relation rel, int nkeys, int norderbys);
extern bool xbtgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 xbtgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void xbtrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys);
extern void xbtendscan(IndexScanDesc scan);
extern void xbtmarkpos(IndexScanDesc scan);
extern void xbtrestrpos(IndexScanDesc scan);
extern IndexBulkDeleteResult* xbtbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult* xbtvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
bool xbtcanreturn(Relation index, int attno);
extern bytea* xbtoptions(Datum reloptions, bool validate);
extern bool xbtproperty(Oid index_oid, int attno,
		   IndexAMProperty prop, const char *propname,
		   bool *res, bool *isnull);
extern char* xbtbuildphasename(int64 phasenum);
extern Size xbtestimateparallelscan(int nkeys, int norderbys);
extern void xbtinitparallelscan(void *target);
extern void xbtparallelrescan(IndexScanDesc scan);

extern Oid xbt_oid(void);
extern Oid xbt_boolopsfamily(void);
extern bool same_opfamily_for_btree_and_xbtree(Oid btreeOpf, Oid xbtreeOpf);
extern bool relation_is_xstore_index(void* relation);
extern Size size_of_xbtpage_opaque_data(void);

/*
 * There are rare cases where _xbt_truncate() will need to enlarge
 * a heap index tuple to make space for a tiebreaker heap TID
 * attribute, which we account for here.
 */
#define XBTMaxItemSize(page) \
    MAXALIGN_DOWN((PageGetPageSize(page) - \
                   MAXALIGN(SizeOfPageHeaderData + \
                            3*sizeof(ItemIdData) + \
                            3*sizeof(ItemPointerData)) - \
                   MAXALIGN(sizeof(XBTPageOpaqueData))) / 3)

#define XBTDefaultMaxItemSize \
    MAXALIGN_DOWN((BLCKSZ - \
                   MAXALIGN(SizeOfPageHeaderData + \
                            3*sizeof(ItemIdData) + \
                            3*sizeof(ItemPointerData)) - \
                   MAXALIGN(sizeof(XBTPageOpaqueData))) / 3)



/*
 * XBTPageOpaqueInternal can be directly converted to BTPageOpaqueInternal for use, but
 * XBTPageOpaque can't convert to BTPageOpaque. Must used carefully.
 */
typedef struct XBTPageOpaqueDataInternal {
    BlockNumber btpo_prev; /* left sibling, or P_NONE if leftmost */
    BlockNumber btpo_next; /* right sibling, or P_NONE if rightmost */
    union {
        uint32 level;                /* tree level --- zero for leaf pages */
        TransactionId xact_old; /* next transaction ID, if deleted */
    } btpo;
    uint16 btpo_flags;      /* flag bits, see below */
    BTCycleId btpo_cycleid; /* vacuum cycle ID of latest split */
    FullTransactionId pd_prune_xid;
    FullTransactionId last_delete_xid;
    int16 active_count;
} XBTPageOpaqueDataInternal;

typedef XBTPageOpaqueDataInternal* XBTPageOpaqueInternal;

typedef struct XBTPageOpaqueData {
    XBTPageOpaqueDataInternal bt_internal;
    FullTransactionId xact; /* next transaction ID, if deleted */
} XBTPageOpaqueData;

typedef XBTPageOpaqueData* XBTPageOpaque;

typedef struct xl_xbtree_split {
    uint32 level;            /* tree level of page being split */
    OffsetNumber firstright; /* first item moved to right page */
    OffsetNumber newitemoff; /* new item's offset (if placed on left page) */
    OffsetNumber newitemrightoff; /* new item's offset (if placed on right page) */
    uint32 rightitemcount;    /* count of right page items*/
    uint32 opaqueversion;    /* the opaque version */
    XBTPageOpaqueDataInternal lopaque;
    XBTPageOpaqueDataInternal ropaque;
} xl_xbtree_split;

#define SizeOfXBTreeSplit (sizeof(xl_xbtree_split))

#define XBTREE_OPAQUE_VERSION 1

/*
 * Notes on B-Tree tuple format, and key and non-key attributes:
 *
 *      !!! Special Note !!!
 *      heapkeyspace feature only used in xbtree, which
 *      is distinguished by RelationIsXstoreIndex(index).
 *
 * INCLUDE B-Tree indexes have non-key attributes.  These are extra
 * attributes that may be returned by index-only scans, but do not influence
 * the order of items in the index (formally, non-key attributes are not
 * considered to be part of the key space).  Non-key attributes are only
 * present in leaf index tuples whose item pointers actually point to heap
 * tuples (non-pivot tuples).  _bt_check_natts() enforces the rules
 * described here.
 *
 * Non-pivot tuple format (plain/non-posting variant):
 *
 *  t_tid | t_info | key values | INCLUDE columns, if any
 *
 * t_tid points to the heap TID, which is a tiebreaker key column as of
 * BTREE_VERSION 4.
 *
 * Non-pivot tuples complement pivot tuples, which only have key columns.
 * The sole purpose of pivot tuples is to represent how the key space is
 * separated.  In general, any B-Tree index that has more than one level
 * (i.e. any index that does not just consist of a metapage and a single
 * leaf root page) must have some number of pivot tuples, since pivot
 * tuples are used for traversing the tree.  Suffix truncation can omit
 * trailing key columns when a new pivot is formed, which makes minus
 * infinity their logical value.  Since BTREE_VERSION 4 indexes treat heap
 * TID as a trailing key column that ensures that all index tuples are
 * physically unique, it is necessary to represent heap TID as a trailing
 * key column in pivot tuples, though very often this can be truncated
 * away, just like any other key column. (Actually, the heap TID is
 * omitted rather than truncated, since its representation is different to
 * the non-pivot representation.)
 *
 * Pivot tuple format:
 *
 *  t_tid | t_info | key values | [heap TID]
 *
 * We store the number of columns present inside pivot tuples by abusing
 * their t_tid offset field, since pivot tuples never need to store a real
 * offset (pivot tuples generally store a downlink in t_tid, though).  The
 * offset field only stores the number of columns/attributes when the
 * INDEX_ALT_TID_MASK bit is set, which doesn't count the trailing heap
 * TID column sometimes stored in pivot tuples -- that's represented by
 * the presence of XBT_PIVOT_HEAP_TID_ATTR.  The INDEX_ALT_TID_MASK bit in
 * t_info is always set on BTREE_VERSION 4 pivot tuples, since
 * XBTreeTupleIsPivot() must work reliably on heapkeyspace versions.
 *
 * In version 2 or version 3 (!heapkeyspace) indexes, INDEX_ALT_TID_MASK
 * might not be set in pivot tuples.  XBTreeTupleIsPivot() won't work
 * reliably as a result.  The number of columns stored is implicitly the
 * same as the number of columns in the index, just like any non-pivot
 * tuple. (The number of columns stored should not vary, since suffix
 * truncation of key columns is unsafe within any !heapkeyspace index.)
 *
 * The 12 least significant offset bits from t_tid are used to represent
 * the number of columns in INDEX_ALT_TID_MASK tuples, leaving 4 status
 * bits (BT_RESERVED_OFFSET_MASK bits), 3 of which that are reserved for
 * future use.  BT_N_KEYS_OFFSET_MASK should be large enough to store any
 * number of columns/attributes <= INDEX_MAX_KEYS.
 */

/* Item pointer offset bit masks */
#define XBT_OFFSET_MASK				0x0FFF
#define XBT_STATUS_OFFSET_MASK		0xF000
/* XBT_STATUS_OFFSET_MASK status bits */
#define XBT_PIVOT_HEAP_TID_ATTR		0x1000
/* XBT_INSERT*/
#define XBT_INSERT_MASK             0x2000
/*
 * Note: XBTreeTupleIsPivot() can have false negatives (but not false
 * positives) when used with !heapkeyspace indexes
 */
static inline bool
XBTreeTupleIsPivot(IndexTuple itup)
{
    return  ((itup->t_info & INDEX_ALT_TID_MASK) != 0);
}

/*
 * Get/set downlink block number in pivot tuple.
 *
 * Note: Cannot assert that tuple is a pivot tuple.  If we did so then
 * !heapkeyspace indexes would exhibit false positive assertion failures.
 */
static inline BlockNumber
XBTreeTupleGetDownLink(IndexTuple pivot)
{
    return ItemPointerGetBlockNumberNoCheck(&pivot->t_tid);
}

static inline void
XBTreeTupleSetDownLink(IndexTuple pivot, BlockNumber blkno)
{
    ItemPointerSetBlockNumber(&pivot->t_tid, blkno);
}

/*
 * Get number of attributes within tuple.
 *
 * Note that this does not include an implicit tiebreaker heap TID
 * attribute, if any.  Note also that the number of key attributes must be
 * explicitly represented in all heapkeyspace pivot tuples.
 *
 * Note: This is defined as a macro rather than an inline function to
 * avoid including rel.h.
 */
#define XBTreeTupleGetNAtts(itup, rel)	\
	( \
		(XBTreeTupleIsPivot(itup)) ? \
		( \
			ItemPointerGetOffsetNumberNoCheck(&(itup)->t_tid) & BT_OFFSET_MASK \
		) \
		: \
		IndexRelationGetNumberOfAttributes(rel) \
	)

/*
 * Set number of key attributes in tuple.
 *
 * The heap TID tiebreaker attribute bit may also be set here, indicating that
 * a heap TID value will be stored at the end of the tuple (i.e. using the
 * special pivot tuple representation).
 */
static inline void
XBTreeTupleSetNAtts(IndexTuple itup, uint16 nkeyatts, bool heaptid)
{
    Assert(nkeyatts <= INDEX_MAX_KEYS);
    Assert((nkeyatts & XBT_STATUS_OFFSET_MASK) == 0);
    Assert(!heaptid || nkeyatts > 0);
    Assert(!XBTreeTupleIsPivot(itup) || nkeyatts == 0);

    itup->t_info |= INDEX_ALT_TID_MASK;

    if (heaptid)
        nkeyatts |= XBT_PIVOT_HEAP_TID_ATTR;

    ItemPointerSetOffsetNumber(&itup->t_tid, nkeyatts);
    Assert(XBTreeTupleIsPivot(itup));
}

/*
 * Get/set leaf page's "top parent" link from its high key.  Used during page
 * deletion.
 *
 * Note: Cannot assert that tuple is a pivot tuple.  If we did so then
 * !heapkeyspace indexes would exhibit false positive assertion failures.
 */
static inline BlockNumber
XBTreeTupleGetTopParent(IndexTuple leafhikey)
{
    return ItemPointerGetBlockNumberNoCheck(&leafhikey->t_tid);
}

static inline void
XBTreeTupleSetTopParent(IndexTuple leafhikey, BlockNumber blkno)
{
    ItemPointerSetBlockNumber(&leafhikey->t_tid, blkno);
    XBTreeTupleSetNAtts(leafhikey, 0, false);
}

/*
 * Get tiebreaker heap TID attribute, if any.
 *
 * This returns the first/lowest heap TID in the case of a posting list tuple.
 * Get tiebreaker heap TID attribute, if any.  Macro works with both pivot
 * and non-pivot tuples, despite differences in how heap TID is represented.
 */
static inline ItemPointer
XBTreeTupleGetHeapTID(IndexTuple itup)
{
    if (XBTreeTupleIsPivot(itup)) {
        if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) & XBT_PIVOT_HEAP_TID_ATTR) != 0) {
            return (ItemPointer) ((char *) itup + IndexTupleSize(itup) - sizeof(ItemPointerData));
        }
        return NULL;
    }

    return &itup->t_tid;
}

/*
 * Get maximum heap TID attribute, which could be the only TID in the case of
 * a non-pivot tuple.
 *
 * Works with non-pivot tuples only.
 */
static inline ItemPointer
XBTreeTupleGetMaxHeapTID(IndexTuple itup)
{
    Assert(!XBTreeTupleIsPivot(itup));

    return &itup->t_tid;
}

/* Working data for heap_page_prune and subroutines */
typedef struct {
    FullTransactionId new_prune_xid;    /* new prune hint value for page */
    FullTransactionId latestRemovedXid; /* latest xid to be removed by this prune */
    int ndead;
    /* arrays that accumulate indexes of items to be changed */
    OffsetNumber nowdead[MaxIndexTuplesPerPage];
    OffsetNumber previousdead[MaxIndexTuplesPerPage];
} IndexPruneState;

#define TXNINFOSIZE (sizeof(FullTransactionId) + sizeof(UndoRecPtr))

/*
 * prototypes for functions in xbtinsert.c
 */
extern bool _xbt_doinsert(Relation rel, IndexTuple itup,
						IndexUniqueCheck checkUnique, Relation heapRel);
extern bool _xbt_dodelete(Relation rel, IndexTuple itup, bool is_dead);

extern bool xbt_page_prune_opt(Relation rel, Buffer buf, bool tryDelete);
extern bool xbt_page_prune(Relation rel, Buffer buf, FullTransactionId oldestXmin);
extern bool xbt_prune_item(Page page, OffsetNumber offnum, FullTransactionId oldestXmin, IndexPruneState* prstate);
extern void xbt_page_prune_execute(Page page, OffsetNumber* nowdead, int ndead, IndexPruneState* prstate,
    FullTransactionId oldest_xmin);
extern void xbt_page_repair_fragmentation(Relation rel, BlockNumber blkno, Page page);
extern void _xbt_finish_split(Relation rel, Buffer lbuf, BTStack stack);
extern Buffer _xbt_getstackbuf(Relation rel, BTStack stack,BlockNumber child);
extern void _xbt_checkpage(Relation rel, Buffer buf);

/*
 * prototypes for functions in xbtundo.c
 */
extern UndoRecPtr _xbt_prepare_undo_insert(Oid relOid, Oid relfilenode, Oid tablespace,
                                            UndoPersistence persistence, FullTransactionId xid,
                                            CommandId cid, IndexTuple itup, UndoRecPtr prevurpInOneBlk,
                                            UndoRecPtr prevurpInOneXact, BlockNumber blk, XLogReaderState *xlog_record,
                                            xl_undo_header *xlundohdr, xl_undo_meta *xlundometa, FullTransactionId subxid);
extern UndoRecPtr _xbt_prepare_undo_delete(Oid relOid, Oid relfilenode, Oid tablespace,
						  UndoPersistence persistence, OffsetNumber offnum, FullTransactionId xid, 
						  CommandId cid, IndexTuple itup, UndoRecPtr prevurpInOneTup, UndoRecPtr prevurpInOneXact,
                          FullTransactionId xactid, BlockNumber blk, XLogReaderState *xlog_record,
                          xl_undo_header *xlundohdr, xl_undo_meta *xlundometa, FullTransactionId subxid);
/*
 * prototypes for functions in xbtsplitloc.c
 */
extern OffsetNumber _xbt_findsplitloc(Relation rel, Page page,
									 OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem,
									 bool *newitemonleft);

/*
 * prototypes for functions in xbtsort.c
 */
extern IndexBuildResult * xbtbuild(Relation heap, Relation index, struct IndexInfo *indexInfo);

/*
 * prototypes for functions in xbtsearch.c
 */
extern BTStack _xbt_search(Relation rel, BTScanInsert key, Buffer *bufP, int access, bool needStack);
extern Buffer _xbt_moveright(Relation rel, BTScanInsert itup_key, Buffer buf, bool forupdate, BTStack stack,
    int access);
extern OffsetNumber _xbt_binsrch(Relation rel, BTScanInsert key, Buffer buf);
extern OffsetNumber _xbt_binsrch_insert(Relation rel, BTInsertState insertstate);
extern int32 _xbt_compare(Relation rel, BTScanInsert key, Page page, OffsetNumber offnum);
extern bool _xbt_first(IndexScanDesc scan, ScanDirection dir);
extern bool _xbt_next(IndexScanDesc scan, ScanDirection dir);
extern Buffer _xbt_get_endpoint(Relation rel, uint32 level, bool rightmost);
extern bool xbt_gettuple_internal(IndexScanDesc scan, ScanDirection dir);

extern OffsetNumber
_xbt_findsameindexloc(Relation rel, Buffer *bufP, OffsetNumber offset,
					BTScanInsert itup_key, IndexTuple itup);

/*
 * prototypes for functions in xbtutils.c
 */
extern BTScanInsert _xbt_mkscankey(Relation rel, IndexTuple itup);
extern bool _xbt_checkkeys(IndexScanDesc scan, IndexTuple tuple, int tupnatts,
			  ScanDirection dir, bool *continuescan);
extern void _xbt_check_third_page(Relation rel, Relation heap, bool needheaptidspace, Page page, IndexTuple newtup);
extern IndexTuple _xbt_truncate(Relation rel, IndexTuple lastleft, IndexTuple firstright,
			   BTScanInsert itup_key, bool itup_undo);
extern int	_xbt_keep_natts_fast(Relation rel, IndexTuple lastleft, IndexTuple firstright);
extern bool _xbt_check_natts(const Relation index, bool heapkeyspace, Page page, OffsetNumber offnum);
extern XidStatus xbt_xidstatus(TransactionId xid);

/*
 * prototypes for functions in xbtpage.c
 */
extern void _xbt_pageinit(Page page, Size size);
extern void _xbt_initmetapage(Page page, BlockNumber rootbknum, uint32 level);
extern Buffer _xbt_getroot(Relation rel, int access);

extern int _xbt_pagedel(Relation rel, Buffer buf);
extern Buffer _xbt_getnewbuf(Relation rel, XBTRecycleQueueAddress* addr);
#endif /* XBTREE_H */
