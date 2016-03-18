#define	_GNU_SOURCE	/* asprintf() */
#define	_POSIX_C_SOURCE 200809L /* getline() */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "main.h"
#include "utils.h"
#include "generate.h"
#include "check.h"

char *output_dir = DEFAULT_OUTPUT_DIR;
static char *progname;

void usage(void) {
	printf("Usage:\n"
	    "\t %s generate [-s symbol_file] [-o kabi_dir] module_dir\n"
	    "\t %s check [-s symbol_file] kabi_dir module_dir\n",
	    progname, progname);
	exit(1);
}

#define	WHITESPACE	" \t\n"

/* Remove white characters from given buffer */
static void strip(char *buf) {
	size_t i = 0, j = 0;
	while (buf[j] != '\0') {
		if (strchr(WHITESPACE, buf[j]) == NULL) {
			if (i != j)
				buf[i] = buf[j];
			i++;
		}
		j++;
	}
	buf[i] = '\0';
}

/* Get list of symbols to generate. */
static void read_symbols(char *filename, char ***symbolsp, size_t *cntp) {
	FILE *fp = fopen(filename, "r");
	char *line = NULL;
	size_t len = 0;
	size_t symbols_len = DEFAULT_BUFSIZE;
	char **symbols = safe_malloc(symbols_len * sizeof (*symbols));
	size_t i = 0;

	if (fp == NULL)
		fail("Failed to open symbol file: %s\n", strerror(errno));

	while ((getline(&line, &len, fp)) != -1) {
		if (i == symbols_len) {
			symbols_len *= 2;
			symbols = realloc(symbols,
			    symbols_len * sizeof (*symbols));
		}
		symbols[i] = line;
		strip(symbols[i]);
		i++;
		line = NULL;
		len = 0;
	}

	if (errno != 0)
		fail("getline() failed: %s\n", strerror(errno));

	if (line != NULL)
		free(line);

	fclose(fp);

	*symbolsp = symbols;
	*cntp = i;
}

static void check_is_directory(char *dir) {
	struct stat dirstat;

	if (stat(dir, &dirstat) != 0) {
		if (errno == ENOENT) {
			fail("Module directory %s does not exist!\n", dir);
		} else {
			fail("Failed to stat() directory %s: %s\n", dir,
			    strerror(errno));
		}
	}

	if (!S_ISDIR(dirstat.st_mode))
		fail("Not a directory: %s\n", dir);
}

static void parse_generate_opts(int argc, char **argv, generate_config_t *conf,
    char **symbol_file) {
	*symbol_file = NULL;

	while ((argc > 0) && (*argv[0] == '-')) {
		if (strcmp(*argv, "-o") == 0) {
			argc--; argv++;
			if (argc < 1)
				usage();
			output_dir = argv[0];
			argc--; argv++;
		} else if (strcmp(*argv, "-s") == 0) {
			argc--; argv++;
			if (argc < 1)
				usage();
			*symbol_file = argv[0];
			argc--; argv++;
		} else {
			usage();
		}
	}

	if (argc != 1)
		usage();

	conf->module_dir = argv[0];
	argc--; argv++;
}

static void generate(int argc, char **argv) {
	char *symbol_file;
	generate_config_t *conf = safe_malloc(sizeof (*conf));

	parse_generate_opts(argc, argv, conf, &symbol_file);
	check_is_directory(output_dir);

	if (symbol_file != NULL) {
		int i;

		read_symbols(symbol_file, &conf->symbols, &conf->symbol_cnt);
		conf->symbols_found = safe_malloc(conf->symbol_cnt *
		    sizeof (bool *));
		for (i = 0; i < conf->symbol_cnt; i++)
			conf->symbols_found[i] = false;
	}

	generate_symbol_defs(conf);

	if (symbol_file != NULL)
		free(conf->symbols_found);
	free(conf);
}

static void parse_check_opts(int argc, char **argv, check_config_t *conf,
    char **symbol_file) {
	*symbol_file = NULL;

	while ((argc > 0) && (*argv[0] == '-')) {
		if (strcmp(*argv, "-s") == 0) {
			argc--; argv++;
			if (argc < 1)
				usage();
			*symbol_file = argv[0];
			argc--; argv++;
		} else {
			usage();
		}
	}

	if (argc != 2)
		usage();

	conf->kabi_dir = *argv;
	argc--; argv++;
	conf->module_dir = *argv;
	argc--; argv++;

	check_is_directory(conf->kabi_dir);
	check_is_directory(conf->module_dir);

	printf("kabi-dir %s\n", conf->kabi_dir);
	printf("module-dir %s\n", conf->module_dir);
	printf("output-dir %s\n", output_dir);
	printf("symbol_file %s\n", *symbol_file);
}

static void check(int argc, char **argv) {
	char *symbol_file;
	check_config_t *conf = safe_malloc(sizeof (*conf));

	parse_check_opts(argc, argv, conf, &symbol_file);

	if (symbol_file != NULL)
		read_symbols(symbol_file, &conf->symbols, &conf->symbol_cnt);

	check_symbol_defs(conf);

	free(conf);
}

int main(int argc, char **argv) {
	progname = argv[0];

	if (argc < 2)
		usage();

	argv++; argc--;

	if (strcmp(argv[0], "generate") == 0) {
		argv++; argc--;
		generate(argc, argv);
	} else if (strcmp(argv[0], "check") == 0) {
		argv++; argc--;
		check(argc, argv);
	} else {
		usage();
	}

	return (0);
}
