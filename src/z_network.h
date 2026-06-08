#ifndef Z_NETWORK_H
#define Z_NETWORK_H

#include "z_config.h"
#include "z_ui.h"
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

// Custom Allocator for ArduinoJson to use PSRAM (v6.4.9)
struct PSRAM_Allocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void* ptr) override {
        heap_caps_free(ptr);
    }
    void* reallocate(void* ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};
extern PSRAM_Allocator psram_alloc;

void setup_network_infrastructure();
void handle_network();
extern SemaphoreHandle_t api_mutex;
extern SemaphoreHandle_t ws_mutex;

// Unified logging system
void z_log(int level, const char* tag, const char* format, ...);
bool is_authenticated(AsyncWebServerRequest *request);

#endif
