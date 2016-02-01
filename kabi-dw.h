#ifndef KABI_DW_H_
#define	KABI_DW_H_

#include <unistd.h>
#include <stdbool.h>

#define	fail(m...)	{			\
	printf("%s():%d ", __func__, __LINE__);	\
	printf(m);				\
	exit(1);				\
}

extern bool print_symbol(const char *, const char *);

#endif /* KABI_DW_H_ */
