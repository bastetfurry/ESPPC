// pctool: the emulator on a PC, without a screen, for testing. Not part of the ESP32 build.
//
//   pctool run [machine tags] [script] [options]
//
// The machine, with the tags of mconfs.txt as tag=value (a bare tag means 1):
//   fd0= fd1= hd0= hd1=   disk images        chs0= chs1=  C,H,S of a hard disk
//   boot=fd0|fd1|hd0|hd1  ram=KB             video=cga|ega|tandy   egarom=FILE (default data/fs/ega.bin)
//   egamem=256|128        the EGA's memory (256 KB default)
//   tandysound tandydac adlib composite hwdisk   clock=at|mm58174|none
//   com=1|2               an 8250 at COM1 or COM2 with nothing attached: what it sends is dropped
//
// The script, at emulated times in seconds (decimals allowed):
//   type=T:TEXT           types TEXT on the keyboard from T on (US layout, \n is Enter)
//   hold=T:NAME:SECONDS   holds one key down that long (names as for key=)
//   key=T:NAME[+NAME..]   presses keys one after the other: ENTER ESC SPACE TAB BKSP UP DOWN LEFT
//                         RIGHT HOME END PGUP PGDN INS DEL F1-F10, KP2 KP4 KP6 KP8 (the keypad's
//                         arrows, which some games read instead), or a single character
//   shot=T:FILE.ppm       draws a frame with the emulator's own renderer and writes it
//   text=T                prints the text screen (CGA or EGA text modes)
//   regs=T                prints the CPU registers and the code at CS:IP
//   steps=T:N             prints CS:IP and IF at the next N batch boundaries (16 instructions each)
//   mem=T:ADDR,LEN        dumps guest memory, hex linear address (B8000 is the CGA's memory)
//   time=T                ends the run (default 30)
//
// Checks, for tests. Each prints "ok" or "FAIL"; the run ends with a summary, and pctool's exit
// code is 1 if any failed:
//   expect=T:TEXT         TEXT must be somewhere on the text screen at T
//   framesum=T[:HEX]      a checksum of the frame drawn at T (as shot= draws it); with HEX, it
//                         must be that. Time is virtual, so a frame comes out the same every run.
//
// Options:
//   mips=N                instructions a second, in millions, that make up the virtual time (1.0)
//   write                 lets the guest write to the images; otherwise it gets copies
//
// Time is virtual and advances with the instructions run, so a run gives the same result every
// time and takes as long as the PC needs, not as long as the guest thinks.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <string>
#include <vector>
#include <algorithm>

#include "host.h"
#include "machine.h"


static Machine * s_machine;
static double    s_nsPerInstruction = 1000.0;   // 1 MIPS


//// The script

struct Event {
  int64_t     us;
  enum Kind { Scancode, Shot, Text, Regs, Steps, Mem, Expect, FrameSum, End } kind;
  int         code;          // Scancode
  std::string arg;           // Shot: file
};

static std::vector<Event> s_events;
static int                s_checks = 0;      // expect= and framesum= with a value
static size_t             s_nextEvent = 0;
static int                s_stepsToPrint = 0;

static int64_t secondsToUs(char const * s)
{
  return (int64_t) (atof(s) * 1000000.0);
}


// set 2 make codes, US layout; 0x100 = needs Shift, 0x200 = extended (E0)
static int charToScancode(char c)
{
  static const char * const ROW_LOWER = "`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./";
  static const char * const ROW_UPPER = "~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?";
  static const uint8_t      CODES[]   = {
    0x0e, 0x16, 0x1e, 0x26, 0x25, 0x2e, 0x36, 0x3d, 0x3e, 0x46, 0x45, 0x4e, 0x55,
    0x15, 0x1d, 0x24, 0x2d, 0x2c, 0x35, 0x3c, 0x43, 0x44, 0x4d, 0x54, 0x5b, 0x5d,
    0x1c, 0x1b, 0x23, 0x2b, 0x34, 0x33, 0x3b, 0x42, 0x4b, 0x4c, 0x52,
    0x1a, 0x22, 0x21, 0x2a, 0x32, 0x31, 0x3a, 0x41, 0x49, 0x4a,
  };
  if (c == ' ')  return 0x29;
  if (c == '\n') return 0x5a;
  if (c == '\t') return 0x0d;
  if (auto p = strchr(ROW_LOWER, c))
    return CODES[p - ROW_LOWER];
  if (auto p = strchr(ROW_UPPER, c))
    return CODES[p - ROW_UPPER] | 0x100;
  return -1;
}

