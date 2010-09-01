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

#include "chassis-log-extended.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <string.h>
#ifndef WIN32
#include <unistd.h> /* close */
#else
#include <windows.h>
#include <io.h>
#endif
#include <errno.h>
#include "string-len.h"

/* forward decls */
static chassis_log_domain_t* chassis_log_extended_get_logger_raw(chassis_log_extended_t *log_ext, const gchar *logger_name);

/* log_extended functions */

chassis_log_extended_t* chassis_log_extended_new() {
	chassis_log_extended_t *log_ext =  g_slice_new0(chassis_log_extended_t);

	/* don't free the keys, they are part of the value for both hashes
	 * the individual loggers should _not_ free their target, this is taken care of here as the target might be in use somewhere else
	 */
	log_ext->domains = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)chassis_log_domain_free);
	log_ext->backends = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)chassis_log_backend_free);

	return log_ext;
}

void chassis_log_extended_free(chassis_log_extended_t* log_ext) {
	if (!log_ext) return;

	if (log_ext->domains) g_hash_table_destroy(log_ext->domains);
	if (log_ext->backends) g_hash_table_destroy(log_ext->backends);

	g_slice_free(chassis_log_extended_t, log_ext);
}

gboolean chassis_log_extended_register_backend(chassis_log_extended_t *log_ext, chassis_log_backend_t *target) {
	GHashTable *targets = log_ext->backends;
	chassis_log_backend_t *registered_target;
	
	/* check for a valid target */
	if (!target) return FALSE;
	if (!target->file_path) return FALSE;
	
	registered_target = g_hash_table_lookup(targets, target->file_path);

	/* don't allow registering a target twice */
	if (registered_target) return FALSE;

	g_hash_table_insert(targets, target->file_path, target);

	return TRUE;
}

static void chassis_log_domain_invalidate_hierarchy(gpointer data, gpointer G_GNUC_UNUSED user_data) {
	chassis_log_domain_t *logger = (chassis_log_domain_t*)data;
	
	/* don't touch explicit loggers - stop condition for the recursion */
	if (logger->is_implicit == FALSE) return;
	
	/* otherwise reset the target and effective level for this logger and recurse into the children */
	logger->effective_level = 0;
	logger->backend = NULL;

	g_ptr_array_foreach(logger->children, chassis_log_domain_invalidate_hierarchy, NULL);
}

gboolean chassis_log_extended_register_domain(chassis_log_extended_t *log_ext, chassis_log_domain_t *logger) {
	GHashTable *loggers;
	chassis_log_domain_t *existing_logger = NULL;

	if (NULL == log_ext) return FALSE;
	if (NULL == logger) return FALSE;
	if (NULL == logger->name) return FALSE;

	loggers = log_ext->domains;

	/* if we already have a logger registered, implicit or explicit, we need to update it to reflect the new values (target, level)
	 * the newly registered logger is always marked as being explicit
	 * in any case we need to update existing implicit loggers in the hierarchy "below" this one, to reflect potentially new
	 *   effective log levels and targets.
	 * TODO: the use of a hash to store them might have been a bad choice.
	 *       trees fit more naturally, but also make lookup slower when logging.
	 *       consider linking to parent/children within the logger_t
	 */

	/* if we are auto-registering a logger (when we log to a logger that hasn't been seen yet) we don't have to check for
	 * an existing one - saves a hash lookup
	 */

	if (FALSE == logger->is_autocreated) {
		existing_logger = chassis_log_extended_get_logger_raw(log_ext, logger->name);
	}

	if (NULL != existing_logger) {
		existing_logger->is_implicit = FALSE;
		existing_logger->min_level = logger->min_level;

		/* invalidate the effective level, this will be calculated upon the first lookup */
		existing_logger->effective_level = 0;

		/* TODO check for logger->backend being a valid and registered target! */
		existing_logger->backend = logger->backend;
		existing_logger->is_autocreated = FALSE;

		/* invalidate the hierarchy below this logger, up until each explicit logger encountered */
		g_ptr_array_foreach(existing_logger->children, chassis_log_domain_invalidate_hierarchy, NULL);

	} else {
		chassis_log_domain_t *implicit = NULL;
		chassis_log_domain_t *previous = NULL;
		gsize levels;
		gint i; /* do _not_ make this unsigned! that would break the if below */
		gchar **name_parts = NULL;

		/* insert the explicit logger, and all the implicit ones all the way to the root */
		g_hash_table_insert(loggers, logger->name, logger);
		name_parts = chassis_log_extract_hierarchy_names(logger->name, &levels);
		previous = logger;

		/* walk the name parts in reverse but leave out the last element (levels-1) - we have just inserted that one */
		for (i = levels-2; i >= 0; i--) {
			chassis_log_domain_t *parent = NULL;

			/* stop inserting on the first logger that's already present, irrespective of whether it's implicit or explicit.
			 * otherwise we would overwrite previously registered loggers (such as the root logger)
			 * we simply add the last logger created to the children list of the pre-existing logger and set our parent pointer to it
			 */
			if ((parent = chassis_log_extended_get_logger_raw(log_ext, name_parts[i]))) {
				/* if we haven't previously created an implicit logger, our direct parent already exists.
				 * in that case the explicit logger we inserted is the "child"
				 */
				if (!implicit) {
					implicit = logger;
				}
				g_ptr_array_add(parent->children, implicit);
				implicit->parent = parent;
				break;
			}

			/* implicit loggers have practically no information yet, only a name and that they are implicit */
			implicit = chassis_log_domain_new(name_parts[i], 0, NULL);
			implicit->is_implicit = TRUE;
			implicit->is_autocreated = logger->is_autocreated;

			g_hash_table_insert(loggers, implicit->name, implicit);

			previous->parent = implicit;
			g_ptr_array_add(implicit->children, previous);

			previous = implicit;
		}

		if (name_parts) g_strfreev(name_parts); /* theoretically it could be NULL */
	}

	return TRUE;
}

