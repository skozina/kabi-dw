#define	_GNU_SOURCE	/* asprintf() */
#define	_POSIX_C_SOURCE 200809L /* mkdtemp() */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <libgen.h> /* basename(), dirname() */

#include "main.h"
#include "utils.h"
#include "check.h"
#include "generate.h"

#define	DEF_BUFSIZE (16)

typedef bool (*parse_func_t)(FILE *, FILE *, check_config_t *);

static bool parse_func(FILE *, FILE *, check_config_t *);
static bool parse_typedef(FILE *, FILE *, check_config_t *);
static bool parse_var(FILE *, FILE *, check_config_t *);
static bool parse_enum(FILE *, FILE *, check_config_t *);
static bool parse_struct(FILE *, FILE *, check_config_t *);
static bool parse_union(FILE *, FILE *, check_config_t *);

/*
 * Table of known types we should fine as the first word on the third line
 * of each type file.
 */
static struct type_prefix {
	char *prefix;
	parse_func_t parse_func;
} type_prefixes[] = {
	{ "func", parse_func },
	{ "typedef", parse_typedef },
	{ "var", parse_var },
	{ "enum", parse_enum },
	{ "struct", parse_struct },
	{ "union", parse_union },
	{ "NULL", NULL },
};

static void print_warning(char *msg, check_config_t *conf, char *name,
    char *expected, char *actual) {
	printf("=========\n");
	printf("Difference found in file %s:\n", conf->file_name);
	printf("%s:\n", msg);
	if (name != NULL)
		printf("Symbol: %s\n", name);
	printf("Expected: %s\n", expected);
	printf("Actual: %s\n", actual);
}

static parse_func_t get_parse_func(char *type) {
	struct type_prefix *current;

	for (current = type_prefixes; current->prefix != NULL; current++) {
		if (strcmp(type, current->prefix) == 0)
			break;
	}

	return (current->parse_func);
}

/*
 * Read word delimited by ' ' or '\n' from fp.
 * Return the last char in *endchar.
 */
static char *read_word(FILE *fp, int *endchar) {
	size_t size = DEF_BUFSIZE;
	char *result = malloc(size);
	size_t i = 0;
	int c = 0;

	while (true) {
		if (i == size - 1) {
			size *= 2;
			result = realloc(result, size);
		}

		c = fgetc(fp);
		if (c == EOF || strchr(" \n", c) != NULL)
			break;

		result[i] = c;
		i++;
	}

	result[i] = '\0';

	if (endchar != NULL)
		*endchar = c;
	return (result);
}

static int verify_const_word_fp(FILE *fp, const char *word) {
	char *real_word;
	bool result = true;
	int c;

	real_word = read_word(fp, &c);

	if (c == EOF) {
		result = false;
	} else {
		if (strcmp(real_word, word) != 0)
			result = false;
		free(real_word);
	}

	return (result);
}

static bool verify_const_word(FILE *fp_old, FILE *fp_new, const char *word) {
	bool result = true;

	if (fp_old != NULL)
		result &= verify_const_word_fp(fp_old, word);
	result &= verify_const_word_fp(fp_new, word);

	return (result);
}

static void verify_word(FILE *fp, check_config_t *conf, char **word) {
	int c;

	*word = read_word(fp, &c);
	if (c == EOF)
		fail("Required word missing in %s!\n", conf->file_name);
}

static bool verify_words(FILE *fp_old, FILE *fp_new, check_config_t *conf,
    char **oldw, char **neww) {
	verify_word(fp_new, conf, neww);

	if (fp_old != NULL) {
		verify_word(fp_old, conf, oldw);
		if (strcmp(*oldw, *neww) != 0)
			return (false);
	} else {
		*oldw = strdup("");
	}

	return (true);
}

