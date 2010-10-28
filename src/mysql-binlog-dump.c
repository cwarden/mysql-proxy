/* $%BEGINLICENSE%$
 Copyright (c) 2008, 2009, Oracle and/or its affiliates. All rights reserved.

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
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <glib/gstdio.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>

#include "glib-ext.h"
#include "chassis-mainloop.h"
#include "chassis-log.h"
#include "chassis-keyfile.h"
#include "network-mysqld-proto.h"
#include "network-mysqld-myisam.h"
#include "network-mysqld-binlog.h"

#define S(x) x->str, x->len

void network_mysqld_table_print(network_mysqld_table *tbl) {
	GString *out = g_string_new(NULL);
	guint i;

	g_string_append_printf(out, "CREATE TABLE %s.%s (\n",
			tbl->db_name->str,
			tbl->table_name->str);

	for (i = 0; i < tbl->columns->len; i++) {
		network_mysqld_column *field = tbl->columns->pdata[i];

		if (i > 0) {
			g_string_append(out, ",\n");
		}

		switch ((guchar)field->type) {
		case MYSQL_TYPE_TINY:
		case MYSQL_TYPE_SHORT:
		case MYSQL_TYPE_INT24:
		case MYSQL_TYPE_LONG:
			g_string_append_printf(out, "  field_%d %s %s NULL",
					i,
					network_mysqld_column_get_typestring(field),
					field->flags & NOT_NULL_FLAG ? "NOT" : "DEFAULT"
				 );
			break;
		case MYSQL_TYPE_DECIMAL:
		case MYSQL_TYPE_NEWDECIMAL:
			g_string_append_printf(out, "  field_%d %s(%lu, %u) %s NULL",
					i,
					network_mysqld_column_get_typestring(field),
					field->max_length, field->decimals,
					field->flags & NOT_NULL_FLAG ? "NOT" : "DEFAULT"
				 );
			break;
		default:
			g_string_append_printf(out, "  field_%d %s(%lu) %s NULL",
					i,
					network_mysqld_column_get_typestring(field),
					field->max_length,
					field->flags & NOT_NULL_FLAG ? "NOT" : "DEFAULT"
				 );
			break;
		}
	}
	g_string_append(out, "\n)");

	g_print("-- %s:\n%s\n\n",
			G_STRLOC, out->str);

	g_string_free(out, TRUE);
}

/**
 * decode a binlog event
 */
