=====================
Scripting MySQL Proxy
=====================

Tutorial
========

Commands passing by
-------------------

.. literalinclude:: ../examples/tutorial-basic.lua
  :language: lua
  :linenos:
  :lines: 26-

Rewriting packets
-----------------

.. literalinclude:: ../examples/tutorial-rewrite.lua
  :language: lua
  :linenos:
  :lines: 26-

Decoding Prepared Statements
----------------------------

.. literalinclude:: ../examples/tutorial-prep-stmts.lua
  :language: lua
  :linenos:
  :lines: 21-

Hooks
=====

The proxy plugin exposes some hooks to the scripting layer that get called in the different stages of the 
communication between client and backend:

.. msc::
	Client, Core, Backend, Plugin;

	--- [ label = "connect" ];
	Client -> Core [ label = "connect()" ];
	Core => Plugin [ label = "connect_server()" ];
	Core << Plugin [ label = "NO_DECISION" ];
	Core -> Backend [ label = "connect()" ];

	--- [ label = "auth challenge" ];
	Backend -> Core [ label = "read(auth challenge packet)" ];
	Core => Plugin [ label = "read_auth_handshake()" ];
	Core << Plugin [ label = "NO_DECISION" ];
	Core -> Client [ label = "write(auth challenge packet)" ];
	--- [ label = "auth response" ];
	Client -> Core [ label = "read(auth response packet)" ];
	Core => Plugin [ label = "read_auth()" ];
	Core << Plugin [ label = "NO_DECISION" ];
	Core -> Backend [ label = "write(auth response packet)" ];
	--- [ label = "auth status" ];
	Backend -> Core [ label = "read(auth response packet)" ];
	Core => Plugin [ label = "read_auth_result()" ];
	Core << Plugin [ label = "NO_DECISION" ];
	Core -> Client [ label = "write(auth response packet)" ];
	--- [ label = "query" ];
	Client -> Core [ label = "read(command packet)" ];
	Core => Plugin [ label = "read_query()" ];
	Core << Plugin [ label = "NO_DECISION" ];
	Core -> Backend [ label = "write(command packet)" ];
	--- [ label = "query response" ];
	Backend -> Core [ label = "read(command response packet)" ];
	Core => Plugin [ label = "read_query_result()" ];
	Core << Plugin [ label = "NO_DECISION" ];
	Core -> Client [ label = "write(command response packet)" ];

	--- [ label = "disconnect" ];
	Client -> Core [ label = "close()" ];
	Core => Plugin [ label = "disconnect_client()" ];
	Core << Plugin [ label = "ignored" ];
	Core -> Backend [ label = "close()" ];

They allow change the normal life-cycle:

* never connect to a backend
* replace commands
* inject commands
* replace responses

connect_server
--------------

.. js:function:: connect_server()

  intercept the ``connect()`` call to the backend

  :returns:
    nothing or ``nil``
      to connect to the backend using the standard backend selection algorithm
  
    :js:data:`proxy.PROXY_SEND_RESULT`
      doesn't connect to the backend, but returns the content of :js:data:`proxy.response`
      to the client

read_auth
---------

.. js:function:: read_auth(packet)

  :param string packet: the auth challenge packet as raw string
  :returns:
    nothing or ``nil``
      to forward the auth packet to the client

    :js:data:`proxy.PROXY_SEND_RESULT`
      replace the backends packet with the content of :js:data:`proxy.response`


read_auth_result
----------------

.. js:function:: read_auth_result(packet)

  :param string packet: the auth response packet as raw string
  :returns:
    nothing or ``nil``
      to forward the auth packet to the backend

    :js:data:`proxy.PROXY_SEND_RESULT`
      replace the clients packet with the content of :js:data:`proxy.response`

read_query
----------

.. js:function:: read_query(packet)

  :param string packet: the command packet as raw string
  :returns:
    nothing or ``nil``
      to forward the command packet to the backend

    :js:data:`proxy.PROXY_SEND_QUERY`
      send the first packet of proxy.queries to the backend

    :js:data:`proxy.PROXY_SEND_RESULT`
      send the client the content of :js:data:`proxy.response` and send nothing to the backend

read_query_result
-----------------

.. js:function:: read_query_result(inj)

  intercept the response to a command packet

  :param Injection inj: injection object
  :returns:
    nothing or ``nil``
      to forward the resultset to the client

    :js:data:`proxy.PROXY_SEND_RESULT`
      send the client the content of :js:data:`proxy.response`

    :js:data:`proxy.PROXY_IGNORE_RESULT`
      don't send the resultset to the client.

