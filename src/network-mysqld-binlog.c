/* $%BEGINLICENSE%$
 Copyright (C) 2008 MySQL AB, 2008 Sun Microsystems, Inc

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
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

/**
 * replication 
 */
#include "glib-ext.h"
#include "network-mysqld-binlog.h"

#define S(x) x->str, x->len

static guint guint64_hash(gconstpointer _key) {
	const guint64 *key = _key;

	return *key & 0xffffffff;
}

static gboolean guint64_equal(gconstpointer _a, gconstpointer _b) {
	const guint64 *a = _a;
	const guint64 *b = _b;

	return *a == *b;
}

guint64 *guint64_new(guint64 i) {
	guint64 *ip;

	ip = g_new0(guint64, 1);
	*ip = i;

	return ip;
}

network_mysqld_binlog *network_mysqld_binlog_new() {
	network_mysqld_binlog *binlog;

	binlog = g_new0(network_mysqld_binlog, 1);
	binlog->rbr_tables = g_hash_table_new_full(
			guint64_hash,
			guint64_equal,
			g_free,
			(GDestroyNotify)network_mysqld_table_free);

	return binlog;
}

void network_mysqld_binlog_free(network_mysqld_binlog *binlog) {
	if (!binlog) return;

	g_hash_table_destroy(binlog->rbr_tables);

	g_free(binlog);
}

/**
 * open a binlog-file for reading or writing 
 *
 * @param mode 'r' or 'w'
 */
int network_mysqld_binlog_open(network_mysqld_binlog *binlog, const char *filename, const char *mode_str) {
	char binlog_header[4];

	if (0 == strcmp(mode_str, "r")) {
		binlog->mode = BINLOG_MODE_READ;
	} else if (0 == strcmp(mode_str, "w")) {
		binlog->mode = BINLOG_MODE_WRITE;
	} else {
		return -1;
	}

	if (binlog->mode == BINLOG_MODE_READ) {
		if (-1 == (binlog->fd = g_open(filename, O_RDONLY, 0))) {
			g_critical("%s: opening '%s' failed: %s",
					G_STRLOC,
					filename,
					g_strerror(errno));
			return -1;
		}

		if (4 != read(binlog->fd, binlog_header, 4)) {
			g_return_val_if_reached(-1);
		}

		if (binlog_header[0] != '\xfe' ||
		    binlog_header[1] != 'b' ||
		    binlog_header[2] != 'i' ||
		    binlog_header[3] != 'n') {

			g_critical("%s: binlog-header should be: %02x%02x%02x%02x, got %02x%02x%02x%02x",
					G_STRLOC,
					'\xfe', 'b', 'i', 'n',
					binlog_header[0],
					binlog_header[1],
					binlog_header[2],
					binlog_header[3]
					);

			g_return_val_if_reached(-1);
		}
	} else {
		if (-1 == (binlog->fd = g_open(filename, O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR))) {
			g_critical("%s: opening '%s' failed: %s",
					G_STRLOC,
					filename,
					g_strerror(errno));
			return -1;
		}
		binlog_header[0] = '\xfe';
		binlog_header[1] = 'b';
		binlog_header[2] = 'i';
		binlog_header[3] = 'n';

		if (4 != write(binlog->fd, binlog_header, 4)) {
			g_return_val_if_reached(-1);
		}
		binlog->log_pos = 4;
	}

	return 0;
}

int network_mysqld_binlog_append(network_mysqld_binlog *binlog, network_mysqld_binlog_event *event) {
	GString *packet, *header;
	ssize_t r;

	if (binlog->mode != BINLOG_MODE_WRITE) {
		g_critical("%s: trying to append to a read-only binlog stream", G_STRLOC);
		return -1;
	}

	packet = g_string_new(NULL);
	if (network_mysqld_proto_append_binlog_event(packet, event)) {
		g_critical("%s", G_STRLOC);
		g_string_free(packet, TRUE);
		return -1;
	}
	
	event->event_size = 19 /* the header */ + packet->len;
	event->log_pos = binlog->log_pos + event->event_size;
	
	header = g_string_new(NULL);
	if (network_mysqld_proto_append_binlog_event_header(header, event)) {
		g_critical("%s", G_STRLOC);
		g_string_free(packet, TRUE);
		g_string_free(header, TRUE);
		return -1;
	}
	
	r = write(binlog->fd, header->str, header->len);
	if (r < 0) {
		g_critical("%s", G_STRLOC);
		return -1;
	} else if (r < header->len) {
		g_critical("%s: wrote = %d, wanted to write: %d", 
				G_STRLOC,
				r,
				header->len);
		return -1;
	}
	
	g_string_free(header, TRUE);

	r = write(binlog->fd, packet->str, packet->len);

	g_string_free(packet, TRUE);

	if (r < 0) {
		g_critical("%s", G_STRLOC);
		return -1;
	} else if (r < packet->len) {
		g_critical("%s: wrote = %d, wanted to write: %d", 
				G_STRLOC,
				r,
				packet->len);
		return -1;
	}
	binlog->log_pos = event->log_pos;

	return 0;
}

