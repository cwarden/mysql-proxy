.. _page-protocol:

==============
MySQL Protocol
==============

The MySQL protocol is used between the MySQL Clients and the MySQL Server. It is implemented by

* the Connectors (Connector/C, ...)
* MySQL Proxy
* the MySQL Server itself for the slaves

It supports:

* transparent encryption via `SSL`_
* transparent compression via `Compressed Packet`_
* a challenge-response `Auth Phase`_ to never send the clients password in cleartext
* and a `Command Phase`_ which supports needs of `Prepared Statements`_ and `Stored Procedures`_


Documentation
=============

The documentation is sparse and is split between:

  http://forge.mysql.com/wiki/MySQL_Internals_ClientServer_Protocol

and the source files of the MySQL Server:

* sql/sql_parse.cc for the protocol basics

  * dispatch_command()

* sql/sql_prepare.cc for the prepared statement protocol

  * mysqld_stmt_prepare()
  * mysqld_stmt_execute()
  * mysqld_stmt_close()
  * mysqld_stmt_reset()
  * mysqld_stmt_fetch()
  * mysql_stmt_get_longdata()

* sql/sql_repl.cc for the binlog protocol

  * mysql_binlog_send()

* sql/protocol.cc for the value and type encoding

A mysql client logs in
======================

.. hint::
  All the examples here are captured with::

    $ ngrep -x -q -d lo0 '' 'port 3306'


Taking a look at the packet dump when a mysql-client logs in::

  client -> server 
    <connect>

The client initiates the communication by connecting to the server.::

  server -> client
    36 00 00 00 0a 35 2e 35    2e 32 2d 6d 32 00 03 00    6....5.5.2-m2...
    00 00 27 75 3e 6f 38 66    79 4e 00 ff f7 08 02 00    ..'u>o8fyN......
    00 00 00 00 00 00 00 00    00 00 00 00 00 57 4d 5d    .............WM]
    6a 7c 53 68 32 5c 59 2e    73 00                      j|Sh2\Y.s.      
  
which responds with a handshake packet which contains the version, some flags and a password challenge.::

  client -> server
    3a 00 00 01 05 a6 03 00    00 00 00 01 08 00 00 00    :...............
    00 00 00 00 00 00 00 00    00 00 00 00 00 00 00 00    ................
    00 00 00 00 72 6f 6f 74    00 14 cb b5 ea 68 eb 6b    ....root.....h.k
    3b 03 cb ae fb 9b df 5a    cb 0f 6d b5 de fd          ;......Z..m...  

The client answers with username, some flags and the response to the challenge.::

  server -> client
    07 00 00 02 00 00 00 02    00 00 00                   ...........     
  
As the client provided the right password and the flags are fine, the server responds with a `OK packet`_. That closes auth-phase
and switches to the command-phase.::

  client -> server
    21 00 00 00 03 73 65 6c    65 63 74 20 40 40 76 65    !....select @@ve
    72 73 69 6f 6e 5f 63 6f    6d 6d 65 6e 74 20 6c 69    rsion_comment li
    6d 69 74 20 31                                        mit 1           
 
The mysql client first checks the version string of the server and sends a `COM_QUERY`_ packet.::

  server -> client
    01 00 00 01 01 27 00 00    02 03 64 65 66 00 00 00    .....'....def...
    11 40 40 76 65 72 73 69    6f 6e 5f 63 6f 6d 6d 65    .@@version_comme
    6e 74 00 0c 08 00 1c 00    00 00 fd 00 00 1f 00 00    nt..............
    05 00 00 03 fe 00 00 02    00 1d 00 00 04 1c 4d 79    ..............My
    53 51 4c 20 43 6f 6d 6d    75 6e 69 74 79 20 53 65    SQL Community Se
    72 76 65 72 20 28 47 50    4c 29 05 00 00 05 fe 00    rver (GPL)......
    00 02 00                                              ...             

The server responds with a resultset containing the version-string.::

  client -> server
    0e 00 00 00 03 73 65 6c    65 63 74 20 55 53 45 52    .....select USER
    28 29                                                 ()              

For the prompt (\u ...) the mysql client also asks for the current username.::

  server -> client
    01 00 00 01 01 1c 00 00    02 03 64 65 66 00 00 00    ..........def...
    06 55 53 45 52 28 29 00    0c 08 00 4d 00 00 00 fd    .USER()....M....
    01 00 1f 00 00 05 00 00    03 fe 00 00 02 00 0f 00    ................
    00 04 0e 72 6f 6f 74 40    6c 6f 63 61 6c 68 6f 73    ...root@localhos
    74 05 00 00 05 fe 00 00    02 00                      t.........      

which is 'root@localhost' in this example.

Basic Types
===========

The protocol has a few very basic types that are used throughout the protocol:

* integers and
* strings

Integer
-------

The MySQL Protocol has a set of possible encodings for integers:

* fixed length intergers
* length encoded integers

fixed length integer
....................

The fixed length integers can be of a byte-length 1, 2, 3, 4 or 8 and send their first byte first. The packet length
for example is::

  01 00 00

is a 3-byte fixed length integer with the value `1`.

length encoded integer
......................

In other places integers have a variable size of 1, 3, 4 or 9 bytes depending on their value:

==========================  ======
value                       bytes
==========================  ======
``< 251``                   1
``>= 251 < (2^16 - 1)``     3
``>= (2^16) < (2^24 - 1)``  4
``>= (2^24)``               9
==========================  ======

The 1-byte values from 251 to 255 have a special meaning and aren't used for integers. Instead they
signal special packets or the 3 other variable length integer types:

========  ===  ===========
hex       dec  description
========  ===  ===========
``0xfb``  251  NULL in the `Text Resultset Row`_
``0xfc``  252  indicator for a 2-byte integer
``0xfd``  253  indicator for a 3-byte integer
``0xfe``  254  indicator for a 8-byte integer or first byte of a `EOF packet`_
``0xff``  255  first byte of a `ERR packet`_
========  ===  ===========

They also send least significant byte first.

String
------

Strings are sequences of bytes and appear in a few forms in the protocol:

_`Fixed Length String`
  Fixed length strings have a known, hardcoded length. An example is the `sql-state` of the `ERR packet`_ which is always 5 byte long.

_`NUL-terminated String`
  Strings that are terminated by a [00] byte.

_`Length Encoded String`
  A length encoded string is a string that is prefixed with `length encoded integer`_ describing the length of the string.

_`End-of-packet String`
  If a string is the last component of a packet, its length can be calculated from the overall-packet length minus the current position.

Describing packets
------------------

In this document we describe each packet by first defining its ``payload`` and provide a ``example`` showing each `MySQL Packet` that is sent include the `Packet Header`_::

  <packetname>
    <description>

    direction: client -> server
    response: <response>
    
    payload:
      <type>        <description>

    Example:
      01 00 00 00 01

The `<type>` describes the sequence of bytes of the packet:

============== ===========
type           description
============== ===========
1              1 byte `fixed length integer`_
2              2 byte `fixed length integer`_
3              3 byte `fixed length integer`_
4              4 byte `fixed length integer`_
8              8 byte `fixed length integer`_
lenenc-int     `length encoded integer`_
string         `NUL-terminated string`_
string[p]      `End-of-packet string`_
string[`<n>`]  fixed length string with the length `<n>`
lenenc-str     `length encoded string`_
n              a byte sequence of any length
============== ===========

.. attention::
  Some packets have optional fields or a different layout depending on the `capability flags`_ that are sent as part of the
  `Auth Response Packet`_.

If a field has a fixed value its description will show it as hex value in brackets like `[00]`.

MySQL Packet
============

If a MySQL Client or Server wants to send data it:

* splits the data in chunks of (2^24-1) packets
* each chunk is prepended with a `packet header`_

Packet Header
-------------

The packets that are exchanged between client and server look like::

  ...
  T 127.0.0.1:51656 -> 127.0.0.1:3306 [AP]
    01 00 00 00 01 

The example shows a `COM_QUIT`_ packet. It starts (like all packets) with a 4 byte packet header:

* 3 byte payload length
* 1 byte sequence-id

::

  MySQL packet
    layout:
      3              payload length
      1              sequence id
      n              payload 

sending more than 16Mbyte
.........................

If the payload is larger than or equal to 2^24-1 bytes the length is set to 2^24-1 (``ff ff ff``) and a additional packets are sent
with the rest of the payload until the payload of a packet is less than 2^24-1 bytes.

Sending a payload of 16 777 215 (2^16-1) bytes looks like::

  ff ff ff 00 ...
  00 00 00 01

Sequence ID
........... 

The sequence-id is incremented with each packet and may wrap around. It starts at 0 and is reset to 0 when a new command begins in the `Command Phase`_.

Compressed Packet
=================

Compression is extension to the MySQL Protocol, is transparent to the rest of the MySQL protocol and allows to compress chunks of bytes (e.g. a `MySQL Packet`_).

It is enabled if the server announces `CLIENT_COMPRESS`_ in its `Auth Challenge Packet`_ and the clients requests it too in its `Auth Response Packet`_. It is used
only in the `Command Phase`_.

It uses the `deflate` algorithm as defined in RFC 1951 and implemented in zlib. The header of the compressed packet
has the parameters of the `uncompress()` function in mind::

  ZEXTERN int ZEXPORT uncompress OF((Bytef *dest,   uLongf *destLen,
                                   const Bytef *source, uLong sourceLen));

The compressed packet consists of a header and a payload::

  compressed packet
    layout:
      3              length of compressed payload 
      1              compressed sequence id
      3              length of payload before compression
      n              compressed payload

A `COM_QUERY`_ for ``select "012345678901234567890123456789012345"`` without `CLIENT_COMPRESS`_ has a `payload length` of 46 bytes looks like::

  2e 00 00 00 03 73 65 6c    65 63 74 20 22 30 31 32    .....select "012
  33 34 35 36 37 38 39 30    31 32 33 34 35 36 37 38    3456789012345678
  39 30 31 32 33 34 35 36    37 38 39 30 31 32 33 34    9012345678901234
  35 22                                                 5"

with `CLIENT_COMPRESS`_ the packet is:: 

  22 00 00 00 32 00 00 78    9c d3 63 60 60 60 2e 4e    "...2..x..c```.N
  cd 49 4d 2e 51 50 32 30    34 32 36 31 35 33 b7 b0    .IM.QP20426153..
  c4 cd 52 02 00 0c d1 0a    6c                         ..R.....l 

+--------------+--------+--------------+--------------------------------------------+
| comp-length  | seq-id | uncomp-len   | compressed payload                         |
+--------------+--------+--------------+--------------------------------------------+
| ``22 00 00`` | ``00`` | ``32 00 00`` | compress(``"\x2e\x00\x00\x00select ..."``) |
+--------------+--------+--------------+--------------------------------------------+

The compressed packet is 41 bytes long and splits into::

  raw packet length                      -> 41
  compressed payload length   = 22 00 00 -> 34 (41 - 7)
  sequence id                 = 00       ->  0
  uncompressed payload length = 32 00 00 -> 50

_`length of compressed payload`
  `raw packet length` minus the size of the compressed packet header (7 bytes) itself. 

_`compressed sequence id`
  sequence id of the compressed packets, reset in same way as the `MySQL Packet`_, but incremented independently

_`length of payload before compression`
  size of `compressed payload`_ before it was compressed.

_`compressed payload`
  payload compressed with zlib's deflate algorithm, see `More than one MySQL Packet`_ 

.. attention:: If `length of payload before compression`_ is `0`, the `compressed payload`_ field contains the `uncompressed payload`_.

Mapping this back to the `uncompress()` function we get:

dest
  has to be at least `length of payload before compression`_ long

destLen
  `length of payload before compression`_

source
  `compressed payload`_

sourceLen
  `length of compressed payload`_

Using aboves example as input the code should look a bit like:

