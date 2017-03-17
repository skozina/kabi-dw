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
 * This file contains the code which generates the kabi information for the
 * given build of the Linux kernel.
 */

#include <dwarf.h>
#include <inttypes.h>
#include <libelf.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <getopt.h>

#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <elfutils/known-dwarf.h>

#include "main.h"
#include "utils.h"
#include "generate.h"
#include "ksymtab.h"
#include "stack.h"
#include "hash.h"

#define	EMPTY_NAME	"(NULL)"
#define PROCESSED_SIZE 1024

struct set;

typedef struct {
	bool verbose;
	char *kernel_dir; /* Path to  the kernel modules to process */
	char *kabi_dir; /* Where to put the output */
	struct ksymtab *symbols; /* List of symbols to generate */
	size_t symbol_cnt;
	bool *symbols_found;
	size_t symbols_found_cnt;
} generate_config_t;

struct cu_ctx {
	generate_config_t *conf;
	Dwarf_Die *cu_die;
	stack_t *stack; /* Current stack of symbol we're parsing */
	struct set *processed; /* Set of processed types for this CU */
};

struct file_ctx {
	generate_config_t *conf;
	struct ksymtab *ksymtab;
};

struct dwarf_type {
	unsigned int dwarf_tag;
	char *prefix;
} known_dwarf_types[] = {
	{ DW_TAG_subprogram, FUNC_FILE },
	{ DW_TAG_typedef, TYPEDEF_FILE },
	{ DW_TAG_variable, VAR_FILE },
	{ DW_TAG_enumeration_type, ENUM_FILE },
	{ DW_TAG_structure_type, STRUCT_FILE },
	{ DW_TAG_union_type, UNION_FILE },
	{ 0, NULL }
};

/* List of types built-in the compiler */
static const char *builtin_types[] = {
	"__va_list_tag",
	"__builtin_strlen",
	"__builtin_strcpy",
	NULL
};

static const bool is_builtin(const char *name) {
	const char **p;

	for (p = builtin_types; *p != NULL; p++) {
		if (strcmp(*p, name) == 0)
			return (true);
	}

	return (false);
}

static const char *get_die_name(Dwarf_Die *die) {
	if (dwarf_hasattr(die, DW_AT_name))
		return (dwarf_diename(die));
	else
		return (EMPTY_NAME);
}

/*
 * Check if given DIE has DW_AT_declaration attribute.
 * That indicates that the symbol is just a declaration, not full definition.
 */
static bool is_declaration(Dwarf_Die *die) {
	Dwarf_Attribute attr;

	if (!dwarf_hasattr(die, DW_AT_declaration))
		return (false);
	(void) dwarf_attr(die, DW_AT_declaration, &attr);
	if (!dwarf_hasform(&attr, DW_FORM_flag_present))
		return (false);
	return (true);
}

static char *get_file_replace_path = NULL;

static char *get_file(Dwarf_Die *cu_die, Dwarf_Die *die) {
	Dwarf_Files *files;
	size_t nfiles;
	Dwarf_Attribute attr;
	Dwarf_Word file;
	const char *filename;
	char *ret;

	/*
	 * Handle types built-in in C compiler. These are for example the
	 * variable argument list which is defined as * struct __va_list_tag.
	 */
	if (is_builtin(get_die_name(die)))
		return (safe_strdup(BUILTIN_PATH));

	if (!dwarf_hasattr(die, DW_AT_decl_file))
		fail("DIE missing file information: %s\n",
		    dwarf_diename(die));

	(void) dwarf_attr(die, DW_AT_decl_file, &attr);
	(void) dwarf_formudata(&attr, &file);

	if (dwarf_getsrcfiles(cu_die, &files, &nfiles) != 0)
		fail("cannot get files for CU %s\n", dwarf_diename(cu_die));

	filename = dwarf_filesrc(files, file, NULL, NULL);

	if (get_file_replace_path) {
		int len = strlen(get_file_replace_path);

		if (strncmp(filename, get_file_replace_path, len) == 0) {
			filename = filename + len;
			while (*filename == '/')
				filename++;
		}
	}


	ret = safe_strdup(filename);
	path_normalize(ret);

	return (ret);
}

static long get_line(Dwarf_Die *cu_die, Dwarf_Die *die) {
	Dwarf_Attribute attr;
	Dwarf_Word line;

	if (!dwarf_hasattr(die, DW_AT_decl_line))
		fail("DIE missing file or line information: %s\n",
		    dwarf_diename(die));

	(void) dwarf_attr(die, DW_AT_decl_line, &attr);
	(void) dwarf_formudata(&attr, &line);

	return (line);
}