static int keyNameToScancode(std::string const & name)
{
  static const struct { char const * name; int code; } KEYS[] = {
    { "ENTER", 0x5a }, { "ESC", 0x76 }, { "SPACE", 0x29 }, { "TAB", 0x0d }, { "BKSP", 0x66 },
    { "UP", 0x275 }, { "DOWN", 0x272 }, { "LEFT", 0x26b }, { "RIGHT", 0x274 },
    { "HOME", 0x26c }, { "END", 0x269 }, { "PGUP", 0x27d }, { "PGDN", 0x27a },
    { "INS", 0x270 }, { "DEL", 0x271 },
    { "KP2", 0x72 }, { "KP4", 0x6b }, { "KP6", 0x74 }, { "KP8", 0x75 },   // the keypad's arrows, without E0
    { "F1", 0x05 }, { "F2", 0x06 }, { "F3", 0x04 }, { "F4", 0x0c }, { "F5", 0x03 },
    { "F6", 0x0b }, { "F7", 0x83 }, { "F8", 0x0a }, { "F9", 0x01 }, { "F10", 0x09 },
  };
  for (auto & k : KEYS)
    if (name == k.name)
      return k.code;
  if (name.size() == 1)
    return charToScancode(name[0]);
  return -1;
}

// a key press as the keyboard sends it: make, held 40 ms (or hold), break, then 40 ms before the next
static int64_t addKeyStroke(int64_t us, int code, int64_t HOLD = 40000)
{
  constexpr int64_t GAP = 40000;
  bool shift = code & 0x100, ext = code & 0x200;
  int  sc    = code & 0xff;
  auto add = [&](int64_t t, int c) { s_events.push_back({ t, Event::Scancode, c, "" }); };
  if (shift)
    add(us, 0x12);
  if (ext)
    add(us, 0xe0);
  add(us, sc);
  if (ext)
    add(us + HOLD, 0xe0);
  add(us + HOLD, 0xf0);
  add(us + HOLD, sc);
  if (shift) {
    add(us + HOLD, 0xf0);
    add(us + HOLD, 0x12);
  }
  return us + HOLD + GAP;
}


//// Output

// The emulator's own renderer, into a frame of RGB pixels (8 bits a channel, from RGB222). The
// frame is the modeline's visible area with the view port placed as FabGL places it, centred on a
// 4 pixel grid, and the rest in the border colour. False when no video mode is up.
static bool renderFrame(std::vector<uint8_t> & rgb, int & fw, int & fh)
{
  using fabgl::VGADirectController;
  int w = VGADirectController::s_viewW, h = VGADirectController::s_viewH;
  if (!VGADirectController::s_callback || w <= 0 || h <= 0)
    return false;
  fw = w;
  fh = h;
  if (char const * q = VGADirectController::s_modeline ? strchr(VGADirectController::s_modeline + 1, '"') : nullptr) {
    float clock;
    int hd, hs, he, ht, vd;
    if (sscanf(q + 1, "%f %d %d %d %d %d", &clock, &hd, &hs, &he, &ht, &vd) == 6 && hd >= w && vd >= h) {
      fw = hd;
      fh = vd;
    }
  }
  int col = ((fw - w) / 2) & ~3, row = ((fh - h) / 2) & ~3;

  int lines = std::max(1, VGADirectController::s_linesPerCallback);
  std::vector<uint8_t> raw((size_t) w * (h + lines), 0);
  if (VGADirectController::s_running)
    for (int y = 0; y < h; y += lines)
      VGADirectController::s_callback(VGADirectController::s_arg, raw.data() + (size_t) y * w, y);

  auto b = VGADirectController::s_border;
  uint8_t border = b.R | (b.G << 2) | (b.B << 4);
  rgb.resize((size_t) fw * fh * 3);
  uint8_t * out = rgb.data();
  for (int y = 0; y < fh; ++y)
    for (int x = 0; x < fw; ++x) {
      bool view = y >= row && y < row + h && x >= col && x < col + w;
      uint8_t p = view ? raw[(size_t) (y - row) * w + ((x - col) ^ 2)] : border;
      *out++ = (p & 3) * 85;
      *out++ = ((p >> 2) & 3) * 85;
      *out++ = ((p >> 4) & 3) * 85;
    }
  return true;
}


