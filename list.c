#include "list.h"

#include <stdlib.h>

#include "utils.h"

struct list *list_new(void (*free)(void *))
{
	struct list *list = safe_zmalloc(sizeof(*list));

	list_init(list, free);

	return list;
}

void list_init(struct list *list, void (*free)(void *))
{
	list->free = free;
	list->first = NULL;
	list->last = NULL;
	list->len = 0;
}

void list_clear(struct list *list)
{
	struct list_node *next;

	next = list->first;
	while (next != NULL) {
		struct list_node *curr;

		curr = next;
		next = next->next;

		if (list->free && curr->data)
			list->free(curr->data);

		free(curr);
	}

	list->first = NULL;
	list->last = NULL;
	list->len = 0;
}

void list_free(struct list *list)
{
	list_clear(list);
	free(list);
}

struct list_node *list_add(struct list *list, void *data)
{
	struct list_node *node = safe_zmalloc(sizeof(*node));

	node->data = data;
	node->next = NULL;
	node->prev = list->last;
	node->list = list;

	if (list_len(list) == 0)
		list->first = node;
	else
		list->last->next = node;
	list->last = node;
	list->len++;

	return node;
}

void list_del(struct list_node *node)
{
	struct list *list;
	struct list_node *next_temp;

	list = node->list;
	if (list->first == node)
		list->first = node->next;
	if (list->last == node)
		list->last = node->prev;

	next_temp = node->next;
	if (node->next)
		node->next->prev = node->prev;
	if (node->prev)
		node->prev->next = next_temp;

	list->len--;

	free(node);
}

void list_concat(struct list *dst, struct list *src)
{
	struct list_node *iter;

	if (list_len(src) == 0)
		return;

	LIST_FOR_EACH(src, iter)
		iter->list = dst;

	if (dst->len == 0) {
		dst->first = src->first;
		dst->last = src->last;
		dst->len = src->len;
	} else {
		dst->last->next = src->first;
		src->first->prev = dst->last;
		dst->last = src->last;
		dst->len += src->len;
	}

	src->first = NULL;
	src->last = NULL;
	src->len = 0;
}
