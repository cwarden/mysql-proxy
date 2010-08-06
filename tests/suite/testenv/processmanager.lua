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