static void writeShot(std::string const & file)
{
  std::vector<uint8_t> rgb;
  int fw, fh;
  if (!renderFrame(rgb, fw, fh)) {
    printf("[shot] no video mode, %s not written\n", file.c_str());
    return;
  }
  FILE * f = fopen(file.c_str(), "wb");
  if (!f) {
    printf("[shot] cannot write %s\n", file.c_str());
    return;
  }
  fprintf(f, "P6\n%d %d\n255\n", fw, fh);
  fwrite(rgb.data(), 1, rgb.size(), f);
  fclose(f);
  printf("[shot] %s, %dx%d\n", file.c_str(), fw, fh);
}


//// Checks, for tests: each failure is counted, and the run's exit code is 1 if there was any

static int s_failures = 0;

// FNV-1a over the frame's RGB bytes and its size. Prints it; with an expected value, checks it.
static void frameSum(std::string const & expected)
{
  std::vector<uint8_t> rgb;
  int fw, fh;
  uint64_t h = 14695981039346656037ull;
  if (renderFrame(rgb, fw, fh)) {
    for (uint8_t v : rgb)
      h = (h ^ v) * 1099511628211ull;
    h = (h ^ (uint64_t) fw) * 1099511628211ull;
    h = (h ^ (uint64_t) fh) * 1099511628211ull;
  } else
    fw = fh = 0;
  char sum[17];
  snprintf(sum, sizeof(sum), "%016llx", (unsigned long long) h);
  if (expected.empty())
    printf("[framesum] %s, %dx%d at %.2f s\n", sum, fw, fh, esp_timer_get_time() / 1e6);
  else if (strcasecmp(expected.c_str(), sum) == 0)
    printf("[framesum] ok %s at %.2f s\n", sum, esp_timer_get_time() / 1e6);
  else {
    printf("[framesum] FAIL: %s, expected %s, %dx%d at %.2f s\n", sum, expected.c_str(), fw, fh, esp_timer_get_time() / 1e6);
    ++s_failures;
  }
}


static char const * cp437ToUTF8(uint8_t c)
{
  static const char * const HIGH[128] = {
    "Ç","ü","é","â","ä","à","å","ç","ê","ë","è","ï","î","ì","Ä","Å",
    "É","æ","Æ","ô","ö","ò","û","ù","ÿ","Ö","Ü","¢","£","¥","₧","ƒ",
    "á","í","ó","ú","ñ","Ñ","ª","º","¿","⌐","¬","½","¼","¡","«","»",
    "░","▒","▓","│","┤","╡","╢","╖","╕","╣","║","╗","╝","╜","╛","┐",
    "└","┴","┬","├","─","┼","╞","╟","╚","╔","╩","╦","╠","═","╬","╧",
    "╨","╤","╥","╙","╘","╒","╓","╫","╪","┘","┌","█","▄","▌","▐","▀",
    "α","ß","Γ","π","Σ","σ","µ","τ","Φ","Θ","Ω","δ","∞","φ","ε","∩",
    "≡","±","≥","≤","⌠","⌡","÷","≈","°","∙","·","√","ⁿ","²","■"," ",
  };
  static char ascii[2];
  if (c >= 0x80)
    return HIGH[c - 0x80];
  ascii[0] = (c >= 0x20 && c < 0x7f) ? (char) c : (c == 0 ? ' ' : '.');
  return ascii;
}

// the text screen as UTF-8 lines, trailing blanks dropped; false when not in a text mode
static bool textScreen(std::vector<std::string> & lines, int & cols, int & rows)
{
  auto ga = s_machine->graphicsAdapter();
  auto em = ga->emulation();
  bool ega = em == GraphicsAdapter::Emulation::PC_Text_EGA_80x25_16Colors && s_machine->EGAActive();
  bool cga = em == GraphicsAdapter::Emulation::PC_Text_80x25_16Colors ||
             em == GraphicsAdapter::Emulation::PC_Text_40x25_16Colors ||
             em == GraphicsAdapter::Emulation::PC_Text_CGA_80Columns;
  uint8_t const * fb = s_machine->frameBuffer();
  cols = ga->getTextColumns();
  rows = ga->getTextRows();
  if (!(ega || (cga && fb)) || cols <= 0 || rows <= 0)
    return false;
  lines.clear();
  for (int r = 0; r < rows; ++r) {
    std::string line;
    for (int c = 0; c < cols; ++c)
      line += cp437ToUTF8(ega ? s_machine->ega()->planeByte(0, (uint32_t) r * cols + c) : fb[(r * cols + c) * 2]);
    while (!line.empty() && line.back() == ' ')
      line.pop_back();
    lines.push_back(line);
  }
  return true;
}


