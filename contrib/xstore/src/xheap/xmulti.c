/* -------------------------------------------------------------------------
 *
 * xmulti.c
 * the multi xact system for xstore.
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 * src/xheap/xmulti.c
 * -------------------------------------------------------------------------
 */

#include "c.h"
#include "utils/elog.h"
#include "xheap/xmulti.h"
#include "xstore.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xloginsert.h"
#include "access/twophase_rmgr.h"
#include "storage/proc.h"
#include "common/file_perm.h"
#include "miscadmin.h"
#include "port.h"
#include "xheap/xlru.h"
#include <unistd.h>
#include "storage/lmgr.h"
#include "util/xrmgr.h"

#define MaxOldestSlot	(MaxBackends + max_prepared_xacts)
/* Number of SLRU buffers to use for Xmultixact */
#define NUM_XMULTIXACTOFFSET_BUFFERS		8
#define NUM_XMULTIXACTMEMBER_BUFFERS		16

#define debug_elog2(a,b) elog(a,b)
#define debug_elog3(a,b,c) elog(a,b,c)
#define debug_elog4(a,b,c,d) elog(a,b,c,d)
#define debug_elog5(a,b,c,d,e) elog(a,b,c,d,e)
#define debug_elog6(a,b,c,d,e,f) elog(a,b,c,d,e,f)

/*
 * Links to shared-memory data structures for XMultiXact control
 */
static XlruCtlData XMultiXactOffsetCtlData;
static XlruCtlData XMultiXactMemberCtlData;

#define XMultiXactOffsetCtl	(&XMultiXactOffsetCtlData)
#define XMultiXactMemberCtl	(&XMultiXactMemberCtlData)

#define MAX_CACHE_ENTRIES	256
static dlist_head XMXactCache = DLIST_STATIC_INIT(XMXactCache);
static int	XMXactCacheMembers = 0;
static MemoryContext XMXactContext = NULL;

LWLockId XMultiXactGenLock = NULL;
LWLockId XMultiXactOffsetSLRULock = NULL;
LWLockId XMultiXactMemberSLRULock = NULL;
LWLockId XMultiXactTruncationLock = NULL;

static char *XMultiXactGenLockName = "XMultiXactGenLock";
static char *XMultiXactOffsetSLRULockName = "XMultiXactOffsetSLRULock ";
static char *XMultiXactMemberSLRULockName = "XMultiXactMemberSLRULock";
static char *XMultiXactTruncationLockName = "XMultiXactTruncationLock";

static XMultiXactStateData *XMultiXactState;
static XMultiXactId *OldestMemberXMXactId;
static XMultiXactId *OldestVisibleXMXactId;

static bool XMultiXactOffsetPrecedes(XMultiXactOffset offset1, XMultiXactOffset offset2);
static void create_dir_if_needed(char *path);
static bool Do_XMultiXactIdWait(XMultiXactId multi, XMultiXactStatus status, bool nowait);

int XMultiOffsetSyncHandlerPos = -1;
int XMultiMemberSyncHandlerPos = -1;


static const struct
{
	LOCKMODE	hwlock;
	int			lockstatus;
	int			updstatus;
}

			tupleLockExtraInfo[MaxLockTupleMode + 1] =
{
	{							/* LockTupleKeyShare */
		AccessShareLock,
		XMultiXactStatusForKeyShare,
		-1						/* KeyShare does not allow updating tuples */
	},
	{							/* LockTupleShare */
		RowShareLock,
		XMultiXactStatusForShare,
		-1						/* Share does not allow updating tuples */
	},
	{							/* LockTupleNoKeyExclusive */
		ExclusiveLock,
		XMultiXactStatusForNoKeyUpdate,
		XMultiXactStatusNoKeyUpdate
	},
	{							/* LockTupleExclusive */
		AccessExclusiveLock,
		XMultiXactStatusForUpdate,
		XMultiXactStatusUpdate
	}
};

/*
 * This table maps tuple lock strength values for each particular
 * XMultiXactStatus value.
 */
static const int XMultiXactStatusLock[MaxXMultiXactStatus + 1] =
{
	LockTupleKeyShare,			/* ForKeyShare */
	LockTupleShare,				/* ForShare */
	LockTupleNoKeyExclusive,	/* ForNoKeyUpdate */
	LockTupleExclusive,			/* ForUpdate */
	LockTupleNoKeyExclusive,	/* NoKeyUpdate */
	LockTupleExclusive			/* Update */
};

/* Get the LockTupleMode for a given XMultiXactStatus */
#define TUPLOCK_from_xmxstatus(status) \
			(XMultiXactStatusLock[(status)])

/* Get the LOCKMODE for a given MultiXactStatus */
#define LOCKMODE_from_xmxstatus(status) \
			(tupleLockExtraInfo[TUPLOCK_from_xmxstatus((status))].hwlock)

/* internal XMultiXactId management */
static void XMultiXactIdSetOldestVisible(void);
static XMultiXactId CreateXMultiXactId(int nmembers, XMultiXactMember *members);
static void RecordNewXMultiXact(XMultiXactId multi, XMultiXactOffset offset, int nmembers, XMultiXactMember *members);
static XMultiXactId GetNewXMultiXactId(int nxids, XMultiXactOffset *offset);

/* XMultiXact cache management */
static int XMXactMemberComparator(const void *arg1, const void *arg2);
static XMultiXactId XmXactCacheGetBySet(int nmembers, XMultiXactMember *members);
static int XmXactCacheGetById(XMultiXactId multi, XMultiXactMember **members);
static void XmXactCachePut(XMultiXactId multi, int nmembers, XMultiXactMember *members);

static char *xmxid_to_string(XMultiXactId multi, int nmembers, XMultiXactMember *members);
static char *xmxstatus_to_string(XMultiXactStatus status);

/* management of SLRU infrastructure */
static int ZeroXMultiXactOffsetPage(int64 pageno, bool writeXlog);
static int ZeroXMultiXactMemberPage(int64 pageno, bool writeXlog);
static void ExtendXMultiXactOffset(XMultiXactId multi);
static void ExtendXMultiXactMember(XMultiXactOffset offset, int nmembers);
static void out_member(StringInfo buf, XMultiXactMember *member);
static void WriteXMZeroPageXlogRec(int64 pageno, uint8 info);
static bool find_xmultixact_start(XMultiXactId multi, XMultiXactOffset *result);
static void WriteXMTruncateXlogRec(XMultiXactId endTruncOff, XMultiXactOffset endTruncMemb);
static void PerformXMembersTruncation(XMultiXactOffset newOldestOffset);
static void PerformXOffsetsTruncation(XMultiXactId newOldestXMulti);
static bool XlruScanDirCbFindEarliest(XlruCtl ctl, char *filename, int64 segpage, void *data);
static void create_xmulti_dir(void);
static void BootStrapXMultiXact(void);

/*
 * XMultiXactIdCreate
 *		Construct a XMultiXactId representing two FullTransactionIds.
 *
 * The two XIDs must be different, or be requesting different statuses.
 *
 * NB - we don't worry about our local XMultiXactId cache here, because that
 * is handled by the lower-level routines.
 */
XMultiXactId
XMultiXactIdCreate(FullTransactionId xid1, XMultiXactStatus status1,
                   FullTransactionId xid2, XMultiXactStatus status2)
{
    XMultiXactId newMulti;
    XMultiXactMember members[2];

    Assert(FullTransactionIdIsValid(xid1));
    Assert(FullTransactionIdIsValid(xid2));

    Assert(!FullTransactionIdEquals(xid1, xid2) || (status1 != status2));

    /*
     * Note: unlike XMultiXactIdExpand, we don't bother to check that both XIDs
     * are still running.  In typical usage, xid2 will be our own XID and the
     * caller just did a check on xid1, so it'd be wasted effort.
     */
    members[0].xid = xid1;
    members[0].status = status1;
    members[1].xid = xid2;
    members[1].status = status2;

    newMulti = CreateXMultiXactId(2, members);

	debug_elog3(DEBUG2, "Create: %s",
				xmxid_to_string(newMulti, 2, members));

    return newMulti;
}

/*
 * XMultiXactIdExpand
 *		Add a FullTransactionId to a pre-existing XMultiXactId.
 *
 * If the FullTransactionId is already a member of the passed XMultiXactId with the
 * same status, just return it as-is.
 *
 * Note that we do NOT actually modify the membership of a pre-existing
 * XMultiXactId; instead we create a new one.  This is necessary to avoid
 * a race condition against code trying to wait for one XMultiXactId to finish;
 * see notes in heapam.c.
 *
 * NB - we don't worry about our local XMultiXactId cache here, because that
 * is handled by the lower-level routines.
 */
XMultiXactId 
XMultiXactIdExpand(XMultiXactId multi, FullTransactionId xid, XMultiXactStatus status)
{
    XMultiXactId newMulti;
    XMultiXactMember *members = NULL;
    XMultiXactMember *newMembers = NULL;
    int nmembers;
    int i;
    int j;

    Assert(XMultiXactIdIsValid(multi));
    Assert(FullTransactionIdIsValid(xid));
	/* XMultiXactIdSetOldestMember() must have been called already. */
	Assert(XMultiXactIdIsValid(OldestMemberXMXactId[MyProcNumber]));

	debug_elog5(DEBUG2, "Expand: received multi %lu, xid %lu status %s",
				multi.value, xid.value, xmxstatus_to_string(status));

    nmembers = GetXMultiXactIdMembers(multi, &members);
    if (nmembers < 0)
    {
        /*
         * The XMultiXactId is obsolete.  This can only happen if all the
         * XMultiXactId members stop running between the caller checking and
         * passing it to us.  It would be better to return that fact to the
         * caller, but it would complicate the API and it's unlikely to happen
         * too often, so just deal with it by creating a singleton XMultiXact.
         */
        XMultiXactMember member;
        member.xid = xid;
        member.status = status;
        newMulti = CreateXMultiXactId(1, &member);

		debug_elog4(DEBUG2, "Expand: %lu has no members, create singleton %lu",
					multi.value, newMulti.value);
        return newMulti;
    }

    /*
     * If the FullTransactionId is already a member of the XMultiXactId with the
     * same status, just return the existing XMultiXactId.
     */
    for (i = 0; i < nmembers; i++)
    {
        if (FullTransactionIdEquals(members[i].xid, xid) && 
            (members[i].status == status))
        {
			debug_elog4(DEBUG2, "Expand: %lu is already a member of %lu",
						xid.value, multi.value);
            pfree(members);
            members = NULL;
            return multi;
        }
    }

    /*
     * Determine which of the members of the XMultiXactId are still of interest.
     * This is any running transaction, and also any transaction that grabbed
     * something stronger than just a lock and was committed.  (An update that
     * aborted is of no interest here.)
     *
     * (Removing dead members is just an optimization, but a useful one.
     * Note we have the same race condition here as above: j could be 0 at the
     * end of the loop.)
     */
    newMembers = (XMultiXactMember *)palloc(sizeof(XMultiXactMember) * (unsigned)(nmembers + 1));

    for (i = 0, j = 0; i < nmembers; i++)
    {
        if (xstore_transaction_id_is_in_progress(members[i].xid) ||
            (ISUPDATE_from_xmxstatus(members[i].status) && 
            xstore_transaction_id_did_commit(members[i].xid)))
        {
            newMembers[j].xid = members[i].xid;
            newMembers[j++].status = members[i].status;
        }
    }

    newMembers[j].xid = xid;
    newMembers[j++].status = status;
    newMulti = CreateXMultiXactId(j, newMembers);

    pfree(members);
    pfree(newMembers);
    members = NULL;
    newMembers = NULL;

    debug_elog3(DEBUG2, "Expand: returning new multi %lu", newMulti.value);

    return newMulti;
}

