// Host build: FreeRTOS as far as the emulator uses it. The first task created is the machine's,
// which host.cpp then runs on the main thread; any later one (the AdLib synthesiser) is refused.
#pragma once
#include <stdint.h>
typedef void * TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdPASS          1
#define pdFAIL          0
#define pdTRUE          1
#define pdFALSE         0
#define portMAX_DELAY   0xffffffff
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) (ms)
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, char const * name, uint32_t stack, void * arg,
                                   int priority, TaskHandle_t * handle, int core);
typedef void * SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex()          { return (SemaphoreHandle_t) 1; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t)       { return pdTRUE; }
inline void vSemaphoreDelete(SemaphoreHandle_t)           { }
inline void vTaskDelay(TickType_t)   { }
inline void vTaskDelete(TaskHandle_t) { }
inline int  xPortGetCoreID()         { return 1; }
inline void delay(uint32_t)          { }
inline void taskEmuCheck()           { }    // FabGL's PC build hook; the host uses the step callback
