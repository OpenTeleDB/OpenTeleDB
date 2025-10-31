#ifndef ODYSSEY_SERVER_H
#define ODYSSEY_SERVER_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

typedef struct od_server od_server_t;

typedef enum {
	OD_SERVER_UNDEF,
	OD_SERVER_IDLE,
	OD_SERVER_ACTIVE,
} od_server_state_t;

typedef struct {
	od_hash_t hash;
	size_t opname_len;
	char *opname;
	size_t query_len;
	char *query;
} od_lru_prep_stmt_t;

typedef struct {
	char name[64];
	size_t len;
	od_list_t link;
} od_temporary_table_t;

typedef struct {
	char name[64];
	size_t len;
	od_list_t link;
} od_listen_channel_t;

typedef struct {
	char name[64];
	size_t len;
	od_list_t link;
} od_declared_cursor_t;

struct od_server {
	od_server_state_t state;
#ifdef USE_SCRAM
	od_scram_state_t scram_state;
#endif
	od_id_t id;
	machine_tls_t *tls;
	od_io_t io;
	od_relay_t relay;
	int is_transaction;
	/* Copy stmt state */
	uint64_t done_fail_response_received;
	uint64_t in_out_response_received;
	/**/
	int deploy_sync;
	od_stat_state_t stats_state;

	uint64_t sync_request;
	uint64_t sync_reply;

	/* to swallow some internal msgs */
	machine_msg_t *parse_msg;
	int idle_time;

	kiwi_key_t key;
	kiwi_key_t key_client;
	kiwi_vars_t vars;
	od_hashmap_t *other_vars; /* gucs without GUC_REPORT flag */

	machine_msg_t *error_connect;
	/* od_client_t */
	void *client;
	/* od_route_t  */
	void *route;

	/* storage endpoiunt index, which we are connected to */
	size_t endpoint_selector;

	/* discard server and change a primary*/
	bool is_dropped;

	/* allocated prepared statements ids */
	od_hashmap_t *prep_stmts;
	od_lru_t *prep_stmts_lru;
	int sync_point;

	/* allocated prepared statements with query protocal */
	od_hashmap_t *query_prep_stmts;

	/* temporary tables */
	od_list_t temporary_tables;
	od_list_t listen_channels;
	od_list_t declared_cursors;

	/* session level advisory locks */
	bool hold_advisory_locks;

	/* if the backend pg install the "query_check" plugin*/
	bool backend_plugin_installed;

	od_global_t *global;
	int offline;
	uint64_t init_time_us;
	bool synced_settings;

	od_list_t link;
};

static const size_t OD_SERVER_DEFAULT_HASHMAP_SZ = 420;
static const size_t OD_SERVER_SMALL_HASHMAP_SZ = 16;

static inline void lru_prep_stmt_free(void *data)
{
	od_lru_prep_stmt_t *stmt = data;
	free(stmt->opname);
	free(stmt->query);
	free(stmt);
}

static inline void lru_prep_stmt_init(od_lru_prep_stmt_t *stmt,
		od_hash_t hash,
		char *opname, int opname_len,
		char *query, int query_len)
{
	stmt->hash = hash;
	stmt->opname = strdup(opname);
	stmt->opname_len = opname_len;
	stmt->query = malloc(query_len);
	memcpy(stmt->query, query, query_len);
	stmt->query_len = query_len;
}

static inline void od_server_init(od_server_t *server, int reserve_prep_stmts)
{
	memset(server, 0, sizeof(od_server_t));
	server->state = OD_SERVER_UNDEF;
	server->route = NULL;
	server->client = NULL;
	server->global = NULL;
	server->tls = NULL;
	server->idle_time = 0;
	server->is_transaction = 0;
	server->done_fail_response_received = 0;
	server->in_out_response_received = 0;
	server->deploy_sync = 0;
	server->sync_request = 0;
	server->sync_reply = 0;
	server->sync_point = 0;
	server->parse_msg = NULL;
	server->init_time_us = machine_time_us();
	server->error_connect = NULL;
	server->offline = 0;
	server->synced_settings = false;
	server->endpoint_selector = 0;
	server->is_dropped = false;
	od_stat_state_init(&server->stats_state);

#ifdef USE_SCRAM
	od_scram_state_init(&server->scram_state);
#endif

	kiwi_key_init(&server->key);
	kiwi_key_init(&server->key_client);
	kiwi_vars_init(&server->vars);
	server->other_vars = od_hashmap_create(32, false);

	od_io_init(&server->io);
	od_relay_init(&server->relay, &server->io);
	od_list_init(&server->link);
	memset(&server->id, 0, sizeof(server->id));

	if (reserve_prep_stmts) {
		server->prep_stmts =
			od_hashmap_create(OD_SERVER_DEFAULT_HASHMAP_SZ, false);
		server->query_prep_stmts =
			od_hashmap_create(OD_SERVER_SMALL_HASHMAP_SZ, false);
		server->prep_stmts_lru = od_lru_init(128, lru_prep_stmt_free);
	} else {
		server->prep_stmts = NULL;
		server->query_prep_stmts = NULL;
		server->prep_stmts_lru = NULL;
	}
	od_list_init(&server->temporary_tables);
	od_list_init(&server->listen_channels);
	od_list_init(&server->declared_cursors);
	server->hold_advisory_locks = false;
	server->backend_plugin_installed = false;
}

static inline od_server_t *od_server_allocate(int reserve_prep_stmts)
{
	od_server_t *server = malloc(sizeof(od_server_t));
	if (server == NULL)
		return NULL;
	od_server_init(server, reserve_prep_stmts);
	return server;
}

static inline void od_server_free(od_server_t *server)
{
	od_relay_free(&server->relay);
	od_io_free(&server->io);
	if (server->prep_stmts) {
		od_hashmap_free(server->prep_stmts);
		od_lru_free(server->prep_stmts_lru);
	}
	od_hashmap_free(server->other_vars);

	if (server->parse_msg)
		machine_msg_free(server->parse_msg);

	while (!od_list_empty(&server->temporary_tables)) {
		od_list_t *it = od_list_pop(&server->temporary_tables);
		od_temporary_table_t *tbl = od_container_of(it, od_temporary_table_t, link);
		free(tbl);
	}

	while (!od_list_empty(&server->listen_channels)) {
		od_list_t *it = od_list_pop(&server->listen_channels);
		od_listen_channel_t *channel = od_container_of(it, od_listen_channel_t, link);
		free(channel);
	}

	while (!od_list_empty(&server->declared_cursors)) {
		od_list_t *it = od_list_pop(&server->declared_cursors);
		od_declared_cursor_t *channel = od_container_of(it, od_declared_cursor_t, link);
		free(channel);
	}

	free(server);
}

static inline void od_server_sync_request(od_server_t *server, uint64_t count)
{
	server->sync_request += count;
}

static inline void od_server_sync_reply(od_server_t *server)
{
	server->sync_reply++;
}

static inline int od_server_in_deploy(od_server_t *server)
{
	return server->deploy_sync > 0;
}

static inline int od_server_in_sync_point(od_server_t *server)
{
	return server->sync_point > 0;
}

static inline int od_server_synchronized(od_server_t *server)
{
	assert(server->sync_request >= server->sync_reply);
	return server->sync_request == server->sync_reply;
}

static inline int od_server_grac_shutdown(od_server_t *server)
{
	server->offline = 1;
	return 0;
}

static inline int od_server_reload(od_attribute_unused() od_server_t *server)
{
	// TODO: set offline to 1 if storage/auth rules changed
	return 0;
}

#endif /* ODYSSEY_SERVER_H */