/*
 * CreateXMultiXactId
 *		Make a new XMultiXactId
 *
 * Make XLOG, SLRU and cache entries for a new XMultiXactId, recording the
 * given FullTransactionIds as members.  Returns the newly created XMultiXactId.
 *
 * NB: the passed members[] array will be sorted in-place.
 */
static XMultiXactId
CreateXMultiXactId(int nmembers, XMultiXactMember *members)
{
    XMultiXactId multi;
    XMultiXactOffset offset;
    xl_xmultixact_create xlrec;

    debug_elog3(DEBUG2, "Create: %s", 
				xmxid_to_string(InvalidXMultiXactId, nmembers, members));

    /*
     * See if the same set of members already exists in our cache; if so, just
     * re-use that XMultiXactId.  (Note: it might seem that looking in our
     * cache is insufficient, and we ought to search disk to see if a
     * duplicate definition already exists.  But since we only ever create
     * XMultiXacts containing our own XID, in most cases any such XMultiXacts
     * were in fact created by us, and so will be in our cache.  There are
     * corner cases where someone else added us to a XMultiXact without our
     * knowledge, but it's not worth checking for.)
     */
    multi = XmXactCacheGetBySet(nmembers, members);
    if (XMultiXactIdIsValid(multi)) 
    {
        debug_elog2(DEBUG2, "Create: in cache!");
        return multi;
    }

    /* Verify that there is a single update Xid among the given members. */
    {
        int i;
        bool has_update = false;

        for (i = 0; i < nmembers; i++)
        {
            if (ISUPDATE_from_xmxstatus(members[i].status)) 
            {
                if (has_update)
					elog(ERROR, "new xmultixact has more than one updating member: %s",
						 xmxid_to_string(InvalidXMultiXactId, nmembers, members));
                has_update = true;
            }
        }
    }

    /*
     * Assign the MXID and offsets range to use, and make sure there is space
     * in the OFFSETs and MEMBERs files.  NB: this routine does START_CRIT_SECTION().
     */
    multi = GetNewXMultiXactId(nmembers, &offset);

    /*
     * Make an XLOG entry describing the new XMXID.
     *
     * Note: we need not flush this XLOG entry to disk before proceeding. The
     * only way for the XMXID to be referenced from any data page is for
     * heap_lock_tuple() to have put it there, and heap_lock_tuple() generates
     * an XLOG record that must follow ours.  The normal LSN interlock between
     * the data page and that XLOG record will ensure that our XLOG record
     * reaches disk first.	If the SLRU members/offsets data reaches disk
     * sooner than the XLOG record, we do not care because we'll overwrite it
     * with zeroes unless the XLOG record is there too; see notes at top of
     * this file.
     */
    xlrec.mid = multi;
    xlrec.moff = offset;
    xlrec.nmembers = nmembers;

    XLogBeginInsert();
    XLogRegisterData((char *)(&xlrec), SizeOfXMultiXactCreate);
    XLogRegisterData((char *)members, (unsigned)nmembers * sizeof(XMultiXactMember));

    (void)XLogInsert(RM_XMULTIXACT_ID, XLOG_XMULTIXACT_CREATE_ID);

    /* Now enter the information into the OFFSETs and MEMBERs logs */
    RecordNewXMultiXact(multi, offset, nmembers, members);

    /* Done with critical section */
    END_CRIT_SECTION();

    /* Store the new XMultiXactId in the local cache, too */
    XmXactCachePut(multi, nmembers, members);

    ereport(DEBUG2, (errmsg("Create: all done")));

    return multi;
}


/*
 * RecordNewXMultiXact
 *		Write info about a new Xmultixact into the offsets and members files
 *
 * This is broken out of CreateXMultiXactId so that xlog replay can use it.
 */
