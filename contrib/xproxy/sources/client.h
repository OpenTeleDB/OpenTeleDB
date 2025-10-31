#ifndef ODYSSEY_CLIENT_H
#define ODYSSEY_CLIENT_H

#include "scram.h"
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

typedef struct od_client od_client_t;

typedef enum {
	OD_CLIENT_UNDEF,
	OD_CLIENT_PENDING,
	OD_CLIENT_ACTIVE,
	OD_CLIENT_QUEUE
} od_client_state_t;

#define OD_CLIENT_MAX_PEERLEN 128

struct od_client {
	od_client_state_t state;
	od_pool_client_type_t type;
	od_id_t id;
	uint64_t coroutine_id;
	machine_tls_t *tls;
	od_io_t io;
	machine_cond_t *cond;
	od_relay_t relay;
	od_rule_t *rule;
	od_config_listen_t *config_listen;

	uint64_t time_accept;
	uint64_t time_setup;
	uint64_t time_last_active;
	uint64_t pool_wait_start;

	bool is_watchdog;

	kiwi_be_startup_t startup;
	kiwi_vars_t vars;
	kiwi_key_t key;
	od_hashmap_t *other_vars; /* gucs without GUC_REPORT flag */

	kiwi_fe_type_t cached_query_type;
	uint32_t cached_query_len;
	char *cached_query; /* 缓存的是 QUERY 或 PARSE 语句，不包括命令字 */

	bool has_error;
	char error_code[128];
	char error_msg[1024];

	od_server_t *server;
	/* od_route_t */
	void *route;
	char peer[OD_CLIENT_MAX_PEERLEN];

	/* parse name of current parse packet */
	char parse_name[64];
	
	/* desc preparet statements ids */
	od_hashmap_t *prep_stmt_ids;

	od_hashmap_t *prep_stmt_target_roles;

	/* passwd from config rule */
	kiwi_password_t password;
	uint8_t scram_client_key[OD_SCRAM_MAX_KEY_LEN];

	/* user - proveded passwd, fallback to use this when no other option is available*/
	kiwi_password_t received_password;
	od_global_t *global;
	od_list_t link_pool;
	od_list_t link;

	/* Used to kill client in kill_client or odyssey reload */
	od_atomic_u64_t killed;

	/* storage_user & storage_password provided by ldapsearch result */
#ifdef LDAP_FOUND
	char *ldap_storage_username;
	int ldap_storage_username_len;
	char *ldap_storage_password;
	int ldap_storage_password_len;
	char *ldap_auth_dn;
#endif

	/* external_id for logging additional ifno about client */
	char *external_id;
	/* wait for idle connection */
	bool wait_for_idle;
	bool backend_plugin_installed;

	/* stats */
	uint64_t query_start_time;
	uint64_t transaction_start_time;
	uint64_t last_query_finished_time;
};

static const size_t OD_CLIENT_DEFAULT_HASHMAP_SZ = 420;

static inline od_retcode_t od_client_init_hm(od_client_t *client)
{
	client->prep_stmt_ids = od_hashmap_create(OD_CLIENT_DEFAULT_HASHMAP_SZ, false);
	if (client->prep_stmt_ids == NULL) {
		return NOT_OK_RESPONSE;
	}
	return OK_RESPONSE;
}

static inline void od_client_init(od_client_t *client)
{
	client->state = OD_CLIENT_UNDEF;
	client->type = OD_POOL_CLIENT_EXTERNAL;
	client->coroutine_id = 0;
	client->tls = NULL;
	client->cond = NULL;
	client->rule = NULL;
	client->config_listen = NULL;
	client->server = NULL;
	client->route = NULL;
	client->global = NULL;
	client->time_accept = 0;
	client->time_setup = 0;
#ifdef LDAP_FOUND
	client->ldap_storage_username = NULL;
	client->ldap_storage_username_len = 0;
	client->ldap_storage_password = NULL;
	client->ldap_storage_password_len = 0;
	client->ldap_auth_dn = NULL;
#endif
	client->external_id = NULL;

	client->cached_query_type = 0;
	client->cached_query_len = 0;
	client->cached_query = NULL;
	
	client->has_error = false;

	kiwi_be_startup_init(&client->startup);
	kiwi_vars_init(&client->vars);
	kiwi_key_init(&client->key);
	client->other_vars = od_hashmap_create(32, false);

	od_io_init(&client->io);
	od_relay_init(&client->relay, &client->io);

	kiwi_password_init(&client->password);
	kiwi_password_init(&client->received_password);

	od_list_init(&client->link_pool);
	od_list_init(&client->link);

	client->prep_stmt_ids = NULL;

	client->prep_stmt_target_roles = od_hashmap_create(OD_CLIENT_DEFAULT_HASHMAP_SZ, false); // TODO

	od_atomic_u64_set(&client->killed, 0);
	client->wait_for_idle = false;
	client->backend_plugin_installed = true;
	client->query_start_time = 0;
	client->transaction_start_time = 0;
	client->last_query_finished_time = 0;
	client->time_last_active = 0;
	client->is_watchdog = false;
}

static inline od_client_t *od_client_allocate(void)
{
	od_client_t *client = malloc(sizeof(od_client_t));
	if (client == NULL)
		return NULL;
	od_client_init(client);
	return client;
}

static inline void od_client_free(od_client_t *client)
{
	od_relay_free(&client->relay);
	od_io_free(&client->io);
	if (client->cond)
		machine_cond_free(client->cond);
	/* clear password if saved any */
	kiwi_password_free(&client->password);
	kiwi_password_free(&client->received_password);
	if (client->prep_stmt_ids) {
		od_hashmap_free(client->prep_stmt_ids);
	}
	if (client->prep_stmt_target_roles) {
		od_hashmap_free(client->prep_stmt_target_roles);
	}
	if (client->external_id) {
		free(client->external_id);
	}
	od_hashmap_free(client->other_vars);
	if (client->cached_query != NULL)
	{
		client->cached_query_type = 0;
		client->cached_query_len = 0;
		free(client->cached_query);
		client->cached_query = NULL;
	}
	free(client);
}

static inline void od_client_kill(od_client_t *client)
{
	od_atomic_u64_set(&client->killed, 1UL);
}

static inline void od_stat_pool_wait_start(od_client_t *client)
{
	client->pool_wait_start = machine_time_us();
}

static inline void od_stat_pool_wait_end(od_stat_t *stat, od_client_t *client)
{
	int64_t diff = machine_time_us() - client->pool_wait_start;
	if (diff > 0) {
		od_atomic_u64_add(&stat->pool_wait_time, diff);
		if (stat->enable_quantiles)
			od_histogram_update(stat->pool_wait_hgram, diff);
	}
	client->pool_wait_start = 0;
}

static inline void od_stat_client_query_start(od_client_t *client, uint64_t ts)
{
	if (!client->query_start_time)
		client->query_start_time= ts;

	if (!client->transaction_start_time)
		client->transaction_start_time = ts;
}

static inline void od_stat_client_query_end(od_client_t *client, bool in_transaction)
{
	client->last_query_finished_time = machine_time_us();
	client->query_start_time = 0;
	if (!in_transaction)
		client->transaction_start_time = 0;
}

#endif /* ODYSSEY_CLIENT_H */
