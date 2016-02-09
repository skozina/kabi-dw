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
#include <assert.h>
#include <libgen.h>

#include <elfutils/libdw.h>
#include <elfutils/known-dwarf.h>

#include "main.h"
#include "kabi-dw.h"

static void print_die(Dwarf *, FILE *, Dwarf_Die *, Dwarf_Die *);

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

static char * get_symbol_file(FILE *fout, Dwarf_Die *die) {
	const char *name = dwarf_diename(die);
	unsigned int tag = dwarf_tag(die);
	char *file_prefix = NULL;
	char *file_name = NULL;
	size_t output_len;

	switch (tag) {
	case DW_TAG_subprogram:
		file_prefix = FUNC_FILE;
		break;
	case DW_TAG_typedef:
		file_prefix = TYPEDEF_FILE;
		break;
	case DW_TAG_variable:
		file_prefix = VAR_FILE;
		break;
	case DW_TAG_enumeration_type:
		/* Anonymous enums can be a variable type */
		if (name != NULL) {
			file_prefix = ENUM_FILE;
		} else {
			return NULL;
		}
		break;
	case DW_TAG_structure_type:
		/* Anonymous structure can be a variable type */
		if (name != NULL) {
			file_prefix = STRUCT_FILE;
		} else {
			return NULL;
		}
		break;
	case DW_TAG_union_type:
		/*
		 * Anonymous union can be a variable type.
		 * But it can also be included in a structure!
		 */
		if (name != NULL) {
			file_prefix = UNION_FILE;
		} else {
			return NULL;
		}
		break;
	default:
		/* No need to redirect output for other types */
		return NULL;
	}

	/* We don't expect our name to be empty now */
	assert(name != NULL);

	output_len = strlen(output_dir) + strlen("/") + strlen(file_prefix) +
	    strlen(name) + strlen(".txt") + 1;
	file_name = malloc(output_len);
	snprintf(file_name, output_len, "%s/%s%s.txt",
	    output_dir, file_prefix, name);

	return file_name;
}

