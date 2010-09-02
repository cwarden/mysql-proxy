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
  

#ifndef _CHASSIS_LOG_H_
#define _CHASSIS_LOG_H_

#include <glib.h>
#ifdef _WIN32
#include <windows.h>
#endif

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
 *  - the domain repository is not pluggable
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
 * The same is true for the domain's backend - most likely a file name to write to.
 * 
 * There always is a "root domain" with the name of \c "" (empty string). If no other domain is configured, every other domain will inherit its
 * settings (effective log level and backend).
 * 
 * @section development Future development
 * 
 * Given GLib2's code for g_log()/g_logv(), which uses a single global mutex to protect looking up log domain handlers, there's an incentive
 * to bypass GLib2 altogether and to supply our own logging macros. This would be relatively easy, since the interface to GLib2 is just a
 * single function, namely chassis_log_domain_log_func().
 */

#include "chassis_log_domain.h" /* the log-domains like 'chassis.network' */
#include "chassis_log_backend.h" /* the backends like syslog, file, stderr, ... */

/**
 * The log stores the available hierarchical domains and manages operations on them.
 * 
 * It supports registration and rotation of domains and force-broadcasting a message to all logs.
 * @note This is currently not threadsafe when manipulating the available domains!
 */

/** @addtogroup chassis */
/*@{*/
typedef struct {
	gboolean rotate_logs;

	GHashTable *domains;			/**< <gchar*, chassis_log_domain_t*> a map of all available domains, keyed by domain name */
	GHashTable *backends;			/**< <gchar*, chassis_log_backend_t*> a map of all available backends, keyed by file name */
} chassis_log_t;

typedef chassis_log_t chassis_log G_GNUC_DEPRECATED;

CHASSIS_API chassis_log_t *chassis_log_init(void) G_GNUC_DEPRECATED;
CHASSIS_API chassis_log_t *chassis_log_new(void);
CHASSIS_API GLogLevelFlags chassis_log_level_string_to_level(const gchar *level);
CHASSIS_API void chassis_log_free(chassis_log_t *log);
CHASSIS_API void chassis_log_set_logrotate(chassis_log_t *log);
CHASSIS_API const char *chassis_log_skip_topsrcdir(const char *message);

/**
 * Registers a domain backend so it can be used with individual domains.
 * 
 * @param log the log structure
 * @param backend the backend to be added
 * @retval TRUE if the domain was registered
 * @retval FALSE if the registration failed or it already was registered (you should dispose backend yourself in this case)
 */
CHASSIS_API gboolean chassis_log_register_backend(chassis_log_t *log, chassis_log_backend_t *backend);

/**
 * Register a domain
 *
 * @retval TRUE on success
 * @retval FALSE if the registration failed or it already was registered (you should dispose backend yourself in this case)
 */
CHASSIS_API gboolean chassis_log_register_domain(chassis_log_t *log, chassis_log_domain_t *domain);
CHASSIS_API void chassis_log_unregister_domain(chassis_log_t *log, chassis_log_domain_t *domain);
CHASSIS_API chassis_log_domain_t* chassis_log_get_domain(chassis_log_t *log, const gchar *domain_name);
CHASSIS_API void chassis_log_reopen(chassis_log_t* log);
CHASSIS_API void chassis_log_force_log_all(chassis_log_t* log, const gchar *message);
CHASSIS_API GLogLevelFlags chassis_log_get_effective_level(chassis_log_t *log, const gchar *domain_name);

/**
 * Interface to glib2's logging system.
 * 
 * chassis_log_domain_log_func looks up the corresponding domain from the log_domain and passes control on.
 * The domain will determine whether or not to actually log the message and ask its backend to write the message.
 * @param log_domain the name of the domain this message belongs to.
 * @param log_level the log_level this message is on
 * @param message the pre-formatted message to log
 * @param user_data a pointer to chassis_log
 */
CHASSIS_API void chassis_log_func(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data);

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
 * @param domain_name a hierarchical name
 * @param len pointer where to store the number of names, may to be NULL
 * @return an array of hierarchy names
 * @retval NULL if the domain_name is NULL
 */
CHASSIS_API gchar** chassis_log_extract_hierarchy_names(const gchar *domain_name, gsize *len);

CHASSIS_API gboolean chassis_log_load_config(chassis_log_t *log, const gchar *file_name, GError **gerr);

#ifndef _WIN32
CHASSIS_API gboolean chassis_log_chown(chassis_log_t *log, uid_t uid, gid_t gid, GError **gerr);
#endif

CHASSIS_API int chassis_log_set_default(chassis_log_t *log, const char *log_filename, GLogLevelFlags log_lvl);

/*@}*/

#endif
