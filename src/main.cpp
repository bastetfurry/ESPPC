/*
  Created by Fabrizio Di Vittorio (fdivitto2013@gmail.com) - <http://www.fabgl.com>
  Copyright (c) 2019-2022 Fabrizio Di Vittorio.
  All rights reserved.


* Please contact fdivitto2013@gmail.com if you need a commercial license.


* This library and related software is available under GPL v3.

  FabGL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  FabGL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with FabGL.  If not, see <http://www.gnu.org/licenses/>.
 */


 /* Instructions:

    - to run this application you need an ESP32 with PSRAM installed and an SD-CARD slot (ie TTGO VGA32 v1.4 or FabGL Development Board with WROVER)
    - build and upload with PlatformIO: `pio run -t upload`
    - PSRAM must NOT be enabled at build time (no -DBOARD_HAS_PSRAM): SPIRAM is initialized
      manually in setup() to avoid the -mfix-esp32-psram-cache-issue compiler workaround
    - the partition table (3MB app) comes from partitions.csv, see platformio.ini

 */


// The partition table is pinned by "board_build.partitions = partitions.csv" in
// platformio.ini (equivalent to the Arduino IDE's "Huge APP" scheme).


#include <Arduino.h>

#include <memory>

#include "esp32-hal-psram.h"
extern "C" {
#include "esp_spiram.h"
}
#include "esp_sntp.h"

#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "fabgl.h"

#include "mconf.h"
#include "machine.h"
#include "textui.h"
#include "setupui.h"



// UART Pins for USB serial
#define UART_URX 3
#define UART_UTX 1




using fabgl::imin;
using fabgl::imax;


// forward declarations (the Arduino preprocessor used to generate these automatically)
bool tryToConnect();
bool checkWiFi();
void shutdownHandler();
void updateDateTime();
bool downloadURL(char const * URL, FILE * file);
char const * getDisk(char const * url);
void sysReqCallback();
void setup();
void loop();



Preferences   preferences;
TextUI        ui;
Machine     * machine;


// noinit! Used to maintain datetime between reboots
__NOINIT_ATTR static timeval savedTimeValue;


static bool wifiConnected = false;

// Host settings from the Advanced Settings page, kept in Preferences. Global because SNTP keeps a
// pointer to the server name for as long as it runs.
static HostSettings hostSettings;

// The clock runs in UTC; the zone only decides how it reads locally, so it can change at any time.
static void applyTimeZone()
{
  setenv("TZ", hostSettings.timeZone[0] ? hostSettings.timeZone : HostSettings::DEFAULT_TIME_ZONE, 1);
  tzset();
}


static char const * ntpServer()
{
  return hostSettings.ntpServer[0] ? hostSettings.ntpServer : HostSettings::DEFAULT_NTP_SERVER;
}
static bool downloadOK    = false;


// try to connected using saved parameters
bool tryToConnect()
{
  bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected) {
    char SSID[32] = "";
    char psw[32]  = "";
    if (preferences.getString("SSID", SSID, sizeof(SSID)) && preferences.getString("WiFiPsw", psw, sizeof(psw))) {
      char label[48];
      snprintf(label, sizeof(label), "Wi-Fi %s", SSID);
      ui.postItem(label);
      ui.progress("Wi-Fi", true, [&](TextUI::Progress & p) {
        WiFi.begin(SSID, psw);
        for (int i = 0; i < 32 && WiFi.status() != WL_CONNECTED; ++i) {
          if (!p.update(i * 100 / 32, "Connecting to %s...", SSID))
            break;
          delay(500);
          if (i == 16)
            WiFi.reconnect();
        }
        connected = (WiFi.status() == WL_CONNECTED);
      });
      if (connected) {
        ui.postResult(WiFi.localIP().toString().c_str(), TUI_POST_OK);
      } else {
        WiFi.disconnect();
        ui.postResult("Failed", TUI_POST_FAIL);
      }
    }
  }
  return connected;
}


