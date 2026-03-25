//
// Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
// Created by 王早 on 2024/11/22.
//

#include <odyssey.h>
#include <regex.h>

#define MAX_SQL_LENGTH 1024
#define MAX_MATCHES 13

bool contains_readonly_in_comments(const char *str) {
	const char *start_comment = "/*+";
	const char *end_comment = "*/";
	const char *readonly = "READ-ONLY";

	const char *start;
	while ((start = strstr(str, start_comment)) != NULL) {
		/* if hint string is not a comment, break */
		size_t prev_hint_length = start - str;
		char prev_hint[prev_hint_length];
		if (prev_hint_length > 0)
		{
			size_t single_quote_cnt = 0;
			size_t double_quote_cnt = 0;
			strncpy(prev_hint, str, prev_hint_length);
			for (size_t i = 0; i < prev_hint_length; i++)
			{
				if (prev_hint[i] == '\'')
					single_quote_cnt++;
				else if (prev_hint[i] == '\"')
					double_quote_cnt++;
			}

			if ((single_quote_cnt % 2 == 1) || (double_quote_cnt % 2 == 1)) 
				break;
		}

		const char *end = strstr(start, end_comment);
		if (end == NULL)
			break;

		size_t comment_length = end - (start + 3);
		char comment[comment_length + 1];
		strncpy(comment, start + 3, comment_length);
		comment[comment_length] = '\0';

		if (strcasestr(comment, readonly) != NULL) {
			return true;
		}

		str = end + 2;
	}

	return false;
}

/* 关键字匹配判断 sql 是否只读 */
bool is_read_only_sql(const char *sql) {
	if (contains_readonly_in_comments(sql)) {
		return true; // 只读
	}

	if (strcasestr(sql, "SELECT") == NULL) {
		return false;
	}

	/* 检查是否包含非只读操作关键字 */
	if (strcasestr(sql, "INSERT") ||
		strcasestr(sql, "UPDATE") ||
		strcasestr(sql, "DELETE") ||
		strcasestr(sql, "ALTER") ||
		strcasestr(sql, "CREATE") ||
		strcasestr(sql, "DROP") ||
		strcasestr(sql, "MERGE") ||
		strcasestr(sql, "TRUNCATE") ||
		strcasestr(sql, "COPY") ||
		strcasestr(sql, "BEGIN") ||
		strcasestr(sql, "COMMIT") ||
		strcasestr(sql, "ROLLBACK") ||
		strcasestr(sql, "CLUSTER")) {
		return false; // 非只读
	}
	return true; // 只读
}

/**
 * check if the digit is a constant value or not.
 */
static bool is_constant(const char *sql, int i) {
	char prev_char = i > 0 ? sql[i - 1] : '\0';

	if (i == 0 ||
		prev_char == '=' || prev_char == '<' || prev_char == '>' || prev_char == '!' ||
		prev_char == ',' || prev_char == '(' || prev_char == ')' || prev_char == ';' ||
		isspace(sql[i - 1])) {
		return true;
	}
	return false;
}

/**
 * check if the string is a boolean constant value or not.
 */
static bool is_boolean_constant(const char *sql, int i) {
	if (!is_constant(sql, i))
		return false;
	
	if ((strncasecmp(sql + i, "true", 4) == 0 && is_constant(sql, i+5)) || 
		(strncasecmp(sql + i, "false", 5) == 0 && is_constant(sql, i+6))) 
		return true;
	
	return false;
}

/**
 * create a generalized SQL statement by replacing sensitive values with '*'
 *	- compare operations (=, <>, !=, >=, <=, >, <)
 *	- LIKE/ILIKE operations
 *	- BETWEEN AND operations
 *	- IN operations
 *	- LIMIT operation
 */
static size_t generalize_sql(const char *sql, char *output, size_t size) {
	size_t i = 0, j = 0;

	while (i < size) {
		if (isspace(sql[i])) {
			output[j++] = sql[i++];
			continue;
		}

		if (isdigit(sql[i])) {  // replace digit constant with '*'
			if (is_constant(sql, i)) {
				while (i < size && isdigit(sql[i])) {
					i++;
				}
				output[j++] = '*';
			} else {
				output[j++] = sql[i++];
			}
		} else if (sql[i] == '\'' || sql[i] == '\"') {  // replace string constant with '*'
			char quote = sql[i++];

			while (i < size && sql[i] != quote) {
				i++;
			}

			if (i < size) {
				output[j++] = sql[i++];
			}
			output[j-1] = '*';
		} else if (is_boolean_constant(sql, i)) {
			if (sql[i] == 't' || sql[i] == 'T')
				i += 4;
			else
				i += 5;

			output[j++] = '*';
		} else {
			output[j++] = sql[i++];
		}
	}

	output[j] = '\0';
	return j;
}

/*
 * 把误判发给备机的 sql 缓存到 hashmap，下次遇到同样的 sql 不再发给备机
 */

bool is_in_blacklist(od_hashmap_t *blacklist, const char *data, size_t size) {
	od_hashmap_elt_t key;
	size_t normalized_sql_size = 0;
	char *normalized_sql = NULL;

	if (data == NULL || size == 0 || size > MAX_SQL_LENGTH) 
		return false;

	/* normalize SQL, and if it's too long or not a valid SQL, return false */
	normalized_sql = (char *)malloc(sizeof(char) * size + 1);
	normalized_sql_size = generalize_sql(data, normalized_sql, size);
	if (normalized_sql[0] == '\0')
		return false;

	key.data = normalized_sql;
	key.len = normalized_sql_size;
	od_hash_t hash = od_murmur_hash(key.data, key.len);
	od_hashmap_elt_t *ret = od_hashmap_find(blacklist, hash, &key);
	free(normalized_sql);
	if (ret == NULL)
		return false;
	else
		return true;
}

char *add_to_blacklist(od_hashmap_t *blacklist, const char *data, size_t size) {
	od_hashmap_elt_t key;
	size_t normalized_sql_size = 0;
	char *normalized_sql = NULL;

	if (data == NULL || size == 0 || size > MAX_SQL_LENGTH) 
		return NULL;

	/* normalize SQL, and if it's too long or not a valid SQL, return false */
	normalized_sql = (char *)malloc(sizeof(char) * size + 1);
	normalized_sql_size = generalize_sql(data, normalized_sql, size);
	if (normalized_sql[0] == '\0')
		return NULL;

	key.data = normalized_sql;
	key.len = normalized_sql_size;
	od_hash_t hash = od_murmur_hash(key.data, key.len);
	od_hashmap_elt_t value = {.data = "none", .len = 5};
	od_hashmap_elt_t *value_ptr = &value;
	if (od_hashmap_insert(blacklist, hash, &key, &value_ptr, false) == 0) {
		/* for printf debug log */
		return normalized_sql;
	}

	return NULL;
}

void delete_from_blacklist(od_hashmap_t *blacklist, const char *data, uint32_t size) {
	od_hashmap_elt_t key;
	char normalized_sql[size + 1];
	normalized_sql[0] = '\0';

	/* normalize SQL, and if it's too long or not a valid SQL, return false */
	generalize_sql(data, normalized_sql, size);
	if (size > MAX_SQL_LENGTH || normalized_sql[0] == '\0')
		return ;

	key.data = normalized_sql;
	key.len = strlen(normalized_sql);
	od_hash_t hash = od_murmur_hash(key.data, key.len);
	if (!od_hashmap_delete(blacklist, hash, &key)) {
		;
	}
}
