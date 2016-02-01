#include <stdio.h>
#include <stdlib.h>

#include "kabi-dw.h"

int main(int argc, char **argv) {
	int i = 0;

	if(argc != 2) {
		printf("Usage: %s file\n", argv[0]);
		exit(1);
	}

	char *names[] = {
		"init_task",
		"x86_64_start_kernel",
		"acpi_disabled",
		"acpi_evaluate_integer",
		"acpi_initialize_hp_context",
		NULL
	};

	while (names[i] != NULL) {
		if (!print_symbol(argv[1], names[i]))
			printf("%s not found!\n", names[i]);
		printf("============================\n");
		i++;
	}

	return 0;
}
