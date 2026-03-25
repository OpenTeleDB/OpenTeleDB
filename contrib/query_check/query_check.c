/*-------------------------------------------------------------------------
 * 
 * Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * query_check.c
 *      check the SQL query for notification of the read-only statement and
 *      statements that require memory caching
 *
 * IDENTIFICATION
 *	  contrib/query_check/query_check.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "storage/lock.h"
#include "storage/ipc.h"
#include "executor/spi.h"
#include "tcop/utility.h"
#include "utils/elog.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

#define ALF_LIST_SIZE 7
#define LOCK_FUNC_NUMS 4

/*
 * Functions for manipulating advisory locks
 *
 * We make use of the locktag fields as follows:
 *
 *	field1: MyDatabaseId ... ensures locks are local to each database
 *	field2: first of 2 int4 keys, or high-order half of an int8 key
 *	field3: second of 2 int4 keys, or low-order half of an int8 key
 *	field4: 1 if using an int8 key, 2 if using 2 int4 keys
 */
#define SET_LOCKTAG_INT64(tag, key64) \
	SET_LOCKTAG_ADVISORY(tag, \
						 MyDatabaseId, \
						 (uint32) ((key64) >> 32), \
						 (uint32) (key64), \
						 1)
#define SET_LOCKTAG_INT32(tag, key1, key2) \
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, key1, key2, 2)

#define COMPARE_LOCKTAG(tag1, tag2) \
	((tag1).locktag_field1 == (tag2).locktag_field1 && \
     (tag1).locktag_field2 == (tag2).locktag_field2 && \
     (tag1).locktag_field3 == (tag2).locktag_field3 && \
     (tag1).locktag_field4 == (tag2).locktag_field4 && \
     (tag1).locktag_type == (tag2).locktag_type && \
     (tag1).locktag_lockmethodid == (tag2).locktag_lockmethodid)

typedef struct
{
    bool has_system_catalog; /* True if system catalog table is used */
    bool has_function_call;  /* True if write function call is used */} SelectContext;

typedef struct TransRecords {
    List *records;
    struct TransRecords *parent;
} TransRecords;

typedef struct  {
	const char *name;
	Oid oid;
	bool shared;
	bool lock;
} AdvisoryFunction; 

typedef struct {
    const char *name;
    Oid oid;
} SystemObject;

/*---- Local variables ----*/

static bool query_check_enabled;
static bool check_readonly_enabled;

/* Whether the current query uses a read-only hint */
static bool is_read_only_hint = false;

/* Current nesgting depth of Executor calls */
static int g_in_exec_func = 0;

/* Record the response message within the transaction block */
static TransRecords *CurrentTransRecords = NULL;
static List *trans_records = NIL;
static List *session_records = NIL;
static MemoryContext trans_records_ctx = NULL;
static MemoryContext session_records_ctx = NULL;

/* Record the locks within the session level */
static bool hold_unknown_session_locks = false;
static List *session_locks = NIL;
static MemoryContext session_locks_ctx = NULL;

/* Whether the current query has advisory lock related functions */
static bool advisory_lock_function_in_progress = false;

static AdvisoryFunction advisory_lock_function_list[] = {
    {"pg_advisory_lock(int8)", 0, false, true},
    {"pg_advisory_lock_shared(int8)", 0, true, true},
    {"pg_try_advisory_lock(int8)", 0, false, true},
    {"pg_try_advisory_lock_shared(int8)", 0, true, true},
    {"pg_advisory_unlock(int8)", 0, false, false},
    {"pg_advisory_unlock_shared(int8)", 0, true, false},
    {"pg_advisory_lock(int4,int4)", 0, false, true},
    {"pg_advisory_lock_shared(int4,int4)", 0, true, true},
    {"pg_try_advisory_lock(int4,int4)", 0, false, true},
    {"pg_try_advisory_lock_shared(int4,int4)", 0, true, true},
    {"pg_advisory_unlock(int4,int4)", 0, false, false},
    {"pg_advisory_unlock_shared(int4,int4)", 0, true, false},
    {"pg_advisory_unlock_all()", 0, false, false},
    {NULL, 0, false, false}};

// TODO: add more system functions that can cause write
/* List of system functions that can cause write */
static SystemObject system_function_write_list[] = {
    {"nextval(regclass)", 0},
    {"setval(regclass,bigint)", 0},
    {"setval(regclass,bigint,boolean)", 0},
    {NULL, 0}
};

// TODO: add more system catalogs that need to read from primary
/* List of system catalog tables that need to read from primary */
static SystemObject system_catalog_need_primary_list[] = {
    {"pg_stat_replication", 0},
    {"pg_replication_slots", 0},
    {"pg_stat_user_functions", 0},
    {"pg_stat_archiver", 0},
    {"pg_stat_bgwriter", 0},
    {"pg_stat_progress_vacuum", 0},
    {"pg_stat_progress_create_index", 0},
    {"pg_stat_progress_cluster", 0},
#if PG_VERSION_NUM >= 130000
    {"pg_stat_progress_basebackup", 0},
    {"pg_stat_progress_analyze", 0},
    {"pg_stat_progress_copy", 0},
#endif
    {NULL, 0}
};

/* Saved hook values in case of unload */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static object_access_hook_type prev_object_access_hook = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/*---- Function declarations ----*/

void _PG_init(void);
void _PG_fini(void);

PG_FUNCTION_INFO_V1(load_dynamic_library);

static void qc_post_parse_analyze(ParseState *pstate, Query *query,
                                  JumbleState *jstate);
static void qc_object_access(ObjectAccessType access,
                             Oid classId,
                             Oid objectId,
                             int subId,
                             void *arg);
static void qc_ExecutorRun(QueryDesc *queryDesc,
                           ScanDirection direction,
                           uint64 count, bool execute_once);
static void qc_ExecutorFinish(QueryDesc *queryDesc);
static void qc_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
#if PG_VERSION_NUM >= 140000
                              bool readOnlyTree,