static void print_die(struct cu_ctx *, FILE *, Dwarf_Die *);

static const char * dwarf_tag_string(unsigned int tag) {
	switch (tag)
	{
#define	DWARF_ONE_KNOWN_DW_TAG(NAME, CODE) case CODE: return #NAME;
		DWARF_ALL_KNOWN_DW_TAG
#undef DWARF_ONE_KNOWN_DW_TAG
		default:
			return (NULL);
	}
}

static char * get_file_prefix(unsigned int dwarf_tag) {
	struct dwarf_type *current;

	for (current = known_dwarf_types; current->prefix != NULL; current++) {
		if (dwarf_tag == current->dwarf_tag)
			break;
	}

	return (current->prefix);
}

static char * get_symbol_file(Dwarf_Die *die, Dwarf_Die *cu_die) {
	const char *name = dwarf_diename(die);
	unsigned int tag = dwarf_tag(die);
	char *file_prefix = NULL;
	char *file_name = NULL;
	char *dec_file;

	if ((file_prefix = get_file_prefix(tag)) == NULL) {
		/* No need to redirect output for this type */
		return (NULL);
	}

	/*
	 * DW_AT_declaration don't have DW_AT_decl_file.
	 * Pretend like it's in other, non existent file.
	 */
	if (is_declaration(die)) {
		safe_asprintf(&file_name, DECLARATION_PATH "/%s%s.txt",
		    file_prefix, name);

		return (file_name);
	}

	/*
	 * Following types can be anonymous, eg. used directly as variable type
	 * in the declaration. We don't create new file for them if that's
	 * the case, embed them directly in the current file.
	 * Note that anonymous unions can also be embedded directly in the
	 * structure!
	 */
	switch (tag) {
	case DW_TAG_enumeration_type:
	case DW_TAG_structure_type:
	case DW_TAG_union_type:
		if (name == NULL)
			return (NULL);
		break;
	}

	/* We don't expect our name to be empty now */
	assert(name != NULL);

	dec_file = get_file(cu_die, die);
	assert(dec_file != NULL);

	safe_asprintf(&file_name, "%s/%s%s.txt", dec_file, file_prefix, name);
	free(dec_file);

	return (file_name);
}

static FILE * open_temp_file(generate_config_t *conf, char **temp_path) {
	FILE *file;
	int fd;

	safe_asprintf(temp_path, "%s/%s/kabi-dw.XXXXXX", conf->kabi_dir,
	    TEMP_PATH);
	if ((fd = mkstemp(*temp_path)) == -1)
		fail("mkstemp() failed: %s\n", strerror(errno));

	file = fdopen(fd, "w");
	if (file == NULL)
		fail("Failed to open file %s: %s\n", *temp_path,
		    strerror(errno));

	return (file);
}

/* Check if given DIE has DW_AT_external attribute */
static bool is_external(Dwarf_Die *die) {
	Dwarf_Attribute attr;

	if (!dwarf_hasattr(die, DW_AT_external))
		return (false);
	(void) dwarf_attr(die, DW_AT_external, &attr);
	if (!dwarf_hasform(&attr, DW_FORM_flag_present))
		return (false);
	return (true);
}

/* Check if given DIE was declared as inline */
static bool is_inline(Dwarf_Die *die) {
	Dwarf_Attribute attr;
	Dwarf_Word value;

	if (!dwarf_hasattr(die, DW_AT_inline))
		return (false);
	(void) dwarf_attr(die, DW_AT_external, &attr);
	(void) dwarf_formudata(&attr, &value);

	if (value >= DW_INL_declared_not_inlined)
		return (true);
	else
		return (false);
}

struct set *set_init(size_t size)
{
	struct hash *h;

	h = hash_new(size, free);
	if (h == NULL)
		fail("Cannot create hash");

	return (struct set *)h;
}

static void set_add(struct set *set, const char *key)
{
	char *storage;
	struct hash *h = (struct hash *)set;

	storage = safe_strdup(key);
	hash_add(h, storage, storage);
}

static bool set_contains(struct set *set, const char *key)
{
	void *v;
	struct hash *h = (struct hash *)set;

	v = hash_find(h, key);
	return v != NULL;
}

static void set_free(struct set *set)
{
	struct hash *h = (struct hash *)set;

	hash_free(h);
}