static void 
RecordNewXMultiXact(XMultiXactId multi, XMultiXactOffset offset, int nmembers, XMultiXactMember *members)
{
    int64 pageno;
    int64 prev_pageno;
    int entryno;
    int slotno;
    XMultiXactOffset *offptr = NULL;
    int i;

    LWLockAcquire(XMultiXactOffsetSLRULock, LW_EXCLUSIVE);

    pageno = (int64)XMultiXactIdToOffsetPage(multi);
    entryno = XMultiXactIdToOffsetEntry(multi);

    /*
     * Note: we pass the XMultiXactId to SimpleLruReadPage as the "transaction"
     * to complain about if there's any I/O error.  This is kinda bogus, but
     * since the errors will always give the full pathname, it should be clear
     * enough that a XMultiXactId is really involved.  Perhaps someday we'll
     * take the trouble to generalize the slru.c error reporting code.
     */
    slotno = XlruReadPage(XMultiXactOffsetCtl, pageno, true, multi);
    offptr = (XMultiXactOffset *)XMultiXactOffsetCtl->shared->page_buffer[slotno];
    offptr += entryno;

    *offptr = offset;

    XMultiXactOffsetCtl->shared->page_dirty[slotno] = true;

    /* Exchange our lock */
    LWLockRelease(XMultiXactOffsetSLRULock);

    LWLockAcquire(XMultiXactMemberSLRULock, LW_EXCLUSIVE);

    prev_pageno = -1;

    for (i = 0; i < nmembers; i++, offset++)
    {
		FullTransactionId *memberptr;
		uint32	   *flagsptr;
		uint32		flagsval;
		int			bshift;
		int			flagsoff;
		int			memberoff;

		Assert(members[i].status <= XMultiXactStatusUpdate);

		pageno = XMXOffsetToMemberPage(offset);
		memberoff = XMXOffsetToMemberOffset(offset);
		flagsoff = XMXOffsetToFlagsOffset(offset);
		bshift = XMXOffsetToFlagsBitShift(offset);

        if (pageno != prev_pageno)
        {
            slotno = XlruReadPage(XMultiXactMemberCtl, pageno, true, multi);
            prev_pageno = pageno;
        }
		memberptr = (FullTransactionId *)
			(XMultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		*memberptr = members[i].xid;

		flagsptr = (uint32 *)
			(XMultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		flagsval = *flagsptr;
        /* Clear the flag for the specified member */
		flagsval &= ~(((1 << XMXACT_MEMBER_BITS_PER_XACT) - 1) << bshift);
        /* add the corresponding flag for the specified member */
		flagsval |= (members[i].status << bshift);
		*flagsptr = flagsval;

		XMultiXactMemberCtl->shared->page_dirty[slotno] = true;
    }

    LWLockRelease(XMultiXactMemberSLRULock);
}

/*
 * GetNewXMultiXactId
 *		Get the next XMultiXactId.
 *
 * Also, reserve the needed amount of space in the "members" area.	The
 * starting offset of the reserved space is returned in *offset.
 *
 * This may generate XLOG records for expansion of the offsets and/or members
 * files.  Unfortunately, we have to do that while holding XMultiXactGenLock
 * to avoid race conditions --- the XLOG record for zeroing a page must appear
 * before any backend can possibly try to store data in that page!
 *
 * We start a critical section before advancing the shared counters.  The
 * caller must end the critical section after writing SLRU data.
 */
static XMultiXactId 
GetNewXMultiXactId(int nmembers, XMultiXactOffset *offset)
{
    XMultiXactId result;
    XMultiXactOffset nextOffset;

    debug_elog3(DEBUG2, "GetNew: for %d xids", nmembers);

	/* safety check, we should never get this far in a HS standby */
	if (RecoveryInProgress())
		elog(ERROR, "cannot assign XMultiXactIds during recovery");

    LWLockAcquire(XMultiXactGenLock, LW_EXCLUSIVE);

    /* Handle nextMXact first init value is 0 */
    if (XMultiXactState->nextXMXact.value < FirstXMultiXactId.value)
        XMultiXactState->nextXMXact.value = FirstXMultiXactId.value;

    /*
     * Assign the MXID, and make sure there is room for it in the file.
     */
    result = XMultiXactState->nextXMXact;

    ExtendXMultiXactOffset(result);

    /*
     * Reserve the members space, similarly to above.  Also, be careful not to
     * return zero as the starting offset for any Xmultixact. See
     * GetXMultiXactIdMembers() for motivation.
     */
    nextOffset = XMultiXactState->nextOffset;
    if (nextOffset == 0)
    {
        *offset = 1;
        nmembers++; /* allocate member slot 0 too */
    } 
    else
        *offset = nextOffset;

    ExtendXMultiXactMember(nextOffset, nmembers);

    /*
     * Critical section from here until caller has written the data into the
     * just-reserved SLRU space; we don't want to error out with a partly
     * written XMultiXact structure.  (In particular, failing to write our
     * start offset after advancing nextMXact would effectively corrupt the
     * previous XMultiXact.)
     */
    START_CRIT_SECTION();

    /*
     * Advance counters.  As in GetNewTransactionId(), this must not happen
     * until after file extension has succeeded!
     *
     * Note that nextMXact may be InvalidXMultiXactId after this routine exits,
     * so anyone else looking at the variable must be prepared to deal with that.
     * Similarly, nextOffset may be zero, but we won't use that as the
     * actual start offset of the next Xmultixact.
     */
    (XMultiXactState->nextXMXact.value)++;

    XMultiXactState->nextOffset += nmembers;

    LWLockRelease(XMultiXactGenLock);

    debug_elog4(DEBUG2, "GetNew: returning %lu offset %lu", result.value, *offset);
    return result;
}


/*
 * GetXMultiXactIdMembers
 *      Returns the set of XMultiXactMembers that make up a XMultiXactId
 *
 * If the given XMultiXactId is older than the value we know to be oldest, we
 * return -1.
 *
 * Other border conditions, such as trying to read a value that's larger than
 * the value currently known as the next to assign, raise an error.  Previously
 * these also returned -1, but since this can lead to the wrong visibility
 * results, it is dangerous to do that.
 */
int GetXMultiXactIdMembers(XMultiXactId multi, XMultiXactMember **members)
{
    int64 pageno;
    int64 prev_pageno;
    int entryno;
    int slotno;
    XMultiXactOffset *offptr = NULL;
    XMultiXactOffset offset;
    int length;
    int truelength;
    int i;
    XMultiXactId nextMXact;
    XMultiXactId tmpMXact;
    XMultiXactOffset nextOffset;
    XMultiXactMember *ptr = NULL;
    XMultiXactId oldestMXact;


    Assert(XMultiXactIdIsValid(multi));

    /* See if the XMultiXactId is in the local cache */
    length = XmXactCacheGetById(multi, members);
    if (length >= 0)
    {
        debug_elog3(DEBUG2, "GetMembers: found %s in the cache", xmxid_to_string(multi, length, *members));
        return length;
    }

    /* Set our OldestVisibleXMXactId[] entry if we didn't already */

    XMultiXactIdSetOldestVisible();

    oldestMXact = OldestVisibleXMXactId[MyProcNumber];

    /*
     * We check known limits on XMultiXact before resorting to the SLRU area.
     *
     * An ID >= nextMXact shouldn't ever be seen here;
     *
     * Shared lock is enough here since we aren't modifying any global state.
     * Acquire it just long enough to grab the current counter values.	We may
     * need both nextMXact and nextOffset; see below.
     */
    LWLockAcquire(XMultiXactGenLock, LW_SHARED);

    nextMXact = XMultiXactState->nextXMXact;
    nextOffset = XMultiXactState->nextOffset;

    LWLockRelease(XMultiXactGenLock);

    if (XMultiXactIdPrecedes(multi, oldestMXact)) {
        *members = NULL;
        return -1;
    }

    if (!XMultiXactIdPrecedes(multi, nextMXact))
    {
        ereport(PANIC, (errmsg("XMultiXactId %lu has not been created yet", multi.value)));
        *members = NULL;
        return -1;
    }

    /*
     * Find out the offset at which we need to start reading XMultiXactMembers
     * and the number of members in the Xmultixact.	We determine the latter as
     * the difference between this Xmultixact's starting offset and the next
     * one's.  However, there are some corner cases to worry about:
     *
     * 1. This Xmultixact may be the latest one created, in which case there is
     * no next one to look at.	In this case the nextOffset value we just
     * saved is the correct endpoint.
     *
     * 2. The next Xmultixact may still be in process of being filled in: that
     * is, another process may have done GetNewXMultiXactId but not yet written
     * the offset entry for that ID.  In that scenario, it is guaranteed that
     * the offset entry for that Xmultixact exists (because GetNewXMultiXactId
     * won't release XMultiXactGenLock until it does) but contains zero
     * (because we are careful to pre-zero offset pages). Because
     * GetNewXMultiXactId will never return zero as the starting offset for a
     * Xmultixact, when we read zero as the next Xmultixact's offset, we know we
     * have this case.	We sleep for a bit and try again.
     *
     * 3. Because GetNewXMultiXactId increments offset zero to offset one to
     * handle case #2, there is an ambiguity near the point of offset
     * wraparound. If we see next Xmultixact's offset is one, is that our Xmultixact's actual
     * endpoint, or did it end at zero with a subsequent increment? We
     * handle this using the knowledge that if the zero'th member slot wasn't
     * filled, it'll contain zero, and zero isn't a valid transaction ID so it can't
     * be a Xmultixact member.  Therefore, if we read a zero from the
     * members array, just ignore it.
     *
     * This is all pretty messy, but the mess occurs only in infrequent corner
     * cases, so it seems better than holding the XMultiXactGenLock for a long
     * time on every Xmultixact creation.
     */
retry:
    LWLockAcquire(XMultiXactOffsetSLRULock, LW_EXCLUSIVE);

    pageno = XMultiXactIdToOffsetPage(multi);
    entryno = XMultiXactIdToOffsetEntry(multi);

	slotno = XlruReadPage(XMultiXactOffsetCtl, pageno, true, multi);
	offptr = (XMultiXactOffset *) XMultiXactOffsetCtl->shared->page_buffer[slotno];
    offptr += entryno;
    offset = *offptr;

    Assert(offset != 0);

    /* Use the same increment rule as GetNewXMultiXactId() */
    tmpMXact.value = multi.value + 1;

    if(XMultiXactIdEquals(nextMXact, tmpMXact))
    {
        /* Corner case 1: there is no next Xmultixact */
        length = nextOffset - offset;
    } 
    else
    {
        XMultiXactOffset nextMXOffset;

		/* handle wraparound if needed */
		if (tmpMXact.value < FirstXMultiXactId.value)
			tmpMXact.value = FirstXMultiXactId.value;

        prev_pageno = pageno;
        pageno = (int64)XMultiXactIdToOffsetPage(tmpMXact);
        entryno = XMultiXactIdToOffsetEntry(tmpMXact);

        if (pageno != prev_pageno)
            slotno = XlruReadPage(XMultiXactOffsetCtl, pageno, true, tmpMXact);

        offptr = (XMultiXactOffset *)XMultiXactOffsetCtl->shared->page_buffer[slotno];
        offptr += entryno;
        nextMXOffset = *offptr;

        if (nextMXOffset == 0)
        {
            /* Corner case 2: next xmultixact is still being filled in */
            LWLockRelease(XMultiXactOffsetSLRULock);
            pg_usleep(1000L);
            goto retry;
        }

        length = nextMXOffset - offset;
    }

    LWLockRelease(XMultiXactOffsetSLRULock);

    ptr = (XMultiXactMember *)palloc((unsigned)length * sizeof(XMultiXactMember));

    /* Now get the members themselves. */
    LWLockAcquire(XMultiXactMemberSLRULock, LW_EXCLUSIVE);

    truelength = 0;
    prev_pageno = -1;
    for (i = 0; i < length; i++, offset++)
    {
		FullTransactionId *xactptr;
		uint32	   *flagsptr;
		int			flagsoff;
		int			bshift;
		int			memberoff;

		pageno = XMXOffsetToMemberPage(offset);
		memberoff = XMXOffsetToMemberOffset(offset);

		if (pageno != prev_pageno)
		{
			slotno = XlruReadPage(XMultiXactMemberCtl, pageno, true, multi);
			prev_pageno = pageno;
		}

		xactptr = (FullTransactionId *)
			(XMultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		if (!FullTransactionIdIsValid(*xactptr))
		{
			/* Corner case 3: we must be looking at unused slot zero */
			Assert(offset == 0);
			continue;
		}

		flagsoff = XMXOffsetToFlagsOffset(offset);
		bshift = XMXOffsetToFlagsBitShift(offset);
		flagsptr = (uint32 *) (XMultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		ptr[truelength].xid = *xactptr;
		ptr[truelength].status = (*flagsptr >> bshift) & XMXACT_MEMBER_XACT_BITMASK;
		truelength++;
    }

    LWLockRelease(XMultiXactMemberSLRULock);

	/* A xmultixid with zero members should not happen */
	Assert(truelength > 0);

    /*
     * Copy the result into the local cache.
     */
    XmXactCachePut(multi, truelength, ptr);

	debug_elog3(DEBUG2, "GetMembers: no cache for %s",
				xmxid_to_string(multi, truelength, ptr));
    *members = ptr;
    return truelength;
}

/*
 * XMXactMemberComparator
 *      qsort comparison function for XMultiXactMember
 */
static int XMXactMemberComparator(const void *arg1, const void *arg2)
{
    XMultiXactMember member1 = *(const XMultiXactMember *)arg1;
    XMultiXactMember member2 = *(const XMultiXactMember *)arg2;

    if (member1.xid.value > member2.xid.value)
    {
        return 1;
    }
    if (member1.xid.value < member2.xid.value)
    {
        return -1;
    }

	if (member1.status > member2.status)
		return 1;
	if (member1.status < member2.status)
		return -1;
    return 0;
}


/*
 * XmXactCacheGetBySet
 *		returns a XMultiXactId from the cache based on the set of
 *		TransactionIds that compose it, or InvalidXMultiXactId if
 *		none matches.
 *
 * This is helpful, for example, if two transactions want to lock a huge
 * table.  By using the cache, the second will use the same XMultiXactId
 * for the majority of tuples, thus keeping XMultiXactId usage low (saving
 * both I/O).
 *
 * NB: the passed members[] array will be sorted in-place.
 */
static XMultiXactId XmXactCacheGetBySet(int nmembers, XMultiXactMember *members)
{
	dlist_iter	iter;

	debug_elog3(DEBUG2, "CacheGet: looking for %s",
				xmxid_to_string(InvalidXMultiXactId, nmembers, members));
    /* sort the array so comparison is easy */
    qsort(members, nmembers, sizeof(XMultiXactMember), XMXactMemberComparator);

	dlist_foreach(iter, &XMXactCache)
	{
		XmXactCacheEnt *entry = dlist_container(XmXactCacheEnt, node, iter.cur);

		if (entry->nmembers != nmembers)
			continue;

		/*
		 * We assume the cache entries are sorted, and that the unused bits in
		 * "status" are zeroed.
		 */
		if (memcmp(members, entry->members, nmembers * sizeof(XMultiXactMember)) == 0)
		{
			debug_elog3(DEBUG2, "CacheGet: found %lu", (entry->multi).value);
			dlist_move_head(&XMXactCache, iter.cur);
			return entry->multi;
		}
	}

	debug_elog2(DEBUG2, "CacheGet: not found :-(");
    return InvalidXMultiXactId;
}

/*
 * XmXactCacheGetById
 *		returns the composing XMultiXactMember set from the cache for a
 *		given XMultiXactId, if present.
 *
 * If successful, *members is set to the address of a palloc'd copy of the
 * XMultiXactMember set.  Return value is number of members, or -1 on failure.
 */
static int XmXactCacheGetById(XMultiXactId multi, XMultiXactMember **members)
{
	dlist_iter	iter;

	debug_elog3(DEBUG2, "CacheGet: looking for %lu", multi.value);

	dlist_foreach(iter, &XMXactCache)
	{
		XmXactCacheEnt *entry = dlist_container(XmXactCacheEnt, node, iter.cur);

        if(XMultiXactIdEquals(entry->multi, multi))
		{
			XMultiXactMember *ptr;
			Size		size;

			size = sizeof(XMultiXactMember) * entry->nmembers;
			ptr = (XMultiXactMember *) palloc(size);

			memcpy(ptr, entry->members, size);

			debug_elog3(DEBUG2, "CacheGet: found %s",
						xmxid_to_string(multi,
									   entry->nmembers,
									   entry->members));

			/*
			 * Note we modify the list while not using a modifiable iterator.
			 * This is acceptable only because we exit the iteration
			 * immediately afterwards.
			 */
			dlist_move_head(&XMXactCache, iter.cur);

			*members = ptr;
			return entry->nmembers;
		}
	}

	debug_elog2(DEBUG2, "CacheGet: not found");
	return -1;
}

/*
 * XmXactCachePut
 *		Add a new XMultiXactId and its composing set into the local cache.
 */
static void XmXactCachePut(XMultiXactId multi, int nmembers, XMultiXactMember *members)
{
	XmXactCacheEnt *entry;

	debug_elog3(DEBUG2, "CachePut: storing %s",
				xmxid_to_string(multi, nmembers, members));

	if (XMXactContext == NULL)
	{
		/* The cache only lives as long as the current transaction */
		debug_elog2(DEBUG2, "CachePut: initializing memory context");
		XMXactContext = AllocSetContextCreate(TopTransactionContext,
											 "XMultiXact cache context",
											 ALLOCSET_SMALL_SIZES);
	}

	entry = (XmXactCacheEnt *)
		MemoryContextAlloc(XMXactContext,
						   offsetof(XmXactCacheEnt, members) +
						   nmembers * sizeof(XMultiXactMember));

	(entry->multi).value = multi.value;
	entry->nmembers = nmembers;
	memcpy(entry->members, members, nmembers * sizeof(XMultiXactMember));

	/* XmXactCacheGetBySet assumes the entries are sorted, so sort them */
	qsort(entry->members, nmembers, sizeof(XMultiXactMember), XMXactMemberComparator);

	dlist_push_head(&XMXactCache, &entry->node);
	if (XMXactCacheMembers++ >= MAX_CACHE_ENTRIES)
	{
		dlist_node *node;

		node = dlist_tail_node(&XMXactCache);
		dlist_delete(node);
		XMXactCacheMembers--;

		entry = dlist_container(XmXactCacheEnt, node, node);
		debug_elog3(DEBUG2, "CachePut: pruning cached multi %lu",
					(entry->multi).value);

		pfree(entry);
	}
}

static char *
xmxstatus_to_string(XMultiXactStatus status)
{
    switch (status)
    {
        case XMultiXactStatusForKeyShare:
            return "keysh";
        case XMultiXactStatusForShare:
            return "sh";
        case XMultiXactStatusForNoKeyUpdate:
            return "fornokeyupd";
        case XMultiXactStatusForUpdate:
            return "forupd";
        case XMultiXactStatusNoKeyUpdate:
            return "nokeyupd";
        case XMultiXactStatusUpdate:
            return "upd";
        default:
            elog(ERROR, "unrecognized Xmultixact status %d", (int)status);
            return "";
	}
}

static char *
xmxid_to_string(XMultiXactId multi, int nmembers, XMultiXactMember *members)
{
	static char *str = NULL;
	StringInfoData buf;
	int			i;

	if (str != NULL)
		pfree(str);

	initStringInfo(&buf);

	appendStringInfo(&buf, "%lu %d[%lu (%s)", multi.value, nmembers, members[0].xid.value,
					 xmxstatus_to_string(members[0].status));

	for (i = 1; i < nmembers; i++)
		appendStringInfo(&buf, ", %lu (%s)", members[i].xid.value,
						 xmxstatus_to_string(members[i].status));

	appendStringInfoChar(&buf, ']');
	str = MemoryContextStrdup(TopMemoryContext, buf.data);
	pfree(buf.data);
	return str;
}

/*
 * Initialize (or reinitialize) a page of XMultiXactOffset to zeroes.
 * If writeXlog is TRUE, also emit an XLOG record saying we did this.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroXMultiXactOffsetPage(int64 pageno, bool writeXlog)
{
    int slotno;

    slotno = XlruZeroPage(XMultiXactOffsetCtl, pageno);

    if (writeXlog)
        WriteXMZeroPageXlogRec(pageno, XLOG_XMULTIXACT_ZERO_OFF_PAGE);

    return slotno;
}

/*
 * Ditto, for XMultiXactMember
 */
static int
ZeroXMultiXactMemberPage(int64 pageno, bool writeXlog)
{
    int slotno;
    slotno = XlruZeroPage(XMultiXactMemberCtl, pageno);

    if (writeXlog)
        WriteXMZeroPageXlogRec(pageno, XLOG_XMULTIXACT_ZERO_MEM_PAGE);

    return slotno;
}

/*
 * Make sure that XMultiXactOffset has room for a newly-allocated XMultiXactId.
 *
 * NB: this is called while holding XMultiXactGenLock.  We want it to be very
 * fast most of the time; even when it's not so fast, no actual I/O need
 * happen unless we're forced to write out a dirty log or xlog page to make
 * room in shared memory.
 */
static void
ExtendXMultiXactOffset(XMultiXactId multi)
{
    int64 pageno;

    /*
     * No work except at first XMultiXactId of a page.
     */
    if (XMultiXactIdToOffsetEntry(multi) != 0 && !XMultiXactIdEquals(multi, FirstXMultiXactId))
        return;

    pageno = (int64)XMultiXactIdToOffsetPage(multi);

    LWLockAcquire(XMultiXactOffsetSLRULock, LW_EXCLUSIVE);

    /* Zero the page and make an XLOG entry about it */
    ZeroXMultiXactOffsetPage(pageno, true);

    LWLockRelease(XMultiXactOffsetSLRULock);
}

/*
 * Make sure that XMultiXactMember has room for the members of a newly-
 * allocated XMultiXactId.
 *
 * Like the above routine, this is called while holding XMultiXactGenLock;
 * same comments apply.
 */
static void
ExtendXMultiXactMember(XMultiXactOffset offset, int nmembers)
{
    /*
     * It's possible that the members span more than one page of the members
     * file, so we loop to ensure we consider each page.  The coding is not
     * optimal if the members span several pages, but that seems unusual
     * enough to not worry much about.
     */
    while (nmembers > 0)
    {
		int			flagsoff;
		int			flagsbit;
		uint32		difference;
        /*
         * Only zero when at first entry of a page.
         */
		flagsoff = XMXOffsetToFlagsOffset(offset);
		flagsbit = XMXOffsetToFlagsBitShift(offset);
        if (flagsoff == 0 && flagsbit == 0)
        {
            int64 pageno;

            pageno = (int64)XMXOffsetToMemberPage(offset);

            LWLockAcquire(XMultiXactMemberSLRULock, LW_EXCLUSIVE);

            /* Zero the page and make an XLOG entry about it */
            ZeroXMultiXactMemberPage(pageno, true);

            LWLockRelease(XMultiXactMemberSLRULock);
        }
		
		difference = XMULTIXACT_MEMBERS_PER_PAGE - offset % XMULTIXACT_MEMBERS_PER_PAGE;

		/*
		 * Advance to next page, taking care to properly handle the wraparound
		 * case.  OK if nmembers goes negative.
		 */
		nmembers -= difference;
		offset += difference;
    }
}

Size
XMultiShmemSize(void)
{
	Size		size;

	/* We need 2*MaxOldestSlot + 1 perBackendXactIds[] entries */
#define SHARED_XMULTIXACT_STATE_SIZE \
	add_size(offsetof(XMultiXactStateData, perBackendXactIds) + sizeof(XMultiXactId), \
			 mul_size(sizeof(XMultiXactId) * 2, MaxOldestSlot))

	size = SHARED_XMULTIXACT_STATE_SIZE;
	size = add_size(size, XlruShmemSize(NUM_XMULTIXACTOFFSET_BUFFERS, 0));
	size = add_size(size, XlruShmemSize(NUM_XMULTIXACTMEMBER_BUFFERS, 0));

	return size;
}

void
XMultiRequestNamedLWLockTranche(void)
{
    RequestNamedLWLockTranche(XMultiXactGenLockName, 1);
    RequestNamedLWLockTranche(XMultiXactOffsetSLRULockName, 1);
    RequestNamedLWLockTranche(XMultiXactMemberSLRULockName, 1);
    RequestNamedLWLockTranche(XMultiXactTruncationLockName, 1);
}

void
XMultiXactShmemInit(Pointer ptr, bool found)
{
	XMultiXactStateData *state;


	state =  (XMultiXactStateData *) ptr;
	if (!found)
	{
		int offsetBufferLockTranche;
		int memberBufferLockTranche;

		debug_elog2(DEBUG2, "Shared Memory Init for XMultiXact");

		XMultiXactGenLock = &(GetNamedLWLockTranche(XMultiXactGenLockName)->lock);
		XMultiXactOffsetSLRULock = &(GetNamedLWLockTranche(XMultiXactOffsetSLRULockName)->lock);
		XMultiXactMemberSLRULock = &(GetNamedLWLockTranche(XMultiXactMemberSLRULockName)->lock);
		XMultiXactTruncationLock = &(GetNamedLWLockTranche(XMultiXactTruncationLockName)->lock);

		offsetBufferLockTranche = LWLockNewTrancheId();
		memberBufferLockTranche = LWLockNewTrancheId();

		LWLockRegisterTranche(offsetBufferLockTranche, "XMultiXactOffsetBuffer");
		LWLockRegisterTranche(memberBufferLockTranche, "XMultiXactMemberBuffer");

		XlruInit(XMultiXactOffsetCtl,
					"XMultiXactOffset", NUM_XMULTIXACTOFFSET_BUFFERS, 0,
					XMultiXactOffsetSLRULock, "pg_xmultixact/offsets",
					offsetBufferLockTranche,
					XMultiOffsetSyncHandlerPos);
		XlruInit(XMultiXactMemberCtl,
					"XMultiXactMember", NUM_XMULTIXACTMEMBER_BUFFERS, 0,
					XMultiXactMemberSLRULock, "pg_xmultixact/members",
					memberBufferLockTranche,
					XMultiMemberSyncHandlerPos);

		/*
		* Set up array pointers.  Note that perBackendXactIds[0] is wasted space
		* since we only use indexes 1..MaxOldestSlot in each array.
		*/
		OldestMemberXMXactId = state->perBackendXactIds;
		OldestVisibleXMXactId = OldestMemberXMXactId + MaxOldestSlot;
	}
	XMultiXactState = state;
}

/*
 * Decide which of two XMultiXactIds is earlier.
 *
 * XXX do we need to do something special for XInvalidMultiXactId?
 * (Doesn't look like it.)
 */
bool
XMultiXactIdPrecedes(XMultiXactId multi1, XMultiXactId multi2)
{
	return multi1.value < multi2.value;
}

/*
 * XMultiXactIdPrecedesOrEquals -- is multi1 logically <= multi2?
 *
 * XXX do we need to do something special for XInvalidMultiXactId?
 * (Doesn't look like it.)
 */
bool
XMultiXactIdPrecedesOrEquals(XMultiXactId multi1, XMultiXactId multi2)
{
	return multi1.value <= multi2.value;
}


/*
 * Decide which of two offsets is earlier.
 */
static bool
XMultiXactOffsetPrecedes(XMultiXactOffset offset1, XMultiXactOffset offset2)
{
	return offset1 < offset2;
}

/*
 * Entrypoint for sync.c to sync offsets files.
 */
int
xmultixactoffsetssyncfiletag(const FileTag *ftag, char *path)
{
	return XlruSyncFileTag(XMultiXactOffsetCtl, ftag, path);
}

/*
 * Entrypoint for sync.c to sync members files.
 */
int
xmultixactmemberssyncfiletag(const FileTag *ftag, char *path)
{
	return XlruSyncFileTag(XMultiXactMemberCtl, ftag, path);
}

void
create_xmulti_dir()
{
	char *path;

	path = psprintf("%s/%s", DataDir, "pg_xmultixact");
	create_dir_if_needed(path);

	path = psprintf("%s/%s", DataDir, "pg_xmultixact/offsets");
	create_dir_if_needed(path);

	path = psprintf("%s/%s", DataDir, "pg_xmultixact/members");
	create_dir_if_needed(path);
}

static void
create_dir_if_needed(char *path)
{
	int ret;

	ret = pg_mkdir_p(path, pg_dir_create_mode);

	if (ret != 0 && ret != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("xstore can't found or create directory \"%s\": %m",
						path)));
}

/*
 * MULTIXACT resource manager's routines
 */
void
xmultixact_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in multixact records */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_XMULTIXACT_ZERO_OFF_PAGE)
	{
		int64			pageno;
		int				slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int64));

		LWLockAcquire(XMultiXactOffsetSLRULock, LW_EXCLUSIVE);

		slotno = ZeroXMultiXactOffsetPage(pageno, false);
		XlruWritePage(XMultiXactOffsetCtl, slotno);
		Assert(!XMultiXactOffsetCtl->shared->page_dirty[slotno]);

		LWLockRelease(XMultiXactOffsetSLRULock);
	}
	else if (info == XLOG_XMULTIXACT_ZERO_MEM_PAGE)
	{
		int64			pageno;
		int				slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int64));

		LWLockAcquire(XMultiXactMemberSLRULock, LW_EXCLUSIVE);

		slotno = ZeroXMultiXactMemberPage(pageno, false);
		XlruWritePage(XMultiXactMemberCtl, slotno);
		Assert(!XMultiXactMemberCtl->shared->page_dirty[slotno]);

		LWLockRelease(XMultiXactMemberSLRULock);
	}
	else if (info == XLOG_XMULTIXACT_CREATE_ID)
	{
		xl_xmultixact_create *xlrec =
		(xl_xmultixact_create *) XLogRecGetData(record);
		TransactionId max_xid;
		int			i;

		/* Store the data back into the XLRU files */
		RecordNewXMultiXact(xlrec->mid, xlrec->moff, xlrec->nmembers,
						   xlrec->members);

		/* Make sure nextMXact/nextOffset are beyond what this record has */
		XMultiXactAdvanceNextXMXact(FullTransactionIdFromU64(xlrec->mid.value + 1),
								  xlrec->moff + xlrec->nmembers);

		/*
		 * Make sure nextXid is beyond any XID mentioned in the record. This
		 * should be unnecessary, since any XID found here ought to have other
		 * evidence in the XLOG, but let's be safe.
		 */
		max_xid = XLogRecGetXid(record);
		for (i = 0; i < xlrec->nmembers; i++)
		{
			if (TransactionIdPrecedes(max_xid, XidFromFullTransactionId(xlrec->members[i].xid)))
				max_xid = XidFromFullTransactionId(xlrec->members[i].xid);
		}

		AdvanceNextFullTransactionIdPastXid(max_xid);
	}
	else if (info == XLOG_XMULTIXACT_TRUNCATE_ID)
	{
		xl_xmultixact_truncate xlrec;
		int64			pageno;

		memcpy(&xlrec, XLogRecGetData(record),
			   SizeOfXMultiXactTruncate);

		elog(DEBUG1, "replaying xmultixact truncation to: "
			 "offsets %lu, offsets segments %lx, "
			 "members %lu, members segments %lx",
			 xlrec.endTruncOff.value, XMultiXactIdToOffsetSegment(xlrec.endTruncOff),
			 xlrec.endTruncMemb, XMXOffsetToMemberSegment(xlrec.endTruncMemb));

		/* should not be required, but more than cheap enough */
		LWLockAcquire(XMultiXactTruncationLock, LW_EXCLUSIVE);
		
		PerformXMembersTruncation(xlrec.endTruncMemb);

		/*
		 * During XLOG replay, latest_page_number isn't necessarily set up
		 * yet; insert a suitable value to bypass the sanity test in
		 * SimpleLruTruncate.
		 */
		pageno = XMultiXactIdToOffsetPage(xlrec.endTruncOff);
		XMultiXactOffsetCtl->shared->latest_page_number = pageno;
		PerformXOffsetsTruncation(xlrec.endTruncOff);

		LWLockRelease(XMultiXactTruncationLock);
	}
	else
		elog(PANIC, "multixact_redo: unknown op code %u", info);
}

