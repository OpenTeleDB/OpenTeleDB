/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>
#include <util.h>

static inline int od_print_route_rule_cb(od_route_t *route, void **argv);
static inline void od_print_route_rules(od_router_t *router);
static inline int od_get_poll_arbitration_from_agent(const char *host, int port, const char* path, char *buffer, char* error_msg);

od_storage_watchdog_t *od_storage_watchdog_allocate(od_global_t *global)
{
	od_storage_watchdog_t *watchdog;
	watchdog = malloc(sizeof(od_storage_watchdog_t));
	if (watchdog == NULL)
		return NULL;

	memset(watchdog, 0, sizeof(od_storage_watchdog_t));
	watchdog->check_retry = 10;
	watchdog->current_endpoint = 0;
	watchdog->global = global;
	watchdog->online = 1;
	pthread_mutex_init(&watchdog->mu, NULL);
	watchdog->lifecheck_coroutines = NULL;
	watchdog->query = NULL;
	watchdog->primary_arbitration_cmd = NULL;

	return watchdog;
}

static inline int
od_storage_watchdog_online_status(od_storage_watchdog_t *watchdog)
{
	int ret;
	pthread_mutex_lock(&watchdog->mu);
	ret = watchdog->online;
	pthread_mutex_unlock(&watchdog->mu);
	return ret;
}

static inline int od_storage_watchdog_soft_exit(od_storage_watchdog_t *watchdog)
{
	pthread_mutex_lock(&watchdog->mu);
	watchdog->online = 0;
	pthread_mutex_unlock(&watchdog->mu);
	return OK_RESPONSE;
}

int od_storage_watchdog_free(od_storage_watchdog_t *watchdog)
{
	if (watchdog == NULL)
		return NOT_OK_RESPONSE;

	if (watchdog->query)
		free(watchdog->query);

	if (watchdog->primary_arbitration_cmd)
		free(watchdog->primary_arbitration_cmd);

	pthread_mutex_destroy(&watchdog->mu);
	free(watchdog->lifecheck_coroutines);
	free(watchdog);
	return OK_RESPONSE;
}

od_rule_storage_t *od_rules_storage_allocate(void)
{
	/* Allocate and force defaults */
	od_rule_storage_t *storage =
		(od_rule_storage_t *)malloc(sizeof(od_rule_storage_t));
	if (storage == NULL)
		return NULL;
	memset(storage, 0, sizeof(*storage));
	storage->tls_opts = od_tls_opts_alloc();
	if (storage->tls_opts == NULL)
	{
		free(storage);
		return NULL;
	}
	storage->target_session_attrs = OD_TARGET_SESSION_ATTRS_ANY;
	storage->rr_counter = 0;

#define OD_STORAGE_DEFAULT_HASHMAP_SZ 420u

	storage->acache = od_hashmap_create(OD_STORAGE_DEFAULT_HASHMAP_SZ, true);
	storage->bcache = od_hashmap_create(OD_STORAGE_DEFAULT_HASHMAP_SZ, true);
	if (storage->acache == NULL || storage->bcache == NULL)
	{
		od_rules_storage_free(storage);
		return NULL;
	}

	od_list_init(&storage->link);
	storage->host = NULL;
	storage->endpoints = NULL;
	return storage;
}

void od_rules_storage_free(od_rule_storage_t *storage)
{
	if (storage->name)
		free(storage->name);
	if (storage->type)
		free(storage->type);
	if (storage->host)
		free(storage->host);

	if (storage->tls_opts)
	{
		od_tls_opts_free(storage->tls_opts);
	}

	if (storage->watchdog)
	{
		od_storage_watchdog_soft_exit(storage->watchdog);
	}

	if (storage->endpoints_count)
	{
		for (size_t i = 0; i < storage->endpoints_count; ++i)
		{
			free(storage->endpoints[i].host);
			free(storage->endpoints[i].application_name);
		}

		free(storage->endpoints);
	}

	if (storage->acache)
	{
		od_hashmap_free(storage->acache);
	}

	if (storage->bcache)
	{
		od_hashmap_free(storage->bcache);
	}

	od_list_unlink(&storage->link);
	free(storage);
}

