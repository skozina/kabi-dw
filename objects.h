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

#include <stdbool.h>
#include <stdio.h>

#include "list.h"
#include "utils.h"

#ifdef DEBUG
#define debug(args...) do { printf(args); } while (0)
#else
#define debug(args...)
#endif

struct set;

enum merge_flag {
	MERGE_DEFAULT = 0,
	MERGE_FLAG_DECL_MERGE = 1 << 0,
	MERGE_FLAG_VER_IGNORE = 1 << 1,
	MERGE_FLAG_DECL_EQ = 1 << 2,
};

typedef enum {
	__type_reffile,
	__type_struct,
	__type_union,
	__type_enum,
	__type_func,
	__type_ptr,
	__type_typedef,
	__type_array,
	__type_var, /* a variable, member of an union or a function argument */
	__type_struct_member,
	__type_qualifier, /* a type qualifier such as "const" or "volatile" */
	__type_base,
	__type_constant, /* An element of an enumeration */
	__type_assembly,
	__type_weak,
	NR_OBJ_TYPES
} obj_types;

struct obj;
typedef struct obj_list {
	struct obj *member;
	struct obj_list *next;
} obj_list_t;

typedef struct obj_list_head {
	obj_list_t *first, *last;
	struct obj *object;
} obj_list_head_t;

/*
 * Structure representing symbols. Several field are overloaded.
 *
 * type:	type of the symbol (such as struct, function, pointer, base
 *		type...)
 * is_bitfield:	(var) It's a bitfield
 * first_bit, last_bit:	(var) bit range within the offset.
 * name:	name of the symbol
 * ref_record:	(reffile) pointer to the referenced record (only while
 *              generating records, otherwise base_type with string is used)
 * base_type:	(base type) the type of the symbol,
 *		(qualifier) the type qualifier (const or volatile)
 *		(reffile) path to the file
 * alignment:	value of DW_AT_alignment attribute or 0 if not present
 * member_list: (struct, union, enum) list of members
 *              (function) list of arguments
 * ptr:		(pointer) object pointed to
 *		(typedef) defined type
 *		(function) return type
 *		(var) type
 * constant:	(constant) constant value of an enumeration
 * index:	(array) index of array
 * link:	(weak) weak alias link
 * offset:	(var) offset of a struct member
 * depend_rec_node:	(reffile) node from dependents field of record where
 *			this obj references.
 *
 * Note the dual parent/child relationship with the n-ary member_list and the
 * the unary ptr. Only functions uses both.
 */
typedef struct obj {
	obj_types type;
	unsigned char is_bitfield, first_bit, last_bit;
	union {
		const char *name;
		struct record *ref_record;
	};
	const char *base_type;
	unsigned alignment;
	unsigned int byte_size;
	obj_list_head_t *member_list;
	struct obj *ptr, *parent;
	union {
		unsigned long constant;
		unsigned long index;
		char *link;
		unsigned long offset;
		struct list_node *depend_rec_node;
	};
} obj_t;

static inline bool has_offset(obj_t *o)
{
	return o->type == __type_struct_member;
}

static inline bool has_constant(obj_t *o)
{
	return o->type == __type_constant;
}

static inline bool has_index(obj_t *o)
{
	return o->type == __type_array;
}

static inline bool is_bitfield(obj_t *o)
{
	return o->is_bitfield != 0;
}

static inline bool is_terminal(obj_t *o)
{
	switch (o->type) {
	case __type_reffile:
	case __type_base:
	case __type_constant:
		return true;
	default:
		return false;
	}
}

static inline bool is_unary(obj_t *o)
{
	switch (o->type) {
	case __type_ptr:
	case __type_typedef:
	case __type_array:
	case __type_var:
	case __type_struct_member:
	case __type_qualifier:
		return true;
	default:
		return false;
	}
}

static inline bool is_n_ary(obj_t *o)
{
	switch (o->type) {
	case __type_struct:
	case __type_union:
	case __type_enum:
	case __type_func:
		return true;
	default:
		return false;
	}
}

static inline bool is_weak(obj_t *o)
{
	return o->type == __type_weak;
}

/*
 * Display options
 *
 * Used for show and compare commands.
 */
struct dopt {
	int no_offset;		/* Don't display struct offset */
};
extern struct dopt display_options;

/* Return values for tree walk callbacks */
typedef enum {
	CB_CONT = 0,	/* Continue tree walk */
	CB_SKIP,	/* Skip the children of this node */
	CB_FAIL,	/* Failed: stop the walk */
} cb_ret_t;

typedef int cb_t(obj_t *o, void *args);

obj_list_t *obj_list_new(obj_t *obj);
obj_list_head_t *obj_list_head_new(obj_t *obj);
void obj_list_add(obj_list_head_t *head, obj_t *obj);
void obj_free(obj_t *o);

obj_t *obj_struct_new(char *name);
obj_t *obj_union_new(char *name);
obj_t *obj_enum_new(char *name);
obj_t *obj_constant_new(char *name);
obj_t *obj_reffile_new();
obj_t *obj_func_new_add(char *name, obj_t *obj);
obj_t *obj_typedef_new_add(char *name, obj_t *obj);
obj_t *obj_var_new_add(char *name, obj_t *obj);
obj_t *obj_struct_member_new_add(char *name, obj_t *obj);
obj_t *obj_ptr_new_add(obj_t *obj);
obj_t *obj_array_new_add(obj_t *obj);
obj_t *obj_qualifier_new_add(obj_t *obj);
obj_t *obj_assembly_new(char *name);
obj_t *obj_weak_new(char *name);

obj_t *obj_basetype_new(char *base_type);

void obj_print_tree(obj_t *root);
void obj_print_tree__prefix(obj_t *root, const char *prefix, FILE *stream);
int obj_debug_tree(obj_t *root);
void obj_fill_parent(obj_t *root);
int obj_walk_tree(obj_t *root, cb_t cb, void *args);
int obj_walk_tree3(obj_t *o, cb_t cb_pre, cb_t cb_in, cb_t cb_post,
	       void *args, bool ptr_first);

int obj_hide_kabi(obj_t *root, bool show_new_field);

obj_t *obj_parse(FILE *file, char *fn);
obj_t *obj_merge(obj_t *o1, obj_t *o2, unsigned int flags);
void obj_dump(obj_t *o, FILE *f);

bool obj_eq(obj_t *o1, obj_t *o2, bool ignore_versions);

bool obj_same_declarations(obj_t *o1, obj_t *o2, struct set *processed);

#endif
