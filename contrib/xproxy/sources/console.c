
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

typedef enum {
	OD_LKILL_CLIENT,
	OD_LRELOAD,
	OD_LSHOW,
	OD_LSTATS,
	OD_LSERVERS,
	OD_LSERVER_PREP_STMTS,
	OD_LCLIENTS,
	OD_LLISTS,
	OD_LHELP,
	OD_LSET,
	OD_LCREATE,
	OD_LDROP,
	OD_LPOOLS,
	OD_LPOOLS_EXTENDED,
	OD_LDATABASES,
	OD_LMODULE,
	OD_LERRORS,
	OD_LERRORS_PER_ROUTE,
	OD_LFRONTEND,
	OD_LROUTER,
	OD_LVERSION,
	OD_LLISTEN,
	OD_LSTORAGES,
	OD_LROUTE,
	OD_LGLOBAL_CONFIG,
	OD_LALL_RULES,  
	OD_LRULES,  
} od_console_keywords_t;

static od_keyword_t od_console_keywords[] = {
	od_keyword("kill_client", OD_LKILL_CLIENT),
	od_keyword("reload", OD_LRELOAD),
	od_keyword("help", OD_LHELP),
	od_keyword("show", OD_LSHOW),
	od_keyword("stats", OD_LSTATS),
	od_keyword("servers", OD_LSERVERS),
	od_keyword("server_prep_stmts", OD_LSERVER_PREP_STMTS),
	od_keyword("clients", OD_LCLIENTS),
	od_keyword("lists", OD_LLISTS),
	od_keyword("set", OD_LSET),
	od_keyword("pools", OD_LPOOLS),
	od_keyword("pools_extended", OD_LPOOLS_EXTENDED),
	od_keyword("databases", OD_LDATABASES),
	od_keyword("create", OD_LCREATE),
	od_keyword("module", OD_LMODULE),
	od_keyword("errors", OD_LERRORS),
	od_keyword("errors_per_route", OD_LERRORS_PER_ROUTE),
	od_keyword("frontend", OD_LFRONTEND),
	od_keyword("router", OD_LROUTER),
	od_keyword("drop", OD_LDROP),
	od_keyword("version", OD_LVERSION),
	od_keyword("listen", OD_LLISTEN),
	od_keyword("storages", OD_LSTORAGES),
	od_keyword("route", OD_LROUTE),
	od_keyword("global_config", OD_LGLOBAL_CONFIG),
	od_keyword("all_rules", OD_LALL_RULES),  
	od_keyword("rules", OD_LRULES),  
	{ 0, 0, 0 }
};

static inline int od_console_show_stats_add(machine_msg_t *stream,
					    char *database, int database_len,
					    od_stat_t *total, od_stat_t *avg)
{
	assert(stream);
	int offset;
	machine_msg_t *msg;
	msg = kiwi_be_write_data_row(stream, &offset);
	if (msg == NULL)
		return NOT_OK_RESPONSE;
	int rc;
	rc = kiwi_be_write_data_row_add(stream, offset, database, database_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	char data[64];
	int data_len;
	/* total_xact_count */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, total->count_tx);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* total_query_count */
	data_len =
		od_snprintf(data, sizeof(data), "%" PRIu64, total->count_query);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* total_received */
	data_len =
		od_snprintf(data, sizeof(data), "%" PRIu64, total->recv_client);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* total_sent */
	data_len =
		od_snprintf(data, sizeof(data), "%" PRIu64, total->recv_server);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* total_xact_time */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, total->tx_time);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* total_query_time */
	data_len =
		od_snprintf(data, sizeof(data), "%" PRIu64, total->query_time);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* total_pool_wait_time */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, total->pool_wait_time);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* avg_xact_count */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, avg->count_tx);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* avg_query_count */
	data_len =
		od_snprintf(data, sizeof(data), "%" PRIu64, avg->count_query);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* avg_recv */
	data_len =
		od_snprintf(data, sizeof(data), "%" PRIu64, avg->recv_client);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* avg_sent */
	data_len =
		od_snprintf(data, sizeof(data), "%" PRIu64, avg->recv_server);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* avg_xact_time */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, avg->tx_time);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* avg_query_time */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, avg->query_time);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* avg_pool_wait_time */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, avg->pool_wait_time);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* count of backend parse msgs */
	data_len =
		od_snprintf(data, sizeof(data), "%" PRIu64, total->count_parse);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* count of backend parse msgs reuse */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
			       total->count_parse_reuse);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* count of read/write split */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
			       total->count_rwsplit);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
			       total->count_rwsplit_wrong);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
			       total->count_rwsplit_blacklist_hit);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	return 0;
}

static inline od_retcode_t
od_console_show_frontend_stats_err_add(machine_msg_t *stream,
				       od_route_pool_t *route_pool)
{
	assert(stream);

	for (size_t i = 0; i < OD_FRONTEND_STATUS_ERRORS_TYPES_COUNT; ++i) {
		int offset;
		int rc;
		machine_msg_t *msg;

		msg = kiwi_be_write_data_row(stream, &offset);
		if (msg == NULL)
			return NOT_OK_RESPONSE;

		size_t total_count = od_err_logger_get_aggr_errors_count(
			route_pool->err_logger, od_frontend_status_errs[i]);

		char *err_type =
			od_frontend_status_to_str(od_frontend_status_errs[i]);

		rc = kiwi_be_write_data_row_add(stream, offset, err_type,
						strlen(err_type));
		if (rc != OK_RESPONSE) {
			return rc;
		}
		char data[64];
		int data_len;
		/* error_type */
		data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
				       total_count);

		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) {
			return rc;
		}
	}

	return OK_RESPONSE;
}

static inline int
od_console_show_router_stats_err_add(machine_msg_t *stream,
				     od_error_logger_t *err_logger)
{
	assert(stream);

	for (size_t i = 0; i < OD_ROUTER_STATUS_ERRORS_TYPES_COUNT; ++i) {
		int offset;
		int rc;
		machine_msg_t *msg;

		msg = kiwi_be_write_data_row(stream, &offset);
		if (msg == NULL) {
			return NOT_OK_RESPONSE;
		}

		char *err_type =
			od_router_status_to_str(od_router_status_errs[i]);

		rc = kiwi_be_write_data_row_add(stream, offset, err_type,
						strlen(err_type));
		if (rc != OK_RESPONSE) {
			return rc;
		}

		/* error_type */
		char data[64];
		int data_len;
		data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
				       od_err_logger_get_aggr_errors_count(
					       err_logger,
					       od_router_status_errs[i]));

		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) {
			return rc;
		}
	}

	return OK_RESPONSE;
}

static int od_console_show_stats_cb(char *database, int database_len,
				    od_stat_t *total, od_stat_t *avg,
				    void **argv)
{
	machine_msg_t *stream = argv[0];
	return od_console_show_stats_add(stream, database, database_len, total,
					 avg);
}

static int od_console_show_err_frontend_stats_cb(od_route_pool_t *pool,
						 void **argv)
{
	machine_msg_t *stream = argv[0];
	return od_console_show_frontend_stats_err_add(stream, pool);
}

static int od_console_show_err_router_stats_cb(od_error_logger_t *l,
					       void **argv)
{
	machine_msg_t *stream = argv[0];
	return od_console_show_router_stats_err_add(stream, l);
}

static inline int od_console_show_help(od_client_t *client,
				       machine_msg_t *stream)
{
	assert(stream);
	(void)client;

	char *message =
		"\n"
		"Console usage\n"
		"\tSHOW STATS|HELP|POOLS|POOLS_EXTENDED|DATABASES|SERVER_PREP_STMTS|SERVERS|CLIENTS\n"
		"\tSHOW LISTS|ERRORS|ERRORS_PER_ROUTE|VERSION|LISTEN|STORAGES|RULES|GLOBAL_CONFIG\n"
		"\tKILL_CLIENT <client_id>\n"
		"\tRELOAD\n"
		"\tSET key=arg\n"
		"\tCREATE <module_path>\n"
		"\tDROP SERVERS|MODULE <servers>|<module>";
	stream = kiwi_be_write_notice_console_usage(stream, message);

	int rc = kiwi_be_write_complete(stream, "SHOW", 5);
	return rc;
}

static inline int od_console_show_stats(od_client_t *client,
					machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;
	od_cron_t *cron = client->global->cron;

	if (kiwi_be_write_row_descriptionf(
		    stream, "slllllllllllllllllll", "database", "total_xact_count",
		    "total_query_count", "total_received", "total_sent",
		    "total_xact_time", "total_query_time", "total_pool_wait_time",
		    "avg_xact_count", "avg_query_count", "avg_recv", "avg_sent",
		    "avg_xact_time", "avg_query_time", "avg_pool_wait_time",
		    "total_parse_count", "total_parse_count_reuse",
			"total_rwsplit_count",
			"total_rwsplit_wrong_count",
			"total_rwsplit_blacklist_hit_count") == NULL) {
		return NOT_OK_RESPONSE;
	}

	void *argv[] = { stream };
	od_route_pool_stat_database(&router->route_pool,
				    od_console_show_stats_cb,
				    cron->stat_time_us, argv);

	int rc = kiwi_be_write_complete(stream, "SHOW", 5);
	if (rc == NOT_OK_RESPONSE) {
		return rc;
	}

	return rc;
}

static inline od_retcode_t od_console_show_errors(od_client_t *client,
						  machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;

	void *argv[] = { stream };

	if (kiwi_be_write_row_descriptionf(stream, "sl", "error_type",
					   "count") == NULL) {
		return NOT_OK_RESPONSE;
	}

	int rc;
	rc = od_route_pool_stat_err_router(
		router, od_console_show_err_router_stats_cb, argv);

	if (rc != OK_RESPONSE)
		return rc;

	rc = od_route_pool_stat_err_frontend(
		&router->route_pool, od_console_show_err_frontend_stats_cb,
		argv);

	if (rc != OK_RESPONSE)
		return rc;

	rc = kiwi_be_write_complete(stream, "SHOW", 5);
	return rc;
}

