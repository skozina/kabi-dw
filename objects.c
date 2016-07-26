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

obj_t *root = NULL;

obj_list_t *new_list(obj_t *obj) {
	obj_list_t *list = malloc(sizeof(obj_list_t));
	list->member = obj;
	list->next = NULL;
	return list;
}

void list_init(obj_list_head_t *head, obj_t *obj) {
	obj_list_t *list = new_list(obj);
	head->first = head->last = list;
}

obj_list_head_t *new_list_head(obj_t *obj) {
	obj_list_head_t *h = malloc(sizeof(obj_list_head_t));

	list_init(h, obj);

	return h;
}

bool list_empty(obj_list_head_t *head) {
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
	add_member(new, obj);				\
	return new;					\
}
#define CREATE_NEW_ADD_FUNC(type) _CREATE_NEW_ADD_FUNC(type, new)
#define CREATE_NEW_ADD_FUNC_NONAME(type)		\
_CREATE_NEW_ADD_FUNC(type, _new)			\
obj_t *new_##type##_add(obj_t *obj) {				\
	return _new_##type##_add(NULL, obj);			\
}

CREATE_NEW_FUNC(struct)
CREATE_NEW_FUNC(union)
CREATE_NEW_FUNC(enum)
CREATE_NEW_FUNC(func)
CREATE_NEW_FUNC(typedef)
CREATE_NEW_FUNC_NONAME(none)
CREATE_NEW_FUNC_NONAME(ptr)
CREATE_NEW_FUNC_NONAME(array)

CREATE_NEW_ADD_FUNC(struct)
CREATE_NEW_ADD_FUNC(union)
CREATE_NEW_ADD_FUNC(enum)
CREATE_NEW_ADD_FUNC(func)
CREATE_NEW_ADD_FUNC(typedef)
CREATE_NEW_ADD_FUNC_NONAME(none)
CREATE_NEW_ADD_FUNC_NONAME(ptr)
CREATE_NEW_ADD_FUNC_NONAME(array)

obj_t *new_base(char *base_type) {
	obj_t *new = new_obj(__type_base, NULL);

	new->base_type = malloc(MAX_BASE_TYPE_LEN);
	strcpy(new->base_type, base_type);

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
			       "base"};

const char *typetostr(obj_types t) {
	return obj_type_name[t];
}

bool print_node_pre(obj_t *node, int depth, bool is_newline){
	bool ret = true;

	if (is_newline)
		printf("%*s", depth*4, "");

	if (!node) {
		printf("(nil)\n");
		return ret;
	}

	switch(node->type) {
	case __type_struct:
	case __type_union:
	case __type_enum:
		if (strcmp(node->name,"(NULL)"))
			printf("%s %s {\n",
			       typetostr(node->type), node->name);
		else
			printf("%s {\n", typetostr(node->type));
		break;
	case __type_func:
		printf("%s (\n", node->name);
		break;
	case __type_ptr:
		if (node->name)
			printf("%s ", node->name);
		printf("*");
		ret = false;
		break;
	case __type_typedef:
		printf("typedef %s\n", node->name);
		break;
	case __type_array:
		printf("[%lu]", node->index);
		ret = false;
		break;
	case __type_base:
		printf("%s", node->base_type);
		if (node->name)
			printf(" %s", node->name);
		printf("\n");
		break;
	default:
		printf("<%s, \"%s\", \"%s\", %p %lu>\n",
		       typetostr(node->type), node->name,
		       node->base_type, node->ptr, node->constant);
	}

	return ret;
}

bool print_node_mid(obj_t *node, int depth, bool is_newline){
	bool ret = is_newline;

	if (!node)
		return ret;

	switch(node->type) {
	case __type_struct:
	case __type_union:
	case __type_enum:
		printf("%*s}\n", depth*4, "");
		ret = true;
		break;
	case __type_func:
		printf("%*s) ", depth*4, "");
		ret = false;
		break;
	default:
		;
	}

	return ret;
}

void walk_graph_rec(obj_t *o, int depth, bool is_newline) {
	obj_list_t *list = NULL;
	bool newline;
	int next_depth = depth;

	newline = print_node_pre(o, depth, is_newline);
	if (newline)
		next_depth++;

	if (o->member_list)
		list = o->member_list->first;

	while ( list ) {
		walk_graph_rec(list->member, next_depth, newline);
		list = list->next;
	}

	newline = print_node_mid(o, depth, newline);
	if (newline)
		next_depth = depth+1;
	else
		next_depth = depth;
	if (o->ptr)
		walk_graph_rec(o->ptr, next_depth, newline);
}

void walk_graph() {
	if (root)
		walk_graph_rec(root, 0, true);
	else {
		fprintf(stderr, "No root\n");
		exit(1);
	}
}
