#define	_GNU_SOURCE	/* asprintf() */
#define	_POSIX_C_SOURCE 200809L /* mkdtemp() */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <libgen.h> /* basename(), dirname() */

#include "main.h"
#include "utils.h"
#include "check.h"
#include "generate.h"

#define	DEF_BUFSIZE (16)

typedef bool (*parse_func_t)(FILE *, FILE *, char *, check_config_t *);

static bool parse_func(FILE *, FILE *, char *, check_config_t *);
static bool parse_typedef(FILE *, FILE *, char *, check_config_t *);
static bool parse_var(FILE *, FILE *, char *, check_config_t *);
static bool parse_enum(FILE *, FILE *, char *, check_config_t *);
static bool parse_struct(FILE *, FILE *, char *, check_config_t *);
static bool parse_union(FILE *, FILE *, char *, check_config_t *);

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

	result &= verify_const_word_fp(fp_old, word);
	result &= verify_const_word_fp(fp_new, word);

	return (result);
}

static bool verify_word(FILE *fp, const char *file_name, char **word) {
	bool result = true;
	int c;

	*word = read_word(fp, &c);
	if (c == EOF)
		fail("Required word missing in %s!\n", file_name);

	return (result);
}

static bool verify_words(FILE *fp_old, FILE *fp_new, const char *file_name,
    char **oldw, char **neww) {
	bool result = true;

	result &= verify_word(fp_old, file_name, oldw);
	result &= verify_word(fp_new, file_name, neww);

	if (strcmp(*oldw, *neww) != 0)
		result = false;

	return (result);
}

static bool parse_type(FILE *fp_old, FILE *fp_new, char *file_name,
    check_config_t *conf) {
	char *oldw, *neww;
	parse_func_t parse_func;
	bool result = true;
	int c = 0;

	while (c != EOF) {
		/* Read the type from the original file */
		oldw = read_word(fp_old, &c);
		neww = read_word(fp_new, NULL);
		if (c == EOF)
			goto done;

		/* Verify the word we just read */
		if (strcmp(oldw, neww) != 0) {
			printf("Different type in %s:\n", file_name);
			printf("Expected: %s\n", oldw);
			printf("Current: %s\n", neww);
			result = false;
		}

		parse_func = get_parse_func(oldw);
		if (parse_func != NULL) {
			result &= parse_func(fp_old, fp_new, file_name, conf);
			goto done;
		}

		/*
		 * Verify remote reference files.
		 * As we walk all the files in the kabi dir there's no need to
		 * descent into the referenced file.
		 */
		if (oldw[0] == '@') {
			if (conf->verbose)
				printf("Reference: %s\n", oldw);
			goto done;
		}

		if (conf->verbose)
			printf("%s", oldw);
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

		free(oldw);
		free(neww);
	}

done:
	free(oldw);
	free(neww);
	return (result);
}

static bool parse_typedef(FILE *fp_old, FILE * fp_new, char *file_name,
    check_config_t *conf) {
	char *oldw, *neww;
	bool result = true;

	/* The name of the typedef */
	result &= verify_words(fp_old, fp_new, file_name, &oldw, &neww);
	if (!result) {
		printf("Different typedef name in %s:\n", file_name);
		printf("Expected: %s\n", oldw);
		printf("Current: %s\n", neww);
	}
	if (conf->verbose)
		printf("Typedef name parsed: %s\n", oldw);
	free(oldw);
	free(neww);

	/* The type of the typedef follows */
	if (conf->verbose)
		printf("Type: ");

	result &= parse_type(fp_old, fp_new, file_name, conf);
	return (result);
}