bool checkWiFi()
{
  wifiConnected = tryToConnect();
  if (!wifiConnected) {

    // configure WiFi?
    if (ui.message("Wi-Fi Configuration", "Configure Wi-Fi now?", "No;Yes") == 1) {

      // repeat until connected or until user cancels
      do {

        // scan for networks
        int networksCount = 0;
        ui.progress("Wi-Fi", false, [&](TextUI::Progress & p) {
          p.update(0, "Scanning Wi-Fi networks...");
          networksCount = WiFi.scanNetworks();
          p.update(100, "Scanning Wi-Fi networks...");
        });

        if (networksCount > 0) {

          // one line per network, kept alive until the menu is done with them
          char (*lines)[48] = (char (*)[48]) malloc(networksCount * 48);
          char const ** items = (char const **) malloc(networksCount * sizeof(char const *));
          for (int i = 0; i < networksCount; ++i) {
            snprintf(lines[i], 48, "%-32.32s %4d dBm", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
            items[i] = lines[i];
          }
          int s = ui.menu("Wi-Fi Configuration", "Select a network", items, networksCount);
          free(items);
          free(lines);

          if (s > -1) {
            char psw[32] = "";
            if (ui.textInput("Wi-Fi Configuration", "Password", psw, 31, true)) {
              preferences.putString("SSID", WiFi.SSID(s).c_str());
              preferences.putString("WiFiPsw", psw);
              wifiConnected = tryToConnect();
              if (wifiConnected)
                ui.message("Wi-Fi", "Connection succeeded.");
            } else
              break;
          } else
            break;
        } else {
          ui.message("Wi-Fi", "No Wi-Fi network found.");
          break;
        }
        WiFi.scanDelete();

      } while (!wifiConnected);

    }

  }

  return wifiConnected;
}


// handle soft restart
void shutdownHandler()
{
  // save current datetime into Preferences
  gettimeofday(&savedTimeValue, nullptr);
}


void updateDateTime()
{
  applyTimeZone();

  // get datetime from savedTimeValue? (noinit section)
  if (esp_reset_reason() == ESP_RST_SW) {
    // adjust time taking account elapsed time since ESP32 started
    savedTimeValue.tv_usec += (int) esp_timer_get_time();
    savedTimeValue.tv_sec  += savedTimeValue.tv_usec / 1000000;
    savedTimeValue.tv_usec %= 1000000;
    settimeofday(&savedTimeValue, nullptr);
    ui.postItem("Real Time Clock");
    ui.postResult("Kept across restart", TUI_POST_OK);
    return;
  }

  if (checkWiFi()) {

    // we need time right now
    ui.postItem("Real Time Clock");
    bool synced = false;
    ui.progress("Date and Time", true, [&](TextUI::Progress & p) {
      sntp_setoperatingmode(SNTP_OPMODE_POLL);
      sntp_setservername(0, (char*) ntpServer());
      sntp_init();
      // sntp_get_sync_status() reports COMPLETED once and then resets itself, so the answer has to
      // be kept from the read that saw it; asking again afterwards always says it failed.
      for (int i = 0; i < 12; ++i) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
          synced = true;
          break;
        }
        if (!p.update(i * 100 / 12, "Getting date and time from %s...", ntpServer()))
          break;
        delay(500);
      }
      sntp_stop();
    });
    {
      char r[80];
      snprintf(r, sizeof(r), synced ? "Synchronised with %s" : "No answer from %s", ntpServer());
      ui.postResult(r, synced ? TUI_POST_OK : TUI_POST_FAIL);
    }
    // restart either way: the Wi-Fi stack is only fully gone after a reset, and the saved time
    // carries over it (see shutdownHandler())
    ui.message(nullptr, "Date and time updated.\nRestarting...", "OK", 2);
    #ifndef FABGL_EMULATED
    esp_restart();
    #endif

  } else {

    // set default time
    auto tm = (struct tm){ .tm_sec  = 0, .tm_min  = 0, .tm_hour = 8, .tm_mday = 14, .tm_mon  = 7, .tm_year = 84 };
    auto now = (timeval){ .tv_sec = mktime(&tm) };
    settimeofday(&now, nullptr);
    ui.postItem("Real Time Clock");
    ui.postResult("Not set, using 14 Aug 1984", TUI_POST_NOTE);

  }
}


// download specified filename from URL
bool downloadURL(char const * URL, FILE * file)
{
  downloadOK = false;

  char const * filename = strrchr(URL, '/') + 1;

  ui.progress("Download", true, [&](TextUI::Progress & p) {
    p.update(0, "Preparing to download %s", filename);
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(URL);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      if (file) {
        int tlen = http.getSize();
        int len = tlen;
        auto buf = (uint8_t*) SOC_EXTRAM_DATA_LOW; // use PSRAM as buffer
        WiFiClient * stream = http.getStreamPtr();
        int dsize = 0;
        while (http.connected() && (len > 0 || len == -1)) {
          size_t size = stream->available();
          if (size) {
            int c = stream->readBytes(buf, size);
            auto wr = fwrite(buf, 1, c, file);
            if (wr != c) {
              dsize = 0;
              break;  // writing failure!
            }
            dsize += c;
            if (len > 0)
              len -= c;
            if (!p.update(tlen > 0 ? (int)((int64_t)dsize * 100 / tlen) : 0, "%s  %.2f / %.2f MB", filename, (double)dsize / 1048576.0, tlen / 1048576.0))
              break;
          }
        }
        downloadOK = (len == 0 || (len == -1 && dsize > 0));
      }
    }
    http.end();
  });

  return downloadOK;
}


