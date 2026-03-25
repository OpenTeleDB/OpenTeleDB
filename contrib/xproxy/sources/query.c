
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

machine_msg_t *od_query_do(od_server_t *server, char *context, char *query,
			   char *param, bool *failed)
{
	od_instance_t *instance = server->global->instance;
	od_debug(&instance->logger, context, server->client, server, "%s",
		 query);

	if (od_backend_query_send(server, context, query, param,
				  strlen(query) + 1) == NOT_OK_RESPONSE) {
		return NULL;
	}
	machine_msg_t *ret_msg = NULL;
	machine_msg_t *msg;

	/* wait for response */
	int has_result = 0;
	for (;;) {
		msg = od_read(&server->io, OD_QUERY_TIMEOUT);
		if (msg == NULL) {
			if (!machine_timedout()) {
				od_error(&instance->logger, context,
					 server->client, server,
					 "read error: %s",
					 od_io_error(&server->io));
			}
			od_error(&instance->logger, context,
					 server->client, server,
					 "read timeout: %s",
					 od_io_error(&server->io));
			/* 由于等待服务端返回超时，可能有忽略 READY_FOR_QUERY msg，进而导致 od_server_synchronized 校验不通过。故释放当前 server */
			server->offline = 1;
			return NULL;
		}

		int save_msg = 0;
		kiwi_be_type_t type;
		type = *(char *)machine_msg_data(msg);

		od_debug(&instance->logger, context, server->client, server,
			 "%s", kiwi_be_type_to_string(type));

		switch (type) {
		case KIWI_BE_ERROR_RESPONSE:
			od_backend_error(server, context, machine_msg_data(msg),
					 machine_msg_size(msg));
			if (failed)
				*failed = true;
			goto error;
		case KIWI_BE_ROW_DESCRIPTION:
			break;
		case KIWI_BE_DATA_ROW: {
			if (has_result) {
				goto error;
			}

			ret_msg = msg;
			has_result = 1;
			save_msg = 1;
			break;
		}
		case KIWI_BE_READY_FOR_QUERY:
			od_backend_ready(server, machine_msg_data(msg),
					 machine_msg_size(msg));

			machine_msg_free(msg);
			return ret_msg;
		default:
			break;
		}

		if (!save_msg) {
			machine_msg_free(msg);
		}
	}
	return ret_msg;
error:
	machine_msg_free(msg);
	return NULL;
}

/*
* Cases when a query returns multiple rows.
* num_rows indicates the number of rows the caller expects.
* However, the actual number of rows returned may be less than num_rows.
* The return array is initialized with NULLs, by this way the caller can check
* the actual number of rows returned.
*/
machine_msg_t **od_query_multiple_return(od_server_t *server, char *context,
					char *query, char *param, int expected_num_rows, int *actual_num_rows, uint32_t timeout)
{
	od_instance_t *instance = server->global->instance;
	od_debug(&instance->logger, context, server->client, server, "%s",
		 query);

	if (od_backend_query_send_timeout(server, context, query, param,
				  strlen(query) + 1, timeout) == NOT_OK_RESPONSE) {
		return NULL;
	}

	machine_msg_t **ret_msgs = NULL;
	machine_msg_t *msg;
	int row_received = 0;

	/* wait for response */
	ret_msgs = (machine_msg_t **)malloc(expected_num_rows * sizeof(machine_msg_t *));
	if (ret_msgs == NULL) {
		return NULL;
	}
	for (int i = 0; i < expected_num_rows; i++) {
		ret_msgs[i] = NULL;
	}

	for (;;) {
		msg = od_read(&server->io, UINT32_MAX);
		if (msg == NULL) {
			if (!machine_timedout()) {
				od_error(&instance->logger, context,
					 server->client, server,
					 "read error: %s",
					 od_io_error(&server->io));
			}
			od_error(&instance->logger, context,
					 server->client, server,
					 "read timeout: %s",
					 od_io_error(&server->io));
					 
			/* 由于等待服务端返回超时，可能有忽略 READY_FOR_QUERY msg，进而导致 od_server_synchronized 校验不通过。故释放当前 server */
			server->offline = 1;
			return NULL;
		}

		kiwi_be_type_t type;
		type = *(char *)machine_msg_data(msg);

		od_debug(&instance->logger, context, server->client, server,
			 "%s", kiwi_be_type_to_string(type));

		switch (type) {
		case KIWI_BE_ERROR_RESPONSE:
			od_backend_error(server, context, machine_msg_data(msg),
					 machine_msg_size(msg));
			goto error;
		case KIWI_BE_ROW_DESCRIPTION:
			break;
		case KIWI_BE_DATA_ROW: {
			if (row_received >= expected_num_rows) {
				ret_msgs = realloc(ret_msgs, (row_received + 1) * sizeof(machine_msg_t *));
				if (ret_msgs == NULL) 
					goto error;
				memset(ret_msgs + row_received, 0, sizeof(machine_msg_t *));
			}
			machine_msg_t *msg_copy = machine_msg_create(machine_msg_size(msg));
			if (msg_copy == NULL) {
				goto error;
			}
			memcpy(machine_msg_data(msg_copy), machine_msg_data(msg), machine_msg_size(msg));
			ret_msgs[row_received++] = msg_copy;
			*actual_num_rows = row_received;
			break;
		}
		case KIWI_BE_READY_FOR_QUERY:
			od_backend_ready(server, machine_msg_data(msg),
					 machine_msg_size(msg));

			machine_msg_free(msg);
			return ret_msgs;
		default:
			break;
		}

		machine_msg_free(msg);
	}
	return ret_msgs;
error:
	machine_msg_free(msg);
	for (int i = 0; i < row_received; i++) {
			machine_msg_free(ret_msgs[i]);
	}
	free(ret_msgs);
	return NULL;
}


