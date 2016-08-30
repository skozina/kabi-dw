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
 * Internal representation of symbols
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
	new->name = name; /* Should it be strduped ? */

	return new;
}

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

static const char *typetostr(obj_types t) {
	if (t >= NR_OBJ_TYPES)
		t = NR_OBJ_TYPES;
	return obj_type_name[t];
}

/* Removes the two dashes at the end of the prefix */
#define IS_PREFIX(s, prefix) !strncmp(s, prefix, strlen(prefix) - 2)

char *filenametotype(char *filename) {
	char *base = basename(filename);
	char *prefix= NULL, *name = NULL, *type = NULL;

	if ( sscanf(base, "%m[a-z]--%m[^.].txt", &prefix, &name) != 2 )
		fail("Unexpected file name: %s\n", filename);

	if (IS_PREFIX(prefix, TYPEDEF_FILE)||
	    IS_PREFIX(prefix, ENUM_FILE)) {
		type = name;
	} else if (IS_PREFIX(prefix, STRUCT_FILE)||
		   IS_PREFIX(prefix, UNION_FILE)) {
		type = malloc(strlen(prefix)+strlen(name)+2);
		sprintf(type, "%s %s", prefix, name);
	} else
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

static bool is_paren_needed(obj_t *node) {
	obj_t *child = node->ptr;

	while(child) {
		if (c_precedence(child) < c_precedence(node)) {
			child->close_paren++;
			return true;
		}
		child = child->ptr;
	}
	return false;
}

typedef struct print_node_args {
	int depth;
	bool newline;
	const char *prefix;
	/*
	 * Number of indirection that need to be handled.
	 * It also indicates that parentheses are needed since "*" has a
	 * lower precedence than array "[]" or functions "()"
	 */
	int ptrs;
} pn_args_t;

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

typedef struct {
	char *prefix;
	char *postfix;
} pp_t;

void free_pp(pp_t pp) {
	free(pp.prefix);
	free(pp.postfix);
}

static pp_t _print_tree(obj_t *o, int depth, bool newline, const char *prefix);

/* Add prefix p at the begining of string s */
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

/*static char *postfix_str_space(char **s, char *p) {
	if (!p)
		return *s;
	return _postfix_str(s, p, true, false);
	}*/

static pp_t print_base(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};

	ret.prefix = malloc(strlen(o->base_type) + 2);

	strcpy(ret.prefix, o->base_type);
	strcat(ret.prefix, " ");

	return ret;
}

#define CONSTANT_FMT "%s = %lu"
static pp_t print_constant(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};
	size_t len = snprintf(NULL, 0, CONSTANT_FMT, o->name, o->constant) + 1;

	ret.prefix = malloc(len);
	snprintf(ret.prefix, len, CONSTANT_FMT, o->name, o->constant);

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

static pp_t print_structlike(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL}, tmp;
	obj_list_t *list = NULL;
	char *s, *margin;
	size_t sz;

	sz = strlen(typetostr(o->type)) + 4;
	if (o->name)
		sz += strlen(o->name) + 1;
	s = malloc(sz);

	if (o->name)
		snprintf(s, sz, "%s %s {\n", typetostr(o->type), o->name);
	else
		snprintf(s, sz, "%s {\n", typetostr(o->type));

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
	postfix_str(&s,  depth == 0 ? "};\n" : "}");

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

	s = malloc(strlen(name)+3);
	sprintf(s, "%s(\n", name);

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
	postfix_str(&s, depth == 0 ? ");\n" : ")");

	ret.postfix = s;
	return ret;
}

static pp_t print_array(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};
	char s[16];
	obj_t *next = o ->ptr;

	ret = _print_tree(next, depth, false, prefix);

	snprintf(s, 16, "[%lu]", o->constant);
	prefix_str(&ret.postfix, s);

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

static pp_t print_varlike(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};
	char *s = NULL;

	if (is_bitfield(o)) {
		s = malloc(strlen(o->name) + 5);
		sprintf(s, "%s:%i", o->name, o->last_bit - o->first_bit + 1);
	} else
		s = o->name;

	ret = _print_tree(o->ptr, depth, false, prefix);

	if (s)
		postfix_str(&ret.prefix, s);

	if (!depth)
		postfix_str(&ret.postfix, ";\n");

	if (is_bitfield(o))
		free(s);

	return ret;
}

