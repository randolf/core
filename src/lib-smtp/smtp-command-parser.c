/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "unichar.h"
#include "istream.h"
#include "istream-failure-at.h"
#include "istream-sized.h"
#include "istream-dot.h"

#include "smtp-parser.h"
#include "smtp-command-parser.h"

#include <ctype.h>

#define SMTP_COMMAND_PARSER_MAX_COMMAND_LENGTH 32

enum smtp_command_parser_state {
	SMTP_COMMAND_PARSE_STATE_INIT = 0,
	SMTP_COMMAND_PARSE_STATE_SKIP_LINE,
	SMTP_COMMAND_PARSE_STATE_COMMAND,
	SMTP_COMMAND_PARSE_STATE_SP,
	SMTP_COMMAND_PARSE_STATE_PARAMETERS,
	SMTP_COMMAND_PARSE_STATE_CR,
	SMTP_COMMAND_PARSE_STATE_LF,
	SMTP_COMMAND_PARSE_STATE_ERROR,
};

struct smtp_command_parser_state_data {
	enum smtp_command_parser_state state;

	char *cmd_name;
	char *cmd_params;

	size_t poff;
};

struct smtp_command_parser {
	struct istream *input;

	struct smtp_command_limits limits;

	const unsigned char *cur, *end;
	buffer_t *line_buffer;
	struct istream *data;

	struct smtp_command_parser_state_data state;

	enum smtp_command_parse_error error_code;
	char *error;

	bool auth_response:1;
};

static inline void ATTR_FORMAT(3, 4)
smtp_command_parser_error(struct smtp_command_parser *parser,
			  enum smtp_command_parse_error code,
			  const char *format, ...)
{
	va_list args;

	parser->state.state = SMTP_COMMAND_PARSE_STATE_ERROR;

	i_free(parser->error);
	parser->error_code = code;

	va_start(args, format);
	parser->error = i_strdup_vprintf(format, args);
	va_end(args);
}

struct smtp_command_parser *
smtp_command_parser_init(struct istream *input,
			 const struct smtp_command_limits *limits)
{
	struct smtp_command_parser *parser;

	parser = i_new(struct smtp_command_parser, 1);
	parser->input = input;
	i_stream_ref(input);

	if (limits != NULL)
		parser->limits = *limits;
	if (parser->limits.max_parameters_size == 0) {
		parser->limits.max_parameters_size =
			SMTP_COMMAND_DEFAULT_MAX_PARAMETERS_SIZE;
	}
	if (parser->limits.max_auth_size == 0) {
		parser->limits.max_auth_size =
			SMTP_COMMAND_DEFAULT_MAX_AUTH_SIZE;
	}
	if (parser->limits.max_data_size == 0) {
		parser->limits.max_data_size =
			SMTP_COMMAND_DEFAULT_MAX_DATA_SIZE;
	}

	return parser;
}

void smtp_command_parser_deinit(struct smtp_command_parser **_parser)
{
	struct smtp_command_parser *parser = *_parser;

	i_stream_unref(&parser->data);
	buffer_free(&parser->line_buffer);
	i_free(parser->state.cmd_name);
	i_free(parser->state.cmd_params);
	i_free(parser->error);
	i_stream_unref(&parser->input);
	i_free(parser);
	*_parser = NULL;
}

static void smtp_command_parser_restart(struct smtp_command_parser *parser)
{
	buffer_free(&parser->line_buffer);
	i_free(parser->state.cmd_name);
	i_free(parser->state.cmd_params);

	i_zero(&parser->state);
}

void smtp_command_parser_set_stream(struct smtp_command_parser *parser,
				    struct istream *input)
{
	i_stream_unref(&parser->input);
	if (input != NULL) {
		parser->input = input;
		i_stream_ref(parser->input);
	}
}

static inline const char *_chr_sanitize(unsigned char c)
{
	if (c >= 0x20 && c < 0x7F)
		return t_strdup_printf("`%c'", c);
	if (c == 0x0a)
		return "<LF>";
	if (c == 0x0d)
		return "<CR>";
	return t_strdup_printf("<0x%02x>", c);
}

