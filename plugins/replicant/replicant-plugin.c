/* $%BEGINLICENSE%$
 Copyright (C) 2007-2008 MySQL AB, 2008 Sun Microsystems, Inc

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 $%ENDLICENSE%$ */
 
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h> /* FIONREAD */
#elif defined(WIN32)
#include <windows.h>
#include <winsock2.h>
#include <io.h>
#define ioctl ioctlsocket

#define STDERR_FILENO 2
#else
#include <unistd.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "network-mysqld.h"
#include "network-mysqld-proto.h"
#include "network-mysqld-binlog.h"
#include "network-mysqld-binlog-lua.h"
#include "network-mysqld-packet.h"
#include "sys-pedantic.h"
#include "glib-ext.h"

#define C(x) x, sizeof(x) - 1
#define S(x) x->str, x->len

/**
 * we have two phases
 * - getting the binglog-pos with SHOW MASTER STATUS
 * - running the BINLOG_DUMP
 *
 * - split binlog stream into multiple streams based on
 *   lua-script and push the streams into the slaves
 *   - thread-ids
 *   - server-id
 *   - database name
 *   - table-names
 * - rewrite binlogs as delayed streams (listening port per delay)
 *
 * - chaining of replicants is desired
 *   a delayed replicator can feed a splitter or the other way around
 *
 * - we have to maintain the last know position per backend, perhaps 
 *   we want to maintain this in lua land and use the tbl2str functions
 *
 * - we may want to share the config
 *
 * - we have to parse the binlog stream and should also provide a 
 *   binlog reading library
 *
 *
 */
typedef struct {
	enum { REPCLIENT_BINLOG_GET_POS, REPCLIENT_BINLOG_DUMP } state;
	char *binlog_file;
	int binlog_pos;
} plugin_con_state;

struct chassis_plugin_config {
	gchar *master_address;                   /**< listening address of the proxy */

	gchar *mysqld_username;
	gchar *mysqld_password;
	gchar *lua_script;

	network_mysqld_con *listen_con;

	gchar *binlog_file;
	int binlog_pos;
};


static plugin_con_state *plugin_con_state_init() {
	plugin_con_state *st;

	st = g_new0(plugin_con_state, 1);

	return st;
}

static void plugin_con_state_free(plugin_con_state *st) {
	if (!st) return;

	if (st->binlog_file) g_free(st->binlog_file);

	g_free(st);
}

/**
 * decode the result-set of SHOW MASTER STATUS
 */