od_rule_storage_t *od_rules_storage_copy(od_rule_storage_t *storage)
{
	od_rule_storage_t *copy;
	copy = od_rules_storage_allocate();
	if (copy == NULL)
		return NULL;
	copy->storage_type = storage->storage_type;
	copy->name = strdup(storage->name);
	copy->server_max_routing = storage->server_max_routing;
	if (copy->name == NULL)
		goto error;
	copy->type = strdup(storage->type);
	if (copy->type == NULL)
		goto error;
	if (storage->host)
	{
		copy->host = strdup(storage->host);
		if (copy->host == NULL)
			goto error;
	}
	copy->port = storage->port;
	copy->tls_opts->tls_mode = storage->tls_opts->tls_mode;
	if (storage->tls_opts->tls)
	{
		copy->tls_opts->tls = strdup(storage->tls_opts->tls);
		if (copy->tls_opts->tls == NULL)
			goto error;
	}
	if (storage->tls_opts->tls_ca_file)
	{
		copy->tls_opts->tls_ca_file =
			strdup(storage->tls_opts->tls_ca_file);
		if (copy->tls_opts->tls_ca_file == NULL)
			goto error;
	}
	if (storage->tls_opts->tls_key_file)
	{
		copy->tls_opts->tls_key_file =
			strdup(storage->tls_opts->tls_key_file);
		if (copy->tls_opts->tls_key_file == NULL)
			goto error;
	}
	if (storage->tls_opts->tls_cert_file)
	{
		copy->tls_opts->tls_cert_file =
			strdup(storage->tls_opts->tls_cert_file);
		if (copy->tls_opts->tls_cert_file == NULL)
			goto error;
	}
	if (storage->tls_opts->tls_protocols)
	{
		copy->tls_opts->tls_protocols =
			strdup(storage->tls_opts->tls_protocols);
		if (copy->tls_opts->tls_protocols == NULL)
			goto error;
	}

	if (storage->endpoints_count)
	{
		copy->endpoints_count = storage->endpoints_count;
		copy->endpoints = malloc(sizeof(od_storage_endpoint_t) * copy->endpoints_count);
		memset(copy->endpoints, 0, sizeof(od_storage_endpoint_t));

		if (copy->endpoints == NULL)
			goto error;

		for (size_t i = 0; i < copy->endpoints_count; ++i)
		{
			copy->endpoints[i].host = strdup(storage->endpoints[i].host);
			copy->endpoints[i].application_name = strdup(storage->endpoints[i].application_name);
			if (copy->endpoints[i].host == NULL || copy->endpoints[i].application_name == NULL)
				goto error;

			copy->endpoints[i].port = storage->endpoints[i].port;
			copy->endpoints[i].weight = storage->endpoints[i].weight;
			copy->endpoints[i].node_id = storage->endpoints[i].node_id;
		}
	}

	/* storage auth cache not copied */
	copy->target_session_attrs = storage->target_session_attrs;
	return copy;
error:
	od_rules_storage_free(copy);
	return NULL;
}

static inline int od_storage_watchdog_parse_role_response(machine_msg_t *msg, od_replica_role_t *role);

/*
 * Parse the response of the query "SELECT pg_is_in_recovery()"
 * the implementation in backend.c is not robust enough
 */
static inline int od_storage_watchdog_parse_role_response(machine_msg_t *msg, od_replica_role_t *role)
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
	if (kiwi_unlikely(rc == -1))
	{
		goto error;
	}

	/* we expect exactly one row */
	if (resp_len != 1)
	{
		return NOT_OK_RESPONSE;
	}
	/* pg is in recovery false means db is open for write */
	*role = pos[0] == 'f' ? REPLICA_ROLE_PRIMARY : REPLICA_ROLE_STANDBY;
	return OK_RESPONSE;
	/* fallthrough to error */
error:
	return NOT_OK_RESPONSE;
}

static inline int od_storage_watchdog_parse_replag_from_datarow(machine_msg_t *msg,
															 long *replag_musec)
{
	char *pos = (char *)machine_msg_data(msg) + 1;
	uint32_t pos_size = machine_msg_size(msg) - 1;

	/* size */
	uint32_t size;
	int rc;
	int stol_error = 0;

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
	uint32_t lag_len;
	rc = kiwi_read32(&lag_len, &pos, &pos_size);
	if (kiwi_unlikely(rc == -1))
	{
		goto error;
	}

	/* pos_size being 0 indicates a fully replayed idle system */
	*replag_musec = pos_size == 0 ? 0 : od_stringtol(pos, pos_size, &stol_error);
	if (stol_error)
	{
		goto error;
	}
	return OK_RESPONSE;
error:
	return NOT_OK_RESPONSE;
}

static inline int od_storage_watchdog_parse_heartbeat_from_datarow(machine_msg_t *msg,
																 long *heartbeat)
{
	char *pos = (char *)machine_msg_data(msg) + 1;
	uint32_t pos_size = machine_msg_size(msg) - 1;

	/* size */
	uint32_t size;
	int rc;
	int stol_error;

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
	uint32_t lag_len;
	rc = kiwi_read32(&lag_len, &pos, &pos_size);
	if (kiwi_unlikely(rc == -1))
	{
		goto error;
	}

	*heartbeat = od_stringtol(pos, pos_size, &stol_error);
	if (stol_error)
	{
		goto error;
	}
	return OK_RESPONSE;
error:
	return NOT_OK_RESPONSE;
}

/*
 * Parse application_name and replag from a datarow
 * if the application_name column is NULL, the output will be " "
 * if the replag column is NULL, the output will be 0
 */
static inline int od_storage_watchdog_parse_info_from_datarow(machine_msg_t *msg,
															  char **application_name, long *replag_musec)
{
	char *pos = (char *)machine_msg_data(msg) + 1;
	uint32_t pos_size = machine_msg_size(msg) - 1;

	/* size */
	uint32_t size;
	int rc;
	int stol_error = 0;

	rc = kiwi_read32(&size, &pos, &pos_size);
	if (kiwi_unlikely(rc == -1))
		return NOT_OK_RESPONSE;

	/* count */
	uint16_t count;
	rc = kiwi_read16(&count, &pos, &pos_size);
	/* Currently, we expect exactly two columns: application_name and replag */
	if (kiwi_unlikely(rc == -1) || count != 2)
		return NOT_OK_RESPONSE;

	for (int i = 0; i < count; i++)
	{
		int32_t col_len;
		/* TODO： col_len should be a signed integer */
		rc = kiwi_read32((uint32_t *)&col_len, &pos, &pos_size);
		if (kiwi_unlikely(rc == -1))
			return NOT_OK_RESPONSE;

		if (i == 0)
		{
			// application_name
			*application_name = col_len == -1 ? strdup("") : strndup(pos, col_len);
		}
		else if (i == 1)
		{
			// replag
			*replag_musec = col_len == -1 ? 0 : od_stringtol(pos, col_len, &stol_error);
			if (stol_error)
				return NOT_OK_RESPONSE;
		}
		pos += col_len;
		pos_size -= col_len;
	}

	return OK_RESPONSE;
}

