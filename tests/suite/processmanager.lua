module("processmanager", package.seeall)

function new(self, o)
	-- create a new processes object
	--
	o = o or {}
	setmetatable(o, self)
	self.__index = self

	self.processes = { }

	return o
end

function add(self, proc)
	table.insert(self.processes, proc)
end

---
-- stop all monitored
--
function shutdown_all(self)
	-- shuts down every proxy in the proxy list
	--
	for proc_name, proc in pairs(self.processes) do
		if proc.pid then
			proc:shutdown()
		end
	end

	for proc_name, proc in pairs(self.processes) do
		if proc.pid then
			proc:wait_down()
		end
		self.processes[proc_name] = nil
	end
end