static void print_die_type(struct cu_ctx *ctx, FILE *fout, Dwarf_Die *die) {
	Dwarf_Die type_die;
	Dwarf_Attribute attr;

	if (!dwarf_hasattr(die, DW_AT_type)) {
		fprintf(fout, "\"void\"\n");
		return;
	}

	(void) dwarf_attr(die, DW_AT_type, &attr);
	if (dwarf_formref_die(&attr, &type_die) == NULL)
		fail("dwarf_formref_die() failed for %s\n",
		    dwarf_diename(die));

	/* Print the type of the die */
	print_die(ctx, fout, &type_die);
}

static void print_die_struct_member(struct cu_ctx *ctx, FILE *fout,
				    Dwarf_Die *die, const char *name) {
	Dwarf_Attribute attr;
	Dwarf_Word value;

	if (dwarf_attr(die, DW_AT_data_member_location, &attr) == NULL)
		fail("Offset of member %s missing!\n", name);

	(void) dwarf_formudata(&attr, &value);
	fprintf(fout, "0x%lx", value);

	if (dwarf_hasattr(die, DW_AT_bit_offset)) {
		Dwarf_Word offset, size;

		if (!dwarf_hasattr(die, DW_AT_bit_size))
			fail("Missing expected bit size attribute in %s!\n",
			    name);

		if (dwarf_attr(die, DW_AT_bit_offset, &attr) == NULL)
			fail("Bit offset of member %s missing!\n", name);
		(void) dwarf_formudata(&attr, &offset);
		if (dwarf_attr(die, DW_AT_bit_size, &attr) == NULL)
			fail("Bit size of member %s missing!\n", name);
		(void) dwarf_formudata(&attr, &size);
		fprintf(fout, ":%ld-%ld", offset, offset + size - 1);
	}

	fprintf(fout, " %s ", name);
	print_die_type(ctx, fout, die);
}

static void print_die_structure(struct cu_ctx *ctx, FILE *fout,
				Dwarf_Die *die) {
	unsigned int tag = dwarf_tag(die);
	const char *name = get_die_name(die);

	fprintf(fout, "struct %s {\n", name);

	if (!dwarf_haschildren(die))
		goto done;

	dwarf_child(die, die);
	do {
		name = get_die_name(die);
		tag = dwarf_tag(die);
		if (tag != DW_TAG_member)
			fail("Unexpected tag for structure type children: "
			    "%s\n", dwarf_tag_string(tag));
		print_die_struct_member(ctx, fout, die, name);
	} while (dwarf_siblingof(die, die) == 0);

done:
	fprintf(fout, "}\n");
}

static void print_die_enumerator(struct cu_ctx *ctx, FILE *fout, Dwarf_Die *die,
    const char *name) {
	Dwarf_Attribute attr;
	Dwarf_Word value;

	if (dwarf_attr(die, DW_AT_const_value, &attr) == NULL)
		fail("Value of enumerator %s missing!\n", name);

	(void) dwarf_formudata(&attr, &value);
	fprintf(fout, "%s = 0x%lx\n", name, value);
}

static void print_die_enumeration(struct cu_ctx *ctx, FILE *fout, Dwarf_Die *die) {
	const char *name = get_die_name(die);

	fprintf(fout, "enum %s {\n", name);

	if (!dwarf_haschildren(die))
		goto done;

	dwarf_child(die, die);
	do {
		name = get_die_name(die);
		print_die_enumerator(ctx, fout, die, name);
	} while (dwarf_siblingof(die, die) == 0);

done:
	fprintf(fout, "}\n");
}

static void print_die_union(struct cu_ctx *ctx, FILE *fout, Dwarf_Die *die) {
	const char *name = get_die_name(die);
	unsigned int tag = dwarf_tag(die);

	fprintf(fout, "union %s {\n", name);

	if (!dwarf_haschildren(die))
		goto done;

	dwarf_child(die, die);
	do {
		name = get_die_name(die);
		tag = dwarf_tag(die);
		if (tag != DW_TAG_member)
			fail("Unexpected tag for union type children: %s\n",
			    dwarf_tag_string(tag));
		fprintf(fout, "%s ", name);
		print_die_type(ctx, fout, die);
	} while (dwarf_siblingof(die, die) == 0);

done:
	fprintf(fout, "}\n");
}

