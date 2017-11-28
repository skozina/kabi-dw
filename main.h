/*
	Copyright(C) 2016, Red Hat, Inc., Stanislav Kozina

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

/* Path used for built-in types */
#define	BUILTIN_PATH		"<built-in>"
/* Path used for DW_AT_declaration, ie. those we don't have */
#define	DECLARATION_PATH	"<declarations>"

#define RH_KABI_HIDE		"__UNIQUE_ID_rh_kabi_hide"
#define RH_KABI_HIDE_LEN	24

#endif /* MAIN_H_ */
