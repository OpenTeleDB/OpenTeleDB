#ifndef ODYSSEY_RULE_STORAGE_H
#define ODYSSEY_RULE_STORAGE_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

typedef struct od_rule_storage od_rule_storage_t;
typedef struct od_storage_watchdog od_storage_watchdog_t;
typedef struct od_storage_watchdog_coroutine_state od_storage_watchdog_coroutine_state_t;	

/* Storage Watchdog */
typedef enum {
	OD_RULE_STORAGE_REMOTE,
	OD_RULE_STORAGE_LOCAL,
} od_rule_storage_type_t;

/* 8 seconds, TODO: use macro for now */
#define OD_WATCHDOG_TIMEOUT 8*1000
struct od_storage_watchdog_coroutine_state {
	int64_t coroutine_id;
	// int online;
};

typedef struct {
    size_t index;
    od_storage_watchdog_t *watchdog;
	od_rule_storage_t *storage;
} watchdog_coroutine_args_t;

struct od_storage_watchdog {
	char *route_usr;
	char *route_db;

	char *storage_user;
	char *storage_db;

	char *primary_arbitration_cmd;
	char *query;	/* deprecate */
	int interval; /* deprecate */
	int check_retry;

	/* soft shutdown on reload */
	pthread_mutex_t mu;
	int online;

	/* current host being accessed */
	size_t current_endpoint;

	/* state of all coroutines */
	/* if this struct only contains a coroutine_id, 
	use an variable to replace it */
	od_storage_watchdog_coroutine_state_t *lifecheck_coroutines;

	int64_t replica_lag_coroutine;
	od_global_t *global;
	
	/* 
	 * Track the number of active coroutines, 
	 * regardless of the health check or lag polling 
	 */
	uint16_t active_coroutines;
};

od_storage_watchdog_t *od_storage_watchdog_allocate(od_global_t *);
int od_storage_watchdog_free(od_storage_watchdog_t *watchdog);

/* */
typedef struct od_storage_endpoint od_storage_endpoint_t;

struct od_storage_endpoint {
	char *host; /* NULL - terminated */
	int port; /* TODO: support somehow */
	int node_id;
	double weight;
	char *application_name;
};

typedef enum {
	OD_TARGET_SESSION_ATTRS_RW,
	OD_TARGET_SESSION_ATTRS_RO,
	OD_TARGET_SESSION_ATTRS_ANY,
} od_target_session_attrs_t;

typedef struct od_auth_cache_value od_auth_cache_value_t;
struct od_auth_cache_value {
	uint64_t timestamp;
	char *passwd;
	uint32_t passwd_len;
};

struct od_rule_storage {
	od_tls_opts_t *tls_opts;

	char *name;
	char *type;
	od_rule_storage_type_t storage_type;
	/* round-robin atomic counter for endpoint selection */
	od_atomic_u32_t rr_counter;

	od_storage_endpoint_t *endpoints;
	size_t endpoints_count;

	char *host; /* host or host,host or [host]:port[,host...] */
	int port; /* default port */

	od_target_session_attrs_t target_session_attrs;

	int server_max_routing;
	od_storage_watchdog_t *watchdog;

	od_hashmap_t *acache;
	od_hashmap_t *bcache;

	od_list_t link;
};

/* storage API */
od_rule_storage_t *od_rules_storage_allocate(void);
od_rule_storage_t *od_rules_storage_copy(od_rule_storage_t *);

void od_rules_storage_free(od_rule_storage_t *);

/* watchdog */
void od_storage_watchdog_replica_lag(void *arg);
void od_storage_watchdog_healthcheck_node(void *arg);
od_retcode_t od_storage_watchdog_launch(od_rule_storage_t *storage);

#endif /* ODYSSEY_RULE_STORAGE_H */
