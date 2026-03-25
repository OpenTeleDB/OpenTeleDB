#ifndef ODYSSEY_LRU_H
#define ODYSSEY_LRU_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

//
// Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
//

typedef struct {
    int prev;
    int next;
    uint64_t last_atime;
    void *data; // 通用数据指针
    size_t len;
} od_lru_elem_t;

typedef struct {
    od_lru_elem_t *elems;
    int head;
    int last;
    int free;
    int size;
	int count;
    void (*free_data)(void *); // 数据释放回调
} od_lru_t;

static inline od_lru_t *od_lru_init(int init_size, void (*free_data)(void *)) {
	od_lru_t *lru = malloc(sizeof(*lru));
	if (!lru) return NULL;

	lru->elems = calloc(init_size, sizeof(od_lru_elem_t));
	if (!lru->elems) {
		free(lru);
		return NULL;
	}

	lru->size = init_size;
	lru->free = 0;
	lru->head = -1;
	lru->last = -1;
	lru->free_data = free_data;
	lru->count = 0;

	for (int i = 0; i < init_size; ++i) {
		lru->elems[i].prev = -1;
		lru->elems[i].next = (i < init_size - 1) ? (i + 1) : -1;
	}

	return lru;
}

static inline int od_lru_size(od_lru_t *lru) {
	return lru->count;
}

static inline void od_lru_free(od_lru_t *lru) {
	int pos;
    for (pos = lru->head; pos >= 0;) {
        od_lru_elem_t *elem = &lru->elems[pos];
        pos = elem->next;
        if (lru->free_data) {
            lru->free_data(elem->data); // 使用回调释放数据
        }
    }
    free(lru->elems);
    free(lru);
}

static inline void od_lru_empty(od_lru_t *lru) {
	for (int pos = lru->head; pos >= 0;) {
		od_lru_elem_t *elem = &lru->elems[pos];
		pos = elem->next;
		if (lru->free_data) {
			lru->free_data(elem->data);
		}
	}

	for (int i = 0; i < lru->size; ++i) {
		lru->elems[i].next = od_unlikely(i < lru->size - 1) ? (i + 1) : -1;
	}

	lru->free = 0;
	lru->head = -1;
	lru->last = -1;
	lru->count = 0;
}

static inline int od_lru_add(od_lru_t *lru, void *data, size_t len) {
	od_lru_elem_t *elem;
	int pos;

	if (lru->free == -1) {
		int new_size = lru->size * 2;
		od_lru_elem_t *new_elems = realloc(lru->elems, new_size * sizeof(od_lru_elem_t));
		if (!new_elems) return -1;

		lru->elems = new_elems;
		for (int i = lru->size; i < new_size; ++i) {
			elem = &lru->elems[i];
			elem->prev = -1;
			elem->next =  (i < new_size - 1) ? (i + 1) : -1;
		}

		lru->free = lru->size;
		lru->size = new_size;
	}

	pos = lru->free;
	elem = &lru->elems[pos];
	// 复制数据
	elem->data = malloc(len);
	if (!elem->data) {
		return -1;
	}
	memcpy(elem->data, data, len);
	elem->len = len;
	elem->last_atime = machine_time_us();

	lru->free = elem->next;

	elem->next = lru->head;
	elem->prev = -1;
	if (lru->head >= 0) {
		lru->elems[lru->head].prev = pos;
	} else {
		lru->last = pos;
	}
	lru->head = pos;
	lru->count += 1;

	return pos;
}

static inline void od_lru_delete(od_lru_t *lru, int pos) {
    od_lru_elem_t *elem = &lru->elems[pos];

    if (elem->prev >= 0) {
        lru->elems[elem->prev].next = elem->next;
    } else {
        lru->head = elem->next;
    }

    if (elem->next >= 0) {
        lru->elems[elem->next].prev = elem->prev;
    } else {
        lru->last = elem->prev;
    }

    elem->next = lru->free;
    elem->prev = -1;
    lru->free = pos;
	lru->count -= 1;

    if (lru->free_data) {
        lru->free_data(elem->data); // 使用回调释放数据
    }
}

static inline void od_lru_touch(od_lru_t *lru, int pos) {
	if (pos != lru->head) {
		od_lru_elem_t *elem = &lru->elems[pos];

		if (elem->prev >= 0) {
			lru->elems[elem->prev].next = elem->next;
		} else {
			lru->head = elem->next;
		}

		if (elem->next >= 0) {
			lru->elems[elem->next].prev = elem->prev;
		} else {
			lru->last = elem->prev;
		}

		elem->next = lru->head;
		if (lru->head >= 0) {
			lru->elems[lru->head].prev = pos;
		}
		elem->prev = -1;
		elem->last_atime = machine_time_us();
		lru->head = pos;
	}
}

static inline od_lru_elem_t *od_lru_last(od_lru_t *lru)
{
	return (lru->last >= 0) ? &lru->elems[lru->last] : NULL;
}

static inline od_lru_elem_t *od_lru_last_with_id(od_lru_t *lru, int *last_id)
{
	*last_id = lru->last; 
	return (lru->last >= 0) ? &lru->elems[lru->last] : NULL;
}

static inline void od_lru_pop_last(od_lru_t *lru)
{
	if (lru->last >= 0)
		od_lru_delete(lru, lru->last);
}


#endif
