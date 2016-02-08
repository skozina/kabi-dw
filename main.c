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

void usage(char *progname) {
	printf("Usage:\n\t %s generate [-o output_dir] modules_path\n",
		progname);
	exit(1);
}

static void generate(char *progname, int argc, char **argv) {
	char *module_dir;
	struct stat dirstat;

	if (argc == 2) {
		output_dir = argv[0];
		argc--; argv++;
	}

	if (argc != 1)
		usage(progname);

	module_dir = argv[0];

	if (stat(output_dir, &dirstat) != 0) {
		if (errno == ENOENT) {
			fail("Target directory %s does not exist\n",
			    output_dir);
		} else {
			fail("Failed to stat() directory %s: %s\n",
			    output_dir, strerror(errno));
		}
	}

	char *names[] = {
		"init_task",
		"x86_64_start_kernel",
		"acpi_disabled",
		"acpi_evaluate_integer",
		"acpi_initialize_hp_context",
	};

	print_symbols(module_dir, names, sizeof (names) / sizeof (*names));
}

int main(int argc, char **argv) {
	char *progname = argv[0];

	if (argc < 2)
		usage(progname);

	argv++; argc--;

	if (strcmp(argv[0], "generate") == 0) {
		argv++; argc--;
		generate(progname, argc, argv);
	} else {
		usage(progname);
	}

	return (0);
}