static bool parse_func(FILE *fp_old, FILE * fp_new, char *file_name,
    check_config_t *conf) {
	char *oldw, *neww;
	bool result = true;

	/* The name of the function */
	result &= verify_words(fp_old, fp_new, file_name, &oldw, &neww);
	if (!result) {
		printf("Different variable name in %s:\n", file_name);
		printf("Expected: %s\n", oldw);
		printf("Current: %s\n", neww);
	}
	if (conf->verbose)
		printf("Func name parsed: %s\n", oldw);
	free(oldw);
	free(neww);

	if (!verify_const_word(fp_old, fp_new, "("))
		fail("Missing function left bracket in: %s\n", file_name);

	/* Arguments */
	while (true) {
		int rv = verify_words(fp_old, fp_new, file_name, &oldw, &neww);
		result &= rv;

		if (!rv) {
			if (strcmp(oldw, ")") == 0) {
				printf("Unexpected argument found in %s:\n",
				    file_name);
				printf("Argument: %s\n", neww);
			} else if (strcmp(neww, ")") == 0) {
				printf("Argument missing in %s:\n",
				    file_name);
				printf("Argument: %s\n", oldw);
			} else {
				printf("Different argument name in %s:\n",
				    file_name);
				printf("Expected: %s\n", oldw);
				printf("Current: %s\n", neww);
			}
		}

		if (strcmp(oldw, ")") == 0) {
			free(oldw);
			free(neww);
			break;
		}

		if (conf->verbose)
			printf("Argument name: %s, ", oldw);
		free(oldw);
		free(neww);

		/* Argument type */
		if (conf->verbose)
			printf("Type: ");

		result &= parse_type(fp_old, fp_new, file_name, conf);
	}

	/* Function return type */
	if (conf->verbose)
		printf("Return value: ");

	result &= parse_type(fp_old, fp_new, file_name, conf);
	return (result);
}

static bool get_next_field(FILE *fp, char *file_name, char *oldw, char **neww,
    char **newoff, check_config_t *conf) {
	bool result = true;

	while (true) {
		/* Skip type of the field */
		/* TODO Test this!!! */
		(void) parse_type(NULL, fp, file_name, conf);

		/* Get new offset */
		free(*newoff);
		(void) verify_word(fp, file_name, newoff);

		/* And new field name */
		free(*neww);
		result = verify_word(fp, file_name, neww);
		if (!result || strcmp(*neww, "}"))
			return (false);

		if (strcmp(oldw, *neww) == 0)
			return (true);
	}

	/* Not reached */
	return (false);
}

static bool parse_struct(FILE *fp_old, FILE * fp_new, char *file_name,
    check_config_t *conf) {
	char *oldw, *neww;
	bool result = true;

	/* The name of the struct */
	result &= verify_words(fp_old, fp_new, file_name, &oldw, &neww);
	if (!result) {
		printf("Different struct name in %s:\n", file_name);
		printf("Expected: %s\n", oldw);
		printf("Current: %s\n", neww);
	}
	if (conf->verbose)
		printf("Struct name parsed: %s\n", oldw);
	free(oldw);
	free(neww);

	if (!verify_const_word(fp_old, fp_new, "{"))
		fail("Missing struct left bracket in: %s\n", file_name);

	/* Struct fields */
	while (true) {
		char *oldoff, *newoff;
		char *oldname, *newname;

		/* Field offsets */
		int rv = verify_words(fp_old, fp_new, file_name, &oldoff,
		    &newoff);
		result &= rv;

		/* End of old kabi file, we're done. */
		if (strcmp(oldoff, "}") == 0) {
			free(oldoff);
			free(newoff);
			break;
		}

		/* End of new kabi file, fields missing. */
		if (strcmp(newoff, "}") == 0) {
			int c;
			char *name;
			printf("Struct content missing in %s:\n", file_name);
			name = read_word(fp_old, &c);
			printf("Field name: %s\n", name);
			free(name);
			free(oldoff);
			free(newoff);
			break;
		}

		/* Field names */
		rv = verify_words(fp_old, fp_new, file_name, &oldname,
		    &newname);
		result &= rv;

		/*
		 * If the struct field name differs, try to find the right
		 * one on the next line.
		 */
		if (!rv) {
			/*
			 * If we didn't find the old struct field value
			 * on the current line or till the end of file,
			 * it's missing.
			 */
			if (!get_next_field(fp_new, file_name, oldname,
				    &newname, &newoff, conf)) {
				printf("Struct field missing in %s:\n",
				    file_name);
				printf("Field name: %s\n", oldname);
				free(oldname);
				free(newname);
				break;
			}
		}

		if (strcmp(oldname, newname) != 0)
			fail("Struct field names not the same in %s: "
			    "%s vs. %s\n", file_name, oldname, newname);

		/* Verify the struct field offset */
		if (strcmp(oldoff, newoff) != 0) {
			printf("Offset of field %s differs in %s:\n", oldname,
			    file_name);
			printf("Expected: %s\n", oldoff);
			printf("Current: %s\n", newoff);
			result = false;
		}

		if (conf->verbose) {
			printf("Field name: %s\n", oldname);
			printf("Field offset: %s\n", oldoff);
		}

		/* Function return type */
		if (conf->verbose)
			printf("Field type: ");

		result &= parse_type(fp_old, fp_new, file_name, conf);
	}
	return (true);
}