static int smtp_command_parse_identifier(struct smtp_command_parser *parser)
{
	const unsigned char *p;

	/* The commands themselves are alphabetic characters.
	 */
	p = parser->cur + parser->state.poff;
	i_assert(p <= parser->end);
	while (p < parser->end && i_isalpha(*p))
		p++;
	if ((p - parser->cur) > SMTP_COMMAND_PARSER_MAX_COMMAND_LENGTH) {
		smtp_command_parser_error(
			parser, SMTP_COMMAND_PARSE_ERROR_BAD_COMMAND,
			"Command name is too long");
		return -1;
	}
	parser->state.poff = p - parser->cur;
	if (p == parser->end)
		return 0;
	parser->state.cmd_name = i_strdup_until(parser->cur, p);
	parser->cur = p;
	parser->state.poff = 0;
	return 1;
}

static int smtp_command_parse_parameters(struct smtp_command_parser *parser)
{
	const unsigned char *p, *mp;
	size_t max_size = (parser->auth_response ?
			   parser->limits.max_auth_size :
			   parser->limits.max_parameters_size);
	size_t buf_size = (parser->line_buffer == NULL ?
			   0 : parser->line_buffer->used);
	int nch = 1;

	i_assert(max_size == 0 || buf_size <= max_size);
	if (max_size > 0 && buf_size == max_size) {
		smtp_command_parser_error(
			parser, SMTP_COMMAND_PARSE_ERROR_LINE_TOO_LONG,
			"%s line is too long",
			(parser->auth_response ? "AUTH response" : "Command"));
		return -1;
	}

	/* We assume parameters to match textstr (HT, SP, Printable US-ASCII).
	   For command parameters, we also accept valid UTF-8 characters.
	 */
	p = parser->cur + parser->state.poff;
	while (p < parser->end) {
		unichar_t ch;

		if (parser->auth_response)
			ch = *p;
		else {
			nch = uni_utf8_get_char_n(p, (size_t)(parser->end - p),
						  &ch);
		}
		if (nch == 0)
			break;
		if (nch < 0) {
			smtp_command_parser_error(
				parser, SMTP_COMMAND_PARSE_ERROR_BAD_COMMAND,
				"Invalid UTF-8 character in command parameters");
			return -1;
		}
		if (nch == 1 && !smtp_char_is_textstr((unsigned char)ch))
			break;
		p += nch;
	}
	if (max_size > 0 && (size_t)(p - parser->cur) > (max_size - buf_size)) {
		smtp_command_parser_error(
			parser, SMTP_COMMAND_PARSE_ERROR_LINE_TOO_LONG,
			"%s line is too long",
			(parser->auth_response ? "AUTH response" : "Command"));
		return -1;
	}
	parser->state.poff = p - parser->cur;
	if (p == parser->end || nch == 0) {
		/* Parsed up to end of what is currently buffered in the input
		   stream. */
		unsigned int ch_size = (p == parser->end ?
					0 : uni_utf8_char_bytes(*p));
		size_t max_input = i_stream_get_max_buffer_size(parser->input);

		/* Move parsed data to parser's line buffer if the input stream
		   buffer is full. This can happen when the parser's limits
		   exceed the input stream max buffer size. */
		if ((parser->state.poff + ch_size) >= max_input) {
			if (parser->line_buffer == NULL) {
				buf_size = (max_input < SIZE_MAX / 2 ?
					    max_input * 2 : SIZE_MAX);
				buf_size = I_MAX(buf_size, 2048);
				buf_size = I_MIN(buf_size, max_size);

				parser->line_buffer = buffer_create_dynamic(
					default_pool, buf_size);
			}
			buffer_append(parser->line_buffer, parser->cur,
				      (p - parser->cur));

			parser->cur = p;
			parser->state.poff = 0;
		}
		return 0;
	}

	/* In the interest of improved interoperability, SMTP receivers SHOULD
	   tolerate trailing white space before the terminating <CRLF>.

	   WSP =  SP / HTAB ; white space

	   --> Trim the end of the buffer
	 */
	mp = p;
	if (mp > parser->cur) {
		while (mp > parser->cur && (*(mp-1) == ' ' || *(mp-1) == '\t'))
			mp--;
	}

	if (!parser->auth_response && mp > parser->cur && *parser->cur == ' ') {
		smtp_command_parser_error(
			parser, SMTP_COMMAND_PARSE_ERROR_BAD_COMMAND,
			"Duplicate space after command name");
		return -1;
	}

	if (parser->line_buffer == NULL) {
		/* Buffered only in input stream */
		parser->state.cmd_params = i_strdup_until(parser->cur, mp);
	} else {
		/* Buffered also in the parser */
		buffer_append(parser->line_buffer, parser->cur,
			      (mp - parser->cur));
		parser->state.cmd_params =
			buffer_free_without_data(&parser->line_buffer);
	}
	parser->cur = p;
	parser->state.poff = 0;
	return 1;
}

