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
#else
#include <windows.h>
#include <io.h>
#define STDERR_FILENO 2
#endif
#include <glib.h>

#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif

#include "sys-pedantic.h"
#include "chassis-log.h"

#define S(x) x->str, x->len

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

	/* don't free the keys, they are part of the value for both hashes
	 * the individual domains should _not_ free their target, this is taken care of here as the target might be in use somewhere else
	 */
	log->domains = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)chassis_log_domain_free);
	log->backends = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)chassis_log_backend_free);

	return log;
}

GLogLevelFlags chassis_log_level_string_to_level(const gchar *level_str) {
	GQuark level_quark = g_quark_from_string(level_str);
#define STATIC_QUARK(x) static GQuark x ## _quark = 0
#define INIT_QUARK(x) if (0 == x ## _quark) { x ## _quark = g_quark_from_static_string(#x); }
	STATIC_QUARK(error);
	STATIC_QUARK(critical);
	STATIC_QUARK(warning);
	STATIC_QUARK(message);
	STATIC_QUARK(info);
	STATIC_QUARK(debug);
	INIT_QUARK(error)
	INIT_QUARK(critical)
	INIT_QUARK(warning)
	INIT_QUARK(message)
	INIT_QUARK(info)
	INIT_QUARK(debug)
#undef STATIC_QUARK
#undef INIT_QUARK
	
	if (level_quark == error_quark) { return G_LOG_LEVEL_ERROR; }
	else if (level_quark == critical_quark) { return G_LOG_LEVEL_CRITICAL; }
	else if (level_quark == warning_quark) { return G_LOG_LEVEL_WARNING; }
	else if (level_quark == message_quark) { return G_LOG_LEVEL_MESSAGE; }
	else if (level_quark == info_quark) { return G_LOG_LEVEL_DEBUG; } /* backward compat: info wasn't used before, now it is internally used for broadcasts */
	else if (level_quark == debug_quark) { return G_LOG_LEVEL_DEBUG; }
	return 0;
}

void chassis_log_free(chassis_log_t *log) {
	if (!log) return;

	if (log->domains) g_hash_table_destroy(log->domains);
	if (log->backends) g_hash_table_destroy(log->backends);

	g_free(log);
}

void chassis_log_set_logrotate(chassis_log_t *log) {
	log->rotate_logs = TRUE;
}

/* forward decls */
static chassis_log_domain_t* chassis_log_get_domain_raw(chassis_log_t *log, const gchar *domain_name);

gboolean chassis_log_register_backend(chassis_log_t *log, chassis_log_backend_t *backend) {
	GHashTable *backends = log->backends;
	chassis_log_backend_t *registered_backend;
	GError *gerr = NULL;
	
	/* check for a valid backend */
	if (!backend) return FALSE;
	if (!backend->name) return FALSE;
	
	registered_backend = g_hash_table_lookup(backends, backend->name);

	/* don't allow registering a backend twice */
	if (registered_backend) return FALSE;

	if (FALSE == chassis_log_backend_open(backend, &gerr)) {
		g_critical("%s: opening backend '%s' failed: %s",
				G_STRLOC,
				backend->name,
				gerr->message);
		g_clear_error(&gerr);
		return FALSE;
	}

	g_hash_table_insert(backends, backend->name, backend);

	return TRUE;
}

static void chassis_log_domain_invalidate_hierarchy(gpointer data, gpointer G_GNUC_UNUSED user_data) {
	chassis_log_domain_t *domain = (chassis_log_domain_t*)data;
	
	/* don't touch explicit domains - stop condition for the recursion */
	if (domain->is_implicit == FALSE) return;
	
	/* otherwise reset the target and effective level for this domain and recurse into the children */
	domain->effective_level = 0;
	domain->backend = NULL;

	g_ptr_array_foreach(domain->children, chassis_log_domain_invalidate_hierarchy, NULL);
}

