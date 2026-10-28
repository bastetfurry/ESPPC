// The host platform under the emulator: the virtual clock and esp_timer, the one task, the PS/2
// keyboard queue, the PSRAM window, FabGL's fonts, and an absent serial debug console.
//
// Time is virtual: it advances with the instructions the emulator runs (hostAdvance(), called from
// the machine's step callback), so a run is the same every time and goes as fast as the PC allows.

#include <vector>

#include "host.h"
#include "fabgl.h"
#include "serialdbg.h"

#define FABGL_FONT_INCLUDE_DEFINITION
#include "fonts/font_8x8.h"
#include "fonts/font_8x14.h"
#include "fonts/font_8x16.h"


// In the binary's own data, not on the heap: the 8086 core keeps its registers' address as a 32
// bit offset from guest memory (i8086::reset()), so both have to lie within 2 GB of each other.
static uint8_t s_psram[4 * 1024 * 1024];
uint8_t * host_psram = s_psram;

static uint64_t s_timeNs = 0;

int64_t esp_timer_get_time()
{
  return (int64_t) (s_timeNs / 1000);
}


struct host_timer {
  esp_timer_cb_t callback;
  void *         arg;
  int64_t        period;    // us, 0 = one shot
  int64_t        due;       // us, -1 = stopped
};

static std::vector<host_timer *> s_timers;

esp_err_t esp_timer_init()
{
  return ESP_OK;
}

esp_err_t esp_timer_create(esp_timer_create_args_t const * args, esp_timer_handle_t * out)
{
  auto t = new host_timer { args->callback, args->arg, 0, -1 };
  s_timers.push_back(t);
  *out = t;
  return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period)
{
  t->period = period;
  t->due    = esp_timer_get_time() + period;
  return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout)
{
  t->period = 0;
  t->due    = esp_timer_get_time() + timeout;
  return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
  t->due = -1;
  return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
  for (size_t i = 0; i < s_timers.size(); ++i)
    if (s_timers[i] == t) {
      s_timers.erase(s_timers.begin() + i);
      break;
    }
  delete t;
  return ESP_OK;
}


void hostAdvance(uint64_t ns)
{
  s_timeNs += ns;
  int64_t now = esp_timer_get_time();
  for (size_t i = 0; i < s_timers.size(); ++i) {
    host_timer * t = s_timers[i];
    if (t->due >= 0 && now >= t->due) {
      t->due = t->period ? t->due + t->period : -1;
      t->callback(t->arg);
    }
  }
}


// The machine's task is the first one; host main runs it. Others (the AdLib synthesiser) are
// refused, so the machine goes on without them.
static TaskFunction_t s_taskFn  = nullptr;
static void *         s_taskArg = nullptr;

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, char const *, uint32_t, void * arg, int,
                                   TaskHandle_t * handle, int)
{
  if (s_taskFn)
    return pdFAIL;
  s_taskFn  = fn;
  s_taskArg = arg;
  if (handle)
    *handle = (TaskHandle_t) 1;
  return pdPASS;
}

void hostRunTask()
{
  if (s_taskFn)
    s_taskFn(s_taskArg);
}


namespace fabgl {

bool                 VGADirectController::s_running          = false;
char const *         VGADirectController::s_modeline         = nullptr;
int                  VGADirectController::s_viewW            = 0;
int                  VGADirectController::s_viewH            = 0;
DrawScanlineCallback VGADirectController::s_callback         = nullptr;
void *               VGADirectController::s_arg              = nullptr;
int                  VGADirectController::s_linesPerCallback = 1;

Keyboard PS2Controller::s_keyboard;
Mouse    PS2Controller::s_mouse;
bool     PS2Controller::s_initialized = false;


void Keyboard::push(int code)
{
  int next = (m_head + 1) % 1024;
  if (next != m_tail) {
    m_queue[m_head] = code;
    m_head = next;
  }
}

int Keyboard::scancodeAvailable()
{
  return (m_head - m_tail + 1024) % 1024;
}

int Keyboard::getNextScancode(int, bool)
{
  if (m_head == m_tail)
    return -1;
  int code = m_queue[m_tail];
  m_tail = (m_tail + 1) % 1024;
  return code;
}

// What a keyboard answers: FA for every byte, and after reset its self test result, after
// identify its ID. Commands that take a data byte (LEDs, typematic rate, scancode set) are
// followed by one that is only acknowledged.
void Keyboard::sendCommand(uint8_t cmd)
{
  if (m_pendingData) {
    m_pendingData = 0;
    push(0xfa);
    return;
  }
  switch (cmd) {
    case 0xff:                          // reset
      push(0xfa);
      push(0xaa);
      break;
    case 0xf2:                          // identify
      push(0xfa);
      push(0xab);
      push(0x83);
      break;
    case 0xee:                          // echo
      push(0xee);
      break;
    case 0xed:                          // LEDs
    case 0xf3:                          // typematic rate
    case 0xf0:                          // scancode set
      m_pendingData = 1;
      push(0xfa);
      break;
    default:
      push(0xfa);
      break;
  }
}

bool Keyboard::sendCommand(uint8_t cmd, uint8_t expectedReply)
{
  (void) cmd;
  (void) expectedReply;
  return true;                          // the BIOS's own commands: nothing to answer on the host
}


uint8_t Keyboard::convScancodeSet2To1(uint8_t code)
{
  // 8042 scancodes set 2 to 1 translation table (FabGL's keyboard.cpp)
  static const uint8_t S2TOS1[256] = {
    0xff, 0x43, 0x41, 0x3f, 0x3d, 0x3b, 0x3c, 0x58, 0x64, 0x44, 0x42, 0x40, 0x3e, 0x0f, 0x29, 0x59,
    0x65, 0x38, 0x2a, 0x70, 0x1d, 0x10, 0x02, 0x5a, 0x66, 0x71, 0x2c, 0x1f, 0x1e, 0x11, 0x03, 0x5b,
    0x67, 0x2e, 0x2d, 0x20, 0x12, 0x05, 0x04, 0x5c, 0x68, 0x39, 0x2f, 0x21, 0x14, 0x13, 0x06, 0x5d,
    0x69, 0x31, 0x30, 0x23, 0x22, 0x15, 0x07, 0x5e, 0x6a, 0x72, 0x32, 0x24, 0x16, 0x08, 0x09, 0x5f,
    0x6b, 0x33, 0x25, 0x17, 0x18, 0x0b, 0x0a, 0x60, 0x6c, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0c, 0x61,
    0x6d, 0x73, 0x28, 0x74, 0x1a, 0x0d, 0x62, 0x6e, 0x3a, 0x36, 0x1c, 0x1b, 0x75, 0x2b, 0x63, 0x76,
    0x55, 0x56, 0x77, 0x78, 0x79, 0x7a, 0x0e, 0x7b, 0x7c, 0x4f, 0x7d, 0x4b, 0x47, 0x7e, 0x7f, 0x6f,
    0x52, 0x53, 0x50, 0x4c, 0x4d, 0x48, 0x01, 0x45, 0x57, 0x4e, 0x51, 0x4a, 0x37, 0x49, 0x46, 0x54,
    0x80, 0x81, 0x82, 0x41, 0x54, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
  };
  return S2TOS1[code];
}

}  // namespace fabgl


// No serial debug console on the host: the script in main.cpp does its job.
SerialDebug::SerialDebug() : m_machine(nullptr) { }
void SerialDebug::begin(Machine *)          { }
void SerialDebug::poll()                    { }
void SerialDebug::breakNow()                { }
void SerialDebug::setUsbSerialRoute(int)    { }
