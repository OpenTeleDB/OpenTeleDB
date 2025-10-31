
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

int od_cancel(od_global_t *global, od_rule_storage_t *storage, kiwi_key_t *key,
	      od_id_t *server_id, size_t endpoint_selector)
{
	od_instance_t *instance = global->instance;
	od_log(&instance->logger, "cancel", NULL, NULL, "cancel for %s%.*s",
	       server_id->id_prefix, sizeof(server_id->id), server_id->id);
	od_server_t *server = od_server_allocate(0);
	server->global = global;
	server->endpoint_selector = endpoint_selector;
	od_backend_connect_cancel(server, storage, key);
	od_backend_close_connection(server);
	od_backend_close(server);
	return 0;
}
