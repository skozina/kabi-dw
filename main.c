#include <stdio.h>
#include <stdlib.h>

#include "kabi-dw.h"

int main(int argc, char **argv) {
	if(argc != 2) {
		printf("Usage: %s file\n", argv[0]);
		exit(1);
	}

	print_symbol(argv[1], "init_task");
	return 0;
}
