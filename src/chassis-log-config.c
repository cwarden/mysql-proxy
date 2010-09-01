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

#define TARGETS_GROUP "targets"
#define DEFAULT_LOGGER "default"
#define LEVEL_KEY "level"
#define TARGET_KEY "target"

static GLogLevelFlags log_level_string_to_level(const gchar *level_str) {
	GQuark level_quark = g_quark_from_string(level_str);
#define STATIC_QUARK(x) static GQuark x ## _quark = 0
#define INIT_QUARK(x) if (0 == x ## _quark) { x ## _quark = g_quark_from_static_string(#x); }
	STATIC_QUARK(error);
	STATIC_QUARK(critical);
	STATIC_QUARK(warning);
	STATIC_QUARK(message);
	STATIC_QUARK(info);
	STATIC_QUARK(debug);
	INIT_QUARK(error)
	INIT_QUARK(critical)
	INIT_QUARK(warning)
	INIT_QUARK(message)
	INIT_QUARK(info)
	INIT_QUARK(debug)
#undef STATIC_QUARK
#undef INIT_QUARK
	
	if (level_quark == error_quark) { return G_LOG_LEVEL_ERROR; }
	else if (level_quark == critical_quark) { return G_LOG_LEVEL_CRITICAL; }
	else if (level_quark == warning_quark) { return G_LOG_LEVEL_WARNING; }
	else if (level_quark == message_quark) { return G_LOG_LEVEL_MESSAGE; }
	else if (level_quark == info_quark) { return G_LOG_LEVEL_INFO; }
	else if (level_quark == debug_quark) { return G_LOG_LEVEL_DEBUG; }
	return 0;
}

gboolean chassis_log_load_config(chassis_log_t *log, const gchar *file_name, GError **gerr) {
	GKeyFile *config = g_key_file_new();
	gchar **keys, **groups;
	gsize keys_count, groups_count;
	guint i = 0;
	GHashTable *targets = g_hash_table_new(g_str_hash, g_str_equal);  /* target name -> target */
	gchar *default_log_level_str;
	GLogLevelFlags default_log_level;
	gchar *default_target_name;
	chassis_log_domain_t *default_logger;
	chassis_log_backend_t *default_target;
	gboolean ret = FALSE;

	g_assert(log);
	g_assert(file_name);
	if (FALSE == g_key_file_load_from_file(config, file_name, G_KEY_FILE_NONE, gerr)) {
		goto error_cleanup;
	}

	if (!g_key_file_has_group(config, TARGETS_GROUP)) {
		/* FIXME: complain about missing targets */
		goto error_cleanup;
	}

	if (!g_key_file_has_group(config, DEFAULT_LOGGER)) {
		/* FIXME: should we take it from the global config for compatibility/convenience reasons? */
		goto error_cleanup;
	}

	/* collect all the targets and register them */
	keys = g_key_file_get_keys(config, TARGETS_GROUP, &keys_count, NULL);
	if (keys_count == 0) {
		/* FIXME: complain about missing targets */
		goto error_cleanup;
	}

	/* register all targets we've found */
	for (i = 0; i < keys_count; i++) {
		chassis_log_backend_t *target;
		gchar *target_file = g_key_file_get_string(config, TARGETS_GROUP, keys[i], NULL);

		target = chassis_log_backend_new(target_file);
		g_hash_table_insert(targets, keys[i], target);
		chassis_log_register_backend(log, target);

		g_free(target_file);
	}

	default_log_level_str = g_key_file_get_string(config, DEFAULT_LOGGER, LEVEL_KEY, NULL);
	default_log_level = log_level_string_to_level(default_log_level_str);
	if (default_log_level == 0) {
		/* FIXME: complain about unknown log level, don't just default to critical */
		default_log_level = G_LOG_LEVEL_CRITICAL;
	}
	default_target_name = g_key_file_get_string(config, DEFAULT_LOGGER, TARGET_KEY, NULL);
	default_target = g_hash_table_lookup(targets, default_target_name);
	default_logger = chassis_log_domain_new("", default_log_level, default_target);
	chassis_log_register_domain(log, default_logger);

	g_free(default_log_level_str);


	/* register all loggers defined in the config file */
	groups = g_key_file_get_groups(config, &groups_count);
	if (groups_count > 2) {	/* 2 because we already confirmed that we have "targets" and "default" groups */
		guint group_idx;
		for (group_idx = 0; group_idx < groups_count; group_idx++) {
			gchar *logger_name = groups[group_idx]; /* the group's name is the logger name */
			gchar *level_str;
			GLogLevelFlags level;
			gchar *target_name;
			chassis_log_domain_t *logger;
			chassis_log_backend_t *target;

			/* skip the two special groups in the file */
			if (g_str_equal(TARGETS_GROUP, groups[group_idx]) || g_str_equal(DEFAULT_LOGGER, groups[group_idx])) {
				continue;
			}
			level_str = g_key_file_get_string(config, logger_name, LEVEL_KEY, NULL);
			level = log_level_string_to_level(level_str);
			if (level == 0) {
				/* FIXME: complain about unknown log level, don't just default to critical */
				level = G_LOG_LEVEL_CRITICAL;
			}
			
			target_name = g_key_file_get_string(config, logger_name, TARGET_KEY, NULL);
			if (target_name) {
				target = g_hash_table_lookup(targets, target_name);
				if (!target) {
					/* FIXME: complain about using unknown target, don't just fall back to the root target */
					target = default_target;
				}
				logger = chassis_log_domain_new(logger_name, level, target);
				chassis_log_register_domain(log, logger);
			}

			if (level_str) g_free(level_str);
			if (target_name) g_free(target_name);
		}
	}
	g_strfreev(groups);

	ret = TRUE; /* everything was fine, we read the config */

error_cleanup:
	g_strfreev(keys);
	g_hash_table_destroy(targets);
	g_key_file_free(config);

	return ret;
}

