
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

// from https://github.com/aappleby/smhasher/blob/master/src/MurmurHash1.cpp
//
#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
 
#if __has_attribute(fallthrough)
#define FALLTHROUGH __attribute__((fallthrough))
#else
#define FALLTHROUGH
#endif

/*
 * Copyright (C) Austin Appleby
 */
const od_hash_t seed = 0x5bd1e995;

od_hash_t od_murmur_hash(const void *raw, size_t len)
{
	const unsigned int m = 0xc6a4a793;

	const int r = 16;

	unsigned int h = seed ^ (len * m);
	unsigned int k;

	const unsigned char *data = (const unsigned char *)raw;
	char buf[4]; // raw may be misaligned
	memcpy(buf, data, len >= 4 ? 4 : len);

	while (len >= 4) {
		k = *(unsigned int *)buf;

		h += k;
		h *= m;
		h ^= h >> 16;

		data += 4;
		len -= 4;
		memcpy(buf, data, len >= 4 ? 4 : len);
	}

	//----------

	switch (len) {
	case 3:
		h += buf[2] << 16;
		FALLTHROUGH;
	case 2:
		h += buf[1] << 8;
		FALLTHROUGH;
	case 1:
		h += buf[0];
		h *= m;
		h ^= h >> r;
		break;
	};

	//----------

	h *= m;
	h ^= h >> 10;
	h *= m;
	h ^= h >> 17;

	return h;
}
