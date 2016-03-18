#define	_GNU_SOURCE	/* asprintf() */

#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "utils.h"

/*
 * Call cb() on each file in the directory structure @path.
 * The cb() has to return true if we continue directory walk or false if we're
 * all done.
 */
void walk_dir(char *path, bool (*cb)(char *, void *), void *arg) {
	DIR *dir;
	struct dirent *ent;
	bool proceed = true;

	if ((dir = opendir(path)) == NULL) {
		fail("Failed to open module directory %s: %s\n", path,
		    strerror(errno));
	}

	/* print all the files and directories within directory */
	while (proceed && ((ent = readdir(dir)) != NULL)) {
		struct stat entstat;
		char *new_path;

		if ((strcmp(ent->d_name, "..") == 0) ||
		    (strcmp(ent->d_name, ".") == 0))
			continue;

		if (asprintf(&new_path, "%s/%s", path, ent->d_name) == -1)
			fail("asprintf() failed");

		if (lstat(new_path, &entstat) != 0) {
			fail("Failed to stat directory %s: %s\n", new_path,
			    strerror(errno));
		}

		if (S_ISDIR(entstat.st_mode)) {
			/* Ignore symlinks */
			if (!S_ISLNK(entstat.st_mode))
				walk_dir(new_path, cb, arg);
		} else if (S_ISREG(entstat.st_mode)) {
			proceed = cb(new_path, arg);
		}

		free(new_path);
	}

	closedir(dir);
}