disconnect_client
-----------------

.. js:function:: disconnect_client()

  intercept the ``close()`` of the client connection

Public contants
===============

.. js:data:: proxy.MYSQLD_PACKET_OK
.. js:data:: proxy.MYSQLD_PACKET_ERR
.. js:data:: proxy.MYSQLD_PACKET_RAW

.. js:data:: proxy.PROXY_SEND_RESULT
.. js:data:: proxy.PROXY_IGNORE_RESULT
.. js:data:: proxy.PROXY_SEND_QUERY

.. js:data:: proxy.COM_QUERY
.. js:data:: proxy.COM_QUIT

.. js:data:: proxy.MYSQL_TYPE_STRING

.. todo::
  add all constants

Public structures
=================

.. js:data:: proxy.response

  carries the information what to return to the client in case of :js:data:`proxy.PROXY_SEND_RESULT`

  In has the fields:

  ``type`` (int)
    one of :js:data:`proxy.MYSQLD_PACKET_OK` :js:data:`proxy.MYSQLD_PACKET_ERR` or :js:data:`proxy.MYSQLD_PACKET_RAW`

  and more fields depending on ``type``:

  :js:data:`proxy.MYSQLD_PACKET_OK`
    ``resultset`` (table)
      a resultset which has the fields:
  
      ``fields``
        array of `name` and `type`
  
      ``rows``
        array of tables that contain the fields of each row
  
    ``affected_rows`` (int)
      affected rows 
   
    ``insert_id`` (int)
      insert id

  :js:data:`proxy.MYSQLD_PACKET_RAW`
    ``raw``
      string or table of packes to the return AS IS 

  :js:data:`proxy.MYSQLD_PACKET_ERR`
    ``errmsg``
      error message
  
    ``errcode``
      error code 
  
    ``sqlstate``
      SQL state

.. js:data:: proxy.global

  table that is shared between all connections

.. js:data:: proxy.connection

  table of connection data

  `client` (:js:class:`socket`)
    
  `server` (:js:class:`socket`)

  `backend_ndx` (int)

.. js:class:: socket

  table of socket info

  .. versionchanged:: 0.7.0

  `dst` (:js:class:`address`)
    destination address of this socket

  `src` (:js:class:`address`)
    source address of this socket

.. js:class:: address

  table of address info

  `name` (string)
    string of `address:port`

  `address` (string)
    
  `port` (int)

.. js:class:: Injection

  injection class

  `id` (int)
    id as passed in as first param of :js:func:`proxy.queries:append` or :js:func:`proxy.queries:prepend`

  `query` (string)
    packet as passed in as second param of :js:func:`proxy.queries:append` or :js:func:`proxy.queries:prepend`

  `query_time` (number)
    duration of the 

  `response_time` (number)
    duration of the 

  `resultset` (:js:data:`InjectionResultset`)
    resultset

.. js:class:: InjectionResultset

  resultset

.. js:attribute:: InjectionResultset.fields

  :type: (:js:class:`InjectionResultsetFields`)

.. js:attribute:: InjectionResultset.rows

  iterator that returns one row of the resultset at a time (:js:data:`InjectionResultsetRow`)

  .. code-block:: lua

    for row in inj.resultset.rows do
      print(row[1])
    end

.. js:attribute:: InjectionResultset.row_count

  rows in the rows table
  
  :type: int

.. js:attribute:: InjectionResultset.bytes

  bytes received for this resultset

  :type: int

.. js:attribute:: InjectionResultset.raw

  access to the first packet of the command response

  :type: string

.. js:attribute:: InjectionResultset.flags

  table of flags
     
  `in_trans` (bool)
    we are in a transaction

  `auto_commit` (bool)
    AUTO COMMIT is enabled

  `no_good_index_used` (bool)
    no good index used for this query

  `no_index_used` (bool)
    no index used for this query

.. js:attribute:: InjectionResultset.warning_count

  warnings generated by this command

  :type: int

.. js:attribute:: InjectionResultset.affected_rows

  rows affected

  :type: int

.. js:attribute:: InjectionResultset.insert_id

  insert id

  :type: int

.. js:attribute:: InjectionResultset.query_status

  :type: int or ``nil``

.. js:class:: InjectionResultsetFields 

  array of :js:class:`InjectionResultsetField`

