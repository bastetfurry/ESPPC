// The few Arduino APIs ESPPC uses, on plain ESP-IDF (no Arduino core in the firmware): delay and
// millis, the serial console on UART0, ESP.getCpuFreqMHz(), Preferences on NVS and a small WiFi.
// Only what the emulator's setup and console need, with the same behaviour where it matters
// (Preferences keeps Arduino's NVS entry types, so the saved settings carry over).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"


// --- time ---

inline uint32_t millis()                   { return (uint32_t)(esp_timer_get_time() / 1000); }
inline void     delay(uint32_t ms)         { vTaskDelay(pdMS_TO_TICKS(ms)); }
inline void     yield()                    { taskYIELD(); }


// --- Serial: the console on UART0 ---

class ConsoleSerial {
public:
  void   begin(int baud);
  int    available();
  int    read();
  size_t write(uint8_t c)                       { return write(&c, 1); }
  size_t write(uint8_t const * buf, size_t len);
  void   flush();
  void   printf(char const * format, ...) __attribute__((format(printf, 2, 3)));
};

extern ConsoleSerial Serial;


// --- ESP ---

struct EspClass {
  uint32_t getCpuFreqMHz();
};

extern EspClass ESP;


// --- Preferences: a namespace in NVS ---

class Preferences {
public:
  ~Preferences()                                { end(); }
  bool   begin(char const * name, bool readOnly = false);
  void   end();
  bool   clear();
  bool   isKey(char const * key);
  size_t getString(char const * key, char * value, size_t maxLen);
  size_t putString(char const * key, char const * value);
  int32_t getInt(char const * key, int32_t defaultValue = 0);
  size_t putInt(char const * key, int32_t value);
  size_t getBytesLength(char const * key);
  size_t getBytes(char const * key, void * buf, size_t maxLen);
  size_t putBytes(char const * key, void const * value, size_t len);
private:
  uint32_t m_handle = 0;
  bool     m_open   = false;
};


// --- WiFi: the station, a scan, and the state of the connection ---

enum { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_CONNECTED = 3, WL_CONNECT_FAILED = 4, WL_DISCONNECTED = 6 };

// transmit power in 0.25 dBm, as esp_wifi_set_max_tx_power() takes it
enum { WIFI_POWER_8_5dBm = 34 };

class WiFiClass {
public:
  void        begin(char const * ssid, char const * password);
  int         status();
  bool        reconnect();
  bool        disconnect();
  void        setSleep(bool enabled);
  void        setTxPower(int quarterDBm);
  std::string localIP();
  int         RSSI();                           // of the access point we are connected to
  int         scanNetworks();                   // blocks until done; the count, or -1
  std::string SSID(int i);                      // of a scanned network
  int         RSSI(int i);
  void        scanDelete();
  void        onDisconnect(void (*callback)(int reason))   { m_onDisconnect = callback; }

  // internal
  void        event(char const * base, int32_t id, void * data);
private:
  void        init();
  bool        m_init = false;
  volatile bool m_gotIP = false;
  uint32_t    m_ip = 0;
  void      * m_scan = nullptr;                 // wifi_ap_record_t[m_scanCount]
  int         m_scanCount = 0;
  void     (* m_onDisconnect)(int) = nullptr;
};

extern WiFiClass WiFi;
