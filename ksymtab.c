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
 * This file contains the code which reads the __ksymtab section of the kernel
 * binaries to ensure that the symbol we parse is actually exported using the
 * EXPORT_SYMBOL() macro.
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <libelf.h>
#include <gelf.h>
#include "main.h"
#include "utils.h"
#include "hash.h"

#define	KSYMTAB_STRINGS	"__ksymtab_strings"

#define KSYMTAB_SIZE 8192

struct ksymtab {
	struct hash *hash;
	size_t mark_count;
};

struct ksym {
	size_t idx;
	bool mark;
	struct ksymtab *ksymtab;
	char key[];
};

void ksymtab_free(struct ksymtab *ksymtab)
{
	struct hash *h;

	if (ksymtab == NULL)
		return;

	h = ksymtab->hash;

	hash_free(h);
	free(ksymtab);
}

struct ksymtab *ksymtab_new(size_t size)
{
	struct hash *h;
	struct ksymtab *ksymtab;

	h = hash_new(size, free);
	assert(h != NULL);

	ksymtab = safe_malloc(sizeof(*ksymtab));
	ksymtab->hash = h;
	/* ksymtab->mark_count is zeroed by the allocator */

	return ksymtab;
}

void ksymtab_add_sym(struct ksymtab *ksymtab,
		     const char *str,
		     size_t len,
		     size_t idx)
{
	struct hash *h = ksymtab->hash;
	struct ksym *ksym;

	ksym = safe_malloc(sizeof(*ksym) + len + 1);
	memcpy(ksym->key, str, len);
	ksym->key[len] = '\0';
	ksym->idx = idx;
	ksym->ksymtab = ksymtab;
	hash_add(h, ksym->key, ksym);
}

/* Parses raw content of  __ksymtab_strings section to a ksymtab */
static struct ksymtab *parse_ksymtab_strings(const char *d_buf, size_t d_size)
{
	char *p, *oldp;
	size_t size = 0;
	size_t i = 0;
	struct ksymtab *res;

	res = ksymtab_new(KSYMTAB_SIZE);

	p = oldp = (char *)d_buf;

	/* Make sure we have the final '\0' */
	if (p[d_size - 1] != '\0')
		fail("Mallformed " KSYMTAB_STRINGS " section: %s\n", p);

	for (size = 0; size < d_size; size++, p++) {
		/* End of symbol? */
		if (*p == '\0') {
			size_t len = p - oldp;

			/* Skip empty strings */
			if (len == 0) {
				oldp = p + 1;
				continue;
			}

			ksymtab_add_sym(res, oldp, len, i);
			i++;
			oldp = p + 1;
		}
	}

	return (res);
}

static struct ksymtab *print_section(Elf *elf, Elf_Scn *scn) {
	GElf_Shdr shdr;
	Elf_Data *data;

	if (gelf_getshdr(scn, &shdr) != &shdr)
		fail("getshdr() failed: %s\n", elf_errmsg(-1));

	data = elf_getdata(scn, NULL);
	if (data == NULL || data->d_size == 0)
		fail(KSYMTAB_STRINGS " section empty!\n");

	return parse_ksymtab_strings(data->d_buf, data->d_size);
}

static int ksymtab_elf_open(char *filename,
			    Elf **elf_out,
			    int *fd_out,
			    size_t *shstrndx_out)
{
	Elf *elf;
	int fd;
	Elf_Kind ek;
	int class;
	GElf_Ehdr ehdr;
	size_t shstrndx;
	int ret = -1;

	if (elf_version(EV_CURRENT) == EV_NONE)
		fail("elf_version() failed: %s\n", elf_errmsg(-1));

	if ((fd = open(filename, O_RDONLY, 0)) < 0)
		fail("Failed to open file %s: %s\n", filename,
		    strerror(errno));

	if ((elf = elf_begin(fd, ELF_C_READ, NULL)) == NULL)
		fail("elf_begin() failed: %s\n", elf_errmsg(-1));

	if ((ek = elf_kind(elf) != ELF_K_ELF))
		goto done;

	if (gelf_getehdr(elf, &ehdr) == NULL)
		fail("getehdr () failed: %s\n", elf_errmsg(-1));

	/* Check elf header */
	if (memcmp(&ehdr.e_ident, ELFMAG, SELFMAG) != 0)
		goto done;

	class = gelf_getclass(elf);
	if (class != ELFCLASS64 && class != ELFCLASS32)
		fail("Unsupported elf class: %d\n", class);

	if (elf_getshdrstrndx(elf, &shstrndx) != 0)
		fail("elf_getshdrstrndx() failed: %s\n", elf_errmsg(-1));

	*elf_out = elf;
	*fd_out = fd;
	*shstrndx_out = shstrndx;

	ret = 0;
done:
	return ret;
}

