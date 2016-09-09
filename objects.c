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

#define _GNU_SOURCE /* We use GNU basename() that doesn't modify the arg */
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

obj_list_t *new_list(obj_t *obj) {
	obj_list_t *list = malloc(sizeof(obj_list_t));
	list->member = obj;
	list->next = NULL;
	return list;
}

static void list_init(obj_list_head_t *head, obj_t *obj) {
	obj_list_t *list = new_list(obj);
	head->first = head->last = list;
}

obj_list_head_t *new_list_head(obj_t *obj) {
	obj_list_head_t *h = malloc(sizeof(obj_list_head_t));

	list_init(h, obj);

	return h;
}

static bool list_empty(obj_list_head_t *head) {
	return head->first == NULL;
}

void list_add(obj_list_head_t *head, obj_t *obj) {
	obj_list_t *list;

	if (list_empty(head)) {
		list_init(head, obj);
		return;
	}
	list = new_list(obj);

	if (head->last->next)
		fprintf(stderr, "head->last is not the last\n");

	head->last->next = list;
	head->last = list;
}

bool list_remove(obj_list_head_t *head, obj_t *obj) {
	obj_list_t *l = head->first, *previous = NULL;

	while (l) {
		if (l->member == obj) {
			if (previous)
				previous->next = l->next;
			if (l == head->first)
				head->first = l->next;
			if (l == head->last)
				head->last = previous;
			return true;
		}
		l = l->next;
	}
	return false;
}


obj_t *new_obj(obj_types type, char *name) {
	obj_t *new = malloc(sizeof(obj_t));
	bzero(new, sizeof(obj_t));

	new->type = type;
	new->name = name;

	return new;
}

/*
 * Free the tree o, but keep the subtree skip.
 */
static void _free_obj(obj_t *o, obj_t *skip) {
	obj_list_t *list = NULL, *next;

	if (!o || (o == skip))
		return;
	if(o->name)
		free(o->name);
	if(o->base_type)
		free(o->base_type);

	if (o->member_list) {
		list = o->member_list->first;
		free(o->member_list);
	}

	while ( list ) {
		_free_obj(list->member, skip);
		next = list->next;
		free(list);
		list = next;
	}

	if(o->ptr)
		_free_obj(o->ptr, skip);

	free(o);
}

/*
 * Free the all object
 */
void free_obj(obj_t *o) {
	_free_obj(o, NULL);
}

#define _CREATE_NEW_FUNC(type, prefix)			\
obj_t *prefix##_##type(char *name) {			\
	obj_t *new = new_obj(__type_##type, name);	\
	return new;					\
}
#define CREATE_NEW_FUNC(type) _CREATE_NEW_FUNC(type, new)
#define CREATE_NEW_FUNC_NONAME(type)			\
_CREATE_NEW_FUNC(type, _new)				\
obj_t *new_##type() {					\
	return _new_##type(NULL);			\
}

#define _CREATE_NEW_ADD_FUNC(type, prefix)		\
obj_t *prefix##_##type##_add(char *name, obj_t *obj) {	\
	obj_t *new = new_obj(__type_##type, name);	\
	new->ptr = obj;					\
	return new;					\
}
#define CREATE_NEW_ADD_FUNC(type) _CREATE_NEW_ADD_FUNC(type, new)
#define CREATE_NEW_ADD_FUNC_NONAME(type)		\
_CREATE_NEW_ADD_FUNC(type, _new)			\
obj_t *new_##type##_add(obj_t *obj) {			\
	return _new_##type##_add(NULL, obj);		\
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

obj_t *new_base(char *base_type) {
	obj_t *new = new_obj(__type_base, NULL);

	new->base_type = base_type;

	return new;
}

const char *obj_type_name[NR_OBJ_TYPES+1] =
	{"reference file",
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
	 "unknown type"
	};

static const char *typetostr(obj_t *o) {
	int t = o->type;
	if (t >= NR_OBJ_TYPES)
		t = NR_OBJ_TYPES;
	return obj_type_name[t];
}

/* Removes the two dashes at the end of the prefix */
#define IS_PREFIX(s, prefix) !strncmp(s, prefix, strlen(prefix) - 2)

#define asprintf_safe(args...)					\
do {								\
	if (asprintf(args) == -1 )				\
		fail("asprintf failed: %s", strerror(errno));	\
} while(0)

/*
 * Get the type of a symbol from the name of the kabi file
 *
 * It allocates the string which must be freed by the caller.
 */
