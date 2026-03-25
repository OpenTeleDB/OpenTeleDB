
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

static inline bool od_host_is_local(od_instance_t *instance, char *host);
static inline bool od_unix_socket_available(od_instance_t *instance, int port);

typedef enum NoticeType {
	NOTICE_TYPE_DISCARD_ALL = 0,
	NOTICE_TYPE_DISCARD_TEMP,
	NOTICE_TYPE_RESET_ALL,
	NOTICE_TYPE_DEALLOCATE_ALL,
	NOTICE_TYPE_DEALLOCATE_STMT,
	NOTICE_TYPE_PREPARE_STMT,
	NOTICE_TYPE_SET_VARIABLE,
	NOTICE_TYPE_RESET_VARIABLE,
	NOTICE_TYPE_CREATE_TEMP_TABLE,
	NOTICE_TYPE_DROP_TEMP_TABLE,
	NOTICE_TYPE_SESSIN_LOCKS,
	NOTICE_TYPE_NO_SESSION_LOCKS,
	NOTICE_TYPE_LISTEN,
	NOTICE_TYPE_UNLISTEN,
	NOTICE_TYPE_UNLISTEN_ALL,
	NOTICE_TYPE_DECLARE_CURSOR,
	NOTICE_TYPE_CLOSE_CURSOR,
	NOTICE_TYPE_CLOSE_ALL,
	NOTICE_TYPE_NR
} NoticeType;

typedef struct {
	NoticeType type;
	char *prefix;
	size_t prefix_len;
} XProxyNoticeInfo;

#define STRING_AND_SIZE(x) x, (sizeof(x) - 1)

static XProxyNoticeInfo all_notice_list[] = {
	{NOTICE_TYPE_DISCARD_ALL, STRING_AND_SIZE("discard all")},
	{NOTICE_TYPE_DISCARD_TEMP, STRING_AND_SIZE("discard temp")},
	{NOTICE_TYPE_RESET_ALL, STRING_AND_SIZE("reset all")},
	{NOTICE_TYPE_DEALLOCATE_ALL, STRING_AND_SIZE("deallocate all")},
	{NOTICE_TYPE_DEALLOCATE_STMT, STRING_AND_SIZE("deallocate stmt ")},
	{NOTICE_TYPE_PREPARE_STMT, STRING_AND_SIZE("prepare stmt ")},
	{NOTICE_TYPE_SET_VARIABLE, STRING_AND_SIZE("set variable ")},
	{NOTICE_TYPE_RESET_VARIABLE, STRING_AND_SIZE("reset variable ")},
	{NOTICE_TYPE_CREATE_TEMP_TABLE, STRING_AND_SIZE("create temp table ")},
	{NOTICE_TYPE_DROP_TEMP_TABLE, STRING_AND_SIZE("drop temp table ")},
	{NOTICE_TYPE_SESSIN_LOCKS, STRING_AND_SIZE("session level advisory locks")},
	{NOTICE_TYPE_NO_SESSION_LOCKS, STRING_AND_SIZE("no session level advisory locks")},
	{NOTICE_TYPE_LISTEN, STRING_AND_SIZE("listen on ")},
	{NOTICE_TYPE_UNLISTEN, STRING_AND_SIZE("unlisten on ")},
	{NOTICE_TYPE_UNLISTEN_ALL, STRING_AND_SIZE("unlisten all")},
	{NOTICE_TYPE_DECLARE_CURSOR, STRING_AND_SIZE("declare cursor ")},
	{NOTICE_TYPE_CLOSE_CURSOR, STRING_AND_SIZE("close cursor ")},
	{NOTICE_TYPE_CLOSE_ALL, STRING_AND_SIZE("close all")},
	{NOTICE_TYPE_NR, NULL, 0}
};

void od_backend_close(od_server_t *server)
{
	assert(server->route == NULL);
	assert(server->io.io == NULL);
	assert(server->tls == NULL);
	server->is_transaction = 0;
	server->idle_time = 0;
	kiwi_key_init(&server->key);
	kiwi_key_init(&server->key_client);
	od_server_free(server);
}

static inline void
od_backend_error_is_too_many_connections(od_client_t *client)
{
	od_server_t *server = client->server;
	assert(server != NULL);
	if (server->error_connect == NULL)
	{
		client->wait_for_idle = false;
		return;
	}
	kiwi_fe_error_t error;
	int rc;
	rc = kiwi_fe_read_error(machine_msg_data(server->error_connect),
				machine_msg_size(server->error_connect),
				&error);
	if (rc == -1)
	{
		client->wait_for_idle = false;
		return;
	}
	client->wait_for_idle = strcmp(error.code, KIWI_TOO_MANY_CONNECTIONS) == 0;
}

static inline int od_backend_terminate(od_server_t *server)
{
	machine_msg_t *msg;
	msg = kiwi_fe_write_terminate(NULL);
	if (msg == NULL)
		return -1;
	return od_write(&server->io, msg);
}

void od_backend_close_connection(od_server_t *server)
{
	/* failed to connect to endpoint, so notring to do */
	if (server->io.io == NULL) {
		return;
	}
	if (machine_connected(server->io.io))
		od_backend_terminate(server);

	od_io_close(&server->io);

	if (server->error_connect) {
		machine_msg_free(server->error_connect);
		server->error_connect = NULL;
	}

	if (server->tls) {
		machine_tls_free(server->tls);
		server->tls = NULL;
	}
}

static int parse_notice_message(char **data, size_t *size, char **token, size_t *token_len)
{
	char *p = *data;
	size_t len = *size;
	/* skip space */
	while (len > 0 && isspace(*p)) {
		++p;
		--len;
	}
	if (len == 0)
		return -1;

	/* check quote */
	char quote = ' ';
	if (*p == '\'') {
		quote = *p;
		++p;
		--len;
	}

	char *q = p;
	*token = p;

	while (len > 0 && *p) {
		/* quote ?*/
		if (*p == quote) {
			if (len > 1 && quote == '\'' && p[1] == quote) {
				++p;
				--len;
			} else {
				break;
			}
		}
        /* blackslash */
        else if (*p == '\\') {
            ++p;
            --len;
            if (len == 0)
                return -1;
        }
		/* seperator ? */
        else if (quote == ' ' && *p == ',') {
			break;
		}
		*q = *p;
		++p;
		++q;
		--len;
	}

	if (quote == '\'' && len == 0)
		return -1;

	/* skip quote / seperator after token */
	if (len > 0) {
		bool is_seperator;
		is_seperator = (*p == ',');
		++p;
		--len;

		/* skip seperator */
		if (!is_seperator) {
			while (len > 0 && isspace(*p)) {
				++p;
				--len;
			}
			if (len > 0 && *p == ',') {
				++p;
				--len;
			}
		}
        /* skip space */
        while (len > 0 && (isspace(*p) || *p == '\0')) {
            ++p;
            --len;
        }
	}

	*q = '\0';
	*token_len = q - *token + 1;
	*data = p;
	*size = len;
	return 0;
}