// return filename if successfully downloaded or already exist
char const * getDisk(char const * url)
{
  FileBrowser fb(SD_MOUNT_PATH);

  char const * filename = nullptr;
  if (url) {
    if (strncmp("http://", url, 7) == 0 || strncmp("https://", url, 7) == 0) {
      // this is actually an URL
      filename = strrchr(url, '/') + 1;
      if (filename && !fb.exists(filename, false)) {
        // disk doesn't exist, try to download
        if (!checkWiFi())
          return nullptr;
        auto file = fb.openFile(filename, "wb");
        bool success = downloadURL(url, file);
        fclose(file);
        if (!success) {
          fb.remove(filename);
          return nullptr;
        }
      }
    } else if (url[0] == '/') {
      // absolute path, ie an image inside the SPIFFS partition: used exactly as given
      auto file = fopen(url, "rb");
      if (file) {
        fclose(file);
        filename = url;
      }
    } else {
      // this is just a file, relative to the SD card mount path
      if (fb.filePathExists(url))
        filename = url;
    }
  }
  return filename;
}


// user pressed SYSREQ (ALT + PRINTSCREEN)
// What the machine selection screen asks the caller to do next.
enum class SetupAction { Boot, Edit, New, Remove, Files, Advanced };


// last path component of a disk entry, which is what fits in the details panel
static char const * diskName(char const * path)
{
  if (!path || !path[0])
    return "None";
  char const * s = strrchr(path, '/');
  return s && s[1] ? s + 1 : path;
}


static void drawSetupFrame()
{
  ui.clear(TUI_SCREEN);
  ui.centre(0, "ESP32 PC Emulator Setup Utility", TUI_SCREEN);
  ui.centre(1, "Machine Configurations", TUI_LABEL);

  // main area: list on the left, details on the right, joined by a single line divider
  ui.frame(0, 2, 80, 18, TUI_SCREEN, true);
  for (int y = 3; y < 19; ++y)
    ui.put(47, y, 0xB3, TUI_SCREEN);
  ui.put(47, 2, 0xD1, TUI_SCREEN);
  ui.put(47, 19, 0xCF, TUI_SCREEN);

  // key help
  ui.frame(0, 20, 80, 5, TUI_SCREEN, true);
  ui.text(2, 21, "\x18\x19 PgUp PgDn : Select Machine      Enter : Boot        F4  : Browse Files", TUI_SCREEN);
  ui.text(2, 22, "F2 : Edit     F3 : New     Del : Remove     F5  : Advanced Settings", TUI_SCREEN);
}


static void drawSetupDetails(MachineConfItem * c)
{
  static char const * VIDEO[] = { "CGA / Hercules", "EGA", "Tandy 1000" };
  static char const * USB[]   = { "Off", "COM1", "COM2" };

  for (int y = 3; y < 19; ++y)
    ui.text(48, y, "", TUI_SCREEN, 31);
  if (!c)
    return;

  ui.text(49, 3, "Machine Details", TUI_LABEL);
  char const * label[] = { "Floppy A", "Floppy B", "Hard Disk 0", "Hard Disk 1" };
  for (int i = 0; i < DISKCOUNT; ++i) {
    ui.text(49, 5 + i, label[i], TUI_LABEL);
    ui.text(61, 5 + i, diskName(c->disk[i]), TUI_VALUE, 17);
  }
  ui.text(49, 10, "Boot From", TUI_LABEL);
  ui.text(61, 10, MachineConfItem::driveIndexToStr(c->bootDrive), TUI_VALUE);
  ui.text(49, 11, "Memory", TUI_LABEL);
  ui.textf(61, 11, TUI_VALUE, "%d KB", c->ramSizeKB);
  ui.text(49, 12, "Video", TUI_LABEL);
  ui.text(61, 12, (c->videoAdapter == 0 && c->composite) ? "CGA, composite" : VIDEO[c->videoAdapter % 3], TUI_VALUE);
  ui.text(49, 13, "Debug Cons.", TUI_LABEL);
  ui.text(61, 13, c->debugConsole ? "Enabled" : "Disabled", TUI_VALUE);
  ui.text(49, 14, "USB Serial", TUI_LABEL);
  ui.text(61, 14, USB[c->effectiveUsbSerial() % 3], TUI_VALUE);
  ui.text(49, 15, "Tandy Sound", TUI_LABEL);
  ui.text(61, 15, !(c->tandySound || c->videoAdapter == 2) ? "Disabled" : (c->tandyDAC ? "With DAC" : "Enabled"), TUI_VALUE);

  // a disk given as a URL is fetched on boot if the SD card does not have it yet
  for (int i = 0; i < DISKCOUNT; ++i)
    if (c->disk[i] && (!strncmp(c->disk[i], "http://", 7) || !strncmp(c->disk[i], "https://", 8))) {
      ui.text(49, 17, "Downloads missing disks", TUI_DIM);
      break;
    }
}


