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
#include <ctype.h>
#include <libelf.h>
#include <gelf.h>
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
#include <limits.h>

#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <elfutils/known-dwarf.h>

#include "main.h"
#include "utils.h"
#include "generate.h"
#include "ksymtab.h"
#include "stack.h"
#include "hash.h"
#include "objects.h"
#include "list.h"

#define	EMPTY_NAME	"(NULL)"
#define PROCESSED_SIZE 1024
/*
  DB size is number of hash buckets, do not have to be exact,
  but since now we have ~20K records, make it this
*/
#define DB_SIZE (20 * 1024)
#define INITIAL_RECORD_SIZE 512

/*
 * Dwarf5 spec, 7.5.4 Attribute Encodings
 * libdw doesn't support it yet
 */
#ifndef DW_AT_alignment
#define DW_AT_alignment 0x88
#endif

struct set;
struct record_db;

typedef struct {
	char *kernel_dir; /* Path to  the kernel modules to process */
	char *kabi_dir; /* Where to put the output */
	struct ksymtab *symbols; /* List of symbols to generate */
	size_t symbol_cnt;
	struct record_db *db;
	bool rhel_tree;
	bool verbose;
	bool gen_extra;
} generate_config_t;

struct cu_ctx {
	generate_config_t *conf;
	Dwarf_Die *cu_die;
	stack_t *stack; /* Current stack of symbol we're parsing */
	struct set *processed; /* Set of processed types for this CU */
	unsigned char dw_version : 6;
	unsigned char elf_endian : 2;
};

struct file_ctx {
	generate_config_t *conf;
	struct ksymtab *ksymtab; /* ksymtab of the current kernel module */
	unsigned char dw_version : 6;
	unsigned char elf_endian : 2;
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


/*
 * Structure of the database record:
 *
 * key: record key, usually includes path the file, where the type is
 *      defined (may include pseudo path, like <declaration>);
 *
 * version: type's version, used when we need to add another type of the same
 *	    name. It may happend, for example, when because of defines the same
 *          structure has changed for different compilation units.
 *
 *          It is not for the case, when the same structure defined in
 *	    different files -- it will have different keys, since it includes
 *	    the path;
 *
 * ref_count: reference counter, needed since the ownership is shared with the
 *            internal database;
 *
 * base_file: base part of the key (without version), used to generate the
 *            unique key for the new version;
 *
 * cu: compilation unit, where the type for the record defined;
 *
 * origin: "File <file>:<line>" string, describing the source, where the type
 *         for the record defined;
 *
 * stack: stack of types to reach this one.
 *         Ex.: on the toplevel
 *              struct A {
 *                        struct B fieldA;
 *              }
 *         in another file:
 *              struct B {
 *                        basetype fieldB;
 *              }
 *         the "struct B" description will contain key of the "struct A"
 *         description record in the stack;
 *
 * obj: pointer to the abstract type object, representing the toplevel type of
 *      the record.
 *
 * link: name of weak link alisas for the weak aliases.
 *
 * free: type specific function to free the record
 *       (there are normal, weak and assembly records).
 *
 * dump: type specific function for record output.
 *
 * dependents: objects that reference this record.
 */
struct record {
	char *key;
	int version;
	int ref_count;
	char *base_file;
	char *cu;
	char *origin;
	stack_t *stack;
	obj_t *obj;
	char *link;
	void (*free)(struct record *);
	void (*dump)(struct record *, FILE *);

