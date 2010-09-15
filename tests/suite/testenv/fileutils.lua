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

require("lfs")

module("testenv.fileutils", package.seeall)

---
-- check if the file exists and is readable 
function file_exists(f)
	local r = lfs.attributes(f)
	return (r ~= nil)
end

---
-- check if a path is absolute 
--
-- FIXME: right now it is Unix only
function path_is_absolute(f)
	return f:byte() == "/"
end

---
-- get the directory-name of a path
--
-- @param filename path to create the directory name from
function dirname(filename)
	local dirname = filename

	attr = assert(lfs.attributes(dirname))

	if attr.mode == "directory" then
		return dirname
	end

	dirname = filename:gsub("/[^/]+$", "")
	
	attr = assert(lfs.attributes(dirname))

	assert(attr.mode == "directory", "dirname("..filename..") failed: is ".. attr.mode)

	return dirname
end

---
-- get the file-name of a path
--
-- @param filename path to create the directory name from
function basename(filename)
	name = filename:gsub(".*/", "")
	
	return name
end

---
-- create a empty file
--
-- if the file exists, it will be truncated to 0
--
-- @param dst filename to create and truncate
function create_empty_file(dst)
	-- print_verbose("emptying " .. dst)
	local dst_fd = assert(io.open(dst, "wb+"))
	dst_fd:close();
end


