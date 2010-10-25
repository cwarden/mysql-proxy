.. _page-plugins:

=======
Plugins
=======

What is usually being referred to as `MySQL Proxy` is in fact the :ref:`plugin-proxy`.

While the :ref:`page-chassis` and :ref:`page-core` make up an important part, it is really the plugins that make MySQL Proxy so flexible.

The MySQL Proxy package contains these plugins:

* :ref:`plugin-proxy`
* :ref:`plugin-admin`
* :ref:`plugin-debug`
* Replicator plugin

.. _plugin-proxy:

Proxy plugin
============

The :ref:`plugin-proxy` accepts connections on its :option:`--proxy-address` and forwards the data to one of the :option:`--proxy-backend-addresses`.

Its default behaviour can be overwritten by providing a :option:`--proxy-lua-script` and using :ref:`page-scripting`.

Options
-------

.. option:: --proxy-lua-script=<file>, -s

  Lua script to load at starting

.. option:: --proxy-address=<host:port|file>, -P <host:port|file>

  listening socket

  can be a unix-domain-socket or a IPv4 address

  :default: :4040

.. option:: --proxy-backend-addresses=<host:port|file>, -b <host:port|file>

  :default: 127.0.0.1:3306

.. option:: --proxy-read-only-backend-addresses=<host:port>, -r <host:port|file>

  only used if the scripting layer makes use of it

.. option:: --proxy-skip-profiling

  unused option

  .. deprecated:: 0.9.0

.. option:: --proxy-fix-bug-25371
  
  unused option

  .. deprecated:: 0.9.0

.. option:: --no-proxy

  unused option

  .. deprecated:: 0.9.0

.. option:: --proxy-pool-no-change-user

  don't use :ref:`protocol-com-change-user` to reset the connection before giving a connection
  from the connection pool to another client


.. _plugin-admin:

Admin plugin
============

Options
-------

.. option:: --admin-username=<username>

.. option:: --admin-password=<password>

.. option:: --admin-address=<host:port>

  :default: :4041

.. option:: --admin-lua-script=<file>

  :default: lib/mysql-proxy/admin.lua


.. _plugin-debug:

Debug plugin
============

The debug plugin accepts a connection from the mysql client and executes the queries as Lua commands.

Options
-------

.. option:: --debug-address=<host:port>

  :default: :4043

Examples
--------

Logging in into the debug plugin with the :command:`mysql` client::

  $ mysql --host=127.0.0.1 --port=4043

and executing a simple Lua command that returns the result of ``1 + 2``::

  mysql> return 1 + 2;
  +------+
  | lua  |
  +------+
  | 3    |
  +------+
  1 row in set (0.00 sec)

To dive into the internals of the chassis we can also load modules and get stats with :js:func:`chassis.get_stats`::

  mysql> require("chassis")
  mysql> return chassis.get_stats();
  +--------------+-------------------+---------------+---------------+
  | lua_mem_free | lua_mem_bytes_max | lua_mem_bytes | lua_mem_alloc |
  +--------------+-------------------+---------------+---------------+
  | 38           | 31963             | 31931         | 512           |
  +--------------+-------------------+---------------+---------------+




