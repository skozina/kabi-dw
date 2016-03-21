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
	char *module_dir; /* Path to  the kernel modules to process */
	char *kabi_dir; /* Where to put the output */
	char **symbols; /* List of symbols to generate */
	size_t symbol_cnt;
	bool *symbols_found;
	char **ksymtab; /* ksymtab of the current kernel module */
	size_t ksymtab_len;
} generate_config_t;

typedef struct {
	bool verbose;
	char *module_dir; /* Path to the kernel modules to check */
	char *kabi_dir; /* Path to the stored kabi information */
	char **symbols; /* List of symbols used to generate the info as hint */
	size_t symbol_cnt;
	char *temp_kabi_dir; /* Temporary directory to store the new kabi */
} check_config_t;

#endif /* MAIN_H_ */