static inline od_retcode_t od_route_suitable_for_watchdog(od_route_t *route, size_t idx)
{
	/* Check if route is obsolete */
	// if (route->rule->obsolete)
	// 	return NOT_OK_RESPONSE;
	
	// if (route->rule->mark)
	// 	return NOT_OK_RESPONSE;
	
	if (route->server_pool_size == 0)
		return NOT_OK_RESPONSE;
	
	if ((size_t)route->server_pool_size - 1 < idx)
		return NOT_OK_RESPONSE;

	return OK_RESPONSE;
}

/* Redirect index when a route has obsolete rule, but route->rule->storage has valid endpoint */
/* This is to ensure that after reload, old routes can still be maintaned by watchdog */
/* od_router_update_heartbeat_cb and od_router_update_role_cb need this function, and od_router_update_replag_cb dosen't */
/* because idx in od_router_update_replag_cb is guided by application_name */
static inline int od_route_index_redirect(od_route_t *route, size_t idx, const od_rule_storage_t *current_storage)
{
	if (!route->rule->obsolete && !route->rule->mark)
		return idx;

	int redirected_idx = -1;
	od_rule_storage_t *route_storage = route->rule->storage;

	for (int i = 0; (size_t)i < route_storage->endpoints_count; ++i) {
		if (od_storage_endpoints_same_host_and_port(&route_storage->endpoints[i], &current_storage->endpoints[idx]))
			redirected_idx = i;
	}

	return redirected_idx;
}

/* Reruen # of nodes that are marked as primary by watchdog */
static inline int od_route_primary_count(od_route_t *route)
{
	int count = 0;
	for (int i = 0; i < route->server_pool_size; ++i) {
		count += route->server_pool[i].replica_role == REPLICA_ROLE_PRIMARY ? 1 : 0;
	}
	return count;
}

static inline int od_parse_agent_url(const char *agent_url, char *host, int *port, char *path)
{
	const char *http_prefix = "http://";
	// Check if the URL starts with http:// or https:// and skip it
    const char *start = agent_url;
    if (strncmp(agent_url, http_prefix, strlen(http_prefix)) == 0) {
        start += strlen(http_prefix);
    } else {
		return NOT_OK_RESPONSE;
	}

    // Find the end of the host part
    const char *end_host = strchr(start, ':');
    if (!end_host) {
        end_host = strchr(start, '/');
    }
    if (!end_host) {
        end_host = start + strlen(start);
    }

	 // Extract the host
    strncpy(host, start, end_host - start);
    host[end_host - start] = '\0';

	 // Check if there is a port specified
    if (*end_host == ':') {
		const char *port_start = end_host + 1;
        const char *end_port = strchr(end_host + 1, '/');
        if (!end_port) {
            end_port = end_host + strlen(end_host);
        }
		size_t port_len = end_port - port_start;
		char port_str[port_len];
		strncpy(port_str, port_start, port_len);
		port_str[port_len] = '\0';
        *port = atoi(port_str);
        start = end_port;
    } else {
        return NOT_OK_RESPONSE;
    }

	if (*start != '/') {
		return NOT_OK_RESPONSE;
	}
	 // Extract the path
    strcpy(path, start);
	return OK_RESPONSE;
}

/* Return the host & port of the primary determined by agent */
static inline int od_primary_arbitration(od_storage_watchdog_t* watchdog, char* primary_host)
{
	char response_buffer[1024] = {0};
	od_instance_t* instance = watchdog->global->instance;

	if (!watchdog->primary_arbitration_cmd) {
		od_error(&instance->logger, "watchdog", NULL, NULL, "split-brain happens but primary_arbitration_cmd not set");
		return NOT_OK_RESPONSE;
	}

	char agent_host[1024] = {0};
	int agent_port = 0;
	char agent_path[1024] = {0};
	char error_msg[1024] = {0};

	if(od_parse_agent_url(watchdog->primary_arbitration_cmd,agent_host, &agent_port, agent_path) == NOT_OK_RESPONSE) {
		od_error(&instance->logger, "watchdog", NULL, NULL, "od_primary_arbitration failed to pass agent url");
		return NOT_OK_RESPONSE;
	}

    if (od_get_poll_arbitration_from_agent(agent_host, agent_port, agent_path, response_buffer, error_msg) == OK_RESPONSE) {
        od_extract_http_body(response_buffer); // Extract the body
    } else {
		od_error(&instance->logger, "watchdog", NULL, NULL, "od_primary_arbitration failed to poll agent. error: %s", error_msg);
		return NOT_OK_RESPONSE;
    }

	if (od_parse_arbitration_json(response_buffer, primary_host) == NOT_OK_RESPONSE) {
		od_error(&instance->logger, "watchdog", NULL, NULL, "od_parse_arbitration_json failed with parsed host:%s", primary_host);
		return NOT_OK_RESPONSE;
	}
	
	return OK_RESPONSE;
}