gboolean chassis_log_register_domain(chassis_log_t *log, chassis_log_domain_t *domain) {
	GHashTable *domains;
	chassis_log_domain_t *existing_domain = NULL;

	if (NULL == log) return FALSE;
	if (NULL == domain) return FALSE;
	if (NULL == domain->name) return FALSE;

	domains = log->domains;

	/* if we already have a domain registered, implicit or explicit, we need to update it to reflect the new values (target, level)
	 * the newly registered domain is always marked as being explicit
	 * in any case we need to update existing implicit domains in the hierarchy "below" this one, to reflect potentially new
	 *   effective log levels and targets.
	 * TODO: the use of a hash to store them might have been a bad choice.
	 *       trees fit more naturally, but also make lookup slower when logging.
	 *       consider linking to parent/children within the domain_t
	 */

	/* if we are auto-registering a domain (when we log to a domain that hasn't been seen yet) we don't have to check for
	 * an existing one - saves a hash lookup
	 */

	if (FALSE == domain->is_autocreated) {
		existing_domain = chassis_log_get_domain_raw(log, domain->name);
	}

	if (NULL != existing_domain) {
		existing_domain->is_implicit = FALSE;
		existing_domain->min_level = domain->min_level;

		/* invalidate the effective level, this will be calculated upon the first lookup */
		existing_domain->effective_level = 0;

		/* TODO check for domain->backend being a valid and registered target! */
		existing_domain->backend = domain->backend;
		existing_domain->is_autocreated = FALSE;

		/* invalidate the hierarchy below this domain, up until each explicit domain encountered */
		g_ptr_array_foreach(existing_domain->children, chassis_log_domain_invalidate_hierarchy, NULL);

	} else {
		chassis_log_domain_t *implicit = NULL;
		chassis_log_domain_t *previous = NULL;
		gsize levels;
		gint i; /* do _not_ make this unsigned! that would break the if below */
		gchar **name_parts = NULL;

		/* insert the explicit domain, and all the implicit ones all the way to the root */
		g_hash_table_insert(domains, domain->name, domain);
		name_parts = chassis_log_extract_hierarchy_names(domain->name, &levels);
		previous = domain;

		/* walk the name parts in reverse but leave out the last element (levels-1) - we have just inserted that one */
		for (i = levels-2; i >= 0; i--) {
			chassis_log_domain_t *parent = NULL;

			/* stop inserting on the first domain that's already present, irrespective of whether it's implicit or explicit.
			 * otherwise we would overwrite previously registered domains (such as the root domain)
			 * we simply add the last domain created to the children list of the pre-existing domain and set our parent pointer to it
			 */
			if ((parent = chassis_log_get_domain_raw(log, name_parts[i]))) {
				/* if we haven't previously created an implicit domain, our direct parent already exists.
				 * in that case the explicit domain we inserted is the "child"
				 */
				if (!implicit) {
					implicit = domain;
				}
				g_ptr_array_add(parent->children, implicit);
				implicit->parent = parent;
				break;
			}

			/* implicit domains have practically no information yet, only a name and that they are implicit */
			implicit = chassis_log_domain_new(name_parts[i], 0, NULL);
			implicit->is_implicit = TRUE;
			implicit->is_autocreated = domain->is_autocreated;

			g_hash_table_insert(domains, implicit->name, implicit);

			previous->parent = implicit;
			g_ptr_array_add(implicit->children, previous);

			previous = implicit;
		}

		if (name_parts) g_strfreev(name_parts); /* theoretically it could be NULL */
	}

	return TRUE;
}

void chassis_log_unregister_domain(chassis_log_t G_GNUC_UNUSED *log, chassis_log_domain_t G_GNUC_UNUSED *domain) {
	/* TODO: currently unimplemented */
	g_assert_not_reached();
}

static chassis_log_domain_t* chassis_log_get_domain_raw(chassis_log_t *log, const gchar *domain_name) {
	if (!log) return NULL;
	if (!domain_name) return NULL;

	return g_hash_table_lookup(log->domains, domain_name);
}

chassis_log_domain_t* chassis_log_get_domain(chassis_log_t *log, const gchar *domain_name) {
	chassis_log_domain_t *domain = chassis_log_get_domain_raw(log, domain_name);

	/* if this domain doesn't exist, create an implicit one.
	 * this should only happen when a log_domain is being passed in for a domain we have no explicit domain registered for.
	 */
	if (NULL == domain) {
		domain = chassis_log_domain_new(domain_name, 0, NULL);
		domain->is_implicit = TRUE;
		domain->is_autocreated = TRUE;
		chassis_log_register_domain(log, domain);
	}

	/* if this domain doesn't have its effective level set up yet, trigger a resolution */
	if (domain->effective_level == 0) {
		chassis_log_get_effective_level(log, domain_name);
	}
	return domain;
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
			g_critical("%s: reopening domain target '%s' failed: %s",
					G_STRLOC,
					target_name,
					gerr->message);
			g_clear_error(&gerr);
		}
	}
}

#ifndef _WIN32
gboolean chassis_log_chown(chassis_log_t *log, uid_t uid, gid_t gid, GError **gerr) {
	GHashTableIter iterator;
	gpointer key, value;

	g_hash_table_iter_init (&iterator, log->backends);
	while (g_hash_table_iter_next (&iterator, &key, &value)) {
		chassis_log_backend_t *backend = (chassis_log_backend_t*)value;

		if (FALSE == chassis_log_backend_chown(backend, uid, gid, gerr)) {
			return FALSE;
		}
	}

	return TRUE;
}
#endif

void chassis_log_force_log_all(chassis_log_t *log, const gchar *message) {
	GHashTableIter iterator;
	gpointer key, value;

	g_assert(log->backends);
	g_hash_table_iter_init (&iterator, log->backends);
	while (g_hash_table_iter_next (&iterator, &key, &value)) {
		chassis_log_backend_t *target = (chassis_log_backend_t*)value;
		(void)key; /* silence unused variable warning */

		/* log level 0 will trigger a "forced" dummy log level */
		chassis_log_backend_log(target, "all", CHASSIS_LOG_LEVEL_BROADCAST, message);
	}
}

