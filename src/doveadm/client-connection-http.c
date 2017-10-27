/* Copyright (c) 2010-2017 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "compat.h"
#include "lib-signals.h"
#include "base64.h"
#include "ioloop.h"
#include "str.h"
#include "str-sanitize.h"
#include "istream.h"
#include "ostream.h"
#include "strescape.h"
#include "settings-parser.h"
#include "iostream-ssl.h"
#include "iostream-temp.h"
#include "istream-seekable.h"
#include "master-service.h"
#include "master-service-ssl.h"
#include "master-service-settings.h"
#include "mail-storage-service.h"
#include "http-server.h"
#include "http-request.h"
#include "http-response.h"
#include "http-url.h"
#include "doveadm-util.h"
#include "doveadm-server.h"
#include "doveadm-mail.h"
#include "doveadm-print.h"
#include "doveadm-settings.h"
#include "client-connection-private.h"
#include "json-parser.h"

#include <unistd.h>
#include <ctype.h>

enum client_request_parse_state {
	CLIENT_REQUEST_PARSE_INIT,
	CLIENT_REQUEST_PARSE_CMD,
	CLIENT_REQUEST_PARSE_CMD_NAME,
	CLIENT_REQUEST_PARSE_CMD_PARAMS,
	CLIENT_REQUEST_PARSE_CMD_PARAM_KEY,
	CLIENT_REQUEST_PARSE_CMD_PARAM_VALUE,
	CLIENT_REQUEST_PARSE_CMD_PARAM_ARRAY,
	CLIENT_REQUEST_PARSE_CMD_PARAM_ISTREAM,
	CLIENT_REQUEST_PARSE_CMD_ID,
	CLIENT_REQUEST_PARSE_CMD_DONE,
	CLIENT_REQUEST_PARSE_DONE
};

struct client_request_http {
	pool_t pool;
	struct client_connection_http *conn;

	struct http_server_request *http_request;

	struct io *io;
	struct istream *input;
	struct ostream *output;

	struct json_parser *json_parser;

	const struct doveadm_cmd_ver2 *cmd;
	struct doveadm_cmd_param *cmd_param;
	struct ioloop *ioloop;
	ARRAY_TYPE(doveadm_cmd_param_arr_t) pargv;
	int method_err;
	char *method_id;
	bool first_row;
	bool value_is_array;

	enum client_request_parse_state parse_state;
};

struct client_connection_http {
	struct client_connection conn;

	struct http_server_connection *http_conn;

	struct client_request_http *request;
};

typedef void doveadm_server_handler_t(struct client_request_http *req);

struct doveadm_http_server_mount {
	const char *verb;
	const char *path;
	doveadm_server_handler_t *handler;
	bool auth;
};

static struct http_server *doveadm_http_server;

static void doveadm_http_server_send_response(struct client_request_http *req);

/*
 * API
 */

static void doveadm_http_server_options_handler(struct client_request_http *);
static void doveadm_http_server_print_mounts(struct client_request_http *);
static void doveadm_http_server_send_api_v1(struct client_request_http *);
static void doveadm_http_server_read_request_v1(struct client_request_http *);

static struct doveadm_http_server_mount doveadm_http_server_mounts[] = {
{
	.verb = "OPTIONS",
	.path = NULL,
	.handler = doveadm_http_server_options_handler,
	.auth = FALSE
},{
	.verb = "GET",
	.path = "/",
	.handler = doveadm_http_server_print_mounts,
	.auth = TRUE
},{
	.verb = "GET",
	.path = "/doveadm/v1",
	.handler = doveadm_http_server_send_api_v1,
	.auth = TRUE
},{
	.verb = "POST",
	.path = "/doveadm/v1",
	.handler = doveadm_http_server_read_request_v1,
	.auth = TRUE
}
};

static void doveadm_http_server_json_error(void *context, const char *error)
{
	struct client_request_http *req = context;
	struct ostream *output = req->output;
	string_t *escaped;

	escaped = str_new(req->pool, 10);

	o_stream_nsend_str(output, "[\"error\",{\"type\":\"");
	json_append_escaped(escaped, error);
	o_stream_nsend_str(output, str_c(escaped));
	o_stream_nsend_str(output, "\", \"exitCode\":");
        str_truncate(escaped,0);
	str_printfa(escaped, "%d", doveadm_exit_code);
	o_stream_nsend_str(output, str_c(escaped));
	o_stream_nsend_str(output, "},\"");
	str_truncate(escaped,0);
	if (req->method_id != NULL) {
	        json_append_escaped(escaped, req->method_id);
		o_stream_nsend_str(output, str_c(escaped));
	}
	o_stream_nsend_str(output, "\"]");
}