static inline int od_console_show_errors_per_route_cb(od_route_t *route,
						      void **argv)
{
	machine_msg_t *stream = argv[0];
	assert(stream);

	if (!route || !route->extra_logging_enabled) {
		return OK_RESPONSE;
	}

	for (size_t i = 0; i < OD_FRONTEND_STATUS_ERRORS_TYPES_COUNT; ++i) {
		int offset;
		int rc;
		machine_msg_t *msg;
		msg = kiwi_be_write_data_row(stream, &offset);
		if (msg == NULL) {
			/* message was not successfully allocated */
			return NOT_OK_RESPONSE;
		}

		size_t total_count = od_err_logger_get_aggr_errors_count(
			route->err_logger, od_frontend_status_errs[i]);

		char *err_type =
			od_frontend_status_to_str(od_frontend_status_errs[i]);

		rc = kiwi_be_write_data_row_add(stream, offset, err_type,
						strlen(err_type));

		if (rc != OK_RESPONSE) {
			return rc;
		}

		/* route user */

		rc = kiwi_be_write_data_row_add(stream, offset,
						route->rule->user_name,
						strlen(route->rule->user_name));
		if (rc != OK_RESPONSE) {
			return rc;
		}

		/* route database */

		rc = kiwi_be_write_data_row_add(stream, offset,
						route->rule->db_name,
						strlen(route->rule->db_name));
		if (rc != OK_RESPONSE) {
			return rc;
		}

		/* error_type */

		char data[64];
		int data_len;
		data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
				       total_count);

		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) {
			return rc;
		}
	}

	for (size_t i = 0; i < OD_ROUTER_ROUTE_STATUS_ERRORS_TYPES_COUNT; ++i) {
		int offset;
		int rc;
		machine_msg_t *msg;
		msg = kiwi_be_write_data_row(stream, &offset);
		if (msg == NULL)
			return NOT_OK_RESPONSE;

		size_t total_count = od_err_logger_get_aggr_errors_count(
			route->err_logger, od_router_route_status_errs[i]);

		char *err_type =
			od_router_status_to_str(od_router_route_status_errs[i]);

		rc = kiwi_be_write_data_row_add(stream, offset, err_type,
						strlen(err_type));
		if (rc != OK_RESPONSE) {
			return rc;
		}

		/* route user */

		rc = kiwi_be_write_data_row_add(stream, offset,
						route->rule->user_name,
						strlen(route->rule->user_name));
		if (rc != OK_RESPONSE) {
			return rc;
		}

		/* route database */

		rc = kiwi_be_write_data_row_add(stream, offset,
						route->rule->db_name,
						strlen(route->rule->db_name));
		if (rc != OK_RESPONSE) {
			return rc;
		}

		/* error_type */

		char data[64];
		int data_len;
		data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
				       total_count);

		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) {
			return rc;
		}
	}

	return OK_RESPONSE;
}

static inline od_retcode_t
od_console_show_errors_per_route(od_client_t *client, machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;

	void *argv[] = { stream };

	if (kiwi_be_write_row_descriptionf(stream, "sssl", "error_type", "user",
					   "database", "count") == NULL) {
		return NOT_OK_RESPONSE;
	}

	od_router_foreach(router, od_console_show_errors_per_route_cb, argv);

	od_retcode_t rc = kiwi_be_write_complete(stream, "SHOW", 5);
	return rc;
}

static inline int od_console_show_version(machine_msg_t *stream)
{
	assert(stream);

	if (kiwi_be_write_row_descriptionf(stream, "s", "version") == NULL) {
		return NOT_OK_RESPONSE;
	}

	int offset;
	if (kiwi_be_write_data_row(stream, &offset) == NULL) {
		return NOT_OK_RESPONSE;
	}

	char data[128];
	int data_len;
	/* current version and build */
	data_len =
		od_snprintf(data, sizeof(data), "%s-%s-%s", OD_VERSION_NUMBER,
			    OD_VERSION_GIT, OD_VERSION_BUILD);

	int rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE)
		return rc;

	rc = kiwi_be_write_complete(stream, "SHOW", 5);
	return rc;
}

static inline od_retcode_t
od_console_show_quantiles(machine_msg_t *stream, int offset,
			  const int quantiles_count, const double *quantiles,
			  od_histogram_t *transactions_hgram,
			  od_histogram_t *queries_hgram,
			  od_histogram_t *pool_wait_hgram)
{
	char data[64];
	int data_len;
	int rc = OK_RESPONSE;
	double query_pcts[quantiles_count];
	double transaction_pcts[quantiles_count];
	double pool_wait_pcts[quantiles_count];

	if (queries_hgram)
		od_histogram_get_pct_cumulative(queries_hgram,
										quantiles, query_pcts, quantiles_count);
	else
		memset(query_pcts, 0, quantiles_count * sizeof(double));

	if (transactions_hgram)
		od_histogram_get_pct_cumulative(transactions_hgram,
										quantiles, transaction_pcts, quantiles_count);
	else
		memset(transaction_pcts, 0, quantiles_count * sizeof(double));

	if (pool_wait_hgram)
		od_histogram_get_pct_cumulative(pool_wait_hgram,
										quantiles, pool_wait_pcts, quantiles_count);
	else
		memset(pool_wait_pcts, 0, quantiles_count * sizeof(double));

	for (int i = 0; i < quantiles_count; i++) {
		/* query quantile */
		double query_quantile = query_pcts[i];
		double transaction_quantile = transaction_pcts[i];
		double pool_wait_quantile = pool_wait_pcts[i];
		if (isnan(query_quantile)) {
			query_quantile = 0;
		}
		if (isnan(transaction_quantile)) {
			transaction_quantile = 0;
		}
		if (isnan(pool_wait_quantile)) {
			pool_wait_quantile = 0;
		}
		data_len = od_snprintf(data, sizeof(data), "%"PRIu64, (uint64_t)query_quantile);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc == NOT_OK_RESPONSE)
			return rc;
		/* transaction quantile */
		data_len = od_snprintf(data, sizeof(data), "%"PRIu64, (uint64_t)transaction_quantile);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc == NOT_OK_RESPONSE)
			return rc;
		data_len = od_snprintf(data, sizeof(data), "%" PRIu64, (uint64_t)pool_wait_quantile);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc == NOT_OK_RESPONSE)
			return rc;
	}
	return rc;
}

static int show_endpoints_detail(od_route_t *route, machine_msg_t *stream, int offset)
{
	machine_msg_t *buffer = machine_msg_create(0);
	char data[64];
	machine_msg_write(buffer, "{", 1);
	for (int i=0; i<route->server_pool_size; ++i) {
		od_server_pool_t *sv = &route->server_pool[i];
		int len = od_snprintf(data, sizeof(data), "%d/%d/%d",
				sv->count_active, sv->count_idle, route->waiters[i]);
		machine_msg_write(buffer, data, len);
		if (i < route->server_pool_size - 1)
			machine_msg_write(buffer, ", ", 2);
	}
	machine_msg_write(buffer, "}", 1);
	int rc = kiwi_be_write_data_row_add(stream, offset,
			machine_msg_data(buffer),
			machine_msg_size(buffer));
	machine_msg_free(buffer);
	return rc;
}

static inline int od_console_show_pools_add_cb(od_route_t *route, void **argv)
{
	int offset;
	machine_msg_t *stream = argv[0];
	bool *extended = argv[1];
	double *quantiles = argv[2];
	int *quantiles_count = argv[3];
	od_histogram_t **common_transactions_hgram = argv[4];
	od_histogram_t **common_queries_hgram = argv[5];
	od_histogram_t **common_pool_wait_hgram = argv[6];

	machine_msg_t *msg;
	msg = kiwi_be_write_data_row(stream, &offset);
	if (msg == NULL)
		return NOT_OK_RESPONSE;

	od_route_lock(route);
	int rc;
	rc = kiwi_be_write_data_row_add(stream, offset, route->id.database,
					route->id.database_len - 1);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	rc = kiwi_be_write_data_row_add(stream, offset, route->id.user,
					route->id.user_len - 1);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	char data[64];
	int data_len;

	/* cl_active */
	data_len = od_snprintf(data, sizeof(data), "%d",
			       route->client_pool.count_active);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	/* cl_idle */
	data_len = od_snprintf(data, sizeof(data), "%d",
			       route->client_pool.count_pending);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	/* cl_waiting */
	data_len = od_snprintf(data, sizeof(data), "%d",
			       route->client_pool.count_queue);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	/* sv_active */
	data_len = od_snprintf(data, sizeof(data), "%d",
			       od_server_pool_active(route->server_pool, route->server_pool_size));
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	/* sv_idle */
	data_len = od_snprintf(data, sizeof(data), "%d",
			       od_server_pool_idle(route->server_pool, route->server_pool_size));
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	/* sv_used */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, 0UL);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	/* sv_tested */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, 0UL);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	/* sv_login */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, 0UL);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	/* sv_details */
	rc = show_endpoints_detail(route, stream, offset);
	if (rc == NOT_OK_RESPONSE)
		goto error;	
	/* maxwait */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, 0UL);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	/* maxwait_us */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, 0UL);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	/* pool_mode */
	rc = NOT_OK_RESPONSE;

	switch (route->rule->pool->pool) {
	case OD_RULE_POOL_SESSION:
		rc = kiwi_be_write_data_row_add(stream, offset, "session", 7);
		break;
	case OD_RULE_POOL_TRANSACTION:
		rc = kiwi_be_write_data_row_add(stream, offset, "transaction",
						11);
		break;
	default:
		break;
	}

	if (rc == NOT_OK_RESPONSE)
		goto error;

	if (*extended) {
		/* bytes recived */
		data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
				       route->stats.recv_client);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc == NOT_OK_RESPONSE)
			goto error;
		/* bytes sent */
		data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
				       route->stats.recv_server);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc == NOT_OK_RESPONSE)
			goto error;

		/* tcp conn rate */
		data_len = od_snprintf(data, sizeof(data), "%" PRIu64,
				       route->tcp_connections);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc == NOT_OK_RESPONSE)
			goto error;

		if (route->stats.enable_quantiles) {
			*common_queries_hgram =
				od_histogram_merge(*common_queries_hgram, route->stats.query_hgram);
			*common_transactions_hgram =
				od_histogram_merge(*common_transactions_hgram, route->stats.transaction_hgram);
			*common_pool_wait_hgram =
				od_histogram_merge(*common_pool_wait_hgram, route->stats.pool_wait_hgram);
		}
		rc = od_console_show_quantiles(stream, offset, *quantiles_count,
					       quantiles, route->stats.transaction_hgram,
					       route->stats.query_hgram,
						   route->stats.pool_wait_hgram);
		if (rc == NOT_OK_RESPONSE) {
			goto error;
		}
	}
	od_route_unlock(route);
	return 0;
error:
	od_route_unlock(route);
	return NOT_OK_RESPONSE;
}

