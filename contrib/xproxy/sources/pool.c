/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

od_rule_pool_t *od_rule_pool_alloc()
{
	od_rule_pool_t *pool;
	pool = malloc(sizeof(od_rule_pool_t));

	if (pool == NULL) {
		return NULL;
	}
	memset(pool, 0, sizeof(od_rule_pool_t));

	pool->cancel = 1;
	pool->rollback = 1;

	return pool;
}

int od_rule_pool_free(od_rule_pool_t *pool)
{
	if (pool->routing_type) {
		free(pool->routing_type);
	}
	if (pool->type) {
		free(pool->type);
	}
	if (pool->idle_timeout_whitelist_count > 0) {
		for (size_t i = 0; i < pool->idle_timeout_whitelist_count; i++) {
			free(pool->idle_timeout_whitelist[i]);
		}
	}
	free(pool);
	return OK_RESPONSE;
}

int od_rule_pool_compare(od_rule_pool_t *a, od_rule_pool_t *b)
{
	/* pool */
	if (a->pool != b->pool)
		return 0;

	/* pool routing */
	if (a->routing != b->routing)
		return 0;

	/* size */
	if (a->size != b->size)
		return 0;

	/* timeout */
	if (a->timeout != b->timeout)
		return 0;

	/* ttl */
	if (a->ttl != b->ttl)
		return 0;

	/* cancel */
	if (a->cancel != b->cancel)
		return 0;

	/* rollback*/
	if (a->rollback != b->rollback)
		return 0;

	/* client idle timeout */
	if (a->client_idle_timeout != b->client_idle_timeout) {
		return 0;
	}

	/* idle_in_transaction_timeout */
	if (a->idle_in_transaction_timeout != b->idle_in_transaction_timeout) {
		return 0;
	}

	/* reserve_prepared_statement */
	if (a->reserve_prepared_statement != b->reserve_prepared_statement) {
		return 0;
	}

	if (a->prepared_statement_limit != b->prepared_statement_limit) {
		return 0;
	}

	if(a->prepared_statement_expired_time != b->prepared_statement_expired_time) {
		return 0;
	}

	/* taget_servser_attrs */
	if (a->target_server_attrs != b->target_server_attrs) {
		return 0;
	}

	/* white list count*/
	if (a->idle_timeout_whitelist_count != b->idle_timeout_whitelist_count) {
		return 0;
	}

	 /* white list */
	 if (a->idle_timeout_whitelist && b->idle_timeout_whitelist) {
		for (size_t i = 0; i < a->idle_timeout_whitelist_count; i++) {
			if (strcmp(a->idle_timeout_whitelist[i], b->idle_timeout_whitelist[i]) != 0)
				return 0;
		}
	 }

	return 1;
}

int od_rule_matches_client(od_rule_pool_t *pool, od_pool_client_type_t t)
{
	switch (t) {
	case OD_POOL_CLIENT_INTERNAL:
		return pool->routing == OD_RULE_POOL_INTERNAL;
	case OD_POOL_CLIENT_EXTERNAL:
		return pool->routing == OD_RULE_POOL_CLIENT_VISIBLE;
	default:
		// no macthes
		return 0;
	}
}

/*
 * check if user is in the whitelist
 */
int od_rule_matches_whitelist_user(od_rule_pool_t *pool, char *username)
{
	if (pool->idle_timeout_whitelist_count == 0 || username == NULL) {
		return 0;
	}

	for (size_t i = 0; i < pool->idle_timeout_whitelist_count; i++) {
		if (strcmp(pool->idle_timeout_whitelist[i], username) == 0) {
			return 1;
		}
	}

	return 0;
}