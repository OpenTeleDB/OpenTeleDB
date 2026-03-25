#ifndef OD_HISTOGRAM_H
#define OD_HISTOGRAM_H
#include <pthread.h>

typedef struct od_histogram_t
{
	double range_deduct;
	double range_mult;
	double range_min;
	double range_max;
	size_t array_size;
	size_t cumulative_nevents;
	uint64_t **interm_slots;
	uint64_t *temp_array;
	uint64_t *cumulative_array;
	pthread_rwlock_t lock;
} od_histogram_t;

od_histogram_t *od_histogram_new(size_t size, double range_min,
								 double range_max);

od_histogram_t *od_histogram_merge(od_histogram_t *a, od_histogram_t *b);

void od_histogram_delete(od_histogram_t *h);

void od_histogram_update(od_histogram_t *h, double value);

void od_histogram_get_pct_intermediate(od_histogram_t *h,
									   const double percentile[], double pct[], size_t size);

void od_histogram_get_pct_cumulative(od_histogram_t *h,
									   const double percentile[], double pct[], size_t size);

double od_histogram_get_pct_checkpoint(od_histogram_t *h,
									   double percentile);

#endif