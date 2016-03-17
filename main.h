#ifndef MAIN_H_
#define	MAIN_H_

#include <stdbool.h>

#define	DEFAULT_OUTPUT_DIR	"./output"
#define	MODULE_DIR		"/usr/lib/modules"
#define	DEBUG_MODULE_DIR	"/usr/lib/debug/lib/modules"

/* Default size of buffer for symbols loading */
#define	DEFAULT_BUFSIZE	64

#define	TYPEDEF_FILE	"typedef--"
#define	FUNC_FILE	"func--"
#define	STRUCT_FILE	"struct--"
#define	UNION_FILE	"union--"
#define	ENUM_FILE	"enum--"
#define	VAR_FILE	"var--"

extern char *output_dir;

typedef struct {
	char *module_dir;
	char **symbol_names;
	size_t symbol_cnt;
	bool *symbols_found;
	char **ksymtab;
	size_t ksymtab_len;
} config_t;

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

#endif /* MAIN_H_ */
