#ifndef Z_NETWORK_H
#define Z_NETWORK_H

#include "z_config.h"
#include "z_ui.h"
#include <ArduinoOTA.h>

void setup_network_infrastructure();
void handle_network();
extern SemaphoreHandle_t api_mutex;
// Unified logging system
void z_log(int level, const char* tag, const char* format, ...);
bool is_authenticated(AsyncWebServerRequest *request);

#endif
