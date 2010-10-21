.. _page-chassis:

=======
Chassis
=======

The chassis provides the fundamentals that all applications need:

* commandline option handling
* config-file handling
* logging
* plugin loading/unloading
* daemon (unix)/service (win32) support

Configfile and Commandline Options
==================================

GLib2 provides us with config-file parser and a command-line option parser. We want to expose 
most options in the same way and accept them from the config-file and the command-line.

The options are parsed in two steps:

* extract the basic command-line options:

  * :option:`--help`
  * :option:`--version`
  * :option:`--defaults-file`

* process the `defaults file`_
* process the other command-line options to override the `defaults file`_

Basic options
-------------

.. option:: --help, -h

  show the help

.. option:: --version, -V

  show the version

.. option:: --defaults-file=<file>

  configuration file 

Defaults file
-------------

The format of the defaults file is defined in

  http://library.gnome.org/devel/glib/stable/glib-Key-value-file-parser.html#glib-Key-value-file-parser.description

and

  http://standards.freedesktop.org/desktop-entry-spec/latest/

Options
-------

The commandline options and the `defaults file`_ share the same set of known options. Depending on the type of option
they accept values in different forms:

============  =============================================================  =============
type          cmdline                                                        defaults file
============  =============================================================  =============
no value      :option:`--daemon`                                             ``daemon=1``
single value  :option:`--user` ``=foo``                                      ``user=foo``
multi value   :option:`--plugins` ``=proxy`` :option:`--plugins` ``=admin``  ``plugins=proxy,admin``
============  =============================================================  =============

`multi value` options always have a plural option name.


Common Options
--------------

Most frontends provide these options

.. option:: --daemon

  go into the background after parsing the configfile and loading the plugins

.. option:: --user=<user>

  try to drop the privileges and run as <user>

.. option:: --basedir=<dir>

  all relatives paths are relative to <dir>

.. _chassis-plugin:

Plugin Interface
================

The chassis provides the fundamentals for the plugin interface:

* it can resolve the path for plugins
* can load them in a portable way
* does version checks
* calls init and shutdown functions 
* exposes the configuration options to the plugins
* `Threaded IO`_

As the chassis is not MySQL specific it can load any kind of plugin as long as it 
exposes the init and shutdown functions. 

For the MySQL Proxy you usually load plugins like:

* :ref:`plugin-proxy`
* :ref:`plugin-admin`

... which interface with MySQL @ref section-lifecycle. 

Threaded IO
===========

In MySQL 0.8 we added threaded network-io to allow the proxy to scale out with the numbers of CPUs 
and network cards available.

To enable network-threading you just start the proxy with::

  --event-threads={2 * no-of-cores} (default: 0)

A event-thread is a simple small thread around ``event_base_dispatch()`` which on a network- or time-event
executes our core functions. These threads either execute the core functions or idle. If they idle 
they can read new events to wait for and add them to their wait-list.

A connection can jump between event-threads: the idling event-thread is taking the wait-for-event
request and executes the code. Whenever the connection has to wait for a event again it is unregister
itself from the thread, send its wait-for-event request to the global event-queue again.

Up to MySQL Proxy 0.8 the execution of the scripting code is single-threaded: a global mutex protects
the plugin interface. As a connection is either sending packets or calling plugin function the network 
events will be handled in parallel and only wait if several connections want to call a plugin function

Implementation
--------------

In ``chassis-event-thread.c`` the ``chassis_event_thread_loop()`` is the event-thread itself. It gets setup by
``chassis_event_threads_init_thread()``.

A typical control flow is depicted below (this does not describe the case where a connection pool is involved).

.. msc::

    hscale = "1.5";
    EventRequestQueue, MainThread, WorkerThread1, WorkerThread2;
    --- [ label = "Accepting new connection "];
    MainThread -> MainThread [ label = "network_mysqld_con_accept()" ];
    MainThread -> MainThread [ label = "network_mysqld_con_handle()" ];

    MainThread -> EventRequestQueue [ label = "Add wait-for-event request" ];
    WorkerThread1 <- EventRequestQueue [ label = "Retrieve Event request" ];
    WorkerThread1 -> WorkerThread1 [ label = "event_base_dispatch()" ];
    ...;
    WorkerThread1 -> WorkerThread1 [ label = "network_mysqld_con_handle()" ];
    
    WorkerThread1 -> EventRequestQueue [ label = "Add wait-for-event request" ];
    
    WorkerThread2 <- EventRequestQueue [ label = "Retrieve Event request" ];
    WorkerThread2 -> WorkerThread2 [ label = "event_base_dispatch()" ];
    ...;
    WorkerThread2 -> WorkerThread2 [ label = "network_mysqld_con_handle()" ];
    
    WorkerThread2 -> EventRequestQueue [ label = "Add wait-for-event request" ];
    ...;


