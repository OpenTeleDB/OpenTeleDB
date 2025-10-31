
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>
#include "rw_select.h"

// 8 hex
#define OD_HASH_LEN 9
// log10(INT_32_MAX) + 2 
#define OD_LRUID_LEN  11 

static inline void od_frontend_close(od_client_t *client)
{
	assert(client->route == NULL);
	assert(client->server == NULL);

	od_router_t *router = client->global->router;
	od_atomic_u32_dec(&router->clients);

	od_io_close(&client->io);
	od_client_free(client);
}

int od_frontend_info(od_client_t *client, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	machine_msg_t *msg;
	msg = od_frontend_info_msg(client, NULL, fmt, args);
	va_end(args);
	if (msg == NULL) {
		return -1;
	}
	return od_write(&client->io, msg);
}

int od_frontend_error(od_client_t *client, char *code, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	machine_msg_t *msg;
	msg = od_frontend_error_msg(client, NULL, code, fmt, args);
	va_end(args);
	if (msg == NULL) {
		return -1;
	}
	return od_write(&client->io, msg);
}

int od_frontend_fatal(od_client_t *client, char *code, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	machine_msg_t *msg;
	msg = od_frontend_fatal_msg(client, NULL, code, fmt, args);
	va_end(args);
	if (msg == NULL)
		return -1;
	return od_write(&client->io, msg);
}

static inline int od_frontend_error_fwd(od_client_t *client)
{
	od_server_t *server = client->server;
	assert(server != NULL);
	assert(server->error_connect != NULL);
	kiwi_fe_error_t error;
	int rc;
	rc = kiwi_fe_read_error(machine_msg_data(server->error_connect),
				machine_msg_size(server->error_connect),
				&error);
	if (rc == -1)
		return -1;
	char text[512];
	int text_len;
	text_len =
		od_snprintf(text, sizeof(text), "odyssey: %s%.*s: %s",
			    client->id.id_prefix, (signed)sizeof(client->id.id),
			    client->id.id, error.message);
	int detail_len = error.detail ? strlen(error.detail) : 0;
	int hint_len = error.hint ? strlen(error.hint) : 0;

	machine_msg_t *msg;
	msg = kiwi_be_write_error_as(NULL, error.severity, error.code,
				     error.detail, detail_len, error.hint,
				     hint_len, text, text_len);
	if (msg == NULL)
		return -1;
	return od_write(&client->io, msg);
}

static inline bool
od_frontend_error_is_too_many_connections(od_client_t *client)
{
	od_server_t *server = client->server;
	assert(server != NULL);
	if (server->error_connect == NULL)
		return false;
	kiwi_fe_error_t error;
	int rc;
	rc = kiwi_fe_read_error(machine_msg_data(server->error_connect),
				machine_msg_size(server->error_connect),
				&error);
	if (rc == -1)
		return false;
	return strcmp(error.code, KIWI_TOO_MANY_CONNECTIONS) == 0;
}

static inline char *role_to_string(od_replica_role_t role) {
	if (role == REPLICA_ROLE_PRIMARY)
		return "primary";
	else if (role == REPLICA_ROLE_STANDBY)
		return "standby";
	else if (role == REPLICA_ROLE_ANY)
		return "any";
	else
		return "unknown";
}

static int od_frontend_startup(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;
	machine_msg_t *msg;

	for (uint32_t startup_attempt = 0;
	     startup_attempt < MAX_STARTUP_ATTEMPTS; startup_attempt++) {
		msg = od_read_startup(
			&client->io,
			client->config_listen->client_login_timeout);
		if (msg == NULL)
			goto error;

		int rc = kiwi_be_read_startup(machine_msg_data(msg),
					      machine_msg_size(msg),
					      &client->startup, &client->vars);
		machine_msg_free(msg);
		if (rc == -1)
			goto error;

		if (!client->startup.unsupported_request)
			break;
		/* not supported 'N' */
		msg = machine_msg_create(sizeof(uint8_t));
		if (msg == NULL)
			return -1;
		uint8_t *type = machine_msg_data(msg);
		*type = 'N';
		rc = od_write(&client->io, msg);
		if (rc == -1) {
			od_error(&instance->logger,
				 "unsupported protocol (gssapi)", client, NULL,
				 "write error: %s", od_io_error(&client->io));
			return -1;
		}
		od_debug(&instance->logger, "unsupported protocol (gssapi)",
			 client, NULL, "ignoring");
	}

	/* client ssl request */
	int rc = od_tls_frontend_accept(client, &instance->logger,
					client->config_listen, client->tls);
	if (rc == -1)
		goto error;

	if (!client->startup.is_ssl_request) {
		rc = od_compression_frontend_setup(
			client, client->config_listen, &instance->logger);
		if (rc == -1)
			return -1;
		return 0;
	}

	/* read startup-cancel message followed after ssl
	 * negotiation */
	assert(client->startup.is_ssl_request);
	msg = od_read_startup(&client->io,
			      client->config_listen->client_login_timeout);
	if (msg == NULL)
		return -1;
	rc = kiwi_be_read_startup(machine_msg_data(msg), machine_msg_size(msg),
				  &client->startup, &client->vars);
	machine_msg_free(msg);
	if (rc == -1)
		goto error;

	rc = od_compression_frontend_setup(client, client->config_listen,
					   &instance->logger);
	if (rc == -1) {
		return -1;
	}

	return 0;

error:
	od_debug(&instance->logger, "startup", client, NULL,
		 "startup packet read error");
	od_cron_t *cron = client->global->cron;
	od_atomic_u64_inc(&cron->startup_errors);
	return -1;
}

static inline od_frontend_status_t
od_frontend_attach(od_client_t *client, char *context,
		   kiwi_params_t *route_params, od_replica_role_t replica_role)
{
	od_instance_t *instance = client->global->instance;
	od_router_t *router = client->global->router;
	od_route_t *route = client->route;

	if (route->rule->pool->reserve_prepared_statement) {
		client->relay.require_full_prep_stmt = 1;
	}

	client->wait_for_idle = false;
	for (;;) {
		od_router_status_t status;
		status = od_router_attach(router, client, replica_role, client->wait_for_idle);
		if (status != OD_ROUTER_OK) {
			if (status == OD_ROUTER_ERROR_TIMEDOUT) {
				od_error(&instance->logger, "router", client,
					 NULL,
					 "server pool wait timed out, closing");
				return OD_EATTACH_TOO_MANY_CONNECTIONS;
			}
			return OD_EATTACH;
		}

		od_server_t *server = client->server;
		if (server->io.io && !machine_connected(server->io.io)) {
			od_log(&instance->logger, context, client, server,
			       "server disconnected, close connection and retry attach");
			od_router_close(router, client);
			continue;
		}
		od_debug(&instance->logger, context, client, server,
			 "client %s%.*s attached to %s%.*s",
			 client->id.id_prefix,
			 (int)sizeof(client->id.id_prefix), client->id.id,
			 server->id.id_prefix,
			 (int)sizeof(server->id.id_prefix), server->id.id);

		assert(od_server_synchronized(server));
		assert(server->relay.iov == 0 ||
		       !machine_iov_pending(server->relay.iov));

		/* connect to server, if necessary */
		if (server->io.io) {
			return OD_OK;
		}

		int rc;
		od_atomic_u32_inc(&router->servers_routing);
		rc = od_backend_connect(server, context, route_params, client);
		od_atomic_u32_dec(&router->servers_routing);
		if (rc == -1) {
			/* In case of 'too many connections' error, retry attach attempt by
			 * waiting for a idle server connection for pool_timeout ms
			 */
			if (client->wait_for_idle) {
				od_router_close(router, client);
				if (instance->config.server_login_retry) {
					machine_sleep(instance->config.server_login_retry);
				}
				continue;
			}
			return OD_ESERVER_CONNECT;
		}

		return OD_OK;
	}
}

static od_frontend_status_t close_expired_prep_stmt(od_server_t *server, char *exclude, size_t exclude_len, int *pclosed)
{
	if (server->prep_stmts_lru == NULL)
		return OD_OK;
	od_instance_t *instance = server->global->instance;
	od_route_t *route = server->route;
	od_rule_t *rule = route->rule;

	int prep_stmts_limit = rule->pool->prepared_statement_limit;
	const int Prep_stmts_expired_timeout = rule->pool->prepared_statement_expired_time * 1000000;
	const int Prep_stmt_close_batch = 10;
	od_lru_elem_t *elem;
	int elem_id = 0;
	od_lru_prep_stmt_t *stmt;
	machine_msg_t *msg = NULL;
	od_hashmap_elt_t key;
	uint64_t expired_threshold = 0;
	int closed = 0;
	bool deleted;

	if (prep_stmts_limit <= 0 && Prep_stmts_expired_timeout <= 0)
		return OD_OK;

	if (Prep_stmts_expired_timeout > 0) {
		uint64_t now = machine_time_us();
		if (now > (uint64_t)Prep_stmts_expired_timeout)
			expired_threshold = now - (uint64_t)Prep_stmts_expired_timeout;
	}

	if (prep_stmts_limit <= 0) {
		prep_stmts_limit = INT_MAX; 
	}

	do {
		elem = od_lru_last_with_id(server->prep_stmts_lru, &elem_id);
		if ((elem == NULL || elem->last_atime > expired_threshold) &&
				od_lru_size(server->prep_stmts_lru) <= prep_stmts_limit)
			break;

		stmt = (od_lru_prep_stmt_t *)elem->data;
		/* do no close the prepared statement that is currently being bound. */
		if (stmt->opname_len == exclude_len &&
				strcmp(stmt->opname, exclude) == 0)
			break;

		char opname_with_lruid[OD_HASH_LEN + OD_LRUID_LEN];
		int opname_with_lruid_len = snprintf(opname_with_lruid, sizeof(opname_with_lruid), "%.*s_%d", 
				(int)stmt->opname_len, stmt->opname, elem_id);
		opname_with_lruid_len += 1;  /* \0 also counts */
		msg = kiwi_fe_write_close(msg, KIWI_FE_CLOSE_PREPARED_STATEMENT,
				opname_with_lruid, opname_with_lruid_len);

		if (msg == NULL)
			return OD_ESERVER_WRITE;

		if (instance->config.log_query || route->rule->log_query) {
			od_log(&instance->logger, "close", NULL, server,
					"close prepared statement, name: %.*s",
					opname_with_lruid_len, opname_with_lruid);
		}

		key.data = stmt->query;
		key.len = stmt->query_len;
		deleted = od_hashmap_delete(server->prep_stmts, stmt->hash, &key);
		assert(deleted);

		od_lru_pop_last(server->prep_stmts_lru);
		closed += 1;
	} while (closed < Prep_stmt_close_batch);

	if (msg) {
		if (od_write(&server->io, msg) ==  -1)
			return OD_ESERVER_WRITE;
	}

	if (pclosed)
		*pclosed = closed;

	return OD_OK;
}

static inline od_frontend_status_t
od_frontend_attach_and_deploy(od_client_t *client, char *context,
		od_replica_role_t replica_role)
{
	/* attach and maybe connect server */
	od_frontend_status_t status;
	status = od_frontend_attach(client, context, NULL, replica_role);
	if (status != OD_OK)
		return status;
	od_server_t *server = client->server;

	/* close long-term unused prepared statement */
	int closed = 0;
	status = close_expired_prep_stmt(server, NULL, 0, &closed);
	if (status != OD_OK)
		return status;

	/* configure server using client parameters */
	int rc;
	rc = od_deploy(client, context);
	if (rc == -1)
		return OD_ESERVER_WRITE;

	/* add a sync message */
	if (closed > 0 && rc == 0) {
		machine_msg_t *msg;
		msg = kiwi_fe_write_sync(NULL);
		if (msg == NULL)
			return OD_ESERVER_WRITE;
		if (od_write(&server->io, msg) == -1)
			return OD_ESERVER_WRITE;

		rc = 1;
	}
	/* set number of replies to discard */
	server->deploy_sync = rc;

	od_server_sync_request(server, server->deploy_sync);
	return OD_OK;
}

