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
#include <stdbool.h>

#include <elfutils/libdw.h>
#include <elfutils/known-dwarf.h>

#include "kabi-dw.h"

static void print_die(Dwarf *, Dwarf_Die *);

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

/* Check if given DIE has DW_AT_external attribute */
static bool is_external(Dwarf_Die *die) {
	Dwarf_Attribute attr;

	if (!dwarf_hasattr(die, DW_AT_external))
		return false;
	(void) dwarf_attr(die, DW_AT_external, &attr);
	if (!dwarf_hasform(&attr, DW_FORM_flag_present))
		return false;
	return true;
}

/* Check if given DIE has DW_AT_external attribute */
static bool is_declaration(Dwarf_Die *die) {
	Dwarf_Attribute attr;

	if (!dwarf_hasattr(die, DW_AT_declaration))
		return false;
	(void) dwarf_attr(die, DW_AT_declaration, &attr);
	if (!dwarf_hasform(&attr, DW_FORM_flag_present))
		return false;
	return true;
}

static void print_die_attrs(Dwarf_Die *die) {
	size_t cnt;
	int i;

	printf(" Attrs     :");
	for (cnt = 0; cnt < 0xffff; ++cnt)
		if (dwarf_hasattr(die, cnt))
			printf(" %s", dwarf_attr_string (cnt));
	puts ("");

	if (dwarf_hasattr(die, DW_AT_byte_size) &&
	    (i = dwarf_bytesize(die)) != -1)
		printf(" byte size : %d\n", i);
	if (dwarf_hasattr(die, DW_AT_bit_size) &&
	    (i = dwarf_bitsize(die)) != -1)
		printf(" bit size  : %d\n", i);
	if (dwarf_hasattr(die, DW_AT_bit_offset) &&
	    (i = dwarf_bitoffset(die)) != -1)
		printf(" bit offset: %d\n", i);
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

	name = dwarf_diename(die);
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

static void print_die_type(Dwarf *dbg, Dwarf_Die *die) {
	Dwarf_Die type_die;
	Dwarf_Attribute attr;
	const char *name;

	name = dwarf_diename(die);

	if (!dwarf_hasattr(die, DW_AT_type)) {
		printf("void\n");
		return;
	}

	(void) dwarf_attr(die, DW_AT_type, &attr);
	if (dwarf_formref_die(&attr, &type_die) == NULL)
		fail("dwarf_formref_die() failed for %s\n", name);

	print_die(dbg, &type_die);
}

static void print_die_subprogram(Dwarf *dbg, Dwarf_Die *die) {
	Dwarf_Die child_die;
	const char *name;
	int i = 0;

	name = dwarf_diename(die);
	printf("Function: %s\n", name);

	if (!is_external(die))
		fail("Function is not external: %s\n", name);

	/* Print return value */
	printf("Returned value: ");
	print_die_type(dbg, die);

	if (!dwarf_haschildren(die))
		return;

	/* Grab the first argument */
	dwarf_child(die, &child_die);
	/* Walk all arguments until we run into the function body */
	do {
		printf("Argument %d: ", i);
		print_die(dbg, &child_die);
		i++;
	} while ((dwarf_siblingof(&child_die, &child_die) == 0) &&
	    ((dwarf_tag(&child_die) == DW_TAG_formal_parameter) ||
	    (dwarf_tag(&child_die) == DW_TAG_unspecified_parameters)));
}

static void print_die(Dwarf *dbg, Dwarf_Die *die) {
	unsigned int tag;
	const char *name;

	name = dwarf_diename(die);
	tag = dwarf_tag(die);

	if (tag == DW_TAG_invalid)
		fail("DW_TAG_invalid: %s\n", name);

	switch (tag) {
	case DW_TAG_subprogram:
	case DW_TAG_inlined_subroutine:
		print_die_subprogram(dbg, die);
		break;
	case DW_TAG_variable: {
		printf("Variable: %s\n", name);

		if (!is_external(die))
			fail("Variable is not external: %s\n", name);
		print_die_type(dbg, die);
		break;
	}
	case DW_TAG_compile_unit:
		printf("Compilation unit: %s\n", name);
		print_die_attrs(die);
		break;
	case DW_TAG_base_type:
		printf("%s\n", name);
		break;
	case DW_TAG_pointer_type:
		printf("* ");
		print_die_type(dbg, die);
		break;
	case DW_TAG_structure_type:
		print_die_structure(dbg, die);
		break;
	case DW_TAG_union_type:
		printf("Union: %s\n", name);
		break;
	case DW_TAG_typedef:
		printf("Typedef: %s\n", name);
		print_die_type(dbg, die);
		break;
	case DW_TAG_formal_parameter:
		printf("%s\n", name);
		print_die_type(dbg, die);
		break;
	case DW_TAG_unspecified_parameters:
		printf("...\n");
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

/*
 * Walk all DIEs in a CU.
 * Returns true if the given symbol_name was found, otherwise false.
 */
static bool process_cu_die(Dwarf *dbg, Dwarf_Die *die,
    const char *symbol_name) {
	Dwarf_Die child_die;

	if (!dwarf_haschildren(die))
		return false;

	/* Walk all DIEs in the CU */
	dwarf_child(die, &child_die);
	do {
		const char *name = dwarf_diename(&child_die);

		/* Did we find full definition of our symbol? */
		if (name != NULL && (strcmp(name, symbol_name) == 0) &&
		    !is_declaration(&child_die)) {
			/* Print both the CU DIE and symbol DIE */
			print_die(dbg, die);
			print_die(dbg, &child_die);
			return true;
		}
	} while (dwarf_siblingof(&child_die, &child_die) == 0);

	return false;
}

/*
 * Print symbol definition by walking all DIEs in a .debug_info section.
 * Returns true if the definition was printed, otherwise false.
 */
bool print_symbol(const char *filepath, const char *symbol_name) {
	int fd = open(filepath, O_RDONLY);
	bool found = false;
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

		/* CU is followed by a single DIE */
		Dwarf_Die die;
		if (dwarf_offdie(dbg, old_off + hsize, &die) == NULL) {
			fail("dwarf_offdie failed for cu!\n");
		}

		if (process_cu_die(dbg, &die, symbol_name) == true) {
			found = true;
			break;
		}

		old_off = off;
	}

	dwarf_end(dbg);
	close(fd);

	return found;
}
