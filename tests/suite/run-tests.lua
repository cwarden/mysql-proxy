--[[ $%BEGINLICENSE%$
 Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; version 2 of the
 License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA

 $%ENDLICENSE%$ --]]

-- vim:sw=4:noexpandtab 

---
-- a lua baed test-runner for the mysql-proxy
--
-- to stay portable it is written in lua
--

-- we require LFS (LuaFileSystem)
require("lfs")
require("glib2")
require("posix")
local fileutils = require("testenv.fileutils")
local shellutils = require("testenv.shellutils")
local testutils = require("testenv.testutils")
local process   = require("testenv.process")
local processmanager = require("testenv.processmanager")

-- 
-- a set of user variables which can be overwritten from the environment
--

TestRunner = {
	num_tests   = 0,
	num_passes  = 0,
	num_skipped = 0,
	num_fails   = 0,
	failed_test = {},
	tests       = {},

	testenv = {
	},

	force_on_error = true,
	tests_to_skip_filename = "tests_to_skip.lua",
	tests_regex = os.getenv("TESTS_REGEX")
}

function TestRunner:new(o)
	-- create a new process object
	--
	o = o or {}
	setmetatable(o, self)
	self.__index = self

	local port_base = testutils.get_port_base(os.getenv("MYSQL_PROXY_START_PORT"), os.getenv("MYSQL_PROXY_END_PORT"))

	self.testenv.testdir          = fileutils.dirname(arg[0])
	self.testenv.srcdir	      = os.getenv("srcdir")		 	or self.testenv.testdir .. "/"
	self.testenv.top_builddir     = os.getenv("top_builddir")   		or self.testenv.testdir .. "/../"
	self.testenv.builddir         = os.getenv("builddir")	   	or self.testenv.testdir .. "/" -- same as srcdir by default
	self.testenv.plugin_dir       = self.testenv.top_builddir .. "/plugins/"
	self.testenv.basedir          = lfs.currentdir()
	self.testenv.lua_path         = self.testenv.basedir .. "/" .. self.testenv.srcdir .. "/../../lib/?.lua"
	self.testenv.lua_cpath        = self.testenv.basedir .. "/../../lib/.libs/?.so"

	self.testenv.PROXY_HOST       = os.getenv("PROXY_HOST")		or "127.0.0.1"
	self.testenv.PROXY_PORT       = os.getenv("PROXY_PORT")		or tostring(port_base + 0)
	self.testenv.MYSQL_TEST_BIN   = os.getenv("MYSQL_TEST_BIN") 	or "mysqltest"
	self.testenv.MYSQL_CLIENT_BIN = os.getenv("MYSQL_CLIENT_BIN")   or "mysql"
	self.testenv.PROXY_CHAIN_PORT = os.getenv("PROXY_CHAIN_PORT")	or tostring(port_base + 30)

	self.testenv.abs_srcdir	      = os.getenv("abs_srcdir")

	if not self.testenv.abs_srcdir then
		if not fileutils.path_is_absolute(self.testenv.srcdir) then
			local abs_srcdir = posix.getcwd() .. "/" .. self.testenv.srcdir
			self.testenv.abs_srcdir = abs_srcdir
		else
			self.testenv.abs_srcdir = self.testenv.srcdir
		end
		glib2.setenv("abs_srcdir", self.testenv.abs_srcdir) -- expose the abs_srcdir again as we run the proxy as --daemon which chdir()s to /
	end

	return o
end