static inline od_frontend_status_t od_frontend_setup_params(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;
	od_router_t *router = client->global->router;
	od_route_t *route = client->route;

	/* ensure route has cached server parameters */
	int rc;
	rc = kiwi_params_lock_count(&route->params);
	if (rc == 0) {
		kiwi_params_t route_params;
		kiwi_params_init(&route_params);

		od_frontend_status_t status;

		/* 如果是会话级，需要的节点身份是固定的，不能指定为ANY */
		od_replica_role_t need_server_role = REPLICA_ROLE_ANY;
		if (route->rule->pool->pool == OD_RULE_POOL_SESSION) {
			if (client->config_listen && client->config_listen->port_attrs == OD_PORT_ATTR_RO)
				need_server_role = REPLICA_ROLE_STANDBY;
			else
				need_server_role = REPLICA_ROLE_PRIMARY;
		}
			
		status = od_frontend_attach(client, "setup", &route_params, need_server_role);
		if (status != OD_OK) {
			kiwi_params_free(&route_params);
			return status;
		}

		// close backend connection
		od_router_close(router, client);

		/* There is possible race here, so we will discard our
		 * attempt if params are already set */
		rc = kiwi_params_lock_set_once(&route->params, &route_params);
		if (!rc)
			kiwi_params_free(&route_params);
	}

	od_debug(&instance->logger, "setup", client, NULL, "sending params:");

	/* send parameters set by client or cached by the route */
	kiwi_param_t *param = route->params.params.list;

	machine_msg_t *stream = machine_msg_create(0);
	if (stream == NULL)
		return OD_EOOM;

	while (param) {
		kiwi_var_type_t type;
		type = kiwi_vars_find(&client->vars, kiwi_param_name(param),
				      param->name_len);
		kiwi_var_t *var;
		var = kiwi_vars_get(&client->vars, type);

		machine_msg_t *msg;
		if (var) {
			msg = kiwi_be_write_parameter_status(stream, var->name,
							     var->name_len,
							     var->value,
							     var->value_len);

			od_debug(&instance->logger, "setup", client, NULL,
				 " %.*s = %.*s", var->name_len, var->name,
				 var->value_len, var->value);
		} else {
			msg = kiwi_be_write_parameter_status(
				stream, kiwi_param_name(param), param->name_len,
				kiwi_param_value(param), param->value_len);

			od_debug(&instance->logger, "setup", client, NULL,
				 " %.*s = %.*s", param->name_len,
				 kiwi_param_name(param), param->value_len,
				 kiwi_param_value(param));
		}
		if (msg == NULL) {
			machine_msg_free(stream);
			return OD_EOOM;
		}

		param = param->next;
	}

	rc = od_write(&client->io, stream);
	if (rc == -1)
		return OD_ECLIENT_WRITE;

	return OD_OK;
}

static inline od_frontend_status_t od_frontend_setup(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;
	od_route_t *route = client->route;

	/* set paremeters */
	od_frontend_status_t status;
	status = od_frontend_setup_params(client);
	if (status != OD_OK)
		return status;

	/* disable readonly check for readonly user */
	if (route->rule->pool->target_server_attrs == OD_TARGET_SERVER_RO ||
			client->config_listen->port_attrs == OD_PORT_ATTR_RO) {
		od_hashmap_elt_t key = { .data = "query_check.check_readonly_enabled", .len = 35};
		od_hashmap_elt_t value = { .data = "off", .len = 4}, *value_ptr = &value;
		od_hash_t hash = od_murmur_hash(key.data, key.len);
		od_hashmap_insert(client->other_vars, hash, &key, &value_ptr, true);
	}

	if (route->rule->pool->reserve_prepared_statement) {
		if (od_client_init_hm(client) != OK_RESPONSE) {
			od_log(&instance->logger, "setup", client, NULL,
			       "failed to initialize hash map for prepared statements");
			return OD_EOOM;
		}
	}

	/* write key data message */
	machine_msg_t *stream;
	machine_msg_t *msg;
	msg = kiwi_be_write_backend_key_data(NULL, client->key.key_pid,
					     client->key.key);
	if (msg == NULL)
		return OD_EOOM;
	stream = msg;

	/* write ready message */
	msg = kiwi_be_write_ready(stream, 'I');
	if (msg == NULL) {
		machine_msg_free(stream);
		return OD_EOOM;
	}

	int rc;
	rc = od_write(&client->io, stream);
	if (rc == -1)
		return OD_ECLIENT_WRITE;

	if (instance->config.log_session) {
		client->time_setup = machine_time_us();
		od_log(&instance->logger, "setup", client, NULL,
		       "login time: %d microseconds",
		       (client->time_setup - client->time_accept));
		od_log(&instance->logger, "setup", client, NULL,
		       "client connection from %s to route %s.%s accepted",
		       client->peer, route->rule->db_name,
		       route->rule->user_name);
	}

	return OD_OK;
}

static inline od_frontend_status_t od_frontend_local_setup(od_client_t *client)
{
	machine_msg_t *stream;
	stream = machine_msg_create(0);

	if (stream == NULL)
		goto error;
	/* client parameters */
	machine_msg_t *msg;
	char data[128];
	int data_len;
	/* current version and build */
	data_len =
		od_snprintf(data, sizeof(data), "%s-%s-%s", OD_VERSION_NUMBER,
			    OD_VERSION_GIT, OD_VERSION_BUILD);
	msg = kiwi_be_write_parameter_status(stream, "server_version", 15, data,
					     data_len + 1);
	if (msg == NULL)
		goto error;
	msg = kiwi_be_write_parameter_status(stream, "server_encoding", 16,
					     "UTF-8", 6);
	if (msg == NULL)
		goto error;
	msg = kiwi_be_write_parameter_status(stream, "client_encoding", 16,
					     "UTF-8", 6);
	if (msg == NULL)
		goto error;
	msg = kiwi_be_write_parameter_status(stream, "DateStyle", 10, "ISO", 4);
	if (msg == NULL)
		goto error;
	msg = kiwi_be_write_parameter_status(stream, "TimeZone", 9, "GMT", 4);
	if (msg == NULL)
		goto error;
	/* ready message */
	msg = kiwi_be_write_ready(stream, 'I');
	if (msg == NULL)
		goto error;
	int rc;
	rc = od_write(&client->io, stream);
	if (rc == -1)
		return OD_ECLIENT_WRITE;
	return OD_OK;
error:
	if (stream)
		machine_msg_free(stream);
	return OD_EOOM;
}

static inline bool od_eject_conn_with_rate(od_client_t *client,
					   od_server_t *server,
					   od_instance_t *instance)
{
	if (server == NULL) {
		/* server is null - client was never attached to any server so its ok to eject this conn  */
		return true;
	}
	od_thread_global **gl = od_thread_global_get();
	if (gl == NULL) {
		od_log(&instance->logger, "shutdown", client, server,
		       "drop client connection on restart, unable to throttle");
		/* this is clearly something bad, TODO: handle properly */
		return true;
	}

	od_conn_eject_info *info = (*gl)->info;

	struct timeval tv;
	gettimeofday(&tv, NULL);
	bool res = false;

	pthread_mutex_lock(&info->mu);
	{
		if (info->last_conn_drop_ts + /* 1 sec */ 1 > tv.tv_sec) {
			od_log(&instance->logger, "shutdown", client, server,
			       "delay drop client connection on restart, last drop was too recent (wid %d, last drop %d, curr time %d)",
			       (*gl)->wid, info->last_conn_drop_ts, tv.tv_sec);
		} else {
			info->last_conn_drop_ts = tv.tv_sec;
			res = true;

			od_log(&instance->logger, "shutdown", client, server,
			       "drop client connection on restart (wid %d, last eject %d, curr time %d)",
			       (*gl)->wid, info->last_conn_drop_ts, tv.tv_sec);
		}
	}
	pthread_mutex_unlock(&info->mu);

	return res;
}

static inline bool od_eject_conn_with_timeout(od_client_t *client,
					      uint64_t timeout)
{
	od_dbg_printf_on_dvl_lvl(1, "current time %lld, drop horizon %lld\n",
				 machine_time_us(),
				 client->time_last_active + timeout);

	if (client->time_last_active + timeout < machine_time_us()) {
		return true;
	}

	return false;
}

static inline bool od_should_drop_connection(od_client_t *client,
					     od_server_t *server)
{
	od_instance_t *instance = client->global->instance;
	od_rule_pool_t *pool = client->rule->pool;
	od_route_t *route = client->route;
	char *username = route->id.user;

	if (client->time_last_active == 0)
		return false;

	/*
	 * In order to strictly drop idle connections based on the client_idle_timeout,
	 * we do not distinguish between pool modes.
	 */
	if (od_likely(client->rule->pool->client_idle_timeout &&
		!od_rule_matches_whitelist_user(pool, username))) {
		if (server == NULL || (!server->is_transaction &&
			od_server_synchronized(server))) {
			if (od_eject_conn_with_timeout(
					client,
					client->rule->pool->client_idle_timeout)) {
				od_log(&instance->logger, "shutdown",
						client, server,
						"drop idle client connection on due timeout %d sec",
						client->rule->pool->client_idle_timeout/interval_usec);

				return true;
			}
		}
	}

	if (od_unlikely(
			client->rule->pool->idle_in_transaction_timeout &&
			!od_rule_matches_whitelist_user(pool, username))) {
		// the same as above but we are going to drop client inside transaction block
		if (server == NULL || (server->is_transaction &&
			/*server is sync - that means client executed some stmts and got get result, and now just... do nothing */
			od_server_synchronized(server))) {
			if (od_eject_conn_with_timeout(
					client,
					client->rule->pool->idle_in_transaction_timeout)) {
				od_log(&instance->logger, "shutdown",
						client, server,
						"drop idle in transaction connection on due timeout %d sec",
						client->rule->pool->idle_in_transaction_timeout/interval_usec);

				return true;
			}
		}
	}

	switch (client->rule->pool->pool) {
#if 0
	case OD_RULE_POOL_SESSION: {
		if (od_unlikely(client->rule->pool->client_idle_timeout)) {
			// as we do not unroute client in session pooling after transaction block etc
			// we should consider this case separately
			// general logic is: if client do nothing long enough we can assume this is just a stale connection
			// but we need to ensure this connection was initialized etc
			if (od_unlikely(
				    server != NULL && !server->is_transaction &&
				    /* case when we are out of any transactional block ut perform some stmt */
				    od_server_synchronized(server))) {
				if (od_eject_conn_with_timeout(
					    client,
					    client->rule->pool
						    ->client_idle_timeout)) {
					od_log(&instance->logger, "shutdown",
					       client, server,
					       "drop idle client connection on due timeout %d sec",
					       client->rule->pool
						       ->client_idle_timeout);

					return true;
				}
			}
		}
		if (od_unlikely(
			    client->rule->pool->idle_in_transaction_timeout)) {
			// the same as above but we are going to drop client inside transaction block
			if (server != NULL && server->is_transaction &&
			    /*server is sync - that means client executed some stmts and got get result, and now just... do nothing */
			    od_server_synchronized(server)) {
				if (od_eject_conn_with_timeout(
					    client,
					    client->rule->pool
						    ->idle_in_transaction_timeout)) {
					od_log(&instance->logger, "shutdown",
					       client, server,
					       "drop idle in transaction connection on due timeout %d sec",
					       client->rule->pool
						       ->idle_in_transaction_timeout);

					return true;
				}
			}
		}
	}
#endif
		/* fall through */
	case OD_RULE_POOL_TRANSACTION: {
		//TODO:: drop no more than X connection per sec/min/whatever
		if (od_likely(instance->shutdown_worker_id ==
			      INVALID_COROUTINE_ID)) {
			// try to optimize likely path
			return false;
		}

		if (od_unlikely(client->rule->storage->storage_type ==
				OD_RULE_STORAGE_LOCAL)) {
			/* local server is not very important (db like console, pgbouncer used for stats)*/
			return true;
		}

		if (od_unlikely(server == NULL)) {
			return od_eject_conn_with_rate(client, server,
						       instance);
		}
		if (server->state ==
			    OD_SERVER_ACTIVE /* we can drop client that are just connected and do not perform any queries */
		    && !od_server_synchronized(server)) {
			/* most probably we are not in transcation, but still executing some stmt */
			return false;
		}
		if (od_unlikely(!server->is_transaction)) {
			return od_eject_conn_with_rate(client, server,
						       instance);
		}
		return false;
	} break;
	default:
		return false;
	}
}
static od_frontend_status_t od_frontend_ctl(od_client_t *client)
{
	if (od_atomic_u64_of(&client->killed) == 1) {
		return OD_STOP;
	}

	return OD_OK;
}