void
xmultixact_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_XMULTIXACT_ZERO_OFF_PAGE ||
		info == XLOG_XMULTIXACT_ZERO_MEM_PAGE)
	{
		int64			pageno;

		memcpy(&pageno, rec, sizeof(int64));
		appendStringInfo(buf, "%lu", pageno);
	}
	else if (info == XLOG_XMULTIXACT_CREATE_ID)
	{
		xl_xmultixact_create *xlrec = (xl_xmultixact_create *) rec;
		int			i;

		appendStringInfo(buf, "%lu offset %lu nmembers %d: ", xlrec->mid.value,
						 xlrec->moff, xlrec->nmembers);
		for (i = 0; i < xlrec->nmembers; i++)
			out_member(buf, &xlrec->members[i]);
	}
	else if (info == XLOG_XMULTIXACT_TRUNCATE_ID)
	{
		xl_xmultixact_truncate *xlrec = (xl_xmultixact_truncate *) rec;

		appendStringInfo(buf, "offsets %lu, members %lu",
						 xlrec->endTruncOff.value, xlrec->endTruncMemb);
	}
}


static void
out_member(StringInfo buf, XMultiXactMember *member)
{
	appendStringInfo(buf, "%lu ", member->xid.value);
	switch (member->status)
	{
		case XMultiXactStatusForKeyShare:
			appendStringInfoString(buf, "(keysh) ");
			break;
		case XMultiXactStatusForShare:
			appendStringInfoString(buf, "(sh) ");
			break;
		case XMultiXactStatusForNoKeyUpdate:
			appendStringInfoString(buf, "(fornokeyupd) ");
			break;
		case XMultiXactStatusForUpdate:
			appendStringInfoString(buf, "(forupd) ");
			break;
		case XMultiXactStatusNoKeyUpdate:
			appendStringInfoString(buf, "(nokeyupd) ");
			break;
		case XMultiXactStatusUpdate:
			appendStringInfoString(buf, "(upd) ");
			break;
		default:
			appendStringInfoString(buf, "(unk) ");
			break;
	}
}

