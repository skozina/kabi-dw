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

#ifndef KSYMTAB_H_
#define	KSYMTAB_H_

#include <stdint.h>

struct elf_data {
	Elf *elf;
	GElf_Ehdr *ehdr;
	size_t shstrndx;
	const char *strtab;
	size_t strtab_size;
	int fd;
};

struct ksymtab;
struct ksym {
	uint64_t value;
	bool mark;
	char *link;
	struct ksymtab *ksymtab;
	char key[];
};

static inline bool ksymtab_ksym_is_marked(struct ksym *ksym)
{
	return ksym->mark;
}

static inline const char *ksymtab_ksym_get_name(struct ksym *ksym)
{
	return ksym->key;
}

static inline uint64_t ksymtab_ksym_get_value(struct ksym *ksym)
{
	return ksym->value;
}

static inline char *ksymtab_ksym_get_link(struct ksym *ksym)
{
	return ksym->link;
}

static inline void ksymtab_ksym_set_link(struct ksym *ksym, const char *link)
{
	if (ksym->link)
		free(ksym->link);
	ksym->link = safe_strdup_or_null(link);
}

extern void ksymtab_free(struct ksymtab *);
extern struct elf_data *elf_open(const char *);
extern int elf_get_exported(struct elf_data *, struct ksymtab **,
			    struct ksymtab **);
extern void elf_close(struct elf_data *);
extern int elf_get_endianness(struct elf_data *, unsigned int *);
extern struct ksym *ksymtab_find(struct ksymtab *, const char *);
extern size_t ksymtab_len(struct ksymtab *);
extern struct ksymtab *ksymtab_new(size_t);
extern struct ksym *ksymtab_add_sym(struct ksymtab *,
				    const char *, size_t, uint64_t);
extern struct ksym *ksymtab_copy_sym(struct ksymtab *, struct ksym *);
extern void ksymtab_for_each(struct ksymtab *,
			     void (*f)(struct ksym *, void *),
			     void *);
extern size_t ksymtab_mark_count(struct ksymtab *);
extern void ksymtab_ksym_mark(struct ksym *);

#endif /* KSYMTAB_H_ */
