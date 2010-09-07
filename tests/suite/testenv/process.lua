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

local _G = _G

local shellutils = require("testenv.shellutils")
local glib2 = require("glib2")

module("testenv.process")

---
-- life cycle management for processes
--
-- * PID files
-- * wait until they are started
-- * shutting down
--
function new(self, o)
	-- create a new process object
	--
	o = o or {}
	_G.setmetatable(o, self)
	self.__index = self
	return o
end

function get_pid(self)
	return self.pid
end

function set_pid(self, pid)
	self.pid = pid
end

---
-- read a PID from a pidfile
function set_pid_from_pidfile(self, pid_file_name)
	local fh, errmsg = _G.io.open(pid_file_name, 'r')
	if not fh then
		return false, errmsg
	end
	local pid = fh:read("*n")
	fh:close()

	if _G.type(pid) ~= "number" then
		return false
	end
	if not pid or pid == 0 then
		return false
	end

	self:set_pid(pid)

	return true
end

function is_running(self)
	_G.assert(self.pid)

	local ret = _G.os.execute("kill -0 ".. self.pid .."  2> /dev/null")

	return (ret == 0)
end

---
-- wait until the process is up and running
--
function wait_running(self, pid_file_name)
	-- try to get a PID
	local rounds = 0
	local wait_interval_ms = 100

	while not self.pid and rounds < 10 do
		self:set_pid_from_pidfile(pid_file_name)
		if self.pid then
			break
		end

		glib2.usleep(wait_interval_ms * 1000) -- wait until process is gone
		rounds = rounds + 1
		shellutils.print_verbose(("process:wait_running(pid = %s) waited %dms"):format(pid_file_name, wait_interval_ms * rounds))
	end

	-- check if the process is actually alive
	if not self.pid or not self:is_running() then
		return false
	end

	return true
end

function wait_down(self)
	_G.assert(self.pid)
	local rounds = 0
	local wait_interval_ms = 200

	-- wait until the proc in the pid file is dead
	-- the shutdown takes at about 500ms
	while self:is_running() do
		glib2.usleep(wait_interval_ms * 1000) -- wait until process is gone
		rounds = rounds + 1
		shellutils.print_verbose(("process:wait_down(pid = %d) waited %dms"):format(self.pid, wait_interval_ms * rounds))
	end

	return true
end

function shutdown(self)
	-- shut down the proxy
	--
	-- win32 has tasklist and taskkill on the shell
	if self.pid then
		return _G.os.execute("kill -TERM ".. self.pid)
	end
end

---
-- execute a process 
function execute(self, cmd, env, opts)
	local env_str = ""
	local opts_str = ""

	if env then
		env_str = shellutils.env_tostring(env)
	end

	if opts then
		opts_str = shellutils.options_tostring(opts)
	end

	local cmdline = env_str .. " " .. cmd .. " " .. opts_str
	shellutils.print_verbose("$ " .. cmdline)
	return _G.os.execute(cmdline)
end

function popen(self, cmd, env, opts)
	local env_str = ""
	local opts_str = ""

	if env then
		env_str = shellutils.env_tostring(env)
	end

	if opts then
		opts_str = shellutils.options_tostring(opts)
	end

	local cmdline = env_str .. " " .. cmd .. " " .. opts_str
	shellutils.print_verbose("$ " .. cmdline)

	local fh = _G.io.popen(cmdline)
	_G.assert(fh, 'error executing '.. cmdline)
	local result = ''
	local line = fh:read()
	while line do
		result = result .. line
		line = fh:read()
	end
	fh:close()
	return result
end