static void doveadm_http_server_json_success(void *context, struct istream *result)
{
	struct client_request_http *req = context;
	struct ostream *output = req->output;
	string_t *escaped;

	escaped = str_new(req->pool, 10);

	o_stream_nsend_str(output, "[\"doveadmResponse\",");
	o_stream_nsend_istream(output, result);
	o_stream_nsend_str(output, ",\"");
	if (req->method_id != NULL) {
		json_append_escaped(escaped, req->method_id);
		o_stream_nsend_str(output, str_c(escaped));
	}
	o_stream_nsend_str(output, "\"]");
}

static void
doveadm_http_server_command_execute(struct client_request_http *req)
{
	struct client_connection_http *conn = req->conn;
	struct doveadm_cmd_context cctx;
	struct istream *is;
	const char *user;
	struct ioloop *ioloop, *prev_ioloop;

	/* final preflight check */
	if (req->method_err == 0 && !doveadm_client_is_allowed_command(conn->conn.set, req->cmd->name))
		req->method_err = 403;
	if (req->method_err != 0) {
		if (req->method_err == 404) {
			doveadm_http_server_json_error(req, "unknownMethod");
		} else if (req->method_err == 403) {
			doveadm_http_server_json_error(req, "unAuthorized");
		} else if (req->method_err == 400) {
			doveadm_http_server_json_error(req, "invalidRequest");
		} else {
			doveadm_http_server_json_error(req, "internalError");
		}
		return;
	}

	prev_ioloop = current_ioloop;
	i_zero(&cctx);
	cctx.conn_type = conn->conn.type;
	cctx.input = req->input;
	cctx.output = req->output;

	// create iostream
	doveadm_print_ostream = iostream_temp_create("/tmp/doveadm.", 0);
	cctx.cmd = req->cmd;

	if ((cctx.cmd->flags & CMD_FLAG_NO_PRINT) == 0)
		doveadm_print_init(DOVEADM_PRINT_TYPE_JSON);

	/* then call it */
	doveadm_cmd_params_null_terminate_arrays(&req->pargv);
	cctx.argv = array_get(&req->pargv, (unsigned int*)&cctx.argc);
	ioloop = io_loop_create();
	doveadm_exit_code = 0;

	cctx.local_ip = conn->conn.local_ip;
	cctx.local_port = conn->conn.local_port;
	cctx.remote_ip = conn->conn.remote_ip;
	cctx.remote_port = conn->conn.remote_port;

	if (doveadm_cmd_param_str(&cctx, "user", &user))
		i_info("Executing command '%s' as '%s'", cctx.cmd->name, user);
	else
		i_info("Executing command '%s'", cctx.cmd->name);
	client_connection_set_proctitle(&conn->conn, cctx.cmd->name);
	cctx.cmd->cmd(&cctx);
	client_connection_set_proctitle(&conn->conn, "");

	io_loop_set_current(prev_ioloop);
	o_stream_switch_ioloop(req->output);
	io_loop_set_current(ioloop);
	io_loop_destroy(&ioloop);

	if ((cctx.cmd->flags & CMD_FLAG_NO_PRINT) == 0)
		doveadm_print_deinit();
	if (o_stream_nfinish(doveadm_print_ostream) < 0) {
		i_info("Error writing output in command %s: %s",
		       req->cmd->name,
		       o_stream_get_error(req->output));
		doveadm_exit_code = EX_TEMPFAIL;
	}

	is = iostream_temp_finish(&doveadm_print_ostream, 4096);

	if (req->first_row == TRUE)
		req->first_row = FALSE;
	else
		o_stream_nsend_str(req->output,",");

	if (doveadm_exit_code != 0) {
		if (doveadm_exit_code == 0 || doveadm_exit_code == EX_TEMPFAIL)
			i_error("Command %s failed", req->cmd->name);
		doveadm_http_server_json_error(req, "exitCode");
	} else {
		doveadm_http_server_json_success(req, is);
	}
	i_stream_unref(&is);
}