static pp_t print_typedef(obj_t *o, int depth, const char *prefix) {
	pp_t ret = {NULL, NULL};

	ret = _print_tree(o->ptr, depth, false, prefix);

	prefix_str(&ret.prefix, "typedef ");
	postfix_str(&ret.postfix, ";\n");

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

static void show_node(obj_t *o, int margin); /*FIXME: DBG */

static pp_t _print_tree(obj_t *o, int depth, bool newline, const char *prefix) {
	pp_t ret = {NULL, NULL};
	char *margin;

	if (!o)
		fail("NULL pointer in _print_tree\n");
	debug("_print_tree(): %s\n", typetostr(o->type));

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
		fail("WIP: doesn't handle %s\n", typetostr(o->type));
	}

	if (!newline)
		return ret;

	if (o->type == __type_struct_member && !display_options.no_offset) {
		char offstr[16];
		if (o->last_bit)
			snprintf(offstr, 16, "0x%lx:%2i-%-2i ",
				 o->offset, o->first_bit, o->last_bit);
		else
			snprintf(offstr, 16, "0x%lx ", o->offset);
		margin = print_margin_offset(prefix, offstr, depth);
	} else
		margin = print_margin(prefix, depth);

	prefix_str_free(&ret.prefix, margin);
	return ret;
}

static void print_tree_prefix(obj_t *root, const char *prefix) {
	pp_t s = _print_tree(root, 0, true, prefix);

	printf("%s%s",
	       s.prefix ? s.prefix : "",
	       s.postfix ? s.postfix : "");
	free_pp(s);
}

struct dopt display_options;

void print_tree(obj_t *root) {
	print_tree_prefix(root, NULL);
}

static int fill_parent_cb(obj_t *o, void *args) {
	obj_t **parent = (obj_t **) args;

	o->parent = *parent;
	*parent = o;

	return 0;
}

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

int walk_tree(obj_t *root, cb_t cb, void *args) {
	return walk_tree3(root, cb, NULL, NULL, args, false);
}

