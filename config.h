#ifndef CONFIG_H
#define CONFIG_H

// Define the configuration structure
typedef struct {
	char *soc_family;
	char *sensor_1_name;
	char *sensor_1_bus;
	int sensor_1_i2c_address;
	int sensor_1_width;
	int sensor_1_height;
	int sensor_1_fps;
	char *debug;
	int uds_buffer_size;
	int ring_buffer_size;
} configuration;

// Define the type for configuration values
typedef enum { TYPE_STRING, TYPE_INT } config_value_type;

// Define the structure for mapping configuration options
typedef struct {
	const char *section;
	const char *key;
	size_t offset;
	config_value_type type;
} config_option_t;

// Function to load the configuration from a file
int load_configuration(const char* filename, configuration* config);

// Function to free allocated configuration resources
void free_configuration(configuration* config);

extern configuration config;

#endif // CONFIG_H