static char *filenametotype(char *filename) {
	char *base = basename(filename);
	char *prefix= NULL, *name = NULL, *type = NULL;

	if ( sscanf(base, "%m[a-z]--%m[^.].txt", &prefix, &name) != 2 )
		fail("Unexpected file name: %s\n", filename);

	if (IS_PREFIX(prefix, TYPEDEF_FILE))
		type = name;
	else if (IS_PREFIX(prefix, STRUCT_FILE)||
		 IS_PREFIX(prefix, UNION_FILE) ||
		 IS_PREFIX(prefix, ENUM_FILE))
		asprintf_safe(&type, "%s %s", prefix, name);
	else
		fail("Unexpected file prefix: %s\n", prefix);

	free(prefix);
	if (name != type)
		free(name);

	return type;
}

static int c_precedence(obj_t *o) {
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
static bool is_paren_needed(obj_t *node) {
	obj_t *child = node->ptr;

	while(child) {
		if (c_precedence(child) < c_precedence(node))
			return true;

		child = child->ptr;
	}
	return false;
}

static char *print_margin_offset(const char *prefix, const char *s, int depth) {
	size_t len = snprintf(NULL, 0, "%-*s", depth * C_INDENT_OFFSET, s) + 1;
	char *ret;

	if (prefix)
		len += strlen(prefix);

	if (!len)
		return NULL;
	ret = malloc(len);

	snprintf(ret, len, "%s%-*s",
		 prefix ? prefix : "", depth * C_INDENT_OFFSET, s);

	return ret;
}

static char *print_margin(const char *prefix, int depth) {
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

void free_pp(pp_t pp) {
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
char *_prefix_str(char **s, char *p, bool space, bool freep) {
	size_t lenp = strlen(p), lens = 0, newlen;

	if (*s)
		lens = strlen(*s);
	newlen = lens + lenp + 1;

	if (space)
		newlen++;

	*s = realloc(*s, newlen);
	if (!*s)
		fail("realloc failed in _prefix_str(): %s\n", strerror(errno));

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

static char *prefix_str(char **s, char *p) {
	if (!p)
		return *s;
	return _prefix_str(s, p, false, false);
}

static char *prefix_str_free(char **s, char *p) {
	if (!p)
		return *s;
	return _prefix_str(s, p, false, true);
}

static char *prefix_str_space(char **s, char *p) {
	if (!p)
		return *s;
	return _prefix_str(s, p, true, false);
}

/*
 * Add suffix p at the end of string s (realocated)
 *
 * space: add a space between p and s
 * freep: free the string p
 */
static char *_postfix_str(char **s, char *p, bool space, bool freep) {
	int lenp = strlen(p), lens = 0, newlen;
	if (*s)
		lens = strlen(*s);
	newlen = lens + lenp + 1;

	if (space)
		newlen++;

	*s = realloc(*s, newlen);
	if (!*s)
		fail("realloc failed in _postfix_str(): %s\n", strerror(errno));

	if (lens == 0)
		(*s)[0] = '\0';
	if (space)
		strcat(*s, " ");
	strcat(*s, p);

	if (freep)
		free(p);

	return *s;
}

static char *postfix_str(char **s, char *p) {
	if (!p)
		return *s;
	return _postfix_str(s, p, false, false);
}

static char *postfix_str_free(char **s, char *p) {
	if (!p)
		return *s;
	return _postfix_str(s, p, false, true);
}

static pp_t print_base(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};

	asprintf_safe(&ret.prefix, "%s ", o->base_type);

	return ret;
}

static pp_t print_constant(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};

	asprintf_safe(&ret.prefix, "%s = %li", o->name, (long)o->constant);

	return ret;
}

static pp_t print_reffile(obj_t *o, int depth,
			  const char *prefix) {
	pp_t ret = {NULL, NULL};
	char *s = filenametotype(o->base_type);

	s = realloc(s, strlen(s) + 2);
	strcat(s, " ");
	ret.prefix = s;

	return ret;
}

/* Print a struct, enum or an union */
static pp_t print_structlike(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL}, tmp;
	obj_list_t *list = NULL;
	char *s, *margin;

	if (o->name)
		asprintf_safe(&s, "%s %s {\n", typetostr(o), o->name);
	else
		asprintf_safe(&s, "%s {\n", typetostr(o));

	if (o->member_list)
		list = o->member_list->first;
	while (list) {
		tmp = _print_tree(list->member, depth+1, true, prefix);
		postfix_str_free(&s, tmp.prefix);
		postfix_str_free(&s, tmp.postfix);
		postfix_str(&s, o->type == __type_enum ? ",\n" :";\n");
		list = list->next;
	}

	margin = print_margin(prefix, depth);
	postfix_str_free(&s, margin);
	postfix_str(&s, "}");

	ret.prefix = s;
	return ret;
}

static pp_t print_func(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL}, return_type;
	obj_list_t *list = NULL;
	obj_t *next = o->ptr;
	char *s, *name, *margin;

	return_type = _print_tree(next, depth, false, prefix);
	ret.prefix = return_type.prefix;

	if (o->name)
		name = o->name;
	else
		name = "";

	asprintf_safe(&s, "%s(\n", name);

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

static pp_t print_array(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};
	char *s;
	obj_t *next = o ->ptr;

	ret = _print_tree(next, depth, false, prefix);

	asprintf_safe(&s, "[%lu]", o->constant);
	prefix_str_free(&ret.postfix, s);

	return ret;
}

