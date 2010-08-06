require("lfs")

module("testenv.fileutils", package.seeall)

---
-- check if the file exists and is readable 
function file_exists(f)
	local r = lfs.attributes(f)
	return (r ~= nil)
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