static void printText()
{
  std::vector<std::string> lines;
  int cols, rows;
  if (!textScreen(lines, cols, rows)) {
    printf("[text] not a text mode\n");
    return;
  }
  printf("[text] %dx%d at %.2f s\n", cols, rows, esp_timer_get_time() / 1e6);
  for (auto & line : lines)
    printf("%s\n", line.c_str());
  printf("[text] end\n");
  fflush(stdout);
}


// a check: TEXT somewhere on the text screen. On failure the screen is printed, to see why.
static void expectText(std::string const & text)
{
  std::vector<std::string> lines;
  int cols, rows;
  bool found = false;
  if (textScreen(lines, cols, rows))
    for (auto & line : lines)
      if (line.find(text) != std::string::npos)
        found = true;
  if (found) {
    printf("[expect] ok \"%s\" at %.2f s\n", text.c_str(), esp_timer_get_time() / 1e6);
    return;
  }
  printf("[expect] FAIL: \"%s\" not on the screen at %.2f s\n", text.c_str(), esp_timer_get_time() / 1e6);
  ++s_failures;
  printText();
}


static void printRegs()
{
  using fabgl::i8086;
  printf("[regs] at %.2f s: AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X\n"
         "       CS=%04X DS=%04X ES=%04X SS=%04X IP=%04X  IF=%d ZF=%d CF=%d  code",
         esp_timer_get_time() / 1e6, i8086::AX(), i8086::BX(), i8086::CX(), i8086::DX(), i8086::SI(),
         i8086::DI(), i8086::BP(), i8086::SP(), i8086::CS(), i8086::DS(), i8086::ES(), i8086::SS(),
         i8086::IP(), i8086::flagIF(), i8086::flagZF(), i8086::flagCF());
  uint32_t a = ((uint32_t) i8086::CS() << 4) + i8086::IP();
  for (int i = 0; i < 12; ++i)
    printf(" %02X", s_machine->memory()[(a + i) & 0xfffff]);
  uint8_t const * bda = s_machine->memory() + 0x400;
  printf("\n       BIOS ticks %u\n", (unsigned) (bda[0x6c] | bda[0x6d] << 8 | bda[0x6e] << 16 | bda[0x6f] << 24));
}


// guest memory as hex and text; B8000 and up is the CGA's memory, which is not in the guest RAM
static void printMem(std::string const & arg)
{
  unsigned addr = 0, len = 0;
  if (sscanf(arg.c_str(), "%x,%x", &addr, &len) != 2)
    return;
  printf("[mem] %05X, %u bytes at %.2f s\n", addr, len, esp_timer_get_time() / 1e6);
  for (unsigned a = addr; a < addr + len; a += 16) {
    char ascii[17] = { };
    printf("%05X ", a);
    for (unsigned i = 0; i < 16 && a + i < addr + len; ++i) {
      unsigned x = a + i;
      uint8_t v = (x >= 0xb8000 && x < 0xc0000 && s_machine->frameBuffer())
                  ? s_machine->videoMemory()[x - 0xb8000] : s_machine->memory()[x & 0xfffff];
      printf(" %02X", v);
      ascii[i] = (v >= 0x20 && v < 0x7f) ? v : '.';
    }
    printf("  %s\n", ascii);
  }
}


//// The run