static bool parse_type(FILE *fp_old, FILE *fp_new, check_config_t *conf) {
	char *oldw, *neww;
	parse_func_t parse_func;
	bool result = true;
	int c = 0;

	assert(fp_new != NULL);

	while (c != EOF) {
		/* Read the type from the original file */
		neww = read_word(fp_new, &c);
		if (fp_old != NULL) {
			oldw = read_word(fp_old, &c);
			if (c == EOF)
				goto done;

			/* Verify the word we just read */
			if (strcmp(oldw, neww) != 0) {
				print_warning("Different type", conf, NULL,
				    oldw, neww);
				result = false;
				goto done;
			}
		}

		parse_func = get_parse_func(neww);
		if (parse_func != NULL) {
			result &= parse_func(fp_old, fp_new, conf);
			goto done;
		}

		/*
		 * Verify remote reference files.
		 * As we walk all the files in the kabi dir there's no need to
		 * descent into the referenced file.
		 */
		if (neww[0] == '@') {
			if (conf->verbose)
				printf("Reference: %s\n", neww);
			goto done;
		}

		if (conf->verbose)
			printf("%s", neww);
		/*
		 * If the word ended up with a newline, we just parsed base
		 * type.
		 */
		if (c == '\n') {
			if (conf->verbose)
				printf("\n");
			goto done;
		}
		if (conf->verbose)
			printf(" ");

		if (fp_old != NULL)
			free(oldw);
		free(neww);
	}

done:
	if (fp_old != NULL)
		free(oldw);
	free(neww);
	return (result);
}

static bool parse_typedef(FILE *fp_old, FILE * fp_new, check_config_t *conf) {
	char *oldw, *neww;
	bool result = true;

	/* The name of the typedef */
	result &= verify_words(fp_old, fp_new, conf, &oldw, &neww);
	if (!result) {
		print_warning("Different typedef name", conf, NULL, oldw,
		    neww);
	}
	if (conf->verbose)
		printf("Typedef name parsed: %s\n", oldw);
	free(oldw);
	free(neww);

	/* The type of the typedef follows */
	if (conf->verbose)
		printf("Type: ");

	result &= parse_type(fp_old, fp_new, conf);
	return (result);
}

static bool parse_func(FILE *fp_old, FILE * fp_new, check_config_t *conf) {
	char *oldw, *neww;
	bool result = true;

	/* The name of the function */
	result &= verify_words(fp_old, fp_new, conf, &oldw, &neww);
	if (!result) {
		print_warning("Different variable name", conf, NULL, oldw,
		    neww);
	}
	if (conf->verbose)
		printf("Func name parsed: %s\n", neww);
	free(oldw);
	free(neww);

	if (!verify_const_word(fp_old, fp_new, "("))
		fail("Missing function left bracket in: %s\n",
		    conf->file_name);

	/* Arguments */
	while (true) {
		int rv = verify_words(fp_old, fp_new, conf, &oldw, &neww);
		result &= rv;

		if (!rv) {
			if (strcmp(oldw, ")") == 0) {
				print_warning("Unexpected argument", conf,
				    NULL, NULL, neww);
			} else if (strcmp(neww, ")") == 0) {
				print_warning("Argument missing", conf,
				    NULL, oldw, NULL);
			} else {
				print_warning("Different argument name", conf,
				    NULL, oldw, neww);
			}
		}

		if ((strcmp(oldw, ")") == 0) ||
		    (strcmp(neww, ")") == 0)) {
			free(oldw);
			free(neww);
			break;
		}

		if (conf->verbose)
			printf("Argument name: %s, ", neww);
		free(oldw);
		free(neww);

		/* Argument type */
		if (conf->verbose)
			printf("Type: ");

		result &= parse_type(fp_old, fp_new, conf);
	}

	/* Function return type */
	if (conf->verbose)
		printf("Return value: ");

	result &= parse_type(fp_old, fp_new, conf);
	return (result);
}

static bool get_next_field(FILE *fp_old, FILE *fp_new, char **oldname,
    char **newname, char **oldoff, char **newoff, check_config_t *conf) {

	/* Field offsets */
	(void) verify_words(fp_old, fp_new, conf, oldoff, newoff);

	/* End of old kabi file, we're done. */
	if (strcmp(*oldoff, "}") == 0) {
		*newname = NULL;
		*oldname = NULL;
		return (true);
	}

	/* End of new kabi file, fields missing. */
	if (strcmp(*newoff, "}") == 0) {
		int c;
		*oldname = read_word(fp_old, &c);
		*newname = NULL;
		return (false);
	}

	/* Field names */
	if (verify_words(fp_old, fp_new, conf, oldname, newname) &&
	    (fp_old != NULL))
		return (true);

	while (true) {
		if (conf->verbose)
			printf("Skipping field: %s %s ", *newoff, *newname);

		/* Skip type of the field */
		(void) parse_type(NULL, fp_new, conf);

		/* Get new offset */
		free(*newoff);
		verify_word(fp_new, conf, newoff);

		if (strcmp(*newoff, "}") == 0)
			return (false);

		/* And new field name */
		free(*newname);
		verify_word(fp_new, conf, newname);

		if (strcmp(*oldname, *newname) == 0)
			return (true);
	}

	/* Not reached */
	return (false);
}