.. js:function:: #InjectionResultsetFields

  :returns: number of columns in the resultset

.. js:class:: InjectionResultsetField

.. js:attribute:: InjectionResultsetField.type

  Column type as defined in :ref:`protocol-column-type`

  :type: int

.. js:attribute:: InjectionResultsetField.name

  name of the column

  :type: string

.. js:attribute:: InjectionResultsetField.org_name

  name of the column before aliasing with ``AS``

  :type: string

  .. code-block:: sql

    SELECT "foo" AS bar;

  results in :js:attr:`InjectionResultsetField.name` == ``bar`` and :js:attr:`InjectionResultsetField.org_name` == ``foo``

.. js:attribute:: InjectionResultsetField.table

  name of the table the column is from

  :type: string

.. js:attribute:: InjectionResultsetField.org_table

  :type: string

.. js:class:: InjectionResultsetRow

  array of fields of the row

Public functions
================

.. js:function:: proxy.queries:append(id, packet, [options])

  append a command packet to injection queue

  :param int id: injection id
  :param string packet: command packet to send to the backend 
  :param table options:
    `resultset_is_needed` (bool)
      true if `read_query_result` needs access to the resultset
  
.. js:function:: proxy.queries:prepend(id, packet, [options])

  prepend a command packet to injection queue

  :param int id: injection id
  :param string packet: command packet to send to the backend
  :param table options: 
    `resultset_is_needed` (bool)
      true if `read_query_result` needs access to the resultset
  

.. js:function:: proxy.queries:reset()

  reset the injection queue

.. js:function:: #proxy.queries

  :returns: number of command packets in the queue

Modules
=======

.. index:: 
  module: chassis

chassis
-------

.. js:function:: chassis.log(level, string)

  log string with level

  :param string level: log-level
  :param string string: string to log

.. js:function:: chassis.critical(string)

  log string with level ``critical``

  :param string string: string to log

.. js:function:: chassis.debug(string)

  log string with level ``debug``

  :param string string: string to log

.. js:function:: chassis.message(string)

  log string with level ``message``

  :param string string: string to log

.. js:function:: chassis.info(string)

  log string with level ``info``

.. js:function:: chassis.warning(string)

  log string with level ``warning``

  :param string string: string to log

.. js:function:: chassis.set_shutdown()

  asks the chassis to shutdown

.. js:function:: chassis.get_stats()

  :returns: table of internal stats

.. js:function:: chassis.mem_profile()

  calls http://library.gnome.org/devel/glib/stable/glib-Memory-Allocation.html#g-mem-profile


.. index:: 
  module: glib2

glib2
-----

.. js:function:: glib2.get_current_time()

  :returns: table

    `tv_sec`
      seconds since epoch

    `tv_usec`
      microseconds since last second

.. js:function:: glib2.md5(text)

  :param string text: text to hash
  :returns: (string) hashed text

.. js:function:: glib2.setenv(key, value)

  :param string key: environment variable to set
  :param string value: value of the environment variable

.. js:function:: glib2.usleep()

  :param int time: time to sleep in micro-seconds

.. index:: 
  module: mysql.proto

mysql.proto
-----------

The ``mysql.proto`` module provides encoders and decoders for the packets exchanged between client and server


.. js:function:: mysql.proto.from_err_packet(packet)

  Decodes a ERR-packet into a table.

  :param string packet: mysql packet
  :throws: an error
  :returns: a table

    ``errmsg`` (string)
    
    ``sqlstate`` (string)
    
    ``errcode`` (int)


.. js:function:: mysql.proto.to_err_packet(err)

  Encode a table containing a ERR packet into a MySQL packet.
  
  :param table err:
  
    ``errmsg`` (string)
    
    ``sqlstate`` (string)
    
    ``errcode`` (int)
  
  :returns: Returns a string.

.. js:function:: mysql.proto.from_ok_packet(packet)

  Decodes a OK-packet

  :param string packet: mysql packet
  :throws: an error
  :returns: table:

    ``server_status`` (int) bit-mask of the connection status
    
    ``insert_id`` (int) last used insert id
    
    ``warnings`` (int) number of warnings for the last executed statement
    
    ``affected_rows`` (int) rows affected by the last statement


.. js:function:: mysql.proto.to_ok_packet(ok)

  Encode a OK packet

.. js:function:: mysql.proto.from_eof_packet(packet)

  Decodes a EOF-packet
  
  :param string packet: mysql packet
  :throws: an error
  :returns: table

    ``server_status``
      (int) bit-mask of the connection status
    
    ``warnings``
      (int)
  

