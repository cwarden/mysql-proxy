#ifndef __CHASSIS_LOG_DOMAIN_H__
#define __CHASSIS_LOG_DOMAIN_H__

/* previously known as _extended_logger_t */

#include <glib.h>

#include "chassis_log_backend.h" /* the backends like syslog, file, stderr, ... */

/**
 * A logger describes the attributes of a point in the logging hierarchy, such as the effective log level
 * and the backend the messages go to.
 */
typedef struct chassis_log_domain {
	gchar *name;					/**< the full name of this logger */
	GLogLevelFlags min_level;			/**< the minimum log level for this logger */
	GLogLevelFlags effective_level;			/**< the effective log level, calculated from min_level and its parent's min_levels */
	gboolean is_implicit;				/**< this logger hasn't been explicitly set, but is part of a hierarchy chain */
	gboolean is_autocreated;			/**< this logger has been created in response to a write to a non-existing logger */
	chassis_log_backend_t *backend;			/**< backend for this logger, essentially the fd and write function */
	
	struct chassis_log_domain *parent;		/**< our parent in the hierarchy, NULL for the root logger */
	GPtrArray *children;				/**< the list of loggers directly below us in the hierarchy */
} chassis_log_domain_t;

CHASSIS_API chassis_log_domain_t* chassis_log_domain_new(const gchar *logger_name, GLogLevelFlags min_level, chassis_log_backend_t *backend);
CHASSIS_API void chassis_log_domain_free(chassis_log_domain_t* logger);

/**
 * Conditionally logs a message to a logger's backend.
 * 
 * This function performs the checking of the effective log level against the message's log level.
 * It does not modify the message in any way.
 * 
 * @param logger the logger to validate the level against
 * @param level the log level of the message
 * @param message the string to log
 */
CHASSIS_API void chassis_log_domain_log(chassis_log_domain_t* logger, GLogLevelFlags level, const gchar *message);

#endif
