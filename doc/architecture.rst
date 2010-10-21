============
Architecture
============

The MySQL Proxy is a simple program which sits between a mysql client and a mysql server and
can inspect, transform and act on the data sent through it.

You can use it for:

* load balancing
* fail over
* query tracking
* query analysis
* ... and much more

On the source code level it is built on 4 blocks:

+----------------------+----------------------+
| :ref:`page-chassis`  | :ref:`page-plugins`  |
|                      +----------------------+
|                      | :ref:`page-protocol` |
|                      +----------------------+
|                      | :ref:`page-core`     |
+----------------------+----------------------+

The :ref:`page-chassis` provides the common functions that all commandline and daemon applications
need: 

* commandline and configfiles
* logging
* daemon/service support
* plugin loading

The MySQL Procotol libraries encode and decode the :ref:`page-protocol`:

* client protocol
* binlog protocol
* myisam files
* frm files
* masterinfo files

The :ref:`page-core` exposes the phases of the :ref:`page-protocol` to a :ref:`page-plugins`:

.. digraph:: phases

    connect -> auth;
    auth -> command;
    command -> disconnect;
    command -> command;
    connect -> disconnect;
    auth -> disconnect;

Each of the phases of the life-cycle lead to several more protocol-states. For example the auth phase is made up of at least 3 packets:

.. msc::
	Client, Proxy, Server;

	Client -> Proxy [ label = "accept()" ];
	Proxy -> Proxy [ label = "script: connect_server()" ];
	Proxy -> Server [ label = "connect()" ];
	...;
	Server -> Proxy [ label = "recv(auth-challenge)" ];
	Proxy -> Proxy [ label = "script: read_handshake()" ];
	Proxy -> Client [ label = "send(auth-challenge)" ];
	Client -> Proxy [ label = "recv(auth-response)" ];
	Proxy -> Proxy [ label = "script: read_auth()" ];
	Server -> Proxy [ label = "send(auth-response)" ];
	Server -> Proxy [ label = "recv(auth-result)" ];
	Proxy -> Proxy [ label = "script: read_auth_result()" ];
	Proxy -> Client [ label = "send(auth-result)" ];
	...;

While the :ref:`page-core` is scalable to a larger number of connections, the plugin/:ref:`scripting`
layer hides the complexity from the end-users and simplifies the customization. 