static void _show_node(FILE *f, obj_t *o, int margin) {
	if (o)
		fprintf(f,
			"\%*s<%s, \"%s\", \"%s\", %p, %p, %p, %lu, %i, %i>\n",
			margin, "", typetostr(o->type), o->name, o->base_type,
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

int debug_tree(obj_t *root) {
	int depth = 0;

	return walk_tree3(root, debug_node, NULL, dec_depth, &depth, false);
}

static void print_two_nodes(const char *s, obj_t *o1, obj_t *o2) {
	printf("%s:\n", s);
	print_tree_prefix(o1, DEL_PREFIX);
	print_tree_prefix(o2, ADD_PREFIX);
}

static void _print_node_list(const char *s, const char *prefix,
			    obj_list_t *list, obj_list_t *last) {
	obj_list_t *l = list;

	printf("%s:\n", s);
	while (l && l != last) {
		print_tree_prefix(l->member, prefix);
		l = l->next;
	}
}

static void print_node_list(const char *s, const char *prefix,
			   obj_list_t *list) {
	_print_node_list(s, prefix, list, NULL);
}

static int cmp_str(char *s1, char *s2) {
	if ((s1 == NULL) != (s2 == NULL))
		return 1;
	if (s1)
		return strcmp(s1, s2);
	return 0;
}

typedef enum {
	CMP_SAME = 0,
	CMP_OFFSET,	/* Only the offset has changed */
	CMP_DIFF,	/* Nodes are differents */
} cmp_ret_t;

static int cmp_node_reffile(obj_t *o1, obj_t *o2) {
	char *s1 = filenametotype(o1->base_type);
	char *s2 = filenametotype(o2->base_type);
	int ret;

	ret = cmp_str(s1, s2);
	free(s1);
	free(s2);

	return ret;
}
static int cmp_nodes(obj_t *o1, obj_t *o2) {
	if ((o1->type != o2->type) ||
	    cmp_str(o1->name, o2->name) ||
	    (o1->type == __type_reffile ?
	     cmp_node_reffile(o1, o2) :
	     cmp_str(o1->base_type, o2->base_type)) ||
	    ((o1->ptr == NULL) != (o2->ptr == NULL)) ||
	    (has_constant(o1) && (o1->constant != o2->constant)) ||
	    (has_index(o1) && (o1->index != o2->index)))
		return CMP_DIFF;

	if ((o1->offset != o2->offset) ||
	    (o1->first_bit != o2->first_bit) ||
	    (o1->last_bit != o2->last_bit))
		return CMP_OFFSET;

	return CMP_SAME;
}

obj_list_t *find_object(obj_t *o, obj_list_t *l) {
	obj_list_t *list = l;

	while (list) {
		if (cmp_nodes(o, list->member) == CMP_SAME)
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

int _compare_tree(obj_t *o1, obj_t *o2) {
	obj_list_t *list1 = NULL, *list2 = NULL, *next;
	int ret, tmp;

	tmp = cmp_nodes(o1, o2);
	if (tmp) {
		if (worthy_of_print(o1)) {
			if ((tmp == CMP_OFFSET && !display_options.no_offset) ||
			    (tmp != CMP_OFFSET && !display_options.no_replaced))
			{
					const char *s =	(tmp == CMP_OFFSET) ?
						"Shifted" : "Replaced";

					print_two_nodes(s, o1, o2);
			}
			return COMP_DIFF;
		} else {
			if (tmp == CMP_OFFSET)
				fail("CMP_OFFSET unexpected here\n");
			return COMP_NEED_PRINT;
		}
	}

	if (o1->member_list)
		list1 = o1->member_list->first;
	if (o2->member_list)
		list2 = o2->member_list->first;

	while ( list1 && list2 ) {
		if (cmp_nodes(list1->member, list2->member) == CMP_DIFF) {
			if ((next = find_object(list1->member, list2))) {
				/* Insertion */
				if (!display_options.no_inserted)
					_print_node_list("Inserted", ADD_PREFIX,
							 list2, next);
				list2 = next;
				ret = COMP_DIFF;
			} else if ((next = find_object(list2->member, list1))) {
				/* Removal */
				if (!display_options.no_deleted)
					_print_node_list("Deleted", DEL_PREFIX,
							 list1, next);
				list1 = next;
				ret = COMP_DIFF;
			}
		}

		tmp = _compare_tree(list1->member, list2->member);
		if (tmp == COMP_NEED_PRINT) {
			if (!worthy_of_print(list1->member))
				fail("Unworthy objects are unexpected here\n");
			if (!display_options.no_replaced)
				print_two_nodes("Replaced",
						list1->member, list2->member);
		}
		if (tmp != COMP_SAME)
			ret = COMP_DIFF;

		list1 = list1->next;
		list2 = list2->next;
		if (!list1 && list2) {
			if (!display_options.no_added)
				print_node_list("Added", ADD_PREFIX, list2);
			return COMP_DIFF;
		}
		if (list1 && !list2) {
			if (!display_options.no_removed)
				print_node_list("Removed", DEL_PREFIX, list1);
			return COMP_DIFF;
		}
	}

	if (o1->ptr && o2->ptr) {
		tmp = _compare_tree(o1->ptr, o2->ptr);
		if (tmp == COMP_NEED_PRINT) {
			if (worthy_of_print(o1->ptr) &&
			    !display_options.no_replaced)
				print_two_nodes("Replaced", o1, o2);
		}
		if (tmp != COMP_SAME)
			ret = tmp;
	}

	return ret;
}

int compare_tree(obj_t *o1, obj_t *o2) {
	int ret = _compare_tree(o1, o2);

	if (ret == COMP_NEED_PRINT && !display_options.no_replaced)
		print_two_nodes("Replaced", o1, o2);

	if (ret != COMP_SAME)
		return COMP_DIFF;

	return COMP_SAME;
}

/*
 * Hide some RH_KABI magic
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
 */
static int hide_kabi_cb(obj_t *o, void *args) {
	obj_t *kabi_struct, *new, **parent = (obj_t **)args;
	obj_list_head_t *lh;
	obj_list_t *l;

	if (o->name && !strncmp(o->name, "__UNIQUE_ID_rh_kabi_hide", 24))
		fail("Missed a kabi unique ID\n");

	if ((o->type != __type_union) || o->name ||
	    !(lh = o->member_list) || list_empty(lh) ||
	    !(l = lh->first) || !(new = l->member) ||
	    !(l = l->next) || !(kabi_struct = l->member) ||
	    (kabi_struct->type != __type_var) ||
	    !kabi_struct->name ||
	    strncmp(kabi_struct->name, "__UNIQUE_ID_rh_kabi_hide", 24)) {
		*parent = o;
		return CB_CONT;
	}

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
	 *                <base, "(null)", "int", (nil) 0 0 0>
	 *       <var, "(null)", "(null)", 0x1ea97a0 0 0 0>
	 *          <union, "(null)", "(null)", (nil) 0 0 0>
	 *
	 * by that:
	 * <struct member, "new_field", "(null)", 0x1ea9540 16 0 0>
	 *    <base, "(null)", "int", (nil) 0 0 0>
	 *
	 * Parent is always an unary node, struct_member or var
	 */

	if (!*parent ||
	    !( ((*parent)->type == __type_var) ||
	       ((*parent)->type == __type_struct_member) ) ||
	    ((*parent)->ptr != o) || (*parent)->name) {
		_show_node(stderr, *parent, 0);
		fail("Unexpected parent\n");
	}
	if (new->type != __type_var) {
		_show_node(stderr, new, 0);
		fail("Unexpected new\n");
	}

	(*parent)->name = new->name;
	(*parent)->ptr = new->ptr;
	_free_obj(o, new);
	free(new);

	return CB_SKIP;
}

int hide_kabi(obj_t *root) {
	void *parent;
	return walk_tree(root, hide_kabi_cb, &parent);
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
	FILE *file;
} show_config = {false, false, NULL};

void show_usage() {
	printf("Usage:\n"
	       "\tcompare [options] kabi_file [kabi_file]\n"
	       "\nGeneral options:\n"
	       "    -k, --hide-kabi:\thide some rh specific kabi trickery\n"
	       "    -d, --debug:\tprint the raw tree\n"
	       "    --no-offset:\tdon't display the offset of struct fields\n");
	exit(1);
}

#define DISPLAY_NO_OPT(name) \
	{"no-"#name, no_argument, &display_options.no_##name, 1}

int show(int argc, char **argv) {
	obj_t *root;
	int opt, opt_index, ret = 0;
	struct option loptions[] = {
		{"debug", no_argument, 0, 'd'},
		{"hide-kabi", no_argument, 0, 'k'},
		{"help", no_argument, 0, '?'},
		DISPLAY_NO_OPT(offset),
		{0, 0, 0, 0}
	};

	memset(&display_options, 0, sizeof(display_options));

	while ((opt = getopt_long(argc, argv, "dk",
				  loptions, &opt_index)) != -1) {
		switch (opt) {
		case 0:
			break;
		case 'd':
			show_config.debug = true;
			break;
		case 'k':
			show_config.hide_kabi = true;
			break;
		default:
			show_usage();
		}
	}

	if (optind + 1 != argc)
		show_usage();

	show_config.file = fopen_safe(argv[optind]);

	root = parse(show_config.file);

	if (show_config.hide_kabi)
		hide_kabi(root);

	if (show_config.debug)
		debug_tree(root);

	print_tree(root);

	free_obj(root);
	fclose(show_config.file);

	return ret;
}

struct {
	bool debug;
	bool hide_kabi;
	char *path1;
	char *path2;
} compare_config = {false, false, NULL, NULL};

void compare_usage() {
	printf("Usage:\n"
	       "\tcompare [options] kabi_file kabi_file\n"
	       "\tcompare [options] kabi_dir kabi_dir\n"
	       "\nGeneral options:\n"
	       "    -k, --hide-kabi:\thide some rh specific kabi trickery\n"
	       "    -d, --debug:\tprint the raw tree\n"
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
	       "hide symbols removed from the end of a struct, union...\n");
	exit(1);
}

static int compare_two_files(char *path1, char *path2) {
	obj_t *root1, *root2;
	FILE *file1, *file2;
	int ret = 0, tmp;

	file1 = fopen_safe(path1);
	file2 = fopen_safe(path2);

	root1 = parse(file1);
	root2 = parse(file2);

	if (compare_config.hide_kabi) {
		hide_kabi(root1);
		hide_kabi(root2);
	}

	if (compare_config.debug) {
		debug_tree(root1);
		debug_tree(root2);
	}

	printf("Comparing %s\n", basename(path1));
	tmp = compare_tree(root1, root2);
	if (tmp == COMP_NEED_PRINT)
		fail("compare_tree still need to print\n");
	if (tmp == COMP_DIFF)
		ret = EXIT_KABI_CHANGE;

	free_obj(root1);
	free_obj(root2);
	fclose(file1);
	fclose(file2);

	return ret;

}

typedef struct cf_cb {
	char *kabi_dir_old;
	char *kabi_dir_new;
	char *file_name;
} cf_cb_t;

static bool compare_files_cb(char *kabi_path, void *arg) {
	cf_cb_t *conf = (cf_cb_t *)arg;
	struct stat fstat;
	char *temp_kabi_path;

	/* If conf->*_dir contains slashes, skip them */
	conf->file_name = kabi_path + strlen(conf->kabi_dir_old);
	while (*conf->file_name == '/')
		conf->file_name++;

	if (asprintf(&temp_kabi_path, "%s/%s", conf->kabi_dir_new,
	    conf->file_name) == -1)
		fail("asprintf() failed\n");

	if (stat(temp_kabi_path, &fstat) != 0) {
		if (errno == ENOENT)
			printf("Symbol removed or moved: %s\n",
			       conf->file_name);
		else
			fail("Failed to stat() file%s: %s\n", temp_kabi_path,
			    strerror(errno));

		goto out;
	}

	compare_two_files(kabi_path, temp_kabi_path);

out:
	free(temp_kabi_path);
	return true;
}

int compare(int argc, char **argv) {
	int opt, opt_index, ret = 0;
	char *path1, *path2;
	struct stat sb1, sb2;
	cf_cb_t conf = {NULL, NULL, NULL};
	struct option loptions[] = {
		{"debug", no_argument, 0, 'd'},
		{"hide-kabi", no_argument, 0, 'k'},
		{"help", no_argument, 0, '?'},
		DISPLAY_NO_OPT(offset),
		DISPLAY_NO_OPT(replaced),
		DISPLAY_NO_OPT(shifted),
		DISPLAY_NO_OPT(inserted),
		DISPLAY_NO_OPT(deleted),
		DISPLAY_NO_OPT(added),
		DISPLAY_NO_OPT(removed),
		{0, 0, 0, 0}
	};

	memset(&display_options, 0, sizeof(display_options));

	while ((opt = getopt_long(argc, argv, "dk",
				  loptions, &opt_index)) != -1) {
		switch (opt) {
		case 0:
			break;
		case 'd':
			compare_config.debug = true;
			break;
		case 'k':
			compare_config.hide_kabi = true;
			break;
		default:
			compare_usage();
		}
	}

	if (optind + 2 != argc) {
		printf("%i %i\n", optind, argc);
		compare_usage();
	}

	path1 = compare_config.path1 = argv[optind++];
	path2 = compare_config.path2 = argv[optind];

	if ((stat(path1, &sb1) == -1) || (stat(path2, &sb2) == -1))
		fail("stat failed: %s\n", strerror(errno));

	if (S_ISREG(sb1.st_mode)) {
		if (S_ISREG(sb2.st_mode))
			return compare_two_files(path1, path2);
		else
			fail("Second file is not a regular file\n");
	}

	if (S_ISDIR(sb1.st_mode)) {
		if (!S_ISDIR(sb2.st_mode))
			fail("Second file is not a directory\n");
	} else
		fail("Only support directories and regular files\n");

	conf.kabi_dir_old = path1;
	conf.kabi_dir_new = path2;
	walk_dir(path1, false, compare_files_cb, &conf);

	return ret;
}
