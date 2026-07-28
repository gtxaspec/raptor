/*
 * ric_json.h -- GPIO pin discovery from a thingino device file
 */
#ifndef RIC_JSON_H
#define RIC_JSON_H

#include "ric.h"

/*
 * Fill any still-unset (-1) IR-cut / IR LED pins in c from the JSON
 * device description at path. A missing file is silent; a file that
 * exists but cannot be used warns.
 */
void ric_json_gpio_load(ric_config_t *c, const char *path);

#endif /* RIC_JSON_H */
