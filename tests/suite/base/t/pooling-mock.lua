--[[ $%BEGINLICENSE%$
 Copyright (c) 2008, Oracle and/or its affiliates. All rights reserved.

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
-- test if connection pooling works
--
-- by comparing the statement-ids and the connection ids we can 
-- track if the ro-pooling script was reusing a connection
--

local proto = require("mysql.proto")

-- will be called once after connect
stmt_id = 0
conn_id = 0

function connect_server()
	-- the first connection inits the global counter
	if not proxy.global.conn_id then
		proxy.global.conn_id = 0
	end
	proxy.global.conn_id = proxy.global.conn_id + 1

	-- set our connection id
	conn_id = proxy.global.conn_id

	-- emulate a server
	proxy.response = {
		type = proxy.MYSQLD_PACKET_RAW,
		packets = {
			proto.to_challenge_packet({})
		}
	}
	return proxy.PROXY_SEND_RESULT
end

function read_query(packet)
	if packet:byte() ~= proxy.COM_QUERY then
		proxy.response = {
			type = proxy.MYSQLD_PACKET_OK
		}
		return proxy.PROXY_SEND_RESULT
	end

	-- query-counter for this connection
	stmt_id = stmt_id + 1

	local query = packet:sub(2) 
	if query == 'SELECT conn_id, stmt_id' then
		proxy.response = {
			type = proxy.MYSQLD_PACKET_OK,
			resultset = {
				fields = {
					{ name = 'conn_id', type = proxy.MYSQLD_TYPE_INT },
					{ name = 'stmt_id', type = proxy.MYSQLD_TYPE_INT },
				},
				rows = { { conn_id, stmt_id } }
			}
		}
	else
		proxy.response = {
			type = proxy.MYSQLD_PACKET_ERR,
			errmsg = "(pooling-mock) " .. query
		}
	end
	return proxy.PROXY_SEND_RESULT
end