static int doveadm_http_server_istream_read(struct client_request_http *req)
{
	struct istream *v_input = req->cmd_param->value.v_istream;
	const unsigned char *data;
	size_t size;

	while (i_stream_read_more(v_input, &data, &size) > 0)
		i_stream_skip(v_input, size);
	if (!v_input->eof)
		return 0;

	if (v_input->stream_errno != 0) {
		i_error("read(%s) failed: %s",
			i_stream_get_name(v_input),
			i_stream_get_error(v_input));
		req->method_err = 400;
		return -1;
	}
	return 1;
}

/**
 * this is to ensure we can handle arrays and other special parameter types
 */
static int doveadm_http_server_json_parse_next(struct client_request_http *req, enum json_type *type, const char **value)
{
	int ret;
	const char *tmp;

	switch (req->parse_state) {
	case CLIENT_REQUEST_PARSE_CMD_PARAM_ISTREAM:
		ret = doveadm_http_server_istream_read(req);
		if (ret != 1)
			return ret;
		req->parse_state = CLIENT_REQUEST_PARSE_CMD_PARAM_KEY;
		break;
	case CLIENT_REQUEST_PARSE_CMD_PARAM_ARRAY:
		/* reading through parameters in an array */
		while ((ret = json_parse_next(req->json_parser,
					     type, value)) > 0) {
			if (*type == JSON_TYPE_ARRAY_END)
				break;
			if (*type != JSON_TYPE_STRING)
				return -2;
			tmp = p_strdup(req->pool,*value);
			array_append(&req->cmd_param->value.v_array, &tmp, 1);
		}
		if (ret <= 0)
			return ret;
		req->parse_state = CLIENT_REQUEST_PARSE_CMD_PARAM_KEY;
		break;
	case CLIENT_REQUEST_PARSE_CMD_PARAM_VALUE:
		if (req->cmd_param->type == CMD_PARAM_ISTREAM) {
			struct istream* is[2] = {0};

			ret = json_parse_next_stream(req->json_parser, &is[0]);
			if (ret != 1)
				return ret;
			req->cmd_param->value.v_istream = i_stream_create_seekable_path(is, IO_BLOCK_SIZE, "/tmp/doveadm.");
			i_stream_unref(&is[0]);
			req->cmd_param->value_set = TRUE;
			req->parse_state = CLIENT_REQUEST_PARSE_CMD_PARAM_ISTREAM;
			return doveadm_http_server_json_parse_next(req, type, value);
		}
		ret = json_parse_next(req->json_parser, type, value);
		if (ret != 1)
			return ret;
		if (req->cmd_param->type == CMD_PARAM_ARRAY) {
			p_array_init(&req->cmd_param->value.v_array, req->pool, 1);
			req->cmd_param->value_set = TRUE;
			if (*type == JSON_TYPE_ARRAY) {
				/* start of array */
				req->value_is_array = TRUE;
				req->parse_state = CLIENT_REQUEST_PARSE_CMD_PARAM_ARRAY;
				return doveadm_http_server_json_parse_next(req, type, value);
			}
			if (*type != JSON_TYPE_STRING) {
				/* FIXME: should handle other than string too */
				return -2;
			}
			tmp = p_strdup(req->pool, *value);
			array_append(&req->cmd_param->value.v_array, &tmp, 1);
		} else {
			req->cmd_param->value_set = TRUE;
			switch(req->cmd_param->type) {
			case CMD_PARAM_BOOL:
				req->cmd_param->value.v_bool = (strcmp(*value,"true") == 0);
				break;
			case CMD_PARAM_INT64:
				if (str_to_int64(*value, &req->cmd_param->value.v_int64) != 0) {
					req->method_err = 400;
				}
				break;
			case CMD_PARAM_IP:
				if (net_addr2ip(*value, &req->cmd_param->value.v_ip) != 0) {
					req->method_err = 400;
				}
				break;
			case CMD_PARAM_STR:
				req->cmd_param->value.v_string = p_strdup(req->pool, *value);
				break;
			default:
				break;
			}
		}
		req->parse_state = CLIENT_REQUEST_PARSE_CMD_PARAM_KEY;
		break;
	default:
		break;
	}

	return json_parse_next(req->json_parser, type, value); /* just get next */
}

static bool
doveadm_http_handle_json_v1(struct client_request_http *req,
			    enum json_type type, const char *value)
{
	const struct doveadm_cmd_ver2 *ccmd;
	struct doveadm_cmd_param *par;
	bool found;

