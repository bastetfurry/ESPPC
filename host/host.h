// The host platform's own entry points (host.cpp), used by the host tool (main.cpp)

#pragma once

#include <stdint.h>

// moves the virtual clock on and fires the esp_timer timers that came due
void hostAdvance(uint64_t ns);

// runs the machine's task, which Machine::run() created; does not return
void hostRunTask();