int network_mysqld_binlog_read_event_header(network_mysqld_binlog *binlog, network_packet *packet) {
	if (19 == (packet->data->len = read(binlog->fd, packet->data->str, 19))) {
		return 0;
	} else {
		return -1;
	}
}

int network_mysqld_binlog_read_event(network_mysqld_binlog *binlog, 
		network_packet *packet,
		goffset event_size) {
	gssize len;

	g_string_set_size(packet->data, event_size); /* resize the string */
	packet->data->len = 19;

	len = read(binlog->fd, packet->data->str + 19, event_size - 19);

	if (-1 == len) {
		g_critical("%s: read(..., %d, ...) failed: %s",
				G_STRLOC,
				event_size - 19,
				g_strerror(errno));
		return -1;
	}
	g_assert_cmpint(len, ==, event_size - 19); /* read error */

	g_assert_cmpint(packet->data->len, ==, 19);
	packet->data->len += len;
	g_assert_cmpint(packet->data->len, ==, event_size);

	return 0;
}

network_mysqld_binlog_event *network_mysqld_binlog_event_new() {
	network_mysqld_binlog_event *binlog;

	binlog = g_new0(network_mysqld_binlog_event, 1);

	return binlog;
}

int network_mysqld_proto_get_binlog_status(network_packet *packet) {
	guint8 ok;

	/* on the network we have a length and packet-number of 4 bytes */
	if (0 != network_mysqld_proto_get_int8(packet, &ok)) {
		return -1;
	}
	g_return_val_if_fail(ok == 0, -1);

	return 0;
}

int network_mysqld_proto_get_binlog_event_header(network_packet *packet, network_mysqld_binlog_event *event) {
	int err = 0;

	err = err || network_mysqld_proto_get_int32(packet, &event->timestamp);
	err = err || network_mysqld_proto_get_int8(packet,  (guint8 *)&event->event_type); /* map a enum to a guint8 */
	err = err || network_mysqld_proto_get_int32(packet, &event->server_id);
	err = err || network_mysqld_proto_get_int32(packet, &event->event_size);
	err = err || network_mysqld_proto_get_int32(packet, &event->log_pos);
	err = err || network_mysqld_proto_get_int16(packet, &event->flags);

	return err ? -1 : 0;
}

int network_mysqld_proto_append_binlog_event_header(GString *packet, network_mysqld_binlog_event *event) {
	/* write the normal event header */
	network_mysqld_proto_append_int32(packet, event->timestamp);
	network_mysqld_proto_append_int8(packet, (guint8)event->event_type);
	network_mysqld_proto_append_int32(packet, event->server_id);
	network_mysqld_proto_append_int32(packet, event->event_size);
	network_mysqld_proto_append_int32(packet, event->log_pos);
	network_mysqld_proto_append_int16(packet, event->flags);

	return 0;
}

