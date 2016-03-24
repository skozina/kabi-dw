#define	_GNU_SOURCE	/* asprintf() */
#define	_POSIX_C_SOURCE 200809L /* mkdtemp() */

#include <sys/types.h>
#include <sys/stat.h>
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

typedef bool (*parse_func_t)(char *, check_config_t *);

bool parse_typedef_file(char *, check_config_t *);
bool parse_func_file(char *, check_config_t *);
bool parse_struct_file(char *, check_config_t *);
bool parse_union_file(char *, check_config_t *);
bool parse_enum_file(char *, check_config_t *);
bool parse_var_file(char *, check_config_t *);

struct file_prefix {
	char *prefix;
	parse_func_t parse_func;
} file_prefixes[] = {
	{ TYPEDEF_FILE, parse_typedef_file},
	{ FUNC_FILE, parse_func_file},
	{ STRUCT_FILE, parse_struct_file},
	{ UNION_FILE, parse_union_file},
	{ ENUM_FILE, parse_enum_file},
	{ VAR_FILE, parse_var_file},
	{ NULL, NULL}
};

bool parse_typedef_file(char *file_name, check_config_t *conf) {
	printf("Typedef file: %s\n", file_name);
	return (true);
}

bool parse_func_file(char *file_name, check_config_t *conf) {
	printf("Func file: %s\n", file_name);
	return (true);
}

bool parse_struct_file(char *file_name, check_config_t *conf) {
	printf("Struct file: %s\n", file_name);
	return (true);
}

bool parse_union_file(char *file_name, check_config_t *conf) {
	printf("Union file: %s\n", file_name);
	return (true);
}

bool parse_enum_file(char *file_name, check_config_t *conf) {
	printf("Union file: %s\n", file_name);
	return (true);
}

bool parse_var_file(char *file_name, check_config_t *conf) {
	printf("Var file: %s\n", file_name);
	return (true);
}

parse_func_t get_parse_func(char *file_name) {
	struct file_prefix *current;
	char *delimiter_str;

	if ((delimiter_str = strstr(file_name, "--")) == NULL)
		return (NULL);

	for (current = file_prefixes; current->prefix != NULL; current++) {
		if (strncmp(file_name, current->prefix,
		    delimiter_str - file_name) == 0)
			break;
	}

	return (current->parse_func);
}

bool check_symbol_file(char *kabi_path, void *arg) {
	check_config_t *conf = (check_config_t *)arg;
	char *file_name = kabi_path + strlen(conf->kabi_dir);
	struct stat fstat;
	char *temp_kabi_path;
	FILE *fp;
	bool (*parse_func)(char *, check_config_t *);

	if (conf->verbose)
		printf("Checking %s\n", file_name);

	if (asprintf(&temp_kabi_path, "%s/%s", conf->temp_kabi_dir, file_name)
	    == -1)
		fail("asprintf() failed\n");

	if (stat(temp_kabi_path, &fstat) != 0) {
		if (errno == ENOENT) {
			printf("Symbol removed from kabi: %s\n", file_name);
		} else {
			printf("Failed to stat() file%s: %s\n", temp_kabi_path,
			    strerror(errno));
		}

		goto done;
	}

	if ((parse_func = get_parse_func(file_name)) == NULL) {
		printf("Unexpected name of the file: %s\n", file_name);
		goto done;
	}

	if ((fp = fopen(temp_kabi_path, "r")) == NULL) {
		printf("Failed to open file: %s\n", temp_kabi_path);
		goto done;
	}

	(*parse_func)(file_name, conf);

	fclose(fp);
done:
	free(temp_kabi_path);
	return (true);
}

void generate_new_defs(check_config_t *conf) {
	generate_config_t *gen_conf = safe_malloc(sizeof (*gen_conf));

	gen_conf->kabi_dir = conf->temp_kabi_dir;
	gen_conf->verbose = conf->verbose;
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
	printf("Comparing symbols defs of %s with %s...\n", conf->module_dir,
	    conf->kabi_dir);

	if (asprintf(&conf->temp_kabi_dir, "%s", TMP_DIR) == -1)
		fail("asprintf() failed\n");

	if (mkdtemp(conf->temp_kabi_dir) == NULL)
		fail("mkdtemp failed: %s\n", strerror(errno));

	printf("Working directory: %s\n", conf->temp_kabi_dir);

	generate_new_defs(conf);
	walk_dir(conf->kabi_dir, check_symbol_file, conf);

	walk_dir(conf->temp_kabi_dir, remove_file, NULL);
	if (rmdir(conf->temp_kabi_dir) != 0) {
		printf("ERROR: Failed to remove temp directory %s: %s\n",
		    conf->temp_kabi_dir, strerror(errno));
	}

	free(conf->temp_kabi_dir);
	conf->temp_kabi_dir = NULL;
}