static int smtp_command_parse_line(struct smtp_command_parser *parser)
{
	int ret;

	/* RFC 5321, Section 4.1.1:

	   SMTP commands are character strings terminated by <CRLF>. The
	   commands themselves are alphabetic characters terminated by <SP> if
	   parameters follow and <CRLF> otherwise. (In the interest of improved
	   interoperability, SMTP receivers SHOULD tolerate trailing white space
	   before the terminating <CRLF>.)
	 */
	for (;;) {
		switch (parser->state.state) {
		case SMTP_COMMAND_PARSE_STATE_INIT:
			smtp_command_parser_restart(parser);
			if (parser->auth_response) {
				/* Parse AUTH response as bare parameters */
				parser->state.state =
					SMTP_COMMAND_PARSE_STATE_PARAMETERS;
			} else {
				parser->state.state =
					SMTP_COMMAND_PARSE_STATE_COMMAND;
			}
			if (parser->cur == parser->end)
				return 0;
			if (parser->auth_response)
				break;
			/* fall through */
		case SMTP_COMMAND_PARSE_STATE_COMMAND:
			ret = smtp_command_parse_identifier(parser);
			if (ret <= 0)
				return ret;
			parser->state.state = SMTP_COMMAND_PARSE_STATE_SP;
			if (parser->cur == parser->end)
				return 0;
			/* fall through */
		case SMTP_COMMAND_PARSE_STATE_SP:
			if (*parser->cur == '\r') {
				parser->state.state =
					SMTP_COMMAND_PARSE_STATE_CR;
				break;
			} else if (*parser->cur == '\n') {
				parser->state.state =
					SMTP_COMMAND_PARSE_STATE_LF;
				break;
			} else if (*parser->cur != ' ') {
				smtp_command_parser_error(parser,
					SMTP_COMMAND_PARSE_ERROR_BAD_COMMAND,
					"Unexpected character %s in command name",
					_chr_sanitize(*parser->cur));
				return -1;
			}
			parser->cur++;
			parser->state.state =
				SMTP_COMMAND_PARSE_STATE_PARAMETERS;
			if (parser->cur >= parser->end)
				return 0;
			/* fall through */
		case SMTP_COMMAND_PARSE_STATE_PARAMETERS:
			ret = smtp_command_parse_parameters(parser);
			if (ret <= 0)
				return ret;
			parser->state.state = SMTP_COMMAND_PARSE_STATE_CR;
			if (parser->cur == parser->end)
				return 0;
			/* fall through */
		case SMTP_COMMAND_PARSE_STATE_CR:
			if (*parser->cur == '\r') {
				parser->cur++;
			} else if (*parser->cur != '\n') {
				smtp_command_parser_error(parser,
					SMTP_COMMAND_PARSE_ERROR_BAD_COMMAND,
					"Unexpected character %s in %s",
					_chr_sanitize(*parser->cur),
					(parser->auth_response ?
					 "AUTH response" :
					 "command parameters"));
				return -1;
			}
			parser->state.state = SMTP_COMMAND_PARSE_STATE_LF;
			if (parser->cur == parser->end)
				return 0;
			/* fall through */
		case SMTP_COMMAND_PARSE_STATE_LF:
			if (*parser->cur != '\n') {
				smtp_command_parser_error(parser,
					SMTP_COMMAND_PARSE_ERROR_BAD_COMMAND,
					"Expected LF after CR at end of %s, "
					"but found %s",
					(parser->auth_response ?
					 "AUTH response" : "command"),
					_chr_sanitize(*parser->cur));
				return -1;
			}
			parser->cur++;
			parser->state.state = SMTP_COMMAND_PARSE_STATE_INIT;
			return 1;
		case SMTP_COMMAND_PARSE_STATE_ERROR:
			/* Skip until end of line */
			while (parser->cur < parser->end &&
			       *parser->cur != '\n')
				parser->cur++;
			if (parser->cur == parser->end)
				return 0;
			parser->cur++;
			parser->state.state = SMTP_COMMAND_PARSE_STATE_INIT;
			break;
		default:
			i_unreached();
		}
	}

	i_unreached();
}