function TestRunner:register_tests(suites)
	for i, suite in ipairs(suites) do
		local suite_srcdir = self.testenv.srcdir .. '/' .. suite
		local suite_skipfile = suite_srcdir .. '/' .. self.tests_to_skip_filename
		local stat = assert(lfs.attributes(suite_srcdir))

		-- if it is a directory, execute all of them
		if stat.mode == "directory" then
			if fileutils.file_exists(suite_skipfile) then
				assert(loadfile(suite_skipfile))()
			end

			for file in lfs.dir(suite_srcdir .. "/t/") do
				local testname = file:match("(.+)\.test$")

				if testname then
					local is_skipped = false

					if (self.tests_regex and not testname:match(self.tests_regex)) or tests_to_skip[testname] then
						is_skipped = true
					end

					local test = MySQLProxyTest:new()
					test.testname = testname
					test.suite_srcdir = suite_srcdir
					test.testenv = self.testenv
					test.skipped = is_skipped

					self.tests[#self.tests + 1] = test 
				end
			end
		end
	end
end

function TestRunner:run_all()
	local exitcode = 0

	for i, test in ipairs(self.tests) do
		shellutils.print_verbose("# >> " .. test.testname .. " started")

		self.num_tests = self.num_tests + 1
		local r = test:run()
		if (r == 0) then
			self.num_passes = self.num_passes + 1
		elseif (r == 77) then
			self.num_skipped = self.num_skipped + 1
		else
			self.num_fails = self.num_fails + 1
			table.insert(self.failed_test, test)
		end

		shellutils.print_verbose("# << (exitcode = " .. r .. ")" )

		if r ~= 0 and r ~= 77 and exitcode == 0 then
			exitcode = r
		end

		if self.num_fails > 0 and (not self.force_on_error) then
			break
		end
	end

	if self.num_fails > 0 then
		print ("*** ERRORS OCCURRED - The following tests failed")
		for i, test in pairs(self.failed_test) do
			print(test.testname)
		end
	end

	--
	-- prints test suite statistics
	shellutils.print_verbose (string.format('tests: %d - skipped: %d (%4.1f%%) - passed: %d (%4.1f%%) - failed: %d (%4.1f%%)',
				self.num_tests,
				self.num_skipped,
				self.num_skipped / self.num_tests * 100,
				self.num_passes,
				self.num_passes / (self.num_tests - self.num_skipped) * 100,
				self.num_fails,
				self.num_fails / (self.num_tests  - self.num_skipped) * 100))
	
	return exitcode
end



local VERBOSE		= 	os.getenv('TEST_VERBOSE') or 
						os.getenv('VERBOSE') or 
						os.getenv('DEBUG') or 0
VERBOSE = VERBOSE + 0

local FORCE_ON_ERROR 	= os.getenv('FORCE_ON_ERROR') 	or os.getenv('FORCE')


local MYSQL_USER	 	= os.getenv("MYSQL_USER")	 	or "root"
local MYSQL_PASSWORD 	= os.getenv("MYSQL_PASSWORD") 	or ""
local MYSQL_HOST	 	= os.getenv("MYSQL_HOST")	 	or "127.0.0.1"
local MYSQL_PORT	 	= os.getenv("MYSQL_PORT")	 	or "3306"
local MYSQL_DB	   		= os.getenv("MYSQL_DB")	   		or "test"


--
-- end of user-vars
--

-- options for the MySQL Proxy
MySQLProxy = {
}
function MySQLProxy:new(o)
	-- create a new process object
	
	o = o or {}
	setmetatable(o, self)
	self.__index = self

	self.backends = { }
	return o
end

function MySQLProxy:add_backend(addr)
	self.backends[#self.backends + 1] = addr
end

function MySQLProxy:get_args(opts)
	-- add a default backend 
	if #self.backends == 0 then
		self:add_backend(("%s:%d"):format(MYSQL_HOST, MYSQL_PORT))
	end

	-- init ourself
	opts = opts or { }
	opts["plugin-dir"]   = self.testenv["plugin_dir"]
	opts["basedir"]      = self.testenv["basedir"]
	opts["pid-file"]     = opts["pid-file"] or ("%s/mysql-proxy.pid"):format(self.testenv.basedir)
	opts["plugins"]      = "proxy"
	opts["proxy-address"]= opts["proxy-address"] or ("%s:%d"):format(self.testenv.PROXY_HOST, self.testenv.PROXY_PORT)
	self.proxy_address   = opts["proxy-address"]
	opts["daemon"]       = true
	opts["lua-path"]     = self.testenv.lua_path
	opts["lua-cpath"]    = self.testenv.lua_cpath

	opts["proxy-backend-addresses"] = self.backends

	local extra_params = os.getenv("PROXY_PARAMS")
	if extra_params then
		-- FIXME: implement me
	end

	return opts
end

function MySQLProxy:get_env()
	return {}
end

function MySQLProxy:get_command()
	return os.getenv("PROXY_BINPATH") or self.testenv.top_builddir .. "/src/mysql-proxy"
end

---
-- the backend
MockBackend = MySQLProxy:new()
function MockBackend:chain_with_frontend(frontend)
	frontend:add_backend(self.proxy_address)
end

Test = {
}

function Test:new(o)
	-- create a new process object
	--
	o = o or {}
	setmetatable(o, self)
	self.__index = self

	self.skipped = false
	self.testname = nil

	return o
end

---
-- @return true[, msg] if test should be skipped, false otherwise
function Test:is_skipped()
	return self.skipped
end

function Test:setup()
	return true
end

function Test:teardown()
	return true
end

function Test:run()
	local is_skipped, skip_msg = self:is_skipped() 

	if is_skipped then
		print(('skip %s %s'):format(
			self.testname,
			skip_msg or 'no reason given'))
		return 77
	end

	self.procs = processmanager:new()

	local ret, errmsg = self:setup()
	if not ret then
		print(("err %s # setup failed: %s"):format(
			self.testname,
			errmsg))
		ret = -1
	else
		ret = self:run_test()
		self:teardown()
	end

	self.procs:shutdown_all() -- shutdown all the processes we started

	return ret
end

---
-- a Test baseclass that knows about 
-- * mysqltest as test-driver
-- * starting MySQL Proxies before starting mysqltest
--
-- inherited from Test
MySQLProxyTest = Test:new()

---
-- a ugly hack ... don't talk about it
test_self = nil

-- setup the backend proxies and wire them together
function chain_proxy(backend_filenames, script_filename)
	local self = test_self

	if type(backend_filenames) ~= "table" then
		backend_filenames = { backend_filenames }
	end

	local backends = { }
	for backend_ndx, backend_filename in ipairs(backend_filenames) do
		local mock = MockBackend:new()
		mock.testenv = self.testenv

		local port = tonumber(self.testenv.PROXY_CHAIN_PORT) + backend_ndx - 1 -- we start with 1

		local mock_args = mock:get_args({
			["proxy-lua-script"] = ("%s/t/%s"):format(self.suite_srcdir, backend_filename),
			["proxy-address"]    = ("%s:%d"):format(self.testenv.PROXY_HOST, port),
			["pid-file"]         = ("%s/chain-%d.pid"):format(self.testenv.basedir, backend_ndx)
		})

		local proc  = process:new()
		local ret = proc:execute(mock:get_command(),
			mock:get_env(),
			mock_args)
		if ret ~= 0 then
			return false, ""
		end

		if mock_args["pid-file"] then
			local is_running, errmsg = proc:wait_running(mock_args["pid-file"])
			if not is_running then
				return false, errmsg or "not running"
			end
		end

		self.procs:add(proc)
		table.insert(backends, mock)
	end
	-- all the backends are up, start the middle proxy

	script_filename  = ("%s/t/%s"):format(self.suite_srcdir, script_filename)

	local proxy = MySQLProxy:new()
	proxy.testenv = self.testenv

	for _, backend in ipairs(backends) do
		backend:chain_with_frontend(proxy)
	end

	local proxy_args = proxy:get_args({
		["proxy-lua-script"] = script_filename,
	})

	local proc  = process:new()
	local ret = proc:execute(proxy:get_command(),
		proxy:get_env(),
		proxy_args)
	if ret ~= 0 then
		return false, ""
	end
	if proxy_args["pid-file"] then
		local is_running, errmsg = proc:wait_running(proxy_args["pid-file"])
		if not is_running then
			return false, errmsg or "not running"
		end
	end

	self.procs:add(proc)
end

function MySQLProxyTest:start_proxy(script_filename)
	local proxy = MySQLProxy:new()
	proxy.testenv = self.testenv
	local proxy_args = proxy:get_args({
		["proxy-lua-script"] = script_filename,
	})

	local proc  = process:new()
	local ret = proc:execute(proxy:get_command(),
		proxy:get_env(),
		proxy_args)
	if ret ~= 0 then
		return false, ""
	end
	if proxy_args["pid-file"] then
		local is_running, errmsg = proc:wait_running(proxy_args["pid-file"])
		if not is_running then
			return false, errmsg or "not running"
		end
	end

	self.procs:add(proc)
end

function MySQLProxyTest:setup()
	local script_filename  = ("%s/t/%s.lua"):format(self.suite_srcdir, self.testname)
	local options_filename = ("%s/t/%s.options"):format(self.suite_srcdir, self.testname)
	local has_script_file = fileutils.file_exists(script_filename)
	local has_options_file = fileutils.file_exists(options_filename)

	-- if we have a options file, run it as part of the setup phases
	-- if not, assume that we want to start a proxy with the script_filename
	if has_options_file then
		-- load the options file
		test_self = self
		local setup_func, errmsg = loadfile(options_filename)
		if not setup_func then
			return false, errmsg
		end

		-- try to call the setup func
		local ret, errmsg = pcall(setup_func)
		if not ret then
			return false, errmsg
		end
	else
		self:start_proxy(script_filename)
	end
	return true
end

function MySQLProxyTest:run_test()
	local result = 0

	local proc = process:new()
	local ret = proc:popen(
		self.testenv.MYSQL_TEST_BIN,
		{
			['MYSQL_USER']      = self.testenv.MYSQL_USER,
			['MYSQL_PASSWORD']  = self.testenv.MYSQL_PASSWORD,
			['PROXY_HOST']      = self.testenv.PROXY_HOST,
			['PROXY_PORT']      = self.testenv.PROXY_PORT,
			['PROXY_CHAIN_PORT']  = self.testenv.PROXY_CHAIN_PORT,
			['MASTER_PORT']     = self.testenv.PROXY_MASTER_PORT,
			['SLAVE_PORT']      = self.testenv.PROXY_SLAVE_PORT,
		},
		{
			user	 = self.testenv.MYSQL_USER,
			password = self.testenv.MYSQL_PASSWORD,
			database = self.testenv.MYSQL_DB,
			host	 = self.testenv.PROXY_HOST,
			port	 = self.testenv.PROXY_PORT,
			verbose  = (VERBOSE > 0) and "TRUE" or "FALSE", -- pass down the verbose setting 
			["test-file"]   = self.suite_srcdir .. "/t/" .. self.testname .. ".test",
			["result-file"] = self.suite_srcdir .. "/r/" .. self.testname .. ".result",
			["logdir"]      = self.suite_builddir, -- the .result dir might not be writable
		})

	if (ret == "ok") then
		print(("ok # %s"):format(self.testname))
		return 0
	elseif (ret == "not ok") then
		print(("not ok # %s"):format(self.testname))
		return -1
	else 
		print(("not ok # (%s) %s"):format(ret, self.testname))
		return -1
	end
end

local runner = TestRunner:new()
runner:register_tests({"base"})
local exitcode = runner:run_all()

if exitcode == 0 then
	os.exit(0)
else
	shellutils.print_verbose("mysql-test exit-code: " .. exitcode)
	os.exit(-1)
end