static bool parse_struct(FILE *fp_old, FILE * fp_new, check_config_t *conf) {
	char *oldoff, *newoff;
	char *oldname, *newname;
	bool result = true;

	/* The name of the struct */
	result &= verify_words(fp_old, fp_new, conf, &oldname, &newname);
	if (!result) {
		print_warning("Different struct name", conf, NULL, oldname,
		    newname);
	}
	if (conf->verbose)
		printf("Struct name parsed: %s\n", newname);
	free(oldname);
	free(newname);

	if (!verify_const_word(fp_old, fp_new, "{"))
		fail("Missing struct left bracket in: %s\n", conf->file_name);

	/* Struct fields */
	while (true) {
		/* Find first two fields in the struct of the same name. */
		if (!get_next_field(fp_old, fp_new, &oldname, &newname,
		    &oldoff, &newoff, conf)) {
			if (fp_old != NULL)
				print_warning("Struct field missing", conf,
				    NULL, oldname, NULL);
			result = false;
			break;
		} else {
			if (strcmp(oldoff, "}") == 0)
				break;
		}

		assert(strcmp(oldname, newname) == 0);

		/* Verify the struct field offset */
		if (strcmp(oldoff, newoff) != 0) {
			print_warning("Field offset differs", conf, oldname,
			    oldoff, newoff);
			result = false;
		}

		if (conf->verbose) {
			printf("Field name: %s\n", oldname);
			printf("Field offset: %s\n", oldoff);
			printf("Field type: ");
		}

		free(oldoff);
		free(newoff);
		free(oldname);
		free(newname);
		result &= parse_type(fp_old, fp_new, conf);
	}

	free(oldoff);
	free(newoff);
	free(oldname);
	free(newname);
	return (result);
}

static bool get_next_union(FILE *fp_old, FILE *fp_new, char **oldname,
    char **newname, check_config_t *conf) {
	/* Field names */
	(void) verify_words(fp_old, fp_new, conf, oldname, newname);

	/* End of old kabi file, we're done. */
	if (strcmp(*oldname, "}") == 0)
		return (true);

	/* End of new kabi file, fields missing. */
	if (strcmp(*newname, "}") == 0)
		return (false);

	if (strcmp(*oldname, *newname) == 0)
		return (true);

	while (true) {
		if (conf->verbose)
			printf("Skipping field: %s ", *newname);

		/* Skip type of the field */
		(void) parse_type(NULL, fp_new, conf);

		/* Get new field name */
		free(*newname);
		verify_word(fp_new, conf, newname);

		if (strcmp(*newname, "}") == 0)
			return (false);

		if (strcmp(*oldname, *newname) == 0)
			return (true);
	}

	/* Not reached */
	return (false);
}

static bool parse_union(FILE *fp_old, FILE * fp_new, check_config_t *conf) {
	char *oldname, *newname;
	bool result = true;

	/* The name of the union */
	result &= verify_words(fp_old, fp_new, conf,
	    &oldname, &newname);
	if (!result) {
		print_warning("Different union name", conf, NULL, oldname,
		    newname);
	}
	if (conf->verbose)
		printf("Union name parsed: %s\n", newname);
	free(oldname);
	free(newname);

	if (!verify_const_word(fp_old, fp_new, "{"))
		fail("Missing union left bracket in: %s\n", conf->file_name);

	/* Union fields */
	while (true) {
		/* Find first two fields in the union of the same name. */
		if (!get_next_union(fp_old, fp_new, &oldname, &newname,
		    conf)) {
			if (fp_old != NULL)
				print_warning("Union field missing", conf,
				    NULL, oldname, NULL);
			result = false;
			break;
		} else {
			if (strcmp(oldname, "}") == 0)
				break;
		}

		assert(strcmp(oldname, newname) == 0);

		if (conf->verbose) {
			printf("Union Name: %s\n", oldname);
			printf("Type: ");
		}

		free(oldname);
		free(newname);
		result &= parse_type(fp_old, fp_new, conf);
	}

	free(oldname);
	free(newname);
	return (result);
}

