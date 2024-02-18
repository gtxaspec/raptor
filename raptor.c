#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <imp/imp_log.h>
#include <imp/imp_system.h>
#include "system.h"
#include "version.h"
#include "encoder.h"
#include "config.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define TAG "raptor"

configuration config = {0}; // Initialize the config structure

void displayUsage() {
	printf("Raptor Video Daemon for %s Version: %s\n", TOSTRING(SOC), VERSION);
	printf("usage: ingenic-vidcap [args...]\n\n"
		" --help            display this help message\n");
	exit(0);
}

void handle_sigpipe(int sig) {
	// Ignore for now
	//printf("SIGPIPE caught.\n");
}

int main(int argc, char *argv[])
{
	signal(SIGPIPE, handle_sigpipe);
	int i, ret;

	if (load_configuration("raptor.ini", &config) < 0) {
		printf("Can't load 'raptor.ini'\n");
		return 1;
	}

	printf("Config loaded from 'raptor.ini': soc_family=%s, sensor=%s, frame_rate=%d, debug=%s, sensor_i2c=%x,sensor_width=%d\n\n",
	config.soc_family, config.sensor_1_name, config.sensor_1_fps, config.debug, config.sensor_1_i2c_address, config.sensor_1_width);
	//return 0;

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

    printf("Raptor Video Daemon for %s Sensor: %s Version: %s\n", TOSTRING(SOC), TOSTRING(SENSOR), VERSION);
    IMP_System_GetCPUInfo();
    ret = system_initalize();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "system init failed\n");
		return -1;
	}

	free_configuration(&config); // Free the configuration resources before exiting

	return 0;
}