static inline int od_console_show_databases_add_cb(od_route_t *route,
						   void **argv)
{
	int offset;
	machine_msg_t *stream = argv[0];
	machine_msg_t *msg;
	msg = kiwi_be_write_data_row(stream, &offset);
	if (msg == NULL)
		return NOT_OK_RESPONSE;

	od_route_lock(route);
	int rc;
	rc = kiwi_be_write_data_row_add(stream, offset, route->id.database,
					route->id.database_len - 1);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	od_rule_t *rule = route->rule;
	od_rule_storage_t *storage = rule->storage;

	/* host */
	char *host = storage->host;
	if (!host) {
		host = "";
	}

	rc = kiwi_be_write_data_row_add(stream, offset, host, strlen(host));
	if (rc == NOT_OK_RESPONSE) {
		goto error;
	}

	char data[64];
	int data_len;

	/* port */
	data_len = od_snprintf(data, sizeof(data), "%d", storage->port);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	/* database */
	rc = kiwi_be_write_data_row_add(stream, offset, rule->db_name,
					rule->db_name_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	/* force_user */
	rc = kiwi_be_write_data_row_add(stream, offset, rule->user_name,
					rule->user_name_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	/* pool size */
	data_len = od_snprintf(data, sizeof(data), "%d", rule->pool->size);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	/* reserve_pool */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, 0UL);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	/* pool mode */
	rc = NOT_OK_RESPONSE;
	if (rule->pool->pool == OD_RULE_POOL_SESSION)
		rc = kiwi_be_write_data_row_add(stream, offset, "session", 7);
	if (rule->pool->pool == OD_RULE_POOL_TRANSACTION)
		rc = kiwi_be_write_data_row_add(stream, offset, "transaction",
						11);

	if (rc == NOT_OK_RESPONSE) {
		goto error;
	}

	/* max_connections */
	data_len = od_snprintf(data, sizeof(data), "%d", rule->client_max);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	/* current_connections */
	data_len = od_snprintf(data, sizeof(data), "%d",
			       route->client_pool.count_active +
				       route->client_pool.count_pending +
				       route->client_pool.count_queue);

	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	/* paused */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, 0UL);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	/* disabled */
	data_len = od_snprintf(data, sizeof(data), "%" PRIu64, 0UL);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		goto error;

	od_route_unlock(route);
	return 0;
error:
	od_route_unlock(route);
	return NOT_OK_RESPONSE;
}

static inline int od_console_show_databases(od_client_t *client,
					    machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;

	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(
		stream, "sslssllsllll", "name", "host", "port", "database",
		"force_user", "pool_size", "reserve_pool", "pool_mode",
		"max_connections", "current_connections", "paused", "disabled");
	if (msg == NULL)
		return NOT_OK_RESPONSE;

	void *argv[] = { stream };
	int rc;
	rc = od_router_foreach(router, od_console_show_databases_add_cb, argv);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;

	return kiwi_be_write_complete(stream, "SHOW", 5);
}

static inline int od_console_show_pools(od_client_t *client,
					machine_msg_t *stream, bool extended)
{
	assert(stream);
	int rc;
	od_router_t *router = client->global->router;
	od_route_t *route = client->route;
	double *quantiles = route->rule->quantiles;
	int quantiles_count = route->rule->quantiles_count;

	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(stream, "ssllllllllslls", "database",
					     "user", "cl_active", "cl_idle", "cl_waiting",
					     "sv_active", "sv_idle", "sv_used",
					     "sv_tested", "sv_login", "sv_detail", "maxwait",
					     "maxwait_us", "pool_mode");
	if (msg == NULL)
		return NOT_OK_RESPONSE;

	if (extended) {
		char *bytes_rcv = "bytes_recieved";
		rc = kiwi_be_write_row_description_add(msg, 0, bytes_rcv,
						       strlen(bytes_rcv), 0, 0,
						       23 /* INT4OID */, 4, 0,
						       0);
		if (rc == NOT_OK_RESPONSE)
			return NOT_OK_RESPONSE;
		char *bytes_sent = "bytes_sent";
		rc = kiwi_be_write_row_description_add(msg, 0, bytes_sent,
						       strlen(bytes_sent), 0, 0,
						       23 /* INT4OID */, 4, 0,
						       0);
		if (rc == NOT_OK_RESPONSE)
			return NOT_OK_RESPONSE;

		char *tcp_conn_rate = "tcp_conn_count";
		rc = kiwi_be_write_row_description_add(msg, 0, tcp_conn_rate,
						       strlen(tcp_conn_rate), 0,
						       0, 23 /* INT4OID */, 4,
						       0, 0);
		if (rc == NOT_OK_RESPONSE)
			return NOT_OK_RESPONSE;

		for (int i = 0; i < quantiles_count; i++) {
			char caption[KIWI_MAX_VAR_SIZE];
			int caption_len;
			caption_len = od_snprintf(caption, sizeof(caption),
						  "query_%.6g", quantiles[i]);
			rc = kiwi_be_write_row_description_add(
				msg, 0, caption, caption_len, 0, 0,
				23 /* INT4OID */, 4, 0, 0);
			if (rc == NOT_OK_RESPONSE)
				return NOT_OK_RESPONSE;
			caption_len =
				od_snprintf(caption, sizeof(caption),
					    "transaction_%.6g", quantiles[i]);
			rc = kiwi_be_write_row_description_add(
				msg, 0, caption, caption_len, 0, 0,
				23 /* INT4OID */, 4, 0, 0);
			if (rc == NOT_OK_RESPONSE)
				return NOT_OK_RESPONSE;
			caption_len =
				od_snprintf(caption, sizeof(caption),
					    "pool_wait_%.6g", quantiles[i]);
			rc = kiwi_be_write_row_description_add(
				msg, 0, caption, caption_len, 0, 0,
				23 /* INT4OID */, 4, 0, 0);
			if (rc == NOT_OK_RESPONSE)
				return NOT_OK_RESPONSE;
		}
	}

	od_histogram_t *transactions_hgram = NULL;
	od_histogram_t *queries_hgram = NULL;
	od_histogram_t *pool_wait_hgram = NULL;
	void *argv[] = { stream,	   &extended,	       quantiles,
			 &quantiles_count, &transactions_hgram, &queries_hgram, &pool_wait_hgram };
	rc = od_router_foreach(router, od_console_show_pools_add_cb, argv);
	if (rc == NOT_OK_RESPONSE)
		goto error;
	if (extended) {
		int offset;
		msg = kiwi_be_write_data_row(stream, &offset);
		if (msg == NULL)
			goto error;
		char *aggregated_name = "aggregated";
		rc = kiwi_be_write_data_row_add(stream, offset, aggregated_name,
						strlen(aggregated_name));
		if (rc == NOT_OK_RESPONSE) {
			goto error;
		}
		rc = kiwi_be_write_data_row_add(stream, offset, aggregated_name,
						strlen(aggregated_name));
		if (rc == NOT_OK_RESPONSE) {
			goto error;
		}
		const size_t rest_columns_count = 15;
		for (size_t i = 0; i < rest_columns_count; ++i) {
			rc = kiwi_be_write_data_row_add(stream, offset, NULL,
							NULL_MSG_LEN);
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}
		}
		rc = od_console_show_quantiles(stream, offset, quantiles_count,
					       quantiles, transactions_hgram,
					       queries_hgram,
						   pool_wait_hgram);
		if (rc == NOT_OK_RESPONSE) {
			goto error;
		}
	}
	od_histogram_delete(transactions_hgram);
	od_histogram_delete(queries_hgram);
	od_histogram_delete(pool_wait_hgram);
	return kiwi_be_write_complete(stream, "SHOW", 5);
error:
	od_histogram_delete(transactions_hgram);
	od_histogram_delete(queries_hgram);
	od_histogram_delete(pool_wait_hgram);
	return NOT_OK_RESPONSE;
}

static inline int od_console_show_servers_server_cb(od_server_t *server,
						    void **argv)
{
	od_route_t *route = server->route;

	int offset;
	machine_msg_t *stream = argv[0];
	machine_msg_t *msg;
	msg = kiwi_be_write_data_row(stream, &offset);
	if (msg == NULL)
		return NOT_OK_RESPONSE;
	/* type */
	char data[256];
	size_t data_len;
	od_client_t *client = server->client;
	if (client != NULL && client->type == OD_POOL_CLIENT_INTERNAL) {
		data_len = od_snprintf(data, sizeof(data), "SI");
	} else {
		data_len = od_snprintf(data, sizeof(data), "S");
	}
	int rc;
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* user */
	rc = kiwi_be_write_data_row_add(stream, offset, route->id.user,
					route->id.user_len - 1);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* database */
	rc = kiwi_be_write_data_row_add(stream, offset, route->id.database,
					route->id.database_len - 1);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* state */
	char *state = "";
	if (server->state == OD_SERVER_IDLE)
		state = "idle";
	else if (server->state == OD_SERVER_ACTIVE)
		state = "active";
	rc = kiwi_be_write_data_row_add(stream, offset, state, strlen(state));
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* addr */
	od_getpeername(server->io.io, data, sizeof(data), 1, 0);
	data_len = strlen(data);
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* port */
	od_getpeername(server->io.io, data, sizeof(data), 0, 1);
	data_len = strlen(data);
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* local_addr */
	od_getsockname(server->io.io, data, sizeof(data), 1, 0);
	data_len = strlen(data);
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* local_port */
	od_getsockname(server->io.io, data, sizeof(data), 0, 1);
	data_len = strlen(data);
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* connect_time */
	rc = kiwi_be_write_data_row_add(msg, offset, NULL, -1);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* request_time */
	rc = kiwi_be_write_data_row_add(msg, offset, NULL, -1);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* wait */
	data_len = od_snprintf(data, sizeof(data), "0");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* wait_us */
	data_len = od_snprintf(data, sizeof(data), "0");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* ptr */
	data_len =
		od_snprintf(data, sizeof(data), "%s%.*s", server->id.id_prefix,
			    (signed)sizeof(server->id.id), server->id.id);
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* link */
	data_len = od_snprintf(data, sizeof(data), "%s", "");
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* remote_pid */
	data_len = od_snprintf(data, sizeof(data), "%u", server->key.key_pid);
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* tls */
	data_len = od_snprintf(data, sizeof(data), "%s",
			       route->rule->storage->tls_opts->tls);
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* offline */
	data_len = od_snprintf(data, sizeof(data), "%d", server->offline);
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* plugin installed */
	data_len = od_snprintf(data, sizeof(data), "%s", server->backend_plugin_installed ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* session resources */
	data_len = 0;
	if (server->hold_advisory_locks)
		data_len += od_snprintf(data + data_len, sizeof(data) - data_len, "AD LOCKS;");
	if (!od_list_empty(&server->listen_channels))
		data_len += od_snprintf(data + data_len, sizeof(data) - data_len, "CHANNEL=%d;", od_list_size(&server->listen_channels));
	if (!od_list_empty(&server->temporary_tables))
		data_len += od_snprintf(data + data_len, sizeof(data) - data_len, "TABLES=%d;", od_list_size(&server->temporary_tables));
	if (!od_list_empty(&server->declared_cursors))
		data_len += od_snprintf(data + data_len, sizeof(data) - data_len, "CURSORS=%d;", od_list_size(&server->declared_cursors));
	if (server->query_prep_stmts && server->query_prep_stmts->items > 0) {
		data_len += od_snprintf(data + data_len, sizeof(data) - data_len, "PREPARED=%u;", od_atomic_u32_of(&server->query_prep_stmts->items));
	}
	if (data_len > 0)
		rc = kiwi_be_write_data_row_add(msg, offset, data, data_len);
	else
		rc = kiwi_be_write_data_row_add(msg, offset, NULL, -1);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;

	return 0;
}

static inline int od_console_show_server_prep_stmt_cb(od_server_t *server,
						      void **argv)
{
	od_route_t *route = server->route;
	od_lru_t *lru= server->prep_stmts_lru;
	uint64_t now = machine_time_us();

	if (lru == NULL)
		return 0;

	for (int pos = lru->head; pos>=0; pos = lru->elems[pos].next) {
		od_lru_elem_t *elem = &lru->elems[pos];
		od_lru_prep_stmt_t *prep_stmt = elem->data;
		{
			int offset;
			machine_msg_t *stream = argv[0];
			machine_msg_t *msg;
			msg = kiwi_be_write_data_row(stream, &offset);
			if (msg == NULL) {
				goto error;
			}

			/* type */
			char data[64];
			size_t data_len;
			data_len = od_snprintf(data, sizeof(data), "S");

			int rc;
			rc = kiwi_be_write_data_row_add(stream, offset, data,
							data_len);
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}

			/* user */
			rc = kiwi_be_write_data_row_add(stream, offset,
							route->id.user,
							route->id.user_len - 1);
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}

			/* database */
			rc = kiwi_be_write_data_row_add(
				stream, offset, route->id.database,
				route->id.database_len - 1);
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}

			/* sid */
			data_len = od_snprintf(data, sizeof(data), "%s%.*s",
					       server->id.id_prefix,
					       (signed)sizeof(server->id.id),
					       server->id.id);
			rc = kiwi_be_write_data_row_add(msg, offset, data,
							data_len);
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}

			// description
			rc = kiwi_be_write_data_row_add(stream, offset,
							prep_stmt->query,
							prep_stmt->query_len);
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}

			// time
			uint64_t ts = (now - elem->last_atime) / 1000;
			data_len = snprintf(data, sizeof(data), "%lu ms", ts);
			rc = kiwi_be_write_data_row_add(stream, offset, data,
							data_len);
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}
		}

		continue;
	error:
		return NOT_OK_RESPONSE;
	}

	return 0;
}

