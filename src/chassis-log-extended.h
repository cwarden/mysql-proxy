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
 * The same is true for the logger's backend - most likely a file name to write to.
 * 
 * There always is a "root logger" with the name of \c "" (empty string). If no other logger is configured, every other logger will inherit its
 * settings (effective log level and backend).
 * 
 * @section development Future development
 * 
 * Given GLib2's code for g_log()/g_logv(), which uses a single global mutex to protect looking up log domain handlers, there's an incentive
 * to bypass GLib2 altogether and to supply our own logging macros. This would be relatively easy, since the interface to GLib2 is just a
 * single function, namely chassis_log_extended_log_func().
 */

#include "chassis_log_domain.h" /* the log-domains like 'chassis.network' */
#include "chassis_log_backend.h" /* the backends like syslog, file, stderr, ... */

/**
 * The extended log stores the available hierarchical loggers and manages operations on them.
 * 
 * It supports registration and rotation of loggers and force-broadcasting a message to all logs.
 * @note This is currently not threadsafe when manipulating the available loggers!
 */
typedef struct chassis_log_extended {
	GHashTable *domains;			/**< <gchar*, chassis_log_domain_t*> a map of all available domains, keyed by logger name */
	GHashTable *backends;			/**< <gchar*, chassis_log_backend_t*> a map of all available backends, keyed by file name */
} chassis_log_extended_t;



CHASSIS_API chassis_log_extended_t* chassis_log_extended_new();
CHASSIS_API void chassis_log_extended_free(chassis_log_extended_t* log_ext);
/**
 * Registers a logger backend so it can be used with individual loggers.
 * 
 * @param log_ext the extended log structure
 * @param backend the backend to be added
 * @retval TRUE if the logger was registered
 * @retval FALSE if the registration failed or it already was registered (you should dispose backend yourself in this case)
 */
CHASSIS_API gboolean chassis_log_extended_register_backend(chassis_log_extended_t *log_ext, chassis_log_backend_t *backend);

/**
 * Register a logger
 *
 * @retval TRUE on success
 * @retval FALSE if the registration failed or it already was registered (you should dispose backend yourself in this case)
 */
CHASSIS_API gboolean chassis_log_extended_register_domain(chassis_log_extended_t *log_ext, chassis_log_domain_t *logger);
CHASSIS_API void chassis_log_extended_unregister_domain(chassis_log_extended_t *log_ext, chassis_log_domain_t *logger);
CHASSIS_API chassis_log_domain_t* chassis_log_extended_get_logger(chassis_log_extended_t *log_ext, const gchar *logger_name);
CHASSIS_API void chassis_log_extended_reopen(chassis_log_extended_t* log_ext);
CHASSIS_API void chassis_log_extended_force_log_all(chassis_log_extended_t* log_ext, const gchar *message);
CHASSIS_API GLogLevelFlags chassis_log_extended_get_effective_level(chassis_log_extended_t *log_ext, const gchar *logger_name);

/**
 * Interface to glib2's logging system.
 * 
 * chassis_log_extended_log_func looks up the corresponding logger from the log_domain and passes control on.
 * The logger will determine whether or not to actually log the message and ask its backend to write the message.
 * @param log_domain the name of the logger this message belongs to.
 * @param log_level the log_level this message is on
 * @param message the pre-formatted message to log
 * @param user_data a pointer to chassis_log
 */
CHASSIS_API void chassis_log_extended_log_func(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data);

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
