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
#include <limits.h>

#include "utils.h"

#define abort(...)				\
{						\
	fprintf(stderr, __VA_ARGS__);		\
	YYABORT;				\
}

#define check_and_free_keyword(identifier, expected)			\
{									\
	if (strcmp(identifier, expected))				\
		abort("Wrong keyword: %s expected, %s received\n",	\
		      expected, identifier);				\
	free(identifier);						\
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
%token <ul> CONSTANT

%token NEWLINE
%token TYPEDEF
%token CONST VOLATILE
%token STRUCT UNION ENUM ELLIPSIS
%token STACK

%type <str> type_qualifier
%type <obj> typed_type base_type reference_file array_type
%type <obj> type ptr_type variable_var_list func_type elt enum_elt enum_type
%type <obj> union_type struct_type struct_elt
%type <obj> declaration_var declaration_typedef declaration
%type <obj> kabi_dw_file kabi_dw_file_subtype
%type <list> elt_list arg_list enum_list struct_list
%type <obj> assembly_file weak_file symbol
%type <ul> alignment

%parse-param {obj_t **root}

%%

kabi_dw_file:
	fmt_version kabi_dw_file_subtype
	{
		$$ =  *root = $kabi_dw_file_subtype;
	}
	;

fmt_version:
	IDENTIFIER ':' CONSTANT '.' CONSTANT NEWLINE
	{
		if (strcmp($IDENTIFIER, "Version"))
			abort("Wrong keyword (\"Version\" expected): \"%s\"\n",
			      $IDENTIFIER);
		if (($3 != FILEFMT_VERSION_MAJOR) |
		    ($5 > FILEFMT_VERSION_MINOR))
			abort("Unsupported file version: %lu.%lu\n", $3, $5);
		free($IDENTIFIER);
	}
	;

kabi_dw_file_subtype:
	assembly_file
	{
		$$ = *root = $assembly_file;
	}
        | weak_file
	{
		$$ = *root = $weak_file;
	}
	| cu_file source_file stack_list symbol
	{
	    $$ = *root = $symbol;
	    obj_fill_parent(*root);
	}
	;

assembly_file:
	IDENTIFIER IDENTIFIER NEWLINE
	{
		check_and_free_keyword($1, "assembly");
		$$ = obj_assembly_new($2);
	}
	;

weak_file:
        IDENTIFIER IDENTIFIER STACK IDENTIFIER NEWLINE
	{
		check_and_free_keyword($1, "weak");
		$$ = obj_weak_new($2);
		$$->link = $4;
	}
	;

cu_file:
	IDENTIFIER ':' STRING NEWLINE
	{
	    check_and_free_keyword($IDENTIFIER, "CU");
	    free($STRING);
	}
	;

source_file:
	IDENTIFIER ':' SRCFILE ':' CONSTANT NEWLINE
	{
	    check_and_free_keyword($IDENTIFIER, "File");
	    free($SRCFILE);
	}
	;

stack_list:
	/* empty */
	| stack_list stack_elt NEWLINE
	;

stack_elt:
	STACK STRING
	{
		free($STRING);
	}
	;

symbol:
	IDENTIFIER ':' NEWLINE declaration NEWLINE
	{
		check_and_free_keyword($IDENTIFIER, "Symbol");
		$$ = $declaration;
	}
	| IDENTIFIER ':' NEWLINE alignment declaration NEWLINE
	{
		check_and_free_keyword($IDENTIFIER, "Symbol");
		$$ = $declaration;
		$$->alignment = $alignment;
	}

alignment:
        IDENTIFIER CONSTANT NEWLINE
	{
		check_and_free_keyword($IDENTIFIER, "Alignment");
		$$ = $CONSTANT;
	}

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
	    $$ = obj_typedef_new_add($IDENTIFIER, $type);
	}
	;

declaration_var:
	IDENTIFIER IDENTIFIER type
	{
	    check_and_free_keyword($1, "var");
	    $$ = obj_var_new_add($2, $type);
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
	    $$ = obj_struct_new($IDENTIFIER);
	}
	| STRUCT IDENTIFIER '{' NEWLINE struct_list NEWLINE '}'
	{
	    $$ = obj_struct_new($IDENTIFIER);
	    $$->member_list = $struct_list;
	}
	;

struct_list:
	struct_elt
	{
	    $$ = obj_list_head_new($struct_elt);
	}
	| struct_list NEWLINE struct_elt
	{
	    obj_list_add($1, $struct_elt);
	    $$ = $1;
	}
	;

