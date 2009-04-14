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

local f = assert(binlog.open("/tmp/binlog-test.log", "w"))

assert(f:append({
	-- timestamp defaults to _now_,
	server_id = 1,
	type = "FORMAT_DESCRIPTION_EVENT",
	-- log_pos is maintained automaticly
	-- flags defaults to 0
	-- event_size is maintained automaticly
	format = {
		master_version = "mysql-proxy-0.7.0",
		-- binlog_version = 4,
		-- created_ts defaults to _now_
	}

}))

assert(f:append({
	-- timestamp defaults to _now_,
	server_id = 1,
	type = "QUERY_EVENT",
	-- log_pos is maintained automaticly
	-- flags defaults to 0
	-- event_size is maintained automaticly
	query = {
		thread_id = 1,
		-- exec_time defaults to 0
		-- error_code defaults to 0
		query = "SELECT 1"
	}
}))

assert(f:append({
	-- timestamp defaults to _now_,
	server_id = 1,
	type = "QUERY_EVENT",
	-- log_pos is maintained automaticly
	-- flags defaults to 0
	-- event_size is maintained automaticly
	query = {
		thread_id = 1,
		db_name = "test",
		query = "SELECT 1"
	}
}))


assert(f:append({
	server_id = 1,
	type = "XID_EVENT",
	xid = {
		xid_id = 42
	}
}))

assert(f:append({
	server_id = 1,
	type = "STOP_EVENT",
}))


f:close()

local f = assert(binlog.open("/tmp/binlog-test.log"))
-- local f = assert(binlog.open("/usr/local/mysql/data/jan-kneschkes-macbook-pro-bin.001014"))

for event in f:next() do
	assert(event.timestamp)
	assert(event.server_id)
	assert(event.type)
	assert(event.log_pos)
	assert(event.flags)
	assert(event.event_size)

	-- try to decode the event 
	if event.type == "QUERY_EVENT" then
		assert(event.query.thread_id)
		assert(event.query.exec_time)
		assert(event.query.error_code)
		assert(event.query.query)
		-- print(("%d: %s"):format(event.query.thread_id, event.query.query))
	elseif event.type == "ROTATE_EVENT" then
		assert(event.rotate.binlog_file)
		assert(event.rotate.binlog_pos)
		--
		-- file = event.rotate.binlog_file
	elseif event.type == "XID_EVENT" then
		assert(event.xid.xid_id)
	elseif event.type == "INTVAR_EVENT" then
		assert(event.intvar.type) -- that should be a string 
		assert(event.intvar.value)
	elseif event.type == "FORMAT_DESCRIPTION_EVENT" then
		assert(event.format.master_version)
		assert(event.format.binlog_version)
		assert(event.format.created_ts)
		-- print(("format: %d, %s, %d"):format(event.format.binlog_version, event.format.master_version, event.format.created_ts))
	elseif event.type == "TABLE_MAP_EVENT" then
		-- if we want RBR to work, we have to track the table-map events 
		--
		f:table_register(event.table_map) -- register the current table, can be table or event

		assert(event.table_map.table_id)
		assert(event.table_map.flags)
		assert(event.table_map.db_name)
		assert(event.table_map.table_name)
		-- print(("tablemap: %d, %d, %s::%s"):format(event.table_map.table_id, event.table_map.flags, event.table_map.db_name, event.table_map.table_name))
	elseif event.type == "DELETE_ROWS_EVENT" or event.type == "UPDATE_ROWS_EVENT" or event.type == "WRITE_ROWS_EVENT" then
		local tbl = f:table_get(event.rbr.table_id)

		assert(event.rbr.table_id)
		assert(event.rbr.flags)
		print(("RBR: [%s] table=%d (%s), flags=%d"):format(event.type, event.rbr.table_id, tbl.table_name, event.rbr.flags))

		for row in event.rbr:next(tbl) do
			print(" row")
			local before = row.before
			local after  = row.after
			for field_ndx, field in ipairs(before) do
				print(("  before [%d] %s"):format(field_ndx, field))
			end
			if after then
				for field_ndx, field in ipairs(after) do
					print(("  after [%d] %s"):format(field_ndx, field))
				end
			end
		end
	else
		-- dump the unknown event to make it easier to add a decoder for them
		print(("-- unknown-event: %d, %d, %s"):format(event.timestamp, event.server_id, event.type))
	end

	---
	-- RBR 
	--
	-- track table-map definitions and decode RBR events
end

f:close()
