#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "ini.h"

static int handler(void* user, const char* section, const char* name, const char* value)
{
	configuration* pconfig = (configuration*)user;
	if (strcmp(section, "platform") == 0 && strcmp(name, "soc_family") == 0) {
		pconfig->soc_family = strdup(value);
	} else if (strcmp(section, "sensor_1") == 0 && strcmp(name, "sensor_name") == 0) {
		pconfig->sensor_1_name = strdup(value);
	} else if (strcmp(section, "sensor_1") == 0 && strcmp(name, "sensor_bus") == 0) {
		pconfig->sensor_1_bus = strdup(value);
	} else if (strcmp(section, "sensor_1") == 0 && strcmp(name, "sensor_i2c_address") == 0) {
		pconfig->sensor_1_i2c_address = (int)strtol(value, NULL, 0);
	} else if (strcmp(section, "sensor_1") == 0 && strcmp(name, "sensor_width") == 0) {
		pconfig->sensor_1_width = atoi(value);
	} else if (strcmp(section, "sensor_1") == 0 && strcmp(name, "sensor_height") == 0) {
		pconfig->sensor_1_height = atoi(value);
	} else if (strcmp(section, "sensor_1") == 0 && strcmp(name, "sensor_fps") == 0) {
		pconfig->sensor_1_fps = atoi(value);
	} else if (strcmp(section, "development") == 0 && strcmp(name, "debug") == 0) {
		pconfig->debug = strdup(value);
	} else {
		return 0; // unknown section/name, error
	}
	return 1;
}

int load_configuration(const char* filename, configuration* config)
{
	if (ini_parse(filename, handler, config) < 0) {
		return -1; // Failed to load file
	}
	return 0; // Success
}

void free_configuration(configuration* config)
{
	if (config->soc_family) {
		free((void*)config->soc_family);
	}
	if (config->sensor_1_name) {
		free((void*)config->sensor_1_name);
	}
	if (config->debug) {
		free((void*)config->debug);
	}
}
