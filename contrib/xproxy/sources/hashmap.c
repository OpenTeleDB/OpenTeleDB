/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

od_hashmap_list_item_t *od_hashmap_list_item_create(void)
{
	od_hashmap_list_item_t *list;
	list = malloc(sizeof(od_hashmap_list_item_t));
	if (list == NULL) {
		return NULL;
	}

	memset(list, 0, sizeof(od_hashmap_list_item_t));
	od_list_init(&list->link);
	return list;
}

void od_hashmap_list_item_add(od_hashmap_list_item_t *list,
			      od_hashmap_list_item_t *it)
{
	od_list_append(&list->link, &it->link);
}

od_retcode_t od_hashmap_list_item_free(od_hashmap_list_item_t *l)
{
	od_list_unlink(&l->link);
	free(l->key.data);
	if (l->value.data) {
		free(l->value.data);
	}
	free(l);

	return OK_RESPONSE;
}

static inline od_retcode_t od_hash_bucket_init(od_hashmap_bucket_t **b)
{
	*b = malloc(sizeof(od_hashmap_bucket_t));
	if (*b == NULL) {
		return NOT_OK_RESPONSE;
	}
	pthread_mutex_init(&(*b)->mu, NULL);
	(*b)->nodes = od_hashmap_list_item_create();
	if ((*b)->nodes == NULL) {
		return NOT_OK_RESPONSE;
	}

	return OK_RESPONSE;
}

static inline od_retcode_t od_hash_bucket_free(od_hashmap_bucket_t *b)
{
	pthread_mutex_destroy(&b->mu);
	od_hashmap_list_item_free(b->nodes);

	free(b);
	return OK_RESPONSE;
}

od_hashmap_t *od_hashmap_create(size_t sz, bool concurrent)
{
	od_hashmap_t *hm;

	hm = malloc(sizeof(od_hashmap_t));
	if (hm == NULL) {
		return NULL;
	}

	hm->size = sz;
	hm->items = 0;
	hm->concurrent = concurrent;
	hm->buckets = malloc(sz * sizeof(od_hashmap_bucket_t *));

	if (hm->buckets == NULL) {
		free(hm);
		return NULL;
	}

	for (size_t i = 0; i < sz; ++i) {
		if (od_hash_bucket_init(&hm->buckets[i]) == NOT_OK_RESPONSE) {
			free(hm->buckets);
			free(hm);
			return NULL;
		}
	}

	return hm;
}

od_retcode_t od_hashmap_free(od_hashmap_t *hm)
{
	for (size_t i = 0; i < hm->size; ++i) {
		od_list_t *j, *n;

		od_list_foreach_safe(&hm->buckets[i]->nodes->link, j, n)
		{
			od_hashmap_list_item_t *it;
			it = od_container_of(j, od_hashmap_list_item_t, link);
			od_hashmap_list_item_free(it);
		}

		od_hash_bucket_free(hm->buckets[i]);
	}

	free(hm->buckets);
	free(hm);

	return OK_RESPONSE;
}

od_retcode_t od_hashmap_foreach(od_hashmap_t *hm,
		void (*cb)(od_hashmap_list_item_t *, void *), void *context)
{
	for (size_t i = 0; i < hm->size; ++i) {
		if (od_unlikely(hm->concurrent))
			pthread_mutex_lock(&hm->buckets[i]->mu);

		od_list_t *j;

		od_list_foreach(&hm->buckets[i]->nodes->link, j)
		{
			od_hashmap_list_item_t *it;
			it = od_container_of(j, od_hashmap_list_item_t, link);
			cb(it, context);
		}

		if (od_unlikely(hm->concurrent))
			pthread_mutex_unlock(&hm->buckets[i]->mu);
	}

	return OK_RESPONSE;
}

od_retcode_t od_hashmap_empty(od_hashmap_t *hm)
{
	for (size_t i = 0; i < hm->size; ++i) {
		if (od_unlikely(hm->concurrent))
			pthread_mutex_lock(&hm->buckets[i]->mu);

		od_list_t *j, *n;

		od_list_foreach_safe(&hm->buckets[i]->nodes->link, j, n)
		{
			od_hashmap_list_item_t *it;
			it = od_container_of(j, od_hashmap_list_item_t, link);
			od_hashmap_list_item_free(it);
		}

		if (od_unlikely(hm->concurrent))
			pthread_mutex_unlock(&hm->buckets[i]->mu);
	}
	hm->items = 0;

	return OK_RESPONSE;
}

od_retcode_t od_hashmap_empty_except(od_hashmap_t *hm, od_hashmap_elt_t *except_key)
{
	for (size_t i = 0; i < hm->size; ++i) {
		if (od_unlikely(hm->concurrent))
			pthread_mutex_lock(&hm->buckets[i]->mu);

		od_list_t *j, *n;

		od_list_foreach_safe(&hm->buckets[i]->nodes->link, j, n)
		{
			od_hashmap_list_item_t *it;
			it = od_container_of(j, od_hashmap_list_item_t, link);
			if (except_key &&
					except_key->len == it->key.len &&
					memcmp(except_key->data, it->key.data, except_key->len) == 0)
				continue;
			od_hashmap_list_item_free(it);
			od_atomic_u32_dec(&hm->items);
		}

		if (od_unlikely(hm->concurrent))
			pthread_mutex_unlock(&hm->buckets[i]->mu);
	}

	return OK_RESPONSE;
}