void chassis_log_func(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data) {
	chassis_log_t *log = (chassis_log_t *)user_data;
	chassis_log_domain_t *domain;

	/* revert to our root domain if we don't have a log_domain set */
	domain = chassis_log_get_domain(log, NULL == log_domain ? "" : log_domain);

	chassis_log_domain_log(domain, log_level, message);
}

/**
 * Internal helper function for chassis_log_get_effective_level.
 * @param log the extended_log structure
 * @param domain_name name of the domain to get the level for
 * @param target optional output value to also get the effective target of the domain
 * @return the effective log level for the domain_name
 */
static GLogLevelFlags chassis_log_get_effective_level_and_target(chassis_log_t *log,
		const gchar *domain_name, chassis_log_backend_t **target) {
	chassis_log_domain_t *domain;

	domain = chassis_log_get_domain_raw(log, domain_name);
	if (!domain) return 0;

	if (domain->effective_level == 0) {
		if (domain->is_implicit) {
			/* for implicit domains, we need to calculate their effective level:
			 * to keep it simply and avoid code duplication, we will simply recurse on the hierarchy one above us.
			 * This has several benefits:
			 *   * the implementation is concise
			 *   * it will fill out all intermediate domains
			 *   * it stops on the first explicit domain automatically (recursion stop condition)
			 * The downside is that it performs more computation (esp string ops) than the iterative version.
			 * TODO: measure the overhead - computing the effective levels should be very infrequent, so it's likely ok to do this.
			 */
			gchar **hierarchy;
			gsize parts;
			chassis_log_backend_t *parent_target = NULL;
			GLogLevelFlags parent_effective_level;

			hierarchy = chassis_log_extract_hierarchy_names(domain_name, &parts);

			if (NULL != hierarchy) {
				g_assert_cmpint(parts, >=, 2);

				parent_effective_level = chassis_log_get_effective_level_and_target(log, hierarchy[parts - 2], &parent_target);
				domain->effective_level = parent_effective_level;
				domain->backend = parent_target;

				g_strfreev(hierarchy);
			}
		} else {
			/* explicit domains have their effective_level given as their min_level */
			domain->effective_level = domain->min_level;
		}
	}

	/* if requested, also return our target */
	if (target) {
		*target = domain->backend;
	}

	return domain->effective_level;
}

GLogLevelFlags chassis_log_get_effective_level(chassis_log_t *log, const gchar *domain_name) {
	return chassis_log_get_effective_level_and_target(log, domain_name, NULL);
}

/* utility functions */

gchar** chassis_log_extract_hierarchy_names(const gchar *domain_name, gsize *len) {
	gchar **substrings;
	gchar *occurrence;
	const gchar *haystack = domain_name;
	guint num_dots;
	guint i;

	if (domain_name == NULL) return NULL;

	for (i = 0, num_dots = 0; haystack[i] != '\0'; i++) {
		if (haystack[i] == '.') {
			num_dots++;
		}
	}

	/* +3 because n dots means n+1 parts and we always include the root domain (empty string) and need a trailing NULL pointer */
	substrings = g_malloc0((num_dots+3) * sizeof(gchar*));

	/* always insert the root domain (check for domain_name == CHASSIS_LOG_DEFAULT_DOMAIN is in the if stmt below) */
	i = 0;
	substrings[i++] = g_strdup(CHASSIS_LOG_DEFAULT_DOMAIN);
	do {
		occurrence = g_strstr_len(haystack, -1, ".");
		if (occurrence) {
			/* copy up to the dot (exclusive)*/
			substrings[i++] = g_strndup(domain_name, occurrence - domain_name);
			/* skip past the dot we found */
			haystack += (occurrence - haystack) + 1;
		} else if (g_strcmp0(domain_name, CHASSIS_LOG_DEFAULT_DOMAIN) != 0) {
			/* last part is simply the original name, but don't copy the root domain twice! */
			substrings[i++] = g_strdup(domain_name);
		}
	} while (occurrence != NULL);

	/* add trailing NULL, so callers know when to stop */
	substrings[i] = NULL;

	if (len) {
		*len = i;
	}

	return substrings;
}

int chassis_log_set_default(chassis_log_t *log, const char *log_filename, GLogLevelFlags log_lvl) {
	chassis_log_backend_t *backend;
	chassis_log_domain_t *domain;

	if (log_filename) {
		backend = chassis_log_backend_file_new(log_filename);
	} else {
		backend = chassis_log_backend_stderr_new();
	}
	chassis_log_register_backend(log, backend);

	domain = chassis_log_domain_new(CHASSIS_LOG_DEFAULT_DOMAIN, log_lvl, backend);
	chassis_log_register_domain(log, domain);

	return 0;
}


