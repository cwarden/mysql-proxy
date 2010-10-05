/* $%BEGINLICENSE%$
 Copyright (C) 2009 Sun Microsystems, Inc

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 $%ENDLICENSE%$ */

#include <glib.h>
#include "chassis-exports.h"
#include "chassis-log.h"
#include "chassis_log_error.h"

#define BACKENDS_GROUP "backends"
#define DEFAULT_DOMAIN "default"
#define LEVEL_KEY "level"
#define BACKEND_KEY "backend"

gboolean chassis_log_load_config(chassis_log_t *log, const gchar *file_name, GError **gerr) {
	GKeyFile *config = g_key_file_new();
	gchar **keys = NULL, **groups;
	gsize keys_count, groups_count;
	guint i = 0;
	GHashTable *backends = g_hash_table_new(g_str_hash, g_str_equal);  /* backend name -> backend */
	gchar *default_log_level_str;
	GLogLevelFlags default_log_level;
	gchar *default_backend_name;
	chassis_log_domain_t *default_domain;
	chassis_log_backend_t *default_backend;
	gboolean ret = FALSE;

	g_assert(NULL != log);
	g_assert(NULL != file_name);

	if (FALSE == g_key_file_load_from_file(config, file_name, G_KEY_FILE_NONE, gerr)) {
		goto error_cleanup;
	}

	if (!g_key_file_has_group(config, BACKENDS_GROUP)) {
		g_set_error(gerr, chassis_log_error(), CHASSIS_LOG_ERROR_NO_GROUP,
				"%s has no group [%s]",
				file_name,
				BACKENDS_GROUP);
		goto error_cleanup;
	}

	if (!g_key_file_has_group(config, DEFAULT_DOMAIN)) {
		/* FIXME: should we take it from the global config for compatibility/convenience reasons? */
		g_set_error(gerr, chassis_log_error(), CHASSIS_LOG_ERROR_NO_GROUP,
				"%s has no group [%s]",
				file_name,
				DEFAULT_DOMAIN);
		goto error_cleanup;
	}

	/* collect all the backends and register them */
	keys = g_key_file_get_keys(config, BACKENDS_GROUP, &keys_count, NULL);
	if (keys_count == 0) {
		g_set_error(gerr, chassis_log_error(), CHASSIS_LOG_ERROR_NO_BACKENDS,
				"%s has no backends defined in group [%s]",
				file_name,
				BACKENDS_GROUP);
		goto error_cleanup;
	}

	/* register all backends we've found */
	for (i = 0; i < keys_count; i++) {
		chassis_log_backend_t *backend;
		gchar *backend_file = g_key_file_get_string(config, BACKENDS_GROUP, keys[i], NULL);

		backend = chassis_log_backend_file_new(backend_file);
		g_hash_table_insert(backends, keys[i], backend);
		chassis_log_register_backend(log, backend);

		g_free(backend_file);
	}

	default_log_level_str = g_key_file_get_string(config, DEFAULT_DOMAIN, LEVEL_KEY, NULL);
	if (NULL == default_log_level_str) {
		g_set_error(gerr, chassis_log_error(), CHASSIS_LOG_ERROR_NO_LOGLEVEL,
				"default backend needs a log-level set");
		goto error_cleanup;
	}

	default_log_level = chassis_log_level_string_to_level(default_log_level_str);
	if (default_log_level == 0) {
		g_set_error(gerr, chassis_log_error(), CHASSIS_LOG_ERROR_INVALID_LOGLEVEL,
				"%s is not a valid log-level",
				default_log_level_str);
		g_free(default_log_level_str);
		goto error_cleanup;
	}

	default_backend_name = g_key_file_get_string(config, DEFAULT_DOMAIN, BACKEND_KEY, NULL);
	if (NULL == default_backend_name) {
		g_set_error(gerr, chassis_log_error(), CHASSIS_LOG_ERROR_INVALID_LOGLEVEL,
				"[%s].%s has to be set",
				DEFAULT_DOMAIN,
				BACKEND_KEY);
		goto error_cleanup;
	}
	default_backend = g_hash_table_lookup(backends, default_backend_name);
	default_domain = chassis_log_domain_new(CHASSIS_LOG_DEFAULT_DOMAIN, default_log_level, default_backend);
	chassis_log_register_domain(log, default_domain);

	g_free(default_log_level_str);

	/* register all domains defined in the config file */
	groups = g_key_file_get_groups(config, &groups_count);
	if (groups_count > 2) {	/* 2 because we already confirmed that we have "backends" and "default" groups */
		guint group_idx;

		for (group_idx = 0; group_idx < groups_count; group_idx++) {
			gchar *domain_name = groups[group_idx]; /* the group's name is the domain name */
			gchar *level_str;
			GLogLevelFlags level;
			gchar *backend_name;
			chassis_log_domain_t *domain;
			chassis_log_backend_t *backend;

			/* skip the two special groups in the file */
			if (g_str_equal(BACKENDS_GROUP, groups[group_idx]) || g_str_equal(DEFAULT_DOMAIN, groups[group_idx])) {
				continue;
			}
			level_str = g_key_file_get_string(config, domain_name, LEVEL_KEY, NULL);
			level = chassis_log_level_string_to_level(level_str);
			if (level == 0) {
				g_set_error(gerr, chassis_log_error(), CHASSIS_LOG_ERROR_INVALID_LOGLEVEL,
					"%s is not a valid log-level",
					level_str);

				if (level_str) g_free(level_str);

				goto error_cleanup;
			}
			
			backend_name = g_key_file_get_string(config, domain_name, BACKEND_KEY, NULL);
			if (NULL != backend_name) {
				backend = g_hash_table_lookup(backends, backend_name);
				if (NULL == backend) {
					g_set_error(gerr, chassis_log_error(), CHASSIS_LOG_ERROR_UNKNOWN_BACKEND,
						"[%s].%s %s is not defined in [%s]",
						domain_name,
						BACKEND_KEY,
						backend_name,
						BACKENDS_GROUP);

					if (NULL != level_str) g_free(level_str);
					if (NULL != backend_name) g_free(backend_name);

					goto error_cleanup;
				}
				domain = chassis_log_domain_new(domain_name, level, backend);
				chassis_log_register_domain(log, domain);
			}

			if (NULL != level_str) g_free(level_str);
			if (NULL != backend_name) g_free(backend_name);
		}
	}
	g_strfreev(groups);

	ret = TRUE; /* everything was fine, we read the config */

error_cleanup:
	if (keys) g_strfreev(keys);
	g_hash_table_destroy(backends);
	g_key_file_free(config);

	return ret;
}

