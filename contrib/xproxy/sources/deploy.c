
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

static inline int enquote_values(char *src, int src_len, char *dst, int dst_len)
{
#define APPEND_CHAR(c)	{ if (od_likely(pos < end)) { *pos++ = c; } else { return -1; } }

	if (dst_len < 4)
		return -1;
	char *pos = dst;
	char *end = dst + dst_len - 4;
	char *src_end = src + src_len;

	do {
		if (pos != dst)
			APPEND_CHAR(',');
		APPEND_CHAR('E');
		APPEND_CHAR('\'');

		while (*src) {
			if (*src == '\'') {
				APPEND_CHAR('\'');
			}
			else if (*src == '\\') {
				APPEND_CHAR('\\');
			}
			APPEND_CHAR((*src++));
		}
		APPEND_CHAR('\'');
		++src;
	} while (src < src_end);

	*pos = 0;
	return (int)(pos - dst);
}

/* Compare and set options without GUC_REPORT flag */
static int other_vars_cas(od_hashmap_t *client,
		od_hashmap_t *server,
		char *query, int query_len)
{

	int pos = 0;

	/* Ensure that the size of the hashtable is equal to avoid recalculating vars' hash */
	assert(client->size == server->size);

	for (size_t i = 0; i < client->size; ++i) {
		od_list_t *it;

		if (od_likely(od_list_empty(&client->buckets[i]->nodes->link)) &&
				od_likely(od_list_empty(&server->buckets[i]->nodes->link)))
			continue;

		/* Compare and set client's vars */
		od_list_foreach(&client->buckets[i]->nodes->link, it) {
			od_hashmap_list_item_t *item;
			item = od_container_of(it, od_hashmap_list_item_t, link);
			od_hashmap_elt_t *server_var = od_hashmap_find(server, i, &item->key);

			if (server_var && server_var->len == item->value.len &&
					memcmp(server_var->data, item->value.data, server_var->len) == 0)
				continue;

			/* SET key=quoted_value; */
			int size = 4 + (item->key.len - 1) + 1 + 1;
			if (query_len < size)
				return -1;
			memcpy(query + pos, "SET ", 4);
			pos += 4;
			memcpy(query + pos, item->key.data, item->key.len - 1);
			pos += item->key.len - 1;
			memcpy(query + pos, "=", 1);
			pos += 1;

			int quote_len;
			quote_len = enquote_values(item->value.data,
					item->value.len, query + pos, query_len - pos);
			if (quote_len == -1)
				return -1;
			pos += quote_len;
			query[pos] = ';';
			pos += 1;
		}

		/* Reset vars only modified by server */
		od_list_foreach(&server->buckets[i]->nodes->link, it) {
			od_hashmap_list_item_t *item;
			item = od_container_of(it, od_hashmap_list_item_t, link);
			if(od_hashmap_find(client, i, &item->key))
				continue;

			/* RESET key; */
			int size = 6 + (item->key.len - 1) + 1;
			if (query_len < size)
				return -1;
			memcpy(query + pos, "RESET ", 6);
			pos += 6;
			memcpy(query + pos, item->key.data, item->key.len - 1);
			pos += item->key.len - 1;
			query[pos] = ';';
			pos += 1;
		}

	}


	return pos;

}

int od_deploy(od_client_t *client, char *context)
{
	od_instance_t *instance = client->global->instance;
	od_server_t *server = client->server;
	od_route_t *route = client->route;

	if (route->id.physical_rep || route->id.logical_rep) {
		return 0;
	}

	/* compare and set options which are differs from server */
	int query_count;
	query_count = 0;

	char query[OD_QRY_MAX_SZ];
	int query_size;

	client->server->synced_settings = true;
	for (int i = 0; i < 2; ++i) {
		if (i == 0) {
			/* handle vars with GUC_REPORT flag */
			query_size = kiwi_vars_cas(&client->vars,
					&server->vars,
					&route->params.vars,
					query,
					sizeof(query) - 1);
		}
		else {
			/* handle vars without GUC_REPORT flag */
			query_size = other_vars_cas(client->other_vars,
					server->other_vars,
					query,
					sizeof(query) - 1);
		}

		if (query_size > 0) {
			query[query_size] = 0;
			query_size++;
			machine_msg_t *msg;
			msg = kiwi_fe_write_query(NULL, query, query_size);
			if (msg == NULL)
				return -1;

			int rc;
			rc = od_write(&server->io, msg);
			if (rc == -1)
				return -1;

			query_count++;
			client->server->synced_settings = false;

			od_debug(&instance->logger, context, client, server,
					"deploy: %s", query);
		}
	}

	return query_count;
}
