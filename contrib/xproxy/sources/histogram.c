#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "odyssey.h"
#include "histogram.h"
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

/*
   Number of slots for current histogram array. TODO: replace this constant with
   some autodetection code based on the number of available cores or some such.
*/
#define OD_HISTOGRAM_NSLOTS 16

static inline uint64_t od_atomic_consume(od_atomic_u64_t *atomic)
{
	uint64_t value = od_atomic_u64_of(atomic);
	od_atomic_u64_sub(atomic, value);
	return value;
}

static inline float fast_log2(float val)
{
	int *const exp_ptr = (int *)(&val);
	int x = *exp_ptr;
	const int log_2 = ((x >> 23) & 255) - 128;
	x &= ~(255 << 23);
	x += 127 << 23;
	*exp_ptr = x;

	return (val + log_2);
}

int od_histogram_init(od_histogram_t *h, size_t size,
					  double range_min, double range_max)
{
	size_t i;
	uint64_t *tmp;

	/* Allocate memory for cumulative_array + temp_array + all slot arrays */
	tmp = (uint64_t *)calloc(size * (OD_HISTOGRAM_NSLOTS + 2), sizeof(uint64_t));
	h->interm_slots = (uint64_t **)malloc(OD_HISTOGRAM_NSLOTS *
										  sizeof(uint64_t *));

	if (tmp == NULL || h->interm_slots == NULL)
	{
		return 1;
	}

	h->cumulative_array = tmp;
	tmp += size;

	h->temp_array = tmp;
	tmp += size;

	for (i = 0; i < OD_HISTOGRAM_NSLOTS; i++)
	{
		h->interm_slots[i] = tmp;
		tmp += size;
	}

	h->range_deduct = fast_log2(range_min);
	h->range_mult = (size - 1) / (fast_log2(range_max) - h->range_deduct);

	h->range_min = range_min;
	h->range_max = range_max;

	h->array_size = size;
	h->cumulative_nevents = 0;

	pthread_rwlock_init(&h->lock, NULL);

	return 0;
}

void od_histogram_update(od_histogram_t *h, double value)
{
	size_t slot;
	ssize_t i;

	slot = machine_lrand48() % OD_HISTOGRAM_NSLOTS;

	i = floor((fast_log2(value) - h->range_deduct) * h->range_mult + 0.5);
	if (od_unlikely(i < 0))
		i = 0;
	else if (od_unlikely(i >= (ssize_t)(h->array_size)))
		i = h->array_size - 1;

	od_atomic_u64_inc(&h->interm_slots[slot][i]);
}

void od_histogram_get_pct_intermediate(od_histogram_t *h,
									   const double percentile[], double pct[], size_t count)
{
	size_t i, s;
	uint64_t nevents, ncur, nmax;

	nevents = 0;

	/*
	  This can be called concurrently with other od_histogram_get_pct_*()
	  functions, so use the lock to protect shared structures. This will not block
	  od_histogram_update() calls, but we make sure we don't lose any concurrent
	  increments by atomically fetching each array element and replacing it with
	  0.
	*/
	pthread_rwlock_wrlock(&h->lock);

	/*
	  Merge intermediate slots into temp_array.
	*/
	const size_t size = h->array_size;
	uint64_t *const array = h->temp_array;

	memset(array, 0, sizeof(uint64_t) * size);

	if (h->interm_slots)
	{
		for (s = 0; s < OD_HISTOGRAM_NSLOTS; s++)
		{
			for (i = 0; i < size; i++)
			{
				uint64_t t = od_atomic_consume(&h->interm_slots[s][i]);
				array[i] += t;
				nevents += t;
			}
		}
	}

	/*
	  Now that we have an aggregate 'snapshot' of current arrays and the total
	  number of events in it, calculate the current, intermediate percentile value
	  to return.
	*/
	for (s = 0; s < count; ++s)
	{
		nmax = floor(nevents * percentile[s] + 0.5);

		ncur = 0;
		for (i = 0; i < size; i++)
		{
			ncur += array[i];
			if (ncur >= nmax)
				break;
		}

		pct[i] = exp2(i / h->range_mult + h->range_deduct);
	}

	/* Finally, add temp_array into accumulated values in cumulative_array. */
	for (i = 0; i < size; i++)
	{
		h->cumulative_array[i] += array[i];
	}

	h->cumulative_nevents += nevents;

	pthread_rwlock_unlock(&h->lock);
}

/*
  Aggregate arrays from intermediate slots into cumulative_array. This should be
  called with the histogram lock write-locked.
*/
static void merge_intermediate_into_cumulative(od_histogram_t *h)
{
	size_t i, s;
	uint64_t nevents;

	nevents = h->cumulative_nevents;

	const size_t size = h->array_size;
	uint64_t *const array = h->cumulative_array;

	for (s = 0; s < OD_HISTOGRAM_NSLOTS; s++)
	{
		for (i = 0; i < size; i++)
		{
			uint64_t t = od_atomic_consume(&h->interm_slots[s][i]);
			array[i] += t;
			nevents += t;
		}
	}

	h->cumulative_nevents = nevents;
}

