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
 

/**
 * wrap the network-mysqld-table.h structures
 */


#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <unistd.h>
#include <errno.h>

#include "network-mysqld-binlog.h"
#include "network-mysqld-myisam.h"
#include "network-mysqld-table-lua.h"
#include "glib-ext.h"
#include "lua-env.h"

#define C(x) x, sizeof(x) - 1
#define S(x) x->str, x->len

#define LUA_UDATA_EXPORT_STR(tbl, name) \
	if (strleq(C(G_STRINGIFY(name)), key, keysize)) { \
		lua_pushlstring(L, S(tbl->name)); \
		return 1; \
	}

#define LUA_UDATA_IMPORT_STR(tbl, name) \
	if (strleq(C(G_STRINGIFY(name)), key, keysize)) { \
		gsize s_len; \
		const char *s = luaL_checklstring(L, 3, &s_len); \
		g_string_assign_len(tbl->name, s, s_len); \
		return 0; \
	}

#define LUA_UDATA_EXPORT_CSTR(tbl, name) \
	if (strleq(C(G_STRINGIFY(name)), key, keysize)) { \
		lua_pushstring(L, tbl->name); \
		return 1; \
	}

#define LUA_UDATA_EXPORT_INT(tbl, name) \
	if (strleq(C(G_STRINGIFY(name)), key, keysize)) { \
		lua_pushinteger(L, tbl->name); \
		return 1; \
	}

typedef struct {
	gboolean free_me;
	network_mysqld_binlog_event *event;
} network_mysqld_binlog_event_freeable;

typedef struct {
	network_mysqld_binlog_event *event;
	network_mysqld_table *table;
	network_packet row_packet;
	GString row;
} network_mysqld_binlog_rows_event_iter;

static int network_mysqld_binlog_get_myisam_row(network_packet *packet, 
		network_mysqld_table *table,
		network_mysqld_myisam_row *row, int null_bits_len) {
	int err = 0;
	gchar *null_bits = NULL;

	err = err || network_mysqld_proto_get_string_len(
			packet, 
			&null_bits,
			null_bits_len);

	err = err || network_mysqld_myisam_row_init(row,
			table, 
			null_bits, 
			null_bits_len);

	err = err || network_mysqld_proto_get_myisam_row(packet, row);

	if (null_bits) g_free(null_bits);

	return err;
}

static int lua_mysqld_binlog_rows_event_push_row(lua_State *L, network_mysqld_myisam_row *row) {
	gsize i;

	lua_newtable(L);
	for (i = 0; i < row->fields->len; i++) {
		network_mysqld_myisam_field *field = row->fields->pdata[i];

		lua_pushinteger(L, i + 1);
		if (field->is_null) {
			lua_pushnil(L);
		} else {
			switch((guchar)field->column->type) {
			case MYSQL_TYPE_TIMESTAMP:
			case MYSQL_TYPE_DATE:
			case MYSQL_TYPE_DATETIME:

			case MYSQL_TYPE_TINY:
			case MYSQL_TYPE_SHORT:
			case MYSQL_TYPE_INT24:
			case MYSQL_TYPE_LONG:
			case MYSQL_TYPE_LONGLONG:
			case MYSQL_TYPE_ENUM:
				lua_pushnumber(L, field->data.i);
				break;
			case MYSQL_TYPE_DOUBLE:
				lua_pushnumber(L, field->data.f);
				break;
			case MYSQL_TYPE_VARCHAR:
			case MYSQL_TYPE_VAR_STRING:
			case MYSQL_TYPE_STRING:
				lua_pushstring(L, field->data.s);
				break;
			case MYSQL_TYPE_BLOB:
				lua_pushstring(L, "(blob)");
				break;
			case MYSQL_TYPE_NEWDECIMAL:
				lua_pushstring(L, "(decimal)");
				break;
			default:
				luaL_error(L, "%s: field-type %d isn't known",
						G_STRLOC,
						field->column->type);
				break;
			}
		}

		lua_settable(L, -3);
	}

	return 1;
}

static int lua_mysqld_binlog_rows_event_next_iter(lua_State *L) {
	network_mysqld_binlog_rows_event_iter *iter = (network_mysqld_binlog_rows_event_iter *)lua_touserdata(L, lua_upvalueindex(1));
	network_mysqld_binlog_event *event = iter->event;
	network_mysqld_table *tbl = iter->table;
	network_packet *packet = &(iter->row_packet);

	/* get a row from the current packet */
	network_mysqld_myisam_row *pre_fields, *post_fields = NULL;
	int err = 0;

	/* check if all data is parsed */
	if (packet->offset >= packet->data->len) return 0;

	pre_fields = network_mysqld_myisam_row_new();

	err = err || network_mysqld_binlog_get_myisam_row(packet, 
			tbl,
			pre_fields, 
			event->event.row_event.null_bits_len);

	if (event->event_type == UPDATE_ROWS_EVENT) {
		post_fields = network_mysqld_myisam_row_new();

		err = err || network_mysqld_binlog_get_myisam_row(packet, 
				tbl,
				post_fields, 
				event->event.row_event.null_bits_len);
	}

	if (err) return luaL_error(L, "decoding the row-event failed");

	/* push a pre and a post table */
	lua_newtable(L);
	lua_mysqld_binlog_rows_event_push_row(L, pre_fields);
	lua_setfield(L, -2, "before");
	if (post_fields) {
		lua_mysqld_binlog_rows_event_push_row(L, post_fields);
		lua_setfield(L, -2, "after");
	}

	return 1;
}