static inline int od_console_show_servers_cb(od_route_t *route, void **argv)
{
	int i;
	od_route_lock(route);

	for (i = 0; i < route->server_pool_size; ++i) {
		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_ACTIVE,
				od_console_show_servers_server_cb, argv);

		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_IDLE,
				od_console_show_servers_server_cb, argv);
	}
	od_route_unlock(route);
	return 0;
}

static inline int od_console_show_server_prep_stmts_cb(od_route_t *route,
						       void **argv)
{
	int i;
	od_route_lock(route);

	for (i = 0; i < route->server_pool_size;  ++i) {
		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_ACTIVE,
				od_console_show_server_prep_stmt_cb, argv);

		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_IDLE,
				od_console_show_server_prep_stmt_cb, argv);
	}

	od_route_unlock(route);
	return 0;
}

static inline int od_console_show_servers(od_client_t *client,
					  machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;

	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(
		stream, "sssssdsdssddssdssss", "type", "user", "database",
		"state", "addr", "port", "local_addr", "local_port",
		"connect_time", "request_time", "wait", "wait_us", "ptr",
		"link", "remote_pid", "tls", "offline", "plugin_installed",
		 "session resources");
	if (msg == NULL)
		return NOT_OK_RESPONSE;

	void *argv[] = { stream };
	od_router_foreach(router, od_console_show_servers_cb, argv);

	return kiwi_be_write_complete(stream, "SHOW", 5);
}

static inline int od_console_show_server_prep_stmts(od_client_t *client,
						    machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;

	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(stream, "ssssss", "type", "user",
					     "database", "sid", "definition",
					     "time");
	if (msg == NULL)
		return NOT_OK_RESPONSE;

	void *argv[] = { stream };
	od_router_foreach(router, od_console_show_server_prep_stmts_cb, argv);

	return kiwi_be_write_complete(stream, "SHOW", 5);
}

static inline int od_console_show_clients_callback(od_client_t *client,
						   void **argv)
{
	/* Odyssey should not expose its internal routes to SHOW utilities */
	if (client->type == OD_POOL_CLIENT_INTERNAL) {
		return 0;
	}
	int offset;
	machine_msg_t *stream = argv[0];
	machine_msg_t *msg;
	msg = kiwi_be_write_data_row(stream, &offset);
	if (msg == NULL)
		return NOT_OK_RESPONSE;
	char data[64];
	size_t data_len;
	/* type */
	data_len = od_snprintf(data, sizeof(data), "C");
	int rc;
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* user */
	rc = kiwi_be_write_data_row_add(stream, offset,
					client->startup.user.value,
					client->startup.user.value_len - 1);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* database */
	rc = kiwi_be_write_data_row_add(stream, offset,
					client->startup.database.value,
					client->startup.database.value_len - 1);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* state */
	char *state = "";
	if (client->state == OD_CLIENT_ACTIVE)
		state = "active";
	else if (client->state == OD_CLIENT_PENDING)
		state = "pending";
	else if (client->state == OD_CLIENT_QUEUE)
		state = "queue";
	rc = kiwi_be_write_data_row_add(stream, offset, state, strlen(state));
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* storage_user */
	rc = kiwi_be_write_data_row_add(stream, offset,
					client->rule->storage_user,
					client->rule->storage_user_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* addr */
	od_getpeername(client->io.io, data, sizeof(data), 1, 0);
	data_len = strlen(data);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* port */
	od_getpeername(client->io.io, data, sizeof(data), 0, 1);
	data_len = strlen(data);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* local_addr */
	od_getsockname(client->io.io, data, sizeof(data), 1, 0);
	data_len = strlen(data);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* local_port */
	od_getsockname(client->io.io, data, sizeof(data), 0, 1);
	data_len = strlen(data);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* query_time */
	uint64_t elapsed;
	if (client->query_start_time) {
		elapsed = machine_time_us() - client->query_start_time;
		data_len = od_snprintf(data, sizeof(data), "%ld", elapsed);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	} else {
		rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
	}
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* transaction_time */
	if (client->transaction_start_time) {
		elapsed = machine_time_us() - client->transaction_start_time;
		data_len = od_snprintf(data, sizeof(data), "%ld", elapsed );
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	} else {
		rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
	}
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* from_last_query */
	if (client->last_query_finished_time) {
		elapsed = 0;
		if (machine_time_us() > client->last_query_finished_time)
			elapsed = machine_time_us() - client->last_query_finished_time;
		data_len = od_snprintf(data, sizeof(data), "%ld", elapsed);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	} else {
		rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
	}
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* wait */
	data_len = od_snprintf(data, sizeof(data), "0");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* wait_us */
	data_len = od_snprintf(data, sizeof(data), "0");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* id */
	data_len =
		od_snprintf(data, sizeof(data), "%s%.*s", client->id.id_prefix,
			    (signed)sizeof(client->id.id), client->id.id);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* ptr */
	data_len = od_snprintf(data, sizeof(data), "%p", client);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* coro */
	data_len = od_snprintf(data, sizeof(data), "%d", client->coroutine_id);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE) {
		return NOT_OK_RESPONSE;
	}
	/* remote_pid */
	if (client->server)
		data_len = od_snprintf(data, sizeof(data), "%u", client->server->key.key_pid);
	else
		data_len = od_snprintf(data, sizeof(data), "0");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* tls */
	data_len = od_snprintf(data, sizeof(data), "%s", "");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	return 0;
}

static inline od_retcode_t od_console_show_clients_cb(od_route_t *route,
						      void **argv)
{
	od_route_lock(route);

	od_client_pool_foreach(&route->client_pool, OD_CLIENT_ACTIVE,
			       od_console_show_clients_callback, argv);

	od_client_pool_foreach(&route->client_pool, OD_CLIENT_PENDING,
			       od_console_show_clients_callback, argv);

	od_client_pool_foreach(&route->client_pool, OD_CLIENT_QUEUE,
			       od_console_show_clients_callback, argv);

	od_route_unlock(route);
	return 0;
}

static inline int od_console_show_clients(od_client_t *client,
					  machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;

	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(
		stream, "ssssssdsdsssddssdds", "type", "user", "database",
		"state", "storage_user", "addr", "port", "local_addr",
		"local_port", "query_time", "transaction_time", "from_last_query", "wait", "wait_us",
		"id", "ptr", "coro", "remote_pid", "tls");
	if (msg == NULL)
		return NOT_OK_RESPONSE;

	void *argv[] = { stream };
	od_router_foreach(router, od_console_show_clients_cb, argv);

	return kiwi_be_write_complete(stream, "SHOW", 5);
}

static inline int od_console_show_lists_add(machine_msg_t *stream, char *list,
					    int items)
{
	int offset;
	machine_msg_t *msg;
	msg = kiwi_be_write_data_row(stream, &offset);
	if (msg == NULL)
		return NOT_OK_RESPONSE;
	/* list */
	int rc;
	rc = kiwi_be_write_data_row_add(stream, offset, list, strlen(list));
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* items */
	char data[64];
	int data_len;
	data_len = od_snprintf(data, sizeof(data), "%d", items);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	return 0;
}

static inline int od_console_show_lists_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);

	int *used_servers = argv[0];
	int *free_servers = argv[1];
	(*used_servers) += od_server_pool_active(route->server_pool, route->server_pool_size);
	(*free_servers) += od_server_pool_idle(route->server_pool, route->server_pool_size);

	od_route_unlock(route);
	return 0;
}

static inline int od_console_show_lists(od_client_t *client,
					machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;

	/* Gather router information.

	   router_used_servers can be inconsistent here, since it depends on
	   separate route locks.
	*/
	od_router_lock(router);

	int router_used_servers = 0;
	int router_free_servers = 0;
	int router_pools = router->route_pool.count;
	int router_clients = od_atomic_u32_of(&router->clients);

	void *argv[] = { &router_used_servers, &router_free_servers };
	od_route_pool_foreach(&router->route_pool, od_console_show_lists_cb,
			      argv);

	od_router_unlock(router);

	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(stream, "sd", "list", "items");
	if (msg == NULL)
		return NOT_OK_RESPONSE;
	int rc;
	/* databases */
	rc = od_console_show_lists_add(stream, "databases", 0);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* users */
	rc = od_console_show_lists_add(stream, "users", 0);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* pools */
	rc = od_console_show_lists_add(stream, "pools", router_pools);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* free_clients */
	rc = od_console_show_lists_add(stream, "free_clients", 0);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* used_clients */
	rc = od_console_show_lists_add(stream, "used_clients", router_clients);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* login_clients */
	rc = od_console_show_lists_add(stream, "login_clients", 0);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* free_servers */
	rc = od_console_show_lists_add(stream, "free_servers",
				       router_free_servers);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* used_servers */
	rc = od_console_show_lists_add(stream, "used_servers",
				       router_used_servers);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* dns_names */
	rc = od_console_show_lists_add(stream, "dns_names", 0);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* dns_zones */
	rc = od_console_show_lists_add(stream, "dns_zones", 0);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* dns_queries */
	rc = od_console_show_lists_add(stream, "dns_queries", 0);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	/* dns_pending */
	rc = od_console_show_lists_add(stream, "dns_pending", 0);
	if (rc == NOT_OK_RESPONSE)
		return NOT_OK_RESPONSE;
	return kiwi_be_write_complete(stream, "SHOW", 5);
}

