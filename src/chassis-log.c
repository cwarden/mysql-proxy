/* $%BEGINLICENSE%$
 Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; version 2 of the
 License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA

 $%ENDLICENSE%$ */
 

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#ifndef WIN32
#include <unistd.h> /* close */
/* define eventlog types when not on windows, saves code below */
#define EVENTLOG_ERROR_TYPE	0x0001
#define EVENTLOG_WARNING_TYPE	0x0002
#define EVENTLOG_INFORMATION_TYPE	0x0004
#else
#include <windows.h>
#include <io.h>
#define STDERR_FILENO 2
#endif
#include <glib.h>

#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#else
/* placeholder values for platforms not having syslog support */
#define LOG_USER	0   /* placeholder for user-level syslog facility */
#define LOG_CRIT	0
#define LOG_ERR	0
#define LOG_WARNING	0
#define LOG_NOTICE	0
#define LOG_INFO	0
#define LOG_DEBUG	0
#endif

#include "sys-pedantic.h"
#include "chassis-log.h"

#define S(x) x->str, x->len

/**
 * the mapping of our internal log levels to various log systems
 */
/* Attention: this needs to be adjusted should glib ever change its log level ordering */
#define G_LOG_ERROR_POSITION 3
const struct {
	char *name;
	GLogLevelFlags lvl;
	int syslog_lvl;
	int win_evtype;
} log_lvl_map[] = {	/* syslog levels are different to the glib ones */
	{ "error", G_LOG_LEVEL_ERROR,		LOG_CRIT,		EVENTLOG_ERROR_TYPE},
	{ "critical", G_LOG_LEVEL_CRITICAL, LOG_ERR,		EVENTLOG_ERROR_TYPE},
	{ "warning", G_LOG_LEVEL_WARNING,	LOG_WARNING,	EVENTLOG_WARNING_TYPE},
	{ "message", G_LOG_LEVEL_MESSAGE,	LOG_NOTICE,		EVENTLOG_INFORMATION_TYPE},
	{ "info", G_LOG_LEVEL_INFO,			LOG_INFO,		EVENTLOG_INFORMATION_TYPE},
	{ "debug", G_LOG_LEVEL_DEBUG,		LOG_DEBUG,		EVENTLOG_INFORMATION_TYPE},

	{ NULL, 0, 0, 0 }
};

/**
 * @deprecated will be removed in 1.0
 * @see chassis_log_new()
 */
chassis_log_t *chassis_log_init(void) {
	return chassis_log_new();
}

chassis_log_t *chassis_log_new(void) {
	chassis_log_t *log;

	log = g_new0(chassis_log_t, 1);

	log->log_file_fd = -1;
	log->log_ts_str = g_string_sized_new(sizeof("2004-01-01T00:00:00.000Z"));
	log->log_ts_resolution = CHASSIS_LOG_RESOLUTION_DEFAULT;
	log->min_lvl = G_LOG_LEVEL_CRITICAL;

	log->last_msg = g_string_new(NULL);
	log->last_msg_ts = 0;
	log->last_msg_count = 0;

	/* don't free the keys, they are part of the value for both hashes
	 * the individual loggers should _not_ free their target, this is taken care of here as the target might be in use somewhere else
	 */
	log->domains = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)chassis_log_domain_free);
	log->backends = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)chassis_log_backend_free);

	return log;
}

int chassis_log_set_level(chassis_log_t *log, const gchar *level) {
	gint i;

	for (i = 0; log_lvl_map[i].name; i++) {
		if (0 == strcmp(log_lvl_map[i].name, level)) {
			log->min_lvl = log_lvl_map[i].lvl;
			return 0;
		}
	}

	return -1;
}

/**
 * open the log-file
 *
 * open the log-file set in log->log_filename
 * if no log-filename is set, returns TRUE
 *
 * FIXME: the return value is not following 'unix'-style (0 on success, -1 on error),
 *        nor does it say it is a gboolean. Has to be fixed in 0.9.0
 *
 * @return TRUE on success, FALSE on error
 */
int chassis_log_open(chassis_log_t *log) {
	if (!log->log_filename) return TRUE;

	log->log_file_fd = open(log->log_filename, O_RDWR | O_CREAT | O_APPEND, 0660);

	return (log->log_file_fd != -1);
}

