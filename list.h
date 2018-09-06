#ifndef LIST_H_
#define LIST_H_

struct list_node {
	void *data;
	struct list_node *next;
	struct list_node *prev;
	struct list *list;
};

struct list {
	struct list_node *first;
	struct list_node *last;
	void (*free)(void *data);
	int len;
};

struct list *list_new(void (*free)(void *));
void list_init(struct list *list, void (*free)(void *));

/*
 * Frees every node contained by list.
 * Does not accept NULL value for list.
 */
void list_clear(struct list *list);

/*
 * Frees list and every node contained by list.
 * Does not accept NULL value for list.
 */
void list_free(struct list *list);

struct list_node *list_add(struct list *list, void *data);

/*
 * Removes node from the parent list, without freeing it.
 */
void list_del(struct list_node *node);

/*
 * Appends nodes from src to dst, emptying src.
 */
void list_concat(struct list *dst, struct list *src);

#define LIST_FOR_EACH(list, iter) \
	for ((iter) = (list)->first; (iter) != NULL; (iter) = (iter)->next)


static inline int list_len(struct list *list)
{
	return list->len;
}

static inline void *list_node_data(struct list_node *node)
{
	return node->data;
}

static inline struct list_node *list_node_add(struct list_node *node,
					      void *data)
{
	return list_add(node->list, data);
}

#endif /* LIST_H_ */