.. code-block:: c

  const Bytef *src = "\x22\x00...";
  const Bytef *comp_payload = src + 7; /* skip the compressed packet header */
  uLongf comp_payload_len = 34;
  uLongf dst_len = 50;
  Bytef *dst = malloc(dst_len);

  if ((Z_OK != uncompress(dst, &dst_len, comp_payload, comp_payload_len))) { ...

Pretty printing `dst` results in::

  2e 00 00 00 03 73 65 6c    65 63 74 20 22 30 31 32    .....select "012
  33 34 35 36 37 38 39 30    31 32 33 34 35 36 37 38    3456789012345678
  39 30 31 32 33 34 35 36    37 38 39 30 31 32 33 34    9012345678901234
  35 22                                                 5"

... aka our uncompressed MySQL packet.

More than one MySQL packet
--------------------------

A compressed packet can contain several MySQL Packets.

A `Text resultset`_ for ``SELECT repeat("a", 50)`` looks like::

  01 00 00 01 01 25 00 00    02 03 64 65 66 00 00 00    .....%....def...
  0f 72 65 70 65 61 74 28    22 61 22 2c 20 35 30 29    .repeat("a", 50)
  00 0c 08 00 32 00 00 00    fd 01 00 1f 00 00 05 00    ....2...........
  00 03 fe 00 00 02 00 33    00 00 04 32 61 61 61 61    .......3...2aaaa
  61 61 61 61 61 61 61 61    61 61 61 61 61 61 61 61    aaaaaaaaaaaaaaaa
  61 61 61 61 61 61 61 61    61 61 61 61 61 61 61 61    aaaaaaaaaaaaaaaa
  61 61 61 61 61 61 61 61    61 61 61 61 61 61 05 00    aaaaaaaaaaaaaa..
  00 05 fe 00 00 02 00                                  .......         

which consists of 5 `MySQL Packet`_.

With `CLIENT_COMPRESS`_ on it is sent in one compressed packet::

  4a 00 00 01 77 00 00 78    9c 63 64 60 60 64 54 65    J...w..x.cd``dTe
  60 60 62 4e 49 4d 63 60    60 e0 2f 4a 2d 48 4d 2c    ``bNIMc``./J-HM,
  d1 50 4a 54 d2 51 30 35    d0 64 e0 e1 60 30 02 8a    .PJT.Q05.d..`0..
  ff 65 64 90 67 60 60 65    60 60 fe 07 54 cc 60 cc    .ed.g``e``..T.`.
  c0 c0 62 94 48 32 00 ea    67 05 eb 07 00 8d f9 1c    ..b.H2..g.......
  64                                                    d 

+--------------+--------+--------------+----------------------------------------------------------------------------------------------------------------------------------+
| comp-length  | seq-id | uncomp-len   | compressed payload                                                                                                               |
+--------------+--------+--------------+----------------------------------------------------------------------------------------------------------------------------------+
| ``4a 00 00`` | ``01`` | ``77 00 00`` | compress(the `Text resultset`_ which is                                                                                          |
|              |        |              |                                                                                                                                  |
|              |        |              | * ``01 00 00 01 01``                                                                                                             |
|              |        |              | * ``25 00 00 02 03 64 65 66 00 00 00 0f 72 65 70 65 61 74 28 22 61 22 2c 20 35 30 29 00 0c 08 00 32 00 00 00 fd 01 00 1f 00 00`` |
|              |        |              | * ``05 00 00 03 fe 00 00 02 00``                                                                                                 |
|              |        |              | * ``33 00 00 04 32 61 61 61 61 ...``                                                                                             |
|              |        |              | * ``05 00 00 05 fe 00 00 02 00``                                                                                                 |
|              |        |              |                                                                                                                                  |
|              |        |              | )                                                                                                                                |
+--------------+--------+--------------+----------------------------------------------------------------------------------------------------------------------------------+

Splitting MySQL Packets
-----------------------

A MySQL packet may be split across several compressed packets.

If look at the `Text resultset`_ for query like ``SELECT repeat("a", 256 * 256 * 256 - 5)`` it generate 6 MySQL Packets with the lengths and sequence-ids::

  01 00 00 01
  36 00 00 02
  05 00 00 03
  ff ff ff 04
  00 00 00 05
  05 00 00 06

If we would try to squeeze into one compressed packet the `length of payload before compression`_ would be::

  (0x01 + 4) +
  (0x36 + 4) +
  (0x05 + 4) +
  (0xffffff + 4) +
  (0x00 + 4)
  (0x05 + 4)
  = 0x100005c

which is too large for one compressed packet. Instead the server splits the packet stream into 3 compressed packets::

  6d 00 00 01 00 40 00 78    9c ed cb 31 0a c2 40 00    m....@.x...1..@.
  44 d1 59 a3 60 e1 1d 0c    a9 54 b4 11 92 fb 04 8c    D.Y.`....T......
  b5 88 a7 57 74 4d 69 6b    ff 5e f1 8b 81 29 49 29    ...WtMik.^...)I)
  43 b2 68 2e d3 35 49 7b    9f 6e d3 f8 d8 75 63 77    C.h..5I{.n...ucw
  6c cf fd d0 1e 7e 7a 6a    fb 7d 36 eb bc 6a cd b3    l....~zj.}6..j..
  64 9b ac 92 e6 33 bf 53    6b 5d be e7 7d 04 00 00    d....3.Sk]..}...
  00 00 00 00 00 00 00 00    00 00 00 00 00 fe f6 05    ................
  0c 60 37 dc                                           .`7.

... a first packet containing the first 16kByte (``00 40 00``) which is packet 01, 02, 03 and the start of packet 04. The rest of packet 04 is sent alone:: 

  af 3f 00 02 4b c0 ff 78    9c ec c1 81 00 00 00 00    .?..K..x........
  80 20 d6 fd 25 16 a9 0a    00 00 00 00 00 00 00 00    . ..%...........
  00 00 00 00 00 00 00 00    00 00 00 00 00 00 00 00    ................
  [...]

... and last packet that is too small to be compressed and is sent as `Uncompressed payload`_ containing packet 05 and 06::

  0d 00 00 03 00 00 00 00    00 00 05 05 00 00 06 fe    ................
  00 00 02 00                                           ....

Uncompressed payload
--------------------

If the `length of payload before compression`_ is `0` the `compressed payload`_ field contains the uncompressed payload.

.. hint:: Usually payloads less than 50 bytes (``MIN_COMPRESS_LENGTH``) aren't compressed.

Taking the example from above::

  0d 00 00 03 00 00 00 00    00 00 05 05 00 00 06 fe    ................
  00 00 02 00                                           ....

decodes into::

  raw packet length                      -> 20
  compressed payload length   = 0d 00 00 -> 13 (20 - 7)
  sequence id                 = 03       ->  3
  uncompressed payload length = 00 00 00 -> uncompressed

... with the `uncompressed payload` starting right after the 7 byte header::

  00 00 00 05                   -- a "empty" packet
  05 00 00 06 fe 00 00 02 00    -- a EOF packet

SSL
===

The MySQL Protocol also supports encryption and authentication via SSL. The encryption is transparent to
the rest of the protocol and is applied after the data is compressed right before the data is written
to the network layer.

The SSL suppport is announced in
`Auth Challenge Packet`_ sent by the server via `CLIENT_SSL`_ and is enabled if the client returns the same
capability.

For a unencrypted connection the server starts with its `Auth Challenge Packet`_::

  36 00 00 00 0a 35 2e 35    2e 32 2d 6d 32 00 52 00    6....5.5.2-m2.R.
  00 00 22 3d 4e 50 29 75    39 56 00 ff ff 08 02 00    .."=NP)u9V......
  00 00 00 00 00 00 00 00    00 00 00 00 00 29 64 40    .............)d@
  52 5c 55 78 7a 7c 21 29    4b 00                      R\Uxz|!)K.      

... and the client returns its `Auth Response Packet`_::

  3a 00 00 01 05 a6 03 00    00 00 00 01 08 00 00 00    :...............
  00 00 00 00 00 00 00 00    00 00 00 00 00 00 00 00    ................
  00 00 00 00 72 6f 6f 74    00 14 14 63 6b 70 99 8a    ....root...ckp..
  b6 9e 96 87 a2 30 9a 40    67 2b 83 38 85 4b          .....0.@g+.8.K

If client wants to do SSL and the server supports it, it would send a shortened `Auth Response Packet`_
with the `CLIENT_SSL`_ capability enabled instead::

  20 00 00 01 05 ae 03 00    00 00 00 01 08 00 00 00     ...............
  00 00 00 00 00 00 00 00    00 00 00 00 00 00 00 00    ................
  00 00 00 00                                           ....            

The `Auth Response Packet`_ is truncated right before the `username` field. The rest of the communication
is switch to SSL::

  16 03 01 00 5e 01 00 00    5a 03 01 4c a3 49 2e 7a    ....^...Z..L.I.z
  b5 06 75 68 5c 30 36 73    f1 82 79 70 58 4c 64 bb    ..uh\06s..ypXLd.
  47 7e 90 cd 9b 30 c5 66    65 da 35 00 00 2c 00 39    G~...0.fe.5..,.9
  00 38 00 35 00 16 00 13    00 0a 00 33 00 32 00 2f    .8.5.......3.2./
  00 9a 00 99 00 96 00 05    00 04 00 15 00 12 00 09    ................
  00 14 00 11 00 08 00 06    00 03 02 01 00 00 04 00    ................
  23 00 00                                              #..

The above packet is from `SSL_connect()` which does the SSL greeting and certificate exchange. Once the SSL
tunnel is established, the client sends the _full_ `Auth Response Packet`_ again and the normal communication
continues.

Generic Response Packets
========================

For most of the commands the client sends to the server one of two packets is returned as response:

* `OK packet`_
* `ERR packet`_

OK packet
---------

The payload of the OK packet contains a warning count if `CLIENT_PROTOCOL_41`_ is enabled.

::

  OK
    
    direction: server -> client

    payload:
      1              [00] the OK header
      lenenc-int     affected rows
      lenenc-int     last-insert-id
      2              status flags
        if capabilities & CLIENT_PROTOCOL_41:
      2              warnings 

    example:
      07 00 00 02 00 00 00 02    00 00 00                   ...........     

Status Flags
............
 
The status flags are a bit-field:

====== =============
flag   constant name
====== =============
0x0001 SERVER_STATUS_IN_TRANS
0x0002 SERVER_STATUS_AUTOCOMMIT
0x0008 _`SERVER_MORE_RESULTS_EXISTS`
0x0010 SERVER_STATUS_NO_GOOD_INDEX_USED
0x0020 SERVER_STATUS_NO_INDEX_USED
0x0040 SERVER_STATUS_CURSOR_EXISTS
0x0080 SERVER_STATUS_LAST_ROW_SENT
0x0100 SERVER_STATUS_DB_DROPPED
0x0200 SERVER_STATUS_NO_BACKSLASH_ESCAPES
0x0400 SERVER_STATUS_METADATA_CHANGED
0x0800 SERVER_QUERY_WAS_SLOW
0x1000 SERVER_PS_OUT_PARAMS
====== =============

ERR packet
----------

The ERR packet contains a SQL-state if `CLIENT_PROTOCOL_41`_ is enabled.

::

  ERR
    
    direction: server -> client

    payload:
      1              [ff] the ERR header
      2              error code 
        if capabilities & CLIENT_PROTOCOL_41:
      1              '#' the sql-state marker
      string[5]      sql-state
        all protocols:
      string[p]      error-message

    example:
      17 00 00 01 ff 48 04 23    48 59 30 30 30 4e 6f 20    .....H.#HY000No 
      74 61 62 6c 65 73 20 75    73 65 64                   tables used 

Character Set
=============

MySQL has a very flexible character set support as documented in:

  http://dev.mysql.com/doc/refman/5.1/en/charset.html

