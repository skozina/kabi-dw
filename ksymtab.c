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

#define KSYMTAB "__ksymtab"
#define KSYMTAB_GPL "__ksymtab_gpl"
#define	KSYMTAB_STRINGS	 "__ksymtab_strings"
#define SYMTAB		 ".symtab"
#define STRTAB		 ".strtab"
#define STRTAB_NS_PREFIX "__kstrtabns_"
#define KSYMTAB_PREFIX "__ksymtab_"

#define KSYMTAB_SIZE 8192

struct ksymtab {
	struct hash *hash;
	size_t mark_count;
	Elf64_Addr addr;
};

struct ksym;

static Elf64_Addr elf_get_section(Elf *elf,
				  size_t shstrndx,
				  const char *section,
				  const char **d_data,
				  size_t *size)
{
	Elf_Scn *scn;
	GElf_Shdr shdr;
	char *name;
	Elf_Data *data;

	scn = elf_nextscn(elf, NULL);
	for (; scn != NULL; scn = elf_nextscn(elf, scn)) {
		if (gelf_getshdr(scn, &shdr) != &shdr)
			fail("getshdr() failed: %s\n", elf_errmsg(-1));
		name = elf_strptr(elf, shstrndx, shdr.sh_name);
		if (name == NULL)
			fail("elf_strptr() failed: %s\n", elf_errmsg(-1));

		if (strcmp(name, section) == 0)
			break;
	}

	if (scn == NULL) /* no suitable section */
		return -1;

	/*
	 * This is unlucky. Fedora/EL builds -debuginfo packages by running
	 * eu-strip --reloc-debug-sections which places only standard .debug*
	 * sections into the -debuginfo modules. The sections which cannot
	 * be stripped completely (because they are allocated) are changed
	 * to SHT_NOBITS type to indicate you need to look in the original
	 * (non-debug) module for them. But those are xzipped.
	 * So we reject such stuff. We only support fresh output from the
	 * kernel build.
	 */
	if (shdr.sh_type == SHT_NOBITS) {
		printf("The %s section has type SHT_NOBITS. Most likely you're "
		    "running this tool on modules coming from kernel-debuginfo "
		    "packages. They don't contain the %s section, you need to "
		    "use the raw modules before they are stripped\n", section,
		    section);
		exit(1);
	}

	if (gelf_getshdr(scn, &shdr) != &shdr)
		fail("getshdr() failed: %s\n", elf_errmsg(-1));

	data = elf_getdata(scn, NULL);
	if (data == NULL || data->d_size == 0)
		fail("%s section empty!\n", section);

	*d_data = data->d_buf;
	*size = data->d_size;

	return shdr.sh_addr;
}

struct elf_data *elf_open(const char *filename)
{
	Elf *elf;
	int fd;
	int class;
	GElf_Ehdr *ehdr;
	size_t shstrndx;
	struct elf_data *data = NULL;

	if (elf_version(EV_CURRENT) == EV_NONE)
		fail("elf_version() failed: %s\n", elf_errmsg(-1));

	fd = open(filename, O_RDONLY, 0);
	if (fd < 0)
		fail("Failed to open file %s: %s\n", filename,
		     strerror(errno));

	elf = elf_begin(fd, ELF_C_READ, NULL);
	if (elf == NULL)
		fail("elf_begin() failed: %s\n", elf_errmsg(-1));

	if (elf_kind(elf) != ELF_K_ELF) {
		printf("Doesn't look like an ELF file, ignoring: %s\n",
		       filename);
		(void) elf_end(elf);
		(void) close(fd);
		goto out;
	}

	ehdr = safe_zmalloc(sizeof(*ehdr));

	if (gelf_getehdr(elf, ehdr) == NULL)
		fail("getehdr() failed: %s\n", elf_errmsg(-1));

	class = gelf_getclass(elf);
	if (class != ELFCLASS64) {
		printf("Unsupported elf class of %s: %d\n", filename, class);
		free(ehdr);
		(void) elf_end(elf);
		(void) close(fd);
		goto out;
	}

	/*
	 * Get section index of the string table associated with the section
	 * headers in the ELF file.
	 * Required by elf_get_section calls.
	 */
	if (elf_getshdrstrndx(elf, &shstrndx) != 0)
		fail("elf_getshdrstrndx() failed: %s\n", elf_errmsg(-1))

	data = safe_zmalloc(sizeof(*data));

