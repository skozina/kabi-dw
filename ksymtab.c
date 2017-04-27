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
#include "ksymtab.h"

#define	KSYMTAB_STRINGS	"__ksymtab_strings"
#define SYMTAB		".symtab"
#define STRTAB		".strtab"

#define KSYMTAB_SIZE 8192

struct ksymtab_elf {
	Elf *elf;
	size_t shstrndx;
	const char *strtab;
	size_t strtab_size;
	int fd;
};

struct ksymtab {
	struct hash *hash;
	size_t mark_count;
};

struct ksym;

static int ksymtab_elf_get_section(struct ksymtab_elf *ke,
				   const char *section,
				   const char **d_data,
				   size_t *size)
{
	Elf *elf = ke->elf;
	size_t shstrndx = ke->shstrndx;
	Elf_Scn *scn;
	GElf_Shdr shdr;
	char *name;
	Elf_Data *data;

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

	if (gelf_getshdr(scn, &shdr) != &shdr)
		fail("getshdr() failed: %s\n", elf_errmsg(-1));

	data = elf_getdata(scn, NULL);
	if (data == NULL || data->d_size == 0)
		fail("%s section empty!\n", section);

	*d_data = data->d_buf;
	*size = data->d_size;

	return 0;
}

static struct ksymtab_elf *ksymtab_elf_open(const char *filename)
{
	Elf *elf;
	int fd;
	int class;
	GElf_Ehdr ehdr;
	size_t shstrndx;
	const char *strtab;
	size_t strtab_size;
	struct ksymtab_elf *ke = NULL;

	if (elf_version(EV_CURRENT) == EV_NONE)
		fail("elf_version() failed: %s\n", elf_errmsg(-1));

	if ((fd = open(filename, O_RDONLY, 0)) < 0)
		fail("Failed to open file %s: %s\n", filename,
		    strerror(errno));

	if ((elf = elf_begin(fd, ELF_C_READ, NULL)) == NULL)
		fail("elf_begin() failed: %s\n", elf_errmsg(-1));

	if (elf_kind(elf) != ELF_K_ELF)
		goto done;

	if (gelf_getehdr(elf, &ehdr) == NULL)
		fail("getehdr () failed: %s\n", elf_errmsg(-1));

	/* Check elf header */
	if (memcmp(&ehdr.e_ident, ELFMAG, SELFMAG) != 0)
		goto done;

	class = gelf_getclass(elf);
	if (class != ELFCLASS64)
		fail("Unsupported elf class: %d\n", class);

	if (elf_getshdrstrndx(elf, &shstrndx) != 0)
		fail("elf_getshdrstrndx() failed: %s\n", elf_errmsg(-1));

	ke = safe_malloc(sizeof(*ke));
	ke->elf = elf;
	ke->fd = fd;
	ke->shstrndx = shstrndx;

	ksymtab_elf_get_section(ke, STRTAB, &strtab, &strtab_size);

	ke->strtab = strtab;
	ke->strtab_size = strtab_size;
done:
	return ke;
}

static void ksymtab_elf_close(struct ksymtab_elf *ke)
{
	(void) elf_end(ke->elf);
	(void) close(ke->fd);
	free(ke);
}

static void ksymtab_elf_for_each_global_sym(struct ksymtab_elf *ke,
					    void (*fn)(const char *name,
						       uint64_t value,
						       int binding,
						       void *ctx),
					    void *ctx)
{
	const Elf64_Sym *end;
	Elf64_Sym *sym;
	int binding;
	const char *name;
	const char *data;
	size_t size;

	ksymtab_elf_get_section(ke, SYMTAB, &data, &size);

	sym = (Elf64_Sym *)data;
	end = (Elf64_Sym *)(data + size);

	sym++; /* skip first zero record */
	for (; sym < end; sym++) {

		binding = ELF64_ST_BIND(sym->st_info);
		if (! (binding == STB_GLOBAL ||
		       binding == STB_WEAK))
			continue;

		if (sym->st_name == 0)
			continue;

		if (sym->st_name > ke->strtab_size)
			fail("Symbol name index out of range\n");

		name = ke->strtab + sym->st_name;
		if (name == NULL)
			fail("Could not find symbol name\n");

		fn(name, sym->st_value, binding, ctx);
	}
}

void ksymtab_ksym_mark(struct ksym *ksym)
{
	if (!ksym->mark)
		ksym->ksymtab->mark_count++;
	ksym->mark = true;
}

static inline void ksymtab_ksym_set_link(struct ksym *ksym, const char *link)
{
	if (ksym->link)
		free(ksym->link);
	ksym->link = safe_strdup_or_null(link);
}

static void ksymtab_ksym_free(void *arg)
{
	struct ksym *ksym = arg;

	free(ksym->link);
	free(ksym);
}

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

	h = hash_new(size, ksymtab_ksym_free);
	assert(h != NULL);

	ksymtab = safe_malloc(sizeof(*ksymtab));
	ksymtab->hash = h;
	/* ksymtab->mark_count is zeroed by the allocator */

	return ksymtab;
}

struct ksym *ksymtab_add_sym(struct ksymtab *ksymtab,
			     const char *str,
			     size_t len,
			     uint64_t value)
{
	struct hash *h = ksymtab->hash;
	struct ksym *ksym;

	ksym = safe_malloc(sizeof(*ksym) + len + 1);
	memcpy(ksym->key, str, len);
	ksym->key[len] = '\0';
	ksym->value = value;
	ksym->ksymtab = ksymtab;
	hash_add(h, ksym->key, ksym);

	return ksym;
}