int network_mysqld_proto_append_binlog_event(GString *packet, network_mysqld_binlog_event *event) {
	guint i;
	
	switch ((guchar)event->event_type) {
	case FORMAT_DESCRIPTION_EVENT:
		g_return_val_if_fail (event->event.format_event.binlog_version == 4, -1);
		g_return_val_if_fail (event->event.format_event.master_version != NULL, -1);
		g_return_val_if_fail (event->event.format_event.event_header_sizes_len != 0, -1);
		g_return_val_if_fail (event->event.format_event.log_header_len == 19, -1);

		network_mysqld_proto_append_int16(packet, event->event.format_event.binlog_version); /* should be 4 */

		/* the master-version is a 50byte char-string
		 *
		 * if the string is shorter, the rest has to be filled up with 0x00
		 */
		i = strlen(event->event.format_event.master_version);
		g_return_val_if_fail(i > 0, -1);

		g_string_append_len( /* NUL-term string */
				packet, 
				event->event.format_event.master_version,
				i);
		while (i++ < 50) g_string_append_c(packet, 0x00);

		network_mysqld_proto_append_int32(packet, 
				event->event.format_event.created_ts);

		network_mysqld_proto_append_int8(packet, event->event.format_event.log_header_len);

		/* the event headers */
		for (i = 0; i < event->event.format_event.event_header_sizes_len; i++) {
			network_mysqld_proto_append_int8(packet, event->event.format_event.event_header_sizes[i]);
		}

		break;
	case QUERY_EVENT:
		g_return_val_if_fail (event->event.query_event.query != NULL, -1);

		network_mysqld_proto_append_int32(packet, event->event.query_event.thread_id);
		network_mysqld_proto_append_int32(packet, event->event.query_event.exec_time);
		network_mysqld_proto_append_int8(packet, event->event.query_event.db_name_len);
		network_mysqld_proto_append_int16(packet, event->event.query_event.error_code);

		network_mysqld_proto_append_int16(packet, 0); /* var-size attributes */
		if (event->event.query_event.db_name_len) {
			g_string_append_len(packet, event->event.query_event.db_name, event->event.query_event.db_name_len);
		}
		g_string_append_c(packet, '\0');
		g_string_append(packet, event->event.query_event.query);

		break;
	case ROTATE_EVENT:
		network_mysqld_proto_append_int32(packet, event->event.rotate_event.binlog_pos);
		g_string_append_c(packet, '\0');
		g_string_append_c(packet, '\0');
		g_string_append_c(packet, '\0');
		g_string_append_c(packet, '\0');
		g_string_append_len(
				packet, 
				event->event.rotate_event.binlog_file,
				strlen(event->event.rotate_event.binlog_file));
		break;
	case STOP_EVENT:
		/* no data to write */
		break;
	case XID_EVENT:
		network_mysqld_proto_append_int64(packet, event->event.xid.xid_id);
		break;
	case INTVAR_EVENT:
		network_mysqld_proto_append_int8(packet, event->event.intvar.type);
		network_mysqld_proto_append_int64(packet, event->event.intvar.value);

		break;
	case USER_VAR_EVENT:
		network_mysqld_proto_append_int32(packet,
				event->event.user_var_event.name_len);
		g_string_append_len(
				packet,
				event->event.user_var_event.name,
				event->event.user_var_event.name_len);

		network_mysqld_proto_append_int8(packet,
				event->event.user_var_event.is_null);
		network_mysqld_proto_append_int8(packet,
				event->event.user_var_event.type );
		network_mysqld_proto_append_int32(packet,
				event->event.user_var_event.charset);
		network_mysqld_proto_append_int32(packet,
				event->event.user_var_event.value_len);
		g_string_append_len(
				packet,
				event->event.user_var_event.value,
				event->event.user_var_event.value_len);

		break;
	case INCIDENT_EVENT:
		network_mysqld_proto_append_int16(packet,
				event->event.incident.incident_id);
		network_mysqld_proto_append_int8(packet,
				event->event.incident.message_len);
		g_string_append_len(packet, 
				event->event.incident.message, 
				event->event.incident.message_len);

		break;
	case TABLE_MAP_EVENT:
		/**
		 * looks like a abstract definition of a table 
		 *
		 * no, we don't want to know
		 */
		network_mysqld_proto_append_int48(packet,
				event->event.table_map_event.table_id); /* 6 bytes */
		network_mysqld_proto_append_int16(packet,
				event->event.table_map_event.flags);

		network_mysqld_proto_append_int8(packet,
				event->event.table_map_event.db_name_len);
		
		g_string_append_len(
				packet,
				event->event.table_map_event.db_name,
				event->event.table_map_event.db_name_len);
		g_string_append_c(packet, '\0');

		network_mysqld_proto_append_int8(packet,
				event->event.table_map_event.table_name_len);
		g_string_append_len(
				packet,
				event->event.table_map_event.table_name,
				event->event.table_map_event.table_name_len);
		g_string_append_c(packet, '\0');

		network_mysqld_proto_append_lenenc_int(packet,
				event->event.table_map_event.columns_len);
		g_string_append_len(
				packet,
				event->event.table_map_event.columns,
				event->event.table_map_event.columns_len);

		network_mysqld_proto_append_lenenc_int(packet,
				event->event.table_map_event.metadata_len);
		g_string_append_len(
				packet,
				event->event.table_map_event.metadata,
				event->event.table_map_event.metadata_len);

		/**
		 * the null-bit count is columns/8 
		 */

		event->event.table_map_event.null_bits_len = (int)((event->event.table_map_event.columns_len+7)/8);
		g_string_append_len(
				packet,
				event->event.table_map_event.null_bits,
				event->event.table_map_event.null_bits_len);

		break;
	case DELETE_ROWS_EVENT: /* 25 */
	case UPDATE_ROWS_EVENT: /* 24 */
	case WRITE_ROWS_EVENT:  /* 23 */
		network_mysqld_proto_append_int48(packet,
				event->event.row_event.table_id); /* 6 bytes */
		network_mysqld_proto_append_int16(packet,
				event->event.row_event.flags );
		
		network_mysqld_proto_append_lenenc_int(packet,
				event->event.row_event.columns_len);

		/* a bit-mask of used-fields (m_cols.bitmap) */
		event->event.row_event.used_columns_len = (int)((event->event.row_event.columns_len+7)/8);
		g_string_append_len(
				packet,
				event->event.row_event.used_columns,
				event->event.row_event.used_columns_len);

		if (event->event_type == UPDATE_ROWS_EVENT) {
			/* the before image */
			g_string_append_len(
					packet,
					event->event.row_event.used_columns,
					event->event.row_event.used_columns_len);

		}

		/* null-bits for all the columns */
		event->event.row_event.null_bits_len = (int)((event->event.row_event.columns_len+7)/8);

		/* the null-bits + row,
		 *
		 * the rows are stored in field-format, to decode we have to see
		 * the table description
		 */
		g_string_append_len(
				packet,
				S(event->event.row_event.row));
		
		break;

	default:
		g_critical("%s: don't know how to write binlog-event %d (%s) yet", 
				G_STRLOC,
				event->event_type,
				network_mysqld_binlog_event_get_name(event));
		return -1;
	}

	return 0;
}

