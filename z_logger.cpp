#include "z_logger.h"
#include <stdarg.h>

String log_backlog[BACKLOG_SIZE];
int backlog_index = 0;
int backlog_count = 0;
SemaphoreHandle_t log_mutex = NULL;

void init_log_system() { 
    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex(); 
    }
}

void log_to_web(uint8_t level, const char* format, ...) {
    // 1. Protection contre accès NULL (Cause probable du crash 0x30)
    if (log_mutex == NULL) {
        // Fallback sur Serial uniquement si mutex non prêt
        char fmsg[128]; va_list args; va_start(args, format);
        vsnprintf(fmsg, sizeof(fmsg), format, args); va_end(args);
        Serial.printf("[INIT] %s\n", fmsg);
        return;
    }

    if (level > sysCfg.log_level) return;
    
    char fmsg[256];
    va_list args; va_start(args, format);
    vsnprintf(fmsg, sizeof(fmsg), format, args);
    va_end(args);
    
    String lvl = (level == 0) ? "ERR" : (level == 1) ? "WRN" : (level == 3) ? "DBG" : "INF";
    String line = "[" + lvl + "] " + String(fmsg);
    
    Serial.println(line);
    
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        log_backlog[backlog_index] = line;
        backlog_index = (backlog_index + 1) % BACKLOG_SIZE;
        if (backlog_count < BACKLOG_SIZE) backlog_count++;
        xSemaphoreGive(log_mutex);
    }

    // Protection WebSocket
    if (ws.count() > 0) {
        ws.textAll(line);
    }
}
