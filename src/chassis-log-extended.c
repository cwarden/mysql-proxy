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
static chassis_log_extended_logger_t* chassis_log_extended_get_logger_raw(chassis_log_extended_t *log_ext, gchar *logger_name);
static void chassis_log_extended_logger_target_update_timestamp(chassis_log_extended_logger_target_t *target);

/* log_extended functions */

chassis_log_extended_t* chassis_log_extended_new() {
	chassis_log_extended_t *log_ext =  g_new0(chassis_log_extended_t, 1);
	if (log_ext) {
		/* don't free the keys, they are part of the value for both hashes
		 * the individual loggers should _not_ free their target, this is taken care of here as the target might be in use somewhere else
		 */
		log_ext->loggers = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)chassis_log_extended_logger_free);
		log_ext->logger_targets = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)chassis_log_extended_logger_target_free);
	}
	return log_ext;
}

void chassis_log_extended_free(chassis_log_extended_t* log_ext) {
	if (!log_ext) return;

	if (log_ext->loggers) g_hash_table_destroy(log_ext->loggers);
	if (log_ext->logger_targets) g_hash_table_destroy(log_ext->logger_targets);

	g_free(log_ext);
}

gboolean chassis_log_extended_register_target(chassis_log_extended_t *log_ext, chassis_log_extended_logger_target_t *target) {
	GHashTable *targets = log_ext->logger_targets;
	chassis_log_extended_logger_target_t *registered_target;
	
	/* check for a valid target */
	if (!target) return FALSE;
	if (!target->file_path) return FALSE;
	
	registered_target = g_hash_table_lookup(targets, target->file_path);
	/* don't allow registering a target twice */
	if (registered_target)
		return FALSE;

	g_hash_table_insert(targets, target->file_path, target);
	return TRUE;
}

static void chassis_log_extended_logger_invalidate_hierarchy(gpointer data, gpointer user_data) {
	chassis_log_extended_logger_t *logger = (chassis_log_extended_logger_t*)data;
	(void)user_data;
	
	/* don't touch explicit loggers - stop condition for the recursion */
	if (logger->is_implicit == FALSE) return;
	
	/* otherwise reset the target and effective level for this logger and recurse into the children */
	logger->effective_level = 0;
	logger->target = NULL;

	g_ptr_array_foreach(logger->children, chassis_log_extended_logger_invalidate_hierarchy, NULL);
}

void chassis_log_extended_register_logger(chassis_log_extended_t *log_ext, chassis_log_extended_logger_t *logger) {
	GHashTable *loggers;
	chassis_log_extended_logger_t *existing_logger = NULL;

	if (!log_ext) return;
	if (!logger) return;
	g_assert(logger->name);

	loggers = log_ext->loggers;
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
	if (logger->is_autocreated == FALSE) {
		existing_logger = chassis_log_extended_get_logger_raw(log_ext, logger->name);
	}
	if (existing_logger) {
		existing_logger->is_implicit = FALSE;
		existing_logger->min_level = logger->min_level;
		/* invalidate the effective level, this will be calculated upon the first lookup */
		existing_logger->effective_level = 0;
		/* TODO check for logger->target being a valid and registered target! */
		existing_logger->target = logger->target;
		existing_logger->is_autocreated = FALSE;
		/* invalidate the hierarchy below this logger, up until each explicit logger encountered */
		g_ptr_array_foreach(existing_logger->children, chassis_log_extended_logger_invalidate_hierarchy, NULL);

	} else {
		chassis_log_extended_logger_t *implicit = NULL;
		chassis_log_extended_logger_t *previous = NULL;
		guint levels = 0;
		gint i; /* do _not_ make this unsigned! that would break the if below */
		gchar **name_parts = NULL;

		/* insert the explicit logger, and all the implicit ones all the way to the root */
		g_hash_table_insert(loggers, logger->name, logger);
		name_parts = chassis_log_extract_hierarchy_names(logger->name);
		levels = g_strv_length(name_parts);
		previous = logger;

		/* walk the name parts in reverse but leave out the last element (levels-1) - we have just inserted that one */
		for (i = levels-2; i >= 0; i--) {
			chassis_log_extended_logger_t *parent = NULL;
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
			implicit = chassis_log_extended_logger_new(name_parts[i], 0, NULL);
			implicit->is_implicit = TRUE;
			implicit->is_autocreated = logger->is_autocreated;
			g_hash_table_insert(loggers, implicit->name, implicit);
			previous->parent = implicit;
			g_ptr_array_add(implicit->children, previous);
			previous = implicit;
		}
		g_strfreev(name_parts);
	}
}