const char *
xmultixact_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_XMULTIXACT_ZERO_OFF_PAGE:
			id = "ZERO_OFF_PAGE";
			break;
		case XLOG_XMULTIXACT_ZERO_MEM_PAGE:
			id = "ZERO_MEM_PAGE";
			break;
		case XLOG_XMULTIXACT_CREATE_ID:
			id = "CREATE_ID";
			break;
		case XLOG_XMULTIXACT_TRUNCATE_ID:
			id = "TRUNCATE_ID";
			break;
	}

	return id;
}

static void
WriteXMZeroPageXlogRec(int64 pageno, uint8 info)
{
	XLogBeginInsert();
	XLogRegisterData((char *) (&pageno), sizeof(int64));
	(void) XLogInsert(RM_XMULTIXACT_ID, info);
}


/*
 * Ensure the next-to-be-assigned MultiXactId is at least minMulti,
 * and similarly nextOffset is at least minMultiOffset.
 *
 * This is used when we can determine minimum safe values from an XLog
 * record (either an on-line checkpoint or an mxact creation log entry).
 * Although this is only called during XLog replay, we take the lock in case
 * any hot-standby backends are examining the values.
 */
void
XMultiXactAdvanceNextXMXact(XMultiXactId minMulti,
						    XMultiXactOffset minMultiOffset)
{
	LWLockAcquire(XMultiXactGenLock, LW_EXCLUSIVE);
	if (XMultiXactIdPrecedes(XMultiXactState->nextXMXact, minMulti))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next multi to %lu", minMulti.value);
		XMultiXactState->nextXMXact = minMulti;
	}
	if (XMultiXactOffsetPrecedes(XMultiXactState->nextOffset, minMultiOffset))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next offset to %lu",
					minMultiOffset);
		XMultiXactState->nextOffset = minMultiOffset;
	}
	LWLockRelease(XMultiXactGenLock);
}