static inline int od_console_write_nullable_str(machine_msg_t *stream,
						int offset, char *str)
{
	char data[64];
	int data_len;

	if (str == NULL) {
		data_len = od_snprintf(data, sizeof(data), "(None)");
		return kiwi_be_write_data_row_add(stream, offset, data,
						  data_len);
	}

	return kiwi_be_write_data_row_add(stream, offset, str, strlen(str));
}

static inline int od_console_show_tls_options(od_tls_opts_t *tls_opts,
					      int offset, machine_msg_t *stream)
{
	char *tls = od_config_tls_to_str(tls_opts->tls_mode);

	od_retcode_t rc;

	/* tls */
	rc = od_console_write_nullable_str(stream, offset, tls);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	/* tls_cert_file */
	rc = od_console_write_nullable_str(stream, offset,
					   tls_opts->tls_cert_file);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	/* tls_key_file */
	rc = od_console_write_nullable_str(stream, offset,
					   tls_opts->tls_key_file);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	/* tls_ca_file */
	rc = od_console_write_nullable_str(stream, offset,
					   tls_opts->tls_ca_file);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	/* tls_protocols */
	rc = od_console_write_nullable_str(stream, offset,
					   tls_opts->tls_protocols);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	return OK_RESPONSE;
}

static inline int od_console_show_listen(od_client_t *client,
					 machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;

	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(stream, "sdsssssdss", "host", "port",
					     "tls", "tls_cert_file",
					     "tls_key_file", "tls_ca_file",
					     "tls_protocols","backlog","compression","port_attrs");

	if (msg == NULL) {
		return NOT_OK_RESPONSE;
	}

	od_instance_t *instance = router->global->instance;
	od_config_t *config = &instance->config;

	char data[64];
	int data_len;
	int rc;
	int offset;

	od_list_t *i;
	od_list_foreach(&config->listen, i)
	{
		od_config_listen_t *listen_config;
		listen_config = od_container_of(i, od_config_listen_t, link);

		msg = kiwi_be_write_data_row(stream, &offset);
		if (msg == NULL) {
			return NOT_OK_RESPONSE;
		}
		/* host */
		rc = od_console_write_nullable_str(stream, offset,
						   listen_config->host);
		if (rc != OK_RESPONSE) {
			return rc;
		}

		/* port */
		data_len = od_snprintf(data, sizeof(data), "%d",
				       listen_config->port);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) {
			return rc;
		}

		rc = od_console_show_tls_options(listen_config->tls_opts,
						 offset, stream);
		if (rc != OK_RESPONSE) {
			return rc;
		}

		data_len = od_snprintf(data, sizeof(data), "%d",
				       listen_config->backlog);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) {
			return rc;
		}

		char *compression = listen_config->compression ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, compression, strlen(compression));
		if (rc != OK_RESPONSE) {
			return rc;
		}

		char *port_attrs;
		switch (listen_config->port_attrs) {
			case OD_PORT_ATTR_RW: port_attrs = "read-write"; break;
			case OD_PORT_ATTR_RO: port_attrs = "read-only"; break;
			case OD_PORT_ATTR_WO: port_attrs = "write-only"; break;
			default: port_attrs = "undef"; break;
		}
		rc = kiwi_be_write_data_row_add(stream, offset, port_attrs, strlen(port_attrs));
		if (rc != OK_RESPONSE) {
			return rc;
		}
	}

	return kiwi_be_write_complete(stream, "SHOW", 5);
}

/*
 * show storage endpoint related information while handling SHOW STORAGEs command
 */
static inline int od_console_show_storage_endpoint(
					   od_storage_endpoint_t storage_endpoint,
					   uint64_t endpoint_select_cnt,
					   size_t primary_endpoint_index,
					   size_t current_endpoint_index,
					   long heatbeat_lag,
					   long replication_lag,
					   int offset, machine_msg_t *stream)
{
	od_retcode_t rc;

	/* endpoint_hostname */
	rc = od_console_write_nullable_str(stream, offset, storage_endpoint.host);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	/* endpoint_port */
	char data[64];
	int data_len;

	data_len = od_snprintf(data, sizeof(data), "%d", storage_endpoint.port);

	if (data == NULL) {
		data_len = od_snprintf(data, sizeof(data), "(None)");
	}
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	/* endpoint_role */
	if (primary_endpoint_index == current_endpoint_index) {
		data_len = od_snprintf(data, sizeof(data), "primary");
	} else if (primary_endpoint_index == INT32_MAX) {
		data_len = od_snprintf(data, sizeof(data), "(None)");
	} else {
		data_len = od_snprintf(data, sizeof(data), "standby");
	}
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	/* endpoint_weight */
	data_len = od_snprintf(data, sizeof(data), "%f", storage_endpoint.weight);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	/* endpoint_select_cnt */
	data_len = od_snprintf(data, sizeof(data), "%d", endpoint_select_cnt);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) {
		return rc;
	}

	/* endpoin_heatbeat_lag */
	if (heatbeat_lag == -1) {
		data_len = od_snprintf(data, sizeof(data), "(None)");
	} else {
		data_len = od_snprintf(data, sizeof(data), "%ld", heatbeat_lag);
	}
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE)
		return rc;

	/* endpoint_replication_lag */
	if (replication_lag == -1) {
		data_len = od_snprintf(data, sizeof(data), "(None)");
	} else {
		data_len = od_snprintf(data, sizeof(data), "%ld", replication_lag);
	}
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE)
		return rc;


	return OK_RESPONSE;
}

/*
 * sum the route select count for each storage endpoint
 */
static inline int od_console_sum_route_select_cnt_cb(od_route_t *route,
						      void **argv)
{
	uint64_t *count_select_endpoints = argv[0];
	size_t *primary_endpoint_index = argv[1];
	char *storage_name = argv[2];
	od_route_t **proute = argv[3];
	assert(count_select_endpoints);

	if (!route || !route->extra_logging_enabled) {
		return OK_RESPONSE;
	}

	od_stat_t *stat = &route->stats;
	char *route_storage_name = route->rule->storage_name;

	if (strcmp(route_storage_name, storage_name) == 0) {
		for (size_t i = 0; i < stat->endpoints_count; ++i) {
			count_select_endpoints[i] += stat->count_select_endpoints[i];
		}
		*primary_endpoint_index = route->endpoint_state->primary_endpoint_index;
		if (proute && *proute == NULL && route->rule && !route->rule->obsolete)
			*proute = route;
	}

	return OK_RESPONSE;
}

static inline int od_console_show_storages(od_client_t *client,
					   machine_msg_t *stream)
{
	assert(stream);
	od_router_t *router = client->global->router;

	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(stream, "sssdsfdsssssss", "name", "type",
						 "endpoint_host", "endpoint_port", "endpoint_role",
						 "endpoint_weight", "endpoint_select_cnt", 
						 "endpoint_heartbeat_lag", "endpoint_replication_lag",
						 "tls", "tls_cert_file",
						 "tls_key_file", "tls_ca_file",
						 "tls_protocols");

	if (msg == NULL) {
		return NOT_OK_RESPONSE;
	}

	od_rules_t *rules = &router->rules;

	int rc;
	int offset;

	pthread_mutex_lock(&rules->mu);

	od_list_t *item;
	od_list_foreach(&rules->storages, item)
	{
		uint64_t *count_select_endpoints = NULL;
		od_route_t *any_route = NULL;
		size_t primary_endpoint_index = INT32_MAX;
		od_rule_storage_t *storage;
		storage = od_container_of(item, od_rule_storage_t, link);

		if (storage->storage_type == OD_RULE_STORAGE_LOCAL)
			continue;

		count_select_endpoints = malloc(sizeof(uint64_t) * storage->endpoints_count);
		if (count_select_endpoints == NULL) {
			goto error;
		}
		memset(count_select_endpoints, 0, sizeof(uint64_t) * storage->endpoints_count);

		void *argv[] = { count_select_endpoints, &primary_endpoint_index, storage->name, &any_route};
		od_router_foreach(router, od_console_sum_route_select_cnt_cb, argv);


		for (size_t i = 0; i < storage->endpoints_count; ++i) {
			long heartbeat_lag = -1;
			long replication_lag = -1;
			if (any_route) {
				heartbeat_lag = machine_timeofday_sec() - any_route->server_pool[i].last_heartbeat;
				replication_lag = any_route->server_pool[i].last_replag_millisec / 1000;
			}
			msg = kiwi_be_write_data_row(stream, &offset);
			if (msg == NULL) {
				rc = NOT_OK_RESPONSE;
				goto error;
			}

			/* name */
			char *name = storage->name;
			if (!name) {
				name = "";
			}

			rc = kiwi_be_write_data_row_add(stream, offset, name, strlen(name));
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}

			if (storage->storage_type == OD_RULE_STORAGE_REMOTE) {
				rc = kiwi_be_write_data_row_add(stream, offset,
								"remote", 6 + 1);
				if (rc == NOT_OK_RESPONSE) {
					goto error;
				}
			} else {
				rc = kiwi_be_write_data_row_add(stream, offset, "local",
								5 + 1);
				if (rc == NOT_OK_RESPONSE) {
					goto error;
				}
			}

#if 0
			char *host = storage->host;
			if (!host) {
				host = "";
			}

			rc = kiwi_be_write_data_row_add(stream, offset, host,
							strlen(host));
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}

			char data[64];
			int data_len;

			/* port */
			data_len = od_snprintf(data, sizeof(data), "%d", storage->port);
			rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
			if (rc == NOT_OK_RESPONSE) {
				goto error;
			}
#endif
			rc = od_console_show_storage_endpoint(storage->endpoints[i],
							count_select_endpoints[i], primary_endpoint_index,
							i, heartbeat_lag, replication_lag, offset, stream);
			if (rc != OK_RESPONSE) {
				goto error;
			}

			rc = od_console_show_tls_options(storage->tls_opts, offset,
							stream);
			if (rc != OK_RESPONSE) {
				goto error;
			}
		}

		free(count_select_endpoints);	
	}

	pthread_mutex_unlock(&rules->mu);
	return kiwi_be_write_complete(stream, "SHOW", 5);
error:
	pthread_mutex_unlock(&rules->mu);
	return rc;
}

