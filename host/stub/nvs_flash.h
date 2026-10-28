// Host build: see nvs.h
#pragma once
#include "nvs.h"
inline esp_err_t nvs_flash_init()  { return ESP_OK; }
inline esp_err_t nvs_flash_erase() { return ESP_OK; }