/*
 * XMultiXactIdSetOldestMember
 *		Save the oldest XMultiXactId this transaction could be a member of.
 *
 * We set the OldestMemberXMXactId for a given transaction the first time it's
 * going to do some operation that might require a XMultiXactId (tuple lock,
 * update or delete).  We need to do this even if we end up using a
 * TransactionId instead of a XMultiXactId, because there is a chance that
 * another transaction would add our XID to a XMultiXactId.
 *
 * The value to set is the next-to-be-assigned XMultiXactId, so this is meant to
 * be called just before doing any such possibly-XMultiXactId-able operation.
 */
void
XMultiXactIdSetOldestMember(void)
{
	if (!XMultiXactIdIsValid(OldestMemberXMXactId[MyProcNumber]))
	{
		XMultiXactId nextMXact;

		/*
		 * You might think we don't need to acquire a lock here, since
		 * fetching and storing of TransactionIds is probably atomic, but in
		 * fact we do: suppose we pick up nextMXact and then lose the CPU for
		 * a long time.  Someone else could advance nextMXact, and then
		 * another someone else could compute an OldestVisibleMXactId that
		 * would be after the value we are going to store when we get control
		 * back.  Which would be wrong.
		 *
		 * Note that a shared lock is sufficient, because it's enough to stop
		 * someone from advancing nextMXact; and nobody else could be trying
		 * to write to our OldestMember entry, only reading (and we assume
		 * storing it is atomic.)
		 */
		LWLockAcquire(XMultiXactGenLock, LW_SHARED);

		/*
		 * We have to beware of the possibility that nextMXact is in the
		 * wrapped-around state.  We don't fix the counter itself here, but we
		 * must be sure to store a valid value in our array entry.
		 */
		nextMXact = XMultiXactState->nextXMXact;
		if (XMultiXactIdPrecedes(nextMXact, FirstXMultiXactId))
			nextMXact = FirstXMultiXactId;

		OldestMemberXMXactId[MyProcNumber] = nextMXact;

		LWLockRelease(XMultiXactGenLock);

		debug_elog4(DEBUG2, "XMultiXact: setting OldestMember[%d] = %lu",
					MyProcNumber, nextMXact.value);
	}
}


/*
 * MultiXactIdSetOldestVisible
 *		Save the oldest MultiXactId this transaction considers possibly live.
 *
 * We set the OldestVisibleMXactId for a given transaction the first time
 * it's going to inspect any MultiXactId.  Once we have set this, we are
 * guaranteed that the checkpointer won't truncate off SLRU data for
 * MultiXactIds at or after our OldestVisibleMXactId.
 *
 * The value to set is the oldest of nextMXact and all the valid per-backend
 * OldestMemberMXactId[] entries.  Because of the locking we do, we can be
 * certain that no subsequent call to MultiXactIdSetOldestMember can set
 * an OldestMemberMXactId[] entry older than what we compute here.  Therefore
 * there is no live transaction, now or later, that can be a member of any
 * MultiXactId older than the OldestVisibleMXactId we compute here.
 */
static void
XMultiXactIdSetOldestVisible(void)
{
	if (!XMultiXactIdIsValid(OldestVisibleXMXactId[MyProcNumber]))
	{
		XMultiXactId oldestMXact;
		int			i;

		LWLockAcquire(XMultiXactGenLock, LW_EXCLUSIVE);

		/*
		 * We have to beware of the possibility that nextMXact is in the
		 * wrapped-around state.  We don't fix the counter itself here, but we
		 * must be sure to store a valid value in our array entry.
		 */
		oldestMXact = XMultiXactState->nextXMXact;
		if (XMultiXactIdPrecedes(oldestMXact, FirstXMultiXactId))
			oldestMXact = FirstXMultiXactId;

		for (i = 1; i <= MaxOldestSlot; i++)
		{
			XMultiXactId thisoldest = OldestMemberXMXactId[i];

			if (XMultiXactIdIsValid(thisoldest) &&
				XMultiXactIdPrecedes(thisoldest, oldestMXact))
				oldestMXact = thisoldest;
		}

		OldestVisibleXMXactId[MyProcNumber] = oldestMXact;

		LWLockRelease(XMultiXactGenLock);

		debug_elog4(DEBUG2, "MultiXact: setting OldestVisible[%d] = %lu",
					MyProcNumber, oldestMXact.value);
	}
}

void
xmultixact_twophase_recover(TransactionId xid, uint16 info,
						   void *recdata, uint32 len)
{
	ProcNumber	dummyProcNumber = TwoPhaseGetDummyProcNumber(xid, false);
	XMultiXactId oldestMember;

	/*
	 * Get the oldest member XID from the state file record, and set it in the
	 * OldestMemberMXactId slot reserved for this prepared transaction.
	 */
	Assert(len == sizeof(XMultiXactId));
	oldestMember = *((XMultiXactId *) recdata);

	OldestMemberXMXactId[dummyProcNumber] = oldestMember;
}

void
xmultixact_twophase_postcommit(TransactionId xid, uint16 info,
							  void *recdata, uint32 len)
{
	ProcNumber	dummyProcNumber = TwoPhaseGetDummyProcNumber(xid, true);

	Assert(len == sizeof(XMultiXactId));

	OldestMemberXMXactId[dummyProcNumber] = InvalidXMultiXactId;
}

void
xmultixact_twophase_postabort(TransactionId xid, uint16 info,
							 void *recdata, uint32 len)
{
	xmultixact_twophase_postcommit(xid, info, recdata, len);
}


/*
 * AtPrepare_XMultiXact
 *		Save xmultixact state at 2PC transaction prepare
 *
 * In this phase, we only store our OldestMemberXMXactId value in the two-phase
 * state file.
 */
void
AtPrepare_XMultiXact(void)
{
	XMultiXactId myOldestMember = OldestMemberXMXactId[MyProcNumber];

	if (XMultiXactIdIsValid(myOldestMember))
		RegisterTwoPhaseRecord(TWOPHASE_RM_XMULTIXACT_ID, 0,
							   &myOldestMember, sizeof(XMultiXactId));
}

/*
 * PostPrepare_XMultiXact
 *		Clean up after successful PREPARE TRANSACTION
 */
void
PostPrepare_XMultiXact(TransactionId xid)
{
	XMultiXactId myOldestMember;

	/*
	 * Transfer our OldestMemberMXactId value to the slot reserved for the
	 * prepared transaction.
	 */
	myOldestMember = OldestMemberXMXactId[MyProcNumber];
	if (XMultiXactIdIsValid(myOldestMember))
	{
		ProcNumber	dummyProcNumber = TwoPhaseGetDummyProcNumber(xid, false);

		/*
		 * Even though storing MultiXactId is atomic, acquire lock to make
		 * sure others see both changes, not just the reset of the slot of the
		 * current backend. Using a volatile pointer might suffice, but this
		 * isn't a hot spot.
		 */
		LWLockAcquire(XMultiXactGenLock, LW_EXCLUSIVE);

		OldestMemberXMXactId[dummyProcNumber] = myOldestMember;
		OldestMemberXMXactId[MyProcNumber] = InvalidXMultiXactId;

		LWLockRelease(XMultiXactGenLock);
	}

	/*
	 * We don't need to transfer OldestVisibleXMXactId value, because the
	 * transaction is not going to be looking at any more xmultixacts once it's
	 * prepared.
	 *
	 * We assume that storing a xMultiXactId is atomic and so we need not take
	 * XMultiXactGenLock to do this.
	 */
	OldestVisibleXMXactId[MyProcNumber] = InvalidXMultiXactId;

	/*
	 * Discard the local MultiXactId cache like in AtEOXact_XMultiXact.
	 */
	XMXactContext = NULL;
	dlist_init(&XMXactCache);
	XMXactCacheMembers = 0;
}

/*
 * AtEOXact_XMultiXact
 *		Handle transaction end for XMultiXact
 *
 * This is called at top transaction commit or abort (we don't care which).
 */
void
AtEOXact_XMultiXact(void)
{
	/*
	 * Reset our OldestMemberXMXactId and OldestVisibleXMXactId values, both of
	 * which should only be valid while within a transaction.
	 *
	 * We assume that storing a XMultiXactId is atomic and so we need not take
	 * XMultiXactGenLock to do this.
	 */
	OldestMemberXMXactId[MyProcNumber] = InvalidXMultiXactId;
	OldestVisibleXMXactId[MyProcNumber] = InvalidXMultiXactId;

	/*
	 * Discard the local MultiXactId cache.  Since MXactContext was created as
	 * a child of TopTransactionContext, we needn't delete it explicitly.
	 */
	XMXactContext = NULL;
	dlist_init(&XMXactCache);
	XMXactCacheMembers = 0;
}

/*
 * GetOldestXMultiXactId
 *
 * Return the oldest XMultiXactId that's still possibly still seen as live by
 * any running transaction.  Older ones might still exist on disk, but they no
 * longer have any running member transaction.
 */
XMultiXactId
GetOldestXMultiXactId(void)
{
	XMultiXactId oldestMXact;
	XMultiXactId nextMXact;
	int			i;

	/*
	 * This is the oldest valid value among all the OldestMemberXMXactId[] and
	 * OldestVisibleXMXactId[] entries, or nextMXact if none are valid.
	 */
	LWLockAcquire(XMultiXactGenLock, LW_SHARED);

	/* XMultiXactId is 64 bit and nerver wrap around */
	nextMXact = XMultiXactState->nextXMXact;

	oldestMXact = nextMXact;
	for (i = 1; i <= MaxOldestSlot; i++)
	{
		XMultiXactId thisoldest;

		thisoldest = OldestMemberXMXactId[i];
		if (XMultiXactIdIsValid(thisoldest) &&
			XMultiXactIdPrecedes(thisoldest, oldestMXact))
			oldestMXact = thisoldest;
		thisoldest = OldestVisibleXMXactId[i];
		if (XMultiXactIdIsValid(thisoldest) &&
			XMultiXactIdPrecedes(thisoldest, oldestMXact))
			oldestMXact = thisoldest;
	}

	LWLockRelease(XMultiXactGenLock);

	return oldestMXact;
}