static bool get_next_enum(FILE *fp_old, FILE *fp_new, char **oldname,
    char **newname, check_config_t *conf) {
	int i;

	/* Enum name */
	(void) verify_words(fp_old, fp_new, conf, oldname, newname);

	/* End of old kabi file, we're done. */
	if (strcmp(*oldname, "}") == 0)
		return (true);

	/* End of new kabi file, fields missing. */
	if (strcmp(*newname, "}") == 0)
		return (false);

	if (strcmp(*oldname, *newname) == 0)
		return (true);

	while (true) {
		/* Each enum line has 3 parts */
		for (i = 0; i < 3; i++) {
			free(*newname);
			verify_word(fp_new, conf, newname);
			if (strcmp(*newname, "}") == 0)
				return (false);
		}

		if (strcmp(*oldname, *newname) == 0)
			return (true);
	}

	/* Not reached */
	return (false);
}

static bool parse_enum(FILE *fp_old, FILE * fp_new, check_config_t *conf) {
	char *oldval, *newval;
	char *oldname, *newname;
	bool result = true;

	/* The name of the enum */
	result &= verify_words(fp_old, fp_new, conf, &oldname, &newname);
	if (!result) {
		print_warning("Different enum name", conf, NULL, oldname,
		    newname);
	}
	if (conf->verbose)
		printf("Enum name parsed: %s\n", newname);
	free(oldname);
	free(newname);

	if (!verify_const_word(fp_old, fp_new, "{"))
		fail("Missing enum left bracket in: %s\n", conf->file_name);

	/* Enum values */
	while (true) {

		oldval = NULL;
		newval = NULL;
		if (!get_next_enum(fp_old, fp_new, &oldname, &newname, conf)) {
			print_warning("Enum value missing", conf, NULL,
			    oldname, NULL);
			result = false;
			break;
		} else {
			if (strcmp(oldname, "}") == 0)
				break;
		}

		/* The enum names are the same, verify them */
		if (!verify_const_word(fp_old, fp_new, "="))
			fail("Missing equal sign in: %s\n", conf->file_name);

		/* Enum value */
		result &= verify_words(fp_old, fp_new, conf, &oldval,
		    &newval);

		if (strcmp(oldval, newval) != 0) {
			print_warning("Value of enum differs", conf,
			    oldname, oldval, newval);
			result = false;
		}

		if (conf->verbose) {
			printf("Enum value name: %s, ", oldname);
			printf("value: %s\n", oldname);
		}
		free(oldval);
		free(newval);
		free(oldname);
		free(newname);
	}

	free(oldval);
	free(newval);
	free(oldname);
	free(newname);
	return (result);
}

static bool parse_var(FILE *fp_old, FILE * fp_new, check_config_t *conf) {
	char *oldw, *neww;
	bool result = true;

	/* The name of the variable */
	result &= verify_words(fp_old, fp_new, conf, &oldw, &neww);
	if (!result) {
		print_warning("Different variable name", conf, NULL,
		    oldw, neww);
	}

	if (conf->verbose)
		printf("Variable name parsed: %s\n", oldw);
	free(oldw);
	free(neww);

	/* The type of the variable follows */
	if (conf->verbose)
		printf("Type: ");

	result &= parse_type(fp_old, fp_new, conf);
	return (result);
}