static bool get_next_union(FILE *fp, char *file_name, char *oldw,
    char **neww, check_config_t *conf) {
	bool result = true;

	while (true) {
		/* Skip type of the field */
		(void) parse_type(NULL, fp, file_name, conf);

		/* Get new field name */
		free(*neww);
		result = verify_word(fp, file_name, neww);
		if (!result || strcmp(*neww, "}"))
			return (false);

		if (strcmp(oldw, *neww) == 0)
			return (true);
	}

	/* Not reached */
	return (false);
}

static bool parse_union(FILE *fp_old, FILE * fp_new, char *file_name,
    check_config_t *conf) {
	char *oldw, *neww;
	bool result = true;

	/* The name of the union */
	result &= verify_words(fp_old, fp_new, file_name, &oldw, &neww);
	if (!result) {
		printf("Different union name in %s:\n", file_name);
		printf("Expected: %s\n", oldw);
		printf("Current: %s\n", neww);
	}
	if (conf->verbose)
		printf("Union name parsed: %s\n", oldw);
	free(oldw);
	free(neww);

	if (!verify_const_word(fp_old, fp_new, "{"))
		fail("Missing struct left bracket in: %s\n", file_name);

	/* Struct values */
	while (true) {
		/* Field names */
		int rv = verify_words(fp_old, fp_new, file_name, &oldw, &neww);
		result &= rv;

		/* End of old kabi file, we're done. */
		if (strcmp(oldw, "}") == 0) {
			free(oldw);
			free(neww);
			break;
		}

		/* End of new kabi file, fields missing. */
		if (strcmp(neww, "}") == 0) {
			int c;
			char *name;
			printf("Union field missing in %s:\n", file_name);
			name = read_word(fp_old, &c);
			printf("Field name: %s\n", name);
			free(name);
			free(oldw);
			free(neww);
			break;
		}

		/*
		 * If the union field name differs, try to find the right
		 * one on the next line.
		 */
		if (!rv) {
			/*
			 * If we didn't find the old field on the current
			 * line or till the end of file, it's missing.
			 */
			if (!get_next_union(fp_new, file_name, oldw,
				    &neww, conf)) {
				printf("Union field missing in %s:\n",
				    file_name);
				printf("Field name: %s\n", oldw);
				free(oldw);
				free(neww);
				break;
			}
		}

		if (strcmp(oldw, neww) != 0)
			fail("Union field names not the same in %s: "
			    "%s vs. %s\n", file_name, oldw, neww);
		if (conf->verbose)
			printf("Union Name: %s\n", oldw);

		/* Union type */
		if (conf->verbose)
			printf("Type: ");

		result &= parse_type(fp_old, fp_new, file_name, conf);
	}

	return (true);
}

static bool get_next_enum(FILE *fp, char *file_name, char *oldw, char **neww) {
	int i;
	bool result = true;

	while (true) {
		/* Each enum line has 3 parts */
		for (i = 0; i < 3; i++) {
			free(*neww);
			result = verify_word(fp, file_name, neww);
			if (!result || strcmp(*neww, "}"))
				return (false);
		}

		if (strcmp(oldw, *neww) == 0)
			return (true);
	}

	/* Not reached */
	return (false);
}

