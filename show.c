/*
	Copyright(C) 2017, Red Hat, Inc.

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

#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include "objects.h"
#include "utils.h"

struct {
	bool debug;
	bool hide_kabi;
	bool hide_kabi_new;
	FILE *file;
} show_config = {false, false, false, NULL};

static void show_usage()
{
	printf("Usage:\n"
	       "\tshow [options] kabi_file...\n"
	       "\nOptions:\n"
	       "    -h, --help:\t\tshow this message\n"
	       "    -k, --hide-kabi:\thide changes made by RH_KABI_REPLACE()\n"
	       "    -n, --hide-kabi-new:\n\t\t\thide the kabi trickery made by"
	       " RH_KABI_REPLACE, but show the new field\n"

	       "    -d, --debug:\tprint the raw tree\n"
	       "    --no-offset:\tdon't display the offset of struct fields\n");
	exit(1);
}

/*
 * Performs the show command
 */
int show(int argc, char **argv)
{
	obj_t *root;
	int opt, opt_index, ret = 0;
	struct option loptions[] = {
		{"debug", no_argument, 0, 'd'},
		{"hide-kabi", no_argument, 0, 'k'},
		{"hide-kabi-new", no_argument, 0, 'n'},
		{"help", no_argument, 0, 'h'},
		{"no-offset", no_argument, &display_options.no_offset, 1},
		{0, 0, 0, 0}
	};

	memset(&display_options, 0, sizeof(display_options));

	while ((opt = getopt_long(argc, argv, "dknh",
				  loptions, &opt_index)) != -1) {
		switch (opt) {
		case 0:
			break;
		case 'd':
			show_config.debug = true;
			break;
		case 'n':
			show_config.hide_kabi_new = true;
			/* fall through */
		case 'k':
			show_config.hide_kabi = true;
			break;
		case 'h':
		default:
			show_usage();
		}
	}

	if (optind >= argc)
		show_usage();

	while (optind < argc) {
		char *fn = argv[optind++];

		show_config.file = safe_fopen(fn);

		root = obj_parse(show_config.file, fn);

		if (show_config.hide_kabi)
			obj_hide_kabi(root, show_config.hide_kabi_new);

		if (show_config.debug)
			obj_debug_tree(root);

		obj_print_tree(root);
		if (optind < argc)
			putchar('\n');

		obj_free(root);
		fclose(show_config.file);
	}

	return ret;
}
