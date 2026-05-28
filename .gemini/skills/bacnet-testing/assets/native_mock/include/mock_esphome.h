#pragma once

#include <vector>
#include <string>
#include <iostream>
#include <cstdint>

#define ESPHOME_LOG_LEVEL_NONE 0
#define ESPHOME_LOG_LEVEL_WARN 1
#define ESPHOME_LOG_LEVEL_INFO 2
#define ESPHOME_LOG_LEVEL_DEBUG 3

#define ESP_LOGI(tag, format, ...) printf("[INFO] %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) printf("[WARN] %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, format, ...) printf("[ERROR] %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) printf("[DEBUG] %s: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGCONFIG(tag, format, ...) printf("[CONFIG] %s: " format "\n", tag, ##__VA_ARGS__)

#define xSemaphoreCreateMutex() (void*)1
#define xSemaphoreTake(m, d)
#define xSemaphoreGive(m)
#define portMAX_DELAY 0
#define millis() 0
#define yield()

typedef void* SemaphoreHandle_t;
typedef void* BaseType_t;
#define pdPASS 1
#define pdMS_TO_TICKS(x) x

namespace esphome {

class Component {
public:
    virtual void setup() {}
    virtual void loop() {}
    virtual void dump_config() {}
    void mark_failed() {}
};

class PollingComponent : public Component {};

namespace uart {
class UARTDevice {
public:
    void write_array(const uint8_t *data, size_t len) {}
    void flush() {}
    bool available() { return false; }
    uint8_t read() { return 0; }
    bool read_array(uint8_t *data, size_t len) { return false; }
};
class UARTComponent {};
}

namespace output {
class BinaryOutput {
public:
    void turn_on() {}
    void turn_off() {}
};
}

namespace udp {
class UDPComponent {
public:
    template<typename F> void add_listener(F f) {}
};
}

namespace sensor {
class Sensor {
public:
    void publish_state(float value) {}
    std::string get_name() const { return "mock_sensor"; }
};
}

namespace select {
class Select {
public:
    virtual void control(const std::string &value) {}
};
}

template<typename... Ts> class Action {
public:
    virtual void play(Ts... x) = 0;
};
template<typename T, typename... Ts> class TemplatableValue {
public:
    T value(Ts... x) { return val_; }
    void set_value(T val) { val_ = val; }
private:
    T val_;
};
#define TEMPLATABLE_VALUE(type, name) TemplatableValue<type, Ts...> name##_; void set_##name(type val) { name##_.set_value(val); }

} // esphome
