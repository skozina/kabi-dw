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

#ifndef	_GNU_SOURCE /* We use GNU basename() that doesn't modify the arg */
#error "We need GNU version of basename()!"
#endif

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <libgen.h>

#include "main.h"
#include "objects.h"
#include "utils.h"
#include "compare.h"

/* diff -u style prefix for tree comparison */
#define ADD_PREFIX "+"
#define DEL_PREFIX "-"

/* Return values for the (_)compare_tree functions */
enum {
	COMP_SAME = 0,	/* Subtree are equal */
	COMP_DIFF,	/* Subtree differs, stop the scanning */
	COMP_CONT,	/* Only offset or alignment change, continue */
};

int comp_return_value(int old, int new) {
	switch (new) {
	case COMP_DIFF:
		return COMP_DIFF;
	case COMP_CONT:
		if (old != COMP_DIFF)
			return COMP_CONT;
	case COMP_SAME:
		;
	}
	return old;
}

/*
 * Is this symbol a duplicate, i.e. is not the first version of this symbol.
 */
static bool is_duplicate(char *filename)
{
	char *base = basename(filename);
	char *prefix = NULL, *name = NULL;
	int version = 0;
	bool ret = (sscanf(base, "%m[a-z]--%m[^.-]-%i.txt",
			   &prefix, &name, &version) == 3);

	free(prefix);
	free(name);

	return ret;
}

static void _print_node_list(const char *s, const char *prefix,
			     obj_list_t *list, obj_list_t *last, FILE *stream) {
	obj_list_t *l = list;

	fprintf(stream, "%s:\n", s);
	while (l && l != last) {
		obj_print_tree__prefix(l->member, prefix, stream);
		l = l->next;
	}
}

static void print_node_list(const char *s, const char *prefix,
			    obj_list_t *list, FILE *stream) {
	_print_node_list(s, prefix, list, NULL, stream);
}


/*
 * There is some ambiguity here that need to be cleared and a
 * hierarchy that need to be explicitly established. The current
 * situation is: if there is a real change to the object
 * (different name, type...) we return CMP_DIFF; If that's not
 * the case, but a referred symbol has changed, we return
 * CMP_REFFILE; If that's not the case, but the offset has
 * changed, we return CMP_OFFSET. So the current order is
 * CMP_DIFF > CMP_REFFILE > CMP_OFFSET > CMP_ALIGNMENT > CMP_BYTE_SIZE
 * In case of alignment, if the structure alignment has changed,
 * only that is reported. If not, then the fields are checked and
 * the all the different fields are reported.
 * The same is true of byte size changes.
 */

typedef enum {
	CMP_SAME = 0,
	CMP_OFFSET,	/* Only the offset has changed */
	CMP_DIFF,	/* Nodes are differents */
	CMP_REFFILE,	/* A refered symbol has changed */
	CMP_ALIGNMENT,  /* An alignment has changed */
	CMP_BYTE_SIZE,  /* Byte size has changed */
} cmp_ret_t;

static int compare_two_files(const char *filename, const char *newfile,
			     bool follow);

static int cmp_node_reffile(obj_t *o1, obj_t *o2)
{
	char *s1 = filenametotype(o1->base_type);
	char *s2 = filenametotype(o2->base_type);
	int len;
	bool ret;

	ret = safe_streq(s1, s2);
	free(s1);
	free(s2);

	if (!ret)
		return CMP_DIFF;

	/*
	 * Compare the symbol referenced by file, but be careful not
	 * to follow imaginary declaration path.
	 *
	 * TODO: This is quite wasteful. We reopen files and parse
	 * them again many times.
	 */
	len = strlen(DECLARATION_PATH);
	if (strncmp(o1->base_type, DECLARATION_PATH, len) &&
	    strncmp(o2->base_type, DECLARATION_PATH, len) &&
	    compare_two_files(o1->base_type, o2->base_type, true))
		return CMP_REFFILE;

	return CMP_SAME;
}

