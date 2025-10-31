#ifndef ODYSSEY_BACKEND_H
#define ODYSSEY_BACKEND_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

int od_backend_connect(od_server_t *, char *, kiwi_params_t *, od_client_t *);
int od_backend_connect_index(od_server_t *, char *, kiwi_params_t *, od_client_t *, size_t, uint32_t);
int od_backend_connect_timeout(od_server_t *, char *, kiwi_params_t *, od_client_t *,  uint32_t);
int od_backend_connect_cancel(od_server_t *, od_rule_storage_t *, kiwi_key_t *);

void od_backend_close_connection(od_server_t *);
void od_backend_close(od_server_t *);
int od_backend_error(od_server_t *, char *, char *, uint32_t);

int od_backend_update_parameter(od_server_t *, char *, char *, uint32_t, int);
int od_backend_ready(od_server_t *, char *, uint32_t);
int od_backend_ready_wait(od_server_t *, char *, int, uint32_t, uint32_t);

od_retcode_t od_backend_query_send(od_server_t *server, char *context,
				   char *query, char *param, int len);
od_retcode_t od_backend_query_send_timeout(od_server_t *server, char *context,
				   char *query, char *param, int len, uint32_t timeout);
od_retcode_t od_backend_query(od_server_t *, char *, char *, char *, int,
			      uint32_t, uint32_t, uint32_t);

od_frontend_status_t od_backend_notice(od_server_t *, char *, int, bool);
int od_storage_parse_rw_check_response(machine_msg_t *msg);

#endif /* ODYSSEY_BACKEND_H */
