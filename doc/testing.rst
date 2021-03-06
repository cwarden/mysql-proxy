===================
testing MySQL Proxy
===================

The normal test suite is under ``./tests/suite/base``

Run it with::

  $ make check

Or if you want to run it manually, do::

  $ lua tests/run-tests.lua tests/suite/base

creating a new test
===================

A test case in this testing environment is made of the following components::

  ./t/test_name.test
  ./t/test_name.lua      (optional)
  ./t/test_name.options  (optional)  
  ./r/test_name.result

The basics are like in the server test suite.

a ``./t/test_name.test`` contains SQL statements and mysqltest commands as defined in the manual
http://dev.mysql.com/doc/mysqltest/en/mysqltest-commands.html

The corresponding ``./r/test_name.result`` contains the output of the statements above, as produced by ``mysqltest``.

In addition to the above basics, when preparing a test case for MySSQL Proxy, you can add the following ones:

./t/test_name.lua
  This is a Lua script that gets loaded to MySQL Proxy before the test starts.
  If no such test is defined, the Proxy starts with an empty script.

./t/test_name.options
  This file contains Lua instructions that are executed before the test.
  For the user's convenience, there are a few functions that you can call from this file
  
  * ``start_proxy(proxy_name, options_table)``
    starts a proxy instance, with given parameters. The proxy name is
    used to retrieve information about this instance, to use the proxy
    or to remove it
    example:

    .. code-block:: lua

      start_proxy('my_name', 
        {
         ["proxy-backend-addresses"] = PROXY_HOST .. ':' .. PROXY_MASTER_PORT ,
         ["proxy-read-only-backend-addresses"] = PROXY_HOST .. ':' .. PROXY_SLAVE_PORT ,
         ["proxy-address"]           = PROXY_HOST .. ':' .. PROXY_PORT ,
         ["admin-address"]           = PROXY_HOST .. ':' .. ADMIN_PORT ,
         ["pid-file"]                = PROXY_PIDFILE,
         ["proxy-lua-script"]        = 'my_name.lua',
         })
   
   As illustrated, there are several global variables that can be referenced within this
   options file::

     PROXY_HOST
     PROXY_PORT
     ADMIN_PORT
     (TODO: complete the list)
  
  * ``stop_proxy()``
    removes all proxy instances created with ``start_proxy()``
  
  * simulate_replication([master_options,slave_options])
    starts two instances of the proxy, both pointing to the same
    backend server. You can connect to a real replication, by
    supplying appropriate options
  
  * chain_proxy(first_lua_script, second_lua_script [, use_replication])
    starts two proxy instances, the first one pointing to the backend
    server, the second one pointing to the first instance.
    If use_replication is given (boolean), then a master backend is used
    instead of a real backend. If no master/slave replication is
    available, simulate_replication() is called
  
  * sql_execute(queries, proxy_name)
    sends one or more queries to a given proxy. ('queries' can be either
    a string or an array of strings)
    If no proxy name is provided, the query is sent to the backend server.




skipping tests
--------------

If you want to skip one or more tests, edit the file ``suite_name/tests_to_skip.lua``


creating a new test-suite
-------------------------

The Proxy test suite follows the same principles used by the server test suite.
There is a directory (usually ./t) containing .test scripts, with the 
instructions to run. And there is another directory (./r) containing the
expected results. For each `.test` file in `./t`, there must be a corresponding 
`.result` file in `./r`
For more information on the test framework, see the manual.
http://dev.mysql.com/doc/mysqltest/en/index.html

To run your test suite, create a directory under ./trunk/tests/suite, add two 
subdirectories /t and /r, and then use the above mentioned command.
For example::
 
  $ mkdir tests/suite/myapp
  $ mkdir tests/suite/myapp/t
  $ mkdir tests/suite/myapp/r
  
  # add test and result files under t and r
  
  $ lua tests/run-tests.lua tests/suite/myapp

library paths
-------------
The test suite uses the following paths to search for Lua libraries when 
a 'require' statement is issued. Each path is associated to an environment 
variable:

This directory contains Lua libraries

+-------------------------+--------------------------+---------------------------+
| variable                | default                  | description               |
+-------------------------+--------------------------+---------------------------+
| :envvar:`LUA_LDIR`      | /usr/share/lua/5.1/?.lua | server wide Lua libraries |
+-------------------------+--------------------------+---------------------------+
| :envvar:`LUA_PATH`      | /usr/local/share/?.lua   | MySQL Proxy Lua libraries |
+-------------------------+--------------------------+---------------------------+
| :envvar:`LUA_USER_PATH` | ./trunk/lib/?.lua        | user defined libraries    |
+-------------------------+--------------------------+---------------------------+

In addition to the above paths, the current suite is searched for
libraries as well.::

    suite_name ..  '/t/?.lua'  

troubleshooting
---------------

If Lua complains about missing the ``lfs`` library, prepend the :envvar:`LUA_CPATH` variable to the actual command::

  $ LUA_CPATH='tests/.libs/?.so' lua tests/run-tests.lua tests/suite/base

If the test suite complains about access denied, perhaps you need to provide a password. 

The default user for the test suite is 'root', with no password.

If you want to run the tests with a different username and password,
set the following environment variables:

* :envvar:`MYSQL_USER`
* :envvar:`MYSQL_PASSWORD`