	switch (req->parse_state) {
	case CLIENT_REQUEST_PARSE_INIT:
		if (type != JSON_TYPE_ARRAY)
			return FALSE;
		req->parse_state = CLIENT_REQUEST_PARSE_CMD;
		req->first_row = TRUE;
		o_stream_nsend_str(req->output,"[");
		return TRUE;
	case CLIENT_REQUEST_PARSE_CMD:
		if (type == JSON_TYPE_ARRAY_END) {
			req->parse_state = CLIENT_REQUEST_PARSE_DONE;
			return TRUE;
		}
		if (type != JSON_TYPE_ARRAY)
			return FALSE;
		req->method_err = 0;
		p_free_and_null(req->pool, req->method_id);
		req->cmd = NULL;
		doveadm_cmd_params_clean(&req->pargv);
		req->parse_state = CLIENT_REQUEST_PARSE_CMD_NAME;
		return TRUE;
	case CLIENT_REQUEST_PARSE_CMD_NAME:
		if (type != JSON_TYPE_STRING)
			return FALSE;
		/* see if we can find it */
		found = FALSE;
		array_foreach(&doveadm_cmds_ver2, ccmd) {
			if (i_strccdascmp(ccmd->name, value) == 0) {
				req->cmd = ccmd;
				found = TRUE;
				break;
			}
		}
		if (!found) {
			json_parse_skip_next(req->json_parser);
			req->parse_state = CLIENT_REQUEST_PARSE_CMD_ID;
			req->method_err = 404;
		} else {
			struct doveadm_cmd_param *param;
			int pargc;

			/* initialize pargv */
			for (pargc = 0; req->cmd->parameters[pargc].name != NULL; pargc++) {
				param = array_append_space(&req->pargv);
				*param = req->cmd->parameters[pargc];
				param->value_set = FALSE;
			}
			req->parse_state = CLIENT_REQUEST_PARSE_CMD_PARAMS;
		}
		return TRUE;
	case CLIENT_REQUEST_PARSE_CMD_PARAMS:
		if (type == JSON_TYPE_OBJECT_END) {
			req->parse_state = CLIENT_REQUEST_PARSE_CMD_ID;
			return TRUE;
		}
		if (type != JSON_TYPE_OBJECT)
			return FALSE;
		req->parse_state = CLIENT_REQUEST_PARSE_CMD_PARAM_KEY;
		return TRUE;
	case CLIENT_REQUEST_PARSE_CMD_PARAM_KEY:
		if (type == JSON_TYPE_OBJECT_END) {
			req->parse_state = CLIENT_REQUEST_PARSE_CMD_ID;
			return TRUE;
		}
		i_assert(type == JSON_TYPE_OBJECT_KEY);
		/* go hunting */
		found = FALSE;
		array_foreach_modifiable(&req->pargv, par) {
			if (i_strccdascmp(par->name, value) == 0) {
				/* it's already set, cannot have same key twice in json */
				if (par->value_set)
					return FALSE;
				req->cmd_param = par;
				found = TRUE;
				break;
			}
		}
		/* skip parameters if error has already occurred */
		if (!found || req->method_err != 0) {
			json_parse_skip_next(req->json_parser);
			req->parse_state = CLIENT_REQUEST_PARSE_CMD_PARAM_KEY;
			req->method_err = 400;
		} else {
			if (req->cmd_param->value_set) {
				// FIXME: should be returned as error to client, not logged
				i_info("Parameter %s already set",
				       req->cmd_param->name);
				return FALSE;
			}
			req->value_is_array = FALSE;
			req->parse_state = CLIENT_REQUEST_PARSE_CMD_PARAM_VALUE;
		}
		return TRUE;
	case CLIENT_REQUEST_PARSE_CMD_ID:
		if (type != JSON_TYPE_STRING)
			return FALSE;
		req->method_id = p_strdup(req->pool, value);
		req->parse_state = CLIENT_REQUEST_PARSE_CMD_DONE;
		return TRUE;
	case CLIENT_REQUEST_PARSE_CMD_DONE:
		/* should be end of array */
		if (type != JSON_TYPE_ARRAY_END)
			return FALSE;
		doveadm_http_server_command_execute(req);
		req->parse_state = CLIENT_REQUEST_PARSE_CMD;
		return TRUE;
	case CLIENT_REQUEST_PARSE_DONE:
		// FIXME: should be returned as error to client, not logged
		i_info("Got unexpected elements in JSON data");
		return TRUE;
	default:
		break;
	}
	i_unreached();
}

