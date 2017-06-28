/*
	Copyright(C) 2016, Red Hat, Inc., Stanislav Kozina

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
 * A trivial stack implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "utils.h"
#include "stack.h"

#define	INIT_CAPACITY	10

stack_t *stack_init(void)
{
	stack_t *st = safe_zmalloc(sizeof(*st));

	st->st_capacity = INIT_CAPACITY;
	st->st_count = 0;
	st->st_data = safe_zmalloc(st->st_capacity * sizeof(*st->st_data));

	return st;
}

void stack_destroy(stack_t *st)
{
	if (st->st_count > 0)
		fail("Stack not empty!\n");

	free(st->st_data);
	(void) memset(st, 0, sizeof(*st));
	free(st);
}

void stack_push(stack_t *st, void *data)
{
	if (st->st_count == st->st_capacity) {
		st->st_capacity *= 2;
		st->st_data = realloc(st->st_data,
		    st->st_capacity * sizeof(*st->st_data));
	}

	st->st_data[st->st_count] = data;
	st->st_count++;
}

void *stack_pop(stack_t *st)
{
	if (st->st_count == 0)
		return NULL;

	st->st_count--;
	return st->st_data[st->st_count];
}

void *stack_head(stack_t *st)
{
	if (st->st_count == 0)
		return NULL;

	return st->st_data[st->st_count-1];
}

void walk_stack(stack_t *st, void (*cb)(void *, void *), void *arg)
{
	unsigned int i;

	for (i = 0; i < st->st_count; i++)
		cb(st->st_data[i], arg);
}

void walk_stack_backward(stack_t *st, void (*cb)(void *, void *), void *arg)
{
	unsigned int i;

	for (i = st->st_count; i > 0; i--)
		cb(st->st_data[i - 1], arg);
}