static inline int od_console_show_global_config(od_client_t *client, machine_msg_t *stream)
{
	assert(stream);
	assert(client);
	
	od_instance_t *instance = client->global->instance;
	od_config_t *config = &instance->config;

	/* 定义输出表格的列结构 */
	if (kiwi_be_write_row_descriptionf(stream, "ss", "parameter", "value") == NULL) {
		return NOT_OK_RESPONSE;
	}

	char data[512];
	int data_len;
	int rc;
	int offset;

	/* 服务配置部分 */
	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "daemonize", 9);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->daemonize ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "priority", 8);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->priority);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	/* 系统相关配置 */
	if (config->unix_socket_dir) {
		if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
		rc = kiwi_be_write_data_row_add(stream, offset, "unix_socket_dir", 15);
		if (rc != OK_RESPONSE) return rc;
		rc = kiwi_be_write_data_row_add(stream, offset, config->unix_socket_dir, strlen(config->unix_socket_dir));
		if (rc != OK_RESPONSE) return rc;
	}

	if (config->unix_socket_mode) {
		if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
		rc = kiwi_be_write_data_row_add(stream, offset, "unix_socket_mode", 16);
		if (rc != OK_RESPONSE) return rc;
		rc = kiwi_be_write_data_row_add(stream, offset, config->unix_socket_mode, strlen(config->unix_socket_mode));
		if (rc != OK_RESPONSE) return rc;
	}

	if (config->locks_dir) {
		if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
		rc = kiwi_be_write_data_row_add(stream, offset, "locks_dir", 9);
		if (rc != OK_RESPONSE) return rc;
		rc = kiwi_be_write_data_row_add(stream, offset, config->unix_socket_dir, strlen(config->locks_dir));
		if (rc != OK_RESPONSE) return rc;
	}

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "use_unix_socket_if_possible", 27);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->use_unix_socket_if_possible ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "graceful_die_on_errors", 22);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->graceful_die_on_errors ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "enable_online_restart", 21);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->enable_online_restart_feature ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "bindwith_reuseport", 18);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->bindwith_reuseport ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (config->pid_file) {
		if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
		rc = kiwi_be_write_data_row_add(stream, offset, "pid_file", 8);
		if (rc != OK_RESPONSE) return rc;
		rc = kiwi_be_write_data_row_add(stream, offset, config->pid_file, strlen(config->pid_file));
		if (rc != OK_RESPONSE) return rc;
	}

	/* 日志配置部分 */
	if (config->log_file) {
		if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
		rc = kiwi_be_write_data_row_add(stream, offset, "log_file", 8);
		if (rc != OK_RESPONSE) return rc;
		rc = kiwi_be_write_data_row_add(stream, offset, config->log_file, strlen(config->log_file));
		if (rc != OK_RESPONSE) return rc;
	}

	if (config->log_format) {
		if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
		rc = kiwi_be_write_data_row_add(stream, offset, "log_format", 10);
		if (rc != OK_RESPONSE) return rc;
		rc = kiwi_be_write_data_row_add(stream, offset, config->log_format, strlen(config->log_format));
		if (rc != OK_RESPONSE) return rc;
	}

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "log_to_stdout", 13);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->log_to_stdout ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "log_syslog", 10);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->log_syslog ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if(config->log_syslog_ident) {
		if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
		rc = kiwi_be_write_data_row_add(stream, offset, "log_syslog_ident", 16);
		if (rc != OK_RESPONSE) return rc;
		rc = kiwi_be_write_data_row_add(stream, offset, config->log_format, strlen(config->log_syslog_ident));
		if (rc != OK_RESPONSE) return rc;
	}

	if(config->log_syslog_facility) {
		if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
		rc = kiwi_be_write_data_row_add(stream, offset, "log_syslog_facility", 19);
		if (rc != OK_RESPONSE) return rc;
		rc = kiwi_be_write_data_row_add(stream, offset, config->log_format, strlen(config->log_syslog_facility));
		if (rc != OK_RESPONSE) return rc;
	}

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "log_to_stdout", 13);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->log_to_stdout ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "log_debug", 9);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->log_debug ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "log_query", 9);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->log_query ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "log_session", 11);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->log_session ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "log_stats", 11);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->log_stats ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "log_general_stats_prom", 22);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->log_general_stats_prom ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "log_route_stats_prom", 20);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->log_route_stats_prom ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "stats_interval", 14);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->stats_interval);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	/* 性能配置部分 */
	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "workers", 7);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->workers);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "resolvers", 9);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->resolvers);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "readahead", 9);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->readahead);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "cache_coroutine", 15);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->cache_coroutine);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "coroutine_stack_size", 20);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->coroutine_stack_size);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "nodelay", 7);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%s", config->nodelay ? "yes" : "no");
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	/* TCP Keepalive 配置 */
	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "keepalive", 9);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->keepalive);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "keepalive_keep_interval", 23);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->keepalive_keep_interval);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "keepalive_probes", 16);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->keepalive_probes);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "keepalive_usr_timeout", 21);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->keepalive_usr_timeout);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	/* 客户端限制配置 */
	if (config->client_max_set) {
		if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
		rc = kiwi_be_write_data_row_add(stream, offset, "client_max", 10);
		if (rc != OK_RESPONSE) return rc;
		data_len = od_snprintf(data, sizeof(data), "%d", config->client_max);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) return rc;
	}

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "client_max_routing", 18);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->client_max_routing);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	/* 统计相关配置 */
	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "stats_interval", 14);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->stats_interval);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	/* 协程相关配置 */
	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "cache_coroutine", 15);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->cache_coroutine);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	if (kiwi_be_write_data_row(stream, &offset) == NULL) return NOT_OK_RESPONSE;
	rc = kiwi_be_write_data_row_add(stream, offset, "coroutine_stack_size", 20);
	if (rc != OK_RESPONSE) return rc;
	data_len = od_snprintf(data, sizeof(data), "%d", config->coroutine_stack_size);
	rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
	if (rc != OK_RESPONSE) return rc;

	rc = kiwi_be_write_complete(stream, "SHOW", 5);
	return rc;
}

static inline int od_console_show_all_rules(od_client_t *client, machine_msg_t *stream)
{ 
	assert(stream);
	od_router_t *router = client->global->router;
	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(stream, "ssssds", "user", "database",
					     "marked", "obsoleted",
					     "refs", "storages");

	if (msg == NULL) {
		return NOT_OK_RESPONSE;
	}

	char data[64];
	int data_len;
	int rc;
	int offset;

	od_list_t *i;
	od_router_lock(router);
	od_rules_t *rules = &router->rules;
	od_list_foreach(&rules->rules, i)
	{
		od_rule_t *rule;
		rule = od_container_of(i, od_rule_t, link);
		msg = kiwi_be_write_data_row(stream, &offset);
		if (msg == NULL) 
			goto error;
	
		/* user */
		rc = kiwi_be_write_data_row_add(stream, offset, rule->user_name,
						rule->user_name_len);
		if (rc	== NOT_OK_RESPONSE) 
			goto error;
	
		/* database */
		rc = kiwi_be_write_data_row_add(stream, offset, rule->db_name,
						rule->db_name_len);
		if (rc == NOT_OK_RESPONSE) 
			goto error;
	
		/* marked */
		rc = kiwi_be_write_data_row_add(stream, offset,
						rule->mark ? "true" : "false",
						rule->mark ? 4 + 1 : 5 + 1);
		if (rc == NOT_OK_RESPONSE ) 
			goto error;
	
		/* obsoleted  */
		rc = kiwi_be_write_data_row_add(stream, offset,
						rule->obsolete ? "true" : "false",
						rule->obsolete ? 4 + 1 : 5 + 1);
		if (rc == NOT_OK_RESPONSE) 
			goto error;
	
		/* refs */
		data_len = od_snprintf(data, sizeof(data), "%d", rule->refs);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if(rc == NOT_OK_RESPONSE) 
			goto error;
	
		/* storages */
		rc = kiwi_be_write_data_row_add(stream, offset,
						rule->storage->name ? rule->storage->name : "null",
						rule->storage->name ? strlen(rule->storage->name) : 4 + 1);
		if(rc == NOT_OK_RESPONSE) 
			goto error;
	}
	od_router_unlock(router);
	return kiwi_be_write_complete(stream, "SHOW", 5);

error:
	od_router_unlock(router);
    return NOT_OK_RESPONSE;

}

