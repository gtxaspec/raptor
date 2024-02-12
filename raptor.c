#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <imp/imp_log.h>
#include "system.h"

#define TAG "raptor"

void displayUsage() {
	printf("usage: ingenic-vidcap [args...]\n\n"
		" --help            display this help message\n");
	exit(0);
}

int main(int argc, char *argv[])
{
	int i, ret;

	// parse args
	for (i = 0; i < argc; i++) {
		char *arg = argv[i];

		if (*arg == '-') {
			arg++;
			if (*arg == '-') arg++; // tolerate 2 dashes

			if (strcmp(arg, "help") == 0) {
				displayUsage();
				exit(0);
			} else {
				printf("unrecognized argument %s\n\n", argv[i]);
				displayUsage();
				exit(2);
			}
		}
	}

    ret = system_initalize();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "system init failed\n");
		return -1;
	}

	return 0;
}
