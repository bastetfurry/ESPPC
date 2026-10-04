/*
  PCEmulator - "Add from Repository" on the machine selection screen (F6)

  Fetches a repository's files.list (see repolist.h), shows its machines and adds the chosen ones
  to mconfs.txt. The images themselves are not downloaded here: a new machine stores the image
  URL, and getDisk() fetches it on its first boot, as for any machine with a URL.
*/


#include <Arduino.h>
#include <HTTPClient.h>
#include <sys/stat.h>

#include "repository.h"
#include "repolist.h"
#include "httpopen.h"


using fabgl::VirtualKey;
using fabgl::imin;
using fabgl::imax;


bool checkWiFi();


// files.list is a few lines of text; anything longer is not one
static constexpr int MAXLISTBYTES = 8192;


// GETs the list into buf. Returns the number of bytes, or -1 with the reason in error.
static int fetchList(TextUI & ui, char const * listURL, char * buf, int bufSize, char * error, int errorSize)
{
  int got = -1;
  snprintf(error, errorSize, "Cancelled.");

  ui.progress("Repository", true, [&](TextUI::Progress & p) {
    p.update(0, "Fetching %.60s", listURL);
    HTTPClient http;
    int code = httpOpen(http, listURL);
    if (code != HTTP_CODE_OK) {
      if (code < 0)
        snprintf(error, errorSize, "No connection to the server:\n%s", http.errorToString(code).c_str());
      else
        snprintf(error, errorSize, "The server answered %d.\nIs there a files.list at that URL?", code);
      http.end();
      return;
    }

    WiFiClient * stream = http.getStreamPtr();
    int len = http.getSize();       // -1 without Content-Length: read until the server closes
    int n = 0;
    bool aborted = false;
    uint32_t idle = millis();
    while (stream && n < bufSize && (len < 0 || n < len)) {
      int avail = stream->available();
      if (avail > 0) {
        n += stream->readBytes(buf + n, imin(avail, bufSize - n));
        idle = millis();
      } else if (!http.connected() || millis() - idle > 10000)
        break;
      else
        delay(5);
      if (!p.update(len > 0 ? n * 100 / len : 0, "Fetching %.60s", listURL)) {
        aborted = true;
        break;
      }
    }
    http.end();

    if (aborted)
      return;
    if (n >= bufSize) {
      snprintf(error, errorSize, "The list is longer than %d bytes,\nthat is not a files.list.", bufSize);
      return;
    }
    if (len > 0 && n < len) {
      snprintf(error, errorSize, "The connection dropped after %d of %d bytes.", n, len);
      return;
    }
    got = n;
  });

  return got;
}


// The image's size, from the SD card if it is there already (getDisk() would use that file), else
// from an HTTP HEAD. -1 if neither says.
static long long imageSize(TextUI & ui, char const * url)
{
  char path[300];
  snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_PATH, repoImageName(url));
  struct stat st;
  if (stat(path, &st) == 0)
    return st.st_size;

  long long size = -1;
  ui.progress("Repository", true, [&](TextUI::Progress & p) {
    p.update(0, "Asking for the size of %.50s", repoImageName(url));
    HTTPClient http;
    if (httpOpen(http, url, "HEAD") == HTTP_CODE_OK)
      size = http.getSize();
    http.end();
  });
  return size;
}


