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

#ifndef STACK_H_
#define	STACK_H_

typedef struct {
	unsigned int st_capacity; /* Total size of the stack */
	unsigned int st_count; /* Number of items stored */
	void **st_data; /* Stack itself */
} stack_t;

extern stack_t *stack_init(void);
extern void stack_destroy(stack_t *);
extern void stack_push(stack_t *, void *);
extern void *stack_pop(stack_t *);
extern void *stack_head(stack_t *);
extern void walk_stack(stack_t *, void (*)(void *, void *), void *);
extern void walk_stack_backward(stack_t *, void (*cb)(void *, void *), void *);

#endif /* STACK_H_ */
