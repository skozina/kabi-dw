#define	_POSIX_C_SOURCE 200809L /* getline() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "main.h"
#include "kabi-dw.h"

char *output_dir = DEFAULT_OUTPUT_DIR;
static char *progname;

void usage(void) {
	printf("Usage:\n\t %s generate -s symbol_file [-o output_dir] "
	    "module_dir\n", progname);
	exit(1);
}

#define	DEFAULT_BUFSIZE	64
#define WHITESPACE	" \t\n"

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
static char **read_symbols(char *filename, size_t *symbol_cnt) {
	FILE *fp = fopen(filename, "r");
	char *line = NULL;
	size_t len = 0;
	size_t result_len = DEFAULT_BUFSIZE;
	char **result = safe_malloc(result_len * sizeof(*result));
	size_t i = 0;

	if (fp == NULL)
		fail("Failed to open symbol file: %s\n", strerror(errno));

	while ((getline(&line, &len, fp)) != -1) {
		if (i == result_len) {
			result_len *= 2;
			result = realloc(result, result_len * sizeof(*result));
		}
		result[i] = line;
		strip(result[i]);
		i++;
		line = NULL;
		len = 0;
	}

	if (errno != 0)
		fail("getline() failed: %s\n", strerror(errno));

	if (line != NULL)
		free(line);

	fclose(fp);

	*symbol_cnt = i;
	return (result);
}

static void parse_generate_opts(int argc, char **argv, char **symbol_file,
    char **module_dir) {
	*symbol_file = NULL;
	*module_dir = NULL;

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
		}
	}

	if (*symbol_file == NULL)
		usage();

	if (argc != 1)
		usage();

	*module_dir = argv[0];
	argc--; argv++;

	if (*module_dir == NULL)
		usage();
}

static void generate(int argc, char **argv) {
	char *module_dir;
	char *symbol_file;
	struct stat dirstat;
	size_t symbol_cnt;
	char **symbol_names;

	parse_generate_opts(argc, argv, &symbol_file, &module_dir);

	if (stat(output_dir, &dirstat) != 0) {
		if (errno == ENOENT) {
			fail("Target directory %s does not exist\n",
			    output_dir);
		} else {
			fail("Failed to stat() directory %s: %s\n",
			    output_dir, strerror(errno));
		}
	}
	if (!S_ISDIR(dirstat.st_mode))
		fail("Not a directory %s\n", output_dir);

	symbol_names = read_symbols(symbol_file, &symbol_cnt);

	generate_symbol_defs(module_dir, symbol_names, symbol_cnt);
}

int main(int argc, char **argv) {
	progname = argv[0];

	if (argc < 2)
		usage();

	argv++; argc--;

	if (strcmp(argv[0], "generate") == 0) {
		argv++; argc--;
		generate(argc, argv);
	} else {
		usage();
	}

	return (0);
}
