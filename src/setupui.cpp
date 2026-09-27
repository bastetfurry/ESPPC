/*
  PCEmulator - host setup screens on the text UI, see setupui.h
*/


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <Arduino.h>
#include "soc/soc.h"

#include "setupui.h"


using fabgl::VirtualKey;
using fabgl::FileBrowser;
using fabgl::imin;
using fabgl::imax;


namespace {

constexpr int MAXPATH = 256;

bool isEnter(VirtualKey vk)   { return vk == VirtualKey::VK_RETURN || vk == VirtualKey::VK_KP_ENTER; }

// the common list movement keys; true if the key was one of them
bool listMove(VirtualKey vk, int & sel, int count, int page)
{
  switch (vk) {
    case VirtualKey::VK_UP:       case VirtualKey::VK_KP_UP:       sel = imax(0, sel - 1);          return true;
    case VirtualKey::VK_DOWN:     case VirtualKey::VK_KP_DOWN:     sel = imin(count - 1, sel + 1);  return true;
    case VirtualKey::VK_PAGEUP:   case VirtualKey::VK_KP_PAGEUP:   sel = imax(0, sel - page);       return true;
    case VirtualKey::VK_PAGEDOWN: case VirtualKey::VK_KP_PAGEDOWN: sel = imin(count - 1, sel + page); return true;
    case VirtualKey::VK_HOME:     case VirtualKey::VK_KP_HOME:     sel = 0;                         return true;
    case VirtualKey::VK_END:      case VirtualKey::VK_KP_END:      sel = count - 1;                 return true;
    default:                                                                                         return false;
  }
}


// Writes s into a box of width w and up to h lines, breaking at spaces and at '\n'.
void wrapText(TextUI & ui, int x, int y, int w, int h, char const * s, uint8_t attr)
{
  for (int line = 0; line < h; ++line)
    ui.text(x, y + line, "", attr, w);
  int line = 0;
  while (s && *s && line < h) {
    int len = 0, lastSpace = -1;
    while (s[len] && s[len] != '\n' && len < w) {
      if (s[len] == ' ')
        lastSpace = len;
      ++len;
    }
    if (s[len] && s[len] != '\n' && lastSpace > 0)
      len = lastSpace;
    char buf[81];
    memcpy(buf, s, len);
    buf[len] = 0;
    ui.text(x, y + line, buf, attr);
    s += len;
    while (*s == ' ' || *s == '\n') {
      bool nl = *s == '\n';
      ++s;
      if (nl)
        break;
    }
    ++line;
  }
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// directory list, shared by the file picker and the Files browser


struct DirList {
  FileBrowser fb;
  int         sel    = 0;
  int         scroll = 0;
  int         x, y, w, h;          // list area inside the frame

  bool open(char const * dir) {
    if (!dir || !dir[0] || !fb.setDirectory(dir))
      return fb.setDirectory(SD_MOUNT_PATH);
    return true;
  }

  int count()                     { return fb.count(); }
  fabgl::DirItem const * item()   { return count() ? fb.get(sel) : nullptr; }

  void select(char const * name) {
    sel = 0;
    for (int i = 0; i < count(); ++i)
      if (!strcmp(fb.get(i)->name, name)) {
        sel = i;
        break;
      }
  }

  void draw(TextUI & ui, uint8_t attr, uint8_t selAttr) {
    if (sel >= count())
      sel = imax(0, count() - 1);
    if (sel < scroll)
      scroll = sel;
    else if (sel >= scroll + h)
      scroll = sel - h + 1;
    for (int r = 0; r < h; ++r) {
      int i = scroll + r;
      bool isSel = (i == sel);
      ui.text(x, y + r, "", isSel ? selAttr : attr, w);
      if (i < count()) {
        auto it = fb.get(i);
        ui.text(x + 1, y + r, it->name, isSel ? selAttr : attr, w - 9);
        if (it->isDir)
          ui.text(x + w - 7, y + r, "<DIR>", isSel ? selAttr : attr);
      }
    }
    ui.put(x + w, y, scroll > 0 ? 0x1E : 0xB3, attr);
    ui.put(x + w, y + h - 1, scroll + h < count() ? 0x1F : 0xB3, attr);
  }

  // Enter on a directory, or Backspace: move and keep the list sensible
  void enter(char const * sub) {
    // coming back up, land on the directory we came from
    char from[64] = "";
    if (!strcmp(sub, "..")) {
      char const * slash = strrchr(fb.directory(), '/');
      if (slash)
        strlcpy(from, slash + 1, sizeof(from));
    }
    fb.changeDirectory(sub);
    scroll = 0;
    select(from);
  }

  // type-ahead: first entry after the current one that starts with the letter
  void jump(char c) {
    int n = count();
    for (int k = 1; k <= n; ++k) {
      int i = (sel + k) % n;
      if (toupper((unsigned char) fb.get(i)->name[0]) == toupper((unsigned char) c)) {
        sel = i;
        return;
      }
    }
  }
};


void drawDirHeader(TextUI & ui, DirList & dl, int x, int y, int w, uint8_t attr)
{
  char path[80];
  snprintf(path, sizeof(path), " %s ", dl.fb.directory());
  ui.text(x, y, path, attr, w);
}


void showSelectedSize(TextUI & ui, DirList & dl, int x, int y, int w, uint8_t attr)
{
  auto it = dl.item();
  if (!it || it->isDir || !strcmp(it->name, "..")) {
    ui.text(x, y, "", attr, w);
    return;
  }
  size_t size = dl.fb.fileSize(it->name);
  if (size >= 1048576)
    ui.textf(x, y, attr, "%-*.*s %8.2f MB", w - 12, w - 12, it->name, size / 1048576.0);
  else
    ui.textf(x, y, attr, "%-*.*s %8u  B", w - 12, w - 12, it->name, (unsigned) size);
}

}   // namespace


////////////////////////////////////////////////////////////////////////////////////////////////////
// file picker


bool setupFileDialog(TextUI & ui, char const * title, char * dir, int maxDir, char * filename, int maxFilename, bool allowNew)
{
  uint8_t * saved = (uint8_t*) malloc(TextUI::SCREENBYTES);
  if (saved)
    ui.save(saved);

  constexpr int X = 6, Y = 2, W = 68, H = 21;
  ui.box(X, Y, W, H, TUI_DIALOG);
  char t[80];
  snprintf(t, sizeof(t), " %s ", title);
  ui.text(X + (W - (int) strlen(t)) / 2, Y, t, TUI_DIALOG_HEAD);
  ui.text(X + 2, Y + H - 1, allowNew ? " Enter:Open/Select  Bksp:Up  F2:New Name  Esc:Cancel "
                                     : " Enter:Open/Select  Bksp:Up  Esc:Cancel ", TUI_DIALOG_HEAD);

  DirList dl;
  dl.x = X + 2;  dl.y = Y + 3;  dl.w = W - 5;  dl.h = H - 6;
  dl.open(dir);
  dl.select(filename);

  bool ok = false;
  while (true) {
    drawDirHeader(ui, dl, X + 2, Y + 1, W - 4, TUI_DIALOG_HEAD);
    dl.draw(ui, TUI_DIALOG, TUI_DIALOG_SEL);
    showSelectedSize(ui, dl, X + 2, Y + H - 2, W - 4, TUI_DIALOG);

    TextUI::Key k;
    ui.getKey(&k);
    if (listMove(k.vk, dl.sel, dl.count(), dl.h))
      continue;
    if (k.vk == VirtualKey::VK_ESCAPE)
      break;
    if (k.vk == VirtualKey::VK_BACKSPACE) {
      if (strcmp(dl.fb.directory(), "/"))
        dl.enter("..");
      continue;
    }
    if (isEnter(k.vk)) {
      auto it = dl.item();
      if (!it)
        continue;
      if (it->isDir) {
        dl.enter(it->name);
        continue;
      }
      strlcpy(dir, dl.fb.directory(), maxDir);
      strlcpy(filename, it->name, maxFilename);
      ok = true;
      break;
    }
    if (k.vk == VirtualKey::VK_F2 && allowNew) {
      if (!strcmp(dl.fb.directory(), "/")) {
        ui.message("New Name", "Open the SD card or the flash\nfile system first.");
        continue;
      }
      char name[64];
      strlcpy(name, filename, sizeof(name));
      if (ui.textInput(title, "File name", name, imin((int) sizeof(name) - 1, maxFilename - 1)) && name[0]) {
        strlcpy(dir, dl.fb.directory(), maxDir);
        strlcpy(filename, name, maxFilename);
        ok = true;
        break;
      }
      continue;
    }
    if (k.ascii > ' ' && k.ascii < 0x7f)
      dl.jump(k.ascii);
  }

  if (saved) {
    ui.restore(saved);
    free(saved);
  }
  return ok;
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Files browser


static void copyFile(TextUI & ui, char const * srcDir, char const * name, FileBrowser & dst)
{
  FileBrowser src(srcDir);
  size_t total = src.fileSize(name);
  FILE * in = src.openFile(name, "rb");
  if (!in) {
    ui.message("Copy", "Cannot open the source file.");
    return;
  }
  if (dst.exists(name, false) && ui.message("Copy", "The file exists here already.\nOverwrite it?", "No;Yes") != 1) {
    fclose(in);
    return;
  }
  FILE * out = dst.openFile(name, "wb");
  if (!out) {
    fclose(in);
    ui.message("Copy", "Cannot create the file here.");
    return;
  }
  bool failed = false;
  ui.progress("Copying", true, [&](TextUI::Progress & p) {
    constexpr int BUFLEN = 4096;
    uint8_t * buf = (uint8_t*) malloc(BUFLEN);
    size_t done = 0;
    int lastPerc = -1;
    while (buf && done < total) {
      size_t r = fread(buf, 1, imin(BUFLEN, (int)(total - done)), in);
      if (r == 0 || fwrite(buf, 1, r, out) != r) {
        failed = true;
        break;
      }
      done += r;
      int perc = total ? (int)((uint64_t) done * 100 / total) : 100;
      if (perc != lastPerc) {
        lastPerc = perc;
        if (!p.update(perc, "%s  %u / %u bytes", name, (unsigned) done, (unsigned) total)) {
          failed = true;
          break;
        }
      }
    }
    free(buf);
  });
  fclose(in);
  fclose(out);
  if (failed) {
    dst.remove(name);
    ui.message("Copy", "The copy did not complete\nand was removed.");
  }
}


void setupFilesBrowser(TextUI & ui, char const * startDir)
{
  uint8_t * saved = (uint8_t*) malloc(TextUI::SCREENBYTES);
  if (saved)
    ui.save(saved);

  ui.clear(TUI_SCREEN);
  ui.centre(0, "ESP32 PC Emulator Setup Utility", TUI_SCREEN);
  ui.centre(1, "Browse Files", TUI_LABEL);
  ui.frame(0, 2, 80, 20, TUI_SCREEN, true);
  ui.frame(0, 21, 80, 4, TUI_SCREEN, true);
  ui.put(0, 21, 0xCC, TUI_SCREEN);
  ui.put(79, 21, 0xB9, TUI_SCREEN);
  ui.text(2, 22, "Enter:Open  Bksp:Up  F2:Rename  F5:Copy  F6:Paste  F7:New Folder  F8:Delete", TUI_SCREEN);
  ui.text(2, 23, "Esc:Back to machine selection", TUI_DIM);

  DirList dl;
  dl.x = 2;  dl.y = 4;  dl.w = 75;  dl.h = 16;
  dl.open(startDir);

  char clipDir[MAXPATH] = "";
  char clipName[64]     = "";

  while (true) {
    drawDirHeader(ui, dl, 2, 3, 76, TUI_LABEL);
    dl.draw(ui, TUI_SCREEN, TUI_HILITE);
    if (clipName[0])
      ui.textf(2, 20, TUI_DIM, "Clipboard: %-.60s", clipName);
    else
      showSelectedSize(ui, dl, 2, 20, 76, TUI_DIM);

    TextUI::Key k;
    ui.getKey(&k);
    if (listMove(k.vk, dl.sel, dl.count(), dl.h))
      continue;

    auto it       = dl.item();
    bool atRoot   = !strcmp(dl.fb.directory(), "/");
    bool realItem = it && strcmp(it->name, "..") && !atRoot;

    switch (k.vk) {

      case VirtualKey::VK_ESCAPE:
        if (saved) {
          ui.restore(saved);
          free(saved);
        }
        return;

      case VirtualKey::VK_BACKSPACE:
        if (!atRoot)
          dl.enter("..");
        break;

      case VirtualKey::VK_F2:
        if (realItem) {
          char name[64];
          strlcpy(name, it->name, sizeof(name));
          if (ui.textInput("Rename", "New name", name, sizeof(name) - 1) && name[0] && strcmp(name, it->name)) {
            dl.fb.rename(it->name, name);
            dl.fb.reload();
            dl.select(name);
          }
        }
        break;

      case VirtualKey::VK_F5:
        if (realItem && !it->isDir) {
          strlcpy(clipDir, dl.fb.directory(), sizeof(clipDir));
          strlcpy(clipName, it->name, sizeof(clipName));
        }
        break;

      case VirtualKey::VK_F6:
        if (!clipName[0] || atRoot)
          break;
        if (!strcmp(clipDir, dl.fb.directory())) {
          ui.message("Paste", "Open a different folder first.");
          break;
        }
        copyFile(ui, clipDir, clipName, dl.fb);
        dl.fb.reload();
        dl.select(clipName);
        clipName[0] = 0;
        ui.text(2, 20, "", TUI_DIM, 76);
        break;

      case VirtualKey::VK_F7:
        if (!atRoot) {
          char name[64] = "";
          if (ui.textInput("New Folder", "Folder name", name, sizeof(name) - 1) && name[0]) {
            dl.fb.makeDirectory(name);
            dl.fb.reload();
            dl.select(name);
          }
        }
        break;

      case VirtualKey::VK_F8:
      case VirtualKey::VK_DELETE:
      case VirtualKey::VK_KP_DELETE:
        if (realItem) {
          char q[96];
          snprintf(q, sizeof(q), "Delete %s\n\"%.40s\"?", it->isDir ? "the folder" : "the file", it->name);
          if (ui.message("Please Confirm", q, "No;Yes") == 1) {
            int keep = dl.sel;
            dl.fb.remove(it->name);
            dl.fb.reload();
            dl.sel = keep;
          }
        }
        break;

      default:
        if (isEnter(k.vk)) {
          if (it && it->isDir)
            dl.enter(it->name);
        } else if (k.ascii > ' ' && k.ascii < 0x7f)
          dl.jump(k.ascii);
        break;
    }
  }
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Create Disk


void setupCreateDiskImage(TextUI & ui)
{
  static char const * TYPES[] = {
    "Floppy 320K  (FAT12)",       // 0, the order createFATFloppyImage() numbers them
    "Floppy 360K  (FAT12)",       // 1
    "Floppy 720K  (FAT12)",       // 2
    "Floppy 1.2M  (FAT12)",       // 3
    "Floppy 1.44M (FAT12)",       // 4
    "Floppy 2.88M (FAT12)",       // 5
    "Hard Disk (unformatted)",    // 6
  };

  int s = ui.menu("Create Disk Image", "Disk type", TYPES, 7, 4);
  if (s < 0)
    return;

  int hdSize = 0;
  if (s == 6) {
    char str[4] = "10";
    if (!ui.textInput("Hard Disk Size", "Size in megabytes (1-512)", str, 3))
      return;
    hdSize = atoi(str);
    if (hdSize <= 0 || hdSize > 512) {
      ui.message("Create Disk Image", "Invalid hard disk size.");
      return;
    }
  }

  char dir[MAXPATH]  = SD_MOUNT_PATH;
  char filename[64]  = "newimage.img";
  if (!setupFileDialog(ui, "Image File Name", dir, sizeof(dir), filename, sizeof(filename), true))
    return;

  FileBrowser fb(dir);
  if (fb.exists(filename, false)) {
    char q[120];
    snprintf(q, sizeof(q), "\"%.40s\" exists already.\nOverwrite it?", filename);
    if (ui.message("Create Disk Image", q, "No;Yes") != 1)
      return;
  }

  if (s == 6) {
    // Hard disk: zeros, the guest partitions and formats it itself
    ui.progress("Create Disk Image", true, [&](TextUI::Progress & p) {
      auto buf = (uint8_t*) SOC_EXTRAM_DATA_LOW;   // PSRAM as buffer, nothing else uses it yet
      constexpr int BUFSIZE = 32768;
      memset(buf, 0, BUFSIZE);
      auto file = fb.openFile(filename, "wb");
      if (!file) {
        p.update(0, "Cannot create %s", filename);
        delay(1500);
        return;
      }
      const int totSize = hdSize * 1048576;
      int prevPerc = -1;
      for (int sz = totSize; sz > 0; ) {
        int written = fwrite(buf, 1, imin(BUFSIZE, sz), file);
        if (written <= 0)
          break;    // out of space or write error: don't spin forever
        sz -= written;
        int perc = (int)((double)(totSize - sz) / totSize * 100);
        if (perc != prevPerc) {
          prevPerc = perc;
          if (!p.update(perc, "Writing %s  %d / %d MB", filename, (totSize - sz) >> 20, hdSize))
            break;
        }
      }
      fclose(file);
    });
  } else {
    if (!createFATFloppyImage(&ui, s, dir, filename))
      ui.message("Create Disk Image", "Cannot create the image file.");
  }
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// machine editor, in the manner of an Award "Standard CMOS Features" page


namespace {

enum class Kind { Text, Path, CHS, Choice };

struct Field {
  char const *         label;
  Kind                 kind;
  char const *         help;
  char const * const * options;      // Choice only
  int                  optionCount;
};

char const * const BOOT_OPTS[]  = { "Floppy A", "Floppy B", "Hard Disk 0", "Hard Disk 1" };
char const * const RAM_OPTS[]   = { "64 KB", "128 KB", "192 KB", "256 KB", "320 KB", "384 KB",
                                    "448 KB", "512 KB", "576 KB", "640 KB", "704 KB" };
char const * const VIDEO_OPTS[] = { "CGA / Hercules", "EGA (BIOS ROM)", "Tandy 1000" };
char const * const ONOFF_OPTS[] = { "Disabled", "Enabled" };
char const * const USB_OPTS[]   = { "Not Connected", "COM1", "COM2" };

enum {
  F_DESC, F_FD0, F_FD1, F_HD0, F_HD0CHS, F_HD1, F_HD1CHS,
  F_BOOT, F_RAM, F_VIDEO, F_COMPOSITE, F_EGAROM, F_TSOUND, F_TDAC, F_ADLIB, F_DEBUG, F_USB,
  F_COUNT
};

Field const FIELDS[F_COUNT] = {
  { "Description",      Kind::Text,   "The name shown in the machine list.", nullptr, 0 },
  { "Floppy A",         Kind::Path,   "Disk image for drive A. A path on the SD card, an absolute path such as /spiffs/rescue.img, or an http(s) URL that is downloaded on first boot.", nullptr, 0 },
  { "Floppy B",         Kind::Path,   "Disk image for drive B.", nullptr, 0 },
  { "Hard Disk 0",      Kind::Path,   "Image for the first hard disk (C:).", nullptr, 0 },
  { "  Cyl/Head/Sect",  Kind::CHS,    "Geometry of hard disk 0 as cylinders, heads, sectors. 0,0,0 lets the emulator guess it from the image size.", nullptr, 0 },
  { "Hard Disk 1",      Kind::Path,   "Image for the second hard disk (D:).", nullptr, 0 },
  { "  Cyl/Head/Sect",  Kind::CHS,    "Geometry of hard disk 1 as cylinders, heads, sectors. 0,0,0 lets the emulator guess it from the image size.", nullptr, 0 },
  { "Boot From",        Kind::Choice, "The drive the BIOS boots from.", BOOT_OPTS, 4 },
  { "Base Memory",      Kind::Choice, "Conventional memory. Above 640 KB the A000 segment becomes ordinary RAM, which only non-EGA machines can use.", RAM_OPTS, 11 },
  { "Video Adapter",    Kind::Choice, "CGA / Hercules is the original adapter. EGA runs a real EGA BIOS option ROM. Tandy 1000 adds its 16 colour modes 8 and 9 and the three voice sound chip.", VIDEO_OPTS, 3 },
  { "  Composite Mon.", Kind::Choice, "A composite colour monitor instead of RGB: CGA graphics with the colour burst on show NTSC artifact colours, 160x200 in 16 colours, for games that offer a composite mode.", ONOFF_OPTS, 2 },
  { "EGA BIOS ROM",     Kind::Path,   "Option ROM image for the EGA adapter. None uses /spiffs/ega.bin.", nullptr, 0 },
  { "Tandy Sound",      Kind::Choice, "The Tandy 1000's three voice SN76496 at port C0h, as on the TNDY style sound cards, for games that offer Tandy sound. Tandy 1000 video always has it.", ONOFF_OPTS, 2 },
  { "  Tandy DAC",      Kind::Choice, "The Tandy 1000 SL/TL digital sound next to the sound chip, as KQ5 and other Sierra games use it for speech and effects. Played through the BIOS, INT 1Ah.", ONOFF_OPTS, 2 },
  { "AdLib",            Kind::Choice, "An AdLib sound card: the Yamaha OPL2 FM synthesiser at port 388h, the music of most games from the late 1980s on. It is synthesised on the video core, after the picture.", ONOFF_OPTS, 2 },
  { "Serial Debug Cons.", Kind::Choice, "Host debug console on the USB serial line: screen mirror, monitor, breakpoints.", ONOFF_OPTS, 2 },
  { "USB Serial Port",  Kind::Choice, "Which emulated COM port reaches the host over the USB serial. Works alongside the debug console.", USB_OPTS, 3 },
};

// the values being edited, apart from the configuration item until they are saved
struct Values {
  char     text[F_COUNT][MAXPATH];   // Text and Path fields
  uint16_t chs[4][3];                // [drive][c,h,s], drives 2 and 3 used
  int      choice[F_COUNT];          // Choice fields
};

int chsDrive(int f)   { return f == F_HD0CHS ? 2 : 3; }

void valueText(Values & v, int f, char * out, int size)
{
  switch (FIELDS[f].kind) {
    case Kind::Text:
    case Kind::Path:
      if (v.text[f][0]) {
        // paths are long; the file name is what matters at a glance
        char const * s = v.text[f];
        if (FIELDS[f].kind == Kind::Path && (int) strlen(s) >= size) {
          char const * slash = strrchr(s, '/');
          if (slash)
            s = slash + 1;
        }
        strlcpy(out, s, size);
      } else
        strlcpy(out, "None", size);
      break;
    case Kind::CHS:
    {
      auto c = v.chs[chsDrive(f)];
      if (c[0] && c[1] && c[2])
        snprintf(out, size, "%u / %u / %u", c[0], c[1], c[2]);
      else
        strlcpy(out, "Auto", size);
      break;
    }
    case Kind::Choice:
      strlcpy(out, FIELDS[f].options[v.choice[f]], size);
      break;
  }
}

bool isVisible(Values & v, int f)
{
  // the ROM only matters for the EGA path, and the Tandy video has the sound chip anyway
  if (f == F_EGAROM)
    return v.choice[F_VIDEO] == 1;
  if (f == F_COMPOSITE)
    return v.choice[F_VIDEO] == 0;
  if (f == F_TSOUND)
    return v.choice[F_VIDEO] != 2;
  if (f == F_TDAC)
    return v.choice[F_VIDEO] == 2 || v.choice[F_TSOUND] != 0;
  return true;
}

}   // namespace


bool setupEditMachine(TextUI & ui, MachineConf & mconf, int idx)
{
  auto item = mconf.getItem(idx);
  if (!item)
    return false;

  Values * v = (Values*) calloc(1, sizeof(Values));   // 3.5 KB: the setup task's stack is small
  if (!v)
    return false;

  strlcpy(v->text[F_DESC], item->desc ? item->desc : "", MAXPATH);
  int const diskField[DISKCOUNT] = { F_FD0, F_FD1, F_HD0, F_HD1 };
  for (int d = 0; d < DISKCOUNT; ++d)
    strlcpy(v->text[diskField[d]], item->disk[d] ? item->disk[d] : "", MAXPATH);
  strlcpy(v->text[F_EGAROM], item->egaRom ? item->egaRom : "", MAXPATH);
  for (int d = 2; d < 4; ++d) {
    v->chs[d][0] = item->cylinders[d];
    v->chs[d][1] = item->heads[d];
    v->chs[d][2] = item->sectors[d];
  }
  v->choice[F_BOOT]  = item->bootDrive;
  v->choice[F_RAM]   = imax(0, imin(10, item->ramSizeKB / 64 - 1));
  v->choice[F_VIDEO] = item->videoAdapter % 3;
  v->choice[F_TSOUND] = item->tandySound ? 1 : 0;
  v->choice[F_TDAC]   = item->tandyDAC ? 1 : 0;
  v->choice[F_COMPOSITE] = item->composite ? 1 : 0;
  v->choice[F_ADLIB]  = item->adlib ? 1 : 0;
  v->choice[F_DEBUG] = item->debugConsole ? 1 : 0;
  v->choice[F_USB]   = item->effectiveUsbSerial() % 3;

  // --- screen ---
  ui.clear(TUI_SCREEN);
  ui.centre(0, "ESP32 PC Emulator Setup Utility", TUI_SCREEN);
  ui.centre(1, "Machine Configuration", TUI_LABEL);
  ui.frame(0, 2, 80, 18, TUI_SCREEN, true);
  for (int y = 3; y < 19; ++y)
    ui.put(52, y, 0xB3, TUI_SCREEN);
  ui.put(52, 2, 0xD1, TUI_SCREEN);
  ui.put(52, 19, 0xCF, TUI_SCREEN);
  ui.text(58, 3, "Item Help", TUI_LABEL);
  ui.frame(0, 20, 80, 5, TUI_SCREEN, true);
  ui.text(2, 21, "\x18\x19:Move   Enter:Select   +/-/PU/PD:Value   F9:Create Disk Image", TUI_SCREEN);
  ui.text(2, 22, "F10:Save & Exit   Esc:Exit", TUI_SCREEN);

  int  sel      = 0;
  bool modified = false;
  bool saved    = false;

  while (true) {

    // items, one row each, hidden ones skipped
    int row = 3;
    for (int f = 0; f < F_COUNT; ++f) {
      if (!isVisible(*v, f))
        continue;
      bool isSel = (f == sel);
      char val[40];
      valueText(*v, f, val, 26);
      ui.text(2, row, FIELDS[f].label, TUI_SCREEN, 20);
      ui.text(22, row, "[", TUI_SCREEN);
      ui.text(23, row, val, isSel ? TUI_HILITE : TUI_LABEL, 26);
      ui.text(49, row, "]", TUI_SCREEN);
      ++row;
    }
    for (; row < 19; ++row)
      ui.text(2, row, "", TUI_SCREEN, 49);

    // help, and the full value where it did not fit
    wrapText(ui, 54, 5, 24, 9, FIELDS[sel].help, TUI_SCREEN);
    if ((FIELDS[sel].kind == Kind::Path || FIELDS[sel].kind == Kind::Text) && v->text[sel][0])
      wrapText(ui, 54, 15, 24, 4, v->text[sel], TUI_DIM);
    else
      wrapText(ui, 54, 15, 24, 4, "", TUI_DIM);

    TextUI::Key k;
    ui.getKey(&k);

    // movement skips hidden rows
    int dir = 0;
    if (k.vk == VirtualKey::VK_UP || k.vk == VirtualKey::VK_KP_UP)         dir = -1;
    if (k.vk == VirtualKey::VK_DOWN || k.vk == VirtualKey::VK_KP_DOWN)     dir = +1;
    if (dir) {
      int n = sel;
      do {
        n += dir;
      } while (n >= 0 && n < F_COUNT && !isVisible(*v, n));
      if (n >= 0 && n < F_COUNT)
        sel = n;
      continue;
    }
    if (k.vk == VirtualKey::VK_HOME || k.vk == VirtualKey::VK_KP_HOME) { sel = 0; continue; }
    if (k.vk == VirtualKey::VK_END || k.vk == VirtualKey::VK_KP_END)   { sel = F_USB; continue; }

    Field const & fd = FIELDS[sel];

    // value stepping for choices, as a BIOS setup does with +/- and PgUp/PgDn
    int step = 0;
    if (k.vk == VirtualKey::VK_PAGEDOWN || k.vk == VirtualKey::VK_KP_PAGEDOWN || k.ascii == '+' || k.vk == VirtualKey::VK_RIGHT) step = +1;
    if (k.vk == VirtualKey::VK_PAGEUP || k.vk == VirtualKey::VK_KP_PAGEUP || k.ascii == '-' || k.vk == VirtualKey::VK_LEFT)    step = -1;
    if (step && fd.kind == Kind::Choice) {
      v->choice[sel] = (v->choice[sel] + step + fd.optionCount) % fd.optionCount;
      modified = true;
      continue;
    }

    if (k.vk == VirtualKey::VK_F9) {
      setupCreateDiskImage(ui);
      continue;
    }

    if (isEnter(k.vk)) {
      switch (fd.kind) {

        case Kind::Choice:
        {
          int s = ui.menu(fd.label, nullptr, fd.options, fd.optionCount, v->choice[sel]);
          if (s >= 0 && s != v->choice[sel]) {
            v->choice[sel] = s;
            modified = true;
          }
          break;
        }

        case Kind::Text:
        {
          char buf[64];
          strlcpy(buf, v->text[sel], sizeof(buf));
          if (ui.textInput(fd.label, nullptr, buf, 60)) {
            strlcpy(v->text[sel], buf, MAXPATH);
            modified = true;
          }
          break;
        }

        case Kind::CHS:
        {
          auto c = v->chs[chsDrive(sel)];
          char buf[20];
          snprintf(buf, sizeof(buf), "%u,%u,%u", c[0], c[1], c[2]);
          if (ui.textInput(fd.label + 2, "Cylinders,Heads,Sectors  (0,0,0 = auto)", buf, 16)) {
            unsigned cc = 0, hh = 0, ss = 0;
            sscanf(buf, "%u,%u,%u", &cc, &hh, &ss);
            c[0] = cc;  c[1] = hh;  c[2] = ss;
            modified = true;
          }
          break;
        }

        case Kind::Path:
        {
          static char const * ACTIONS[] = { "Browse SD Card / Flash...", "Type a Path or URL...", "None" };
          int a = ui.menu(fd.label, nullptr, ACTIONS, 3, 0);
          if (a == 0) {
            // start where the current image is; a stored path is absolute or relative to /SD
            char dirBuf[MAXPATH] = SD_MOUNT_PATH;
            char nameBuf[64]     = "";
            char const * cur = v->text[sel];
            if (cur[0] && !strstr(cur, "://")) {
              char const * slash = strrchr(cur, '/');
              if (slash) {
                if (cur[0] == '/')
                  snprintf(dirBuf, sizeof(dirBuf), "%.*s", (int)(slash - cur), cur);
                else
                  snprintf(dirBuf, sizeof(dirBuf), "%s/%.*s", SD_MOUNT_PATH, (int)(slash - cur), cur);
                if (!dirBuf[0])
                  strcpy(dirBuf, "/");
                strlcpy(nameBuf, slash + 1, sizeof(nameBuf));
              } else
                strlcpy(nameBuf, cur, sizeof(nameBuf));
            }
            if (setupFileDialog(ui, "Select Disk Image", dirBuf, sizeof(dirBuf), nameBuf, sizeof(nameBuf), false)) {
              // the full path, so images in the flash file system work as well as SD ones
              snprintf(v->text[sel], MAXPATH, "%s%s%s", dirBuf, strcmp(dirBuf, "/") ? "/" : "", nameBuf);
              modified = true;
            }
          } else if (a == 1) {
            char * buf = (char*) malloc(MAXPATH);
            if (buf) {
              strlcpy(buf, v->text[sel], MAXPATH);
              if (ui.textInput(fd.label, "Path or http(s) URL", buf, MAXPATH - 1)) {
                strlcpy(v->text[sel], buf, MAXPATH);
                modified = true;
              }
              free(buf);
            }
          } else if (a == 2 && v->text[sel][0]) {
            v->text[sel][0] = 0;
            modified = true;
          }
          break;
        }
      }
      continue;
    }

    if (k.vk == VirtualKey::VK_ESCAPE) {
      if (!modified || ui.message("Exit", "Quit without saving (Y/N)?", "No;Yes") == 1)
        break;
      continue;
    }

    if (k.vk == VirtualKey::VK_F10) {
      if (ui.message("Save & Exit", "Save to SD card and exit (Y/N)?", "No;Yes") != 1)
        continue;

      // Warn when the boot drive has no image: the BIOS then boots from an empty drive, which looks
      // like a dead machine rather than a reported failure. Only a warning, because booting a floppy
      // mounted later from the SysReq menu is legitimate.
      if (!v->text[diskField[v->choice[F_BOOT]]][0]) {
        char q[120];
        snprintf(q, sizeof(q), "Boot drive %s has no disk image.\nThe machine will not boot.", BOOT_OPTS[v->choice[F_BOOT]]);
        if (ui.message("Warning", q, "Cancel;Save Anyway") != 1)
          continue;
      }

      item->setDesc(v->text[F_DESC]);
      for (int d = 0; d < DISKCOUNT; ++d)
        item->setDisk(d, v->text[diskField[d]]);
      for (int d = 2; d < 4; ++d) {
        item->cylinders[d] = v->chs[d][0];
        item->heads[d]     = v->chs[d][1];
        item->sectors[d]   = v->chs[d][2];
      }
      item->bootDrive    = v->choice[F_BOOT];
      item->ramSizeKB    = (v->choice[F_RAM] + 1) * 64;
      item->videoAdapter = v->choice[F_VIDEO];
      item->setEgaRom(v->text[F_EGAROM]);
      item->tandySound   = v->choice[F_TSOUND] != 0;
      item->tandyDAC     = v->choice[F_TDAC] != 0;
      item->composite    = v->choice[F_COMPOSITE] != 0;
      item->adlib        = v->choice[F_ADLIB] != 0;
      item->debugConsole = v->choice[F_DEBUG] != 0;
      // written explicitly once the page has been used, so the file records the choice
      item->setUsbSerial(v->choice[F_USB] == 2 ? "com2" : (v->choice[F_USB] == 1 ? "com1" : "off"));

      saveMachineConfiguration(&mconf);
      saved = true;
      break;
    }
  }

  free(v);
  return saved;
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Advanced Settings, in the manner of an Award "Advanced BIOS Features" page


namespace {

// Time zones offered by name. The ESP32 wants POSIX TZ strings, which nobody should have to type;
// "Custom..." is there for any zone not listed.
struct TimeZone { char const * name; char const * tz; };
TimeZone const TIME_ZONES[] = {
  { "UTC",                              "UTC0" },
  { "UK, Ireland, Portugal (GMT/BST)",  "GMT0BST,M3.5.0/1,M10.5.0" },
  { "Central Europe (CET/CEST)",        "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Eastern Europe (EET/EEST)",        "EET-2EEST,M3.5.0/3,M10.5.0/4" },
  { "Moscow (MSK)",                     "MSK-3" },
  { "India (IST)",                      "IST-5:30" },
  { "China, Singapore (UTC+8)",         "CST-8" },
  { "Japan, Korea (UTC+9)",             "JST-9" },
  { "Australia East (AEST/AEDT)",       "AEST-10AEDT,M10.1.0,M4.1.0/3" },
  { "New Zealand (NZST/NZDT)",          "NZST-12NZDT,M9.5.0,M4.1.0/3" },
  { "Brazil, Sao Paulo (UTC-3)",        "<-03>3" },
  { "US Eastern (EST/EDT)",             "EST5EDT,M3.2.0,M11.1.0" },
  { "US Central (CST/CDT)",             "CST6CDT,M3.2.0,M11.1.0" },
  { "US Mountain (MST/MDT)",            "MST7MDT,M3.2.0,M11.1.0" },
  { "US Arizona (MST)",                 "MST7" },
  { "US Pacific (PST/PDT)",             "PST8PDT,M3.2.0,M11.1.0" },
};
constexpr int TIME_ZONE_COUNT = sizeof(TIME_ZONES) / sizeof(TIME_ZONES[0]);

// the listed zone for a TZ string, or -1 for a custom one
int findTimeZone(char const * tz)
{
  for (int i = 0; i < TIME_ZONE_COUNT; ++i)
    if (!strcmp(TIME_ZONES[i].tz, tz))
      return i;
  return -1;
}

void trimSpaces(char * buf)
{
  char * s = buf;
  while (*s == ' ')
    ++s;
  char * e = s + strlen(s);
  while (e > s && e[-1] == ' ')
    *--e = 0;
  memmove(buf, s, strlen(s) + 1);
}

}   // namespace


bool setupAdvancedSettings(TextUI & ui, HostSettings & settings)
{
  enum class Kind { Text, TimeZone, AutoBoot, Choice };

  static char const * const DRIVE_LED_OPTS[] = { "Off", "One for all drives", "One per drive (A B C D)" };

  struct Item {
    char const * label;
    Kind         kind;
    char *       value;         // the setting's buffer, edited in place (text kinds)
    int          maxLength;
    char const * whenEmpty;     // what an empty value means
    char const * help;
    int *        number;        // the setting (AutoBoot, Choice)
    char const * const * options;  // Choice: the names of the values 0..optionCount-1
    int          optionCount;
  };

  HostSettings edit = settings;

  Item const ITEMS[] = {
    { "NTP Server", Kind::Text, edit.ntpServer, (int) sizeof(edit.ntpServer) - 1, HostSettings::DEFAULT_NTP_SERVER,
      "Time server asked for the date and time at a cold boot, when Wi-Fi is configured. Empty "
      "means pool.ntp.org. Takes effect at the next power on.", nullptr },
    { "Time Zone", Kind::TimeZone, edit.timeZone, (int) sizeof(edit.timeZone) - 1, HostSettings::DEFAULT_TIME_ZONE,
      "Local time for the clock the guest sees, daylight saving included. Pick one, or Custom "
      "for a POSIX TZ string such as CET-1CEST,M3.5.0,M10.5.0/3. Takes effect when saved.", nullptr },
    { "Auto Boot", Kind::AutoBoot, nullptr, 0, nullptr,
      "Countdown before the highlighted machine boots on its own, 1-99 seconds. Immediately (0) "
      "skips the machine list: press DEL during the POST to get to it. Disabled (-1) waits for "
      "a key. +/- steps.", &edit.autoBootSeconds },
    { "Drive LED", Kind::Choice, nullptr, 0, nullptr,
      "A small green LED in the lower right corner of the screen that lights up on floppy and hard "
      "disk access, since the board has no free one. One for all drives, or one each for A, B, C and "
      "D from left to right. Takes effect when saved.", &edit.driveLed, DRIVE_LED_OPTS, 3 },
  };
  constexpr int COUNT = sizeof(ITEMS) / sizeof(ITEMS[0]);

  ui.clear(TUI_SCREEN);
  ui.centre(0, "ESP32 PC Emulator Setup Utility", TUI_SCREEN);
  ui.centre(1, "Advanced Settings", TUI_LABEL);
  ui.frame(0, 2, 80, 18, TUI_SCREEN, true);
  for (int y = 3; y < 19; ++y)
    ui.put(52, y, 0xB3, TUI_SCREEN);
  ui.put(52, 2, 0xD1, TUI_SCREEN);
  ui.put(52, 19, 0xCF, TUI_SCREEN);
  ui.text(58, 3, "Item Help", TUI_LABEL);
  ui.frame(0, 20, 80, 5, TUI_SCREEN, true);
  ui.text(2, 21, "\x18\x19:Move   Enter:Select", TUI_SCREEN);
  ui.text(2, 22, "F10:Save & Exit   Esc:Exit", TUI_SCREEN);

  int  sel      = 0;
  bool modified = false;
  bool saved    = false;

  while (true) {

    for (int i = 0; i < COUNT; ++i) {
      char numText[16];
      char const * v;
      if (ITEMS[i].kind == Kind::Choice) {
        v = ITEMS[i].options[imax(0, imin(ITEMS[i].optionCount - 1, *ITEMS[i].number))];
      } else if (ITEMS[i].kind == Kind::AutoBoot) {
        int n = *ITEMS[i].number;
        if (n < 0)
          v = "Disabled";
        else if (n == 0)
          v = "Immediately";
        else {
          snprintf(numText, sizeof(numText), "%d s", n);
          v = numText;
        }
      } else
        v = ITEMS[i].value[0] ? ITEMS[i].value : ITEMS[i].whenEmpty;
      if (ITEMS[i].kind == Kind::TimeZone) {
        int z = findTimeZone(v);
        if (z >= 0)
          v = TIME_ZONES[z].name;
      }
      ui.text(2, 3 + i, ITEMS[i].label, TUI_SCREEN, 20);
      ui.text(22, 3 + i, "[", TUI_SCREEN);
      ui.text(23, 3 + i, v, i == sel ? TUI_HILITE : TUI_LABEL, 26);
      ui.text(49, 3 + i, "]", TUI_SCREEN);
    }
    wrapText(ui, 54, 5, 24, 9, ITEMS[sel].help, TUI_SCREEN);
    // the full value where the field is too short, and the TZ string behind a named zone
    if (ITEMS[sel].value) {
      char const * v = ITEMS[sel].value[0] ? ITEMS[sel].value : ITEMS[sel].whenEmpty;
      wrapText(ui, 54, 15, 24, 4, v, TUI_DIM);
    } else
      wrapText(ui, 54, 15, 24, 4, "", TUI_DIM);

    TextUI::Key k;
    ui.getKey(&k);

    if (k.vk == VirtualKey::VK_UP || k.vk == VirtualKey::VK_KP_UP)
      sel = imax(0, sel - 1);
    else if (k.vk == VirtualKey::VK_DOWN || k.vk == VirtualKey::VK_KP_DOWN)
      sel = imin(COUNT - 1, sel + 1);
    else if (ITEMS[sel].kind == Kind::AutoBoot && (k.ascii == '+' || k.ascii == '-' ||
             k.vk == VirtualKey::VK_PAGEUP || k.vk == VirtualKey::VK_PAGEDOWN ||
             k.vk == VirtualKey::VK_KP_PAGEUP || k.vk == VirtualKey::VK_KP_PAGEDOWN ||
             k.vk == VirtualKey::VK_LEFT || k.vk == VirtualKey::VK_RIGHT)) {
      int step = (k.ascii == '+' || k.vk == VirtualKey::VK_PAGEDOWN || k.vk == VirtualKey::VK_KP_PAGEDOWN ||
                  k.vk == VirtualKey::VK_RIGHT) ? 1 : -1;
      *ITEMS[sel].number = imax(-1, imin(99, *ITEMS[sel].number + step));
      modified = true;
    }
    else if (ITEMS[sel].kind == Kind::Choice && (k.ascii == '+' || k.ascii == '-' ||
             k.vk == VirtualKey::VK_LEFT || k.vk == VirtualKey::VK_RIGHT)) {
      int step = (k.ascii == '+' || k.vk == VirtualKey::VK_RIGHT) ? 1 : -1;
      int n    = ITEMS[sel].optionCount;
      *ITEMS[sel].number = (*ITEMS[sel].number + step + n) % n;
      modified = true;
    }
    else if (isEnter(k.vk) && ITEMS[sel].kind == Kind::Choice) {
      int s = ui.menu(ITEMS[sel].label, nullptr, ITEMS[sel].options, ITEMS[sel].optionCount, *ITEMS[sel].number);
      if (s >= 0 && s != *ITEMS[sel].number) {
        *ITEMS[sel].number = s;
        modified = true;
      }
    }
    else if (isEnter(k.vk) && ITEMS[sel].kind == Kind::AutoBoot) {
      char buf[8];
      snprintf(buf, sizeof(buf), "%d", *ITEMS[sel].number);
      if (ui.textInput(ITEMS[sel].label, "Seconds, 0 = immediately, -1 = disabled", buf, 3)) {
        char * end;
        long n = strtol(buf, &end, 10);
        if (end != buf && n >= -1 && n <= 99) {
          *ITEMS[sel].number = (int) n;
          modified = true;
        } else
          ui.message(ITEMS[sel].label, "Enter a number from -1 to 99.");
      }
    }
    else if (isEnter(k.vk)) {
      Item const & it = ITEMS[sel];
      bool typeIt = (it.kind == Kind::Text);

      if (it.kind == Kind::TimeZone) {
        char const * names[TIME_ZONE_COUNT + 1];
        for (int i = 0; i < TIME_ZONE_COUNT; ++i)
          names[i] = TIME_ZONES[i].name;
        names[TIME_ZONE_COUNT] = "Custom (POSIX TZ string)...";
        int cur = findTimeZone(it.value[0] ? it.value : it.whenEmpty);
        int s = ui.menu(it.label, nullptr, names, TIME_ZONE_COUNT + 1, cur >= 0 ? cur : TIME_ZONE_COUNT);
        if (s >= 0 && s < TIME_ZONE_COUNT) {
          strlcpy(it.value, TIME_ZONES[s].tz, it.maxLength + 1);
          modified = true;
        } else if (s == TIME_ZONE_COUNT)
          typeIt = true;
      }

      if (typeIt) {
        char buf[128];
        strlcpy(buf, it.value[0] ? it.value : (it.kind == Kind::TimeZone ? it.whenEmpty : ""), sizeof(buf));
        if (ui.textInput(it.label, it.kind == Kind::TimeZone ? "POSIX TZ string" : nullptr, buf,
                         imin(it.maxLength, (int) sizeof(buf) - 1))) {
          trimSpaces(buf);    // no spaces in a host name or a TZ string; blank means the default
          strlcpy(it.value, buf, it.maxLength + 1);
          modified = true;
        }
      }
    } else if (k.vk == VirtualKey::VK_ESCAPE) {
      if (!modified || ui.message("Exit", "Quit without saving (Y/N)?", "No;Yes") == 1)
        break;
    } else if (k.vk == VirtualKey::VK_F10) {
      if (ui.message("Save & Exit", "Save settings and exit (Y/N)?", "No;Yes") == 1) {
        settings = edit;
        saved    = true;
        break;
      }
    }
  }

  return saved;
}