The list of character sets and their IDs can be queried with::

  SELECT id, collation_name FROM information_schema.collations ORDER BY id;
  +----+-------------------+
  | id | collation_name    |
  +----+-------------------+
  |  1 | big5_chinese_ci   |
  |  2 | latin2_czech_cs   |
  |  3 | dec8_swedish_ci   |
  |  4 | cp850_general_ci  |
  |  5 | latin1_german1_ci |
  |  6 | hp8_english_ci    |
  |  7 | koi8r_general_ci  |
  |  8 | latin1_swedish_ci |
  |  9 | latin2_general_ci |
  | 10 | swe7_swedish_ci   |
  +----+-------------------+

A few common ones are:

====== ==== ==================
number hex  character set name
====== ==== ==================
[...] 
8      0x08 latin1_swedish_ci
[...] 
33     0x21 utf8_general_ci
[...] 
63     0x3f binary
[...]
====== ==== ==================

Auth Phase
==========

A simple MySQL 4.1+ auth starts with:

1. the client connecting to the server
2. the server responds with the `Auth Challenge Packet`_
3. the client sends the `Auth Response Packet`_
4. the server responds with `OK Packet`_

If the auth fails, it sends a `ERR Packet`_ instead of a `OK Packet`_ and closes the connection:

1. the client connecting to the server
2. the server responds with the `Auth Challenge Packet`_
3. the client sends the `Auth Response Packet`_
4. the server responds with `ERR Packet`_ and closes connection

or the server denies the client right away if for example its IP is deny:

1. the client connecting to the server
2. the server responds with the `ERR Packet`_ and closes connection

MySQL 4.1+ server also may respond at step 4 with a `Auth Method Switch Request Packet`_:

1. the client connecting to the server
2. the server responds with the `Auth Challenge Packet`_
3. the client sends the `Auth Response Packet`_
4. the server responds with the `Auth Method Switch Request Packet`_
5. the client sends the `Auth Method Switch Response Packet`_
6. the server responds with `OK Packet`_ or `ERR Packet`_ and closes the connection

Auth Challenge Packet
---------------------

As first packet the server sends a Auth Challenge to the client. It contains several other fields:

* the protocol version
* the mysql-server version string
* the server capabilities
* the auth challenge

The client answers with a `Auth Response Packet`_.

::

  Auth Challenge Packet
    response: Auth Response Packet

    payload:
      1              [0a] protocol version
      string         server version
      4              connection id
      string[8]      challenge-part-1
      1              [00] filler
      2              capability flags
      1              character set
      2              status flags
      string[13]     reserved
        if capabilities & SECURE_CONNECTION:
      string[12]     challenge-part-2
      1              [00] filler

    example:
      36 00 00 00 0a 35 2e 35    2e 32 2d 6d 32 00 0b 00    6....5.5.2-m2...
      00 00 64 76 48 40 49 2d    43 4a 00 ff f7 08 02 00    ..dvH@I-CJ......
      00 00 00 00 00 00 00 00    00 00 00 00 00 2a 34 64    .............*4d
      7c 63 5a 77 6b 34 5e 5d    3a 00                      |cZwk4^]:.      

`status flags` is defined as the `Status Flags`_ of the `OK packet`_.

`character set` is the server's default character set and is defined in `Character Set`_.

The `auth challenge` is concated string of `challenge-part-1` and `challenge-part-2`.

Capability flags
................

The capability flags are used by the client and server to indicate which features
they support and want to use.

====== ==============================  ==================================
flags    constant name                   description
====== ==============================  ==================================
0x0001 CLIENT_LONG_PASSWORD            new more secure passwords
0x0002 CLIENT_FOUND_ROWS               Found instead of affected rows
0x0004 CLIENT_LONG_FLAG                Get all column flags
0x0008 CLIENT_CONNECT_WITH_DB          One can specify db on connect
0x0010 CLIENT_NO_SCHEMA                Don't allow database.table.column
0x0020 _`CLIENT_COMPRESS`              Can use compression protocol
0x0040 CLIENT_ODBC                     Odbc client
0x0080 _`CLIENT_LOCAL_FILES`           Can use LOAD DATA LOCAL
0x0100 CLIENT_IGNORE_SPACE             Ignore spaces before '('
0x0200 _`CLIENT_PROTOCOL_41`           New 4.1 protocol
0x0400 CLIENT_INTERACTIVE              This is an interactive client
0x0800 _`CLIENT_SSL`                   Switch to SSL after handshake
0x1000 CLIENT_IGNORE_SIGPIPE           IGNORE sigpipes
0x2000 CLIENT_TRANSACTIONS             Client knows about transactions
0x4000 CLIENT_RESERVED                 Old flag for 4.1 protocol 
0x8000 _`CLIENT_SECURE_CONNECTION`     New 4.1 authentication
====== ==============================  ==================================



Auth Response Packet
--------------------

The client answers the `Auth Challenge Packet`_ with:

* its capability flags
* its password hashed with challenge

If the capabilities have a `CLIENT_PROTOCOL_41`_ flag set the response packet is::

  Auth Response Packet 4.1+
    payload:
      4              capability flags
      4              max-packet size
      1              character set
      string[23]     reserved
      string         username
        if capabilities & CLIENT_SECURE_CONNECTION:
      lenenc-str     auth-response
        else:
      string         auth-response
        all:
      string         database
        if capabilities & CLIENT_PLUGIN_AUTH:
      string         plugin name

If not, it is::

  Auth Response Packet pre-4.1
    payload:
      2              capability flags
      3              max-packet size
      string         username
      string         auth-response

If `CLIENT_SSL`_ is set the `Auth Response Packet`_ is looking slightly differently, see `SSL`_.

`capability flags` are the same as defined in the `Capability flags`_ of the `Auth Challenge Packet`_ plus:

========== ==============================  ==================================
flags      constant name                   description
========== ==============================  ==================================
0x00010000 _`CLIENT_MULTI_STATEMENTS`      Enable/disable multi-stmt support
0x00020000 _`CLIENT_MULTI_RESULTS`         Enable/disable multi-results
0x00040000 _`CLIENT_PS_MULTI_RESULTS`      Multi-results in PS-protocol
0x00080000 _`CLIENT_PLUGIN_AUTH`           Client supports plugin auth
0x40000000 CLIENT_SSL_VERIFY_SERVER_CERT
0x80000000 CLIENT_REMEMBER_OPTIONS
========== ==============================  ==================================

`character set` is the connection's default character set and is defined in `Character Set`_.

The `auth-response` depends on the `Auth Method`_ that is used.


Auth Method Switch Request Packet
---------------------------------

If the server can not or doesn't want to authenticate the client based on the current `Auth Method`_ it can 
ask to client to switch to another one.

If `CLIENT_PLUGIN_AUTH`_ is set in the clients `Auth Response Packet`_ capabilites the server may send a plugin name
and additional auth data which is plugin specific.

If `CLIENT_PLUGIN_AUTH`_ was *not* set the `plugin name` is assumed to be the one of the `Old Auth`_ method.

::

  Auth Method Switch Request Packet
    ask the client to switch to another auth method

    payload:
      1              [fe]
        if capabilities & CLIENT_PLUGIN_AUTH:
      string         plugin name
      string[p]      plugin data              

    example:
      01 00 00 02 fe 

Auth Method Switch Response Packet
----------------------------------

::

  Auth Method Switch Response Packet
    the response of the auth plugin

    payload:
      string         auth plugin response

    example:
      09 00 00 03 5c 49 4d 5e    4e 58 4f 47 00             ....\IM^NXOG.
  
The data here is completely dependent on the `plugin name` of the `Auth Method Switch Request Packet`_.

Auth Method
-----------

Depending on the capability flags the client and server support different authentication methods.

* `Old Auth`_
* `Secure Auth`_
* methods provided by auth plugins as defined in `WL1054`_

.. _`WL1054`: http://forge.mysql.com/worklog/task.php?id=1054

It is negotiated as part of the auth handshake. The server announces its supported auth methods as
part of its capabilties in the `Auth Challenge Packet`_ and the client responds with its set in `Auth Response Packet`_.

===================== =========================== ===================== =================
`CLIENT_PROTOCOL_41`_ `CLIENT_SECURE_CONNECTION`_ `CLIENT_PLUGIN_AUTH`_ `Auth Method`_
===================== =========================== ===================== =================
no                    ignored                     ignored               `Old Auth`_
yes                   no                          no                    `Old Auth`_
yes                   yes                         no                    `Secure Auth`_
yes                   yes                         yes                   see `plugin name`
===================== =========================== ===================== =================

Old Auth
........

+--------------------+
| plugin name        |
+--------------------+
| mysql_old_password |
+--------------------+

The server sends a 8 or 20-byte "challenge" as part of the `Auth Challenge Packet`_ and the client returns 
its password scrambled using the first 8 byte of the "challenge".

.. warning:: The hashing algorithm used for this auth method is *broken* as shown at http://sqlhack.com/ and `CVE-2000-0981`_

.. _`CVE-2000-0981`:  http://cve.mitre.org/cgi-bin/cvename.cgi?name=CVE-2000-0981

For illustration purposes this code snippet from Connector/J's ``/src/com/mysql/jdbc/Util.java``:

.. code-block:: java

  static String newCrypt(String password, String seed) {
          byte b;
          double d;
          
          if ((password == null) || (password.length() == 0)) {
                  return password;
          }
          
          long[] pw = newHash(seed);
          long[] msg = newHash(password);
          long max = 0x3fffffffL;
          long seed1 = (pw[0] ^ msg[0]) % max;
          long seed2 = (pw[1] ^ msg[1]) % max;
          char[] chars = new char[seed.length()];
          
          for (int i = 0; i < seed.length(); i++) {
                  seed1 = ((seed1 * 3) + seed2) % max;
                  seed2 = (seed1 + seed2 + 33) % max;
                  d = (double) seed1 / (double) max;
                  b = (byte) java.lang.Math.floor((d * 31) + 64);
                  chars[i] = (char) b;
          }
          
          seed1 = ((seed1 * 3) + seed2) % max;
          seed2 = (seed1 + seed2 + 33) % max;
          d = (double) seed1 / (double) max;
          b = (byte) java.lang.Math.floor(d * 31);
          
          for (int i = 0; i < seed.length(); i++) {
                  chars[i] ^= (char) b;
          }
          
          return new String(chars);
  }
  
  static long[] newHash(String password) {
          long nr = 1345345333L;
          long add = 7;
          long nr2 = 0x12345671L;
          long tmp;
  
          for (int i = 0; i < password.length(); ++i) {
                  if ((password.charAt(i) == ' ') || (password.charAt(i) == '\t')) {
                          continue; // skip spaces
                  }
  
                  tmp = (0xff & password.charAt(i));
                  nr ^= ((((nr & 63) + add) * tmp) + (nr << 8));
                  nr2 += ((nr2 << 8) ^ nr);
                  add += tmp;
          }
  
          long[] result = new long[2];
          result[0] = nr & 0x7fffffffL;
          result[1] = nr2 & 0x7fffffffL;
  
          return result;
  }

Secure Auth
...........

+-----------------------+
| plugin name           |
+-----------------------+
| mysql_native_password |
+-----------------------+

The secure authentication was introduced in MySQL Server 4.1.1 and the server announces it by setting `CLIENT_SECURE_CONNECTION`_ capability flag.

This method fixes a 2 short-comings of the `Old Auth`_:

* using a tested, crypto-graphic hashing function which isn't broken
* knowning the content of the hash in the ``mysql.user`` table isn't enough to authenticate against the MySQL Server.

The server sends a 20-byte "challenge" as part of the `Auth Challenge Packet`_ and the client returns::

  SHA1( password ) XOR SHA1( challenge + SHA1( SHA1( password ) ) )

Command Phase
=============

In the command phase the client sends a command packet with the sequence-id [00]::

   13 00 00 00 03 53 ... 
   01 00 00 00 01 
               ^^- command-byte
            ^^---- sequence-id == 0

The first byte of the payload describes the command-type like:

