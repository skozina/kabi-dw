/*
	Copyright(C) 2017, Red Hat, Inc.

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

#ifndef KABI_DW_COMAPRE_H_
#define KABI_DW_COMAPRE_H_

/* Return value when we detect a kABI change */
#define EXIT_KABI_CHANGE 2

int compare(int argc, char **argv);

#endif
