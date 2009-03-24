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

#include "glib-ext.h"
#include "network-mysqld-myisam.h"

#define S(x) x->str, x->len

/**
 * create the field-defition for a "myisam packed" field
 *
 * - contains the def and its value
 */
network_mysqld_myisam_field *network_mysqld_myisam_field_new() {
	network_mysqld_myisam_field *field;

	field = g_new0(network_mysqld_myisam_field, 1);

	return field;
}

/**
 * free myisam-row-format field
 */
void network_mysqld_myisam_field_free(network_mysqld_myisam_field *field) {
	if (!field) return;

	switch ((guchar)field->column->type) {
	case MYSQL_TYPE_TIMESTAMP:
	case MYSQL_TYPE_DATE:
	case MYSQL_TYPE_DATETIME:

	case MYSQL_TYPE_TINY:
	case MYSQL_TYPE_SHORT:
	case MYSQL_TYPE_INT24:
	case MYSQL_TYPE_LONG:

	case MYSQL_TYPE_DECIMAL:
	case MYSQL_TYPE_NEWDECIMAL:

	case MYSQL_TYPE_ENUM:
		break;
	case MYSQL_TYPE_BLOB:
	case MYSQL_TYPE_VARCHAR:
	case MYSQL_TYPE_VAR_STRING:
	case MYSQL_TYPE_STRING:
		if (field->data.s) g_free(field->data.s);
		break;
	default:
		g_message("%s: unknown field_type to free: %d",
				G_STRLOC,
				field->column->type);
		break;
	}

	g_free(field);
}

/**
 * decode a packet into proto_field according to field->fielddef->type
 *
 * @param field field definition
 * @returns 0 on success, -1 on error
 */
int network_mysqld_proto_get_myisam_field(network_packet *packet, 
		network_mysqld_myisam_field *field) {
	guint64 length;
	guint8  i8;
	guint16 i16;
	guint32 i32;
	guint64 i64;
	network_mysqld_column *column = field->column;
	int err = 0;

	switch ((guchar)column->type) {
	case MYSQL_TYPE_TIMESTAMP: /* int4store */
	case MYSQL_TYPE_LONG:
		err = err || network_mysqld_proto_get_int32(packet, &i32);
		if (!err) field->data.i = i32;
		break;
	case MYSQL_TYPE_DATETIME: /* int8store */
	case MYSQL_TYPE_LONGLONG:
		err = err || network_mysqld_proto_get_int64(packet, &i64);
		if (!err) field->data.i = i64;
		break;
	case MYSQL_TYPE_INT24:     
	case MYSQL_TYPE_DATE:      /* int3store, a newdate, old-data is 4 byte */
		err = err || network_mysqld_proto_get_int24(packet, &i32);
		if (!err) field->data.i = i32;
		break;
	case MYSQL_TYPE_SHORT:     
		err = err || network_mysqld_proto_get_int16(packet, &i16);
		if (!err) field->data.i = i16;
		break;
	case MYSQL_TYPE_TINY:     
		err = err || network_mysqld_proto_get_int8(packet, &i8);
		if (!err) field->data.i = i8;
		break;
	case MYSQL_TYPE_ENUM:
		switch (column->max_length) {
		case 1:
			err = err || network_mysqld_proto_get_int8(packet, &i8);
			if (!err) field->data.i = i8;
			break;
		case 2:
			err = err || network_mysqld_proto_get_int16(packet, &i16);
			if (!err) field->data.i = i16;
			break;
		default:
			g_error("%s: enum-length = %lu", 
					G_STRLOC,
					column->max_length);
			break;
		}
		break;
	case MYSQL_TYPE_BLOB:
		switch (column->max_length) {
		case 1:
			err = err || network_mysqld_proto_get_int8(packet, &i8);
			if (!err) length = i8;
			break;
		case 2:
			err = err || network_mysqld_proto_get_int16(packet, &i16);
			if (!err) length = i16;
			break;
		case 3:
			err = err || network_mysqld_proto_get_int24(packet, &i32);
			if (!err) length = i32;
			break;
		case 4:
			err = err || network_mysqld_proto_get_int32(packet, &i32);
			if (!err) length = i32;
			break;
		default:
			/* unknown blob-length */
			g_debug_hexdump(G_STRLOC, S(packet->data));
			g_error("%s: blob-length = %lu", 
					G_STRLOC,
					column->max_length);
			break;
		}
		err = err || network_mysqld_proto_get_string_len(packet, &field->data.s, length);
		break;
	case MYSQL_TYPE_VARCHAR:
	case MYSQL_TYPE_VAR_STRING:
	case MYSQL_TYPE_STRING:
		if (column->max_length < 256) {
			err = err || network_mysqld_proto_get_int8(packet, &i8);
			err = err || network_mysqld_proto_get_string_len(packet, &field->data.s, i8);
		} else {
			err = err || network_mysqld_proto_get_int16(packet, &i16);
			err = err || network_mysqld_proto_get_string_len(packet, &field->data.s, i16);
		}

		break;
	case MYSQL_TYPE_NEWDECIMAL: {
		/* the decimal is binary encoded
		 */
		guchar digits_per_bytes[] = { 0, 1, 1, 2, 2, 3, 3, 4, 4, 4 }; /* how many bytes are needed to store x decimal digits */

		guint i_digits = column->max_length - column->decimals;
		guint f_digits = column->decimals;

		guint decimal_full_blocks       = i_digits / 9; /* 9 decimal digits in 4 bytes */
		guint decimal_last_block_digits = i_digits % 9; /* how many digits are left ? */

		guint scale_full_blocks         = f_digits / 9; /* 9 decimal digits in 4 bytes */
		guint scale_last_block_digits   = f_digits % 9; /* how many digits are left ? */

		guint size = 0;

		size += decimal_full_blocks * digits_per_bytes[9] + digits_per_bytes[decimal_last_block_digits];
		size += scale_full_blocks   * digits_per_bytes[9] + digits_per_bytes[scale_last_block_digits];

#if 0
		g_debug_hexdump(G_STRLOC " (NEWDECIMAL)", packet->data->str, packet->data->len);
#endif
#if 0
		g_critical("%s: don't know how to decode NEWDECIMAL(%lu, %u) at offset %u (%d)",
				G_STRLOC,
				column->max_length,
				column->decimals,
				packet->offset,
				size
				);
#endif
		err = err || network_mysqld_proto_skip(packet, size);
		break; }
	default:
		g_debug_hexdump(G_STRLOC, packet->data->str, packet->data->len);
		g_error("%s: unknown field-type to fetch: %d",
				G_STRLOC,
				column->type);
		break;
	}

	return err ? -1 : 0;
}

