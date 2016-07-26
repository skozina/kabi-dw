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

char *cat_base_type(char *s1, const char *s2) {
	size_t sz1 = strlen(s1), sz2 = strlen(s2);

	if (sz1 + sz2 + 2 >= MAX_BASE_TYPE_LEN) {
		fprintf(stderr, "error string too long\n");
		return s1;
	}
	memmove(s1+sz2+1, s1, sz1);
	strcpy(s1, s2);
	s1[sz2] = ' ';

	return s1;
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

%token <str> IDENTIFIER SRCFILE REFFILE
%token <l> CONSTANT

%token NEWLINE
%token TYPEDEF
%token CHAR SHORT INT LONG SIGNED UNSIGNED VOID BOOL FLOAT DOUBLE
%token CONST VOLATILE
%token STRUCT UNION ENUM ELLIPSIS

%type <str> type_qualifier sign_qualifier length_qualifier
%type <obj> typed_type lengthed_type type_specifier base_type array_type
%type <obj> type ptr_type variable_var_list func_type elt enum_elt enum_type
%type <obj> union_type struct_type struct_elt file_reference
%type <obj> declaration_var declaration_typedef declaration kabi_dw_file
%type <list> elt_list arg_list enum_list struct_list

%%

kabi_dw_file:
	cu_file NEWLINE source_file NEWLINE declaration NEWLINE
	{
	    $$ = root = $5;
	}
	;

cu_file:
	IDENTIFIER SRCFILE
	{
	    if (strcmp($1,"CU"))
		fprintf(stderr, "Wrong CU keyword: \"%s\"\n", $1);
	}
	;

source_file:
	IDENTIFIER SRCFILE ':' CONSTANT
	{
	    if (strcmp($1,"File"))
		fprintf(stderr, "Wrong file keyword: \"%s\"\n", $1);
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
	    $$ = new_typedef($2);
	    $$->ptr = $4;
	}
	;

declaration_var:
	IDENTIFIER IDENTIFIER type
	{
	    if (strcmp($1,"var"))
		fprintf(stderr, "Wrong var keyword: \"%s\"\n", $1);
	    $3->name = $2;
	    $$ = $3;
	}
	;

type:
	base_type
	| struct_type
	| union_type
	| enum_type
	| func_type
	| file_reference
	| ptr_type
	| array_type
	| typed_type
	;

file_reference:
	'@' REFFILE
	{
	    /* TODO: need to parse that file */
	    $$ = new_none();
	    $$->base_type = $2;
	}
	;

struct_type:
	STRUCT IDENTIFIER '{' NEWLINE '}'
	{
	    $$ = new_struct($2);
	}
	| STRUCT IDENTIFIER '{' NEWLINE struct_list NEWLINE '}'
	{
	    $$ = new_struct($2);
	    $$->member_list = $5;
	}
	;

struct_list:
	struct_elt
	{
	    $$ = new_list_head($1);
	}
	| struct_list NEWLINE struct_elt
	{
	    list_add($1, $3);
	    $$ = $1;
	}
	;

struct_elt:
	struct_offset IDENTIFIER type
	{
	    if ($3->name)
		fprintf(stderr, "New struct_elt: name is already set \"%s\"\n",
			$3->name);
	    $3->name = $2;
	    $$ = $3;
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
	    $$ = new_union($2);
	}
	| UNION IDENTIFIER '{' NEWLINE elt_list NEWLINE '}'
	{
	    $$ = new_union($2);
	    $$->member_list = $5;
	}
	;

enum_type:
	ENUM IDENTIFIER '{' NEWLINE enum_list NEWLINE '}'
	{
	    $$ = new_enum($2);
	    $$->member_list = $5;
	}
	;

enum_list:
	enum_elt
	{
	    $$ = new_list_head($1);
	}
	| enum_list NEWLINE enum_elt
	{
	    list_add($1, $3);
	    $$ = $1;
	}
	;

enum_elt:
	IDENTIFIER '=' CONSTANT
	{
	    $$ = new_enum($1);
	    $$->constant = $3;
	}
	;

func_type:
	IDENTIFIER IDENTIFIER '(' NEWLINE arg_list ')' NEWLINE type
	{
	    if (strcmp($1,"func"))
		fprintf(stderr, "Wrong func keyword: \"%s\"\n", $1);
	    $$ = new_func($2);
	    $$->member_list = $5;
	    $$->ptr = $8;
	}
	| IDENTIFIER file_reference /* protype define as typedef */
	{
	    if (strcmp($1,"func"))
		fprintf(stderr, "Wrong func keyword: \"%s\"\n", $1);
	    /* TODO: Need to parse other file */
	    $$ = new_func($1);
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
	    $$ = $1;
	}
	| variable_var_list NEWLINE
	{
	    $$ = new_list_head($1);
	}
	| elt_list NEWLINE variable_var_list NEWLINE
	{
	    list_add($1, $3);
	    $$ = $1;
	}
	;

variable_var_list:
	IDENTIFIER ELLIPSIS
	{
	    /* TODO: there may be a better solution */
	    $$ = new_base("...");
	}
	;

elt_list:
	elt
	{
	    $$ = new_list_head($1);
	}
	| elt_list NEWLINE elt
	{
	    list_add($1, $3);
	    $$ = $1;
	}
	;

elt:
	IDENTIFIER type
	{
	    if ($2->name)
		fprintf(stderr, "New elt: name is already set \"%s\"\n",
			$2->name);
	    $2->name = $1;
	    $$ = $2;
	}
	;

ptr_type:
	'*' type
	{
	    $$ = new_ptr();
	    $$->ptr = $2;
	}
	;

/* TODO: need to deal with this sizetype nonsense */
/* WARN: so it can be void too?! */
array_type:
	'[' CONSTANT ']' IDENTIFIER
	{
	    if (strcmp($4,"sizetype"))
		fprintf(stderr, "Array type: \"%s\"\n", $4);
	    $$ = new_array();
	    $$->index = $2;
	    $$->base_type = "sizetype";
	}
	| '[' CONSTANT ']' VOID
	{
	    $$ = new_array();
	    $$->index = $2;
	    $$->base_type = "void";
	}
	| '[' CONSTANT ']' array_type
	{
	    $$ = new_array();
	    $$->index = $2;
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
	    $$ = $2;
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

	/*
	 * Base type specifier seem to be void, char, int, _Bool, float, double
	 * short and long are used only as length qualifier
	 * The order seem to be length sign specifier
	 */
base_type:
	lengthed_type
	| length_qualifier lengthed_type
	{
	    cat_base_type($2->base_type, $1);
	    $$ = $2;
	}
	;

length_qualifier:
	SHORT
	{
	    debug("Length: short\n");
	    $$ = "short";
	}
	| LONG
	{
	    debug("Length: long\n");
	    $$ = "long";
	}
	| LONG LONG
	{
	    debug("Length: long long\n");
	    $$ = "long long";
	}
	;

lengthed_type:
	type_specifier
	| sign_qualifier type_specifier
	{
	    cat_base_type($2->base_type, $1);
	    $$ = $2;
	}
	;


sign_qualifier:
	SIGNED
	{
	    debug("Sign: signed\n");
	    $$ = "signed";
	}
	| UNSIGNED
	{
	    debug("Sign: unsigned\n");
	    $$ = "unsigned";
	}
	;

type_specifier:
        VOID
	{
	    debug("Type: void\n");
	    $$ = new_base("void");
	}
	| CHAR
	{
	    debug("Type: char\n");
	    $$ = new_base("char");
	}
	| INT
	{
	    debug("Type: int\n");
	    $$ = new_base("int");
	}
	| BOOL
	{
	    debug("Type: _Bool\n");
	    $$ = new_base("_Bool");
	}
	| FLOAT
	{
	    debug("Type: float\n");
	    $$ = new_base("float");
	}
	| DOUBLE
	{
	    debug("Type: double\n");
	    $$ = new_base("double");
	}
	;

%%

extern void usage(void);

int parse(int argc, char **argv)
{
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
	yyparse();

	walk_graph();

	return 0;
}

int yyerror(char *s)
{
	fprintf(stderr, "error: %s\n", s);
	return 0;
}
