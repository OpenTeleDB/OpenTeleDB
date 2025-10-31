/* -------------------------------------------------------------------------
 *
 * xmulti.h
 * the multi xact system for xstore.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * include/xheap/xmulti.h
 * -------------------------------------------------------------------------
 */

#ifndef XMULTI_H
#define XMULTI_H

#include "c.h"
#include "postgres.h"

#include "access/multixact.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/heapam.h"
#include "storage/lwlock.h"
#include "storage/sync.h"
#include "lib/ilist.h"
#include "storage/procarray.h"
#include "util/xxact.h"
#include "utils/memutils.h"

typedef FullTransactionId XMultiXactId;
typedef uint64 XMultiXactOffset;

static inline XMultiXactId
XMultiXactIdFromU64(uint64 value)
{
	XMultiXactId result;

	result.value = value;

	return result;
}

#define InvalidXMultiXactId	XMultiXactIdFromU64((uint64) 0)
#define FirstXMultiXactId	XMultiXactIdFromU64((uint64) 1)
#define MaxXMultiXactId		XMultiXactIdFromU64((uint64) 0xFFFFFFFFFFFFFFFF)

#define XMultiXactIdIsValid(multi) ((multi.value) != InvalidXMultiXactId.value)
#define XMultiXactIdEquals(a, b)	((a).value == (b).value)
#define MaxXMultiXactOffset	((XMultiXactOffset) 0xFFFFFFFFFFFFFFFF)


#define PreviousXMultiXactId(xid) \
	((xid.value) == FirstXMultiXactId.value ? MaxXMultiXactId : XMultiXactIdFromU64(xid.value - 1))


/*
 * Possible xmultixact lock modes ("status").  The first four modes are for
 * tuple locks (FOR KEY SHARE, FOR SHARE, FOR NO KEY UPDATE, FOR UPDATE); the
 * next two are used for update and delete modes.
 */
typedef MultiXactStatus XMultiXactStatus;

#define XMultiXactStatusForKeyShare MultiXactStatusForKeyShare
#define XMultiXactStatusForShare MultiXactStatusForShare
#define XMultiXactStatusForNoKeyUpdate MultiXactStatusForNoKeyUpdate
#define XMultiXactStatusForUpdate MultiXactStatusForUpdate
#define XMultiXactStatusNoKeyUpdate MultiXactStatusNoKeyUpdate
#define XMultiXactStatusUpdate MultiXactStatusUpdate

#define MaxXMultiXactStatus XMultiXactStatusUpdate

/* does a status value correspond to a tuple update? */
#define ISUPDATE_from_xmxstatus(status) \
                        ((status) > XMultiXactStatusForUpdate)

#define XMaxMultiXactStatus XMultiXactStatusUpdate

/* ----------------
 *		multixact-related XLOG entries
 * ----------------
 */
#define XLOG_XMULTIXACT_ZERO_OFF_PAGE	0x00
#define XLOG_XMULTIXACT_ZERO_MEM_PAGE	0x10
#define XLOG_XMULTIXACT_CREATE_ID		0x20
#define XLOG_XMULTIXACT_TRUNCATE_ID		0x30

typedef struct XMultiXactMember
{
	FullTransactionId xid;
	XMultiXactStatus status;
} XMultiXactMember;	

typedef struct xl_xmultixact_create
{
	XMultiXactId mid;			/* new XMultiXact's ID */
	XMultiXactOffset moff;		/* its starting offset in members file */
	int32		nmembers;		/* number of member XIDs */
	XMultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} xl_xmultixact_create;

#define SizeOfXMultiXactCreate (offsetof(xl_xmultixact_create, members))

typedef struct xl_xmultixact_truncate
{
	/* to-be-truncated limit of xmultixact offsets */
	XMultiXactId endTruncOff;

	/* to-be-truncated limit of multixact members */
	XMultiXactOffset endTruncMemb;
} xl_xmultixact_truncate;

