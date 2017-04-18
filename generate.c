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

#define	EMPTY_NAME	"(NULL)"
#define PROCESSED_SIZE 1024
/*
  DB size is number of hash buckets, do not have to be exact,
  but since now we have ~20K records, make it this
*/
#define DB_SIZE (20 * 1024)
#define INITIAL_RECORD_SIZE 512

struct set;
struct record_db;

typedef struct {
	char *kernel_dir; /* Path to  the kernel modules to process */
	char *kabi_dir; /* Where to put the output */
	struct ksymtab *symbols; /* List of symbols to generate */
	size_t symbol_cnt;
	bool *symbols_found;
	size_t symbols_found_cnt;
	struct record_db *db;
	bool verbose;
	bool gen_extra;
} generate_config_t;

struct cu_ctx {
	generate_config_t *conf;
	Dwarf_Die *cu_die;
	stack_t *stack; /* Current stack of symbol we're parsing */
	struct set *processed; /* Set of processed types for this CU */
};

struct file_ctx {
	generate_config_t *conf;
	struct ksymtab *ksymtab; /* ksymtab of the current kernel module */
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
	bool should_free_cu;
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

static obj_t *print_die(struct cu_ctx *, struct record *, Dwarf_Die *);

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

	safe_asprintf(&file_name, "%s%s.txt", file_prefix, name);

	return (file_name);
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
	int rc;

	if (!dwarf_hasattr(die, DW_AT_inline))
		return (false);
	(void) dwarf_attr(die, DW_AT_inline, &attr);
	rc = dwarf_formudata(&attr, &value);
	if (rc == -1)
		fail("dwarf_formudata error %d", rc);

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

static struct record *record_alloc(void)
{
	struct record *rec;