static int _cmp_nodes(obj_t *o1, obj_t *o2, bool search)
{
	if ((o1->type != o2->type) ||
	    !safe_streq(o1->name, o2->name) ||
	    (is_weak(o1) != is_weak(o2)) ||
	    (is_weak(o1) && is_weak(o2) && !safe_streq(o1->link, o2->link)) ||
	    ((o1->ptr == NULL) != (o2->ptr == NULL)) ||
	    (has_constant(o1) && (o1->constant != o2->constant)) ||
	    (has_index(o1) && (o1->index != o2->index)) ||
	    (is_bitfield(o1) != is_bitfield(o2)) ||
	    (is_bitfield(o1) && ((o1->last_bit - o1->first_bit) !=
				 (o2->last_bit - o2->first_bit))))
		return CMP_DIFF;

	if (o1->type == __type_reffile) {
		int ret;

		ret = cmp_node_reffile(o1, o2);
		if (ret)
			return ret;
	} else if (!safe_streq(o1->base_type, o2->base_type))
		return CMP_DIFF;

	if (has_offset(o1) &&
	    ((o1->offset != o2->offset) ||
	     (is_bitfield(o1) && (o1->first_bit != o2->first_bit)))) {
		if (search && o1->name == NULL)
			/*
			 * This field is an unnamed struct or union. When
			 * searching for a node, avoid to consider the next
			 * unnamed struct or union to be the same one.
			 */
			return CMP_DIFF;
		return CMP_OFFSET;
	}

	if (o1->alignment != o2->alignment)
		return CMP_ALIGNMENT;

	if (o1->byte_size != o2->byte_size)
		return CMP_BYTE_SIZE;

	return CMP_SAME;
}

static int cmp_nodes(obj_t *o1, obj_t *o2)
{
	return _cmp_nodes(o1, o2, false);
}

typedef enum {
	DIFF_INSERT,
	DIFF_DELETE,
	DIFF_REPLACE,
	DIFF_CONT,
} diff_ret_t;

/*
 * When field are changed or moved around, there can be several diff
 * representations for the change.  We are trying to keep the diff as
 * small as possible, while keeping most significant changes in term
 * of kABI (mainly shifted fields, which most likely indicate that
 * some change to the ABI have been overlooked).
 *
 * This function compare two lists whose first member diverge. We're
 * looking at four different scenarios:
 * - N fields appears only in list2, then the lists rejoined (insertion)
 * - P fields appears only in list1, then the lists rejoined (deletion)
 * - Q fields diverges, then the lists rejoined (replacement)
 * - the lists never rejoined
 *
 * Since for the same change, several of the scenarios above might
 * represent the change, we choose the one that minimize the diff
 * (min(N,P,Q)). So we're looking for the first element of list1 in
 * list2, the first element of list2 in list1 or the first line where
 * list1 and list2 do not differ, whichever comes first.
 */
static diff_ret_t list_diff(obj_list_t *list1, obj_list_t **next1,
			    obj_list_t *list2, obj_list_t **next2)
{
	obj_t *o1 = list2->member, *o2 = list1->member, *o = o1;
	int d1 = 0, d2 = 0, ret;
	obj_list_t *next;

	next = *next1 = list1;
	*next2 = list2;

	while (next) {
		ret = _cmp_nodes(o, next->member, true);
		if (ret == CMP_SAME || ret == CMP_OFFSET
		    || ret == CMP_ALIGNMENT) {
			if (o == o1)
				/* We find the first element of list2
				   on list1, that is d1 elements have
				   been removed from list1 */
				return DIFF_DELETE;
			else
				return DIFF_INSERT;
		}

		if (d1 == d2)  {
			ret = _cmp_nodes((*next1)->member, (*next2)->member,
					 true);
			if (ret == CMP_SAME || ret == CMP_OFFSET
			    || ret == CMP_ALIGNMENT) {
				/* d1 fields have been replaced */
				return DIFF_REPLACE;
			}

		}

		if (!(*next1) || !((*next1)->next) || (d2  < d1)) {
			next = *next2 = (*next2)->next;
			o = o2;
			d2++;
		} else {
			next = *next1 = (*next1)->next;
			o = o1;
			d1++;
		}
	}
	return DIFF_CONT;
}

