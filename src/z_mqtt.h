#ifndef Z_MQTT_H
#define Z_MQTT_H
#include "z_config.h"
#include "z_bacnet.h" // Pour avoir accès à la struct BACnetObject

void setup_mqtt();
void handle_mqtt();
bool is_mqtt_connected();

// Fonction centralisée pour la publication
void publish_mqtt_topic(uint32_t device_id, BACnetObject& obj, uint8_t prop_id, bool retain);
void publish_ha_autodiscovery();
void trigger_ha_discovery(uint32_t did = 0, uint32_t inst = 0, uint16_t type = 0xFFFF);

#endif
