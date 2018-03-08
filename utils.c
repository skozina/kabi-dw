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
 * This file contains couple of generally useful functions.
 */

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
#include <libgen.h> /* dirname() */

#include "main.h"
#include "utils.h"

/*
 * Sort function for scandir.
 * Walk regular file first, then process subdirectories.
 */
int reg_first(const struct dirent **a, const struct dirent **b)
{
	if ((*a)->d_type == DT_REG && (*b)->d_type != DT_REG)
		return -1;
	if ((*b)->d_type == DT_REG && (*a)->d_type != DT_REG)
		return 1;

	/*
	 * Backup to default collation
	 * Note: the behavior depends on LC_COLLATE
	 */
	return alphasort(a, b);
}

/*
 * Call cb() on all nodes in the directory structure @path.
 * If list_dirs == true run cb() on subdirectories as well, otherwise list only
 * files.
 * The cb() has to return true if we continue directory walk or false if we're
 * all done.
 */
void walk_dir(char *path, bool list_dirs, walk_rv_t (*cb)(char *, void *),
		void *arg)
{
	struct dirent **entlist;
	walk_rv_t cb_rv = WALK_CONT;
	int entries, i;

	assert(path != NULL && strlen(path) >= 1);

	entries = scandir(path, &entlist, NULL, reg_first);
	if (entries == -1) {
		fail("Failed to scan module directory %s: %s\n", path,
		    strerror(errno));
	}

	/* process all the files and directories within directory */
	for (i = 0; i < entries; i++) {
		struct dirent *ent = entlist[i];
		struct stat entstat;
		char *new_path;

		if ((strcmp(ent->d_name, "..") == 0) ||
		    (strcmp(ent->d_name, ".") == 0)) {
			free(ent);
			continue;
		}

		if (path[strlen(path) - 1] == '/')
			safe_asprintf(&new_path, "%s%s", path, ent->d_name);
		else
			safe_asprintf(&new_path, "%s/%s", path, ent->d_name);

		if (lstat(new_path, &entstat) != 0) {
			fail("Failed to stat directory %s: %s\n", new_path,
			    strerror(errno));
		}

		if (S_ISDIR(entstat.st_mode)) {
			if (list_dirs) {
				cb_rv = cb(new_path, arg);
				if (cb_rv != WALK_CONT)
					goto out;
			}

			/* Ignore symlinks */
			if (!S_ISLNK(entstat.st_mode))
				walk_dir(new_path, list_dirs, cb, arg);
		} else if (S_ISREG(entstat.st_mode)) {
			cb_rv = cb(new_path, arg);
		}

out:
		free(new_path);
		free(ent);

		if (cb_rv == WALK_STOP)
			break;
		if (cb_rv == WALK_SKIP) {
			cb_rv = WALK_CONT;
			break;
		}
	}

	free(entlist);
}

int check_is_directory(char *dir)
{
	struct stat dirstat;

	if (stat(dir, &dirstat) != 0)
		return errno;

	if (!S_ISDIR(dirstat.st_mode))
		return ENOTDIR;

	return 0;
}

static void safe_mkdir(char *path)
{
	if (mkdir(path, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0)
		fail("%s", strerror(errno));
}

void rec_mkdir(char *path)
{
	char *buf;
	char *pos;
	size_t len = strlen(path);

	assert(path != NULL && len > 0);

	buf = safe_strdup(path);

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
			if (rv == ENOENT)
				safe_mkdir(buf);
			else
				fail("%s", strerror(rv));
		}

		if (pos != NULL)
			*pos = '/';
	}

	free(buf);
}

void safe_rename(const char *oldpath, const char *newpath)
{
	char *temp;

	temp = safe_strdup(newpath);
	/* dirname() modifies its buffer! */
	rec_mkdir(dirname(temp));
	free(temp);

	if (rename(oldpath, newpath) != 0)
		fail("rename() failed: %s\n", strerror(errno));
}

struct norm_ctx {
	char *path;
	char *p;
	char *outp;
};

/* actually, second last, skip the whole directory */
static char *last_slash(char *str, char *end)
{
	char c = '/';
	int met = 0;

	for (; end > str; end--) {
		if (*end == c) {
			if (met)
				return end;
			else
				met = 1;
		}
	}
	return NULL;
}

typedef void *(*state_t)(struct norm_ctx *);

