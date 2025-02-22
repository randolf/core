/* Copyright (c) 2016-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "lib-signals.h"
#include "ioloop.h"
#include "dict-private.h"

#include <stdio.h>

static int pending = 0;
static volatile bool stop = FALSE;

static void sig_die(const siginfo_t *si ATTR_UNUSED, void *context ATTR_UNUSED)
{
	stop = TRUE;
}

static void lookup_callback(const struct dict_lookup_result *result,
			    void *context ATTR_UNUSED)
{
	if (result->error != NULL)
		i_error("%s", result->error);
	/*else if (result->ret == 0)
		i_info("not found");
	else
		i_info("%s", result->value);*/
	pending--;
}

static void commit_callback(const struct dict_commit_result *result,
			    void *context ATTR_UNUSED)
{
	if (result->ret < 0)
		i_error("commit %d", result->ret);
	pending--;
}

int main(int argc, char *argv[])
{
	const char *prefix, *uri;
	struct dict *dict;
	struct dict_settings set;
	struct dict_op_settings opset;
	struct ioloop *ioloop;
	const char *error;
	unsigned int i;
	char key[1000], value[100];

	lib_init();
	lib_signals_init();
	ioloop = io_loop_create();
	lib_signals_set_handler(SIGINT, LIBSIG_FLAG_RESTART, sig_die, NULL);
	dict_driver_register(&dict_driver_client);

	if (argc < 3)
		i_fatal("Usage: <prefix> <uri>");
	prefix = argv[1];
	uri = argv[2];

	i_zero(&set);
	i_zero(&opset);
	set.base_dir = "/var/run/dovecot";
	opset.username = "testuser";

	if (dict_init(uri, &set, &dict, &error) < 0)
		i_fatal("dict_init(%s) failed: %s", argv[1], error);

	for (i = 0; !stop; i++) {
		i_snprintf(key, sizeof(key), "%s/%02x", prefix,
			   i_rand_limit(0xff));
		i_snprintf(value, sizeof(value), "%04x", i_rand_limit(0xffff));
		switch (i_rand_limit(4)) {
		case 0:
			pending++;
			dict_lookup_async(dict, &opset, key, lookup_callback, NULL);
			break;
		case 1: {
			struct dict_transaction_context *trans;

			pending++;
			trans = dict_transaction_begin(dict, &opset);
			dict_set(trans, key, value);
			dict_transaction_commit_async(&trans, commit_callback, NULL);
			break;
		}
		case 2: {
			struct dict_transaction_context *trans;

			pending++;
			trans = dict_transaction_begin(dict, &opset);
			dict_unset(trans, key);
			dict_transaction_commit_async(&trans, commit_callback, NULL);
			break;
		}
		case 3: {
			struct dict_iterate_context *iter;
			const char *k, *v;

			iter = dict_iterate_init(dict, &opset, prefix, DICT_ITERATE_FLAG_EXACT_KEY);
			while (dict_iterate(iter, &k, &v)) ;
			if (dict_iterate_deinit(&iter, &error) < 0)
				i_error("iter failed: %s", error);
			break;
		}
		}
		while (pending > 100) {
			dict_wait(dict);
			printf("%d\n", pending); fflush(stdout);
		}
	}
	dict_wait(dict);
	dict_deinit(&dict);
	dict_driver_unregister(&dict_driver_client);

	io_loop_destroy(&ioloop);
	lib_signals_deinit();
	lib_deinit();
}