// Adds entry e to mconf as a new machine at the end of the list. Returns its index, or -1.
static int addMachine(TextUI & ui, MachineConf & mconf, RepoEntry const & e)
{
  if (!e.valid()) {
    char m[120];
    snprintf(m, sizeof(m), "This line of the list cannot be used:\n%s", e.error);
    ui.message(e.name[0] ? e.name : "Repository", m);
    return -1;
  }

  // the same image already in a machine, or another image that would land in the same file:
  // getDisk() saves a download under its base name in the SD card root and reuses what is there
  char const * name = repoImageName(e.url);
  int count = 0;
  for (auto c = mconf.getFirstItem(); c; c = c->next, ++count)
    for (int d = 0; d < DISKCOUNT; ++d) {
      char const * disk = c->disk[d];
      if (!disk || !disk[0])
        continue;
      char q[200];
      if (!strcmp(disk, e.url)) {
        snprintf(q, sizeof(q), "\"%.40s\" already uses this image.\nAdd it once more?", c->desc);
        if (ui.message("Already There", q, "No;Yes") != 1)
          return -1;
      } else if ((!strncmp(disk, "http://", 7) || !strncmp(disk, "https://", 8)) && !strcmp(repoImageName(disk), name)) {
        snprintf(q, sizeof(q), "\"%.40s\" has another image that is\nalso saved as %.30s on the SD card.\nBoth machines would share one file.", c->desc, name);
        if (ui.message("Same File Name", q, "Cancel;Add Anyway") != 1)
          return -1;
      }
    }

  // Floppy or hard disk: the list does not say, so the image's size decides
  int drive;
  long long size = imageSize(ui, e.url);
  if (size > 0)
    drive = repoIsFloppySize(size) ? 0 : 2;
  else {
    static char const * KINDS[] = { "Floppy Disk (A:)", "Hard Disk (C:)" };
    int k = ui.menu("Disk Type", "The server did not say how big the image is.", KINDS, 2, 1);
    if (k < 0)
      return -1;
    drive = k == 0 ? 0 : 2;
  }

  // mconfs.txt quotes the description and has no escape for a quote inside it
  char desc[sizeof(e.name)];
  strcpy(desc, e.name);
  for (char * q = desc; (q = strchr(q, '"')); )
    *q = '\'';

  auto item = new MachineConfItem;
  item->setDesc(desc);
  item->setDisk(drive, e.url);
  item->bootDrive    = drive;
  item->ramSizeKB    = e.ramSizeKB;
  item->videoAdapter = e.videoAdapter;
  item->adlib        = e.adlib;
  item->tandyDAC     = e.tandyDAC;
  // the DAC needs the sound chip, which the Tandy has anyway
  item->tandySound   = e.tandyDAC && e.videoAdapter != 2;
  mconf.addItem(item);
  saveMachineConfiguration(&mconf);
  return count;
}


static void drawFrame(TextUI & ui, char const * listURL)
{
  ui.clear(TUI_SCREEN);
  ui.centre(0, "ESP32 PC Emulator Setup Utility", TUI_SCREEN);
  ui.centre(1, "Add from Repository", TUI_LABEL);

  ui.frame(0, 2, 80, 18, TUI_SCREEN, true);
  ui.text(2, 3, "Machine", TUI_LABEL);
  ui.text(47, 3, "Type", TUI_LABEL);
  ui.text(69, 3, "Memory", TUI_LABEL);

  ui.frame(0, 20, 80, 5, TUI_SCREEN, true);
  ui.text(2, 22, "\x18\x19 : Select     Enter : Add Machine     Esc : Back to the Machine List", TUI_SCREEN);
  char u[80];
  snprintf(u, sizeof(u), " %.70s ", listURL);
  ui.text(2, 20, u, TUI_DIM);
}