static void *initial(struct norm_ctx *ctx);
static void *normal(struct norm_ctx *ctx);
static void *one_dot(struct norm_ctx *ctx);
static void *two_dots(struct norm_ctx *ctx);
static void *slash(struct norm_ctx *ctx);
static void *end(struct norm_ctx *ctx);

static void *initial(struct norm_ctx *ctx)
{
	char c = *ctx->p++;

	switch (c) {
	case '\0':
		*ctx->outp = c;
		return end;
	case '/':
		*ctx->outp++ = c;
		return slash;
	case '.':
		return one_dot;
	default:
		*ctx->outp++ = c;
	}
	return normal;
}

static void *normal(struct norm_ctx *ctx)
{
	char c = *ctx->p++;

	switch (c) {
	case '\0':
		*ctx->outp++ = c;
		return end;
	case '/':
		*ctx->outp++ = c;
		return slash;
	default:
		*ctx->outp++ = c;
	}
	return normal;
}

static void *slash(struct norm_ctx *ctx)
{
	char c = *ctx->p++;

	switch (c) {
	case '\0':
		fail("Cannot normalize path %s", ctx->path);
	case '/':
		return slash;
	case '.':
		return one_dot;
	default:
		*ctx->outp++ = c;
	}
	return normal;
}

static void *one_dot(struct norm_ctx *ctx)
{
	char c = *ctx->p++;

	switch (c) {
	case '\0':
		*--ctx->outp = c;
		return end;
	case '/':
		return slash;
	case '.':
		return two_dots;
	default:
		*ctx->outp++ = '.';
		*ctx->outp++ = c;
	}
	return normal;
}

static void *two_dots(struct norm_ctx *ctx)
{
	char c = *ctx->p++;
	char *p;

	switch (c) {
	case '\0':
		p = last_slash(ctx->path, ctx->outp);
		if (p == NULL)
			p = ctx->path;
		*p = c;
		return end;
	case '/':
		p = last_slash(ctx->path, ctx->outp);
		if (p == NULL) {
			ctx->outp = ctx->path;
			return normal;
		}
		ctx->outp = ++p;
		return slash;
	default:
		*ctx->outp++ = '.';
		*ctx->outp++ = '.';
		*ctx->outp++ = c;
	}
	return normal;
}

static void *end(struct norm_ctx *ctx)
{
	fail("Cannot normalize path %s", ctx->path);
}

char *path_normalize(char *path)
{
	struct norm_ctx ctx = {
		.path = path,
		.p = path,
		.outp = path,
	};
	state_t state = initial;

	while (state != end)
		state = state(&ctx);

	return path;
}

/* Removes the two dashes at the end of the prefix */
#define IS_PREFIX(s, prefix) !strncmp(s, prefix, strlen(prefix) - 2)

static void split_filename(char *filename, char **prefix,
			   char **name, int *version)
{
	char *base = basename(filename);

	version = 0;

	if ((sscanf(base, "%m[a-z]--%m[^.-].txt", prefix, name) != 2) &&
	    (sscanf(base, "%m[a-z]--%m[^.-]-%i.txt",
		    prefix, name, version) != 3))
		fail("Unexpected file name: %s\n", filename);
}

/*
 * Get the type of a symbol from the name of the kabi file
 *
 * It allocates the string which must be freed by the caller.
 */
char *filenametotype(char *filename)
{
	char *prefix = NULL, *name = NULL, *type = NULL;
	int version = 0;

	split_filename(filename, &prefix, &name, &version);

	if (IS_PREFIX(prefix, TYPEDEF_FILE))
		type = name;
	else if (IS_PREFIX(prefix, STRUCT_FILE) ||
		 IS_PREFIX(prefix, UNION_FILE) ||
		 IS_PREFIX(prefix, ENUM_FILE))
		safe_asprintf(&type, "%s %s", prefix, name);
	else
		fail("Unexpected file prefix: %s\n", prefix);

	free(prefix);
	if (name != type)
		free(name);

	return type;
}

/*
 * Get the name of a symbol from the name of the kabi file
 *
 * It allocates the string which must be freed by the caller.
 */
char *filenametosymbol(char *filename)
{
	char *prefix = NULL, *name = NULL;
	int version = 0;

	split_filename(filename, &prefix, &name, &version);
	free(prefix);

	return name;
}
