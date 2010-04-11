---
-- interate over each event
--
function binlog_event_iterate(f, event)
	print(("# pos %d, size %d, event %s"):format(event.log_pos - event.event_size, event.event_size, event.type))
	if event.type == "QUERY_EVENT" then
		print("  " .. event.query.query)
	elseif event.type == "ROTATE_EVENT" then
		print(("  %s:%d"):format(event.rotate.binlog_file, event.rotate.binlog_pos))
	elseif event.type == "TABLE_MAP_EVENT" then
		print(("  id = %d (%s::%s)"):format(
			event.table_map.table_id,
			event.table_map.db_name,
			event.table_map.table_name
			))
		for i = 1, #event.table_map.fields do
			local field = assert(event.table_map.fields[i])

			print(("  [%d] %s = %s %s"):format(
				i,
				field.name or "fld_"..i,
				field.type,
				field.is_nullable and "nullable" or ""))
		end
		f:table_register(event) -- register the current table, can be table or event
	elseif event.type == "WRITE_ROWS_EVENT" then
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
	end
end
