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
#include <assert.h>

#include "utils.h"

/*
 * Call cb() on each node in the directory structure @path.
 * If list_dirs == true list subdirectories as well, otherwise list only files.
 * The cb() has to return true if we continue directory walk or false if we're
 * all done.
 */
void walk_dir(char *path, bool list_dirs, bool (*cb)(char *, void *),
    void *arg) {
	DIR *dir;
	struct dirent *ent;
	bool proceed = true;

	assert(path != NULL && strlen(path) >= 1);

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

		if (path[strlen(path) - 1] == '/') {
			if (asprintf(&new_path, "%s%s", path, ent->d_name)
			    == -1)
				fail("asprintf() failed");
		} else {
			if (asprintf(&new_path, "%s/%s", path, ent->d_name)
			    == -1)
				fail("asprintf() failed");
		}

		if (lstat(new_path, &entstat) != 0) {
			fail("Failed to stat directory %s: %s\n", new_path,
			    strerror(errno));
		}

		if (S_ISDIR(entstat.st_mode)) {
			/* Ignore symlinks */
			if (!S_ISLNK(entstat.st_mode))
				walk_dir(new_path, list_dirs, cb, arg);
			if (list_dirs)
				proceed = cb(new_path, arg);
		} else if (S_ISREG(entstat.st_mode)) {
			proceed = cb(new_path, arg);
		}

		free(new_path);
	}

	closedir(dir);
}

int check_is_directory(char *dir) {
	struct stat dirstat;

	if (stat(dir, &dirstat) != 0)
		return (errno);

	if (!S_ISDIR(dirstat.st_mode))
		return (ENOTDIR);

	return (0);
}

static void safe_mkdir(char *path) {
	if (mkdir(path, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0)
		fail(strerror(errno));
}

void rec_mkdir(char *path) {
	char *buf;
	char *pos;
	size_t len = strlen(path);

	assert(path != NULL && len > 0);

	buf = safe_malloc(strlen(path) + 1);
	strcpy(buf, path);

	/* Get rid of trailing slashes */
	for (pos = buf + len - 1; pos > buf && *pos == '/'; --pos)
		*pos = '\0';

	pos = buf;
	while (pos != NULL) {
		int rv;
		char *next;

		/* Skip multiple slashes */
		for (next = pos + 1; *next == '/'; next++)
			;

		pos = strchr(next, '/');
		if (pos != NULL)
			*pos = '\0';
		rv = check_is_directory(buf);
		if (rv != 0) {
			if (rv == ENOENT) {
				safe_mkdir(buf);
			} else {
				fail(strerror(rv));
			}
		}

		if (pos != NULL)
			*pos = '/';
	}

	free(buf);
}
