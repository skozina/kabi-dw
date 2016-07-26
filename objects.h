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

#ifndef _OBJECTS_H
#define _OBJECTS_H

/* Should be abl to contain "const volatile long unsigned int" */
#define MAX_BASE_TYPE_LEN 40

typedef enum {
	__type_none,
	__type_struct,
	__type_union,
	__type_enum,
	__type_func,
	__type_ptr,
	__type_typedef, /* do we keep that, or should it be always expanded */
	__type_array,
	__type_base,
} obj_types;

struct obj;
typedef struct obj_list {
	struct obj *member;
	struct obj_list *next;
} obj_list_t;

typedef struct obj_list_head {
	obj_list_t *first, *last;
} obj_list_head_t;

/*
 * Structure representing symbols
 *
 * type:	type of the symbol (such as struct, function, pointer, base type...)
 * name:	name of the symbol
 * base_type:	(base type) the type of the symbol,
 *		(function) the type of the return value.
 * member_list: (struct, union, enum) list of members
 *              (function) list of arguments
 * ptr:		(pointer) object pointed to
 *		(typedef) defined type
 *		(function) return type
 * constant:	(enum) constant value
 * index:	index of array
 */
typedef struct obj {
	obj_types type;
	char *name;
	char *base_type;
	obj_list_head_t *member_list;
	struct obj *ptr;
	union {
		unsigned long constant;
		unsigned long index;
	};
} obj_t;

obj_list_t *new_list(obj_t *obj);
obj_list_head_t *new_list_head(obj_t *obj);
void list_add(obj_list_head_t *head, obj_t *obj);
void add_member(obj_t *symbol, obj_t *member);

obj_t *new_struct(char *name);
obj_t *new_union(char *name);
obj_t *new_enum(char *name);
obj_t *new_func(char *name);
obj_t *new_typedef(char *name);
obj_t *new_none();
obj_t *new_ptr();
obj_t *new_array();
obj_t *new_struct_add(char *name, obj_t *obj);
obj_t *new_union_add(char *name, obj_t *obj);
obj_t *new_enum_add(char *name, obj_t *obj);
obj_t *new_func_add(char *name, obj_t *obj);
obj_t *new_typedef_add(char *name, obj_t *obj);
obj_t *new_none_add(obj_t *obj);
obj_t *new_ptr_add(obj_t *obj);
obj_t *new_array_add(obj_t *obj);

obj_t *new_base(char *base_type);

void walk_graph();
void pop_obj();


#endif
