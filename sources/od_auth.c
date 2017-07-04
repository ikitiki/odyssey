
/*
 * ODISSEY.
 *
 * PostgreSQL connection pooler and request router.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>

#include <machinarium.h>
#include <shapito.h>

#include "od_macro.h"
#include "od_version.h"
#include "od_list.h"
#include "od_pid.h"
#include "od_id.h"
#include "od_syslog.h"
#include "od_log.h"
#include "od_daemon.h"
#include "od_scheme.h"
#include "od_lex.h"
#include "od_config.h"
#include "od_msg.h"
#include "od_system.h"
#include "od_instance.h"

#include "od_server.h"
#include "od_server_pool.h"
#include "od_client.h"
#include "od_client_pool.h"
#include "od_route_id.h"
#include "od_route.h"
#include "od_route_pool.h"
#include "od_io.h"

#include "od_router.h"
#include "od_pooler.h"
#include "od_relay.h"
#include "od_frontend.h"
#include "od_backend.h"
#include "od_auth.h"

static inline int
od_auth_frontend_cleartext(od_client_t *client)
{
	od_instance_t *instance = client->system->instance;

	/* AuthenticationCleartextPassword */
	so_stream_t *stream = &client->stream;
	so_stream_reset(stream);
	int rc;
	rc = so_bewrite_authentication_clear_text(stream);
	if (rc == -1)
		return -1;
	rc = od_write(client->io, stream);
	if (rc == -1) {
		od_error_client(&instance->log, &client->id,
		                "auth", "write error: %s",
		                machine_error(client->io));
		return -1;
	}

	/* wait for password response */
	while (1) {
		so_stream_reset(stream);
		rc = od_read(client->io, stream, UINT32_MAX);
		if (rc == -1) {
			od_error_client(&instance->log, &client->id, "auth",
			                "read error: %s",
			                machine_error(client->io));
			return -1;
		}
		uint8_t type = *stream->s;
		od_debug_client(&instance->log, &client->id, "auth",
		                "%c", *stream->s);
		/* PasswordMessage */
		if (type == 'p')
			break;
	}

	/* read password message */
	so_password_t client_token;
	so_password_init(&client_token);
	rc = so_beread_password(&client_token, stream->s,
	                        so_stream_used(stream));
	if (rc == -1) {
		od_error_client(&instance->log, &client->id, "auth",
		                "password read error");
		od_frontend_error(client, SO_ERROR_PROTOCOL_VIOLATION,
		                  "bad password message");
		so_password_free(&client_token);
		return -1;
	}

	/* set user password */
	so_password_t client_password = {
		.password_len = client->scheme->user_password_len + 1,
		.password     = client->scheme->user_password,
	};

	/* authenticate */
	int check = so_password_compare(&client_password, &client_token);
	so_password_free(&client_token);
	if (! check) {
		od_log_client(&instance->log, &client->id, "auth",
		              "user '%s' incorrect password",
		              client->startup.user);
		od_frontend_error(client, SO_ERROR_INVALID_PASSWORD,
		                  "incorrect password");
		return -1;
	}
	return 0;
}