static void
doveadm_http_server_read_request_v1(struct client_request_http *req)
{
	struct http_server_request *http_sreq = req->http_request;
	enum json_type type;
	const char *value, *error;
	int ret;

	if (req->json_parser == NULL) {
		req->json_parser = json_parser_init_flags(req->input, JSON_PARSER_NO_ROOT_OBJECT);
	}

	while ((ret = doveadm_http_server_json_parse_next(req, &type, &value)) == 1) {
		if (!doveadm_http_handle_json_v1(req, type, value))
			break;
	}

	if (!req->input->eof && ret == 0)
		return;
	io_remove(&req->io);

	doveadm_cmd_params_clean(&req->pargv);

	if (ret == -2 || (ret == 1 && req->parse_state != CLIENT_REQUEST_PARSE_DONE)) {
		/* this will happen if the parser above runs into unexpected element, but JSON is OK */
		http_server_request_fail_close(http_sreq, 400, "Unexpected element in input");
		// FIXME: should be returned as error to client, not logged
		i_info("unexpected element");
		return;
	}

	if (req->input->stream_errno != 0) {
		http_server_request_fail_close(http_sreq, 400, "Client disconnected");
		i_info("read(client) failed: %s",
		       i_stream_get_error(req->input));
		return;
	}

	if (json_parser_deinit(&req->json_parser, &error) != 0) {
		// istream JSON parsing failures do not count as errors
		http_server_request_fail_close(http_sreq, 400, "Invalid JSON input");
		// FIXME: should be returned as error to client, not logged
		i_info("JSON parse error: %s", error);
		return;
	}

	i_stream_destroy(&req->input);
	o_stream_nsend_str(req->output,"]");

	doveadm_http_server_send_response(req);
}

static void doveadm_http_server_camelcase_value(string_t *value)
{
	size_t i, k;
	char *ptr = str_c_modifiable(value);

	for (i = 0, k = 0; i < strlen(ptr);) {
		if (ptr[i] == ' ' || ptr[i] == '-') {
			i++;
			ptr[k++] = i_toupper(ptr[i++]);
		} else {
			ptr[k++] = ptr[i++];
		}
	}
	str_truncate(value, k);
}

static void
doveadm_http_server_send_api_v1(struct client_request_http *req)
{
	struct ostream *output = req->output;
	const struct doveadm_cmd_ver2 *cmd;
	const struct doveadm_cmd_param *par;
	unsigned int i, k;
	string_t *tmp;
	bool sent;

	tmp = str_new(req->pool, 8);

	o_stream_nsend_str(output,"[\n");
	for (i = 0; i < array_count(&doveadm_cmds_ver2); i++) {
		cmd = array_idx(&doveadm_cmds_ver2, i);
		if (i > 0)
			o_stream_nsend_str(output, ",\n");
		o_stream_nsend_str(output, "\t{\"command\":\"");
		json_append_escaped(tmp, cmd->name);
		doveadm_http_server_camelcase_value(tmp);
		o_stream_nsend_str(output, str_c(tmp));
		o_stream_nsend_str(output, "\", \"parameters\":[");

		sent = FALSE;
		for (k = 0; cmd->parameters[k].name != NULL; k++) {
			str_truncate(tmp, 0);
			par = &(cmd->parameters[k]);
			if ((par->flags & CMD_PARAM_FLAG_DO_NOT_EXPOSE) != 0)
				continue;
			if (sent)
				o_stream_nsend_str(output, ",\n");
			else
				o_stream_nsend_str(output, "\n");
			sent = TRUE;
			o_stream_nsend_str(output, "\t\t{\"name\":\"");
			json_append_escaped(tmp, par->name);
			doveadm_http_server_camelcase_value(tmp);
			o_stream_nsend_str(output, str_c(tmp));
			o_stream_nsend_str(output, "\",\"type\":\"");
			switch(par->type) {
			case CMD_PARAM_BOOL:
				o_stream_nsend_str(output, "boolean");
				break;
			case CMD_PARAM_INT64:
				o_stream_nsend_str(output, "integer");
				break;
			case CMD_PARAM_ARRAY:
				o_stream_nsend_str(output, "array");
				break;
			case CMD_PARAM_IP:
			case CMD_PARAM_ISTREAM:
			case CMD_PARAM_STR:
				o_stream_nsend_str(output, "string");
			}
			o_stream_nsend_str(output, "\"}");
		}
		if (k > 0)
			o_stream_nsend_str(output,"\n\t");
		o_stream_nsend_str(output,"]}");
		str_truncate(tmp, 0);
	}
	o_stream_nsend_str(output,"\n]");
	doveadm_http_server_send_response(req);
}