int network_mysqld_binlog_event_print(network_mysqld_binlog *binlog, 
		network_mysqld_binlog_event *event) {
	guint i;
	network_mysqld_table *tbl;
	int err = 0;
#if 0
	g_message("%s: timestamp = %u, type = %u, server-id = %u, size = %u, pos = %u, flags = %04x",
			G_STRLOC,
			event->timestamp,
			event->event_type,
			event->server_id,
			event->event_size,
			event->log_pos,
			event->flags);
#endif

	switch (event->event_type) {
	case QUERY_EVENT: /* 2 */
#if 0
		g_message("%s: QUERY: thread_id = %d, exec_time = %d, error-code = %d\ndb = %s, query = %s",
				G_STRLOC,
				event->event.query_event.thread_id,
				event->event.query_event.exec_time,
				event->event.query_event.error_code,
				event->event.query_event.db_name ? event->event.query_event.db_name : "(null)",
				event->event.query_event.query ? event->event.query_event.query : "(null)"
			 );
#else
		g_print("-- %s: db = %s\n%s\n\n",
				G_STRLOC,
				event->event.query_event.db_name ? event->event.query_event.db_name : "(null)",
				event->event.query_event.query ? event->event.query_event.query : "(null)"
			 );

#endif
		break;
	case STOP_EVENT:
		break;
	case TABLE_MAP_EVENT:
		tbl = network_mysqld_table_new();

		network_mysqld_binlog_event_tablemap_to_table(event, tbl);
	
		g_hash_table_insert(binlog->rbr_tables, guint64_new(tbl->table_id), tbl);

		network_mysqld_table_print(tbl);
		break; 
	case FORMAT_DESCRIPTION_EVENT: /* 15 */
		break;
	case INTVAR_EVENT: /* 5 */
	 	break;
	case XID_EVENT: /* 16 */
		break;
	case ROTATE_EVENT: /* 4 */
		break;
	case WRITE_ROWS_EVENT:
	case UPDATE_ROWS_EVENT:
	case DELETE_ROWS_EVENT: {
		network_packet row_packet;
		GString row;
		tbl = g_hash_table_lookup(binlog->rbr_tables, &(event->event.row_event.table_id));

		if (!tbl) {
			g_critical("%s: table-id: %"G_GUINT64_FORMAT" isn't known, needed for a %d event",
					G_STRLOC,
					event->event.row_event.table_id,
					event->event_type
					);
			break;
		}

		row.str = event->event.row_event.row->str;
		row.len = event->event.row_event.row->len;

		row_packet.data = &row;
		row_packet.offset = 0;
#if 0
		g_debug_hexdump(G_STRLOC " (used colums)", event->event.row_event.used_columns, event->event.row_event.used_columns_len);
#endif

		do {
			network_mysqld_myisam_row *pre_fields, *post_fields = NULL;
			GString *out = g_string_new(NULL);
			gchar *post_bits = NULL, *pre_bits;

			err = err || network_mysqld_proto_get_string_len(
					&row_packet, 
					&pre_bits,
					event->event.row_event.null_bits_len);

			if (err) break;

			pre_fields = network_mysqld_myisam_row_new();

			err = err || network_mysqld_myisam_row_init(pre_fields,
					tbl, 
					pre_bits, 
					event->event.row_event.null_bits_len);

			err = err || network_mysqld_proto_get_myisam_row(&row_packet, pre_fields);

			if (event->event_type == UPDATE_ROWS_EVENT) {
				err = err || network_mysqld_proto_get_string_len(
						&row_packet, 
						&post_bits,
						event->event.row_event.null_bits_len);
		
				post_fields = network_mysqld_myisam_row_new();

				err = err || network_mysqld_myisam_row_init(post_fields,
						tbl,
						post_bits, 
						event->event.row_event.null_bits_len);
				err = err || network_mysqld_proto_get_myisam_row(&row_packet, post_fields);
			}

			/* call lua */

			switch (event->event_type) {
			case UPDATE_ROWS_EVENT:
				g_string_append_printf(out, "UPDATE %s.%s\n   SET ",
						tbl->db_name->str,
						tbl->table_name->str);

				for (i = 0; i < post_fields->fields->len; i++) {
					network_mysqld_myisam_field *field = post_fields->fields->pdata[i];
					if (i > 0) {
						g_string_append_printf(out, ", ");
					}
					if (field->is_null) {
						g_string_append_printf(out, "field_%d = NULL", i);
					} else {
						switch((guchar)field->column->type) {
						case MYSQL_TYPE_DATE:
						case MYSQL_TYPE_TIMESTAMP:
						case MYSQL_TYPE_DATETIME:

						case MYSQL_TYPE_TINY:
						case MYSQL_TYPE_SHORT:
						case MYSQL_TYPE_INT24:
						case MYSQL_TYPE_LONG:
						case MYSQL_TYPE_ENUM:
							g_string_append_printf(out, "field_%d = %"G_GUINT64_FORMAT, i, field->data.i);
							break;
						case MYSQL_TYPE_VARCHAR:
						case MYSQL_TYPE_VAR_STRING:
						case MYSQL_TYPE_STRING:
							g_string_append_printf(out, "field_%d = '%s'", i, field->data.s ? field->data.s : "");
							break;
						case MYSQL_TYPE_BLOB:
							g_string_append_printf(out, "field_%d = '...(blob)'", i);
							break;
						case MYSQL_TYPE_NEWDECIMAL:
							g_string_append_printf(out, "'...(decimal)'");
							break;
						default:
							g_error("%s: field-type %d isn't known",
									G_STRLOC,
									field->column->type);
							break;
						}
					}
				}

				g_string_append_printf(out, "\n WHERE ");
				for (i = 0; i < pre_fields->fields->len; i++) {
					network_mysqld_myisam_field *field = pre_fields->fields->pdata[i];
					if (i > 0) {
						g_string_append_printf(out, " AND ");
					}
					if (field->is_null) {
						g_string_append_printf(out, "field_%d IS NULL", i);
					} else {
						switch((guchar)field->column->type) {
						case MYSQL_TYPE_TIMESTAMP:
						case MYSQL_TYPE_DATE:
						case MYSQL_TYPE_DATETIME:

						case MYSQL_TYPE_TINY:
						case MYSQL_TYPE_SHORT:
						case MYSQL_TYPE_INT24:
						case MYSQL_TYPE_LONG:
						case MYSQL_TYPE_ENUM:
							g_string_append_printf(out, "field_%d = %"G_GUINT64_FORMAT, i, field->data.i);
							break;
						case MYSQL_TYPE_VARCHAR:
						case MYSQL_TYPE_VAR_STRING:
						case MYSQL_TYPE_STRING:
							g_string_append_printf(out, "field_%d = '%s'", i, field->data.s ? field->data.s : "");
							break;
						case MYSQL_TYPE_BLOB:
							g_string_append_printf(out, "field_%d = '...(blob)'", i);
							break;
						case MYSQL_TYPE_NEWDECIMAL:
							g_string_append_printf(out, "'...(decimal)'");
							break;
						default:
							g_error("%s: field-type %d isn't known",
									G_STRLOC,
									field->column->type);
							break;
						}
					}
				}
				break;
			case WRITE_ROWS_EVENT:
				g_string_append_printf(out, "INSERT INTO %s.%s VALUES\n  (",
						tbl->db_name->str,
						tbl->table_name->str);

				for (i = 0; i < pre_fields->fields->len; i++) {
					network_mysqld_myisam_field *field = pre_fields->fields->pdata[i];
					if (i > 0) {
						g_string_append_printf(out, ", ");
					}
					if (field->is_null) {
						g_string_append(out, "NULL");
					} else {
						switch((guchar)field->column->type) {
						case MYSQL_TYPE_TIMESTAMP:
						case MYSQL_TYPE_DATE:
						case MYSQL_TYPE_DATETIME:

						case MYSQL_TYPE_TINY:
						case MYSQL_TYPE_SHORT:
						case MYSQL_TYPE_INT24:
						case MYSQL_TYPE_LONG:
						case MYSQL_TYPE_ENUM:
							g_string_append_printf(out, "%"G_GUINT64_FORMAT, field->data.i);
							break;
						case MYSQL_TYPE_VARCHAR:
						case MYSQL_TYPE_VAR_STRING:
						case MYSQL_TYPE_STRING:
							g_string_append_printf(out, "'%s'", field->data.s ? field->data.s : "");
							break;
						case MYSQL_TYPE_BLOB:
							g_string_append_printf(out, "'...(blob)'");
							break;
						case MYSQL_TYPE_NEWDECIMAL:
							g_string_append_printf(out, "'...(decimal)'");
							break;
						default:
							g_error("%s: field-type %d isn't known",
									G_STRLOC,
									field->column->type);
							break;
						}
					}
				}

				g_string_append_printf(out, ")");
				break;
			case DELETE_ROWS_EVENT:
				g_string_append_printf(out, "DELETE FROM %s.%s\n WHERE ",
						tbl->db_name->str,
						tbl->table_name->str);

				for (i = 0; i < pre_fields->fields->len; i++) {
					network_mysqld_myisam_field *field = pre_fields->fields->pdata[i];
					if (i > 0) {
						g_string_append_printf(out, " AND ");
					}
					if (field->is_null) {
						g_string_append_printf(out, "field_%d IS NULL", i);
					} else {
						switch((guchar)field->column->type) {
						case MYSQL_TYPE_TIMESTAMP:
						case MYSQL_TYPE_DATE:
						case MYSQL_TYPE_DATETIME:

						case MYSQL_TYPE_TINY:
						case MYSQL_TYPE_SHORT:
						case MYSQL_TYPE_INT24:
						case MYSQL_TYPE_LONG:
						case MYSQL_TYPE_ENUM:
							g_string_append_printf(out, "field_%d = %"G_GUINT64_FORMAT, i, field->data.i);
							break;
						case MYSQL_TYPE_VARCHAR:
						case MYSQL_TYPE_VAR_STRING:
						case MYSQL_TYPE_STRING:
							g_string_append_printf(out, "field_%d = '%s'", i, field->data.s ? field->data.s : "");
							break;
						case MYSQL_TYPE_BLOB:
							g_string_append_printf(out, "field_%d = '...(blob)'", i);
							break;
						case MYSQL_TYPE_NEWDECIMAL:
							g_string_append_printf(out, "'...(decimal)'");
							break;
						default:
							g_error("%s: field-type %d isn't known",
									G_STRLOC,
									field->column->type);
							break;
						}
					}
				}
				break;

			default:
				break;
			}
#if 0
			g_print("-- %s:\n%s\n\n", G_STRLOC, out->str);
#endif

			g_string_free(out, TRUE);

			if (pre_fields) network_mysqld_myisam_row_free(pre_fields);
			if (post_fields) network_mysqld_myisam_row_free(post_fields);
			if (pre_bits) g_free(pre_bits);
			if (post_bits) g_free(post_bits);
		} while (row_packet.data->len > row_packet.offset);

		g_assert_cmpint(row_packet.data->len, ==, row_packet.offset);

		break; }
	default:
		g_message("%s: unknown event-type: %d",
				G_STRLOC,
				event->event_type);
		return -1;
	}
	return err ? -1 : 0;
}