static int network_mysqld_resultset_master_status(chassis *UNUSED_PARAM(chas), network_mysqld_con *con) {
	GList *chunk;
	guint i;
	network_socket *sock = con->server;
	plugin_con_state *st = con->plugin_con_state;
	GPtrArray *fields;
	int err = 0;

	/* scan the resultset */
	chunk = sock->recv_queue->chunks->head;

	fields = network_mysqld_proto_fielddefs_new();
	chunk = network_mysqld_proto_get_fielddefs(chunk, fields);
	g_assert(chunk);

	/* a data row */
	while (NULL != (chunk = chunk->next)) {
		network_packet packet;
		guint8 status;

		packet.data = chunk->data;
		packet.offset = 0;

		err = err || network_mysqld_proto_skip_network_header(&packet);
		err = err || network_mysqld_proto_peek_int8(&packet, &status);

		/* if we find the 2nd EOF packet we are done */
		if (status == MYSQLD_PACKET_EOF &&
		    packet.data->len < 10) break;

		for (i = 0; i < fields->len; i++) {
			guint64 field_len;
			err = err || network_mysqld_proto_get_lenenc_int(&packet, &field_len);

			if (i == 0) {
				g_assert(field_len > 0);
				/* Position */
				
				if (st->binlog_file) g_free(st->binlog_file);

				err = err || network_mysqld_proto_get_string_len(&packet, &st->binlog_file, field_len);
			} else if (i == 1) {
				/* is a string */
				gchar *num;

				g_assert(field_len > 0);

				st->binlog_pos = 0;

				err = err || network_mysqld_proto_get_string_len(&packet, &num, field_len);
				st->binlog_pos = g_ascii_strtoull(num, NULL, 10);
			} else {
				err = err || network_mysqld_proto_skip(&packet, field_len);
			}
		}

		g_message("reading binlog from: binlog-file: %s, binlog-pos: %d", 
				st->binlog_file, st->binlog_pos);
	}

	return err ? -1 : 0;
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_read_handshake) {
	network_packet packet;
	network_socket *recv_sock;
	network_socket *send_sock = NULL;
	chassis_plugin_config *config = con->config;
	network_mysqld_auth_challenge *shake;
	network_mysqld_auth_response  *auth;
	GString *auth_packet;
	int err = 0;
	
	recv_sock = con->server;
	send_sock = con->server;

	/* there should only be on packet */
	packet.data = g_queue_peek_tail(recv_sock->recv_queue->chunks);
	packet.offset = 0;

	if (packet.data->len != recv_sock->packet_len + NET_HEADER_SIZE) {
		/**
		 * packet is too short, looks nasty.
		 *
		 * report an error and let the core send a error to the 
		 * client
		 */

			g_critical("%s", G_STRLOC);
		return NETWORK_SOCKET_ERROR;
	}

	err = err || network_mysqld_proto_skip_network_header(&packet);
	if (err) return NETWORK_SOCKET_ERROR;

	shake = network_mysqld_auth_challenge_new();
	err = err || network_mysqld_proto_get_auth_challenge(&packet, shake);

	g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

	recv_sock->packet_len = PACKET_LEN_UNSET;
	
	if (err) {
		network_mysqld_auth_challenge_free(shake);
		return NETWORK_SOCKET_ERROR;
	}

	/* build the auth packet */
	auth_packet = g_string_new(NULL);

	auth = network_mysqld_auth_response_new();

	auth->capabilities    = CLIENT_BASIC_FLAGS;
	auth->charset         = shake->charset;
	auth->max_packet_size = 1 * 1024 * 1024;

	if (config->mysqld_username) {
		g_string_append(auth->username, config->mysqld_username);
	}

	if (config->mysqld_password) {
		network_mysqld_proto_scramble(auth->response, shake->challenge, config->mysqld_password);
	}

	network_mysqld_proto_append_auth_response(auth_packet, auth);

	network_mysqld_queue_append(send_sock->send_queue, S(auth_packet), send_sock->packet_id + 1);


	network_mysqld_auth_response_free(auth);
	network_mysqld_auth_challenge_free(shake);

	con->state = CON_STATE_SEND_AUTH;

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_read_auth_result) {
	network_socket *recv_sock;
	network_socket *send_sock = NULL;
	network_packet packet;
	guint8 status;
	int err = 0;

	const char query_packet[] = 
		"\x03"                    /* COM_QUERY */
		"SHOW MASTER STATUS"
		;

	recv_sock = con->server;
	send_sock = con->server;

	packet.data = g_queue_peek_tail(recv_sock->recv_queue->chunks);
	packet.offset = 0;

	/* we aren't finished yet */
	if (packet.data->len != recv_sock->packet_len + NET_HEADER_SIZE) {
		return NETWORK_SOCKET_SUCCESS;
	}

	err = err || network_mysqld_proto_skip_network_header(&packet);
	err = err || network_mysqld_proto_peek_int8(&packet, &status);
	if (err) return NETWORK_SOCKET_ERROR;

	/* the auth should be fine */
	switch (status) {
	case MYSQLD_PACKET_ERR: {
		network_mysqld_err_packet_t *err_packet;

		err_packet = network_mysqld_err_packet_new();

		err = err || network_mysqld_proto_get_err_packet(&packet, err_packet);

		if (!err) {
			g_critical("%s: repclient_read_auth_result() failed: %s (errno = %d)", 
					G_STRLOC,
					err_packet->errmsg->len ? err_packet->errmsg->str : "",
					err_packet->errcode);
		} 

		network_mysqld_err_packet_free(err_packet);

		return NETWORK_SOCKET_ERROR; }
	case MYSQLD_PACKET_OK: 
		break; 
	default:
		g_critical("%s: packet should be (OK|ERR), got: 0x%02x",
				G_STRLOC,
				status);

		return NETWORK_SOCKET_ERROR;
	} 

	recv_sock->packet_len = PACKET_LEN_UNSET;

	g_string_free(g_queue_pop_tail(recv_sock->recv_queue->chunks), TRUE);

	network_mysqld_queue_append(send_sock->send_queue, C(query_packet), 0);

	con->state = CON_STATE_SEND_QUERY;

	return NETWORK_SOCKET_SUCCESS;
}

