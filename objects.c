/*
	Copyright(C) 2016, Red Hat, Inc., Jerome Marchand

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
 * Internal representation and manipulation of symbols
 */

#ifndef	_GNU_SOURCE /* We use GNU basename() that doesn't modify the arg */
#error "We need GNU version of basename()!"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <strings.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <sys/stat.h>

#include "objects.h"
#include "utils.h"
#include "main.h"
#include "record.h"

/* Indentation offset for c-style and tree debug outputs */
#define C_INDENT_OFFSET   8
#define DBG_INDENT_OFFSET 4

obj_list_t *obj_list_new(obj_t *obj)
{
	obj_list_t *list = safe_zmalloc(sizeof(obj_list_t));
	list->member = obj;
	list->next = NULL;
	return list;
}

static void obj_list_init(obj_list_head_t *head, obj_t *obj)
{
	obj_list_t *list = obj_list_new(obj);
	head->first = head->last = list;
}

obj_list_head_t *obj_list_head_new(obj_t *obj)
{
	obj_list_head_t *h = safe_zmalloc(sizeof(obj_list_head_t));

	obj_list_init(h, obj);

	return h;
}

static bool obj_list_empty(obj_list_head_t *head)
{
	return head->first == NULL;
}

void obj_list_add(obj_list_head_t *head, obj_t *obj)
{
	obj_list_t *list;

	if (obj_list_empty(head)) {
		obj_list_init(head, obj);
		return;
	}
	list = obj_list_new(obj);

	if (head->last->next)
		fprintf(stderr, "head->last is not the last\n");

	head->last->next = list;
	head->last = list;
}

obj_t *obj_new(obj_types type, char *name)
{
	obj_t *new = safe_zmalloc(sizeof(obj_t));

	new->type = type;
	new->name = global_string_get_move(name);

	return new;
}

static void _obj_free(obj_t *o, obj_t *skip);

static void _obj_list_free(obj_list_head_t *l, obj_t *skip)
{
	obj_list_t *list;
	obj_list_t *next;

	if (l == NULL)
		return;

	list = l->first;
	free(l);

	while (list) {
		_obj_free(list->member, skip);
		next = list->next;
		free(list);
		list = next;
	}
}

static void obj_list_free(obj_list_head_t *l)
{
	_obj_list_free(l, NULL);
}

/*
 * Free the tree o, but keep the subtree skip.
 */
static void _obj_free(obj_t *o, obj_t *skip)
{
	if (!o || (o == skip))
		return;

	if (o->type == __type_reffile && o->depend_rec_node) {
		list_del(o->depend_rec_node);
		o->depend_rec_node = NULL;
	}

	_obj_list_free(o->member_list, skip);

	if (o->ptr)
		_obj_free(o->ptr, skip);

	if (is_weak(o))
		free(o->link);

	free(o);
}

/*
 * Free the all object
 */
void obj_free(obj_t *o)
{
	_obj_free(o, NULL);
}