typedef struct xmxtruncinfo
{
	int64			earliestExistingPage;
} xmxtruncinfo;

/*
 * Remove all XMultiXactOffset and XMultiXactMember segments before the oldest
 * ones still of interest.
 *
 * newOldestXMulti is the oldest currently required xmultixact
 */
void
TruncateXMultiXact(void)
{
	XMultiXactId newOldestXMulti;
	XMultiXactId nextXMulti;
	XMultiXactOffset newOldestOffset;
	XMultiXactOffset nextOffset;
	xmxtruncinfo trunc;
	XMultiXactId earliest;

	Assert(!RecoveryInProgress());

	newOldestXMulti = GetOldestXMultiXactId();
	if (XMultiXactIdPrecedesOrEquals(newOldestXMulti, FirstXMultiXactId))
		return;
	/*
	 * We can only allow one truncation to happen at once. Otherwise parts of
	 * members might vanish while we're doing lookups or similar. There's no
	 * need to have an interlock with creating new multis or such, since those
	 * are constrained by the limits (which only grow, never shrink).
	 */
	LWLockAcquire(XMultiXactTruncationLock, LW_EXCLUSIVE);

	LWLockAcquire(XMultiXactGenLock, LW_SHARED);
	nextXMulti = XMultiXactState->nextXMXact;
	nextOffset = XMultiXactState->nextOffset;
	LWLockRelease(XMultiXactGenLock);

	/*
	 * Note we can't just plow ahead with the truncation; it's possible that
	 * there are no segments to truncate, which is a problem because we are
	 * going to attempt to read the offsets page to determine where to
	 * truncate the members SLRU.  So we first scan the directory to determine
	 * the earliest offsets page number that we can read without error.
	 *
	 * When nextMXact is less than one segment away from multiWrapLimit,
	 * SlruScanDirCbFindEarliest can find some early segment other than the
	 * actual earliest.  (MultiXactOffsetPagePrecedes(EARLIEST, LATEST)
	 * returns false, because not all pairs of entries have the same answer.)
	 * That can also arise when an earlier truncation attempt failed unlink()
	 * or returned early from this function.  The only consequence is
	 * returning early, which wastes space that we could have liberated.
	 *
	 * NB: It's also possible that the page that oldestMulti is on has already
	 * been truncated away, and we crashed before updating oldestMulti.
	 */
	trunc.earliestExistingPage = -1;

	XlruScanDirectory(XMultiXactOffsetCtl, XlruScanDirCbFindEarliest, &trunc);

	earliest.value = trunc.earliestExistingPage * XMULTIXACT_OFFSETS_PER_PAGE;

	/* If there's nothing to remove, we can bail out early. */
	if (XMultiXactIdPrecedes(newOldestXMulti, earliest))
	{
		LWLockRelease(XMultiXactTruncationLock);
		return;
	}

	/*
	 * First, compute up to where to truncate. Lookup the corresponding
	 * member offset for newOldestMulti for that.
	 */
	if (XMultiXactIdEquals(newOldestXMulti, nextXMulti))
	{
		/* there are NO MultiXacts */
		newOldestOffset = nextOffset;

		/*
		* We stop truncate to avoid passing a cutoff page that hasn't
		* been created yet in the rare case that newOldestOffset would be the first
		* item on a page and newOldestOffset == nextOffset.  In that case, if we
		* truncate member page, we'd trigger SimpleLruTruncate's wraparound
		* detection.
		*/
		if (newOldestOffset % XMULTIXACT_MEMBERS_PER_PAGE == 0)
			return;
	}
	else if (!find_xmultixact_start(newOldestXMulti, &newOldestOffset))
	{
		ereport(LOG,
				(errmsg("cannot truncate up to XMultiXact %lu because it does not exist on disk, skipping truncation",
						newOldestXMulti.value)));
		LWLockRelease(XMultiXactTruncationLock);
		return;
	}

	elog(DEBUG1, "performing multixact truncation to: "
		 "offsets %lu, offsets pages %lu, offsets segments %lx, "
		 "members %lu, members pages %lu, members segments %lx, "
		 "nextXMulti %lu, nextOffset %lu",
		 newOldestXMulti.value, XMultiXactIdToOffsetPage(newOldestXMulti),
		 XMultiXactIdToOffsetSegment(newOldestXMulti), newOldestOffset,
		 XMXOffsetToMemberPage(newOldestOffset), XMXOffsetToMemberSegment(newOldestOffset),
		 nextXMulti.value, nextOffset);

	/*
	 * Do truncation, and the WAL logging of the truncation, in a critical
	 * section. That way offsets/members cannot get out of sync anymore, i.e.
	 * once consistent the newOldestMulti will always exist in members, even
	 * if we crashed in the wrong moment.
	 */
	START_CRIT_SECTION();

	/*
	 * Prevent checkpoints from being scheduled concurrently. This is critical
	 * because otherwise a truncation record might not be replayed after a
	 * crash/basebackup, even though the state of the data directory would
	 * require it.
	 */
	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;

	/* WAL log truncation */
	WriteXMTruncateXlogRec(newOldestXMulti, newOldestOffset);

	/* First truncate members */
	PerformXMembersTruncation(newOldestOffset);

	/* Then offsets */
	PerformXOffsetsTruncation(newOldestXMulti);

	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;

	END_CRIT_SECTION();
	LWLockRelease(XMultiXactTruncationLock);
}

/*
 * Find the starting offset of the given MultiXactId.
 *
 * Returns false if the file containing the multi does not exist on disk.
 * Otherwise, returns true and sets *result to the starting member offset.
 *
 * This function does not prevent concurrent truncation, so if that's
 * required, the caller has to protect against that.
 */
static bool
find_xmultixact_start(XMultiXactId multi, XMultiXactOffset *result)
{
	XMultiXactOffset offset;
	int64			pageno;
	int64			entryno;
	int64			slotno;
	XMultiXactOffset *offptr;

	pageno = XMultiXactIdToOffsetPage(multi);
	entryno = XMultiXactIdToOffsetEntry(multi);

	/*
	 * Write out dirty data, so PhysicalPageExists can work correctly.
	 */
	XlruWriteAll(XMultiXactOffsetCtl, true);
	XlruWriteAll(XMultiXactMemberCtl, true);

	if (!XlruDoesPhysicalPageExist(XMultiXactOffsetCtl, pageno))
		return false;

	/* lock is acquired by XlruReadPage_ReadOnly */
	slotno = XlruReadPage_ReadOnly(XMultiXactOffsetCtl, pageno, multi);
	offptr = (XMultiXactOffset *) XMultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;
	offset = *offptr;
	LWLockRelease(XMultiXactOffsetSLRULock);

	*result = offset;
	return true;
}

/*
 * Write a TRUNCATE xlog record
 *
 * We must flush the xlog record to disk before returning --- see notes in
 * TruncateCLOG().
 */
static void
WriteXMTruncateXlogRec(XMultiXactId endTruncOff, XMultiXactOffset endTruncMemb)
{
	XLogRecPtr	recptr;
	xl_xmultixact_truncate xlrec;

	xlrec.endTruncOff = endTruncOff;
	xlrec.endTruncMemb = endTruncMemb;

	XLogBeginInsert();
	XLogRegisterData((char *) (&xlrec), SizeOfXMultiXactTruncate);
	recptr = XLogInsert(RM_XMULTIXACT_ID, XLOG_XMULTIXACT_TRUNCATE_ID);
	XLogFlush(recptr);
}

/*
 * Remove all segments before the page number of newOldestOffset
 */
static void
PerformXMembersTruncation(XMultiXactOffset newOldestOffset)
{
	XlruTruncate(XMultiXactMemberCtl,
					XMXOffsetToMemberPage(newOldestOffset));
}

/*
 * Remove all segments before the page number of newOldestXMulti
 */
static void
PerformXOffsetsTruncation(XMultiXactId newOldestXMulti)
{
	/*
	 * We step back one multixact to avoid passing a cutoff page that hasn't
	 * been created yet in the rare case that oldestMulti would be the first
	 * item on a page and oldestMulti == nextMulti.  In that case, if we
	 * didn't subtract one, we'd trigger SimpleLruTruncate's wraparound
	 * detection.
	 */

	XlruTruncate(XMultiXactOffsetCtl,
					  XMultiXactIdToOffsetPage(PreviousXMultiXactId(newOldestXMulti)));
}


/*
 * This must be called ONCE during postmaster or standalone-backend startup.
 *
 * StartupXLOG has already established nextXMXact/nextOffset by calling
 * XMultiXactSetNextXMXact and/or XMultiXactAdvanceNextMXact, and the oldestXMulti
 * info from pg_control and/or XMultiXactAdvanceOldest, but we haven't yet
 * replayed WAL.
 */
void
StartupXMultiXact(void)
{
	XMultiXactId multi = XMultiXactState->nextXMXact;
	XMultiXactOffset offset = XMultiXactState->nextOffset;
	int64			pageno;

	/*
	 * Initialize offset's idea of the latest page number.
	 */
	pageno = XMultiXactIdToOffsetPage(multi);
	XMultiXactOffsetCtl->shared->latest_page_number = pageno;

	/*
	 * Initialize member's idea of the latest page number.
	 */
	pageno = XMXOffsetToMemberPage(offset);
	XMultiXactMemberCtl->shared->latest_page_number = pageno;
}

/*
 * Set the next-to-be-assigned XMultiXactId and offset
 *
 * This is used when we can determine the correct next ID/offset exactly
 * from a checkpoint record.  Although this is only called during bootstrap
 * and XLog replay, we take the lock in case any hot-standby backends are
 * examining the values.
 */
void
XMultiXactSetNextXMXact(XMultiXactId nextXMulti,
					   XMultiXactOffset nextXMultiOffset)
{
	debug_elog4(DEBUG2, "XMultiXact: setting next xmulti to %lu offset %lu",
				nextXMulti.value, nextXMultiOffset);
	LWLockAcquire(XMultiXactGenLock, LW_EXCLUSIVE);
	XMultiXactState->nextXMXact = nextXMulti;
	XMultiXactState->nextOffset = nextXMultiOffset;
	LWLockRelease(XMultiXactGenLock);
}

/*
 * Get the XMultiXact data to save in a checkpoint record. We have 64 bit XMultiXactId and do not need
 * to freeze the multi xact id in tuple, so there is no need to store the oldestXmultiId in checkpoint.
 * The only thing we need to do is finding a minmal XMultiXactId according to XMulti files when starting up.
 */