static void
doveadm_http_server_options_handler(struct client_request_http *req)
{
	struct http_server_request *http_sreq = req->http_request;
	struct http_server_response *http_resp;

	http_resp = http_server_response_create(http_sreq, 200, "OK");
	http_server_response_add_header(http_resp,
		"Access-Control-Allow-Origin", "*");
	http_server_response_add_header(http_resp,
		"Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	http_server_response_add_header(http_resp,
		"Access-Control-Allow-Request-Headers",
		"Content-Type, X-API-Key, Authorization");
	http_server_response_add_header(http_resp,
		"Access-Control-Allow-Headers",
		"Content-Type, WWW-Authenticate");
	http_server_response_submit_close(http_resp);
}

static void
doveadm_http_server_print_mounts(struct client_request_http *req)
{
	struct ostream *output = req->output;
	unsigned int i;

	o_stream_nsend_str(output, "[\n");
	for (i = 0; i < N_ELEMENTS(doveadm_http_server_mounts); i++) {
		if (i > 0)
			o_stream_nsend_str(output, ",\n");
		o_stream_nsend_str(output, "{\"method\":\"");
		if (doveadm_http_server_mounts[i].verb == NULL)
			o_stream_nsend_str(output, "*");
		else
			o_stream_nsend_str(output, doveadm_http_server_mounts[i].verb);
		o_stream_nsend_str(output, "\",\"path\":\"");
		if (doveadm_http_server_mounts[i].path == NULL)
			o_stream_nsend_str(output, "*");
		else
			o_stream_nsend_str(output, doveadm_http_server_mounts[i].path);
		o_stream_nsend_str(output, "\"}");
	}
	o_stream_nsend_str(output, "\n]");
	doveadm_http_server_send_response(req);
}

/*
 * Request
 */

static void doveadm_http_server_send_response(struct client_request_http *req)
{
	struct http_server_request *http_sreq = req->http_request;
	struct http_server_response *http_resp;
	struct istream *payload = NULL;

	if (req->output != NULL) {
		if (o_stream_nfinish(req->output) == -1) {
			i_info("error writing output: %s",
			       o_stream_get_error(req->output));
			o_stream_destroy(&req->output);
			http_server_request_fail_close(http_sreq, 500, "Internal server error");
			return;
		}

		payload = iostream_temp_finish(&req->output,
					       IO_BLOCK_SIZE);
	}
	
	http_resp = http_server_response_create(http_sreq, 200, "OK");
	http_server_response_add_header(http_resp, "Content-Type",
		"application/json; charset=utf-8");

	if (payload != NULL) {
		http_server_response_set_payload(http_resp, payload);
		i_stream_unref(&payload);
	}

	http_server_response_submit_close(http_resp);
}

static void
doveadm_http_server_request_destroy(struct client_request_http *req)
{
	struct client_connection_http *conn = req->conn;
	struct http_server_request *http_sreq = req->http_request;
	const struct http_request *http_req =
		http_server_request_get(http_sreq);
	struct http_server_response *http_resp =
		http_server_request_get_response(http_sreq);

	i_assert(conn->request == req);

	if (http_resp != NULL) {
		const char *agent, *url, *reason;
		uoff_t size;
		int status;

		http_server_response_get_status(http_resp, &status, &reason);
		size = http_server_response_get_total_size(http_resp);
		agent = http_request_header_get(http_req, "User-Agent");
		if (agent == NULL) agent = "";

		url = http_url_create(http_req->target.url);
		i_info("doveadm: %s %s %s \"%s %s "
		       "HTTP/%d.%d\" %d %"PRIuUOFF_T" \"%s\" \"%s\"",
		       net_ip2addr(&conn->conn.remote_ip), "-", "-",
		       http_req->method, http_req->target.url->path,
		       http_req->version_major, http_req->version_minor,
		       status, size, url, agent);
	}
	if (req->json_parser != NULL) {
		const char *error ATTR_UNUSED;
		(void)json_parser_deinit(&req->json_parser, &error);
		// we've already failed, ignore error
	}
	if (req->output != NULL)
		o_stream_set_no_error_handling(req->output, TRUE);
	io_remove(&req->io);
	o_stream_destroy(&req->output);
	i_stream_destroy(&req->input);

	http_server_request_unref(&req->http_request);
	http_server_switch_ioloop(doveadm_http_server);

	pool_unref(&req->pool);
	conn->request = NULL;
}

