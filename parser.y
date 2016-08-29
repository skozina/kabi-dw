/*
	Copyright(C) 2016, Red Hat, Inc., Jerome Marchand

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

%{
#include "parse.h"
#include "parser.h"
#include <limits.h>

#include "utils.h"

#define abort(...)				\
{						\
	fprintf(stderr, __VA_ARGS__);		\
	YYABORT;				\
}

%}

%union {
	int i;
	unsigned int ui;
	long l;
	unsigned long ul;
	void *ptr;
	char *str;
	obj_t *obj;
	obj_list_head_t *list;
}

%token <str> IDENTIFIER STRING SRCFILE
%token <l> CONSTANT

%token NEWLINE
%token TYPEDEF
%token CONST VOLATILE
%token STRUCT UNION ENUM ELLIPSIS
%token STACK

%type <str> type_qualifier
%type <obj> typed_type base_type reference_file array_type
%type <obj> type ptr_type variable_var_list func_type elt enum_elt enum_type
%type <obj> union_type struct_type struct_elt
%type <obj> declaration_var declaration_typedef declaration kabi_dw_file
%type <list> elt_list arg_list enum_list struct_list

%parse-param {obj_t **root}

%%

kabi_dw_file:
	cu_file source_file stack_list declaration NEWLINE
	{
	    $$ = *root = $declaration;
	    fill_parent(*root);
	}
	;

cu_file:
	IDENTIFIER STRING NEWLINE
	{
	    if (strcmp($IDENTIFIER,"CU"))
		abort("Wrong CU keyword: \"%s\"\n", $IDENTIFIER);
	    free($IDENTIFIER);
	    free($STRING);
	}
	;

source_file:
	IDENTIFIER SRCFILE ':' CONSTANT NEWLINE
	{
	    if (strcmp($IDENTIFIER,"File"))
		abort("Wrong file keyword: \"%s\"\n", $IDENTIFIER);
	    free($IDENTIFIER);
	    free($SRCFILE);
	}
	;

stack_list:
	%empty
	| stack_list stack_elt NEWLINE
	;

stack_elt:
	STACK STRING
	{
		free($STRING);
	}
	;

/* Possible types are struct union enum func typedef and var */
declaration:
	struct_type
	| union_type
	| enum_type
	| func_type
	| declaration_typedef
	| declaration_var
	;

declaration_typedef:
	TYPEDEF IDENTIFIER NEWLINE type
	{
	    $$ = new_typedef_add($IDENTIFIER, $type);
	}
	;

declaration_var:
	IDENTIFIER IDENTIFIER type
	{
	    if (strcmp($1,"var"))
		abort("Wrong var keyword: \"%s\"\n", $1);
	    free($1);
	    $$ = new_var_add($2, $type);
	}
	;

type:
	base_type
	| reference_file
	| struct_type
	| union_type
	| enum_type
	| func_type
	| ptr_type
	| array_type
	| typed_type
	;

struct_type:
	STRUCT IDENTIFIER '{' NEWLINE '}'
	{
	    $$ = new_struct($IDENTIFIER);
	}
	| STRUCT IDENTIFIER '{' NEWLINE struct_list NEWLINE '}'
	{
	    $$ = new_struct($IDENTIFIER);
	    $$->member_list = $struct_list;
	}
	;

struct_list:
	struct_elt
	{
	    $$ = new_list_head($struct_elt);
	}
	| struct_list NEWLINE struct_elt
	{
	    list_add($1, $struct_elt);
	    $$ = $1;
	}
	;

struct_elt:
	CONSTANT IDENTIFIER type
	{
	    $$ = new_struct_member_add($IDENTIFIER, $type);
	    $$->offset = $CONSTANT;
	}
	| CONSTANT ':' CONSTANT '-' CONSTANT IDENTIFIER type
	{
	    if ($5 > UCHAR_MAX || $3 > $5)
		abort("Invalid offset: %lx:%lu:%lu\n", $1, $3, $5);
	    $$ = new_struct_member_add($IDENTIFIER, $type);
	    $$->offset = $1;
	    $$->first_bit = $3;
	    $$->last_bit = $5;
	}
	;

