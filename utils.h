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

#ifndef UTILS_H_
#define	UTILS_H_

#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

/*
 * Changes to file format that keep backward compatibility call for
 * incementing the minor number, changes that don't calls for
 * incrementing the major number.
 */
#define _VERSION(s,j) _STR(s,j)
#define _STR(s,j) _STR2(Version: s##.j\n)
#define _STR2(s) #s

#define FILEFMT_VERSION_MAJOR	1
#define FILEFMT_VERSION_MINOR	0
#define FILEFMT_VERSION_STRING	\
	_VERSION(FILEFMT_VERSION_MAJOR,FILEFMT_VERSION_MINOR)

#define	fail(fmt, ...)	{					\
	fprintf(stderr, "%s():%d ", __func__, __LINE__);	\
	fprintf(stderr, fmt, ## __VA_ARGS__);			\
	exit(1);						\
}

static inline void safe_asprintf(char **strp, const char *fmt, ...)
{
	va_list arglist;

	va_start(arglist, fmt);
	if (vasprintf(strp, fmt, arglist) == -1)
		fail("asprintf failed: %s", strerror(errno));
	va_end(arglist);
}

static inline void *safe_zmalloc(size_t size)
{
	void *result = malloc(size);

	if (result == NULL)
		fail("Malloc of size %zu failed", size);
	memset(result, 0, size);
	return result;
}

static inline void *safe_realloc(void *ptr, size_t size)
{
	void *result = realloc(ptr, size);

	if (result == NULL)
		fail("Rellloc of size %zu failed: %s\n", size, strerror(errno));
	return result;
}

static inline void *safe_strdup(const char *s)
{
	void *result = strdup(s);

	if (result == NULL)
		fail("strdup() of \"%s\" failed", s);
	return result;
}

static inline void *safe_strdup_or_null(const char *s)
{
	if (s == NULL)
		return NULL;
	return safe_strdup(s);
}

static inline bool safe_streq(const char *s1, const char *s2)
{
	if ((s1 == NULL) != (s2 == NULL))
		return false;
	if (s1)
		return !strcmp(s1, s2);
	return true;
}

static inline bool safe_strendswith(const char *s1, const char *s2)
{
	int len1, len2;

	if ((s1 == NULL) != (s2 == NULL))
		return false;

	if (!s1)
		return true;

	len1 = strlen(s1);
	len2 = strlen(s2);

	if ((len1 == 0) || (len2 == 0))
		return false;

	if (len2 > len1)
		return false;

	return strcmp(s1 + len1 - len2, s2) == 0;
}

static inline ssize_t safe_getline(char **lineptr, size_t *n, FILE *stream)
{
	ssize_t ret = getline(lineptr, n, stream);

	if ((ret == -1) && (errno != ENOENT))
		fail("getline failed: %s\n", strerror(errno));

	return ret;
}

static inline FILE *safe_fopen(char *filename)
{
	FILE *file;

	file = fopen(filename, "r");
	if (!file)
		fail("Failed to open kABI file: %s\n", filename);

	return file;
}

typedef enum {
	WALK_CONT,
	WALK_STOP,
	WALK_SKIP,
} walk_rv_t;

extern void walk_dir(char *, bool, walk_rv_t (*)(char *, void *), void *);
extern int check_is_directory(char *);
extern void rec_mkdir(char *);
extern void safe_rename(const char *, const char *);
extern char *path_normalize(char *);
extern char *filenametotype(char *);
extern char *filenametosymbol(char *);

#endif /* UTILS_H */