static void print_subprogram_arguments(struct cu_ctx *ctx, FILE *fout,
    Dwarf_Die *die) {
	Dwarf_Die child_die;

	if (!dwarf_haschildren(die))
		return;

	/* Grab the first argument */
	dwarf_child(die, &child_die);

	/* Walk all arguments until we run into the function body */
	while ((dwarf_tag(&child_die) == DW_TAG_formal_parameter) ||
	    (dwarf_tag(&child_die) == DW_TAG_unspecified_parameters)) {
		const char *name = get_die_name(&child_die);
		fprintf(fout, "%s ", name);

		/*
		 * Print type of the argument.
		 * If there are unspecified arguments (... in C) print the DIE
		 * itself.
		 */
		if (dwarf_tag(&child_die) != DW_TAG_unspecified_parameters)
			print_die_type(ctx, fout, &child_die);
		else
			print_die(ctx, fout, &child_die);

		if (dwarf_siblingof(&child_die, &child_die) != 0)
			break;
	}
}

static void print_die_subprogram(struct cu_ctx *ctx, FILE *fout,
    Dwarf_Die *die) {
	fprintf(fout, "func %s (\n", get_die_name(die));
	print_subprogram_arguments(ctx, fout, die);
	fprintf(fout, ")\n");

	/* Print return value */
	print_die_type(ctx, fout, die);
}

static void print_die_array_type(struct cu_ctx *ctx, FILE *fout, Dwarf_Die *die) {
	Dwarf_Attribute attr;
	Dwarf_Word value;
	Dwarf_Die child;

	/* There should be one child of DW_TAG_subrange_type */
	if (!dwarf_haschildren(die))
		fail("Array type missing children!\n");

	/* Grab the child */
	dwarf_child(die, &child);

	do {
		unsigned int tag = dwarf_tag(&child);
		if (tag != DW_TAG_subrange_type)
			fail("Unexpected tag for array type children: %s\n",
			    dwarf_tag_string(tag));

		if (dwarf_hasattr(&child, DW_AT_upper_bound)) {
			(void) dwarf_attr(&child, DW_AT_upper_bound, &attr);
			(void) dwarf_formudata(&attr, &value);
			/* Get the UPPER bound, so add 1 */
			fprintf(fout, "[%lu]", value + 1);
		} else if (dwarf_hasattr(&child, DW_AT_count)) {
			(void) dwarf_attr(&child, DW_AT_count, &attr);
			(void) dwarf_formudata(&attr, &value);
			fprintf(fout, "[%lu]", value);
		} else {
			fprintf(fout, "[0]");
		}
	} while (dwarf_siblingof(&child, &child) == 0);
}

static void print_stack_cb(void *data, void *arg) {
	char *symbol = (char *)data;
	FILE *fp = (FILE *)arg;

	fprintf(fp, "-> \"%s\"\n", symbol);
}

static void get_header_line(FILE *file, char **s, size_t *n,
    const char *template) {
	safe_getline(s, n, file);
	if (strncmp(template, *s, strlen(template)) != 0)
		fail("skipheader: no compile unit\n");
}

static void get_first_line(FILE *file, char **s, size_t *n, FILE *ofile) {
	while (true) {
		safe_getline(s, n, file);
		if (strncmp("-> ", *s, 3) != 0)
			break;
		if (ofile != NULL)
			fprintf(ofile, "%s", *s);
	}
}

static bool merge_files(char *path1, char *path2, char **opath,
    generate_config_t *conf) {
	FILE *file1, *file2, *ofile;
	char *s1 = NULL, *s2 = NULL;
	size_t n1 = 0, n2 = 0;
	bool ret = true;

	file1 = fopen(path1, "r");
	file2 = fopen(path2, "r");
	ofile = open_temp_file(conf, opath);

	/* Ignore CU line */
	get_header_line(file1, &s1, &n1, "CU ");
	get_header_line(file2, &s2, &n2, "CU ");
	fprintf(ofile, "%s", s1);

	/* Check File line */
	get_header_line(file1, &s1, &n1, "File ");
	get_header_line(file2, &s2, &n2, "File ");

	if (strcmp(s1, s2) != 0) {
		ret = false;
		goto out;
	}
	fprintf(ofile, "%s", s1);

	/* Skipt the stack */
	get_first_line(file1, &s1, &n1, ofile);
	get_first_line(file2, &s2, &n2, NULL);

	do {
		bool is_s1_declaration = (strstr(s1, DECLARATION_PATH) != NULL);
		bool is_s2_declaration = (strstr(s2, DECLARATION_PATH) != NULL);

		/*
		 * We can merge the two lines if:
		 *  - they are the same, or
		 *  - they are both RH_KABI_HIDE, or
		 *  - at least one of them is a declaration
		 * The condition below is just the negation of the above.
		 */
		if ((strcmp(s1, s2) != 0) &&
		    ((strncmp(s1, RH_KABI_HIDE, RH_KABI_HIDE_LEN) != 0) ||
		    (strncmp(s2, RH_KABI_HIDE, RH_KABI_HIDE_LEN) != 0)) &&
		    !is_s1_declaration && !is_s2_declaration) {
			ret = false;
			goto out;
		}
		if (is_s1_declaration)
			fprintf(ofile, "%s", s2);
		else
			fprintf(ofile, "%s", s1);
	} while ((safe_getline(&s1, &n1, file1) != -1) &&
	    (safe_getline(&s2, &n2, file2) != -1));

out:
	fclose(ofile);
	if (ret == false) {
		unlink(*opath);
		free(*opath);
		*opath = NULL;
	}
	free(s1);
	free(s2);
	fclose(file1);
	fclose(file2);
	return (ret);
}

