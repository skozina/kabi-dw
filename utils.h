#ifndef UTILS_H_
#define	UTILS_H_

#define	fail(m...)	{			\
	fprintf(stderr, "%s():%d ", __func__, __LINE__);	\
	fprintf(stderr, m);				\
	exit(1);				\
}

static inline void *safe_malloc(size_t size) {
	void *result = malloc(size);
	if (result == NULL)
		fail("Malloc of size %zu failed", size);
	memset(result, 0, size);
	return (result);
}

extern void walk_dir(char *, bool (*)(char *, void *), void *);

#endif /* UTILS_H */
