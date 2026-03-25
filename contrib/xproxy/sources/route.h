#ifndef ODYSSEY_ROUTE_H
#define ODYSSEY_ROUTE_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

typedef struct od_endpoint_state od_endpoint_state_t;
struct od_endpoint_state {
	bool *is_valid_endpoint;
	size_t primary_endpoint_index;
};

typedef struct od_route od_route_t;

struct od_route {
	od_rule_t *rule;
	od_route_id_t id;

	od_stat_t stats;
	od_stat_t stats_prev;
	bool stats_mark_db;

	int server_pool_size;
	od_server_pool_t *server_pool;
	od_client_pool_t client_pool;

	kiwi_params_lock_t params;
	int64_t tcp_connections;
	long last_heartbeat;
	machine_channel_t **wait_bus;
	uint32_t *waiters;
	pthread_mutex_t lock;

	od_error_logger_t *err_logger;
	bool extra_logging_enabled;

	od_list_t link;

	od_endpoint_state_t *endpoint_state;
};

static inline od_endpoint_state_t *od_endpoint_state_allocate(int server_count) {
	od_endpoint_state_t *state = malloc(sizeof(od_endpoint_state_t));
	if (state == NULL) {
		return NULL;
	}

	state->is_valid_endpoint = malloc(sizeof(int) * server_count);
	if (state->is_valid_endpoint == NULL) {
		return NULL;
	}

	memset(state->is_valid_endpoint, 0, sizeof(int) * server_count);

	state->primary_endpoint_index = 0;

	return state;
}

static inline void od_endpoint_state_free(od_endpoint_state_t *state) {
	if (state != NULL) {
		if (state->is_valid_endpoint != NULL) {
			free(state->is_valid_endpoint);
			state->is_valid_endpoint = NULL;
		}

		free(state);
		state = NULL; 
	}
}

/*
 * Select a valid endpoint from the pool using round-robin algorithm with a given weight.
 */
static inline int od_route_weighted_random_select(od_route_t *route, bool is_unrecommended_endpoint[]) {
	od_rule_storage_t *storage = route->rule->storage;
	od_endpoint_state_t *endpoint_state = route->endpoint_state;

	int selected_endpoint = endpoint_state->primary_endpoint_index;
	double random_value;
	double total_weight = 0.0;
	int heartbeat_lag = 0;
	bool *endpoint_valid = NULL;

	endpoint_valid = malloc(sizeof(bool) * storage->endpoints_count);
	memset(endpoint_valid, false, sizeof(bool) * storage->endpoints_count);

	for (size_t i = 0; i < storage->endpoints_count; ++i) {
		if (is_unrecommended_endpoint[i])
			continue;

		heartbeat_lag = machine_timeofday_sec() - route->server_pool[i].last_heartbeat;
		heartbeat_lag = heartbeat_lag < 0 ? 0 : heartbeat_lag;
		bool catchup_valid = (heartbeat_lag <= route->rule->catchup_timeout) || (route->rule->catchup_timeout == 0);
		bool replag_valid = (route->server_pool[i].last_replag_musec <= route->rule->replication_delay_threshold) || (route->rule->replication_delay_threshold == 0);

		endpoint_valid[i] = catchup_valid && replag_valid;

		if (endpoint_valid[i] == true) {
			total_weight += storage->endpoints[i].weight;
		}
	}

	random_value = total_weight * ((double)rand() / (RAND_MAX + 1.0));

	total_weight = 0.0;
	for (size_t i = 0; i < storage->endpoints_count; ++i) {
		if (is_unrecommended_endpoint[i])
			continue;
		
		if (route->server_pool[i].replica_role == REPLICA_ROLE_UNKONWN)
			continue;

		if (endpoint_valid[i] == true && storage->endpoints[i].weight > 0.0) {
			if (total_weight + storage->endpoints[i].weight >= random_value) {
				selected_endpoint = i;
				break;
			}
			total_weight += storage->endpoints[i].weight;
		}
	}

	free(endpoint_valid);
	endpoint_valid = NULL;
	return selected_endpoint;
}

static inline void od_route_init(od_route_t *route, int server_count, bool extra_route_logging)
{
	int i;
	route->rule = NULL;
	route->tcp_connections = 0;
	route->last_heartbeat = 0;

	od_route_id_init(&route->id);

	route->server_pool_size = server_count;
	if (server_count > 0)
		route->server_pool =
			(od_server_pool_t *)malloc(server_count * sizeof(od_server_pool_t));
	else
		route->server_pool = NULL;
	for (i = 0; i < server_count; ++i) {
		od_server_pool_init(&route->server_pool[i]);
	}

	od_client_pool_init(&route->client_pool);

	/* stat init */
	route->stats_mark_db = false;
	route->extra_logging_enabled = extra_route_logging;
	if (extra_route_logging) {
		/* error logging */
		route->err_logger = od_err_logger_create_default();
	} else {
		route->err_logger = NULL;
	}

	od_stat_init(&route->stats, server_count);
	od_stat_init(&route->stats_prev, server_count);
	kiwi_params_lock_init(&route->params);
	od_list_init(&route->link);
	route->wait_bus = NULL;
	route->waiters = NULL;
	pthread_mutex_init(&route->lock, NULL);

	route->endpoint_state = od_endpoint_state_allocate(server_count);
}

