==========================
Plugin and Scripting Layer
==========================

What is usually being referred to as `MySQL Proxy` is in fact the :ref:`page-plugin-proxy`.

While the :ref:`page-chassis` and :ref:`page-core` make up an important part, it is really the plugins that make MySQL Proxy so flexible.

One can describe the currently available plugins as `connection lifecycle interceptors` which can register callbacks for
all states in the :ref:`page-protocol`.

Currently available plugins in the main distribution include:

* :ref:`page-plugin-proxy`
* :ref:`page-plugin-admin`
* Replicator plugin
* Debug plugin
* CLI (command line) plugin

.. note::
  The latter three are not documented in-depth, mainly because they are Proof Of Concept implementations that are not targeted
  for the MySQL Proxy 1.0 GA release.

.. _page-plugin-proxy:

Proxy plugin
============

.. _page-plugin-admin:

Admin plugin
============