/**
 * the upvalue generator for our binlog iterator 
 */
static int lua_mysqld_binlog_rows_event_next(lua_State *L) {
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);
	network_mysqld_table *table;
	network_mysqld_binlog_rows_event_iter *iter;

	luaL_checkany(L, 2);
	table = *(network_mysqld_table **)lua_touserdata(L, 2);

	iter = (network_mysqld_binlog_rows_event_iter *)lua_newuserdata(L, sizeof(network_mysqld_binlog_rows_event_iter)); /* the upvalue */
	iter->event = event;
	iter->table = table;

	if (!iter->table) {
		g_critical("%s: table-id: %"G_GUINT64_FORMAT" isn't known, needed for a %d event",
				G_STRLOC,
				event->event.row_event.table_id,
				event->event_type
				);
		return 0;
	}

	/* setup the row-iterator */
	iter->row.str = event->event.row_event.row->str;
	iter->row.len = event->event.row_event.row->len;

	iter->row_packet.data = &(iter->row);
	iter->row_packet.offset = 0;

	/* push the iterator */
	lua_pushcclosure(L, lua_mysqld_binlog_rows_event_next_iter, 1);

	return 1;
}


/**
 * expose all 3 kinds of _ROWS_EVENT
 */
static int lua_mysqld_binlog_rows_event_get(lua_State *L) {
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	/* FIXME: a bit hacky, but this way we can reuse the macros */
	LUA_UDATA_EXPORT_INT((&(event->event.row_event)), table_id);
	LUA_UDATA_EXPORT_INT((&(event->event.row_event)), flags);

	if (strleq(C("next"), key, keysize)) {
		lua_pushcfunction(L, lua_mysqld_binlog_rows_event_next);

		return 1;
	}

	return 0;
}

int lua_mysqld_binlog_rows_event_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_binlog_rows_event_get },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_binlog_rows_event_push(lua_State *L, network_mysqld_binlog_event *udata) {
	network_mysqld_binlog_event **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_binlog_rows_event_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}

static int lua_mysqld_binlog_tablemap_event_get(lua_State *L) {
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	/* FIXME: a bit hacky, but this way we can reuse the macros */
	LUA_UDATA_EXPORT_INT((&(event->event.table_map_event)), table_id);
	LUA_UDATA_EXPORT_INT((&(event->event.table_map_event)), flags);
	LUA_UDATA_EXPORT_CSTR((&(event->event.table_map_event)), db_name);
	LUA_UDATA_EXPORT_CSTR((&(event->event.table_map_event)), table_name);

	/* expose the fields */
	if (strleq(C("fields"), key, keysize)) {
		network_mysqld_columns *columns;

		columns = network_mysqld_columns_new();
		network_mysqld_binlog_event_tablemap_to_table_columns(event, columns);

		return lua_mysqld_columns_push(L, columns);
	}

	return 0;
}

int lua_mysqld_binlog_tablemap_event_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_binlog_tablemap_event_get },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_binlog_tablemap_event_push(lua_State *L, network_mysqld_binlog_event *udata) {
	network_mysqld_binlog_event **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_binlog_tablemap_event_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}


static int lua_mysqld_binlog_format_event_get(lua_State *L) {
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	/* FIXME: a bit hacky, but this way we can reuse the macros */
	LUA_UDATA_EXPORT_CSTR((&(event->event.format_event)), master_version);
	LUA_UDATA_EXPORT_INT((&(event->event.format_event)), binlog_version);
	LUA_UDATA_EXPORT_INT((&(event->event.format_event)), created_ts);

	return 0;
}

int lua_mysqld_binlog_format_event_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_binlog_format_event_get },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_binlog_format_event_push(lua_State *L, network_mysqld_binlog_event *udata) {
	network_mysqld_binlog_event **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_binlog_format_event_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}


static int lua_mysqld_binlog_intvar_event_get(lua_State *L) {
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	/* FIXME: a bit hacky, but this way we can reuse the macros */
	LUA_UDATA_EXPORT_INT((&(event->event.intvar)), type);
	LUA_UDATA_EXPORT_INT((&(event->event.intvar)), value);

	return 0;
}

int lua_mysqld_binlog_intvar_event_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_binlog_intvar_event_get },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_binlog_intvar_event_push(lua_State *L, network_mysqld_binlog_event *udata) {
	network_mysqld_binlog_event **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_binlog_intvar_event_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}


static int lua_mysqld_binlog_xid_event_get(lua_State *L) {
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	/* FIXME: a bit hacky, but this way we can reuse the macros */
	LUA_UDATA_EXPORT_INT((&(event->event.xid)), xid_id);

	return 0;
}

int lua_mysqld_binlog_xid_event_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_binlog_xid_event_get },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_binlog_xid_event_push(lua_State *L, network_mysqld_binlog_event *udata) {
	network_mysqld_binlog_event **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_binlog_xid_event_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}


