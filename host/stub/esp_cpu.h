// Host build: a 240 MHz cycle count on the virtual clock
#pragma once
#include <stdint.h>
#include "esp_timer.h"
inline uint32_t esp_cpu_get_cycle_count() { return (uint32_t) (esp_timer_get_time() * 240); }
