#ifndef ODYSSEY_STAT_H
#define ODYSSEY_STAT_H

#include "histogram.h"
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#define QUANTILES_WINDOW 2
#define QUANTILES_COMPRESSION 100

typedef struct od_stat_state od_stat_state_t;
typedef struct od_stat od_stat_t;

struct od_stat_state {
	uint64_t query_time_start;
	uint64_t tx_time_start;
};

struct od_stat {
	bool enable_quantiles;

	od_atomic_u64_t count_query;
	od_atomic_u64_t count_tx;

	od_atomic_u64_t query_time;
	od_atomic_u64_t tx_time;
	od_atomic_u64_t pool_wait_time;

	od_atomic_u64_t recv_server;
	od_atomic_u64_t recv_client;
	od_atomic_u64_t count_parse;
	od_atomic_u64_t count_parse_reuse;
	od_atomic_u64_t count_rwsplit;
	od_atomic_u64_t count_rwsplit_wrong;
	od_atomic_u64_t count_rwsplit_blacklist_hit;

	/* per-endpoint select count */
	size_t endpoints_count;
	od_atomic_u64_t *count_select_endpoints;

	od_histogram_t *transaction_hgram;
	od_histogram_t *query_hgram;
	od_histogram_t *pool_wait_hgram;
};

static inline void od_stat_state_init(od_stat_state_t *state)
{
	memset(state, 0, sizeof(*state));
}

static inline void od_stat_init(od_stat_t *stat, size_t endpoints_count)
{
	memset(stat, 0, sizeof(*stat));
	stat->endpoints_count = endpoints_count;
	stat->count_select_endpoints = (od_atomic_u64_t *)malloc(sizeof(od_atomic_u64_t) * endpoints_count);
	memset((void *)stat->count_select_endpoints, 0, sizeof(od_atomic_u64_t) * endpoints_count);
}

static inline void od_stat_free(od_stat_t *stat)
{
	if (stat->enable_quantiles) {
		od_histogram_delete(stat->transaction_hgram);
		od_histogram_delete(stat->query_hgram);
		od_histogram_delete(stat->pool_wait_hgram);
	}

	if (stat->count_select_endpoints)
		free((void *)stat->count_select_endpoints);
}

static inline void od_stat_query_start(od_stat_state_t *state, uint64_t ts)
{
	if (!state->query_time_start)
		state->query_time_start = ts;

	if (!state->tx_time_start)
		state->tx_time_start = ts;
}

static inline void od_stat_parse(od_stat_t *stat)
{
	od_atomic_u64_inc(&stat->count_parse);
}

static inline void od_stat_parse_reuse(od_stat_t *stat)
{
	od_atomic_u64_inc(&stat->count_parse_reuse);
}

static inline void od_stat_rwsplit(od_stat_t *stat)
{
	od_atomic_u64_inc(&stat->count_rwsplit);
}

static inline void od_stat_rwsplit_wrong(od_stat_t *stat)
{
	od_atomic_u64_inc(&stat->count_rwsplit_wrong);
}

static inline void od_stat_rwsplit_blacklist_hit(od_stat_t *stat)
{
	od_atomic_u64_inc(&stat->count_rwsplit_blacklist_hit);
}

static inline void od_stat_select_endpoint_by_index(od_stat_t *stat, size_t index)
{
	od_atomic_u64_inc(&stat->count_select_endpoints[index]);
}

static inline void od_stat_query_end(od_stat_t *stat, od_stat_state_t *state,
				     int in_transaction, int64_t *query_time)
{
	int64_t diff;
	if (state->query_time_start) {
		diff = machine_time_us() - state->query_time_start;
		if (diff > 0) {
			*query_time = diff;
			od_atomic_u64_add(&stat->query_time, diff);
			od_atomic_u64_inc(&stat->count_query);
			if (stat->enable_quantiles) {
				od_histogram_update(stat->query_hgram, diff);
			}
		}
		state->query_time_start = 0;
	}

	if (in_transaction)
		return;

	if (state->tx_time_start) {
		diff = machine_time_us() - state->tx_time_start;
		if (diff > 0) {
			od_atomic_u64_add(&stat->tx_time, diff);
			od_atomic_u64_inc(&stat->count_tx);
			if (stat->enable_quantiles) {
				od_histogram_update(stat->transaction_hgram, diff);
			}
		}
		state->tx_time_start = 0;
	}
}