/**
 * inject a COM_BINLOG_DUMP after we have sent our SHOW MASTER STATUS
 */
NETWORK_MYSQLD_PLUGIN_PROTO(repclient_read_query_result) {
	/* let's send the
	 *
	 * ask the server for the current binlog-file|pos and dump everything from there
	 *  
	 * - COM_BINLOG_DUMP
	 *   - 4byte pos
	 *   - 2byte flags (BINLOG_DUMP_NON_BLOCK)
	 *   - 4byte slave-server-id
	 *   - nul-term binlog name
	 *
	 * we don't need:
	 * - COM_REGISTER_SLAVE
	 *   - 4byte server-id
	 *   - nul-term host
	 *   - nul-term user
	 *   - nul-term password
	 *   - 2byte port
	 *   - 4byte recovery rank
	 *   - 4byte master-id
	 */
	network_packet packet;
	GList *chunk;
	network_socket *recv_sock;
	int is_finished = 0;
	plugin_con_state *st = con->plugin_con_state;
	chassis_plugin_config *config = con->config;
	guint8 status;
	int err = 0;
	GString *query_packet;
	int my_server_id = 2;
	network_mysqld_binlog_dump *dump;

	recv_sock = con->server;

	chunk = recv_sock->recv_queue->chunks->tail;
	packet.data = chunk->data;
	packet.offset = 0;
	
	if (packet.data->len != recv_sock->packet_len + NET_HEADER_SIZE) return NETWORK_SOCKET_SUCCESS; /* packet isn't finished yet */

	is_finished = network_mysqld_proto_get_query_result(&packet, con);
	if (is_finished == -1) {
		g_message("%s", G_STRLOC);
		return NETWORK_SOCKET_ERROR;
	}
	recv_sock->packet_len = PACKET_LEN_UNSET;
	con->resultset_is_finished = is_finished;
	if (is_finished == 0) return NETWORK_SOCKET_SUCCESS; /* wait for more */

	packet.data = chunk->data;
	packet.offset = 0;
	err = err || network_mysqld_proto_skip_network_header(&packet);
	err = err || network_mysqld_proto_peek_int8(&packet, &status);

	switch (con->parse.command) {
	case COM_BINLOG_DUMP:
		err = err || network_mysqld_proto_peek_int8(&packet, &status);

		switch (status) {
		case MYSQLD_PACKET_OK: {
			/* looks like the binlog dump started */
			network_mysqld_binlog *binlog;
			network_mysqld_binlog_event *event;

			binlog = network_mysqld_binlog_new();
			event = network_mysqld_binlog_event_new();

			err = err || network_mysqld_proto_skip(&packet, 1);
			err = err || network_mysqld_proto_get_binlog_event_header(&packet, event);
			err = err || network_mysqld_proto_get_binlog_event(&packet, binlog, event);

			if (!err && config->lua_script) {
				lua_State *L;
				/* call lua to expose the event */

				L = luaL_newstate();

				luaL_openlibs(L);

				if (0 != luaL_loadfile(L, config->lua_script)) {
					g_critical("%s: %s", G_STRLOC, lua_tostring(L, -1));
					return NETWORK_SOCKET_ERROR;
				}
				if (0 != lua_pcall(L, 0, 0, 0)) {
					g_critical("%s: %s", G_STRLOC, lua_tostring(L, -1));
					return NETWORK_SOCKET_ERROR;
				}
				lua_getglobal(L, "binlog_event_iterate");
				lua_mysqld_binlog_event_push(L, event, FALSE);
				if (0 != lua_pcall(L, 1, 1, 0)) {
					g_critical("%s: %s", G_STRLOC, lua_tostring(L, -1));
					return NETWORK_SOCKET_ERROR;
				}

				lua_close(L);
			}

			network_mysqld_binlog_event_free(event);
			network_mysqld_binlog_free(binlog);
			
			con->state = CON_STATE_READ_QUERY_RESULT;

			break; }
		case MYSQLD_PACKET_ERR: {
			network_mysqld_err_packet_t *err_packet;

			err_packet = network_mysqld_err_packet_new();

			err = err || network_mysqld_proto_get_err_packet(&packet, err_packet);

			if (!err) {
				g_critical("%s: COM_BINLOG_DUMP failed: %s (errno = %d)", 
						G_STRLOC,
						err_packet->errmsg->len ? err_packet->errmsg->str : "",
						err_packet->errcode);
			} 

			network_mysqld_err_packet_free(err_packet);

			return NETWORK_SOCKET_ERROR; }
		default:
			g_critical("%s: %d", G_STRLOC, status);
			con->state = CON_STATE_ERROR;
			break;
		}
		break;
	case COM_QUERY:
		break;
	default:
		break;
	}

	/**
	 * the resultset handler might decide to trash the send-queue
	 * 
	 * */

	switch (st->state) {
	case REPCLIENT_BINLOG_GET_POS:
		/* parse the result-set and get the 1st and 2nd column */

		err = err || network_mysqld_resultset_master_status(chas, con);
		if (err) {
			g_message("%s", G_STRLOC);
			return NETWORK_SOCKET_ERROR;
		}

		st->state = REPCLIENT_BINLOG_DUMP;

		dump = network_mysqld_binlog_dump_new();
		dump->server_id   = my_server_id;
		if (config->binlog_file) {
			dump->binlog_pos  = config->binlog_pos;
			dump->binlog_file = g_strdup(config->binlog_file);
		} else {
			dump->binlog_pos  = st->binlog_pos;
			dump->binlog_file = g_strdup(st->binlog_file);
		}

		query_packet = g_string_new(NULL);

		network_mysqld_proto_append_binlog_dump(query_packet, dump);
	       	
		network_mysqld_queue_append(con->server->send_queue, S(query_packet), 0);

		network_mysqld_binlog_dump_free(dump);
	
		g_string_free(query_packet, TRUE);

		con->state = CON_STATE_SEND_QUERY;
		network_mysqld_con_reset_command_response_state(con);

		break;
	case REPCLIENT_BINLOG_DUMP:
		/* remove all packets */

		/* trash the packets for the injection query */

		break;
	}

	if (chunk->data) g_string_free(chunk->data, TRUE);
	g_queue_delete_link(recv_sock->recv_queue->chunks, chunk);

	recv_sock->packet_len = PACKET_LEN_UNSET;

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_connect_server) {
	chassis_plugin_config *config = con->config;
	gchar *address = config->master_address;

	/* the 2nd round */
	if (con->server) {
		switch (network_socket_connect_finish(con->server)) {
		case NETWORK_SOCKET_SUCCESS:
			con->state = CON_STATE_READ_HANDSHAKE;
			return NETWORK_SOCKET_SUCCESS;
		case NETWORK_SOCKET_ERROR:
		case NETWORK_SOCKET_ERROR_RETRY:
		default:
			g_critical("%s: %s", G_STRLOC, g_strerror(errno));
			return NETWORK_SOCKET_ERROR;
		}
	}
	con->server = network_socket_new();

	if (0 != network_address_set_address(con->server->dst, address)) {
		g_critical("%s: network_address_set_address() failed", G_STRLOC);
		return NETWORK_SOCKET_ERROR;
	}
    
	/* FIXME ... add non-blocking support (getsockopt()) */

	switch (network_socket_connect(con->server)) {
	case NETWORK_SOCKET_ERROR:
		g_critical("%s: network_socket_connect() failed", G_STRLOC);
		return NETWORK_SOCKET_ERROR;
	case NETWORK_SOCKET_SUCCESS:
		con->state = CON_STATE_READ_HANDSHAKE;
		return NETWORK_SOCKET_SUCCESS;
	case NETWORK_SOCKET_ERROR_RETRY:
		/* ask it to come back again */
		return NETWORK_SOCKET_ERROR_RETRY;
	default:
		g_message("%s", G_STRLOC);
		return NETWORK_SOCKET_ERROR;
	}
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_init) {
	g_assert(con->plugin_con_state == NULL);

	con->plugin_con_state = plugin_con_state_init();
	
	con->state = CON_STATE_CONNECT_SERVER;

	return NETWORK_SOCKET_SUCCESS;
}

