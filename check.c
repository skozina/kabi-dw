#define	_POSIX_C_SOURCE 200809L /* mkdtemp() */

#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include "main.h"
#include "utils.h"
#include "check.h"
#include "generate.h"

bool check_symbol_file(char *path, void *arg) {
	check_config_t *conf = (check_config_t *)arg;
	char *filename = path + strlen(conf->kabi_dir);

	if (conf->verbose)
		printf("Checking %s\n", filename);

	return (true);
}

void generate_new_defs(char *temp_kabi_dir, check_config_t *conf) {
	generate_config_t *gen_conf = safe_malloc(sizeof (*gen_conf));

	gen_conf->kabi_dir = temp_kabi_dir;
	gen_conf->verbose = false;
	gen_conf->module_dir = conf->module_dir;
	gen_conf->symbols = conf->symbols;
	gen_conf->symbol_cnt = conf->symbol_cnt;
	if (conf->symbols != NULL) {
		int i;

		gen_conf->symbols_found = safe_malloc(conf->symbol_cnt *
		    sizeof (bool *));
		for (i = 0; i < conf->symbol_cnt; i++)
			gen_conf->symbols_found[i] = false;
	}

	generate_symbol_defs(gen_conf);

	if (conf->symbols != NULL)
		free(gen_conf->symbols_found);
	free(gen_conf);
}

static bool remove_file(char *path, void *arg) {
	if (unlink(path) != 0) {
		printf("ERROR: Failed to unlink %s: %s\n", path,
		    strerror(errno));
	}

	return (true);
}

void check_symbol_defs(check_config_t *conf) {
	char *temp_dir = safe_malloc(strlen(TMP_DIR) + 1);

	printf("Comparing symbols defs of %s with %s...\n", conf->module_dir,
	    conf->kabi_dir);

	strncpy(temp_dir, TMP_DIR, strlen(TMP_DIR) + 1);
	if (mkdtemp(temp_dir) == NULL)
		fail("mkdtemp failed: %s\n", strerror(errno));

	printf("Working directory: %s\n", temp_dir);

	generate_new_defs(temp_dir, conf);
	walk_dir(conf->kabi_dir, check_symbol_file, conf);

	walk_dir(temp_dir, remove_file, NULL);
	if (rmdir(temp_dir) != 0) {
		printf("ERROR: Failed to remove temp directory %s: %s\n",
		    temp_dir, strerror(errno));
	}
	free(temp_dir);
}