int network_mysqld_proto_get_binlog_event(network_packet *packet, 
		network_mysqld_binlog G_GNUC_UNUSED *binlog,
		network_mysqld_binlog_event *event) {

	int err = 0;

	switch ((guchar)event->event_type) {
	case QUERY_EVENT:
		err = err || network_mysqld_proto_get_int32(packet, &event->event.query_event.thread_id);
		err = err || network_mysqld_proto_get_int32(packet, &event->event.query_event.exec_time);
		err = err || network_mysqld_proto_get_int8(packet, &event->event.query_event.db_name_len);
		err = err || network_mysqld_proto_get_int16(packet, &event->event.query_event.error_code);

		/* 5.0 has more flags */
		if (packet->data->len > packet->offset) {
			guint16 var_size = 0;

			err = err || network_mysqld_proto_get_int16(packet, &var_size);
			if (var_size) {
				/* skip the variable size part for now */
				err = err || network_mysqld_proto_skip(packet, var_size);
			}
	
			/* default db has <db_name_len> chars */
			err = err || network_mysqld_proto_get_string_len(packet, 
					&event->event.query_event.db_name,
					event->event.query_event.db_name_len);
			err = err || network_mysqld_proto_skip(packet, 1); /* the \0 */
	
			err = err || network_mysqld_proto_get_string_len(packet, 
					&event->event.query_event.query,
					packet->data->len - packet->offset);
		}

		break;
	case ROTATE_EVENT:
		err = err || network_mysqld_proto_get_int32(packet, &event->event.rotate_event.binlog_pos);
		err = err || network_mysqld_proto_skip(packet, 4);
		err = err || network_mysqld_proto_get_string_len(
				packet, 
				&event->event.rotate_event.binlog_file,
				packet->data->len - packet->offset);
		break;
	case STOP_EVENT:
		/* is empty */
		break;
	case FORMAT_DESCRIPTION_EVENT:
		err = err || network_mysqld_proto_get_int16(packet, &event->event.format_event.binlog_version);
		g_assert_cmpint(event->event.format_event.binlog_version, ==, 4);
		err = err || network_mysqld_proto_get_string_len( /* NUL-term string */
				packet, 
				&event->event.format_event.master_version,
				ST_SERVER_VER_LEN);
		err = err || network_mysqld_proto_get_int32(packet, 
				&event->event.format_event.created_ts);

		/* the header length may change in the future, for now we assume it is 19 */
		err = err || network_mysqld_proto_get_int8(packet, &event->event.format_event.log_header_len);
		g_assert_cmpint(event->event.format_event.log_header_len, ==, 19);

		/* fixed length of all events after the initial header
		 *
		 * for a FORMAT_DESCRIPTION_EVENT it is:
		 *   56 = 2 + 50 + 4
		 * */
		event->event.format_event.event_header_sizes_len = packet->data->len - packet->offset;
		err = err || network_mysqld_proto_get_string_len(
				packet, 
				&event->event.format_event.event_header_sizes,
				packet->data->len - packet->offset);

		break;
	case USER_VAR_EVENT:
		err = err || network_mysqld_proto_get_int32(
				packet,
				&event->event.user_var_event.name_len);
		err = err || network_mysqld_proto_get_string_len(
				packet,
				&event->event.user_var_event.name,
				event->event.user_var_event.name_len);

		err = err || network_mysqld_proto_get_int8(packet,
				&event->event.user_var_event.is_null);
		err = err || network_mysqld_proto_get_int8(packet,
				&event->event.user_var_event.type );
		err = err || network_mysqld_proto_get_int32(packet,
				&event->event.user_var_event.charset);
		err = err || network_mysqld_proto_get_int32(packet,
				&event->event.user_var_event.value_len);
		err = err || network_mysqld_proto_get_string_len(
				packet,
				&event->event.user_var_event.value,
				event->event.user_var_event.value_len);
		break;
	case TABLE_MAP_EVENT: /* 19 */
		/**
		 * looks like a abstract definition of a table 
		 *
		 * no, we don't want to know
		 */
		err = err || network_mysqld_proto_get_int48(packet,
				&event->event.table_map_event.table_id); /* 6 bytes */
		err = err || network_mysqld_proto_get_int16(packet,
				&event->event.table_map_event.flags);

		err = err || network_mysqld_proto_get_int8(packet,
				&event->event.table_map_event.db_name_len);
		
		err = err || network_mysqld_proto_get_string_len(
				packet,
				&event->event.table_map_event.db_name,
				event->event.table_map_event.db_name_len);
		err = err || network_mysqld_proto_skip(packet, 1); /* this should be NUL */

		err = err || network_mysqld_proto_get_int8(packet,
				&event->event.table_map_event.table_name_len);
		err = err || network_mysqld_proto_get_string_len(
				packet,
				&event->event.table_map_event.table_name,
				event->event.table_map_event.table_name_len);
		err = err || network_mysqld_proto_skip(packet, 1); /* this should be NUL */

		err = err || network_mysqld_proto_get_lenenc_int(packet,
				&event->event.table_map_event.columns_len);
		err = err || network_mysqld_proto_get_string_len(
				packet,
				&event->event.table_map_event.columns,
				event->event.table_map_event.columns_len);

		err = err || network_mysqld_proto_get_lenenc_int(packet,
				&event->event.table_map_event.metadata_len);
		err = err || network_mysqld_proto_get_string_len(
				packet,
				&event->event.table_map_event.metadata,
				event->event.table_map_event.metadata_len);

		/**
		 * the null-bit count is columns/8 
		 */

		event->event.table_map_event.null_bits_len = (int)((event->event.table_map_event.columns_len+7)/8);
		err = err || network_mysqld_proto_get_string_len(
				packet,
				&event->event.table_map_event.null_bits,
				event->event.table_map_event.null_bits_len);

		if (packet->data->len != packet->offset) { /* this should be the full packet */
			g_critical("%s: TABLE_MAP_EVENT at pos %u we still have %lu left", 
					G_STRLOC,
					packet->offset,
					packet->data->len - packet->offset);
			err = 1;
		}
		break;
	case DELETE_ROWS_EVENT: /* 25 */
	case UPDATE_ROWS_EVENT: /* 24 */
	case WRITE_ROWS_EVENT:  /* 23 */
		err = err || network_mysqld_proto_get_int48(packet,
				&event->event.row_event.table_id); /* 6 bytes */
		err = err || network_mysqld_proto_get_int16(packet,
				&event->event.row_event.flags );
		
		err = err || network_mysqld_proto_get_lenenc_int(packet,
				&event->event.row_event.columns_len);

		/* a bit-mask of used-fields (m_cols.bitmap) */
		event->event.row_event.used_columns_len = (int)((event->event.row_event.columns_len+7)/8);
		err = err || network_mysqld_proto_get_string_len(
				packet,
				&event->event.row_event.used_columns,
				event->event.row_event.used_columns_len);

		if (event->event_type == UPDATE_ROWS_EVENT) {
			/* the before image */
			err = err || network_mysqld_proto_skip(packet, event->event.row_event.used_columns_len);
		}

		/* null-bits for all the columns */
		event->event.row_event.null_bits_len = (int)((event->event.row_event.columns_len+7)/8);

		/* the null-bits + row,
		 *
		 * the rows are stored in field-format, to decode we have to see
		 * the table description
		 */
		if (!event->event.row_event.row) event->event.row_event.row = g_string_new(NULL);
		err = err || network_mysqld_proto_get_gstring_len(
				packet,
				packet->data->len - packet->offset,
				event->event.row_event.row);
		
		break;
	case INTVAR_EVENT:
		err = err || network_mysqld_proto_get_int8(packet,
				&event->event.intvar.type);
		err = err || network_mysqld_proto_get_int64(packet,
				&event->event.intvar.value);

		break;
	case XID_EVENT:
		err = err || network_mysqld_proto_get_int64(packet,
				&event->event.xid.xid_id);
		break;
	case INCIDENT_EVENT:
		err = err || network_mysqld_proto_get_int16(packet,
				&event->event.incident.incident_id);
		err = err || network_mysqld_proto_get_int8(packet,
				&event->event.incident.message_len);
		err = err || network_mysqld_proto_get_string_len(packet,
				&event->event.incident.message,
				event->event.incident.message_len);
		break;
	default:
		g_critical("%s: unhandled binlog-event: %d", 
				G_STRLOC, 
				event->event_type);
		return -1;
	}

	/* FIXME: we should check if we have handled all bytes */

	return err ? -1 : 0;
}

