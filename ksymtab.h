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

#ifndef KSYMTAB_H_
#define	KSYMTAB_H_

struct ksymtab;
struct ksym;

extern void ksymtab_free(struct ksymtab *);
extern struct ksymtab *ksymtab_read(char *);
extern int ksymtab_find(struct ksymtab *, const char *);
extern size_t ksymtab_len(struct ksymtab *);
extern struct ksymtab *ksymtab_new(size_t);
extern void ksymtab_add_sym(struct ksymtab *, const char *, size_t, size_t);
extern void ksymtab_for_each_unmarked(struct ksymtab *,
				      void (*f)(const char *, size_t, void *),
				      void *);
extern size_t ksymtab_mark_count(struct ksymtab *);
extern void ksymtab_ksym_mark(struct ksym *);

#endif /* KSYMTAB_H_ */