static int lua_mysqld_binlog_rotate_event_get(lua_State *L) {
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	/* FIXME: a bit hacky, but this way we can reuse the macros */
	LUA_UDATA_EXPORT_INT((&(event->event.rotate_event)), binlog_pos);
	LUA_UDATA_EXPORT_CSTR((&(event->event.rotate_event)), binlog_file);

	return 0;
}

int lua_mysqld_binlog_rotate_event_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_binlog_rotate_event_get },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_binlog_rotate_event_push(lua_State *L, network_mysqld_binlog_event *udata) {
	network_mysqld_binlog_event **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_binlog_rotate_event_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}


static int lua_mysqld_binlog_query_event_get(lua_State *L) {
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	/* FIXME: a bit hacky, but this way we can reuse the macros */
	LUA_UDATA_EXPORT_INT((&(event->event.query_event)), thread_id);
	LUA_UDATA_EXPORT_INT((&(event->event.query_event)), exec_time);
	LUA_UDATA_EXPORT_INT((&(event->event.query_event)), error_code);
	LUA_UDATA_EXPORT_CSTR((&(event->event.query_event)), db_name);
	LUA_UDATA_EXPORT_CSTR((&(event->event.query_event)), query);

	return 0;
}

int lua_mysqld_binlog_query_event_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_binlog_query_event_get },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_binlog_query_event_push(lua_State *L, network_mysqld_binlog_event *udata) {
	network_mysqld_binlog_event **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_binlog_query_event_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}


static int lua_mysqld_binlog_event_get(lua_State *L) {
	network_mysqld_binlog_event_freeable *udata = (network_mysqld_binlog_event_freeable *)luaL_checkself(L);
	network_mysqld_binlog_event *event = udata->event;
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	LUA_UDATA_EXPORT_INT(event, timestamp);
	LUA_UDATA_EXPORT_INT(event, server_id);
	LUA_UDATA_EXPORT_INT(event, log_pos);
	LUA_UDATA_EXPORT_INT(event, flags);
	LUA_UDATA_EXPORT_INT(event, event_size);

	if (strleq(C("type"), key, keysize)) {
		lua_pushstring(L, network_mysqld_binlog_event_get_name(event));
		return 1;
	}

	if (strleq(C("raw"), key, keysize)) {
		/* push the event-block w/o the header as binary string */
		lua_pushlstring(L, S(event->raw));
		return 1;
	}

	if (strleq(C("query"), key, keysize)) {
		return lua_mysqld_binlog_query_event_push(L, event);
	}

	if (strleq(C("rotate"), key, keysize)) {
		return lua_mysqld_binlog_rotate_event_push(L, event);
	}
	
	if (strleq(C("xid"), key, keysize)) {
		return lua_mysqld_binlog_xid_event_push(L, event);
	}

	if (strleq(C("intvar"), key, keysize)) {
		return lua_mysqld_binlog_intvar_event_push(L, event);
	}

	if (strleq(C("format"), key, keysize)) {
		return lua_mysqld_binlog_format_event_push(L, event);
	}

	if (strleq(C("table_map"), key, keysize)) {
		return lua_mysqld_binlog_tablemap_event_push(L, event);
	}

	if (strleq(C("rbr"), key, keysize)) {
		return lua_mysqld_binlog_rows_event_push(L, event);
	}

	return 0;
}

int lua_mysqld_binlog_event_gc(lua_State *L) {
	network_mysqld_binlog_event_freeable *udata = (network_mysqld_binlog_event_freeable *)luaL_checkself(L);

	if (udata->free_me) {
		network_mysqld_binlog_event_free(udata->event);
	}

	return 0;
}

int lua_mysqld_binlog_event_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_binlog_event_get },
		{ "__gc", lua_mysqld_binlog_event_gc },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_binlog_event_push(lua_State *L, network_mysqld_binlog_event *udata, gboolean free_me) {
	network_mysqld_binlog_event_freeable *_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	_udata->event = udata;
	_udata->free_me = free_me;

	lua_mysqld_binlog_event_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}

static int lua_mysqld_binlog_event_decode(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);
	gsize s_len;
	const char *s = luaL_checklstring(L, 2, &s_len);
	network_mysqld_binlog_event *event;
	network_packet packet;
	GString str;

	str.str = (char *)s;
	str.len = s_len;

	packet.data = &str;
	packet.offset = 0;
	
	event = network_mysqld_binlog_event_new();
	network_mysqld_proto_get_binlog_event_header(&packet, event);

	if (event->event_size < 19) {
		g_critical("%s: event-size = %"G_GUINT32_FORMAT", expected = %"G_GUINT32_FORMAT,
			G_STRLOC,
			event->event_size,
			19);
		network_mysqld_binlog_event_free(event);
		return 0;
	}

	g_string_assign_len(event->raw, packet.data->str + packet.offset, event->event_size - 19);

	if (network_mysqld_proto_get_binlog_event(&packet, binlog, event)) {
		network_mysqld_binlog_event_free(event);
		return 0;
	}

	return lua_mysqld_binlog_event_push(L, event, TRUE);
}

typedef struct {
	network_mysqld_binlog *binlog;
	guint32 off;
	network_packet packet;
} network_mysqld_binlog_iter;

