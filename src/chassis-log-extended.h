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

#ifndef _CHASSIS_LOG_EXTENDED_H_
#define _CHASSIS_LOG_EXTENDED_H_

#include <glib.h>
#include "chassis-exports.h"

/**
 * @defgroup logging Hierarchical Logging
 * 
 * @section overview Overview
 * 
 * The chassis supports a hierarchical logging service that integrates with GLib2's Log Domains.
 * It closely resembles log4j but differs in some ways:
 *  - it has no concept of appenders
 *  - there are not formatters
 *  - the logger repository is not pluggable
 * 
 * Most of these limitations stem from the fact that this is not meant to be a generic log4c implementation,
 * there are already good ones out there.
 * 
 * Rather, it is designed to serve our purposes, while still being flexible enough to support non-file based logging
 * subsystems, such as syslog and the Windows Services Log. Other centralized log systems could also be hooked up to it (with a bit of work).
 * 
 * @section concepts Concepts
 * 
 * GLib2 has a concept called "log domains". Within the GTK environment these are simply strings that the programmer can register different
 * log handlers for.
 * 
 * We have abstracted from this simplistic view a bit, by adopting log4j's hierarchy concept: While still using GLib2's log domains, mainly
 * to gracefully handle plain g_log() calls (and its macro fronts), the log domain string now carries meaning:
 * If the string contains something separated by '.' characters, we treat it the same way as log4j would.
 * 
 * An example makes this obvious:@n
 * A log domain named \c "chassis.network.backend" is the child of \c "chassis.network" and so on.@n
 * If \c "chassis.network" has, via a configuration file, been assigned the log level \c MESSAGE, then \c "chassis.network.backend" would
 * inherit this log level (assuming there was no explicit configuration for it, too).@n
 * The same is true for the logger's target - most likely a file name to write to.
 * 
 * There always is a "root logger" with the name of \c "" (empty string). If no other logger is configured, every other logger will inherit its
 * settings (effective log level and target).
 * 
 * @section development Future development
 * 
 * Given GLib2's code for g_log()/g_logv(), which uses a single global mutex to protect looking up log domain handlers, there's an incentive
 * to bypass GLib2 altogether and to supply our own logging macros. This would be relatively easy, since the interface to GLib2 is just a
 * single function, namely chassis_log_extended_log_func().
 */

/* forward decl, so we can use it in the function ptr */
struct chassis_log_extended_logger_target;

typedef void(*chassis_log_target_write_func_t)(struct chassis_log_extended_logger_target *target, GLogLevelFlags level, gchar *message, gsize len);

/**
 * The extended log stores the available hierarchical loggers and manages operations on them.
 * 
 * It supports registration and rotation of loggers and force-broadcasting a message to all logs.
 * @note This is currently not threadsafe when manipulating the available loggers!
 */
typedef struct chassis_log_extended {
	GHashTable *loggers;			/**< <gchar*, chassis_log_extended_logger_t*> a map of all available loggers, keyed by logger name */
	GHashTable *logger_targets;		/**< <gchar*, chassis_log_extended_logger_target_t*> a map of all available logger_targets, keyed by file name */
} chassis_log_extended_t;

/**
 * A logger target encapsulates the ultimate target of a log message and its writing.
 * 
 * Currently it supports file-based logs or anything that doesn't need extra information, like syslog.
 */
typedef struct chassis_log_extended_logger_target {
	gchar *file_path;				/**< absolute path to the log file */
	gint fd;						/**< file descriptor for this log file */
	GMutex *fd_lock;				/**< lock to serialize log message writing */
	chassis_log_target_write_func_t log_func;	/**< pointer to the function that actually writes the message */

	GString *log_str;				/**< a reusable string for the log message to write */

	GString *last_msg;				/**< a copy of the last message we have written, used to coalesce messages */
	time_t last_msg_ts;				/**< the timestamp of when we have last written a message */
	guint last_msg_count;			/**< a repeat count to track how many messages we have coalesced */
	GHashTable *last_loggers;		/**< a list of the loggers we coalesced messages for, in order of appearance */
} chassis_log_extended_logger_target_t;

/**
 * A logger describes the attributes of a point in the logging hierarchy, such as the effective log level
 * and the target the messages go to.
 */
typedef struct chassis_log_extended_logger {
	gchar *name;						/**< the full name of this logger */
	GLogLevelFlags min_level;			/**< the minimum log level for this logger */
	GLogLevelFlags effective_level;		/**< the effective log level, calculated from min_level and its parent's min_levels */
	gboolean is_implicit;				/**< this logger hasn't been explicitly set, but is part of a hierarchy chain */
	gboolean is_autocreated;			/**< this logger has been created in response to a write to a non-existing logger */
	chassis_log_extended_logger_target_t *target; /**< target for this logger, essentially the fd and write function */
	
	struct chassis_log_extended_logger *parent;		/**< our parent in the hierarchy, NULL for the root logger */
	GPtrArray *children;				/**< the list of loggers directly below us in the hierarchy */
} chassis_log_extended_logger_t;


CHASSIS_API chassis_log_extended_t* chassis_log_extended_new();
CHASSIS_API void chassis_log_extended_free(chassis_log_extended_t* log_ext);
/**
 * Registers a logger target so it can be used with individual loggers.
 * 
 * @param log_ext the extended log structure
 * @param target the target to be added
 * @retval TRUE if the logger was registered
 * @retval FALSE if the registration failed or it already was registered (you should dispose target yourself in this case)
 */