===  ======================
hex  constant name 
===  ======================
00   `COM_SLEEP`_
01   `COM_QUIT`_
02   `COM_INIT_DB`_
03   `COM_QUERY`_
04   `COM_FIELD_LIST`_
05   `COM_CREATE_DB`_
06   `COM_DROP_DB`_
07   `COM_REFRESH`_
08   `COM_SHUTDOWN`_
09   `COM_STATISTICS`_
0a   `COM_PROCESS_INFO`_
0b   `COM_CONNECT`_
0c   `COM_PROCESS_KILL`_
0d   `COM_DEBUG`_
0e   `COM_PING`_
0f   `COM_TIME`_
10   `COM_DELAYED_INSERT`_
11   `COM_CHANGE_USER`_
12   `COM_BINLOG_DUMP`_
13   `COM_TABLE_DUMP`_
14   `COM_CONNECT_OUT`_
15   `COM_REGISTER_SLAVE`_
16   `COM_STMT_PREPARE`_
17   `COM_STMT_EXECUTE`_
18   `COM_STMT_SEND_LONG_DATA`_
19   `COM_STMT_CLOSE`_
1a   `COM_STMT_RESET`_
1b   `COM_SET_OPTION`_
1c   `COM_STMT_FETCH`_
1d   `COM_DAEMON`_
===  ======================

.. _COM_SLEEP: `unhandled commands`_
.. _COM_CONNECT: `unhandled commands`_
.. _COM_TIME: `unhandled commands`_
.. _COM_DELAYED_INSERT: `unhandled commands`_
.. _COM_CONNECT_OUT: `unhandled commands`_
.. _COM_TABLE_DUMP: `unhandled commands`_
.. _COM_DAEMON: `unhandled commands`_

The commands belong to 

* the `Old Commands`_
* the `Prepared Statements`_ Commands
* the `Stored Procedures`_ Commands
* or the Replication Commands

Old Commands
============

The old commands are supported for all MySQL Server versions from 3.23 upwards (and perhaps older).

unhandled commands
------------------

* COM_SLEEP
* COM_CONNECT
* COM_TIME
* COM_DELAYED_INSERT
* COM_DAEMON

These commands are only used internally by the server or are deprecated. Sending the to the server always results in a 
`ERR packet`_.

.. _protocol-com-quit:

COM_QUIT
--------

::

  COM_QUIT
    tells the server that the client wants to close the connection

    direction: client -> server
    response: either a connection close or a OK packet
    
    payload:
      1              [01] COM_QUIT

    Example:
      01 00 00 00 01

.. _protocol-com-init-db:

COM_INIT_DB
-----------

::

  COM_INIT_DB
    change the default schema of the connection

    direction: client -> server
    response: OK or ERR

    payload:
      1              [02] COM_INIT_DB
      string[p]      schema name

    example:
      05 00 00 00 02 74 65 73    74                         .....test     

.. _protocol-com-query:

COM_QUERY
---------

A COM_QUERY is used to send the server a text-based query that is executed immediately.

The server replies to a COM_QUERY packet with a `COM_QUERY Response`_.

::

  COM_QUERY
    tells the server to execute a text-based query

    direction: client -> server
    
    payload:
      1              [03] COM_QUERY
      string[p]      the query the server shall execute

    Example:
      21 00 00 00 03 73 65 6c    65 63 74 20 40 40 76 65    !....select @@ve
      72 73 69 6f 6e 5f 63 6f    6d 6d 65 6e 74 20 6c 69    rsion_comment li
      6d 69 74 20 31                                        mit 1           

The length of the query-string is a taken from the packet length - 1.

API call: `mysql_query() <http://dev.mysql.com/doc/refman/5.1/en/mysql-query.html>`_


COM_QUERY Response
..................

The query-response packet is a meta packet which can be one of

* a `ERR packet`_
* a `OK packet`_
* a `LOCAL INFILE request`_
* a `Text Resultset`_

The type of the packet is defined by the type-identifier::

  COM_QUERY response
    response to a COM_QUERY packet

    payload
      lenenc-int     number of columns in the resultset

If the number of columns in the resultset is 0, this is a `OK packet`_.

If it is not a valid `length encoded integer`_ it is a either a `ERR packet`_ or a `LOCAL INFILE request`_. 

Text Resultset
**************

A Text Resultset is a possible `COM_QUERY Response`_. 

It is made up of a two parts:

* the column definition
* the rows

which consists of a sequence of packets.

The column defintion is starts with a packet containing the column-count and is
followed by as many `Column Definition`_ packets as we have columns and is terminated
by a `EOF packet`.

Each row is a packet too. The rows are terminated by another `EOF packet`_. In case 
the query could generate the column-definition, but generating the rows afterwards
failed a `ERR packet`_ may be sent instead of the last `EOF packet`_.

* a packet containing a `length encoded integer`_ column-count
* column-count * `Column Definition`_ packets
* `EOF packet`_
* row-count * packets as in `Text Resultset Row`_ format
* `EOF packet`_

A query like ``SELECT @@version_comment`` returns::
  
  01 00 00 01 01|27 00 00    02 03 64 65 66 00 00 00    .....'....def...
  11 40 40 76 65 72 73 69    6f 6e 5f 63 6f 6d 6d 65    .@@version_comme
  6e 74 00 0c 08 00 1c 00    00 00 fd 00 00 1f 00 00|   nt..............
  05 00 00 03 fe 00 00 02    00|1d 00 00 04 1c 4d 79    ..............My
  53 51 4c 20 43 6f 6d 6d    75 6e 69 74 79 20 53 65    SQL Community Se
  72 76 65 72 20 28 47 50    4c 29|05 00 00 05 fe 00    rver (GPL)......
  00 02 00                                              ...             

+--------------+--------+--------------------------------------------------------------------------------------------------------------------------+
| packet-len   | seq-id |                                                                                                                          |
+--------------+--------+--------------------------------------------------------------------------------------------------------------------------+
| ``01 00 00`` | ``01`` | ``01`` field count                                                                                                       |
+--------------+--------+--------------------------------------------------------------------------------------------------------------------------+
| ``27 00 00`` | ``02`` | `Text Resultset`_                                                                                                        |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``03 64 65 66``                                           | catalog = `"def"`                                            |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``00``                                                    | schema = `""`                                                |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``00``                                                    | table = `""`                                                 |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``00``                                                    | org_table = `""`                                             |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``11 40 40 76 65 72 73 69 6f 6e 5f 63 6f 6d 6d 65 6e 74`` | name `"@@version_comment"`                                   |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``00``                                                    | org_name = `""`                                              |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``0c``                                                    | filler                                                       |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``08 00``                                                 | `character set`_ = ``latin1_swedish_ci``                     |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``1c 00 00 00``                                           | column length                                                |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``fd``                                                    | `column type`_ = ``MYSQL_TYPE_VAR_STRING``                   |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``00 00``                                                 | flags                                                        |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``1f``                                                    | decimals                                                     |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``00 00``                                                 | filler                                                       |
+--------------+--------+-----------------------------------------------------------+--------------------------------------------------------------+
| ``05 00 00`` | ``03`` | `EOF packet`_                                                                                                            |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``fe``                                                    | EOF indicator                                                |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``00 00``                                                 | warning count                                                |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``02 00``                                                 | `status flags`_ = ``SERVER_STATUS_AUTOCOMMIT``               |
+--------------+--------+-----------------------------------------------------------+--------------------------------------------------------------+
| ``05 00 00`` | ``04`` | ``1c 4d 79 53 51 4c 20 43 6f 6d 6d 75 6e 69 74 79 20 53 65 72 76 65 72 20 28 47 50 4c 29``                               |
+--------------+--------+-----------------------------------------------------------+--------------------------------------------------------------+
| ``05 00 00`` | ``05`` | `EOF packet`_                                                                                                            |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``fe``                                                    | EOF indicator                                                |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``00 00``                                                 | warning count                                                |
|              |        +-----------------------------------------------------------+--------------------------------------------------------------+
|              |        | ``02 00``                                                 | `status flags`_ = ``SERVER_STATUS_AUTOCOMMIT``               |
+--------------+--------+-----------------------------------------------------------+--------------------------------------------------------------+

If the `SERVER_MORE_RESULTS_EXISTS`_ flag is set in the last `EOF packet`_ a `multi-resultset`_ is sent.

It may also be resultset with an closing `ERR packet`_:

* a packet containing a `length encoded integer`_ column-count
* column-count * `Column Definition`_ packets
* `EOF packet`_
* `ERR packet`_

which is generated for queries like `EXPLAIN SELECT * FROM dual`.

.. _`protocol-column-type`:

Column Type
,,,,,,,,,,,

===  ======================
hex  constant name 
===  ======================
00   _`MYSQL_TYPE_DECIMAL`
01   _`MYSQL_TYPE_TINY`
02   _`MYSQL_TYPE_SHORT`
03   _`MYSQL_TYPE_LONG`
04   _`MYSQL_TYPE_FLOAT`
05   _`MYSQL_TYPE_DOUBLE`
06   _`MYSQL_TYPE_NULL`
07   _`MYSQL_TYPE_TIMESTAMP`
08   _`MYSQL_TYPE_LONGLONG`
09   _`MYSQL_TYPE_INT24`
0a   _`MYSQL_TYPE_DATE`
0b   _`MYSQL_TYPE_TIME`
0c   _`MYSQL_TYPE_DATETIME`
0d   _`MYSQL_TYPE_YEAR`
0e   _`MYSQL_TYPE_NEWDATE`
0f   _`MYSQL_TYPE_VARCHAR`
10   _`MYSQL_TYPE_BIT`
f6   _`MYSQL_TYPE_NEWDECIMAL`
f7   _`MYSQL_TYPE_ENUM`
f8   _`MYSQL_TYPE_SET`
f9   _`MYSQL_TYPE_TINY_BLOB`
fa   _`MYSQL_TYPE_MEDIUM_BLOB`
fb   _`MYSQL_TYPE_LONG_BLOB`
fc   _`MYSQL_TYPE_BLOB`
fd   _`MYSQL_TYPE_VAR_STRING`
fe   _`MYSQL_TYPE_STRING`
ff   _`MYSQL_TYPE_GEOMETRY`
===  ======================

Column Definition
,,,,,,,,,,,,,,,,,

If the PROTOCOL_41 capability is set::

  Column Definition - 4.1+

    payload:
      lenenc-str     catalog
      lenenc-str     schema
      lenenc-str     table
      lenenc-str     org_table
      lenenc-str     name
      lenenc-str     org_name
      1              filler [00]
      2              character set
      4              column length
      1              type
      2              flags
      1              decimals
      2              filler [00] [00]

character set
  is the column character set and is defined in `Character Set`_.

column type
  type of the column as defined in `Column Type`_

If not ::

  Column Definition - pre-4.1

    payload:
      lenenc-str     table
      lenenc-str     name
      1              [03]
      3              column length
      1              [01]
      1              type
      1              [02] or [03]
        if above field == 02:
      1              flags
        if ... == 03:
      2              flags
        all:
      1              decimals

Text Resultset Row
,,,,,,,,,,,,,,,,,,

A row with the data for each column. 

* Integers are sent as `length encoded integer`_.
* everything else sent as `length encoded string`_. 

If a field is NULL `0xfb` is sent as described in `length encoded integer`_.

EOF packet
,,,,,,,,,,

If `CLIENT_PROTOCOL_41`_ is enabled the EOF packet will contain a warning count and status flags.

::

  EOF
  
    direction: server -> client

    payload:
      1              [fe] the EOF header
        if capabilities & CLIENT_PROTOCOL_41:
      2              warning count
      2              status flags

    example:
      05 00 00 05 fe 00 00 02 00
 
The status flags are a bit-field as defined in the `Status Flags`_ of the `OK packet`_.

LOCAL INFILE request
********************

If the client wants to LOAD DATA from a LOCAL file into the server it sends::

  LOAD DATA LOCAL INFILE '<filename>' INTO TABLE <table>;  

The LOCAL keyword triggers the server to send a LOAD INFILE packet asks the client
to send the file via a `LOCAL INFILE data`_ response.