static inline void od_discard_temp(od_server_t *server, bool server_only)
{
	od_list_t *it, *next;
	od_temporary_table_t *tbl;

	(void) server_only;
	od_list_foreach_safe(&server->temporary_tables, it, next) {
		tbl = od_container_of(it, od_temporary_table_t, link);
		od_list_unlink(it);
		free(tbl);
	}
}

static inline void od_close_all(od_server_t *server, bool server_only)
{
	od_list_t *it, *next;
	od_declared_cursor_t *cur;

	(void) server_only;
	od_list_foreach_safe(&server->declared_cursors, it, next) {
		cur = od_container_of(it, od_declared_cursor_t, link);
		od_list_unlink(it);
		free(cur);
	}
}

static inline void od_discard_vars(od_server_t *server, bool is_reset, bool server_only)
{
	od_hashmap_elt_t key = {.data = "role", .len = 5};
	od_hashmap_elt_t *key_ptr = is_reset ? &key : NULL;

	od_hashmap_empty_except(server->other_vars, key_ptr);

	if (server->client && !server_only) {
		od_client_t *client = server->client;
		od_hashmap_empty_except(client->other_vars, key_ptr);
	}

	/* set role to none if it's discard operation  */
	if (!is_reset) {
		static od_hash_t hash = 0;
		if (hash == 0)
			hash = od_murmur_hash(key.data, key.len);
		od_hashmap_elt_t value = {.data = "none", .len = 5};
		od_hashmap_elt_t *value_ptr = &value;
		od_hashmap_insert(server->other_vars, hash, &key, &value_ptr, true);

		if (server->client && !server_only) {
			od_client_t *client = server->client;
			od_hashmap_insert(client->other_vars, hash, &key, &value_ptr, true);
		}
	}
}

static inline void od_deallocte_all(od_server_t *server, bool server_only)
{
	/* clean prepared statment with PARSE protocal */
	if (server->prep_stmts) {
		od_hashmap_empty(server->prep_stmts);
		od_lru_empty(server->prep_stmts_lru);
	}

	/* clean prepared statment with PREPARE statment */
	if (server->query_prep_stmts)
		od_hashmap_empty(server->query_prep_stmts);

	if (!server_only && server->client) {
		od_client_t *client = server->client;
		if (client->prep_stmt_ids)
			od_hashmap_empty(client->prep_stmt_ids);
	}
}

static inline void od_prepare_stmt(od_server_t *server, char *stmt, size_t len)
{
	if (!server->query_prep_stmts)
		return;

	od_hashmap_elt_t key = {
		.data =  stmt,
		.len = len
	};
	od_hashmap_elt_t value = {
		.data = "",
		.len = 1
	};
	od_hashmap_elt_t *value_ptr = &value;
	od_hash_t hash = od_murmur_hash(stmt, len);
	od_hashmap_insert(server->query_prep_stmts, hash, &key, &value_ptr, true);
}

static inline bool od_deallocte_stmt(od_server_t *server, char *stmt, size_t len)
{
	// if (!server->query_prep_stmts)
	// 	return;

	bool deleted = false;
	od_client_t *client = server->client;

	od_hashmap_elt_t key = {
		.data = stmt,
		.len = len
	};
	od_hash_t hash = od_murmur_hash(stmt, len);

	/* First check if the target stmt is created by 'PREPARE' */
	if (server->query_prep_stmts)
		deleted = od_hashmap_delete(server->query_prep_stmts, hash, &key);
	
	/* If not, check if the target stmt is created by parse message */ 
	/* because in some cases frontends would use 'DEALLOCATE' to free a parsed stmt created by 'parse' */
	/* If so, delete it from client->prep_stmt_ids, just like processing close message */
	/* It's OK if the server has no client */
	if (!deleted && client && client->prep_stmt_ids) {
		deleted = od_hashmap_delete(client->prep_stmt_ids, hash, &key);
		od_hashmap_delete(client->prep_stmt_target_roles, hash, &key);
	}
	return deleted;
}

static inline bool is_untracked_guc(const char *guc_name)
{
	static const char *untracked_list[4] = {
		"transaction_read_only",
		"transaction_deferrable",
		"transaction_isolation",
		"seed"
	};

	for (int i = 0; i < 4; i++) {
		if (strcmp(guc_name, untracked_list[i]) == 0)
			return true;
	}

	return false;
}

static od_frontend_status_t parse_variable_value(char *data, size_t len, char **out, size_t *out_len)
{
	char *values[20];
	size_t lengths[20];
	size_t cnt = 0;

	*out_len = 0;
	while (len > 0) {
		if (parse_notice_message(&data, &len, &values[cnt], &lengths[cnt]) == -1)
			return OD_ESERVER_READ;
		*out_len += lengths[cnt];
		++cnt;
		if (cnt > 20)
			return OD_ESERVER_READ;
	}

	if (!(*out = malloc(*out_len)))
		return OD_EOOM;

	char *p = *out;
	for (size_t i=0; i < cnt; ++i) {
		memcpy(p, values[i], lengths[i]);
		p += lengths[i];
	}

	return OD_OK;
}

NoticeType get_xproxy_notice_type(char **message, size_t *len)
{
	for (int i=0; i<NOTICE_TYPE_NR; ++i) {
		XProxyNoticeInfo *n = &all_notice_list[i];
		if (n->prefix_len <= *len && 
				strncasecmp(n->prefix, *message, n->prefix_len) == 0) {
			*len -= n->prefix_len;
			*message += n->prefix_len;
			return n->type;
		}
	}

	return NOTICE_TYPE_NR;
}

