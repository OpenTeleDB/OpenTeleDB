#ifndef ODYSSEY_POSTGRES_H
#define ODYSSEY_POSTGRES_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

/*
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#define int8 int8_t
#define uint8 uint8_t
#define uint16 uint16_t
#define uint32 uint32_t
#define uint64 uint64_t

#define PGDLLIMPORT
#define Assert assert
#define _(x) (x)

#define lengthof(array) (sizeof(array) / sizeof((array)[0]))
#define pg_hton32(x) htobe32(x)

#define pg_attribute_noreturn() __attribute__((noreturn))

#define HIGHBIT (0x80)
#define IS_HIGHBIT_SET(ch) ((unsigned char)(ch) & HIGHBIT)

#define FRONTEND

// #include <pg_config.h>
#include <string.h>

#include "postgres/base64.h"
#include "postgres/saslprep.h"
#include "postgres/scram-common.h"
#include "postgres/hmac.h"

#endif /* ODYSSEY_POSTGRES_H */