static void ksymtab_elf_close(Elf *elf, int fd)
{
	(void) elf_end(elf);
	(void) close(fd);
}

static int ksymtab_elf_get_section(Elf *elf,
				   size_t shstrndx,
				   const char *section,
				   Elf_Scn **scn_out)
{
	Elf_Scn *scn;
	GElf_Shdr shdr;
	char *name;

	scn = elf_nextscn(elf, NULL);
	for (; scn != NULL; scn = elf_nextscn(elf, scn)) {
		if (gelf_getshdr(scn, &shdr) != &shdr)
			fail("getshdr() failed: %s\n", elf_errmsg(-1));
		if ((name = elf_strptr(elf, shstrndx, shdr.sh_name)) == NULL)
			fail("elf_strptr() failed: %s\n", elf_errmsg(-1));

		if (strcmp(name, section) == 0)
			break;
	}

	if (scn == NULL) /* no suitable section */
		return -1;

	/*
	 * This is stupid. Fedora/EL builds -debuginfo packages
	 * by running eu-strip --reloc-debug-sections
	 * which places only standard .debug* sections into the
	 * -debuginfo modules. The sections which cannot be stripped
	 * completely (because they are allocated) are changed to
	 * SHT_NOBITS type to indicate you need to look in the original
	 * (non-debug) module for them. But those are xzipped.
	 * So we reject such stuff. We only support fresh output from
	 * the kernel build.
	 */
	if (shdr.sh_type == SHT_NOBITS) {
		printf("The %s section has type "
		       "SHT_NOBITS. Most likely you're running this "
		       "tool on modules coming from kernel-debuginfo "
		       "packages. They don't contain the %s"
		       " section, you need to use the raw modules before "
		       "they are stripped\n", section, section);
		exit(1);
	}

	if (shdr.sh_type != SHT_PROGBITS)
		fail("Unexpected type of section %s: %d\n",
		     name, shdr.sh_type);

	*scn_out = scn;

	return 0;
}

/* Build list of exported symbols, ie. read seciton __ksymtab_strings */
struct ksymtab *ksymtab_read(char *filename) {
	Elf *elf = NULL;
	int fd = 0;
	Elf_Scn *scn = NULL;
	size_t shstrndx;
	struct ksymtab *res = NULL;
	int rc;

	rc = ksymtab_elf_open(filename, &elf, &fd, &shstrndx);
	if (rc < 0)
		goto done;

	rc = ksymtab_elf_get_section(elf, shstrndx, KSYMTAB_STRINGS, &scn);
	if (rc < 0)
		goto done;

	res = print_section(elf, scn);
done:

	ksymtab_elf_close(elf, fd);
	return (res);
}

/*
 * Return the index of symbol in the array or -1 if the symbol was not found.
 */
struct ksym *ksymtab_find(struct ksymtab *ksymtab, const char *name) {
	struct ksym *v;
	struct hash *h = ksymtab->hash;

	if (name == NULL)
		return NULL;

	v = hash_find(h, name);
	if (v == NULL)
		return NULL;

	return v;
}

size_t ksymtab_len(struct ksymtab *ksymtab)
{
	struct hash *h;

	if (ksymtab == NULL)
		return 0;

	h = ksymtab->hash;
	return hash_get_count(h);
}

size_t ksymtab_mark_count(struct ksymtab *ksymtab)
{
	return ksymtab->mark_count;
}

void ksymtab_for_each_unmarked(struct ksymtab *ksymtab,
			       void (*f)(const char *, size_t, void *),
			       void *ctx)
{
	struct hash *h;
	struct hash_iter iter;
	const void *v;
	const struct ksym *vv;

	if (ksymtab == NULL)
		return;

	h = ksymtab->hash;

	hash_iter_init(h, &iter);
        while (hash_iter_next(&iter, NULL, &v)) {
		vv = (const struct ksym *)v;
		if (! vv->mark)
			f(vv->key, vv->idx, ctx);
	}
}

void ksymtab_ksym_mark(struct ksym *ksym)
{
	if (!ksym->mark)
		ksym->ksymtab->mark_count++;
	ksym->mark = true;
}