#define SizeOfXMultiXactTruncate (sizeof(xl_xmultixact_truncate))

/*
 * Defines for XMultiXactOffset page sizes.  A page is the same BLCKSZ as is
 * used everywhere else in Postgres.
 */
/* We need 8 bytes per offset */
#define XMULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(XMultiXactOffset))

#define XMultiXactIdToOffsetPage(xid) \
	((xid).value / (XMultiXactOffset) XMULTIXACT_OFFSETS_PER_PAGE)
#define XMultiXactIdToOffsetEntry(xid) \
	((xid).value % (XMultiXactOffset) XMULTIXACT_OFFSETS_PER_PAGE)
#define XMultiXactIdToOffsetSegment(xid) (XMultiXactIdToOffsetPage(xid) / XLRU_PAGES_PER_SEGMENT)

/*
 * The situation for members is a bit more complex: we store one byte of
 * additional flag bits for each TransactionId.  To do this without getting
 * into alignment issues, we store four bytes of flags, and then the
 * corresponding 4 Xids.  Each such 5-word (36-byte) set we call a "group", and
 * are stored as a whole in pages.  Thus, with 8kB BLCKSZ, we keep 227 groups
 * per page.  This wastes 20 bytes per page, but that's OK -- simplicity (and
 * performance) trumps space efficiency here.
 *
 * Note that the "offset" macros work with byte offset, not array indexes, so
 * arithmetic must be done using "char *" pointers.
 */
/* We need eight bits per xact, so one xact fits in a byte */
#define XMXACT_MEMBER_BITS_PER_XACT			8
#define XMXACT_MEMBER_FLAGS_PER_BYTE			1
#define XMXACT_MEMBER_XACT_BITMASK	((1 << XMXACT_MEMBER_BITS_PER_XACT) - 1)

/* how many full bytes of flags are there in a group? */
#define XMULTIXACT_FLAGBYTES_PER_GROUP		4
#define XMULTIXACT_MEMBERS_PER_MEMBERGROUP	\
	(XMULTIXACT_FLAGBYTES_PER_GROUP * XMXACT_MEMBER_FLAGS_PER_BYTE)
/* size in bytes of a complete group */
#define XMULTIXACT_MEMBERGROUP_SIZE \
	(sizeof(FullTransactionId) * XMULTIXACT_MEMBERS_PER_MEMBERGROUP + XMULTIXACT_FLAGBYTES_PER_GROUP)
#define XMULTIXACT_MEMBERGROUPS_PER_PAGE (BLCKSZ / XMULTIXACT_MEMBERGROUP_SIZE)
#define XMULTIXACT_MEMBERS_PER_PAGE	\
	(XMULTIXACT_MEMBERGROUPS_PER_PAGE * XMULTIXACT_MEMBERS_PER_MEMBERGROUP)

/* page in which a member is to be found */
#define XMXOffsetToMemberPage(offset) ((offset) / (XMultiXactOffset) XMULTIXACT_MEMBERS_PER_PAGE)

#define XMXOffsetToMemberSegment(offset) (XMXOffsetToMemberPage(offset) / XLRU_PAGES_PER_SEGMENT)

/* Location (byte offset within page) of flag word for a given member */
#define XMXOffsetToFlagsOffset(offset) \
	((((offset) / (XMultiXactOffset) XMULTIXACT_MEMBERS_PER_MEMBERGROUP) % \
	  (XMultiXactOffset) XMULTIXACT_MEMBERGROUPS_PER_PAGE) * \
	 (XMultiXactOffset) XMULTIXACT_MEMBERGROUP_SIZE)
#define XMXOffsetToFlagsBitShift(offset) \
	(((offset) % (XMultiXactOffset) XMULTIXACT_MEMBERS_PER_MEMBERGROUP) * \
	 XMXACT_MEMBER_BITS_PER_XACT)