static inline int od_router_update_heartbeat_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);
	/* each server_pool represents backends launched on a primary/standby node */
	size_t idx = *(size_t *)argv[1];
	od_rule_storage_t *current_storage = (od_rule_storage_t *)argv[2];
	od_logger_t *logger = (od_logger_t *)argv[3];
	int heartbeat_lag;

	if (od_route_suitable_for_watchdog(route, idx) != OK_RESPONSE) {
		od_route_unlock(route);
		return 0;
	}

	int redirected_idx = od_route_index_redirect(route, idx, current_storage);
	if (redirected_idx == -1) {
		od_route_unlock(route);
		return 0;
	}
	idx = (size_t) redirected_idx;

	route->server_pool[idx].last_heartbeat = *(long *)argv[0];
	route->last_heartbeat = *(long *)argv[0];
	heartbeat_lag = machine_timeofday_sec() - route->server_pool[idx].last_heartbeat;

	if (route->rule->catchup_timeout != 0 && heartbeat_lag > route->rule->catchup_timeout && strcasecmp(route->rule->db_name, "watchdog_int") != 0) {
		od_log(logger, "watchdog", NULL, NULL, "endpoint[%d] is invalid. heartbeat_lag %d, route->rule <%s %s>, heartbeat_threshold %d", 
				idx, heartbeat_lag, route->rule->user_name, route->rule->db_name, route->rule->catchup_timeout);
	}
	od_route_unlock(route);
	return 0;
}

static inline int od_select_endpoint_by_string(od_rule_storage_t *storage, const char *host) {
	for (size_t i = 0; i < storage->endpoints_count; i++) {
		if (strcmp(storage->endpoints[i].host, host) == 0) {
			return i;
		}
	}
	return -1;
}

static inline int od_router_update_role_cb(od_route_t *route, void **argv)
{
	od_replica_role_t role;
	role = *(od_replica_role_t *)argv[0];
	size_t idx = *(size_t *)argv[1];
	od_storage_watchdog_t *watchdog = (od_storage_watchdog_t *)argv[2];
	od_rule_storage_t *current_storage = (od_rule_storage_t *)argv[3];
	od_instance_t* instance = watchdog->global->instance;

	od_route_lock(route);
	if (od_route_suitable_for_watchdog(route, idx) != OK_RESPONSE) {
		od_route_unlock(route);
		return 0;
	}

	int redirected_idx = od_route_index_redirect(route, idx, current_storage);
	if (redirected_idx == -1) {
		od_route_unlock(route);
		return 0;
	}
	idx = (size_t) redirected_idx;

	route->server_pool[idx].replica_role = role;
	if (role== REPLICA_ROLE_PRIMARY && route->endpoint_state->primary_endpoint_index != idx)
		route->endpoint_state->primary_endpoint_index = idx;
	
	/* After update, hanlde possible split-brain */
	if (od_route_primary_count(route) > 1) {
		od_log(&instance->logger, "watchdog", NULL, NULL, "route %s %s found more than 1 primary",route->rule->db_name, route->rule->user_name);
		/* First, set all roles to standby */
		for (int i = 0; i < route->server_pool_size; i++) {
			route->server_pool[i].replica_role = REPLICA_ROLE_STANDBY;
		}
		char primary_host[1024] = {0};
		if (od_primary_arbitration(watchdog, primary_host) != OK_RESPONSE) {
			od_error(&instance->logger, "watchdog", NULL, NULL, "od_primary_arbitration failed. Setting all role to standby");
			od_route_unlock(route);
			return 0;
		}
		int primary_idx = od_select_endpoint_by_string(current_storage, primary_host);
		if (primary_idx == -1) {
			od_error(&instance->logger, "watchdog", NULL, NULL, "primary_host: %s not found in storage endpoints", primary_host);
			od_route_unlock(route);
			return 0;
		}
		route->server_pool[primary_idx].replica_role = REPLICA_ROLE_PRIMARY;
		route->endpoint_state->primary_endpoint_index = primary_idx;
		od_log(&instance->logger, "watchdog", NULL, NULL, "primary arbitration finished. new primary is %s index %d",primary_host,primary_idx);
	}

	od_route_unlock(route);
	return 0;
}


static inline int od_router_update_replag_reset_cb(od_route_t *route, void **argv)
{
	(void) argv;
	od_route_lock(route);
	for (int i = 0; i < route->server_pool_size-1; i++) {
		route->server_pool[i].last_replag_musec = INT32_MAX;
		route->server_pool[i].last_replag_millisec = INT32_MAX;
	}
	od_route_unlock(route);
	return 0;
}

static inline int od_router_update_replag_cb(od_route_t *route, void **argv)
{
	long lag;
	char *application_name;
	lag = *(long *)argv[0];
	application_name = *(char **)argv[1];
	od_logger_t *logger = (od_logger_t *)argv[2];
	size_t idx = 0;
	bool endpoint_matched = false;

	od_route_lock(route);
	od_rule_storage_t *storage = route->rule->storage;
	if (storage == NULL || storage->storage_type == OD_RULE_STORAGE_LOCAL) {
		if (storage == NULL)
			od_error(logger, "watchdog", NULL, NULL, "route %s %s has no storage", route->rule->user_name, route->rule->db_name);
		od_route_unlock(route);
		return 0;
	}

	/* traverse the route's corresponding storage's endpoints */
	/* Notice that the primary node is automatically skipped here*/
	for (size_t i = 0; i < storage->endpoints_count; i++)
	{
		/* check if application_name matches the current endpoint */
		if (strcmp(storage->endpoints[i].application_name, application_name) == 0) {
			idx = i;
			endpoint_matched = true;
			break;
		}
	}

	/* Maybe there is a WAL receiver (standby) not detected by xproxy */
	/* Simply ignore it */
	if (endpoint_matched == false) {
		od_route_unlock(route);
		od_log(logger, "watchdog", NULL, NULL,
			   "route %s %s no matching endpoint found for application_name: %s",route->rule->user_name, route->rule->db_name,
			   application_name);
		return 0;
	}

	/* role is not standby. Happens when user switch endpoints' order and reload, or the initial update */
	if (route->server_pool[idx].replica_role != REPLICA_ROLE_STANDBY) {
		od_error(logger, "watchdog", NULL, NULL, "route %.*s %.*s got role primary or role any when updating replag", 
				route->id.database_len, route->id.database, route->id.user_len, route->id.user);
		od_route_unlock(route);
		return 0;
	}

	/* Make sure the selected route is suitable  */
	if (od_route_suitable_for_watchdog(route, idx) != OK_RESPONSE) {
		od_error(logger, "watchdog", NULL, NULL, "route %s %s is not suitable for watchdog",route->rule->db_name, route->rule->user_name);
		od_route_unlock(route);
		return 0;
	}
	assert(route->server_pool[idx].replica_role == REPLICA_ROLE_STANDBY || route->server_pool[idx].replica_role == REPLICA_ROLE_UNKONWN);

	/* lag being 0 indicates a fully replayed idle system */
	route->server_pool[idx].last_replag_musec = lag;
	route->server_pool[idx].last_replag_millisec = lag / 1000.0;

	if(route->rule->replication_delay_threshold != 0 && lag > route->rule->replication_delay_threshold && strcasecmp(route->rule->db_name, "watchdog_int") != 0) {
		od_log(logger, "watchdog", NULL, NULL, "endpoint[%d] is invalid. rep_lag %ld, route->rule <%s %s>, replication_delay_threshold %ld, heartbeat_threshold %d", 
				idx, route->server_pool[idx].last_replag_musec, route->rule->user_name, route->rule->db_name, 
				route->rule->replication_delay_threshold);
	}

	od_route_unlock(route);
	return 0;
}

