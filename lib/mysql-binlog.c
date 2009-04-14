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

#include "network-mysqld-binlog.h"
#include "network-mysqld-myisam.h"
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
	iter->row.str = event->event.row_event.row;
	iter->row.len = event->event.row_event.row_len;

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
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);
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
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)luaL_checkself(L);

	network_mysqld_binlog_event_free(event);

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

int lua_mysqld_binlog_event_push(lua_State *L, network_mysqld_binlog_event *udata) {
	network_mysqld_binlog_event **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_binlog_event_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}

typedef struct {
	network_mysqld_binlog *binlog;
	goffset off;
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
		g_critical("%s: event-size = %ld, expected = %"G_GUINT32_FORMAT,
			G_STRLOC,
			event->event_size,
			19);
		return 0;
	}

	if (iter->off + event->event_size != event->log_pos) {
		g_critical("%s: binlog-pos=%lld is invalid, expected = %"G_GUINT32_FORMAT,
			G_STRLOC,
			iter->off + event->event_size,
			event->log_pos);
		return 0;
	}
	iter->off += 19;

	if (network_mysqld_binlog_read_event(binlog, packet, event->event_size)) {
		return 0;
	}

	if (network_mysqld_proto_get_binlog_event(packet, binlog, event)) {
		return 0;
	}
	iter->off += event->event_size - 19;

	return lua_mysqld_binlog_event_push(L, event);
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
	iter->off           = 4;
	
	g_string_set_size(iter->packet.data, 19 + 1);

	/* push the iterator */
	lua_pushcclosure(L, lua_mysqld_binlog_next_event, 1);

	return 1;
}

/**
 */
static int lua_mysqld_binlog_append(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);
	network_mysqld_binlog_event *event;
	GString *packet;

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

		lua_getfield(L, -1, "binlog_version");
		if (lua_isnumber(L, -1)) {
			event->event.format_event.binlog_version = lua_tointeger(L, -1);
		} else if (lua_isnil(L, -1)) {
			event->event.format_event.binlog_version = 4;
		} else {
			luaL_error(L, ".binlog_version has to be a number");
		}
		lua_pop(L, 1);

		lua_getfield(L, -1, "created_ts");
		if (lua_isnumber(L, -1)) {
			event->event.format_event.created_ts = lua_tointeger(L, -1);
		} else if (lua_isnil(L, -1)) {
			event->event.format_event.created_ts = time(NULL);
		} else {
			luaL_error(L, ".created_ts has to be a number");
		}
		lua_pop(L, 1);

		event->event.format_event.log_header_len = 19;

		event->event.format_event.event_header_sizes_len = ENUM_END_EVENT - 1;
		event->event.format_event.event_header_sizes = g_new0(guint8, event->event.format_event.event_header_sizes_len);
		event->event.format_event.event_header_sizes[0] = 1;

		lua_pop(L, 1);
		break;
	case QUERY_EVENT:
		lua_getfield(L, 2, "query");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a QUERY_EVENT needs a .query table");
		}

		lua_getfield(L, -1, "query");
		if (lua_isstring(L, -1)) {
			if (event->event.query_event.query) g_free(event->event.query_event.query);
			event->event.query_event.query = g_strdup(lua_tostring(L, -1));
		} else if (lua_isnil(L, -1)) {
			luaL_error(L, ".query can't be nil");
		} else {
			luaL_error(L, ".query has to be a string");
		}
		lua_pop(L, 1);

		lua_getfield(L, -1, "db_name");
		if (lua_isstring(L, -1)) {
			size_t s_len;
			const char *s;

			s = lua_tolstring(L, -1, &s_len);

			if (s_len >= 255) {
				luaL_error(L, ".db_name can only be 255 char max");
			}

			if (event->event.query_event.db_name) g_free(event->event.query_event.db_name);
			event->event.query_event.db_name = g_strdup(s);
			event->event.query_event.db_name_len = s_len;
		} else if (lua_isnil(L, -1)) {
			if (event->event.query_event.db_name) g_free(event->event.query_event.db_name);
			event->event.query_event.db_name = NULL;
			event->event.query_event.db_name_len = 0;
		} else {
			luaL_error(L, ".db_name has to be a string");
		}
		lua_pop(L, 1);

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

		lua_getfield(L, -1, "xid_id");
		if (lua_isnumber(L, -1)) {
			event->event.xid.xid_id = lua_tonumber(L, -1);
		} else if (lua_isnil(L, -1)) {
			luaL_error(L, ".xid_id can't be nil");
		} else {
			luaL_error(L, ".xid_id has to be a string");
		}
		lua_pop(L, 1);

		lua_pop(L, 1);
		break;
	case INTVAR_EVENT:
		lua_getfield(L, 2, "intvar");
		if (!lua_istable(L, -1)) {
			return luaL_error(L, "a INTVAR_EVENT needs a .intvar table");
		}

		lua_getfield(L, -1, "type");
		if (lua_isnumber(L, -1)) {
			event->event.intvar.type = lua_tonumber(L, -1);
		} else if (lua_isnil(L, -1)) {
			luaL_error(L, ".type can't be nil");
		} else {
			luaL_error(L, ".type has to be a string");
		}
		lua_pop(L, 1);

		lua_getfield(L, -1, "value");
		if (lua_isnumber(L, -1)) {
			event->event.intvar.value = lua_tonumber(L, -1);
		} else if (lua_isnil(L, -1)) {
			luaL_error(L, ".value can't be nil");
		} else {
			luaL_error(L, ".value has to be a string");
		}
		lua_pop(L, 1);

		lua_pop(L, 1);
		break;

	}

	if (network_mysqld_binlog_append(binlog, event)) {
		return luaL_error(L, "appending event to stream failed");
	}

	lua_pushboolean(L, 1);

	return 1;
}