/**
 * read a binlog file
 */
int replicate_binlog_dump_file(
		const char *filename, 
		gint startpos,
		gboolean find_startpos
		) {
	network_packet *packet;
	network_mysqld_binlog *binlog;
	network_mysqld_binlog_event *event;
	off_t binlog_pos;
	int round = 0;
	int ret = 0;

	binlog = network_mysqld_binlog_new();
	if (network_mysqld_binlog_open(binlog, filename, "r")) {
		return -1;
	}

	packet = network_packet_new();
	packet->data = g_string_new(NULL);
	g_string_set_size(packet->data, 19 + 1);

	binlog_pos = 4;

	if (startpos) {
		if (-1 == lseek(binlog->fd, startpos, SEEK_SET)) {
			g_critical("%s: lseek(%d) failed: %s", 
					G_STRLOC,
					startpos,
					g_strerror(errno)
					);
			g_return_val_if_reached(-1);
		}

		binlog_pos = startpos;
	}

	if (find_startpos) {
		/* check if the current binlog-pos is valid,
		 *
		 * if not, just skip a byte a retry until we found a valid header
		 * */
		while (0 == network_mysqld_binlog_read_event_header(binlog, packet)) {
			packet->data->str[packet->data->len] = '\0'; /* term the string */
			packet->offset = 0;

			g_assert_cmpint(packet->data->len, ==, 19);

			event = network_mysqld_binlog_event_new();
			network_mysqld_proto_get_binlog_event_header(packet, event);

			if (event->event_size < 19 ||
			    binlog_pos + event->event_size != event->log_pos) {
				if (-1 == lseek(binlog->fd, -18, SEEK_CUR)) {
					g_critical("%s: lseek(%d) failed: %s", 
							G_STRLOC,
							-18,
							g_strerror(errno)
							);
					g_return_val_if_reached(-1);
				}

				binlog_pos += 1;

				g_message("%s: --binlog-start-pos isn't valid, trying to sync at %ld (attempt: %d)", 
						G_STRLOC,
						binlog_pos,
						round++
						);
			} else {
				if (-1 == lseek(binlog->fd, -19, SEEK_CUR)) {
					g_critical("%s: lseek(%d) failed: %s", 
							G_STRLOC,
							-18,
							g_strerror(errno)
							);
					g_return_val_if_reached(-1);
				}

				network_mysqld_binlog_event_free(event);
				
				break;
			}
			network_mysqld_binlog_event_free(event);
		}
	} 

	packet->offset = 0;

	/* next are the events, without the mysql packet header */
	while (network_mysqld_binlog_read_event_header(binlog, packet)) {
		packet->data->str[packet->data->len] = '\0'; /* term the string */

		g_assert_cmpint(packet->data->len, ==, 19);
	
		event = network_mysqld_binlog_event_new();
		network_mysqld_proto_get_binlog_event_header(packet, event);

		if (event->event_size < 19 ||
		    binlog_pos + event->event_size != event->log_pos) {
			g_critical("%s: binlog-pos=%ld is invalid, you may want to start with --binlog-find-start-pos",
				G_STRLOC,
				binlog_pos
			       );
			ret = -1;
			break;
		}

		g_print("-- %s: (--binlog-start-pos=%ld (next event at %"G_GUINT32_FORMAT")) event = %s (%d)\n",
				G_STRLOC,
				binlog_pos,
				event->log_pos,
				network_mysqld_binlog_event_get_name(event),
				event->event_type
				);
	
		binlog_pos += 19;

		network_mysqld_binlog_read_event(binlog, packet, event->event_size);
		
		if (network_mysqld_proto_get_binlog_event(packet, binlog, event)) {
			g_debug_hexdump(G_STRLOC, packet->data->str + 19, packet->data->len - 19);
		} else if (network_mysqld_binlog_event_print(binlog, event)) {
			g_debug_hexdump(G_STRLOC, packet->data->str + 19, packet->data->len - 19);
			/* ignore it */
		}
	
		binlog_pos += event->event_size;
		network_mysqld_binlog_event_free(event);

		packet->offset = 0;
	}
	g_string_free(packet->data, TRUE);
	network_packet_free(packet);

	network_mysqld_binlog_free(binlog);

	return ret;
}