struct ksym *ksymtab_copy_sym(struct ksymtab *ksymtab, struct ksym *ksym)
{
	const char *name = ksymtab_ksym_get_name(ksym);
	uint64_t value = ksymtab_ksym_get_value(ksym);
	char *link = ksymtab_ksym_get_link(ksym);
	struct ksym *new;

	new = ksymtab_add_sym(ksymtab, name, strlen(name), value);
	ksymtab_ksym_set_link(new, link);

	return new;
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

void ksymtab_for_each(struct ksymtab *ksymtab,
		      void (*f)(struct ksym *, void *),
		      void *ctx)
{
	struct hash *h;
	struct hash_iter iter;
	const void *v;
	struct ksym *vv;

	if (ksymtab == NULL)
		return;

	h = ksymtab->hash;

	hash_iter_init(h, &iter);
        while (hash_iter_next(&iter, NULL, &v)) {
		vv = (struct ksym *)v;
		f(vv, ctx);
	}
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

/*
 * An entry for address -> symbol mapping.
 * The key will be "value".
 * We can use name pointer directly from the elf,
 * it will be freed later.
 */
struct map_entry {
	uint64_t value;
	const char *name;
};

static struct map_entry *map_entry_new(uint64_t value, const char *name)
{
	struct map_entry *res;

	res = safe_malloc(sizeof(*res));
	res->value = value;
	res->name = name;

	return res;
}

struct weak_filter_ctx {
	struct ksymtab *ksymtab;
	struct ksymtab *weaks;
	struct hash *map;
};

/*
 * Does two things in one pass on the symbol table:
 * 1) makes address -> symbol map for GLOBAL symbols;
 * 2) collecs subset of EXPORTed symbol, which have WEAK binding.
 */
static void weak_filter(const char *name, uint64_t value, int bind, void *_ctx)
{
	struct weak_filter_ctx *ctx = _ctx;
	struct map_entry *m;
	struct ksym *ksym;

	if (bind == STB_GLOBAL) {
		m = map_entry_new(value, name);
		hash_add_bin(ctx->map,
			     (const char *)&m->value, sizeof(m->value), m);
		return;
	}

	/* WEAK handling */

	ksym = ksymtab_find(ctx->ksymtab, name);
	if (ksym == NULL)
		/* skip non-exported aliases */
		return;

	ksymtab_add_sym(ctx->weaks, name, strlen(name), value);
}

struct weak_to_alias_ctx {
	struct ksymtab *aliases;
	struct hash *map;
};

static void weak_to_alias(struct ksym *ksym, void *_ctx)
{
	struct weak_to_alias_ctx *ctx = _ctx;
	struct map_entry *m;
	uint64_t value = ksymtab_ksym_get_value(ksym);
	const char *name = ksymtab_ksym_get_name(ksym);
	struct ksym *alias;

	m = hash_find_bin(ctx->map, (const char *)&value, sizeof(value));
	if (m == NULL)
		/* there is no GLOBAL alias for the WEAK exported symbol */
		return;

	alias = ksymtab_add_sym(ctx->aliases, m->name, strlen(m->name), 0);
	ksymtab_ksym_set_link(alias, name);
}

static struct ksymtab *ksymtab_weaks_to_aliases(struct ksymtab *weaks,
						struct hash *map)
{
	struct ksymtab *aliases;
	struct weak_to_alias_ctx ctx;

	aliases = ksymtab_new(KSYMTAB_SIZE);
	if (aliases == NULL)
		fail("Cannot create ksymtab\n");

	ctx.aliases = aliases;
	ctx.map = map;

	ksymtab_for_each(weaks, weak_to_alias, &ctx);

	return aliases;
}

/*
 * Generate weak aliases for the symbols, found in the list of exported.
 * It will work correctly for one alias only.
 */
static struct ksymtab *ksymtab_find_aliases(struct ksymtab *ksymtab,
					    struct ksymtab_elf *elf)
{
	struct ksymtab *aliases;
	struct ksymtab *weaks;
	struct hash *map; /* address to name mapping */
	struct weak_filter_ctx ctx;

	weaks = ksymtab_new(KSYMTAB_SIZE);
	if (weaks == NULL)
		fail("Cannot create weaks symtab\n");

	map = hash_new(KSYMTAB_SIZE, free);
	if (map == NULL)
		fail("Cannot create address->symbol mapping hash\n");

	ctx.ksymtab = ksymtab;
	ctx.weaks = weaks;
	ctx.map = map;
        /*
	 * If there's a weak symbol on the whitelist,
	 * we need to find the proper global
	 * symbol to generate the type for it.
	 *
	 * It is done in two steps below:
	 * 1) create address -> global symbol mapping and
	 *    suitable weak symbol list;
	 * 2) for all weak symbols find its alias with the mapping.
	 */
	ksymtab_elf_for_each_global_sym(elf, weak_filter, &ctx);
	aliases = ksymtab_weaks_to_aliases(weaks, map);

	hash_free(map);
	ksymtab_free(weaks);

	return aliases;
}

/*
 * Build list of exported symbols, ie. read seciton __ksymtab_strings,
 * analyze symbol table and create table of aliases -- list of global symbols,
 * which have the same addresses, as weak symbols,
 * mentioned by __ksymtab_strings
 */
struct ksymtab *ksymtab_read(char *filename, struct ksymtab **aliases)
{
	struct ksymtab_elf *elf;
	const char *data;
	size_t size;
	struct ksymtab *res = NULL;

	assert(aliases != NULL);

	elf = ksymtab_elf_open(filename);
	if (elf == NULL)
		return NULL;

	if (ksymtab_elf_get_section(elf, KSYMTAB_STRINGS, &data, &size) < 0)
		goto done;

	res = parse_ksymtab_strings(data, size);
	*aliases = ksymtab_find_aliases(res, elf);

done:
	ksymtab_elf_close(elf);
	return (res);
}