static inline od_client_t *od_storage_watchdog_client_allocate(od_global_t *global, od_storage_watchdog_t *watchdog)
{
	od_client_t *watchdog_client;
	watchdog_client =
		od_client_allocate_internal(global, "storage-watchog");
	if (watchdog_client == NULL)
	{
		return NULL;
	}

	watchdog_client->is_watchdog = true;
	watchdog_client->global = global;
	watchdog_client->type = OD_POOL_CLIENT_INTERNAL;
	od_id_generate(&watchdog_client->id, "a");

	/* set storage user and database */
	kiwi_var_set(&watchdog_client->startup.user, KIWI_VAR_UNDEF,
				 watchdog->route_usr, strlen(watchdog->route_usr) + 1);

	kiwi_var_set(&watchdog_client->startup.database, KIWI_VAR_UNDEF,
				 watchdog->route_db, strlen(watchdog->route_db) + 1);

	return watchdog_client;
}

void od_storage_watchdog_replica_lag(void *arg)
{
	watchdog_coroutine_args_t *args = (watchdog_coroutine_args_t *)arg;
	od_storage_watchdog_t *watchdog = args->watchdog;
	od_global_t *global = watchdog->global;
	od_router_t *router = global->router;
	od_instance_t *instance = global->instance;
	// od_rule_storage_t *storage = args->storage;
	od_debug(&instance->logger, "watchdog", NULL, NULL,
			 "start replica lag polling watchdog ");
	machine_msg_t **msgs = NULL;
	long *replags_musec = NULL;
	char **application_names = NULL;
	int rc = -1;
	size_t server_pool_size;
	od_route_t *route = NULL;

	/* create internal auth client */
	od_client_t *watchdog_client;
	watchdog_client = od_storage_watchdog_client_allocate(global, watchdog);
	if (watchdog_client == NULL) {
		od_error(&instance->logger, "watchdog", NULL, NULL,
				 "route storage watchdog failed to allocate client");
		goto exit;
	}

	/* route */
	od_router_status_t status;
	status = od_router_route(router, watchdog_client);
	od_debug(&instance->logger, "watchdog", watchdog_client, NULL,
			 "routing to internal wd route status: %s",
			 od_router_status_to_str(status));

	if (status != OD_ROUTER_OK) {
		od_error(&instance->logger, "watchdog", watchdog_client, NULL,
				"route storage watchdog failed: %s", od_router_status_to_str(status));
		goto exit;
	}

	route = watchdog_client->route;
	// assert(storage != NULL);
	assert(route != NULL);

	/* Currently, server_pool_size remain constant
	during the whole program's life (unless reload) */
	od_route_lock(route);
	server_pool_size = (size_t)route->server_pool_size;
	od_route_unlock(route);

	/* server_pool_size - 1 because the primary node is not included */
	/* Allocate memory for replags and application_names */
	replags_musec = malloc(sizeof(long) * (server_pool_size - 1));
	if (replags_musec == NULL) {
		od_error(&instance->logger, "watchdog", watchdog_client, NULL,
				 "storage watchdog failed to allocate repl_lag");
		goto exit;
	}

	application_names = malloc(sizeof(char *) * (server_pool_size - 1));
	if (application_names == NULL) {
		od_error(&instance->logger, "watchdog", watchdog_client, NULL,
				 "storage watchdog failed to allocate application_names");
		goto exit;
	}

	for (;;)
	{					
		if (!od_storage_watchdog_online_status(watchdog))
			goto exit;
		/* In each iteration, reset the values */
		memset(replags_musec, 0, sizeof(long) * (server_pool_size - 1));
		memset(application_names, 0, sizeof(char *) * (server_pool_size - 1));

		/* attach client to some route */
		status = od_router_attach(router, watchdog_client, REPLICA_ROLE_PRIMARY, false);

		if (status != OD_ROUTER_OK)
		{
			od_debug(&instance->logger, "watchdog", watchdog_client, NULL,
					"attaching wd client to backend connection status: %s",
					od_router_status_to_str(status));
			machine_sleep(1000);
			continue;
		}
		od_server_t *server = watchdog_client->server;
		od_debug(&instance->logger, "watchdog", watchdog_client, server,
				 "attached to server %s%.*s", server->id.id_prefix,
				 (int)sizeof(server->id.id), server->id.id);

		/* connect to server, if necessary */
		if (server->io.io == NULL)
		{
			/* This assumes that the current cluster has a least one primary node */
			rc = od_backend_connect_timeout(server, "watchdog", NULL,
											watchdog_client, OD_WATCHDOG_TIMEOUT);
			if (rc == NOT_OK_RESPONSE)
			{
				od_log(
					&instance->logger, "watchdog",
					watchdog_client, server,
					"backend connect failed, retry after 1 sec");
				od_router_close(router, watchdog_client);
				/* 1 second soft interval */
				machine_sleep(1000);
				continue;
			}
		}

		for (int retry = 0; retry < watchdog->check_retry; ++retry)
		{
			int num_row_returned = 0;
			char *qry = "SELECT application_name, (EXTRACT(EPOCH FROM replay_lag) * 1000000)::BIGINT FROM pg_catalog.pg_stat_replication";
			msgs = od_query_multiple_return(server, "watchdog", qry, NULL, server_pool_size - 1, &num_row_returned, OD_WATCHDOG_TIMEOUT);

			if (msgs != NULL) {
				for (int i = 0; i < num_row_returned; i++) {
					rc = od_storage_watchdog_parse_info_from_datarow(msgs[i], &application_names[i], &replags_musec[i]);
					if (rc != OK_RESPONSE)
						break;
				}

				for (int i = 0; i < num_row_returned; i++) 
						machine_msg_free(msgs[i]);
				free(msgs);
				/* close connection if it's the last retry, or parsing is successful */
				if (rc == OK_RESPONSE || retry == watchdog->check_retry - 1)
					od_router_detach(router, watchdog_client);
			}
			else {
				od_log(
					&instance->logger, "watchdog",
					watchdog_client, server,
					"fail to receive msg, closing backend connection");
				rc = NOT_OK_RESPONSE;
				od_router_close(router, watchdog_client);
				break;
			}

			if (rc == OK_RESPONSE)
			{
				for (int i = 0; i < num_row_returned; i++)
				{
					void *argv[] = {&replags_musec[i], &application_names[i], &instance->logger};
					od_router_foreach(router,
									  od_router_update_replag_cb,
									  argv);
					od_log(&instance->logger, "watchdog",
						   watchdog_client, NULL,
						   "replag received from node [%s] with value %ld",
						   application_names[i], replags_musec[i]);
				}
				break;
			}
			// retry
		}

		/* detach and unroute */
		if (watchdog_client->server)
			od_router_detach(router, watchdog_client);

		if (!od_storage_watchdog_online_status(watchdog))
			goto exit;

		/* 1 second soft interval */
		machine_sleep(1000);
	}
exit:
	if (watchdog_client->route && watchdog_client->server == NULL)
		od_router_unroute(router,watchdog_client);
	if (watchdog_client)
		od_client_free(watchdog_client);
	if (replags_musec)
		free(replags_musec);
	if (application_names)
		free(application_names);
	--watchdog->active_coroutines;
	if (watchdog->active_coroutines == 0) {
		od_log(&instance->logger, "watchdog", NULL, NULL, "deallocating obsolete storage watchdog");
		od_storage_watchdog_free(watchdog);
	}
	return;
}