void chassis_log_extended_unregister_domain(chassis_log_extended_t G_GNUC_UNUSED *log_ext, chassis_log_domain_t G_GNUC_UNUSED *logger) {
	/* TODO: currently unimplemented */
	g_assert_not_reached();
}

static chassis_log_domain_t* chassis_log_extended_get_logger_raw(chassis_log_extended_t *log_ext, const gchar *logger_name) {
	if (!log_ext) return NULL;
	if (!logger_name) return NULL;

	return g_hash_table_lookup(log_ext->domains, logger_name);
}

chassis_log_domain_t* chassis_log_extended_get_logger(chassis_log_extended_t *log_ext, const gchar *logger_name) {
	chassis_log_domain_t *logger = chassis_log_extended_get_logger_raw(log_ext, logger_name);

	/* if this logger doesn't exist, create an implicit one.
	 * this should only happen when a log_domain is being passed in for a logger we have no explicit logger registered for.
	 */
	if (NULL == logger) {
		logger = chassis_log_domain_new(logger_name, 0, NULL);
		logger->is_implicit = TRUE;
		logger->is_autocreated = TRUE;
		chassis_log_extended_register_domain(log_ext, logger);
	}

	/* if this logger doesn't have its effective level set up yet, trigger a resolution */
	if (logger->effective_level == 0) {
		chassis_log_extended_get_effective_level(log_ext, logger_name);
	}
	return logger;
}

void chassis_log_extended_reopen(chassis_log_extended_t *log_ext) {
	GHashTableIter iterator;
	gpointer key, value;

	g_assert(log_ext->backends);

	g_hash_table_iter_init (&iterator, log_ext->backends);
	while (g_hash_table_iter_next (&iterator, &key, &value)) {
		chassis_log_backend_t *target = (chassis_log_backend_t*)value;
		const char *target_name = key;
		GError *gerr = NULL;

		if (FALSE == chassis_log_backend_reopen(target, &gerr)) {
			g_critical("%s: reopening logger target '%s' failed: %s",
					G_STRLOC,
					target_name,
					gerr->message);
			g_clear_error(&gerr);
		}
	}
}

void chassis_log_extended_force_log_all(chassis_log_extended_t *log_ext, const gchar *message) {
	GHashTableIter iterator;
	gpointer key, value;

	g_assert(log_ext->backends);
	g_hash_table_iter_init (&iterator, log_ext->backends);
	while (g_hash_table_iter_next (&iterator, &key, &value)) {
		chassis_log_backend_t *target = (chassis_log_backend_t*)value;
		(void)key; /* silence unused variable warning */

		/* log level 0 will trigger a "forced" dummy log level */
		chassis_log_backend_log(target, "all", 0, message);
	}
}