The client has to set the `CLIENT_LOCAL_FILES`_ capability.

::

  LOCAL INFILE packet

    direction: server -> client
    response: LOCAL INFILE data

    payload:
      1              [fb] LOCAL INFILE 
      string[p]      filename the client shall send

    example:
      0c 00 00 01 fb 2f 65 74    63 2f 70 61 73 73 77 64    ...../etc/passwd

LOCAL INFILE data
,,,,,,,,,,,,,,,,,

The client sends its file data AS IS to the server in response to a `LOCAL INFILE request`_.

::

   LOAD INFILE data

     direction: client data

     payload:
       n             the filedata

COM_FIELD_LIST
--------------

::

  COM_FIELD_LIST
    get the column definition of a tables

    direction: client -> server
    response: 

    payload:
      1              [04] COM_FIELD_LIST
      string         table
      string[p]      field wildcard

API call: `mysql_list_fields() <http://dev.mysql.com/doc/refman/5.1/en/mysql-list-fields.html>`_

COM_FIELD_LIST response
.......................

The response to a `COM_FIELD_LIST`_ can either be a

* a `ERR packet`_ or the
* first half of a `Text Resultset`_

  * a packet containing a `length encoded integer`_ column-count
  * column-count * `Column Definition`_ packets
  * `EOF packet`_

COM_CREATE_DB
-------------

::

  COM_CREATE_DB
    create a schema

    direction: client -> server
    response: OK or ERR

    payload:
      1              [05] COM_CREATE_DB
      string[p]      schema name

    example:
      05 00 00 00 05 74 65 73    74                         .....test     

COM_DROP_DB
-----------

::

  COM_DROP_DB
    drop a schema

    direction: client -> server
    response: OK or ERR

    payload:
      1              [06] COM_DROP_DB
      string[p]      schema name

    example:
      05 00 00 00 06 74 65 73    74                         .....test     

COM_REFRESH
-----------

a low-level version of several `FLUSH ...` and `RESET ...` commands.

====  ===============  ===========
flag  constant name    description
====  ===============  ===========
0x01  REFRESH_GRANT    Refresh grant tables `FLUSH PRIVILEGES`
0x02  REFRESH_LOG      Start on new log file `FLUSH LOGS`
0x04  REFRESH_TABLES   close all tables `FLUSH TABLES`
0x08  REFRESH_HOSTS    Flush host cache `FLUSH HOSTS`
0x10  REFRESH_STATUS   Flush status variables `FLUSH STATUS`
0x20  REFRESH_THREADS  Flush thread cache
0x40  REFRESH_SLAVE    Reset master info and restart slave thread `RESET SLAVE`
0x80  REFRESH_MASTER   Remove all bin logs in the index and truncate the index `RESET MASTER`
====  ===============  ===========

::

  COM_REFRESH
    get a list of active threads

    direction: client -> server
    response: OK or ERR

    payload:
      1              [07] COM_REFRESH
      1              flags

COM_SHUTDOWN
------------

COM_SHUTDOWN is used to shutdown the mysql-server. 

Even if several shutdown types are define, right now only one is use: SHUTDOWN_WAIT_ALL_BUFFERS

==== ============================== ===========
type constant name                  description
==== ============================== ===========
0x00 SHUTDOWN_DEFAULT               defaults to SHUTDOWN_WAIT_ALL_BUFFERS
0x01 SHUTDOWN_WAIT_CONNECTIONS      wait for existing connections to finish
0x02 SHUTDOWN_WAIT_TRANSACTIONS     wait for existing trans to finish
0x08 SHUTDOWN_WAIT_UPDATES          wait for existing updates to finish (=> no partial MyISAM update)
0x10 SHUTDOWN_WAIT_ALL_BUFFERS      flush InnoDB buffers and other storage engines' buffers
0x11 SHUTDOWN_WAIT_CRITICAL_BUFFERS don't flush InnoDB buffers, flush other storage engines' buffers
0xfe KILL_QUERY
0xff KILL_CONNECTION
==== ============================== ===========

`SHUTDOWN` privilege is required.

::

  COM_SHUTDOWN
    get a list of active threads

    direction: client -> server
    response: EOF or ERR

    payload:
      1              [08] COM_SHUTDOWN
        if shutdown type != 0x00:
      1              shutdown type

Clients before 4.1.3 don't send the `shutdown type`. `0x00` is assumed in that case.

COM_STATISTICS
--------------

Get a human readable string of internal statistics.

::

  COM_STATISTICS
    get a list of active threads

    direction: client -> server
    response: string[p]

    payload:
      1              [09] COM_STATISTICS


COM_PROCESS_INFO
----------------

The COM_PROCESS_INFO command is deprecated. `SHOW PROCESSLIST` should be used instead.

It either returns a:

* `Text Resultset`_ or
* `ERR packet`_.

::

  COM_PROCESS_INFO
    get a list of active threads

    direction: client -> server
    response: resultset or ERR

    payload:
      1              [0a] COM_PROCCESS_INFO

COM_PROCESS_KILL
----------------

Same as `KILL <id>`.

::

  COM_PROCESS_KILL
    ask the server to terminate a connection

    direction: client -> server
    response: OK or ERR

    payload:
      1              [0c] COM_PROCCESS_KILL
      4              connection id


COM_DEBUG
---------

COM_DEBUG triggers a dump on internal debug info to stdout of the mysql-server. 

The `SUPER` privilege is required for this operation.

::

  COM_DEBUG
    dump debug info to stdout

    direction: client -> server
    response: EOF or ERR

    payload:
      1              [0d] COM_DEBUG

.. _protocol-com-ping:

COM_PING
--------

::

  COM_PING
    check if the server is alive

    direction: client -> server
    response: OK

    payload:
      1              [0e] COM_PING


.. _protocol-com-change-user:

COM_CHANGE_USER
---------------

COM_CHANGE_USER changes the user of the current connection and reset the connection state.

* user variables
* temp tables
* prepared statemants
* ... and others

::

  COM_CHANGE_USER
    change the user of the current connection

    direction: client -> server
    response: EOF or ERR

    payload:
      1              [11] COM_CHANGE_USER
      string         user
        if capabilities & SECURE_CONNECTION:
      lenenc-str     auth-response
        else:
      string         auth-response
        all:
      string         schema-name
        if more bytes in packet:
      2              character-set (since 5.1.23?)
        if more bytes in packet:
      string         auth plugin name (since WL1054 and if CLIENT_PLUGIN_AUTH is used)

`character set` is the connection character set and is defined in `Character Set`_.

Prepared Statements
===================

The prepared statement protocol was introduced in MySQL 4.1 and adds a few new commands:

* `COM_STMT_PREPARE`_
* `COM_STMT_EXECUTE`_
* `COM_STMT_CLOSE`_
* `COM_STMT_RESET`_
* `COM_STMT_SEND_LONG_DATA`_

It also defines a more compact resultset format that is used instead of the `Text Resultset`_ to
return resultsets.

Keep in mind that not all statements can be prepared:

  http://forge.mysql.com/worklog/task.php?id=2871

Binary Protocol Resultset
-------------------------

Binary Protocol Resultset is similar the `Text Resultset`_. It just contains the rows in 
`Binary Protocol Resultset Row`_ format.

* lenenc column-count
* column-count * `Column Definition`_
* `EOF packet`_
* n * rows as in `Binary Protocol Resultset Row`_
* `EOF packet`_

Example::

    01 00 00 01 01|1a 00 00    02 03 64 65 66 00 00 00    ..........def...
    04 63 6f 6c 31 00 0c 08    00 06 00 00 00 fd 00 00    .col1...........
    1f 00 00|05 00 00 03 fe    00 00 02 00|09 00 00 04    ................
    00 00 06 66 6f 6f 62 61    72|05 00 00 05 fe 00 00    ...foobar.......
    02 00                                                 ..     

Binary Protocol Resultset Row
-----------------------------

A Binary Protocol Resultset Row is made up of the ``NULL bitmap`` containing as many bits as we have columns in the
resultset + 2 and the ``values`` for columns that are not NULL in the `Binary Protocol Value`_ format.

::

  Binary Protocol Resultset Row
    row of a binary resultset (COM_STMT_EXECUTE)

    payload:
      1              packet header [00]
      n              NULL-bitmap, length: (column-count + 7 + 2) / 8
      n              values

    example:
      09 00 00 04 00 00 06 66 6f 6f 62 61 72

NULL-bitmap
...........

The binary protocol sends NULL values as bits inside a bitmap instead of a full byte as the `Text Resultset Row`_. If many NULL values 
are sent, it is more efficient than the old way.

.. attention::
  For the `Binary Protocol Resultset Row`_ the ``num-fields`` and the ``field-pos`` need to add a offset of 2. For `COM_STMT_EXECUTE`_ this 
  offset is 0.
  
The NULL-bitmap needs enough space to store a possible NULL bit for each column that is sent. Its space is
calculated with::

  NULL-bitmap-bytes = (num-fields + 7 + offset) / 8

resulting in:

================= =================
num-fields+offset NULL-bitmap-bytes
================= =================
0                 0
1                 1
[...]             [...]
8                 1
9                 2
[...]             [...]
================= =================

To store a NULL bit in the bitmap, you need to calculate the bitmap-byte (starting with 0) and the bitpos (starting with 0) in that byte from the field-index (starting with 0)::

  NULL-bitmap-byte = ((field-pos + offset) / 8)
  NULL-bitmap-bit  = ((field-pos + offset) % 8)  

Example::

  Resultset Row, 9 fields, 9th field is a NULL (9th field -> field-index == 8, offset == 2)
   
  nulls -> [00] [00]
  
  byte_pos = (10 / 8) = 1
  bit_pos  = (10 % 8) = 2
 
  nulls[byte_pos] |= 1 << bit_pos
  nulls[1] |= 1 << 2;
  
  nulls -> [00] [04]

Binary Protocol Value 
----------------------

* Strings like `MYSQL_TYPE_STRING`_ `MYSQL_TYPE_BLOB`_ and `MYSQL_TYPE_DECIMAL`_::

    lenenc-str       string

    example:
      03 66 6f 6f -- string = "foo"

* `MYSQL_TYPE_LONGLONG`_::

    8                integer least significant byte first

    example:
      01 00 00 00 00 00 00 00 -- int64 = 1

* `MYSQL_TYPE_LONG`_ and `MYSQL_TYPE_INT24`_::

    4                integer least significant byte first

    example:
      01 00 00 00 -- int32 = 1

* `MYSQL_TYPE_SHORT`_::

    2                integer least significant byte first

    example:
      01 00 -- int16 = 1

* `MYSQL_TYPE_TINY`_::

    1                integer

    example:
      01 -- int8 = 1

* `MYSQL_TYPE_DOUBLE`_::

    8                double

    example:
      66 66 66 66 66 66 24 40 -- double = 10.2

* `MYSQL_TYPE_FLOAT`_::

    4                float

    example:
      33 33 23 41 -- float = 10.2

* `MYSQL_TYPE_DATE`_::

    1               [04] length of the encoded value
    2               year
    1               month
    1               day
  
    example: 
      04 da 07 0a 11 -- date = 2010-10-17

* `MYSQL_TYPE_DATETIME`_::

    1               [0b] length of the encoded value
    2               year
    1               month
    1               day
    1               hour
    1               minutes
    1               seconds
    4               nseconds
  
    example: 
      0b da 07 0a 11 13 1b 1e 01 00 00 00 -- datetime 2010-10-17 19:27:30.000 000 001

* `MYSQL_TYPE_TIME`_::

    1               [0c] length of the encoded value
    1               sign (1 if minus, 0 for plus)
    4               days
    1               hour
    1               minutes
    1               seconds
    4               nseconds
  
    example: 
      0c 01 78 00 00 00 13 1b 1e 01 00 00 00 -- time  -120d 19:27:30.000 000 001

