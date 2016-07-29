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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "objects.h"
#include "utils.h"

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

void free_obj(obj_t *o) {
	obj_list_t *list = NULL, *next;

	if (!o)
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
		free_obj(list->member);
		next = list->next;
		free(list);
		list = next;
	}

	if(o->ptr)
		free_obj(o->ptr);

	free(o);
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
CREATE_NEW_FUNC_NONAME(none)
CREATE_NEW_FUNC_NONAME(array)
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

const char *obj_type_name[] = {"none",
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
			       "constant"};

static const char *typetostr(obj_types t) {
	return obj_type_name[t];
}

typedef struct print_node_args {
	int depth;
	bool newline;
} pn_args_t;

static int print_node_pre(obj_t *node, void *args){
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
		printf("%-*s", pna->depth * 4, offstr);
	}

	if (!node) {
		printf("(nil)\n");
		goto out_newline;
	}

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
		break;
	case __type_func:
		printf("%s (\n", node->name);
		pna->depth++;
		break;
	case __type_ptr:
		if (node->name)
			printf("%s ", node->name);
		printf("*");
		goto out_sameline;
	case __type_typedef:
		printf("typedef %s\n", node->name);
		break;
	case __type_var:
		if (node->name)
			printf("%s ", node->name);
		goto out_sameline;
	case __type_struct_member:
		if (node->name)
			printf("%s ", node->name);
		goto out_sameline;
	case __type_array:
		printf("[%lu]", node->index);
		if (node->base_type)
			printf("%s\n", node->base_type);
		else
			goto out_sameline;
		break;
	case __type_qualifier:
		printf("%s ", node->base_type);
		goto out_sameline;
	case __type_base:
		printf("%s", node->base_type);
		if (node->name)
			printf(" %s", node->name);
		printf("\n");
		break;
	case __type_constant:
		printf("%s = %lx\n", node->name, node->constant);
		break;
	default:
		printf("<%s, \"%s\", \"%s\", %p %lu>\n",
		       typetostr(node->type), node->name,
		       node->base_type, node->ptr, node->constant);
	}

out_newline:
	pna->newline = true;
	return 0;

out_sameline:
	pna->newline = false;
	return 0;
}

static int print_node_in(obj_t *node, void *args){
	pn_args_t *pna = (pn_args_t *) args;

	if (!node)
		return 0;

	switch(node->type) {
	case __type_struct:
	case __type_union:
	case __type_enum:
		if (pna->depth == 0)
			fail("depth undeflow\n");
		pna->depth--;
		printf("%*s}\n", pna->depth * 4, "");
		pna->newline = true;
		break;
	case __type_func:
		if (pna->depth == 0)
			fail("depth undeflow\n");
		pna->depth--;
		printf("%*s) ", pna->depth * 4, "");
		pna->newline = false;
		break;
	default:
		;
	}

	return 0;
}

void print_tree(obj_t *root) {
	pn_args_t pna = {0, false};
	walk_tree3(root, print_node_pre, print_node_in, NULL, &pna);
}


int walk_tree3(obj_t *o, cb_t cb_pre, cb_t cb_in, cb_t cb_post, void *args) {
	obj_list_t *list = NULL;
	int ret = 0;

	if (cb_pre && (ret = cb_pre(o, args)))
		return ret;

	if (o->member_list)
		list = o->member_list->first;

	while ( list ) {
		ret = walk_tree3(list->member, cb_pre, cb_in, cb_post, args);
		if (ret)
			return ret;
		list = list->next;
	}

	if (cb_in && (ret = cb_in(o, args)))
		return ret;

	if (o->ptr)
		ret = walk_tree3(o->ptr, cb_pre, cb_in, cb_post, args);

	if (cb_post && (ret = cb_post(o, args)))
		return ret;

	return ret;
}

int walk_tree(obj_t *root, cb_t cb, void *args) {
	return walk_tree3(root, cb, NULL, NULL, args);
}

static void show_node(obj_t *o, int margin) {
	printf("\%*s<%s, \"%s\", \"%s\", %p %lu %i %i>\n",
	       margin, "", typetostr(o->type), o->name, o->base_type,
	       o->ptr, o->offset, o->first_bit, o->last_bit);
}

static int debug_node(obj_t *node, void *args) {
	int *depth = (int *) args;

	show_node(node, *depth * 4);
	(*depth)++;

	return 0;
}

static int dec_depth(obj_t *node, void *args) {
	int *depth = (int *) args;

	(*depth)--;

	return 0;
}

int debug_tree(obj_t *root) {
	int depth = 0;

	return walk_tree3(root, debug_node, NULL, dec_depth, &depth);
}

static void show_two_nodes(const char *s, obj_t *o1, obj_t *o2) {
	printf("%s:\n", s);
	show_node(o1, 8);
	show_node(o2, 8);
}

static void show_node_list(const char *s, obj_list_t *list) {
	obj_list_t *l = list;

	printf("%s:\n", s);
	while (l) {
		show_node(l->member, 8);
		l = l->next;
	}
}

static int compare_tree_rec(obj_t *o1, obj_t *o2, cb2_t cb, void *args) {
	obj_list_t *list1 = NULL, *list2 = NULL;
	int ret = 0;

	if (cb && (ret = cb(o1, o2, args)))
		return ret;

	if (o1->member_list)
		list1 = o1->member_list->first;
	if (o2->member_list)
		list2 = o2->member_list->first;

	while ( list1 && list2 ) {
		ret = compare_tree_rec(list1->member, list2->member, cb, args);

		list1 = list1->next;
		list2 = list2->next;
		if (!list1 && list2) {
			show_node_list("Nodes added", list2);
			return 1;
		}
		if (list1 && !list2) {
			show_node_list("Nodes removed", list1);
			return 1;
		}
	}

	if (o1->ptr && o2->ptr)
		ret = compare_tree_rec(o1->ptr, o2->ptr, cb, args);

	return ret;
}

static int cmp_str(char *s1, char *s2) {
	if ((s1 == NULL) != (s2 == NULL))
		return 1;
	if (s1)
		return strcmp(s1, s2);
	return 0;
}

static int cmp_node(obj_t *o1, obj_t *o2, void *args) {
	if ((o1->type != o2->type) ||
	    cmp_str(o1->name, o2->name) ||
	    cmp_str(o1->base_type, o2->base_type) ||
	    (o1->offset != o2->offset) ||
	    (o1->first_bit != o2->first_bit) ||
	    (o1->last_bit != o2->last_bit) ||
	    ((o1->ptr == NULL) != (o2->ptr == NULL))) {
		show_two_nodes("Nodes differ", o1, o2);
		return 1;
	}

	return 0;
}

int compare_tree(obj_t *o1, obj_t *o2) {
	return compare_tree_rec(o1, o2, cmp_node, NULL);
}