.. js:function:: mysql.proto.to_eof_packet(eof)

  Encode a EOF packet into a string

.. js:function:: mysql.proto.from_challenge_packet(packet)

  Decodes a auth-challenge-packet
  
  :param string packet: mysql packet
  :throws: an error
  :returns: table

    ``protocol_version``
      (int) version of the mysql protocol, usually 10
    
    ``server_version``
      (int) version of the server as integer: 50506 is MySQL 5.5.6
    
    ``thread_id``
      (int) connection id
    
    ``capabilities``
      (int) bit-mask of the server capabilities
    
    ``charset``
      (int) server default character-set
    
    ``server_status``
      (int) bit-mask of the connection-status
    
    ``challenge``
      (string) password challenge


.. js:function:: mysql.proto.to_challenge_packet

  Encode a auth-response-packet

.. js:function:: mysql.proto.from_response_packet

  Decodes a auth-response-packet
  
  :param string packet: mysql packet
  :throws: an error
  :returns: table

.. js:function:: mysql.proto.to_response_packet

  Encode a Auth Response packet

.. js:function:: mysql.proto.from_masterinfo_string

  Decodes the content of the ``master.info`` file.


.. js:function:: mysql.proto.to_masterinfo_string

  Encode a table into the content of ``master.info`` file

.. js:function:: mysql.proto.from_stmt_prepare_packet

  Decodes a COM_STMT_PREPARE-packet
  
  :param string packet: mysql packet
  :throws: an error
  :returns: a table containing

    ``stmt_text`` (string)
      text of the prepared statement
  
.. js:function:: mysql.proto.from_stmt_prepare_ok_packet

  Decodes a :ref:`com_stmt_prepare_ok_packet`
  
  :param string packet: mysql packet
  :throws: an error
  :returns: table
  
    ``stmt_id`` (int)
      statement-id
    
    ``num_columns`` (int)
      number of columns in the resultset
    
    ``num_params`` (int)
      number of parameters
    
    ``warnings`` (int)
      warnings generated by the prepare statement

.. js:function:: mysql.proto.from_stmt_execute_packet(packet, num_params)

  Decodes a COM_STMT_EXECUTE-packet
  
  :param string packet: mysql packet
  :param int num_params: number of parameters of the corresponding prepared statement
  
  :returns: table
  
    ``stmt_id`` (int)
      statement-id
    
    ``flags`` (int)
      flags describing the kind of cursor used
    
    ``iteration_count`` (int)
      iteration count: always 1
    
    ``new_params_bound`` (bool) 
    
    ``params`` (nil, table)
      number-index array of parameters if ``new_params_bound`` is ``true``
  
      Each param is a table of:
      
      ``type`` (int)
        MYSQL_TYPE_INT, MYSQL_TYPE_STRING ... and so on
      
      ``value`` (nil, number, string)
        if the value is a NULL, it ``nil``
        if it is a number (_INT, _DOUBLE, ...) it is a ``number``
        otherwise it is a ``string``
  
  If decoding fails it raises an error.
  
  To get the ``num_params`` for this function, you have to track the track the number of parameters as returned
  by the :js:func:`mysql.proto.from_stmt_prepare_ok_packet`. Use js:func:`mysql.proto.stmt_id_from_stmt_execute_packet` to get the ``statement-id`` from
  the COM_STMT_EXECUTE packet and lookup your tracked information.

  .. seealso::
  
    Example `Decoding Prepared Statements`_
       Example how to use :js:func:`stmt_id_from_stmt_execute_packet`

    :js:func:`mysql.proto.stmt_id_from_stmt_execute_packet`
       How to get the statement id 
 
.. js:function:: mysql.proto.stmt_id_from_stmt_execute_packet(packet)

  Decodes statement-id from a COM_STMT_EXECUTE-packet
  
  :param string packet: mysql packet
  :throws: an error
  :returns: the ``statement-id`` as ``int``

  .. seealso::
  
    Example `Decoding Prepared Statements`_
       Example how to use :js:func:`mysql.proto.stmt_id_from_stmt_execute_packet`
  
.. js:function:: mysql.proto.from_stmt_close_packet

  Decodes a COM_STMT_CLOSE-packet
  
  :param string packet: mysql packet
  :throws: an error
  :returns: On success it returns a table containing:
    ``stmt_id`` (int) statement-id that shall be closed

  