static inline void od_stat_recv_server(od_stat_t *stat, uint64_t bytes)
{
	od_atomic_u64_add(&stat->recv_server, bytes);
}

static inline void od_stat_recv_client(od_stat_t *stat, uint64_t bytes)
{
	od_atomic_u64_add(&stat->recv_client, bytes);
}

static inline void od_stat_copy(od_stat_t *dst, od_stat_t *src)
{
	dst->count_query = od_atomic_u64_of(&src->count_query);
	dst->count_tx = od_atomic_u64_of(&src->count_tx);
	dst->query_time = od_atomic_u64_of(&src->query_time);
	dst->tx_time = od_atomic_u64_of(&src->tx_time);
	dst->recv_client = od_atomic_u64_of(&src->recv_client);
	dst->recv_server = od_atomic_u64_of(&src->recv_server);
	dst->count_parse = od_atomic_u64_of(&src->count_parse);
	dst->count_parse_reuse = od_atomic_u64_of(&src->count_parse_reuse);
	dst->pool_wait_time = od_atomic_u64_of(&src->pool_wait_time);
	dst->count_rwsplit = od_atomic_u64_of(&src->count_rwsplit);
	dst->count_rwsplit_wrong = od_atomic_u64_of(&src->count_rwsplit_wrong);
	dst->count_rwsplit_blacklist_hit = od_atomic_u64_of(&src->count_rwsplit_blacklist_hit);

	for (size_t i = 0; i < dst->endpoints_count; i++) {
		dst->count_select_endpoints[i] = od_atomic_u64_of(&src->count_select_endpoints[i]);
	}
}

static inline void od_stat_sum(od_stat_t *sum, od_stat_t *stat)
{
	sum->count_query += od_atomic_u64_of(&stat->count_query);
	sum->count_tx += od_atomic_u64_of(&stat->count_tx);
	sum->query_time += od_atomic_u64_of(&stat->query_time);
	sum->tx_time += od_atomic_u64_of(&stat->tx_time);
	sum->recv_client += od_atomic_u64_of(&stat->recv_client);
	sum->recv_server += od_atomic_u64_of(&stat->recv_server);
	sum->count_parse += od_atomic_u64_of(&stat->count_parse);
	sum->count_parse_reuse += od_atomic_u64_of(&stat->count_parse_reuse);
	sum->pool_wait_time += od_atomic_u64_of(&stat->pool_wait_time);
	sum->count_rwsplit += od_atomic_u64_of(&stat->count_rwsplit);
	sum->count_rwsplit_wrong+= od_atomic_u64_of(&stat->count_rwsplit_wrong);
	sum->count_rwsplit_blacklist_hit += od_atomic_u64_of(&stat->count_rwsplit_blacklist_hit);

	for (size_t i = 0; i < sum->endpoints_count; i++) {
		sum->count_select_endpoints[i] += od_atomic_u64_of(&stat->count_select_endpoints[i]);
	}
}

static inline void od_stat_update_of(od_atomic_u64_t *prev,
				     od_atomic_u64_t *current)
{
	/* todo: this could be made more optimal */
	/* prev <= current */
	__atomic_store((uint64_t *)prev, (uint64_t *)current, __ATOMIC_SEQ_CST);
}

static inline void od_stat_update(od_stat_t *dst, od_stat_t *stat)
{
	od_stat_update_of(&dst->count_query, &stat->count_query);
	od_stat_update_of(&dst->count_tx, &stat->count_tx);
	od_stat_update_of(&dst->query_time, &stat->query_time);
	od_stat_update_of(&dst->tx_time, &stat->tx_time);
	od_stat_update_of(&dst->recv_client, &stat->recv_client);
	od_stat_update_of(&dst->recv_server, &stat->recv_server);
	od_stat_update_of(&dst->count_parse, &stat->count_parse);
	od_stat_update_of(&dst->count_parse_reuse, &stat->count_parse_reuse);
	od_stat_update_of(&dst->pool_wait_time, &stat->pool_wait_time);
	od_stat_update_of(&dst->count_rwsplit, &stat->count_rwsplit);
	od_stat_update_of(&dst->count_rwsplit_wrong, &stat->count_rwsplit_wrong);
	od_stat_update_of(&dst->count_rwsplit_blacklist_hit, &stat->count_rwsplit_blacklist_hit);

	for (size_t i = 0; i < dst->endpoints_count; i++) {
		od_stat_update_of(&dst->count_select_endpoints[i], &stat->count_select_endpoints[i]);
	}
}

