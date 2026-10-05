/*
  Download throughput test - not the emulator.

      pio run -e dltest -t upload
      <read the serial console at 115200>

  Joins the Wi-Fi stored by the emulator (Preferences "ESPPC", SSID / WiFiPsw) and reads a few
  MB from each URL below, throwing the data away, so the network and TLS can be measured apart
  from the SD card and the VGA output. Edit the URLs for your network.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_timer.h"
#include "httpopen.h"
#include "lwip/sockets.h"


static char const * const URLS[] = {
  "https://lionscade.de/pcimages/SvarDOS.img",
  "http://192.168.178.138:8765/test16.bin",
};
static constexpr int AMOUNT = 4 * 1024 * 1024;


// mode 0: available() + delay(1), as the download does; 1: available() + taskYIELD();
// 2: the plain socket, blocking recv() (http only, after what WiFiClient already buffered)
static void measure(char const * url, int mode)
{
  HTTPClient http;
  int64_t t0 = esp_timer_get_time();
  int code = httpOpen(http, url);
  int64_t t1 = esp_timer_get_time();
  if (code != HTTP_CODE_OK) {
    printf("%s: %d\n", url, code);
    http.end();
    return;
  }
  static uint8_t buf[16384];
  WiFiClient * s = http.getStreamPtr();
  int got = 0;
  int64_t idle = 0;
  if (mode == 2) {
    int a;
    while ((a = s->available()) > 0)
      got += s->read(buf, a < (int) sizeof(buf) ? a : sizeof(buf));
    int fd = s->fd();
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (got < AMOUNT) {
      int n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      got += n;
    }
  } else {
    while (got < AMOUNT && http.connected()) {
      int a = s->available();
      if (a > 0)
        got += s->read(buf, a < (int) sizeof(buf) ? a : sizeof(buf));
      else {
        int64_t t = esp_timer_get_time();
        if (mode == 0)
          delay(1);
        else
          taskYIELD();
        idle += esp_timer_get_time() - t;
      }
    }
  }
  int64_t us = esp_timer_get_time() - t1;
  http.end();
  printf("%-42s mode %d: %4d KB/s, idle %d%%\n", url, mode, (int) ((int64_t) got * 1000000 / us / 1024), (int) (idle * 100 / us));
}


void setup()
{
  Serial.begin(115200);
  delay(500);
  Preferences prefs;
  prefs.begin("ESPPC", true);
  char ssid[32] = "", psw[32] = "";
  prefs.getString("SSID", ssid, sizeof(ssid));
  prefs.getString("WiFiPsw", psw, sizeof(psw));
  WiFi.begin(ssid, psw);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i)
    delay(500);
  if (WiFi.status() != WL_CONNECTED) {
    printf("no Wi-Fi\n");
    return;
  }
  printf("\nWi-Fi up, RSSI %d dBm\n", WiFi.RSSI());
  WiFi.setSleep(false);
  for (int mode = 0; mode < 3; ++mode)
    measure(URLS[1], mode);
  for (int mode = 0; mode < 2; ++mode)
    measure(URLS[0], mode);
  printf("done\n");
}


void loop()
{
  delay(1000);
}