#endif
                              ProcessUtilityContext context, ParamListInfo params,
                              QueryEnvironment *queryEnv,
                              DestReceiver *dest,
#if PG_VERSION_NUM >= 130000
                              QueryCompletion *qc
#else
                              char *completionTag
#endif
                              );
static bool is_read_only_query(Query *query);
static bool qc_has_function_call_or_system_catalog(Query *query);
static bool qc_query_tree_walker(Node *node, void *context);
static bool has_advisory_lock_function_call(PlanState *planstate, void *context);
static bool manage_session_locks(Node *node, void *context);
static void qc_send_message_to_frontend(const char *message);
static bool contains_read_only_hint_in_comments(const char *str);
static void get_variableset_message(VariableSetStmt *stmt, char *msg, int size);
static char *flatten_set_variable_args(List *args);
static void simple_quote_literal(StringInfo buf, const char *val);
static void record_in_transaction(const char *record, bool is_session_level);
static void qc_xact_callback(XactEvent event, void *arg);
static void qc_sub_xact_callback(SubXactEvent event, SubTransactionId mySubid,
                                 SubTransactionId parentSubid, void *arg);
static bool query_check_enabled_check_hook(bool *newval, void **extra, GucSource source);

/*
 * Module load callback
 */
void _PG_init(void)
{
    /* Install hooks. */
    DefineCustomBoolVariable("query_check.enabled",
                            "Enable / Disable query_check",
                            NULL,
                            &query_check_enabled,
                            false,
                            PGC_USERSET,
#if PG_VERSION_NUM >= 160000
                            GUC_NO_RESET | GUC_NO_RESET_ALL,
#else
                            GUC_NO_RESET_ALL,
#endif
                            query_check_enabled_check_hook,
                            NULL,
                            NULL);

	DefineCustomBoolVariable("query_check.check_readonly_enabled",
                            "Enable / Disable readonly check logic",
                            NULL,
                            &check_readonly_enabled,
                            true,
                            PGC_USERSET,
                            0,
                            NULL,
                            NULL,
                            NULL);

    prev_post_parse_analyze_hook = post_parse_analyze_hook;
    post_parse_analyze_hook = qc_post_parse_analyze;
    prev_object_access_hook = object_access_hook;
    object_access_hook = qc_object_access;
    prev_ExecutorRun = ExecutorRun_hook;
    ExecutorRun_hook = qc_ExecutorRun;
    prev_ExecutorFinish = ExecutorFinish_hook;
    ExecutorFinish_hook = qc_ExecutorFinish;
    prev_ProcessUtility = ProcessUtility_hook;
    ProcessUtility_hook = qc_ProcessUtility;

    RegisterXactCallback(qc_xact_callback, NULL);
    RegisterSubXactCallback(qc_sub_xact_callback, NULL);

    session_locks_ctx = AllocSetContextCreate(TopMemoryContext,
                                              "query_check session locks context",
                                              ALLOCSET_SMALL_SIZES);
    trans_records_ctx = AllocSetContextCreate(TopMemoryContext,
                                              "query_check transaction  records context",
                                              ALLOCSET_SMALL_SIZES);
    session_records_ctx = AllocSetContextCreate(TopMemoryContext,
                                              "query_check session  records context",
                                              ALLOCSET_SMALL_SIZES);
}

/*
 * Module unload callback
 */
void _PG_fini(void)
{
    /* Uninstall hooks. */
    post_parse_analyze_hook = prev_post_parse_analyze_hook;
    object_access_hook = prev_object_access_hook;
    ExecutorRun_hook = prev_ExecutorRun;
    ExecutorFinish_hook = prev_ExecutorFinish;
    ProcessUtility_hook = prev_ProcessUtility;

    UnregisterXactCallback(qc_xact_callback, NULL);
    UnregisterSubXactCallback(qc_sub_xact_callback, NULL);
}

/*
 * post-parse-analysis hook:
 * determine whether the query is a read-only statement during the parsing phase
 */
static void
qc_post_parse_analyze(ParseState *pstate, Query *query, JumbleState *jstate)
{
    if (prev_post_parse_analyze_hook)
        prev_post_parse_analyze_hook(pstate, query, jstate);

    /*
     * No read-only check if the current query is in a procedure/function
     * that uses a read-only hint
     */
    if (!query_check_enabled || !check_readonly_enabled || (g_in_exec_func && is_read_only_hint))
        return;

    /*
     * whether the query is used read-only hint in comments
     */
    if (contains_read_only_hint_in_comments(pstate->p_sourcetext))
    {
        is_read_only_hint = true;
        return;
    }

    /*
     * check if the query is read only in standy node
     */
    if (RecoveryInProgress() && !is_read_only_query(query))
    {
        ereport(ERROR,
                errmsg("xproxy not_readonly query"));
    }
}

#if PG_VERSION_NUM < 130000
/*
 * Delete the last element of the list.
 *
 * List is a link list before PostgreSQL 13, 
 * and we need to provide a function to delete the last element.
 */
static List *
list_delete_last(List *list)
{
    ListCell *cell;
    ListCell *prev;
    int i = 0;

    if (list == NULL || list->length == 0)
        return list;

    prev = NULL;
    foreach (cell, list)
    {
        if (i == list->length - 1)
            return list_delete_cell(list, cell, prev);

        i++;
        prev = cell;
    }

    /* Didn't find a match: return the list unmodified */
    return list;
}
#endif

/**
 * ProcessUtility hook: check if the query needs to send message to frontend
 *
 * following statements will be sent to frontend:
 * - create/drop temp table
 * - create temp table as
 * - prepare/deallocate statement
 * - set variable (except local mode)
 * - discard
 */
static void
qc_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
#if PG_VERSION_NUM >= 140000
                  bool readOnlyTree,
#endif
                  ProcessUtilityContext context,
                  ParamListInfo params, QueryEnvironment *queryEnv,
                  DestReceiver *dest,