static bool
doveadm_http_server_auth_basic(struct client_request_http *req,
			       const struct http_auth_credentials *creds)
{
	struct client_connection_http *conn = req->conn;
	const struct doveadm_settings *set = conn->conn.set;
	string_t *b64_value;
	char *value;

	if (*set->doveadm_password == '\0') {
		i_error("Invalid authentication attempt to HTTP API: "
			"Basic authentication scheme not enabled");
		return FALSE;
	}

	b64_value = str_new(conn->conn.pool, 32);
	value = p_strdup_printf(conn->conn.pool, "doveadm:%s", set->doveadm_password);
	base64_encode(value, strlen(value), b64_value);
	if (creds->data != NULL && strcmp(creds->data, str_c(b64_value)) == 0)
		return TRUE;

	i_error("Invalid authentication attempt to HTTP API "
		"(using Basic authentication scheme)");
	return FALSE;
}

static bool
doveadm_http_server_auth_api_key(struct client_request_http *req,
				 const struct http_auth_credentials *creds)
{
	struct client_connection_http *conn = req->conn;
	const struct doveadm_settings *set = doveadm_settings;
	string_t *b64_value;

	if (*set->doveadm_api_key == '\0') {
		i_error("Invalid authentication attempt to HTTP API: "
			"X-Dovecot-API authentication scheme not enabled");
		return FALSE;
	}

	b64_value = str_new(conn->conn.pool, 32);
	base64_encode(set->doveadm_api_key, strlen(set->doveadm_api_key), b64_value);
	if (creds->data != NULL && strcmp(creds->data, str_c(b64_value)) == 0)
		return TRUE;

	i_error("Invalid authentication attempt to HTTP API "
		"(using X-Dovecot-API authentication scheme)");
	return FALSE;
}


static bool
doveadm_http_server_auth_verify(struct client_request_http *req,
				const struct http_auth_credentials *creds)
{
	/* see if the mech is supported */
	if (strcasecmp(creds->scheme, "Basic") == 0)
		return doveadm_http_server_auth_basic(req, creds);
	if (strcasecmp(creds->scheme, "X-Dovecot-API") == 0)
		return doveadm_http_server_auth_api_key(req, creds);

	i_error("Unsupported authentication scheme to HTTP API: %s",
		str_sanitize(creds->scheme, 128));
	return FALSE;
}

static bool
doveadm_http_server_authorize_request(struct client_request_http *req)
{
	struct client_connection_http *conn = req->conn;
	struct http_server_request *http_sreq = req->http_request;
	bool auth = FALSE;
	struct http_auth_credentials creds;

	/* no authentication specified */
	if (doveadm_settings->doveadm_api_key[0] == '\0' &&
		*conn->conn.set->doveadm_password == '\0') {
		http_server_request_fail_close(http_sreq, 500, "Internal Server Error");
		i_error("No authentication defined in configuration. Add API key or password");
		return FALSE;
	}
	if (http_server_request_get_auth(http_sreq, &creds) > 0)
		auth = doveadm_http_server_auth_verify(req, &creds);
	if (!auth) {
		struct http_server_response *http_resp;

		http_resp = http_server_response_create(http_sreq, 401, "Authentication required");
		if (doveadm_settings->doveadm_api_key[0] != '\0') {
			http_server_response_add_header(http_resp,
				"WWW-Authenticate", "X-Dovecot-API"
			);
		}
		if (*conn->conn.set->doveadm_password != '\0') {
			http_server_response_add_header(http_resp,
				"WWW-Authenticate", "Basic Realm=\"doveadm\""
			);
		}
		http_server_response_submit_close(http_resp);
	}
	return auth;
}

