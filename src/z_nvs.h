#ifndef Z_NVS_H
#define Z_NVS_H

#include <Preferences.h>
#include "z_config.h"

void load_configuration();
void save_configuration();
void load_device_objects(uint32_t ulDeviceId);
void save_device_objects(uint32_t ulDeviceId);
void save_device_objects_locked(uint32_t ulDeviceId);
void save_object_states(uint32_t ulDeviceId, uint16_t type, uint32_t ulInstance, const std::vector<String>& states);
void load_object_states(uint32_t ulDeviceId, uint16_t type, uint32_t ulInstance, std::vector<String>& states);

#endif
