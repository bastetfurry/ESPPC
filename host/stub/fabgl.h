// Host build stand-in for FabGL: the types and devices the emulator uses, without the ESP32.
//
// The video controller only records the scanline callback, so the host can draw frames with the
// real renderers (host.h). The keyboard is a queue of set 2 scancodes the host script fills, the
// mouse is absent, serial ports and sound do nothing. Time is the host's virtual clock.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc.h"
#include "fabglconf.h"

#define IRAM_ATTR
#define EMU_IRAM
#define DRAM_ATTR
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))
#define PSRAM_WORKAROUND1
#define PSRAM_WORKAROUND2
#define FABGL_ESP_IDF_VERSION_VAL(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define FABGL_ESP_IDF_VERSION FABGL_ESP_IDF_VERSION_VAL(4, 4, 0)


namespace fabgl {


template <typename T> const T & tmax(const T & a, const T & b) { return (a < b) ? b : a; }
template <typename T> const T & tmin(const T & a, const T & b) { return !(b < a) ? a : b; }
template <typename T> const T & tclamp(const T & v, const T & lo, const T & hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr auto imax   = tmax<int>;
constexpr auto imin   = tmin<int>;
constexpr auto iclamp = tclamp<int>;


struct FontInfo {
  uint8_t  pointSize;
  uint8_t  width;
  uint8_t  height;
  uint8_t  ascent;
  uint8_t  inleading;
  uint8_t  exleading;
  uint8_t  flags;
  uint16_t weight;
  uint16_t charset;
  uint8_t const *  data;
  uint32_t const * chptr;
  uint16_t codepage;
};

extern const FontInfo FONT_8x8;
extern const FontInfo FONT_8x14;
extern const FontInfo FONT_8x16;


struct RGB888 {
  uint8_t R, G, B;
  RGB888() : R(0), G(0), B(0) { }
  RGB888(uint8_t red, uint8_t green, uint8_t blue) : R(red), G(green), B(blue) { }
} __attribute__ ((packed));

struct RGB222 {
  uint8_t R : 2;
  uint8_t G : 2;
  uint8_t B : 2;
  RGB222() : R(0), G(0), B(0) { }
  RGB222(uint8_t red, uint8_t green, uint8_t blue) : R(red), G(green), B(blue) { }
};

inline bool operator==(RGB222 const & a, RGB222 const & b) { return a.R == b.R && a.G == b.G && a.B == b.B; }
inline bool operator!=(RGB222 const & a, RGB222 const & b) { return !(a == b); }


// FabGL's free running 23 bit FRC1 counter, at 80 MHz / 16
constexpr int FRC1TimerMax = 8388607;
#define FRC_TIMER_PRESCALER_16 16
inline void FRC1Timer_init(int) { }
inline uint32_t FRC1Timer()     { return (uint32_t) (esp_timer_get_time() * 5) & 0x7fffff; }


typedef void (*DrawScanlineCallback)(void * arg, uint8_t * dest, int scanLine);

// Records what the adapter asks for. A raw pixel is RGB222 in bits 5-0; the scanline buffer keeps
// FabGL's byte order, pixel x in byte x ^ 2 (see host.cpp, which undoes it).
class VGADirectController {
public:
  VGADirectController(bool = false) { }
  void begin()                                                   { }
  void end()                                                     { s_running = false; }
  void run()                                                     { s_running = true; }
  void setResolution(char const * modeline, int w, int h)        { s_modeline = modeline; s_viewW = w; s_viewH = h; }
  int getViewPortWidth()                                         { return s_viewW; }
  int getViewPortHeight()                                        { return s_viewH; }
  void setDrawScanlineCallback(DrawScanlineCallback cb, void * arg) { s_callback = cb; s_arg = arg; }
  void setScanlinesPerCallBack(int n)                            { s_linesPerCallback = n; }
  uint8_t createRawPixel(RGB222 c)                               { return c.R | (c.G << 2) | (c.B << 4); }
  void setBorderColor(RGB222 c)                                  { s_border = c; }
  static bool VSync()                                            { return false; }

  static bool                 s_running;
  static char const *         s_modeline;
  static int                  s_viewW, s_viewH;
  static DrawScanlineCallback s_callback;
  static void *               s_arg;
  static int                  s_linesPerCallback;
  static RGB222               s_border;
};


enum class KbdMode   { NoVirtualKeys, CreateVirtualKeys, CreateVirtualKeysQueue };
enum class PS2Preset { KeyboardPort0_MousePort1, KeyboardPort0, MousePort0 };

struct MousePacket {
  uint8_t data[4];
};

// Set 2 scancodes from host.cpp's script; commands to the keyboard are acknowledged the way a
// real one answers them.
class Keyboard {
public:
  int  scancodeAvailable();
  int  getNextScancode(int timeOutMS = -1, bool requestResendOnTimeOut = false);
  void sendCommand(uint8_t cmd);
  bool sendCommand(uint8_t cmd, uint8_t expectedReply);
  bool parityError()                                 { return false; }
  bool isKeyboardAvailable()                         { return true; }
  void suspendPort()                                 { }
  void resumePort()                                  { }
  void enableVirtualKeys(bool, bool)                 { }
  bool setLEDs(bool numLock, bool capsLock, bool scrollLock) { m_leds = numLock | (capsLock << 1) | (scrollLock << 2); return true; }
  void getLEDs(bool * numLock, bool * capsLock, bool * scrollLock) { *numLock = m_leds & 1; *capsLock = m_leds & 2; *scrollLock = m_leds & 4; }
  static uint8_t convScancodeSet2To1(uint8_t code);

  void push(int code);                               // host side: a scancode as if typed
private:
  int     m_queue[1024];
  int     m_head = 0, m_tail = 0;
  int     m_pendingData = 0;                         // command bytes still expecting a data byte
  uint8_t m_leds = 0;
};

class Mouse {
public:
  static void quickCheckHardware()                   { }
  bool isMouseAvailable()                            { return false; }
  bool packetAvailable()                             { return false; }
  bool getNextPacket(MousePacket *, int = -1, bool = false) { return false; }
  int  getPacketSize()                               { return 3; }
  void sendCommand(uint8_t)                          { }
  bool setSampleRate(int)                            { return false; }
  bool setResolution(int)                            { return false; }
  bool setScaling(int)                               { return false; }
  uint16_t deviceID()                                { return 0; }
  void suspendPort()                                 { }
  void resumePort()                                  { }
};

class PS2Controller {
public:
  static void begin(PS2Preset = PS2Preset::KeyboardPort0_MousePort1, KbdMode = KbdMode::CreateVirtualKeysQueue) { s_initialized = true; }
  static bool initialized()                          { return s_initialized; }
  static Keyboard * keyboard()                       { return &s_keyboard; }
  static Mouse * mouse()                             { return &s_mouse; }
  static Keyboard s_keyboard;
  static Mouse    s_mouse;
  static bool     s_initialized;
};


enum class FlowControl { None, Software, Hardware, Hardsoft };

class SerialPort {
public:
  typedef void (*RXCallback)(void * args, uint8_t value, bool fromISR);
  typedef bool (*RXReadyCallback)(void * args, bool fromISR);
  typedef void (*LineStatusCallback)(void * args, int status, bool fromISR);
  void setCallbacks(void *, RXReadyCallback, RXCallback, LineStatusCallback = nullptr) { }
  void setup(int, uint32_t, int, char, float, FlowControl, bool = false) { }
  void setBaud(int)                                  { }
  void setFrame(int, char, float)                    { }
  bool send(uint8_t)                                 { return true; }
  void sendBreak(bool)                               { }
  void setDTRStatus(bool)                            { }
  void setRTSStatus(bool)                            { }
  bool CTSStatus()                                   { return false; }
  bool DSRStatus()                                   { return false; }
  bool DCDStatus()                                   { return false; }
  bool RIStatus()                                    { return false; }
  bool breakDetected()                               { return false; }
  bool framingError()                                { return false; }
  bool parityError()                                 { return false; }
  bool overflowError()                               { return false; }
};


class WaveformGenerator {
public:
  WaveformGenerator() : next(nullptr), m_sampleRate(16384), m_volume(100), m_enabled(false) { }
  virtual ~WaveformGenerator() { }
  virtual void setFrequency(int value) = 0;
  virtual int getSample() = 0;
  void setVolume(int value)                          { m_volume = value; }
  int volume()                                       { return m_volume; }
  void enable(bool value)                            { m_enabled = value; }
  bool enabled()                                     { return m_enabled; }
  void setSampleRate(int value)                      { m_sampleRate = value; }
  uint16_t sampleRate()                              { return m_sampleRate; }
  WaveformGenerator * next;
private:
  uint16_t m_sampleRate;
  int8_t   m_volume;
  bool     m_enabled;
};

class SineWaveformGenerator : public WaveformGenerator {
public:
  void setFrequency(int) override                    { }
  int getSample() override                           { return 0; }
};

class SquareWaveformGenerator : public WaveformGenerator {
public:
  void setFrequency(int) override                    { }
  void setDutyCycle(int)                             { }
  int getSample() override                           { return 0; }
};

class NoiseWaveformGenerator : public WaveformGenerator {
public:
  void setFrequency(int) override                    { }
  int getSample() override                           { return 0; }
};

class SoundGenerator {
public:
  SoundGenerator(int = 16384) { }
  bool play(bool value)                              { bool r = m_playing; m_playing = value; return r; }
  void attach(WaveformGenerator *)                   { }
  void detach(WaveformGenerator *)                   { }
  void setVolume(int)                                { }
  int volume()                                       { return 127; }
private:
  bool m_playing = false;
};


// Disk images relative to a base directory, as on the SD card
class FileBrowser {
public:
  FileBrowser(char const * dir) : m_dir(dir) { }
  FILE * openFile(char const * filename, char const * mode) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", m_dir ? m_dir : ".", filename);
    return fopen(path, mode);
  }
private:
  char const * m_dir;
};


struct CoreUsage {
  static int busiestCore()                           { return 1; }
  static int quietCore()                             { return 0; }
  static void setBusiestCore(int)                    { }
};


}  // namespace fabgl


// what FabGL's own headers make visible unqualified
using fabgl::SoundGenerator;
using fabgl::SineWaveformGenerator;
using fabgl::SquareWaveformGenerator;
using fabgl::NoiseWaveformGenerator;
using fabgl::WaveformGenerator;
using fabgl::FileBrowser;
using fabgl::CoreUsage;
using fabgl::RGB222;
using fabgl::RGB888;