#define _CREATE_NEW_FUNC(type, suffix)			\
obj_t *obj_##type##_##suffix(char *name)		\
{							\
	obj_t *new = obj_new(__type_##type, name);	\
	return new;					\
}
#define CREATE_NEW_FUNC(type) _CREATE_NEW_FUNC(type, new)
#define CREATE_NEW_FUNC_NONAME(type)			\
_CREATE_NEW_FUNC(type, new_)				\
obj_t *obj_##type##_new()				\
{							\
	return obj_##type##_new_(NULL);			\
}

#define _CREATE_NEW_ADD_FUNC(type, infix)		\
obj_t *obj_##type##_##infix##_##add(char *name, obj_t *obj) \
{							\
	obj_t *new = obj_new(__type_##type, name);	\
	new->ptr = obj;					\
	return new;					\
}
#define CREATE_NEW_ADD_FUNC(type) _CREATE_NEW_ADD_FUNC(type, new)
#define CREATE_NEW_ADD_FUNC_NONAME(type)		\
_CREATE_NEW_ADD_FUNC(type, new_)			\
obj_t *obj_##type##_new_add(obj_t *obj)			\
{							\
	return obj_##type##_new__add(NULL, obj);	\
}

CREATE_NEW_FUNC(struct)
CREATE_NEW_FUNC(union)
CREATE_NEW_FUNC(enum)
CREATE_NEW_FUNC(constant)
CREATE_NEW_FUNC_NONAME(reffile)
CREATE_NEW_ADD_FUNC(func)
CREATE_NEW_ADD_FUNC(typedef)
CREATE_NEW_ADD_FUNC(var)
CREATE_NEW_ADD_FUNC(struct_member)
CREATE_NEW_ADD_FUNC_NONAME(ptr)
CREATE_NEW_ADD_FUNC_NONAME(array)
CREATE_NEW_ADD_FUNC_NONAME(qualifier)
CREATE_NEW_FUNC(assembly)
CREATE_NEW_FUNC(weak)

obj_t *obj_basetype_new(char *base_type)
{
	obj_t *new = obj_new(__type_base, NULL);

	new->base_type = global_string_get_move(base_type);

	return new;
}

const char *obj_type_name[NR_OBJ_TYPES+1] = {
	"reference file",
	"struct",
	"union",
	"enum",
	"func",
	"ptr",
	"typedef",
	"array",
	"var",
	"struct member",
	"type qualifier",
	"base",
	"constant",
	"assembly",
	"weak",
	"unknown type"
};

static const char *typetostr(obj_t *o)
{
	int t = o->type;
	if (t >= NR_OBJ_TYPES)
		t = NR_OBJ_TYPES;
	return obj_type_name[t];
}

static int c_precedence(obj_t *o)
{
	switch (o->type) {
	case __type_func:
	case __type_array:
		return 1;
	case __type_ptr:
		return 2;
	default:
		return INT_MAX;
	}
}

/*
 * Returns whether parentheses are needed
 *
 * Pointer have a higher precedence than function and array, so we need to put
 * parentheses around a pointer to a function of array.
 */
static bool is_paren_needed(obj_t *node)
{
	obj_t *child = node->ptr;

	while (child) {
		if (c_precedence(child) < c_precedence(node))
			return true;

		child = child->ptr;
	}
	return false;
}

static char *print_margin_offset(const char *prefix, const char *s, int depth)
{
	size_t len = snprintf(NULL, 0, "%-*s", depth * C_INDENT_OFFSET, s) + 1;
	char *ret;

	if (prefix)
		len += strlen(prefix);

	if (!len)
		return NULL;
	ret = safe_zmalloc(len);

	snprintf(ret, len, "%s%-*s",
		 prefix ? prefix : "", depth * C_INDENT_OFFSET, s);

	return ret;
}

static char *print_margin(const char *prefix, int depth)
{
	return  print_margin_offset(prefix, "", depth);
}

/*
 * Return type for print_* functions
 *
 * Because C mixes prefix and postfix operator, the code generation from a node
 * may need to add code before, after or in the middle of the code generated by
 * subtrees. Thus we sometimes need two return two strings.
 *
 * Attention to the precedence and associativity sould be taken when
 * deciding where a specific string should be inserted
 */
typedef struct {
	char *prefix;
	char *postfix;
} pp_t;

static void free_pp(pp_t pp)
{
	free(pp.prefix);
	free(pp.postfix);
}

static pp_t _print_tree(obj_t *o, int depth, bool newline, const char *prefix);

/*
 * Add prefix p at the begining of string s (reallocated)
 *
 * space: add a space between p and s
 * freep: free the string p
 */
static char *_prefix_str(char **s, char *p, bool space, bool freep)
{
	size_t lenp = strlen(p), lens = 0, newlen;

	if (*s)
		lens = strlen(*s);
	newlen = lens + lenp + 1;

	if (space)
		newlen++;

	*s = safe_realloc(*s, newlen);

	if (lens)
		memmove(space ? *s+lenp+1 : *s+lenp, *s, lens + 1);
	else
		(*s)[lenp] = '\0';
	memcpy(*s, p, lenp);
	if (space)
		(*s)[lenp] = ' ';

	if (freep)
		free(p);

	return *s;
}

static char *prefix_str(char **s, char *p)
{
	if (!p)
		return *s;
	return _prefix_str(s, p, false, false);
}

static char *prefix_str_free(char **s, char *p)
{
	if (!p)
		return *s;
	return _prefix_str(s, p, false, true);
}

static char *prefix_str_space(char **s, const char *p)
{
	if (!p)
		return *s;
	/* freep is false so we can pass const char * */
	return _prefix_str(s, (char *)p, true, false);
}

/*
 * Add suffix p at the end of string s (realocated)
 *
 * space: add a space between p and s
 * freep: free the string p
 */
static char *_postfix_str(char **s, char *p, bool space, bool freep)
{
	int lenp = strlen(p), lens = 0, newlen;
	if (*s)
		lens = strlen(*s);
	newlen = lens + lenp + 1;

	if (space)
		newlen++;

	*s = safe_realloc(*s, newlen);

	if (lens == 0)
		(*s)[0] = '\0';
	if (space)
		strcat(*s, " ");
	strcat(*s, p);

	if (freep)
		free(p);

	return *s;
}

static char *postfix_str(char **s, const char *p)
{
	if (!p)
		return *s;

	/* freep is false so we can pass const char * */
	return _postfix_str(s, (char *)p, false, false);
}

static char *postfix_str_free(char **s, char *p)
{
	if (!p)
		return *s;
	return _postfix_str(s, p, false, true);
}

static pp_t print_base(obj_t *o, int depth, const char *prefix)
{
	pp_t ret = {NULL, NULL};

	safe_asprintf(&ret.prefix, "%s ", o->base_type);

	return ret;
}

static pp_t print_constant(obj_t *o, int depth, const char *prefix)
{
	pp_t ret = {NULL, NULL};

	safe_asprintf(&ret.prefix, "%s = %li", o->name, (long)o->constant);

	return ret;
}

static pp_t print_reffile(obj_t *o, int depth, const char *prefix)
{
	pp_t ret = {NULL, NULL};
	char *s = filenametotype(o->base_type);

	s = safe_realloc(s, strlen(s) + 2);
	strcat(s, " ");
	ret.prefix = s;

	return ret;
}

/* Print a struct, enum or an union */
static pp_t print_structlike(obj_t *o, int depth, const char *prefix)
{
	pp_t ret = {NULL, NULL}, tmp;
	obj_list_t *list = NULL;
	char *s, *margin;

	if (o->name)
		safe_asprintf(&s, "%s %s {\n", typetostr(o), o->name);
	else
		safe_asprintf(&s, "%s {\n", typetostr(o));

	if (o->member_list)
		list = o->member_list->first;
	while (list) {
		tmp = _print_tree(list->member, depth+1, true, prefix);
		postfix_str_free(&s, tmp.prefix);
		postfix_str_free(&s, tmp.postfix);
		postfix_str(&s, o->type == __type_enum ? ",\n" : ";\n");
		list = list->next;
	}

	margin = print_margin(prefix, depth);
	postfix_str_free(&s, margin);
	postfix_str(&s, "}");

	ret.prefix = s;
	return ret;
}

static pp_t print_func(obj_t *o, int depth, const char *prefix)
{
	pp_t ret = {NULL, NULL}, return_type;
	obj_list_t *list = NULL;
	obj_t *next = o->ptr;
	char *s, *margin;
	const char *name;

	return_type = _print_tree(next, depth, false, prefix);
	ret.prefix = return_type.prefix;

	if (o->name)
		name = o->name;
	else
		name = "";

	safe_asprintf(&s, "%s(\n", name);

	if (o->member_list)
		list = o->member_list->first;
	while (list) {
		pp_t arg = _print_tree(list->member, depth+1, true, prefix);
		postfix_str_free(&s, arg.prefix);
		postfix_str_free(&s, arg.postfix);
		list = list->next;
		postfix_str(&s, list ? ",\n" : "\n");
	}

	margin = print_margin(prefix, depth);
	postfix_str_free(&s, margin);
	postfix_str(&s, ")");

	ret.postfix = s;
	return ret;
}

static pp_t print_array(obj_t *o, int depth, const char *prefix)
{
	pp_t ret;
	char *s;
	obj_t *next = o->ptr;

	ret = _print_tree(next, depth, false, prefix);

	safe_asprintf(&s, "[%lu]", o->constant);
	prefix_str_free(&ret.postfix, s);

	return ret;
}

static pp_t print_ptr(obj_t *o, int depth, const char *prefix)
{
	pp_t ret;
	bool need_paren = is_paren_needed(o);
	obj_t *next = o->ptr;

	ret = _print_tree(next, depth, false, prefix);
	if (need_paren) {
		postfix_str(&ret.prefix, "(*");
		prefix_str(&ret.postfix, ")");
	} else
		postfix_str(&ret.prefix, "*");

	return ret;
}

/* Print a var or a struct_member */
static pp_t print_varlike(obj_t *o, int depth, const char *prefix)
{
	pp_t ret;
	char *s = NULL;

	if (is_bitfield(o))
		safe_asprintf(&s, "%s:%i",
			      o->name, o->last_bit - o->first_bit + 1);
	else
		s = (char *)o->name;

	ret = _print_tree(o->ptr, depth, false, prefix);

	if (s)
		postfix_str(&ret.prefix, s);

	if (is_bitfield(o))
		free(s);

	return ret;
}

static pp_t print_typedef(obj_t *o, int depth, const char *prefix)
{
	pp_t ret;

	ret = _print_tree(o->ptr, depth, false, prefix);

	prefix_str(&ret.prefix, "typedef ");
	postfix_str(&ret.prefix, o->name);

	return ret;
}

static pp_t print_qualifier(obj_t *o, int depth, const char *prefix)
{
	pp_t ret;

	ret = _print_tree(o->ptr, depth, false, prefix);
	prefix_str_space(&ret.prefix, o->base_type);

	return ret;
}

static pp_t print_assembly(obj_t *o, int depth, const char *prefix)
{
	pp_t ret = {NULL, NULL};

	prefix_str(&ret.prefix, "assembly ");
	postfix_str(&ret.prefix, o->name);

	return ret;
}

static pp_t print_weak(obj_t *o, int depth, const char *prefix)
{
	pp_t ret = {NULL, NULL};

	prefix_str(&ret.prefix, "weak ");
	postfix_str(&ret.prefix, o->name);
	postfix_str(&ret.prefix, " -> ");
	postfix_str(&ret.prefix, o->link);

	return ret;
}

#define BASIC_CASE(type)				\
	case __type_##type:				\
		ret = print_##type(o, depth, prefix);	\
		break;

struct dopt display_options;

/*
 * Display an object in a c-like format
 *
 * o:	    object to be displayed
 * depth:   current indentation depth
 * newline: is this the begining of a new line?
 * prefix:  prefix to be printed at the begining of each line
 */
static pp_t _print_tree(obj_t *o, int depth, bool newline, const char *prefix)
{
	pp_t ret = {NULL, NULL};
	char *margin;

	/* silence coverity on write-only variable */
	(void)ret;

	if (!o)
		fail("NULL pointer in _print_tree\n");
	debug("_print_tree(): %s\n", typetostr(o));

	switch (o->type) {
	BASIC_CASE(reffile);
	BASIC_CASE(constant);
	BASIC_CASE(base);
	BASIC_CASE(typedef);
	BASIC_CASE(qualifier);
	BASIC_CASE(func);
	BASIC_CASE(array);
	BASIC_CASE(ptr);
	BASIC_CASE(assembly);
	BASIC_CASE(weak);
	case __type_var:
	case __type_struct_member:
		ret = print_varlike(o, depth, prefix);
		break;
	case __type_struct:
	case __type_union:
	case __type_enum:
		ret = print_structlike(o, depth, prefix);
		break;
	default:
		fail("WIP: doesn't handle %s\n", typetostr(o));
	}

	if (!newline)
		return ret;

	if (o->type == __type_struct_member && !display_options.no_offset) {
		char *offstr;
		if (is_bitfield(o))
			safe_asprintf(&offstr, "0x%lx:%2i-%-2i ",
				      o->offset, o->first_bit, o->last_bit);
		else
			safe_asprintf(&offstr, "0x%lx ", o->offset);
		margin = print_margin_offset(prefix, offstr, depth);
		free(offstr);
	} else
		margin = print_margin(prefix, depth);

	prefix_str_free(&ret.prefix, margin);
	return ret;
}

void obj_print_tree__prefix(obj_t *root, const char *prefix, FILE *stream)
{
	pp_t s = _print_tree(root, 0, true, prefix);

	fprintf(stream, "%s%s;\n",
	       s.prefix ? s.prefix : "",
	       s.postfix ? s.postfix : "");
	free_pp(s);
}

void obj_print_tree(obj_t *root)
{
	obj_print_tree__prefix(root, NULL, stdout);
}

static void fill_parent_rec(obj_t *o, obj_t *parent)
{
	obj_list_t *list = NULL;

	o->parent = parent;

	if (o->member_list)
		list = o->member_list->first;

	while (list) {
		fill_parent_rec(list->member, o);
		list = list->next;
	}

	if (o->ptr)
		fill_parent_rec(o->ptr, o);
}

/*
 * Walk the tree and fill all the parents field
 */
void obj_fill_parent(obj_t *root)
{
	fill_parent_rec(root, NULL);
}

static int walk_list(obj_list_t *list, cb_t cb_pre, cb_t cb_in, cb_t cb_post,
			void *args, bool ptr_first)
{
	int ret = CB_CONT;

	while (list) {
		ret = obj_walk_tree3(list->member, cb_pre, cb_in, cb_post,
				 args, ptr_first);
		if (ret == CB_FAIL)
			return ret;
		else
			ret = CB_CONT;
		list = list->next;
	}

	return ret;
}

static int walk_ptr(obj_t *o, cb_t cb_pre, cb_t cb_in, cb_t cb_post,
			void *args, bool ptr_first)
{
	int ret = CB_CONT;

	if (o->ptr) {
		ret = obj_walk_tree3(o->ptr, cb_pre, cb_in, cb_post,
				 args, ptr_first);
		if (ret == CB_FAIL)
			return ret;
		else
			ret = CB_CONT;
	}

	return ret;
}

/*
 * Tree walk with prefix, infix and postfix callbacks
 *
 * o:	      walked tree
 * cb_pre:    callback function called before walking the subtrees
 * cb_in:     callback function called between walking the subtrees
 * cp_post:   callback function called between walking the subtrees
 * args:      argument passed to the callbacks
 * ptr_first: whether we walk member_list of ptr first
 */
int obj_walk_tree3(obj_t *o, cb_t cb_pre, cb_t cb_in, cb_t cb_post,
			void *args, bool ptr_first)
{
	obj_list_t *list = NULL;
	int ret = CB_CONT;

	if (cb_pre) {
		ret = cb_pre(o, args);
		if (ret)
			return ret;
	}

	if (o->member_list)
		list = o->member_list->first;


	if (ptr_first)
		ret = walk_ptr(o, cb_pre, cb_in, cb_post, args, ptr_first);
	else
		ret = walk_list(list, cb_pre, cb_in, cb_post, args, ptr_first);
	if (ret == CB_FAIL)
		return ret;

	if (cb_in) {
		ret = cb_in(o, args);
		if (ret)
			return ret;
	}

	if (ptr_first)
		ret = walk_list(list, cb_pre, cb_in, cb_post, args, ptr_first);
	else
		ret = walk_ptr(o, cb_pre, cb_in, cb_post, args, ptr_first);
	if (ret == CB_FAIL)
		return ret;

	if (cb_post) {
		ret = cb_post(o, args);
		if (ret)
			return ret;
	}

	return ret;
}

/*
 * Simple tree walk with a prefix callback
 *
 * It walks the member_list subtree first.
 */
int obj_walk_tree(obj_t *root, cb_t cb, void *args)
{
	return obj_walk_tree3(root, cb, NULL, NULL, args, false);
}

static void _show_node(FILE *f, obj_t *o, int margin)
{
	if (o)
		fprintf(f,
			"\%*s<%s, \"%s\", \"%s\", %p, %p, %p, %lu, %i, %i>\n",
			margin, "", typetostr(o), o->name, o->base_type,
			o, o->parent, o->ptr,
			o->offset, o->first_bit, o->last_bit);
	else
		fprintf(f, "\%*s<(nil)>\n", margin, "");
}

static void show_node(obj_t *o, int margin)
{
	_show_node(stdout, o, margin);
}

static int debug_node(obj_t *node, void *args)
{
	int *depth = (int *) args;

	show_node(node, *depth * DBG_INDENT_OFFSET);
	(*depth)++;

	return CB_CONT;
}

static int dec_depth(obj_t *node, void *args)
{
	int *depth = (int *) args;

	(*depth)--;

	return CB_CONT;
}

/*
 * Print a raw representation of the internal object tree
 */
int obj_debug_tree(obj_t *root)
{
	int depth = 0;

	return obj_walk_tree3(root, debug_node, NULL, dec_depth, &depth, false);
}

/*
 * Hide some RH_KABI magic
 *
 * WARNING: this code is ugly and full of add-hoc hacks, but I'm
 * afraid it can't be fixed. It has to follow the internals of
 * RH_KABI_* macros. Also, it may have to change if RH_KABI_*
 * functions change in the future.
 *
 * RH_KABI_REPLACE the old field by a complex construction of union
 * and struct used to check that the new field didn't change the
 * alignement. It is of the form:
 * union {
 *	_new;
 *	struct {
 *		_orig;
 *	} __UNIQUE_ID_rh_kabi_hideXX
 *	union {};
 * }
 *
 * RH_KABI_USE2(_P) replace a single field by two field that fits in
 * the same space. It puts the two new field into an unnamed
 * struct. We don't hide that as we have no way to know if that struct
 * is an artifact from RH_KABI_USE2 or was added deliberately.
 *
 * RH_KABI_DEPRECATE(_FN) prefix the field name with
 * rh_reserved_. This is not the most specific string. It currently
 * appears in a few places that deprecates the field by hand, in which
 * it's OK to hide it too, but for some reason in
 * block_device_operations the reserved fields are of the form "void
 * *rh_reserved_ptrsX" instead of the usual "unsigned long
 * rh_reservedX". Treat this case as an exception.
 *
 * Most RH_KABI_* functions, don't add any recognizable code so we
 * can't hide them here.
 */
static int hide_kabi_cb(obj_t *o, void *args)
{
	obj_t *kabi_struct, *new, *old, *parent = o->parent, *keeper;
	obj_list_head_t *lh;
	obj_list_t *l;
	bool show_new_field = (bool) args;

	if (o->name) {
		if (!strncmp(o->name, RH_KABI_HIDE, RH_KABI_HIDE_LEN))
			fail("Missed a kabi unique ID\n");

		/* Hide RH_KABI_DEPRECATE* */
		if (!strncmp(o->name, "rh_reserved_", 12) &&
		    strncmp(o->name, "rh_reserved_ptrs", 16)) {
			char *tmp = strdup(o->name+12);
			o->name = global_string_get_move(tmp);
		}
	}

	/* Hide RH_KABI_REPLACE */
	if ((o->type != __type_union) || o->name ||
	    !(lh = o->member_list) || obj_list_empty(lh) ||
	    !(l = lh->first) || !(new = l->member) ||
	    !(l = l->next) || !(kabi_struct = l->member) ||
	    (kabi_struct->type != __type_var) ||
	    !kabi_struct->name ||
	    strncmp(kabi_struct->name, RH_KABI_HIDE, RH_KABI_HIDE_LEN))
		return CB_CONT;

	if (!kabi_struct->ptr || kabi_struct->ptr->type != __type_struct ||
	    !(lh = kabi_struct->ptr->member_list) || obj_list_empty(lh) ||
	    !(l = lh->first) || !(old = l->member))
		fail("Unexpeted rh_kabi_hide struct format\n");

	/*
	 * It is a rh_kabi_hide union
	 * old is the first member of kabi_struct
	 *
	 * Need to replace that:
	 * <struct member, "(null)", "(null)", 0x1ea9840 16 0 0> (parent)
	 *    <union, "(null)", "(null)", (nil) 0 0 0>		 (o)
	 *       <var, "new_field", "(null)", 0x1ea9540 0 0 0>	 (new)
	 *          <base, "(null)", "int", (nil) 0 0 0>
	 *       <var, "__UNIQUE_ID_rh_kabi_hide55", "(null)", 0x1ea9700 0 0 0>
	 *          <struct, "(null)", "(null)", (nil) 0 0 0>
	 *             <struct member, "old_field", "(null)", 0x1ea9640 0 0 0>
	 *                <base, "(null)", "int", (nil) 0 0 0>		^(old)
	 *       <var, "(null)", "(null)", 0x1ea97a0 0 0 0>
	 *          <union, "(null)", "(null)", (nil) 0 0 0>
	 *
	 * by that:
	 * <struct member, "new_field", "(null)", 0x1ea9540 16 0 0>
	 *    <base, "(null)", "int", (nil) 0 0 0>
	 *
	 * or that:
	 * <struct member, "old_field", "(null)", 0x1ea9640 0 0 0>
	 *    <base, "(null)", "int", (nil) 0 0 0>
	 *
	 * Parent is always an unary node, struct_member or var
	 */

	if (!parent ||
	    !((parent->type == __type_var) ||
	    (parent->type == __type_struct_member)) ||
	    (parent->ptr != o) || parent->name) {
		_show_node(stderr, parent, 0);
		fail("Unexpected parent\n");
	}
	if (new->type != __type_var) {
		_show_node(stderr, new, 0);
		fail("Unexpected new field\n");
	}
	if (old->type != __type_struct_member) {
		_show_node(stderr, old, 0);
		fail("Unexpected old field\n");
	}

	keeper = show_new_field ? new : old;

	parent->name = keeper->name;
	parent->ptr = keeper->ptr;
	parent->ptr->parent = parent;
	_obj_free(o, keeper);
	free(keeper);

	return CB_SKIP;
}

int obj_hide_kabi(obj_t *root, bool show_new_field)
{
	return obj_walk_tree(root, hide_kabi_cb, (void *)show_new_field);
}

static bool obj_is_declaration(obj_t *obj)
{
	if (obj->type != __type_reffile || obj->ref_record == NULL)
		return false;

	return record_is_declaration(obj->ref_record);
}

static bool obj_is_kabi_hide(obj_t *obj)
{
	if (obj->name == NULL)
		return false;

	return strncmp(obj->name, RH_KABI_HIDE, RH_KABI_HIDE_LEN) == 0;
}

bool obj_eq(obj_t *o1, obj_t *o2, bool ignore_versions)
{
	if (o1->type != o2->type)
		return false;

	if (o1->type == __type_reffile) {
		if (ignore_versions) {
			return record_get_key(o1->ref_record) ==
				record_get_key(o2->ref_record);
		}

		return o1->ref_record == o2->ref_record;
	}

	/* borrow parts from cmp_nodes */
	if ((o1->name != o2->name) ||
	    ((o1->ptr == NULL) != (o2->ptr == NULL)) ||
	    (has_constant(o1) && (o1->constant != o2->constant)) ||
	    (has_index(o1) && (o1->index != o2->index)) ||
	    (is_bitfield(o1) != is_bitfield(o2)) ||
	    (o1->alignment != o2->alignment) ||
	    (o1->byte_size != o2->byte_size))
		return false;

	/* just compare bitfields */
	if (is_bitfield(o1) &&
	    ((o1->last_bit !=  o2->last_bit) ||
	     (o1->first_bit != o2->first_bit)))
		return false;

	if ((o1->member_list == NULL) !=
	    (o2->member_list == NULL))
		return false;

	if (o1->base_type != o2->base_type)
		return false;

	return true;
}

static obj_t *obj_copy(obj_t *o1)
{
	obj_t *o;

	o = safe_zmalloc(sizeof(*o));
	*o = *o1;

	o->ptr = NULL;
	o->member_list = NULL;

	if (o1->type == __type_reffile && o1->depend_rec_node)
		o->depend_rec_node = list_node_add(o1->depend_rec_node, o);

	return o;
}

obj_t *obj_merge(obj_t *o1, obj_t *o2, unsigned int flags);

static obj_list_head_t *obj_members_merge(obj_list_head_t *list1,
					  obj_list_head_t *list2,
					  unsigned int flags)
{
	obj_list_head_t *res = NULL;
	obj_list_t *l1;
	obj_list_t *l2;
	obj_t *o;

	if (list1 == NULL || list2 == NULL)
		return NULL;

	l1 = list1->first;
	l2 = list2->first;

	while (l1 && l2) {
		o = obj_merge(l1->member, l2->member, flags);
		if (o == NULL)
			goto cleanup;

		if (res == NULL)
			res = obj_list_head_new(o);
		else
			obj_list_add(res, o);

		l1 = l1->next;
		l2 = l2->next;
	};

	if (l1 || l2)
		goto cleanup;

	return res;

cleanup:
	obj_list_free(res);
	return NULL;
}

static inline bool obj_can_merge_two_lines(obj_t *o1, obj_t *o2,
					   unsigned int flags)
{
	/*
	 * We cannot merge two lines if:
	 *  - their states of being declarations are not equivalent,
	 *    and we require them to be
	 */
	if (flags & MERGE_FLAG_DECL_EQ &&
	    (obj_is_declaration(o1) != obj_is_declaration(o2)))
		return false;

	/*
	 * We can merge the two lines if:
	 *  - they are the same, or
	 *  - they are both RH_KABI_HIDE, or
	 *  - at least one of them is a declaration,
	 *    and we can merge declarations
	 */
	if (obj_eq(o1, o2, flags & MERGE_FLAG_VER_IGNORE))
		return true;

	if (obj_is_kabi_hide(o1) && obj_is_kabi_hide(o2))
		return true;

	if (flags & MERGE_FLAG_DECL_MERGE &&
	    (obj_is_declaration(o1) || obj_is_declaration(o2)))
		return true;

	return false;
}

obj_t *obj_merge(obj_t *o1, obj_t *o2, unsigned int flags)
{
	obj_t *merged_ptr;
	obj_list_head_t *merged_members;
	obj_t *res = NULL;

	if (o1 == NULL || o2 == NULL)
		return NULL;

	if (!obj_can_merge_two_lines(o1, o2, flags))
		goto no_merge;

	merged_ptr = obj_merge(o1->ptr, o2->ptr, flags);
	if (o1->ptr && !merged_ptr)
		goto no_merge_ptr;

	merged_members = obj_members_merge(o1->member_list,
					   o2->member_list,
					   flags);
	if (o1->member_list && !merged_members)
		goto no_merge_members;

	if (obj_is_declaration(o1))
		res = obj_copy(o2);
	else
		res = obj_copy(o1);

	res->ptr = merged_ptr;

	if (merged_members != NULL)
		merged_members->object = res;
	res->member_list = merged_members;

	return res;

no_merge_members:
	obj_list_free(merged_members);
no_merge_ptr:
	obj_free(merged_ptr);
no_merge:
	return NULL;
}

static void dump_reffile(obj_t *o, FILE *f)
{
	int version = record_get_version(o->ref_record);

	fprintf(f, "@\"%s", record_get_key(o->ref_record));
	if (version > 0)
		fprintf(f, "-%i", version);
	fprintf(f, ".txt\"\n");
}

static void _dump_members(obj_t *o, FILE *f, void (*dumper)(obj_t *, FILE *))
{
	obj_list_head_t *l = o->member_list;
	obj_list_t *list;

	if (l == NULL)
		return;

	list = l->first;

	while (list) {
		dumper(list->member, f);
		list = list->next;
	}
}

static void dump_arg(obj_t *o, FILE *f)
{
	fprintf(f, "%s ", o->name);
	obj_dump(o->ptr, f);
}

static void dump_members(obj_t *o, FILE *f)
{
	_dump_members(o, f, obj_dump);
}

static void dump_args(obj_t *o, FILE *f)
{
	_dump_members(o, f, dump_arg);
}

static void dump_struct(obj_t *o, FILE *f)
{
	fprintf(f, "struct %s {\n", o->name);
	dump_members(o, f);
	fprintf(f, "}\n");
}
static void dump_union(obj_t *o, FILE *f)
{
	fprintf(f, "union %s {\n", o->name);
	dump_args(o, f);
	fprintf(f, "}\n");
}

static void dump_enum(obj_t *o, FILE *f)
{
	fprintf(f, "enum %s {\n", o->name);
	dump_members(o, f);
	fprintf(f, "}\n");
}

static void dump_func(obj_t *o, FILE *f)
{
	fprintf(f, "func %s (\n", o->name);
	dump_args(o, f);
	fprintf(f, ")\n");

	obj_dump(o->ptr, f);
}

static void dump_ptr(obj_t *o, FILE *f)
{
	fprintf(f, "* ");
	obj_dump(o->ptr, f);
}

static void dump_typedef(obj_t *o, FILE *f)
{
	fprintf(f, "typedef %s\n", o->name);
	obj_dump(o->ptr, f);
}

static void dump_array(obj_t *o, FILE *f)
{
	fprintf(f, "[%lu]", o->index);
	obj_dump(o->ptr, f);
}

static void dump_var(obj_t *o, FILE *f)
{
	fprintf(f, "var %s ", o->name);
	obj_dump(o->ptr, f);
}

static void dump_struct_member(obj_t *o, FILE *f)
{
	fprintf(f, "0x%lx", o->offset);
	if (o->is_bitfield)
		fprintf(f, ":%d-%d", o->first_bit, o->last_bit);

	if (o->alignment != 0)
		fprintf(f, " %u", o->alignment);

	fprintf(f, " %s ", o->name);
	obj_dump(o->ptr, f);
}

static void dump_qualifier(obj_t *o, FILE *f)
{
	fprintf(f, "%s ", o->base_type);
	obj_dump(o->ptr, f);
}

static void dump_base(obj_t *o, FILE *f)
{
	const char *type = o->base_type;

	/* variable args (...) is a special base case */
	if (type[0] == '.')
		fprintf(f, "%s\n", o->base_type);
	else
		fprintf(f, "\"%s\"\n", o->base_type);
}

static void dump_constant(obj_t *o, FILE *f)
{
	fprintf(f, "%s = 0x%lx\n", o->name, o->constant);
}

static void dump_fail(obj_t *o, FILE *f)
{
	fail("Dump call for this type unsupported!\n");
}

struct dumper {
	void (*dumper)(obj_t *o, FILE *f);
};

static struct dumper dumpers[] = {
	[__type_reffile].dumper = dump_reffile,
	[__type_struct].dumper = dump_struct,
	[__type_union].dumper = dump_union,
	[__type_enum].dumper = dump_enum,
	[__type_func].dumper = dump_func,
	[__type_ptr].dumper = dump_ptr,
	[__type_typedef].dumper = dump_typedef,
	[__type_array].dumper = dump_array,
	[__type_var].dumper = dump_var,
	[__type_struct_member].dumper = dump_struct_member,
	[__type_qualifier].dumper = dump_qualifier,
	[__type_base].dumper = dump_base,
	[__type_constant].dumper = dump_constant,
	[__type_assembly].dumper = dump_fail,
	[__type_weak].dumper = dump_fail,
};

void obj_dump(obj_t *o, FILE *f)
{
	if (o == NULL)
		return;

	if (o->type >= NR_OBJ_TYPES)
		fail("Wrong object type %d", o->type);

	dumpers[o->type].dumper(o, f);
}

bool obj_same_declarations(obj_t *o1, obj_t *o2,
			   struct set *processed)
{
	const int ignore_versions = true;
	obj_list_t *list1;
	obj_list_t *list2;

	if (o1 == o2)
		return true;

	if (!obj_eq(o1, o2, ignore_versions))
		return false;

	if (o1->type != o2->type ||
	    (o1->ptr == NULL) != (o2->ptr == NULL) ||
	    (o1->member_list == NULL) != (o2->member_list == NULL)) {
		return false;
	}


	if (o1->type == __type_reffile &&
	    !record_same_declarations(o1->ref_record, o2->ref_record,
				      processed)) {
		return false;
	}

	if (o1->ptr &&
	    !obj_same_declarations(o1->ptr, o2->ptr, processed)) {
		return false;
	}

	if (o1->member_list) {
		list1 = o1->member_list->first;
		list2 = o2->member_list->first;

		while (list1) {
			if (list2 == NULL)
				return false;

			if (!obj_same_declarations(list1->member,
						   list2->member,
						   processed))
				return false;

			list1 = list1->next;
			list2 = list2->next;
		}

		if (list1 != list2) {
			/* different member_list lengths */
			return false;
		}
	}

	return true;
}