static int lua_mysqld_binlog_next_event(lua_State *L) {
	network_mysqld_binlog_iter *iter = (network_mysqld_binlog_iter *)lua_touserdata(L, lua_upvalueindex(1));
	network_packet *packet = &(iter->packet);
	network_mysqld_binlog_event *event;
	network_mysqld_binlog *binlog = iter->binlog;

	packet->offset = 0;
	
	if (network_mysqld_binlog_read_event_header(binlog, packet)) {
		return 0;
	}

	event = network_mysqld_binlog_event_new();
	network_mysqld_proto_get_binlog_event_header(packet, event);

	if (event->event_size < 19) {
		g_critical("%s: event-size = %"G_GUINT32_FORMAT", expected = %"G_GUINT32_FORMAT,
			G_STRLOC,
			event->event_size,
			19);
		return 0;
	}

	if (iter->off + event->event_size != event->log_pos) {
		g_critical("%s: binlog-pos=%"G_GUINT32_FORMAT" is invalid, expected = %"G_GUINT32_FORMAT,
			G_STRLOC,
			iter->off + event->event_size,
			event->log_pos);
		return 0;
	}
	iter->off += 19;
	
	g_string_assign_len(event->raw, packet->data->str + packet->offset, event->event_size - 19);

	if (network_mysqld_binlog_read_event(binlog, packet, event->event_size)) {
		return 0;
	}

	if (network_mysqld_proto_get_binlog_event(packet, binlog, event)) {
		return 0;
	}
	iter->off += event->event_size - 19;

	return lua_mysqld_binlog_event_push(L, event, TRUE);
}

/**
 * the upvalue generator for our binlog iterator 
 */
static int lua_mysqld_binlog_next(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);
	network_mysqld_binlog_iter *iter = (network_mysqld_binlog_iter *)lua_newuserdata(L, sizeof(network_mysqld_binlog_iter));

	iter->binlog        = binlog;
	iter->packet.data   = g_string_new(NULL);
	iter->packet.offset = 0;
	iter->off           = binlog->log_pos;
	
	g_string_set_size(iter->packet.data, 19 + 1);

	/* push the iterator */
	lua_pushcclosure(L, lua_mysqld_binlog_next_event, 1);

	return 1;
}

int network_mysqld_binlog_row_event_from_myisam_row(network_mysqld_binlog_event *event, 
		network_mysqld_myisam_row *row) {
	guint i;
	GString *packet;
	int null_bits_offset;
	int null_bits_len;

	if (event->event.row_event.columns_len == 0) {
		event->event.row_event.columns_len = row->fields->len;
	}

	if (event->event.row_event.used_columns_len == 0) {
		event->event.row_event.used_columns_len = (int)((event->event.row_event.columns_len+7)/8);
		event->event.row_event.used_columns = g_new0(gchar, event->event.row_event.used_columns_len);
	}

	if (!event->event.row_event.row) event->event.row_event.row = g_string_new(NULL);

	packet = event->event.row_event.row;
	null_bits_offset = packet->len;
	null_bits_len =  (int)((event->event.row_event.columns_len+7)/8);
	g_string_set_size(packet, packet->len + null_bits_len);

	for (i = 0; i < row->fields->len; i++) {
		network_mysqld_myisam_field *field = row->fields->pdata[i];

		event->event.row_event.used_columns[i / 8] |= 1 << (i % 8);

		if (field->is_null) {
			event->event.row_event.row->str[null_bits_offset + (i / 8)] |= 1 << (i % 8);
		} else {
			network_mysqld_proto_append_myisam_field(packet, field);
		}
	}

	return 0;
}

/**
 * turn a event-lua-table into a event-userdata 
 */
static int lua_mysqld_binlog_event_encode(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);
	network_mysqld_binlog_event *event;

	luaL_checktype(L, 2, LUA_TTABLE);

	event = network_mysqld_binlog_event_new();

	/* server_id */
	lua_getfield(L, 2, "server_id");
	if (lua_isnumber(L, -1)) {
		event->server_id = lua_tonumber(L, -1);
	} else if (lua_isnil(L, -1)) {
		event->server_id = 1;
	} else {
		return luaL_error(L, ".server_id has to be a number");
	}
	lua_pop(L, 1);

	/* server_id */
	lua_getfield(L, 2, "timestamp");
	if (lua_isnumber(L, -1)) {
		event->timestamp = lua_tonumber(L, -1);
	} else if (lua_isnil(L, -1)) {
		event->timestamp = time(NULL);
	} else {
		return luaL_error(L, ".timestamp has to be a number");
	}
	lua_pop(L, 1);

	/* "type" is string that we have to map back to the number */
	lua_getfield(L, 2, "type");
	if (lua_isstring(L, -1)) {
		size_t event_type_len;
		const char *event_type = lua_tolstring(L, -1, &event_type_len);

		event->event_type = network_mysqld_binlog_event_get_id(event_type, event_type_len);

		if (event->event_type == UNKNOWN_EVENT) {
			return luaL_error(L, ".type=%s isn't a known event", event_type);
		}
	} else if (lua_isnumber(L, -1)) {
		event->event_type = lua_tonumber(L, -1);
	} else {
		return luaL_error(L, ".server_id has to be a number");
	}
	lua_pop(L, 1);

#define LUA_IMPORT_NUMBER_NIL(obj, key, on_nil) \
		lua_getfield(L, -1, G_STRINGIFY(key)); \
		if (lua_isnumber(L, -1)) { \
			obj.key = lua_tonumber(L, -1); \
		} else if (lua_isnil(L, -1)) { \
			on_nil; \
		} else { \
			luaL_error(L, "."G_STRINGIFY(key)" has to be a number"); \
		} \
		lua_pop(L, 1);