static int smtp_command_parse(struct smtp_command_parser *parser)
{
	const unsigned char *begin;
	size_t size, old_bytes = 0;
	int ret;

	while ((ret = i_stream_read_data(parser->input, &begin, &size,
					 old_bytes)) > 0) {
		parser->cur = begin;
		parser->end = parser->cur + size;

		ret = smtp_command_parse_line(parser);
		i_stream_skip(parser->input, parser->cur - begin);
		if (ret != 0)
			return ret;
		old_bytes = i_stream_get_data_size(parser->input);
	}
	i_assert(ret != -2);

	if (ret < 0) {
		i_assert(parser->input->eof);
		if (parser->input->stream_errno == 0) {
			if (parser->state.state ==
			    SMTP_COMMAND_PARSE_STATE_INIT)
				ret = -2;
			smtp_command_parser_error(
				parser, SMTP_COMMAND_PARSE_ERROR_BROKEN_COMMAND,
				"Premature end of input");
		} else {
			smtp_command_parser_error(
				parser, SMTP_COMMAND_PARSE_ERROR_BROKEN_STREAM,
				"%s", i_stream_get_disconnect_reason(parser->input));
		}
	}
	return ret;
}

bool smtp_command_parser_pending_data(struct smtp_command_parser *parser)
{
	if (parser->data == NULL)
		return FALSE;
	return i_stream_have_bytes_left(parser->data);
}

static int smtp_command_parse_finish_data(struct smtp_command_parser *parser)
{
	const unsigned char *data;
	size_t size;
	int ret;

	parser->error_code = SMTP_COMMAND_PARSE_ERROR_NONE;
	parser->error = NULL;

	if (parser->data == NULL)
		return 1;
	if (parser->data->eof) {
		i_stream_unref(&parser->data);
		return 1;
	}

	while ((ret = i_stream_read_data(parser->data, &data, &size, 0)) > 0)
		i_stream_skip(parser->data, size);
	if (ret == 0 || parser->data->stream_errno != 0) {
		switch (parser->data->stream_errno) {
		case 0:
			return 0;
		case EMSGSIZE:
			smtp_command_parser_error(
				parser,	SMTP_COMMAND_PARSE_ERROR_DATA_TOO_LARGE,
				"Command data too large");
			break;
		default:
			smtp_command_parser_error(
				parser, SMTP_COMMAND_PARSE_ERROR_BROKEN_STREAM,
				"%s", i_stream_get_disconnect_reason(parser->data));
		}
		return -1;
	}
	i_stream_unref(&parser->data);
	return 1;
}