static od_frontend_status_t od_frontend_local(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;

	for (;;) {
		machine_msg_t *msg = NULL;
		for (;;) {
			/* local server is alwys null */
			if (od_should_drop_connection(client, NULL)) {
				/* Odyssey is in a state of completion, we done
                         * the last client's request and now we can drop the connection  */

				/* a sort of EAGAIN */
				return OD_ECLIENT_READ;
			}
			/* one minute */
			msg = od_read(&client->io, 60000);

			if (machine_timedout()) {
				/* retry wait to recheck exit condition */
				assert(msg == NULL);
				continue;
			}

			if (msg == NULL) {
				return OD_ECLIENT_READ;
			} else {
				break;
			}
		}

		/* client operations */
		od_frontend_status_t status;
		status = od_frontend_ctl(client);

		if (status != OD_OK)
			break;

		kiwi_fe_type_t type;
		type = *(char *)machine_msg_data(msg);

		od_debug(&instance->logger, "local", client, NULL, "%s",
			 kiwi_fe_type_to_string(type));

		if (type == KIWI_FE_TERMINATE) {
			machine_msg_free(msg);
			break;
		}

		machine_msg_t *stream = machine_msg_create(0);
		if (stream == NULL) {
			machine_msg_free(msg);
			return OD_EOOM;
		}

		int rc;
		if (type == KIWI_FE_QUERY) {
			rc = od_console_query(client, stream,
					      machine_msg_data(msg),
					      machine_msg_size(msg));
			machine_msg_free(msg);
			if (rc == -1) {
				machine_msg_free(stream);
				return OD_EOOM;
			}
		} else {
			/* unsupported */
			machine_msg_free(msg);

			od_error(&instance->logger, "local", client, NULL,
				 "unsupported request '%s'",
				 kiwi_fe_type_to_string(type));

			msg = od_frontend_errorf(client, stream,
						 KIWI_FEATURE_NOT_SUPPORTED,
						 "unsupported request '%s'",
						 kiwi_fe_type_to_string(type));
			if (msg == NULL) {
				machine_msg_free(stream);
				return OD_EOOM;
			}
		}

		/* ready */
		msg = kiwi_be_write_ready(stream, 'I');
		if (msg == NULL) {
			machine_msg_free(stream);
			return OD_EOOM;
		}

		rc = od_write(&client->io, stream);
		if (rc == -1) {
			return OD_ECLIENT_WRITE;
		}
	}

	return OD_OK;
}

static void od_remove_server_prepared_stmt(od_server_t *server, machine_msg_t *parse);
static od_frontend_status_t od_frontend_remote_server(od_relay_t *relay,
						      char *data, int size)
{
	od_client_t *client = relay->on_packet_arg;
	od_server_t *server = client->server;
	od_route_t *route = client->route;
	od_instance_t *instance = client->global->instance;
	od_frontend_status_t retstatus;
	retstatus = OD_OK;

	kiwi_be_type_t type = *data;
	if (instance->config.log_debug)
		od_debug(&instance->logger, "main", client, server, "%s",
			 kiwi_be_type_to_string(type));

	int is_deploy = od_server_in_deploy(server);
	int is_ready_for_query = 0;

	int rc;
	switch (type) {
	case KIWI_BE_NOTICE_RESPONSE:
		retstatus = od_backend_notice(server, data, size, 0);
		break;
	case KIWI_BE_ERROR_RESPONSE:
		rc = od_backend_error(server, "main", data, size);
		if (rc != 0) {
			if (rc == 1 && od_get_replica_role(server) != REPLICA_ROLE_PRIMARY) { /* 非主节点且报XX000错误，需要换主节点重试 */
			    od_stat_rwsplit_wrong(&route->stats);
				server->is_dropped = true;
				if (server->parse_msg != NULL)
					od_remove_server_prepared_stmt(server, server->parse_msg);
				od_router_t *router = (od_router_t *)client->global->router;
				if (route->rule->enable_read_only_blacklist) {
					char *normalized_sql = add_to_blacklist(router->blacklist,
												client->cached_query, client->cached_query_len);
					od_debug(&instance->logger, "main", client, server,
						 "add to blacklist, query: %s", normalized_sql);
					free(normalized_sql);
				}
			}
			else {
				// 仅仅 parse 逻辑用
				kiwi_fe_error_t error;
				rc = kiwi_fe_read_error(data, size, &error);
				if (rc != -1) {
					client->has_error = true;
					strcpy(client->error_code, error.code);
					strcpy(client->error_msg, error.message);
				}
			}
		}
		break;
	case KIWI_BE_PARAMETER_STATUS:
		rc = od_backend_update_parameter(server, "main", data, size, 0);
		if (rc == -1)
			return relay->error_read;
		break;
	case KIWI_BE_COPY_IN_RESPONSE:
	case KIWI_BE_COPY_OUT_RESPONSE:
		server->in_out_response_received++;
		break;
	case KIWI_BE_COPY_DONE:
		/* should go after copy out
		* states that backend copy ended
		*/
		server->done_fail_response_received++;
		break;
	case KIWI_BE_COPY_FAIL:
		/*
		* states that backend copy failed
		*/
		return relay->error_write;
	case KIWI_BE_READY_FOR_QUERY: {
		is_ready_for_query = 1;
		od_backend_ready(server, data, size);

		/* exactly one RFQ! */
		if (od_server_in_sync_point(server)) {
			retstatus = OD_SKIP;
		}

		if (is_deploy)
			server->deploy_sync--;

		if (!server->synced_settings) {
			server->synced_settings = true;
			break;
		}
		/* update server stats */
		int64_t query_time = 0;
		od_stat_query_end(&route->stats, &server->stats_state,
				  server->is_transaction, &query_time);
		od_stat_client_query_end(client, server->is_transaction);
		if (instance->config.log_debug && query_time > 0) {
			od_debug(&instance->logger, "main", server->client,
				 server, "query time: %" PRIi64 " microseconds",
				 query_time);
		}

		break;
	}
	case KIWI_BE_PARSE_COMPLETE:
		if (route->rule->pool->reserve_prepared_statement) {
			// skip msg
			retstatus = OD_SKIP;
		}
	default:
		break;
	}

	if (server->is_dropped)
	{
		if (is_ready_for_query)
			return OD_DETACH_AND_RETRY;
		else
			return OD_SKIP;
	}

	/* discard replies during configuration deploy */
	if (is_deploy || retstatus == OD_SKIP)
		return OD_SKIP;

	if (route->id.physical_rep || route->id.logical_rep) {
		// do not detach server connection on replication
		// the exceptional case in offine: we are going to shut down here
		if (server->offline) {
			return OD_DETACH;
		}
	} else {
		if (is_ready_for_query && od_server_synchronized(server) &&
			server->hold_advisory_locks == false &&
			od_list_empty(&server->listen_channels) &&
			od_list_empty(&server->temporary_tables) &&
			od_list_empty(&server->declared_cursors) &&
			(server->query_prep_stmts == NULL ||
			 od_atomic_u32_of(&server->query_prep_stmts->items) == 0) &&
		    server->parse_msg == NULL) {
			switch (route->rule->pool->pool) {
			case OD_RULE_POOL_TRANSACTION:
				if (!server->is_transaction && server->backend_plugin_installed) {
					return OD_DETACH;
				}
				break;
			case OD_RULE_POOL_SESSION:
				if (server->offline &&
				    !server->is_transaction) {
					return OD_DETACH;
				}
				break;
			}
		}
	}

	return retstatus;
}

static inline od_retcode_t od_frontend_log_query(od_instance_t *instance,
						 od_client_t *client,
						 char *data, int size)
{
	uint32_t query_len;
	char *query;
	int rc;
	rc = kiwi_be_read_query(data, size, &query, &query_len);
	if (rc == -1)
		return NOT_OK_RESPONSE;

	od_log(&instance->logger, "query", client, NULL, "%.*s", query_len,
	       query);
	return OK_RESPONSE;
}

static inline od_retcode_t od_frontend_log_describe(od_instance_t *instance,
						    od_client_t *client,
						    char *data, int size)
{
	uint32_t name_len;
	char *name;
	int rc;
	kiwi_fe_describe_type_t t;
	rc = kiwi_be_read_describe(data, size, &name, &name_len, &t);
	if (rc == -1)
		return NOT_OK_RESPONSE;

	od_log(&instance->logger, "describe", client, client->server,
	       "(%s) name: %.*s",
	       t == KIWI_FE_DESCRIBE_PORTAL ? "portal" : "statement", name_len,
	       name);
	return OK_RESPONSE;
}

static inline od_retcode_t od_frontend_log_execute(od_instance_t *instance,
						   od_client_t *client,
						   char *data, int size)
{
	uint32_t name_len;
	char *name;
	int rc;
	rc = kiwi_be_read_execute(data, size, &name, &name_len);
	if (rc == -1)
		return NOT_OK_RESPONSE;

	od_log(&instance->logger, "execute", client, client->server,
	       "name: %.*s", name_len, name);
	return OK_RESPONSE;
}

static inline od_retcode_t od_frontend_parse_close(char *data, int size,
						   char **name,
						   uint32_t *name_len,
						   kiwi_fe_close_type_t *type)
{
	int rc;
	rc = kiwi_be_read_close(data, size, name, name_len, type);
	if (rc == -1)
		return NOT_OK_RESPONSE;
	return OK_RESPONSE;
}

static inline od_retcode_t od_frontend_log_close(od_instance_t *instance,
						 od_client_t *client,
						 char *name, uint32_t name_len,
						 kiwi_fe_close_type_t type)
{
	switch (type) {
	case KIWI_FE_CLOSE_PORTAL:
		od_log(&instance->logger, "close", client, client->server,
		       "portal, name: %.*s", name_len, name);
		return OK_RESPONSE;
	case KIWI_FE_CLOSE_PREPARED_STATEMENT:
		od_log(&instance->logger, "close", client, client->server,
		       "prepared statement, name: %.*s", name_len, name);
		return OK_RESPONSE;
	default:
		od_log(&instance->logger, "close", client, client->server,
		       "unknown close type, name: %.*s", name_len, name);
		return NOT_OK_RESPONSE;
	}
}

static inline od_retcode_t od_frontend_log_parse(od_instance_t *instance,
						 od_client_t *client,
						 char *context, char *data,
						 int size)
{
	uint32_t query_len;
	char *query;
	uint32_t name_len;
	char *name;
	int rc;
	rc = kiwi_be_read_parse(data, size, &name, &name_len, &query,
				&query_len);
	if (rc == -1)
		return NOT_OK_RESPONSE;

	od_log(&instance->logger, context, client, client->server, "%.*s %.*s",
	       name_len, name, query_len, query);
	return OK_RESPONSE;
}

static inline od_retcode_t od_frontend_log_bind(od_instance_t *instance,
						od_client_t *client, char *ctx,
						char *data, int size)
{
	uint32_t name_len;
	char *name;
	int rc;
	rc = kiwi_be_read_bind_stmt_name(data, size, &name, &name_len);
	if (rc == -1)
		return NOT_OK_RESPONSE;

	od_log(&instance->logger, ctx, client, client->server, "bind %.*s",
	       name_len, name);
	return OK_RESPONSE;
}

static inline machine_msg_t *
od_frontend_rewrite_msg(char *data, int size, int opname_start_offset,
			int operator_name_len, char *opname, int opnamelen)
{
	machine_msg_t *msg =
		machine_msg_create(size - operator_name_len + opnamelen);
	char *rewrite_data = machine_msg_data(msg);

	// packet header
	memcpy(rewrite_data, data, opname_start_offset);
	// prefix for opname
	memcpy(rewrite_data + opname_start_offset, opname, opnamelen);
	// rest of msg
	memcpy(rewrite_data + opname_start_offset + opnamelen,
	       data + opname_start_offset + operator_name_len,
	       size - opname_start_offset - operator_name_len);
	// set proper size to package
	kiwi_header_set_size((kiwi_header_t *)rewrite_data,
			     size - operator_name_len + opnamelen);

	return msg;
}

static od_frontend_status_t od_frontend_deploy_prepared_stmt(
	od_server_t *server, od_relay_t *relay, char *ctx, char *data,
	int size /* to adcance or to write? */, od_hash_t body_hash,
	char *opname, int opnamelen)
{
	od_route_t *route = server->route;
	od_instance_t *instance = server->global->instance;
	od_client_t *client = server->client;
	od_frontend_status_t status = OD_OK;

	od_hashmap_elt_t desc;
	desc.data = data;
	desc.len = size;

	od_debug(&instance->logger, ctx, client, server,
		 "statement: %.*s, hash: %08x", desc.len, desc.data, body_hash);

	int lruid = 0;
	od_hashmap_elt_t value;
	value.data = &lruid;
	value.len = sizeof(int);
	od_hashmap_elt_t *value_ptr = &value;

	// send parse msg if needed
	if (od_hashmap_insert(server->prep_stmts, body_hash, &desc,
			      &value_ptr, false) == 0) {
		od_debug(&instance->logger, ctx, client, server,
			 "deploy %.*s operator %.*s to server", desc.len,
			 desc.data, opnamelen, opname);

		/* We save statements' opname(hex hash string) in the lru,  to avoid closing the statement currently being bound 
		 when try to close expired prepared statements . But the name actually send to the database is opname_{lruid}
		 to avoid hash collision when the number of prepared statements in the server become large */

		// add prepared stmt to lru
		int *plruid = (int *)value_ptr->data;
		od_lru_prep_stmt_t stmt;
		lru_prep_stmt_init(&stmt, body_hash, opname, opnamelen, data, size);
		*plruid = od_lru_add(server->prep_stmts_lru, &stmt, sizeof(stmt));

		/* name that actually send to the server */
		char opname_with_lruid[OD_HASH_LEN + OD_LRUID_LEN];
		int opname_with_lruid_len = snprintf(opname_with_lruid, sizeof(opname_with_lruid), "%.*s_%d", opnamelen, opname, *plruid);
		opname_with_lruid_len += 1;  /* \0 also counts */

		// try to close long-term unused PBE
		status = close_expired_prep_stmt(server, opname, opnamelen, NULL);
		if (status != OD_OK)
			return status;

		// rewrite msg
		// allocate prepered statement under name equal to body hash
		od_stat_parse(&route->stats);

		machine_msg_t *pmsg;
		pmsg = kiwi_fe_write_parse_description(NULL, opname_with_lruid, opname_with_lruid_len,
						       desc.data, desc.len);

		if (pmsg == NULL) {
			return OD_ESERVER_WRITE;
		}

		if (instance->config.log_query || route->rule->log_query) {
			od_frontend_log_parse(instance, client, "rewrite parse",
					      machine_msg_data(pmsg),
					      machine_msg_size(pmsg));
		}

		od_stat_parse(&route->stats);
		// msg deallocated here
		od_dbg_printf_on_dvl_lvl(1, "relay %p write msg %c\n", relay,
					 *(char *)machine_msg_data(pmsg));

		od_write(&server->io, pmsg);
		// advance?
		// machine_iov_add(relay->iov, pmsg);

		return OD_OK;
	} else {
		lruid = *(int*)value_ptr->data;
		od_lru_touch(server->prep_stmts_lru, lruid);

		status = close_expired_prep_stmt(server, opname, opnamelen, NULL);
		if (status != OD_OK)
			return status;

		od_stat_parse_reuse(&route->stats);
		return OD_OK;
	}
}