void chassis_log_extended_log_func(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data) {
	chassis_log_extended_t *log_ext = (chassis_log_extended_t *)user_data;
	chassis_log_domain_t *logger;

	/* revert to our root logger if we don't have a log_domain set */
	logger = chassis_log_extended_get_logger(log_ext, NULL == log_domain ? "" : log_domain);

	chassis_log_domain_log(logger, log_level, message);
}

/**
 * Internal helper function for chassis_log_extended_get_effective_level.
 * @param log_ext the extended_log structure
 * @param logger_name name of the logger to get the level for
 * @param target optional output value to also get the effective target of the logger
 * @return the effective log level for the logger_name
 */
static GLogLevelFlags chassis_log_extended_get_effective_level_and_target(chassis_log_extended_t *log_ext,
		const gchar *logger_name, chassis_log_backend_t **target) {
	chassis_log_domain_t *logger;

	logger = chassis_log_extended_get_logger_raw(log_ext, logger_name);
	if (!logger) return 0;

	if (logger->effective_level == 0) {
		if (logger->is_implicit) {
			/* for implicit loggers, we need to calculate their effective level:
			 * to keep it simply and avoid code duplication, we will simply recurse on the hierarchy one above us.
			 * This has several benefits:
			 *   * the implementation is concise
			 *   * it will fill out all intermediate loggers
			 *   * it stops on the first explicit logger automatically (recursion stop condition)
			 * The downside is that it performs more computation (esp string ops) than the iterative version.
			 * TODO: measure the overhead - computing the effective levels should be very infrequent, so it's likely ok to do this.
			 */
			gchar **hierarchy;
			gsize parts;
			chassis_log_backend_t *parent_target = NULL;
			GLogLevelFlags parent_effective_level;

			hierarchy = chassis_log_extract_hierarchy_names(logger_name, &parts);

			if (NULL != hierarchy) {
				g_assert_cmpint(parts, >=, 2);

				parent_effective_level = chassis_log_extended_get_effective_level_and_target(log_ext, hierarchy[parts - 2], &parent_target);
				logger->effective_level = parent_effective_level;
				logger->backend = parent_target;

				g_strfreev(hierarchy);
			}
		} else {
			/* explicit loggers have their effective_level given as their min_level */
			logger->effective_level = logger->min_level;
		}
	}

	/* if requested, also return our target */
	if (target) {
		*target = logger->backend;
	}

	return logger->effective_level;
}

GLogLevelFlags chassis_log_extended_get_effective_level(chassis_log_extended_t *log_ext, const gchar *logger_name) {
	return chassis_log_extended_get_effective_level_and_target(log_ext, logger_name, NULL);
}

/* utility functions */

gchar** chassis_log_extract_hierarchy_names(const gchar *logger_name, gsize *len) {
	gchar **substrings;
	gchar *occurrence;
	const gchar *haystack = logger_name;
	guint num_dots;
	guint i;

	if (logger_name == NULL) return NULL;

	for (i = 0, num_dots = 0; haystack[i] != '\0'; i++) {
		if (haystack[i] == '.') {
			num_dots++;
		}
	}

	/* +3 because n dots means n+1 parts and we always include the root logger (empty string) and need a trailing NULL pointer */
	substrings = g_malloc0((num_dots+3) * sizeof(gchar*));

	/* always insert the root logger (check for logger_name == "" is in the if stmt below) */
	i = 0;
	substrings[i++] = g_strdup("");
	do {
		occurrence = g_strstr_len(haystack, -1, ".");
		if (occurrence) {
			/* copy up to the dot (exclusive)*/
			substrings[i++] = g_strndup(logger_name, occurrence - logger_name);
			/* skip past the dot we found */
			haystack += (occurrence - haystack) + 1;
		} else if (g_strcmp0(logger_name, "") != 0) {
			/* last part is simply the original name, but don't copy the root logger twice! */
			substrings[i++] = g_strdup(logger_name);
		}
	} while (occurrence != NULL);

	/* add trailing NULL, so callers know when to stop */
	substrings[i] = NULL;

	if (len) {
		*len = i;
	}

	return substrings;
}

