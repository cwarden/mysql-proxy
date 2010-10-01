#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>

#include <fcntl.h>
#include <errno.h>

#ifndef WIN32
#include <unistd.h> /* close */
#else
#include <windows.h>
#include <io.h>
#endif

#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif

#include <glib.h>
#include <glib/gstdio.h>

#include "chassis_log_backend.h"
#include "chassis_log_error.h"
#include "string-len.h"

/* Attention: this needs to be adjusted should glib ever change its log level ordering */
#define G_LOG_ERROR_POSITION 3

int chassis_log_backend_resolution_set(chassis_log_backend_t *backend, chassis_log_backend_resolution_t res) {
	g_return_val_if_fail(NULL != backend, -1);

	backend->log_ts_resolution = res;

	return 0;
}

chassis_log_backend_resolution_t chassis_log_backend_resolution_get(chassis_log_backend_t *backend) {
	g_return_val_if_fail(NULL != backend, CHASSIS_LOG_BACKEND_RESOLUTION_DEFAULT);

	return backend->log_ts_resolution;
}

static void chassis_log_backend_update_timestamp(chassis_log_backend_t *backend) {
	GTimeVal tv;
	struct tm *tm;
	GString *s = backend->log_str;
	time_t secs;

	g_get_current_time(&tv);
	secs = tv.tv_sec;
	tm = localtime(&(secs));
	s->len = strftime(s->str, s->allocated_len, "%Y-%m-%d %H:%M:%S", tm);
	if (backend->log_ts_resolution == CHASSIS_LOG_BACKEND_RESOLUTION_MS) {
		g_string_append_printf(s, ".%.3d", (int) tv.tv_usec/1000);
	}
}

#ifdef HAVE_SYSLOG_H
void chassis_log_backend_syslog_log(chassis_log_backend_t G_GNUC_UNUSED * backend, GLogLevelFlags level, const gchar *message, gsize G_GNUC_UNUSED len) {
	int priority;

	if (level && G_LOG_LEVEL_ERROR) {
		priority = LOG_CRIT;
	} else if (level && G_LOG_LEVEL_CRITICAL) {
		priority = LOG_ERR;
	} else if (level && G_LOG_LEVEL_WARNING) {
		priority = LOG_WARNING;
	} else if (level && G_LOG_LEVEL_MESSAGE) {
		priority = LOG_NOTICE;
	} else if (level && G_LOG_LEVEL_INFO) {
		priority = LOG_INFO;
	} else if (level && G_LOG_LEVEL_DEBUG) {
		priority = LOG_DEBUG;
	} else {
		/* by default log as ERR ... we shouldn't be here though */
		priority = LOG_ERR;
	}

	syslog(priority, "%s", message);
}
#endif

int chassis_log_backend_syslog_init(chassis_log_backend_t *backend) {
#ifdef HAVE_SYSLOG_H
	backend->open_func = NULL;
	backend->close_func = NULL;
	backend->log_func = chassis_log_backend_syslog_log;
	backend->needs_timestamp = FALSE;
	backend->needs_compress = FALSE;
	backend->supports_reopen = FALSE;

	return 0;
#else
	return -1;
#endif
}

#ifdef _WIN32

void chassis_log_backend_eventlog_log(chassis_log_backend_t* backend, GLogLevelFlags level, const gchar *message, gsize len) {
	char *log_messages[1];
	int win_evtype;

	if (level && (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL)) {
		win_evtype = EVENTLOG_ERROR_TYPE;
	} else if (level && G_LOG_LEVEL_WARNING) {
		win_evtype = EVENTLOG_WARNING_TYPE;
	} else if (level && (G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG)) {
		win_evtype = EVENTLOG_INFORMATION_TYPE;
	} else {
		/* by default log as ERROR ... we shouldn't be here though */
		win_evtype = EVENTLOG_ERROR_TYPE;
	}

	log_messages[0] = message;
	ReportEvent(log->event_source_handle,
				win_evtype,
				0, /* category, we don't have that yet */
				win_evtype, /* event indentifier, one of MSG_ERROR (0x01), MSG_WARNING(0x02), MSG_INFO(0x04) */
				NULL,
				1, /* number of strings to be substituted */
				0, /* no event specific data */
				log_messages,	/* the actual log message, always the message we came up with, we don't localize using Windows message files*/
				NULL);
}