static inline int
od_auth_frontend_md5(od_client_t *client)
{
	od_instance_t *instance = client->system->instance;

	/* generate salt */
	uint32_t salt = so_password_salt(&client->key);

	/* AuthenticationMD5Password */
	so_stream_t *stream = &client->stream;
	so_stream_reset(stream);
	int rc;
	rc = so_bewrite_authentication_md5(stream, (uint8_t*)&salt);
	if (rc == -1)
		return -1;
	rc = od_write(client->io, stream);
	if (rc == -1) {
		od_error_client(&instance->log, &client->id, "auth",
		                "write error: %s",
		                machine_error(client->io));
		return -1;
	}

	/* wait for password response */
	while (1) {
		int rc;
		so_stream_reset(stream);
		rc = od_read(client->io, stream, UINT32_MAX);
		if (rc == -1) {
			od_error_client(&instance->log, &client->id, "auth",
			                "read error: %s",
			                machine_error(client->io));
			return -1;
		}
		uint8_t type = *stream->s;
		od_debug_client(&instance->log, &client->id, "auth",
		                "%c", *stream->s);
		/* PasswordMessage */
		if (type == 'p')
			break;
	}

	/* read password message */
	so_password_t client_token;
	so_password_init(&client_token);
	rc = so_beread_password(&client_token, stream->s, so_stream_used(stream));
	if (rc == -1) {
		od_error_client(&instance->log, &client->id, "auth",
		                "password read error");
		od_frontend_error(client, SO_ERROR_PROTOCOL_VIOLATION,
		                  "bad password message");
		so_password_free(&client_token);
		return -1;
	}

	/* set user password */
	so_password_t client_password;
	so_password_init(&client_password);
	rc = so_password_md5(&client_password,
	                     so_parameter_value(client->startup.user),
	                     client->startup.user->value_len - 1,
	                     client->scheme->user_password,
	                     client->scheme->user_password_len,
	                     (uint8_t*)&salt);
	if (rc == -1) {
		od_error_client(&instance->log, &client->id, "auth",
		                "memory allocation error");
		so_password_free(&client_password);
		so_password_free(&client_token);
		return -1;
	}

	/* authenticate */
	int check = so_password_compare(&client_password, &client_token);
	so_password_free(&client_password);
	so_password_free(&client_token);
	if (! check) {
		od_log_client(&instance->log, &client->id, "auth",
		              "user '%s' incorrect password",
		              client->startup.user);
		od_frontend_error(client, SO_ERROR_INVALID_PASSWORD,
		                  "incorrect password");
		return -1;
	}
	return 0;
}

static inline int
od_auth_frontend_block(od_client_t *client)
{
	od_instance_t *instance = client->system->instance;
	od_log_client(&instance->log, &client->id, "auth",
	              "user '%s.%s' is blocked",
	              so_parameter_value(client->startup.database),
	              so_parameter_value(client->startup.user));
	od_frontend_error(client, SO_ERROR_INVALID_AUTHORIZATION_SPECIFICATION,
	                  "user blocked");
	return 0;
}

int od_auth_frontend(od_client_t *client)
{
	od_instance_t *instance = client->system->instance;

	/* authentication mode */
	int rc;
	switch (client->scheme->auth_mode) {
	case OD_ACLEAR_TEXT:
		rc = od_auth_frontend_cleartext(client);
		if (rc == -1)
			return -1;
		break;
	case OD_AMD5:
		rc = od_auth_frontend_md5(client);
		if (rc == -1)
			return -1;
		break;
	case OD_ABLOCK:
		od_auth_frontend_block(client);
		return -1;
	case OD_ANONE:
		break;
	default:
		assert(0);
		break;
	}

	/* pass */
	so_stream_t *stream = &client->stream;
	so_stream_reset(stream);
	rc = so_bewrite_authentication_ok(stream);
	if (rc == -1)
		return -1;
	rc = od_write(client->io, stream);
	if (rc == -1) {
		od_error_client(&instance->log, &client->id, "auth",
		                "write error: %s",
		                machine_error(client->io));
		return -1;
	}
	return 0;
}

static inline int
od_auth_backend_cleartext(od_server_t *server)
{
	od_instance_t *instance = server->system->instance;
	od_route_t *route = server->route;
	assert(route != NULL);

	od_debug_server(&instance->log, &server->id, "auth",
	                "requested clear-text authentication");

	/* use storage or user password */
	char *password;
	int   password_len;
	if (route->scheme->storage_password) {
		password = route->scheme->storage_password;
		password_len = route->scheme->storage_password_len;
	} else
	if (route->scheme->user_password) {
		password = route->scheme->user_password;
		password_len = route->scheme->user_password_len;
	} else {
		od_error_server(&instance->log, &server->id, "auth"
		                "password required for route '%s.%s'",
		                route->scheme->db->name,
		                route->scheme->user);
		return -1;
	}

	/* PasswordMessage */
	so_stream_t *stream = &server->stream;
	so_stream_reset(stream);
	int rc;
	rc = so_fewrite_password(stream, password, password_len + 1);
	if (rc == -1) {
		od_error_server(&instance->log, &server->id, "auth",
		                "memory allocation error");
		return -1;
	}
	rc = od_write(server->io, stream);
	if (rc == -1) {
		od_error_server(&instance->log, &server->id, "auth",
		                "write error: %s",
		                machine_error(server->io));
		return -1;
	}
	return 0;
}