* `MYSQL_TYPE_TIMESTAMP`_::

     1               [0b] length of the encoded value
     2               year
     1               month
     1               day
     1               hour
     1               minutes
     1               seconds
     4               nseconds

     example: 
       0b da 07 0a 11 13 1b 1e 01 00 00 00 -- timestamp 
  


COM_STMT_PREPARE
----------------

COM_STMT_PREPARE creates a prepared statement from the passed query string.

The server returns a `COM_STMT_PREPARE Response`_ which contains a statement-id which is
used to identify the prepared statement.

::

  COM_STMT_PREPARE
    create a prepared statement 

    direction: client -> server
    response: COM_STMT_PREPARE response

    payload:
      1              [16] the COM_STMT_PREPARE command
      string[p]      the query to prepare

    example:
      1c 00 00 00 16 53 45 4c    45 43 54 20 43 4f 4e 43    .....SELECT CONC
      41 54 28 3f 2c 20 3f 29    20 41 53 20 63 6f 6c 31    AT(?, ?) AS col1


COM_STMT_PREPARE response
.........................

If the `COM_STMT_PREPARE`_ succeeded, it sends:

* `COM_STMT_PREPARE OK packet`_
* if num-params > 0

  * num-params * `Column Definition`_
  * `EOF packet`_

* if num-columns > 0

  * num-colums * `Column Definition`_
  * `EOF packet`_

Example::
  
   0c 00 00 01 00 01 00 00    00 01 00 02 00 00 00 00|   ................
   17 00 00 02 03 64 65 66    00 00 00 01 3f 00 0c 3f    .....def....?..?
   00 00 00 00 00 fd 80 00    00 00 00|17 00 00 03 03    ................
   64 65 66 00 00 00 01 3f    00 0c 3f 00 00 00 00 00    def....?..?.....
   fd 80 00 00 00 00|05 00    00 04 fe 00 00 02 00|1a    ................
   00 00 05 03 64 65 66 00    00 00 04 63 6f 6c 31 00    ....def....col1.
   0c 3f 00 00 00 00 00 fd    80 00 1f 00 00|05 00 00    .?..............
   06 fe 00 00 02 00                                     ...... 
  
for a query without parameters and resultset like "DO 1" it is::
  
   0c 00 00 01 00 01 00 00    00 00 00 00 00 00 00 00

If it failed, a `ERR packet`_ is sent.

As LOAD DATA isn't supported by `COM_STMT_PREPARE`_ yet, no is `LOCAL INFILE request`_ expected here.
Compare this to `COM_QUERY response`_.

.. _com_stmt_prepare_ok_packet:

COM_STMT_PREPARE OK packet
**************************

The `COM_STMT_PREPARE response`_ starts a packet which contains the meta-information for the following packets::

  COM_STMT_PREPARE OK
    OK response to a COM_STMT_PREPARE packet 

    direction: server -> client

    payload:
      1              [00] OK
      4              statement-id
      2              num-columns
      2              num-params
      1              [00] filler
      2              warning count


COM_STMT_EXECUTE
----------------

COM_STMT_EXECUTE asks the server to execute a prepared statement as identified by `stmt-id`.

It sends the values for the placeholders of the prepared statement (if it contained any) in
`Binary Protocol Value`_ form. The type of each parameter is made up of two bytes:

* the type as in `Column Type`_
* a flag byte which has the highest bit set if the type is unsigned [80]

The `num-params` used for this packet has to match the `num-params` of the `COM_STMT_PREPARE OK packet`_
of the corresponding prepared statement.

The server returns a `COM_STMT_EXECUTE Response`_.

::
 
  COM_STMT_EXECUTE
    execute a prepared statement

    direction: client -> server
    response: COM_STMT_EXECUTE Response

    payload:
      1              [17] COM_STMT_EXECUTE
      4              stmt-id
      1              flags
      4              iteration-count
        if num-params > 0:
      n              NULL-bitmap, length: (num-params+7)/8
      1              new-params-bound-flag
        if new-params-bound-flag == 1:
      n              type of each parameter, length: num-params * 2
      n              value of each parameter
 
    example: 
      12 00 00 00 17 01 00 00    00 00 01 00 00 00 00 01    ................
      0f 00 03 66 6f 6f                                     ...foo

The `iteration-count` is always `1`.

The `flags` are:

===== =============
flags constant name
===== =============
0x00  CURSOR_TYPE_NO_CURSOR
0x01  CURSOR_TYPE_READ_ONLY
0x02  CURSOR_TYPE_FOR_UPDATE
0x04  CURSOR_TYPE_SCROLLABLE
===== =============

``NULL-bitmap`` is like `NULL-bitmap`_ for the `Binary Protocol Resultset Row`_ just that it has a bit-offset of 0.

COM_STMT_EXECUTE Response
.........................

Similar to the `COM_QUERY Response`_ a `COM_STMT_EXECUTE`_ either returns:

* a `OK packet`_
* a `ERR packet`_
* or a resultset: `Binary Protocol Resultset`_

COM_STMT_SEND_LONG_DATA
-----------------------

COM_STMT_SEND_LONG_DATA sends the data for a column. Repeating to send it, appends the data to the parameter.

No response is sent back to the client.

::

  COM_STMT_SEND_LONG_DATA
    direction: client -> server
    response: none

    payload:
      1              [18] COM_STMT_SEND_LONG_DATA
      4              statement-id
      2              param-id
      n              data
 

COM_STMT_CLOSE
--------------

a COM_STMT_CLOSE deallocates a prepared statement

No response is sent back to the client.

::

  COM_STMT_CLOSE
    direction: client -> server
    response: none

    payload:
      1              [19] COM_STMT_CLOSE
      4              statement-id
 
    example: 
      05 00 00 00 19 01 00 00    00                         ......... 

  
COM_STMT_RESET
--------------

a COM_STMT_RESET resets the data of a prepared statement. Useful in together with `COM_STMT_SEND_LONG_DATA`_.

The server will send a `OK packet`_ if the statement could be reset, a `ERR packet`_ if not.

::

  COM_STMT_RESET
    direction: client -> server
    response: OK or ERR

    payload:
      1              [1a] COM_STMT_RESET
      4              statement-id
 
    example: 
      05 00 00 00 1a 01 00 00    00                         ......... 

  
Stored Procedures
=================

In MySQL 5.0 the protocol was extended to handle:

* `multi-resultset`_
* `multi-statement`_

Multi-resultset
---------------

Multi-resultsets are sent up stored procedures if more than one resultset was generated inside of it::

  CREATE TEMPORARY TABLE ins ( id INT );
  DROP PROCEDURE IF EXISTS multi;
  DELIMITER $$
  CREATE PROCEDURE multi() BEGIN
    SELECT 1;
    SELECT 1;
    INSERT INTO ins VALUES (1);
    INSERT INTO ins VALUES (2);
  END$$
  DELIMITER ;

  CALL multi();
  DROP TABLE ins; 

results in:

* a resultset::

    01 00 00 01 01 17 00 00    02 03 64 65 66 00 00 00    ..........def...
    01 31 00 0c 3f 00 01 00    00 00 08 81 00 00 00 00    .1..?...........
    05 00 00 03 fe 00 00 0a    00 02 00 00 04 01 31 05    ..............1.
    00 00 05 fe 00 00 0a 00                               ........        

  * see the `EOF packet`_: `05 00 00 03 fe 00 00 0a 00` with its status-flag being `0a`

* the 2nd resultset::

    01 00 00 06 01 17 00 00    07 03 64 65 66 00 00 00    ..........def...
    01 31 00 0c 3f 00 01 00    00 00 08 81 00 00 00 00    .1..?...........
    05 00 00 08 fe 00 00 0a    00 02 00 00 09 01 31 05    ..............1.
    00 00 0a fe 00 00 0a 00                               ........        

  * see the `EOF packet`_: `05 00 00 0a fe 00 00 0a 00` with its status-flag being `0a`

* ... and a closing empty resultset, a `OK packet`_::

    07 00 00 0b 00 01 00 02    00 00 00                   ...........     

`SERVER_MORE_RESULTS_EXISTS`_ is set to indicate that more resultsets will follow.

The trailing `OK packet`_ is the response to the CALL statement and contains the affected rows of
the last statement. In our case we INSERTed 2 rows, but only the `affected_rows` of the 
last INSERT statement is returned as part of the `OK packet`_. If the last statement is a SELECT
the affected rows is 0.

The client has to announce that it wants multi-resultsets by either setting the `CLIENT_MULTI_RESULTS`_ or 
`CLIENT_PS_MULTI_RESULTS`_ capability.

Multi-statement
---------------

A multi-statement is allowing COM_QUERY to send more than one query to the server, separated by a ';'.

The client has to announce that it wants multi-statements by either setting the `CLIENT_MULTI_STATEMENTS`_ capability
or by using `COM_SET_OPTION`_.

COM_SET_OPTION
--------------

Allows to enable and disable:

* `CLIENT_MULTI_STATEMENTS`_

for the current connection. The option operation is one of:

=== =============
op  constant name
=== =============
0   MYSQL_OPTION_MULTI_STATEMENTS_ON
1   MYSQL_OPTION_MULTI_STATEMENTS_OFF
=== =============

On success it returns a `EOF packet`_ otherwise a `ERR packet`_.

::

  COM_SET_OPTION
    set options for the current connection

    response: EOF or ERR

    payload:
      1              [1b] COM_SET_OPTION
      2              option operation

COM_STMT_FETCH
--------------

::

  COM_STMT_FETCH

    response: binary rows or ERR

    payload:
      1              [1c] COM_STMT_FETCH
      4              stmt-id
      4              num rows

COM_STMT_FETCH response
.......................

A fetch may result:

* a `multi-resultset`_
* a `ERR packet`_

Replication
===========

Replication uses binlogs to ship changes done on the master to the slave and can be written to `Binlog File`_ and
sent over the network as `Binlog Network Stream`_.

Binlog File
-----------

Binlog files start with a `Binlog File Header`_ followed by a series of `Binlog Event`_

Binlog File Header
..................

A binlog file starts with a `Binlog File Header` ``[ fe 'bin' ]``::

  $ hexdump -C /tmp/binlog-test.log
  00000000  fe 62 69 6e 19 6f c9 4c  0f 01 00 00 00 66 00 00  |.bin.o.L.....f..|
  00000010  00 6a 00 00 00 00 00 04  00 6d 79 73 71 6c 2d 70  |.j.......mysql-p|
  00000020  72 6f 78 79 2d 30 2e 37  2e 30 00 00 00 00 00 00  |roxy-0.7.0......|
  ...

Binlog Network Stream
---------------------

Network streams are requested with `COM_BINLOG_DUMP`_ and prepend each `Binlog Event`_ with ``00`` OK-byte.

Binlog Version
--------------

Depending on the MySQL Version that created the binlog the format is slightly different. Four versions are currently known:

.. table:: Binlog Versions

  ==== =============
  ver  MySQL Version
  ==== =============
  1    MySQL 3.23  - < 4.0.0
  2    MySQL 4.0.0 - 4.0.1
  3    MySQL 4.0.2 - < 5.0.0
  4    MySQL 5.0.0+
  ==== =============

Version 1
  supported `statement based replication`_

Version 2
  can be ignored as it was only used in early alpha versions of MySQL 4.1.x and won't be documented here

Version 3
  added the relay logs and changed the meaning of the log position

Version 4
  added the `FORMAT_DESCRIPTION_EVENT`_ and made the protocol extensible 

  In MySQL 5.1.x the `Row Based Replication`_ events were added.

Determining the Binlog Version
..............................

By the time you read the first event from the log you don't know what `binlog version`_ the binlog has.
To determine the version correctly it has to be checked if the first event is:

* a `FORMAT_DESCRIPTION_EVENT`_ version = 4
* a `START_EVENT_V3`_ 

  * if `event-size` == 13 + 56: version = 1
  * if `event-size` == 19 + 56: version = 3
  * otherwise: invalid

Binlog Event
------------

The events contain the actual data that should be shipped from the master to the slave. Depending
on the use, different events are sent.