// The selection screen. countdown > 0 boots the highlighted machine after that many seconds unless
// a key is pressed first (the Auto Boot setting); 0 or -1 waits. Auto Boot "immediately" does not
// come here at all unless DEL was pressed, see setup().
static SetupAction selectMachine(MachineConf & mconf, int & idx, int countdown)
{
  int count = 0;
  for (auto c = mconf.getFirstItem(); c; c = c->next)
    ++count;

  drawSetupFrame();

  constexpr int LISTTOP  = 3;
  constexpr int LISTROWS = 16;
  idx = count ? fabgl::imax(0, fabgl::imin(idx, count - 1)) : 0;
  int scroll = fabgl::imax(0, idx - LISTROWS + 1);

  int64_t deadline = countdown > 0 ? esp_timer_get_time() + (int64_t) countdown * 1000000 : -1;

  while (true) {

    // list
    for (int r = 0; r < LISTROWS; ++r) {
      int i = scroll + r;
      auto c = mconf.getItem(i);
      bool sel = (i == idx) && c;
      ui.text(1, LISTTOP + r, "", sel ? TUI_HILITE : TUI_SCREEN, 46);
      if (c && i < count)
        ui.text(3, LISTTOP + r, c->desc, sel ? TUI_HILITE : TUI_LABEL, 43);
    }
    ui.put(46, LISTTOP, scroll > 0 ? 0x1E : ' ', TUI_SCREEN);
    ui.put(46, LISTTOP + LISTROWS - 1, scroll + LISTROWS < count ? 0x1F : ' ', TUI_SCREEN);
    drawSetupDetails(mconf.getItem(idx));

    // status line: countdown, or the clock
    char status[80];
    if (deadline >= 0) {
      int left = (int)((deadline - esp_timer_get_time() + 999999) / 1000000);
      snprintf(status, sizeof(status), "Booting in %d s, press any key to stop the countdown", fabgl::imax(left, 0));
    } else {
      time_t now = time(nullptr);
      strftime(status, sizeof(status), "%a %d %b %Y  %H:%M", localtime(&now));
    }
    ui.text(2, 23, status, deadline >= 0 ? TUI_LABEL : TUI_SCREEN, 76);

    TextUI::Key k;
    if (!ui.getKey(&k, 250)) {
      if (deadline >= 0 && esp_timer_get_time() >= deadline && count)
        return SetupAction::Boot;
      continue;
    }
    deadline = -1;

    switch (k.vk) {
      case VirtualKey::VK_UP:       case VirtualKey::VK_KP_UP:       idx = fabgl::imax(0, idx - 1); break;
      case VirtualKey::VK_DOWN:     case VirtualKey::VK_KP_DOWN:     idx = fabgl::imin(count - 1, idx + 1); break;
      case VirtualKey::VK_PAGEUP:   case VirtualKey::VK_KP_PAGEUP:   idx = fabgl::imax(0, idx - LISTROWS); break;
      case VirtualKey::VK_PAGEDOWN: case VirtualKey::VK_KP_PAGEDOWN: idx = fabgl::imin(count - 1, idx + LISTROWS); break;
      case VirtualKey::VK_HOME:     case VirtualKey::VK_KP_HOME:     idx = 0; break;
      case VirtualKey::VK_END:      case VirtualKey::VK_KP_END:      idx = count - 1; break;
      case VirtualKey::VK_RETURN:   case VirtualKey::VK_KP_ENTER:
        if (count)
          return SetupAction::Boot;
        break;
      case VirtualKey::VK_F2:       if (count) return SetupAction::Edit; break;
      case VirtualKey::VK_F3:       return SetupAction::New;
      case VirtualKey::VK_DELETE:   case VirtualKey::VK_KP_DELETE:
        if (count) return SetupAction::Remove;
        break;
      case VirtualKey::VK_F4:       return SetupAction::Files;
      case VirtualKey::VK_F5:       return SetupAction::Advanced;
      default:                      break;
    }
    idx = fabgl::imax(0, idx);
    if (idx < scroll)
      scroll = idx;
    else if (idx >= scroll + LISTROWS)
      scroll = idx - LISTROWS + 1;
  }
}


// Stops for good with a message on screen; used where the old code showed an InputBox without
// buttons, which never returned either.
static void fatal(char const * text)
{
  ui.message("Error", text, nullptr);
  while (true)
    delay(1000);
}


// The guest's own text screen behind the menu, so it opens like a notebook BIOS's on-screen
// display over whatever was running. Graphics modes cannot be shown on a text screen; they get a
// plain backdrop, and come back untouched when the menu closes.
static void drawGuestBackdrop()
{
  auto ga  = machine->graphicsAdapter();
  auto emu = ui.guestEmulation();

  if (emu == GraphicsAdapter::Emulation::PC_Text_80x25_16Colors && ui.guestBuffer()) {
    auto buf = ui.guestBuffer();
    for (int i = 0; i < 80 * 25; ++i)
      ui.put(i % 80, i / 80, buf[i * 2], ui.guestBlink() ? (buf[i * 2 + 1] & 0x7f) : buf[i * 2 + 1]);
  } else if (emu == GraphicsAdapter::Emulation::PC_Text_EGA_80x25_16Colors && ga->egaTextColumns() == 80) {
    // characters in plane 0 and attributes in plane 1, from the displayed page on
    auto ega  = machine->ega();
    auto base = ga->egaStartAddress();
    for (int i = 0; i < 80 * 25; ++i) {
      uint8_t attr = ega->planeByte(1, base + i);
      ui.put(i % 80, i / 80, ega->planeByte(0, base + i), ui.guestBlink() ? (attr & 0x7f) : attr);
    }
  } else
    ui.clear(TUI_POST);
}