#define LUA_IMPORT_NUMBER(obj, key) \
	LUA_IMPORT_NUMBER_NIL(obj, key, luaL_error(L, "."G_STRINGIFY(key)" can't be nil"))

#define LUA_IMPORT_STRING_FUNKY(obj, key, before_assign, after_assign, on_nil) \
		lua_getfield(L, -1, G_STRINGIFY(key)); \
		if (lua_isstring(L, -1)) { \
			size_t s_len; \
			const char *s = lua_tolstring(L, -1, &s_len); \
			before_assign;\
			if (obj.key) g_free(obj.key); \
			obj.key = g_strdup(s); \
			after_assign; \
		} else if (lua_isnil(L, -1)) { \
			on_nil; \
		} else { \
			luaL_error(L, "."G_STRINGIFY(key)" has to be a string"); \
		} \
		lua_pop(L, 1);

#define LUA_IMPORT_STRING_LEN_ASSIGN_AND_NIL(obj, key, before_assign, on_nil) LUA_IMPORT_STRING_FUNKY(obj, key, before_assign, obj.key ## _len = s_len, on_nil)
#define LUA_IMPORT_STRING_LEN_ASSIGN(obj, key, before_assign) LUA_IMPORT_STRING_LEN_ASSIGN_AND_NIL(obj, key, before_assign, luaL_error(L, "."G_STRINGIFY(key)" can't be nil"))
#define LUA_IMPORT_STRING_LEN_NIL(obj, key, on_nil)  LUA_IMPORT_STRING_LEN_ASSIGN_AND_NIL(obj, key, , on_nil)
#define LUA_IMPORT_STRING_LEN(obj, key) LUA_IMPORT_STRING_LEN_NIL(obj, key, luaL_error(L, "."G_STRINGIFY(key)" can't be nil"))