Binlog Management
  The first event is either a `START_EVENT_V3`_ or a `FORMAT_DESCRIPTION_EVENT`_ while the last
  event is either a `STOP_EVENT`_ or a `ROTATE_EVENT`_.

  * `START_EVENT_V3`_
  * `FORMAT_DESCRIPTION_EVENT`_
  * `STOP_EVENT`_
  * `ROTATE_EVENT`_
  * `SLAVE_EVENT`_
  * `INCIDENT_EVENT`_
  * `HEARTBEAT_LOG_EVENT`_

_`Statement Based Replication`
  Statement Based Replication or SBR sends the SQL queries a client sent to the master AS IS to the slave.
  It needs extra events to mimik the client connection's state on the slave side. 

  * `QUERY_EVENT`_
  * `INTVAR_EVENT`_
  * `RAND_EVENT`_
  * `USER_VAR_EVENT`_
  * `XID_EVENT`_

_`Row Based Replication` 
  In Row Based replication the changed rows are sent to the slave which removes side-effects and makes
  it more reliable. Now all statements can be sent with RBR though. Most of the time you will see
  RBR and SBR side by side.

  * `TABLE_MAP_EVENT`_
  * `PRE_GA_DELETE_ROWS_EVENT`_
  * `PRE_GA_UPDATE_ROWS_EVENT`_
  * `PRE_GA_WRITE_ROWS_EVENT`_
  * `DELETE_ROWS_EVENT`_
  * `UPDATE_ROWS_EVENT`_
  * `WRITE_ROWS_EVENT`_

LOAD INFILE replication
  ``LOAD DATA|XML INFILE`` is a special SQL statement as it has to ship the files over to the slave too to execute
  the statement.

  * `LOAD_EVENT`_
  * `CREATE_FILE_EVENT`_
  * `APPEND_BLOCK_EVENT`_
  * `EXEC_LOAD_EVENT`_
  * `DELETE_FILE_EVENT`_
  * `NEW_LOAD_EVENT`_
  * `BEGIN_LOAD_QUERY_EVENT`_
  * `EXECUTE_LOAD_QUERY_EVENT`_

A binlog event starts with a `Binlog Event header`_ and is followed by a `Binlog Event Type`_ specific data part.

Binlog Event header
...................

The binlog event header starts each event and is either 13 or 19 bytes long, depending on the `binlog version`_.

::

  Binlog header
    payload:
      4              timestamp
      1              event type
      4              server-id
      4              event-size
         if binlog-version > 1:
      4              log pos
      2              flags

`timestamp`
  seconds since unix epoch

`event type`
  see `Binlog Event Type`_

`server-id`
  server-id of the originating mysql-server. Used to filter out events in circular replication.

`event-size`
  size of the event (header, post-header, body)

`log pos`
  position of the next event

`flags`
  see `Binlog Event Flag`_


Binlog Event Flag
.................

=== =======================================
hex flag
=== =======================================
01  `LOG_EVENT_BINLOG_IN_USE_F`_
02  `LOG_EVENT_FORCED_ROTATE_F`_
04  `LOG_EVENT_THREAD_SPECIFIC_F`_
08  `LOG_EVENT_SUPPRESS_USE_F`_
10  `LOG_EVENT_UPDATE_TABLE_MAP_VERSION_F`_
20  `LOG_EVENT_ARTIFICIAL_F`_
40  `LOG_EVENT_RELAY_LOG_F`_
=== =======================================

_`LOG_EVENT_BINLOG_IN_USE_F`
  gets unset in the `FORMAT_DESCRIPTION_EVENT`_ when the file gets closed to detect broken binlogs

_`LOG_EVENT_FORCED_ROTATE_F`
  unused

_`LOG_EVENT_THREAD_SPECIFIC_F`
  event is thread specific (CREATE TEMPORARY TABLE ...)

_`LOG_EVENT_SUPPRESS_USE_F`
  event doesn't need default database to be updated (CREATE DATABASE, ...)

_`LOG_EVENT_UPDATE_TABLE_MAP_VERSION_F`
  unused

_`LOG_EVENT_ARTIFICIAL_F`
  event is created by the slaves SQL-thread and shouldn't update the master-log pos

_`LOG_EVENT_RELAY_LOG_F`
  event is created by the slaves IO-thread when written to the relay log


Binlog Event Type
.................

=== =========================
hex event name               
=== =========================
00  `UNKNOWN_EVENT`_
01  `START_EVENT_V3`_
02  `QUERY_EVENT`_
03  `STOP_EVENT`_
04  `ROTATE_EVENT`_
05  `INTVAR_EVENT`_
06  `LOAD_EVENT`_
07  `SLAVE_EVENT`_
08  `CREATE_FILE_EVENT`_
09  `APPEND_BLOCK_EVENT`_
0a  `EXEC_LOAD_EVENT`_
0b  `DELETE_FILE_EVENT`_
0c  `NEW_LOAD_EVENT`_
0d  `RAND_EVENT`_
0e  `USER_VAR_EVENT`_
0f  `FORMAT_DESCRIPTION_EVENT`_
10  `XID_EVENT`_
11  `BEGIN_LOAD_QUERY_EVENT`_
12  `EXECUTE_LOAD_QUERY_EVENT`_
13  `TABLE_MAP_EVENT`_
14  `PRE_GA_DELETE_ROWS_EVENT`_
15  `PRE_GA_UPDATE_ROWS_EVENT`_
16  `PRE_GA_WRITE_ROWS_EVENT`_
17  `DELETE_ROWS_EVENT`_
18  `UPDATE_ROWS_EVENT`_
19  `WRITE_ROWS_EVENT`_
1a  `INCIDENT_EVENT`_
1b  `HEARTBEAT_LOG_EVENT`_
=== =========================

ignored events
..............

* _`UNKNOWN_EVENT`
* _`PRE_GA_DELETE_ROWS_EVENT`
* _`PRE_GA_UPDATE_ROWS_EVENT`
* _`PRE_GA_WRITE_ROWS_EVENT`
* _`SLAVE_EVENT`

START_EVENT_V3
..............

A start event is the first event of a binlog for binlog-version 1 to 3.

::

  START_EVENT_V3

  payload:
    2                binlog-version
    string[50]       mysql-server version 
    4                create timestamp


FORMAT_DESCRIPTION_EVENT
........................

A format description event is the first event of a binlog for binlog-version 4. It describes how the other events are layed out.

.. note:: added in MySQL 5.0.0 as replacement for `START_EVENT_V3`_

::

  FORMAT_DESCRIPTION_EVENT

  payload:
    2                binlog-version
    string[50]       mysql-server version 
    4                create timestamp
    1                event header length
    string[p]        event type header lengths

  example:
    $ hexdump -v -s 4 -C relay-bin.000001
    00000004  82 2d c2 4b 0f 02 00 00  00 67 00 00 00 6b 00 00  |.-.K.....g...k..|
    00000014  00 00 00 04 00 35 2e 35  2e 32 2d 6d 32 00 00 00  |.....5.5.2-m2...|
    00000024  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
    00000034  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
    00000044  00 00 00 00 00 00 00 82  2d c2 4b 13 38 0d 00 08  |........-.K.8...|
    00000054  00 12 00 04 04 04 04 12  00 00 54 00 04 1a 08 00  |..........T.....|
    00000064  00 00 08 08 08 02 00                              |........        |

`binlog-version`
  version of this binlog format.

`mysql-server version`
  version of the MySQL Server that created the binlog. The string is evaluted to apply work-arounds in the slave.

`create timestamp`
  seconds since Unix epoch when the binlog was created

`event header length`
  length of the `Binlog Event Header`_ of next events. Should always be 19.

`event type header length`
  a array indexed by `Binlog Event Type` - 1 to extract the length of the event specific header.

For `mysql-5.5.2-m2` the event specific header lengths are:

.. table:: event type header lengths by binlog-version

  +-----------------------------+-----------------+
  | event name                  | header-len      |
  |                             +-----+-----+-----+
  |                             | v=4 | v=3 | v=1 |
  +=============================+=====+=====+=====+
  | `Binlog Event Header`_      |     19    | 13  | 
  +-----------------------------+-----------+-----+
  | `START_EVENT_V3`_           |       56        | 
  +-----------------------------+-----+-----------+
  | `QUERY_EVENT`_              | 13  |     11    | 
  +-----------------------------+-----+-----------+
  | `STOP_EVENT`_               |  0              | 
  +-----------------------------+-----------+-----+
  | `ROTATE_EVENT`_             |      8    |  0  | 
  +-----------------------------+-----------+-----+
  | `INTVAR_EVENT`_             |         0       | 
  +-----------------------------+-----------------+
  | `LOAD_EVENT`_               |        18       | 
  +-----------------------------+-----------------+
  | `SLAVE_EVENT`_              |         0       | 
  +-----------------------------+-----------------+
  | `CREATE_FILE_EVENT`_        |         4       | 
  +-----------------------------+-----------------+
  | `APPEND_BLOCK_EVENT`_       |         4       | 
  +-----------------------------+-----------------+
  | `EXEC_LOAD_EVENT`_          |         4       | 
  +-----------------------------+-----------------+
  | `DELETE_FILE_EVENT`_        |         4       | 
  +-----------------------------+-----------------+
  | `NEW_LOAD_EVENT`_           |        18       | 
  +-----------------------------+-----------------+
  | `RAND_EVENT`_               |         0       | 
  +-----------------------------+-----------------+
  | `USER_VAR_EVENT`_           |         0       | 
  +-----------------------------+-----+-----------+
  | `FORMAT_DESCRIPTION_EVENT`_ | 84  |    ---    | 
  +-----------------------------+-----+-----------+
  | `XID_EVENT`_                |  0  |    ---    | 
  +-----------------------------+-----+-----------+
  | `BEGIN_LOAD_QUERY_EVENT`_   |  4  |    ---    | 
  +-----------------------------+-----+-----------+
  | `EXECUTE_LOAD_QUERY_EVENT`_ | 26  |    ---    | 
  +-----------------------------+-----+-----------+
  | `TABLE_MAP_EVENT`_          |  8  |    ---    | 
  +-----------------------------+-----+-----------+
  | `PRE_GA_DELETE_ROWS_EVENT`_ |  0  |    ---    | 
  +-----------------------------+-----+-----------+
  | `PRE_GA_UPDATE_ROWS_EVENT`_ |  0  |    ---    | 
  +-----------------------------+-----+-----------+
  | `PRE_GA_WRITE_ROWS_EVENT`_  |  0  |    ---    | 
  +-----------------------------+-----+-----------+
  | `DELETE_ROWS_EVENT`_        | 8/6 |    ---    | 
  +-----------------------------+-----+-----------+
  | `UPDATE_ROWS_EVENT`_        | 8/6 |    ---    | 
  +-----------------------------+-----+-----------+
  | `WRITE_ROWS_EVENT`_         | 8/6 |    ---    | 
  +-----------------------------+-----+-----------+
  | `INCIDENT_EVENT`_           |  2  |    ---    | 
  +-----------------------------+-----+-----------+
  | `HEARTBEAT_LOG_EVENT`_      |  0  |    ---    | 
  +-----------------------------+-----+-----------+


The event-size of ``0x67 (103)`` minus the event-header length of ``0x13 (19)`` should match the event type header length of the `FORMAT_DESCRIPTION_EVENT`_ ``0x54 (84)``.

The number of events understood by the master may differ from what the slave supports. It is calculated by::

  event-size - event-header length - 2 - 50 - 4 - 1

For ``mysql-5.5.2-m2`` it is ``0x1b (27)``.

ROTATE_EVENT
............

The rotate event is added to the binlog as last event to tell the reader what binlog to request next.

::

  ROTATE_EVENT

    post-header:
        if binlog-version > 1:
      8              position

    payload:
      string[p]      name of the next binlog


STOP_EVENT
..........

A `STOP_EVENT` has no payload or post-header.

QUERY_EVENT
...........

The query event is used to send text querys right the binlog. 

