#ifndef Z_NVS_H
#define Z_NVS_H

#include <Preferences.h>
#include "z_config.h"

void load_configuration();
void save_configuration();
void load_device_objects(uint32_t device_id);
void save_device_objects(uint32_t device_id);
void save_device_objects_locked(uint32_t device_id);

#endif