gboolean chassis_log_backend_eventlog_open(chassis_log_backend_t* backend, GError **error) {
	backend->use_windows_applog = TRUE;
	backend->event_source_handle = RegisterEventSource(NULL, app_name);

	if (NULL == backend->event_source_handle) {
		int err = GetLastError();

		g_critical("%s: RegisterEventSource(NULL, %s) failed: %s (%d)",
				G_STRLOC,
				app_name,
				g_strerror(err),
				err);

		return -1;
	}

	return 0;
}

gboolean chassis_log_backend_event_close(chassis_log_backend_t* backend, GError **error) {
	if (NULL != backend->event_source_handle) {
		if (!DeregisterEventSource(backend->event_source_handle)) {
			int err = GetLastError();

			g_set_error(error, chassis_log_error(), CHASSIS_LOG_ERROR_CLOSE,
					"unhandled error-code (%d) for DeregisterEventSource()",
					err);

			return FALSE;
		}
	}

	return TRUE;
}
#endif

int chassis_log_backend_eventlog_init(chassis_log_backend_t G_GNUC_UNUSED *backend) {
#ifdef _WIN32
	backend->open_func = NULL;
	backend->close_func = chassis_log_backend_event_close;
	backend->log_func = chassis_log_backend_eventlog_log;
	backend->needs_timestamp = FALSE;
	backend->needs_compress = FALSE;
	backend->supports_reopen = FALSE;

	return 0;
#else
	return -1;
#endif
}

/* the stderr backend */
void chassis_log_backend_stderr_log(chassis_log_backend_t G_GNUC_UNUSED * backend, GLogLevelFlags G_GNUC_UNUSED level, const gchar *message, gsize len) {
	write(STDERR_FILENO, message, len);
	write(STDERR_FILENO, "\n", 1);
}

int chassis_log_backend_stderr_init(chassis_log_backend_t *backend) {
	backend->open_func = NULL;
	backend->close_func = NULL;
	backend->log_func = chassis_log_backend_stderr_log;
	backend->needs_timestamp = TRUE;
	backend->needs_compress = TRUE;
	backend->supports_reopen = FALSE;

	return 0;
}


/* the file backend */
gboolean chassis_log_backend_file_open(chassis_log_backend_t* backend, GError **error) {
	g_assert(backend);
	g_assert(backend->file_path);
	g_assert_cmpint(backend->fd, ==, -1);

	backend->fd = g_open(backend->file_path, O_RDWR | O_CREAT | O_APPEND, 0660);
	if (backend->fd == -1) {
		g_set_error(error, g_file_error_quark(), g_file_error_from_errno(errno), "%s", g_strerror(errno));
		return FALSE;
	}
	return TRUE;
}

gboolean chassis_log_backend_file_close(chassis_log_backend_t* backend, GError **error) {
	g_assert(backend);
	
	if (backend->fd < 0) return TRUE;

	if (-1 == close(backend->fd)) {
		g_set_error(error, g_file_error_quark(), g_file_error_from_errno(errno), "%s", g_strerror(errno));
		return FALSE;
	}
	backend->fd = -1;
	return TRUE;
}

#ifndef _WIN32
gboolean chassis_log_backend_file_chown(chassis_log_backend_t *backend,
		uid_t uid, gid_t gid,
		GError **gerr) {
	/* chown logfile */
	if (-1 == chown(backend->file_path, uid, gid)) {
		g_set_error(gerr, chassis_log_error(), 
				CHASSIS_LOG_ERROR_CHOWN,
				"chown(%s) failed: %s",
				backend->file_path,
				g_strerror(errno));
		return FALSE;
	}

	return TRUE;
}
#endif

void chassis_log_backend_file_log(chassis_log_backend_t* backend, GLogLevelFlags G_GNUC_UNUSED level, const gchar *message, gsize len) {
	if (-1 == write(backend->fd, message, len)) {
		/* writing to the file failed (Disk Full, what ever ... */
		
		write(STDERR_FILENO, message, len);
		write(STDERR_FILENO, "\n", 1);
	} else {
		write(backend->fd, "\n", 1);
	}
}