	data->fd = fd;
	data->elf = elf;
	data->ehdr = ehdr;
	data->shstrndx = shstrndx;
out:
	return data;
}

void elf_close(struct elf_data *ed)
{
	if (ed == NULL)
		return;
	(void) elf_end(ed->elf);
	(void) close(ed->fd);
}

static inline bool elf_iter_global_weak(int st_bind)
{
	return st_bind == STB_GLOBAL || st_bind == STB_WEAK;
}

static inline bool elf_iter_local(int st_bind)
{
	return st_bind == STB_LOCAL;
}

static void elf_for_each_sym(struct elf_data *ed,
				    void (*fn)(const char *name,
					       uint64_t value,
					       int binding,
					       void *ctx),
				    void *ctx,
				    bool (*binding_match)(int))
{
	const Elf64_Sym *end;
	Elf64_Sym *sym;
	int binding;
	const char *name;
	const char *data;
	size_t size;

	if (elf_get_section(ed->elf, ed->shstrndx, SYMTAB, &data, &size) == -1)
		return;

	sym = (Elf64_Sym *)data;
	end = (Elf64_Sym *)(data + size);

	sym++; /* skip first zero record */
	for (; sym < end; sym++) {

		binding = ELF64_ST_BIND(sym->st_info);

		if (!binding_match(binding))
			continue;

		if (sym->st_name == 0)
			continue;

		if (sym->st_name > ed->strtab_size)
			fail("Symbol name index %d out of range %ld\n",
			    sym->st_name, ed->strtab_size);

		name = ed->strtab + sym->st_name;
		if (name == NULL)
			fail("Could not find symbol name\n");

		fn(name, sym->st_value, binding, ctx);
	}
}

static inline void elf_for_each_global_sym(struct elf_data *ed,
				    void (*fn)(const char *name,
					       uint64_t value,
					       int binding,
					       void *ctx),
				    void *ctx)
{
	return elf_for_each_sym(ed, fn, ctx, elf_iter_global_weak);
}

void ksymtab_ksym_mark(struct ksym *ksym)
{
	if (!ksym->mark)
		ksym->ksymtab->mark_count++;
	ksym->mark = true;
}