static void print_die(struct cu_ctx *ctx, FILE *parent_file, Dwarf_Die *die) {
	unsigned int tag = dwarf_tag(die);
	const char *name = dwarf_diename(die);
	char *file;
	char *temp_path = NULL;
	FILE *fout;
	generate_config_t *conf = ctx->conf;

	/*
	 * Sigh. The type of some fields (eg. struct member as a pointer to
	 * another struct) can be defined by a mere declaration without a full
	 * specification of the type.  In such cases we just print a remote
	 * pointer to the full type and pray it will be printed in a different
	 * occasion.
	 */

	/* Check if we need to redirect output or we have a mere declaration */
	file = get_symbol_file(die, ctx->cu_die);

	if (file != NULL) {
		char *dec_file;
		long dec_line;


		/*
		 * Don't try to reenter a file that we have seen already for
		 * this CU.
		 * Note that this is just a pure optimization, the same file
		 * (type) in the same CU must be identical.
		 * But this is major optimization, without it a single
		 * re-generation of a top file would require a full regeneration
		 * of its full tree, thus the difference in speed is many orders
		 * of magnitude!
		 */
		if (set_contains(ctx->processed, file))
			goto done;

		set_add(ctx->processed, file);

		if (is_declaration(die)) {
			if (conf->verbose)
				printf("WARNING: Skipping following file as we "
				    "have only declaration: %s\n", file);
			goto done;
		}

		if (conf->verbose)
			printf("Generating %s\n", file);
		fout = open_temp_file(conf, &temp_path);

		/* Print the CU die on the first line of each file */
		fprintf(fout, "CU \"%s\"\n", dwarf_diename(ctx->cu_die));

		/* Then print the source file & line */
		dec_file = get_file(ctx->cu_die, die);
		dec_line = get_line(ctx->cu_die, die);
		fprintf(fout, "File %s:%lu\n", dec_file, dec_line);
		free(dec_file);

		/* Print the stack and add then add the current file to it */
		walk_stack(ctx->stack, print_stack_cb, fout);
		stack_push(ctx->stack, safe_strdup(file));
	} else {
		fout = parent_file;
	}

	assert(fout != NULL);

	if (tag == DW_TAG_invalid)
		fail("DW_TAG_invalid: %s\n", name);

	switch (tag) {
	case DW_TAG_subprogram:
		print_die_subprogram(ctx, fout, die);
		break;
	case DW_TAG_variable:
		fprintf(fout, "var %s ", name);
		print_die_type(ctx, fout, die);
		break;
	case DW_TAG_base_type:
		fprintf(fout, "\"%s\"\n", name);
		break;
	case DW_TAG_pointer_type:
		fprintf(fout, "* ");
		print_die_type(ctx, fout, die);
		break;
	case DW_TAG_structure_type:
		print_die_structure(ctx, fout, die);
		break;
	case DW_TAG_enumeration_type:
		print_die_enumeration(ctx, fout, die);
		break;
	case DW_TAG_union_type:
		print_die_union(ctx, fout, die);
		break;
	case DW_TAG_typedef:
		fprintf(fout, "typedef %s\n", name);
		print_die_type(ctx, fout, die);
		break;
	case DW_TAG_formal_parameter:
		if (name != NULL)
			fprintf(fout, "%s\n", name);
		print_die_type(ctx, fout, die);
		break;
	case DW_TAG_unspecified_parameters:
		fprintf(fout, "...\n");
		break;
	case DW_TAG_subroutine_type:
		print_die_subprogram(ctx, fout, die);
		break;
	case DW_TAG_volatile_type:
		fprintf(fout, "volatile ");
		print_die_type(ctx, fout, die);
		break;
	case DW_TAG_const_type:
		fprintf(fout, "const ");
		print_die_type(ctx, fout, die);
		break;
	case DW_TAG_array_type:
		print_die_array_type(ctx, fout, die);
		print_die_type(ctx, fout, die);
		break;
	default: {
		const char *tagname = dwarf_tag_string(tag);
		if (tagname == NULL)
			tagname = "<NO TAG>";

		fail("Unexpected tag for symbol %s: %s\n", name, tagname);
		break;
	}
	}

	if (file != NULL) {
		char *final_path, *base_file = NULL;
		char *merged_path = NULL;
		char *dir = conf->kabi_dir;
		int version = 0;

		free(stack_pop(ctx->stack));
		fclose(fout);

	again:
		safe_asprintf(&final_path, "%s/%s", dir, file);

		/*
		 * Now we need to put the new type file we've just generated
		 * (in temp_path) to its final destination (final_path).
		 *
		 * Often the only difference between these files are some
		 * fields which are fully defined in one file (because the
		 * respective header file has been included for the CU
		 * compilation) while these are mere declarations in the other
		 * (because the header file was not used). We detect this case
		 * and if this is the only difference we just merge these two
		 * files into one using the full definitions where available.
		 *
		 * But of course two types (eg. structures) of the same name
		 * might be completely different types. In such case we try to
		 * store them under different names using increasing number as
		 * a suffix.
		 */
		if (access(final_path, F_OK) != 0) {
			/* Target file doesn't exist, trivial */
			if (errno != ENOENT)
				fail("access() failed: %s\n", strerror(errno));

			safe_rename(temp_path, final_path);
			goto out;
		}

		if (merge_files(final_path, temp_path, &merged_path, conf)) {
			/* We managed to merge the files, drop the old one */
			unlink(temp_path);
			unlink(final_path);
			safe_rename(merged_path, final_path);
			free(merged_path);
			goto out;
		}

		/* Two different types detected, bump the name version */
		if (version == 0) {
			base_file = safe_strdup(file);
			/* Remove .txt ending */
			base_file[strlen(base_file) - 4] = '\0';
		}
		version++;
		free(final_path);
		free(file);
		safe_asprintf(&file, "%s-%i.txt",
				base_file, version);
		goto again;

	out:
		free(base_file);
		free(final_path);
		free(temp_path);
	}

done:
	/* Put the link to the new file in the old file */
	if (parent_file != NULL && file != NULL)
		fprintf(parent_file, "@\"%s\"\n", file);

	free(file);
}