void network_mysqld_binlog_event_free(network_mysqld_binlog_event *event) {
	if (!event) return;

	switch (event->event_type) {
	case QUERY_EVENT:
		if (event->event.query_event.db_name) g_free(event->event.query_event.db_name);
		if (event->event.query_event.query) g_free(event->event.query_event.query);
		break;
	case ROTATE_EVENT:
		if (event->event.rotate_event.binlog_file) g_free(event->event.rotate_event.binlog_file);
		break;
	case FORMAT_DESCRIPTION_EVENT:
		if (event->event.format_event.master_version) g_free(event->event.format_event.master_version);
		if (event->event.format_event.event_header_sizes) g_free(event->event.format_event.event_header_sizes);
		break;
	case USER_VAR_EVENT:
		if (event->event.user_var_event.name) g_free(event->event.user_var_event.name);
		if (event->event.user_var_event.value) g_free(event->event.user_var_event.value);
		break;
	case TABLE_MAP_EVENT:
		if (event->event.table_map_event.db_name) g_free(event->event.table_map_event.db_name);
		if (event->event.table_map_event.table_name) g_free(event->event.table_map_event.table_name);
		if (event->event.table_map_event.columns) g_free(event->event.table_map_event.columns);
		if (event->event.table_map_event.metadata) g_free(event->event.table_map_event.metadata);
		if (event->event.table_map_event.null_bits) g_free(event->event.table_map_event.null_bits);
		break;
	case DELETE_ROWS_EVENT:
	case UPDATE_ROWS_EVENT:
	case WRITE_ROWS_EVENT:
		if (event->event.row_event.used_columns) g_free(event->event.row_event.used_columns);
		if (event->event.row_event.row) g_string_free(event->event.row_event.row, TRUE);
		break;
	default:
		break;
	}

	g_free(event);
}

