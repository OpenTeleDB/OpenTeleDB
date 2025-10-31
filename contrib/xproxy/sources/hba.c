
/*
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <odyssey.h>

static void od_put_rules_ref(od_hba_rules_ref_t *rule_ref) {
	if (rule_ref) {
		if (od_atomic_u32_sub(&rule_ref->refcnt, 1) == 0) {
			od_hba_rules_free(&rule_ref->rules);
		}
	}
}

static od_hba_rules_ref_t*
od_get_rules_ref(od_hba_rules_ref_t *rule_ref) {
	if (rule_ref) {
		od_atomic_u32_add(&rule_ref->refcnt, 1);
	}

	return rule_ref;
}

void od_hba_init(od_hba_t *hba)
{
	pthread_mutex_init(&hba->lock, NULL);
	hba->rules_ref = NULL;
}

void od_hba_free(od_hba_t *hba)
{
	od_put_rules_ref(hba->rules_ref);
	pthread_mutex_destroy(&hba->lock);
}

void od_hba_lock(od_hba_t *hba)
{
	pthread_mutex_lock(&hba->lock);
}

void od_hba_unlock(od_hba_t *hba)
{
	pthread_mutex_unlock(&hba->lock);
}

void od_hba_reload(od_hba_t *hba, od_hba_rules_t *new_rules)
{
	od_hba_rules_ref_t *rules_ref;
	od_hba_lock(hba);

	od_put_rules_ref(hba->rules_ref);
	rules_ref = malloc(sizeof(od_hba_rules_ref_t));
	rules_ref->refcnt = 1;
	od_list_init(&rules_ref->rules);
	od_list_move(&rules_ref->rules, new_rules);
	hba->rules_ref = rules_ref;

	od_hba_unlock(hba);
}

bool od_hba_validate_name(char *client_name, od_hba_rule_name_t *name,
			  char *client_other_name)
{
	if (name->flags & OD_HBA_NAME_ALL) {
		return true;
	}

	if ((name->flags & OD_HBA_NAME_SAMEUSER) &&
	    strcmp(client_name, client_other_name) == 0) {
		return true;
	}

	od_list_t *i;
	od_hba_rule_name_item_t *item;
	od_list_foreach(&name->values, i)
	{
		item = od_container_of(i, od_hba_rule_name_item_t, link);
		if (item->value != NULL &&
		    strcmp(client_name, item->value) == 0) {
			return true;
		}
	}

	return false;
}

int od_hba_process(od_client_t *client)
{
	od_instance_t *instance = client->global->instance;
	od_hba_t *hba = client->global->hba;
	od_list_t *i;
	od_hba_rule_t *rule;
	od_hba_rules_t *rules;
	od_hba_rules_ref_t *rules_ref;

	if (instance->config.hba_file == NULL) {
		return OK_RESPONSE;
	}

	struct sockaddr_storage sa;
	int salen = sizeof(sa);
	struct sockaddr *saddr = (struct sockaddr *)&sa;
	int rc = machine_getpeername(client->io.io, saddr, &salen);
	if (rc == -1)
		return -1;

	od_hba_lock(hba);
	rules_ref = od_get_rules_ref(hba->rules_ref);
	od_hba_unlock(hba);

	if (rules_ref == NULL)
		return OK_RESPONSE;

	rules = &rules_ref->rules;

	od_list_foreach(rules, i)
	{
		rule = od_container_of(i, od_hba_rule_t, link);
		if (sa.ss_family == AF_UNIX) {
			if (rule->connection_type != OD_CONFIG_HBA_LOCAL)
				continue;
		} else if (rule->connection_type == OD_CONFIG_HBA_LOCAL) {
			continue;
		} else if (rule->connection_type == OD_CONFIG_HBA_HOSTSSL &&
			   !client->startup.is_ssl_request) {
			continue;
		} else if (rule->connection_type == OD_CONFIG_HBA_HOSTNOSSL &&
			   client->startup.is_ssl_request) {
			continue;
		} else if (sa.ss_family == AF_INET ||
			   sa.ss_family == AF_INET6) {
			if (!od_address_validate(&rule->address_range, &sa)) {
				continue;
			}
		}

		if (!od_hba_validate_name(client->startup.database.value,
					  &rule->database,
					  client->startup.user.value)) {
			continue;
		}
		if (!od_hba_validate_name(client->startup.user.value,
					  &rule->user,
					  client->startup.database.value)) {
			continue;
		}

		rc = rule->auth_method == OD_CONFIG_HBA_ALLOW ? OK_RESPONSE :
								NOT_OK_RESPONSE;

		od_put_rules_ref(rules_ref);
		return rc;
	}

	od_put_rules_ref(rules_ref);
	return NOT_OK_RESPONSE;
}