static inline int od_console_show_rules(od_client_t *client, machine_msg_t *stream)
{
	assert(stream);
	assert(client);
	
	od_router_t *router = client->global->router;
	od_rules_t *rules = &router->rules;

	machine_msg_t *msg;
	msg = kiwi_be_write_row_descriptionf(stream, "ssssssssssssssssssssssssssssssssssssssssss", 
		"database", "user", "address_range", "user_role",
		"authentication", "auth_query", "auth_query_db", "auth_query_user", "auth_common_name_default", "auth_common_names_count",
		"auth_module", "enable_mdb_iamproxy_auth",
		"storage", "storage_db", "storage_user", 
		"pool", "pool_routing", "pool_size", "pool_timeout", "pool_ttl", "pool_cancel", "pool_rollback",
		"pool_client_idle_timeout", "pool_idle_in_transaction_timeout", "pool_reserve_prepared_statement",
		"pool_prepared_statement_limit", "pool_prepared_statement_expired_time", "target_server_attrs",
		"client_max", "client_fwd_error", "reserve_session_server_connection",
		"catchup_timeout", "catchup_checks", "replication_delay_threshold", "server_lifetime_us", "enable_read_only_blacklist",
		"enable_quantiles_stat", "quantiles_count", "enable_password_passthrough", "application_name_add_host", "log_debug", "log_query");
	if (msg == NULL) {
		return NOT_OK_RESPONSE;
	}

	pthread_mutex_lock(&rules->mu);

	int rc;
	od_list_t *i;
	od_list_foreach(&rules->rules, i) {
		od_rule_t *rule;
		rule = od_container_of(i, od_rule_t, link);
		if (rule->mark || rule->obsolete)
			continue;

		int offset;
		msg = kiwi_be_write_data_row(stream, &offset);
		if (msg == NULL) {
			pthread_mutex_unlock(&rules->mu);
			return NOT_OK_RESPONSE;
		}

		char data[256];
		int data_len;

		/* database */
		char *db_name = rule->db_is_default ? "default" : rule->db_name;
		rc = kiwi_be_write_data_row_add(stream, offset, db_name, strlen(db_name));
		if (rc != OK_RESPONSE) goto error;

		/* user */
		char *user_name = rule->user_is_default ? "default" : rule->user_name;
		rc = kiwi_be_write_data_row_add(stream, offset, user_name, strlen(user_name));
		if (rc != OK_RESPONSE) goto error;

		/* address_range */
		char *addr_range = rule->address_range.string_value ? rule->address_range.string_value : "default";
		rc = kiwi_be_write_data_row_add(stream, offset, addr_range, strlen(addr_range));
		if (rc != OK_RESPONSE) goto error;

		/* user_role */
		char *user_role;
		switch (rule->user_role) {
			case OD_RULE_ROLE_ADMIN: user_role = "admin"; break;
			case OD_RULE_ROLE_STAT: user_role = "stat"; break;
			case OD_RULE_ROLE_NOTALLOW: user_role = "notallow"; break;
			case OD_RULE_ROLE_UNDEF:
			default: user_role = "undef"; break;
		}
		rc = kiwi_be_write_data_row_add(stream, offset, user_role, strlen(user_role));
		if (rc != OK_RESPONSE) goto error;

		/* auth */
		char *auth = rule->auth ? rule->auth : "none";
		rc = kiwi_be_write_data_row_add(stream, offset, auth, strlen(auth));
		if (rc != OK_RESPONSE) goto error;

		/* auth_query */
		if (rule->auth_query) {
			rc = kiwi_be_write_data_row_add(stream, offset, rule->auth_query, strlen(rule->auth_query));
		} else {
			rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
		}
		if (rc != OK_RESPONSE) goto error;

		/* auth_query_db */
		if (rule->auth_query_db) {
			rc = kiwi_be_write_data_row_add(stream, offset, rule->auth_query_db, strlen(rule->auth_query_db));
		} else {
			rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
		}
		if (rc != OK_RESPONSE) goto error;

		/* auth_query_user */
		if (rule->auth_query_user) {
			rc = kiwi_be_write_data_row_add(stream, offset, rule->auth_query_user, strlen(rule->auth_query_user));
		} else {
			rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
		}
		if (rc != OK_RESPONSE) goto error;

		/* auth_common_name_default */
		char *auth_common_default = rule->auth_common_name_default ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, auth_common_default, strlen(auth_common_default));
		if (rc != OK_RESPONSE) goto error;

		/* auth_common_names_count */
		data_len = od_snprintf(data, sizeof(data), "%d", rule->auth_common_names_count);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) goto error;

		/* auth_module */
		if (rule->auth_module) {
			rc = kiwi_be_write_data_row_add(stream, offset, rule->auth_module, strlen(rule->auth_module));
		} else {
			rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
		}
		if (rc != OK_RESPONSE) goto error;

		/* enable_mdb_iamproxy_auth */
		char *mdb_iamproxy = rule->enable_mdb_iamproxy_auth ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, mdb_iamproxy, strlen(mdb_iamproxy));
		if (rc != OK_RESPONSE) goto error;

		/* storage_name */
		char *storage_name = rule->storage_name ? rule->storage_name : "unknown";
		rc = kiwi_be_write_data_row_add(stream, offset, storage_name, strlen(storage_name));
		if (rc != OK_RESPONSE) goto error;

		/* storage_db */
		if (rule->storage_db) {
			rc = kiwi_be_write_data_row_add(stream, offset, rule->storage_db, strlen(rule->storage_db));
		} else {
			rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
		}
		if (rc != OK_RESPONSE) goto error;

		/* storage_user */
		if (rule->storage_user) {
			rc = kiwi_be_write_data_row_add(stream, offset, rule->storage_user, strlen(rule->storage_user));
		} else {
			rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
		}
		if (rc != OK_RESPONSE) goto error;

		if (rule->pool) {
			/* pool_type */
			char *pool_type = rule->pool->type ? rule->pool->type : "unknown";
			rc = kiwi_be_write_data_row_add(stream, offset, pool_type, strlen(pool_type));
			if (rc != OK_RESPONSE) goto error;

			/* pool_routing */
			char *pool_routing;
			switch (rule->pool->routing) {
				case OD_RULE_POOL_INTERNAL: pool_routing = "internal"; break;
				case OD_RULE_POOL_CLIENT_VISIBLE: pool_routing = "client_visible"; break;
				default: pool_routing = "unknown"; break;
			}
			rc = kiwi_be_write_data_row_add(stream, offset, pool_routing, strlen(pool_routing));
			if (rc != OK_RESPONSE) goto error;

			/* pool_size */
			data_len = od_snprintf(data, sizeof(data), "%d", rule->pool->size);
			rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
			if (rc != OK_RESPONSE) goto error;

			/* pool_timeout */
			data_len = od_snprintf(data, sizeof(data), "%d", rule->pool->timeout);
			rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
			if (rc != OK_RESPONSE) goto error;

			/* pool_ttl */
			data_len = od_snprintf(data, sizeof(data), "%d", rule->pool->ttl);
			rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
			if (rc != OK_RESPONSE) goto error;

			/* pool_cancel */
			char *pool_cancel = rule->pool->cancel ? "yes" : "no";
			rc = kiwi_be_write_data_row_add(stream, offset, pool_cancel, strlen(pool_cancel));
			if (rc != OK_RESPONSE) goto error;

			/* pool_rollback */
			char *pool_rollback = rule->pool->rollback ? "yes" : "no";
			rc = kiwi_be_write_data_row_add(stream, offset, pool_rollback, strlen(pool_rollback));
			if (rc != OK_RESPONSE) goto error;

			/* pool_client_idle_timeout */
			data_len = od_snprintf(data, sizeof(data), "%" PRIu64, rule->pool->client_idle_timeout);
			rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
			if (rc != OK_RESPONSE) goto error;

			/* pool_idle_in_transaction_timeout */
			data_len = od_snprintf(data, sizeof(data), "%" PRIu64, rule->pool->idle_in_transaction_timeout);
			rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
			if (rc != OK_RESPONSE) goto error;

			/* pool_reserve_prepared_statement */
			char *reserve_prepared = rule->pool->reserve_prepared_statement ? "yes" : "no";
			rc = kiwi_be_write_data_row_add(stream, offset, reserve_prepared, strlen(reserve_prepared));
			if (rc != OK_RESPONSE) goto error;

			/* pool_prepared_statement_limit */
			data_len = od_snprintf(data, sizeof(data), "%d", rule->pool->prepared_statement_limit);
			rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
			if (rc != OK_RESPONSE) goto error;

			/* pool_prepared_statement_expired_time */
			data_len = od_snprintf(data, sizeof(data), "%d", rule->pool->prepared_statement_expired_time);
			rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
			if (rc != OK_RESPONSE) goto error;

			/* pool_target_server_attrs */
			char *target_server;
			switch (rule->pool->target_server_attrs) {
				case OD_TARGET_SERVER_RW: target_server = "read_write"; break;
				case OD_TARGET_SERVER_RO: target_server = "read_only"; break;
				case OD_TARGET_SERVER_AUTO: target_server = "auto"; break;
				default: target_server = "unknown"; break;
			}
			rc = kiwi_be_write_data_row_add(stream, offset, target_server, strlen(target_server));
			if (rc != OK_RESPONSE) goto error;
		} else {
			/* 如果pool为NULL，填充NULL值 */
			for (int j = 0; j < 12; j++) {
				rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
				if (rc != OK_RESPONSE) goto error;
			}
		}

		/* client_max */
		if (rule->client_max_set) {
			data_len = od_snprintf(data, sizeof(data), "%d", rule->client_max);
			rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		} else {
			rc = kiwi_be_write_data_row_add(stream, offset, NULL, -1);
		}
		if (rc != OK_RESPONSE) goto error;

		/* client_fwd_error */
		char *fwd_error = rule->client_fwd_error ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, fwd_error, strlen(fwd_error));
		if (rc != OK_RESPONSE) goto error;

		/* reserve_session_server_connection */
		char *reserve_conn = rule->reserve_session_server_connection ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, reserve_conn, strlen(reserve_conn));
		if (rc != OK_RESPONSE) goto error;

		/* catchup_timeout */
		data_len = od_snprintf(data, sizeof(data), "%d", rule->catchup_timeout);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) goto error;

		/* catchup_checks */
		data_len = od_snprintf(data, sizeof(data), "%d", rule->catchup_checks);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) goto error;

		/* replication_delay_threshold */
		data_len = od_snprintf(data, sizeof(data), "%" PRId64, rule->replication_delay_threshold);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) goto error;

		/* server_lifetime_us */
		data_len = od_snprintf(data, sizeof(data), "%" PRIu64, rule->server_lifetime_us);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) goto error;

		/* enable_read_only_blacklist */
		char *read_only_blacklist = rule->enable_read_only_blacklist ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, read_only_blacklist, strlen(read_only_blacklist));
		if (rc != OK_RESPONSE) goto error;

		/* enable_quantiles_stat */
		char *quantiles_stat = rule->enable_quantiles_stat ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, quantiles_stat, strlen(quantiles_stat));
		if (rc != OK_RESPONSE) goto error;

		/* quantiles_count */
		data_len = od_snprintf(data, sizeof(data), "%d", rule->quantiles_count);
		rc = kiwi_be_write_data_row_add(stream, offset, data, data_len);
		if (rc != OK_RESPONSE) goto error;

		/* enable_password_passthrough */
		char *password_passthrough = rule->enable_password_passthrough ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, password_passthrough, strlen(password_passthrough));
		if (rc != OK_RESPONSE) goto error;

		/* application_name_add_host */
		char *app_name_add_host = rule->application_name_add_host ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, app_name_add_host, strlen(app_name_add_host));
		if (rc != OK_RESPONSE) goto error;

		/* log_debug */
		char *log_debug = rule->log_debug ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, log_debug, strlen(log_debug));
		if (rc != OK_RESPONSE) goto error;

		/* log_query */
		char *log_query = rule->log_query ? "yes" : "no";
		rc = kiwi_be_write_data_row_add(stream, offset, log_query, strlen(log_query));
		if (rc != OK_RESPONSE) goto error;
	}

	pthread_mutex_unlock(&rules->mu);

return  kiwi_be_write_complete(stream, "SHOW", 5);

error:
	pthread_mutex_unlock(&rules->mu);
	return rc;
}

