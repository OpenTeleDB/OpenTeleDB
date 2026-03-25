//
// Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
// Created by 王早 on 2024/11/22.
//

#ifndef ODYSSEY_RW_SELECT_H
#define ODYSSEY_RW_SELECT_H

extern bool is_read_only_sql(const char *sql);
extern bool is_in_blacklist(od_hashmap_t *blacklist, const char *data, uint32_t size);
extern char *add_to_blacklist(od_hashmap_t *blacklist, const char *data, uint32_t size);
extern void delete_from_blacklist(od_hashmap_t *blacklist, const char *data, uint32_t size);

#endif //ODYSSEY_RW_SELECT_H
