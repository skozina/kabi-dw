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
INDENT_FIRST	[a-zA-Z_]
INDENT		[a-zA-Z0-9_]
FILECHAR	[a-zA-Z0-9_/<>\-\.]
HEX		[a-fA-F0-9]

%{
#include "parser.h"
#include "parser.tab.h"
%}

%option nounput
%option noyywrap
%x IN_STRING

%%

"const"		{ return(CONST); }
"enum"		{ return(ENUM); }
"struct"	{ return(STRUCT); }
"typedef"	{ return(TYPEDEF); }
"union"		{ return(UNION); }
"volatile"	{ return(VOLATILE); }

"..."		{ return(ELLIPSIS); }

"->"		{ return(STACK); }

{FILECHAR}+"."[ch]|"<built-in>"	{ yylval.str = strdup(yytext);
				  debug("Source file: %s\n", yylval.str);
				  return(SRCFILE);
				}

{INDENT_FIRST}{INDENT}*	{ yylval.str = strdup(yytext);
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

[{}()\[\];:,*@=-]	{ return(yytext[0]); }

"\n"		{ return(NEWLINE); }

[ \t\v\f]	{ ; }
"\""		{ BEGIN(IN_STRING); }
<IN_STRING>[^"]*	{ yylval.str = strdup(yytext);
		  debug("String: %s\n", yylval.str);
		  return(STRING);
		}
<IN_STRING>"\""	{ BEGIN(INITIAL); }
.		{ printf("Unexpected entry \"%c\"\n", *yytext); }

%%
