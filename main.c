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
 * files, see generate(). The the two dumps can be compared, see compare().
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
#include <getopt.h>

#include "main.h"
#include "utils.h"
#include "generate.h"
#include "objects.h"

static char *progname;

void usage(void) {
	printf("Usage:\n"
	    "\t %s generate [options] kernel_dir\n"
	    "\t %s show [options] kabi_file...\n"
	    "\t %s compare [options] kabi_dir kabi_dir...\n",
	       progname, progname, progname);
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

void generate_usage() {
	printf("Usage:\n"
	       "\tgenerate [options] kernel_dir\n"
	       "\nOptions:\n"
	       "    -h, --help:\t\tshow this message\n"
	       "    -v, --verbose:\tdisplay debug information\n"
	       "    -o, --output kabi_dir:\n\t\t\t"
	       "where to write kabi files (default: \"output\")\n"
	       "    -s, --symbols symbol_file:\n\t\t\ta file containing the"
	       " list of symbols of interest (e.g. whitelisted)\n"
	       "    -r, --replace-path abs_path:\n\t\t\t"
	       "replace the absolute path by a relative path\n");
	exit(1);
}

static void parse_generate_opts(int argc, char **argv, generate_config_t *conf,
    char **symbol_file) {
	*symbol_file = NULL;
	conf->verbose = false;
	conf->kabi_dir = DEFAULT_OUTPUT_DIR;
	int opt, opt_index;
	struct option loptions[] = {
		{"help", no_argument, 0, 'h'},
		{"verbose", no_argument, 0, 'v'},
		{"output", required_argument, 0, 'o'},
		{"symbols", required_argument, 0, 's'},
		{"replace-path", required_argument, 0, 'r'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "hvo:s:r:m:",
				  loptions, &opt_index)) != -1) {
		switch (opt) {
		case 'h':
			generate_usage();
		case 'v':
			conf->verbose = true;
			break;
		case 'o':
			conf->kabi_dir = optarg;
			break;
		case 's':
			*symbol_file = optarg;
			break;
		case 'r':
			get_file_replace_path = optarg;
			break;
		default:
			generate_usage();
		}
	}

	if (optind != argc - 1)
		generate_usage();

	conf->kernel_dir = argv[optind];

	rec_mkdir(conf->kabi_dir);
}

static void generate(int argc, char **argv) {
	char *temp_path;
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

	/* Create a place for temporary files */
	safe_asprintf(&temp_path, "%s/%s", conf->kabi_dir, TEMP_PATH);
	rec_mkdir(temp_path);

	generate_symbol_defs(conf);

	/* Delete the temporary space again */
	if (rmdir(temp_path) != 0)
		printf("WARNING: Failed to delete %s: %s\n", temp_path,
		    strerror(errno));

	free(temp_path);

	if (symbol_file != NULL) {
		int i;

		free(conf->symbols_found);
		for (i = 0; i < conf->symbol_cnt; i++)
			free(conf->symbols[i]);
		free(conf->symbols);
	}

	free(conf);
}

int main(int argc, char **argv) {
	int ret = 0;

	progname = argv[0];

	if (argc < 2)
		usage();

	argv++; argc--;

	if (strcmp(argv[0], "generate") == 0) {
		generate(argc, argv);
	} else if (strcmp(argv[0], "compare") == 0) {
		ret = compare(argc, argv);
	} else if (strcmp(argv[0], "show") == 0) {
		ret = show(argc, argv);
	} else {
		usage();
	}

	return ret;
}
