#ifndef MAIN_H_
#define	MAIN_H_

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

typedef struct {
	bool verbose;
	char *module_dir;
	char *kabi_dir;
	char **symbols;
	size_t symbol_cnt;
	bool *symbols_found;
	char **ksymtab;
	size_t ksymtab_len;
} generate_config_t;

typedef struct {
	bool verbose;
	char *module_dir;
	char *kabi_dir;
	char **symbols;
	size_t symbol_cnt;
} check_config_t;

#endif /* MAIN_H_ */