int chassis_log_backend_file_init(chassis_log_backend_t *backend) {
	backend->open_func = chassis_log_backend_file_open;
	backend->close_func = chassis_log_backend_file_close;
	backend->log_func = chassis_log_backend_file_log;
#ifndef _WIN32
	backend->chown_func = chassis_log_backend_file_chown;
#endif
	backend->needs_timestamp = TRUE;
	backend->needs_compress = TRUE;
	backend->supports_reopen = TRUE;

	return 0;
}

/* backend functions */
chassis_log_backend_t* chassis_log_backend_file_new(const gchar *filename) {
	chassis_log_backend_t *backend;

	if (NULL == filename) return NULL;

	backend = chassis_log_backend_new();

	backend->name = g_strdup_printf("file:%s", filename);
	backend->file_path = g_strdup(filename);
	backend->fd = -1;

	chassis_log_backend_file_init(backend);

	return backend;
}

chassis_log_backend_t* chassis_log_backend_eventlog_new(void) {
	chassis_log_backend_t *backend = chassis_log_backend_new();

	if (0 != chassis_log_backend_eventlog_init(backend)) {
		chassis_log_backend_free(backend);

		return NULL;
	}
	backend->name = g_strdup("eventlog:");

	return backend;
}

chassis_log_backend_t* chassis_log_backend_stderr_new(void) {
	chassis_log_backend_t *backend = chassis_log_backend_new();

	chassis_log_backend_stderr_init(backend);
	backend->name = g_strdup("stderr:");

	return backend;
}

chassis_log_backend_t* chassis_log_backend_syslog_new(void) {
	chassis_log_backend_t *backend = chassis_log_backend_new();

	if (0 != chassis_log_backend_syslog_init(backend)) {
		chassis_log_backend_free(backend);

		return NULL;
	}
	backend->name = g_strdup("syslog:");

	return backend;
}


chassis_log_backend_t* chassis_log_backend_new(void) {
	chassis_log_backend_t *backend = g_slice_new0(chassis_log_backend_t);

	backend->fd = -1;
	backend->fd_lock = g_mutex_new();
	backend->log_str = g_string_sized_new(sizeof("2004-01-01T00:00:00.000Z"));
	backend->last_msg = g_string_new(NULL);
	backend->last_msg_ts = 0;
	backend->last_msg_count = 0;
	/* the value destroy function is NULL, because we treat the hash as a set */
	backend->last_loggers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	backend->log_ts_resolution = CHASSIS_LOG_BACKEND_RESOLUTION_DEFAULT;

	return backend;
}

void chassis_log_backend_free(chassis_log_backend_t* backend) {
	if (NULL == backend) return;

	chassis_log_backend_close(backend, NULL);

	if (NULL != backend->file_path) g_free(backend->file_path);
	if (NULL != backend->fd_lock) g_mutex_free(backend->fd_lock);
	if (NULL != backend->log_str) g_string_free(backend->log_str, TRUE);
	if (NULL != backend->last_msg) g_string_free(backend->last_msg, TRUE);
	if (NULL != backend->last_loggers) g_hash_table_unref(backend->last_loggers);

	g_slice_free(chassis_log_backend_t, backend);
}

