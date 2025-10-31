#ifndef ODYSSEY_AUTH_QUERY_H
#define ODYSSEY_AUTH_QUERY_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 * 
 * Scalable PostgreSQL connection pooler.
 */

#define ODYSSEY_AUTH_QUERY_MAX_PASSSWORD_LEN 4096
/* 10 seconds, TODO: use macro for now */
#define ODYSSEY_AUTH_QUERY_TIMEOUT 10*1000 

int od_auth_query(od_client_t *, char *);

#endif /* ODYSSEY_AUTH_QUERY_H */
