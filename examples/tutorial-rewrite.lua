--[[ $%BEGINLICENSE%$
 Copyright (c) 2007, 2008, Oracle and/or its affiliates. All rights reserved.

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

require("proxy.tokenizer")

---
-- read_query() can rewrite packets
--
-- You can use read_query() to replace the packet sent by the client and rewrite
-- query as you like
--
-- @param packet the mysql-packet sent by the client
--
-- @return 
--   * nothing to pass on the packet as is, 
--   * proxy.PROXY_SEND_QUERY to send the queries from the proxy.queries queue
--   * proxy.PROXY_SEND_RESULT to send your own result-set
--
function read_query( packet )
	if string.byte(packet) == proxy.COM_QUERY then
		local query = string.sub(packet, 2)

		print("we got a normal query: " .. query)

		local tokens = proxy.tokenizer.tokenize(query)

		-- support 
		--
		-- ls [db]
		-- cd db
		-- who

		if tokens[1].token_name == "TK_LITERAL" and tokens[1].text == "ls" then
			if #tokens == 1 then
				proxy.queries:append(1, string.char(proxy.COM_QUERY) .. "SHOW TABLES" )
				return proxy.PROXY_SEND_QUERY
			elseif #tokens == 2 then
				if tokens[2].token_name == "TK_LITERAL" then
					proxy.queries:append(1, string.char(proxy.COM_QUERY) .. "SHOW TABLES FROM " .. tokens[2].text)
					return proxy.PROXY_SEND_QUERY
				end
			end

		elseif tokens[1].text == "who" then
			if #tokens == 1 then
				proxy.queries:append(1, string.char(proxy.COM_QUERY) .. "SHOW PROCESSLIST" )
	
				return proxy.PROXY_SEND_QUERY
			end
		elseif tokens[1].text == "cd" then
			if #tokens == 2 then
				if tokens[2].token_name == "TK_LITERAL" then
					proxy.queries:append(1, string.char(proxy.COM_INIT_DB) .. tokens[2].text)
					return proxy.PROXY_SEND_QUERY
				end
			end
		end
	end
end