void
XMultiXactGetCheckptXMulti(FullTransactionId *nextXMulti, uint64 *nextXMultiOffset)
{
	LWLockAcquire(XMultiXactGenLock, LW_SHARED);
	*nextXMulti = XMultiXactState->nextXMXact;
	*nextXMultiOffset = XMultiXactState->nextOffset;
	LWLockRelease(XMultiXactGenLock);

	debug_elog4(DEBUG2,
				"XMultiXact: checkpoint is nextXMulti %lu, nextOffset %lu",
				nextXMulti->value, *nextXMultiOffset);
}

/* pg kerner don't know XMultiXactId, we use a warap function as the hook */
void
XMultiXactAdvanceNextXMXactWrapper(FullTransactionId minXMulti, uint64 minMultiOffset)
{
	XMultiXactAdvanceNextXMXact((XMultiXactId)minXMulti, (XMultiXactOffset)minMultiOffset);
}

static bool
Do_XMultiXactIdWait(XMultiXactId multi, XMultiXactStatus status, bool nowait)
{
	bool		result = true;
	XMultiXactMember *members;
	int			nmembers;
	FullTransactionId globalFrozenXid = FullTransactionIdFromU64(pg_atomic_read_u64(&(undo_sys_ctx->global_frozen_xid)));

	/* for pre-pg_upgrade tuples, no need to sleep at all */
	nmembers = GetXMultiXactIdMembers(multi, &members);

	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			FullTransactionId memxid = members[i].xid;
			XMultiXactStatus memstatus = members[i].status;

			/* The transaction smaller than globalFrozenXid must have finished */
			if (FullTransactionIdPrecedes(memxid, globalFrozenXid))
				continue;

			if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(memxid)))
				continue;

			if (!DoLockModesConflict(LOCKMODE_from_xmxstatus(memstatus),
									 LOCKMODE_from_xmxstatus(status)))
				continue;

			/*
			 * This member conflicts with our multi, so we have to sleep (or
			 * return failure, if asked to avoid waiting.)
			 *
			 * Note that we don't set up an error context callback ourselves,
			 * but instead we pass the info down to XactLockTableWait.  This
			 * might seem a bit wasteful because the context is set up and
			 * tore down for each member of the multixact, but in reality it
			 * should be barely noticeable, and it avoids duplicate code.
			 */
			if (nowait)
			{
				result = ConditionalXactLockTableWait(XidFromFullTransactionId(memxid));
				if (!result)
					break;
			}
			else
				XactLockTableWait(XidFromFullTransactionId(memxid), NULL, NULL, XLTW_None);
		}

		pfree(members);
	}

	return result;
}

void
XMultiXactIdWait(XMultiXactId multi, XMultiXactStatus status)
{
	(void) Do_XMultiXactIdWait(multi, status, false);
}

bool
ConditionalXMultiXactIdWait(XMultiXactId multi, XMultiXactStatus status)
{
	return Do_XMultiXactIdWait(multi, status, true);
}

XMultiXactStatus
get_xmxact_status_for_lock(LockTupleMode mode, bool is_update)
{
	int			retval;

	if (is_update)
		retval = tupleLockExtraInfo[mode].updstatus;
	else
		retval = tupleLockExtraInfo[mode].lockstatus;

	if (retval == -1)
		elog(ERROR, "invalid lock tuple mode %d/%s", mode,
			 is_update ? "true" : "false");

	return (XMultiXactStatus) retval;
}

/*
* XMultiXactIdIsCurrent
*		Returns true if the current transaction is a member of the XMultiXactId.
*
* We return true if any live subtransaction of the current top-level
		* transaction is a member.  This is appropriate for the same reason that a
		* lock held by any such subtransaction is globally equivalent to a lock
		* held by the current subtransaction: no such lock could be released without
		* aborting this subtransaction, and hence releasing its locks.  So it's not
* necessary to add the current subxact to the MultiXact separately.
*/
bool 
XMultiXactIdIsCurrent(XMultiXactId multi, FullTransactionId *xid)
{
	bool result = false;
	XMultiXactMember *members = NULL;
	int nmembers;
	int i;

	nmembers = GetXMultiXactIdMembers(multi, &members);
	if (nmembers < 0)
		return false;

	for (i = 0; i < nmembers; i++) {
		if (TransactionIdIsCurrentTransactionId(XidFromFullTransactionId(members[i].xid))) 
		{
			if (xid != NULL) 
				*xid = members[i].xid;
			result = true;
			break;
		}
	}

	pfree(members);
	members = NULL;

	return result;
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointXMultiXact(void)
{
	//TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_START(true);

	/*
	 * Write dirty XMultiXact pages to disk.  This may result in sync requests
	 * queued for later handling by ProcessSyncRequests(), as part of the
	 * checkpoint.
	 */
	XlruWriteAll(XMultiXactOffsetCtl, true);
	XlruWriteAll(XMultiXactMemberCtl, true);

	// TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_DONE(true);
}

/*
 * This func must be called ONCE on system install.  It creates the initial
 * XMultiXact segments.  (The XMultiXacts directories are assumed to have been
 * created by initdb, and XMultiXactShmemInit must have been called already.)
 */
void
BootStrapXMultiXact(void)
{
	int64			slotno;

	LWLockAcquire(XMultiXactOffsetSLRULock, LW_EXCLUSIVE);

	/* Create and zero the first page of the offsets log */
	slotno = ZeroXMultiXactOffsetPage(0, false);

	/* Make sure it's written out */
	XlruWritePage(XMultiXactOffsetCtl, slotno);
	Assert(!XMultiXactOffsetCtl->shared->page_dirty[slotno]);

	LWLockRelease(XMultiXactOffsetSLRULock);

	LWLockAcquire(XMultiXactMemberSLRULock, LW_EXCLUSIVE);

	/* Create and zero the first page of the members log */
	slotno = ZeroXMultiXactMemberPage(0, false);

	/* Make sure it's written out */
	XlruWritePage(XMultiXactMemberCtl, slotno);
	Assert(!XMultiXactMemberCtl->shared->page_dirty[slotno]);

	LWLockRelease(XMultiXactMemberSLRULock);
}

/*
 * Initialize XMultiXact files. This function will create pg_xmultxact, pg_xmultxact/members,
 * pg_xmultixact/offsets and init first page of each members and offsets files. It will be called
 * when the xstore.so is loading.
 */	
void 
InitXMultiFiles(void)
{
	char *path;
	int ret;

	path = psprintf("%s/%s", DataDir, "pg_xmultixact");
	switch (ret = pg_check_dir(path)) 
	{
		case 0: /* not exist */
		case 1: /* present but empty */
			create_xmulti_dir();
			BootStrapXMultiXact();
			break;

		case -1:
			elog(ERROR, "pg_xmultixact directory can not be accessed, %s", path);
			break;

		default:
			break;
	}
}

/*
 * XlruScanDirectory callback
 *		This callback determines the earliest existing page number.
 */
static bool
XlruScanDirCbFindEarliest(XlruCtl ctl, char *filename, int64 segpage, void *data)
{
	xmxtruncinfo *trunc = (xmxtruncinfo *) data;

	if (trunc->earliestExistingPage == -1 || segpage < trunc->earliestExistingPage)
		trunc->earliestExistingPage = segpage;

	return false;				/* keep going */
}

/*
 * This must be called ONCE at the end of startup/recovery.
 */
void
TrimXMultiXact(void)
{
	XMultiXactId nextMXact;
	XMultiXactOffset offset;
	int64			pageno;
	int64			entryno;
	int64			flagsoff;

	LWLockAcquire(XMultiXactGenLock, LW_SHARED);
	nextMXact = XMultiXactState->nextXMXact;
	offset = XMultiXactState->nextOffset;
	LWLockRelease(XMultiXactGenLock);

	/* Clean up offsets state */
	LWLockAcquire(XMultiXactOffsetSLRULock, LW_EXCLUSIVE);

	/*
	 * (Re-)Initialize our idea of the latest page number for offsets.
	 */
	pageno = XMultiXactIdToOffsetPage(nextMXact);
	XMultiXactOffsetCtl->shared->latest_page_number = pageno;

	/*
	 * Zero out the remainder of the current offsets page.  See notes in
	 * TrimCLOG() for background.  Unlike CLOG, some WAL record covers every
	 * pg_xmultixact SLRU mutation.  Since, also unlike CLOG, we ignore the WAL
	 * rule "write xlog before data," nextMXact successors may carry obsolete,
	 * nonzero offset values.  Zero those so case 2 of GetMultiXactIdMembers()
	 * operates normally.
	 */
	entryno = XMultiXactIdToOffsetEntry(nextMXact);
	if (entryno != 0)
	{
		int			slotno;
		XMultiXactOffset *offptr;

		slotno = XlruReadPage(XMultiXactOffsetCtl, pageno, true, nextMXact);
		offptr = (XMultiXactOffset *) XMultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;

		MemSet(offptr, 0, BLCKSZ - (entryno * sizeof(XMultiXactOffset)));

		XMultiXactOffsetCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(XMultiXactOffsetSLRULock);

	/* And the same for members */
	LWLockAcquire(XMultiXactMemberSLRULock, LW_EXCLUSIVE);

	/*
	 * (Re-)Initialize our idea of the latest page number for members.
	 */
	pageno = XMXOffsetToMemberPage(offset);
	XMultiXactMemberCtl->shared->latest_page_number = pageno;

	/*
	 * Zero out the remainder of the current members page.  See notes in
	 * TrimCLOG() for motivation.
	 */
	flagsoff = XMXOffsetToFlagsOffset(offset);
	if (flagsoff != 0)
	{
		int			slotno;
		FullTransactionId *xidptr;
		int			memberoff;

		memberoff = XMXOffsetToMemberOffset(offset);
		slotno = XlruReadPage(XMultiXactMemberCtl, pageno, true, FullTransactionIdFromU64(offset));
		xidptr = (FullTransactionId *)
			(XMultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		MemSet(xidptr, 0, BLCKSZ - memberoff);

		/*
		 * Note: we don't need to zero out the flag bits in the remaining
		 * members of the current group, because they are always reset before
		 * writing.
		 */

		XMultiXactMemberCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(XMultiXactMemberSLRULock);
}