static pp_t print_ptr(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};
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
static pp_t print_varlike(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};
	char *s = NULL;

	if (is_bitfield(o))
		asprintf_safe(&s, "%s:%i",
			      o->name, o->last_bit - o->first_bit + 1);
	else
		s = o->name;

	ret = _print_tree(o->ptr, depth, false, prefix);

	if (s)
		postfix_str(&ret.prefix, s);

	if (is_bitfield(o))
		free(s);

	return ret;
}

static pp_t print_typedef(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};

	ret = _print_tree(o->ptr, depth, false, prefix);

	prefix_str(&ret.prefix, "typedef ");
	postfix_str(&ret.prefix, o->name);

	return ret;
}

static pp_t print_qualifier(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};

	ret = _print_tree(o->ptr, depth, false, prefix);
	prefix_str_space(&ret.prefix, o->base_type);

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
static pp_t _print_tree(obj_t *o, int depth, bool newline, const char *prefix) {
	pp_t ret = {NULL, NULL};
	char *margin;

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
			asprintf_safe(&offstr, "0x%lx:%2i-%-2i ",
				      o->offset, o->first_bit, o->last_bit);
		else
			asprintf_safe(&offstr, "0x%lx ", o->offset);
		margin = print_margin_offset(prefix, offstr, depth);
		free(offstr);
	} else
		margin = print_margin(prefix, depth);

	prefix_str_free(&ret.prefix, margin);
	return ret;
}

static void print_tree_prefix(obj_t *root, const char *prefix, FILE *stream) {
	pp_t s = _print_tree(root, 0, true, prefix);

	fprintf(stream, "%s%s;\n",
	       s.prefix ? s.prefix : "",
	       s.postfix ? s.postfix : "");
	free_pp(s);
}

void print_tree(obj_t *root) {
	print_tree_prefix(root, NULL, stdout);
}

static int fill_parent_cb(obj_t *o, void *args) {
	obj_t **parent = (obj_t **) args;

	o->parent = *parent;
	*parent = o;

	return 0;
}

/*
 * Walk the tree and fill all the parents field
 */
void fill_parent(obj_t *root) {
	obj_t *parent = NULL;
	walk_tree(root, fill_parent_cb, &parent);
}

