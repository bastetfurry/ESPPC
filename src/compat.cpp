// The Arduino APIs ESPPC uses, on plain ESP-IDF. See compat.h.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "driver/uart.h"
#include "esp_vfs_dev.h"
#include "esp_rom_sys.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"


///////////////////////////////////////////////////////////////////////////////
// Serial

ConsoleSerial Serial;


// The UART driver with buffers, and printf() through it. The transmit buffer lets the console's
// screen mirror and printf() return before the bytes are on the wire, as far as it reaches; the
// receive buffer holds what the postbox sends in one go.
void ConsoleSerial::begin(int baud)
{
  uart_driver_install(UART_NUM_0, 1024, 2048, 0, nullptr, 0);
  uart_set_baudrate(UART_NUM_0, baud);
  esp_vfs_dev_uart_use_driver(UART_NUM_0);
}


int ConsoleSerial::available()
{
  size_t n = 0;
  uart_get_buffered_data_len(UART_NUM_0, &n);
  return n;
}


int ConsoleSerial::read()
{
  uint8_t c;
  return uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1 ? c : -1;
}


size_t ConsoleSerial::write(uint8_t const * buf, size_t len)
{
  int n = uart_write_bytes(UART_NUM_0, buf, len);
  return n > 0 ? n : 0;
}


void ConsoleSerial::flush()
{
  uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
}


void ConsoleSerial::printf(char const * format, ...)
{
  char buf[256];
  va_list ap;
  va_start(ap, format);
  int n = vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  if (n > 0)
    write((uint8_t const *) buf, n < (int) sizeof(buf) ? n : sizeof(buf) - 1);
}


///////////////////////////////////////////////////////////////////////////////
// ESP

EspClass ESP;


uint32_t EspClass::getCpuFreqMHz()
{
  return esp_rom_get_cpu_ticks_per_us();
}


///////////////////////////////////////////////////////////////////////////////
// Preferences
//
// The same NVS entry types Arduino's Preferences uses: strings as strings, ints as i32, bytes as
// blobs, so what an Arduino build saved is read back.

// as Arduino does at start: a partition that is full or from a newer NVS version is erased
static void nvsInit()
{
  static bool done = false;
  if (done)
    return;
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  done = true;
}


bool Preferences::begin(char const * name, bool readOnly)
{
  end();
  nvsInit();
  nvs_handle_t h;
  if (nvs_open(name, readOnly ? NVS_READONLY : NVS_READWRITE, &h) != ESP_OK)
    return false;
  m_handle = h;
  m_open   = true;
  return true;
}


void Preferences::end()
{
  if (m_open) {
    nvs_close(m_handle);
    m_open = false;
  }
}


bool Preferences::clear()
{
  return m_open && nvs_erase_all(m_handle) == ESP_OK && nvs_commit(m_handle) == ESP_OK;
}


bool Preferences::isKey(char const * key)
{
  return m_open && nvs_find_key(m_handle, key, nullptr) == ESP_OK;
}


size_t Preferences::getString(char const * key, char * value, size_t maxLen)
{
  size_t len = maxLen;
  if (!m_open || nvs_get_str(m_handle, key, value, &len) != ESP_OK)
    return 0;
  return len;
}


size_t Preferences::putString(char const * key, char const * value)
{
  if (!m_open || nvs_set_str(m_handle, key, value) != ESP_OK || nvs_commit(m_handle) != ESP_OK)
    return 0;
  return strlen(value);
}


int32_t Preferences::getInt(char const * key, int32_t defaultValue)
{
  int32_t v = defaultValue;
  if (m_open)
    nvs_get_i32(m_handle, key, &v);
  return v;
}


size_t Preferences::putInt(char const * key, int32_t value)
{
  if (!m_open || nvs_set_i32(m_handle, key, value) != ESP_OK || nvs_commit(m_handle) != ESP_OK)
    return 0;
  return sizeof(value);
}


size_t Preferences::getBytesLength(char const * key)
{
  size_t len = 0;
  if (!m_open || nvs_get_blob(m_handle, key, nullptr, &len) != ESP_OK)
    return 0;
  return len;
}