static FILE * open_output_file(char *file_name) {
	FILE *file;

	file = fopen(file_name, "w");
	if (file == NULL)
		fail("Failed to open file %s: %s\n", file_name,
		    strerror(errno));

	return file;
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

static void print_die_type(Dwarf *dbg, FILE *fout, Dwarf_Die *cu_die,
    Dwarf_Die *die) {
	Dwarf_Die type_die;
	Dwarf_Attribute attr;

	if (!dwarf_hasattr(die, DW_AT_type)) {
		fprintf(fout, "void\n");
		return;
	}

	(void) dwarf_attr(die, DW_AT_type, &attr);
	if (dwarf_formref_die(&attr, &type_die) == NULL)
		fail("dwarf_formref_die() failed for %s\n",
		    dwarf_diename(die));

	print_die(dbg, fout, cu_die, &type_die);
}

static void print_die_struct_member(Dwarf *dbg, FILE *fout, Dwarf_Die *cu_die,
    Dwarf_Die *die, const char *name) {
	Dwarf_Attribute attr;
	Dwarf_Word value;

	if (dwarf_attr(die, DW_AT_data_member_location, &attr) == NULL)
		fail("Offset of member %s missing!\n", name);

	(void) dwarf_formudata(&attr, &value);
	fprintf(fout, "0x%lx %s ", value, name);
	print_die_type(dbg, fout, cu_die, die);
}

static void print_die_structure(Dwarf *dbg, FILE *fout, Dwarf_Die *cu_die,
    Dwarf_Die *die) {
	unsigned int tag = dwarf_tag(die);
	const char *name = dwarf_diename(die);

	if (name != NULL)
		fprintf(fout, "struct %s {\n", name);
	else
		fprintf(fout, "struct {\n");

	if (!dwarf_haschildren(die))
		return;

	dwarf_child(die, die);
	do {
		name = dwarf_diename(die);
		tag = dwarf_tag(die);
		if (tag != DW_TAG_member)
			fail("Unexpected tag for structure type children: "
			    "%s\n", dwarf_tag_string(tag));
		print_die_struct_member(dbg, fout, cu_die, die, name);
	} while (dwarf_siblingof(die, die) == 0);

	fprintf(fout, "}\n");
}

static void print_die_enumerator(Dwarf *dbg, FILE *fout, Dwarf_Die *die,
    const char *name) {
	Dwarf_Attribute attr;
	Dwarf_Word value;

	if (dwarf_attr(die, DW_AT_const_value, &attr) == NULL)
		fail("Value of enumerator %s missing!\n", name);

	(void) dwarf_formudata(&attr, &value);
	fprintf(fout, "%s = 0x%lx\n", name, value);
}

static void print_die_enumeration(Dwarf *dbg, FILE *fout, Dwarf_Die *die) {
	const char *name = dwarf_diename(die);

	if (name != NULL)
		fprintf(fout, "enum %s {\n", name);
	else
		fprintf(fout, "enum {\n");

	if (!dwarf_haschildren(die))
		return;

	dwarf_child(die, die);
	do {
		name = dwarf_diename(die);
		print_die_enumerator(dbg, fout, die, name);
	} while (dwarf_siblingof(die, die) == 0);

	fprintf(fout, "}\n");
}

static void print_die_union(Dwarf *dbg, FILE *fout, Dwarf_Die *cu_die,
    Dwarf_Die *die) {
	const char *name = dwarf_diename(die);
	unsigned int tag = dwarf_tag(die);

	if (name != NULL)
		fprintf(fout, "union %s {\n", name);
	else
		fprintf(fout, "union {\n");

	if (!dwarf_haschildren(die))
		return;

	dwarf_child(die, die);
	do {
		name = dwarf_diename(die);
		tag = dwarf_tag(die);
		if (tag != DW_TAG_member)
			fail("Unexpected tag for union type children: %s\n",
			    dwarf_tag_string(tag));
		fprintf(fout, "%s ", name);
		print_die_type(dbg, fout, cu_die, die);
	} while (dwarf_siblingof(die, die) == 0);

	fprintf(fout, "}\n");
}

static void print_subprogram_arguments(Dwarf *dbg, FILE *fout,
    Dwarf_Die *cu_die, Dwarf_Die *die) {
	Dwarf_Die child_die;
	int i = 0;

	if (!dwarf_haschildren(die))
		return;

	/* Grab the first argument */
	dwarf_child(die, &child_die);
	/* Walk all arguments until we run into the function body */
	do {
		const char *name = dwarf_diename(&child_die);
		fprintf(fout, "%s ", name);
		print_die_type(dbg, fout, cu_die, &child_die);
		i++;
	} while ((dwarf_siblingof(&child_die, &child_die) == 0) &&
	    ((dwarf_tag(&child_die) == DW_TAG_formal_parameter) ||
	    (dwarf_tag(&child_die) == DW_TAG_unspecified_parameters)));
}

/* Function pointer */
/* TODO should function pointers go into their own files? */
static void print_die_subroutine_type(Dwarf *dbg, FILE *fout,
    Dwarf_Die *cu_die, Dwarf_Die *die) {
	fprintf(fout, "func %s (\n", dwarf_diename(die));
	print_subprogram_arguments(dbg, fout, cu_die, die);
	fprintf(fout, ")\n");
	/* Print return value */
	print_die_type(dbg, fout, cu_die, die);
}

static void print_die_subprogram(Dwarf *dbg, FILE *fout, Dwarf_Die *cu_die,
    Dwarf_Die *die) {
	const char *name = dwarf_diename(die);

	if (!is_external(die))
		fail("Function is not external: %s\n", name);

	fprintf(fout, "func %s (\n", name);
	print_subprogram_arguments(dbg, fout, cu_die, die);
	fprintf(fout, ")\n");
	/* Print return value */
	print_die_type(dbg, fout, cu_die, die);
}

static void print_die_array_type(Dwarf *dbg, FILE *fout, Dwarf_Die *die) {
	Dwarf_Attribute attr;
	Dwarf_Word value;

	/* There should be one child of DW_TAG_subrange_type */
	if (!dwarf_haschildren(die))
		fail("Array type missing children!\n");

	/* Grab the child */
	dwarf_child(die, die);

	do {
		unsigned int tag = dwarf_tag(die);
		if (tag != DW_TAG_subrange_type)
			fail("Unexpected tag for array type children: %s\n",
			    dwarf_tag_string(tag));

		if (dwarf_hasattr(die, DW_AT_upper_bound)) {
			(void) dwarf_attr(die, DW_AT_upper_bound, &attr);
			(void) dwarf_formudata(&attr, &value);
			/* Get the UPPER bound, so add 1 */
			fprintf(fout, "[%lu]", value + 1);
		} else if (dwarf_hasattr(die, DW_AT_count)) {
			(void) dwarf_attr(die, DW_AT_count, &attr);
			(void) dwarf_formudata(&attr, &value);
			fprintf(fout, "[%lu]", value);
		} else {
			fprintf(fout, "[0]");
		}
	} while (dwarf_siblingof(die, die) == 0);
}

static void print_die(Dwarf *dbg, FILE *parent_file, Dwarf_Die *cu_die,
    Dwarf_Die *die) {
	unsigned int tag = dwarf_tag(die);
	const char *name = dwarf_diename(die);
	char *file_name = get_symbol_file(parent_file, die);
	FILE *fout;

	/* Check if we need to redirect output */
	if (file_name != NULL) {
		/* Else set our output to the file */
		if (parent_file != NULL)
			fprintf(parent_file, "@%s\n", basename(file_name));

		/* If the file already exist, we're done */
		if (access(file_name, F_OK) == 0) {
			free(file_name);
			return;
		}
		printf("Generating %s\n", basename(file_name));
		fout = open_output_file(file_name);
		free(file_name);
	} else {
		fout = parent_file;
	}

	assert(fout != NULL);

	/* If we are printing the CU die do it now */
	if (cu_die != NULL)
		print_die(dbg, fout, NULL, cu_die);

	if (tag == DW_TAG_invalid)
		fail("DW_TAG_invalid: %s\n", name);

	switch (tag) {
	case DW_TAG_subprogram:
		print_die_subprogram(dbg, fout, cu_die, die);
		break;
	case DW_TAG_variable: {
		fprintf(fout, "var %s ", name);

		if (!is_external(die))
			fail("Variable is not external: %s\n", name);
		print_die_type(dbg, fout, cu_die, die);
		break;
	}
	case DW_TAG_compile_unit:
		fprintf(fout, "CU %s\n", name);
		break;
	case DW_TAG_base_type:
		fprintf(fout, "%s\n", name);
		break;
	case DW_TAG_pointer_type:
		fprintf(fout, "* ");
		print_die_type(dbg, fout, cu_die, die);
		break;
	case DW_TAG_structure_type:
		print_die_structure(dbg, fout, cu_die, die);
		break;
	case DW_TAG_enumeration_type:
		print_die_enumeration(dbg, fout, die);
		break;
	case DW_TAG_union_type:
		print_die_union(dbg, fout, cu_die, die);
		break;
	case DW_TAG_typedef:
		fprintf(fout, "typedef %s\n", name);
		print_die_type(dbg, fout, cu_die, die);
		break;
	case DW_TAG_formal_parameter:
		if (name != NULL)
			fprintf(fout, "%s\n", name);
		print_die_type(dbg, fout, cu_die, die);
		break;
	case DW_TAG_unspecified_parameters:
		fprintf(fout, "...\n");
		break;
	case DW_TAG_subroutine_type:
		print_die_subroutine_type(dbg, fout, cu_die, die);
		break;
	case DW_TAG_volatile_type:
		fprintf(fout, "volatile ");
		print_die_type(dbg, fout, cu_die, die);
		break;
	case DW_TAG_const_type:
		fprintf(fout, "const ");
		print_die_type(dbg, fout, cu_die, die);
		break;
	case DW_TAG_array_type:
		print_die_array_type(dbg, fout, die);
		print_die_type(dbg, fout, cu_die, die);
		break;
	default: {
		const char *tagname = dwarf_tag_string(tag);
		if (tagname == NULL)
			tagname = "<NO TAG>";

		fail("Unexpected tag for symbol %s: %s\n", name, tagname);
		break;
	}
	}

	if (file_name != NULL)
		fclose(fout);
}

/*
 * Return the index of symbol in the array of -1 if the symbol was not found.
 */
static int find_symbol(char **symbol_names, size_t symbol_cnt,
    const char *name) {
	int i = 0;

	if (name == NULL)
		return -1;

	for (i = 0; i < symbol_cnt; i++) {
		if (strcmp(symbol_names[i], name) == 0)
			return i;
	}

	return -1;
}

/*
 * Walk all DIEs in a CU.
 * Returns true if the given symbol_name was found, otherwise false.
 */
static void process_cu_die(Dwarf *dbg, Dwarf_Die *cu_die,
    char **symbol_names, size_t symbol_cnt, bool *symbols_found) {
	Dwarf_Die child_die;

	if (!dwarf_haschildren(cu_die))
		return;

	/* Walk all DIEs in the CU */
	dwarf_child(cu_die, &child_die);
	do {
		const char *name = dwarf_diename(&child_die);
		int found;

		/* Did we find full definition of our symbol? */
		found = find_symbol(symbol_names, symbol_cnt, name);
		if (found != -1 && !is_declaration(&child_die)) {
			/* Print both the CU DIE and symbol DIE */
			print_die(dbg, NULL, cu_die, &child_die);
			symbols_found[found] = true;
		}
	} while (dwarf_siblingof(&child_die, &child_die) == 0);
}

/*
 * Print symbol definition by walking all DIEs in a .debug_info section.
 * Returns true if the definition was printed, otherwise false.
 */
void print_symbols(char *filepath, char **symbol_names,
    size_t symbol_cnt) {
	int fd = open(filepath, O_RDONLY);
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
	bool *symbols_found = malloc(symbol_cnt * sizeof (bool *));
	size_t i;

	for (i = 0; i < symbol_cnt; i++)
		symbols_found[i] = false;

	while (dwarf_next_unit(dbg, off, &off, &hsize, &version, &abbrev,
	    &addresssize, &offsetsize, NULL, &type_offset) == 0)
	{
		if (version < 2 || version > 4) {
			fail("Unsupported dwarf version: %d\n", version);
		}

		/* CU is followed by a single DIE */
		Dwarf_Die cu_die;
		if (dwarf_offdie(dbg, old_off + hsize, &cu_die) == NULL) {
			fail("dwarf_offdie failed for cu!\n");
		}

		process_cu_die(dbg, &cu_die, symbol_names, symbol_cnt,
		    symbols_found);

		old_off = off;
	}

	for (i = 0; i < symbol_cnt; i++) {
		if (symbols_found[i] == false) {
			printf("%s not found!\n", symbol_names[i]);
		}
	}

	free(symbols_found);
	dwarf_end(dbg);
	close(fd);
}