#define GETTEXT_PACKAGE "mysql-binlog-dump"

int main(int argc, char **argv) {
	chassis *chas;
	
	/* read the command-line options */
	GOptionContext *option_ctx;
	GError *gerr = NULL;
	guint i;
	int exit_code = EXIT_SUCCESS;
	int print_version = 0;
	const gchar *check_str = NULL;
	gchar *default_file = NULL;

	gchar *log_level_str = NULL;
	gchar *log_filename = NULL;
	gchar *binlog_filename = NULL;

	GKeyFile *keyfile = NULL;
	chassis_log_t *log;
	GLogLevelFlags log_lvl = G_LOG_LEVEL_CRITICAL;
	gint binlog_start_pos = 0;
	gboolean binlog_find_start_pos = FALSE;

	/* can't appear in the configfile */
	GOptionEntry base_main_entries[] = 
	{
		{ "version",                 'V', 0, G_OPTION_ARG_NONE, NULL, "Show version", NULL },
		{ "defaults-file",            0, 0, G_OPTION_ARG_STRING, NULL, "configuration file", "<file>" },
		
		{ NULL,                       0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
	};

	GOptionEntry main_entries[] = 
	{
		{ "log-level",                0, 0, G_OPTION_ARG_STRING, NULL, "log all messages of level ... or higer", "(error|warning|message|debug)" },
		{ "log-file",                 0, 0, G_OPTION_ARG_STRING, NULL, "log all messages in a file", "<file>" },
		
		{ "binlog-file",              0, 0, G_OPTION_ARG_FILENAME, NULL, "binlog filename", NULL },
		{ "binlog-start-pos",         0, 0, G_OPTION_ARG_INT, NULL, "binlog start position", NULL },
		{ "binlog-find-start-pos",    0, 0, G_OPTION_ARG_NONE, NULL, "find binlog start position", NULL },
		
		{ NULL,                       0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
	};

	if (!GLIB_CHECK_VERSION(2, 6, 0)) {
		g_error("the glib header are too old, need at least 2.6.0, got: %d.%d.%d", 
				GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
	}

	check_str = glib_check_version(GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

	if (check_str) {
		g_error("%s, got: lib=%d.%d.%d, headers=%d.%d.%d", 
			check_str,
			glib_major_version, glib_minor_version, glib_micro_version,
			GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
	}

#if defined(HAVE_LUA_H)
# if defined(DATADIR)
	/**
	 * if the LUA_PATH or LUA_CPATH are not set, set a good default 
	 */
	if (!g_getenv(LUA_PATH)) {
		g_setenv(LUA_PATH, 
				DATADIR "/?.lua", 1);
	}
# endif

# if defined(LIBDIR)
	if (!g_getenv(LUA_CPATH)) {
#  if _WIN32
		g_setenv(LUA_CPATH, 
				LIBDIR "/?.dll", 1);
#  else
		g_setenv(LUA_CPATH, 
				LIBDIR "/?.so", 1);
#  endif
	}
# endif
#endif

	g_thread_init(NULL);

	log = chassis_log_new();
	chassis_log_set_default(log, NULL, log_lvl); /* default to stderr for everything that is critical */
	g_log_set_default_handler(chassis_log_func, log);

	chas = chassis_new();

	i = 0;
	base_main_entries[i++].arg_data  = &(print_version);
	base_main_entries[i++].arg_data  = &(default_file);

	i = 0;
	main_entries[i++].arg_data  = &(log_level_str);
	main_entries[i++].arg_data  = &(log_filename);
	main_entries[i++].arg_data  = &(binlog_filename);
	main_entries[i++].arg_data  = &(binlog_start_pos);
	main_entries[i++].arg_data  = &(binlog_find_start_pos);

	option_ctx = g_option_context_new("- MySQL Binlog Dump");
	g_option_context_add_main_entries(option_ctx, base_main_entries, GETTEXT_PACKAGE);
	g_option_context_add_main_entries(option_ctx, main_entries, GETTEXT_PACKAGE);
	g_option_context_set_help_enabled(option_ctx, TRUE);

	/**
	 * parse once to get the basic options like --defaults-file and --version
	 *
	 * leave the unknown options in the list
	 */
	if (FALSE == g_option_context_parse(option_ctx, &argc, &argv, &gerr)) {
		g_critical("%s", gerr->message);
		
		exit_code = EXIT_FAILURE;
		goto exit_nicely;
	}

	if (default_file) {
		keyfile = g_key_file_new();
		g_key_file_set_list_separator(keyfile, ',');

		if (FALSE == g_key_file_load_from_file(keyfile, default_file, G_KEY_FILE_NONE, &gerr)) {
			g_critical("loading configuration from %s failed: %s", 
					default_file,
					gerr->message);

			exit_code = EXIT_FAILURE;
			goto exit_nicely;
		}
	}

	if (print_version) {
		printf("%s\r\n", PACKAGE_STRING); 
		printf("  glib2: %d.%d.%d\r\n", GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

		exit_code = EXIT_SUCCESS;
		goto exit_nicely;
	}


	/* add the other options which can also appear in the configfile */
	g_option_context_add_main_entries(option_ctx, main_entries, GETTEXT_PACKAGE);

	/**
	 * parse once to get the basic options 
	 *
	 * leave the unknown options in the list
	 */
	if (FALSE == g_option_context_parse(option_ctx, &argc, &argv, &gerr)) {
		g_critical("%s", gerr->message);

		exit_code = EXIT_FAILURE;
		goto exit_nicely;
	}

	if (NULL != keyfile) {
		if (chassis_keyfile_to_options(keyfile, "mysql-binlog-dump", main_entries)) {
			exit_code = EXIT_FAILURE;
			goto exit_nicely;
		}
	}

	if (NULL != log_level_str) {
		log_lvl = chassis_log_level_string_to_level(log_level_str);
		if (0 == log_lvl) {
			g_critical("--log-level=... failed, level '%s' is unknown ", log_level_str);

			exit_code = EXIT_FAILURE;
			goto exit_nicely;
		}
	}

	if (0 != chassis_log_set_default(log, log_filename, log_lvl)) {
		g_critical("can't open log-file '%s': %s", log_filename, g_strerror(errno));

		exit_code = EXIT_FAILURE;
		goto exit_nicely;
	}


	if (NULL == binlog_filename) {
		exit_code = EXIT_FAILURE;
		goto exit_nicely;
	}

	replicate_binlog_dump_file(
			binlog_filename,
			binlog_start_pos,
			binlog_find_start_pos
			);

exit_nicely:
	if (NULL != option_ctx) g_option_context_free(option_ctx);
	if (NULL != keyfile) g_key_file_free(keyfile);
	if (NULL != default_file) g_free(default_file);
	if (NULL != binlog_filename) g_free(binlog_filename);
	if (NULL != gerr) g_error_free(gerr);

	if (NULL != log_level_str) g_free(log_level_str);
	if (NULL != log_filename) g_free(log_filename);
	if (NULL != chas) chassis_free(chas);
	
	chassis_log_free(log);

	return exit_code;
}