/**
 * create a row of fields based on a field-definition and the current null-bits
 *
 * @return a initialized row definition
 */
network_mysqld_myisam_row *network_mysqld_myisam_row_new() {
	network_mysqld_myisam_row *row;

	row = g_new0(network_mysqld_myisam_row, 1);
	row->fields = g_ptr_array_new();

	return row;
}

/**
 * setup a row from a table-definition
 *
 * the field-def stays referenced 
 *
 * @param fielddefs a array of MYSQL_FIELD
 * @param null_bits bitfield of null'ed fields in this row
 * @param null_bits_len length of the null-bits in bytes (we extract it from the field-def's length)
 *
 */
int network_mysqld_myisam_row_init(network_mysqld_myisam_row *row, 
		network_mysqld_table *table,
		gchar *null_bits,
		guint G_GNUC_UNUSED null_bits_len) {
	guint i;
	int err = 0;

	for (i = 0; i < table->columns->len; i++) {
		network_mysqld_column *column = table->columns->pdata[i];
		network_mysqld_myisam_field *field = network_mysqld_myisam_field_new();

		guint byteoffset = i / 8;
		guint bitoffset = i % 8;

		field->column = column;
		field->is_null = (null_bits[byteoffset] >> bitoffset) & 0x1;

		/* the field is defined as NOT NULL, so the null-bit shouldn't be set */
		if ((column->flags & NOT_NULL_FLAG) != 0) {
			if (field->is_null) {
				err = 1;
				g_critical("%s: [%d] field is defined as NOT NULL, but nul-bit is set",
						G_STRLOC,
						i
						);
			}
		}
		g_ptr_array_add(row->fields, field);
	}

	return err ? -1 : 0;
}

/**
 * get all fields from a packet
 */
int network_mysqld_proto_get_myisam_row(network_packet *packet, network_mysqld_myisam_row *row) {
	guint i;

	for (i = 0; i < row->fields->len; i++) {
		network_mysqld_myisam_field *field = row->fields->pdata[i];

		if (!field->is_null) {
			if (network_mysqld_proto_get_myisam_field(packet, field)) return -1;
		}
	}

	return 0;
}

/**
 * free a myisam packed row
 */
void network_mysqld_myisam_row_free(network_mysqld_myisam_row *row) {
	guint i;

	if (!row) return;

	for (i = 0; i < row->fields->len; i++) {
		network_mysqld_myisam_field_free(row->fields->pdata[i]);
	}
	g_ptr_array_free(row->fields, TRUE);

	g_free(row);

}