/*
 * Validate if this is the symbol we should print.
 * Returns index into the symbol array if this is symbol to print.
 * Otherwise returns -1.
 */
static int get_symbol_index(Dwarf_Die *die, struct file_ctx *fctx) {
	const char *name = dwarf_diename(die);
	unsigned int tag = dwarf_tag(die);
	int result = 0;
	generate_config_t *conf = fctx->conf;

	/* If symbol file was provided, is the symbol on the list? */
	if (conf->symbols != NULL) {
		result = ksymtab_find(conf->symbols, name);
		if (result == -1)
			return (-1);
	}

	/* We don't care about declarations */
	if (is_declaration(die))
		return (-1);

	/* Is this symbol exported in this module with EXPORT_SYMBOL? */
	if (ksymtab_find(fctx->ksymtab, name) == -1)
		return (-1);

	/* Anything except inlined functions should be external */
	if (!is_inline(die) && !is_external(die))
		return (-1);

	/* We expect only variables or functions on whitelist */
	switch (tag) {
	case (DW_TAG_subprogram):
		/*
		 * We ignore DW_AT_prototyped. This marks functions with
		 * arguments specified in their declaration which the old
		 * pre-ANSI C didn't require. Unfortunatelly people still omit
		 * arguments instead of using foo(void) so we need to handle
		 * even functions without DW_AT_prototyped. What a pity!
		 */
		break;
	case DW_TAG_variable:
		break;
	case DW_TAG_structure_type:
		break;
	default:
		fail("Symbol %s has unexpected tag: %s!\n", name,
		    dwarf_tag_string(tag));
	}

	return (result);
}

/*
 * Walk all DIEs in a CU.
 * Returns true if the given symbol_name was found, otherwise false.
 */