/*
 * A coroutine that polls the heartbeat of a single node
 * For each node, there is a coroutine running
 */
void od_storage_watchdog_healthcheck_node(void *arg)
{
	watchdog_coroutine_args_t *args = (watchdog_coroutine_args_t *)arg;
	size_t idx = args->index;
	od_storage_watchdog_t *watchdog = args->watchdog;
	uint32_t cid = rand() % 100;
	od_global_t *global = watchdog->global;
	od_router_t *router = global->router;
	od_instance_t *instance = global->instance;
	od_rule_storage_t *storage = od_rules_storage_copy(args->storage);
	od_client_t *watchdog_client = NULL;

	machine_msg_t *msg = NULL;
	machine_msg_t *msg_role = NULL;
	long last_heartbeat = 0;
	int rc = NOT_OK_RESPONSE;
	// od_route_t *route;

	if (storage == NULL) {
		od_error(&instance->logger, "watchdog", NULL, NULL, "route storage watchdog failed to allocate client");
		goto exit;
	}
	od_debug(&instance->logger, "watchdog", NULL, NULL, "start replica lag polling watchdog ");

	/* create internal auth client */
	watchdog_client = od_storage_watchdog_client_allocate(global, watchdog);
	if (watchdog_client == NULL) {
		od_error(&instance->logger, "watchdog", NULL, NULL, "route storage watchdog failed to allocate client");
		goto exit;
	}

	/* route */
	od_router_status_t status;
	status = od_router_route(router, watchdog_client);
	od_debug(&instance->logger, "watchdog", watchdog_client, NULL,
			"routing to internal wd route status: %s",
			od_router_status_to_str(status));

	if (status != OD_ROUTER_OK) {
		od_error(&instance->logger, "watchdog", watchdog_client, NULL,
				"route storage watchdog failed: %s",
				od_router_status_to_str(status));
		goto exit;
	}

	for (;;)
	{
		if (!od_storage_watchdog_online_status(watchdog)) {
			goto exit;
		}

		/* attach client to some route */
		status = od_router_attach_to_pool(router, watchdog_client, idx);

		if (status != OD_ROUTER_OK) {
			od_debug(&instance->logger, "watchdog", watchdog_client, NULL,
					"attaching wd client to pool failed, status: %s",
					od_router_status_to_str(status));
			machine_sleep(1000);
			continue;
		}
		od_server_t *server = watchdog_client->server;
		od_debug(&instance->logger, "watchdog", watchdog_client, server,
				 "attached to server %s%.*s", server->id.id_prefix,
				 (int)sizeof(server->id.id), server->id.id);

		/* connect to server, if necessary */
		if (server->io.io == NULL) {
			rc = od_backend_connect_index(server, "watchdog", NULL, watchdog_client, idx, OD_WATCHDOG_TIMEOUT);
			if (rc == NOT_OK_RESPONSE) {
				od_log(
					&instance->logger, "watchdog",
					watchdog_client, server,
					"backend connect failed, retry after 1 sec");
				od_router_close(router, watchdog_client);
				machine_sleep(1000);
				continue;
			}
		}

		/*
		 * Stage one: check for heartbeat
		 * In the worst case, check_retry times of retry failed.
		 * Heartbeats remain their last value.
		 */
		for (int retry = 0; retry < watchdog->check_retry; ++retry)
		{
			char *qry = "SELECT TRUNC(EXTRACT(EPOCH FROM NOW()))";
			msg = od_query_do(server, "watchdog", qry, NULL, NULL);
			if (server->offline)
				break;
			/* Do not close the connection here. Stage two still needs it */
			if (msg != NULL) {
				rc = od_storage_watchdog_parse_heartbeat_from_datarow(
					msg, &last_heartbeat);
				machine_msg_free(msg);
			}
			else {
				od_log(&instance->logger, "watchdog", watchdog_client, server, "fail to receive heartbeat msg");
				rc = NOT_OK_RESPONSE;
				break;
			}

			if (rc == OK_RESPONSE) {
				/* Use local clock to avoid clock errs between machines */
				last_heartbeat = machine_timeofday_sec();
				od_log(&instance->logger, "watchdog",
					   watchdog_client, server,
					   "dog [%d] received heartbeat information from node [%s:%d] with value %ld",
					   cid, storage->endpoints[idx].host, storage->endpoints[idx].port, last_heartbeat);

				void *argv[] = {&last_heartbeat, &idx, storage, &instance->logger};
				od_router_foreach(router,
								  od_router_update_heartbeat_cb,
								  argv);
				break;
			}
			// retry
		}
		if (server->offline) {
			od_router_detach(router, watchdog_client);
			continue;
		}

		/* Stage two: check for role */
		for (int retry = 0; retry < watchdog->check_retry; ++retry)
		{
			od_replica_role_t role = REPLICA_ROLE_UNKONWN;

			/* Retrieve the server's role */
			msg_role = od_query_do(server, "watchdog", "SELECT pg_is_in_recovery()", NULL, NULL);
			if (server->offline)
				break;
			if (msg_role != NULL) {
				rc = od_storage_watchdog_parse_role_response(msg_role, &role);
				machine_msg_free(msg_role);
			}
			else {
				od_log(&instance->logger, "watchdog", watchdog_client, server,
						 "fail to receive role msg, closing backend connection");
				rc = NOT_OK_RESPONSE;
				od_router_close(router, watchdog_client);
				break;
			}

			if (rc == OK_RESPONSE) {
				assert(role != REPLICA_ROLE_UNKONWN);

				od_log(&instance->logger, "watchdog",
					   watchdog_client, server,
					   "dog [%d] received role information from node [%s:%d] with value %d",
					   cid, storage->endpoints[idx].host, storage->endpoints[idx].port, role);

				void *argv[] = {&role, &idx, watchdog, storage};
				od_router_foreach(router,
								  od_router_update_role_cb,
								  argv);
				od_router_detach(router, watchdog_client);
				break;
			}
			// retry
		}

		server = watchdog_client->server;
		if (server && server->offline) {
			od_router_detach(router, watchdog_client);
			continue;
		}

		/* detach and unroute */
		if (watchdog_client->server) {
			od_router_detach(router, watchdog_client);
		}

		if (!od_storage_watchdog_online_status(watchdog)) {
			goto exit;
		}

		/* 1 second soft interval */
		machine_sleep(1000);
	}
exit:
	if(watchdog_client->route && watchdog_client->server == NULL)
		od_router_unroute(router, watchdog_client);
	if (watchdog_client)
		od_client_free(watchdog_client);
	if (storage)
		od_rules_storage_free(storage);
	--watchdog->active_coroutines;
	if (watchdog->active_coroutines == 0) {
		od_log(&instance->logger, "watchdog", NULL, NULL, "deallocating obsolete storage watchdog");
		od_storage_watchdog_free(watchdog);
	}
	return;
}