/*
 * We want to show practical output to the user.  For instance if a
 * struct member type change, we want to show which struct member
 * changed type, not that somewhere a "signed int" has been changed
 * into a "unsigned bin".
 *
 * For now, we consider that a useful output should start at a named
 * object or at a struct field or var (the field/var itself may be
 * unamed, typically when it's an union or struct of alternative
 * elements but it most likely contains named element).
 */
static bool worthy_of_print(obj_t *o)
{
	return (o->name != NULL) ||
		(o->type == __type_struct_member) ||
		(o->type == __type_var);
}

static void print_two_nodes(const char *s, obj_t *o1, obj_t *o2, FILE *stream)
{

	while (!worthy_of_print(o1)) {
		o1 = o1->parent;
		o2 = o2->parent;
		if ((o1 == NULL) || (o2 == NULL))
			fail("No ancestor worthy of print\n");
	}
	fprintf(stream, "%s:\n", s);
	obj_print_tree__prefix(o1, DEL_PREFIX, stream);
	obj_print_tree__prefix(o2, ADD_PREFIX, stream);
}

typedef struct compare_config_s {
	bool debug;
	bool hide_kabi;
	bool hide_kabi_new;
	bool skip_duplicate; /* Don't show multiple version of a symbol */
	int follow;
	char *old_dir;
	char *new_dir;
	char *filename;
	char **flist;
	int flistsz;
	int flistcnt;
	int ret;
	/*
	 * The following options allow to hide some symbol changes in
	 * kABI comparison. Hides...
	 */
	int no_replaced; /* replaced symbols */
	int no_shifted;  /* symbols whose offset shifted */
	int no_inserted; /* symbols inserted in the middle of a struct/union */
	int no_deleted;  /* symbols removed in the middle (poke a hole) */
	int no_added;    /* symbols added at the end of a struct/union... */
	int no_removed;  /* symbols removed at the end of a struct/union... */
	int no_moved_files; /* file that has been moved (or removed) */
} compare_config_t;