#define LUA_IMPORT_STRING_ASSIGN_AND_NIL(obj, key, before_assign, on_nil) LUA_IMPORT_STRING_FUNKY(obj, key, before_assign, , on_nil)
#define LUA_IMPORT_STRING_ASSIGN(obj, key, before_assign) LUA_IMPORT_STRING_ASSIGN_AND_NIL(obj, key, before_assign, luaL_error(L, "."G_STRINGIFY(key)" can't be nil"))
#define LUA_IMPORT_STRING_NIL(obj, key, on_nil)  LUA_IMPORT_STRING_ASSIGN_AND_NIL(obj, key, , on_nil)
#define LUA_IMPORT_STRING(obj, key) LUA_IMPORT_STRING_NIL(obj, key, luaL_error(L, "."G_STRINGIFY(key)" can't be nil"))

	switch (event->event_type) {
	case FORMAT_DESCRIPTION_EVENT:
		lua_getfield(L, 2, "format");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a FORMAT_DESCRIPTION_EVENT needs a .format table");
		}

		lua_getfield(L, -1, "master_version");
		if (lua_isstring(L, -1)) {
			if (event->event.format_event.master_version) g_free(event->event.format_event.master_version);
			event->event.format_event.master_version = g_strdup(lua_tostring(L, -1));
		} else if (lua_isnil(L, -1)) {
			if (event->event.format_event.master_version) g_free(event->event.format_event.master_version);
			event->event.format_event.master_version = g_strdup("mysql-proxy-0.7.0");
		} else {
			luaL_error(L, ".master_version has to be a string");
		}
		lua_pop(L, 1);

		LUA_IMPORT_NUMBER_NIL(event->event.format_event, binlog_version, 
				event->event.format_event.binlog_version = 4)

		LUA_IMPORT_NUMBER_NIL(event->event.format_event, created_ts, 
				event->event.format_event.created_ts = time(NULL))

		event->event.format_event.log_header_len = 19;

		event->event.format_event.event_header_sizes_len = ENUM_END_EVENT - 1;
		event->event.format_event.event_header_sizes = g_new0(gchar, event->event.format_event.event_header_sizes_len);
		event->event.format_event.event_header_sizes[START_EVENT_V3 - 1] = 0x38;
		event->event.format_event.event_header_sizes[QUERY_EVENT - 1] = 0x0d;
		event->event.format_event.event_header_sizes[STOP_EVENT - 1] = 0x00;
		event->event.format_event.event_header_sizes[ROTATE_EVENT - 1] = 0x08;
		event->event.format_event.event_header_sizes[INTVAR_EVENT -1] = 0x00;
		event->event.format_event.event_header_sizes[LOAD_EVENT - 1] = 0x12;
		event->event.format_event.event_header_sizes[SLAVE_EVENT - 1] = 0x00;
		event->event.format_event.event_header_sizes[CREATE_FILE_EVENT - 1] = 0x04;
		event->event.format_event.event_header_sizes[APPEND_BLOCK_EVENT - 1] = 0x04;
		event->event.format_event.event_header_sizes[EXEC_LOAD_EVENT - 1] = 0x04;
		event->event.format_event.event_header_sizes[DELETE_FILE_EVENT - 1] = 0x04;
		event->event.format_event.event_header_sizes[NEW_LOAD_EVENT - 1] = 0x12;
		event->event.format_event.event_header_sizes[RAND_EVENT - 1] = 0x00;
		event->event.format_event.event_header_sizes[USER_VAR_EVENT - 1] = 0x00;
		event->event.format_event.event_header_sizes[FORMAT_DESCRIPTION_EVENT - 1] = 0x53;
		event->event.format_event.event_header_sizes[XID_EVENT - 1] = 0x00;
		event->event.format_event.event_header_sizes[BEGIN_LOAD_QUERY_EVENT - 1] = 0x04;
		event->event.format_event.event_header_sizes[EXECUTE_LOAD_QUERY_EVENT - 1] = 0x1a;
		event->event.format_event.event_header_sizes[TABLE_MAP_EVENT - 1] = 0x08;
		event->event.format_event.event_header_sizes[PRE_GA_WRITE_ROWS_EVENT - 1] = 0x00;
		event->event.format_event.event_header_sizes[PRE_GA_UPDATE_ROWS_EVENT - 1] = 0x00;
		event->event.format_event.event_header_sizes[PRE_GA_DELETE_ROWS_EVENT - 1] = 0x00;
		event->event.format_event.event_header_sizes[WRITE_ROWS_EVENT - 1] = 0x08;
		event->event.format_event.event_header_sizes[UPDATE_ROWS_EVENT - 1] = 0x08;
		event->event.format_event.event_header_sizes[DELETE_ROWS_EVENT - 1] = 0x08;
		event->event.format_event.event_header_sizes[INCIDENT_EVENT - 1] = 0x02;

		lua_pop(L, 1);
		break;
	case QUERY_EVENT:
		lua_getfield(L, 2, "query");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a QUERY_EVENT needs a .query table");
		}

		LUA_IMPORT_STRING(event->event.query_event, query);
		LUA_IMPORT_NUMBER_NIL(event->event.query_event, thread_id, event->event.query_event.thread_id = 1);
		LUA_IMPORT_STRING_LEN_ASSIGN_AND_NIL(event->event.query_event, db_name,
			if (s_len >= 255) {
				luaL_error(L, ".db_name can only be 255 char max");
			},
			if (event->event.query_event.db_name) g_free(event->event.query_event.db_name);
			event->event.query_event.db_name = NULL;
			event->event.query_event.db_name_len = 0;
			);

		lua_pop(L, 1);
		break;
	case ROTATE_EVENT:
		lua_getfield(L, 2, "rotate");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a ROTATE_EVENT needs a .rotate table");
		}

		LUA_IMPORT_NUMBER(event->event.rotate_event, binlog_pos);
		LUA_IMPORT_STRING(event->event.rotate_event, binlog_file);

		lua_pop(L, 1);
		break;
	case STOP_EVENT:
		/* no data */
		break;
	case XID_EVENT:
		lua_getfield(L, 2, "xid");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a XID_EVENT needs a .xid table");
		}

		LUA_IMPORT_NUMBER(event->event.xid, xid_id);

		lua_pop(L, 1);
		break;
	case INTVAR_EVENT:
		lua_getfield(L, 2, "intvar");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a INTVAR_EVENT needs a .intvar table");
		}

		LUA_IMPORT_NUMBER(event->event.intvar, type);
		LUA_IMPORT_NUMBER(event->event.intvar, value);

		lua_pop(L, 1);
		break;
	case USER_VAR_EVENT:
		lua_getfield(L, 2, "uservar");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a USER_VAR_EVENT needs a .uservar table");
		}
		LUA_IMPORT_STRING_LEN(event->event.user_var_event, name);
		LUA_IMPORT_STRING_LEN_NIL(event->event.user_var_event, value, event->event.user_var_event.is_null = 1);

		LUA_IMPORT_NUMBER(event->event.user_var_event, type);
		LUA_IMPORT_NUMBER(event->event.user_var_event, charset);

		lua_pop(L, 1);
		break;
	case INCIDENT_EVENT:
		lua_getfield(L, 2, "incident");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a INCIDENT_EVENT needs a .incident table");
		}

		LUA_IMPORT_NUMBER(event->event.incident, incident_id);
		LUA_IMPORT_STRING_LEN_ASSIGN(event->event.incident, message, 
			if (s_len >= 255) {
				luaL_error(L, ".message can only be 255 char max");
			}
			);

		lua_pop(L, 1);
		break;
	case TABLE_MAP_EVENT:
		lua_getfield(L, 2, "table_map");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a TABLE_MAP_EVENT needs a .table_map table");
		}

		LUA_IMPORT_NUMBER(event->event.table_map_event, table_id);
		LUA_IMPORT_STRING_LEN(event->event.table_map_event, db_name);
		LUA_IMPORT_STRING_LEN(event->event.table_map_event, table_name);

		/** 
		 * convert the fields-notation as used for the resultsets into the table_map
		 *
		 */
		lua_getfield(L, -1, "fields");
		if (lua_istable(L, -1)) {
			guint i;
			network_mysqld_columns *cols = network_mysqld_columns_new();
			for (i = 0; i < lua_objlen(L, -1); i++) {
				network_mysqld_column *col = network_mysqld_column_new();
				lua_pushinteger(L, i + 1);
				lua_gettable(L, -2);

				if (lua_istable(L, -1)) {
					lua_getfield(L, -1, "name");
					/* ignore it for now */
					lua_pop(L, 1);

					lua_getfield(L, -1, "type");
					if (lua_isnumber(L, -1)) {
						col->type = lua_tonumber(L, -1);
					} else {
						luaL_error(L, "type for [%d] has to be a number", i + 1);
					}
					lua_pop(L, 1);

					lua_getfield(L, -1, "is_nullable");
					if (lua_isboolean(L, -1)) {
						col->flags = lua_toboolean(L, -1) ? 0 : NOT_NULL_FLAG;
					} else if (lua_isnil(L, -1)) {
						/* default to allow NULL */
						col->flags = 0;
					} else {
						luaL_error(L, ".is_nullable for [%d] has to be a boolean", i + 1);
					}
					lua_pop(L, 1);

					lua_getfield(L, -1, "length");
					if (lua_isnumber(L, -1)) {
						col->max_length = lua_tonumber(L, -1);
					} else if (lua_isnil(L, -1)) {
						switch (col->type) {
						case MYSQL_TYPE_STRING:
						case MYSQL_TYPE_VARCHAR:
							luaL_error(L, ".length for [%d] has to be set", i + 1);
							break;
						default:
							break;
						}
					} else {
						luaL_error(L, ".length for [%d] has to be a number", i + 1);
					}
					lua_pop(L, 1);

				}

				g_ptr_array_add(cols, col);

				lua_pop(L, 1);
			}
			network_mysqld_binlog_event_tablemap_from_table_columns(event, cols);
		} else if (lua_isnil(L, -1)) {
			luaL_error(L, "."G_STRINGIFY(key)" can't be nil");
		} else {
			luaL_error(L, "."G_STRINGIFY(key)" has to be a table");
		}
		lua_pop(L, 1);
		
		lua_pop(L, 1);
		break;
	case WRITE_ROWS_EVENT:
		lua_getfield(L, 2, "rbr");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a WRITE_ROWS_EVENT needs a .rbr table");
		}

		LUA_IMPORT_NUMBER(event->event.row_event, table_id);
		LUA_IMPORT_NUMBER(event->event.row_event, flags);

		/** 
		 * convert the fields-notation as used for the resultsets into the table_map
		 *
		 * rows = { 
		 *   {
		 *     before = { },
		 *     after = { },
		 *   }, ...
		 * }
		 *
		 */
		lua_getfield(L, -1, "rows");
		if (lua_istable(L, -1)) {
			network_mysqld_myisam_field *field;
			network_mysqld_table *tbl;
			guint row_ndx, row_count = lua_objlen(L, -1);

			tbl = g_hash_table_lookup(binlog->rbr_tables, &(event->event.row_event.table_id));
			g_assert(tbl);

			for (row_ndx = 0; row_ndx < row_count; row_ndx++) {
				lua_pushinteger(L, row_ndx + 1);
				lua_gettable(L, -2);

				if (lua_istable(L, -1)) {
					network_mysqld_myisam_row *row;

					row = network_mysqld_myisam_row_new();

					lua_getfield(L, -1, "before");
					if (lua_istable(L, -1)) {
						guint i;
						guint len = lua_objlen(L, -1);

						for (i = 0; i < len; i++) {
							lua_pushinteger(L, i + 1);
							lua_gettable(L, -2);

							field = network_mysqld_myisam_field_new();
							field->column = tbl->columns->pdata[i]; /* get the definition from the table */

							if (lua_isnil(L, -1)) {
								if (field->column->flags & NOT_NULL_FLAG) {
									luaL_error(L, "field[%d] is defined as NOT NULL, can't set it to nil", i + 1);
								}
								field->is_null = TRUE;
							} else {
								switch (field->column->type) {
								case MYSQL_TYPE_VARCHAR:
								case MYSQL_TYPE_VAR_STRING:
								case MYSQL_TYPE_STRING:
								case MYSQL_TYPE_BLOB:
									if (lua_isstring(L, -1)) {
										field->data.s = g_strdup(lua_tostring(L, -1));
									} else {
										return luaL_error(L, "args");
									}
									break;
								case MYSQL_TYPE_TIMESTAMP:
								case MYSQL_TYPE_DATETIME:
								case MYSQL_TYPE_DATE:
								case MYSQL_TYPE_ENUM:
								case MYSQL_TYPE_SET:
								case MYSQL_TYPE_TINY:
								case MYSQL_TYPE_SHORT:
								case MYSQL_TYPE_LONG:
								case MYSQL_TYPE_INT24:
								case MYSQL_TYPE_LONGLONG:
									if (lua_isnumber(L, -1)) {
										field->data.i = lua_tonumber(L, -1);
									} else {
										return luaL_error(L, "args");
									}
									break;
								case MYSQL_TYPE_DOUBLE:
									if (lua_isnumber(L, -1)) {
										field->data.f = lua_tonumber(L, -1);
									} else {
										return luaL_error(L, "args");
									}
									break;

								}
							}

							g_ptr_array_add(row->fields, field);

							lua_pop(L, 1);
						}
					} else {
						return luaL_error(L, "");
					}
					lua_pop(L, 1);

					network_mysqld_binlog_row_event_from_myisam_row(event, row);

					network_mysqld_myisam_row_free(row);
				} else {
					return luaL_error(L, "");
				}
				lua_pop(L, 1);
			}
		} else if (lua_isnil(L, -1)) {
			luaL_error(L, "."G_STRINGIFY(key)" can't be nil");
		} else {
			luaL_error(L, "."G_STRINGIFY(key)" has to be a table");
		}
		lua_pop(L, 1);
		
		lua_pop(L, 1);
		break;
	}

	lua_mysqld_binlog_event_push(L, event, TRUE);

	return 1;
}