#if PG_VERSION_NUM >= 130000
                  QueryCompletion *qc
#else
                  char *completionTag
#endif
                  )
{
    Node *node = pstmt->utilityStmt;
    char msg[1024];
	MemoryContext oldcontext = CurrentMemoryContext;

    if (query_check_enabled)
    {
        if (IsA(node, CreateStmt))
        {
            CreateStmt *stmt = (CreateStmt *)node;
            if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP)
            {
                if (stmt->if_not_exists == false ||
                    RangeVarGetRelid(stmt->relation, NoLock, true) == InvalidOid)
                {
                    snprintf(msg, sizeof(msg), "NOTICE: xproxy create temp table %s", stmt->relation->relname);
                    record_in_transaction(msg, false);
                }
            }
        }
        else if (IsA(node, CreateTableAsStmt))
        {
            CreateTableAsStmt *ctas = (CreateTableAsStmt *)node;
            if (ctas->into->rel->relpersistence == RELPERSISTENCE_TEMP)
            {
                if (ctas->if_not_exists == false ||
                    RangeVarGetRelid(ctas->into->rel, NoLock, true) == InvalidOid)
                {
                    snprintf(msg, sizeof(msg), "NOTICE: xproxy create temp table %s", ctas->into->rel->relname);
                    record_in_transaction(msg, false);
                }
            }
        }
        else if (IsA(node, DropStmt))
        {
            /* Delay handling of "drop table" to qc_object_access(), because some dependent temp table will be drop too */
        }
        else if (IsA(node, PrepareStmt))
        {
            PrepareStmt *stmt = (PrepareStmt *)node;
            snprintf(msg, sizeof(msg), "NOTICE: xproxy prepare stmt %s", stmt->name);
            record_in_transaction(msg, true);
        }
        else if (IsA(node, DeallocateStmt))
        {
            DeallocateStmt *stmt = (DeallocateStmt *)node;
            if (stmt->name == NULL)
                snprintf(msg, sizeof(msg), "NOTICE: xproxy deallocate all");
            else
                snprintf(msg, sizeof(msg), "NOTICE: xproxy deallocate stmt %s", stmt->name);

            record_in_transaction(msg, true);
        }
        else if (IsA(node, VariableSetStmt))
        {
            VariableSetStmt *stmt = (VariableSetStmt *)node;

            /*
             * build the message sent to the frontend with
             * the name and value of the GUC parameter
             */
            msg[0] = '\0';
            get_variableset_message(stmt, msg, sizeof(msg));

            /*
             * we do not send the message if the message is NULL
             */
            if (msg[0] != '\0')
                record_in_transaction(msg, false);
        }
        else if (IsA(node, DiscardStmt))
        {
            DiscardStmt *stmt = (DiscardStmt *)node;
            switch (stmt->target)
            {
            case DISCARD_ALL:
                snprintf(msg, sizeof(msg), "NOTICE: xproxy discard all");
                break;
            case DISCARD_PLANS:
                snprintf(msg, sizeof(msg), "NOTICE: xproxy discard plans");
                break;
            case DISCARD_SEQUENCES:
                snprintf(msg, sizeof(msg), "NOTICE: xproxy discard sequences");
                break;
            case DISCARD_TEMP:
                snprintf(msg, sizeof(msg), "NOTICE: xproxy discard temp");
                break;
            default:
                break;
            }

            record_in_transaction(msg, false);
        }
        else if (IsA(node, ListenStmt))
        {
            ListenStmt *stmt = (ListenStmt *)node;
            snprintf(msg, sizeof(msg), "NOTICE: xproxy listen on %s", stmt->conditionname);

            record_in_transaction(msg, false);
        }
        else if (IsA(node, UnlistenStmt))
        {
            UnlistenStmt *stmt = (UnlistenStmt *)node;
            if (stmt->conditionname == NULL)
                snprintf(msg, sizeof(msg), "NOTICE: xproxy unlisten all");
            else
                snprintf(msg, sizeof(msg), "NOTICE: xproxy unlisten on %s", stmt->conditionname);

            record_in_transaction(msg, false);
        }
        else if (IsA(node, DeclareCursorStmt))
        {
            DeclareCursorStmt *stmt = (DeclareCursorStmt *)node;
            if (stmt->options & CURSOR_OPT_HOLD)
            {
                snprintf(msg, sizeof(msg), "NOTICE: xproxy declare cursor %s", stmt->portalname);
                record_in_transaction(msg, false);
            }
        }
        else if (IsA(node, ClosePortalStmt))
        {
            ClosePortalStmt *stmt = (ClosePortalStmt *)node;
            if (stmt->portalname)
            {
                Portal portal = GetPortalByName(stmt->portalname);
                if (portal && portal->cursorOptions & CURSOR_OPT_HOLD)
                {
                    snprintf(msg, sizeof(msg), "NOTICE: xproxy close cursor %s", stmt->portalname);
                    record_in_transaction(msg, true);
                }
            }
            else
            {
                snprintf(msg, sizeof(msg), "NOTICE: xproxy close all");
                record_in_transaction(msg, true);
            }
        }
    }

    PG_TRY();
    {
        if (prev_ProcessUtility)
            prev_ProcessUtility(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
                                readOnlyTree,
#endif
                                context, params, queryEnv,
                                dest,
#if PG_VERSION_NUM >= 130000
                                qc
#else
                                completionTag
#endif
                                );
        else
            standard_ProcessUtility(pstmt, queryString,
#if PG_VERSION_NUM >= 140000
                                    readOnlyTree,
#endif
                                    context, params, queryEnv,
                                    dest,
#if PG_VERSION_NUM >= 130000
                                    qc
#else
                                    completionTag
#endif
                                    );
    }
    PG_CATCH();
    {
	    ErrorData  *edata;
	    /* Save error info */
	    MemoryContextSwitchTo(oldcontext);
	    edata = CopyErrorData();

        /* If the query attemps to delete a non-existent stmt, possible cases are: */
        /* 1. This statement was indeed not created previously. Simply swallow the error */
        /* 2. A statement renamed by xproxy. Send notice and xproxy will deal with it */
        if (IsA(node, DeallocateStmt) && strcmp(edata->funcname,"FetchPreparedStatement") == 0) {
		    // EmitErrorReport();
            FlushErrorState();
		    FreeErrorData(edata);
            return ;
        }  
        else {
            /*
            * If there is an error, we need to pop the last record from session_records
            */
            if (IsA(node, PrepareStmt) || IsA(node, DeallocateStmt) || IsA(node, ClosePortalStmt))
                session_records = list_delete_last(session_records);
            
            PG_RE_THROW();
        }
    }
    PG_END_TRY();
}

