#ifndef __CHASSIS_LOG_BACKEND_H__
#define __CHASSIS_LOG_BACKEND_H__

/* formerly known as _extended_logger_target_t */

/* forward decl, so we can use it in the function ptr */
typedef struct chassis_log_backend chassis_log_backend_t;

typedef void(*chassis_log_backend_write_func_t)(chassis_log_backend_t *target, GLogLevelFlags level, gchar *message, gsize len);

/**
 * A logger target encapsulates the ultimate target of a log message and its writing.
 * 
 * Currently it supports file-based logs or anything that doesn't need extra information, like syslog.
 */
struct chassis_log_backend {
	gchar *file_path;				/**< absolute path to the log file */
	gint fd;						/**< file descriptor for this log file */
	GMutex *fd_lock;				/**< lock to serialize log message writing */
	chassis_log_backend_write_func_t log_func;	/**< pointer to the function that actually writes the message */

	GString *log_str;				/**< a reusable string for the log message to write */

	GString *last_msg;				/**< a copy of the last message we have written, used to coalesce messages */
	time_t last_msg_ts;				/**< the timestamp of when we have last written a message */
	guint last_msg_count;			/**< a repeat count to track how many messages we have coalesced */
	GHashTable *last_loggers;		/**< a list of the loggers we coalesced messages for, in order of appearance */
};

CHASSIS_API chassis_log_backend_t* chassis_log_backend_new(const gchar *filename);
CHASSIS_API void chassis_log_backend_free(chassis_log_backend_t* target);
CHASSIS_API gboolean chassis_log_backend_reopen(chassis_log_backend_t* target, GError **gerr);
/**
 * Unconditionally writes to a target's log file and formats the string to be written.
 * This function also performs message coalescing, local to the target (i.e. coalescing is per-target).
 * 
 * @param target the target to write to
 * @param logger_name name of the logger this message was received for
 * @param level the log level for this message
 * @param message the string to write out - will be subject to formatting
 */
CHASSIS_API void chassis_log_backend_log(chassis_log_backend_t *target, gchar *logger_name, GLogLevelFlags level, const gchar *message);

/**
 * Unconditionally writes to a target's log file, i.e. it doesn't check the effective log level.
 * 
 * @param target the target to write to
 * @param level the log level for this message, used by syslog for example
 * @param message the string to write out
 * @param len the message length
 */
CHASSIS_API void chassis_log_backend_write(chassis_log_backend_t* target, GLogLevelFlags level, gchar *message, gsize len);
CHASSIS_API void chassis_log_backend_lock(chassis_log_backend_t* target);
CHASSIS_API void chassis_log_backend_unlock(chassis_log_backend_t* target);
/**
 * Opens the target's output.
 * 
 * For output targets that don't need to open anything (e.g. syslog) this is a no-op.
 * @param target the target that should open its output
 * @param error used to convey a lower-level error message back to the caller
 * @return TRUE: the operation was successful, FALSE: the operation failed - check error for details
 */
CHASSIS_API gboolean chassis_log_backend_open(chassis_log_backend_t* target, GError **error);
/**
 * Closes the target's output.
 * 
 * For output targets that don't need to close anything (e.g. syslog) this is a no-op.
 * @param target the target that should close its output
 * @param error used to convey a lower-level error message back to the caller
 * @return TRUE: the operation was successful, FALSE: the operation failed - check error for details
 */
CHASSIS_API gboolean chassis_log_backend_close(chassis_log_backend_t* target, GError **error);

#endif
