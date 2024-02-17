#ifndef CONFIG_H
#define CONFIG_H

typedef struct
{
    const char* soc_family;
    const char* sensor_1_name;
    const char* sensor_1_bus;
    int sensor_1_i2c_address;
    int sensor_1_width;
    int sensor_1_height;
    int sensor_1_fps;
    const char* debug;
} configuration;

// Function to load the configuration from a file
int load_configuration(const char* filename, configuration* config);

// Function to free allocated configuration resources
void free_configuration(configuration* config);

extern configuration config;

#endif // CONFIG_H
