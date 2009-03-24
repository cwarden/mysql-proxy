/* $%BEGINLICENSE%$
 Copyright (C) 2007-2008 MySQL AB, 2008 Sun Microsystems, Inc

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
 

#ifndef _NETWORK_MYSQLD_MYISAM_H_
#define _NETWORK_MYSQLD_MYISAM_H_

#include <glib.h>
#include <mysql.h> /* enum field_types */

#include "network-mysqld-proto.h" /* network_packet */
#include "network-mysqld-table.h" /* network_mysqld_table, network_mysqld_column */

/**
 * a field in myisam-packed row
 */
typedef struct {
	network_mysqld_column *column; /* our field-definition */

	union {
		guint64 i;
		gchar *s;
	} data;
	guint64 data_len;

	gboolean is_null;
} network_mysqld_myisam_field;

network_mysqld_myisam_field *network_mysqld_myisam_field_new();
void network_mysqld_myisam_field_free(network_mysqld_myisam_field *field);
int network_mysqld_proto_get_myisam_field(network_packet *packet, network_mysqld_myisam_field *field);
const char *network_mysqld_myisam_field_get_typestring(enum enum_field_types type);

/**
 * a row of myisam-packed fields 
 */
typedef struct {
	GPtrArray *fields; /**< array of network_mysqld_myisam_field */
} network_mysqld_myisam_row;

network_mysqld_myisam_row *network_mysqld_myisam_row_new();
int network_mysqld_myisam_row_init(network_mysqld_myisam_row *row,
		network_mysqld_table *table,
		gchar *null_bits,
		guint null_bits_len);
void network_mysqld_myisam_row_free(network_mysqld_myisam_row *row);
int network_mysqld_proto_get_myisam_row(network_packet *packet, network_mysqld_myisam_row *row);

#endif
