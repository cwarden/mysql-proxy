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

#include <glib.h>
#include <glib/gstdio.h>

#include "chassis_log_backend.h"
#include "string-len.h"

static void chassis_log_backend_update_timestamp(chassis_log_backend_t *target) {
	struct tm *tm;
	time_t t;
	GString *s = target->log_str;

	t = time(NULL);
	tm = localtime(&(t));
	s->len = strftime(s->str, s->allocated_len, "%Y-%m-%d %H:%M:%S", tm);
}

/* logger_target functions */

chassis_log_backend_t* chassis_log_backend_new(const gchar *filename) {
	chassis_log_backend_t *target = g_slice_new0(chassis_log_backend_t);

	target->file_path = g_strdup(filename);
	target->fd = -1;
	target->fd_lock = g_mutex_new();
	target->log_str = g_string_sized_new(sizeof("2004-01-01T00:00:00.000Z"));
	target->last_msg = g_string_new(NULL);
	target->last_msg_ts = 0;
	target->last_msg_count = 0;
	target->log_func = chassis_log_backend_write;
	/* the value destroy function is NULL, because we treat the hash as a set */
	target->last_loggers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	return target;
}

void chassis_log_backend_free(chassis_log_backend_t* target) {
	if (!target) return;

	if (target->fd != -1) chassis_log_backend_close(target, NULL);
	if (target->file_path) g_free(target->file_path);
	if (target->fd_lock) g_mutex_free(target->fd_lock);
	if (target->log_str) g_string_free(target->log_str, TRUE);
	if (target->last_msg) g_string_free(target->last_msg, TRUE);
	if (target->last_loggers) g_hash_table_unref(target->last_loggers);

	g_slice_free(chassis_log_backend_t, target);
}

gboolean chassis_log_backend_reopen(chassis_log_backend_t* target, GError **gerr) {
	gboolean is_ok = TRUE;

	g_return_val_if_fail(NULL != target, TRUE); /* we have no target, log that, but treat it as "success" as didn't fail to reopen the not existing target */
	
	g_mutex_lock(target->fd_lock);
	if (FALSE == chassis_log_backend_close(target, gerr)) {
		g_clear_error(gerr); /* if the close fails we may want to log it, but we just failed to close the target logger */
	}

	if (FALSE == chassis_log_backend_open(target, gerr)) {
		is_ok = FALSE;
	}
	g_mutex_unlock(target->fd_lock);

	return is_ok;
}

void chassis_log_backend_log(chassis_log_backend_t *target, gchar* logger_name, GLogLevelFlags level, const gchar *message) {
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
	chassis_log_backend_lock(target);

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
			guint hash_size = g_hash_table_size(target->last_loggers);

			if (hash_size > 0) { /* should be always true... */
				GHashTableIter iter;
				gpointer key, value;
				guint i = 0;

				g_hash_table_iter_init(&iter, target->last_loggers);
				while (g_hash_table_iter_next(&iter, &key, &value)) {
					g_string_append(logger_names, (gchar*)key);

					g_hash_table_iter_remove(&iter);
					if (++i < hash_size) {
						g_string_append(logger_names, ", ");
					}
				}
			}

			chassis_log_backend_update_timestamp(target);
			g_string_append_printf(target->log_str, ": [%s] last message repeated %d times\n",
					logger_names->str,
					target->last_msg_count);
			target->log_func(target, level, S(target->log_str));
			g_string_free(logger_names, TRUE);
		}
		chassis_log_backend_update_timestamp(target);
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

	chassis_log_backend_unlock(target);
}


void chassis_log_backend_write(chassis_log_backend_t* target, GLogLevelFlags level, gchar *message, gsize len) {
	g_assert(target);
	(void)level; /* unused here, because we have no syslog support yet */
	if (target->fd == -1) chassis_log_backend_open(target, NULL);

	write(target->fd, message, len);
}

void chassis_log_backend_lock(chassis_log_backend_t* target) {
	g_mutex_lock(target->fd_lock);
}

void chassis_log_backend_unlock(chassis_log_backend_t* target) {
	g_mutex_unlock(target->fd_lock);
}

gboolean chassis_log_backend_open(chassis_log_backend_t* target, GError **error) {
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

gboolean chassis_log_backend_close(chassis_log_backend_t* target, GError **error) {
	g_assert(target);
	g_assert_cmpint(target->fd, !=, -1);

	if (-1 == close(target->fd)) {
		g_set_error(error, g_file_error_quark(), g_file_error_from_errno(errno), "%s", g_strerror(errno));
		return FALSE;
	}
	target->fd = -1;
	return TRUE;
}