/**
 * close the log-file
 *
 * @return 0 on success
 *
 * @see chassis_log_open
 */
int chassis_log_close(chassis_log_t *log) {
	if (log->log_file_fd == -1) return 0;

	close(log->log_file_fd);

	log->log_file_fd = -1;

	return 0;
}

void chassis_log_free(chassis_log_t *log) {
	if (!log) return;

	chassis_log_close(log);
#ifdef _WIN32
	if (log->event_source_handle) {
		if (!DeregisterEventSource(log->event_source_handle)) {
			int err = GetLastError();
			g_critical("unhandled error-code (%d) for DeregisterEventSource()", err);
		}
	}
#endif
	g_string_free(log->log_ts_str, TRUE);
	g_string_free(log->last_msg, TRUE);

	if (log->log_filename) g_free(log->log_filename);
	if (log->domains) g_hash_table_destroy(log->domains);
	if (log->backends) g_hash_table_destroy(log->backends);


	g_free(log);
}

static int chassis_log_update_timestamp(chassis_log_t *log) {
	struct tm *tm;
	GTimeVal tv;
	time_t	t;
	GString *s = log->log_ts_str;

	g_get_current_time(&tv);
	t = (time_t) tv.tv_sec;
	tm = localtime(&t);
	
	s->len = strftime(s->str, s->allocated_len, "%Y-%m-%d %H:%M:%S", tm);
	if (log->log_ts_resolution == CHASSIS_LOG_RESOLUTION_MS) {
		g_string_append_printf(s, ".%.3d", (int) tv.tv_usec/1000);
	}
	
	return 0;
}

int chassis_log_set_timestamp_resolution(chassis_log_t *log, chassis_log_resolution_t res) {
	g_return_val_if_fail(NULL != log, -1);

	log->log_ts_resolution = res;

	return 0;
}

chassis_log_resolution_t chassis_log_get_timestamp_resolution(chassis_log_t *log) {
	g_return_val_if_fail(NULL != log, CHASSIS_LOG_RESOLUTION_DEFAULT);

	return log->log_ts_resolution;
}


static int chassis_log_write(chassis_log_t *log, int log_level, GString *str) {
	if (-1 != log->log_file_fd) {
		/* prepend a timestamp */
		if (-1 == write(log->log_file_fd, S(str))) {
			/* writing to the file failed (Disk Full, what ever ... */
			
			write(STDERR_FILENO, S(str));
			write(STDERR_FILENO, "\n", 1);
		} else {
			write(log->log_file_fd, "\n", 1);
		}
#ifdef HAVE_SYSLOG_H
	} else if (log->use_syslog) {
		int log_index = g_bit_nth_lsf(log_level & G_LOG_LEVEL_MASK, -1) - G_LOG_ERROR_POSITION;
		syslog(log_lvl_map[log_index].syslog_lvl, "%s", str->str);
#endif
#ifdef _WIN32
	} else if (log->use_windows_applog && log->event_source_handle) {
		char *log_messages[1];
		int log_index = g_bit_nth_lsf(log_level & G_LOG_LEVEL_MASK, -1) - G_LOG_ERROR_POSITION;

		log_messages[0] = str->str;
		ReportEvent(log->event_source_handle,
					log_lvl_map[log_index].win_evtype,
					0, /* category, we don't have that yet */
					log_lvl_map[log_index].win_evtype, /* event indentifier, one of MSG_ERROR (0x01), MSG_WARNING(0x02), MSG_INFO(0x04) */
					NULL,
					1, /* number of strings to be substituted */
					0, /* no event specific data */
					log_messages,	/* the actual log message, always the message we came up with, we don't localize using Windows message files*/
					NULL);
#endif
	} else {
		write(STDERR_FILENO, S(str));
		write(STDERR_FILENO, "\n", 1);
	}

	return 0;
}

/**
 * skip the 'top_srcdir' from a string starting with G_STRLOC or __FILE__
 *
 * ../trunk/src/chassis-log.c will become src/chassis-log.c
 *
 * NOTE: the code assumes it is located in src/ or src\. If it gets moves somewhere else
 *       it won't crash, but strip too much of pathname
 */
