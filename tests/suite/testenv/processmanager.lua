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

---
-- manage a set of processes
--
-- basicly, all we do is "add a process to the manager" and "shut them all down"
--

local _G = _G

module("testenv.processmanager")

---
-- init a new manager
--
function new(self, o)
	-- create a new processes object
	--
	o = o or {}
	_G.setmetatable(o, self)
	self.__index = self

	self.processes = { }

	return o
end

---
-- add a process
function add(self, proc)
	_G.table.insert(self.processes, proc)
end

---
-- stop all monitored processes
--
function shutdown_all(self)
	-- shuts down every proxy in the proxy list
	--
	for proc_name, proc in _G.pairs(self.processes) do
		if proc.pid then
			proc:shutdown()
		end
	end

	for proc_name, proc in _G.pairs(self.processes) do
		if proc.pid then
			proc:wait_down()
		end
		self.processes[proc_name] = nil
	end
end


