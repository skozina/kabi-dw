#ifndef KABI_DW_H_
#define	KABI_DW_H_

#include <unistd.h>
#include <stdbool.h>

#define	fail(m...)	{			\
	fprintf(stderr, "%s():%d ", __func__, __LINE__);	\
	fprintf(stderr, m);				\
	exit(1);				\
}

extern void print_symbols(char *, char **, size_t);

#endif /* KABI_DW_H_ */