static void process_cu_die(Dwarf_Die *cu_die, struct file_ctx *fctx) {
	generate_config_t *conf = fctx->conf;
	Dwarf_Die child_die;
	bool cu_printed = false;

	if (!dwarf_haschildren(cu_die))
		return;

	/* Walk all DIEs in the CU */
	dwarf_child(cu_die, &child_die);
	do {
		int index = get_symbol_index(&child_die, fctx);
		if (index != -1) {
			void *data;
			struct cu_ctx ctx;

			if (!cu_printed && conf->verbose) {
				printf("Processing CU %s\n",
				    dwarf_diename(cu_die));
				cu_printed = true;
			}

			ctx.conf = conf;
			ctx.cu_die = cu_die;
			/* Grab a fresh stack of symbols */
			ctx.stack = stack_init();
			/* And a set of all processed symbols */
			ctx.processed = set_init(PROCESSED_SIZE);

			/* Print both the CU DIE and symbol DIE */
			print_die(&ctx, NULL, &child_die);

			if (conf->symbols != NULL) {
				if (!conf->symbols_found[index]) {
					conf->symbols_found[index] = true;
					conf->symbols_found_cnt++;
				}
			}

			/* And clear the stack again */
			while ((data = stack_pop(ctx.stack)) != NULL)
				free(data);

			stack_destroy(ctx.stack);
			set_free(ctx.processed);
		}
	} while (dwarf_siblingof(&child_die, &child_die) == 0);
}

static int dwflmod_generate_cb(Dwfl_Module *dwflmod, void **userdata,
    const char *name, Dwarf_Addr base, void *arg) {
	Dwarf_Addr dwbias;
	Dwarf *dbg = dwfl_module_getdwarf(dwflmod, &dwbias);
	struct file_ctx *fctx = (struct file_ctx *)arg;

	if (*userdata != NULL)
		fail("Multiple modules found in %s!\n", name);
	*userdata = dwflmod;

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
		Dwarf_Die cu_die;
		if (dwarf_offdie(dbg, old_off + hsize, &cu_die) == NULL) {
			fail("dwarf_offdie failed for cu!\n");
		}

		process_cu_die(&cu_die, fctx);

		old_off = off;
	}

	return (DWARF_CB_OK);
}

static void generate_type_info(char *filepath, struct file_ctx *ctx) {
	static const Dwfl_Callbacks callbacks =
	{
		.section_address = dwfl_offline_section_address,
		.find_debuginfo = dwfl_standard_find_debuginfo
	};
	Dwfl *dwfl = dwfl_begin(&callbacks);

	if (dwfl_report_offline(dwfl, filepath, filepath, -1) == NULL) {
		dwfl_report_end(dwfl, NULL, NULL);
		fail("dwfl_report_offline failed: %s\n", dwfl_errmsg(-1));
	}
	dwfl_report_end(dwfl, NULL, NULL);
	dwfl_getmodules(dwfl, &dwflmod_generate_cb, ctx, 0);

	dwfl_end(dwfl);
}

static bool is_all_done(generate_config_t *conf) {
	if (conf->symbols == NULL)
		return (false);

	assert(conf->symbols_found_cnt <= conf->symbol_cnt);
	return conf->symbols_found_cnt == conf->symbol_cnt;
}

static bool process_symbol_file(char *path, void *arg) {
	struct file_ctx fctx;
	generate_config_t *conf = (generate_config_t *)arg;

	fctx.conf = conf;
	fctx.ksymtab = ksymtab_read(path);

	if (ksymtab_len(fctx.ksymtab) > 0) {
		if (conf->verbose)
			printf("Processing %s\n", path);

		generate_type_info(path, &fctx);
	} else {
		if (conf->verbose)
			printf("Skip %s (no exported symbols)\n", path);
	}

	ksymtab_free(fctx.ksymtab);

	if (is_all_done(conf))
		return (false);
	return (true);
}

static void print_not_found(const char *s, size_t i, void *ctx)
{
	bool *symbols_found = ctx;

	if (!symbols_found[i])
		printf("%s not found!\n", s);
}

/*
 * Print symbol definition by walking all DIEs in a .debug_info section.
 * Returns true if the definition was printed, otherwise false.
 */
static void generate_symbol_defs(generate_config_t *conf) {
	struct stat st;

	if (stat(conf->kernel_dir, &st) != 0)
		fail("Failed to stat %s: %s\n", conf->kernel_dir,
		    strerror(errno));

	/* Lets walk the normal modules */
	printf("Generating symbol defs from %s...\n", conf->kernel_dir);
	if (S_ISDIR(st.st_mode)) {
		walk_dir(conf->kernel_dir, false, process_symbol_file, conf);
	} else if (S_ISREG(st.st_mode)) {
		char *path = conf->kernel_dir;
		conf->kernel_dir = "";
		process_symbol_file(path, conf);
	} else {
		fail("Not a file or directory: %s\n", conf->kernel_dir);
	}

	ksymtab_for_each(conf->symbols, print_not_found, conf->symbols_found);
}