od_frontend_status_t od_backend_notice(od_server_t *server, char *data, int size, bool server_only)
{
	od_client_t *client = server->client;
	od_instance_t *instance = server->global->instance;
	char *message = NULL;
	size_t len;

	if (kiwi_be_read_notice_message(data, size, &message, &len) == -1) {
		od_error(&instance->logger, "xproxy notice", client, server,
				"failed to parse notice message: %.*s", size, data);
		return OD_ESERVER_READ;
	}

	/* The notice message always terminated by '\0' */
	assert(message[len-1] == '\0');

	if (message && len > 15 && strncmp(message, "NOTICE: xproxy ", 15) == 0) {
		message += 15;
		len -= 15;
		od_debug(&instance->logger, "xproxy notice", server->client,
				server, "receive a notice: %s ", message);

		switch (get_xproxy_notice_type(&message, &len)) {
			case NOTICE_TYPE_DISCARD_ALL:
				{
					od_discard_temp(server, server_only);
					od_discard_vars(server, false, server_only);
					od_deallocte_all(server, server_only);
					od_close_all(server, server_only);
					break;
				}
			case NOTICE_TYPE_DISCARD_TEMP:
				{
					od_discard_temp(server, server_only);
					break;
				}
			case NOTICE_TYPE_RESET_ALL:
				{
					od_discard_vars(server, true, server_only);
					break;
				}
			case NOTICE_TYPE_DEALLOCATE_ALL:
				{
					od_deallocte_all(server, server_only);
					break;
				}
			case NOTICE_TYPE_DEALLOCATE_STMT:
				{
					char *token;
					size_t token_len;
					bool deleted = false;
					if (parse_notice_message(&message, &len, &token, &token_len) == -1)
						return OD_ESERVER_READ;
					deleted = od_deallocte_stmt(server, token, token_len);
					if (!deleted && server->client)
						od_log(&instance->logger, "deallocate stmt", server->client, server, "warning: deallocate a non-existent prepared statement");
					break;
				}
			case NOTICE_TYPE_PREPARE_STMT:
				{
					char *token;
					size_t token_len;
					if (parse_notice_message(&message, &len, &token, &token_len) == -1)
						return OD_ESERVER_READ;
					od_prepare_stmt(server, token, token_len);
					break;
				}
			case NOTICE_TYPE_SET_VARIABLE:
				{
					od_hashmap_elt_t key, value;
					char *token;
					size_t token_len;
					/* name */
					if (parse_notice_message(&message, &len, &token, &token_len) == -1)
						return OD_ESERVER_READ;
					key.data = token;
					key.len = token_len;
					/* set variable name to lower case */
					while (token_len > 0) {
						*token = tolower(*token);
						-- token_len;
						++ token;
					}
					if (!is_untracked_guc(key.data)) {
						/* skip token 'TO' */
						if (parse_notice_message(&message, &len, &token, &token_len) == -1)
							return OD_ESERVER_READ;

						/* values */
						od_frontend_status_t status;
						status = parse_variable_value(message, len, (char **)&value.data, &value.len);
						if (status != OD_OK)
							return status;

						od_hashmap_elt_t *value_ptr = &value;
						od_hash_t hash = od_murmur_hash(key.data, key.len);
						od_hashmap_insert(server->other_vars, hash, &key, &value_ptr, true);
						if (client)
							od_hashmap_insert(client->other_vars, hash, &key, &value_ptr, true);
						if (value.data)
							free(value.data);
					}
					break;
				}
			case NOTICE_TYPE_RESET_VARIABLE:
				{
					od_hashmap_elt_t key;
					char *token;
					size_t token_len;

					/* name */
					if (parse_notice_message(&message, &len, &token, &token_len) == -1)
						return OD_ESERVER_READ;
					key.data = token;
					key.len = token_len;

					/* set variable name to lower case */
					while (token_len > 0) {
						*token = tolower(*token);
						-- token_len;
						++ token;
					}
					od_hash_t hash = od_murmur_hash(key.data, key.len);
					od_hashmap_delete(server->other_vars, hash, &key);
					if (client)
						od_hashmap_delete(client->other_vars, hash, &key);
					break;
				}
			case NOTICE_TYPE_CREATE_TEMP_TABLE:
				{
					char *name;
					size_t name_len;
					od_temporary_table_t *tbl;

					if (parse_notice_message(&message, &len, &name, &name_len) == -1)
						return OD_ESERVER_READ;
					if (name_len > 64)
						return OD_ESERVER_READ;
					tbl = malloc(sizeof(od_temporary_table_t));
					if (!tbl)
						return OD_EOOM;
					tbl->len = name_len;
					memcpy(tbl->name, name, name_len);
					od_list_push(&server->temporary_tables, &tbl->link);
					break;
				}
			case NOTICE_TYPE_DROP_TEMP_TABLE:
				{
					char *name;
					size_t name_len;
					od_temporary_table_t *tbl;
					od_list_t *it;

					if (parse_notice_message(&message, &len, &name, &name_len) == -1)
						return OD_ESERVER_READ;
					if (name_len > 64)
						return OD_ESERVER_READ;

					od_list_foreach(&server->temporary_tables, it) {
						tbl = od_container_of(it, od_temporary_table_t, link);
						if (tbl->len == name_len &&
								memcmp(tbl->name, name, name_len) == 0) {
							od_list_unlink(it);
							free(tbl);
							break;
						}
					}
					break;
				}
			case NOTICE_TYPE_DECLARE_CURSOR:
				{
					char *name;
					size_t name_len;
					od_declared_cursor_t *cur;

					if (parse_notice_message(&message, &len, &name, &name_len) == -1)
						return OD_ESERVER_READ;
					if (name_len > 64)
						return OD_ESERVER_READ;
					cur = malloc(sizeof(od_declared_cursor_t));
					if (!cur)
						return OD_EOOM;
					cur->len = name_len;
					memcpy(cur->name, name, name_len);
					od_list_push(&server->declared_cursors, &cur->link);
					break;
				}
			case NOTICE_TYPE_CLOSE_CURSOR:
				{
					char *name;
					size_t name_len;
					od_declared_cursor_t *cur;
					od_list_t *it;

					if (parse_notice_message(&message, &len, &name, &name_len) == -1)
						return OD_ESERVER_READ;
					if (name_len > 64)
						return OD_ESERVER_READ;

					od_list_foreach(&server->declared_cursors, it) {
						cur = od_container_of(it, od_declared_cursor_t, link);
						if (cur->len == name_len &&
								memcmp(cur->name, name, name_len) == 0) {
							od_list_unlink(it);
							free(cur);
							break;
						}
					}
					break;
				}
			case NOTICE_TYPE_CLOSE_ALL:
				{
					od_close_all(server, server_only);
					break;
				}
			case NOTICE_TYPE_SESSIN_LOCKS:
				{
					server->hold_advisory_locks = true;
					break;
				}
			case NOTICE_TYPE_NO_SESSION_LOCKS:
				{
					server->hold_advisory_locks = false;
					break;
				}
			case NOTICE_TYPE_LISTEN:
				{
					char *name;
					size_t name_len;
					od_listen_channel_t *channel = NULL;;
					od_list_t *it;

					if (parse_notice_message(&message, &len, &name, &name_len) == -1 || name_len > 64)
						return OD_ESERVER_READ;

					/* check if existence of the channel*/
					od_list_foreach(&server->listen_channels, it) {
						channel = od_container_of(it, od_listen_channel_t, link);
						if (channel->len == name_len &&
								memcmp(channel->name, name, name_len) == 0) {
							break;
						}
						channel = NULL;
					}

					if (!channel) {
						channel = malloc(sizeof(od_listen_channel_t));
						memcpy(channel->name, name, name_len);
						channel->len = name_len;
						od_list_push(&server->listen_channels, &channel->link);
					}
					break;
				}
			case NOTICE_TYPE_UNLISTEN:
				{
					char *name;
					size_t name_len;
					od_listen_channel_t *channel;
					od_list_t *it, *n;

					if (parse_notice_message(&message, &len, &name, &name_len) == -1 || name_len > 64)
						return OD_ESERVER_READ;

					od_list_foreach_safe(&server->listen_channels, it, n) {
						channel = od_container_of(it, od_listen_channel_t, link);
						if (channel->len == name_len &&
								memcmp(channel->name, name, name_len) == 0) {
							od_list_unlink(it);
							free(channel);
							break;
						}
					}
					break;
				}
			case NOTICE_TYPE_UNLISTEN_ALL:
				{
					while (!od_list_empty(&server->listen_channels)) {
						od_list_t *it = od_list_pop(&server->listen_channels);
						od_listen_channel_t *channel = od_container_of(it, od_listen_channel_t, link);
						free(channel);
					}
					break;
				}
			default:
				break;
		}

		return OD_SKIP;
	}

	return OD_OK;
}