void chassis_log_extended_unregister_logger(chassis_log_extended_t G_GNUC_UNUSED *log_ext, chassis_log_extended_logger_t G_GNUC_UNUSED *logger) {
	/* TODO: currently unimplemented */
	g_assert_not_reached();
}

static chassis_log_extended_logger_t* chassis_log_extended_get_logger_raw(chassis_log_extended_t *log_ext, gchar *logger_name) {
	if (!log_ext) return NULL;
	if (!logger_name) return NULL;
	return g_hash_table_lookup(log_ext->loggers, logger_name);
}
chassis_log_extended_logger_t* chassis_log_extended_get_logger(chassis_log_extended_t *log_ext, gchar *logger_name) {
	chassis_log_extended_logger_t *logger = chassis_log_extended_get_logger_raw(log_ext, logger_name);
	/* if this logger doesn't exist, create an implicit one.
	 * this should only happen when a log_domain is being passed in for a logger we have no explicit logger registered for.
	 */
	if (logger == NULL) {
		logger = chassis_log_extended_logger_new(logger_name, 0, NULL);
		logger->is_implicit = TRUE;
		logger->is_autocreated = TRUE;
		chassis_log_extended_register_logger(log_ext, logger);
	}
	/* if this logger doesn't have its effective level set up yet, trigger a resolution */
	if (logger->effective_level == 0) {
		chassis_log_extended_get_effective_level(log_ext, logger_name);
	}
	return logger;
}

void chassis_log_extended_rotate(chassis_log_extended_t *log_ext) {
	GHashTableIter iterator;
	gpointer key, value;

	g_assert(log_ext->logger_targets);

	g_hash_table_iter_init (&iterator, log_ext->logger_targets);
	while (g_hash_table_iter_next (&iterator, &key, &value)) {
		chassis_log_extended_logger_target_t *target = (chassis_log_extended_logger_target_t*)value;
		(void)key; /* silence unused variable warning */

		chassis_log_extended_logger_target_rotate(target);
	}
}

void chassis_log_extended_force_log_all(chassis_log_extended_t *log_ext, gchar *message) {
	GHashTableIter iterator;
	gpointer key, value;

	g_assert(log_ext->logger_targets);
	g_hash_table_iter_init (&iterator, log_ext->logger_targets);
	while (g_hash_table_iter_next (&iterator, &key, &value)) {
		chassis_log_extended_logger_target_t *target = (chassis_log_extended_logger_target_t*)value;
		(void)key; /* silence unused variable warning */

		/* log level 0 will trigger a "forced" dummy log level */
		chassis_log_extended_logger_target_log(target, "all", 0, message);
	}
}

void chassis_log_extended_log_func(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data) {
	chassis_log_extended_t *log_ext = (chassis_log_extended_t *)user_data;
	chassis_log_extended_logger_t *logger;

	/* revert to our root logger if we don't have a log_domain set */
	if (log_domain == NULL) {
		log_domain = "";
	}
	logger = chassis_log_extended_get_logger(log_ext, (gchar *)log_domain);
	chassis_log_extended_logger_log(logger, log_level, (gchar *)message);
}

/**
 * Internal helper function for chassis_log_extended_get_effective_level.
 * @param log_ext the extended_log structure
 * @param logger_name name of the logger to get the level for
 * @param target optional output value to also get the effective target of the logger
 * @return the effective log level for the logger_name
 */
static GLogLevelFlags chassis_log_extended_get_effective_level_and_target(chassis_log_extended_t *log_ext, gchar *logger_name, chassis_log_extended_logger_target_t **target) {
	chassis_log_extended_logger_t *logger;

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
			gchar **hierarchy = chassis_log_extract_hierarchy_names(logger_name);
			guint parts = g_strv_length(hierarchy);
			chassis_log_extended_logger_target_t *parent_target = NULL;
			GLogLevelFlags parent_effective_level = chassis_log_extended_get_effective_level_and_target(log_ext, hierarchy[parts - 2], &parent_target);
			logger->effective_level = parent_effective_level;
			logger->target = parent_target;
			g_strfreev(hierarchy);
		} else {
			/* explicit loggers have their effective_level given as their min_level */
			logger->effective_level = logger->min_level;
		}
	}
	/* if requested, also return our target */
	if (target)
		*target = logger->target;
	return logger->effective_level;
}

GLogLevelFlags chassis_log_extended_get_effective_level(chassis_log_extended_t *log_ext, gchar *logger_name) {
	return chassis_log_extended_get_effective_level_and_target(log_ext, logger_name, NULL);
}
/* logger_target functions */

