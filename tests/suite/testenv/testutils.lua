module("testenv.testutils", package.seeall)

--
-- Global variables that can be referenced from .options files
--
function get_port_base(port_base_start, port_base_end) 
	local port_base_start = port_base_start or 32768
	local port_base_end   = port_base_end or 65535
	local port_interval   = 64 -- let's take the base port in steps of ...
	local range = port_base_end - port_base_start - port_interval
	math.randomseed(posix.getpid())
	local rand = math.floor(math.random() * (math.ceil(range / port_interval)))
	local port_base = port_base_start + (rand * port_interval)

	print(("... using tcp-port = %d as start port"):format(port_base))

	return port_base
end


-- setting the include path
--

-- this is the path containing the global Lua modules
local GLOBAL_LUA_PATH = os.getenv('LUA_LDIR')  or '/usr/share/lua/5.1/?.lua'

-- this is the path containing the Proxy libraries 
local PROXY_LUA_PATH = os.getenv('LUA_PATH')  or '/usr/local/share/?.lua'

-- This is the path with specific libraries for the test suite
local PRIVATE_LUA_PATH = arg[1]  .. '/t/?.lua'  

-- This is the path with additional libraries that the user needs
local LUA_USER_PATH = os.getenv('LUA_USER_PATH')  or '../lib/?.lua'

-- Building the final include path
local INCLUDE_PATH = 
		LUA_USER_PATH	 .. ';' ..
		PRIVATE_LUA_PATH  .. ';' ..
		GLOBAL_LUA_PATH   .. ';' .. 
		PROXY_LUA_PATH 

---
-- start_proxy()
--
-- starts an instance of MySQL Proxy
--
-- @param proxy_name internal name of the proxy instance, for retrieval
-- @param proxy_options the options to start the Proxy
function start_proxy(proxy_name, proxy_options)
	-- start the proxy
	assert(type(proxy_options) == 'table')
	if not file_exists(proxy_options['proxy-lua-script']) then
		proxy_options['proxy-lua-script'] = 
			global_basedir .. 
			'/t/' ..  proxy_options['proxy-lua-script'] 
	end
	print_verbose("starting " .. proxy_name .. " with " .. options_tostring(proxy_options))

	-- remove the old pid-file if it exists
	os.remove(proxy_options['pid-file'])
	-- if we are supposed to listen on a UNIX socket, remove it first, because we don't clean it up on exit!
	-- TODO: fix the code, instead of hacking around here! Bug#38415
	if proxy_options['proxy-address'] == '/tmp/mysql-proxy-test.sock' then
		os.remove(proxy_options['proxy-address'])
	end
	-- os.execute("head " .. proxy_options['proxy-lua-script'])
	assert(os.execute( 'LUA_PATH="' .. INCLUDE_PATH  .. '"  ' ..
		PROXY_TRACE .. " " .. PROXY_BINPATH .. " " ..
		options_tostring( proxy_options) .. " &"
	))

	-- wait until the proxy is up
	proc = Process.new()
	proc.set_pid_from_pidfile(proxy_options['pid-file'])

	procs[proxy_name] = proc
end

---
-- simulate_replication()
--
-- creates a fake master/slave by having two proxies
-- pointing at the same backend
--
-- you can alter those backends by changing 
-- the starting parameters
--
-- @param master_options options for master
-- @param slave_options options for slave
function simulate_replication(master_options, slave_options)
	if not master_options then
		master_options = default_master_options
	end
	if not master_options['pid-file'] then
		master_options['pid-file'] = PROXY_MASTER_PIDFILE
	end
	if not slave_options then
		slave_options = default_slave_options
	end
	if not slave_options['pid-file'] then
		slave_options['pid-file'] = PROXY_SLAVE_PIDFILE
	end
	start_proxy('master', master_options)
	start_proxy('slave', slave_options)
end