int od_backend_error(od_server_t *server, char *context, char *data,
		      uint32_t size)
{
	od_instance_t *instance = server->global->instance;
	kiwi_fe_error_t error;

	int rc;
	rc = kiwi_fe_read_error(data, size, &error);
	if (rc == -1) {
		od_error(&instance->logger, context, server->client, server,
			 "failed to parse error message from server");
		return 0;
	}

	od_error(&instance->logger, context, server->client, server, "%s %s %s",
		 error.severity, error.code, error.message);

	if (error.detail) {
		od_error(&instance->logger, context, server->client, server,
			 "DETAIL: %s", error.detail);
	}

	if (error.hint) {
		od_error(&instance->logger, context, server->client, server,
			 "HINT: %s", error.hint);
	}

	/* write SQL send to the standby, the server return error */
	if (error.code != NULL && strcmp(error.message, "xproxy not_readonly query") == 0) {
		return 1;
	}
	
	if (error.code != NULL)
		return 2;

	return 0;
}

int od_backend_ready(od_server_t *server, char *data, uint32_t size)
{
	int status;
	int rc;
	rc = kiwi_fe_read_ready(data, size, &status);
	if (rc == -1)
		return -1;
	if (status == 'I') {
		/* no active transaction */
		server->is_transaction = 0;
	} else if (status == 'T' || status == 'E') {
		/* in active transaction or in interrupted
		 * transaction block */
		server->is_transaction = 1;
	}
	/* update server sync reply state */

	od_server_sync_reply(server);
	return 0;
}

static inline int od_backend_startup_timeout(od_server_t *server, kiwi_params_t *route_params, od_client_t *client, uint32_t timeout)
{
	od_instance_t *instance = server->global->instance;
	od_route_t *route = server->route;

#define DEFAULT_ARGV_SIZE 6

	kiwi_fe_arg_t argv[DEFAULT_ARGV_SIZE +
			   2 * route->rule->backend_startup_vars_sz];

	kiwi_fe_arg_t default_argv[] = {
		{ "user", 5 },
		{ route->id.user, route->id.user_len },
		{ "database", 9 },
		{ route->id.database, route->id.database_len },
		{ "replication", 12 },
		{ NULL, 0 }
	};

	od_debug(&instance->logger, "startup", NULL, server,
		 "startup server connection with user %s & database %s",
		 route->id.user, route->id.database);

	for (size_t i = 0; i < route->rule->backend_startup_vars_sz; i++) {
		argv[i << 1].name = route->rule->backend_startup_vars[i].name;
		argv[i << 1].len =
			route->rule->backend_startup_vars[i].name_len + 1;
		argv[i << 1 | 1].name =
			route->rule->backend_startup_vars[i].value;
		argv[i << 1 | 1].len =
			route->rule->backend_startup_vars[i].value_len + 1;
	}

	int argc = route->rule->backend_startup_vars_sz * 2;

	for (size_t i = 0; i < DEFAULT_ARGV_SIZE; ++i) {
		argv[argc + i] = default_argv[i];
	}

	argc += 4;

	if (route->id.physical_rep) {
		argv[argc + 1].name = "on";
		argv[argc + 1].len = 3;
		argc += 2;
	} else if (route->id.logical_rep) {
		argv[argc + 1].name = "database";
		argv[argc + 1].len = 9;
		argc += 2;
	}

	machine_msg_t *msg;
	msg = kiwi_fe_write_startup_message(NULL, argc, argv);
	if (msg == NULL)
		return -1;
	int rc;
	rc = od_write_timeout(&server->io, msg, timeout);
	if (rc == -1) {
		od_error(&instance->logger, "startup", NULL, server,
			 "write error: %s", od_io_error(&server->io));
		return -1;
	}

	/* update request count and sync state */
	od_server_sync_request(server, 1);
	assert(server->client);

	for (;;) {
		msg = od_read(&server->io, timeout);
		if (msg == NULL) {
			od_error(&instance->logger, "startup", client, server,
				 "read error: %s", od_io_error(&server->io));
			return -1;
		}

		kiwi_be_type_t type = *(char *)machine_msg_data(msg);
		od_debug(&instance->logger, "startup", client, server,
			 "received packet type: %s",
			 kiwi_be_type_to_string(type));

		switch (type) {
		case KIWI_BE_READY_FOR_QUERY:
			od_backend_ready(server, machine_msg_data(msg),
					 machine_msg_size(msg));
			machine_msg_free(msg);
			return 0;
		case KIWI_BE_AUTHENTICATION:
			rc = od_auth_backend(server, msg, client);
			machine_msg_free(msg);
			if (rc == -1)
				return -1;
			break;
		case KIWI_BE_BACKEND_KEY_DATA:
			rc = kiwi_fe_read_key(machine_msg_data(msg),
					      machine_msg_size(msg),
					      &server->key);
			machine_msg_free(msg);
			if (rc == -1) {
				od_error(
					&instance->logger, "startup", client,
					server,
					"failed to parse BackendKeyData message");
				return -1;
			}
			break;
		case KIWI_BE_PARAMETER_STATUS: {
			char *name;
			uint32_t name_len;
			char *value;
			uint32_t value_len;
			rc = kiwi_fe_read_parameter(machine_msg_data(msg),
						    machine_msg_size(msg),
						    &name, &name_len, &value,
						    &value_len);
			if (rc == -1) {
				machine_msg_free(msg);
				od_error(
					&instance->logger, "startup", client,
					server,
					"failed to parse ParameterStatus message");
				return -1;
			}

			/* set server parameters */
			kiwi_vars_update(&server->vars, name, name_len, value,
					 value_len);

			if (route_params) {
				// skip volatile params
				// we skip in_hot_standby here because it may change
				// during connection lifetime, if server was
				// promoted
				if (name_len != sizeof("in_hot_standby") || strncmp(name, "in_hot_standby", name_len)) {
					kiwi_param_t *param;
					param = kiwi_param_allocate(name,
								    name_len,
								    value,
								    value_len);
					if (param)
						kiwi_params_add(route_params, param);
				}
			}

			machine_msg_free(msg);
			break;
		}
		case KIWI_BE_NOTICE_RESPONSE:
			machine_msg_free(msg);
			break;
		case KIWI_BE_ERROR_RESPONSE:
			od_backend_error(server, "startup",
					 machine_msg_data(msg),
					 machine_msg_size(msg));
			server->error_connect = msg;
			return -1;
		default:
			machine_msg_free(msg);
			od_debug(&instance->logger, "startup", client, server,
				 "unexpected message: %s",
				 kiwi_be_type_to_string(type));
			return -1;
		}
	}
	od_unreachable();
	return 0;
}

