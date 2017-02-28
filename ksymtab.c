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

#define	KSYMTAB_STRINGS	"__ksymtab_strings"

void free_ksymtab(char **ksymtab, size_t ksymtab_len) {
	size_t i;

	for (i = 0; i < ksymtab_len; i++) {
		free(ksymtab[i]);
		ksymtab[i] = NULL;
	}

	free(ksymtab);
}

static char **print_section(Elf *elf, Elf_Scn *scn, size_t *symbol_cnt) {
	GElf_Shdr shdr;
	Elf_Data *data;
	char *p, *oldp;
	char **ksymtab;
	size_t ksymtab_len;
	size_t size = 0, i = 0;

	ksymtab_len = DEFAULT_BUFSIZE;
	ksymtab = safe_malloc(ksymtab_len * sizeof (*ksymtab));

	if (gelf_getshdr(scn, &shdr) != &shdr)
		fail("getshdr() failed: %s\n", elf_errmsg(-1));

	data = elf_getdata(scn, NULL);
	if (data == NULL || data->d_size == 0)
		fail(KSYMTAB_STRINGS " section empty!\n");

	p = oldp = (char *)data->d_buf;

	/* Make sure we have the final '\0' */
	if (p[data->d_size - 1] != '\0')
		fail("Mallformed " KSYMTAB_STRINGS " section: %s\n", p);

	for (size = 0; size < data->d_size; size++, p++) {
		/* End of symbol? */
		if (*p == '\0') {
			size_t len = p - oldp;

			/* Skip empty strings */
			if (len == 0) {
				oldp = p + 1;
				continue;
			}

			if (i == ksymtab_len) {
				ksymtab_len *= 2;
				ksymtab = realloc(ksymtab,
				    ksymtab_len * sizeof (*ksymtab));
			}

			ksymtab[i] = safe_malloc(len + 1);
			strncpy(ksymtab[i], oldp, len + 1);
			assert(ksymtab[i][len] == '\0');
			i++;
			oldp = p + 1;
		}
	}

	*symbol_cnt = i;
	return (ksymtab);
}

/* Build list of exported symbols, ie. read seciton __ksymtab_strings */
char **read_ksymtab(char *filename, size_t *ksymtab_len) {
	Elf *elf;
	int fd;
	Elf_Kind ek;
	int class;
	Elf_Scn *scn;
	GElf_Shdr shdr;
	size_t shstrndx;
	char *name;
	char **ksymtab = NULL;
	GElf_Ehdr ehdr;

	*ksymtab_len = 0;

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

	scn = elf_nextscn(elf, NULL);
	for (; scn != NULL; scn = elf_nextscn(elf, scn)) {
		if (gelf_getshdr(scn, &shdr) != &shdr)
			fail("getshdr() failed: %s\n", elf_errmsg(-1));
		if ((name = elf_strptr(elf, shstrndx, shdr.sh_name)) == NULL)
			fail("elf_strptr() failed: %s\n", elf_errmsg(-1));

		if (strcmp(name, KSYMTAB_STRINGS) != 0)
			continue;

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
			printf("The " KSYMTAB_STRINGS " section has type "
			    "SHT_NOBITS. Most likely you're running this "
			    "tool on modules coming from kernel-debuginfo "
			    "packages. They don't contain the " KSYMTAB_STRINGS
			    " section, you need to use the raw modules before "
			    "they are stripped\n");
			exit(1);
		}

		if (shdr.sh_type != SHT_PROGBITS)
			fail("Unexpected type of section %s: %d\n",
			    name, shdr.sh_type);
		ksymtab = print_section(elf, scn, ksymtab_len);
	}

done:
	(void) elf_end(elf);
	(void) close(fd);
	return (ksymtab);
}

/*
 * Return the index of symbol in the array or -1 if the symbol was not found.
 */
int find_symbol(char **symbols, size_t symbol_cnt, const char *name) {
	int i = 0;

	if (name == NULL)
		return (-1);

	for (i = 0; i < symbol_cnt; i++) {
		if (strcmp(symbols[i], name) == 0)
			return (i);
	}

	return (-1);
}
