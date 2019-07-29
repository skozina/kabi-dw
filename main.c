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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "generate.h"
#include "compare.h"
#include "show.h"
#include "utils.h"

static char *progname;

void usage(void)
{
	printf("Usage:\n"
	    "\t %s generate [options] kernel_dir\n"
	    "\t %s show [options] kabi_file...\n"
	    "\t %s compare [options] kabi_dir kabi_dir...\n",
	       progname, progname, progname);
	exit(1);
}

int main(int argc, char **argv)
{
	int ret = 0;

	progname = argv[0];

	if (argc < 2)
		usage();

	argv++; argc--;

	global_string_keeper_init();

	if (strcmp(argv[0], "generate") == 0)
		generate(argc, argv);
	else if (strcmp(argv[0], "compare") == 0)
		ret = compare(argc, argv);
	else if (strcmp(argv[0], "show") == 0)
		ret = show(argc, argv);
	else
		usage();

	global_string_keeper_free();

	return ret;
}