static inline int od_backend_startup(od_server_t *server, kiwi_params_t *route_params, od_client_t *client)
{
	return od_backend_startup_timeout(server, route_params, client, UINT32_MAX);
}

/* Similar to od_backend_connect_to, but with a timeout */
static inline int od_backend_connect_to_timeout(od_server_t *server, char *context,
					char *host, int port,
					od_tls_opts_t *tlsopts, uint32_t timeout)
{
	od_instance_t *instance = server->global->instance;
	assert(server->io.io == NULL);
	assert(host != NULL);

	/* create io handle */
	machine_io_t *io;
	io = machine_io_create();
	if (io == NULL)
		return -1;

	/* set network options */
	machine_set_nodelay(io, instance->config.nodelay);
	if (instance->config.keepalive > 0) {
		machine_set_keepalive(io, 1, instance->config.keepalive,
				      instance->config.keepalive_keep_interval,
				      instance->config.keepalive_probes,
				      instance->config.keepalive_usr_timeout);
	}

	int rc;
	rc = od_io_prepare(&server->io, io, instance->config.readahead);
	if (rc == -1) {
		od_error(&instance->logger, context, NULL, server,
			 "failed to set server io");
		machine_close(io);
		machine_io_free(io);
		return -1;
	}

	/* set tls options */
	if (tlsopts->tls_mode != OD_CONFIG_TLS_DISABLE) {
		server->tls = od_tls_backend(tlsopts);
		if (server->tls == NULL)
			return -1;
	}

	uint64_t time_connect_start = 0;
	if (instance->config.log_session)
		time_connect_start = machine_time_us();

	struct sockaddr_un saddr_un;
	struct sockaddr_in saddr_v4;
	struct sockaddr_in6 saddr_v6;
	struct sockaddr *saddr;
	struct addrinfo *ai = NULL;

	/* resolve server address */
	if (od_host_is_local(instance, host) && od_unix_socket_available(instance, port) && instance->config.use_unix_socket_if_possible) {
		/* if unix socket is available, use it instead of IPv4/v6*/
		/* set unix socket path */
		memset(&saddr_un, 0, sizeof(saddr_un));
		saddr_un.sun_family = AF_UNIX;
		saddr = (struct sockaddr *)&saddr_un;
		od_snprintf(saddr_un.sun_path, sizeof(saddr_un.sun_path),
			    "%s/.s.PGSQL.%d", instance->config.unix_socket_dir,
			    port);
	}
	else {
		/* assume IPv6 or IPv4 is specified */
		int rc_resolve = -1;
		if (strchr(host, ':')) {
			/* v6 */
			memset(&saddr_v6, 0, sizeof(saddr_v6));
			saddr_v6.sin6_family = AF_INET6;
			saddr_v6.sin6_port = htons(port);
			rc_resolve =
				inet_pton(AF_INET6, host, &saddr_v6.sin6_addr);
			saddr = (struct sockaddr *)&saddr_v6;
		} else {
			/* v4 or hostname */
			memset(&saddr_v4, 0, sizeof(saddr_v4));
			saddr_v4.sin_family = AF_INET;
			saddr_v4.sin_port = htons(port);
			rc_resolve =
				inet_pton(AF_INET, host, &saddr_v4.sin_addr);
			saddr = (struct sockaddr *)&saddr_v4;
		}

		/* schedule getaddrinfo() execution */
		if (rc_resolve != 1) {
			char rport[16];
			od_snprintf(rport, sizeof(rport), "%d", port);

			rc = machine_getaddrinfo(host, rport, NULL, &ai, 0);
			if (rc != 0) {
				od_error(&instance->logger, context, NULL,
					 server, "failed to resolve %s:%d",
					 host, port);
				return NOT_OK_RESPONSE;
			}
			assert(ai != NULL);
			saddr = ai->ai_addr;
		}
		/* connected */

	} 

	uint64_t time_resolve = 0;
	if (instance->config.log_session) {
		time_resolve = machine_time_us() - time_connect_start;
	}

	/* connect to server */
	rc = machine_connect(server->io.io, saddr, timeout);
	if (ai) {
		freeaddrinfo(ai);
	}

	if (rc == NOT_OK_RESPONSE) {
		if (host) {
			od_error(&instance->logger, context, server->client,
				 server, "failed to connect to %s:%d", host,
				 port);
		} else {
			od_error(&instance->logger, context, server->client,
				 server, "failed to connect to %s",
				 saddr_un.sun_path);
		}
		return NOT_OK_RESPONSE;
	}

	/* do tls handshake */
	if (tlsopts->tls_mode != OD_CONFIG_TLS_DISABLE) {
		rc = od_tls_backend_connect(server, &instance->logger, tlsopts);
		if (rc == NOT_OK_RESPONSE) {
			return NOT_OK_RESPONSE;
		}
	}

	uint64_t time_connect = 0;
	if (instance->config.log_session) {
		time_connect = machine_time_us() - time_connect_start;
	}

	/* log server connection */
	if (instance->config.log_session) {
		if (host) {
			od_log(&instance->logger, context, server->client,
			       server,
			       "new server connection %s:%d (connect time: %d usec, "
			       "resolve time: %d usec)",
			       host, port, (int)time_connect,
			       (int)time_resolve);
		} else {
			od_log(&instance->logger, context, server->client,
			       server,
			       "new server connection %s (connect time: %d usec, resolve "
			       "time: %d usec)",
			       saddr_un.sun_path, (int)time_connect,
			       (int)time_resolve);
		}
	}

	return 0;
}