static void qc_initialize_advisory_functions_once(void)
{
	if (likely(advisory_lock_function_list[0].oid != InvalidOid))
		return;

	for (AdvisoryFunction *func = &advisory_lock_function_list[0];
		 func->name;
		 ++func)
	{
		PG_TRY();
		{
			func->oid = DirectFunctionCall1(regprocedurein, PointerGetDatum(func->name));
		}
		PG_CATCH();
		{
			EmitErrorReport();
			FlushErrorState();
		}
		PG_END_TRY();
	}
}
/*
 * object_access hook: check if the query has target function call
 */
static void
qc_object_access(ObjectAccessType access,
                 Oid classId,
                 Oid objectId,
                 int subId,
                 void *arg)
{
    if (prev_object_access_hook)
        (*prev_object_access_hook)(access, classId, objectId, subId, arg);

    if (!query_check_enabled)
        return;
    if (access == OAT_FUNCTION_EXECUTE)
    {
        qc_initialize_advisory_functions_once();

        for (AdvisoryFunction *func = &advisory_lock_function_list[0];
             func->name;
             ++func)
        {
            if (func->oid == objectId)
            {
                advisory_lock_function_in_progress = true;
                break;
            }
        }
    }
    else if (access == OAT_DROP && classId == RelationRelationId)
    {
        Relation rel;

        rel = try_relation_open(objectId, NoLock);
        if (rel)
        {
            if ((rel->rd_rel->relkind == RELKIND_RELATION ||
                 rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE) &&
                rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "NOTICE: xproxy drop temp table %s",
                         RelationGetRelationName(rel));
                record_in_transaction(msg, false);
            }
            relation_close(rel, NoLock);
        }
    }
    else if (access == OAT_POST_CREATE && classId == RelationRelationId)
    {
        Relation rel;

        rel = try_relation_open(objectId, NoLock);
        if (rel)
        {
            if ((rel->rd_rel->relkind == RELKIND_RELATION ||
                 rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE) &&
                rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "NOTICE: xproxy create temp table %s",
                         RelationGetRelationName(rel));
                record_in_transaction(msg, false);
            }
            relation_close(rel, NoLock);
        }
    }
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
qc_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
               bool execute_once)
{
    g_in_exec_func++;
    PG_TRY();
    {
        if (prev_ExecutorRun)
            prev_ExecutorRun(queryDesc, direction, count, execute_once);
        else
            standard_ExecutorRun(queryDesc, direction, count, execute_once);
    }
    PG_CATCH();
    {
        g_in_exec_func--;
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/*
 * ExecutorFinish hook: 
 * (1) we need do is track nesting depth
 * (2) if the query is using pg_advisory_lock related functions,
 * we need to send message to frontend
 */
static void
qc_ExecutorFinish(QueryDesc *queryDesc)
{
    g_in_exec_func--;
    PG_TRY();
    {
        if (prev_ExecutorFinish)
            prev_ExecutorFinish(queryDesc);
        else
            standard_ExecutorFinish(queryDesc);
    }
    PG_CATCH();
    {
        g_in_exec_func++;
        PG_RE_THROW();
    }
    PG_END_TRY();

    /*
     * if the query is using pg_advisory_lock related functions,
     * we need to determine whether a session-level advisory lock held by the current session. 
     */
    if (query_check_enabled && advisory_lock_function_in_progress)
    {
        if (!planstate_tree_walker(queryDesc->planstate, has_advisory_lock_function_call, NULL))
            has_advisory_lock_function_call(queryDesc->planstate, NULL);
    }
}

/*
 * Returns true if the SQL statement is regarded as read SELECT from syntax's
 * point of view. However callers need to do additional checking such as if the
 * SELECT does not have write functions or not to make sure that the SELECT is
 * semantically read SELECT.
 *
 * For followings this function returns true:
 * - SELECT/WITH without FOR UPDATE/SHARE
 * - COPY TO STDOUT
 * - EXPLAIN
 * - EXPLAIN ANALYZE and query is SELECT not including writing functions
 *
 * Note that for SELECT INTO this function returns false.
 */
static bool
is_read_only_query(Query *query)
{
    if (query == NULL)
        return false;

    if (query->commandType == CMD_SELECT)
    {
        /* SELECT FOR UPDATE/SHARE */
        if (query->hasForUpdate)
            return false;

        /* Check if there's a WITH clause */
        if (query->cteList)
        {
            ListCell *cte_item;
            foreach (cte_item, query->cteList)
            {
                CommonTableExpr *cte = (CommonTableExpr *)lfirst(cte_item);

                /* Ensure each CTE query is a SELECT query */
                if (!IsA(cte->ctequery, Query) ||
                    ((Query *)cte->ctequery)->commandType != CMD_SELECT)
                {
                    return false;
                }
            }
        }

        /* check if the select query has write function calls or system catalog tables */
        if (qc_has_function_call_or_system_catalog(query))
            return false;

        return true;
    }
    else if (query->commandType == CMD_UTILITY)
    {
        Node *node = query->utilityStmt;

        /*
         * it was written as SELECT INTO
         */
        if (IsA(node, CreateTableAsStmt))
        {
            CreateTableAsStmt *ctas = (CreateTableAsStmt *)node;
            if (ctas->is_select_into)
                return false;
        }
        else if (IsA(node, CopyStmt))
        {
            CopyStmt *copy_stmt = (CopyStmt *)node;

            if (copy_stmt->is_from)
                return false;
            else if (copy_stmt->filename == NULL)
            {
                if (copy_stmt->query == NULL)
                    return true;
                else if (copy_stmt->query && IsA(copy_stmt->query, SelectStmt))
                    return true;
                else
                    return false;
            }
            else
                return false;
        }
        else if (IsA(node, ExplainStmt))
        {
            ExplainStmt *explain_stmt = (ExplainStmt *)node;
            Query *equery = (Query *)explain_stmt->query;
            ListCell *lc;
            bool analyze = false;

            /* Check to see if this is EXPLAIN ANALYZE */
            foreach (lc, explain_stmt->options)
            {
                DefElem *opt = (DefElem *)lfirst(lc);

                if (strcmp(opt->defname, "analyze") == 0)
                {
                    analyze = true;
                    break;
                }
            }

            if (equery->commandType == CMD_SELECT)
            {
                /*
                 * If query is SELECT and there's no ANALYZE option, we can always
                 * load balance.
                 */
                if (!analyze)
                    return true;

                /*
                 * If ANALYZE, we need to check function calls.
                 */
                if (qc_has_function_call_or_system_catalog(equery))
                    return false;
                return true;
            }
            else
            {
                /*
                 * Other than SELECT can be load balance only if ANALYZE is not
                 * specified.
                 */
                if (!analyze)
                    return true;
            }
        }
        else if (IsA(node, VariableSetStmt) || IsA(node, VariableShowStmt))
        {
            return true;
        }
    }

    return false;
}

/**
 * Initialize the system_function_write_list and system_catalog_need_primary_list.
 */
static void
qc_initialize_system_object_once(void)
{
    SystemObject *sys_obj;

    if (likely(system_function_write_list[0].oid != InvalidOid) ||
        likely(system_catalog_need_primary_list[0].oid != InvalidOid))
        return;

    for (sys_obj = &system_function_write_list[0]; sys_obj->name; ++sys_obj)
    {
        PG_TRY();
        {
            sys_obj->oid = DirectFunctionCall1(regprocedurein, PointerGetDatum(sys_obj->name));
        }
        PG_CATCH();
        {
            EmitErrorReport();
            FlushErrorState();
        }
        PG_END_TRY();
    }

    for (sys_obj = &system_catalog_need_primary_list[0]; sys_obj->name; ++sys_obj)
    {
        PG_TRY();
        {
            sys_obj->oid = DirectFunctionCall1(regclassin, PointerGetDatum(sys_obj->name));
        }
        PG_CATCH();
        {
            EmitErrorReport();
            FlushErrorState();
        }
        PG_END_TRY();
    }
}

/*
 * Check if the system object is in the system_function_write_list and system_catalog_need_primary_list.
 */
static bool
qc_system_object_is_in_blacklists(Oid objectId, bool is_function)
{
    SystemObject *sys_obj;

    if (objectId == InvalidOid)
        return false;

    /* this function is a system function which performs write operations */
    for (sys_obj = &system_function_write_list[0]; is_function && sys_obj->name; ++sys_obj)
    {
        if (sys_obj->oid == objectId)
        {
            return true;
        }
    }

    /* this relation is a system catalog which needs to search from the primary node */
    for (sys_obj = &system_catalog_need_primary_list[0]; sys_obj->name; ++sys_obj)
    {
        if (sys_obj->oid == objectId)
        {
            return true;
        }
    }

    return false;
}

/*
 * Check if the query has function call or system catalog in it.
 */
static bool
qc_has_function_call_or_system_catalog(Query *query)
{
    SelectContext ctx;

    if (query->commandType != CMD_SELECT)
        return false;

    ctx.has_function_call = false;
    ctx.has_system_catalog = false;

    qc_initialize_system_object_once();

    query_tree_walker(query, qc_query_tree_walker, &ctx, QTW_EXAMINE_RTES_BEFORE);

    return ctx.has_function_call || ctx.has_system_catalog;
}

/*
 * Query tree walker to check if there's function call or system catalog in the query.
 */
static bool
qc_query_tree_walker(Node *node, void *context)
{
    SelectContext *ctx = (SelectContext *)context;

    if (node == NULL)
        return false;

    if (IsA(node, FuncExpr))
    {
        FuncExpr *funcExpr = (FuncExpr *)node;
        Oid funcid = funcExpr->funcid;

        /* this function is a user-defined function */
        if (funcid > FirstNormalObjectId || qc_system_object_is_in_blacklists(funcid, true))
        {
            ctx->has_function_call = true;
            return true;
        }
    }
    else if (IsA(node, RangeTblFunction))
    {
        RangeTblFunction *rtfunc = (RangeTblFunction *)node;
        FuncExpr *funcExpr = (FuncExpr *)rtfunc->funcexpr;
        Oid funcid = funcExpr->funcid;

        /* this function is a user-defined function */
        if (funcid > FirstNormalObjectId || qc_system_object_is_in_blacklists(funcid, true))
        {
            ctx->has_function_call = true;
            return true;
        }
    }
    else if (IsA(node, RangeTblEntry))
    {
        RangeTblEntry *rte = (RangeTblEntry *)node;

        if (rte->rtekind == RTE_RELATION && rte->relid < FirstNormalObjectId &&
            qc_system_object_is_in_blacklists(rte->relid, false))
        {
            ctx->has_system_catalog = true;
            return true;
        }

        return false;
    }
    else if (IsA(node, Query))
    {
        return query_tree_walker((Query *)node, qc_query_tree_walker, ctx, QTW_EXAMINE_RTES_BEFORE);
    }

    return expression_tree_walker(node, qc_query_tree_walker, ctx);
}

/*
 * A planstate_tree_walker to check if there's advisory lock function call in the plan.
 */
static bool
has_advisory_lock_function_call(PlanState *planstate, void *context)
{
    Plan *plan = planstate->plan;

    (void) context;
    if (plan == NULL)
        return false;

    if (expression_tree_walker((Node *)plan->targetlist, manage_session_locks, NULL) || 
        expression_tree_walker((Node *)plan->qual, manage_session_locks, NULL))
    {
        return true;
    }

    return false;
}

static bool extract_advisory_lock_key(List *args, LOCALLOCKTAG *lock)
{
    int64 keys[2];
    int nkeys = 0;
    ListCell *lc;

    if (list_length(args) == 0 || list_length(args) > 2)
        return false;

    foreach (lc, args)
    {
        Const *constArg = lfirst(lc);
        if (!IsA(constArg, Const) ||
            (constArg->consttype != INT4OID && constArg->consttype != INT8OID))
        {
            hold_unknown_session_locks = true;
            return false;
        }
        keys[nkeys++] = DatumGetInt64(constArg->constvalue);
    }

    if (nkeys == 1)
        SET_LOCKTAG_INT64(lock->lock, keys[0]);
    else
        SET_LOCKTAG_INT32(lock->lock, keys[0], keys[1]);

    return true;
}

/*
 * A expression_tree_walker to check if there's advisory lock function call in the expression.
 */
static bool
manage_session_locks(Node *node, void *context)
{
    (void) context;
    if (node == NULL)
        return false;

    if (IsA(node, FuncExpr))
    {
        FuncExpr *funcexpr = (FuncExpr *)node;
        AdvisoryFunction *func = NULL;

        for (AdvisoryFunction *p = &advisory_lock_function_list[0];
             p->name; ++p)
        {
            if (p->oid == funcexpr->funcid)
            {
                func = p;
                break;
            }
        }

        if (func)
        {
            MemoryContext old_context;

            old_context = MemoryContextSwitchTo(session_locks_ctx);
            /* if the function is the pg_advisory_unlock_all, free the session_locks */
            if (strcmp(func->name, "pg_advisory_unlock_all()") == 0)
            {
                session_locks = NULL;
                hold_unknown_session_locks = false;
                MemoryContextReset(session_locks_ctx);
            }
            else if (!hold_unknown_session_locks)
            {
                LOCALLOCKTAG localtag;

                if (extract_advisory_lock_key(funcexpr->args, &localtag))
                {
                    /* use the function name with 'shared' or not to determine the lock mode */
                    localtag.mode = func->shared ? ShareLock : ExclusiveLock;

                    /*
                     * if the function is pg_advisory_unlock or pg_advisory_unlock_shared,
                     * we need to remove the lock from session_locks.
                     */
                    if (func->lock)
                    {
                        LOCALLOCKTAG *plocaltag;
                        plocaltag = (LOCALLOCKTAG *)palloc(sizeof(struct LOCALLOCKTAG));
                        *plocaltag = localtag;
                        session_locks = lappend(session_locks, plocaltag);
                    }
                    else
                    {
#if PG_VERSION_NUM < 130000
                        ListCell *lc,
                                 *prev;
                        prev = NULL;
#else
                        ListCell *lc;
#endif

                        foreach (lc, session_locks)
                        {
                            LOCALLOCKTAG *llt = (LOCALLOCKTAG *)lfirst(lc);
                            if (llt->mode == localtag.mode && COMPARE_LOCKTAG(llt->lock, localtag.lock))
                            {
                                session_locks = list_delete_cell(session_locks, lc
#if PG_VERSION_NUM < 130000
                                                                 , prev
#endif
                                );
                                break;
                            }
#if PG_VERSION_NUM < 130000
                            prev = lc;
#endif
                        }
                    }
                }
                else
                {
                    /* if failed to extract advisory lock key, there is no need to track which locks have been
                       acquired in the current session until pg_advisory_unlock_all() has been called */
                    if (hold_unknown_session_locks)
                    {
                        session_locks = NULL;
                        MemoryContextReset(session_locks_ctx);
                    }
                }
            }
            MemoryContextSwitchTo(old_context);
        }
    }

    return expression_tree_walker(node, manage_session_locks, NULL);
}

#if PG_VERSION_NUM < 140000
/*
 * Append a text string to the error report being built for the client.
 * (import of the PostgreSQL private fuction)
 */
static void
err_sendstring(StringInfo buf, const char *str)
{
	if (in_error_recursion_trouble())
		pq_send_ascii_string(buf, str);
	else
		pq_sendstring(buf, str);
}
#endif

/*
 * Write notice report to client
 */
static void
qc_send_message_to_frontend(const char *message)
{
    StringInfoData msgbuf;

#if PG_VERSION_NUM < 140000
    /* 'N' (Notice) is for nonfatal conditions */
    pq_beginmessage(&msgbuf, 'N');

    if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
    {
#else
    /*
     * We no longer support pre-3.0 FE/BE protocol, except here.  If a client
     * tries to connect using an older protocol version, it's nice to send the
     * "protocol version not supported" error in a format the client
     * understands.  If protocol hasn't been set yet, early in backend
     * startup, assume modern protocol.
     */
    if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3 || FrontendProtocol == 0)
    {
        pq_beginmessage(&msgbuf, 'N');
#endif

        pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_PRIMARY);
        if (message)
            pq_sendstring(&msgbuf, message);
        else
            pq_sendstring(&msgbuf, _("missing error text"));

        pq_sendbyte(&msgbuf, '\0'); /* terminator */

#if PG_VERSION_NUM >= 140000
        pq_endmessage(&msgbuf);
#endif
    }
    else
    {
        /* Old style --- gin up a backwards-compatible message */
        StringInfoData buf;

        initStringInfo(&buf);

        if (message)
            appendStringInfoString(&buf, message);
        else
            appendStringInfoString(&buf, _("missing error text"));

        appendStringInfoChar(&buf, '\n');

#if PG_VERSION_NUM < 140000
        err_sendstring(&msgbuf, buf.data);
#else
        /* 'N' (Notice) is for nonfatal conditions */
        pq_putmessage_v2('N', buf.data, buf.len + 1);
#endif

        pfree(buf.data);
    }

#if PG_VERSION_NUM < 140000
    pq_endmessage(&msgbuf);
#endif

    /*
     * This flush is normally not necessary, since postgres.c will flush out
     * waiting data when control returns to the main loop. But it seems best
     * to leave it here, so that the client has some clue what happened if the
     * backend dies before getting back to the main loop ... error/notice
     * messages should not be a performance-critical path anyway, so an extra
     * flush won't hurt much ...
     */
    pq_flush();
}

/*
 * Parse query source text, and check if it contains read-only hint.
 */
static bool
contains_read_only_hint_in_comments(const char *str)
{
    const char *start_comment = "/*+";
    const char *end_comment = "*/";
    const char *readonly = "READ-ONLY";

    const char *start;
    const char *end;
    while ((start = strstr(str, start_comment)) != NULL)
    {
        /* if hint string is not a comment, break */
        size_t prev_hint_length = start - str;
        size_t comment_length;
        char *prev_hint = palloc0(prev_hint_length + 1);
        char *comment;
        if (prev_hint_length > 0)
        {
            size_t single_quote_cnt = 0;
            size_t double_quote_cnt = 0;
            strncpy(prev_hint, str, prev_hint_length);
            for (size_t i = 0; i < prev_hint_length; i++)
            {
                if (prev_hint[i] == '\'')
                    single_quote_cnt++;
                else if (prev_hint[i] == '\"')
                    double_quote_cnt++;
            }

            if ((single_quote_cnt % 2 == 1) || (double_quote_cnt % 2 == 1))
                break;
        }

        pfree(prev_hint);

        end = strstr(start, end_comment);
        if (end == NULL)
            break;

        comment_length = end - (start + 3);
        comment = palloc0(comment_length + 1);
        strncpy(comment, start + 3, comment_length);
        comment[comment_length] = '\0';

        if (strcasestr(comment, readonly) != NULL)
        {
            pfree(comment);
            return true;
        }

        str = end + 2;
        pfree(comment);
    }

    return false;
}

/*
 * Get the notice message of variable set statement.
 */
static void
get_variableset_message(VariableSetStmt *stmt, char *msg, int size)
{
    const char *value = NULL;

    /* 
     * we do not need to check the GUC parameter which flag is GUC_REPORT
     * or set in LOCAL mode.
     */
    if(stmt->is_local ||
        (stmt->kind != VAR_RESET_ALL &&
        (GetConfigOptionFlags(stmt->name, true) & GUC_REPORT)))
        return;

    switch (stmt->kind)
    {
        case VAR_SET_VALUE:
            value = flatten_set_variable_args(stmt->args);
            snprintf(msg, size, "NOTICE: xproxy set variable %s to %s", stmt->name, value);
            break;
        case VAR_RESET_ALL:
            snprintf(msg, size, "NOTICE: xproxy reset all");
            break;
        case VAR_SET_CURRENT:
            snprintf(msg, size, "NOTICE: xproxy set variable %s from current", stmt->name);
            break;
        case VAR_SET_DEFAULT:
        case VAR_RESET:
            snprintf(msg, size, "NOTICE: xproxy reset variable %s", stmt->name);
        default:
            break;
    }
}

/*
 * flatten_set_variable_args (import of the PostgreSQL private fuction)
 *		Given a parsenode List as emitted by the grammar for SET,
 *		convert to the flat string representation used by GUC.
 *
 * The result is NULL if args is NIL (ie, SET ... TO DEFAULT), otherwise
 * a palloc'd string.
 */
static char *
flatten_set_variable_args(List *args)
{
    StringInfoData buf;
    ListCell *l;

    /* Fast path if just DEFAULT */
    if (args == NIL)
        return NULL;

    initStringInfo(&buf);

    /*
     * Each list member may be a plain A_Const node, or an A_Const within a
     * TypeCast; the latter case is supported only for ConstInterval arguments
     * (for SET TIME ZONE).
     */
    foreach (l, args)
    {
        Node *arg = (Node *)lfirst(l);
        char *val;
        A_Const *con;

        if (l != list_head(args))
            appendStringInfoString(&buf, ", ");

        if (IsA(arg, TypeCast))
        {
            TypeCast *tc = (TypeCast *)arg;

            arg = tc->arg;
        }

        if (!IsA(arg, A_Const))
            elog(ERROR, "unrecognized node type: %d", (int)nodeTag(arg));
        con = (A_Const *)arg;

        switch (nodeTag(&con->val))
        {
            case T_Integer:
                appendStringInfo(&buf, "%d", intVal(&con->val));
                break;
            case T_Float:
                /* represented as a string, so just copy it */
                appendStringInfoString(&buf, strVal(&con->val));
                break;
            case T_String:
                val = strVal(&con->val);
                simple_quote_literal(&buf, val);
                break;
            default:
                ereport(ERROR, (errmsg("unrecognized node type: %d",
                                    (int)nodeTag(&con->val))));
                break;
        }
    }

    return buf.data;
}

/*
 * simple_quote_literal - Format a string as a SQL literal, append to buf
 */
static void
simple_quote_literal(StringInfo buf, const char *val)
{
	const char *valptr;

	appendStringInfoChar(buf, '\'');
	for (valptr = val; *valptr; valptr++)
	{
		char		ch = *valptr;

		if (SQL_STR_DOUBLE(ch, true))
			appendStringInfoChar(buf, ch);
		appendStringInfoChar(buf, ch);
	}
	appendStringInfoChar(buf, '\'');
}

/*
 * Record the notice message of the query if it's in a transaction.
 */
static void
record_in_transaction(const char *record, bool is_session_level)
{
    MemoryContext old_context = CurrentMemoryContext;

    /*
     * If it's a session level statement, we just append the record to session_records. 
     */
    if (is_session_level)
    {
        MemoryContextSwitchTo(session_records_ctx);
        session_records = lappend(session_records, pstrdup(record));
    }
    /*
     * If it's a sub-transaction, we need to find the corresponding trans_records_item,
     * and append the record to its records.
     */
    else
    {
        MemoryContextSwitchTo(trans_records_ctx);

        if (CurrentTransRecords == NULL)
        {
            CurrentTransRecords = (TransRecords *)palloc0(sizeof(TransRecords));
            trans_records = lappend(trans_records, CurrentTransRecords);
        }

        CurrentTransRecords->records = lappend(CurrentTransRecords->records, pstrdup(record));
    }

    MemoryContextSwitchTo(old_context);
}

/*
 * Xact callback function to send notice message when transaction is committed.
 */
static void
qc_xact_callback(XactEvent event, void *arg)
{
    ListCell *lc;
    char *record;

    if (!query_check_enabled)
        return;

    if (event == XACT_EVENT_PRE_COMMIT ||
        event == XACT_EVENT_PARALLEL_PRE_COMMIT)
    {
        TransRecords *subtrans_records;

        foreach (lc, trans_records)
        {
            ListCell *sub_lc;
            subtrans_records = (TransRecords *)lfirst(lc);
            foreach (sub_lc, subtrans_records->records)
            {
                record = (char *)lfirst(sub_lc);
                qc_send_message_to_frontend(record);
            }
        }
    }

    if (trans_records)
    {
        MemoryContextReset(trans_records_ctx);
        trans_records = NULL;
        CurrentTransRecords = NULL;
    }

    /*
     * Whether the transaction is executed successfully or not, 
     * we send the notice messages of prepare statements to frontend. 
     */
    foreach (lc, session_records)
    {
        record = (char *)lfirst(lc);
        qc_send_message_to_frontend(record);
    }

    if (session_records)
    {
        MemoryContextReset(session_records_ctx);
        session_records = NULL;
    }

    /*
     * if a advisory lock function is in progress, we send the notice message of 
     * whether locks held by the current session.
     */
    if (advisory_lock_function_in_progress)
    {
        if (hold_unknown_session_locks)
            qc_send_message_to_frontend("NOTICE: xproxy session level advisory locks held by the current session");
        else if (session_locks == NIL)
            qc_send_message_to_frontend("NOTICE: xproxy no session level advisory locks held by the current session");
        else
        {
            foreach (lc, session_locks)
            {
                LOCALLOCKTAG *localtag = (LOCALLOCKTAG *)lfirst(lc);
                if (LockHeldByMe(&localtag->lock, localtag->mode
#if PG_VERSION_NUM >= 170000
                                 , false
#endif
                ))
                {
                    char *msg = "NOTICE: xproxy session level advisory locks held by the current session";
                    qc_send_message_to_frontend(msg);
                    break;
                }
            }
        }

        advisory_lock_function_in_progress = false;
    }
}

/*
 * SubXact callback function to reset notice messages when transaction is rollback.
 */
static void
qc_sub_xact_callback(SubXactEvent event, SubTransactionId mySubid,
                     SubTransactionId parentSubid, void *arg)
{
    MemoryContext old_context;

    if (!query_check_enabled)
        return;

    old_context = MemoryContextSwitchTo(trans_records_ctx);

    /*
     * When a sub-transaction starts, we create a new TransRecordsItem and SubTransRecord.
     */
    if (event == SUBXACT_EVENT_START_SUB)
    {
        TransRecords *subtrans_records;

        subtrans_records = (TransRecords *)palloc(sizeof(TransRecords));
        subtrans_records->records = NIL;
        subtrans_records->parent = CurrentTransRecords;
        trans_records = lappend(trans_records, subtrans_records);
        CurrentTransRecords = subtrans_records;
    }
    /*
     * When a sub-transaction is aborted, we free the records of this sub-transaction.
     */
    else if (event == SUBXACT_EVENT_ABORT_SUB)
    {
        ListCell *lc = list_tail(trans_records);
        TransRecords *subtrans_records = lfirst(lc);

        Assert(subtrans_records == CurrentTransRecords);
        CurrentTransRecords = subtrans_records->parent;
#if PG_VERSION_NUM < 130000
        trans_records = list_delete_last(trans_records);
#else
        trans_records = list_delete_cell(trans_records, lc);
#endif

        list_free_deep(subtrans_records->records);
        pfree(subtrans_records);
    }
    else if (event == SUBXACT_EVENT_COMMIT_SUB)
    {
        ListCell *lc = list_tail(trans_records);
        TransRecords *subtrans_records = lfirst(lc);

        Assert(subtrans_records == CurrentTransRecords);
        CurrentTransRecords = subtrans_records->parent;
        if (CurrentTransRecords)
        {
            #if PG_VERSION_NUM < 130000
                    trans_records = list_delete_last(trans_records);
            #else
                    trans_records = list_delete_cell(trans_records, lc);
            #endif

            if (subtrans_records->records)
            {
                CurrentTransRecords->records =
                    list_concat(CurrentTransRecords->records, subtrans_records->records);
            }
            pfree(subtrans_records);
        }
        else
        {
            /* Reuse the subtransaction's TransRecords as the parent's*/
            CurrentTransRecords = subtrans_records;
        }
    }

    MemoryContextSwitchTo(old_context);
}

/*
 * Load the dynamic library of query_check for the current session.
 */
Datum load_dynamic_library(PG_FUNCTION_ARGS)
{
    ereport(DEBUG1, (errmsg("load the dynamic library of query_check, and it effects only in the current session")));

    return (Datum)true;
}

/*
 *  Use this check_hook function to prohibit the parameter from being changed to false 
 */
static bool 
query_check_enabled_check_hook(bool *newval, void **extra, GucSource source)
{
    if (query_check_enabled && *newval == false) 
    {
        GUC_check_errdetail("it is not allowed to modify the query_check.enabled to 'off'");
        return false;
    }
    return true;
}