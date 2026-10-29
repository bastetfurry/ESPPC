/*
  Download throughput test - not the emulator.

      pio run -e dltest -t upload
      <read the serial console at 115200>

  Joins the Wi-Fi stored by the emulator (Preferences "ESPPC", SSID / WiFiPsw) and reads a few
  MB from each URL below through HttpRequest (httpopen.h), as the emulator does, throwing the data
  away, so the network and TLS can be measured apart from the SD card and the VGA output. Edit the
  URLs for your network. Still an Arduino program (see platformio.ini).
*/

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_timer.h"
#include "httpopen.h"


static char const * const URLS[] = {
  "https://lionscade.de/pcimages/SvarDOS.img",
  "http://192.168.178.138:8765/test16.bin",
};
static constexpr int AMOUNT = 4 * 1024 * 1024;


// reads AMOUNT bytes through HttpRequest, as the emulator's downloads do, and throws them away
static void measure(char const * url)
{
  HttpRequest http;
  int code = http.open(url);
  int64_t t1 = esp_timer_get_time();
  if (code != 200) {
    printf("%s: %d %s\n", url, code, code < 0 ? HttpRequest::errorText(code) : "");
    return;
  }
  static uint8_t buf[4096];
  int got = 0, timeouts = 0;
  while (got < AMOUNT) {
    int n = http.read(buf, sizeof(buf));
    if (n == HttpRequest::READ_TIMEOUT && ++timeouts < 10)
      continue;
    if (n <= 0)
      break;
    got += n;
  }
  int64_t us = esp_timer_get_time() - t1;
  printf("%-42s %4d KB/s, %d timeouts\n", url, (int) ((int64_t) got * 1000000 / us / 1024), timeouts);
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
  for (auto url : URLS)
    measure(url);
  printf("done\n");
}


void loop()
{
  delay(1000);
}