/* Location (byte offset within page) of TransactionId of given member */
#define XMXOffsetToMemberOffset(offset) \
	(XMXOffsetToFlagsOffset(offset) + XMULTIXACT_FLAGBYTES_PER_GROUP + \
	 ((offset) % XMULTIXACT_MEMBERS_PER_MEMBERGROUP) * sizeof(FullTransactionId))

extern LWLockId XMultiXactGenLock;
extern LWLockId XMultiXactOffsetSLRULock;
extern LWLockId XMultiXactMemberSLRULock;

extern int XMultiOffsetSyncHandlerPos;
extern int XMultiMemberSyncHandlerPos;

extern Size XMultiShmemSize(void);
extern void StartupXMultiXact(void);
extern void XMultiRequestNamedLWLockTranche(void);
extern void XMultiXactShmemInit(Pointer ptr, bool found);
extern bool XMultiXactIdPrecedes(XMultiXactId multi1, XMultiXactId multi2);
extern bool XMultiXactIdPrecedesOrEquals(XMultiXactId multi1, XMultiXactId multi2);
extern int xmultixactoffsetssyncfiletag(const FileTag *ftag, char *path);
extern int xmultixactmemberssyncfiletag(const FileTag *ftag, char *path);

extern XMultiXactId XMultiXactIdCreate(FullTransactionId xid1, XMultiXactStatus status1,
    								  FullTransactionId xid2, XMultiXactStatus status2);
extern XMultiXactId XMultiXactIdExpand(XMultiXactId multi, FullTransactionId xid, XMultiXactStatus status);
extern int GetXMultiXactIdMembers(XMultiXactId multi, XMultiXactMember** members);
extern void xmultixact_redo(XLogReaderState *record);
extern const char *xmultixact_identify(uint8 info);
extern void xmultixact_desc(StringInfo buf, XLogReaderState *record);
extern void XMultiXactAdvanceNextXMXact(XMultiXactId minMulti, XMultiXactOffset minMultiOffset);
extern void XMultiXactIdSetOldestMember(void);
extern void xmultixact_twophase_recover(TransactionId xid, uint16 info,
									   void *recdata, uint32 len);
extern void xmultixact_twophase_postcommit(TransactionId xid, uint16 info,
										  void *recdata, uint32 len);
extern void xmultixact_twophase_postabort(TransactionId xid, uint16 info,
										 void *recdata, uint32 len);

extern void AtPrepare_XMultiXact(void);
extern void PostPrepare_XMultiXact(TransactionId xid);
extern void AtEOXact_XMultiXact(void);
extern XMultiXactId GetOldestXMultiXactId(void);
extern void TruncateXMultiXact(void);
extern void XMultiXactSetNextXMXact(XMultiXactId nextXMulti, XMultiXactOffset nextXMultiOffset);
extern void XMultiXactGetCheckptXMulti(FullTransactionId *nextXMulti, uint64 *nextXMultiOffset);
extern void XMultiXactAdvanceNextXMXactWrapper(FullTransactionId minXMulti, uint64 minMultiOffset);
extern void XMultiXactIdWait(XMultiXactId multi, XMultiXactStatus status);
extern bool ConditionalXMultiXactIdWait(XMultiXactId multi, XMultiXactStatus status);
extern XMultiXactStatus get_xmxact_status_for_lock(LockTupleMode mode, bool is_update);
extern bool XMultiXactIdIsCurrent(XMultiXactId multi, FullTransactionId *xid);
extern void CheckPointXMultiXact(void);
extern void TrimXMultiXact(void);
extern void InitXMultiFiles(void);

/*
 * XMultiXact state shared across all backends.  All this state is protected
 * by XMultiXactGenLock.  (We also use XMultiXactOffsetSLRULock and
 * XMultiXactMemberSLRULock to guard accesses to the two sets of SLRU
 * buffers.  For concurrency's sake, we avoid holding more than one of these
 * locks at a time.)
 */