static inline int od_backend_connect_to(od_server_t *server, char *context,
					char *host, int port,
					od_tls_opts_t *tlsopts)
{
	return od_backend_connect_to_timeout(server, context, host, port, tlsopts, UINT32_MAX);
}

int od_storage_parse_rw_check_response(machine_msg_t *msg)
{
	char *pos = (char *)machine_msg_data(msg) + 1;
	uint32_t pos_size = machine_msg_size(msg) - 1;

	/* size */
	uint32_t size;
	int rc;
	rc = kiwi_read32(&size, &pos, &pos_size);
	if (kiwi_unlikely(rc == -1))
		goto error;
	/* count */
	uint16_t count;
	rc = kiwi_read16(&count, &pos, &pos_size);

	if (kiwi_unlikely(rc == -1))
		goto error;

	if (count != 1)
		goto error;

	/* (not used) */
	uint32_t resp_len;
	rc = kiwi_read32(&resp_len, &pos, &pos_size);
	if (kiwi_unlikely(rc == -1)) {
		goto error;
	}

	/* we expect exactly one row */
	if (resp_len != 1) {
		return NOT_OK_RESPONSE;
	}
	/* pg is in recovery false means db is open for write */
	if (pos[0] == 'f') {
		return OK_RESPONSE;
	}
	/* fallthrough to error */
error:
	return NOT_OK_RESPONSE;
}


static inline od_retcode_t od_backend_attemp_connect_with_tsa_timeout(
	od_server_t *server, char *context, kiwi_params_t *route_params,
	char *host, int port, od_tls_opts_t *opts,
	od_target_session_attrs_t attrs, od_client_t *client, uint32_t timeout)
{
	assert(attrs == OD_TARGET_SESSION_ATTRS_RO ||
	       attrs == OD_TARGET_SESSION_ATTRS_RW);

	od_retcode_t rc;
	machine_msg_t *msg;
	od_route_t *route = server->route;

	rc = od_backend_connect_to_timeout(server, context, host, port, opts, timeout);
	if (rc == NOT_OK_RESPONSE) {
		od_backend_error_is_too_many_connections(client);
		od_backend_close_connection(server);
		return rc;
	}

	/* send startup and do initial configuration */
	rc = od_backend_startup_timeout(server, route_params, client, timeout);
	if (rc == NOT_OK_RESPONSE) {
		od_backend_error_is_too_many_connections(client);
		od_backend_close_connection(server);
		return rc;
	}

	/* Check if server is read-write */
	msg = od_query_do(server, context, "SELECT pg_is_in_recovery()", NULL, NULL);
	if (msg == NULL) {
		od_backend_error_is_too_many_connections(client);
		od_backend_close_connection(server);
		return NOT_OK_RESPONSE;
	}

	switch (attrs) {
	case OD_TARGET_SESSION_ATTRS_RW:
		rc = od_storage_parse_rw_check_response(msg);
		if (rc != REPLICA_ROLE_STANDBY)
			route->server_pool[server->endpoint_selector].replica_role = REPLICA_ROLE_STANDBY;
		break;
	case OD_TARGET_SESSION_ATTRS_RO:
		/* this is primary, but we are forsed to find ro backend */
		if (od_storage_parse_rw_check_response(msg) == OK_RESPONSE) {
			rc = NOT_OK_RESPONSE;
			route->server_pool[server->endpoint_selector].replica_role = REPLICA_ROLE_PRIMARY;
		} else {
			rc = OK_RESPONSE;
		}
		break;
	default:
		abort();
	}
	machine_msg_free(msg);

	if (rc != OK_RESPONSE) {
		od_backend_close_connection(server);
	}

	return rc;
}

static inline od_retcode_t od_backend_attemp_connect_with_tsa(
	od_server_t *server, char *context, kiwi_params_t *route_params,
	char *host, int port, od_tls_opts_t *opts,
	od_target_session_attrs_t attrs, od_client_t *client)
{
	return od_backend_attemp_connect_with_tsa_timeout(server, context, 
				route_params, host, port, opts, attrs, client, UINT32_MAX);
}

/*
 * Set the endpoint_selector of server.
 * When a server is created, it is assigned to the first pool (by setting endpoint_selector = 0).
 * After a connection is actually established (through od_backend_connect), it needs to be placed in the pool
 * where the endpoint is located.
 */
static inline void od_server_set_endpoint_selector(od_server_t *server, size_t selector)
{
	od_route_t *route = server->route;
	od_server_pool_t *src;
	od_server_pool_t *dst;

	if (server->endpoint_selector != selector) {
		assert(selector < (size_t)route->server_pool_size);
		od_route_lock(route);

		src = &route->server_pool[server->endpoint_selector];
		dst = &route->server_pool[selector];
		od_server_state_t state = server->state;
		od_pg_server_pool_set(src, server, OD_SERVER_UNDEF);
		od_pg_server_pool_set(dst, server, state);
		server->endpoint_selector = selector;

		od_route_unlock(route);
	}
}

