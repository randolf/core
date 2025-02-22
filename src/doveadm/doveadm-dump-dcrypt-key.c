/* Copyright (c) 2016-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "dcrypt.h"
#include "dcrypt-iostream.h"
#include "ostream-encrypt.h"
#include "istream-private.h"
#include "istream-decrypt.h"
#include "doveadm-dump.h"
#include "hex-binary.h"
#include "str.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#define KEY_BUF_SIZE 4096

static void dcrypt_dump_public_key_metadata(struct doveadm_cmd_context *cctx,
					    const char *buf)
{
	const char *error = NULL;
	struct dcrypt_public_key *pub_key;

	bool ret = dcrypt_key_load_public(&pub_key, buf, &error);
	if (ret == FALSE) {
		e_error(cctx->event, "dcrypt_key_load_public failed: %s", error);
		return;
	}
	enum dcrypt_key_type key_type = dcrypt_key_type_public(pub_key);
	if (key_type == DCRYPT_KEY_RSA)
		printf("key type: DCRYPT_KEY_RSA\n");
	else if (key_type == DCRYPT_KEY_EC)
		printf("key type: DCRYPT_KEY_EC\n");

	string_t *hash = t_str_new(128);
	if (!dcrypt_key_id_public(pub_key, "sha256", hash, &error)) {
		e_error(cctx->event, "dcrypt_key_id_public failed: %s", error);
	} else {
		const char *v2_hash = binary_to_hex(hash->data, hash->used);
		printf("v2 hash: %s\n", v2_hash);

		if (key_type == DCRYPT_KEY_EC) {
			buffer_set_used_size(hash, 0);
			if (!dcrypt_key_id_public_old(pub_key, hash, &error)) {
				e_error(cctx->event,
					"dcrypt_key_id_public_old failed: %s",
					error);
			} else {
				const char *v1_hash = binary_to_hex(hash->data,
								    hash->used);
				printf("v1 hash: %s\n", v1_hash);
			}
		}
	}
	dcrypt_key_unref_public(&pub_key);
}

static void dcrypt_dump_private_key_metadata(struct doveadm_cmd_context *cctx,
					     const char *buf)
{
	const char *error = NULL;
	struct dcrypt_private_key *priv_key;

	bool ret = dcrypt_key_load_private(&priv_key, buf, NULL, NULL,
			&error);
	if (ret == FALSE) {
		e_error(cctx->event, "dcrypt_key_load_private failed: %s", error);
		return;
	}
	enum dcrypt_key_type key_type = dcrypt_key_type_private(priv_key);
	if (key_type == DCRYPT_KEY_RSA)
		printf("key type: DCRYPT_KEY_RSA\n");
	else if (key_type == DCRYPT_KEY_EC)
		printf("key type: DCRYPT_KEY_EC\n");

	string_t *hash = t_str_new(128);
	if (!dcrypt_key_id_private(priv_key, "sha256", hash, &error)) {
		e_error(cctx->event, "dcrypt_key_id_private failed: %s", error);
	} else {
		const char *v2_hash = binary_to_hex(hash->data, hash->used);
		printf("v2 hash: %s\n", v2_hash);

		if (key_type == DCRYPT_KEY_EC) {
			buffer_set_used_size(hash, 0);
			if (!dcrypt_key_id_private_old(priv_key, hash, &error)) {
				e_error(cctx->event,
					"dcrypt_key_id_private_old failed: %s", error);
			} else {
				const char *v1_hash = binary_to_hex(hash->data,
								    hash->used);
				printf("v1 hash: %s\n", v1_hash);
			}
		}
	}
	dcrypt_key_unref_private(&priv_key);
}

static bool dcrypt_key_dump_metadata(struct doveadm_cmd_context *cctx,
				     const char *filename, bool print)
{
	bool ret = TRUE;
	int fd = open(filename, O_RDONLY);
	if (fd < 0) {
		if (print) e_error(cctx->event, "open(%s) failed: %m", filename);
		return FALSE;
	}

	char buf[KEY_BUF_SIZE+1];
	ssize_t res = read(fd, buf, KEY_BUF_SIZE);
	if (res < 0) {
		if (print) e_error(cctx->event, "read(%s) failed: %m", filename);
		i_close_fd(&fd);
		return FALSE;
	}
	i_close_fd(&fd);

	buf[res] = '\0';
	enum dcrypt_key_format format;
	enum dcrypt_key_version version;
	enum dcrypt_key_kind kind;
	enum dcrypt_key_encryption_type encryption_type;
	const char *encryption_key_hash;
	const char *key_hash;
	const char *error;

	ret = dcrypt_key_string_get_info(buf, &format, &version,
			&kind, &encryption_type, &encryption_key_hash,
			&key_hash, &error);
	if (ret == FALSE) {
		if (print) e_error(cctx->event,
			   "dcrypt_key_string_get_info failed: %s", error);
		return FALSE;
	}
	if (!print) return TRUE;

	switch (format) {
	case DCRYPT_FORMAT_PEM:
		printf("format: DCRYPT_FORMAT_PEM\n");
		break;
	case DCRYPT_FORMAT_DOVECOT:
		printf("format: DCRYPT_FORMAT_DOVECOT\n");
		break;
	case DCRYPT_FORMAT_JWK:
		printf("format: DCRYPT_FORMAT_JWK\n");
	}

	switch (version) {
	case DCRYPT_KEY_VERSION_1:
		printf("version: DCRYPT_KEY_VERSION_1\n");
		break;
	case DCRYPT_KEY_VERSION_2:
		printf("version: DCRYPT_KEY_VERSION_2\n");
		break;
	case DCRYPT_KEY_VERSION_NA:
		printf("version: DCRYPT_KEY_VERSION_NA\n");
		break;
	}

	switch (kind) {
	case DCRYPT_KEY_KIND_PUBLIC:
		printf("kind: DCRYPT_KEY_KIND_PUBLIC\n");
		break;
	case DCRYPT_KEY_KIND_PRIVATE:
		printf("kind: DCRYPT_KEY_KIND_PRIVATE\n");
		break;
	}

	switch (encryption_type) {
	case DCRYPT_KEY_ENCRYPTION_TYPE_NONE:
		printf("encryption_type: DCRYPT_KEY_ENCRYPTION_TYPE_NONE\n");
		break;
	case DCRYPT_KEY_ENCRYPTION_TYPE_KEY:
		printf("encryption_type: DCRYPT_KEY_ENCRYPTION_TYPE_KEY\n");
		break;
	case DCRYPT_KEY_ENCRYPTION_TYPE_PASSWORD:
		printf("encryption_type: DCRYPT_KEY_ENCRYPTION_TYPE_PASSWORD\n");
		break;
	}

	if (encryption_key_hash != NULL)
		printf("encryption_key_hash: %s\n", encryption_key_hash);
	if (key_hash != NULL)
		printf("key_hash: %s\n", key_hash);

	const char *data = t_str_rtrim(buf, "\r\n\t ");
	switch (kind) {
	case DCRYPT_KEY_KIND_PUBLIC:
		dcrypt_dump_public_key_metadata(cctx, data);
		break;
	case DCRYPT_KEY_KIND_PRIVATE:
		if (encryption_type == DCRYPT_KEY_ENCRYPTION_TYPE_NONE)
			dcrypt_dump_private_key_metadata(cctx, data);
		break;
	}
	return TRUE;
}

static bool test_dump_dcrypt_key(struct doveadm_cmd_context *cctx,
				 const char *path)
{
	if (!dcrypt_initialize("openssl", NULL, NULL))
		return FALSE;
	bool ret = dcrypt_key_dump_metadata(cctx, path, FALSE);
	return ret;
}

static void
cmd_dump_dcrypt_key(struct doveadm_cmd_context *cctx,
		    const char *path, const char *const *args ATTR_UNUSED)
{
	const char *error = NULL;
	if (!dcrypt_initialize("openssl", NULL, &error))
		i_fatal("dcrypt_initialize: %s", error);
	(void)dcrypt_key_dump_metadata(cctx, path, TRUE);
}

struct doveadm_cmd_dump doveadm_cmd_dump_dcrypt_key = {
	"dcrypt-key",
	test_dump_dcrypt_key,
	cmd_dump_dcrypt_key
};