struct {
	enum Log_event_type type;
	const char *name;
	size_t name_len;
} event_type_name[] = {
#define V(x) x, #x, sizeof(#x) - 1
	{ V(UNKNOWN_EVENT) },
	{ V(START_EVENT_V3) },
	{ V(QUERY_EVENT) },
	{ V(STOP_EVENT) },
	{ V(ROTATE_EVENT) },
	{ V(INTVAR_EVENT) },
	{ V(LOAD_EVENT) },
	{ V(SLAVE_EVENT) },
	{ V(CREATE_FILE_EVENT) },
	{ V(APPEND_BLOCK_EVENT) },
	{ V(EXEC_LOAD_EVENT) },
	{ V(DELETE_FILE_EVENT) },
	{ V(NEW_LOAD_EVENT) },
	{ V(RAND_EVENT) },
	{ V(USER_VAR_EVENT) },
	{ V(FORMAT_DESCRIPTION_EVENT) },
	{ V(XID_EVENT) },
	{ V(BEGIN_LOAD_QUERY_EVENT) },
	{ V(EXECUTE_LOAD_QUERY_EVENT) },
	{ V(TABLE_MAP_EVENT ) },
	{ V(PRE_GA_WRITE_ROWS_EVENT ) },
	{ V(PRE_GA_UPDATE_ROWS_EVENT ) },
	{ V(PRE_GA_DELETE_ROWS_EVENT ) },
	{ V(WRITE_ROWS_EVENT ) },
	{ V(UPDATE_ROWS_EVENT ) },
	{ V(DELETE_ROWS_EVENT ) },
	{ V(INCIDENT_EVENT) },

#undef V
	{ 0, NULL, 0 }
};

const char *network_mysqld_binlog_event_get_name(network_mysqld_binlog_event *event) {
	const char *unknown_type = event_type_name[0].name;
	enum Log_event_type type = event->event_type;
	guint i;

	for (i = 0; event_type_name[i].name; i++) {
		if ((guchar)event_type_name[i].type == (guchar)type) return event_type_name[i].name;
	}

	g_critical("%s: event-type %d isn't known yet", 
			G_STRLOC,
			type);

	return unknown_type;
}

/**
 * map a string back to a id 
 */
int network_mysqld_binlog_event_get_id(const char *key, size_t key_len) {
	guint i;

	for (i = 0; event_type_name[i].name; i++) {
		if (strleq(key, key_len, event_type_name[i].name, event_type_name[i].name_len)) return event_type_name[i].type;
	}

	return UNKNOWN_EVENT;
}

network_mysqld_binlog_dump *network_mysqld_binlog_dump_new() {
	network_mysqld_binlog_dump *dump;

	dump = g_new0(network_mysqld_binlog_dump, 1);

	return dump;
}

int network_mysqld_proto_append_binlog_dump(GString *packet, network_mysqld_binlog_dump *dump) {
	network_mysqld_proto_append_int8(packet, COM_BINLOG_DUMP);
	network_mysqld_proto_append_int32(packet, dump->binlog_pos);
	network_mysqld_proto_append_int16(packet, dump->flags); /* flags */
	network_mysqld_proto_append_int32(packet, dump->server_id);
	g_string_append(packet, dump->binlog_file); /* filename */
	network_mysqld_proto_append_int8(packet, 0); /* term-nul */

	return 0;
}

void network_mysqld_binlog_dump_free(network_mysqld_binlog_dump *dump) {
	if (!dump) return;

	g_free(dump);
}

/**
 * get the length of the meta-data for a given field-type
 *
 * ... to calc the right metadata-len of the table-map event 
 */
int network_mysqld_binlog_event_tablemap_get_type_len(enum enum_field_types type) {
	switch (type) {
	case MYSQL_TYPE_DATE:
	case MYSQL_TYPE_DATETIME:
	case MYSQL_TYPE_TIMESTAMP:
	case MYSQL_TYPE_TINY:
	case MYSQL_TYPE_SHORT:
	case MYSQL_TYPE_INT24:
	case MYSQL_TYPE_LONG:
	case MYSQL_TYPE_LONGLONG: return 0;
	case MYSQL_TYPE_DOUBLE:
	case MYSQL_TYPE_FLOAT:
	case MYSQL_TYPE_BLOB: return 1;
	case MYSQL_TYPE_BIT:
	case MYSQL_TYPE_ENUM:
	case MYSQL_TYPE_NEWDECIMAL:
	case MYSQL_TYPE_DECIMAL:
	case MYSQL_TYPE_VARCHAR: 
	case MYSQL_TYPE_VAR_STRING: 
	case MYSQL_TYPE_STRING: return 2;
	}

	g_assert_not_reached();
}