// Mount or eject a floppy from the menu. The current image decides where the file picker opens:
// a mounted path is either absolute (ie "/spiffs/rescue.img") or relative to the SD card mount.
static void sysReqFloppy(int drive)
{
  static char const * ACTIONS[] = { "Insert Disk Image...", "Eject" };
  char title[16];
  snprintf(title, sizeof(title), "Floppy %c", 'A' + drive);

  int a = ui.menu(title, nullptr, ACTIONS, 2, 0);
  char const * cur = machine->diskFilename(drive);

  if (a == 0) {
    constexpr int MAXPATH = 256;
    char * dir  = (char*) malloc(MAXPATH);
    char * name = (char*) malloc(MAXPATH);
    char * path = (char*) malloc(MAXPATH * 2);
    if (dir && name && path) {
      strcpy(dir, SD_MOUNT_PATH);
      name[0] = 0;
      if (cur && *cur) {
        char const * sep = strrchr(cur, '/');
        if (sep) {
          if (cur[0] == '/')
            snprintf(dir, MAXPATH, "%.*s", imax((int)(sep - cur), 1), cur);
          else
            snprintf(dir, MAXPATH, "%s/%.*s", SD_MOUNT_PATH, (int)(sep - cur), cur);
          strlcpy(name, sep + 1, MAXPATH);
        } else
          strlcpy(name, cur, MAXPATH);
      }
      if (setupFileDialog(ui, "Insert Disk Image", dir, MAXPATH, name, MAXPATH, false)) {
        // the full path: images may live on the SD card or in the flash file system
        snprintf(path, MAXPATH * 2, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
        machine->setDriveImage(drive, path);
      }
    }
    free(dir);
    free(name);
    free(path);
  } else if (a == 1) {
    if (cur && *cur) {
      // nullptr closes the image and flags a media change, so the guest does not carry on with
      // the old disk's cached contents
      machine->setDriveImage(drive, nullptr);
      snprintf(title, sizeof(title), "Floppy %c", 'A' + drive);
      ui.message(title, "Disk ejected.");
    } else
      ui.message(title, "The drive is already empty.");
  }
}


// SysReq (Alt+PrintScreen). Runs in the emulator task, so the guest is frozen for as long as the
// menu is open. It borrows the machine's own video adapter and gives it back exactly as it was;
// the guest's video memory, CGA or EGA, is never touched.
void sysReqCallback()
{
  if (!ui.begin(machine->graphicsAdapter())) {
    printf("SysReq: no memory for the menu\n");
    return;
  }
  // the guest is frozen while the menu is open, but the voices would hold whatever they last
  // played, so the sound stops too
  bool soundWasPlaying = machine->pauseSound(true);
  drawGuestBackdrop();

  enum { M_FDA, M_FDB, M_MONITOR, M_RESTART_GUEST, M_RESTART_HOST, M_RETURN, M_COUNT };
  static char const * LABELS[M_COUNT] = { "Floppy A", "Floppy B", "Monitor", "Restart Guest", "Restart Host", "Return to Guest" };

  // RGBI or composite colour monitor, switchable here on the CGA machine; the row stays empty
  // on the others
  bool const canMonitor = machine->compositeCapable();

  constexpr int W = 50, H = 13;
  constexpr int X = (80 - W) / 2, Y = (25 - H) / 2;

  int sel = M_RETURN;
  bool done = false;

  while (!done) {

    ui.box(X, Y, W, H, TUI_DIALOG);
    ui.text(X + (W - 24) / 2, Y, " PCEmulator System Menu ", TUI_DIALOG_HEAD);

    for (int i = 0; i < M_COUNT; ++i) {
      uint8_t a = (i == sel) ? TUI_DIALOG_SEL : TUI_DIALOG;
      int row = Y + 2 + i + (i >= M_RESTART_GUEST ? 1 : 0);    // a gap after the drives
      ui.text(X + 2, row, "", a, W - 4);
      if (i == M_MONITOR && !canMonitor)
        continue;
      ui.text(X + 4, row, LABELS[i], a);
      if (i == M_MONITOR) {
        ui.text(X + 16, row, "[", a);
        ui.text(X + 17, row, machine->composite() ? "Composite" : "RGBI", a);
        ui.text(X + W - 4, row, "]", a);
      }
      if (i <= M_FDB) {
        char const * fn = machine->diskFilename(i);
        char const * s  = (fn && *fn) ? strrchr(fn, '/') : nullptr;
        ui.text(X + 16, row, "[", a);
        ui.text(X + 17, row, (fn && *fn) ? (s ? s + 1 : fn) : "Empty", a, W - 21);
        ui.text(X + W - 4, row, "]", a);
      }
    }
    ui.textf(X + 3, Y + H - 3, TUI_DIALOG, "Host memory free: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ui.text(X + 2, Y + H - 1, " \x18\x19:Select  Enter:OK  Esc:Return ", TUI_DIALOG_HEAD);

    TextUI::Key k;
    ui.getKey(&k);
    switch (k.vk) {
      case VirtualKey::VK_UP:   case VirtualKey::VK_KP_UP:
        sel = (sel + M_COUNT - 1) % M_COUNT;
        if (sel == M_MONITOR && !canMonitor)
          sel = (sel + M_COUNT - 1) % M_COUNT;
        break;
      case VirtualKey::VK_DOWN: case VirtualKey::VK_KP_DOWN:
        sel = (sel + 1) % M_COUNT;
        if (sel == M_MONITOR && !canMonitor)
          sel = (sel + 1) % M_COUNT;
        break;
      case VirtualKey::VK_ESCAPE:
        done = true;
        break;
      case VirtualKey::VK_RETURN: case VirtualKey::VK_KP_ENTER:
        switch (sel) {
          case M_FDA:
          case M_FDB:
            sysReqFloppy(sel);
            break;
          // takes effect when the menu closes (see below); for this session only, the machine
          // configuration keeps its own setting
          case M_MONITOR:
            machine->setComposite(!machine->composite());
            break;
          case M_RESTART_GUEST:
            if (ui.message("Restart Guest", "Restart the guest machine (Y/N)?", "No;Yes") == 1) {
              machine->trigReset();
              done = true;
            }
            break;
          case M_RESTART_HOST:
            if (ui.message("Restart Host", "Restart the whole emulator (Y/N)?", "No;Yes") == 1)
              esp_restart();
            break;
          default:
            done = true;
            break;
        }
        break;
      default:
        break;
    }
  }

  ui.end();

  // The menu had the adapter. TextUI puts the mode back, but let the CGA set itself up in full on
  // its next register write instead of relying on that alone.
  machine->invalidateCGAMode();
  if (soundWasPlaying)
    machine->pauseSound(false);
  // and apply a monitor change at once, rather than on the guest's next register write, which a
  // game that has set its mode may never make
  machine->refreshCGAGraphics();
}


void setup()
{
  Serial.begin(115200); delay(500);


  disableCore0WDT();
  delay(100); // experienced crashes without this delay!
  disableCore1WDT();

  preferences.begin("PCEmulator", false);

  // Every FabGL display controller gets a queue for drawing primitives, sized here. The emulator's
  // VGADirectController renders scanlines itself and never queues one, and FabGL's GUI, which
  // did, is gone. So the queue only costs internal RAM: keep it minimal.
  fabgl::BitmappedDisplayController::queueSize = 4;

  // uncomment to clear preferences
  //preferences.clear();

  // host settings, before anything that uses them (the clock is set during the POST below)
  if (preferences.isKey("NtpServer"))    // asking for a missing key logs an error at every boot
    preferences.getString("NtpServer", hostSettings.ntpServer, sizeof(hostSettings.ntpServer));
  if (preferences.isKey("TimeZone"))
    preferences.getString("TimeZone", hostSettings.timeZone, sizeof(hostSettings.timeZone));
  hostSettings.autoBootSeconds = preferences.isKey("AutoBoot") ? preferences.getInt("AutoBoot")
                                                               : HostSettings::DEFAULT_AUTOBOOT;
  hostSettings.driveLed        = preferences.isKey("DriveLed") ? preferences.getInt("DriveLed")
                                                               : HostSettings::DEFAULT_DRIVE_LED;

  // The machine exists from here on so the setup screens can borrow its video adapter; nothing in
  // it runs until machine->run() below, and TextUI::end() hands the adapter back unconfigured.
  machine = new Machine;

  ui.begin(machine->graphicsAdapter());
  ui.postBegin();

  // With the machine list skipped, DEL during the POST is the way into it, as on a BIOS
  if (hostSettings.autoBootSeconds == 0)
    ui.postFooter("Press DEL to enter SETUP");

  // we need PSRAM for this app, but we will handle it manually, so please DO NOT enable PSRAM on your development env
  #ifdef BOARD_HAS_PSRAM
  ui.message("Warning", "Please disable PSRAM to improve performance!");
  #endif

  // note: we use just 2MB of PSRAM so the infamous PSRAM bug should not happen. But to avoid gcc compiler hack (-mfix-esp32-psram-cache-issue)
  // we enable PSRAM at runtime, otherwise the hack slows down CPU too much (PSRAM_HACK is no more required).
  ui.postItem("PSRAM");
  if (esp_spiram_init() != ESP_OK) {
    ui.postResult("Not found", TUI_POST_FAIL);
    fatal("This app requires a board with PSRAM!");
  }

  #ifndef BOARD_HAS_PSRAM
  esp_spiram_init_cache();
  #endif
  {
    char r[32];
    snprintf(r, sizeof(r), "%u KB OK", (unsigned)(esp_spiram_get_size() / 1024));
    ui.postResult(r, TUI_POST_OK);
  }

  ui.postItem("SD Card");
  if (!FileBrowser::mountSDCard(false, SD_MOUNT_PATH, 8)) {  // @TODO: reduce to 4?
    ui.postResult("Not found", TUI_POST_FAIL);
    fatal("This app requires an SD card!");
  }
  ui.postResult("Mounted", TUI_POST_OK);

  // Mount the on-flash SPIFFS partition, which can hold a disk image (see data/ and
  // "pio run -t uploadfs"). Not fatal if it fails: it only means no flash-resident disks.
  ui.postItem("Flash File System");
  if (FileBrowser::mountSPIFFS(false, SPIFFS_MOUNT_PATH))
    ui.postResult("Mounted", TUI_POST_OK);
  else
    ui.postResult("Not present", TUI_POST_NOTE);

  // uncomment to format SD!
  //FileBrowser::format(fabgl::DriveType::SDCard, 0);

  esp_register_shutdown_handler(shutdownHandler);

  updateDateTime();

  // machine configurations
  MachineConf mconf;

  // the POST screen comes back after the setup screen, to log the disks
  uint8_t * postScreen = (uint8_t*) malloc(TextUI::SCREENBYTES);
  if (postScreen)
    ui.save(postScreen);

  int idx = preferences.getInt("dconf", 0);

  // Auto boot "immediately" skips the list unless DEL is pressed. As on a BIOS, the POST holds for
  // a moment to give the chance: the POST before it can be over in a second (after the NTP
  // restart), and its progress boxes empty the key buffer when they open, so a DEL pressed early
  // may be gone. One still queued counts as well.
  bool skipList = false;
  if (hostSettings.autoBootSeconds == 0) {
    skipList = true;
    int64_t until = esp_timer_get_time() + 2000000;
    TextUI::Key k;
    while (skipList) {
      int64_t left = until - esp_timer_get_time();
      if (!ui.getKey(&k, left > 0 ? (int)(left / 1000) : 0))
        break;
      if (k.vk == VirtualKey::VK_DELETE || k.vk == VirtualKey::VK_KP_DELETE)
        skipList = false;
    }
  }

  // the countdown only runs on the first visit, not after coming back from a setup page
  for (int countdown = hostSettings.autoBootSeconds; ; countdown = -1) {

    loadMachineConfiguration(&mconf);

    if (skipList && mconf.getItem(idx))
      break;                                   // the saved selection; if it is gone, the list shows
    skipList = false;

    SetupAction action = selectMachine(mconf, idx, countdown);
    if (action == SetupAction::Boot)
      break;

    switch (action) {
      case SetupAction::Edit:
        setupEditMachine(ui, mconf, idx);
        break;
      case SetupAction::New:
      {
        // inserted in memory only; leaving the editor without saving drops it again, because the
        // list is reloaded from the file at the top of this loop
        auto item = new MachineConfItem;
        item->setDesc("New Configuration");
        mconf.insertItem(idx, item);
        setupEditMachine(ui, mconf, idx);
        break;
      }
      case SetupAction::Remove:
      {
        char q[96];
        snprintf(q, sizeof(q), "Remove the configuration\n\"%.40s\"?", mconf.getItem(idx)->desc);
        if (ui.message("Please Confirm", q, "No;Yes") == 1) {
          mconf.deleteItem(idx);
          saveMachineConfiguration(&mconf);
        }
        break;
      }
      case SetupAction::Advanced:
        if (setupAdvancedSettings(ui, hostSettings)) {
          preferences.putString("NtpServer", hostSettings.ntpServer);
          preferences.putString("TimeZone", hostSettings.timeZone);
          preferences.putInt("AutoBoot", hostSettings.autoBootSeconds);
          preferences.putInt("DriveLed", hostSettings.driveLed);
          applyTimeZone();
        }
        break;
      case SetupAction::Files:
        setupFilesBrowser(ui, SD_MOUNT_PATH);
        break;
      default:
        break;
    }
  }

  idx = imax(idx, 0);
  preferences.putInt("dconf", idx);

  // setup selected configuration
  auto conf = mconf.getItem(idx);

  if (postScreen) {
    ui.restore(postScreen);
    free(postScreen);
  } else
    ui.postBegin();
  ui.postFooter("");                           // "Press DEL" is past its moment

  ui.postItem("Machine");
  ui.postResult(conf->desc, TUI_POST_BRIGHT);

  static char const * DRIVELABEL[DISKCOUNT] = { "Floppy A", "Floppy B", "Hard Disk 0", "Hard Disk 1" };
  char const * diskFilename[DISKCOUNT];
  downloadOK = true;
  for (int i = 0; i < DISKCOUNT && downloadOK; ++i) {
    diskFilename[i] = getDisk(conf->disk[i]);
    if (conf->disk[i] && conf->disk[i][0]) {
      ui.postItem(DRIVELABEL[i]);
      if (diskFilename[i])
        ui.postResult(diskName(diskFilename[i]), TUI_POST_OK);
      else
        ui.postResult("Not available", TUI_POST_FAIL);
    }
  }

  if (!downloadOK || (!diskFilename[0] && !diskFilename[2])) {
    // unable to get boot disks
    ui.message("Error", "Unable to get the system disks!\nRestarting...", "OK", 5);
    esp_restart();
  }

  if (wifiConnected) {
    // disk downloaded from the Internet, need to reboot to fully disable wifi
    ui.message(nullptr, "Disks downloaded.\nRestarting...", "OK", 2);
    #ifndef FABGL_EMULATED
    esp_restart();
    #endif
  }

  ui.postLine("");
  ui.postLine("Starting the machine...", TUI_POST_BRIGHT);
  delay(400);
  ui.end();

  // without WiFi it is possible to increase SD card speed
  FileBrowser::setSDCardMaxFreqKHz(SDMMC_FREQ_DEFAULT);
  FileBrowser::remountSDCard();

  machine->setBaseDirectory(SD_MOUNT_PATH);
  for (int i = 0; i < DISKCOUNT; ++i)
    machine->setDriveImage(i, diskFilename[i], conf->cylinders[i], conf->heads[i], conf->sectors[i]);

  machine->setBootDrive(conf->bootDrive);

  machine->setRamSize(conf->ramSizeKB);

  // EGA path: hand the machine a real EGA BIOS option ROM and let POST initialise it. Without
  // this the machine keeps the original CGA/Hercules adapter, which is the default.
  if (conf->videoAdapter == 1) {
    char const * rom = (conf->egaRom && conf->egaRom[0]) ? conf->egaRom
                                                         : SPIFFS_MOUNT_PATH "/ega.bin";
    printf("Video adapter: EGA, BIOS ROM \"%s\"\n", rom);
    machine->setEGABios(rom);
  } else if (conf->videoAdapter == 2) {
    printf("Video adapter: Tandy 1000, with its SN76496 sound\n");
    machine->setTandy(true);
  }
  if (conf->tandySound && conf->videoAdapter != 2) {
    printf("Sound: SN76496 at C0h\n");
    machine->setTandySound(true);
  }
  if (conf->composite && conf->videoAdapter == 0) {
    printf("Monitor: composite colour\n");
    machine->setComposite(true);
  }
  if (conf->tandyDAC && (conf->tandySound || conf->videoAdapter == 2)) {
    printf("Sound: Tandy SL/TL DAC through INT 1Ah\n");
    machine->setTandyDAC(true);
  }
  
  // COM1 uses pins 3/1, the same pins as the USB serial console: attaching it makes UART2 take
  // those pins over, so printf() output stops reaching the host from here on. The serial debug
  // console needs those pins too, so the machine gets one or the other, never both.
  int usbSerial = conf->effectiveUsbSerial();   // 0 = none, 1 = COM1, 2 = COM2

  if (conf->debugConsole) {
    // The console owns the pins, so a COM port cannot have them. It is carried in software
    // instead, which leaves the debugger usable at the same time.
    machine->enableDebugConsole();
    machine->setUsbSerialRoute(usbSerial);
    if (usbSerial)
      printf("Serial debug console enabled, carrying COM%d over the USB serial.\n", usbSerial);
    else
      printf("Serial debug console enabled for this machine, no COM port on the USB serial.\n");
  } else if (usbSerial) {
    // No console, so the port takes pins 3/1 directly. printf() output stops reaching the host
    // from here on, because UART2 has taken those pins over.
    auto serial = new SerialPort;
    serial->setSignals(UART_URX, UART_UTX);
    if (usbSerial == 2)
      machine->setCOM2(serial);
    else
      machine->setCOM1(serial);
  }

  /*
  printf("MALLOC_CAP_32BIT : %d bytes (largest %d bytes)\r\n", heap_caps_get_free_size(MALLOC_CAP_32BIT), heap_caps_get_largest_free_block(MALLOC_CAP_32BIT));
  printf("MALLOC_CAP_8BIT  : %d bytes (largest %d bytes)\r\n", heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  printf("MALLOC_CAP_DMA   : %d bytes (largest %d bytes)\r\n\n", heap_caps_get_free_size(MALLOC_CAP_DMA), heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

  heap_caps_dump_all();
  */

  machine->setSysReqCallback(sysReqCallback);

  // the on-screen drive LED, from now on: the setup screens above stay without it
  machine->graphicsAdapter()->setDriveLEDMode(hostSettings.driveLed);

  machine->run();
}



#if FABGLIB_VGAXCONTROLLER_PERFORMANCE_CHECK
namespace fabgl {
  extern volatile uint64_t s_vgapalctrlcycles;
}
using fabgl::s_vgapalctrlcycles;
#endif



void loop()
{

#if FABGLIB_VGAXCONTROLLER_PERFORMANCE_CHECK
  static uint32_t tcpu = 0, s1 = 0, count = 0;
  tcpu = machine->ticksCounter();
  s_vgapalctrlcycles = 0;
  s1 = fabgl::getCycleCount();
  delay(1000);
  printf("%d\tCPU: %d", count, machine->ticksCounter() - tcpu);
  printf("   Graph: %lld / %d   (%d%%)\n", s_vgapalctrlcycles, fabgl::getCycleCount() - s1, (int)((double)s_vgapalctrlcycles/240000000*100));
  ++count;
#else

  vTaskDelete(NULL);

#endif
}


