#ifndef ODYSSEY_SERVER_POOL_H
#define ODYSSEY_SERVER_POOL_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

typedef struct od_server_pool od_server_pool_t;

typedef int (*od_server_pool_cb_t)(od_server_t *, void **);

typedef enum {
	REPLICA_ROLE_UNKONWN  = 0,
	REPLICA_ROLE_PRIMARY,
	REPLICA_ROLE_STANDBY,
	REPLICA_ROLE_ANY
} od_replica_role_t;

struct od_server_pool {
	od_list_t active;
	od_list_t idle;
	int count_active;
	int count_idle;
	long last_heartbeat;
	/* Possible values:
	 * 0 is a valid value, which means the repliacation is idle
	 * -1 should be considered as invalid value. 
	 * Other values are replag in microseconds
	 */
	long last_replag_musec;
	double last_replag_millisec;
	int latency;
	od_replica_role_t replica_role;
};

static inline void od_server_pool_init(od_server_pool_t *pool)
{
	pool->count_active = 0;
	pool->count_idle = 0;
	pool->last_heartbeat = 0;
	pool->last_replag_musec = -1;
	pool->last_replag_millisec = -1;
	pool->latency = INT_MAX;
	pool->replica_role = REPLICA_ROLE_UNKONWN;
	od_list_init(&pool->idle);
	od_list_init(&pool->active);
}

#define OD_SERVER_POOL_FREE_DECLARE(name, type, server_free_cb)  \
	static inline void od_##name##_server_pool_free(         \
		od_server_pool_t *pool)                          \
	{                                                        \
		type *server;                                    \
		od_list_t *i, *n;                                \
                                                                 \
		od_list_foreach_safe(&pool->idle, i, n)          \
		{                                                \
			server = od_container_of(i, type, link); \
			server_free_cb(server);                  \
		}                                                \
                                                                 \
		od_list_foreach_safe(&pool->active, i, n)        \
		{                                                \
			server = od_container_of(i, type, link); \
			server_free_cb(server);                  \
		}                                                \
	}

OD_SERVER_POOL_FREE_DECLARE(pg, od_server_t, od_server_free)

#ifdef LDAP_FOUND
OD_SERVER_POOL_FREE_DECLARE(ldap, od_ldap_server_t, od_ldap_server_free)
#endif

#define OD_SERVER_POOL_SET_DECLARE(name, type)                                 \
	static inline void od_##name##_server_pool_set(                        \
		od_server_pool_t *pool, type *server, od_server_state_t state) \
	{                                                                      \
		if (server->state == state)                                    \
			return;                                                \
		switch (server->state) {                                       \
		case OD_SERVER_UNDEF:                                          \
			break;                                                 \
		case OD_SERVER_IDLE:                                           \
			pool->count_idle--;                                    \
			break;                                                 \
		case OD_SERVER_ACTIVE:                                         \
			pool->count_active--;                                  \
			break;                                                 \
		}                                                              \
                                                                               \
		od_list_t *target = NULL;                                      \
		switch (state) {                                               \
		case OD_SERVER_UNDEF:                                          \
			break;                                                 \
		case OD_SERVER_IDLE:                                           \
			target = &pool->idle;                                  \
			pool->count_idle++;                                    \
			break;                                                 \
		case OD_SERVER_ACTIVE:                                         \
			target = &pool->active;                                \
			pool->count_active++;                                  \
			break;                                                 \
		}                                                              \
                                                                               \
		od_list_unlink(&server->link);                                 \
		od_list_init(&server->link);                                   \
		if (target) {                                                  \
			od_list_push(target, &server->link);                   \
		}                                                              \
		server->state = state;                                         \
	}

OD_SERVER_POOL_SET_DECLARE(pg, od_server_t)

#ifdef LDAP_FOUND
OD_SERVER_POOL_SET_DECLARE(ldap, od_ldap_server_t)
#endif

#define OD_SERVER_POOL_NEXT_DECLARE(name, type)                     \
	static inline type *od_##name##_server_pool_next(           \
		od_server_pool_t *pool, od_server_state_t state)    \
	{                                                           \
		int target_count = 0;                               \
		od_list_t *target = NULL;                           \
		switch (state) {                                    \
		case OD_SERVER_IDLE:                                \
			target_count = pool->count_idle;            \
			target = &pool->idle;                       \
			break;                                      \
		case OD_SERVER_ACTIVE:                              \
			target_count = pool->count_active;          \
			target = &pool->active;                     \
			break;                                      \
		case OD_SERVER_UNDEF:                               \
			assert(0);                                  \
			break;                                      \
		}                                                   \
		if (target_count == 0)                              \
			return NULL;                                \
		type *server;                                       \
		server = od_container_of(target->next, type, link); \
		return server;                                      \
	}

OD_SERVER_POOL_NEXT_DECLARE(pg, od_server_t)

#ifdef LDAP_FOUND
OD_SERVER_POOL_NEXT_DECLARE(ldap, od_ldap_server_t)
#endif

static inline od_server_t *od_server_pool_foreach(od_server_pool_t *pool,
						  od_server_state_t state,
						  od_server_pool_cb_t callback,
						  void **argv)
{
	od_list_t *target = NULL;
	switch (state) {
	case OD_SERVER_IDLE:
		target = &pool->idle;
		break;
	case OD_SERVER_ACTIVE:
		target = &pool->active;
		break;
	case OD_SERVER_UNDEF:
		assert(0);
		break;
	}
	od_server_t *server;
	od_list_t *i, *n;
	od_list_foreach_safe(target, i, n)
	{
		server = od_container_of(i, od_server_t, link);
		int rc;
		rc = callback(server, argv);
		if (rc) {
			return server;
		}
	}
	return NULL;
}

static inline int od_server_pool_idle(od_server_pool_t *pool, int size)
{
	int i;
	int count_idle = 0;
	for (i = 0; i < size; ++i) {
		count_idle += pool[i].count_idle;
	}
	return count_idle;
}

static inline int od_server_pool_active(od_server_pool_t *pool, int size)
{
	int i;
	int count_active = 0;
	for (i = 0; i < size; ++i) {
		count_active += pool[i].count_active;
	}
	return count_active;
}

static inline int od_server_pool_total(od_server_pool_t *pool, int size)
{
	int i;
	int count = 0;
	for (i = 0; i < size; ++i) {
		count += pool[i].count_active + pool[i].count_idle;
	}
	return count;
}

#endif /* ODYSSEY_SERVER_POOL_H */