struct_elt:
	CONSTANT IDENTIFIER type
	{
	    $$ = obj_struct_member_new_add($IDENTIFIER, $type);
	    $$->offset = $CONSTANT;
	}
	/* with alignment */
	| CONSTANT CONSTANT IDENTIFIER type
	{
	    $$ = obj_struct_member_new_add($IDENTIFIER, $type);
	    $$->offset = $1;
            $$->alignment = $2;
	}
	| CONSTANT ':' CONSTANT '-' CONSTANT IDENTIFIER type
	{
	    if ($5 > UCHAR_MAX || $3 > $5)
		abort("Invalid offset: %lx:%lu:%lu\n", $1, $3, $5);
	    $$ = obj_struct_member_new_add($IDENTIFIER, $type);
	    $$->offset = $1;
	    $$->is_bitfield = 1;
	    $$->first_bit = $3;
	    $$->last_bit = $5;
	}
	/* with alignment */
	| CONSTANT ':' CONSTANT '-' CONSTANT CONSTANT IDENTIFIER type
	{
	    if ($5 > UCHAR_MAX || $3 > $5)
		abort("Invalid offset: %lx:%lu:%lu\n", $1, $3, $5);
	    $$ = obj_struct_member_new_add($IDENTIFIER, $type);
	    $$->offset = $1;
	    $$->is_bitfield = 1;
	    $$->first_bit = $3;
	    $$->last_bit = $5;
	    $$->alignment = $6;
	}
	;

union_type:
	UNION IDENTIFIER '{' NEWLINE '}'
	{
	    $$ = obj_union_new($IDENTIFIER);
	}
	| UNION IDENTIFIER '{' NEWLINE elt_list NEWLINE '}'
	{
	    $$ = obj_union_new($IDENTIFIER);
	    $$->member_list = $elt_list;
	    $elt_list->object = $$;
	}
	;

enum_type:
	ENUM IDENTIFIER '{' NEWLINE enum_list NEWLINE '}'
	{
	    $$ = obj_enum_new($IDENTIFIER);
	    $$->member_list = $enum_list;
	    $enum_list->object = $$;
	}
	;

enum_list:
	enum_elt
	{
	    $$ = obj_list_head_new($enum_elt);
	}
	| enum_list NEWLINE enum_elt
	{
	    obj_list_add($1, $enum_elt);
	    $$ = $1;
	}
	;

enum_elt:
	IDENTIFIER '=' CONSTANT
	{
	    $$ = obj_constant_new($IDENTIFIER);
	    $$->constant = $CONSTANT;
	}
	;

func_type:
	IDENTIFIER IDENTIFIER '(' NEWLINE arg_list ')' NEWLINE type
	{
	    check_and_free_keyword($1, "func");
	    $$ = obj_func_new_add($2, $type);
	    $$->member_list = $arg_list;
	    if ($arg_list)
		    $arg_list->object = $$;
	}
	| IDENTIFIER reference_file /* protype define as typedef */
	{
	    check_and_free_keyword($IDENTIFIER, "func");
	    $$ = obj_func_new_add(NULL, $reference_file);
	}
	;

arg_list:
	/* empty */
	{
	    $$ = NULL;
	}
	| elt_list NEWLINE
	{
	    $$ = $elt_list;
	}
	| elt_list NEWLINE variable_var_list NEWLINE
	{
	    obj_list_add($elt_list, $variable_var_list);
	    $$ = $elt_list;
	}
	;

variable_var_list:
	IDENTIFIER ELLIPSIS
	{
	    /* TODO: there may be a better solution */
	    $$ = obj_var_new_add(NULL, obj_basetype_new(strdup("...")));
	}
	;

elt_list:
	elt
	{
	    $$ = obj_list_head_new($elt);
	}
	| elt_list NEWLINE elt
	{
	    obj_list_add($1, $elt);
	    $$ = $1;
	}
	;

elt:
	IDENTIFIER type
	{
	    $$ = obj_var_new_add($IDENTIFIER, $type);
	}
	;

ptr_type:
	'*' type
	{
	    $$ = obj_ptr_new_add($type);
	}
	;

array_type:
        '['CONSTANT ']' type
	{
	    $$ = obj_array_new_add($type);
	    $$->index = $CONSTANT;
	}
	;

typed_type:
	type_qualifier type
	{
	    $$ = obj_qualifier_new_add($type);
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
	    $$ = obj_basetype_new($STRING);
	}
	;

reference_file:
	'@' STRING
	{
	    $$ = obj_reffile_new();
	    $$->base_type = $STRING;
	    }
	;

%%

extern void usage(void);

obj_t *obj_parse(FILE *file, char *fn) {
	obj_t *root = NULL;

#ifdef DEBUG
	yydebug = 1;
#else
	yydebug = 0;
#endif

	yyin = file;
	yyparse(&root);
	if (!root)
		fail("No object build for file %s\n", fn);

	return root;
}

int yyerror(obj_t **root, char *s)
{
	fprintf(stderr, "error: %s\n", s);
	return 0;
}