size_t Preferences::getBytes(char const * key, void * buf, size_t maxLen)
{
  size_t len = maxLen;
  if (!m_open || nvs_get_blob(m_handle, key, buf, &len) != ESP_OK)
    return 0;
  return len;
}


size_t Preferences::putBytes(char const * key, void const * value, size_t len)
{
  if (!m_open || nvs_set_blob(m_handle, key, value, len) != ESP_OK || nvs_commit(m_handle) != ESP_OK)
    return 0;
  return len;
}


///////////////////////////////////////////////////////////////////////////////
// WiFi
//
// The station only. As Arduino's WiFi does, a lost connection is retried on its own until
// disconnect() is called.

WiFiClass WiFi;

static bool s_wantConnected = false;


static void wifiEventHandler(void * arg, esp_event_base_t base, int32_t id, void * data)
{
  ((WiFiClass *) arg)->event(base, id, data);
}


void WiFiClass::event(char const * base, int32_t id, void * data)
{
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    m_gotIP = false;
    if (m_onDisconnect)
      m_onDisconnect(((wifi_event_sta_disconnected_t *) data)->reason);
    if (s_wantConnected)
      esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    m_ip    = ((ip_event_got_ip_t *) data)->ip_info.ip.addr;
    m_gotIP = true;
  }
}


void WiFiClass::init()
{
  if (m_init)
    return;
  nvsInit();
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, this);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEventHandler, this);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  m_init = true;
}


void WiFiClass::begin(char const * ssid, char const * password)
{
  init();
  wifi_config_t c = { };
  strncpy((char *) c.sta.ssid, ssid, sizeof(c.sta.ssid));
  strncpy((char *) c.sta.password, password, sizeof(c.sta.password));
  esp_wifi_set_config(WIFI_IF_STA, &c);
  s_wantConnected = true;
  esp_wifi_connect();
}


int WiFiClass::status()
{
  return m_gotIP ? WL_CONNECTED : WL_DISCONNECTED;
}


bool WiFiClass::reconnect()
{
  if (!m_init)
    return false;
  s_wantConnected = true;
  esp_wifi_disconnect();
  return esp_wifi_connect() == ESP_OK;
}


bool WiFiClass::disconnect()
{
  if (!m_init)
    return false;
  s_wantConnected = false;
  m_gotIP = false;
  return esp_wifi_disconnect() == ESP_OK;
}


void WiFiClass::setSleep(bool enabled)
{
  if (m_init)
    esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}


void WiFiClass::setTxPower(int quarterDBm)
{
  if (m_init)
    esp_wifi_set_max_tx_power(quarterDBm);
}


std::string WiFiClass::localIP()
{
  char s[16];
  snprintf(s, sizeof(s), "%u.%u.%u.%u", (unsigned) (m_ip & 0xff), (unsigned) ((m_ip >> 8) & 0xff),
           (unsigned) ((m_ip >> 16) & 0xff), (unsigned) (m_ip >> 24));
  return s;
}


int WiFiClass::RSSI()
{
  wifi_ap_record_t ap;
  return (m_init && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}


int WiFiClass::scanNetworks()
{
  init();
  scanDelete();
  wifi_scan_config_t cfg = { };
  if (esp_wifi_scan_start(&cfg, true) != ESP_OK)
    return -1;
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n == 0)
    return 0;
  auto recs = (wifi_ap_record_t *) calloc(n, sizeof(wifi_ap_record_t));
  if (!recs)
    return -1;
  esp_wifi_scan_get_ap_records(&n, recs);
  m_scan      = recs;
  m_scanCount = n;
  return n;
}


std::string WiFiClass::SSID(int i)
{
  if (i < 0 || i >= m_scanCount)
    return "";
  return (char const *) ((wifi_ap_record_t *) m_scan)[i].ssid;
}


int WiFiClass::RSSI(int i)
{
  return (i < 0 || i >= m_scanCount) ? 0 : ((wifi_ap_record_t *) m_scan)[i].rssi;
}


void WiFiClass::scanDelete()
{
  free(m_scan);
  m_scan      = nullptr;
  m_scanCount = 0;
}