/**
 */
static int lua_mysqld_binlog_append(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);
	network_mysqld_binlog_event *event;
	GString *packet;

	/* can take a table or a event-userdata */
	if (lua_type(L, 2) == LUA_TUSERDATA) {
		network_mysqld_binlog_event_freeable *udata = (network_mysqld_binlog_event_freeable *)lua_touserdata(L, 2);
		event = udata->event;
	} else {
		network_mysqld_binlog_event_freeable *_freeable;

		lua_mysqld_binlog_event_encode(L);
		_freeable = lua_touserdata(L, -1);
		event = _freeable->event;
	}

	if (binlog->mode == BINLOG_MODE_WRITE) {
		if (network_mysqld_binlog_append(binlog, event)) {
			return luaL_error(L, "appending event to stream failed");
		}
		lua_pop(L, 1); /* pop the converted event */
		lua_pushboolean(L, 1);

		return 1;
	} else {
		GString *header;

		/* a binlog created with .new() ... as we can't write to it, return a packet */
		packet = g_string_new(NULL);
		if (network_mysqld_proto_append_binlog_event(packet, event)) {
			g_critical("%s: _append_binlog_event failed", G_STRLOC);
			g_string_free(packet, TRUE);
			return 0;
		}
		
		event->event_size = 19 /* the header */ + packet->len;
		event->log_pos = binlog->log_pos + event->event_size;
		
		header = g_string_new(NULL);
		if (network_mysqld_proto_append_binlog_event_header(header, event)) {
			g_critical("%s", G_STRLOC);
			g_string_free(packet, TRUE);
			g_string_free(header, TRUE);
			return 0;
		}

		binlog->log_pos = event->log_pos;
		lua_pop(L, 1); /* pop the converted event */

		lua_pushlstring(L, S(header));
		lua_pushlstring(L, S(packet));
		lua_concat(L, 2);
		
		g_string_free(header, TRUE);
		g_string_free(packet, TRUE);

		return 1;
	}
}