static void ksymtab_ksym_free(void *arg)
{
	struct ksym *ksym = arg;

	free(ksym->link);
	free(ksym->ns);
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

	ksymtab = safe_zmalloc(sizeof(*ksymtab));
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

	ksym = safe_zmalloc(sizeof(*ksym) + len + 1);
	memcpy(ksym->key, str, len);
	ksym->key[len] = '\0';
	ksym->value = value;
	ksym->ksymtab = ksymtab;
	ksym->ns = NULL;
	/* ksym->link is zeroed by the allocator */
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

struct ksym *ksymtab_find(struct ksymtab *ksymtab, const char *name)
{
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

struct ksymtab_section
{
	size_t size;
	Elf64_Addr addr;
};

struct ksymtab_symbol_filter_ctx
{
	struct ksymtab *ksymtab;
	struct ksymtab_section sec;
	struct ksymtab_section sec_gpl;
};

static int ksymtab_in_section(Elf64_Addr addr, struct ksymtab_section *sec)
{
	return sec->addr != -1 &&
		addr >= sec->addr &&
		addr < sec->addr + sec->size;
}

static void ksymtab_symbol_filter(const char *name, uint64_t value, int bind, void *_ctx)
{
	struct ksymtab_symbol_filter_ctx *ctx = (struct ksymtab_symbol_filter_ctx *)_ctx;
	static size_t i = 0;

	if (strncmp(name, KSYMTAB_PREFIX, strlen(KSYMTAB_PREFIX)))
		return;

	if (!ksymtab_in_section(value, &ctx->sec) &&
	    !ksymtab_in_section(value, &ctx->sec_gpl))
		return;

	name += strlen(KSYMTAB_PREFIX);
	ksymtab_add_sym(ctx->ksymtab, name, strlen(name), i++);
}

static struct ksymtab *parse_ksymtab_symbols(struct elf_data *data)
{
	struct ksymtab_symbol_filter_ctx ctx;
	const char *unused;

	ctx.ksymtab = ksymtab_new(KSYMTAB_SIZE);

	ctx.sec.addr = elf_get_section(data->elf,
				       data->shstrndx,
				       KSYMTAB,
				       &unused,
				       &ctx.sec.size);

	ctx.sec_gpl.addr = elf_get_section(data->elf,
					   data->shstrndx,
					   KSYMTAB_GPL,
					   &unused,
					   &ctx.sec_gpl.size);

	if (ctx.sec.addr != -1 || ctx.sec_gpl.addr != -1)
		elf_for_each_sym(data,
				 ksymtab_symbol_filter,
				 (void *) &ctx,
				 elf_iter_local);

	return ctx.ksymtab;
}

struct ns_filter_ctx {
	const char *ksymtab_strings;
	struct ksymtab *ksymtab;
	Elf64_Half e_type;
	Elf64_Addr sh_addr;
};

static void ns_filter(const char *name, uint64_t value, int bind, void *_ctx)
{
	char *ns;
	struct ksym *ksym;
	struct ns_filter_ctx *ctx = (struct ns_filter_ctx*) _ctx;

	if (strncmp(name, STRTAB_NS_PREFIX, strlen(STRTAB_NS_PREFIX)))
		return;

	name += strlen(STRTAB_NS_PREFIX);
	ns = (char *) ctx->ksymtab_strings;
	ns += (ctx->e_type == ET_EXEC) ? value - ctx->sh_addr : value;

	if (!strlen(ns))
		return;

	if (!(ksym = hash_find(ctx->ksymtab->hash, name)))
		return;

	safe_asprintf(&ksym->ns, "%s", ns);

	if (!(ksym = hash_find(ctx->ksymtab->hash, ns)))
		return;
	ksymtab_ksym_mark(ksym);
}

static void ksymtab_fill_ns(const char *ksymtab_strings, struct ksymtab *ksymtab,
			    struct elf_data *elf)
{
	struct ns_filter_ctx ctx = {
		.ksymtab = ksymtab,
		.ksymtab_strings = ksymtab_strings,
		.e_type = elf->ehdr->e_type,
		.sh_addr = ksymtab->addr,
	};

	elf_for_each_sym(elf, ns_filter, (void*) &ctx, elf_iter_local);
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

	res = safe_zmalloc(sizeof(*res));
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
					    struct elf_data *elf)
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
	 * If there's a weak symbol on the stablelist,
	 * we need to find the proper global
	 * symbol to generate the type for it.
	 *
	 * It is done in two steps below:
	 * 1) create address -> global symbol mapping and
	 *    suitable weak symbol list;
	 * 2) for all weak symbols find its alias with the mapping.
	 */
	elf_for_each_global_sym(elf, weak_filter, &ctx);
	aliases = ksymtab_weaks_to_aliases(weaks, map);

	hash_free(map);
	ksymtab_free(weaks);

	return aliases;
}

int elf_get_endianness(struct elf_data *data, unsigned int *endianness)
{
	if (data->ehdr->e_ident[EI_DATA] != ELFDATA2LSB &&
	     data->ehdr->e_ident[EI_DATA] != ELFDATA2MSB) {
		printf("Unsupported ELF endianness (EI_DATA) found: %d.\n",
		     data->ehdr->e_ident[EI_DATA]);
		return 1;
	}

	*endianness = data->ehdr->e_ident[EI_DATA];
	return 0;
}

static inline int elf_get_strtab(struct elf_data *data)
{
	const char *strtab;
	size_t strtab_size;

	if (elf_get_section(data->elf, data->shstrndx, STRTAB, &strtab,
			   &strtab_size) == -1) {
		return 1;
	}

	data->strtab = strtab;
	data->strtab_size = strtab_size;

	return 0;
}

/*
 * Build list of exported symbols, i.e. searching the symbol table for
 * the symbols whose prefix is string "__ksymtab_", and create table
 * of aliases -- list of global symbols, which have the same
 * addresses, as weak symbols.
 */
int elf_get_exported(struct elf_data *data, struct ksymtab **ksymtab,
		     struct ksymtab **aliases)
{
	Elf64_Addr addr;
	const char *ksymtab_strings;
	size_t ksymtab_strings_sz;

	if (elf_get_strtab(data) > 0)
		return 1;

	addr = elf_get_section(data->elf, data->shstrndx, KSYMTAB_STRINGS,
			       &ksymtab_strings, &ksymtab_strings_sz);
	if (addr == -1)
		return 1;

	*ksymtab = parse_ksymtab_symbols(data);
	(*ksymtab)->addr = addr;
	*aliases = ksymtab_find_aliases(*ksymtab, data);
	ksymtab_fill_ns(ksymtab_strings, *ksymtab, data);

	return 0;
}