/*
 * launch several watchdogs for health checking for primary/standby servers,
 * and one for replica lag checking
 */
od_retcode_t od_storage_watchdog_launch(od_rule_storage_t *storage)
{
	od_storage_watchdog_t *watchdog = storage->watchdog;
	if (watchdog == NULL)
		return NOT_OK_RESPONSE;

	if(watchdog->active_coroutines != 0 || watchdog->lifecheck_coroutines != NULL)
		return NOT_OK_RESPONSE;

	watchdog->lifecheck_coroutines = malloc(sizeof(od_storage_watchdog_coroutine_state_t) * storage->endpoints_count);
	if (watchdog->lifecheck_coroutines == NULL)
		return NOT_OK_RESPONSE;	
	memset(watchdog->lifecheck_coroutines, 0, sizeof(od_storage_watchdog_coroutine_state_t) * storage->endpoints_count);
	for (size_t i = 0; i < storage->endpoints_count; i++)
		watchdog->lifecheck_coroutines[i].coroutine_id = INVALID_COROUTINE_ID;


	/* For each node, launch a healthcheck coroutines */
	for (size_t i = 0; i < storage->endpoints_count; ++i)
	{
		/* TODO: memory leak possible when args is not freed */
		watchdog_coroutine_args_t *args_healthcheck = malloc(sizeof(watchdog_coroutine_args_t));
		if (args_healthcheck == NULL)
			goto error;
		args_healthcheck->index = i;
		args_healthcheck->watchdog = watchdog;
		args_healthcheck->storage = storage;

		watchdog->lifecheck_coroutines[i].coroutine_id = machine_coroutine_create(
			od_storage_watchdog_healthcheck_node, args_healthcheck);
		if (watchdog->lifecheck_coroutines[i].coroutine_id == INVALID_COROUTINE_ID)
		{
			free(args_healthcheck);
			goto error;
		}
		++watchdog->active_coroutines;
	}

	/*periodly check for the replica lag*/

		watchdog_coroutine_args_t *args_replag = malloc(sizeof(watchdog_coroutine_args_t));
		if (args_replag == NULL)
			goto error;
		args_replag->index = -1;
		args_replag->watchdog = watchdog;
		args_replag->storage = storage;

	watchdog->replica_lag_coroutine = machine_coroutine_create(
		od_storage_watchdog_replica_lag, args_replag);
	if (watchdog->replica_lag_coroutine == INVALID_COROUTINE_ID){
		free(args_replag);
		goto error;
	}	

	++watchdog->active_coroutines;
	return OK_RESPONSE;

error:
	/* Clean up all created coroutines */
	for (size_t i = 0; i < storage->endpoints_count; i++) {
		if (watchdog->lifecheck_coroutines[i].coroutine_id != INVALID_COROUTINE_ID) {
			machine_cancel(watchdog->lifecheck_coroutines[i].coroutine_id);
			watchdog->lifecheck_coroutines[i].coroutine_id = INVALID_COROUTINE_ID;
		}
	}
	watchdog->active_coroutines = 0;
	free(watchdog->lifecheck_coroutines);
	watchdog->lifecheck_coroutines = NULL;
	return NOT_OK_RESPONSE;
}