	rec = safe_malloc(sizeof(*rec));
	return rec;
}

static void record_free(struct record *rec)
{
	void *data;

	free(rec->key);
	free(rec->base_file);
	free(rec->origin);
	if (rec->should_free_cu)
		free(rec->cu);

	while ((data = stack_pop(rec->stack)) != NULL)
		free(data);
	stack_destroy(rec->stack);

	obj_free(rec->obj);

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

static struct record *record_new(char *key)
{
	struct record *rec;

	rec = record_alloc();
	rec->key = safe_strdup(key);
	rec->stack = stack_init();
	rec->cu = "CU \"<nottracked>\"\n";
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
	safe_asprintf(&rec->cu, "CU \"%s\"\n", name);
	rec->should_free_cu = true;
}

static void record_add_origin(struct record *rec,
			      Dwarf_Die *cu_die,
			      Dwarf_Die *die)
{
	char *dec_file;
	long dec_line;

	dec_file = get_file(cu_die, die);
	dec_line = get_line(cu_die, die);

	safe_asprintf(&rec->origin, "File %s:%lu\n", dec_file, dec_line);
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

	rec = record_new(key);

	if (conf->gen_extra)
		record_add_cu(rec, cu_die);
	record_add_origin(rec, cu_die, die);
	record_add_stack(rec, ctx->stack);
done:
	return rec;
}

static void record_inc_version(struct record *rec)
{
	char *base_file = rec->base_file;
	char *key = NULL;

	if (rec->version == 0) {
		base_file = safe_strdup(rec->key);
		/* Remove .txt ending */
		base_file[strlen(base_file) - 4] = '\0';
		rec->base_file = base_file;
	}
	rec->version++;
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
	char *data;

	while ((data = stack_pop(rec->stack)) != NULL) {
		fprintf(f, "-> \"%s\"\n", data);
		free(data);
	}
}

static void record_dump(struct record *rec, const char *dir)
{
	char path[PATH_MAX];
	FILE *f;
	int rc;
	char *slash;

	snprintf(path, sizeof(path), "%s/%s", dir, rec->key);

	slash = strrchr(path, '/');
	assert (slash != NULL);
	*slash = '\0';
	rec_mkdir(path);
	*slash = '/';

	f = fopen(path, "w");
	if (f == NULL)
		fail("Cannot create record file '%s': %m", path);

	rc = fputs(rec->cu, f);
	if (rc == EOF)
		fail("Could not put CU name");
	rc = fputs(rec->origin, f);
	if (rc == EOF)
		fail("Could not put origin");

	record_stack_dump_and_clear(rec, f);
	obj_dump(rec->obj, f);

	fclose(f);
}

static struct record *record_db_lookup(struct record_db *_db, char *key)
{
	struct record *rec;
	struct hash *db = (struct hash *)_db;

	rec = hash_find(db, key);
	if (rec != NULL)
		record_get(rec);

	return rec;
}

static void record_db_push(struct record_db *_db, struct record *rec)
{
	int rc;
	struct hash *db = (struct hash *)_db;

	rc = hash_add_unique(db, rec->key, rec);
	assert(rc == 0);
	record_get(rec);
}


/*
 * merge rec_src to the record rec_dst
 */
static bool record_merge(struct record *rec_dst, struct record *rec_src)
{
	const char *s1;
	const char *s2;
	obj_t *o1;
	obj_t *o2;
	obj_t *o;

	s1 = record_origin(rec_dst);
	s2 = record_origin(rec_src);

	if (strcmp(s1, s2) != 0)
		goto out;

	o1 = record_obj(rec_dst);
	o2 = record_obj(rec_src);

	o = obj_merge(o1, o2);
	if (o == NULL)
		goto out;

	obj_fill_parent(o);
	o = record_obj_exchange(rec_dst, o);
	obj_free(o);

	return true;

out:
	return false;
}

static char *record_db_add(struct record_db *db, struct record *rec)
{
	struct record *tmp_rec;
	char *key;

	for (;;) {

		/*
		 * Now we need to put the new type record we've just generated
		 * to the db.
		 *
		 * Often the only difference between these records are some
		 * fields which are fully defined in one file (because the
		 * respective header file has been included for the CU
		 * compilation) while these are mere declarations in the other
		 * (because the header file was not used). We detect this case
		 * and if this is the only difference we just merge these two
		 * records into one using the full definitions where available.
		 *
		 * But of course two types (eg. structures) of the same name
		 * might be completely different types. In such case we try to
		 * store them under different names using increasing number as
		 * a suffix.
		 */

		tmp_rec = record_db_lookup(db, rec->key);
		if (tmp_rec == NULL) {
			record_db_push(db, rec);
			key = safe_strdup(rec->key);
			break;
		}

		if (record_merge(tmp_rec, rec)) {
			key = safe_strdup(tmp_rec->key);
			record_put(tmp_rec);
			break;
		}

		record_put(tmp_rec);
		/* Two different types detected, bump the name version */
		record_inc_version(rec);
	}

	return key;
}

static void hash_record_free(void *value)
{
	struct record *rec = value;

	record_put(rec);
}

static struct record_db *record_db_init(void)
{
	struct hash *db;

	db = hash_new(DB_SIZE, hash_record_free);
	if (db == NULL)
		fail("Could not create db (hash)\n");

	return (struct record_db*)db;
}

static void record_db_dump(struct record_db *_db, char *dir)
{
	struct hash_iter iter;
	const void *v;
	struct hash *db = (struct hash *)_db;

	hash_iter_init(db, &iter);
        while (hash_iter_next(&iter, NULL, &v))
		record_dump((struct record *)v, dir);
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

	if (!dwarf_hasattr(die, DW_AT_type)) {
		return obj_basetype_new(safe_strdup("void"));
	}

	(void) dwarf_attr(die, DW_AT_type, &attr);
	if (dwarf_formref_die(&attr, &type_die) == NULL)
		fail("dwarf_formref_die() failed for %s\n",
		    dwarf_diename(die));

	/* Print the type of the die */
	return print_die(ctx, rec, &type_die);
}

static obj_t *print_die_struct_member(struct cu_ctx *ctx,
				      struct record *rec,
				      Dwarf_Die *die,
				      const char *name)
{
	Dwarf_Attribute attr;
	Dwarf_Word value;
	obj_t *type;
	obj_t *obj;

	if (dwarf_attr(die, DW_AT_data_member_location, &attr) == NULL)
		fail("Offset of member %s missing!\n", name);

	(void) dwarf_formudata(&attr, &value);

	type = print_die_type(ctx, rec, die);
	obj = obj_struct_member_new_add(safe_strdup(name), type);
	obj->offset = value;

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

		obj->is_bitfield = 1;
		obj->first_bit = offset;
		obj->last_bit = offset + size - 1;
	}
	return obj;
}

static obj_t *print_die_structure(struct cu_ctx *ctx,
				  struct record *rec,
				  Dwarf_Die *die)
{
	unsigned int tag = dwarf_tag(die);
	const char *name = get_die_name(die);
	obj_list_head_t *members = NULL;
	obj_t *obj;
	obj_t *member;

	obj = obj_struct_new(safe_strdup(name));

	if (!dwarf_haschildren(die))
		goto done;

	dwarf_child(die, die);
	do {
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

	} while (dwarf_siblingof(die, die) == 0);

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

	obj = obj_enum_new(safe_strdup(name));

	if (!dwarf_haschildren(die))
		goto done;

	dwarf_child(die, die);
	do {
		name = get_die_name(die);
		member = print_die_enumerator(ctx, rec, die, name);
		if (members == NULL)
			members = obj_list_head_new(member);
		else
			obj_list_add(members, member);
	} while (dwarf_siblingof(die, die) == 0);

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
	unsigned int tag = dwarf_tag(die);
	obj_list_head_t *members = NULL;
	obj_t *member;
	obj_t *type;
	obj_t *obj;

	obj = obj_union_new(safe_strdup(name));

	if (!dwarf_haschildren(die))
		goto done;

	dwarf_child(die, die);
	do {
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

	} while (dwarf_siblingof(die, die) == 0);

	members->object = obj;
	obj->member_list = members;
done:
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
	/* if it creates new version, key/file name can change */
	file = record_db_add(conf->db, rec);
	record_put(rec);
	/* record_db_add() returns allocated string */
	free(old_file);

out:
	ref_obj = obj_reffile_new();
	ref_obj->base_type = file;
	return ref_obj;
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
			ref = print_die(&ctx, NULL, &child_die);
			obj_free(ref);

			if (conf->symbols != NULL) {
				/* possible race if symbol in several CUs */
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

	conf->db = record_db_init();

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

	record_db_dump(conf->db, conf->kabi_dir);
	record_db_free(conf->db);
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
	       "replace the absolute path by a relative path\n"
	       "    -g, --generate-extra-info:\n\t\t\t"
	       "generate extra information (declaration stack, compilation unit)\n");
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
		{"generate-extra-info", no_argument, 0, 'g'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "hvo:s:r:m:g",
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
