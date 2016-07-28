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
#include "parser.h"

extern obj_t *root;

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

%type <str> type_qualifier
%type <obj> typed_type base_type reference_file array_type
%type <obj> type ptr_type variable_var_list func_type elt enum_elt enum_type
%type <obj> union_type struct_type struct_elt
%type <obj> declaration_var declaration_typedef declaration kabi_dw_file
%type <list> elt_list arg_list enum_list struct_list

%%

kabi_dw_file:
	cu_file NEWLINE source_file NEWLINE declaration NEWLINE
	{
	    $$ = root = $declaration;
	}
	;

cu_file:
	IDENTIFIER STRING
	{
	    if (strcmp($IDENTIFIER,"CU"))
		abort("Wrong CU keyword: \"%s\"\n", $IDENTIFIER);
	    free($IDENTIFIER);
	    free($STRING);
	}
	;

source_file:
	IDENTIFIER SRCFILE ':' CONSTANT
	{
	    if (strcmp($IDENTIFIER,"File"))
		abort("Wrong file keyword: \"%s\"\n", $IDENTIFIER);
	    free($IDENTIFIER);
	    free($SRCFILE);
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
	    $$ = new_typedef($IDENTIFIER);
	    $$->ptr = $type;
	}
	;

declaration_var:
	IDENTIFIER IDENTIFIER type
	{
	    if (strcmp($1,"var"))
		abort("Wrong var keyword: \"%s\"\n", $1);
	    free($1);
	    $$ = new_var($2);
	    $$->ptr = $type;
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
	struct_offset IDENTIFIER type
	{
	    $$ = new_var($IDENTIFIER);
	    $$->ptr = $type;
	}
	;

	/* TODO need to add offsets */
struct_offset:
	CONSTANT
	| CONSTANT ':' CONSTANT '-' CONSTANT
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
	}
	;

enum_type:
	ENUM IDENTIFIER '{' NEWLINE enum_list NEWLINE '}'
	{
	    $$ = new_enum($IDENTIFIER);
	    $$->member_list = $enum_list;
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
	    $$ = new_enum($IDENTIFIER);
	    $$->constant = $CONSTANT;
	}
	;

func_type:
	IDENTIFIER IDENTIFIER '(' NEWLINE arg_list ')' NEWLINE type
	{
	    if (strcmp($1,"func"))
		abort("Wrong func keyword: \"%s\"\n", $1);
	    free($1);
	    $$ = new_func($2);
	    $$->member_list = $arg_list;
	    $$->ptr = $type;
	}
	| IDENTIFIER reference_file /* protype define as typedef */
	{
	    if (strcmp($IDENTIFIER,"func"))
		abort("Wrong func keyword: \"%s\"\n", $IDENTIFIER);
	    free($IDENTIFIER);
	    /* TODO: Need to parse other file */
	    $$ = new_func(NULL);
	    $$->ptr = $reference_file;
	}
	;

arg_list:
	%empty
	{
	    /* TODO: that's ugly. Is it correct? */
	    $$ = NULL;
	}
	| elt_list NEWLINE
	{
	    $$ = $elt_list;
	}
	| variable_var_list NEWLINE
	{
	    $$ = new_list_head($variable_var_list);
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
	    $$ = new_base(strdup("..."));
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
	    $$ = new_var($IDENTIFIER);
	    $$->ptr  = $type;
	}
	;

ptr_type:
	'*' type
	{
	    $$ = new_ptr();
	    $$->ptr = $type;
	}
	;

array_type:
	'[' CONSTANT ']' STRING
	{
	    $$ = new_array();
	    $$->index = $CONSTANT;
	    $$->base_type = $STRING;
	}
	| '[' CONSTANT ']' array_type
	{
	    $$ = new_array();
	    $$->index = $CONSTANT;
	    $$->ptr = $4;
	}
	;

typed_type:
	type_qualifier type
	{
	    /*
	     * TODO: need a new object type?
	     * Can we just add the qualifier to base_type?
	     */
	    $$ = $type;
	}
	;

type_qualifier:
	CONST
	{
	    debug("Qualifier: const\n");
	    $$ = "const";
	}
	| VOLATILE
	{
	    debug("Qualifier: volatile\n");
	    $$ = "volatile";
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
	    /* TODO: need to parse that file */
	    $$ = new_none();
	    $$->base_type = $STRING;
	    }
	;

%%

extern void usage(void);

int parse(int argc, char **argv)
{
	int ret;
	char *filename;
	FILE *kabi_file;

#ifdef DEBUG
	yydebug = 1;
#else
	yydebug = 0;
#endif

	if (argc != 1) {
		usage();
	}
	filename = argv[0];
	kabi_file = fopen(filename, "r");
	if (kabi_file == NULL) {
		fprintf(stderr, "Failed to open kABI file: %s\n",
			filename);
		return 1;
	}

	yyin = kabi_file;
	ret = yyparse();
	if (ret)
	    return ret;

	walk_graph();
	free_obj(root);

	return 0;
}

int yyerror(char *s)
{
	fprintf(stderr, "error: %s\n", s);
	return 0;
}