/** 
 * convert a table into table-map event
 */
int network_mysqld_binlog_event_tablemap_from_table_columns(
		network_mysqld_binlog_event *event,
		network_mysqld_columns *columns) {
	guint i, pos;

	event->event.table_map_event.columns_len = columns->len;
	event->event.table_map_event.columns = g_new0(gchar, event->event.table_map_event.columns_len);
	
	event->event.table_map_event.null_bits_len = (int)((event->event.table_map_event.columns_len+7)/8);
	event->event.table_map_event.null_bits = g_new0(gchar, event->event.table_map_event.null_bits_len);
	
	event->event.table_map_event.metadata_len = 0;

	for (i = 0; i < columns->len; i++) {
		network_mysqld_column *col = columns->pdata[i];

		event->event.table_map_event.columns[i] = col->type;
		event->event.table_map_event.null_bits[i / 8] |= ( col->flags & NOT_NULL_FLAG ) ? 0 : 1 << (i % 8);
		event->event.table_map_event.metadata_len += network_mysqld_binlog_event_tablemap_get_type_len(col->type);
	}

	event->event.table_map_event.metadata = g_new0(gchar, event->event.table_map_event.metadata_len);
	for (i = 0, pos = 0; i < columns->len; i++) {
		network_mysqld_column *col = columns->pdata[i];

		switch (col->type) {
		case MYSQL_TYPE_STRING:
			/* byte 0: real_type + upper bits of field-length (see #37426)
			 * byte 1: field-length
			 */
			event->event.table_map_event.metadata[pos++] = col->type;
			event->event.table_map_event.metadata[pos++] = col->max_length;
			break;
		case MYSQL_TYPE_VARCHAR:
			event->event.table_map_event.metadata[pos++] = (col->max_length >> 0) & 0xff;
			event->event.table_map_event.metadata[pos++] = (col->max_length >> 8) & 0xff;
			break;
		case MYSQL_TYPE_DATE:
		case MYSQL_TYPE_DATETIME:
		case MYSQL_TYPE_TIMESTAMP:

		case MYSQL_TYPE_TINY:
		case MYSQL_TYPE_SHORT:
		case MYSQL_TYPE_INT24:
		case MYSQL_TYPE_LONG:
		case MYSQL_TYPE_LONGLONG:
			break;
		default:
			g_error("%s: network_mysqld_binlog_event_tablemap_from_table_columns(%s [%d])",
					G_STRLOC,
					network_mysqld_column_get_typestring(col), col->type);
			break;
		}
	}

	return 0;
}

/**
 * convert a table-map event to a internal table
 */