union_type:
	UNION IDENTIFIER '{' NEWLINE '}'
	{
	    $$ = new_union($IDENTIFIER);
	}
	| UNION IDENTIFIER '{' NEWLINE elt_list NEWLINE '}'
	{
	    $$ = new_union($IDENTIFIER);
	    $$->member_list = $elt_list;
	    $elt_list->object = $$;
	}
	;

enum_type:
	ENUM IDENTIFIER '{' NEWLINE enum_list NEWLINE '}'
	{
	    $$ = new_enum($IDENTIFIER);
	    $$->member_list = $enum_list;
	    $enum_list->object = $$;
	}
	;

enum_list:
	enum_elt
	{
	    $$ = new_list_head($enum_elt);
	}
	| enum_list NEWLINE enum_elt
	{
	    list_add($1, $enum_elt);
	    $$ = $1;
	}
	;

enum_elt:
	IDENTIFIER '=' CONSTANT
	{
	    $$ = new_constant($IDENTIFIER);
	    $$->constant = $CONSTANT;
	}
	;

func_type:
	IDENTIFIER IDENTIFIER '(' NEWLINE arg_list ')' NEWLINE type
	{
	    if (strcmp($1,"func"))
		abort("Wrong func keyword: \"%s\"\n", $1);
	    free($1);
	    $$ = new_func_add($2, $type);
	    $$->member_list = $arg_list;
	    if ($arg_list)
		    $arg_list->object = $$;
	}
	| IDENTIFIER reference_file /* protype define as typedef */
	{
	    if (strcmp($IDENTIFIER,"func"))
		abort("Wrong func keyword: \"%s\"\n", $IDENTIFIER);
	    free($IDENTIFIER);
	    $$ = new_func_add(NULL, $reference_file);
	}
	;

arg_list:
	%empty
	{
	    $$ = NULL;
	}
	| elt_list NEWLINE
	{
	    $$ = $elt_list;
	}
	| elt_list NEWLINE variable_var_list NEWLINE
	{
	    list_add($elt_list, $variable_var_list);
	    $$ = $elt_list;
	}
	;

variable_var_list:
	IDENTIFIER ELLIPSIS
	{
	    /* TODO: there may be a better solution */
	    $$ = new_var_add(NULL, new_base(strdup("...")));
	}
	;

elt_list:
	elt
	{
	    $$ = new_list_head($elt);
	}
	| elt_list NEWLINE elt
	{
	    list_add($1, $elt);
	    $$ = $1;
	}
	;

elt:
	IDENTIFIER type
	{
	    $$ = new_var_add($IDENTIFIER, $type);
	}
	;

ptr_type:
	'*' type
	{
	    $$ = new_ptr_add($type);
	}
	;

array_type:
        '['CONSTANT ']' type
	{
	    $$ = new_array_add($type);
	    $$->index = $CONSTANT;
	}
	;

typed_type:
	type_qualifier type
	{
	    $$ = new_qualifier_add($type);
	    $$->base_type = $type_qualifier;
	}
	;

type_qualifier:
	CONST
	{
	    debug("Qualifier: const\n");
	    $$ = strdup("const");
	}
	| VOLATILE
	{
	    debug("Qualifier: volatile\n");
	    $$ = strdup("volatile");
	}
	;

base_type:
	STRING
	{
	    debug("Base type: %s\n", $STRING);
	    $$ = new_base($STRING);
	}
	;

reference_file:
	'@' STRING
	{
	    $$ = new_reffile();
	    $$->base_type = $STRING;
	    }
	;

%%

extern void usage(void);

struct {
	bool debug;
	bool hide_kabi;
	bool print;
	bool compare;
	FILE *file1;
	FILE *file2;
} parse_config = {false, false, false, false, NULL, NULL};

obj_t *_parse(FILE *file) {
	obj_t *root = NULL;

	yyin = file;
	yyparse(&root);
	if (!root)
		fail("No object build\n");

	if (parse_config.hide_kabi)
		hide_kabi(root);
	if (parse_config.debug)
		debug_tree(root);
	if (parse_config.print)
		print_tree(root);

	return root;
}