/*
 * Connect to the node at the given index.
 * The index is corresponding to the endpoint in the storage's endpoints array.
 * Also check for and set the replica role of the node.
 * TODO: review this function
 */
int od_backend_connect_index(od_server_t *server, char *context,
	kiwi_params_t *route_params, od_client_t *client, size_t index, uint32_t timeout)
{
	od_route_t *route = server->route;
	assert(route != NULL);
	od_rule_storage_t *storage;

	storage = route->rule->storage;
	od_retcode_t rc;

	assert(index < storage->endpoints_count);
	char *host = NULL; /* For UNIX socket */
	int port = storage->port;
	if (storage->endpoints_count) {
		host = storage->endpoints[index].host;
		if (storage->endpoints[index].port)
			port = storage->endpoints[index].port;
	}
	rc = od_backend_connect_to_timeout(server, context, host, port,
					   storage->tls_opts,timeout);
	if (rc == NOT_OK_RESPONSE) {
		return NOT_OK_RESPONSE;
	}

	/* send startup and do initial configuration */
	rc = od_backend_startup(server, route_params, client);
	if (rc == NOT_OK_RESPONSE) {
		return rc;
	}
	od_server_set_endpoint_selector(server, index);

	return rc;
}

static int enable_backend_plugin(od_server_t *server, char *context)
{
	char *query = "DO $$ BEGIN"
		"	IF EXISTS(SELECT 1 FROM pg_proc WHERE proname = 'load_query_check') THEN"
		"		PERFORM load_query_check();"
		"	END IF;"
		"END $$;"
		"SELECT set_config(name, 'on', null) from pg_settings where name = 'query_check.enabled'";
	machine_msg_t *msg;
	int rc = -1;

	do {
		bool failed = false;
		int retry_cnt = 0;
		while (retry_cnt < 3) {
			msg = od_query_do(server, context, query, NULL, &failed);
			if (!failed)
				break;
			retry_cnt += 1;
			machine_sleep(10);
		}
		if (!msg)
			break;
		char *pos = (char *)machine_msg_data(msg) + 1;
		uint32_t pos_size = machine_msg_size(msg) - 1;

		/* size */
		uint32_t size;
		rc = kiwi_read32(&size, &pos, &pos_size);
		if (od_unlikely(rc == -1))
			break;

		/* filed count */
		uint16_t count;
		rc = kiwi_read16(&count, &pos, &pos_size);

		if (od_unlikely(rc == -1))
			break;
		if (od_unlikely(count != 1))
			break;

		/* response length */
		uint32_t resp_len;
		rc = kiwi_read32(&resp_len, &pos, &pos_size);
		if (od_unlikely(rc == -1)) {
			break;
		}

		if (resp_len == 2 && strncasecmp(pos, "on", 2) == 0) {
			rc = 0;
		}
	} while (false);

	if (msg)
		machine_msg_free(msg);
	return rc;
}

/* Similar to od_backend_connect, but with a timeout */
int od_backend_connect_timeout(od_server_t *server, char *context,
		       kiwi_params_t *route_params, od_client_t *client,
			   uint32_t timeout)
{
	od_route_t *route = server->route;
	assert(route != NULL);
	od_rule_storage_t *storage = route->rule->storage;
	size_t idx = server->endpoint_selector;
	od_retcode_t rc;

	od_instance_t *instance = server->global->instance;

	char *host = NULL; /* For UNIX socket */
	int port = storage->port;
	if (storage->endpoints_count) {
		host = storage->endpoints[idx].host;
		if (storage->endpoints[idx].port)
			port = storage->endpoints[idx].port;
	}
	rc = od_backend_connect_to_timeout(server, context, host, port,
									   storage->tls_opts, timeout);
	if (rc == NOT_OK_RESPONSE) { 
		od_backend_error_is_too_many_connections(client);
		return NOT_OK_RESPONSE;
	}

	/* send startup and do initial configuration */
	rc = od_backend_startup_timeout(server, route_params, client, timeout);
	if (rc == OK_RESPONSE) {
		/* enable backend plugin */
		if (enable_backend_plugin(server, context) == -1) {
			if (machine_timedout()) {
				od_error(&instance->logger, context, NULL, server,
						"failed to enable plugin 'query_check' on %s:%d due to IO timeout",
						host, port);
				od_backend_close_connection(server);
				rc = NOT_OK_RESPONSE;
			} else {
					od_log(&instance->logger, context, client, server,
					"failed to enable plugin 'query_check' on %s:%d, the server will run in session mode",
					host, port);
			}
		}
		else {
			server->backend_plugin_installed = true;
		}
	} else {
		od_backend_error_is_too_many_connections(client);
	}

	return rc;
}

int od_backend_connect(od_server_t *server, char *context,
		       kiwi_params_t *route_params, od_client_t *client)
{
	return od_backend_connect_timeout(server, context, route_params, client, UINT32_MAX);
}

// TODO: dose od_backend_connect_index_cancel needs to be implemented
int od_backend_connect_cancel(od_server_t *server, od_rule_storage_t *storage,
			      kiwi_key_t *key)
{
	od_instance_t *instance = server->global->instance;
	/* connect to server */
	int rc;
	char *host = NULL; /* For UNIX socket */
	int port = storage->port;
	if (storage->endpoints_count) {
		host = storage->endpoints[server->endpoint_selector].host;
		if (storage->endpoints[server->endpoint_selector].port)
			port = storage->endpoints[server->endpoint_selector]
				       .port;
	}
	rc = od_backend_connect_to(server, "cancel", host, port,
				   storage->tls_opts);
	if (rc == NOT_OK_RESPONSE) {
		return NOT_OK_RESPONSE;
	}

	/* send cancel request */
	machine_msg_t *msg;
	msg = kiwi_fe_write_cancel(NULL, key->key_pid, key->key);
	if (msg == NULL)
		return -1;

	rc = od_write(&server->io, msg);
	if (rc == -1) {
		od_error(&instance->logger, "cancel", NULL, NULL,
			 "write error: %s", od_io_error(&server->io));
		return -1;
	}

	return 0;
}