It has a post-header::

  QUERY_EVENT post header
  
    payload:
      4              slave_proxy_id
      4              execution time
      1              schema length
      2              error-code
        if binlog-version >= 4:
      2              status-vars length

and a body::

  QUERY_EVENT body

    payload
      n              status-vars
      string[n]      schema
      1              [00]
      string[p]      query

`status-vars length`
  number of bytes in the following sequence of `status-vars`

`status-vars`
  a sequence of status key-value pairs. The key is 1-byte, while its value is dependent on the key.

  ====  ============================== =========
  hex   flag                           value-len
  ====  ============================== =========
  00    `Q_FLAGS2_CODE`_               4
  01    `Q_SQL_MODE_CODE`_             8
  03    `Q_AUTO_INCREMENT`_            2 + 2
  04    `Q_CHARSET_CODE`_              2 + 2 + 2 
  05    `Q_TIME_ZONE_CODE`_            1 + n
  06    `Q_CATALOG_NZ_CODE`_           1 + n
  07    `Q_LC_TIME_NAMES_CODE`_        2
  08    `Q_CHARSET_DATABASE_CODE`_     2
  09    `Q_TABLE_MAP_FOR_UPDATE_CODE`_ 8
  ====  ============================== =========

  The value of the different status vars are:

  _`Q_FLAGS2_CODE`
    Bitmask of flags that are usual set with `SET`_:
    
    * SQL_AUTO_IS_NULL
    * FOREIGN_KEY_CHECKS
    * UNIQUE_CHECKS
    * AUTOCOMMIT

  _`Q_SQL_MODE_CODE`
    Bitmask of flags that are usual set with `SET sql_mode`_

  _`Q_AUTO_INCREMENT`
    2-byte autoincrement-increment and 2-byte autoincrement-offset

    .. note:: only written if the -increment is > 1

  _`Q_CHARSET_CODE`
    2-byte character_set_client + 2-byte collation_connection + 2-byte collation_server

    See `Connection Character Sets and Collations`_
  
  _`Q_TIME_ZONE_CODE`
    1-byte length + <length> chars of the timezone

    timezone the master is in

    See `MySQL Server Time Zone Support`_

    .. note:: only written length > 0
   
  _`Q_CATALOG_NZ_CODE`
    1-byte length + <length> chars of the catalog

    .. note:: only written length > 0

  _`Q_LC_TIME_NAMES_CODE`
    `LC_TIME` of the server. Defines how to parse week-, month and day-names in timestamps.

    .. note:: only written if code > 0 (aka "en_US")

  _`Q_CHARSET_DATABASE_CODE`
    characterset and collation of the schema

  _`Q_TABLE_MAP_FOR_UPDATE_CODE`
    a 64bit-field ... should only be used in `Row Based Replication`_ and multi-table updates

`schema`
  current schema, length taken from `schema length`

`query`
  text of the query

.. _`SET`: http://dev.mysql.com/doc/refman/5.1/en/set-option.html
.. _`SET sql_mode`: http://dev.mysql.com/doc/refman/5.1/en/server-sql-mode.html
.. _`Connection Character Sets and Collations`: http://dev.mysql.com/doc/refman/5.1/en/charset-connection.html
.. _`MySQL Server Time Zone Support`: http://dev.mysql.com/doc/refman/5.1/en/time-zone-support.html

LOAD_EVENT
..........

NEW_LOAD_EVENT
..............

CREATE_FILE_EVENT
.................

APPEND_BLOCK_EVENT
..................

EXEC_LOAD_EVENT
...............

BEGIN_LOAD_QUERY_EVENT
......................

EXECUTE_LOAD_QUERY_EVENT
........................

DELETE_FILE_EVENT
.................

RAND_EVENT
..........

Internal state of the ``RAND()`` function.

::

  RAND_EVENT
    payload:
      8              seed1
      8              seed2

XID_EVENT
.........

Transaction ID for 2PC, written whenever a ``COMMIT`` is expected.

::

  XID_EVENT
    payload:
      8              xid

INTVAR_EVENT
............

Integer based user-variables

::

  INTVAR_EVENT
    payload:
      1              type
      8              value

`type`
  ====  ====================
  hex   intvar event type
  ====  ====================
  00    INVALID_INT_EVENT
  01    LAST_INSERT_ID_EVENT
  02    INSERT_ID_EVENT
  ====  ====================

USER_VAR_EVENT
..............

::

  USER_VAR_EVENT
    payload:
      string[p]      value

`value`
  ``@`name`=...``

TABLE_MAP_EVENT
...............

The first event used in `Row Based Replication`_ declares how a table that is about to be changed is defined.

::

  TABLE_MAP_EVENT

    post-header:
      6              table id
      2              flags

    payload:
      1              schema name length
      string         schema name
      1              table name length
      string         table name
      lenenc-str     column-def
      lenenc-str     column-meta-def
      n              NULL-bitmask, length: (column-length * 8) / 7

`column-def`
  the column definitions. It is sent as length encoded string where the length of the string
  is the number of columns and each byte of it is the `column type`_ of the column.

`column-meta-def`
  type specific meta-data for each column

  ======================== ========
  type                     meta-len
  ------------------------ --------
  `MYSQL_TYPE_STRING`_     2
  `MYSQL_TYPE_VAR_STRING`_ 2
  `MYSQL_TYPE_VARCHAR`_    2
  `MYSQL_TYPE_BLOB`_       1
  `MYSQL_TYPE_DECIMAL`_    2
  `MYSQL_TYPE_NEWDECIMAL`_ 2
  `MYSQL_TYPE_DOUBLE`_     1
  `MYSQL_TYPE_FLOAT`_      1
  `MYSQL_TYPE_ENUM`_       2
  `MYSQL_TYPE_SET`_        see `MYSQL_TYPE_ENUM`
  `MYSQL_TYPE_BIT`_        0
  `MYSQL_TYPE_DATE`_       0
  `MYSQL_TYPE_DATETIME`_   0
  `MYSQL_TYPE_TIMESTAMP`_  0
  `MYSQL_TYPE_TIME`_       --
  `MYSQL_TYPE_TINY`_       0
  `MYSQL_TYPE_SHORT`_      0
  `MYSQL_TYPE_INT24`_      0
  `MYSQL_TYPE_LONG`_       0
  `MYSQL_TYPE_LONGLONG`_   0
  ======================== ========

  `MYSQL_TYPE_STRING`
    due to `Bug37426`_ layout of the string meta-data is a bit tightly packed::

      1              byte0
      1              byte1

    The two bytes encode `type` and `length`

    .. _`Bug37426`: http://bugs.mysql.com/37426

`NULL-bitmap`
  a bitmask contained a bit set for each column that can be NULL. The column-length is taken from the
  `column-def`

DELETE_ROWS_EVENT
.................

UPDATE_ROWS_EVENT
.................

WRITE_ROWS_EVENT
................

INCIDENT_EVENT
..............

::

  INCIDENT_EVENT
    payload:
      2              type
      1              message length
      n              message

`type`
  ==== ====================
  hex  name
  ==== ====================
  0000 INCIDENT_NONE
  0001 INCIDENT_LOST_EVENTS
  ==== ====================

HEARTBEAT_LOG_EVENT
...................

A artificial event generated by the master. It isn't written to the relay logs.

It is added by the master after the replication connection was idle for x-seconds to update the slaves ``Seconds_Behind_Master`` timestamp in the `SHOW SLAVE STATUS`_.

It has no payload nor post-header.

.. _`SHOW SLAVE STATUS`: http://dev.mysql.com/doc/refman/5.1/de/show-slave-status.html

COM_REGISTER_SLAVE
------------------

Registers a slave at the master. Should be sent before requesting a binlog events with `COM_BINLOG_DUMP`_.

::

  COM_REGISTER_SLAVE
    register a slave at the master

    payload:
      4              server-id
      lenenc-str     slaves hostname
      lenenc-str     slaves user
      lenenc-str     slaves password
      2              slaves mysql-port
      4              replication rank
      4              master-id

`slaves hostname`
  see `--report-host`_, usually empty

`slaves user`
  see `--report-user`_, usually empty

`slaves password`
  see `--report-password`_, usually empty

`slaves port`
  see `--report-port`_, usually empty

`replication rank`
  ignored

`server-id`
  the slaves server-id

`master-id`
  usually 0. Appears as "master id" in `SHOW SLAVE HOSTS`_ on the master. Unknown what else it impacts.
  

.. _`--report-host`: http://dev.mysql.com/doc/refman/5.0/en/replication-options-slave.html#option_mysqld_report-host
.. _`--report-user`: http://dev.mysql.com/doc/refman/5.0/en/replication-options-slave.html#option_mysqld_report-user
.. _`--report-password`: http://dev.mysql.com/doc/refman/5.0/en/replication-options-slave.html#option_mysqld_report-password
.. _`--report-port`: http://dev.mysql.com/doc/refman/5.0/en/replication-options-slave.html#option_mysqld_report-port
.. _`SHOW SLAVE HOSTS`: http://dev.mysql.com/doc/refman/5.1/en/show-slave-hosts.html

COM_BINLOG_DUMP
---------------

Requests a `binlog network stream`_ from the master starting a given position.

You can use `SHOW MASTER LOGS`_ to get the current logfile and position from the master.

The master responds either with a

* `binlog network stream`_
* a `ERR packet`_
* or (if `BINLOG_DUMP_NON_BLOCK`_ is set) with `EOF packet`_

::

  COM_BINLOG_DUMP
    request a binlog-stream from the server

    payload:
      4              binlog-pos
      2              flags
      4              server-id
      string[p]      name of the binlog-file


`flags`
  can right now has one value:

  ====  ========================
  flag  description
  ====  ========================
  01    _`BINLOG_DUMP_NON_BLOCK`
  ====  ========================

  `BINLOG_DUMP_NON_BLOCK`_
    if there is no more event to send send a `EOF packet`_ instead of blocking the connection

`server-id`
  server id of this slave

`binlog-filename`
  filename of the binlog on the master

`binlog-pos`
  position in the binlog-file to start the stream with

.. _`SHOW MASTER LOGS`: http://dev.mysql.com/doc/refman/5.1/en/show-master-logs.html

Semi-Sync Replication
=====================

In MySQL 5.5 replication can optionally be made semi-synchronous instead of the traditionally asynchronous replication.

The clients COMMIT (or in auto-commit mode the current statement) waits until _one_ slave acknowledged that it received (not 
neccesarilly executed) the transaction or a timeout is reached. In case the timeout is reached, semi-sync replication is disabled.

See http://dev.mysql.com/doc/refman/5.5/en/replication-semisync.html for more.

To see of the master supports semi-sync replication run:

.. code-block:: sql

  SHOW VARIABLES LIKE 'rpl_semi_sync_master_enabled';

The slave requests semi-sync replication by sending:

.. code-block:: sql

  SET @rpl_semi_sync_slave = 1;

which the master either responds with a `OK packet`_ if it supports semi-sync replication or with `ERR packet`_ if it doesn't.
  
Semi Sync Binlog Event
----------------------

After the ``00`` OK-byte of a `binlog network stream`_ 2 bytes get added before the normal `Binlog Event`_ continues.

  1                  [ef] semi-sync indicator
  1                  semi-sync flags

`semi-sync flags`
  ==== ====================
  flag description 
  ==== ====================
  01   _`SEMI_SYNC_ACK_REQ`
  ==== ====================

  If the `SEMI_SYNC_ACK_REQ`_ flag is set the master waits for a `Semi Sync ACK packet`_ from the slave before it sends the
  next event.

Semi Sync ACK packet
--------------------

Each `Semi Sync Binlog Event`_ with the `SEMI_SYNC_ACK_REQ`_ flag set the slave has to acknowledge with Semi-Sync ACK packet::

  SEMI_SYNC_ACK
    payload:
      1                  [ef]
      8                  log position
      string             log filename

which the master acknowledges with a `OK packet`_ or a `ERR packet`_.