int network_mysqld_binlog_event_tablemap_to_table_columns(
		network_mysqld_binlog_event *event,
		network_mysqld_columns *columns) {
	network_packet metadata_packet;
	GString row;
	guint i;
	int err = 0;

	row.str = event->event.table_map_event.metadata;
	row.len = event->event.table_map_event.metadata_len;

	metadata_packet.data = &row;
	metadata_packet.offset = 0;

	/* the metadata is field specific */
	for (i = 0; i < event->event.table_map_event.columns_len; i++) {
		network_mysqld_column *field = network_mysqld_column_new();
		enum enum_field_types col_type;
		guint8 byte0, byte1;
		guint16 varchar_length;

		guint byteoffset = i / 8;
		guint bitoffset = i % 8;

		field->flags |= (event->event.table_map_event.null_bits[byteoffset] >> bitoffset) & 0x1 ? 0 : NOT_NULL_FLAG;

		col_type = (enum enum_field_types)event->event.table_map_event.columns[i];

		/* the meta-data depends on the type,
		 *
		 * string has 2 byte field-length
		 * floats have precision
		 * ints have display length
		 * */
		switch ((guchar)col_type) {
		case MYSQL_TYPE_STRING: /* 254 (CHAR) */
			/**
			 * due to #37426 the encoding is a bit tricky in 5.1.26+
			 * as we need at least 10 bits to encode the max-field-length
			 *
			 * < 5.1.26
			 *
			 *   byte0      byte1
			 *   xxxx xxxx  .... ....  real-type
			 *   .... ....  xxxx xxxx  field-length
			 *   7       0  7       0
			 *
			 * >= 5.1.26
			 *   byte0      byte1
			 *   xx11 xxxx  .... ....  real-type
			 *   ..xx ....  xxxx xxxx  field-length
			 *   7       0  7       0
			 * 
			 */

			/* byte 0: real_type + upper bits of field-length (see #37426)
			 * byte 1: field-length
			 */
			err = err || network_mysqld_proto_get_int8(&metadata_packet, 
					&byte0);
			err = err || network_mysqld_proto_get_int8(&metadata_packet, 
					&byte1);
			field->max_length = byte1;

			if (!err && ((byte0 & 0x30) != 0x30)) {
				/* a long CHAR() field */
				field->max_length |= (((byte0 & 0x30) ^ 0x30) << 4);
				field->type = byte0 | 0x30; /* see #37426 */
			} else {
				field->type = byte0;
			}

			break;
		case MYSQL_TYPE_VARCHAR: /* 15 (VARCHAR) */
		case MYSQL_TYPE_VAR_STRING:
			/* 2 byte length (int2store)
			 */
			err = err || network_mysqld_proto_get_int16(&metadata_packet, &varchar_length);

			if (!err) {
				field->type = col_type;
				field->max_length = varchar_length;
			}
			break;
		case MYSQL_TYPE_BLOB: /* 252 */
			field->type = col_type;

			/* the packlength (1 .. 4) */
			err = err || network_mysqld_proto_get_int8(&metadata_packet, &byte0);

			field->max_length = byte0;
			break;
		case MYSQL_TYPE_NEWDECIMAL:
		case MYSQL_TYPE_DECIMAL:
			field->type = col_type;
			/**
			 * byte 0: precisions
			 * byte 1: decimals
			 */
			err = err || network_mysqld_proto_get_int8(&metadata_packet, &byte0);
			err = err || network_mysqld_proto_get_int8(&metadata_packet, &byte1);

			if (!err) {
				field->max_length = byte0;
				field->decimals   = byte1;
			}
			break;
		case MYSQL_TYPE_DOUBLE:
		case MYSQL_TYPE_FLOAT:
			/* pack-length */
			err = err || network_mysqld_proto_get_int8(&metadata_packet, &byte0);

			if (!err) {
				field->type = col_type;
				field->max_length = byte0;
			}
			break;
		case MYSQL_TYPE_ENUM:
			/* real-type (ENUM|SET)
			 * pack-length
			 */
			err = err || network_mysqld_proto_get_int8(&metadata_packet, &byte0);
			err = err || network_mysqld_proto_get_int8(&metadata_packet, &byte1);

			if (!err) {
				field->type = byte0;
				field->max_length = byte1;
			}
			break;
		case MYSQL_TYPE_BIT:
			err = err || network_mysqld_proto_skip(&metadata_packet, 2);
			break;
		case MYSQL_TYPE_DATE:
		case MYSQL_TYPE_DATETIME:
		case MYSQL_TYPE_TIMESTAMP:

		case MYSQL_TYPE_TINY:
		case MYSQL_TYPE_SHORT:
		case MYSQL_TYPE_INT24:
		case MYSQL_TYPE_LONG:
		case MYSQL_TYPE_LONGLONG:
			field->type = col_type;
			break;
		default:
			g_error("%s: field-type %d isn't handled",
					G_STRLOC,
					col_type
					);
			break;
		}

		g_ptr_array_add(columns, field);
	}

	if (metadata_packet.offset != metadata_packet.data->len) {
		g_debug_hexdump(G_STRLOC, event->event.table_map_event.columns, event->event.table_map_event.columns_len);
		g_debug_hexdump(G_STRLOC, event->event.table_map_event.metadata, event->event.table_map_event.metadata_len);
	}

	if (metadata_packet.offset != metadata_packet.data->len) {
		g_critical("%s: ",
				G_STRLOC);
		err = 1;
	}
	
	return err ? -1 : 0;
}

/**
 * decode the table-map event
 *
 *  
 */
int network_mysqld_binlog_event_tablemap_to_table(
		network_mysqld_binlog_event *event,
		network_mysqld_table *tbl) {

	int err = 0;

	g_string_assign(tbl->db_name, event->event.table_map_event.db_name);
	g_string_assign(tbl->table_name, event->event.table_map_event.table_name);

	tbl->table_id = event->event.table_map_event.table_id;

	err = err || network_mysqld_binlog_event_tablemap_to_table_columns(event, tbl->columns);

	return err ? -1 : 0;
}

/**
 * convert a table object into a table-map event
 */
int network_mysqld_binlog_event_tablemap_from_table(
		network_mysqld_binlog_event *event,
		network_mysqld_table *tbl) {
	int err = 0;

	event->event.table_map_event.db_name = g_strdup(tbl->db_name->str);
	event->event.table_map_event.db_name_len = tbl->db_name->len;

	event->event.table_map_event.table_name = g_strdup(tbl->table_name->str);
	event->event.table_map_event.table_name_len = tbl->table_name->len;

	 event->event.table_map_event.table_id = tbl->table_id;

	 err = err || network_mysqld_binlog_event_tablemap_from_table_columns(event, tbl->columns);

	 return err ? -1 : 0;
}