---
-- chain_proxy()
--
-- starts two proxy instances, with the first one is pointing to the 
-- default backend server, and the second one (with default ports)
-- is pointing at the first proxy
--
--   client -> proxy -> backend_proxy -> [ mysql-backend ]
--
-- usually we use it to mock a server with a lua script (...-mock.lua) and
-- the script under test in the proxy (...-test.lua)
--
-- in case you want to start several backend_proxies, just provide a array
-- as first param
--
-- @param backend_lua_script
-- @param second_lua_script 
-- @param use_replication uses a master proxy as backend 
function chain_proxy (backend_lua_scripts, second_lua_script, use_replication)
	local backends = { }

	if type(backend_lua_scripts) == "table" then
		backends = backend_lua_scripts
	else
		backends = { backend_lua_scripts }
	end

	local backend_addresses = { }

	for i, backend_lua_script in ipairs(backends) do
		backend_addresses[i] = PROXY_HOST .. ":" .. (PROXY_CHAIN_PORT + i - 1)

		backend_proxy_options = {
			["proxy-backend-addresses"] = MYSQL_HOST .. ":" .. MYSQL_PORT,
			["proxy-address"]		   = backend_addresses[i],
			["admin-address"]		   = PROXY_HOST .. ":" .. (ADMIN_CHAIN_PORT + i - 1),
			["admin-username"]		   	= ADMIN_USER,
			["admin-password"]		   	= ADMIN_PASSWORD,
			["admin-lua-script"]		= ADMIN_DEFAULT_SCRIPT_FILENAME,
			["pid-file"]				= PROXY_CHAIN_PIDFILE .. i,
			["proxy-lua-script"]		= backend_lua_script or DEFAULT_SCRIPT_FILENAME,
			["plugin-dir"]			= PROXY_LIBPATH,
			["basedir"]					= PROXY_TEST_BASEDIR,
			["log-level"]			= (VERBOSE == 4) and "debug" or "critical",
		}
		-- 
		-- if replication was not started, then it is started here
		--
		if use_replication and (use_replication == true) then
			if (proxy_list['master'] == nil) then
				simulate_replication()
			end
			backend_proxy_options["proxy-backend-addresses"] = PROXY_HOST .. ':' .. PROXY_MASTER_PORT
		end
		start_proxy('backend_proxy' .. i, backend_proxy_options) 
	end

	second_proxy_options = {
			["proxy-backend-addresses"] = backend_addresses ,
			["proxy-address"]		   	= PROXY_HOST .. ":" .. PROXY_PORT,
			["admin-address"]		   	= PROXY_HOST .. ":" .. ADMIN_PORT,
			["admin-username"]		   	= ADMIN_USER,
			["admin-password"]		   	= ADMIN_PASSWORD,
			["admin-lua-script"]		= ADMIN_DEFAULT_SCRIPT_FILENAME,
			["pid-file"]				= PROXY_PIDFILE,
			["proxy-lua-script"]		= second_lua_script or DEFAULT_SCRIPT_FILENAME,
			["plugin-dir"]			= PROXY_LIBPATH,
			["basedir"]					= PROXY_TEST_BASEDIR,
			["log-level"]			= (VERBOSE == 3) and "debug" or "critical",
	}
	start_proxy('second_proxy',second_proxy_options) 
end


---
--sql_execute()
--
-- Executes a SQL query in a given Proxy
-- 
-- If no Proxy is indicated, the query is passed directly to the backend server
--
-- @param query A SQL statement to execute, or a table of SQL statements
-- @param proxy_name the name of the proxy that executes the query
function sql_execute(queries, proxy_name)
	local ret = 0
	assert(type(queries) == 'string' or type(queries) == 'table', 'invalid type for query' )
	if type(queries) == 'string' then
		queries = {queries}
	end
	local query = ''
	for i, q in pairs(queries) do
		query = query .. ';' .. q
	end

	if proxy_name then  
		-- 
		-- a Proxy name is passed. 
		-- The query is executed with the given proxy
		local opts = proxy_list[proxy_name]
		assert(opts,'proxy '.. proxy_name .. ' not active')
		assert(opts['proxy-address'],'address for proxy '.. proxy_name .. ' not found')
		local p_host, p_port = opts['proxy-address']:match('(%S+):(%S+)')
		ret = os_execute( MYSQL_CLIENT_BIN .. ' ' ..
			options_tostring({
				user	 = MYSQL_USER,
				password = MYSQL_PASSWORD,
				database = MYSQL_DB,
				host	 = p_host,
				port	 = p_port,
				execute  = query
			})
		)
        assert(ret == 0, 'error using mysql client ')
	else
		--
		-- No proxy name was passed.
		-- The query is executed in the backend server
		--
		ret = os_execute( MYSQL_CLIENT_BIN .. ' ' ..
			options_tostring({
				user	 = MYSQL_USER,
				password = MYSQL_PASSWORD,
				database = MYSQL_DB,
				host	 = PROXY_HOST,
				port	 = MYSQL_PORT,
				execute  = query
			})
		)
	end
	return ret
end

function only_item ( tbl, item)
	local exists = false
	for i,v in pairs(tbl) do
		if i == item then
			exists = true
		else
			return false
		end
	end
	return exists
end

