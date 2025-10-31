#ifndef ODYSSEY_HBA_H
#define ODYSSEY_HBA_H

/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

typedef struct od_hba od_hba_t;

typedef struct {
	od_hba_rules_t rules;
	od_atomic_u32_t refcnt;
} od_hba_rules_ref_t;

struct od_hba {
	pthread_mutex_t lock;
	od_hba_rules_ref_t *rules_ref;
};

void od_hba_init(od_hba_t *hba);
void od_hba_free(od_hba_t *hba);
void od_hba_reload(od_hba_t *hba, od_hba_rules_t *rules);
int od_hba_process(od_client_t *client);

#endif // ODYSSEY_HBA_H
