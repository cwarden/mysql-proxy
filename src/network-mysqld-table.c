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
 

#include <glib.h>

#include "network-mysqld-table.h"

network_mysqld_column *network_mysqld_column_new() {
	network_mysqld_column *col;

	col = g_new0(network_mysqld_column, 1);

	return col;
}

void network_mysqld_column_free(network_mysqld_column *col) {
	if (!col) return;

	g_free(col);
}

network_mysqld_columns *network_mysqld_columns_new() {
	return g_ptr_array_new();
}

void network_mysqld_columns_free(network_mysqld_columns *cols) {
	guint i;

	if (!cols) return;

	for (i = 0; i < cols->len; i++) {
		network_mysqld_column_free(cols->pdata[i]);
	}

	g_ptr_array_free(cols, TRUE);
}

network_mysqld_table *network_mysqld_table_new() {
	network_mysqld_table *tbl;

	tbl = g_new0(network_mysqld_table, 1);
	tbl->db_name = g_string_new(NULL);
	tbl->table_name = g_string_new(NULL);

	tbl->columns = network_mysqld_columns_new();

	return tbl;
}

void network_mysqld_table_free(network_mysqld_table *tbl) {
	if (!tbl) return;

	g_string_free(tbl->db_name, TRUE);
	g_string_free(tbl->table_name, TRUE);

	network_mysqld_columns_free(tbl->columns);

	g_free(tbl);
}