typedef struct XMultiXactStateData
{
	/* next-to-be-assigned MultiXactId */
	XMultiXactId nextXMXact;

	/* next-to-be-assigned offset */
	XMultiXactOffset nextOffset;

	/*
	 * Per-backend data starts here.  We have two arrays stored in the area
	 * immediately following the MultiXactStateData struct. Each is indexed by
	 * BackendId.
	 *
	 * In both arrays, there's a slot for all normal backends (1..MaxBackends)
	 * followed by a slot for max_prepared_xacts prepared transactions. Valid
	 * BackendIds start from 1; element zero of each array is never used.
	 *
	 * OldestMemberMXactId[k] is the oldest MultiXactId each backend's current
	 * transaction(s) could possibly be a member of, or InvalidMultiXactId
	 * when the backend has no live transaction that could possibly be a
	 * member of a MultiXact.  Each backend sets its entry to the current
	 * nextMXact counter just before first acquiring a shared lock in a given
	 * transaction, and clears it at transaction end. (This works because only
	 * during or after acquiring a shared lock could an XID possibly become a
	 * member of a MultiXact, and that MultiXact would have to be created
	 * during or after the lock acquisition.)
	 *
	 * OldestVisibleMXactId[k] is the oldest MultiXactId each backend's
	 * current transaction(s) think is potentially live, or InvalidMultiXactId
	 * when not in a transaction or not in a transaction that's paid any
	 * attention to MultiXacts yet.  This is computed when first needed in a
	 * given transaction, and cleared at transaction end.  We can compute it
	 * as the minimum of the valid OldestMemberMXactId[] entries at the time
	 * we compute it (using nextMXact if none are valid).  Each backend is
	 * required not to attempt to access any SLRU data for MultiXactIds older
	 * than its own OldestVisibleMXactId[] setting; this is necessary because
	 * the checkpointer could truncate away such data at any instant.
	 *
	 * The oldest valid value among all of the OldestMemberMXactId[] and
	 * OldestVisibleMXactId[] entries is considered by vacuum as the earliest
	 * possible value still having any live member transaction.  Subtracting
	 * vacuum_multixact_freeze_min_age from that value we obtain the freezing
	 * point for multixacts for that table.  Any value older than that is
	 * removed from tuple headers (or "frozen"; see FreezeMultiXactId.  Note
	 * that multis that have member xids that are older than the cutoff point
	 * for xids must also be frozen, even if the multis themselves are newer
	 * than the multixid cutoff point).  Whenever a full table vacuum happens,
	 * the freezing point so computed is used as the new pg_class.relminmxid
	 * value.  The minimum of all those values in a database is stored as
	 * pg_database.datminmxid.  In turn, the minimum of all of those values is
	 * stored in pg_control and used as truncation point for pg_multixact.  At
	 * checkpoint or restartpoint, unneeded segments are removed.
	 */
	XMultiXactId perBackendXactIds[FLEXIBLE_ARRAY_MEMBER];
} XMultiXactStateData;

/*
 * Definitions for the backend-local XMultiXactId cache.
 *
 * We use this cache to store known XMultiXacts, so we don't need to go to
 * SLRU areas every time.
 *
 * The cache lasts for the duration of a single transaction, the rationale
 * for this being that most entries will contain our own TransactionId and
 * so they will be uninteresting by the time our next transaction starts.
 * (XXX not clear that this is correct --- other members of the XMultiXact
 * could hang around longer than we did.  However, it's not clear what a
 * better policy for flushing old cache entries would be.)	FIXME actually
 * this is plain wrong now that multixact's may contain update Xids.
 *
 * We allocate the cache entries in a memory context that is deleted at
 * transaction end, so we don't need to do retail freeing of entries.
 */
typedef struct XmXactCacheEnt
{
	XMultiXactId multi;
	int			nmembers;
	dlist_node	node;
	XMultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} XmXactCacheEnt;

#endif