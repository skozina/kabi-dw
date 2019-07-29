#ifndef RECORD_H_
#define RECORD_H_

#include <stdbool.h>
#include <stdio.h>

#include "list.h"
#include "stack.h"
#include "objects.h"

#define RECORD_VERSION_DECLARATION -1

/*
 * Structure of the database record:
 *
 * key: record key, usually includes path the file, where the type is
 *      defined (may include pseudo path, like <declaration>);
 *      Does not contain version and the .txt suffix.
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
 *
 * link: name of weak link alisas for the weak aliases.
 *
 * free: type specific function to free the record
 *       (there are normal, weak and assembly records).
 *
 * dump: type specific function for record output.
 *
 * dependents: objects that reference this record.
 *
 * list_node: node containing the record, record only belong to one list
 *            at a time(usually record_list.records)
 *
 * failed: number of times the record could not be used for merging
 */
struct record {
	const char *key;
	int version;
	int ref_count;
	char *cu;
	const char *origin;
	stack_t *stack;
	obj_t *obj;
	char *link;
	void (*free)(struct record *);
	void (*dump)(struct record *, FILE *);

	struct list dependents;
	struct list_node *list_node;
	unsigned int failed;
};

static inline const char *record_get_key(struct record *record)
{
	return record->key;
}

static inline int record_get_version(struct record *record)
{
	return record->version;
}

static inline bool record_is_declaration(struct record *record)
{
	return record->version == RECORD_VERSION_DECLARATION;
}

bool record_same_declarations(struct record *r1, struct record *r2,
			      struct set *processed);

#endif /* RECORD_H_ */