CHASSIS_API gboolean chassis_log_extended_register_target(chassis_log_extended_t *log_ext, chassis_log_extended_logger_target_t *target);

/**
 * Register a logger
 *
 * @retval TRUE on success
 * @retval FALSE if the registration failed or it already was registered (you should dispose target yourself in this case)
 */
CHASSIS_API gboolean chassis_log_extended_register_logger(chassis_log_extended_t *log_ext, chassis_log_extended_logger_t *logger);
CHASSIS_API void chassis_log_extended_unregister_logger(chassis_log_extended_t *log_ext, chassis_log_extended_logger_t *logger);
CHASSIS_API chassis_log_extended_logger_t* chassis_log_extended_get_logger(chassis_log_extended_t *log_ext, const gchar *logger_name);
CHASSIS_API void chassis_log_extended_rotate(chassis_log_extended_t* log_ext);
CHASSIS_API void chassis_log_extended_force_log_all(chassis_log_extended_t* log_ext, const gchar *message);
CHASSIS_API GLogLevelFlags chassis_log_extended_get_effective_level(chassis_log_extended_t *log_ext, const gchar *logger_name);

/**
 * Interface to glib2's logging system.
 * 
 * chassis_log_extended_log_func looks up the corresponding logger from the log_domain and passes control on.
 * The logger will determine whether or not to actually log the message and ask its logger_target to write the message.
 * @param log_domain the name of the logger this message belongs to.
 * @param log_level the log_level this message is on
 * @param message the pre-formatted message to log
 * @param user_data a pointer to chassis_log
 */
CHASSIS_API void chassis_log_extended_log_func(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data);


CHASSIS_API chassis_log_extended_logger_target_t* chassis_log_extended_logger_target_new(const gchar *filename);
CHASSIS_API void chassis_log_extended_logger_target_free(chassis_log_extended_logger_target_t* target);
CHASSIS_API void chassis_log_extended_logger_target_rotate(chassis_log_extended_logger_target_t* target);
/**
 * Unconditionally writes to a target's log file and formats the string to be written.
 * This function also performs message coalescing, local to the target (i.e. coalescing is per-target).
 * 
 * @param target the target to write to
 * @param logger_name name of the logger this message was received for
 * @param level the log level for this message
 * @param message the string to write out - will be subject to formatting
 */
CHASSIS_API void chassis_log_extended_logger_target_log(chassis_log_extended_logger_target_t *target, gchar *logger_name, GLogLevelFlags level, const gchar *message);

/**
 * Unconditionally writes to a target's log file, i.e. it doesn't check the effective log level.
 * 
 * @param target the target to write to
 * @param level the log level for this message, used by syslog for example
 * @param message the string to write out
 * @param len the message length
 */
CHASSIS_API void chassis_log_extended_logger_target_write(chassis_log_extended_logger_target_t* target, GLogLevelFlags level, gchar *message, gsize len);
CHASSIS_API void chassis_log_extended_logger_target_lock(chassis_log_extended_logger_target_t* target);
CHASSIS_API void chassis_log_extended_logger_target_unlock(chassis_log_extended_logger_target_t* target);
/**
 * Opens the target's output.
 * 
 * For output targets that don't need to open anything (e.g. syslog) this is a no-op.
 * @param target the target that should open its output
 * @param error used to convey a lower-level error message back to the caller
 * @return TRUE: the operation was successful, FALSE: the operation failed - check error for details
 */
CHASSIS_API gboolean chassis_log_extended_logger_target_open(chassis_log_extended_logger_target_t* target, GError **error);
/**
 * Closes the target's output.
 * 
 * For output targets that don't need to close anything (e.g. syslog) this is a no-op.
 * @param target the target that should close its output
 * @param error used to convey a lower-level error message back to the caller
 * @return TRUE: the operation was successful, FALSE: the operation failed - check error for details
 */
CHASSIS_API gboolean chassis_log_extended_logger_target_close(chassis_log_extended_logger_target_t* target, GError **error);

CHASSIS_API chassis_log_extended_logger_t* chassis_log_extended_logger_new(const gchar *logger_name, GLogLevelFlags min_level, chassis_log_extended_logger_target_t *target);
CHASSIS_API void chassis_log_extended_logger_free(chassis_log_extended_logger_t* logger);
/**
 * Conditionally logs a message to a logger's target.
 * 
 * This function performs the checking of the effective log level against the message's log level.
 * It does not modify the message in any way.
 * 
 * @param logger the logger to validate the level against
 * @param level the log level of the message
 * @param message the string to log
 */
CHASSIS_API void chassis_log_extended_logger_log(chassis_log_extended_logger_t* logger, GLogLevelFlags level, const gchar *message);

/* utility functions */
/**
 * Extract a hierarchical name, separated by '.', into its parts.
 * 
 * It's guaranteed that the returned array always is either NULL for invalid input or
 * has at least two elements:
 *   * the root hierarchy (denoted by an emtpy string) as the first element
 *   * and a trailing NULL pointer
 * 
 * chassis_log_extract_hierarchy_names("a.b"); returns:
 *  { "", "a", "a.b", NULL }
 * 
 * @param logger_name a hierarchical name
 * @param len pointer where to store the number of names, may to be NULL
 * @return an array of hierarchy names
 * @retval NULL if the logger_name is NULL
 */
CHASSIS_API gchar** chassis_log_extract_hierarchy_names(const gchar *logger_name, gsize *len);

#endif /* _CHASSIS_LOG_EXTENDED_H_ */