In this example there are two event threads (``--event-threads=2``), each of which has its own ``event_base``.
The network_mysqld_con_accept() could for example be from the Proxy plugin, that opens a socket to listen on and sets the accept handler which
should get called whenever a new connection is made.

The accept handler is registered on the main thread's event_base (which is the same as the global chassis level event_base).
After setting up the ``network_mysqld_con`` structure it then proceeds to call the state machine handler, ``network_mysqld_con_handle()``,
still on the main thread.

The state machine enters its start state ``::CON_STATE_INIT``, which currently will *always* execute on the main thread.

At the first point where MySQL Proxy needs to interact with either the client or the server (either waiting for the socket
to be readable or needing to establish a connection to a backend), network_mysqld_con_handle() will schedule an `event
wait` request (a ``chassis_event_op_t``). It does so by adding the event structure into a asynchronous queue and generating a
file descriptor event by writing a single byte into the write file descriptor of the ``wakeup-pipe()``.

Signaling all threads for new events requests
---------------------------------------------

That pipe is a common hack in libevent to map any kind of event to a the fd-based event-handlers like poll:

* the ``event_base_dispatch()`` blocks until a fd-event triggers
* timers, signals, ... can't interrupt ``event_base_dispatch()`` directly
* instead they cause a ``write(pipe_fd, ".", 1);`` which triggers a fd-event which afterwards gets handled

In chassis-event-thread.c we use the pipe to signal that something is in the global event-queue to be
processed by one of the event-threads ... see chassis_event_handle(). All idling threads will process
that even and will pull from the event queue in parallel to add the event to their events to listen for.

To add a event to the event-queue you can call chassis_event_add() or chassis_event_add_local(). In general
all events are handled by the global event base, only in the case where we use the connection pool we force
events for the server connection to be delivered to the same thread that added it to the pool.

If the event would be delivered to the global event base a different thread could pick it up and that would
modify the unprotected connection pool datastructure, leading to race conditions and crashes. Making the
internal datastructures threadsafe is part of the 0.9 release cycle, thus only the minimal amount of
threadsafety is guaranteed right now.

Typically another thread will pick up this request from the queue (although in theory it could end up on the same thread
that issued the wait request) which will then add it to its thread-local event_base to be notified whenever the file
descriptor is ready.

This process continues until a connection is closed by a client or server or a network error occurs causing the sockets to
be closed. After that no new wait requests will be scheduled.

A single thread can have any number of events added to its thread-local event_base. It is only when a new blocking I/O
operation is necessary that the events can travel between threads, but not at any other point. Thus it is theoretically
possible that one thread ends up with all the active sockets while the other threads are idling.

However, since waiting for network events happens quite frequently, active connections should spread among the threads
fairly quickly, easing the pressure on the thread having the most active connections to process.

Note that, even though not depicted below, the main thread currently takes part in processing events after the accept
state. This is not ideal because all accepted connections need to go through a single thread. On the other hand, it has
not shown up as a bottleneck yet.

In more detail:

.. msc::

    hscale = "1.5";
    Plugin, MainThread, MainThreadEventBase, EventRequestQueue, WorkerThread1, WorkerThread1EventBase, WorkerThread2, WorkerThread2EventBase;
    --- [ label = "Accepting new connection "];
    Plugin -> MainThread [ label = "network_mysqld_con_accept()" ];
    MainThread -> MainThread [ label = "network_mysqld_con_handle()" ];

    MainThread -> EventRequestQueue [ label = "Add wait-for-event request" ];
    WorkerThread1 <- EventRequestQueue [ label = "Retrieve Event request" ];
    WorkerThread1 -> WorkerThread1EventBase [ label = "Wait for event on local event base" ];
    ...;
    WorkerThread1EventBase >> WorkerThread1 [ label = "Process event" ];
    
    WorkerThread1 -> EventRequestQueue [ label = "Add wait-for-event request" ];
    
    WorkerThread2 <- EventRequestQueue [ label = "Retrieve Event request" ];
    WorkerThread2 -> WorkerThread2EventBase [ label = "Wait for event on local event base" ];
    ...;
    WorkerThread2EventBase >> WorkerThread2 [ label = "Process event" ];
    
    WorkerThread2 -> EventRequestQueue [ label = "Add wait-for-event request" ];
    ...;