static bool parse_enum(FILE *fp_old, FILE * fp_new, char *file_name,
    check_config_t *conf) {
	char *oldw, *neww;
	bool result = true;

	/* The name of the enum */
	result &= verify_words(fp_old, fp_new, file_name, &oldw, &neww);
	if (!result) {
		printf("Different enum name in %s:\n", file_name);
		printf("Expected: %s\n", oldw);
		printf("Current: %s\n", neww);
	}
	if (conf->verbose)
		printf("Enum name parsed: %s\n", oldw);
	free(oldw);
	free(neww);

	if (!verify_const_word(fp_old, fp_new, "{"))
		fail("Missing enum left bracket in: %s\n", file_name);

	/* Enum values */
	while (true) {
		char *oldname, *newname;
		int rv = verify_words(fp_old, fp_new, file_name, &oldname,
		    &newname);
		result &= rv;

		/* End of old kabi file, we're done. */
		if (strcmp(oldname, "}") == 0) {
			free(oldname);
			free(newname);
			break;
		}

		/*
		 * If the enum name differs, try to find the right one on
		 * the next line
		 */
		if (!rv) {
			/*
			 * If we didn't find the old enum value on the current
			 * line or till the end of file, it's missing.
			 */
			if ((strcmp(newname, "}") == 0) ||
			    (!get_next_enum(fp_new, file_name, oldname,
				    &newname))) {
				printf("Enum value missing in %s:\n",
				    file_name);
				printf("Name: %s\n", oldname);
				free(oldname);
				free(newname);
				break;
			}
		}

		/* The enum names are the same, verify them */
		if (!verify_const_word(fp_old, fp_new, "="))
			fail("Missing equal sign in: %s\n", file_name);

		/* Enum value */
		rv = verify_words(fp_old, fp_new, file_name, &oldw, &neww);
		result &= rv;

		if (strcmp(oldw, neww) != 0) {
			printf("Value of enum %s differs in %s:\n", oldname,
			    file_name);
			printf("Expected: %s\n", oldw);
			printf("Current: %s\n", neww);
			result = false;
		}

		if (conf->verbose) {
			printf("Enum value name: %s, ", oldname);
			printf("value: %s\n", oldw);
		}
		free(oldname);
		free(newname);
	}

	return (result);
}

static bool parse_var(FILE *fp_old, FILE * fp_new, char *file_name,
    check_config_t *conf) {
	char *oldw, *neww;
	bool result = true;

	/* The name of the variable */
	result &= verify_words(fp_old, fp_new, file_name, &oldw, &neww);
	if (!result) {
		printf("Different variable name in %s:\n", file_name);
		printf("Expected: %s\n", oldw);
		printf("Current: %s\n", neww);
	}

	if (conf->verbose)
		printf("Variable name parsed: %s\n", oldw);
	free(oldw);
	free(neww);

	/* The type of the variable follows */
	if (conf->verbose)
		printf("Type: ");

	result &= parse_type(fp_old, fp_new, file_name, conf);
	return (result);
}

static void check_CU_and_file(FILE *fp_old, FILE *fp_new, char *file_name) {
	char *line_old = NULL, *line_new = NULL;
	size_t len_old = 0, len_new = 0;

	/* Check CU */
	if (getline(&line_old, &len_old, fp_old) == -1) {
		printf("CU line missing in: %s\n", file_name);
	}
	if (getline(&line_new, &len_new, fp_new) == -1) {
		printf("CU line missing in: %s\n", file_name);
	}
	if (strcmp(line_new, line_old) != 0) {
		printf("CU of %s differs:\n", file_name);
		printf("Should be: %s\n", line_old);
		printf("Is: %s\n", line_new);
	}

	/* Check file */
	if (getline(&line_old, &len_old, fp_old) == -1) {
		printf("File line missing in: %s\n", file_name);
	}
	if (getline(&line_new, &len_new, fp_new) == -1) {
		printf("File line missing in: %s\n", file_name);
	}
	if (strcmp(line_new, line_old) != 0) {
		printf("File of %s differs:\n", file_name);
		printf("Should be: %s\n", line_old);
		printf("Is: %s\n", line_new);
	}

	if (line_old != NULL)
		free(line_old);
	if (line_new != NULL)
		free(line_new);
}

static bool check_symbol_file(char *kabi_path, void *arg) {
	check_config_t *conf = (check_config_t *)arg;
	char *file_name = kabi_path + strlen(conf->kabi_dir);
	struct stat fstat;
	char *temp_kabi_path;
	FILE *fp_new, *fp_old;

	/* If conf->kabi_dir doesn't contain trailing slashes, skip them too */
	while (*file_name == '/')
		file_name++;

	if (conf->verbose)
		printf("Checking %s\n", file_name);

	if (asprintf(&temp_kabi_path, "%s/%s", conf->temp_kabi_dir, file_name)
	    == -1)
		fail("asprintf() failed\n");

	if (stat(temp_kabi_path, &fstat) != 0) {
		if (errno == ENOENT) {
			printf("Symbol removed from kabi: %s\n", file_name);
		} else {
			printf("Failed to stat() file%s: %s\n", temp_kabi_path,
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

	check_CU_and_file(fp_old, fp_new, file_name);
	(void) parse_type(fp_old, fp_new, file_name, conf);

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
