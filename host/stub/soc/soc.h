// Host build: the PSRAM window is a 4 MB host buffer (host.cpp), split per core as on the ESP32
#pragma once
#include <stdint.h>
extern uint8_t * host_psram;
#define SOC_EXTRAM_DATA_LOW ((uintptr_t) host_psram)
