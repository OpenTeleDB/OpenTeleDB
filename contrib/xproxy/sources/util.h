#ifndef ODYSSEY_UTIL_H
#define ODYSSEY_UTIL_H

#include"cJSON.h"
#include <unistd.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

static inline int od_vsnprintf(char *buf, int size, char *fmt, va_list args)
{
	int rc;
	rc = vsnprintf(buf, size, fmt, args);
	if (od_unlikely(rc >= size))
		rc = (size - 1);
	return rc;
}

static inline int od_vasprintf(char **__restrict bufp, char *fmt, va_list args)
{
	vasprintf(bufp, fmt, args);

	if (*bufp == NULL) {
		return NOT_OK_RESPONSE;
	}

	return OK_RESPONSE;
}

static inline int od_asprintf(char **__restrict bufp, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int rc = vasprintf(bufp, fmt, args);
	va_end(args);
	if (rc == -1) {
		return NOT_OK_RESPONSE;
	}

	if (*bufp == NULL) {
		return NOT_OK_RESPONSE;
	}

	return OK_RESPONSE;
}

static inline int od_snprintf(char *buf, int size, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int rc;
	rc = od_vsnprintf(buf, size, fmt, args);
	va_end(args);
	return rc;
}

static inline char *od_strdup_from_buf(const char *source, size_t size)
{
	char *str = malloc(size + 1);
	memcpy(str, source, size);
	str[size] = '\0';
	return str;
}

static inline long od_memtol(char *data, size_t data_size, char **end_ptr,
			     int base)
{
	// Only 10 is supported
	if (base != 10)
		abort();

	size_t i = 0;
	while (i < data_size && isspace(data[i]))
		i++;
	if (i >= data_size)
		return 0;

	char sign = data[i];
	if (sign == '-' || sign == '+')
		i++;
	if (i >= data_size || !isdigit(data[i]))
		return 0;

	long result = 0;
	while (i < data_size && isdigit(data[i])) {
		result = result * 10 + (data[i] - '0');
		i++;
	}

	if (i < data_size && !isspace(data[i]))
		return 0;

	if (end_ptr)
		*end_ptr = data + i;

	if (sign == '-')
		return -result;
	return result;
}

static inline uint32 od_bswap32(uint32 x)
{
	return ((x << 24) & 0xff000000) | ((x << 8) & 0x00ff0000) |
	       ((x >> 8) & 0x0000ff00) | ((x >> 24) & 0x000000ff);
}

static inline long od_stringtol(const char *str, size_t size, int *error) {
    long num = 0;
    int sign = 1;
    size_t i = 0;

    if (error != NULL) {
        *error = 1; 
    }

    if (str == NULL || size == 0) {
        return 0; 
    }

    if (str[0] == '-') {
        sign = -1;
        i++;
    } else if (str[0] == '+') {
        i++;
    }

    for (; i < size; i++) {
        char c = str[i];
        if (c < '0' || c > '9') {
            return 0; 
        }
        num = num * 10 + (c - '0');
    }

    if (error != NULL) {
        *error = 0; 
    }

    return sign * num;
}

static inline int od_parse_arbitration_json(const char *json_string, char *host) {    
    cJSON *root = cJSON_Parse(json_string);
    if (root == NULL) {
        return NOT_OK_RESPONSE;
    }

    // int code = cJSON_GetObjectItemCaseSensitive(root, "code")->valueint;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const char *primary_host = cJSON_GetObjectItemCaseSensitive(data, "host")->valuestring;

	strcpy(host, primary_host);
    cJSON_Delete(root);
    return OK_RESPONSE;
}

static inline void od_extract_http_body(char *buffer) {
    char *body_start = strstr(buffer, "\r\n\r\n");
    if (body_start != NULL) {
        // Move body to the beginning of the buffer
        // body_start points to the first \r of \r\n\r\n
        // so the actual body starts 4 characters later
        body_start += 4; 
        memmove(buffer, body_start, strlen(body_start) + 1); // +1 for null terminator
    } else {
        // If no \r\n\r\n is found, it might be an invalid response
        // or a response without a body.
        // For now, let's clear the buffer to indicate no body or an error.
        // Alternatively, you could leave the buffer as is or handle error differently.
        buffer[0] = '\0'; 
    }
}

/* Extract a quoted string from ERROR 26000 prepared
 statement "xxx" does not exist.*/
static inline char* extract_quoted(const char* input) {
    const char* start = strchr(input, '"'); 
    if (!start) return NULL;

    const char* end = strchr(start + 1, '"'); 
    if (!end) return NULL;

    size_t length = end - start - 1; 
    char* result = (char*)malloc(length + 1); 
    if (!result) return NULL;

    memcpy(result, start + 1, length); 
    result[length] = '\0'; 

    return result;
}

#endif /* ODYSSEY_UTIL_H */