static inline void od_stat_average(od_stat_t *avg, od_stat_t *current,
				   od_stat_t *prev, uint64_t prev_time_us)
{
	const uint64_t interval_usec = 1000000;
	uint64_t interval_us;
	interval_us = machine_time_us() - prev_time_us;
	if (interval_us <= 0)
		return;

	uint64_t count_query;
	count_query = od_atomic_u64_of(&current->count_query) -
		      od_atomic_u64_of(&prev->count_query);

	uint64_t count_tx;
	count_tx = od_atomic_u64_of(&current->count_tx) -
		   od_atomic_u64_of(&prev->count_tx);

	uint64_t count_parse;
	count_parse = od_atomic_u64_of(&current->count_parse) -
		      od_atomic_u64_of(&prev->count_parse);

	uint64_t count_parse_reuse;
	count_parse_reuse = od_atomic_u64_of(&current->count_parse_reuse) -
			    od_atomic_u64_of(&prev->count_parse_reuse);

	uint64_t count_rwsplit, count_rwsplit_wrong, count_rwsplit_blacklist_hit;
	count_rwsplit = od_atomic_u64_of(&current->count_rwsplit) -
					od_atomic_u64_of(&prev->count_rwsplit);
	count_rwsplit_wrong = od_atomic_u64_of(&current->count_rwsplit_wrong) -
						  od_atomic_u64_of(&prev->count_rwsplit_wrong);
	count_rwsplit_blacklist_hit = od_atomic_u64_of(&current->count_rwsplit_blacklist_hit) -
								  od_atomic_u64_of(&prev->count_rwsplit_blacklist_hit);
	
	uint64_t *count_select_endpoints = malloc(sizeof(uint64_t) * avg->endpoints_count);
	for (size_t i = 0; i < avg->endpoints_count; i++) {
		count_select_endpoints[i] = od_atomic_u64_of(&current->count_select_endpoints[i]) -
					  od_atomic_u64_of(&prev->count_select_endpoints[i]);
	}

	avg->count_query = (count_query * interval_usec) / interval_us;
	avg->count_tx = (count_tx * interval_usec) / interval_us;
	avg->count_parse = (count_parse * interval_usec) / interval_us;
	avg->count_parse_reuse =
		(count_parse_reuse * interval_usec) / interval_us;
	avg->count_rwsplit = (count_rwsplit * interval_usec) / interval_us;
	avg->count_rwsplit_wrong = (count_rwsplit_wrong * interval_usec) / interval_us;
	avg->count_rwsplit_blacklist_hit = (count_rwsplit_blacklist_hit * interval_usec) / interval_us;

	for (size_t i = 0; i < avg->endpoints_count; i++) {
		avg->count_select_endpoints[i] = (count_select_endpoints[i] * interval_usec) / interval_us;
	}
	free(count_select_endpoints);

	if (count_query > 0) {
		avg->query_time = (od_atomic_u64_of(&current->query_time) -
				   od_atomic_u64_of(&prev->query_time)) /
				  count_query;
	}

	if (count_tx > 0) {
		avg->tx_time = (od_atomic_u64_of(&current->tx_time) -
						od_atomic_u64_of(&prev->tx_time)) /
					   count_tx;
		avg->pool_wait_time = (od_atomic_u64_of(&current->pool_wait_time) -
							   od_atomic_u64_of(&prev->pool_wait_time)) /
							  count_tx;
	}

	avg->recv_client = ((od_atomic_u64_of(&current->recv_client) -
			     od_atomic_u64_of(&prev->recv_client)) *
			    interval_usec) /
			   interval_us;

	avg->recv_server = ((od_atomic_u64_of(&current->recv_server) -
			     od_atomic_u64_of(&prev->recv_server)) *
			    interval_usec) /
			   interval_us;
}

#endif /* ODYSSEY_STAT_H */
