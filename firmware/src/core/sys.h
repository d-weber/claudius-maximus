#pragma once
// The handful of Arduino conveniences this code base grew up with, on plain ESP-IDF: millisecond/microsecond
// clocks from esp_timer (the same source Arduino's millis()/micros() read), delay() as a FreeRTOS delay (1 ms
// tick, see sdkconfig.defaults), min/max from <algorithm>. Everything else is the IDF's own API.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>
#include <esp_timer.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline uint32_t micros() { return (uint32_t)esp_timer_get_time(); }
static inline void delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

using std::max;
using std::min;
