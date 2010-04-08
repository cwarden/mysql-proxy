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

#include "network-mysqld-binlog-lua.h"

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