static inline int od_console_show(od_client_t *client, machine_msg_t *stream,
				  od_parser_t *parser)
{
	assert(stream);
	od_token_t token;
	int rc;
	rc = od_parser_next(parser, &token);
	switch (rc) {
	case OD_PARSER_KEYWORD:
		break;
	case OD_PARSER_EOF:
	default:
		return NOT_OK_RESPONSE;
	}
	od_keyword_t *keyword;
	keyword = od_keyword_match(od_console_keywords, &token);
	if (keyword == NULL)
		return NOT_OK_RESPONSE;
	switch (keyword->id) {
	case OD_LSTATS:
		return od_console_show_stats(client, stream);
	case OD_LHELP:
		return od_console_show_help(client, stream);
	case OD_LPOOLS:
		return od_console_show_pools(client, stream, false);
	case OD_LPOOLS_EXTENDED:
		return od_console_show_pools(client, stream, true);
	case OD_LDATABASES:
		return od_console_show_databases(client, stream);
	case OD_LSERVER_PREP_STMTS:
		return od_console_show_server_prep_stmts(client, stream);
	case OD_LSERVERS:
		return od_console_show_servers(client, stream);
	case OD_LCLIENTS:
		return od_console_show_clients(client, stream);
	case OD_LLISTS:
		return od_console_show_lists(client, stream);
	case OD_LERRORS:
		return od_console_show_errors(client, stream);
	case OD_LERRORS_PER_ROUTE:
		return od_console_show_errors_per_route(client, stream);
	case OD_LVERSION:
		return od_console_show_version(stream);
	case OD_LLISTEN:
		return od_console_show_listen(client, stream);
	case OD_LSTORAGES:
		return od_console_show_storages(client, stream);
	case OD_LGLOBAL_CONFIG:
		return od_console_show_global_config(client, stream);
	case OD_LALL_RULES:
		return od_console_show_all_rules(client, stream);
	case OD_LRULES:
		return od_console_show_rules(client, stream);
	}
	return NOT_OK_RESPONSE;
}

static inline int od_console_kill_client(od_client_t *client,
					 machine_msg_t *stream,
					 od_parser_t *parser)
{
	(void)stream;
	od_token_t token;
	int rc;
	rc = od_parser_next(parser, &token);
	if (rc != OD_PARSER_KEYWORD)
		return NOT_OK_RESPONSE;
	od_id_t id;
	if (token.value.string.size != (sizeof(id.id) + 1))
		return NOT_OK_RESPONSE;
	memcpy(id.id, token.value.string.pointer + 1, sizeof(id.id));

	od_router_kill(client->global->router, &id);
	return 0;
}

static inline int od_console_reload(od_client_t *client, machine_msg_t *stream)
{
	od_instance_t *instance = client->global->instance;

	od_log(&instance->logger, "console", NULL, NULL,
	       "RELOAD command received");
	od_system_config_reload(client->global->system);
	return kiwi_be_write_complete(stream, "RELOAD", 7);
}

static inline int od_console_set(od_client_t *client, machine_msg_t *stream)
{
	(void)client;
	/* reply success */
	return kiwi_be_write_complete(stream, "SET", 4);
}

static inline int od_console_add_module(od_client_t *client,
					machine_msg_t *stream,
					od_parser_t *parser)
{
	assert(stream);
	od_token_t token;
	int rc;
	rc = od_parser_next(parser, &token);
	od_instance_t *instance = client->global->instance;

	switch (rc) {
	case OD_PARSER_STRING: {
		char module_path[MAX_MODULE_PATH_LEN];
		od_token_to_string_dest(&token, module_path);

		od_log(&instance->logger, "od module dynamic load", NULL, NULL,
		       "loading module with path %s", module_path);
		int retcode = od_target_module_add(
			&instance->logger,
			((od_extention_t *)client->global->extentions)->modules,
			module_path);
		if (retcode == 0) {
			od_frontend_infof(client, stream,
					  "module was successfully loaded!");
		} else {
			od_frontend_errorf(
				client, stream, KIWI_SYSTEM_ERROR,
				"module was NOT successfully loaded! Check logs for details");
		}
		return retcode;
	}
	case OD_PARSER_EOF:
	default:
		return NOT_OK_RESPONSE;
	}
}

static inline int od_console_unload_module(od_client_t *client,
					   machine_msg_t *stream,
					   od_parser_t *parser)
{
	assert(stream);
	od_token_t token;
	int rc;
	rc = od_parser_next(parser, &token);
	od_instance_t *instance = client->global->instance;

	switch (rc) {
	case OD_PARSER_STRING: {
		char module_path[MAX_MODULE_PATH_LEN];
		od_token_to_string_dest(&token, module_path);

		od_log(&instance->logger, "od module dynamic unload", NULL,
		       NULL, "unloading module with path %s", module_path);
		int retcode = od_target_module_unload(
			&instance->logger,
			((od_extention_t *)client->global->extentions)->modules,
			module_path);
		if (retcode == 0) {
			od_frontend_infof(client, stream,
					  "module was successfully unloaded!");
		} else {
			od_frontend_errorf(client, stream, KIWI_SYSTEM_ERROR,
					   "module was NOT successfully "
					   "unloaded! Check logs for details");
		}
		return retcode;
	}
	case OD_PARSER_EOF:
	default:
		return NOT_OK_RESPONSE;
	}
}

static inline int od_console_create(od_client_t *client, machine_msg_t *stream,
				    od_parser_t *parser)
{
	assert(stream);
	od_token_t token;
	int rc;
	rc = od_parser_next(parser, &token);
	switch (rc) {
	case OD_PARSER_KEYWORD:
		break;
	case OD_PARSER_EOF:
	default:
		return NOT_OK_RESPONSE;
	}
	od_keyword_t *keyword;
	keyword = od_keyword_match(od_console_keywords, &token);
	if (keyword == NULL)
		return NOT_OK_RESPONSE;

	switch (keyword->id) {
	case OD_LMODULE:
		return od_console_add_module(client, stream, parser);
	}

	return NOT_OK_RESPONSE;
}

static inline int od_console_drop_server_cb(od_server_t *server,
					    od_attribute_unused() void **argv)
{
	server->offline = 1;
	return OK_RESPONSE;
}

static inline od_retcode_t od_console_drop_server(od_route_t *route,
						  void **argv)
{
	int i;
	od_route_lock(route);

	for (i=0; i<route->server_pool_size; ++i) {
		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_ACTIVE,
				od_console_drop_server_cb, argv);

		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_IDLE,
				od_console_drop_server_cb, argv);
	}

	od_route_unlock(route);
	return OK_RESPONSE;
}

static inline od_retcode_t od_console_drop_servers(od_client_t *client,
						   machine_msg_t *stream,
						   od_parser_t *parser)
{
	(void)client;
	assert(stream);

	od_token_t token;
	int rc;
	rc = od_parser_next(parser, &token);
	switch (rc) {
	case OD_PARSER_EOF:
		break;
	default:
		return NOT_OK_RESPONSE;
	}

	od_router_t *router = client->global->router;

	void *argv[] = { stream };
	od_router_foreach(router, od_console_drop_server, argv);
	return OK_RESPONSE;
}

static inline od_retcode_t
od_console_drop(od_client_t *client, machine_msg_t *stream, od_parser_t *parser)
{
	assert(stream);
	od_token_t token;
	int rc;
	rc = od_parser_next(parser, &token);
	switch (rc) {
	case OD_PARSER_KEYWORD:
		break;
	case OD_PARSER_EOF:
	default:
		return NOT_OK_RESPONSE;
	}
	od_keyword_t *keyword;
	keyword = od_keyword_match(od_console_keywords, &token);
	if (keyword == NULL)
		return NOT_OK_RESPONSE;

	switch (keyword->id) {
	case OD_LSERVERS:
		return od_console_drop_servers(client, stream, parser);
	case OD_LMODULE:
		return od_console_unload_module(client, stream, parser);
	default:
		return NOT_OK_RESPONSE;
	}

	return NOT_OK_RESPONSE;
}

int od_console_query(od_client_t *client, machine_msg_t *stream,
		     char *query_data, uint32_t query_data_size)
{
	od_instance_t *instance = client->global->instance;

	uint32_t query_len;
	char *query;
	machine_msg_t *msg;
	if (client->rule->user_role != OD_RULE_ROLE_ADMIN &&
	    client->rule->user_role != OD_RULE_ROLE_STAT) {
		goto incorrect_role;
	}
	int rc;
	rc = kiwi_be_read_query(query_data, query_data_size, &query,
				&query_len);
	if (rc == NOT_OK_RESPONSE) {
		od_error(&instance->logger, "console", client, NULL,
			 "bad console command");
		msg = od_frontend_errorf(client, stream, KIWI_SYNTAX_ERROR,
					 "bad console command");
		if (msg == NULL)
			return NOT_OK_RESPONSE;

		return 0;
	}

	if (instance->config.log_query)
		od_debug(&instance->logger, "console", client, NULL, "%.*s",
			 query_len, query);

	od_parser_t parser;
	od_parser_init(&parser, query, query_len);

	od_token_t token;
	rc = od_parser_next(&parser, &token);
	switch (rc) {
	case OD_PARSER_KEYWORD:
		break;
	case OD_PARSER_EOF:
	default:
		goto bad_query;
	}
	od_keyword_t *keyword;
	keyword = od_keyword_match(od_console_keywords, &token);
	if (keyword == NULL)
		goto bad_query;
	switch (keyword->id) {
	case OD_LSHOW:
		rc = od_console_show(client, stream, &parser);
		if (rc == NOT_OK_RESPONSE)
			goto bad_query;
		break;
	case OD_LKILL_CLIENT:
		if (client->rule->user_role != OD_RULE_ROLE_ADMIN)
			goto incorrect_role;
		rc = od_console_kill_client(client, stream, &parser);
		if (rc == NOT_OK_RESPONSE)
			goto bad_query;
		break;
	case OD_LRELOAD:
		if (client->rule->user_role != OD_RULE_ROLE_ADMIN)
			goto incorrect_role;
		rc = od_console_reload(client, stream);
		if (rc == NOT_OK_RESPONSE)
			goto bad_query;
		break;
	case OD_LSET:
		if (client->rule->user_role != OD_RULE_ROLE_ADMIN)
			goto incorrect_role;
		rc = od_console_set(client, stream);
		if (rc == NOT_OK_RESPONSE)
			goto bad_query;
		break;
	case OD_LCREATE:
		if (client->rule->user_role != OD_RULE_ROLE_ADMIN)
			goto incorrect_role;
		rc = od_console_create(client, stream, &parser);
		if (rc == NOT_OK_RESPONSE) {
			goto bad_query;
		}
		break;
	case OD_LDROP:
		if (client->rule->user_role != OD_RULE_ROLE_ADMIN)
			goto incorrect_role;
		rc = od_console_drop(client, stream, &parser);
		if (rc == NOT_OK_RESPONSE) {
			goto bad_query;
		}
		break;
	default:
		goto bad_query;
	}

	return 0;

incorrect_role:
	od_error(&instance->logger, "console", client, NULL,
		 "Unsuitable user role to emit console command");
	msg = od_frontend_errorf(
		client, stream, KIWI_INSUFFICIENT_PRIVILEGE,
		"Unsuitable user role to emit console command");
	if (msg == NULL)
		return NOT_OK_RESPONSE;

	return 0;

bad_query:
	od_error(&instance->logger, "console", client, NULL,
		 "console command error: %.*s", query_len, query);

	msg = od_frontend_errorf(client, stream, KIWI_SYNTAX_ERROR,
				 "console command error: %.*s", query_len,
				 query);
	if (msg == NULL)
		return NOT_OK_RESPONSE;

	return 0;
}
