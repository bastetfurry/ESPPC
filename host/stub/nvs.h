// Host build: no non-volatile storage, every read finds nothing
#pragma once
#include <stddef.h>
#include "esp_timer.h"
typedef uint32_t nvs_handle;
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode;
inline esp_err_t nvs_open(char const *, nvs_open_mode, nvs_handle_t * h) { *h = 1; return ESP_OK; }
inline void      nvs_close(nvs_handle_t)                                  { }
inline esp_err_t nvs_get_blob(nvs_handle_t, char const *, void *, size_t *) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_set_blob(nvs_handle_t, char const *, void const *, size_t) { return ESP_OK; }
inline esp_err_t nvs_commit(nvs_handle_t)                                 { return ESP_OK; }
