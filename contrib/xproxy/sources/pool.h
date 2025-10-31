#ifndef ODYSSEY_POOL_H
#define ODYSSEY_POOL_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

typedef struct od_rule_pool od_rule_pool_t;

typedef enum {
	OD_RULE_POOL_SESSION,
	OD_RULE_POOL_TRANSACTION,
} od_rule_pool_type_t;

typedef enum {
	OD_RULE_POOL_INTERNAL,
	OD_RULE_POOL_CLIENT_VISIBLE,
} od_rule_routing_type_t;

typedef enum {
	OD_POOL_CLIENT_UNDEF,
	OD_POOL_CLIENT_INTERNAL,
	OD_POOL_CLIENT_EXTERNAL,
} od_pool_client_type_t;

typedef enum {
	OD_TARGET_SERVER_RW,
	OD_TARGET_SERVER_RO,
	OD_TARGET_SERVER_AUTO
} od_target_server_type_t;

struct od_rule_pool {
	/* pool */
	od_rule_pool_type_t pool;
	od_rule_routing_type_t routing;

	char *type;
	char *routing_type;

	int size;
	int timeout;
	int ttl;
	int discard;
	int cancel;
	int rollback;

	// --------  makes sense only for transaction pooling ---------------------------
	int reserve_prepared_statement;
	int prepared_statement_limit;
	int prepared_statement_expired_time;
	// ------------------------------------------------------------------------------

	od_target_server_type_t target_server_attrs;
	
	// --------  makes sense only for session pooling -------------------------------
	uint64_t client_idle_timeout;
	uint64_t idle_in_transaction_timeout;
	char **idle_timeout_whitelist;
	size_t idle_timeout_whitelist_count;
	// ------------------------------------------------------------------------------
};

od_rule_pool_t *od_rule_pool_alloc();

int od_rule_pool_free(od_rule_pool_t *pool);

int od_rule_pool_compare(od_rule_pool_t *a, od_rule_pool_t *b);

int od_rule_matches_client(od_rule_pool_t *pool, od_pool_client_type_t t);

int od_rule_matches_whitelist_user(od_rule_pool_t *pool, char *username);

#endif /* ODYSSEY_POOL_H */