gboolean chassis_log_backend_reopen(chassis_log_backend_t* backend, GError **gerr) {
	gboolean is_ok = TRUE;

	g_return_val_if_fail(NULL != backend, TRUE); /* we have no backend, log that, but treat it as "success" as didn't fail to reopen the not existing backend */

	if (!backend->supports_reopen) return TRUE;

	chassis_log_backend_lock(backend);
	if (FALSE == chassis_log_backend_close(backend, gerr)) {
		g_clear_error(gerr); /* if the close fails we may want to log it, but we just failed to close the backend logger */
	}

	if (FALSE == chassis_log_backend_open(backend, gerr)) {
		is_ok = FALSE;
	}
	chassis_log_backend_unlock(backend);

	return is_ok;
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

void chassis_log_backend_log(chassis_log_backend_t *backend, gchar* logger_name, GLogLevelFlags level, const gchar *message) {
	gboolean is_duplicate = FALSE;
	const gchar *log_lvl_name;
	const gchar *logger_name_clean = (logger_name[0] == '\0') ? "global" : logger_name;
	const gchar *stripped_message = chassis_log_skip_topsrcdir(message);

	switch (level & G_LOG_LEVEL_MASK) {
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
	case CHASSIS_LOG_LEVEL_BROADCAST:
		log_lvl_name = "*"; break;
	default:
		log_lvl_name = "unknown"; break;
	}

	chassis_log_backend_lock(backend);

	/* check for a duplicate message
	 * never consider this to be a duplicate if the log level is INFO (which being used to force a message, e.g. in broadcasting)
	 */
	if (backend->last_msg->len > 0 &&
			0 == strcmp(backend->last_msg->str, stripped_message) &&
			level != CHASSIS_LOG_LEVEL_BROADCAST && /* a broadcast */
			backend->needs_compress) {
		is_duplicate = TRUE;
	}

	if (!is_duplicate ||
			backend->last_msg_count > 100 ||
			time(NULL) - backend->last_msg_ts > 30) {	/* TODO: make these limits configurable */
		if (backend->last_msg_count) {
			GString *logger_names = g_string_new("");
			guint hash_size = g_hash_table_size(backend->last_loggers);

			if (hash_size > 0) { /* should be always true... */
				GHashTableIter iter;
				gpointer key, value;
				guint i = 0;

				g_hash_table_iter_init(&iter, backend->last_loggers);
				while (g_hash_table_iter_next(&iter, &key, &value)) {
					g_string_append(logger_names, (gchar*)key);

					g_hash_table_iter_remove(&iter);
					if (++i < hash_size) {
						g_string_append(logger_names, ", ");
					}
				}
			}

			if (backend->needs_timestamp) {
				chassis_log_backend_update_timestamp(backend);
				g_string_append_len(backend->log_str, C(": "));
			}
			g_string_append_printf(backend->log_str, "[%s] last message repeated %d times\n",
					logger_names->str,
					backend->last_msg_count);
			backend->log_func(backend, level, S(backend->log_str));
			g_string_free(logger_names, TRUE);
		}

		if (backend->needs_timestamp) {
			chassis_log_backend_update_timestamp(backend);
			g_string_append_len(backend->log_str, C(": "));
		}
		g_string_append_printf(backend->log_str, "[%s] (%s) %s",
				logger_name_clean,
				log_lvl_name,
				stripped_message);

		/* reset the last-logged message */	
		g_string_assign(backend->last_msg, stripped_message);
		backend->last_msg_count = 0;
		backend->last_msg_ts = time(NULL);

		/* ask the backend to perform the write */
		backend->log_func(backend, level, S(backend->log_str));
	} else {
		/* save the logger_name to print all of the coalesced logger sources later */
		gchar *hash_logger_name = g_strdup(logger_name_clean);

		g_hash_table_insert(backend->last_loggers, hash_logger_name, hash_logger_name);

		backend->last_msg_count++;
	}

	chassis_log_backend_unlock(backend);
}

void chassis_log_backend_lock(chassis_log_backend_t* target) {
	g_mutex_lock(target->fd_lock);
}

void chassis_log_backend_unlock(chassis_log_backend_t* target) {
	g_mutex_unlock(target->fd_lock);
}

gboolean chassis_log_backend_open(chassis_log_backend_t* backend, GError **error) {
	if (!backend->open_func) return TRUE;

	return backend->open_func(backend, error);
}

gboolean chassis_log_backend_close(chassis_log_backend_t* backend, GError **error) {
	if (!backend->close_func) return TRUE;

	return backend->close_func(backend, error);
}

#ifndef _WIN32
gboolean chassis_log_backend_chown(chassis_log_backend_t* backend,
		uid_t uid, gid_t gid,
		GError **error) {
	if (!backend->chown_func) return TRUE;

	return backend->chown_func(backend, uid, gid, error);
}
#endif

