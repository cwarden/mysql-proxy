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

#include "network-mysqld-binlog.h"
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

	return 0;
}

int lua_mysqld_binlog_event_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_binlog_event_get },
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

	if (event->event_size < 19 ||
	    iter->off + event->event_size != event->log_pos) {
		g_critical("%s: binlog-pos=%lld is invalid, expected = %"G_GUINT32_FORMAT,
			G_STRLOC,
			iter->off,
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

int lua_mysqld_binlog_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "next", lua_mysqld_binlog_next },
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

	udata = network_mysqld_binlog_new();

	if (network_mysqld_binlog_open(udata, filename)) {
		return 0;
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
