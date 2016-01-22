/* Copyright (C) 1998-2002, 2004, 2006, 2012, 2015 Red Hat, Inc.
   This file is part of elfutils.
   Written by Ulrich Drepper <drepper@redhat.com>, 1998.

   This file is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   elfutils is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <dwarf.h>
#include <inttypes.h>
#include <libelf.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include <elfutils/libdw.h>
#include <elfutils/known-dwarf.h>

#include "kabi-dw.h"

static const char * dwarf_tag_string (unsigned int tag) {
	switch (tag)
	{
#define DWARF_ONE_KNOWN_DW_TAG(NAME, CODE) case CODE: return #NAME;
		DWARF_ALL_KNOWN_DW_TAG
#undef DWARF_ONE_KNOWN_DW_TAG
		default:
			return NULL;
	}
}

static const char * dwarf_attr_string (unsigned int attrnum) {
	switch (attrnum)
	{
#define DWARF_ONE_KNOWN_DW_AT(NAME, CODE) case CODE: return #NAME;
		DWARF_ALL_KNOWN_DW_AT
#undef DWARF_ONE_KNOWN_DW_AT
		default:
			return NULL;
	}
}

static void print_die_offsets_attrs(Dwarf_Die *die) {
	Dwarf_Off off;
	Dwarf_Off cuoff;
	size_t cnt;
	int i;

	off = dwarf_dieoffset (die);
	cuoff = dwarf_cuoffset (die);

	printf (" Offset    : %lld\n", (long long int) off);
	printf (" CU offset : %lld\n", (long long int) cuoff);

	printf (" Attrs     :");
	for (cnt = 0; cnt < 0xffff; ++cnt)
		if (dwarf_hasattr (die, cnt))
			printf (" %s", dwarf_attr_string (cnt));
	puts ("");

	if (dwarf_hasattr (die, DW_AT_byte_size) &&
	    (i = dwarf_bytesize (die)) != -1)
		printf (" byte size : %d\n", i);
	if (dwarf_hasattr (die, DW_AT_bit_size) &&
	    (i = dwarf_bitsize (die)) != -1)
		printf (" bit size  : %d\n", i);
	if (dwarf_hasattr (die, DW_AT_bit_offset) &&
	    (i = dwarf_bitoffset (die)) != -1)
		printf (" bit offset: %d\n", i);
}

static void print_die_member(Dwarf_Die *die, const char *name) {
	Dwarf_Attribute attr;
	Dwarf_Word value;

	if (dwarf_attr(die, DW_AT_data_member_location, &attr) == NULL)
		fail("Offset of member %s missing!\n", name);

	(void) dwarf_formudata(&attr, &value);
	printf("[0x%lx] %s\n", value, name);
}

static void print_die_structure(Dwarf *dbg, Dwarf_Die *die) {
	const char *name;

	name = dwarf_diename (die);
	printf("struct %s {\n", name);

	if (!dwarf_haschildren(die))
		goto done;
	dwarf_child(die, die);

	do {
		name = dwarf_diename(die);
		print_die_member(die, name);
	} while (dwarf_siblingof(die, die) == 0);

done:
	printf("}\n");
}

static void print_die(Dwarf *dbg, Dwarf_Die *die) {
	unsigned int tag;
	const char *name;

	name = dwarf_diename (die);
	tag = dwarf_tag(die);

	if (tag == DW_TAG_invalid)
		fail("DW_TAG_invalid: %s\n", name);

	switch (tag) {
	case DW_TAG_compile_unit:
		printf("Compilation unit: %s\n", name);
		print_die_offsets_attrs(die);
		break;
	case DW_TAG_structure_type:
		print_die_structure(dbg, die);
		break;
	case DW_TAG_union_type:
		printf("Union: %s\n", name);
		break;
	case DW_TAG_typedef:
		printf("Typedef: %s\n", name);
		break;
	default: {
		const char *tagname = dwarf_tag_string(tag);
		if (tagname == NULL)
			tagname = "<NO TAG>";

		printf("Unexpected tag for symbol %s: %s\n", name, tagname);
		exit(1);
		break;
	}
	}

}

static void process_symbol_die(Dwarf *dbg, Dwarf_Die *die, const char *symbol_name) {
	unsigned int tag;
	const char *name;

	name = dwarf_diename (die);
	/* Did we find our symbol? */
	if (name == 0 || strcmp(name, symbol_name) != 0)
		return;

	tag = dwarf_tag(die);
	if (tag == DW_TAG_invalid)
		fail("DW_TAG_invalid: %s\n", name);

	switch (tag) {
	case DW_TAG_subprogram:
		printf("Function: %s\n", name);
		break;
	case DW_TAG_variable: {
		Dwarf_Die type_die;
		Dwarf_Attribute attr;

		printf("Variable: %s\n", name);

		if (!dwarf_hasattr(die, DW_AT_type))
			fail("Variable missing type attribute: %s\n", name);
		(void) dwarf_attr(die, DW_AT_type, &attr);
		if (dwarf_formref_die(&attr, &type_die) == NULL)
			fail("dwarf_formref_die() failed for %s\n", name);

		print_die(dbg, &type_die);
		break;
	}
	default: {
		const char *tagname = dwarf_tag_string(tag);
		if (tagname == NULL)
			tagname = "<NO TAG>";

		printf("Unexpected tag for symbol %s: %s\n", name, tagname);
		exit(1);
		break;
	}
	}

}

static void process_cu_die(Dwarf *dbg, Dwarf_Die *die,
    const char *symbol_name) {
	/* Print CU DIE */
	process_symbol_die(dbg, die, symbol_name);

	if (!dwarf_haschildren(die))
		return;

	dwarf_child(die, die);
	do {
		process_symbol_die(dbg, die, symbol_name);
	} while (dwarf_siblingof(die, die) == 0);
}

void print_symbol(const char *filepath, const char *symbol_name) {
	int fd = open (filepath, O_RDONLY);
	Dwarf *dbg;

	if (fd < 0) {
		fail("Error opening file: %s (%s)\n", filepath,
		    strerror(errno));
	}

	dbg = dwarf_begin (fd, DWARF_C_READ);
	if (dbg == NULL)
	{
		close (fd);
		fail("Error opening DWARF: %s\n", filepath);
	}

	Dwarf_Off off = 0;
	Dwarf_Off old_off = 0;
	Dwarf_Off type_offset = 0;
	Dwarf_Half version;
	size_t hsize;
	Dwarf_Off abbrev;
	uint8_t addresssize;
	uint8_t offsetsize;
	while (dwarf_next_unit(dbg, off, &off, &hsize, &version, &abbrev,
	    &addresssize, &offsetsize, NULL, &type_offset) == 0)
	{
		if (version < 2 || version > 4) {
			fail("Unsupported dwarf version: %d\n", version);
		}

		Dwarf_Die die;
		if (dwarf_offdie (dbg, old_off + hsize, &die) == NULL) {
			fail("dwarf_offdie failed for cu!\n");
		}

		process_cu_die(dbg, &die, symbol_name);
		old_off = off;
	}

	dwarf_end (dbg);
	close (fd);
}
