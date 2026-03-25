#ifndef ODYSSEY_LIST_H
#define ODYSSEY_LIST_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

typedef struct od_list od_list_t;

struct od_list {
	od_list_t *next;
	od_list_t *prev;
};

static inline void od_list_init(od_list_t *list)
{
	list->next = list->prev = list;
}

static inline void od_list_append(od_list_t *list, od_list_t *node)
{
	node->next = list;
	node->prev = list->prev;
	node->prev->next = node;
	node->next->prev = node;
}

static inline void od_list_unlink(od_list_t *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
}

static inline void od_list_push(od_list_t *list, od_list_t *node)
{
	node->next = list->next;
	node->prev = list;
	node->prev->next = node;
	node->next->prev = node;
}

static inline od_list_t *od_list_pop(od_list_t *list)
{
	register od_list_t *pop = list->next;
	od_list_unlink(pop);
	return pop;
}

static inline od_list_t *od_list_pop_back(od_list_t *list)
{
	register od_list_t *pop = list->prev;
	od_list_unlink(pop);
	return pop;
}

static inline int od_list_empty(od_list_t *list)
{
	return list->next == list && list->prev == list;
}

static inline int od_list_move(od_list_t *dst, od_list_t *src)
{
	if (!od_list_empty(dst))
		return NOT_OK_RESPONSE;
	if (od_list_empty(src))
		return OK_RESPONSE;

	dst->next = src->next;
	dst->prev = src->prev;
	src->prev->next = dst;
	src->next->prev = dst;

	src->next = src->prev = src;
	return OK_RESPONSE;
}

static inline void od_list_move_to_tail(od_list_t *dst, od_list_t *src)
{
	if (od_list_empty(src))
		return;
	dst->prev->next = src->next;
	src->next->prev = dst->prev;
	src->prev->next = dst;
	dst->prev = src->prev;
	src->next = src->prev = src;
}

#define od_list_foreach(list, iterator)                 \
	for (iterator = (list)->next; iterator != list; \
	     iterator = (iterator)->next)

#define od_list_foreach_safe(list, iterator, safe) \
	for (iterator = (list)->next;              \
	     iterator != list && (safe = iterator->next); iterator = safe)

#define od_list_foreach_with_start(list, iterator) \
	for (; iterator != list; iterator = (iterator)->next)

static inline int od_list_size(od_list_t *list)
{
	int cnt = 0;
	od_list_t *it;
	od_list_foreach(list, it) {
		cnt += 1;
	}
	return cnt;
}

#endif /* ODYSSEY_LIST_H */