static inline int od_print_route_rule_cb(od_route_t *route, void **argv)
{
	od_logger_t *logger = argv[0];
	od_rule_t *rule = route->rule;

	od_log(logger, "route_rules", NULL, NULL,
		   "\nRoute Rule Info:"
		   "\n  User: %s (is_default: %d)"
		   "\n  Database: %s (is_default: %d)"
		   "\n  Mark: %d"
		   "\n  Obsolete: %d"
		   "\n  Refs: %d"
		   "\n  Order: %d"
		   "\n  Client Max: %d (set: %d)"
		   "\n  Storage: %s"
		   "\n  Address Range: %s"
		   "\n  Server Pool Size: %zu"
		   "\n  Client Pool Size: %zu"
		   "\n  Target Server Attrs: %d"
		   "\n------------------------------------------",
		   rule->user_name ? rule->user_name : "null",
		   rule->user_is_default,
		   rule->db_name ? rule->db_name : "null",
		   rule->db_is_default,
		   rule->mark,
		   rule->obsolete,
		   rule->refs,
		   rule->order,
		   rule->client_max,
		   rule->client_max_set,
		   rule->storage_name ? rule->storage_name : "null",
		   rule->address_range.string_value ? rule->address_range.string_value : "null",
		   od_server_pool_total(route->server_pool, route->server_pool_size),
		   od_client_pool_total(&route->client_pool),
		   rule->pool->target_server_attrs ? rule->pool->target_server_attrs : 0);

	return 0;
}

static inline void od_print_route_rules(od_router_t *router)
{
	od_instance_t *instance = router->global->instance;
	od_logger_t *logger = &instance->logger;

	void *argv[] = {logger};
	od_router_foreach(router, od_print_route_rule_cb, argv);
}

#define ARBITRATION_TIMEOUT 5

static inline int od_get_poll_arbitration_from_agent(const char *host, int port, const char* path, char *buffer, char *error_msg) {
    int sockfd;
    struct sockaddr_in server_addr;
    char request[1024] = {0};
    struct timeval timeout;
    timeout.tv_sec = ARBITRATION_TIMEOUT;
    timeout.tv_usec = 0;
    
    snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		strcpy(error_msg,"Error creating socket");
		return NOT_OK_RESPONSE;
	}
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
        close(sockfd);
		strcpy(error_msg,"Error setting timeout");
        return NOT_OK_RESPONSE;
    }
        
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
		close(sockfd);
		strcpy(error_msg,"Invalid address");
		return NOT_OK_RESPONSE;
	}

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		close(sockfd);
		strcpy(error_msg,"Connection failed");
		return NOT_OK_RESPONSE;
	}

    if (send(sockfd, request, strlen(request), 0) == -1) {
		close(sockfd);
		strcpy(error_msg,"Send failed");
		return NOT_OK_RESPONSE;
	}

    ssize_t bytes_received;
    size_t total_bytes_received = 0;
    // Max buffer size is 1024, leave one for null terminator.
    size_t buffer_size = 1023; 

    while (total_bytes_received < buffer_size) {
        bytes_received = recv(sockfd, buffer + total_bytes_received, buffer_size - total_bytes_received, 0);
        if (bytes_received == -1) { // Error
            close(sockfd);
			strcpy(error_msg,"Received failed");
            return NOT_OK_RESPONSE;
        }
        if (bytes_received == 0) { // Connection closed by peer
            break;
        }
        total_bytes_received += bytes_received;
    }
    
    buffer[total_bytes_received] = '\0';
    close(sockfd);

    return OK_RESPONSE;
}

