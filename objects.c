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

void add_member(obj_t *parent, obj_t *member) {
	obj_list_head_t *head = parent->member_list;

	if (!head) {
		head = malloc(sizeof(obj_list_head_t));
		parent->member_list = head;
	}

	list_add(head, member);
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
		if (c_precedence(child) < c_precedence(node))
			return true;
		child = child->ptr;
	}
	return false;
}

typedef struct print_node_args {
	int depth;
	bool newline;
	const char *prefix;
	char *elt_name; /* Name of a struct field or a var */
	/*
	 * Number of indirection that need to be handled.
	 * It also indicates that parentheses are needed since "*" has a
	 * lower precedence than array "[]" or functions "()"
	 */
	int ptrs;
} pn_args_t;

static void print_margin(const char *prefix, const char *s, int depth) {
	if (prefix)
		printf("%s", prefix);
	printf("%-*s", depth * C_INDENT_OFFSET, s);
}

static int print_node_pre(obj_t *node, void *args) {
	pn_args_t *pna = (pn_args_t *) args;
	char offstr[16];

	if (pna->newline) {
		if (node->type == __type_struct_member) {
			if (node->last_bit)
				snprintf(offstr, 16, "0x%lx:%2i-%-2i ",
					 node->offset,
					 node->first_bit,
					 node->last_bit);
			else
				snprintf(offstr, 16, "0x%lx ", node->offset);
		} else
			offstr[0] = 0;
		print_margin(pna->prefix, offstr, pna->depth);
	}

	if (!node)
		fail("No node\n");

	switch(node->type) {
	case __type_struct:
	case __type_union:
	case __type_enum:
		if (node->name)
			printf("%s %s {\n",
			       typetostr(node->type), node->name);
		else
			printf("%s {\n", typetostr(node->type));
		pna->depth++;
		goto out_newline;
	case __type_qualifier:
		printf("%s ", node->base_type);
		break;
	case __type_base:
		printf("%s ", node->base_type);
		if (node->name)
			fail("Base type has name %s\n", node->name);
		break;
	case __type_constant:
		printf("%s = %lx,\n", node->name, node->constant);
		goto out_newline;
	case __type_reffile:
	{
		char *type = filenametotype(node->base_type);
		printf("%s ", type);
		free(type);
		break;
	}
	case __type_var:
	case __type_struct_member:
		pna->elt_name = node->name;
		break;
	case __type_ptr:
		if (is_paren_needed(node))
			pna->ptrs++;
		break;
	default:
		;
	}

	pna->newline = false;
	return CB_CONT;

out_newline:
	pna->newline = true;
	return CB_CONT;
}

static int print_node_in(obj_t *node, void *args){
	pn_args_t *pna = (pn_args_t *) args;

	if (!node)
		fail("No node\n");

	switch(node->type) {
	case __type_func:
	{
		char *s = NULL;
		bool paren = pna->ptrs != 0;

		if (node->name)
			s = node->name;
		else if (pna->elt_name) {
			s = pna->elt_name;
			pna->elt_name = NULL;
		}
		if (paren) {
			putchar('(');
			while (pna->ptrs) {
				putchar('*');
				pna->ptrs--;
			}
		}
		if (s)
			printf("%s", s);
		if (paren)
			printf(")");
		puts(" (");
		pna->depth++;
		pna->newline = true;
		break;
	}
	default:
		;
	}

	return CB_CONT;
}

static int print_node_post(obj_t *node, void *args) {
	pn_args_t *pna = (pn_args_t *) args;

	if (!node)
		fail("No node\n");

	switch(node->type) {
	case __type_struct:
	case __type_union:
	case __type_enum:
	case __type_func:
		if (pna->depth == 0)
			fail("depth underflow\n");
		pna->depth--;
		print_margin(pna->prefix, "", pna->depth);
		if (node->type == __type_func)
			putchar(')');
		else
			putchar('}');
		if (pna->depth == 0) {
			puts(";");
			break;
		}
		goto out_sameline;
	case __type_ptr:
		if (!is_paren_needed(node))
			putchar('*');
		goto out_sameline;
	case __type_typedef:
		printf("typedef %s;\n", node->name);
		break;
	case __type_var:
	case __type_struct_member:
		if (pna->elt_name) {
			fputs(pna->elt_name, stdout);
			pna->elt_name = NULL;
		}
		if (pna->ptrs)
			fail("Unmatched ptrs\n");
		puts(";");
		break;
	case __type_array:
	{
		bool paren = pna->ptrs != 0;
		if (paren) {
			putchar('(');
			while (pna->ptrs) {
				putchar('*');
				pna->ptrs--;
			}
		}

		if (pna->elt_name) {
			fputs(pna->elt_name, stdout);
			pna->elt_name = NULL;
		}
		if (paren)
			printf(")");

		printf("[%lu]", node->index);
		goto out_sameline;
		break;
	}
	default:
		;
	}

	pna->newline = true;
	return CB_CONT;

out_sameline:
	pna->newline = false;
	return CB_CONT;

}

void _print_tree(obj_t *root, int depth, bool newline, const char *prefix) {
	pn_args_t pna = {depth, newline, prefix};
	walk_tree3(root, print_node_pre, print_node_in, print_node_post,
		   &pna, true);
}

void print_tree(obj_t *root) {
	_print_tree(root, 0, false, NULL);
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
		fprintf(f, "\%*s<%s, \"%s\", \"%s\", %p %lu %i %i>\n",
			margin, "", typetostr(o->type), o->name, o->base_type,
			o->ptr, o->offset, o->first_bit, o->last_bit);
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
	_print_tree(o1, 0, true, DEL_PREFIX);
	_print_tree(o2, 0, true, ADD_PREFIX);
}

static void _print_node_list(const char *s, const char *prefix,
			    obj_list_t *list, obj_list_t *last) {
	obj_list_t *l = list;

	printf("%s:\n", s);
	while (l && l != last) {
		_print_tree(l->member, 0, true, prefix);
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

static int cmp_nodes(obj_t *o1, obj_t *o2) {
	if ((o1->type != o2->type) ||
	    cmp_str(o1->name, o2->name) ||
	    (o1->type == __type_reffile ?
	     cmp_str(filenametotype(o1->base_type),
		     filenametotype(o2->base_type)) :
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
			const char *s =
				(tmp == CMP_OFFSET) ? "Shifted" : "Replaced";
			print_two_nodes(s, o1, o2);
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
				_print_node_list("Inserted", ADD_PREFIX,
						list2, next);
				list2 = next;
				ret = COMP_DIFF;
			} else if ((next = find_object(list2->member, list1))) {
				/* Removal */
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
			print_two_nodes("Replaced",
				       list1->member, list2->member);
		}
		if (tmp != COMP_SAME)
			ret = COMP_DIFF;

		list1 = list1->next;
		list2 = list2->next;
		if (!list1 && list2) {
			print_node_list("Added", ADD_PREFIX, list2);
			return COMP_DIFF;
		}
		if (list1 && !list2) {
			print_node_list("Removed", DEL_PREFIX, list1);
			return COMP_DIFF;
		}
	}

	if (o1->ptr && o2->ptr) {
		tmp = _compare_tree(o1->ptr, o2->ptr);
		if (tmp == COMP_NEED_PRINT) {
			if (worthy_of_print(o1->ptr))
				print_two_nodes("Replaced", o1, o2);
		}
		if (tmp != COMP_SAME)
			ret = tmp;
	}

	return ret;
}

int compare_tree(obj_t *o1, obj_t *o2) {
	int ret = _compare_tree(o1, o2);

	if (ret == COMP_NEED_PRINT)
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
