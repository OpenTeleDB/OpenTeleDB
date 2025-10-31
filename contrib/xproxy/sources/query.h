#ifndef ODYSSEY_QUERY_H
#define ODYSSEY_QUERY_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

typedef struct {
    char *field_name;
    uint32_t table_oid;
    uint16_t attribute_num;
    uint32_t data_type_oid;
    uint16_t data_type_size;
    uint32_t type_modifier;
    uint16_t format_code;
} field_description_t;

typedef struct {
    uint16_t num_fields;
    field_description_t *fields;
} row_description_t;


// execute query with (optional) single string param
extern machine_msg_t *od_query_do(od_server_t *server, char *context,
				  char *query, char *param, bool *failed);

// execute query with (optional) single string param
extern machine_msg_t **od_query_multiple_return(od_server_t *server, char *context,
				  char *query, char *param, int expected_num_rows,  int *actual_num_rows, uint32_t timeout);

extern int od_query_parse_row_description(machine_msg_t *msg, 
				  row_description_t *row_desc);

__attribute__((hot)) extern int od_query_format(char *format_pos,
						char *format_end,
						kiwi_var_t *user, char *peer,
						char *output, int output_len);

/* 8 seconds, TODO: use macro for now，in od_query_do() */
# define OD_QUERY_TIMEOUT 8*1000

#endif /* ODYSSEY_QUERY_H */