chassis_log_extended_logger_target_t* chassis_log_extended_logger_target_new(const gchar *filename) {
	chassis_log_extended_logger_target_t *target = g_new0(chassis_log_extended_logger_target_t, 1);
	if (target) {
		target->file_path = g_strdup(filename);
		target->fd = -1;
		target->fd_lock = g_mutex_new();
		target->log_str = g_string_sized_new(sizeof("2004-01-01T00:00:00.000Z"));
		target->last_msg = g_string_new(NULL);
		target->last_msg_ts = 0;
		target->last_msg_count = 0;
		target->log_func = chassis_log_extended_logger_target_write;
		/* the value destroy function is NULL, because we treat the hash as a set */
		target->last_loggers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	}
	return target;
}

void chassis_log_extended_logger_target_free(chassis_log_extended_logger_target_t* target) {
	if (!target) return;
	if (target->fd != -1) chassis_log_extended_logger_target_close(target, NULL);
	if (target->file_path) g_free(target->file_path);
	if (target->fd_lock) g_mutex_free(target->fd_lock);
	if (target->log_str) g_string_free(target->log_str, TRUE);
	if (target->last_msg) g_string_free(target->last_msg, TRUE);
	if (target->last_loggers) g_hash_table_unref(target->last_loggers);

	g_free(target);
}

void chassis_log_extended_logger_target_rotate(chassis_log_extended_logger_target_t* target) {
	GError *error = NULL;
	g_assert(target);
	
	g_mutex_lock(target->fd_lock);

	if (FALSE == chassis_log_extended_logger_target_close(target, &error)) {
		/* TODO: handle errors somehow */
	}
	error = NULL;
	if (FALSE == chassis_log_extended_logger_target_open(target, &error)) {
		/* TODO: handle errors somehow */
	}
	g_mutex_unlock(target->fd_lock);
}

void chassis_log_extended_logger_target_log(chassis_log_extended_logger_target_t *target, gchar* logger_name, GLogLevelFlags level, gchar *message) {
	gboolean is_duplicate = FALSE;
	gchar *log_lvl_name = "forced";
	gchar *logger_name_clean = (logger_name[0] == '\0') ? "global" : logger_name;

	switch (level) {
	case G_LOG_LEVEL_CRITICAL:
		log_lvl_name = "critical"; break;
	case G_LOG_LEVEL_ERROR:
		log_lvl_name = "error"; break;
	case G_LOG_LEVEL_WARNING:
		log_lvl_name = "warning"; break;
	case G_LOG_LEVEL_MESSAGE:
		log_lvl_name = "message"; break;
	case G_LOG_LEVEL_INFO:
		log_lvl_name = "info"; break;
	case G_LOG_LEVEL_DEBUG:
		log_lvl_name = "debug"; break;
	default: break;
	}
	chassis_log_extended_logger_target_lock(target);

	/* check for a duplicate message
	 * never consider this to be a duplicate if the log level is 0 (which being used to force a message, e.g. in broadcasting)
	 */
	if (target->last_msg->len > 0 &&
			0 == strcmp(target->last_msg->str, message) &&
			level != 0) {
		is_duplicate = TRUE;
	}
	if (!is_duplicate ||
			target->last_msg_count > 100 ||
			time(NULL) - target->last_msg_ts > 30) {	/* TODO: make these limits configurable */
		if (target->last_msg_count) {
			GString *logger_names = g_string_new("");
			if (g_hash_table_size(target->last_loggers) > 0) { /* should be always true... */
				GHashTableIter iter;
				gpointer key, value;
				guint i = 0;
				guint hash_size = g_hash_table_size(target->last_loggers);

				g_hash_table_iter_init(&iter, target->last_loggers);
				while (g_hash_table_iter_next(&iter, &key, &value)) {
					g_string_append(logger_names, (gchar*)key);
					g_hash_table_iter_remove(&iter);
					if (++i < hash_size) {
						g_string_append(logger_names, ", ");
					}
				}
			}

			chassis_log_extended_logger_target_update_timestamp(target);
			g_string_append_printf(target->log_str, ": [%s] last message repeated %d times\n",
					logger_names->str,
					target->last_msg_count);
			target->log_func(target, level, S(target->log_str));
			g_string_free(logger_names, TRUE);
		}
		chassis_log_extended_logger_target_update_timestamp(target);
		g_string_append_printf(target->log_str, ": [%s] (%s) %s\n",
				logger_name_clean,
				log_lvl_name,
				message);

		/* reset the last-logged message */	
		g_string_assign(target->last_msg, message);
		target->last_msg_count = 0;
		target->last_msg_ts = time(NULL);

		/* ask the target to perform the write */
		target->log_func(target, level, S(target->log_str));
	} else {
		/* save the logger_name to print all of the coalesced logger sources later */
		gchar *hash_logger_name = g_strdup(logger_name_clean);
		g_hash_table_insert(target->last_loggers, hash_logger_name, hash_logger_name);
		target->last_msg_count++;
	}

	chassis_log_extended_logger_target_unlock(target);
}


