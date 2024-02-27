#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "ini.h"

const config_option_t config_options[] = {
	{"development", "debug", offsetof(configuration, debug), TYPE_STRING},
	{"platform", "soc_family", offsetof(configuration, soc_family), TYPE_STRING},
	{"sensor_1", "sensor_name", offsetof(configuration, sensor_1_name), TYPE_STRING},
	{"sensor_1", "sensor_bus", offsetof(configuration, sensor_1_bus), TYPE_STRING},
	{"sensor_1", "sensor_i2c_address", offsetof(configuration, sensor_1_i2c_address), TYPE_INT},
	{"sensor_1", "sensor_width", offsetof(configuration, sensor_1_width), TYPE_INT},
	{"sensor_1", "sensor_height", offsetof(configuration, sensor_1_height), TYPE_INT},
	{"sensor_1", "sensor_fps", offsetof(configuration, sensor_1_fps), TYPE_INT},
	{"network", "buffer_size", offsetof(configuration, uds_buffer_size), TYPE_INT},
	{"network", "ring_buffer_size", offsetof(configuration, ring_buffer_size), TYPE_INT},
	{NULL, NULL, 0, 0} // Mark the end of the options
};

static int handler(void* user, const char* section, const char* name, const char* value) {
	configuration* pconfig = (configuration*)user;
	for (const config_option_t* option = config_options; option->section; ++option) {
		if (strcmp(section, option->section) == 0 && strcmp(name, option->key) == 0) {
			char* dest = (char*)pconfig + option->offset;
			switch (option->type) {
				case TYPE_STRING:
					*((char**)dest) = strdup(value);
					break;
				case TYPE_INT:
					*((int*)dest) = (int)strtol(value, NULL, 0);
					break;
			}
			return 1;
		}
	}
	return 0; // unknown section/name, error
}

int load_configuration(const char* filename, configuration* config)
{
	if (ini_parse(filename, handler, config) < 0) {
		return -1; // Failed to load file
	}

	if (strcmp(config->debug, "true") == 0){
		printf("Config loaded from 'raptor.ini':\n");
		printf("debug=%s\n", config->debug);
		printf("soc_family=%s\n", config->soc_family);
		printf("sensor_1_name=%s\n", config->sensor_1_name);
		printf("sensor_1_frame_rate=%d\n", config->sensor_1_fps);
		printf("sensor_1_bus=%s\n", config->sensor_1_bus);
		printf("sensor_1_i2c_address=0x%x\n", config->sensor_1_i2c_address);
		printf("sensor_1_width=%d\n", config->sensor_1_width);
		printf("sensor_1_height=%d\n", config->sensor_1_height);
		printf("socket_buffer_size=%d\n", config->uds_buffer_size);
		printf("ring_buffer_size=%d\n\n", config->ring_buffer_size);
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