int od_backend_update_parameter(od_server_t *server, char *context, char *data,
				uint32_t size, int server_only)
{
	od_instance_t *instance = server->global->instance;
	od_client_t *client = server->client;

	char *name;
	uint32_t name_len;
	char *value;
	uint32_t value_len;

	int rc;
	rc = kiwi_fe_read_parameter(data, size, &name, &name_len, &value,
				    &value_len);
	if (rc == -1) {
		od_error(&instance->logger, context, NULL, server,
			 "failed to parse ParameterStatus message");
		return -1;
	}

	/* update server only or client and server parameter */
	od_debug(&instance->logger, context, client, server, "%.*s = %.*s",
		 name_len, name, value_len, value);

	int status;
	if (server_only) {
		status = kiwi_vars_update(&server->vars, name, name_len, value,
				value_len);
	} else {
		status = kiwi_vars_update_both(&client->vars, &server->vars, name,
				name_len, value, value_len);
	}
#if 0
	if (status == -1) {
		od_hashmap_elt_t key = {
			.data = name,
			.len = name_len
		}, val = {
			.data = value,
			.len = value_len
		};
		od_hash_t hash = od_murmur_hash(key.data, key.len);
		od_hashmap_elt_t *value_ptr = &val;

		od_hashmap_insert(server->other_vars, hash, &key, &value_ptr, true);
		if (!server_only)
			od_hashmap_insert(client->other_vars, hash, &key, &value_ptr, true);
	}
#else
	(void)status;
#endif
	return 0;
}

int od_backend_ready_wait(od_server_t *server, char *context, int count,
			  uint32_t time_ms, uint32_t ignore_errors)
{
	od_instance_t *instance = server->global->instance;
	int query_rc;
	query_rc = 0;

	(void) count;
	for (; !od_server_synchronized(server);) {
		machine_msg_t *msg;
		msg = od_read(&server->io, time_ms);
		if (msg == NULL) {
			if (!machine_timedout()) {
				od_error(&instance->logger, context,
					 server->client, server,
					 "read error: %s",
					 od_io_error(&server->io));
			}
			return -1;
		}
		kiwi_be_type_t type = *(char *)machine_msg_data(msg);
		od_debug(&instance->logger, context, server->client, server,
			 "%s", kiwi_be_type_to_string(type));

		if (type == KIWI_BE_PARAMETER_STATUS) {
			/* update server parameter */
			int rc;
			rc = od_backend_update_parameter(server, context,
							 machine_msg_data(msg),
							 machine_msg_size(msg),
							 1);
			machine_msg_free(msg);
			if (rc == -1) {
				return -1;
			}
		} else if (type == KIWI_BE_ERROR_RESPONSE) {
			od_backend_error(server, context, machine_msg_data(msg),
					 machine_msg_size(msg));
			machine_msg_free(msg);
			if (!ignore_errors) {
				query_rc = -1;
			}
		} else if (type == KIWI_BE_READY_FOR_QUERY) {
			od_backend_ready(server, machine_msg_data(msg),
					 machine_msg_size(msg));
			machine_msg_free(msg);
		}
		else if (type == KIWI_BE_NOTICE_RESPONSE) {
			od_backend_notice(server, machine_msg_data(msg),
					machine_msg_size(msg), 1);
			machine_msg_free(msg);
		} else {
			machine_msg_free(msg);
		}
	}

	return query_rc;
}


od_retcode_t od_backend_query_send_timeout(od_server_t *server, char *context,
				   char *query, char *param, int len, uint32_t timeout)
{
	od_instance_t *instance = server->global->instance;

	machine_msg_t *msg;
	if (param) {
		msg = kiwi_fe_write_prep_stmt(NULL, query, param);
	} else {
		msg = kiwi_fe_write_query(NULL, query, len);
	}

	if (msg == NULL) {
		return NOT_OK_RESPONSE;
	}

	int rc;
	rc = od_write_timeout(&server->io, msg, timeout);
	if (rc == -1) {
		od_error(&instance->logger, context, server->client, server,
			 "write error: %s", od_io_error(&server->io));
		return NOT_OK_RESPONSE;
	}

	/* update server sync state */
	od_server_sync_request(server, 1);
	return OK_RESPONSE;
}

od_retcode_t od_backend_query_send(od_server_t *server, char *context,
				   char *query, char *param, int len)
{
	return od_backend_query_send_timeout(server, context, query, param, len, UINT32_MAX);
}

od_retcode_t od_backend_query(od_server_t *server, char *context, char *query,
			      char *param, int len, uint32_t timeout,
			      uint32_t count, uint32_t ignore_errors)
{
	if (od_backend_query_send(server, context, query, param, len) ==
	    NOT_OK_RESPONSE) {
		return NOT_OK_RESPONSE;
	}
	od_retcode_t rc = od_backend_ready_wait(server, context, count, timeout,
						ignore_errors);
	return rc;
}


static inline bool od_host_is_local(od_instance_t *instance, char *host)
{
	struct ifaddrs *ifaddr, *ifa;
	int family, s;
	char local_host[NI_MAXHOST];

	if (getifaddrs(&ifaddr) == -1)
	{
		od_log(&instance->logger, "", NULL, NULL,
		       "getifaddrs error: %s", strerror(errno));	
		return false;
	}

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
	{
		if (ifa->ifa_addr == NULL)
			continue;
		family = ifa->ifa_addr->sa_family;

		if (family == AF_INET || family == AF_INET6)
		{
			s = getnameinfo(ifa->ifa_addr, (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6), local_host, NI_MAXHOST, NULL, 0 ,NI_NUMERICHOST);
			if (s != 0)
			{
				od_log(&instance->logger, "", NULL, NULL,
					"getnameinfo() failed: %s", gai_strerror(s));	
				freeifaddrs(ifaddr);
				return false;	
			}
            if(strcmp(host, local_host) == 0)
			{
				od_log(&instance->logger, "", NULL, NULL,
				    "host %s match local host: %s", host, local_host);
                freeifaddrs(ifaddr);
				return true;
			}
		}
	}

	freeifaddrs(ifaddr);
	return false;
}

static inline bool od_unix_socket_available(od_instance_t *instance, int port)
{
	char unix_socket_path[108];
	od_snprintf(unix_socket_path, sizeof(unix_socket_path),
		"%s/.s.PGSQL.%d", instance->config.unix_socket_dir,
		port);

	if(access(unix_socket_path, F_OK) != 0)
	{
		od_log(&instance->logger, "", NULL, NULL,
			"expected unix socket path for host [%d]: %s doesn't exist", port, unix_socket_path);	
		return false;
	}
	return true;
}