static inline int
od_auth_backend_md5(od_server_t *server, uint8_t salt[4])
{
	od_instance_t *instance = server->system->instance;
	od_route_t *route = server->route;
	assert(route != NULL);

	od_debug_server(&instance->log, &server->id, "auth",
	                "requested md5 authentication");

	/* use storage user or route user */
	char *user;
	int   user_len;
	if (route->scheme->storage_user) {
		user = route->scheme->storage_user;
		user_len = route->scheme->storage_user_len;
	} else {
		user = route->scheme->user;
		user_len = route->scheme->user_len;
	}

	/* use storage or user password */
	char *password;
	int   password_len;
	if (route->scheme->storage_password) {
		password = route->scheme->storage_password;
		password_len = route->scheme->storage_password_len;
	} else
	if (route->scheme->user_password) {
		password = route->scheme->user_password;
		password_len = route->scheme->user_password_len;
	} else {
		od_error_server(&instance->log, &server->id, "auth"
		                "password required for route '%s.%s'",
		                route->scheme->db->name,
		                route->scheme->user);
		return -1;
	}

	/* prepare md5 password using server supplied salt */
	so_password_t client_password;
	so_password_init(&client_password);
	int rc;
	rc = so_password_md5(&client_password, user, user_len,
	                     password,
	                     password_len, (uint8_t*)salt);
	if (rc == -1) {
		od_error_server(&instance->log, &server->id, "auth",
		                "memory allocation error");
		so_password_free(&client_password);
		return -1;
	}

	/* PasswordMessage */
	so_stream_t *stream = &server->stream;
	so_stream_reset(stream);
	rc = so_fewrite_password(stream,
	                         client_password.password,
	                         client_password.password_len);
	so_password_free(&client_password);
	if (rc == -1) {
		od_error_server(&instance->log, &server->id, "auth",
		                "memory allocation error");
		return -1;
	}
	rc = od_write(server->io, stream);
	if (rc == -1) {
		od_error_server(&instance->log, &server->id, "auth",
		                "write error: %s",
		                machine_error(server->io));
		return -1;
	}
	return 0;
}

int od_auth_backend(od_server_t *server)
{
	od_instance_t *instance = server->system->instance;

	so_stream_t *stream = &server->stream;
	assert(*stream->s == 'R');

	uint32_t auth_type;
	uint8_t  salt[4];
	int rc;
	rc = so_feread_auth(&auth_type, salt, stream->s,
	                    so_stream_used(stream));
	if (rc == -1) {
		od_error_server(&instance->log, &server->id, "auth",
		                "failed to parse authentication message");
		return -1;
	}
	switch (auth_type) {
	/* AuthenticationOk */
	case 0:
		return 0;
	/* AuthenticationCleartextPassword */
	case 3:
		rc = od_auth_backend_cleartext(server);
		if (rc == -1)
			return -1;
		break;
	/* AuthenticationMD5Password */
	case 5:
		rc = od_auth_backend_md5(server, salt);
		if (rc == -1)
			return -1;
		break;
	/* unsupported */
	default:
		od_error_server(&instance->log, &server->id, "auth",
		                "unsupported authentication method");
		return -1;
	}

	/* wait for authentication response */
	while (1) {
		int rc;
		so_stream_reset(stream);
		rc = od_read(server->io, &server->stream, UINT32_MAX);
		if (rc == -1) {
			od_error_server(&instance->log, &server->id, "auth",
			                "read error: %s",
			                machine_error(server->io));
			return -1;
		}
		char type = *server->stream.s;
		od_debug_server(&instance->log, &server->id, "auth",
		                "%c", type);
		switch (type) {
		case 'R':
			rc = so_feread_auth(&auth_type, salt, stream->s,
			                    so_stream_used(stream));
			if (rc == -1) {
				od_error_server(&instance->log, &server->id, "auth",
				                "failed to parse authentication message");
				return -1;
			}
			if (auth_type != 0) {
				od_error_server(&instance->log, &server->id, "auth",
				                "incorrect authentication flow");
				return 0;
			}
			return 0;
		case 'E':
			od_backend_error(server, "auth", stream->s,
			                 so_stream_used(stream));
			return -1;
		}
	}
	return 0;
}