bool od_hashmap_delete(od_hashmap_t *hm, od_hash_t keyhash,
		od_hashmap_elt_t *key)
{
	bool found = false;
	size_t bucket_index = keyhash % hm->size;
	od_list_t *i;
	od_hashmap_bucket_t *b = hm->buckets[bucket_index];
	if (od_unlikely(hm->concurrent))
		pthread_mutex_lock(&b->mu);

	od_list_foreach(&(b->nodes->link), i)
	{
		od_hashmap_list_item_t *item;
		item = od_container_of(i, od_hashmap_list_item_t, link);
		if (item->key.len == key->len &&
				memcmp(item->key.data, key->data, key->len) == 0) {
			od_hashmap_list_item_free(item);
			od_atomic_u32_dec(&hm->items);
			found = true;
			break;
		}
	}

	if (od_unlikely(hm->concurrent))
		pthread_mutex_unlock(&b->mu);
	return found;
}

static inline od_hashmap_elt_t *od_bucket_search(od_hashmap_bucket_t *b,
						 void *value, size_t value_len)
{
	od_list_t *i;
	od_list_foreach(&(b->nodes->link), i)
	{
		od_hashmap_list_item_t *item;
		item = od_container_of(i, od_hashmap_list_item_t, link);
		if (item->key.len == value_len &&
		    memcmp(item->key.data, value, value_len) == 0) {
			// find
			return &item->value;
		}
	}

	return NULL;
}

static inline int od_hashmap_elt_copy(od_hashmap_elt_t *dst,
				      od_hashmap_elt_t *src)
{
	dst->len = src->len;
	dst->data = malloc(src->len * sizeof(char));
	if (dst->data == NULL) {
		return -1;
	}

	memcpy(dst->data, src->data, src->len);
	return 0;
}

int od_hashmap_insert(od_hashmap_t *hm, od_hash_t keyhash,
		      od_hashmap_elt_t *key, od_hashmap_elt_t **value,
			  bool replace)
{
	size_t bucket_index = keyhash % hm->size;
	if (od_unlikely(hm->concurrent))
		pthread_mutex_lock(&hm->buckets[bucket_index]->mu);

	od_hashmap_elt_t *ptr = od_bucket_search(hm->buckets[bucket_index],
						 key->data, key->len);

	int ret = 1;

	if (ptr == NULL) {
		od_hashmap_list_item_t *it;
		it = od_hashmap_list_item_create();
		if (it != NULL) {
			od_hashmap_elt_copy(&it->key, key);
			od_hashmap_elt_copy(&it->value, *value);

			od_hashmap_list_item_add(
				hm->buckets[bucket_index]->nodes, it);
			*value = &it->value;
			od_atomic_u32_inc(&hm->items);
			ret = 0;
		} else {
			/* oom or other error */
			if (od_unlikely(hm->concurrent))
				pthread_mutex_unlock(&hm->buckets[bucket_index]->mu);
			return -1;
		}
	} else {
		/* element alrady exists,
		* copy *value content to ptr data
		* free previous value */
		if (replace) {
			free(ptr->data);
			od_hashmap_elt_copy(ptr, *value);
		}
		*value = ptr;
	}

	if (od_unlikely(hm->concurrent))
		pthread_mutex_unlock(&hm->buckets[bucket_index]->mu);
	return ret;
}

od_hashmap_elt_t *od_hashmap_find(od_hashmap_t *hm, od_hash_t keyhash,
				  od_hashmap_elt_t *key)
{
	size_t bucket_index = keyhash % hm->size;
	if (od_unlikely(hm->concurrent))
		pthread_mutex_lock(&hm->buckets[bucket_index]->mu);

	od_hashmap_elt_t *ptr = od_bucket_search(hm->buckets[bucket_index],
						 key->data, key->len);

	if (od_unlikely(hm->concurrent))
		pthread_mutex_unlock(&hm->buckets[bucket_index]->mu);
	return ptr;
}

od_hashmap_elt_t *od_hashmap_lock_key(od_hashmap_t *hm, od_hash_t keyhash,
				      od_hashmap_elt_t *key)
{
	size_t bucket_index = keyhash % hm->size;
	/*
	 * This function is used to aquire long locks in auth_query.
	 * To avoid intra-machine locks we must yield cpu slice from time to time
	 * even if waiting for other lock.
	 */
	while (pthread_mutex_trylock(&hm->buckets[bucket_index]->mu) != 0)
		machine_sleep(1);

	od_hashmap_elt_t *ptr = od_bucket_search(hm->buckets[bucket_index],
						 key->data, key->len);
	if (ptr == NULL) {
		od_hashmap_list_item_t *it;
		it = od_hashmap_list_item_create();
		if (it != NULL) {
			od_hashmap_elt_copy(&it->key, key);
			od_hashmap_list_item_add(
				hm->buckets[bucket_index]->nodes, it);
			return &it->value;
		} else {
			/* oom or other error */
			return NULL;
		}
	} else {
		/* element alrady exists, simpty return locked key */
		return ptr;
	}
}

int od_hashmap_unlock_key(od_hashmap_t *hm, od_hash_t keyhash,
			  od_hashmap_elt_t *key)
{
	(void)key;
	size_t bucket_index = keyhash % hm->size;
	pthread_mutex_unlock(&hm->buckets[bucket_index]->mu);
	return 0 /* OK */;
}
