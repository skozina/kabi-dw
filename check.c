#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "main.h"
#include "utils.h"
#include "check.h"

bool check_symbol_file(char *path, void *arg) {
	check_config_t *conf = (check_config_t *)arg;
	char *filename = path + strlen(conf->symbols_dir);

	printf("Checking %s\n", filename);

	return (true);
}

void check_symbol_defs(check_config_t *conf) {
	printf("symbols dir: %s\n", conf->symbols_dir);
	walk_dir(conf->symbols_dir, check_symbol_file, conf);
}