/*
* 	Layout of Message RowDescription is:
* 'T 1Byte'
* 'Length Int32'
* 'FieldCount Int16'
* and then descriptions for each field
*/
int od_query_parse_row_description(machine_msg_t *msg, row_description_t *row_desc)
{
    char *pos = (char *)machine_msg_data(msg) + 1;
    uint32_t pos_size = machine_msg_size(msg) - 1;

    int rc;

    uint32_t msg_length;
    rc = kiwi_read32(&msg_length, &pos, &pos_size);
    if (kiwi_unlikely(rc == -1))
        goto error;

    uint16_t field_count;
    rc = kiwi_read16(&field_count, &pos, &pos_size);
    if (kiwi_unlikely(rc == -1))
        goto error;

    row_desc->num_fields = field_count;
    row_desc->fields = (field_description_t *)malloc(
		sizeof(field_description_t) * field_count);
    if (!row_desc->fields)
        goto error;

    for (uint16_t i = 0; i < field_count; i++) {
        char *field_name = pos;
        size_t name_len = strnlen(field_name, pos_size);
        if (name_len >= pos_size)
            goto error;
        row_desc->fields[i].field_name = strdup(field_name);
        pos += name_len + 1;
        pos_size -= name_len + 1;

        rc = kiwi_read32(&row_desc->fields[i].table_oid, &pos, &pos_size);
        if (kiwi_unlikely(rc == -1))
            goto error;

        rc = kiwi_read16(&row_desc->fields[i].attribute_num, &pos, &pos_size);
        if (kiwi_unlikely(rc == -1))
            goto error;

        rc = kiwi_read32(&row_desc->fields[i].data_type_oid, &pos, &pos_size);
        if (kiwi_unlikely(rc == -1))
            goto error;

        rc = kiwi_read16(&row_desc->fields[i].data_type_size, &pos, &pos_size);
        if (kiwi_unlikely(rc == -1))
            goto error;

        rc = kiwi_read32(&row_desc->fields[i].type_modifier, &pos, &pos_size);
        if (kiwi_unlikely(rc == -1))
            goto error;

        rc = kiwi_read16(&row_desc->fields[i].format_code, &pos, &pos_size);
        if (kiwi_unlikely(rc == -1))
            goto error;
    }

    return OK_RESPONSE;

error:
    if (row_desc->fields) {
        for (uint16_t i = 0; i < row_desc->num_fields; i++) {
            free(row_desc->fields[i].field_name);
        }
        free(row_desc->fields);
    }
    return NOT_OK_RESPONSE;
}

__attribute__((hot)) int od_query_format(char *format_pos, char *format_end,
					 kiwi_var_t *user, char *peer,
					 char *output, int output_len)
{
	char *dst_pos = output;
	char *dst_end = output + output_len;
	while (format_pos < format_end) {
		if (*format_pos == '%') {
			format_pos++;
			if (od_unlikely(format_pos == format_end))
				break;
			int len;
			switch (*format_pos) {
			case 'u':
				len = od_snprintf(dst_pos, dst_end - dst_pos,
						  "%s", user->value);
				dst_pos += len;
				break;
			case 'h':
				len = od_snprintf(dst_pos, dst_end - dst_pos,
						  "%s", peer);
				dst_pos += len;
				break;
			default:
				if (od_unlikely((dst_end - dst_pos) < 2))
					break;
				dst_pos[0] = '%';
				dst_pos[1] = *format_pos;
				dst_pos += 2;
				break;
			}
		} else {
			if (od_unlikely((dst_end - dst_pos) < 1))
				break;
			dst_pos[0] = *format_pos;
			dst_pos += 1;
		}
		format_pos++;
	}
	if (od_unlikely((dst_end - dst_pos) < 1))
		return -1;
	dst_pos[0] = 0;
	dst_pos++;
	return dst_pos - output;
}