compare_config_t compare_config = {false, false, false, false, 0,
				   NULL, NULL, NULL, NULL,
				   0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static void message_alignment_value(unsigned v, FILE *stream)
{
	if (v == 0)
		fprintf(stream, "<undefined>");
	else
		fprintf(stream, "%u", v);
}

static void message_byte_size_value(unsigned int v, FILE *stream)
{
	if (v == 0)
		fprintf(stream, "<undefined>");
	else
		fprintf(stream, "%u", v);
}

static void message_alignment(obj_t *o1, obj_t *o2, FILE *stream)
{
	char *part_str;

	if (o1->type == __type_struct_member) {
		part_str = "field";
	} else {
		part_str = "symbol";
	}

	fprintf(stream, "The alignment of %s '%s' has changed from ",
		part_str, o1->name);

	message_alignment_value(o1->alignment, stream);
	fprintf(stream, " to ");
	message_alignment_value(o2->alignment, stream);
	fprintf(stream, "\n");
}

static void message_byte_size(obj_t *o1, obj_t *o2, FILE *stream)
{
	fprintf(stream, "The byte size of symbol '%s' has changed from ",
		o1->name);

	message_byte_size_value(o1->byte_size, stream);
	fprintf(stream, " to ");
	message_byte_size_value(o2->byte_size, stream);
	fprintf(stream, "\n");
}

static int _compare_tree(obj_t *o1, obj_t *o2, FILE *stream)
{
	obj_list_t *list1 = NULL, *list2 = NULL;
	int ret = COMP_SAME, tmp;

	tmp = cmp_nodes(o1, o2);
	if (tmp) {
		if (tmp == CMP_REFFILE) {
			fprintf(stream, "symbol %s has changed\n",
				o1->base_type);
			ret = COMP_DIFF;
		} else if ((tmp == CMP_OFFSET && !compare_config.no_shifted) ||
			   (tmp == CMP_DIFF && !compare_config.no_replaced)) {
			const char *s =	(tmp == CMP_OFFSET) ?
				"Shifted" : "Replaced";
			print_two_nodes(s, o1, o2, stream);
			ret = COMP_CONT;
		} else if (tmp == CMP_ALIGNMENT) {
			message_alignment(o1, o2, stream);
			ret = COMP_CONT;
		} else if (tmp == CMP_BYTE_SIZE) {
			message_byte_size(o1, o2, stream);
			ret = COMP_CONT;
		}

		if (ret == COMP_DIFF)
			return ret;
	}

	if (o1->member_list)
		list1 = o1->member_list->first;
	if (o2->member_list)
		list2 = o2->member_list->first;

	while (list1 && list2) {
		if (cmp_nodes(list1->member, list2->member) == CMP_DIFF) {
			int index;
			obj_list_t *next1, *next2;

			index = list_diff(list1, &next1, list2, &next2);

			switch (index) {
			case DIFF_INSERT:
				/* Insertion */
				if (!compare_config.no_inserted) {
					_print_node_list("Inserted", ADD_PREFIX,
							 list2, next2, stream);
					ret = COMP_DIFF;
				}
				list2 = next2;
				break;
			case DIFF_DELETE:
				/* Removal */
				if (!compare_config.no_deleted) {
					_print_node_list("Deleted", DEL_PREFIX,
							 list1, next1, stream);
					ret = COMP_DIFF;
				}
				list1 = next1;
				break;
			case DIFF_REPLACE:
				/*
				 * We could print the diff here, but relying on
				 * the next calls to _compare_tree() to display
				 * the replaced fields individually works too.
				 */
			case DIFF_CONT:
				/* Nothing to do */
				;
			}
		}

		tmp =_compare_tree(list1->member, list2->member, stream);
		ret = comp_return_value(ret, tmp);

		list1 = list1->next;
		list2 = list2->next;
		if (!list1 && list2) {
			if (!compare_config.no_added) {
				print_node_list("Added", ADD_PREFIX,
						list2, stream);
				ret = COMP_DIFF;
			}
			return ret;
		}
		if (list1 && !list2) {
			if (!compare_config.no_removed) {
				print_node_list("Removed", DEL_PREFIX,
						list1, stream);
				ret = COMP_DIFF;
			}
			return ret;
		}
	}

	if (o1->ptr && o2->ptr) {
		tmp = _compare_tree(o1->ptr, o2->ptr, stream);
		ret = comp_return_value(ret, tmp);
	}

	return ret;
}

/*
 * Compare two symbols and show the difference in a c-like format
 */
static int compare_tree(obj_t *o1, obj_t *o2, FILE *stream)
{
	return _compare_tree(o1, o2, stream);
}

static bool push_file(const char *filename)
{
	int i, sz = compare_config.flistsz;
	int cnt = compare_config.flistcnt;
	char **flist = compare_config.flist;

	for (i = 0; i < cnt; i++)
		if (!strcmp(flist[i], filename))
			return false;

	if (!sz) {
		compare_config.flistsz = sz = 16;
		compare_config.flist = flist =
			safe_zmalloc(16 * sizeof(char *));
	}
	if (cnt >= sz) {
		sz *= 2;
		compare_config.flistsz = sz;
		compare_config.flist = flist =
			safe_realloc(flist, sz * sizeof(char *));
	}

	flist[cnt] = strdup(filename);
	compare_config.flistcnt++;

	return true;
}

static void free_files()
{
	int i;

	for (i = 0; i < compare_config.flistcnt; i++)
		free(compare_config.flist[i]);
	free(compare_config.flist);
	compare_config.flistcnt = compare_config.flistsz = 0;
}

static void compare_usage()
{
	printf("Usage:\n"
	       "\tcompare [options] kabi_dir kabi_dir [kabi_file...]\n"
	       "\tcompare [options] kabi_file kabi_file\n"
	       "\nOptions:\n"
	       "    -h, --help:\t\tshow this message\n"
	       "    -k, --hide-kabi:\thide changes made by RH_KABI_REPLACE()\n"
	       "    -n, --hide-kabi-new:\n\t\t\thide the kabi trickery made by"
	       " RH_KABI_REPLACE, but show the new field\n"
	       "    -d, --debug:\tprint the raw tree\n"
	       "    --follow:\t\tfollow referenced symbols\n"
	       "    --no-offset:\tdon't display the offset of struct fields\n"
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
	       "hide symbols removed from the end of a struct, union...\n"
	       "    --no-moved-files:\thide changes caused by symbols "
	       "definition moving to another file\n\t\t\t"
	       "Warning: it also hides symbols that are removed entirely\n"
	       "    -s, --skip-duplicate:\tshow only the first version of a "
	       "symbol when several exist\n");

	exit(1);
}

/*
 * Parse two files and compare the resulting tree.
 *
 * filename: file to compare (relative to compare_config.*_dir)
 * newfile:  if not NULL, the file to use in compare_config.new_dir,
 *           otherwise, filename is used for both.
 * follow:   Are we here because we followed a reference file? If so,
 *           don't print anything and exit immediately if follow
 *           option isn't set.
 */
static int compare_two_files(const char *filename, const char *newfile,
			     bool follow)
{
	obj_t *root1, *root2;
	char *old_dir = compare_config.old_dir;
	char *new_dir = compare_config.new_dir;
	char *path1, *path2, *s = NULL;
	const char *filename2;
	FILE *file1, *file2, *stream;
	struct stat fstat;
	size_t sz;
	int ret = 0, tmp;

	if (follow && !compare_config.follow)
		return 0;

	/* Avoid infinite loop */
	if (!push_file(filename))
		return 0;

	safe_asprintf(&path1, "%s/%s", old_dir, filename);
	filename2 = newfile ? newfile : filename;
	safe_asprintf(&path2, "%s/%s", new_dir, filename2);

	if (stat(path2, &fstat) != 0) {
		if (errno == ENOENT) {
			/* Don't consider an incomplete definition a change */
			if (strncmp(filename2, DECLARATION_PATH,
				    strlen(DECLARATION_PATH)) &&
			    !compare_config.no_moved_files) {
				ret = EXIT_KABI_CHANGE;
				printf("Symbol removed or moved: %s\n",
				       filename);
			}

			free(path1);
			free(path2);

			return ret;
		} else {
			fail("Failed to stat() file%s: %s\n",
			     path2, strerror(errno));
		}
	}

	file1 = safe_fopen(path1);
	file2 = safe_fopen(path2);

	root1 = obj_parse(file1, path1);
	root2 = obj_parse(file2, path2);

	free(path1);
	free(path2);

	if (compare_config.hide_kabi) {
		obj_hide_kabi(root1, compare_config.hide_kabi_new);
		obj_hide_kabi(root2, compare_config.hide_kabi_new);
	}

	if (compare_config.debug && !follow) {
		obj_debug_tree(root1);
		obj_debug_tree(root2);
	}

	if (follow) {
		stream = fopen("/dev/null", "w");
		if (stream == NULL)
			fail("Unable to open /dev/null: %s\n", strerror(errno));
	} else {
		stream = open_memstream(&s, &sz);
	}
	tmp = compare_tree(root1, root2, stream);

	if (tmp != COMP_SAME) {
		if (!follow) {
			printf("Changes detected in: %s\n", filename);
			fflush(stream);
			fputs(s, stdout);
			putchar('\n');
		}
		ret = EXIT_KABI_CHANGE;
	}

	obj_free(root1);
	obj_free(root2);
	fclose(file1);
	fclose(file2);
	fclose(stream);
	free(s);

	return ret;

}

static walk_rv_t compare_files_cb(char *kabi_path, void *arg)
{
	compare_config_t *conf = (compare_config_t *)arg;
	char *filename;

	if (compare_config.skip_duplicate && is_duplicate(kabi_path))
		return WALK_CONT;

	/* If conf->*_dir contains slashes, skip them */
	filename = kabi_path + strlen(conf->old_dir);
	while (*filename == '/')
		filename++;

	free_files();
	if (compare_two_files(filename, NULL, false))
		conf->ret = EXIT_KABI_CHANGE;

	return WALK_CONT;
}

#define COMPARE_NO_OPT(name) \
	{"no-"#name, no_argument, &compare_config.no_##name, 1}

/*
 * Performs the compare command
 */
int compare(int argc, char **argv)
{
	int opt, opt_index;
	char *old_dir, *new_dir;
	struct stat sb1, sb2;
	struct option loptions[] = {
		{"debug", no_argument, 0, 'd'},
		{"hide-kabi", no_argument, 0, 'k'},
		{"hide-kabi-new", no_argument, 0, 'n'},
		{"help", no_argument, 0, 'h'},
		{"skip-duplicate", no_argument, 0, 's'},
		{"follow", no_argument, &compare_config.follow, 1},
		{"no-offset", no_argument, &display_options.no_offset, 1},
		COMPARE_NO_OPT(replaced),
		COMPARE_NO_OPT(shifted),
		COMPARE_NO_OPT(inserted),
		COMPARE_NO_OPT(deleted),
		COMPARE_NO_OPT(added),
		COMPARE_NO_OPT(removed),
		{"no-moved-files", no_argument,
		 &compare_config.no_moved_files, 1},
		{0, 0, 0, 0}
	};

	memset(&display_options, 0, sizeof(display_options));

	while ((opt = getopt_long(argc, argv, "dknhs",
				  loptions, &opt_index)) != -1) {
		switch (opt) {
		case 0:
			break;
		case 'd':
			compare_config.debug = true;
			break;
		case 'n':
			compare_config.hide_kabi_new = true;
			/* fall through */
		case 'k':
			compare_config.hide_kabi = true;
			break;
		case 's':
			compare_config.skip_duplicate = true;
			break;
		case 'h':
		default:
			compare_usage();
		}
	}

	if (argc < optind + 2) {
		printf("Wrong number of argument\n");
		compare_usage();
	}

	old_dir = compare_config.old_dir = argv[optind++];
	new_dir = compare_config.new_dir = argv[optind++];

	if ((stat(old_dir, &sb1) == -1) || (stat(new_dir, &sb2) == -1))
		fail("stat failed: %s\n", strerror(errno));

	if (S_ISREG(sb1.st_mode) && S_ISREG(sb2.st_mode)) {
		char *oldname = basename(old_dir);
		char *newname = basename(new_dir);

		if (optind != argc) {
			printf("Too many arguments\n");
			compare_usage();
		}
		compare_config.old_dir = dirname(old_dir);
		compare_config.new_dir = dirname(new_dir);

		return compare_two_files(oldname, newname, false);
	}

	if (!S_ISDIR(sb1.st_mode) || !S_ISDIR(sb2.st_mode)) {
		printf("Compare takes two directories or two regular"
		       " files as arguments\n");
		compare_usage();
	}

	if (optind == argc) {
		walk_dir(old_dir, false, compare_files_cb, &compare_config);

		return compare_config.ret;
	}

	while (optind < argc) {
		char *path, *filename;

		filename = compare_config.filename =  argv[optind++];
		safe_asprintf(&path, "%s/%s", old_dir, filename);

		if (stat(path, &sb1) == -1) {
			if (errno == ENOENT)
				fail("file does not exist: %s\n", path);
			fail("stat failed: %s\n", strerror(errno));
		}

		if (!S_ISREG(sb1.st_mode)) {
			printf("Compare third argument must be a regular file");
			compare_usage();
		}
		free(path);

		if (compare_two_files(filename, NULL, false))
			compare_config.ret = EXIT_KABI_CHANGE;
	}

	return compare_config.ret;
}