static int walk_list(obj_list_t *list, cb_t cb_pre, cb_t cb_in, cb_t cb_post,
		     void *args, bool ptr_first) {
	int ret = CB_CONT;

	while ( list ) {
		ret = walk_tree3(list->member, cb_pre, cb_in, cb_post,
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
		    void *args, bool ptr_first) {
	int ret = CB_CONT;

	if (o->ptr) {
		ret = walk_tree3(o->ptr, cb_pre, cb_in, cb_post,
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
int walk_tree3(obj_t *o, cb_t cb_pre, cb_t cb_in, cb_t cb_post,
	       void *args, bool ptr_first) {
	obj_list_t *list = NULL;
	int ret = CB_CONT;

	if (cb_pre && (ret = cb_pre(o, args)))
		return ret;

	if (o->member_list)
		list = o->member_list->first;


	if (ptr_first)
		ret = walk_ptr(o, cb_pre, cb_in, cb_post, args, ptr_first);
	else
		ret = walk_list(list, cb_pre, cb_in, cb_post, args, ptr_first);
	if (ret == CB_FAIL)
		return ret;

	if (cb_in && (ret = cb_in(o, args)))
		return ret;

	if (ptr_first)
		ret = walk_list(list, cb_pre, cb_in, cb_post, args, ptr_first);
	else
		ret = walk_ptr(o, cb_pre, cb_in, cb_post, args, ptr_first);
	if (ret == CB_FAIL)
		return ret;

	if (cb_post && (ret = cb_post(o, args)))
		return ret;

	return ret;
}

/*
 * Simple tree walk with a prefix callback
 *
 * It walks the member_list subtree first.
 */
int walk_tree(obj_t *root, cb_t cb, void *args) {
	return walk_tree3(root, cb, NULL, NULL, args, false);
}

static void _show_node(FILE *f, obj_t *o, int margin) {
	if (o)
		fprintf(f,
			"\%*s<%s, \"%s\", \"%s\", %p, %p, %p, %lu, %i, %i>\n",
			margin, "", typetostr(o), o->name, o->base_type,
			o, o->parent, o->ptr,
			o->offset, o->first_bit, o->last_bit);
	else
		fprintf(f, "\%*s<(nil)>\n", margin, "");
}

static void show_node(obj_t *o, int margin) {
	_show_node(stdout, o, margin);
}

static int debug_node(obj_t *node, void *args) {
	int *depth = (int *) args;

	show_node(node, *depth * DBG_INDENT_OFFSET);
	(*depth)++;

	return CB_CONT;
}

static int dec_depth(obj_t *node, void *args) {
	int *depth = (int *) args;

	(*depth)--;

	return CB_CONT;
}

/*
 * Print a raw representation of the internal object tree
 */
int debug_tree(obj_t *root) {
	int depth = 0;

	return walk_tree3(root, debug_node, NULL, dec_depth, &depth, false);
}

static void _print_node_list(const char *s, const char *prefix,
			     obj_list_t *list, obj_list_t *last, FILE *stream) {
	obj_list_t *l = list;

	fprintf(stream, "%s:\n", s);
	while (l && l != last) {
		print_tree_prefix(l->member, prefix, stream);
		l = l->next;
	}
}

static void print_node_list(const char *s, const char *prefix,
			    obj_list_t *list, FILE *stream) {
	_print_node_list(s, prefix, list, NULL, stream);
}

typedef enum {
	CMP_SAME = 0,
	CMP_OFFSET,	/* Only the offset has changed */
	CMP_DIFF,	/* Nodes are differents */
	CMP_REFFILE,	/* A refered symbol has changed */
} cmp_ret_t;

static int compare_two_files(char *filename, char *newfile, bool follow);

static int cmp_node_reffile(obj_t *o1, obj_t *o2) {
	char *s1 = filenametotype(o1->base_type);
	char *s2 = filenametotype(o2->base_type);
	int ret, len;

	ret = cmp_str(s1, s2);
	free(s1);
	free(s2);

	if (ret)
		return CMP_DIFF;

	/*
	 * Compare the symbol referenced by file, but be careful not
	 * to follow imaginary declaration path.
	 *
	 * TODO: This is quite wasteful. We reopen files and parse
	 * them again many times.
	 */
	len = strlen(DECLARATION_PATH);
	if (strncmp(o1->base_type, DECLARATION_PATH, len) &&
	    strncmp(o1->base_type, DECLARATION_PATH, len) &&
	    compare_two_files(o1->base_type, o2->base_type, true))
		return CMP_REFFILE;

	return CMP_SAME;
}
static int cmp_nodes(obj_t *o1, obj_t *o2) {
	if ((o1->type != o2->type) ||
	    cmp_str(o1->name, o2->name) ||
	    ((o1->ptr == NULL) != (o2->ptr == NULL)) ||
	    (has_constant(o1) && (o1->constant != o2->constant)) ||
	    (has_index(o1) && (o1->index != o2->index)))
		return CMP_DIFF;

	if (o1->type == __type_reffile) {
		int ret;

		ret = cmp_node_reffile(o1, o2);
		if (ret)
			return ret;
	} else if (cmp_str(o1->base_type, o2->base_type))
		return CMP_DIFF;

	if (has_offset(o1) &&
	    ((o1->offset != o2->offset) ||
	     (o1->first_bit != o2->first_bit) ||
	     (o1->last_bit != o2->last_bit)))
		return CMP_OFFSET;

	return CMP_SAME;
}

obj_list_t *find_object(obj_t *o, obj_list_t *l) {
	obj_list_t *list = l;
	int ret;

	while (list) {
		ret = cmp_nodes(o, list->member);
		if (ret == CMP_SAME || ret == CMP_OFFSET)
			return list;
		list = list->next;
	}
	return NULL;
}

/*
 * We want to show practical output to the user.  For instance if a
 * struct member type change, we want to show which struct member
 * changed type, not that somewhere a "signed int" has been changed
 * into a "unsigned bin".
 *
 * For now, we consider that a useful output should start at a named
 * object or at a struct field or var (the field/var itself may be
 * unamed, typically when it's an union or struct of alternative
 * elements but it most likely contains named element).
 */
bool worthy_of_print(obj_t *o) {
	return (o->name != NULL) ||
		(o->type == __type_struct_member) ||
		(o->type == __type_var) ;
}

static void print_two_nodes(const char *s, obj_t *o1, obj_t *o2, FILE *stream) {

	while (!worthy_of_print(o1)) {
		o1 = o1->parent;
		o2 = o2->parent;
		if ((o1 == NULL) || (o2 == NULL))
			fail("No ancestor worthy of print\n");
	}
	fprintf(stream, "%s:\n", s);
	print_tree_prefix(o1, DEL_PREFIX, stream);
	print_tree_prefix(o2, ADD_PREFIX, stream);
}

obj_t *worthy_parent(obj_t *o) {
	do {
		if (worthy_of_print(o))
			return o;
	} while((o = o->parent));

	fail("No ancestor worthy of print\n");
}

typedef struct compare_config_s {
	bool debug;
	bool hide_kabi;
	bool hide_kabi_new;
	int follow;
	char *old_dir;
	char *new_dir;
	char *filename;
	char **flist;
	int flistsz;
	int flistcnt;
	int ret;
	/*
	 * The following options allow to hide some symbol changes in
	 * kABI comparison. Hides...
	 */
	int no_replaced; /* replaced symbols */
	int no_shifted;  /* symbols whose offset shifted */ 
	int no_inserted; /* symbols inserted in the middle of a struct/union */
	int no_deleted;  /* symbols removed in the middle (poke a hole) */
	int no_added;    /* symbols added at the end of a struct/union... */
	int no_removed;  /* symbols removed at the end of a struct/union... */
	int no_moved_files; /* file that has been moved (or removed) */
} compare_config_t;

compare_config_t compare_config = {false, false, 0,
				   NULL, NULL, NULL, NULL,
				   0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

int _compare_tree(obj_t *o1, obj_t *o2, FILE *stream) {
	obj_list_t *list1 = NULL, *list2 = NULL, *next;
	int ret = COMP_SAME, tmp;

	tmp = cmp_nodes(o1, o2);
	if (tmp) {
		if (tmp == CMP_REFFILE) {
			fprintf(stream, "symbol %s has changed\n",
				o1->base_type);
			ret = COMP_DIFF;
		} else if ((tmp == CMP_OFFSET && !compare_config.no_shifted) ||
			   (tmp == CMP_DIFF && !compare_config.no_replaced)) {
			const char *s =	(tmp == CMP_OFFSET) ?
				"Shifted" : "Replaced";
			print_two_nodes(s, o1, o2, stream);
			ret = COMP_DIFF;
		}
		return ret;
	}

	if (o1->member_list)
		list1 = o1->member_list->first;
	if (o2->member_list)
		list2 = o2->member_list->first;

	while ( list1 && list2 ) {
		if (cmp_nodes(list1->member, list2->member) == CMP_DIFF) {
			if ((next = find_object(list1->member, list2))) {
				/* Insertion */
				if (!compare_config.no_inserted) {
					_print_node_list("Inserted", ADD_PREFIX,
							 list2, next, stream);
					ret = COMP_DIFF;
				}
				list2 = next;
			} else if ((next = find_object(list2->member, list1))) {
				/* Removal */
				if (!compare_config.no_deleted) {
					_print_node_list("Deleted", DEL_PREFIX,
							 list1, next, stream);
					ret = COMP_DIFF;
				}
				list1 = next;
			}
		}

		if (_compare_tree(list1->member, list2->member, stream) !=
		    COMP_SAME)
			ret = COMP_DIFF;

		list1 = list1->next;
		list2 = list2->next;
		if (!list1 && list2) {
			if (!compare_config.no_added) {
				print_node_list("Added", ADD_PREFIX,
						list2, stream);
				ret = COMP_DIFF;
			}
			return ret;
		}
		if (list1 && !list2) {
			if (!compare_config.no_removed) {
				print_node_list("Removed", DEL_PREFIX,
						list1, stream);
				ret = COMP_DIFF;
			}
			return ret;
		}
	}

	if (o1->ptr && o2->ptr)
		if (_compare_tree(o1->ptr, o2->ptr, stream) != COMP_SAME)
			ret = COMP_DIFF;

	return ret;
}

/*
 * Compare two symbols and show the difference in a c-like format
 */
int compare_tree(obj_t *o1, obj_t *o2, FILE *stream) {
	return _compare_tree(o1, o2, stream);
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
static int hide_kabi_cb(obj_t *o, void *args) {
	obj_t *kabi_struct, *new, *old, *parent = o->parent, *keeper;
	obj_list_head_t *lh;
	obj_list_t *l;
	bool show_new_field = (bool) args;

	if (o->name) {
		if (!strncmp(o->name, "__UNIQUE_ID_rh_kabi_hide", 24))
			fail("Missed a kabi unique ID\n");

		/* Hide RH_KABI_DEPRECATE* */
		if (!strncmp(o->name, "rh_reserved_", 12) &&
		    strncmp(o->name, "rh_reserved_ptrs", 16)) {
			char *tmp = strdup(o->name+12);
			free(o->name);
			o->name = tmp;
		}
	}

	/* Hide RH_KABI_REPLACE */
	if ((o->type != __type_union) || o->name ||
	    !(lh = o->member_list) || list_empty(lh) ||
	    !(l = lh->first) || !(new = l->member) ||
	    !(l = l->next) || !(kabi_struct = l->member) ||
	    (kabi_struct->type != __type_var) ||
	    !kabi_struct->name ||
	    strncmp(kabi_struct->name, "__UNIQUE_ID_rh_kabi_hide", 24))
		return CB_CONT;

	if (!kabi_struct->ptr || kabi_struct->ptr->type != __type_struct ||
	    !(lh = kabi_struct->ptr->member_list) || list_empty(lh) ||
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
	    !( (parent->type == __type_var) ||
	       (parent->type == __type_struct_member) ) ||
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
	_free_obj(o, keeper);
	free(keeper);

	return CB_SKIP;
}

int hide_kabi(obj_t *root, bool show_new_field) {
	return walk_tree(root, hide_kabi_cb, (void *)show_new_field);
}

static FILE *fopen_safe(char *filename) {
	FILE *file;

	file = fopen(filename, "r");
	if (!file)
		fail("Failed to open kABI file: %s\n", filename);

	return file;
}

extern obj_t *parse(FILE *file);

struct {
	bool debug;
	bool hide_kabi;
	bool hide_kabi_new;
	FILE *file;
} show_config = {false, false, NULL};

void show_usage() {
	printf("Usage:\n"
	       "\tcompare [options] kabi_file...\n"
	       "\nOptions:\n"
	       "    -h, --help:\tshow this message\n"
	       "    -k, --hide-kabi:\thide changes made by RH_KABI_REPLACE()\n"
	       "    -n, --hide-kabi-new:\thide the kabi trickery made by"
	       " RH_KABI_REPLACE, but show the new field\n"

	       "    -d, --debug:\tprint the raw tree\n"
	       "    --no-offset:\tdon't display the offset of struct fields\n");
	exit(1);
}

/*
 * Performs the show command
 */
int show(int argc, char **argv) {
	obj_t *root;
	int opt, opt_index, ret = 0;
	struct option loptions[] = {
		{"debug", no_argument, 0, 'd'},
		{"hide-kabi", no_argument, 0, 'k'},
		{"hide-kabi-new", no_argument, 0, 'n'},
		{"help", no_argument, 0, 'h'},
		{"no-offset", no_argument, &display_options.no_offset, 1},
		{0, 0, 0, 0}
	};

	memset(&display_options, 0, sizeof(display_options));

	while ((opt = getopt_long(argc, argv, "dknh",
				  loptions, &opt_index)) != -1) {
		switch (opt) {
		case 0:
			break;
		case 'd':
			show_config.debug = true;
			break;
		case 'n':
			show_config.hide_kabi_new = true;
		case 'k':
			show_config.hide_kabi = true;
			break;
		case 'h':
		default:
			show_usage();
		}
	}

	if (optind >= argc)
		show_usage();

	while (optind < argc) {
		show_config.file = fopen_safe(argv[optind++]);

		root = parse(show_config.file);

		if (show_config.hide_kabi)
			hide_kabi(root, show_config.hide_kabi_new);

		if (show_config.debug)
			debug_tree(root);

		print_tree(root);
		if (optind < argc)
			putchar('\n');

		free_obj(root);
		fclose(show_config.file);
	}

	return ret;
}

static bool push_file(char *filename) {
	int i, sz = compare_config.flistsz;
	int cnt = compare_config.flistcnt;
	char **flist = compare_config.flist;

	for (i = 0; i < cnt; i++)
		if (!strcmp(flist[i], filename))
			return false;

	if (!sz) {
		compare_config.flistsz = sz = 16;
		compare_config.flist = flist = malloc(16 * sizeof(char *));
	}
	if (cnt >= sz) {
		sz *= 2;
		compare_config.flistsz = sz;
		compare_config.flist = flist =
			realloc(flist, sz * sizeof(char *));
	}

	flist[cnt] = strdup(filename);
	compare_config.flistcnt++;

	return true;
}

static void free_files() {
	int i;

	for (i = 0; i < compare_config.flistcnt; i++)
		free(compare_config.flist[i]);
	free(compare_config.flist);
	compare_config.flistcnt = compare_config.flistsz = 0;
}

void compare_usage() {
	printf("Usage:\n"
	       "\tcompare [options] kabi_dir kabi_dir [kabi_file...]\n"
	       "\tcompare [options] kabi_file kabi_file\n"
	       "\nOptions:\n"
	       "    -h, --help:\tshow this message\n"
	       "    -k, --hide-kabi:\thide changes made by RH_KABI_REPLACE()\n"
	       "    -n, --hide-kabi-new:\thide the kabi trickery made by"
	       " RH_KABI_REPLACE, but show the new field\n"
	       "    -d, --debug:\tprint the raw tree\n"
	       "    --follow:\t\tfollow referenced symbols\n"
	       "    --no-offset:\tdon't display the offset of struct fields\n"
	       "    --no-replaced:\thide replaced symbols"
	       " (symbols that changed, but hasn't moved)\n"
	       "    --no-shifted:\thide shifted symbols"
	       " (symbol that hasn't changed, but whose offset changed)\n"
	       "    --no-inserted:\t"
	       "hide symbols inserted in the middle of a struct, union...\n"
	       "    --no-deleted:\t"
	       "hide symbols removed from the middle of a struct, union...\n"
	       "    --no-added:\t\t"
	       "hide symbols added at the end of a struct, union...\n"
	       "    --no-removed:\t"
	       "hide symbols removed from the end of a struct, union...\n"
	       "    --no-moved-files:\thide changes caused by symbols "
	       "definition moving to another\n\t\t\t"
		"Warning: it also hides symbols that are removed entirely\n");

	exit(1);
}

/*
 * Parse two files and compare the resulting tree.
 *
 * filename: file to compare (relative to compare_config.*_dir)
 * newfile:  if not NULL, the file to use in compare_config.new_dir,
 *           otherwise, filename is used for both.
 * follow:   Are we here because we followed a reference file? If so,
 *           don't print anything and exit immediately if follow
 *           option isn't set.
 */
static int compare_two_files(char *filename, char *newfile, bool follow) {
	obj_t *root1, *root2;
	char *old_dir = compare_config.old_dir;
	char *new_dir = compare_config.new_dir;
	char *path1, *path2, *s = NULL, *filename2;
	FILE *file1, *file2, *stream;
	struct stat fstat;
	size_t sz;
	int ret = 0, tmp;

	if (follow && !compare_config.follow)
		return 0;

	/* Avoid infinite loop */
	if (!push_file(filename))
		return 0;

	asprintf_safe(&path1, "%s/%s", old_dir, filename);
	filename2 = newfile ? newfile : filename;
	asprintf_safe(&path2, "%s/%s", new_dir, filename2);

	if (stat(path2, &fstat) != 0) {
		if (errno == ENOENT) {
			/* Don't consider an incomplete definition a change */
			if (strncmp(filename2, DECLARATION_PATH,
				    strlen(DECLARATION_PATH)) &&
			    !compare_config.no_moved_files) {
				ret = EXIT_KABI_CHANGE;
				printf("Symbol removed or moved: %s\n",
				       filename);
			}

			free(path1);
			free(path2);

			return ret;
		}
		else
			fail("Failed to stat() file%s: %s\n",
			     path2, strerror(errno));
	}

	file1 = fopen_safe(path1);
	file2 = fopen_safe(path2);
	free(path1);
	free(path2);

	root1 = parse(file1);
	root2 = parse(file2);

	if (compare_config.hide_kabi) {
		hide_kabi(root1, compare_config.hide_kabi_new);
		hide_kabi(root2, compare_config.hide_kabi_new);
	}

	if (compare_config.debug && !follow) {
		debug_tree(root1);
		debug_tree(root2);
	}

	if (follow)
		stream = fopen("/dev/null", "w");
	else
		stream = open_memstream(&s, &sz);
	tmp = compare_tree(root1, root2, stream);

	if (tmp == COMP_DIFF) {
		if (!follow) {
			printf("Changes detected in: %s\n", filename);
			fflush(stream);
			fputs(s, stdout);
			putchar('\n');
		}
		ret = EXIT_KABI_CHANGE;
	}

	free_obj(root1);
	free_obj(root2);
	fclose(file1);
	fclose(file2);
	fclose(stream);
	free(s);

	return ret;

}

static bool compare_files_cb(char *kabi_path, void *arg) {
	compare_config_t *conf = (compare_config_t *)arg;
	char *filename;

	/* If conf->*_dir contains slashes, skip them */
	filename = kabi_path + strlen(conf->old_dir);
	while (*filename == '/')
		filename++;

	free_files();
	if (compare_two_files(filename, NULL, false))
		conf->ret = EXIT_KABI_CHANGE;

	return true;
}

#define COMPARE_NO_OPT(name) \
	{"no-"#name, no_argument, &compare_config.no_##name, 1}

/*
 * Performs the compare command
 */
int compare(int argc, char **argv) {
	int opt, opt_index;
	char *old_dir, *new_dir;
	struct stat sb1, sb2;
	struct option loptions[] = {
		{"debug", no_argument, 0, 'd'},
		{"hide-kabi", no_argument, 0, 'k'},
		{"hide-kabi-new", no_argument, 0, 'n'},
		{"help", no_argument, 0, 'h'},
		{"follow", no_argument, &compare_config.follow, 1},
		{"no-offset", no_argument, &display_options.no_offset, 1},
		COMPARE_NO_OPT(replaced),
		COMPARE_NO_OPT(shifted),
		COMPARE_NO_OPT(inserted),
		COMPARE_NO_OPT(deleted),
		COMPARE_NO_OPT(added),
		COMPARE_NO_OPT(removed),
		{"no-moved-files", no_argument,
		 &compare_config.no_moved_files, 1},
		{0, 0, 0, 0}
	};

	memset(&display_options, 0, sizeof(display_options));

	while ((opt = getopt_long(argc, argv, "dknh",
				  loptions, &opt_index)) != -1) {
		switch (opt) {
		case 0:
			break;
		case 'd':
			compare_config.debug = true;
			break;
		case 'n':
			compare_config.hide_kabi_new = true;
		case 'k':
			compare_config.hide_kabi = true;
			break;
		case 'h':
		default:
			compare_usage();
		}
	}

	if (argc < optind + 2) {
		printf("Wrong number of argument\n");
		compare_usage();
	}

	old_dir = compare_config.old_dir = argv[optind++];
	new_dir = compare_config.new_dir = argv[optind++];

	if ((stat(old_dir, &sb1) == -1) || (stat(new_dir, &sb2) == -1))
		fail("stat failed: %s\n", strerror(errno));

	if (S_ISREG(sb1.st_mode) && S_ISREG(sb2.st_mode)) {
		char *oldname = basename(old_dir);
		char *newname = basename(new_dir);

		if (optind != argc) {
			printf("Too many arguments\n");
			compare_usage();
		}
		compare_config.old_dir = dirname(old_dir);
		compare_config.new_dir = dirname(new_dir);

		return compare_two_files(oldname, newname, false);
	}

	if (!S_ISDIR(sb1.st_mode) || !S_ISDIR(sb2.st_mode)) {
		printf("Compare takes two directories or two regular"
		       " files as arguments\n");
		compare_usage();
	}

	if (optind == argc) {
		walk_dir(old_dir, false, compare_files_cb, &compare_config);

		return compare_config.ret;
	}

	while (optind < argc) {
		char *path, *filename;

		filename = compare_config.filename =  argv[optind++];
		asprintf_safe(&path, "%s/%s", old_dir, filename);

		if (stat(path, &sb1) == -1) {
			if (errno == ENOENT)
				fail("file does not exist: %s\n", path);
			fail("stat failed: %s\n", strerror(errno));
		}

		if (!S_ISREG(sb1.st_mode)) {
			printf("Compare third argument must be a regular file");
			compare_usage();
		}
		free(path);

		if (compare_two_files(filename, NULL, false))
			compare_config.ret = EXIT_KABI_CHANGE;
	}

	return compare_config.ret;
}
