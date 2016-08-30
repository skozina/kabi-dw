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

#ifdef DEBUG
#define debug(args...) do { printf(args); } while(0)
#else
#define debug(args...)
#endif

/* Return value when we detect a kABI change */
#define EXIT_KABI_CHANGE 3

/* Indentation offset for c-style and tree debug outputs */
#define C_INDENT_OFFSET   8
#define DBG_INDENT_OFFSET 4

/* diff -u style prefix for tree comparison */
#define ADD_PREFIX "+"
#define DEL_PREFIX "-"

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
 * name:	name of the symbol
 * base_type:	(base type) the type of the symbol,
 *		(qualifier) the type qualifier (const or volatile)
 * member_list: (struct, union, enum) list of members
 *              (function) list of arguments
 * ptr:		(pointer) object pointed to
 *		(typedef) defined type
 *		(function) return type
 *		(var) type
 * constant:	(constant) constant value of an enumeration
 * index:	index of array
 * offset:	(var) offset of a struct member
 * first_bit, last_bit: (var) bit range within the offset.
 *			Only valid if last_bit != 0
 *
 * Note the dual parent/child relationship with the n-ary member_list and the
 * the unary ptr. Only functions uses both.
 */
typedef struct obj {
	obj_types type;	
	char *name;
	char *base_type;
	obj_list_head_t *member_list;
	struct obj *ptr, *parent;
	union {
		unsigned long constant;
		unsigned long index;
		struct {
			unsigned long offset;
			unsigned char first_bit, last_bit;
		};
	};
} obj_t;

static inline bool has_offset(obj_t *o) {
	return o->type == __type_struct_member;
}

static inline bool has_constant(obj_t *o) {
	return o->type == __type_constant;
}

static inline bool has_index(obj_t *o) {
	return o->type == __type_array;
}

static inline bool is_bitfield(obj_t *o) {
	return o->last_bit != 0;
}

static inline bool is_terminal(obj_t *o) {
	switch(o->type) {
	case __type_reffile:
	case __type_base:
	case __type_constant:
		return true;
	default:
		return false;
	}
}

static inline bool is_unary(obj_t *o) {
	switch(o->type) {
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

static inline bool is_n_ary(obj_t *o) {
	switch(o->type) {
	case __type_struct:
	case __type_union:
	case __type_enum:
	case __type_func:
		return true;
	default:
		return false;
	}
}

/*
 * Display options
 *
 * Used for show and compare commands.
 */
struct dopt {
	int no_offset;		/* Don't display struct offset */
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
};
extern struct dopt display_options;

/* Return values for tree walk callbacks */
typedef enum {
	CB_CONT = 0,	/* Continue tree walk */
	CB_SKIP,	/* Skip the children of this node */
	CB_FAIL,	/* Failed: stop the walk */
} cb_ret_t;

typedef int cb_t(obj_t *o, void *args);

obj_list_t *new_list(obj_t *obj);
obj_list_head_t *new_list_head(obj_t *obj);
void list_add(obj_list_head_t *head, obj_t *obj);
void free_obj(obj_t *o);

obj_t *new_struct(char *name);
obj_t *new_union(char *name);
obj_t *new_enum(char *name);
obj_t *new_constant(char *name);
obj_t *new_reffile();
obj_t *new_func_add(char *name, obj_t *obj);
obj_t *new_typedef_add(char *name, obj_t *obj);
obj_t *new_var_add(char *name, obj_t *obj);
obj_t *new_struct_member_add(char *name, obj_t *obj);
obj_t *new_ptr_add(obj_t *obj);
obj_t *new_array_add(obj_t *obj);
obj_t *new_qualifier_add(obj_t *obj);

obj_t *new_base(char *base_type);

void print_tree(obj_t *root);
int debug_tree(obj_t *root);
void fill_parent(obj_t *root);
int walk_tree(obj_t *root, cb_t cb, void *args);
int walk_tree3(obj_t *o, cb_t cb_pre, cb_t cb_in, cb_t cb_post,
	       void *args, bool ptr_first);

/* Return values for the (_)compare_tree functions */
enum {
	COMP_SAME = 0,	/* Subtree are equal */
	COMP_DIFF,	/* Subtree differs */
	/*
	 * Subtree differs and we need to display the change at a higher level
	 * for the output to have enough context to be understandable (see 
	 * worthy_of_print())
	 */
	COMP_NEED_PRINT,
};
int compare_tree(obj_t *o1, obj_t *o2);
int hide_kabi(obj_t *root);

int show(int argc, char **argv);
int compare(int argc, char **argv);

#endif