static void step(void *)
{
  hostAdvance((uint64_t) (16 * s_nsPerInstruction));   // the machine runs 16 instructions a step
  int64_t now = esp_timer_get_time();
  if (s_stepsToPrint) {
    --s_stepsToPrint;
    printf("[step] %04X:%04X IF=%d\n", fabgl::i8086::CS(), fabgl::i8086::IP(), fabgl::i8086::flagIF());
  }
  while (s_nextEvent < s_events.size() && s_events[s_nextEvent].us <= now) {
    Event & e = s_events[s_nextEvent++];
    switch (e.kind) {
      case Event::Scancode: fabgl::PS2Controller::keyboard()->push(e.code); break;
      case Event::Shot:     writeShot(e.arg); break;
      case Event::Text:     printText(); break;
      case Event::Regs:     printRegs(); break;
      case Event::Steps:    s_stepsToPrint = e.code; break;
      case Event::Mem:      printMem(e.arg); break;
      case Event::Expect:   expectText(e.arg); break;
      case Event::FrameSum: frameSum(e.arg); break;
      case Event::End:
        if (s_checks)
          printf("[result] %s: %d of %d checks failed\n", s_failures ? "FAIL" : "ok", s_failures, s_checks);
        fflush(stdout);
        exit(s_failures ? 1 : 0);
    }
  }
}


// a private copy of an image, so a test does not change it
static std::string copyImage(char const * path)
{
  FILE * in = fopen(path, "rb");
  if (!in) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(1);
  }
  char tmpl[] = "/tmp/pctool-XXXXXX";
  int fd = mkstemp(tmpl);
  FILE * out = fdopen(fd, "wb");
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  return tmpl;
}

static int driveIndex(char const * s)
{
  if (!strcmp(s, "fd1")) return 1;
  if (!strcmp(s, "hd0")) return 2;
  if (!strcmp(s, "hd1")) return 3;
  return 0;
}