static inline void hash_to_opname(char opname[OD_HASH_LEN], od_hash_t body_hash)
{
	static const char digits_char[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
	for (int i = 0; i < 8; ++i)
	{
		uint8_t idx = body_hash & 0xf;
		opname[i] = digits_char[idx];
		body_hash >>= 4;
	}
	opname[8] = 0;
}

static inline od_frontend_status_t
od_frontend_deploy_prepared_stmt_msg(od_server_t *server, od_relay_t *relay,
				     char *ctx)
{
	od_frontend_status_t rc;
	char *data = machine_msg_data(server->parse_msg);
	int size = machine_msg_size(server->parse_msg);

	od_hash_t body_hash = od_murmur_hash(data, size);
	char opname[OD_HASH_LEN];
	hash_to_opname(opname, body_hash);
	rc = od_frontend_deploy_prepared_stmt(server, relay, ctx, data, size,
					      body_hash, opname, OD_HASH_LEN);

	// machine_msg_free(server->parse_msg);
	// server->parse_msg = NULL;
	return rc;
}

static void od_remove_client_prepared_stmt(od_client_t *client, char *opname)
{
	od_hashmap_elt_t key = {.data = opname, .len = strlen(opname) + 1};
	od_hash_t hash = od_murmur_hash(key.data, key.len);
	od_hashmap_delete(client->prep_stmt_ids, hash, &key);
	od_hashmap_delete(client->prep_stmt_target_roles, hash, &key);
}

static void od_remove_server_prepared_stmt(od_server_t *server, machine_msg_t *parse)
{
	assert(parse);
	char *data = machine_msg_data(server->parse_msg);
	int size = machine_msg_size(server->parse_msg);
	od_hashmap_elt_t *item = NULL;
	(void) parse;

	od_hash_t body_hash = od_murmur_hash(data, size);
	od_hashmap_elt_t key = {.data = data, .len = size};
	item = od_hashmap_find(server->prep_stmts, body_hash, &key);
	if (item)
	{
		int lruid = *(int*)item->data;
		od_lru_delete(server->prep_stmts_lru, lruid);
		od_hashmap_delete(server->prep_stmts, body_hash, &key);
	}

}

static void od_frontend_remote_server_on_read(od_relay_t *relay, int size);

static od_server_t *od_get_or_attach_server(od_client_t *client,
		od_frontend_status_t *pstatus, od_replica_role_t role) {
	od_server_t *server = client->server;
	od_frontend_status_t status = OD_OK;

	if (!server) {
		od_route_t *route = client->route;
		bool  reserve_session_server_connection =
			route->rule->reserve_session_server_connection;
		status = od_frontend_attach_and_deploy(client, "main", role);

		if (status == OD_OK) {
			server = client->server;
			status = od_relay_start(
					&server->relay, client->cond, OD_ESERVER_READ,
					OD_ECLIENT_WRITE,
					od_frontend_remote_server_on_read,
					&route->stats, od_frontend_remote_server,
					client, reserve_session_server_connection);

			if (status == OD_OK) {
				od_relay_attach(&client->relay, &server->io);
				od_relay_attach(&server->relay, &client->io);
			}

			od_replica_role_t real_server_role = route->server_pool[server->endpoint_selector].replica_role;
			if (role != real_server_role) {
				od_instance_t *instance = client->global->instance;
				od_debug(
						&instance->logger, "remote client",
						client, server,
						"od_get_or_attach_server not match need(%s) server(%d,%s)",
						role_to_string(role), server->endpoint_selector, role_to_string(real_server_role));
			}
		}
	}

	if (status != OD_OK)
		*pstatus = status;

	return server;
}

static od_replica_role_t choose_server_role(od_client_t *client, char *context,
											char *query, uint32 query_len)
{
	od_replica_role_t need_server_role = REPLICA_ROLE_ANY;
	od_route_t *route = client->route;
	od_router_t *router = (od_router_t *)client->global->router;
	od_target_server_type_t target_server_attrs = route->rule->pool->target_server_attrs;
	od_instance_t *instance = client->global->instance;

	if (client->server) { 
		/* Only choose new server_role when client->server is null. */
	}
	else if (client->config_listen && client->config_listen->port_attrs == OD_PORT_ATTR_RO) {
		need_server_role = REPLICA_ROLE_STANDBY;
	} else if (client->config_listen && client->config_listen->port_attrs == OD_PORT_ATTR_WO) {
		need_server_role = REPLICA_ROLE_PRIMARY;
	} else if (client->backend_plugin_installed == false ||
			 route->rule->pool->pool == OD_RULE_POOL_SESSION) {
		need_server_role = REPLICA_ROLE_PRIMARY;
	}
	else if (target_server_attrs == OD_TARGET_SERVER_AUTO) {
		if (is_read_only_sql(query)) {
			/* SQL 已在黑名单中，不再发往备节点 */
			if (route->rule->enable_read_only_blacklist &&
				is_in_blacklist(router->blacklist, query, query_len)) {
				od_stat_rwsplit_blacklist_hit(&route->stats);
				need_server_role = REPLICA_ROLE_PRIMARY;
				od_debug(&instance->logger, context, client, NULL,
						 "is in blacklist, query: %.*s", query_len, query);
			}
			else {
				od_stat_rwsplit(&route->stats);
				need_server_role = REPLICA_ROLE_STANDBY;
				od_debug(&instance->logger, context, client, NULL,
						 "is read only sql, query: %.*s", query_len, query);
			}
		} else {
			need_server_role = REPLICA_ROLE_PRIMARY;
			od_debug(&instance->logger, context, client, NULL,
					 "is not read only sql, query: %.*s", query_len, query);
		}
	}
	else if (target_server_attrs == OD_TARGET_SERVER_RW)
	{
		need_server_role = REPLICA_ROLE_PRIMARY;
	}
	else if (target_server_attrs == OD_TARGET_SERVER_RO)
	{
		need_server_role = REPLICA_ROLE_STANDBY;
	}

	return need_server_role;
}

static void set_cached_query(od_client_t *client, kiwi_fe_type_t type,
							 char *query, uint32 query_len)
{
	if (client->cached_query != NULL) {
		free(client->cached_query);
	}
	/* cache query, perhaps retry if specific error */
	client->cached_query_type = type;
	client->cached_query_len = query_len;
	client->cached_query = malloc(client->cached_query_len);
	memcpy(client->cached_query, query, client->cached_query_len);
}

static od_frontend_status_t od_frontend_remote_client(od_relay_t *relay,
						      char *data, int size)
{
	od_client_t *client = relay->on_packet_arg;
	od_instance_t *instance = client->global->instance;
	uint64_t now = machine_time_us();
	(void)size;
	int rc;
	od_route_t *route = client->route;
	assert(route != NULL);

	kiwi_fe_type_t type = *data;
	if (type == KIWI_FE_TERMINATE)
		return OD_STOP;

	od_server_t *server = NULL;
	if (instance->config.log_debug)
		od_debug(&instance->logger, "remote client", client, server,
			 "%s", kiwi_fe_type_to_string(type));

	od_frontend_status_t retstatus = OD_OK;
	od_replica_role_t need_server_role; /* 需要 attach 的 server role */
	od_replica_role_t server_role; /* 实际 attach 的 server role */

	switch (type) {
	case KIWI_FE_COPY_DONE:
	case KIWI_FE_COPY_FAIL:
		/* client finished copy */
		need_server_role = REPLICA_ROLE_ANY;
		/* 如果是会话级，需要的节点身份是固定的，不能指定为ANY */
		if (route->rule->pool->pool == OD_RULE_POOL_SESSION) {
			if (client->config_listen && client->config_listen->port_attrs == OD_PORT_ATTR_RO)
				need_server_role = REPLICA_ROLE_STANDBY;
			else
				need_server_role = REPLICA_ROLE_PRIMARY;
		}

		if ((server = od_get_or_attach_server(client, &retstatus, need_server_role))) {
			server->done_fail_response_received++;
		}
		break;
	case KIWI_FE_QUERY:
		if (instance->config.log_query || route->rule->log_query)
			od_frontend_log_query(instance, client, data, size);

		char *query;
		uint32_t query_len;
		rc = kiwi_be_read_query(data, size, &query, &query_len);
		if (rc == -1)
			return OD_ECLIENT_READ;

		do
		{
			need_server_role = choose_server_role(client, "simple query", query, query_len);
			/* update server sync state */
			server = od_get_or_attach_server(client, &retstatus, need_server_role);
			if (server) {
				server_role = route->server_pool[server->endpoint_selector].replica_role;
				/* Check backend plugin was installed */
				if (client->config_listen->port_attrs == OD_PORT_ATTR_RW &&
					server->backend_plugin_installed == false &&
					server_role == REPLICA_ROLE_STANDBY)
				{
					int rc = -1;
					rc = od_reset(server);
					od_relay_detach(&client->relay);
					od_relay_detach(&server->relay);
					if (rc == -1) 
						od_router_close(client->global->router, client);
					else
						od_router_detach(client->global->router, client);
					client->backend_plugin_installed = false;
					continue;
				}
			}
			break;
		} while (true);

		if (server) {
			od_server_sync_request(server, 1);
			od_debug(
					&instance->logger, "simple query",
					client, server,
					"KIWI_FE_QUERY od_get_or_attach_server need(%s) server(%d,%s)",
					role_to_string(need_server_role), server->endpoint_selector, role_to_string(server_role));

			/* 发往主节点的 SQL 无须缓存 */
			if (server_role != REPLICA_ROLE_PRIMARY) { 
				set_cached_query(client, KIWI_FE_QUERY, query, query_len);
			}
		}
		break;
	case KIWI_FE_FUNCTION_CALL:
	case KIWI_FE_SYNC:
		/* update server sync state */
		need_server_role = REPLICA_ROLE_ANY;
		/* 如果是会话级，需要的节点身份是固定的，不能指定为ANY */
		if (route->rule->pool->pool == OD_RULE_POOL_SESSION) {
			if (client->config_listen && client->config_listen->port_attrs == OD_PORT_ATTR_RO)
				need_server_role = REPLICA_ROLE_STANDBY;
			else
				need_server_role = REPLICA_ROLE_PRIMARY;
		}

		if ((server = od_get_or_attach_server(client, &retstatus, need_server_role))) {
			od_server_sync_request(server, 1);

			server_role = route->server_pool[server->endpoint_selector].replica_role;
			od_debug(
					&instance->logger, "sync",
					client, server,
					"KIWI_FE_SYNC od_get_or_attach_server server(%d,%s)",
					server->endpoint_selector, role_to_string(server_role));
		}
		break;
	case KIWI_FE_DESCRIBE:
		if (instance->config.log_query || route->rule->log_query)
			od_frontend_log_describe(instance, client, data, size);

		if (route->rule->pool->reserve_prepared_statement) {
			uint32_t operator_name_len;
			char *operator_name;
			kiwi_fe_describe_type_t type;
			rc = kiwi_be_read_describe(data, size, &operator_name,
						   &operator_name_len, &type);
			if (rc == -1) {
				return OD_ECLIENT_READ;
			}
			if (type == KIWI_FE_DESCRIBE_PORTAL) {
				break; // skip this, we only need to rewrite statement
			}

			od_hashmap_elt_t key;
			key.len = operator_name_len;
			key.data = operator_name;
			od_hash_t keyhash = od_murmur_hash(key.data, key.len);
			need_server_role = REPLICA_ROLE_ANY;
			/* reuse server  instead of an arbitrary one */
			if (client->server == NULL) { 
				od_hashmap_elt_t *value = (od_hashmap_elt_t *) od_hashmap_find(client->prep_stmt_target_roles, keyhash, &key);
				if (value != NULL) {
					need_server_role = *(od_replica_role_t *) value->data;
				}
			}

			/* We doesn't care whether the plugin was installed here. This will be took care of in BIND */
			if (!(server = od_get_or_attach_server(client, &retstatus, need_server_role))) {
				return retstatus;
			}
			server_role = route->server_pool[server->endpoint_selector].replica_role;
			od_debug(
					&instance->logger, "describe",
					client, server,
					"KIWI_FE_DESCRIBE od_get_or_attach_server server(%d,%s)",
					server->endpoint_selector, role_to_string(server_role));

			assert(client->prep_stmt_ids);
			retstatus = OD_SKIP;

			od_hashmap_elt_t *desc = od_hashmap_find(client->prep_stmt_ids, keyhash, &key);
			if (desc == NULL) {
				od_debug(
					&instance->logger, "describe",
					client, server,
					"%.*s (len %d) (%u) operator was not prepared by this client",
					operator_name_len, operator_name,
					operator_name_len, keyhash);
				// return OD_ESERVER_WRITE;
				return OD_OK; /* Pass to backend PG to handle it. */
			}

			od_hash_t body_hash =
				od_murmur_hash(desc->data, desc->len);

			char opname[OD_HASH_LEN];
			char opname_with_lruid[OD_HASH_LEN + OD_LRUID_LEN] ;
			int opname_with_lruid_len = -1;
			hash_to_opname(opname, body_hash);

			/* fill internals structs in, send parse if needed */
			if (od_frontend_deploy_prepared_stmt(
				    server, &server->relay, "parse before describe",
				    desc->data, desc->len, body_hash, opname,
				    OD_HASH_LEN) != OD_OK) {
				return OD_ESERVER_WRITE;
			}

			od_hashmap_elt_t *item = NULL;
			od_hashmap_elt_t key_server = {.data = desc->data, .len = desc->len};
			item = od_hashmap_find(server->prep_stmts, body_hash, &key_server);
			if (item) {
				int lruid = *(int*)item->data;
				opname_with_lruid_len = snprintf(opname_with_lruid, sizeof(opname_with_lruid), "%.*s_%d", OD_HASH_LEN, opname, lruid);
				opname_with_lruid_len += 1;  /* \0 also counts */
			}

			machine_msg_t *msg;
			msg = kiwi_fe_write_describe(NULL, 'S', opname_with_lruid,
						     opname_with_lruid_len);

			if (msg == NULL) {
				return OD_ESERVER_WRITE;
			}

			if (instance->config.log_query ||
			    route->rule->log_query) {
				od_frontend_log_describe(instance, client,
							 machine_msg_data(msg),
							 machine_msg_size(msg));
			}

			// msg if deallocated automaictly
			machine_iov_add(relay->iov, msg);
			od_dbg_printf_on_dvl_lvl(
				1, "client relay %p advance msg %c\n", relay,
				*(char *)machine_msg_data(msg));
		}
		break;
	case KIWI_FE_PARSE:
		if (instance->config.log_query || route->rule->log_query)
			od_frontend_log_parse(instance, client, "parse", data,
					      size);

		if (route->rule->pool->reserve_prepared_statement) {
			/* skip client parse msg */
			retstatus = OD_REQ_SYNC;
			kiwi_prepared_statement_t desc;
			rc = kiwi_be_read_parse_dest(data, size, &desc);
			if (rc) {
				return OD_ECLIENT_READ;
			}

			do {
				need_server_role = choose_server_role(client, "parse", desc.description, desc.description_len);
				server = od_get_or_attach_server(client, &retstatus, need_server_role);
				if (!server) {
					return retstatus;
				}
				server_role = route->server_pool[server->endpoint_selector].replica_role;
				/* Check backend plugin was installed */
				if (client->config_listen->port_attrs == OD_PORT_ATTR_RW)
				{
					if (server->backend_plugin_installed == false &&
						server_role == REPLICA_ROLE_STANDBY) {
						int rc = -1;
						rc = od_reset(server);
						od_relay_detach(&client->relay);
						od_relay_detach(&server->relay);
						if (rc == -1) 
							od_router_close(client->global->router, client);
						else
							od_router_detach(client->global->router, client);
						client->backend_plugin_installed = false;
						continue;
					}
					/* Reset client->backend_plugin_installed to true; */
					if (server->backend_plugin_installed &&
						client->backend_plugin_installed == false) {
						client->backend_plugin_installed = true;
					}
				}
				break;
			} while (true);

			od_debug(
					&instance->logger, "parse",
					client, server,
					"KIWI_FE_PARSE od_get_or_attach_server need(%s) server(%d,%s) sql(%s)",
					role_to_string(need_server_role), server->endpoint_selector, role_to_string(server_role), desc.description);
			if (server_role != REPLICA_ROLE_PRIMARY) { /* 发往主节点的 SQL 无须缓存 */
				set_cached_query(client, KIWI_FE_PARSE, desc.description, desc.description_len);
			}

			od_hash_t keyhash = od_murmur_hash(
				desc.operator_name, desc.operator_name_len);
			od_debug(&instance->logger, "parse", client, server,
				 "saving %.*s operator hash %u",
				 desc.operator_name_len, desc.operator_name,
				 keyhash);

			od_hashmap_elt_t key;
			key.len = desc.operator_name_len;
			key.data = desc.operator_name;

			od_hashmap_elt_t value2;
			value2.len = sizeof(od_replica_role_t);
			value2.data = &server_role;
			od_hashmap_elt_t *value_ptr2 = &value2;
			if (od_hashmap_insert(client->prep_stmt_target_roles, keyhash, &key, &value_ptr2, true)) {
			}

			od_hashmap_elt_t value;
			value.len = desc.description_len;
			value.data = desc.description;

			od_hashmap_elt_t *value_ptr = &value;

			memcpy(client->parse_name, desc.operator_name,
				   desc.operator_name_len);
			server->parse_msg =
				machine_msg_create(desc.description_len);
			if (server->parse_msg == NULL) {
				return OD_ESERVER_WRITE;
			}
			memcpy(machine_msg_data(server->parse_msg),
			       desc.description, desc.description_len);

			assert(client->prep_stmt_ids);
			if (od_hashmap_insert(client->prep_stmt_ids, keyhash,
					      &key, &value_ptr, true)) {
				if (value_ptr->len != desc.description_len ||
				    strncmp(desc.description, value_ptr->data,
					    value_ptr->len) != 0) {
					/*
					* Raise error:
					* client allocated prepared stmt with same name
					*/
					return OD_ESERVER_WRITE;
				}
			}
		}
		break;
	case KIWI_FE_BIND:
		if (instance->config.log_query || route->rule->log_query)
			od_frontend_log_bind(instance, client, "bind", data,
					     size);

		if (route->rule->pool->reserve_prepared_statement) {
			retstatus = OD_SKIP;
			uint32_t operator_name_len;
			char *operator_name;

			int rc;
			rc = kiwi_be_read_bind_stmt_name(
				data, size, &operator_name, &operator_name_len);

			if (rc == -1) {
				return OD_ECLIENT_READ;
			}

			od_hashmap_elt_t key;
			key.len = operator_name_len;
			key.data = operator_name;
			od_hash_t keyhash = od_murmur_hash(key.data, key.len);
			need_server_role = REPLICA_ROLE_ANY;

			if (client->server == NULL) { /* 若 client->server 不空，只会 get 不会 attach，没必要再传 role */
				od_hashmap_elt_t *value =
						(od_hashmap_elt_t *) od_hashmap_find(client->prep_stmt_target_roles, keyhash, &key);
				if (value != NULL) {
					need_server_role = *(od_replica_role_t *) value->data;
				}
			}
			if (need_server_role == REPLICA_ROLE_STANDBY) {
				od_stat_rwsplit(&route->stats);
			}

			do
			{
				if (!(server = od_get_or_attach_server(client, &retstatus, need_server_role)))
				{
					return retstatus;
				}
				server_role = route->server_pool[server->endpoint_selector].replica_role;
				/* Check backend plugin was installed */
				if (client->config_listen->port_attrs == OD_PORT_ATTR_RW)
				{
					if (server->backend_plugin_installed == false &&
						server_role == REPLICA_ROLE_STANDBY) {
						int rc = -1;
						rc = od_reset(server);
						od_relay_detach(&client->relay);
						od_relay_detach(&server->relay);
						if (rc == -1) 
							od_router_close(client->global->router, client);
						else
							od_router_detach(client->global->router, client);
						client->backend_plugin_installed = false;
						continue;
					}
					/* Reset client->backend_plugin_installed to true; */
					if (server->backend_plugin_installed &&
						client->backend_plugin_installed == false) {
						client->backend_plugin_installed = true;
					}
				}
				break;
			} while (true);

			od_debug(
					&instance->logger, "bind",
					client, server,
					"KIWI_FE_BIND od_get_or_attach_server need(%s) server(%d,%s)",
					role_to_string(need_server_role), server->endpoint_selector, role_to_string(server_role));

			od_hashmap_elt_t *desc =
				(od_hashmap_elt_t *)od_hashmap_find(
					client->prep_stmt_ids, keyhash, &key);
			if (desc == NULL) {
				od_debug(
					&instance->logger, "bind",
					client, server,
					"%.*s (%u) operator was not prepared by this client",
					operator_name_len, operator_name,
					keyhash);
				// return OD_ESERVER_WRITE;
				return OD_OK; /* Pass to backend PG to handle it. */
			}

			od_hash_t body_hash =
				od_murmur_hash(desc->data, desc->len);

			int invalidate = 0;

			if (desc->len >= 7) {
				if (strncmp(desc->data, "DISCARD", 7) == 0) {
					od_debug(
						&instance->logger,
						"rewrite bind", client, server,
						"discard detected, invalidate caches");
					invalidate = 1;
				}
			}

			char opname[OD_HASH_LEN];
			char opname_with_lruid[OD_HASH_LEN + OD_LRUID_LEN] ;
			int opname_with_lruid_len = -1;
			hash_to_opname(opname, body_hash);

			/* fill internals structs in, send parse if needed */
			if (od_frontend_deploy_prepared_stmt(
				    server, &server->relay, "parse before bind",
				    desc->data, desc->len, body_hash, opname,
				    OD_HASH_LEN) != OD_OK) {
				return OD_ESERVER_WRITE;
			}

			int opname_start_offset =
				kiwi_be_bind_opname_offset(data, size);
			if (opname_start_offset < 0) {
				return OD_ECLIENT_READ;
			}

			machine_msg_t *msg;
			if (invalidate) {
				od_hashmap_empty(server->prep_stmts);
			}

			od_hashmap_elt_t *item = NULL;
			od_hashmap_elt_t key_server = {.data = desc->data, .len = desc->len};
			item = od_hashmap_find(server->prep_stmts, body_hash, &key_server);
			if (item) {
				int lruid = *(int*)item->data;
				opname_with_lruid_len = snprintf(opname_with_lruid, sizeof(opname_with_lruid), "%.*s_%d", OD_HASH_LEN, opname, lruid);
				opname_with_lruid_len += 1;  /* \0 also counts */
			}
			
			msg = od_frontend_rewrite_msg(data, size,
						      opname_start_offset,
						      operator_name_len, opname_with_lruid,
						      opname_with_lruid_len);

			if (msg == NULL) {
				return OD_ESERVER_WRITE;
			}

			if (instance->config.log_query ||
			    route->rule->log_query) {
				od_frontend_log_bind(instance, client,
						     "rewrite bind",
						     machine_msg_data(msg),
						     machine_msg_size(msg));
			}

			machine_iov_add(relay->iov, msg);

			od_dbg_printf_on_dvl_lvl(
				1, "client relay %p advance msg %c\n", relay,
				*(char *)machine_msg_data(msg));
		}
		break;
	case KIWI_FE_EXECUTE:
		if (instance->config.log_query || route->rule->log_query)
			od_frontend_log_execute(instance, client, data, size);
		break;
	case KIWI_FE_CLOSE:
		if (route->rule->pool->reserve_prepared_statement) {
			char *name;
			uint32_t name_len;
			kiwi_fe_close_type_t type;
			int rc;

			if (od_frontend_parse_close(data, size, &name,
						    &name_len,
						    &type) != OK_RESPONSE) {
				return OD_ESERVER_WRITE;
			}

			if (type == KIWI_FE_CLOSE_PREPARED_STATEMENT) {
				retstatus = OD_SKIP;
				od_debug(
					&instance->logger,
					"ignore closing prepared statement, report its closed",
					client, server, "statement: %.*s",
					name_len, name);

				machine_msg_t *pmsg;
				pmsg = kiwi_be_write_close_complete(NULL);

				rc = od_write(&client->io, pmsg);
				if (rc == -1) {
					od_error(&instance->logger,
						 "close report", NULL, server,
						 "write error: %s",
						 od_io_error(&server->io));
					return OD_ECLIENT_WRITE;
				}

				od_hash_t keyhash = od_murmur_hash(name, name_len);
				od_debug(&instance->logger, "parse", client, server,
						"clean %.*s operator hash %u",
						name_len, name,
						keyhash);

				od_hashmap_elt_t key;
				key.len = name_len;
				key.data = name;
				od_hashmap_delete(client->prep_stmt_ids, keyhash, &key);
				od_hashmap_delete(client->prep_stmt_target_roles, keyhash, &key);
			}

			if (instance->config.log_query ||
			    route->rule->log_query) {
				od_frontend_log_close(instance, client, name,
						      name_len, type);
			}

		} else if (instance->config.log_query ||
			   route->rule->log_query) {
			char *name;
			uint32_t name_len;
			kiwi_fe_close_type_t type;

			if (od_frontend_parse_close(data, size, &name,
						    &name_len,
						    &type) != OK_RESPONSE) {
				return OD_ESERVER_WRITE;
			}

			od_frontend_log_close(instance, client, name, name_len,
					      type);
		}
		break;
	default:
		break;
	}

	/* If the retstatus is not SKIP */
	/* update server stats */
	if (server)
		od_stat_query_start(&server->stats_state, now);
	od_stat_client_query_start(client, now);

	return retstatus;
}

static void od_frontend_remote_server_on_read(od_relay_t *relay, int size)
{
	od_stat_t *stats = relay->on_read_arg;
	od_stat_recv_server(stats, size);
}

static void od_frontend_remote_client_on_read(od_relay_t *relay, int size)
{
	od_stat_t *stats = relay->on_read_arg;
	od_stat_recv_client(stats, size);
}

/*
* machine_sleep with ODYSSEY_CATCHUP_RECHECK_INTERVAL value
* will be effitiently just a context switch.
*/

#define ODYSSEY_CATCHUP_RECHECK_INTERVAL 1

static inline od_frontend_status_t od_frontend_poll_catchup(od_client_t *client,
							    od_route_t *route,
							    uint32_t timeout)
{
	od_instance_t *instance = client->global->instance;

	od_dbg_printf_on_dvl_lvl(
		1, "client %s polling replica for catchup with timeout %d\n",
		client->id.id, timeout);

	/*
	 * Ensure heartbeet is initialized at least once.
	 * Heartbeat might be 0 after reload\restart.
	 */
	int absent_heartbeat_checks = 0;
	while (route->last_heartbeat == 0) {
		machine_sleep(ODYSSEY_CATCHUP_RECHECK_INTERVAL);
		/* add cast to int64_t for correct camparison 
  			(int64_t > int and int64_t > uint32_t) */
		if ((int64_t)absent_heartbeat_checks++ >
		    (timeout * (int64_t)1000 /
		     ODYSSEY_CATCHUP_RECHECK_INTERVAL)) {
			od_debug(&instance->logger, "catchup", client, NULL,
				 "No heartbeat for route detected\n");
			return OD_ECATCHUP_TIMEOUT;
		}
	}

	/* TODO: Possibly move this elsewhere */
	absent_heartbeat_checks = 0;
	for (int i = 0; i < route->server_pool_size; ++i) {
		od_server_pool_t *pool = &route->server_pool[i];
		while(pool->last_heartbeat == 0) {
			machine_sleep(ODYSSEY_CATCHUP_RECHECK_INTERVAL);
			/* add cast to int64_t for correct camparison 
  				(int64_t > int and int64_t > uint32_t) */
			if ((int64_t)absent_heartbeat_checks++ >
		    (timeout * (int64_t)1000 /
		     ODYSSEY_CATCHUP_RECHECK_INTERVAL)) {
			od_debug(&instance->logger, "catchup", client, NULL,
				 "No heartbeat for pool[%d] detected\n", i);
				return OD_ECATCHUP_TIMEOUT;
			}
		}
	}

	for (int check = 1; check <= route->rule->catchup_checks; ++check) {
		od_dbg_printf_on_dvl_lvl(1, "current cached time %d\n",
					 machine_timeofday_sec());
		int lag = machine_timeofday_sec() - route->last_heartbeat;
		if (lag < 0) {
			lag = 0;
		}
		if ((uint32_t)lag < timeout) {
			return OD_OK;
		}
		od_log(&instance->logger, "catchup", client, NULL,
			"client %s replication %d lag is over catchup timeout %d\n",
			client->id.id, lag, timeout);
		/*
		 * TBD: Consider configuring `ODYSSEY_CATCHUP_RECHECK_INTERVAL` in
		 * frontend rule.
		 */
		if (check < route->rule->catchup_checks) {
			machine_sleep(ODYSSEY_CATCHUP_RECHECK_INTERVAL);
		}
	}
	return OD_ECATCHUP_TIMEOUT;
}

static inline od_frontend_status_t
od_frontend_remote_process_server(od_server_t *server, od_client_t *client,
				  bool await_read)
{
	od_frontend_status_t status = od_relay_step(&server->relay, await_read);
	int rc;
	od_instance_t *instance = client->global->instance;

	if (status == OD_DETACH || status == OD_DETACH_AND_RETRY) {
		/* detach on transaction or statement pooling  */
		/* write any pending data to server first */
		od_frontend_status_t status2;
		od_router_t *router = client->global->router;
		status2 = od_relay_flush(&server->relay);
		if (status2 != OD_OK)
			return status2;

		od_relay_detach(&client->relay);
		od_relay_stop(&server->relay);

		/* cleanup server */
		rc = od_reset(server);
		if (rc != 1) {
			od_router_close(router, client);
		} else {
			od_debug(&instance->logger, "detach", client, server,
				"client %s%.*s detached from %s%.*s",
				client->id.id_prefix,
				(int)sizeof(client->id.id_prefix), client->id.id,
				server->id.id_prefix,
				(int)sizeof(server->id.id_prefix), server->id.id);

			/* push server connection back to route pool */
			
			od_router_detach(router, client);
		}
		server = NULL;
	} else if (status != OD_OK) {
		return status;
	}

	if (status == OD_DETACH_AND_RETRY)
		return OD_DETACH_AND_RETRY;
	return OD_OK;
}

#if 0
static od_frontend_status_t
od_frontend_check_replica_catchup(od_instance_t *instance, od_client_t *client)
{
	od_route_t *route = client->route;

	assert(route);

	uint32_t catchup_timeout = route->rule->catchup_timeout;
	kiwi_var_t *timeout_var =
		kiwi_vars_get(&client->vars, KIWI_VAR_ODYSSEY_CATCHUP_TIMEOUT);
	od_frontend_status_t status = OD_OK;

	if (timeout_var != NULL) {
		/* if there is catchup pgoption variable in startup packet */
		char *end;
		uint32_t user_catchup_timeout =
			strtol(timeout_var->value, &end, 10);
		if (end == timeout_var->value + timeout_var->value_len) {
			// if where is no junk after number, thats ok
			catchup_timeout = user_catchup_timeout;
		} else {
			od_error(&instance->logger, "catchup", client, NULL,
				 "junk after catchup timeout, ignore value");
		}
	}

	if (catchup_timeout) {
		od_debug(&instance->logger, "catchup", client, NULL,
			 "checking for lag before doing any actual work");
		status = od_frontend_poll_catchup(client, route,
						  catchup_timeout);
	}

	return status;
}
#endif

static od_frontend_status_t od_frontend_remote_sync_req(od_client_t *client)
{
	od_frontend_status_t status = OD_OK;
	od_instance_t *instance = client->global->instance;
	od_server_t *server = client->server;

	od_debug(&instance->logger, "sync-point", client,
			 server, "process, %d",
			 od_server_synchronized(server));

	for (;;) {
		if (od_server_synchronized(server)) {
			break;
		}
		// await here
		od_debug(&instance->logger, "sync-point-await",
				 client, server, "process await");
		status = od_frontend_remote_process_server(
				server, client, true);

		if (status != OD_OK) {
			break;
		}
	}

	if (status != OD_OK) {
		od_log(&instance->logger, "sync-point", client,
			   server, "failed to meet sync point");
		return status;
	}

	// deploy here

	assert(server->parse_msg != NULL);

	/* fill internals structs in */
	if (od_frontend_deploy_prepared_stmt_msg(
			server, &server->relay,
			"sync-point-deploy") != OD_OK) {
		status = OD_ESERVER_WRITE;
		return status;
	}

	machine_msg_t *msg;
	msg = kiwi_fe_write_sync(NULL);
	if (msg == NULL) {
		status = OD_ESERVER_WRITE;
		return status;
	}
	int rc;
	rc = od_write(&server->io, msg);
	if (rc == -1) {
		status = OD_ESERVER_WRITE;
		return status;
	}

	/* enter sync piont mode */
	server->sync_point = 1;
	od_server_sync_request(server, 1);
	client->has_error = false;

	for (;;) {
		if (od_server_synchronized(server)) {
			break;
		}
		// await here

		od_debug(&instance->logger, "sync-point",
				 client, server, "process await");
		status = od_frontend_remote_process_server(
				server, client, true);
		if (status != OD_OK) {
			break;
		}
	}

	server->sync_point = 0;

	if (status == OD_DETACH_AND_RETRY)
	{
		od_route_t *route = client->route;
		od_replica_role_t server_role;
		/* 重新 attach 主节点 */
		od_log(&instance->logger, "main", client, server, "standby return error, change primary retry");
		assert(client->server == NULL);
		status = OD_OK;
		server = od_get_or_attach_server(client, &status, REPLICA_ROLE_PRIMARY);
		if (server == NULL)
		{
			return status;
		}
		assert(od_get_replica_role(server) == REPLICA_ROLE_PRIMARY);
		server_role = route->server_pool[server->endpoint_selector].replica_role;
		od_debug(
				&instance->logger, "detach and retry",
				client, server,
				"remote_sync_req OD_DETACH_AND_RETRY od_get_or_attach_server server(%d,%s)",
				server->endpoint_selector, role_to_string(server_role));

		od_hashmap_elt_t key = {.data = client->parse_name, .len = strlen(client->parse_name) + 1};
		od_hash_t keyhash = od_murmur_hash(key.data, key.len);

		od_hashmap_delete(client->prep_stmt_target_roles, keyhash, &key);

		od_hashmap_elt_t value2;
		value2.len = sizeof(od_replica_role_t);
		value2.data = &server_role;
		od_hashmap_elt_t *value_ptr2 = &value2;
		if (od_hashmap_insert(client->prep_stmt_target_roles, keyhash, &key, &value_ptr2, true)) {
		}
		
		od_debug(&instance->logger, "parse", client, server,
				 "update %.*s operator hash %u",
				 key.len, key.data,
				 keyhash);

		if (client->cached_query_type == KIWI_FE_PARSE) {
			assert(client->cached_query != NULL);

			/* 重新构造 parse_msg 待发送给主节点 */
			server->parse_msg = machine_msg_create(client->cached_query_len);
			if (server->parse_msg == NULL) {
				return OD_ESERVER_WRITE;
			}
			memcpy(machine_msg_data(server->parse_msg), client->cached_query, client->cached_query_len);

			return od_frontend_remote_sync_req(client);
		}
	}

	if (status != OD_OK) {
		return status;
	}

	if (client->has_error == false) {
		machine_msg_t *pmsg;
		pmsg = kiwi_be_write_parse_complete(NULL);
		if (pmsg == NULL) {
			return OD_ECLIENT_WRITE;
		}
		machine_iov_add(server->relay.iov, pmsg);
	}
	else {
		od_remove_client_prepared_stmt(client, client->parse_name);
		od_remove_server_prepared_stmt(server, server->parse_msg);
	}
	client->has_error = false; 

	return OD_OK;
}

static od_frontend_status_t od_frontend_remote(od_client_t *client)
{
	od_route_t *route = client->route;
	client->cond = machine_cond_create();

	if (client->cond == NULL) {
		return OD_EOOM;
	}

	od_frontend_status_t status;

	bool reserve_session_server_connection =
		route->rule->reserve_session_server_connection;

	status = od_relay_start(&client->relay, client->cond, OD_ECLIENT_READ,
				OD_ESERVER_WRITE,
				od_frontend_remote_client_on_read,
				&route->stats, od_frontend_remote_client,
				client, reserve_session_server_connection);

	if (status != OD_OK) {
		return status;
	}

	od_server_t *server = NULL;
	od_instance_t *instance = client->global->instance;

	/* Consult lag polling logic and immudiately close connection with
	* error, if lag polling policy says so.
	*/

#if 0
	od_frontend_status_t catchup_status =
		od_frontend_check_replica_catchup(instance, client);
	if (od_frontend_status_is_err(catchup_status)) {
		return catchup_status;
	}
#endif

	for (;;) {
		for (;;) {
			if (od_should_drop_connection(client, client->server)) {
				/* Odyssey is going to shut down or client conn is dropped
				* due some idle timeout, we drop the connection  */
				/* a sort of EAGAIN */
				status = OD_ECLIENT_READ;
				break;
			}

#if OD_DEVEL_LVL != OD_RELEASE_MODE
			if (server != NULL && server->is_transaction &&
			    od_server_synchronized(server)) {
				od_dbg_printf_on_dvl_lvl(
					1,
					"here we have idle in transaction: cid %s\n",
					client->id.id);
			}
#endif

			/* one minute */
			if (machine_cond_wait(client->cond, 60000) == 0) {
				client->time_last_active = machine_time_us();
				od_dbg_printf_on_dvl_lvl(
					1,
					"change client last active time %lld\n",
					client->time_last_active);
				break;
			}
		}

		if (od_frontend_status_is_err(status))
			break;

		/* client operations */
		status = od_frontend_ctl(client);
		if (status != OD_OK)
			break;

#if 0
		/* Check for replication lag and reject query if too big */
		od_frontend_status_t catchup_status =
			od_frontend_check_replica_catchup(instance, client);
		if (od_frontend_status_is_err(catchup_status)) {
			status = catchup_status;
			break;
		}
#endif

		/*
		 * check if it is in session pool and failover has occurred on the primary,
		 * then return server connect error.
		 */
		if (route->rule->pool->pool == OD_RULE_POOL_SESSION &&
			client->server &&
			route->server_pool[client->server->endpoint_selector].replica_role != REPLICA_ROLE_PRIMARY &&
			client->config_listen && 
			client->config_listen->port_attrs != OD_PORT_ATTR_RO)
		{
			od_error(&instance->logger, "main", client, server, 
					"A failover may occur. Disconnect the client beacause seesion pool without RO port should stick to the primary.");
			return OD_ESERVER_CONNECT;
		}

		server = client->server;
		bool sync_req = 0;

		/* attach */
		status = od_relay_step(&client->relay, false);
		if (status == OD_ATTACH) {
			assert(server == NULL);

			od_replica_role_t need_server_role = REPLICA_ROLE_ANY;
			if (route->rule->pool->pool == OD_RULE_POOL_SESSION) {
				if (client->config_listen && client->config_listen->port_attrs == OD_PORT_ATTR_RO)
					need_server_role = REPLICA_ROLE_STANDBY;
				else
					need_server_role = REPLICA_ROLE_PRIMARY;
			}

			status = od_frontend_attach_and_deploy(client, "main", need_server_role);
			if (status != OD_OK)
				break;
			server = client->server;
			status = od_relay_start(
				&server->relay, client->cond, OD_ESERVER_READ,
				OD_ECLIENT_WRITE,
				od_frontend_remote_server_on_read,
				&route->stats, od_frontend_remote_server,
				client, reserve_session_server_connection);
			if (status != OD_OK)
				break;
			od_relay_attach(&client->relay, &server->io);
			od_relay_attach(&server->relay, &client->io);

			/* retry read operation after attach */
			continue;
		} else if (status == OD_REQ_SYNC) {
			sync_req = 1;
		} else if (status != OD_OK) {
			break;
		}

		if ((server = client->server) == NULL)
			continue;

		status = od_frontend_remote_process_server(server, client,
							   false);
		if (status == OD_DETACH_AND_RETRY)
		{
			od_route_t *route = client->route;
			od_replica_role_t server_role;
			/* 重新 attach 主节点 */
			od_log(&instance->logger, "main", client, server, "standby return error, change primary retry");
			assert(client->server == NULL);
			status = OD_OK;
			server = od_get_or_attach_server(client, &status, REPLICA_ROLE_PRIMARY);
			if (server == NULL)
			{
				return status;
			}
			assert(od_get_replica_role(server) == REPLICA_ROLE_PRIMARY);
			server_role = route->server_pool[server->endpoint_selector].replica_role;
			od_debug(
					&instance->logger, "detach and retry",
					client, server,
					"OD_DETACH_AND_RETRY od_get_or_attach_server server(%d,%s)",
					server->endpoint_selector, role_to_string(server_role));

			if (client->cached_query_type == KIWI_FE_QUERY) {
				assert(client->cached_query != NULL);

				/* 重新发一遍包给主节点 */
				machine_msg_t *pmsg = NULL;
				pmsg = kiwi_fe_write_query(NULL, client->cached_query, client->cached_query_len);
				if (pmsg == NULL) {
					return OD_ESERVER_WRITE;
				}
				od_write(&server->io, pmsg);
				od_server_sync_request(server, 1);
			}

			server = client->server;
			status = OD_OK;
		}
		if (status != OD_OK) {
			/* should not return this here */
			assert(status != OD_REQ_SYNC);
			break;
		}

		// are we requested to meet sync point?
		if (sync_req) {
			status = od_frontend_remote_sync_req(client);
			server = client->server;
			if (server && server->parse_msg) {
				machine_msg_free(server->parse_msg);
				server->parse_msg = NULL;
			}
			if (status != OD_OK) {
				break;
			}
		}
	}

	if (client->server) {
		od_server_t *curr_server = client->server;
		od_frontend_status_t flush_status;

		if (status != OD_ESERVER_CONNECT) // TODO 不加会core
		{
			flush_status = od_relay_flush(&curr_server->relay);
			od_relay_stop(&curr_server->relay);
			if (flush_status != OD_OK) {
				return flush_status;
			}
		}

		flush_status = od_relay_flush(&client->relay);
		if (flush_status != OD_OK) {
			return flush_status;
		}
	}

	od_relay_stop(&client->relay);
	return status;
}

static void od_frontend_cleanup(od_client_t *client, char *context,
				od_frontend_status_t status,
				od_error_logger_t *l)
{
	od_instance_t *instance = client->global->instance;
	od_router_t *router = client->global->router;
	od_route_t *route = client->route;
	char peer[128];
	const char *err = "";
	int rc;

	od_server_t *server = client->server;

	if (od_frontend_status_is_err(status)) {
		od_error_logger_store_err(l, status);

		if (route->extra_logging_enabled &&
		    !od_route_is_dynamic(route)) {
			od_error_logger_store_err(route->err_logger, status);
		}
	}

	switch (status) {
	case OD_STOP:
	/* fallthrough */
	case OD_OK:
		/* graceful disconnect or kill */
		if (instance->config.log_session) {
			od_log(&instance->logger, context, client, server,
			       "client disconnected (route %s.%s)",
			       route->rule->db_name, route->rule->user_name);
		}
		if (!client->server)
			break;

		rc = od_reset(server);
		if (rc != 1) {
			/* close backend connection */
			od_router_close(router, client);
			break;
		}
		/* push server to router server pool */
		od_router_detach(router, client);
		break;

	case OD_EOOM:
		od_error(&instance->logger, context, client, server, "%s",
			 "memory allocation error");
		if (client->server)
			od_router_close(router, client);
		break;

	case OD_EATTACH:
		assert(server == NULL);
		assert(client->route != NULL);
		od_frontend_fatal(client, KIWI_CONNECTION_FAILURE,
				  "failed to get remote server connection");
		break;

	case OD_EATTACH_TOO_MANY_CONNECTIONS:
		assert(server == NULL);
		assert(client->route != NULL);
		od_frontend_fatal(
			client, KIWI_TOO_MANY_CONNECTIONS,
			"too many active clients for user (pool_size for "
			"user %s.%s reached %d)",
			client->startup.database.value,
			client->startup.user.value,
			client->rule != NULL ? client->rule->pool->size : -1);
		break;

	case OD_ECLIENT_READ:
		/*fallthrough*/
	case OD_ECLIENT_WRITE:
		/* close client connection and reuse server
			 * link in case of client errors */

		err = od_io_error(&client->io);
		od_getpeername(client->io.io, peer, sizeof(peer), 1, 1);
		od_log(&instance->logger, context, client, server,
		       "client disconnected (read/write error, addr %s): %s, status %s",
		       peer,err,
		       od_frontend_status_to_str(status));
		if (!client->server)
			break;
		rc = od_reset(server);
		if (rc != 1) {
			/* close backend connection */
			od_log(&instance->logger, context, client, server,
			       "reset unsuccessful, closing server connection");
			od_router_close(router, client);
			break;
		}
		/* push server to router server pool */
		od_router_detach(router, client);
		break;

	case OD_ESERVER_CONNECT:
		/* server attached to client and connection failed */
		if (server->error_connect && route->rule->client_fwd_error) {
			/* forward server error to client */
			od_frontend_error_fwd(client);
		} else {
			od_frontend_fatal(
				client, KIWI_CONNECTION_FAILURE,
				"failed to connect to remote server %s%.*s",
				server->id.id_prefix,
				(int)sizeof(server->id.id), server->id.id);
		}
		/* close backend connection */
		od_router_close(router, client);
		break;
	case OD_ECATCHUP_TIMEOUT:
		/* close client connection and close server
			 * connection in case of server errors */
		od_log(&instance->logger, context, client, server,
		       "replication lag is too big, failed to wait replica for catchup: status %s",
		       od_frontend_status_to_str(status));
		od_frontend_fatal(
			client, KIWI_CONNECTION_FAILURE,
			"remote server read/write error: failed to wait replica for catchup");

		if (client->server != NULL) {
			od_router_close(router, client);
		}
		break;
	case OD_ESERVER_READ:
	case OD_ESERVER_WRITE:
		/* close client connection and close server
			 * connection in case of server errors */
		od_log(&instance->logger, context, client, server,
		       "server disconnected (read/write error): %s, status %s",
		       od_io_error(&server->io),
		       od_frontend_status_to_str(status));
		od_frontend_error(client, KIWI_CONNECTION_FAILURE,
				  "remote server read/write error %s%.*s: %s",
				  server->id.id_prefix,
				  (int)sizeof(server->id.id), server->id.id,
				  od_io_error(&server->io));
		/* close backend connection */
		od_router_close(router, client);
		break;
	case OD_UNDEF:
	case OD_SKIP:
	case OD_REQ_SYNC:
	case OD_ATTACH:
	/* fallthrough */
	case OD_DETACH:
	case OD_DETACH_AND_RETRY:
	case OD_ESYNC_BROKEN:
		od_error(&instance->logger, context, client, server,
			 "unexpected error status %s (%d)",
			 od_frontend_status_to_str(status), (uint32)status);
		od_router_close(router, client);
		break;
	default:
		od_error(
			&instance->logger, context, client, server,
			"unexpected error status %s (%d), possible corruption, abort()",
			od_frontend_status_to_str(status), (uint32)status);
		abort();
	}
}

static void od_application_name_add_host(od_client_t *client)
{
	if (client == NULL || client->io.io == NULL)
		return;
	char app_name_with_host[KIWI_MAX_VAR_SIZE];
	char peer_name[KIWI_MAX_VAR_SIZE];
	int app_name_len = 7;
	char *app_name = "unknown";
	kiwi_var_t *app_name_var =
		kiwi_vars_get(&client->vars, KIWI_VAR_APPLICATION_NAME);
	if (app_name_var != NULL) {
		app_name_len = app_name_var->value_len;
		app_name = app_name_var->value;
	}
	od_getpeername(client->io.io, peer_name, sizeof(peer_name), 1,
		       0); // return code ignored

	int length =
		od_snprintf(app_name_with_host, KIWI_MAX_VAR_SIZE, "%.*s - %s",
			    app_name_len, app_name, peer_name);
	kiwi_vars_set(&client->vars, KIWI_VAR_APPLICATION_NAME,
		      app_name_with_host,
		      length + 1); // return code ignored
}

void od_frontend(void *arg)
{
	od_client_t *client = arg;
	od_instance_t *instance = client->global->instance;
	od_router_t *router = client->global->router;
	od_extention_t *extentions = client->global->extentions;
	od_module_t *modules = extentions->modules;

	/* log client connection */
	if (instance->config.log_session) {
		od_getpeername(client->io.io, client->peer,
			       OD_CLIENT_MAX_PEERLEN, 1, 1);
		od_log(&instance->logger, "startup", client, NULL,
		       "new client connection %s", client->peer);
	}

	/* attach client io to worker machine event loop */
	int rc;
	rc = od_io_attach(&client->io);
	if (rc == -1) {
		od_error(&instance->logger, "startup", client, NULL,
			 "failed to transfer client io");
		od_io_close(&client->io);
		od_client_free(client);
		od_atomic_u32_dec(&router->clients_routing);
		return;
	}

	/* ensure global client_max limit */
	uint32_t clients = od_atomic_u32_inc(&router->clients);
	if (instance->config.client_max_set &&
	    clients >= (uint32_t)instance->config.client_max) {
		od_frontend_error(
			client, KIWI_TOO_MANY_CONNECTIONS,
			"too many tcp connections (global client_max %d)",
			instance->config.client_max);
		od_frontend_close(client);
		od_atomic_u32_dec(&router->clients_routing);
		return;
	}

	/* handle startup */
	rc = od_frontend_startup(client);
	if (rc == -1) {
		od_frontend_close(client);
		od_atomic_u32_dec(&router->clients_routing);
		return;
	}

	/* handle cancel request */
	if (client->startup.is_cancel) {
		od_log(&instance->logger, "startup", client, NULL,
		       "cancel request");
		od_router_cancel_t cancel;
		od_router_cancel_init(&cancel);
		rc = od_router_cancel(router, &client->startup.key, &cancel);
		if (rc == 0) {
			od_cancel(client->global, cancel.storage, &cancel.key,
				  &cancel.id, cancel.endpoint_selector);
			od_router_cancel_free(&cancel);
		}
		od_frontend_close(client);
		od_atomic_u32_dec(&router->clients_routing);
		return;
	}

	/* Use client id as backend key for the client.
	 *
	 * This key will be used to identify a server by
	 * user cancel requests. The key must be regenerated
	 * for each new client-server assignment, to avoid
	 * possibility of cancelling requests by a previous
	 * server owners.
	 */
	client->key.key_pid = client->id.id_a;
	client->key.key = client->id.id_b;

	/* route client */
	od_router_status_t router_status;
	router_status = od_router_route(router, client);

	/* routing is over */
	od_atomic_u32_dec(&router->clients_routing);
	od_log(&instance->logger, "route", client, NULL,"clint routing finished. decreased, clients_routing: %u", od_atomic_u32_of(&router->clients_routing));

	if (od_likely(router_status == OD_ROUTER_OK)) {
		od_route_t *route = client->route;
		if (route->rule->application_name_add_host) {
			od_application_name_add_host(client);
		}

		//override clients pg options if configured
		rc = kiwi_vars_override(&client->vars, &route->rule->vars);
		if (rc == -1) {
			goto cleanup;
		}

		char peer[128];
		od_getpeername(client->io.io, peer, sizeof(peer), 1, 0);

		if (instance->config.log_session) {
			od_log(&instance->logger, "startup", client, NULL,
			       "route '%s.%s' to '%s.%s'",
			       client->startup.database.value,
			       client->startup.user.value, route->rule->db_name,
			       route->rule->user_name);
		}
	} else {
		char peer[128];
		od_getpeername(client->io.io, peer, sizeof(peer), 1, 1);

		if (od_router_status_is_err(router_status)) {
			od_error_logger_store_err(router->router_err_logger,
						  router_status);
		}

		switch (router_status) {
		case OD_ROUTER_ERROR:
			od_error(&instance->logger, "startup", client, NULL,
				 "routing failed for '%s' client, closing",
				 peer);
			od_frontend_error(client, KIWI_SYSTEM_ERROR,
					  "client routing failed");
			break;
		case OD_ROUTER_INSUFFICIENT_ACCESS:
			// disabling blind ldapsearch via odyssey error messages
			// to collect user account attributes
			od_error(
				&instance->logger, "startup", client, NULL,
				"route for '%s.%s' is not found by ldapsearch for '%s' client, closing",
				client->startup.database.value,
				client->startup.user.value, peer);
			od_frontend_error(client, KIWI_SYNTAX_ERROR,
					  "incorrect password");
			break;
		case OD_ROUTER_ERROR_NOT_FOUND:
			od_error(
				&instance->logger, "startup", client, NULL,
				"route for '%s.%s' is not found for '%s' client, closing",
				client->startup.database.value,
				client->startup.user.value, peer);
			od_frontend_error(client, KIWI_UNDEFINED_DATABASE,
					  "route for '%s.%s' is not found",
					  client->startup.database.value,
					  client->startup.user.value);
			break;
		case OD_ROUTER_ERROR_LIMIT:
			od_error(
				&instance->logger, "startup", client, NULL,
				"global connection limit reached for '%s' client, closing",
				peer);

			od_frontend_error(
				client, KIWI_TOO_MANY_CONNECTIONS,
				"too many client tcp connections (global client_max)");
			break;
		case OD_ROUTER_ERROR_LIMIT_ROUTE:
			od_error(
				&instance->logger, "startup", client, NULL,
				"route connection limit reached for client '%s', closing",
				peer);
			od_frontend_error(
				client, KIWI_TOO_MANY_CONNECTIONS,
				"too many client tcp connections (client_max for user %s.%s "
				"%d)",
				client->startup.database.value,
				client->startup.user.value,
				client->rule != NULL ?
					client->rule->client_max :
					-1);
			break;
		case OD_ROUTER_ERROR_REPLICATION:
			od_error(
				&instance->logger, "startup", client, NULL,
				"invalid value for parameter \"replication\" for client '%s'",
				peer);

			od_frontend_error(
				client, KIWI_CONNECTION_FAILURE,
				"invalid value for parameter \"replication\"");
			break;
		default:
			assert(0);
			break;
		}

		od_frontend_close(client);
		return;
	}

	/* pre-auth callback */
	od_list_t *i;
	od_list_foreach(&modules->link, i)
	{
		od_module_t *module;
		module = od_container_of(i, od_module_t, link);
		if (module->auth_attempt_cb(client) ==
		    OD_MODULE_CB_FAIL_RETCODE) {
			goto cleanup;
		}
	}

	/* HBA check */
	rc = od_hba_process(client);

	char client_ip[64];
	od_getpeername(client->io.io, client_ip, sizeof(client_ip), 1, 0);

	/* client authentication */
	if (rc == OK_RESPONSE) {
#if 0
		/* Check for replication lag and reject query if too big before auth */
		od_frontend_status_t catchup_status =
			od_frontend_check_replica_catchup(instance, client);
#else
		od_frontend_status_t catchup_status = OD_OK;
#endif
		if (od_frontend_status_is_err(catchup_status)) {
			od_error(
				&instance->logger, "catchup", client, NULL,
				"replicaion lag too big, connection rejected: %s %s",
				client->rule->db_is_default ?
					"(unknown database)" :
					client->startup.database.value,
				client->rule->user_is_default ?
					"(unknown user)" :
					client->startup.user.value);

			od_frontend_fatal(
				client,
				KIWI_INVALID_AUTHORIZATION_SPECIFICATION,
				"replicaion lag too big, connection rejected: %s %s",
				client->rule->db_is_default ?
					"(unknown database)" :
					client->startup.database.value,
				client->rule->user_is_default ?
					"(unknown user)" :
					client->startup.user.value);
			rc = NOT_OK_RESPONSE;
		} else {
			rc = od_auth_frontend(client);
			od_log(&instance->logger, "auth", client, NULL,
			       "ip '%s' user '%s.%s': host based authentication allowed",
			       client_ip,
			       client->rule->db_is_default ?
				       "(unknown database)" :
				       client->startup.database.value,
			       client->rule->user_is_default ?
				       "(unknown user)" :
				       client->startup.user.value);
		}
	} else {
		od_error(
			&instance->logger, "auth", client, NULL,
			"ip '%s' user '%s.%s': host based authentication rejected",
			client_ip,
			client->rule->db_is_default ?
				"(unknown database)" :
				client->startup.database.value,
			client->rule->user_is_default ?
				"(unknown user)" :
				client->startup.user.value);

		od_frontend_error(client, KIWI_INVALID_PASSWORD,
				  "host based authentication rejected");
	}

	if (rc != OK_RESPONSE) {
		/* rc == -1
		 * here we ignore module retcode because auth already failed
		 * we just inform side modules that usr was trying to log in
		 */
		od_list_foreach(&modules->link, i)
		{
			od_module_t *module;
			module = od_container_of(i, od_module_t, link);
			module->auth_complete_cb(client, rc);
		}
		goto cleanup;
	}

	/* auth result callback */
	od_list_foreach(&modules->link, i)
	{
		od_module_t *module;
		module = od_container_of(i, od_module_t, link);
		rc = module->auth_complete_cb(client, rc);
		if (rc != OD_MODULE_CB_OK_RETCODE) {
			// user blocked from module callback
			goto cleanup;
		}
	}

	/* setup client and run main loop */
	od_route_t *route = client->route;

	od_frontend_status_t status;
	status = OD_UNDEF;
	switch (route->rule->storage->storage_type) {
	case OD_RULE_STORAGE_LOCAL: {
		status = od_frontend_local_setup(client);
		if (status != OD_OK)
			break;

		status = od_frontend_local(client);
		break;
	}
	case OD_RULE_STORAGE_REMOTE: {
		status = od_frontend_setup(client);
		if (status != OD_OK)
			break;

		status = od_frontend_remote(client);
		break;
	}
	}
	od_error_logger_t *l;
	l = router->route_pool.err_logger;

	od_frontend_cleanup(client, "main", status, l);

	od_list_foreach(&modules->link, i)
	{
		od_module_t *module;
		module = od_container_of(i, od_module_t, link);
		module->disconnect_cb(client, status);
	}

	/* cleanup */

cleanup:
	/* detach client from its route */
	od_router_unroute(router, client);
	/* close frontend connection */
	od_frontend_close(client);
}

od_frontend_status_t cron_close_expired_prep_stmt(od_server_t *server)
{
	od_frontend_status_t status;
	int closed;
	status = close_expired_prep_stmt(server, NULL, 0, &closed);
	if (status != OD_OK)
		return status;
	
	if (closed > 0) {
		machine_msg_t *msg;
		msg = kiwi_fe_write_sync(NULL);
		if (msg == NULL)
			return OD_ESERVER_WRITE;
		if (od_write(&server->io, msg) == -1)
			return OD_ESERVER_WRITE;

		od_server_sync_request(server, 1);

		int wait_timeout = 1000;
		while (!od_server_synchronized(server)) {
			int rc = od_backend_ready_wait(server, "reset", 1,
					wait_timeout,
					1 /*ignore server errors*/);
			if (rc == NOT_OK_RESPONSE)
				return OD_ESERVER_READ;
		}
	}

	od_route_t *route = server->route;
	od_server_pool_t *server_pool = &route->server_pool[server->endpoint_selector];
	od_io_detach(&server->io);
	
	od_route_lock(route);
	od_pg_server_pool_set(server_pool, server, OD_SERVER_IDLE);
	od_route_unlock(route);

	return OD_OK;
}

#if 0
od_frontend_status_t od_frontend_deallocate_check(char *err_data, int size, od_client_t *client)
{
	int rc = -1;
	od_instance_t *instance = client->global->instance;
	kiwi_fe_error_t error;
	char * prep_stmt_name = NULL;
	uint32_t name_len;
	machine_msg_t *pmsg = NULL;
	bool deleted = false;
	od_server_t *server = client->server == NULL ? NULL : client->server;

	rc = kiwi_fe_read_error(err_data, size, &error);
	if (rc == -1)
		return OD_ESERVER_READ;

	prep_stmt_name = extract_quoted(error.message);
	if (prep_stmt_name == NULL) {
		/* An unname prepared statement */	
		prep_stmt_name = strdup("");
	}
	name_len = strlen(prep_stmt_name) + 1;
	od_hash_t keyhash = od_murmur_hash(prep_stmt_name, name_len);
	od_hashmap_elt_t key;
	key.len = name_len;
	key.data = prep_stmt_name;

	/* Not found in client hashmap, it maybe a prep statement created by 'PREPARE' */
	if (od_hashmap_find(client->prep_stmt_ids, keyhash, &key) == NULL) {
		/* Thus user may actually try to deallocate a statement they didn't prepare */
		/* report client has error*/
		client->has_error = true;
		strcpy(client->error_code, error.code);
		strcpy(client->error_msg, error.message);

		od_error(&instance->logger, "deallocate check", client, server, "%s %s %s",
		 error.severity, error.code, error.message);

		if (error.detail) {
			od_error(&instance->logger, "deallocate check",client, server,
				"DETAIL: %s", error.detail);
		}

		if (error.hint) {
			od_error(&instance->logger, "deallocate check", client,server,
				"HINT: %s", error.hint);
		}
		if (prep_stmt_name) 
			free(prep_stmt_name);
		return OD_OK;
	}

	od_log(&instance->logger, "deallocate check", client, server, "Error ignored: %s %s %s. It's a prepared statement renamed by proxy",
		 error.severity, error.code, error.message);
	pmsg = kiwi_be_write_command_complete(NULL,"",1);
	rc = od_write(&client->io, pmsg);

	/* A statement whose name is modified by us */
	deleted = od_hashmap_delete(client->prep_stmt_ids, keyhash, &key);
	od_hashmap_delete(client->prep_stmt_target_roles, keyhash, &key);
	assert(deleted);
	if (prep_stmt_name) 
		free(prep_stmt_name);
	return OD_SKIP;  /* skip the error response */
}
#endif