--[[ $%BEGINLICENSE%$
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

 $%ENDLICENSE%$ --]]
local binlog = assert(require("mysql.binlog"))

local f = assert(binlog.open("/Users/jan/jan-kneschkes-macbook-pro-bin.000001"))

for event in f:next() do
	assert(event.timestamp)
	assert(event.server_id)
	assert(event.type)
	assert(event.log_pos)
	assert(event.flags)
	assert(event.event_size)

	-- print(("%d, %d, %s"):format(event.timestamp, event.server_id, event.type))

	-- try to decode the event 
	if event.type == "QUERY_EVENT" then
		assert(event.query.thread_id)
		assert(event.query.query)
		-- print(("%d: %s"):format(event.query.thread_id, event.query.query))
	end
end