const char *chassis_log_skip_topsrcdir(const char *message) {
	const char *my_filename = __FILE__;
	int ndx;

	/* usually the message start with G_STRLOC which may contain a rather long, absolute path. If
	 * it matches the TOP_SRCDIR, we filter it out
	 *
	 * - strip what is the same as our __FILE__
	 * - don't strip our own sub-path 'src/'
	 */
	for (ndx = 0; message[ndx]; ndx++) {
		if (0 == strncmp(message + ndx, "src" G_DIR_SEPARATOR_S, sizeof("src" G_DIR_SEPARATOR_S) - 1)) break;
		if (message[ndx] != my_filename[ndx]) break;
	}

	if (message[ndx] != '\0') {
		message += ndx;
	}

	return message;
}

void chassis_log_func(const gchar *UNUSED_PARAM(log_domain), GLogLevelFlags log_level, const gchar *message, gpointer user_data) {
	chassis_log_t *log = user_data;
	int i;
	gchar *log_lvl_name = "(error)";
	gboolean is_duplicate = FALSE;
	gboolean is_log_rotated = FALSE;
	const char *stripped_message = chassis_log_skip_topsrcdir(message);

	/**
	 * make sure we syncronize the order of the write-statements 
	 */
	static GStaticMutex log_mutex = G_STATIC_MUTEX_INIT;

	/**
	 * rotate logs straight away if log->rotate_logs is true
	 * we do this before ignoring any log levels, so that rotation 
	 * happens straight away - see Bug#55711 
	 */
	if (-1 != log->log_file_fd) {
		if (log->rotate_logs) {
			chassis_log_close(log);
			chassis_log_open(log);

			is_log_rotated = TRUE; /* we will need to dump even duplicates */
		}
	}

	/* ignore the verbose log-levels */
	if (log_level > log->min_lvl) return;

	g_static_mutex_lock(&log_mutex);

	for (i = 0; log_lvl_map[i].name; i++) {
		if (log_lvl_map[i].lvl == log_level) {
			log_lvl_name = log_lvl_map[i].name;
			break;
		}
	}

	if (log->last_msg->len > 0 &&
	    0 == strcmp(log->last_msg->str, stripped_message)) {
		is_duplicate = TRUE;
	}

	/**
	 * if the log has been rotated, we always dump the last message even if it 
	 * was a duplicate. Otherwise, do not print duplicates unless they have been
	 * ignored at least 100 times, or they were last printed greater than 
	 * 30 seconds ago.
	 */
	if (is_log_rotated ||
	    !is_duplicate ||
	    log->last_msg_count > 100 ||
	    time(NULL) - log->last_msg_ts > 30) {

		/* if we lave the last message repeating, log it */
		if (log->last_msg_count) {
			chassis_log_update_timestamp(log);
			g_string_append_printf(log->log_ts_str, ": (%s) last message repeated %d times",
					log_lvl_name,
					log->last_msg_count);

			chassis_log_write(log, log_level, log->log_ts_str);
		}
		chassis_log_update_timestamp(log);
		g_string_append(log->log_ts_str, ": (");
		g_string_append(log->log_ts_str, log_lvl_name);
		g_string_append(log->log_ts_str, ") ");

		g_string_append(log->log_ts_str, stripped_message);

		/* reset the last-logged message */	
		g_string_assign(log->last_msg, stripped_message);
		log->last_msg_count = 0;
		log->last_msg_ts = time(NULL);
			
		chassis_log_write(log, log_level, log->log_ts_str);
	} else {
		log->last_msg_count++;
	}

	log->rotate_logs = FALSE;

	g_static_mutex_unlock(&log_mutex);
}

void chassis_log_set_logrotate(chassis_log_t *log) {
	log->rotate_logs = TRUE;
}

int chassis_log_set_event_log(chassis_log_t *log, const char G_GNUC_UNUSED *app_name) {
	g_return_val_if_fail(log != NULL, -1);

#if _WIN32
	log->use_windows_applog = TRUE;
	log->event_source_handle = RegisterEventSource(NULL, app_name);

	if (!log->event_source_handle) {
		int err = GetLastError();

		g_critical("%s: RegisterEventSource(NULL, %s) failed: %s (%d)",
				G_STRLOC,
				app_name,
				g_strerror(err),
				err);

		return -1;
	}

	return 0;
#else
	return -1;
#endif
}