static int run(int argc, char * argv[])
{
  std::string disk[4];
  int  chs[4][3] = { };
  int  boot = 0, ram = 640;
  std::string video = "cga", egaRom;
  bool tandySound = false, tandyDAC = false, adlib = false, composite = false, hwDisk = false, write = false;
  std::string clock = "at";
  int  com = 0;
  int  egaMem = 256;
  int64_t endUs = 30000000;

  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    auto eq = a.find('=');
    std::string tag = a.substr(0, eq), val = eq == std::string::npos ? "1" : a.substr(eq + 1);
    // script values are T:ARG
    auto colon = val.find(':');
    std::string at = val.substr(0, colon), arg = colon == std::string::npos ? "" : val.substr(colon + 1);

    if (tag == "fd0" || tag == "fd1" || tag == "hd0" || tag == "hd1")
      disk[driveIndex(tag.c_str())] = val;
    else if (tag == "chs0" || tag == "chs1")
      sscanf(val.c_str(), "%d,%d,%d", &chs[tag == "chs0" ? 2 : 3][0], &chs[tag == "chs0" ? 2 : 3][1], &chs[tag == "chs0" ? 2 : 3][2]);
    else if (tag == "boot")        boot = driveIndex(val.c_str());
    else if (tag == "ram")         ram = atoi(val.c_str());
    else if (tag == "video")       video = val;
    else if (tag == "egarom")      egaRom = val;
    else if (tag == "tandysound")  tandySound = val != "0";
    else if (tag == "tandydac")    tandyDAC = val != "0";
    else if (tag == "adlib")       adlib = val != "0";
    else if (tag == "composite")   composite = val != "0";
    else if (tag == "hwdisk")      hwDisk = val != "0";
    else if (tag == "clock")       clock = val;
    else if (tag == "com")         com = atoi(val.c_str());
    else if (tag == "egamem")      egaMem = atoi(val.c_str());
    else if (tag == "mips")        s_nsPerInstruction = 1000.0 / atof(val.c_str());
    else if (tag == "write")       write = val != "0";
    else if (tag == "time")        endUs = secondsToUs(val.c_str());
    else if (tag == "text")        s_events.push_back({ secondsToUs(at.c_str()), Event::Text, 0, "" });
    else if (tag == "regs")        s_events.push_back({ secondsToUs(at.c_str()), Event::Regs, 0, "" });
    else if (tag == "steps")       s_events.push_back({ secondsToUs(at.c_str()), Event::Steps, atoi(arg.c_str()), "" });
    else if (tag == "mem")         s_events.push_back({ secondsToUs(at.c_str()), Event::Mem, 0, arg });
    else if (tag == "expect") {
      s_events.push_back({ secondsToUs(at.c_str()), Event::Expect, 0, arg });
      ++s_checks;
    } else if (tag == "framesum") {
      s_events.push_back({ secondsToUs(at.c_str()), Event::FrameSum, 0, arg });
      if (!arg.empty())
        ++s_checks;
    } else if (tag == "shot")        s_events.push_back({ secondsToUs(at.c_str()), Event::Shot, 0, arg });
    else if (tag == "type") {
      int64_t t = secondsToUs(at.c_str());
      for (size_t k = 0; k < arg.size(); ++k) {
        char c = arg[k];
        if (c == '\\' && k + 1 < arg.size() && arg[k + 1] == 'n') {
          c = '\n';
          ++k;
        }
        int code = charToScancode(c);
        if (code < 0) {
          fprintf(stderr, "type: no key for '%c'\n", c);
          return 1;
        }
        t = addKeyStroke(t, code);
      }
    } else if (tag == "hold") {
      // hold=T:NAME:SECONDS, one key held down (make, then the break that much later), for games
      // that move while a key is down
      auto colon2 = arg.find(':');
      std::string name = arg.substr(0, colon2);
      int code = keyNameToScancode(name);
      if (code < 0 || colon2 == std::string::npos) {
        fprintf(stderr, "hold: use hold=T:KEY:SECONDS\n");
        return 1;
      }
      addKeyStroke(secondsToUs(at.c_str()), code, secondsToUs(arg.substr(colon2 + 1).c_str()));
    } else if (tag == "key") {
      int64_t t = secondsToUs(at.c_str());
      size_t p = 0;
      while (p <= arg.size()) {
        size_t q = arg.find('+', p);
        if (q == p && p < arg.size())            // a '+' of its own, as the key
          q = arg.find('+', p + 1);
        std::string name = arg.substr(p, q == std::string::npos ? std::string::npos : q - p);
        int code = keyNameToScancode(name);
        if (code < 0) {
          fprintf(stderr, "key: unknown key '%s'\n", name.c_str());
          return 1;
        }
        t = addKeyStroke(t, code);
        if (q == std::string::npos)
          break;
        p = q + 1;
      }
    } else {
      fprintf(stderr, "unknown argument '%s'\n", a.c_str());
      return 1;
    }
  }
  s_events.push_back({ endUs, Event::End, 0, "" });
  std::stable_sort(s_events.begin(), s_events.end(), [](Event const & a, Event const & b) { return a.us < b.us; });

  s_machine = new Machine;
  s_machine->setBaseDirectory(".");
  if (hwDisk)
    s_machine->setHwDisk(true);
  if (clock == "mm58174")
    s_machine->setClock(Machine::Clock::MM58174);
  else if (clock == "none")
    s_machine->setClock(Machine::Clock::None);
  if (com)
    s_machine->bridgeCOM(com, nullptr, [](void *, uint8_t) { });
  s_machine->setEGAMemory(egaMem);

  for (int i = 0; i < 4; ++i) {
    if (disk[i].empty())
      continue;
    std::string path = disk[i];
    if (!write)
      path = copyImage(path.c_str());
    char full[PATH_MAX];
    if (!realpath(path.c_str(), full)) {
      fprintf(stderr, "cannot find %s\n", path.c_str());
      return 1;
    }
    s_machine->setDriveImage(i, strdup(full), chs[i][0], chs[i][1], chs[i][2]);
  }
  s_machine->setBootDrive(boot);
  s_machine->setRamSize(ram);

  if (video == "ega") {
    std::string rom = egaRom.empty() ? "data/fs/ega.bin" : egaRom;
    char full[PATH_MAX];
    if (!realpath(rom.c_str(), full)) {
      fprintf(stderr, "cannot find the EGA BIOS %s\n", rom.c_str());
      return 1;
    }
    s_machine->setEGABios(strdup(full));
  } else if (video == "tandy")
    s_machine->setTandy(true);
  if (tandySound && video != "tandy")
    s_machine->setTandySound(true);
  if (adlib)
    s_machine->setAdLib(true);
  if (composite && video == "cga")
    s_machine->setComposite(true);
  if (tandyDAC && (tandySound || video == "tandy"))
    s_machine->setTandyDAC(true);

  s_machine->setStepCallback(step);
  s_machine->run();
  hostRunTask();
  return 0;
}


int main(int argc, char * argv[])
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc >= 2 && !strcmp(argv[1], "run"))
    return run(argc - 2, argv + 2);
  fprintf(stderr, "usage: pctool run [fd0=IMG] [hd0=IMG] [video=cga|ega|tandy] [type=T:TEXT] [shot=T:FILE.ppm] [text=T] [time=T] ...\n"
                  "       (see the comment at the top of host/main.cpp)\n");
  return 1;
}