void parse_usage() {
	printf("Usage:\n"
	       "\tparse [options] kabi_file [kabi_file]\n"
	       "\nGeneral options:\n"
	       "    -p, --print:\tdisplay the symbol in a c-like format\n"
	       "    -c, --compare:\t"
	       "compare two symbols (need two kabi files args)\n"
	       "    -h, --hide-kabi:\thide some rh specific kabi trickery\n"
	       "    -d, --debug:\tprint the raw tree\n"
	       "    --no-offset:\tdon't display the offset of struct fields\n"
	       "\nCompare options:\n"
	       "    --no-replaced:\thide replaced symbols"
	       " (symbols that changed, but hasn't moved)\n"
	       "    --no-shifted:\thide shifted symbols"
	       " (symbol that hasn't changed, but whose offset changed)\n"
	       "    --no-inserted:\t"
	       "hide symbols inserted in the middle of a struct, union...\n"
	       "    --no-deleted:\t"
	       "hide symbols removed from the middle of a struct, union...\n"
	       "    --no-added:\t\t"
	       "hide symbols added at the end of a struct, union...\n"
	       "    --no-removed:\t"
	       "hide symbols removed from the end of a struct, union...\n");
	exit(1);
}

FILE *fopen_safe(char *filename) {
	FILE *file;

	file = fopen(filename, "r");
	if (!file)
		fail("Failed to open kABI file: %s\n", filename);

	return file;
}

#define DISPLAY_NO_OPT(name) \
	{"no-"#name, no_argument, &display_options.no_##name, 1}

int parse(int argc, char **argv) {
	obj_t *root, *root2;
	int opt, opt_index, ret = 0;
	struct option loptions[] = {
		{"compare", no_argument, 0, 'c'},
		{"debug", no_argument, 0, 'd'},
		{"hide-kabi", no_argument, 0, 'k'},
		{"print", no_argument, 0, 'p'},
		{"show", no_argument, 0, 'p'},
		{"help", no_argument, 0, '?'},
		DISPLAY_NO_OPT(offset),
		DISPLAY_NO_OPT(replaced),
		DISPLAY_NO_OPT(shifted),
		DISPLAY_NO_OPT(inserted),
		DISPLAY_NO_OPT(deleted),
		DISPLAY_NO_OPT(added),
		DISPLAY_NO_OPT(removed),
		{0, 0, 0, 0}
	};

#ifdef DEBUG
	yydebug = 1;
#else
	yydebug = 0;
#endif

	memset(&display_options, 0, sizeof(display_options));

	while ((opt = getopt_long(argc, argv, "cdhp",
				  loptions, &opt_index)) != -1) {
		switch (opt) {
		case 0:
			break;
		case 'c':
			parse_config.compare = true;
			break;
		case 'd':
			parse_config.debug = true;
			break;
		case 'h':
			parse_config.hide_kabi = true;
			break;
		case 'p':
			parse_config.print = true;
			break;
		default:
			parse_usage();
		}
	}

	if (optind >= argc) {
		parse_usage();
	}

	parse_config.file1 = fopen_safe(argv[optind]);
	optind++;

	if (parse_config.compare) {
		if (optind >= argc)
			parse_usage();
		parse_config.file2 = fopen_safe(argv[optind]);
		optind++;
	}

	if ( optind != argc) {
		parse_usage();
	}

	root = _parse(parse_config.file1);

	if (parse_config.compare) {
		int tmp;

		root2 = _parse(parse_config.file2);
		tmp = compare_tree(root, root2);
		if (tmp == COMP_NEED_PRINT)
			fail("compare_tree still need to print\n");
		if (tmp == COMP_DIFF)
			ret = EXIT_KABI_CHANGE;
		free_obj(root2);
		fclose(parse_config.file2);
	}
	free_obj(root);
	fclose(parse_config.file1);

	return ret;
}

int yyerror(obj_t **root, char *s)
{
	fprintf(stderr, "error: %s\n", s);
	return 0;
}
