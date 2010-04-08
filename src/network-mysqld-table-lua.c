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

#include "network-mysqld-table.h"
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

#define LUA_UDATA_EXPORT_NUMBER(tbl, name) \
	if (strleq(C(G_STRINGIFY(name)), key, keysize)) { \
		lua_pushnumber(L, tbl->name); \
		return 1; \
	}


static int lua_mysqld_column_get(lua_State *L) {
	network_mysqld_column *col = *(network_mysqld_column **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	LUA_UDATA_EXPORT_CSTR(col, name);
	LUA_UDATA_EXPORT_CSTR(col, org_name);
	
	if (strleq(C("type"), key, keysize)) {
		lua_pushstring(L, network_mysqld_column_get_typestring(col));
		return 1;
	}

	if (strleq(C("is_nullable"), key, keysize)) {
		lua_pushboolean(L, (col->flags & NOT_NULL_FLAG) ? 0 : 1);
		return 1;
	}

	if (strleq(C("length"), key, keysize)) {
		lua_pushinteger(L, col->max_length);
		return 1;
	}

	return 0;
}

int lua_mysqld_column_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_column_get },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_column_push(lua_State *L, network_mysqld_column *udata) {
	network_mysqld_column **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_column_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}

static int lua_mysqld_columns_length(lua_State *L) {
	network_mysqld_columns *cols = *(network_mysqld_columns **)luaL_checkself(L);

	lua_pushinteger(L, cols->len);

	return 1;
}

static int lua_mysqld_columns_get(lua_State *L) {
	network_mysqld_columns *cols = *(network_mysqld_columns **)luaL_checkself(L);
	gint ndx = luaL_checkint(L, 2);

	if (ndx < 1 || ndx > cols->len) {
		/* out of range */
		return 0;
	}

	return lua_mysqld_column_push(L, cols->pdata[ndx - 1]);
}

int lua_mysqld_columns_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__len", lua_mysqld_columns_length },
		{ "__index", lua_mysqld_columns_get },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_columns_push(lua_State *L, network_mysqld_columns *udata) {
	network_mysqld_columns **_udata;

	if (!udata) {
		return 0;
	}

	_udata = lua_newuserdata(L, sizeof(*_udata));
	*_udata = udata;

	lua_mysqld_columns_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}


static int lua_mysqld_table_get(lua_State *L) {
	network_mysqld_table *tbl = *(network_mysqld_table **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	LUA_UDATA_EXPORT_STR(tbl, db_name);
	LUA_UDATA_EXPORT_STR(tbl, table_name);

	if (strleq(C("columns"), key, keysize)) {
		/* columns is a array */
		return lua_mysqld_columns_push(L, tbl->columns);
	}
	
	return 0;
}

static int lua_mysqld_table_set(lua_State *L) {
	network_mysqld_table *tbl = *(network_mysqld_table **)luaL_checkself(L);
	gsize keysize = 0;
	const char *key = luaL_checklstring(L, 2, &keysize);

	LUA_UDATA_IMPORT_STR(tbl, db_name);
	LUA_UDATA_IMPORT_STR(tbl, table_name);

	return 0;
}


int lua_mysqld_table_getmetatable(lua_State *L) {
	static const struct luaL_reg methods[] = {
		{ "__index", lua_mysqld_table_get },
		{ "__newindex", lua_mysqld_table_set },
		{ NULL, NULL },
	};
	return proxy_getmetatable(L, methods);
}

int lua_mysqld_table_push(lua_State *L, network_mysqld_table *tbl) {
	network_mysqld_table **_p;

	if (!tbl) {
		return 0;
	}

	_p = lua_newuserdata(L, sizeof(*_p));
	*_p = tbl;

	lua_mysqld_table_getmetatable(L);
	lua_setmetatable(L, -2); /* tie the metatable to the table   (sp -= 1) */

	return 1;
}