NETWORK_MYSQLD_PLUGIN_PROTO(repclient_cleanup) {
	if (con->plugin_con_state == NULL) return NETWORK_SOCKET_SUCCESS;

	plugin_con_state_free(con->plugin_con_state);
	
	con->plugin_con_state = NULL;

	return NETWORK_SOCKET_SUCCESS;
}

int network_mysqld_repclient_connection_init(chassis G_GNUC_UNUSED *chas, network_mysqld_con *con) {
	con->plugins.con_init                      = repclient_init;
	con->plugins.con_connect_server            = repclient_connect_server;
	con->plugins.con_read_handshake            = repclient_read_handshake;
	con->plugins.con_read_auth_result          = repclient_read_auth_result;
	con->plugins.con_read_query_result         = repclient_read_query_result;
	con->plugins.con_cleanup                   = repclient_cleanup;

	return 0;
}

chassis_plugin_config * network_mysqld_replicant_plugin_init(void) {
	chassis_plugin_config *config;

	config = g_new0(chassis_plugin_config, 1);

	return config;
}

void network_mysqld_replicant_plugin_free(chassis_plugin_config *config) {
	if (config->listen_con) {
		/**
		 * the connection will be free()ed by the network_mysqld_free()
		 */
#if 0
		event_del(&(config->listen_con->server->event));
		network_mysqld_con_free(config->listen_con);
#endif
	}

	if (config->master_address) {
		/* free the global scope */
		g_free(config->master_address);
	}

	if (config->mysqld_username) g_free(config->mysqld_username);
	if (config->mysqld_password) g_free(config->mysqld_password);

	g_free(config);
}

