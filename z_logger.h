#ifndef Z_LOGGER_H
#define Z_LOGGER_H

#include <Arduino.h>
#include "z_config.h"

#define BACKLOG_SIZE 100

extern String log_backlog[BACKLOG_SIZE];
extern int backlog_index;
extern int backlog_count;
extern SemaphoreHandle_t log_mutex;

void init_log_system();
void log_to_web(uint8_t level, const char* format, ...);

#endif