/*
  Calculate a given percentile from the cumulative array. This should be called
  with the histogram lock either read- or write-locked.
*/
static double get_pct_cumulative(od_histogram_t *h, double percentile)
{
	size_t i;
	uint64_t ncur, nmax;

	nmax = floor(h->cumulative_nevents * percentile + 0.5);

	ncur = 0;
	for (i = 0; i < h->array_size; i++)
	{
		ncur += h->cumulative_array[i];
		if (ncur >= nmax)
			break;
	}

	return exp2(i / h->range_mult + h->range_deduct);
}

void od_histogram_get_pct_cumulative(od_histogram_t *h,
									 const double percentile[], double pct[], size_t count)
{
	/*
	  This can be called concurrently with other od_histogram_get_pct_*()
	  functions, so use the lock to protect shared structures. This will not block
	  od_histogram_update() calls, but we make sure we don't lose any concurrent
	  increments by atomically fetching each array element and replacing it with
	  0.
	*/
	pthread_rwlock_wrlock(&h->lock);

	if (h->interm_slots)
		merge_intermediate_into_cumulative(h);

	for (size_t i=0; i<count; ++i)
	{
		pct[i] = get_pct_cumulative(h, percentile[i]);
	}

	pthread_rwlock_unlock(&h->lock);
}

double od_histogram_get_pct_checkpoint(od_histogram_t *h,
									   double percentile)
{
	double res;

	/*
	  This can be called concurrently with other od_histogram_get_pct_*()
	  functions, so use the lock to protect shared structures. This will not block
	  od_histogram_update() calls, but we make sure we don't lose any concurrent
	  increments by atomically fetching each array element and replacing it with
	  0.
	*/
	pthread_rwlock_wrlock(&h->lock);

	merge_intermediate_into_cumulative(h);

	res = get_pct_cumulative(h, percentile);

	/* Reset the cumulative array */
	memset(h->cumulative_array, 0, h->array_size * sizeof(uint64_t));
	h->cumulative_nevents = 0;

	pthread_rwlock_unlock(&h->lock);

	return res;
}

void od_histogram_print(od_histogram_t *h)
{
	uint64_t maxcnt;
	int width;
	size_t i;

	pthread_rwlock_wrlock(&h->lock);

	merge_intermediate_into_cumulative(h);

	uint64_t *const array = h->cumulative_array;

	maxcnt = 0;
	for (i = 0; i < h->array_size; i++)
	{
		if (array[i] > maxcnt)
			maxcnt = array[i];
	}

	if (maxcnt == 0)
		return;

	printf("       value  ------------- distribution ------------- count\n");

	for (i = 0; i < h->array_size; i++)
	{
		if (array[i] == 0)
			continue;

		width = floor(array[i] * (double)40 / maxcnt + 0.5);

		printf("%12.3f |%-40.*s %lu\n",
			   exp2(i / h->range_mult + h->range_deduct),		  /* value */
			   width, "****************************************", /* distribution */
			   (unsigned long)array[i]);						  /* count */
	}

	pthread_rwlock_unlock(&h->lock);
}

void od_histogram_done(od_histogram_t *h)
{
	pthread_rwlock_destroy(&h->lock);

	free(h->cumulative_array);
	if (h->interm_slots)
		free(h->interm_slots);
}

/*
  Allocate a new histogram and initialize it with od_histogram_init().
*/

od_histogram_t *od_histogram_new(size_t size, double range_min,
								 double range_max)
{
	od_histogram_t *h;

	if ((h = malloc(sizeof(*h))) == NULL)
		return NULL;

	if (od_histogram_init(h, size, range_min, range_max))
	{
		free(h);
		return NULL;
	}

	return h;
}

od_histogram_t *od_histogram_merge(od_histogram_t *a, od_histogram_t *b)
{
	if (a == NULL && b)
	{
		if ((a = malloc(sizeof(od_histogram_t))) == NULL)
			return NULL;
		a->array_size = b->array_size;
		a->cumulative_nevents = 0;
		a->range_max = b->range_max;
		a->range_min = b->range_min;
		a->range_deduct = b->range_deduct;
		a->range_mult = b->range_mult;
		pthread_rwlock_init(&a->lock, NULL);
		a->cumulative_array = (uint64_t *)calloc(a->array_size, sizeof(uint64_t));
		a->temp_array = NULL;
		a->interm_slots = NULL;
	}

	if (b)
	{
		assert(a->array_size == b->array_size &&
			   a->range_min == b->range_min &&
			   a->range_max == b->range_max);
		pthread_rwlock_wrlock(&a->lock);

		for (size_t i = 0; i < b->array_size; ++i)
		{
			a->cumulative_array[i] += b->cumulative_array[i];
		}
		a->cumulative_nevents += b->cumulative_nevents;
		pthread_rwlock_unlock(&a->lock);
	}

	return a;
}

/*
  Deallocate a histogram allocated with od_histogram_new().
*/

void od_histogram_delete(od_histogram_t *h)
{
	if (h)
	{
		od_histogram_done(h);
		free(h);
	}
}