/**
 * plugin options 
 */
static GOptionEntry * network_mysqld_replicant_plugin_get_options(chassis_plugin_config *config) {
	guint i;

	/* make sure it isn't collected */
	static GOptionEntry config_entries[] = 
	{
		{ "replicant-master-address",            0, 0, G_OPTION_ARG_STRING, NULL, "... (default: :4040)", "<host:port>" },
		{ "replicant-username",                  0, 0, G_OPTION_ARG_STRING, NULL, "username", "" },
		{ "replicant-password",                  0, 0, G_OPTION_ARG_STRING, NULL, "password", "" },
		{ "replicant-lua-script",                0, 0, G_OPTION_ARG_STRING, NULL, "filename", "" },
		{ "replicant-binlog-file",               0, 0, G_OPTION_ARG_STRING, NULL, "filename", "" },
		{ "replicant-binlog-pos",                0, 0, G_OPTION_ARG_INT, NULL, "filename", "" },
		{ NULL,                       0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
	};

	i = 0;
	config_entries[i++].arg_data = &(config->master_address);
	config_entries[i++].arg_data = &(config->mysqld_username);
	config_entries[i++].arg_data = &(config->mysqld_password);
	config_entries[i++].arg_data = &(config->lua_script);
	config_entries[i++].arg_data = &(config->binlog_file);
	config_entries[i++].arg_data = &(config->binlog_pos);
	
	return config_entries;
}


/**
 * init the plugin with the parsed config
 */
int network_mysqld_replicant_plugin_apply_config(chassis G_GNUC_UNUSED *chas, chassis_plugin_config *config) {
	network_mysqld_con *con;

	if (!config->master_address) config->master_address = g_strdup(":4040");
	if (!config->mysqld_username) config->mysqld_username = g_strdup("repl");
	if (!config->mysqld_password) config->mysqld_password = g_strdup("");
	
	g_message("%s: setting defaults for the replicant", G_STRLOC);

	/* setup the connection and */
	con = network_mysqld_con_new();
	con->srv = chas;
	con->config = config;
	network_mysqld_repclient_connection_init(chas, con);

	network_mysqld_con_handle(-1, 0, con);

	return 0;
}

G_MODULE_EXPORT int plugin_init(chassis_plugin *p) {
	p->magic        = CHASSIS_PLUGIN_MAGIC;
	p->name         = g_strdup("replicant");
	p->version		= g_strdup("0.7.0");
	/* append the our init function to the init-hook-list */

	p->init         = network_mysqld_replicant_plugin_init;
	p->get_options  = network_mysqld_replicant_plugin_get_options;
	p->apply_config = network_mysqld_replicant_plugin_apply_config;
	p->destroy      = network_mysqld_replicant_plugin_free;

	return 0;
}

