#ifndef Z_NETWORK_H
#define Z_NETWORK_H

#include "z_config.h"
#include "z_ui.h"
#include <ArduinoOTA.h>

extern QueueHandle_t log_queue;
void setup_network_infrastructure();
void handle_network();
void z_log(const char* format, ...);
bool is_authenticated(AsyncWebServerRequest *request);

#endif
