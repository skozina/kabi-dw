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

NUM		[0-9]
ALPHA		[a-zA-Z]
IDENT_FIRST	[a-zA-Z_]
IDENT		[a-zA-Z0-9_]
FILECHAR	[a-zA-Z0-9_/<>\-\.]
HEX		[a-fA-F0-9]

%{
#include "parser.h"
#include "parser.tab.h"
%}

%option nounput
%option noyywrap
%s SYMBOL
%x IN_STRING UNKNOWN_FIELD

%%
	static int last_sc = INITIAL;

<SYMBOL>{ /* Keywords used by symbol declaration */
"const"		{ return(CONST); }
"enum"		{ return(ENUM); }
"struct"	{ return(STRUCT); }
"typedef"	{ return(TYPEDEF); }
"union"		{ return(UNION); }
"volatile"	{ return(VOLATILE); }

"..."		{ return(ELLIPSIS); }
}


<INITIAL,UNKNOWN_FIELD>{ /* keywords used only in the header */
"Version:"	{ BEGIN(INITIAL); return(VERSION_KW); }
"CU:"		{ BEGIN(INITIAL); return(CU_KW); }
"File:"		{ BEGIN(INITIAL); return(FILE_KW); }
"Stack:"	{ BEGIN(INITIAL); return(STACK_KW); }
"Symbol:\n"	{ BEGIN(SYMBOL); return(SYMBOL_KW_NL); }
{IDENT}+":"	{ BEGIN(UNKNOWN_FIELD); }
}

<UNKNOWN_FIELD>{ /* Ignore unknown fields. */
[^\n:]*"\n"	{ ; }
 /* A line that doesn't start with {IDENT}+":", i.e. not a valid field */
{IDENT}*[^a-zA-Z0-9_\n:]+{IDENT}*[^\n]*"\n" { ; }
}

"->"	{ return(ARROW); }

{FILECHAR}+"."[chS]|"<built-in>"	{ yylval.str = strdup(yytext);
				  debug("Source file: %s\n", yylval.str);
				  return(SRCFILE);
				}

{IDENT_FIRST}{IDENT}*	{ yylval.str = strdup(yytext);
			  debug("Identifier: %s\n", yylval.str);
			  return(IDENTIFIER);
			}
"(NULL)"	{ yylval.str = NULL;
		  debug("Identifier: (NULL)\n");
		  return(IDENTIFIER);
		}

0[xX]{HEX}+	{ yylval.ul = strtoul(yytext, NULL, 16);
		  debug("Constant: 0x%lx\n", yylval.ul);
		  return(CONSTANT);
		}
{NUM}+		{ yylval.ul = strtoul(yytext, NULL, 10);
		  debug("Constant: %li\n", yylval.ul);
		  return(CONSTANT);
		}

[{}()\[\];:,.*@=-]	{ return(yytext[0]); }

"\n"		{ return(NEWLINE); }

[ \t\v\f]	{ ; }

"\""		{ last_sc = YY_START; BEGIN(IN_STRING); }
<IN_STRING>[^"]* { yylval.str = strdup(yytext);
		  debug("String: %s\n", yylval.str);
		  return(STRING);
		}
<IN_STRING>"\""	{ BEGIN(last_sc); }
.		{ printf("Unexpected entry \"%c\"\n", *yytext); }

 /* Get back to initial condition or we'll start the next file in SYMBOL */
<<EOF>>		{ BEGIN(INITIAL); yyterminate(); }

%%
