// Host build stub: just what i8086.cpp needs from FabGL to compile outside the ESP32.
#pragma once
#include <stdint.h>
#include <stdio.h>
#define IRAM_ATTR
#define DRAM_ATTR
#define PSRAM_WORKAROUND2
#define FABGL_ESP_IDF_VERSION_VAL(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define FABGL_ESP_IDF_VERSION FABGL_ESP_IDF_VERSION_VAL(4, 4, 0)