#define	WHITESPACE	" \t\n"

/* Remove white characters from given buffer */
static void strip(char *buf) {
	size_t i = 0, j = 0;
	while (buf[j] != '\0') {
		if (strchr(WHITESPACE, buf[j]) == NULL) {
			if (i != j)
				buf[i] = buf[j];
			i++;
		}
		j++;
	}
	buf[i] = '\0';
}

/* Get list of symbols to generate. */
static struct ksymtab *read_symbols(char *filename)
{
	FILE *fp = fopen(filename, "r");
	char *line = NULL;
	size_t len = 0;
	size_t i = 0;
	struct ksymtab *symbols;

	symbols = ksymtab_new(DEFAULT_BUFSIZE);

	if (fp == NULL)
		fail("Failed to open symbol file: %s\n", strerror(errno));

	errno = 0;
	while ((getline(&line, &len, fp)) != -1) {
		strip(line);
		ksymtab_add_sym(symbols, line, len, i);
		i++;
	}

	if (errno != 0)
		fail("getline() failed for %s: %s\n", filename,
		    strerror(errno));

	if (line != NULL)
		free(line);

	fclose(fp);

	return symbols;
}

static void generate_usage() {
	printf("Usage:\n"
	       "\tgenerate [options] kernel_dir\n"
	       "\nOptions:\n"
	       "    -h, --help:\t\tshow this message\n"
	       "    -v, --verbose:\tdisplay debug information\n"
	       "    -o, --output kabi_dir:\n\t\t\t"
	       "where to write kabi files (default: \"output\")\n"
	       "    -s, --symbols symbol_file:\n\t\t\ta file containing the"
	       " list of symbols of interest (e.g. whitelisted)\n"
	       "    -r, --replace-path abs_path:\n\t\t\t"
	       "replace the absolute path by a relative path\n");
	exit(1);
}

static void parse_generate_opts(int argc, char **argv, generate_config_t *conf,
    char **symbol_file) {
	*symbol_file = NULL;
	conf->verbose = false;
	conf->kabi_dir = DEFAULT_OUTPUT_DIR;
	int opt, opt_index;
	struct option loptions[] = {
		{"help", no_argument, 0, 'h'},
		{"verbose", no_argument, 0, 'v'},
		{"output", required_argument, 0, 'o'},
		{"symbols", required_argument, 0, 's'},
		{"replace-path", required_argument, 0, 'r'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "hvo:s:r:m:",
				  loptions, &opt_index)) != -1) {
		switch (opt) {
		case 'h':
			generate_usage();
		case 'v':
			conf->verbose = true;
			break;
		case 'o':
			conf->kabi_dir = optarg;
			break;
		case 's':
			*symbol_file = optarg;
			break;
		case 'r':
			get_file_replace_path = optarg;
			break;
		default:
			generate_usage();
		}
	}

	if (optind != argc - 1)
		generate_usage();

	conf->kernel_dir = argv[optind];

	rec_mkdir(conf->kabi_dir);
}

void generate(int argc, char **argv) {
	char *temp_path;
	char *symbol_file;
	generate_config_t *conf = safe_malloc(sizeof (*conf));

	parse_generate_opts(argc, argv, conf, &symbol_file);

	if (symbol_file != NULL) {
		int i;

		conf->symbols = read_symbols(symbol_file);
		conf->symbol_cnt = ksymtab_len(conf->symbols);

		if (conf->verbose)
			printf("Loaded %ld symbols\n", conf->symbol_cnt);
		conf->symbols_found = safe_malloc(conf->symbol_cnt *
						  sizeof (*conf->symbols_found));
		for (i = 0; i < conf->symbol_cnt; i++)
			conf->symbols_found[i] = false;
	}

	/* Create a place for temporary files */
	safe_asprintf(&temp_path, "%s/%s", conf->kabi_dir, TEMP_PATH);
	rec_mkdir(temp_path);

	generate_symbol_defs(conf);

	/* Delete the temporary space again */
	if (rmdir(temp_path) != 0)
		printf("WARNING: Failed to delete %s: %s\n", temp_path,
		    strerror(errno));

	free(temp_path);

	if (symbol_file != NULL) {
		free(conf->symbols_found);
		ksymtab_free(conf->symbols);
	}

	free(conf);
}