int smtp_command_parse_next(struct smtp_command_parser *parser,
			    const char **cmd_name_r, const char **cmd_params_r,
			    enum smtp_command_parse_error *error_code_r,
			    const char **error_r)
{
	int ret;

	i_assert(!parser->auth_response ||
		 parser->state.state == SMTP_COMMAND_PARSE_STATE_INIT ||
		 parser->state.state == SMTP_COMMAND_PARSE_STATE_ERROR);
	parser->auth_response = FALSE;

	*error_code_r = parser->error_code = SMTP_COMMAND_PARSE_ERROR_NONE;
	*error_r = NULL;

	i_free_and_null(parser->error);

	/* Make sure we finished streaming payload from previous command
	   before we continue. */
	ret = smtp_command_parse_finish_data(parser);
	if (ret <= 0) {
		if (ret < 0) {
			*error_code_r = parser->error_code;
			*error_r = parser->error;
		}
		return ret;
	}

	ret = smtp_command_parse(parser);
	if (ret <= 0) {
		if (ret < 0) {
			*error_code_r = parser->error_code;
			*error_r = parser->error;
			parser->state.state = SMTP_COMMAND_PARSE_STATE_ERROR;
		}
		return ret;
	}

	i_assert(parser->state.state == SMTP_COMMAND_PARSE_STATE_INIT);
	*cmd_name_r = parser->state.cmd_name;
	*cmd_params_r = (parser->state.cmd_params == NULL ?
			 "" : parser->state.cmd_params);
	return 1;
}

struct istream *
smtp_command_parse_data_with_size(struct smtp_command_parser *parser,
				  uoff_t size)
{
	i_assert(parser->data == NULL);
	if (size > parser->limits.max_data_size) {
		/* Not supposed to happen; command should check size */
		parser->data = i_stream_create_error_str(EMSGSIZE, 
			"Command data size exceeds maximum "
			"(%"PRIuUOFF_T" > %"PRIuUOFF_T")",
			size, parser->limits.max_data_size);
	} else {
		// FIXME: Make exact_size stream type
		struct istream *limit_input =
			i_stream_create_limit(parser->input, size);
		parser->data = i_stream_create_min_sized(limit_input, size);
		i_stream_unref(&limit_input);
	}
	i_stream_ref(parser->data);
	return parser->data;
}

struct istream *
smtp_command_parse_data_with_dot(struct smtp_command_parser *parser)
{
	struct istream *data;
	i_assert(parser->data == NULL);

	data = i_stream_create_dot(parser->input, TRUE);
	if (parser->limits.max_data_size != UOFF_T_MAX) {
		parser->data = i_stream_create_failure_at(
			data, parser->limits.max_data_size, EMSGSIZE,
			t_strdup_printf("Command data size exceeds maximum "
					"(> %"PRIuUOFF_T")",
					parser->limits.max_data_size));
		i_stream_unref(&data);
	} else {
		parser->data = data;
	}
	i_stream_ref(parser->data);
	return parser->data;
}

int smtp_command_parse_auth_response(
	struct smtp_command_parser *parser, const char **line_r,
	enum smtp_command_parse_error *error_code_r, const char **error_r)
{
	int ret;

	i_assert(parser->auth_response ||
		 parser->state.state == SMTP_COMMAND_PARSE_STATE_INIT ||
		 parser->state.state == SMTP_COMMAND_PARSE_STATE_ERROR);
	parser->auth_response = TRUE;

	*error_code_r = parser->error_code = SMTP_COMMAND_PARSE_ERROR_NONE;
	*error_r = NULL;

	i_free_and_null(parser->error);

	/* Make sure we finished streaming payload from previous command
	   before we continue. */
	ret = smtp_command_parse_finish_data(parser);
	if (ret <= 0) {
		if (ret < 0) {
			*error_code_r = parser->error_code;
			*error_r = parser->error;
		}
		return ret;
	}

	ret = smtp_command_parse(parser);
	if (ret <= 0) {
		if (ret < 0) {
			*error_code_r = parser->error_code;
			*error_r = parser->error;
			parser->state.state = SMTP_COMMAND_PARSE_STATE_ERROR;
		}
		return ret;
	}

	i_assert(parser->state.state == SMTP_COMMAND_PARSE_STATE_INIT);
	*line_r = parser->state.cmd_params;
	parser->auth_response = FALSE;
	return 1;
}