/* forward decls */
static chassis_log_domain_t* chassis_log_get_logger_raw(chassis_log_t *log, const gchar *logger_name);

gboolean chassis_log_register_backend(chassis_log_t *log, chassis_log_backend_t *target) {
	GHashTable *targets = log->backends;
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

gboolean chassis_log_register_domain(chassis_log_t *log, chassis_log_domain_t *logger) {
	GHashTable *loggers;
	chassis_log_domain_t *existing_logger = NULL;

	if (NULL == log) return FALSE;
	if (NULL == logger) return FALSE;
	if (NULL == logger->name) return FALSE;

	loggers = log->domains;

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
		existing_logger = chassis_log_get_logger_raw(log, logger->name);
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
			if ((parent = chassis_log_get_logger_raw(log, name_parts[i]))) {
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

void chassis_log_unregister_domain(chassis_log_t G_GNUC_UNUSED *log, chassis_log_domain_t G_GNUC_UNUSED *logger) {
	/* TODO: currently unimplemented */
	g_assert_not_reached();
}

static chassis_log_domain_t* chassis_log_get_logger_raw(chassis_log_t *log, const gchar *logger_name) {
	if (!log) return NULL;
	if (!logger_name) return NULL;

	return g_hash_table_lookup(log->domains, logger_name);
}

chassis_log_domain_t* chassis_log_get_logger(chassis_log_t *log, const gchar *logger_name) {
	chassis_log_domain_t *logger = chassis_log_get_logger_raw(log, logger_name);

	/* if this logger doesn't exist, create an implicit one.
	 * this should only happen when a log_domain is being passed in for a logger we have no explicit logger registered for.
	 */
	if (NULL == logger) {
		logger = chassis_log_domain_new(logger_name, 0, NULL);
		logger->is_implicit = TRUE;
		logger->is_autocreated = TRUE;
		chassis_log_register_domain(log, logger);
	}

	/* if this logger doesn't have its effective level set up yet, trigger a resolution */
	if (logger->effective_level == 0) {
		chassis_log_get_effective_level(log, logger_name);
	}
	return logger;
}

void chassis_log_reopen(chassis_log_t *log) {
	GHashTableIter iterator;
	gpointer key, value;

	g_assert(log->backends);

	g_hash_table_iter_init (&iterator, log->backends);
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

void chassis_log_force_log_all(chassis_log_t *log, const gchar *message) {
	GHashTableIter iterator;
	gpointer key, value;

	g_assert(log->backends);
	g_hash_table_iter_init (&iterator, log->backends);
	while (g_hash_table_iter_next (&iterator, &key, &value)) {
		chassis_log_backend_t *target = (chassis_log_backend_t*)value;
		(void)key; /* silence unused variable warning */

		/* log level 0 will trigger a "forced" dummy log level */
		chassis_log_backend_log(target, "all", 0, message);
	}
}

void chassis_log_domain_log_func(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data) {
	chassis_log_t *log = (chassis_log_t *)user_data;
	chassis_log_domain_t *logger;

	/* revert to our root logger if we don't have a log_domain set */
	logger = chassis_log_get_logger(log, NULL == log_domain ? "" : log_domain);

	chassis_log_domain_log(logger, log_level, message);
}

/**
 * Internal helper function for chassis_log_get_effective_level.
 * @param log the extended_log structure
 * @param logger_name name of the logger to get the level for
 * @param target optional output value to also get the effective target of the logger
 * @return the effective log level for the logger_name
 */
static GLogLevelFlags chassis_log_get_effective_level_and_target(chassis_log_t *log,
		const gchar *logger_name, chassis_log_backend_t **target) {
	chassis_log_domain_t *logger;

	logger = chassis_log_get_logger_raw(log, logger_name);
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

				parent_effective_level = chassis_log_get_effective_level_and_target(log, hierarchy[parts - 2], &parent_target);
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

GLogLevelFlags chassis_log_get_effective_level(chassis_log_t *log, const gchar *logger_name) {
	return chassis_log_get_effective_level_and_target(log, logger_name, NULL);
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

