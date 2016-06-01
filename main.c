/*
	Copyright(C) 2016, Red Hat, Inc., Stanislav Kozina

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * The aim of this program is to compare two builds of the Linux kernel and
 * list all the ABI differences in the given list of symbols between these two
 * builds.
 *
 * For both builds the kabi information first needs to be stored in a text
 * files, see generate(). The the two dumps can be compared, see check().
 * The format of the kabi information loosely follow the syntax of the Go
 * programming language for its ease of parsing.
 */

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

static char *progname;

void usage(void) {
	printf("Usage:\n"
	    "\t %s generate [-v] [-s symbol_file] [-o kabi_dir] kernel_dir\n"
	    "\t %s check [-v] [-s symbol_file] kabi_dir_old kabi_dir_new\n",
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

	errno = 0;
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
		fail("getline() failed for %s: %s\n", filename,
		    strerror(errno));

	if (line != NULL)
		free(line);

	fclose(fp);

	*symbolsp = symbols;
	*cntp = i;
}

static void parse_generate_opts(int argc, char **argv, generate_config_t *conf,
    char **symbol_file) {
	*symbol_file = NULL;
	conf->verbose = false;
	conf->kabi_dir = DEFAULT_OUTPUT_DIR;

	while ((argc > 0) && (*argv[0] == '-')) {
		if (strcmp(*argv, "-v") == 0) {
			argc--; argv++;
			conf->verbose = true;
		} else if (strcmp(*argv, "-o") == 0) {
			argc--; argv++;
			if (argc < 1)
				usage();
			conf->kabi_dir = argv[0];
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

	conf->kernel_dir = argv[0];
	argc--; argv++;

	rec_mkdir(conf->kabi_dir);
}

static void generate(int argc, char **argv) {
	char *symbol_file;
	generate_config_t *conf = safe_malloc(sizeof (*conf));

	parse_generate_opts(argc, argv, conf, &symbol_file);

	if (symbol_file != NULL) {
		int i;

		read_symbols(symbol_file, &conf->symbols, &conf->symbol_cnt);

		if (conf->verbose)
			printf("Loaded %ld symbols\n", conf->symbol_cnt);
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
	conf->verbose = false;
	int rv;

	while ((argc > 0) && (*argv[0] == '-')) {
		if (strcmp(*argv, "-v") == 0) {
			argc--; argv++;
			conf->verbose = true;
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

	if (argc != 2)
		usage();

	conf->kabi_dir_old = *argv;
	argc--; argv++;
	conf->kabi_dir_new = *argv;
	argc--; argv++;

	if ((rv = check_is_directory(conf->kabi_dir_old)) != 0)
		fail("%s: %s\n", strerror(rv), conf->kabi_dir_old);
	if ((rv = check_is_directory(conf->kabi_dir_new)) != 0)
		fail("%s: %s\n", strerror(rv), conf->kabi_dir_new);
}

static void check(int argc, char **argv) {
	char *symbol_file;
	check_config_t *conf = safe_malloc(sizeof (*conf));

	parse_check_opts(argc, argv, conf, &symbol_file);

	if (symbol_file != NULL)
		read_symbols(symbol_file, &conf->symbols, &conf->symbol_cnt);

	if (conf->verbose)
		printf("Loaded %ld symbols\n", conf->symbol_cnt);

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