static void
doveadm_http_server_handle_request(void *context, struct http_server_request *http_sreq)
{
	struct client_connection_http *conn = context;
	struct client_request_http *req;
	const struct http_request *http_req =
		http_server_request_get(http_sreq);
	struct doveadm_http_server_mount *ep = NULL;
	pool_t pool;
	unsigned int i;

	/* no pipelining possible due to synchronous handling of requests */
	i_assert(conn->request == NULL);

	pool = pool_alloconly_create("doveadm request", 1024*16);
	req = p_new(pool, struct client_request_http, 1);
	req->pool = pool;
	req->conn = conn;

	req->http_request = http_sreq;
	http_server_request_ref(req->http_request);

	http_server_request_set_destroy_callback(http_sreq, doveadm_http_server_request_destroy, req);

	conn->request = req;

	for (i = 0; i < N_ELEMENTS(doveadm_http_server_mounts); i++) {
		if (doveadm_http_server_mounts[i].verb == NULL ||
		    strcmp(http_req->method, doveadm_http_server_mounts[i].verb) == 0) {
			if (doveadm_http_server_mounts[i].path == NULL ||
                            strcmp(http_req->target.url->path, doveadm_http_server_mounts[i].path) == 0) {
				ep = &doveadm_http_server_mounts[i];
				break;
			}
		}
	}

	if (ep == NULL) {
		http_server_request_fail_close(http_sreq, 404, "Path Not Found");
		return;
	}

 	if (ep->auth == TRUE && !doveadm_http_server_authorize_request(req))
		return;

	if (strcmp(http_req->method, "POST") == 0) {
		/* handle request */
		req->input = http_req->payload;
		i_stream_set_name(req->input, net_ip2addr(&conn->conn.remote_ip));
		i_stream_ref(req->input);
		req->io = io_add_istream(req->input, *ep->handler, req);
		req->output = iostream_temp_create_named
			("/tmp/doveadm.", 0, net_ip2addr(&conn->conn.remote_ip));
		p_array_init(&req->pargv, req->pool, 5);
		ep->handler(req);
	} else {
		req->output = iostream_temp_create_named
			("/tmp/doveadm.", 0, net_ip2addr(&conn->conn.remote_ip));
		ep->handler(req);
	}
}

/*
 * Connection
 */

static void doveadm_http_server_connection_destroy(void *context, const char *reason);

static const struct http_server_callbacks doveadm_http_callbacks = {
        .connection_destroy = doveadm_http_server_connection_destroy,
        .handle_request = doveadm_http_server_handle_request
};

static void
client_connection_http_free(struct client_connection *_conn)
{
	struct client_connection_http *conn =
		(struct client_connection_http *)_conn;

	if (conn->http_conn != NULL) {
		/* We're not in the lib-http/server's connection destroy callback. */
		http_server_connection_close(&conn->http_conn,
			"Server shutting down");
	}
}

struct client_connection *
client_connection_http_create(int fd, bool ssl)
{
	struct client_connection_http *conn;
	pool_t pool;

	pool = pool_alloconly_create("doveadm client", 1024);
	conn = p_new(pool, struct client_connection_http, 1);

	if (client_connection_init(&conn->conn,
		CLIENT_CONNECTION_TYPE_HTTP, pool, fd) < 0) {
		pool_unref(&conn->conn.pool);
		return NULL;
	}
	conn->conn.free = client_connection_http_free;

	conn->http_conn = http_server_connection_create(doveadm_http_server,
			fd, fd, ssl, &doveadm_http_callbacks, conn);
	return &conn->conn;
}

static void
doveadm_http_server_connection_destroy(void *context,
	const char *reason ATTR_UNUSED)
{
	struct client_connection_http *conn =
		(struct client_connection_http *)context;
	struct client_connection *bconn = &conn->conn;

	if (conn->http_conn == NULL) {
		/* already destroying client directly */
		return;
	}

	/* HTTP connection is destroyed already now */
	conn->http_conn = NULL;

	/* destroy the connection itself */
	client_connection_destroy(&bconn);
}

/*
 * Server
 */

void doveadm_http_server_init(void)
{
	struct http_server_settings http_set = {
		.rawlog_dir = doveadm_settings->doveadm_http_rawlog_dir,
	};

	doveadm_http_server = http_server_init(&http_set);
}

void doveadm_http_server_deinit(void)
{
	http_server_deinit(&doveadm_http_server);
}