	struct list dependents;
};

void record_update_dependents(struct record *record)
{
	struct list_node *iter;

	LIST_FOR_EACH(&record->dependents, iter) {
		obj_t *obj = list_node_data(iter);

		free(obj->base_type);
		obj->base_type = safe_strdup(record->key);
	}
}

static const bool is_builtin(Dwarf_Die *die)
{
	char *fname;
	const char *path = dwarf_decl_file(die);

	if (path == NULL)
		return true;

	fname = basename(path);
	assert (fname != NULL);
	if (strcmp(fname, "<built-in>") == 0)
		return true;

	return false;
}

static const char *get_die_name(Dwarf_Die *die)
{
	if (dwarf_hasattr(die, DW_AT_name))
		return dwarf_diename(die);
	else
		return EMPTY_NAME;
}

/*
 * Check if given DIE has DW_AT_declaration attribute.
 * That indicates that the symbol is just a declaration, not full definition.
 */
static bool is_declaration(Dwarf_Die *die)
{
	Dwarf_Attribute attr;

	if (!dwarf_hasattr(die, DW_AT_declaration))
		return false;
	(void) dwarf_attr(die, DW_AT_declaration, &attr);
	if (dwarf_hasform(&attr, DW_FORM_flag))
		return attr.valp != NULL;
	if (!dwarf_hasform(&attr, DW_FORM_flag_present))
		return false;
	return true;
}

static char *get_file_replace_path;

static char *_get_file(Dwarf_Die *cu_die, Dwarf_Die *die)
{
	const char *filename;
	char *ret;

	filename = dwarf_decl_file(die);

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

	return ret;
}

static char *get_file(Dwarf_Die *cu_die, Dwarf_Die *die)
{
	Dwarf_Attribute attr;
	Dwarf_Die spec_die;

	/*
	 * Handle types built-in in C compiler. These are for example the
	 * variable argument list which is defined as * struct __va_list_tag.
	 */
	if (is_builtin(die))
		return safe_strdup(BUILTIN_PATH);

	if (dwarf_hasattr(die, DW_AT_decl_file))
		return _get_file(cu_die, die);

	if ((dwarf_attr(die, DW_AT_specification, &attr) == NULL) ||
	    (dwarf_formref_die(&attr, &spec_die) == NULL)) {
		fail("DIE missing file information: %s\n",
		     dwarf_diename(die));
	}

	return _get_file(cu_die, &spec_die);
}

static long get_line(Dwarf_Die *cu_die, Dwarf_Die *die)
{
	Dwarf_Attribute attr;
	Dwarf_Word line;
	Dwarf_Die spec_die;

	if (is_builtin(die))
		return 0;

	if (dwarf_attr(die, DW_AT_decl_line, &attr) != NULL) {
		dwarf_formudata(&attr, &line);
		return line;
	}

	if ((dwarf_attr(die, DW_AT_specification, &attr) == NULL) ||
	    (dwarf_formref_die(&attr, &spec_die) == NULL)) {
		fail("DIE missing line information: %s\n",
		     dwarf_diename(die));
	}

	return get_line(cu_die, &spec_die);
}

static obj_t *print_die(struct cu_ctx *, struct record *, Dwarf_Die *);

static const char *dwarf_tag_string(unsigned int tag)
{
	switch (tag) {
#define	DWARF_ONE_KNOWN_DW_TAG(NAME, CODE) case CODE: return #NAME;
		DWARF_ALL_KNOWN_DW_TAG
#undef DWARF_ONE_KNOWN_DW_TAG
	default:
		return NULL;
	}
}

static char *get_file_prefix(unsigned int dwarf_tag)
{
	struct dwarf_type *current;

	for (current = known_dwarf_types; current->prefix != NULL; current++) {
		if (dwarf_tag == current->dwarf_tag)
			break;
	}

	return current->prefix;
}

static char *get_symbol_file(Dwarf_Die *die, Dwarf_Die *cu_die)
{
	const char *name = dwarf_diename(die);
	unsigned int tag = dwarf_tag(die);
	char *file_prefix;
	char *file_name = NULL;

	file_prefix = get_file_prefix(tag);
	if (file_prefix == NULL) {
		/* No need to redirect output for this type */
		return NULL;
	}

	/*
	 * DW_AT_declaration don't have DW_AT_decl_file.
	 * Pretend like it's in other, non existent file.
	 */
	if (is_declaration(die)) {
		safe_asprintf(&file_name, DECLARATION_PATH "/%s%s.txt",
		    file_prefix, name);

		return file_name;
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
			return NULL;
		break;
	}

	/* We don't expect our name to be empty now */
	assert(name != NULL);

	safe_asprintf(&file_name, "%s%s.txt", file_prefix, name);

	return file_name;
}

/*
 * Check if given DIE is external.
 * It can has DW_AT_external attribute itself,
 * or in case of reference to the specification,
 * the specification DIE can be exported.
 */
static int is_external(Dwarf_Die *die)
{
	Dwarf_Attribute attr;
	Dwarf_Die spec_die;

	if (dwarf_hasattr(die, DW_AT_external)) {
		dwarf_attr(die, DW_AT_external, &attr);
		if (dwarf_hasform(&attr, DW_FORM_flag))
			return attr.valp != NULL;
		if (!dwarf_hasform(&attr, DW_FORM_flag_present))
			return false;
		return true;
	}

	if (dwarf_attr(die, DW_AT_specification, &attr) == NULL)
		return false;

	if (dwarf_formref_die(&attr, &spec_die) == NULL)
		fail("dwarf_formref_die() failed for %s\n",
		     dwarf_diename(die));
	return is_external(&spec_die);
}

static uint8_t die_attr_eval_op(Dwarf_Attribute *attr, Dwarf_Word *value)
{
	size_t op_idx, op_cnt;
	uint8_t loc_expr_type = 0;
	Dwarf_Op *loc_expr_oper;

	dwarf_getlocation(attr, &loc_expr_oper, &op_cnt);

	if (op_cnt == 0)
		loc_expr_type = -1;

	for (op_idx = 0; op_idx < op_cnt; ++op_idx) {
		loc_expr_type = loc_expr_oper[op_idx].atom;
		switch (loc_expr_oper[op_idx].atom) {

		/* supported 0-ary operations */
		case DW_OP_const1u: /* unsigned 1-byte constant */
		case DW_OP_const1s: /* signed   1-byte constant */
		case DW_OP_const2u: /* unsigned 2-byte constant */
		case DW_OP_const2s: /* signed   2-byte constant */
		case DW_OP_skip:    /* signed   2-byte constant */
		case DW_OP_const4u: /* unsigned 4-byte constant */
		case DW_OP_const4s: /* signed   4-byte constant */
		case DW_OP_const8u: /* unsigned 8-byte constant */
		case DW_OP_const8s: /* signed   8-byte constant */
		case DW_OP_constu:  /* unsigned LEB128 constant */
		case DW_OP_consts:  /* signed   LEB128 constant */
		case DW_OP_plus_uconst: /* unsigned LEB128 addend */
			*value = loc_expr_oper[op_idx].number;
			break;

		/* supported 1-ary operations */
		case DW_OP_abs:
			*value = abs(loc_expr_oper[op_idx].number);
			break;
		case DW_OP_neg:
		case DW_OP_not:
			*value = !loc_expr_oper[op_idx].number;
			break;

		/* supported 2-ary operations */
		case DW_OP_and:
			*value = loc_expr_oper[op_idx].number;
			*value &= loc_expr_oper[op_idx].number2;
			break;
		case DW_OP_or:
			*value = loc_expr_oper[op_idx].number;
			*value |= loc_expr_oper[op_idx].number2;
			break;
		case DW_OP_xor:
			*value = loc_expr_oper[op_idx].number;
			*value ^= loc_expr_oper[op_idx].number2;
			break;
		case DW_OP_plus:
			*value = loc_expr_oper[op_idx].number;
			*value += loc_expr_oper[op_idx].number2;
			break;
		case DW_OP_minus:
			*value = loc_expr_oper[op_idx].number;
			*value -= loc_expr_oper[op_idx].number2;
			break;
		case DW_OP_mul:
			*value = loc_expr_oper[op_idx].number;
			*value *= loc_expr_oper[op_idx].number2;
			break;
		case DW_OP_div:
			*value = loc_expr_oper[op_idx].number;
			*value /= loc_expr_oper[op_idx].number2;
			break;
		case DW_OP_mod:
			*value = loc_expr_oper[op_idx].number;
			*value %= loc_expr_oper[op_idx].number2;
			break;

		/* sink */
		default:
			printf("Unsupported Dwarf operation %x.\n",
			       loc_expr_oper[op_idx].atom);
			break;
		}
	}

	return loc_expr_type;
}

static Dwarf_Word die_get_attr(Dwarf_Die *die, Dwarf_Half ar_attr)
{
	int attr_form;
	Dwarf_Word value = 0;
	Dwarf_Attribute attr;

	if (!dwarf_hasattr(die, ar_attr))
		return value;

	if (dwarf_attr(die, ar_attr, &attr) == NULL)
		return value;

	attr_form = dwarf_whatform(&attr);

	switch (attr_form) {
	case DW_FORM_data1:
	case DW_FORM_data2:
	case DW_FORM_data4:
	case DW_FORM_data8:
	case DW_FORM_sec_offset:
	case DW_FORM_sdata:
	case DW_FORM_udata:
	case DW_FORM_rnglistx:
	case DW_FORM_loclistx:
	case DW_FORM_implicit_const:
	case DW_FORM_GNU_addr_index:
	case DW_FORM_addrx:
	case DW_FORM_addrx1:
	case DW_FORM_addrx2:
	case DW_FORM_addrx3:
	case DW_FORM_addrx4:
		if (dwarf_formudata(&attr, &value) == -1)
			fail("Unable to get DWARF data for %s:0x%x:0x%x\n",
			     dwarf_diename(die), attr_form, ar_attr);
		break;
	case DW_FORM_block:
	case DW_FORM_block1:
	case DW_FORM_block2:
	case DW_FORM_block4:
		die_attr_eval_op(&attr, &value);
		break;
	default:
		fail("Unsupported DWARF form 0x%x for DIE %s, type 0x%x\n",
		     attr_form, dwarf_diename(die), ar_attr);
		break;
	}

	return value;
}

static unsigned int die_get_byte_size(Dwarf_Die *die, obj_t *obj)
{
	unsigned int byte_sz_1;
	unsigned int byte_sz_2;

	/**
	 * Make sure that this function will not be used on bitfields.
	 * This is not supported as space requirements in such a case are
	 * likely not to be divisible by CHAR_BIT and thus not applicable.
	 */
	assert(obj->is_bitfield == 0);

	if (obj->byte_size > 0)
		return obj->byte_size;

	/*
	 * Since any subset of {DW_AT_byte_size, DW_AT_bit_size} may be
	 * specified in DWARF for any given DIE, we need to check both to
	 * get byte size.
	 */
	byte_sz_1 = die_get_attr(die, DW_AT_byte_size);
	byte_sz_2 = die_get_attr(die, DW_AT_bit_size);

	assert(byte_sz_2 % CHAR_BIT == 0);

	byte_sz_2 /= CHAR_BIT;

	if (byte_sz_1 > 0 && byte_sz_2 > 0 && byte_sz_1 != byte_sz_2)
		fail("DIE %s: DW_AT_byte_size and DW_AT_bit_size differ\n",
		     dwarf_diename(die));

	if (byte_sz_1 > 0)
		return byte_sz_1;

	return byte_sz_2;
}

static obj_t *die_read_byte_size(Dwarf_Die *die, obj_t *obj)
{
	obj_t *ptr = obj;
	unsigned int coeff = 1;
	unsigned int byte_size = 0;

	while (ptr != NULL) {
		byte_size = die_get_byte_size(die, ptr);

		if (ptr->index && dwarf_tag(die) == DW_TAG_array_type)
			coeff *= ptr->index;

		if (byte_size > 0) {
			obj->byte_size = byte_size * coeff;
			break;
		}

		ptr = ptr->ptr;
	}

	return obj;
}

static obj_t *die_read_alignment(Dwarf_Die *die, obj_t *obj)
{
	obj->alignment = die_get_attr(die, DW_AT_alignment);
	return obj;
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

static struct record *record_alloc(void)
{
	struct record *rec;

	rec = safe_zmalloc(sizeof(*rec));
	return rec;
}

static void record_free_regular(struct record *rec)
{
	void *data;
	struct list_node *iter;

	free(rec->base_file);
	free(rec->origin);
	if (rec->cu)
		free(rec->cu);

	while ((data = stack_pop(rec->stack)) != NULL)
		free(data);
	stack_destroy(rec->stack);

	LIST_FOR_EACH(&rec->dependents, iter) {
		obj_t *o = list_node_data(iter);

		o->depend_rec_node = NULL;
	}
	list_clear(&rec->dependents);

	obj_free(rec->obj);
}

static void record_free_weak(struct record *rec)
{
	free(rec->link);
}

static void record_free(struct record *rec)
{
	free(rec->key);

	if (rec->free)
		rec->free(rec);
	free(rec);
}

static void record_put(struct record *rec)
{
	assert(rec->ref_count > 0);

	if (--rec->ref_count == 0)
		record_free(rec);
}

static void record_get(struct record *rec)
{
	rec->ref_count++;
}

static void record_dump_regular(struct record *rec, FILE *f);

static struct record *record_new_regular(const char *key)
{
	struct record *rec;

	rec = record_alloc();
	rec->key = safe_strdup(key);
	rec->stack = stack_init();
	rec->free = record_free_regular;
	rec->dump = record_dump_regular;
	list_init(&rec->dependents, NULL);
	record_get(rec);
	return rec;
}

static void record_dump_assembly(struct record *rec, FILE *f);

static struct record *record_new_assembly(const char *key)
{
	struct record *rec;

	rec = record_alloc();
	rec->key = safe_strdup(key);

	/*
	 * The symbol not necessary belongs to an assembly function,
	 * it is actually "no definition found in the debug information",
	 * but the main goal is to find the assembly ones.
	 */

	rec->free = NULL;
	rec->dump = record_dump_assembly;

	record_get(rec);

	return rec;
}

static void record_dump_weak(struct record *rec, FILE *f);

static struct record *record_new_weak(const char *key, const char *link)
{
	struct record *rec;

	rec = record_alloc();
	rec->key = safe_strdup(key);
	rec->link = safe_strdup(link);

	rec->free = record_free_weak;
	rec->dump = record_dump_weak;

	record_get(rec);

	return rec;
}

static obj_t *record_obj(struct record *rec)
{
	return rec->obj;
}

static obj_t *record_obj_exchange(struct record *rec, obj_t *o)
{
	obj_t *old;

	old = rec->obj;
	rec->obj = o;
	return old;
}

static const char *record_origin(struct record *rec)
{
	return rec->origin;
}

static void copy_stack_cb(void *data, void *arg)
{
	char *symbol = (char *)data;
	struct record *fp = (struct record *)arg;
	char *copy;

	copy = safe_strdup(symbol);
	stack_push(fp->stack, copy);
}

static void record_add_stack(struct record *rec, stack_t *stack)
{
	walk_stack_backward(stack, copy_stack_cb, rec);
}

static void record_add_cu(struct record *rec, Dwarf_Die *cu_die)
{
	const char *name;

	if (cu_die == NULL)
		return;

	name = dwarf_diename(cu_die);
	safe_asprintf(&rec->cu, "CU: \"%s\"\n", name);
}

static void record_add_origin(struct record *rec,
			      Dwarf_Die *cu_die,
			      Dwarf_Die *die)
{
	char *dec_file;
	long dec_line;

	dec_file = get_file(cu_die, die);
	dec_line = get_line(cu_die, die);

	safe_asprintf(&rec->origin, "File: %s:%lu\n", dec_file, dec_line);
	free(dec_file);
}

static struct record *record_start(struct cu_ctx *ctx,
				   Dwarf_Die *die,
				   char *key)
{
	struct record *rec = NULL;
	generate_config_t *conf = ctx->conf;
	Dwarf_Die *cu_die = ctx->cu_die;

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
	if (set_contains(ctx->processed, key))
		goto done;

	set_add(ctx->processed, key);

	if (is_declaration(die)) {
		if (conf->verbose)
			printf("WARNING: Skipping following file as we "
			       "have only declaration: %s\n", key);
		goto done;
	}

	if (conf->verbose)
		printf("Generating %s\n", key);

	rec = record_new_regular(key);

	if (conf->gen_extra)
		record_add_cu(rec, cu_die);
	record_add_origin(rec, cu_die, die);
	record_add_stack(rec, ctx->stack);
done:
	return rec;
}

static void record_set_version(struct record *rec, int version)
{
	char *base_file = rec->base_file;
	char *key = NULL;

	if (version == 0)
		return;

	if (rec->version == 0) {
		base_file = safe_strdup(rec->key);
		/* Remove .txt ending */
		base_file[strlen(base_file) - 4] = '\0';
		rec->base_file = base_file;
	}
	rec->version = version;
	safe_asprintf(&key, "%s-%i.txt", base_file, rec->version);
	free(rec->key);
	rec->key = key;
}

static void record_close(struct record *rec, obj_t *obj)
{
	obj_fill_parent(obj);
	rec->obj = obj;
}

static void record_stack_dump_and_clear(struct record *rec, FILE *f)
{
	char *data = stack_pop(rec->stack);

	if (data == NULL)
		return;

	fprintf(f, "Stack:\n");
	do {
		fprintf(f, "-> \"%s\"\n", data);
		free(data);
	} while ((data = stack_pop(rec->stack)) != NULL);
}

static void record_dump_regular(struct record *rec, FILE *f)
{
	int rc;

	fprintf(f, FILEFMT_VERSION_STRING);
	if (rec->cu != NULL) {
		rc = fputs(rec->cu, f);
		if (rc == EOF)
			fail("Could not put CU name");
	}
	rc = fputs(rec->origin, f);
	if (rc == EOF)
		fail("Could not put origin");

	record_stack_dump_and_clear(rec, f);

	fprintf(f, "Symbol:\n");
	if (rec->obj->byte_size != 0)
		fprintf(f, "Byte size %u\n", rec->obj->byte_size);
	if (rec->obj->alignment != 0)
		fprintf(f, "Alignment %u\n", rec->obj->alignment);

	obj_dump(rec->obj, f);
}

static void record_dump_assembly(struct record *rec, FILE *f)
{
	char *name = filenametosymbol(rec->key);
	int rc;

	rc = fprintf(f, FILEFMT_VERSION_STRING "Symbol:\nassembly %s\n", name);
	free(name);
	if (rc < 0)
		fail("Could not put assembly\n");
}

static void record_dump_weak(struct record *rec, FILE *f)
{
	char *name = filenametosymbol(rec->key);
	int rc;

	rc = fprintf(f, FILEFMT_VERSION_STRING "Symbol:\nweak %s -> %s\n",
		     name, rec->link);
	free(name);
	if (rc < 0)
		fail("Could not put weak link\n");
}

static void record_dump(struct record *rec, const char *dir)
{
	char path[PATH_MAX];
	FILE *f;
	char *slash;

	snprintf(path, sizeof(path), "%s/%s", dir, rec->key);

	slash = strrchr(path, '/');
	assert(slash != NULL);
	*slash = '\0';
	rec_mkdir(path);
	*slash = '/';

	f = fopen(path, "w");
	if (f == NULL)
		fail("Cannot create record file '%s': %m", path);

	rec->dump(rec, f);

	fclose(f);
}

static void list_record_free(void *value)
{
	struct record *rec = value;

	record_put(rec);
}

/*
 * merge rec_src to the record rec_dst
 */
static bool record_merge(struct record *rec_dst,
			 struct record *rec_src,
			 bool merge_decl)
{
	const char *s1;
	const char *s2;
	obj_t *o1;
	obj_t *o2;
	obj_t *o;

	s1 = record_origin(rec_dst);
	s2 = record_origin(rec_src);

	if (!safe_streq(s1, s2))
		return false;

	o1 = record_obj(rec_dst);
	o2 = record_obj(rec_src);

	o = obj_merge(o1, o2, merge_decl);
	if (o == NULL)
		return false;

	obj_fill_parent(o);
	o = record_obj_exchange(rec_dst, o);
	obj_free(o);

	return true;
}

static char *record_db_add(struct record_db *db, struct record *rec)
{
	/*
	 * Now we need to put the new type record we've just generated
	 * to the db.
	 *
	 * The problem stopping us from merging every record possible is that
	 * multiple records can't be merged if it is not possible to create
	 * groups in which they should be merged. This is not a problem if all
	 * the records are able to be merged together, but when there is one or
	 * more incompatible merging then merging them aggressively could group
	 * them in the wrong way. And because the right way to group them can't
	 * be decided, it is better not to group and subsequently merge them at
	 * all.
	 *
	 * Meaning that at this stage, when we don't know all the records, we
	 * can't decide if all records can be merged into one, and so the only
	 * records we can merge are those that are completely same.
	 *
	 * In case they aren't same we store them as another node of the list.
	 * We also change the name of the new record, so that two reffile obj_t
	 * referencing records that we couldn't merge wouldn't get merged.
	 */

	struct hash *hash = (struct hash *)db;
	struct record *tmp_rec;
	struct list *list;
	struct list_node *iter;

	list = hash_find(hash, rec->key);
	if (list == NULL) {
		list = list_new(list_record_free);

		hash_add(hash, rec->key, list);
	}

	LIST_FOR_EACH(list, iter) {
		tmp_rec = list_node_data(iter);

		if (record_merge(tmp_rec, rec, NO_MERGE_DECL)) {
			list_concat(&tmp_rec->dependents, &rec->dependents);
			return safe_strdup(tmp_rec->key);
		}
	}

	record_get(rec);
	record_set_version(rec, list->len);
	list_add(list, rec);

	return safe_strdup(rec->key);
}

static void hash_list_free(void *value)
{
	struct list *list = value;

	list_free(list);
}

static struct record_db *record_db_init(void)
{
	struct hash *db;

	db = hash_new(DB_SIZE, hash_list_free);
	if (db == NULL)
		fail("Could not create db (hash)\n");

	return (struct record_db *)db;
}

static void record_db_dump(struct record_db *_db, char *dir)
{
	struct hash_iter iter;
	const void *v;
	struct hash *db = (struct hash *)_db;

	hash_iter_init(db, &iter);
	while (hash_iter_next(&iter, NULL, &v)) {
		struct list *list = (struct list *)v;
		struct list_node *iter;

		LIST_FOR_EACH(list, iter) {
			struct record *rec = list_node_data(iter);

			record_dump(rec, dir);
		}
	}
}

static void record_db_free(struct record_db *_db)
{
	struct hash *db = (struct hash *)_db;

	hash_free(db);
}

static obj_t *print_die_type(struct cu_ctx *ctx,
			     struct record *rec,
			     Dwarf_Die *die)
{
	Dwarf_Die type_die;
	Dwarf_Attribute attr;

	if (!dwarf_hasattr(die, DW_AT_type))
		return obj_basetype_new(safe_strdup("void"));

	(void) dwarf_attr(die, DW_AT_type, &attr);
	if (dwarf_formref_die(&attr, &type_die) == NULL)
		fail("dwarf_formref_die() failed for %s\n",
		    dwarf_diename(die));

	if (dwarf_hasattr(&type_die, DW_AT_endianity))
		fail("DIE %s has non-standard endianity\n",
		     dwarf_diename(&type_die))

	/* Print the type of the die */
	return print_die(ctx, rec, &type_die);
}

static obj_t *print_die_struct_member(struct cu_ctx *ctx,
				      struct record *rec,
				      Dwarf_Die *die,
				      const char *name)
{
	obj_t *type;
	obj_t *obj;
	Dwarf_Half dw_attr_bit_offset;
	unsigned int bit_offset = 0;

	type = print_die_type(ctx, rec, die);
	obj = obj_struct_member_new_add(safe_strdup(name), type);
	die_read_alignment(die, obj);

	/*
	 * DWARF attribute specifying offset varies depending on DWARF version.
	 * DW_AT_data_member_location is not guaranteed to be emitted; a fall-
	 * back attribute DW_AT_data_bit_offset (present in DWARF v4 and later)
	 * is used when not encountered.
	 */
	if (dwarf_hasattr(die, DW_AT_data_member_location))
		obj->offset = die_get_attr(die, DW_AT_data_member_location);
	else if (dwarf_hasattr(die, DW_AT_data_bit_offset))
		obj->offset = die_get_attr(die, DW_AT_data_bit_offset)/CHAR_BIT;

	/*
	 * DWARF attribute specifying bit-offset. Note that DW_AT_bit_offset
	 * is endian-sensitive, whereas DW_AT_data_bit_offset is not.
	 * Presence of this attribute indicates that we're dealing with
	 * bit-field.
	 */
	if (dwarf_hasattr(die, DW_AT_bit_offset))
		dw_attr_bit_offset = DW_AT_bit_offset;
	else if (dwarf_hasattr(die, DW_AT_data_bit_offset))
		dw_attr_bit_offset = DW_AT_data_bit_offset;
	else
		goto out;

	/*
	 * Bit-field section; offset, first and last bits are converted to
	 * DWARF5-compliant (endian-oblivious) format.
	 */
	obj->is_bitfield = 1;

	if (dwarf_hasattr(die, DW_AT_data_bit_offset)) {
		bit_offset = die_get_attr(die, dw_attr_bit_offset);
	} else if (ctx->elf_endian == ELFDATA2MSB) {
		bit_offset = die_get_attr(die, dw_attr_bit_offset) \
			   + obj->offset*CHAR_BIT;
	} else {
		bit_offset = die_get_attr(die, DW_AT_byte_size) * CHAR_BIT \
			   + obj->offset * CHAR_BIT \
			   - die_get_attr(die, DW_AT_bit_offset) \
			   - die_get_attr(die, DW_AT_bit_size);
	}

	obj->offset = bit_offset / CHAR_BIT;
	obj->first_bit = bit_offset % CHAR_BIT;
	obj->last_bit  = die_get_attr(die, DW_AT_bit_size) + obj->first_bit;

out:
	return obj;
}

static obj_t *print_die_structure(struct cu_ctx *ctx,
				  struct record *rec,
				  Dwarf_Die *die)
{
	const char *name = get_die_name(die);
	unsigned int tag;
	obj_list_head_t *members = NULL;
	obj_t *obj;
	obj_t *member;
	Dwarf_Die child_die;

	obj = obj_struct_new(safe_strdup(name));

	if (!dwarf_haschildren(die))
		goto done;

	dwarf_child(die, &child_die);
	do {
		Dwarf_Die *die = &child_die;

		name = get_die_name(die);
		tag = dwarf_tag(die);
		if (tag != DW_TAG_member)
			fail("Unexpected tag for structure type children: "
			    "%s\n", dwarf_tag_string(tag));

		member = print_die_struct_member(ctx, rec, die, name);
		if (members == NULL)
			members = obj_list_head_new(member);
		else
			obj_list_add(members, member);

	} while (dwarf_siblingof(&child_die, &child_die) == 0);

	obj->member_list = members;
done:
	return obj;
}

static obj_t *print_die_enumerator(struct cu_ctx *ctx,
				   struct record *rec,
				   Dwarf_Die *die,
				   const char *name)
{
	Dwarf_Attribute attr;
	Dwarf_Word value;
	obj_t *obj;

	if (dwarf_attr(die, DW_AT_const_value, &attr) == NULL)
		fail("Value of enumerator %s missing!\n", name);

	(void) dwarf_formudata(&attr, &value);

	obj = obj_constant_new(safe_strdup(name));
	obj->constant = value;

	return obj;
}

static obj_t *print_die_enumeration(struct cu_ctx *ctx,
				    struct record *rec,
				    Dwarf_Die *die) {
	const char *name = get_die_name(die);
	obj_list_head_t *members = NULL;
	obj_t *member;
	obj_t *obj;
	Dwarf_Die child_die;

	obj = obj_enum_new(safe_strdup(name));

	if (!dwarf_haschildren(die))
		goto done;

	dwarf_child(die, &child_die);
	do {
		Dwarf_Die *die = &child_die;

		name = get_die_name(die);
		member = print_die_enumerator(ctx, rec, die, name);
		if (members == NULL)
			members = obj_list_head_new(member);
		else
			obj_list_add(members, member);
	} while (dwarf_siblingof(&child_die, &child_die) == 0);

	members->object = obj;
	obj->member_list = members;
done:
	return obj;
}

static obj_t *print_die_union(struct cu_ctx *ctx,
			      struct record *rec,
			      Dwarf_Die *die)
{
	const char *name = get_die_name(die);
	unsigned int tag;
	obj_list_head_t *members = NULL;
	obj_t *member;
	obj_t *type;
	obj_t *obj;
	Dwarf_Die child_die;

	obj = obj_union_new(safe_strdup(name));

	if (!dwarf_haschildren(die))
		goto done;

	dwarf_child(die, &child_die);
	do {
		Dwarf_Die *die = &child_die;

		name = get_die_name(die);
		tag = dwarf_tag(die);
		if (tag != DW_TAG_member)
			fail("Unexpected tag for union type children: %s\n",
			    dwarf_tag_string(tag));

		type = print_die_type(ctx, rec, die);
		member = obj_var_new_add(safe_strdup(name), type);

		if (members == NULL)
			members = obj_list_head_new(member);
		else
			obj_list_add(members, member);

	} while (dwarf_siblingof(&child_die, &child_die) == 0);

	members->object = obj;
	obj->member_list = members;
done:
	die_read_alignment(die, obj);
	return obj;
}

static obj_list_head_t *print_subprogram_arguments(struct cu_ctx *ctx,
						   struct record *rec,
						   Dwarf_Die *die)
{
	Dwarf_Die child_die;
	obj_t *arg_type;
	obj_t *arg;
	obj_list_head_t *arg_list = NULL;

	if (!dwarf_haschildren(die))
		return NULL;

	/* Grab the first argument */
	dwarf_child(die, &child_die);

	/* Walk all arguments until we run into the function body */
	while ((dwarf_tag(&child_die) == DW_TAG_formal_parameter) ||
	    (dwarf_tag(&child_die) == DW_TAG_unspecified_parameters)) {
		const char *name = get_die_name(&child_die);

		if (dwarf_tag(&child_die) != DW_TAG_unspecified_parameters)
			arg_type = print_die_type(ctx, rec, &child_die);
		else
			arg_type = obj_basetype_new(safe_strdup("..."));

		arg = obj_var_new_add(safe_strdup(name), arg_type);
		if (arg_list == NULL)
			arg_list = obj_list_head_new(arg);
		else
			obj_list_add(arg_list, arg);

		if (dwarf_siblingof(&child_die, &child_die) != 0)
			break;
	}
	return arg_list;
}

static obj_t *print_die_subprogram(struct cu_ctx *ctx,
				   struct record *rec,
				   Dwarf_Die *die)
{
	char *name;
	obj_list_head_t *arg_list;
	obj_t *ret_type;
	obj_t *obj;

	arg_list = print_subprogram_arguments(ctx, rec, die);
	ret_type = print_die_type(ctx, rec, die);
	name = safe_strdup(get_die_name(die));

	obj = obj_func_new_add(name, ret_type);
	if (arg_list)
		arg_list->object = obj;
	obj->member_list = arg_list;

	return obj;
}

static obj_t *_print_die_array_type(struct cu_ctx *ctx,
				    struct record *rec,
				    Dwarf_Die *child,
				    obj_t *base_type)
{
	Dwarf_Die next_child;
	Dwarf_Word value;
	Dwarf_Attribute attr;
	int rc;
	unsigned int tag;
	unsigned long arr_idx;
	obj_t *obj;
	obj_t *sub;

	if (child == NULL)
		return base_type;

	tag = dwarf_tag(child);
	if (tag != DW_TAG_subrange_type)
		fail("Unexpected tag for array type children: %s\n",
		     dwarf_tag_string(tag));

	if (dwarf_hasattr(child, DW_AT_upper_bound)) {
		(void) dwarf_attr(child, DW_AT_upper_bound, &attr);
		(void) dwarf_formudata(&attr, &value);
		/* Get the UPPER bound, so add 1 */
		arr_idx = value + 1;
	} else if (dwarf_hasattr(child, DW_AT_count)) {
		(void) dwarf_attr(child, DW_AT_count, &attr);
		(void) dwarf_formudata(&attr, &value);
		arr_idx = value;
	} else {
		arr_idx = 0;
	}

	rc = dwarf_siblingof(child, &next_child);
	child = rc == 0 ? &next_child : NULL;

	sub = _print_die_array_type(ctx, rec, child, base_type);
	obj = obj_array_new_add(sub);
	obj->index = arr_idx;

	return obj;
}

static obj_t *print_die_array_type(struct cu_ctx *ctx,
				   struct record *rec,
				   Dwarf_Die *die)
{
	Dwarf_Die child;
	obj_t *base_type;

	/* There should be one child of DW_TAG_subrange_type */
	if (!dwarf_haschildren(die))
		fail("Array type missing children!\n");

	base_type = print_die_type(ctx, rec, die);

	/* Grab the child */
	dwarf_child(die, &child);

	return _print_die_array_type(ctx, rec, &child, base_type);
}

static obj_t *print_die_tag(struct cu_ctx *ctx,
			    struct record *rec,
			    Dwarf_Die *die)
{
	unsigned int tag = dwarf_tag(die);
	const char *name = dwarf_diename(die);
	obj_t *obj = NULL;

	if (tag == DW_TAG_invalid)
		fail("DW_TAG_invalid: %s\n", name);

	switch (tag) {
	case DW_TAG_subprogram:
		obj = print_die_subprogram(ctx, rec, die);
		break;
	case DW_TAG_variable:
		obj = print_die_type(ctx, rec, die);
		obj = obj_var_new_add(safe_strdup(name), obj);
		break;
	case DW_TAG_base_type:
		obj = obj_basetype_new(safe_strdup(name));
		break;
	case DW_TAG_pointer_type:
		obj = print_die_type(ctx, rec, die);
		obj = obj_ptr_new_add(obj);
		break;
	case DW_TAG_structure_type:
		obj = print_die_structure(ctx, rec, die);
		break;
	case DW_TAG_enumeration_type:
		obj = print_die_enumeration(ctx, rec, die);
		break;
	case DW_TAG_union_type:
		obj = print_die_union(ctx, rec, die);
		break;
	case DW_TAG_typedef:
		obj = print_die_type(ctx, rec, die);
		obj = obj_typedef_new_add(safe_strdup(name), obj);
		break;
	case DW_TAG_subroutine_type:
		obj = print_die_subprogram(ctx, rec, die);
		break;
	case DW_TAG_volatile_type:
		obj = print_die_type(ctx, rec, die);
		obj = obj_qualifier_new_add(obj);
		obj->base_type = safe_strdup("volatile");
		break;
	case DW_TAG_const_type:
		obj = print_die_type(ctx, rec, die);
		obj = obj_qualifier_new_add(obj);
		obj->base_type = safe_strdup("const");
		break;
	case DW_TAG_array_type:
		obj = print_die_array_type(ctx, rec, die);
		break;
	default: {
		const char *tagname = dwarf_tag_string(tag);
		if (tagname == NULL)
			tagname = "<NO TAG>";

		fail("Unexpected tag for symbol %s: %s\n", name, tagname);
		break;
	}
	}

	if (tag != DW_TAG_subprogram && tag != DW_TAG_subroutine_type)
		obj = die_read_byte_size(die, obj);

	obj = die_read_alignment(die, obj);
	return obj;
}

static obj_t *print_die(struct cu_ctx *ctx,
			struct record *parent_file,
			Dwarf_Die *die)
{
	char *file;
	struct record *rec;
	char *old_file;
	obj_t *obj;
	obj_t *ref_obj;
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
	if (file == NULL) {
		/* no need for new record, output to the current one */
		assert(parent_file != NULL);
		obj = print_die_tag(ctx, parent_file, die);
		return obj;
	}

	ref_obj = obj_reffile_new();

	/* else handle new record */
	rec = record_start(ctx, die, file);
	if (rec == NULL)
		/* declaration or already processed */
		goto out;

	if (conf->gen_extra)
		stack_push(ctx->stack, safe_strdup(file));
	obj = print_die_tag(ctx, rec, die);
	if (conf->gen_extra)
		free(stack_pop(ctx->stack));

	record_close(rec, obj);

	old_file = file;
	ref_obj->depend_rec_node = list_add(&rec->dependents, ref_obj);
	/* if it creates new version, key/file name can change */
	file = record_db_add(conf->db, rec);
	record_put(rec);
	/* record_db_add() returns allocated string */
	free(old_file);

out:
	ref_obj->base_type = file;
	return ref_obj;
}

/*
 * Validate if this is the symbol we should print.
 * Returns true if should.
 */
static bool is_symbol_valid(struct file_ctx *fctx, Dwarf_Die *die)
{
	const char *name = dwarf_diename(die);
	unsigned int tag = dwarf_tag(die);
	bool result = false;
	generate_config_t *conf = fctx->conf;
	struct ksym *ksym1 = NULL;
	struct ksym *ksym2;

	/* Shortcut, unnamed die cannot be part of whitelist */
	if (name == NULL)
		goto out;

	/* If symbol file was provided, is the symbol on the list? */
	if (conf->symbols != NULL) {
		ksym1 = ksymtab_find(conf->symbols, name);
		if (ksym1 == NULL)
			goto out;
	}

	/* Is this symbol exported in this module with EXPORT_SYMBOL? */
	ksym2 = ksymtab_find(fctx->ksymtab, name);
	if (ksym2 == NULL)
		goto out;

	/* We don't care about declarations */
	if (is_declaration(die))
		goto out;
	/*
	 * Mark the symbol as not eligible to fake symbol generation.
	 * We can come till this place with an assembly function,
	 * because it may have dwarf info for its C declaration,
	 * but others in dwarf are supposed to be normal C functions.
	 */
	ksymtab_ksym_mark(ksym2);

	/* Anything EXPORT_SYMBOLed should be external */
	if (!is_external(die))
		goto out;

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
	default:
		fail("Symbol %s has unexpected tag: %s!\n", name,
		    dwarf_tag_string(tag));
	}

	result = true;

	/*
	 * Mark the symbol as fully processed,
	 * so it will not be in the subset of not found symbols.
	 * We are talking here about kabi symbols set,
	 * which is passed by -s switch.
	 *
	 * The actual processing starts later in the caller,
	 * but the decision is made here.
	 */
	if (conf->symbols != NULL)
		ksymtab_ksym_mark(ksym1);

out:
	return result;
}

/*
 * Walk all DIEs in a CU.
 * Returns true if the given symbol_name was found, otherwise false.
 */
static void process_cu_die(Dwarf_Die *cu_die, struct file_ctx *fctx)
{
	Dwarf_Die child_die;
	bool cu_printed = false;
	obj_t *ref;
	generate_config_t *conf = fctx->conf;

	if (!dwarf_haschildren(cu_die))
		return;

	/* Walk all DIEs in the CU */
	dwarf_child(cu_die, &child_die);
	do {
		void *data;
		struct cu_ctx ctx;

		if (!is_symbol_valid(fctx, &child_die))
			continue;

		if (!cu_printed && conf->verbose) {
			printf("Processing CU %s\n",
			       dwarf_diename(cu_die));
			cu_printed = true;
		}

		ctx.dw_version = fctx->dw_version;
		ctx.elf_endian = fctx->elf_endian;
		ctx.conf = conf;
		ctx.cu_die = cu_die;

		/* Grab a fresh stack of symbols */
		ctx.stack = stack_init();
		/* And a set of all processed symbols */
		ctx.processed = set_init(PROCESSED_SIZE);

		/* Print both the CU DIE and symbol DIE */
		ref = print_die(&ctx, NULL, &child_die);
		obj_free(ref);

		/* And clear the stack again */
		while ((data = stack_pop(ctx.stack)) != NULL)
			free(data);

		stack_destroy(ctx.stack);
		set_free(ctx.processed);
	} while (dwarf_siblingof(&child_die, &child_die) == 0);
}

static int dwflmod_generate_cb(Dwfl_Module *dwflmod, void **userdata,
		const char *name, Dwarf_Addr base, void *arg)
{
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
	    &addresssize, &offsetsize, NULL, &type_offset) == 0) {
		fctx->dw_version = version;

		if (version < 2 || version > 5)
			fail("Unsupported dwarf version: %d\n", version);

		/* CU is followed by a single DIE */
		Dwarf_Die cu_die;
		if (dwarf_offdie(dbg, old_off + hsize, &cu_die) == NULL) {
			fail("dwarf_offdie failed for cu!\n");
		}

		process_cu_die(&cu_die, fctx);

		old_off = off;
	}

	return DWARF_CB_OK;
}