void chassis_log_extended_logger_target_write(chassis_log_extended_logger_target_t* target, GLogLevelFlags level, gchar *message, gsize len) {
	g_assert(target);
	(void)level; /* unused here, because we have no syslog support yet */
	if (target->fd == -1) chassis_log_extended_logger_target_open(target, NULL);

	write(target->fd, message, len);
}

void chassis_log_extended_logger_target_lock(chassis_log_extended_logger_target_t* target) {
	g_mutex_lock(target->fd_lock);
}

void chassis_log_extended_logger_target_unlock(chassis_log_extended_logger_target_t* target) {
	g_mutex_unlock(target->fd_lock);
}

gboolean chassis_log_extended_logger_target_open(chassis_log_extended_logger_target_t* target, GError **error) {
	g_assert(target);
	g_assert(target->file_path);
	g_assert_cmpint(target->fd, ==, -1);

	target->fd = g_open(target->file_path, O_RDWR | O_CREAT | O_APPEND, 0660);
	if (target->fd == -1) {
		g_set_error(error, g_file_error_quark(), g_file_error_from_errno(errno), "%s", g_strerror(errno));
		return FALSE;
	}
	return TRUE;
}

gboolean chassis_log_extended_logger_target_close(chassis_log_extended_logger_target_t* target, GError **error) {
	g_assert(target);
	g_assert_cmpint(target->fd, !=, -1);

	if (-1 == close(target->fd)) {
		g_set_error(error, g_file_error_quark(), g_file_error_from_errno(errno), "%s", g_strerror(errno));
		return FALSE;
	}
	target->fd = -1;
	return TRUE;
}


/* logger functions */

chassis_log_extended_logger_t* chassis_log_extended_logger_new(gchar *logger_name, GLogLevelFlags min_level, chassis_log_extended_logger_target_t *target) {
	chassis_log_extended_logger_t *logger = g_new0(chassis_log_extended_logger_t, 1);
	if (logger) {
		logger->name = g_strdup(logger_name);
		logger->min_level = min_level;
		logger->target = target;
		logger->is_autocreated = FALSE;
		logger->parent = NULL;
		logger->children = g_ptr_array_new();
	}
	return logger;
}

void chassis_log_extended_logger_free(chassis_log_extended_logger_t* logger) {
	if (logger == NULL) return;
	if (logger->name) g_free(logger->name);
	if (logger->children) g_ptr_array_free(logger->children, TRUE);
	g_free(logger);
}

CHASSIS_API void chassis_log_extended_logger_log(chassis_log_extended_logger_t* logger, GLogLevelFlags level, gchar *message) {
	if (logger->effective_level < level) {
		return;
	}
	chassis_log_extended_logger_target_log(logger->target, logger->name, level, message);
}


/* utility functions */

static void chassis_log_extended_logger_target_update_timestamp(chassis_log_extended_logger_target_t *target) {
	struct tm *tm;
	time_t t;
	GString *s = target->log_str;

	t = time(NULL);
	tm = localtime(&(t));
	s->len = strftime(s->str, s->allocated_len, "%Y-%m-%d %H:%M:%S", tm);
}

gchar** chassis_log_extract_hierarchy_names(gchar *logger_name) {
	gchar **substrings;
	gchar *occurrence;
	gchar *haystack = logger_name;
	guint num_dots = 0;
	guint i = 0;

	if (logger_name == NULL) return NULL;

	while (haystack[i] != '\0') {
		if (haystack[i] == '.')
			num_dots++;
		i++;
	}
	/* +3 because n dots means n+1 parts and we always include the root logger (empty string) and need a trailing NULL pointer */
	substrings = g_malloc0((num_dots+3) * sizeof(gchar*));
	/* always insert the root logger (check for logger_name == "" is in the if stmt below) */
	substrings[0] = g_strdup("");
	i = 1;
	do {
		occurrence = g_strstr_len(haystack, -1, ".");
		if (occurrence) {
			/* copy up to the dot (exclusive)*/
			substrings[i] = g_strndup(logger_name, occurrence - logger_name);
			/* skip past the dot we found */
			haystack += (occurrence - haystack) + 1;
		} else {
			/* last part is simply the original name, but don't copy the root logger twice! */
			if (g_strcmp0(logger_name, "") != 0) {
				substrings[i] = g_strdup(logger_name);
			}
		}
		i++;
	} while (occurrence != NULL);

	/* add trailing NULL, so callers know when to stop */
	substrings[i] = NULL;
	return substrings;
}