static inline void od_route_free(od_route_t *route)
{
    int i;
	od_route_id_free(&route->id);

	if (route->server_pool_size > 0) {
		for (i = 0; i < route->server_pool_size; ++i)
			od_pg_server_pool_free(&route->server_pool[i]);
		free(route->server_pool);
		route->server_pool = NULL;
	}

	kiwi_params_lock_free(&route->params);
	if (route->wait_bus) {
		for (i = 0; i < route->server_pool_size; ++i) {
			if (route->wait_bus[i])
				machine_channel_free(route->wait_bus[i]);
		}
		free(route->wait_bus);
		route->wait_bus = NULL;
	}
	if (route->waiters) {
		free(route->waiters);
		route->waiters = NULL;
	}
	if (route->stats.enable_quantiles) {
		od_stat_free(&route->stats);
	}

	if (route->extra_logging_enabled) {
		od_err_logger_free(route->err_logger);
		route->err_logger = NULL;
	}

	pthread_mutex_destroy(&route->lock);
	od_endpoint_state_free(route->endpoint_state);
	free(route);
}

static inline od_route_t *od_route_allocate(int server_count)
{
	od_route_t *route = malloc(sizeof(od_route_t));
	if (route == NULL)
		return NULL;
	od_route_init(route, server_count, true);
	if (server_count > 0) {
		int i;
		route->waiters = malloc(server_count * sizeof(uint32));
		if (!route->waiters) {
			od_route_free(route);
			return NULL;
		}
		route->wait_bus = (machine_channel_t **)malloc(sizeof(void *) * server_count);
		if (!route->wait_bus) {
			od_route_free(route);
			return NULL;
		}
		memset(route->wait_bus, 0, sizeof(void *) * server_count);
		for (i=0; i<server_count; ++i) {
			route->waiters[i] = 0;
			route->wait_bus[i] = machine_channel_create();
			if (route->wait_bus[i] == NULL) {
				od_route_free(route);
				return NULL;
			}
		}
	}
	return route;
}

static inline void od_route_lock(od_route_t *route)
{
	pthread_mutex_lock(&route->lock);
}

static inline void od_route_unlock(od_route_t *route)
{
	pthread_mutex_unlock(&route->lock);
}

static inline int od_route_is_dynamic(od_route_t *route)
{
	return route->rule->db_is_default || route->rule->user_is_default;
}

static inline int od_route_match_compare_client_cb(od_client_t *client,
						   void **argv)
{
	return od_id_cmp(&client->id, argv[0]);
}

static inline od_client_t *od_route_match_client(od_route_t *route, od_id_t *id)
{
	void *argv[] = { id };
	od_client_t *match;
	match = od_client_pool_foreach(&route->client_pool, OD_CLIENT_ACTIVE,
				       od_route_match_compare_client_cb, argv);
	if (match)
		return match;
	match = od_client_pool_foreach(&route->client_pool, OD_CLIENT_QUEUE,
				       od_route_match_compare_client_cb, argv);
	if (match)
		return match;
	match = od_client_pool_foreach(&route->client_pool, OD_CLIENT_PENDING,
				       od_route_match_compare_client_cb, argv);
	if (match)
		return match;

	return NULL;
}

static inline void od_route_kill_client(od_route_t *route, od_id_t *id)
{
	od_client_t *client;
	client = od_route_match_client(route, id);
	if (client)
		od_client_kill(client);
}

static inline int od_route_kill_cb(od_client_t *client, void **argv)
{
	(void)argv;
	od_client_kill(client);
	return 0;
}

static inline int od_grac_shutdown_cb(od_server_t *server, void **argv)
{
	(void)argv;
	od_server_grac_shutdown(server);
	return 0;
}

static inline int od_route_reload_cb(od_server_t *server, void **argv)
{
	(void)argv;
	od_server_reload(server);
	return 0;
}

static inline void od_route_kill_client_pool(od_route_t *route)
{
	od_client_pool_foreach(&route->client_pool, OD_CLIENT_ACTIVE,
			       od_route_kill_cb, NULL);
	od_client_pool_foreach(&route->client_pool, OD_CLIENT_PENDING,
			       od_route_kill_cb, NULL);
	od_client_pool_foreach(&route->client_pool, OD_CLIENT_QUEUE,
			       od_route_kill_cb, NULL);
}

static inline void od_route_grac_shutdown_pool(od_route_t *route)
{
	int i;
	for (i = 0; i < route->server_pool_size; ++i) {
		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_ACTIVE,
				od_grac_shutdown_cb, NULL);
		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_IDLE,
				od_grac_shutdown_cb, NULL);
	}
}

static inline void od_route_reload_pool(od_route_t *route)
{
	int i;
	for (i = 0; i < route->server_pool_size; ++i) {
		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_ACTIVE,
				od_route_reload_cb, NULL);
		od_server_pool_foreach(&route->server_pool[i], OD_SERVER_IDLE,
				od_route_reload_cb, NULL);
	}
}

static inline uint32_t od_route_waiters_count(od_route_t *route, int endpoint)
{
	return od_atomic_u32_of(&route->waiters[endpoint]);
}

static inline void od_route_inc_waiters(od_route_t *route, int endpoint)
{
	od_atomic_u32_inc(&route->waiters[endpoint]);
}

static inline void od_route_dec_waiters(od_route_t *route, int endpoint)
{
	od_atomic_u32_dec(&route->waiters[endpoint]);
}

static inline int od_route_wait(od_route_t *route, int endpoint , uint32_t time_ms)
{
	machine_msg_t *msg;
	msg = machine_channel_read(route->wait_bus[endpoint], time_ms);
	if (msg) {
		machine_msg_free(msg);
		return 0;
	}
	return -1;
}

static inline int od_route_signal(od_route_t *route, int endpoint)
{
	machine_msg_t *msg;
	msg = machine_msg_create(0);
	if (msg == NULL) {
		return -1;
	}
	machine_channel_write(route->wait_bus[endpoint], msg);
	return 0;
}

#endif /* ODYSSEY_ROUTE_H */
