#include <glib.h>

#include "chassis_log_domain.h"

/* domain functions */

chassis_log_domain_t* chassis_log_domain_new(const gchar *domain_name, GLogLevelFlags min_level, chassis_log_backend_t *target) {
	chassis_log_domain_t *domain;

	g_return_val_if_fail(domain_name, NULL);

	domain = g_slice_new0(chassis_log_domain_t);

	domain->name = g_strdup(domain_name);
	domain->min_level = min_level;
	domain->backend = target;
	domain->is_autocreated = FALSE;
	domain->parent = NULL;
	domain->children = g_ptr_array_new();

	return domain;
}

void chassis_log_domain_free(chassis_log_domain_t* domain) {
	if (NULL == domain) return;

	if (NULL != domain->name) g_free(domain->name);
	if (NULL != domain->children) g_ptr_array_free(domain->children, TRUE);

	g_slice_free(chassis_log_domain_t, domain);
}

void chassis_log_domain_log(chassis_log_domain_t* domain, GLogLevelFlags level, const gchar *message) {
	if (level != CHASSIS_LOG_LEVEL_BROADCAST &&  /* _BROADCAST is logged always */
	    domain->effective_level < level) {
		return;
	}
	chassis_log_backend_log(domain->backend, domain->name, level, message);
}