static void check_CU_and_file(FILE *fp_old, FILE *fp_new,
    check_config_t *conf) {
	char *line_old = NULL, *line_new = NULL;
	size_t len_old = 0, len_new = 0;

	/* Check CU */
	if (getline(&line_old, &len_old, fp_old) == -1) {
		fail("CU line missing in: %s\n", conf->file_name);
	}
	if (getline(&line_new, &len_new, fp_new) == -1) {
		fail("CU line missing in: %s\n", conf->file_name);
	}
	/*
	 * We don't care if the CU has changed, it only needs to be provided by
	 * the same module.
	 */

	/* Check file */
	if (getline(&line_old, &len_old, fp_old) == -1) {
		fail("File line missing in: %s\n", conf->file_name);
	}
	if (getline(&line_new, &len_new, fp_new) == -1) {
		fail("File line missing in: %s\n", conf->file_name);
	}
	/* No need to compare file either. */

	if (line_old != NULL)
		free(line_old);
	if (line_new != NULL)
		free(line_new);
}

static bool check_symbol_file(char *kabi_path, void *arg) {
	check_config_t *conf = (check_config_t *)arg;
	struct stat fstat;
	char *temp_kabi_path;
	FILE *fp_new, *fp_old;

	/* If conf->kabi_dir doesn't contain trailing slashes, skip them too */
	conf->file_name = kabi_path + strlen(conf->kabi_dir);
	while (*conf->file_name == '/')
		conf->file_name++;

	if (conf->verbose)
		printf("Checking %s\n", conf->file_name);

	if (asprintf(&temp_kabi_path, "%s/%s", conf->temp_kabi_dir,
	    conf->file_name) == -1)
		fail("asprintf() failed\n");

	if (stat(temp_kabi_path, &fstat) != 0) {
		if (errno == ENOENT) {
			print_warning("Symbol removed from KABI", conf, NULL,
			    conf->file_name, NULL);
		} else {
			fail("Failed to stat() file%s: %s\n", temp_kabi_path,
			    strerror(errno));
		}

		goto all_done;
	}

	if ((fp_new = fopen(temp_kabi_path, "r")) == NULL) {
		printf("Failed to open file: %s\n", temp_kabi_path);
		goto all_done;
	}
	if ((fp_old = fopen(kabi_path, "r")) == NULL) {
		printf("Failed to open file: %s\n", kabi_path);
		goto new_done;
	}

	check_CU_and_file(fp_old, fp_new, conf);
	(void) parse_type(fp_old, fp_new, conf);

	fclose(fp_new);
new_done:
	fclose(fp_old);
all_done:
	free(temp_kabi_path);
	return (true);
}

static void generate_new_defs(check_config_t *conf) {
	generate_config_t *gen_conf = safe_malloc(sizeof (*gen_conf));

	gen_conf->kabi_dir = conf->temp_kabi_dir;
	gen_conf->verbose = conf->verbose;
	gen_conf->kernel_dir = conf->kernel_dir;
	gen_conf->symbols = conf->symbols;
	gen_conf->symbol_cnt = conf->symbol_cnt;
	if (conf->symbols != NULL) {
		int i;

		gen_conf->symbols_found = safe_malloc(conf->symbol_cnt *
		    sizeof (bool *));
		for (i = 0; i < conf->symbol_cnt; i++)
			gen_conf->symbols_found[i] = false;
	}

	generate_symbol_defs(gen_conf);

	if (conf->symbols != NULL)
		free(gen_conf->symbols_found);
	free(gen_conf);
}

static bool remove_node(char *path, void *arg) {
	if (remove(path) != 0) {
		printf("ERROR: Failed to remove %s: %s\n", path,
		    strerror(errno));
	}

	return (true);
}

void check_symbol_defs(check_config_t *conf) {
	printf("Comparing symbols defs of %s with %s...\n", conf->kernel_dir,
	    conf->kabi_dir);

	if (asprintf(&conf->temp_kabi_dir, "%s", TMP_DIR) == -1)
		fail("asprintf() failed\n");

	if (mkdtemp(conf->temp_kabi_dir) == NULL)
		fail("mkdtemp failed: %s\n", strerror(errno));

	printf("Working directory: %s\n", conf->temp_kabi_dir);

	generate_new_defs(conf);
	walk_dir(conf->kabi_dir, false, check_symbol_file, conf);

	walk_dir(conf->temp_kabi_dir, true, remove_node, NULL);
	if (rmdir(conf->temp_kabi_dir) != 0) {
		printf("ERROR: Failed to remove temp directory %s: %s\n",
		    conf->temp_kabi_dir, strerror(errno));
	}

	free(conf->temp_kabi_dir);
	conf->temp_kabi_dir = NULL;
}
