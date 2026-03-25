#ifndef ODYSSEY_CANCEL_H
#define ODYSSEY_CANCEL_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

int od_cancel(od_global_t *, od_rule_storage_t *, kiwi_key_t *, od_id_t *, size_t);

#endif /* ODYSSEY_CANCEL_H */