int setupAddFromRepository(TextUI & ui, MachineConf & mconf, char * url, int urlSize)
{
  if (!ui.textInput("Add from Repository", "Repository URL (files.list is fetched from it):", url, urlSize - 1))
    return -1;

  char listURL[300];
  if (!repoListURL(url, listURL, sizeof(listURL)) ||
      (strncasecmp(listURL, "http://", 7) && strncasecmp(listURL, "https://", 8))) {
    ui.message("Repository", "That is not an http:// or https:// URL.");
    return -1;
  }

  if (!checkWiFi())
    return -1;

  // nothing runs yet at this point, so there is internal RAM to spare for these
  auto text = (char *) malloc(MAXLISTBYTES);
  auto list = (RepoList *) malloc(sizeof(RepoList));
  if (!text || !list) {
    free(text);
    free(list);
    ui.message("Repository", "Not enough memory for the list.");
    return -1;
  }

  int lastAdded = -1;
  char error[160];
  int len = fetchList(ui, listURL, text, MAXLISTBYTES, error, sizeof(error));
  if (len < 0) {
    ui.message("Repository", error);
  } else {
    repoParse(text, len, list);
    if (list->count == 0)
      ui.message("Repository", "The list has no machines in it.");
  }
  free(text);

  if (len >= 0 && list->count > 0) {

    drawFrame(ui, listURL);

    constexpr int TOP  = 5;
    constexpr int ROWS = 14;
    int sel = 0, scroll = 0;
    char status[80] = "";
    if (list->truncated)
      snprintf(status, sizeof(status), "Only the first %d machines of the list are shown.", REPO_MAXENTRIES);

    while (true) {
      for (int r = 0; r < ROWS; ++r) {
        int i = scroll + r;
        bool isSel = (i == sel);
        uint8_t bar = isSel ? TUI_HILITE : TUI_SCREEN;
        ui.text(1, TOP + r, "", bar, 78);
        if (i >= list->count)
          continue;
        auto const & e = list->entries[i];
        uint8_t a = isSel ? TUI_HILITE : (e.valid() ? TUI_VALUE : TUI_DIM);
        ui.text(2, TOP + r, e.name, a, 43);
        if (e.valid()) {
          char t[24];
          repoTypeText(e, t, sizeof(t));
          ui.text(47, TOP + r, t, a, 21);
          ui.textf(69, TOP + r, a, "%d KB", e.ramSizeKB);
        } else
          ui.text(47, TOP + r, "(cannot be used)", a, 30);
      }
      ui.put(78, TOP, scroll > 0 ? 0x1E : ' ', TUI_SCREEN);
      ui.put(78, TOP + ROWS - 1, scroll + ROWS < list->count ? 0x1F : ' ', TUI_SCREEN);

      // the provider's explanation of the line under the cursor, or why it was rejected
      auto const & cur = list->entries[sel];
      if (cur.valid())
        ui.text(1, 21, cur.explain, TUI_LABEL, 78);
      else
        ui.textf(1, 21, TUI_DIM, "Not usable: %s", cur.error);
      ui.text(2, 23, status, TUI_SCREEN, 76);

      TextUI::Key k;
      ui.getKey(&k);
      status[0] = 0;
      switch (k.vk) {
        case VirtualKey::VK_UP:       case VirtualKey::VK_KP_UP:       sel = imax(0, sel - 1); break;
        case VirtualKey::VK_DOWN:     case VirtualKey::VK_KP_DOWN:     sel = imin(list->count - 1, sel + 1); break;
        case VirtualKey::VK_PAGEUP:   case VirtualKey::VK_KP_PAGEUP:   sel = imax(0, sel - ROWS); break;
        case VirtualKey::VK_PAGEDOWN: case VirtualKey::VK_KP_PAGEDOWN: sel = imin(list->count - 1, sel + ROWS); break;
        case VirtualKey::VK_HOME:     case VirtualKey::VK_KP_HOME:     sel = 0; break;
        case VirtualKey::VK_END:      case VirtualKey::VK_KP_END:      sel = list->count - 1; break;
        case VirtualKey::VK_RETURN:   case VirtualKey::VK_KP_ENTER:
        {
          int added = addMachine(ui, mconf, list->entries[sel]);
          if (added >= 0) {
            lastAdded = added;
            snprintf(status, sizeof(status), "Added \"%.30s\". Its image is downloaded on the first boot.", list->entries[sel].name);
          }
          break;
        }
        case VirtualKey::VK_ESCAPE:
          free(list);
          return lastAdded;
        default:
          break;
      }
      if (sel < scroll)
        scroll = sel;
      else if (sel >= scroll + ROWS)
        scroll = sel - ROWS + 1;
    }
  }

  free(list);
  return lastAdded;
}