static int lua_mysqld_binlog_close(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);

	close(binlog->fd);

	return 0;
}

static int lua_mysqld_binlog_seek(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);
	goffset off = luaL_checknumber(L, 2);

	if (off < 0) {
		luaL_error(L, "binlog.seek(...) can't be negative");
	}

	binlog->log_pos = off;
	if (binlog->fd != -1) {
		if (binlog->log_pos != lseek(binlog->fd, binlog->log_pos, SEEK_SET)) {
			luaL_error(L, "binlog.seek(%"G_GUINT32_FORMAT") failed ... somehow: %s (%d)",
					binlog->log_pos,
					g_strerror(errno),
					errno);
		}
	}

	return 0;
}


/**
 * register the table that is tracked with the table-map event
 *
 * FIXME: should also take a lua-table as parameter
 */
static int lua_mysqld_binlog_tablemap_register(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);
	network_mysqld_binlog_event *event;
	network_mysqld_table *tbl;

	if (lua_type(L, 2) == LUA_TTABLE) {
		lua_mysqld_binlog_event_encode(L);
		event = lua_touserdata(L, -1);
	} else if (lua_type(L, 2) == LUA_TUSERDATA) {
		network_mysqld_binlog_event_freeable *udata = (network_mysqld_binlog_event_freeable *)lua_touserdata(L, 2);
		event = udata->event;
	} else {
		return luaL_error(L, ":register() should get a event-userdata or a table");
	}

	tbl = network_mysqld_table_new();

	network_mysqld_binlog_event_tablemap_to_table(event, tbl);
	
	g_hash_table_insert(binlog->rbr_tables, guint64_new(tbl->table_id), tbl);
	
	if (lua_type(L, 2) == LUA_TTABLE) {
		lua_pop(L, 1);
	}

	return 0;
}

/**
 * get the table definition referenced by the table-id 
 */
static int lua_mysqld_binlog_tablemap_get(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);
	guint64 tbl_id = (guint64)luaL_checknumber(L, 2);
	network_mysqld_table *tbl;

	tbl = g_hash_table_lookup(binlog->rbr_tables, &(tbl_id));
	if (!tbl) {
		lua_pushnil(L);
		return 1;
	}

	return lua_mysqld_table_push(L, tbl);
}

int lua_mysqld_binlog_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "next", lua_mysqld_binlog_next },
		{ "append", lua_mysqld_binlog_append },
		{ "close", lua_mysqld_binlog_close },
		{ "table_register", lua_mysqld_binlog_tablemap_register },
		{ "table_get", lua_mysqld_binlog_tablemap_get },
		{ "to_event", lua_mysqld_binlog_event_encode },
		{ "from_event", lua_mysqld_binlog_event_decode },
		{ "seek", lua_mysqld_binlog_seek },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_binlog_push(lua_State *L, network_mysqld_binlog *udata) {
	network_mysqld_binlog **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_binlog_getmetatable(L);
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}