static int lua_mysqld_binlog_close(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);

	close(binlog->fd);

	return 0;
}

/**
 * register the table that is tracked with the table-map event
 *
 * FIXME: should also take a lua-table as parameter
 */
static int lua_mysqld_binlog_tablemap_register(lua_State *L) {
	network_mysqld_binlog *binlog = *(network_mysqld_binlog **)luaL_checkself(L);
	network_mysqld_binlog_event *event = *(network_mysqld_binlog_event **)lua_touserdata(L, 2);
	network_mysqld_table *tbl = network_mysqld_table_new();

	network_mysqld_binlog_event_tablemap_get(event, tbl);
	
	g_hash_table_insert(binlog->rbr_tables, guint64_new(tbl->table_id), tbl);

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

static int lua_mysqld_binlog_new (lua_State *L) {
	network_mysqld_binlog *udata;

	udata = network_mysqld_binlog_new();
	return lua_mysqld_binlog_push(L, udata);
}

static int lua_mysqld_binlog_open (lua_State *L) {
	network_mysqld_binlog *udata;
	const char *filename = luaL_checkstring(L, 1);
	const char *mode     = luaL_optstring(L, 2, "r");

	udata = network_mysqld_binlog_new();

	if (network_mysqld_binlog_open(udata, filename, mode)) {
		lua_pushnil(L);
		lua_pushstring(L, "opening file failed");
		
		return 2;
	}

	return lua_mysqld_binlog_push(L, udata);
}

/*
** Assumes the table is on top of the stack.
*/
static void set_info (lua_State *L) {
	lua_pushliteral (L, "_COPYRIGHT");
	lua_pushliteral (L, "Copyright (C) 2009 Sun Microsystems, Inc");
	lua_settable (L, -3);
	lua_pushliteral (L, "_DESCRIPTION");
	lua_pushliteral (L, "export mysql.binlog");
	lua_settable (L, -3);
	lua_pushliteral (L, "_VERSION");
	lua_pushliteral (L, "LuaMySQLBinlog 0.1");
	lua_settable (L, -3);
}


static const struct luaL_reg mysql_binloglib[] = {
	{"new", lua_mysqld_binlog_new},
	{"open", lua_mysqld_binlog_open},
	{NULL, NULL},
};

#if defined(_WIN32)
# define LUAEXT_API __declspec(dllexport)
#else
# define LUAEXT_API extern
#endif

LUAEXT_API int luaopen_mysql_binlog (lua_State *L) {
	luaL_register (L, "binlog", mysql_binloglib);
	set_info (L);
	return 1;
}