static void generate_type_info(char *filepath, struct file_ctx *ctx)
{
	static const Dwfl_Callbacks callbacks = {
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

static bool is_all_done(generate_config_t *conf)
{
	if (conf->symbols == NULL)
		return false;

	return ksymtab_mark_count(conf->symbols) == conf->symbol_cnt;
}

static void generate_assembly_record(generate_config_t *conf, const char *key)
{
	struct record *rec;
	char *new_key, *name;

	if (conf->verbose)
		printf("Generating assembly record for %s\n", key);

	safe_asprintf(&name, "asm--%s.txt", key);

	rec = record_new_assembly(name);
	new_key = record_db_add(conf->db, rec);

	record_put(rec);
	free(name);
	free(new_key);
}

static bool try_generate_alias(generate_config_t *conf, struct ksym *ksym)
{
	char *link = ksymtab_ksym_get_link(ksym);
	const char *key = ksymtab_ksym_get_name(ksym);
	struct record *rec;
	char *new_key, *name;

	if (!link)
		return false;

	if (conf->verbose)
		printf("Generating weak record %s -> %s\n",
		       key, link);

	safe_asprintf(&name, "weak--%s.txt", key);

	rec = record_new_weak(name, link);
	new_key = record_db_add(conf->db, rec);

	record_put(rec);
	free(name);
	free(new_key);

	return true;
}

/*
 * process_not_found:
 *
 * Generate fake records for symbols, not found in the debug info,
 * but existed in the export list (most probably, assembly function).
 *
 * If there is a checklist, mark the symbol there (as processed).
 * If the symbol not in the checklist, ignore it completely,
 * means, do not mark and do not generate fake record for it either.
 */
static void process_not_found(struct ksym *exported, void *ctx)
{
	generate_config_t *conf = ctx;
	struct ksym *ksym;
	const char *key = ksymtab_ksym_get_name(exported);

	if (ksymtab_ksym_is_marked(exported))
		return;

	if (conf->symbols) {
		ksym = ksymtab_find(conf->symbols, key);
		if (ksym == NULL)
			return;
		ksymtab_ksym_mark(ksym);
	}

	if (!try_generate_alias(conf, exported))
		generate_assembly_record(conf, key);
}

static void ksymtab_add_alias(struct ksym *ksym, void *ctx)
{
	struct ksymtab *ksymtab = ctx;

	ksymtab_copy_sym(ksymtab, ksym);
}

static void ksymtab_add_and_link_alias(struct ksym *ksym, void *ctx)
{
	struct ksymtab *ksymtab = ctx;
	char *link;
	const char *name;
	struct ksym *link_ksym;

	link = ksymtab_ksym_get_link(ksym);
	link_ksym = ksymtab_find(ksymtab, link);

	/* if we linked, there must be the symbol in the symtab */
	assert(link_ksym != NULL);

	name = ksymtab_ksym_get_name(ksym);
	ksymtab_ksym_set_link(link_ksym, name);

	ksymtab_copy_sym(ksymtab, ksym);
}

static void merge_aliases(struct ksymtab *ksymtab,
			  struct ksymtab *symbols,
			  struct ksymtab *aliases)
{
	ksymtab_for_each(aliases, ksymtab_add_and_link_alias, ksymtab);
	if (symbols != NULL)
		ksymtab_for_each(aliases, ksymtab_add_alias, symbols);
}

static walk_rv_t process_symbol_file(char *path, void *arg)
{
	unsigned int endianness;
	struct elf_data *elf;
	struct file_ctx fctx;
	generate_config_t *conf = (generate_config_t *)arg;
	struct ksymtab *ksymtab;
	struct ksymtab *aliases = NULL;
	walk_rv_t ret = WALK_CONT;

	/* We want to process only .ko kernel modules and vmlinux itself */
	if (!safe_strendswith(path, ".ko") &&
	    !safe_strendswith(path, "/vmlinux")) {
		if (conf->kernel_dir) {
			if (conf->verbose)
				printf("Skip non-object file %s\n", path);
			return ret;
		} else {
			if (conf->verbose)
				printf("Force processing file %s\n", path);
		}
	}

	/*
	 * Don't look into RHEL build cache directories.
	 */
	if (conf->rhel_tree) {
		if (strstr(path, "redhat/rpm") != NULL)
			return WALK_SKIP;
	}

	elf = elf_open(path);
	if (elf == NULL) {
		if (conf->verbose)
			printf("Skip %s (unable to process ELF file)\n",
			       path);
		goto out;
	}

	if (elf_get_endianness(elf, &endianness) > 0)
		goto clean_elf;

	if (elf_get_exported(elf, &ksymtab, &aliases) > 0)
		goto clean_elf;

	if (ksymtab_len(ksymtab) == 0) {
		if (conf->verbose)
			printf("Skip %s (no exported symbols)\n", path);
		goto clean_ksymtab;
	}

	merge_aliases(ksymtab, conf->symbols, aliases);

	fctx.conf = conf;
	fctx.ksymtab = ksymtab;
	fctx.elf_endian = endianness;

	if (conf->verbose)
		printf("Processing %s\n", path);

	generate_type_info(path, &fctx);
	ksymtab_for_each(ksymtab, process_not_found, conf);

	if (is_all_done(conf))
		ret = WALK_STOP;
clean_ksymtab:
	ksymtab_free(aliases);
	ksymtab_free(ksymtab);
clean_elf:
	elf_close(elf);
	free(elf->ehdr);
	free(elf);
out:
	return ret;
}

static void print_not_found(struct ksym *ksym, void *ctx)
{
	const char *s = ksymtab_ksym_get_name(ksym);

	if (ksymtab_ksym_is_marked(ksym))
		return;
	printf("%s not found!\n", s);
}

struct record *record_copy(struct record *src)
{
	struct record *res = record_new_regular("");
	obj_t *o1 = record_obj(src);

	res->obj = obj_merge(o1, o1, MERGE_DECL);
	obj_fill_parent(res->obj);
	res->origin = safe_strdup(src->origin);
	res->base_file = NULL;

	return res;
}

bool record_list_can_merge(struct list *rec_list)
{
	bool result = true;
	struct record *merger;
	struct list_node *iter;

	if (list_len(rec_list) <= 1) {
		/* only one record -> nothing to merge */
		return false;
	}

	merger = record_copy(list_node_data(rec_list->first));
	LIST_FOR_EACH(rec_list, iter) {
		struct record *record = list_node_data(iter);

		if (!record_merge(merger, record, MERGE_DECL)) {
			result = false;
			break;
		}
	}
	record_put(merger);

	return result;
}

void record_list_merge(struct list *rec_list)
{
	struct record *first = rec_list->first->data;
	struct list_node *next = rec_list->first->next;
	struct list_node *curr;

	struct record *record;

	while (next != NULL) {
		curr = next;
		next = next->next;

		record = curr->data;

		list_concat(&first->dependents, &record->dependents);
		record_merge(first, record, MERGE_DECL);
		record_put(record);

		free(curr);
	}

	record_update_dependents(first);
	rec_list->first->next = NULL;
	rec_list->last = rec_list->first;
	rec_list->len = 1;
}

void record_db_merge(struct record_db *db)
{
	struct hash *hash = (struct hash *)db;
	bool merged;
	struct hash_iter iter;
	const char *key;
	const void *val;

	do {
		merged = false;

		hash_iter_init(hash, &iter);
		while (hash_iter_next(&iter, &key, &val)) {
			struct list *list = (struct list *)val;

			if (list != NULL && record_list_can_merge(list)) {
				record_list_merge(list);
				merged = true;
			}
		}
	} while (merged);
}

/*
 * Print symbol definition by walking all DIEs in a .debug_info section.
 * Returns true if the definition was printed, otherwise false.
 */
static void generate_symbol_defs(generate_config_t *conf)
{
	struct stat st;

	if (stat(conf->kernel_dir, &st) != 0)
		fail("Failed to stat %s: %s\n", conf->kernel_dir,
		    strerror(errno));

	/* Lets walk the normal modules */
	printf("Generating symbol defs from %s\n", conf->kernel_dir);

	conf->db = record_db_init();

	if (S_ISDIR(st.st_mode)) {
		walk_dir(conf->kernel_dir, false, process_symbol_file, conf);
	} else if (S_ISREG(st.st_mode)) {
		char *path = conf->kernel_dir;
		conf->kernel_dir = NULL;
		process_symbol_file(path, conf);
	} else {
		fail("Not a file or directory: %s\n", conf->kernel_dir);
	}

	ksymtab_for_each(conf->symbols, print_not_found, NULL);

	record_db_merge(conf->db);

	record_db_dump(conf->db, conf->kabi_dir);
	record_db_free(conf->db);
}

#define	WHITESPACE	" \t\n"

/* Remove white characters from given buffer */
static void strip(char *buf)
{
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

/*
 * Check if the string is valid C identifier.
 * We do this so we can easily provide the standard kabi whitelist file as the
 * symbol list.
 */
static bool is_valid_c_identifier(char *s)
{
	int i, len;

	if (s == NULL)
		return false;

	len = strlen(s);
	if (len == 0)
		return false;
	if (s[0] != '_' && !isalpha(s[0]))
		return false;

	for (i = 1; i < len; i++) {
		if (s[i] != '_' && !isalnum(s[i]))
			return false;
	}

	return true;
}

static bool is_kabi_header(char *s)
{
	const char *suffix = "_whitelist]";
	int suffixlen = strlen(suffix);
	int len;

	assert(s != NULL);

	len = strlen(s);
	if (len <= suffixlen + 1)
		return false;

	if (s[0] != '[')
		return false;

	if (strcmp(s + (len - suffixlen), suffix) != 0)
		return false;

	return true;
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
		if (!is_valid_c_identifier(line)) {
			if (!is_kabi_header(line)) {
				printf("WARNING: Ignoring line \'%s\' from the"
				    " symbols file as it's not a valid C "
				    "identifier.\n", line);
			}
			continue;
		}
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

static void generate_usage()
{
	printf("Usage:\n"
	       "\tgenerate [options] kernel_dir\n"
	       "\nOptions:\n"
	       "    -h, --help:\t\tshow this message\n"
	       "    -v, --verbose:\tdisplay debug information\n"
	       "    -o, --output kabi_dir:\n\t\t\t"
	       "where to write kabi files (default: \"output\")\n"
	       "    -s, --symbols symbol_file:\n\t\t\ta file containing the"
	       " list of symbols of interest (e.g. whitelisted)\n"
	       "    -r, --rhel:\n\t\t\trun on the RHEL build tree\n"
	       "    -a, --abs-path abs_path:\n\t\t\t"
	       "replace the absolute path by a relative path\n"
	       "    -g, --generate-extra-info:\n\t\t\t"
	       "generate extra information (declaration stack, compilation unit)\n");
	exit(1);
}

static void parse_generate_opts(int argc, char **argv, generate_config_t *conf,
		char **symbol_file)
{
	*symbol_file = NULL;
	conf->rhel_tree = false;
	conf->verbose = false;
	conf->kabi_dir = DEFAULT_OUTPUT_DIR;
	int opt, opt_index;
	struct option loptions[] = {
		{"help", no_argument, 0, 'h'},
		{"verbose", no_argument, 0, 'v'},
		{"output", required_argument, 0, 'o'},
		{"symbols", required_argument, 0, 's'},
		{"rhel", no_argument, 0, 'r'},
		{"abs-path", required_argument, 0, 'a'},
		{"generate-extra-info", no_argument, 0, 'g'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "hvo:s:ra:m:g",
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
			conf->rhel_tree = true;
			break;
		case 'a':
			get_file_replace_path = optarg;
			break;
		case 'g':
			conf->gen_extra = true;
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

void generate(int argc, char **argv)
{
	char *symbol_file;
	generate_config_t *conf = safe_zmalloc(sizeof(*conf));

	parse_generate_opts(argc, argv, conf, &symbol_file);

	if (symbol_file != NULL) {
		conf->symbols = read_symbols(symbol_file);
		conf->symbol_cnt = ksymtab_len(conf->symbols);

		if (conf->verbose)
			printf("Loaded %ld symbols\n", conf->symbol_cnt);
	}

	generate_symbol_defs(conf);

	if (symbol_file != NULL)
		ksymtab_free(conf->symbols);

	free(conf);
}
