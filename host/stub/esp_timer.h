// Host build: esp_timer on the host's virtual clock (host.cpp). Periodic timers fire from the
// emulator loop, between instruction batches, when the virtual time passes their deadline.
#pragma once
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK                         0
#define ESP_FAIL                       -1
#define ESP_ERR_NVS_NO_FREE_PAGES      0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND  0x1110
#define ESP_ERR_NVS_NOT_FOUND          0x1102

typedef struct host_timer * esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void * arg);
typedef enum { ESP_TIMER_TASK } esp_timer_dispatch_t;
typedef struct {
  esp_timer_cb_t       callback;
  void *               arg;
  esp_timer_dispatch_t dispatch_method;
  char const *         name;
  bool                 skip_unhandled_events;
} esp_timer_create_args_t;

int64_t   esp_timer_get_time();
esp_err_t esp_timer_init();
esp_err_t esp_timer_create(esp_timer_create_args_t const * args, esp_timer_handle_t * out);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
