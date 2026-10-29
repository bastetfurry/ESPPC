/*
  Serial debug console -- see serialdbg.h for what it is and how it is reached.
 */


#include "esp_rom_sys.h"
#include "compat.h"
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "fabgl.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "opl/dbopl.h"

#include "machine.h"
#include "bios.h"
#include "serialdbg.h"


using fabgl::i8086;
using fabgl::GraphicsAdapter;


// Host ASCII -> XT "set 1" scancode for the key that produces it on a US layout. Shifted
// characters carry the scancode of the unshifted key, which is what the BIOS keyboard buffer
// wants: the buffer holds a scancode in the high byte and the character in the low byte, and
// nothing downstream re-derives one from the other.
static const uint8_t ASCII_TO_SCANCODE[128] = {
  0x00, 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x0e, 0x0f, 0x24, 0x25, 0x26, 0x1c, 0x31, 0x18,
  0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x39, 0x02, 0x28, 0x04, 0x05, 0x06, 0x08, 0x28, 0x0a, 0x0b, 0x09, 0x0d, 0x33, 0x0c, 0x34, 0x35,
  0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x27, 0x27, 0x33, 0x0d, 0x34, 0x35,
  0x03, 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
  0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c, 0x1a, 0x2b, 0x1b, 0x07, 0x0c,
  0x29, 0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
  0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c, 0x1a, 0x2b, 0x1b, 0x29, 0x00,
};


// how often the mirrored screen may be repainted, and how long the guest has to change it
static constexpr uint32_t REFRESH_INTERVAL_MS = 150;


SerialDebug::SerialDebug()
  : m_machine(nullptr),
    m_state(State::Detached),
    m_escCount(0),
    m_pendingCtrlC(false),
    m_lastRefreshMS(0),
    m_lastHash(0),
    m_lastWasCR(false),
    m_dumpAddr(0x00000400)      // 0040:0000, the BIOS data area, as a harmless starting point
{
}


void SerialDebug::write(char const * s)
{
  Serial.write((uint8_t const *)s, strlen(s));
}


void SerialDebug::writeFmt(char const * fmt, ...)
{
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  write(buf);
}


void SerialDebug::begin(Machine * machine)
{
  m_machine = machine;
  if (!m_machine)
    return;

  write("\r\n");
  write("=====================================================\r\n");
  write(" ESPPC: machine booted, serial debug console on\r\n");
  write("=====================================================\r\n");
  write("  ESC ESC ESC   attach to the text screen\r\n");
  write("  Ctrl+C Ctrl+D once attached, drop into the monitor\r\n");
  write("\r\n");
  if (m_usbSerialRoute) {
    writeFmt("COM%d is carried over this serial line. Typing goes to that\r\n", m_usbSerialRoute);
    write("port instead of the keyboard; use 'k' from the monitor to\r\n");
    write("type at the guest, or 'u off' to stop.\r\n");
  } else {
    write("No COM port is on this serial line. \"usbserial com1\" in the\r\n");
    write("machine configuration, or 'u com1' from the monitor, puts one\r\n");
    write("here without taking the debug console away.\r\n");
  }
  write("\r\n");
}


// ---------------------------------------------------------------------------------------------
// state machine
// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// postbox transport
// ---------------------------------------------------------------------------------------------
//
// Host frames are   0x10 'P' <cmd> <len> <payload...>
// and are answered  0x10 'p' <cmd> <len> <payload...>
//
// 0x10 is DLE, which no key on the emulated keyboard produces, so this can be parsed before the
// byte would otherwise reach the guest or the monitor. That is what lets a file transfer run
// while the guest is running -- the monitor stops the CPU, which would stop the program doing the
// transfer -- and it also means the debugger stays usable during a transfer.
//
// Commands:
//   'O' open a host session      'C' close it
//   'S' payload -> guest         'R' payload[0] = max bytes wanted back from the guest
//   'Q' query: replies with host-open, bytes waiting for the guest, bytes waiting for the host
#define PB_DLE 0x10

bool SerialDebug::postboxFeed(uint8_t ch)
{
  switch (m_pbState) {

    case PbState::Idle:
      if (ch == PB_DLE) {
        m_pbState = PbState::Intro;
        return true;
      }
      return false;

    case PbState::Intro:
      // only 'P' makes this a frame; anything else means the DLE was not ours after all
      m_pbState = (ch == 'P') ? PbState::Cmd : PbState::Idle;
      return true;

    case PbState::Cmd:
      m_pbCmd   = ch;
      m_pbState = PbState::Len;
      return true;

    case PbState::Len:
      m_pbLen  = ch;
      m_pbFill = 0;
      if (m_pbLen == 0) {
        m_pbState = PbState::Idle;
        postboxHandleFrame();
      } else {
        m_pbState = PbState::Payload;
      }
      return true;

    case PbState::Payload:
      m_pbBuf[m_pbFill++] = ch;
      if (m_pbFill >= m_pbLen) {
        m_pbState = PbState::Idle;
        postboxHandleFrame();
      }
      return true;
  }
  return false;
}


void SerialDebug::postboxHandleFrame()
{
  uint8_t out[256];
  uint8_t n = 0;

  switch (m_pbCmd) {

    case 'O':
      m_machine->postboxToHost().clear();
      m_machine->postboxToGuest().clear();
      m_machine->setPostboxHostOpen(true);
      break;

    case 'C':
      m_machine->setPostboxHostOpen(false);
      break;

    // host -> guest. The reply says how many were taken, so the host can throttle.
    case 'S':
      for (uint8_t i = 0; i < m_pbLen; ++i)
        if (m_machine->postboxToGuest().push(m_pbBuf[i]))
          ++n;
      out[0] = n;
      n = 1;
      break;

    // guest -> host
    case 'R':
    {
      uint8_t want = m_pbLen ? m_pbBuf[0] : 0;
      if (want > sizeof(out))
        want = sizeof(out);
      uint8_t v;
      while (n < want && m_machine->postboxToHost().pop(&v))
        out[n++] = v;
      break;
    }

    case 'Q':
      out[0] = m_machine->postboxHostOpen() ? 1 : 0;
      out[1] = (uint8_t) m_machine->postboxToGuest().count();
      out[2] = (uint8_t) m_machine->postboxToHost().count();
      n = 3;
      break;

    default:
      m_pbCmd = '?';
      break;
  }

  uint8_t hdr[4] = { PB_DLE, 'p', m_pbCmd, n };
  Serial.write(hdr, 4);
  if (n)
    Serial.write(out, n);
  Serial.flush();
}


void SerialDebug::poll()
{
  if (!m_machine)
    return;

  while (Serial.available()) {

    uint8_t ch = (uint8_t) Serial.read();

    // postbox frames are claimed before anything else can consume the byte
    if (postboxFeed(ch))
      continue;

    switch (m_state) {

      case State::Detached:
        // nothing reaches the guest here, so three ESCs in a row are unambiguous
        if (ch == 0x1b) {
          if (++m_escCount >= 3) {
            m_escCount = 0;
            m_state    = State::Screen;
            m_lastHash = 0;
            write("\x1b[2J\x1b[H");
            write("--- attached to the text screen, Ctrl+C Ctrl+D for the monitor ---\r\n");
            refreshScreen(true);
          }
        } else {
          m_escCount = 0;
        }
        break;

      case State::Screen:
        if (m_pendingCtrlC) {
          m_pendingCtrlC = false;
          if (ch == 0x04) {
            m_state = State::Monitor;
            runMonitor();
            break;
          }
          // it was an ordinary Ctrl+C after all, so let the guest have it, then this byte
          sendKeyToGuest(0x03);
        }
        if (ch == 0x03) {
          m_pendingCtrlC = true;      // hold it back until we know what follows
        } else if (m_usbSerialRoute == 1) {
          m_machine->com1()->injectRx(ch);
        } else if (m_usbSerialRoute == 2) {
          m_machine->com2()->injectRx(ch);
        } else {
          sendKeyToGuest(ch);
        }
        break;

      case State::Monitor:
        // runMonitor() owns the line while it is active, so this is unreachable
        break;
    }
  }

  if (m_state == State::Screen && !m_machine->postboxHostOpen() && m_usbSerialRoute == 0)
    refreshScreen(false);
}


void SerialDebug::breakNow()
{
  if (!m_machine)
    return;
  write("\r\n--- breakpoint ---\r\n");
  m_state = State::Monitor;
  runMonitor();
}


// ---------------------------------------------------------------------------------------------
// screen mirroring
// ---------------------------------------------------------------------------------------------

// Reports the text geometry, and whether the characters live in a linear CGA buffer or in the
// EGA's plane 0. buffer is only meaningful for the CGA case.
bool SerialDebug::textMode(uint8_t const * * buffer, int * columns, int * rows, bool * ega)
{
  auto ga = m_machine->graphicsAdapter();
  auto em = ga->emulation();

  *ega = false;

  if (em == GraphicsAdapter::Emulation::PC_Text_EGA_80x25_16Colors && m_machine->EGAActive()) {
    *ega     = true;
    *buffer  = nullptr;
    *columns = ga->getTextColumns();
    *rows    = ga->getTextRows();
    return *columns > 0 && *rows > 0;
  }

  if (em != GraphicsAdapter::Emulation::PC_Text_80x25_16Colors &&
      em != GraphicsAdapter::Emulation::PC_Text_40x25_16Colors)
    return false;

  auto fb = m_machine->frameBuffer();
  if (!fb)
    return false;

  *buffer  = fb;
  *columns = ga->getTextColumns();
  *rows    = ga->getTextRows();

  return *columns > 0 && *rows > 0;
}


// CP437 is not worth a full table here. Printable ASCII passes through, the box drawing
// characters become the nearest ASCII thing, and everything else becomes a dot so that the
// column alignment of the original screen survives.
char SerialDebug::cp437ToASCII(uint8_t c)
{
  if (c >= 0x20 && c <= 0x7e)
    return (char) c;

  switch (c) {
    case 0x00: case 0xff: return ' ';
    case 0xb3: case 0xba: return '|';
    case 0xc4: case 0xcd: return '-';
    case 0xda: case 0xbf: case 0xc0: case 0xd9:
    case 0xc9: case 0xbb: case 0xc8: case 0xbc:
    case 0xc3: case 0xb4: case 0xc2: case 0xc1: case 0xc5:
    case 0xcc: case 0xb9: case 0xcb: case 0xca: case 0xce: return '+';
    case 0xb0: case 0xb1: case 0xb2: case 0xdb: return '#';
    case 0x07: return 'o';
    case 0x10: return '>';
    case 0x11: return '<';
    case 0x1a: return '>';
    case 0x1b: return '<';
    case 0x18: return '^';
    case 0x19: return 'v';
    default:   return '.';
  }
}


void SerialDebug::refreshScreen(bool force)
{
  uint32_t now = millis();
  if (!force && (uint32_t)(now - m_lastRefreshMS) < REFRESH_INTERVAL_MS)
    return;
  m_lastRefreshMS = now;

  uint8_t const * fb;
  int columns, rows;
  bool ega;

  if (!textMode(&fb, &columns, &rows, &ega)) {
    // A graphics mode has nothing to mirror. Say so once rather than every refresh.
    uint32_t h = 0x9e3779b9;
    if (h != m_lastHash) {
      m_lastHash = h;
      write("\x1b[H\x1b[2J");
      writeFmt("--- guest is in a graphics mode, nothing to mirror ---\r\n");
    }
    return;
  }

  // FNV-1a over the character cells and the cursor, so an unchanged screen costs one pass and
  // no serial traffic. At 115200 baud a full repaint is about 190 ms of line time, which is why
  // this matters rather than just repainting on a timer.
  // one place to fetch a character cell, whichever kind of text mode this is
  auto charAt = [&](int r, int c) -> uint8_t {
    if (ega)
      return m_machine->ega()->planeByte(0, (uint32_t)r * columns + c);
    return fb[(r * columns + c) * 2];
  };

  uint32_t hash = 2166136261u;
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < columns; ++c)
      hash = (hash ^ charAt(r, c)) * 16777619u;

  if (hash == m_lastHash && !force)
    return;
  m_lastHash = hash;

  char line[132];

  write("\x1b[H");
  for (int r = 0; r < rows; ++r) {
    int n = 0;
    for (int c = 0; c < columns && n < (int)sizeof(line) - 1; ++c)
      line[n++] = cp437ToASCII(charAt(r, c));
    // trailing blanks carry no information and cost line time
    while (n > 0 && line[n - 1] == ' ')
      --n;
    line[n] = 0;
    write(line);
    write("\x1b[K");
    // no newline after the last row: it would scroll the mirror by one line every repaint
    if (r < rows - 1)
      write("\r\n");
  }
}


// ---------------------------------------------------------------------------------------------
// key injection
// ---------------------------------------------------------------------------------------------

void SerialDebug::sendKeyToGuest(uint8_t ch)
{
  // Terminals disagree about Enter: some send CR, some LF, some CRLF. Accept all three and
  // collapse a CRLF pair into one keystroke.
  if (ch == 0x0a) {
    if (m_lastWasCR) {
      m_lastWasCR = false;
      return;
    }
    ch = 0x0d;
  } else {
    m_lastWasCR = (ch == 0x0d);
  }

  if (ch == 0x7f)
    ch = 0x08;              // DEL from the terminal means Backspace here

  if (ch >= 128)
    return;

  uint8_t scancode = ASCII_TO_SCANCODE[ch];
  if (scancode == 0)
    return;

  // BIOS keyboard buffer entry: scancode in the high byte, character in the low byte
  m_machine->bios()->injectKey(((uint16_t)scancode << 8) | ch);
}


// ---------------------------------------------------------------------------------------------
// monitor
// ---------------------------------------------------------------------------------------------

uint32_t SerialDebug::parseHex(char const * s, bool * ok)
{
  uint32_t v = 0;
  bool any = false;
  while (*s == ' ')
    ++s;
  while (*s) {
    int d;
    if (*s >= '0' && *s <= '9')      d = *s - '0';
    else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
    else break;
    v = v * 16 + d;
    any = true;
    ++s;
  }
  if (ok)
    *ok = any;
  return v;
}


// accepts "seg:off" or a flat linear address, both hex
bool SerialDebug::parseAddress(char const * s, uint32_t * addr)
{
  while (*s == ' ')
    ++s;
  if (!*s)
    return false;

  bool ok = false;
  uint32_t first = parseHex(s, &ok);
  if (!ok)
    return false;

  char const * colon = strchr(s, ':');
  if (colon) {
    bool ok2 = false;
    uint32_t off = parseHex(colon + 1, &ok2);
    if (!ok2)
      return false;
    *addr = ((first & 0xffff) << 4) + (off & 0xffff);
  } else {
    *addr = first;
  }

  return *addr < RAM_SIZE;
}


bool SerialDebug::readLine(char * buf, int bufSize)
{
  int n = 0;
  while (true) {
    while (!Serial.available())
      vTaskDelay(1);        // the guest is frozen here, so yield rather than spin

    uint8_t ch = (uint8_t) Serial.read();

    if (ch == '\r' || ch == '\n') {
      write("\r\n");
      buf[n] = 0;
      return true;
    }
    if (ch == 0x08 || ch == 0x7f) {
      if (n > 0) {
        --n;
        write("\b \b");
      }
      continue;
    }
    if (ch == 0x03) {       // Ctrl+C abandons the line
      write("^C\r\n");
      buf[0] = 0;
      return false;
    }
    if (ch >= 0x20 && ch < 0x7f && n < bufSize - 1) {
      buf[n++] = (char) ch;
      Serial.write(ch);
    }
  }
}


void SerialDebug::help()
{
  write("  r                 registers\r\n");
  write("  d [addr] [len]    dump memory, addr is seg:off or linear hex\r\n");
  write("  e addr bb bb ..   write bytes\r\n");
  write("  w reg value       set a CPU register\r\n");
  write("  k word [word ..]  inject raw BIOS key words (scancode<<8 | ascii)\r\n");
  write("  z                 reboot the guest, as Ctrl+Alt+Del does\r\n");
  write("  l addr len        load raw bytes straight into guest memory\r\n");
  write("  j seg:off         point CS:IP at an address, then 'g' to run it\r\n");
  write("  u [off|com1|com2] carry an emulated COM port over this USB serial\r\n");
  write("  f bb bb ..        search conventional memory for a byte sequence\r\n");
  write("  p                 dump the framebuffer as a binary stream\r\n");
  write("  v                 report what the video side is driving\r\n");
  write("  h                 heap map: every RAM region, free, largest block, low water\r\n");
  write("  b [addr]          one-shot breakpoint on a linear address (no addr clears)\r\n");
  write("  x addr len        stream guest memory as raw binary\r\n");
  write("  i port            read an I/O port\r\n");
  write("  o port val        write an I/O port\r\n");
  write("  t lo hi [r|w|rw] [int lo [hi]]\r\n");
  write("                    trace port accesses lo..hi (default writes), optionally INTs too\r\n");
  write("  t                 show the trace so far;  t off  stop it and free the buffer\r\n");
  write("  c [s]             free CPU time on the video core, measured over s seconds (default 2)\r\n");
  write("  a                 benchmark the OPL2 synthesiser: all nine channels, one second of samples\r\n");
  write("  m on|off          video memory statistics: count and time the guest's accesses; m  report\r\n");
  write("  y on|off          per opcode CPU profile (cycles and counts); y  report the top 24\r\n");
  write("  s [n]             single step n instructions, then show registers\r\n");
  write("  g                 resume, back to the screen\r\n");
  write("  q                 resume and detach completely\r\n");
  write("  ?                 this list\r\n");
}


void SerialDebug::cmdRegisters()
{
  writeFmt("AX=%04X BX=%04X CX=%04X DX=%04X  SI=%04X DI=%04X BP=%04X SP=%04X\r\n",
           i8086::AX(), i8086::BX(), i8086::CX(), i8086::DX(),
           i8086::SI(), i8086::DI(), i8086::BP(), i8086::SP());
  writeFmt("CS=%04X DS=%04X ES=%04X SS=%04X  IP=%04X\r\n",
           i8086::CS(), i8086::DS(), i8086::ES(), i8086::SS(), i8086::IP());
  writeFmt("ticks=%u resetPending=%d\r\n",
           (unsigned) m_machine->ticksCounter(), (int) m_machine->resetPending());
  writeFmt("flags: %c%c%c%c%c%c%c%c%c\r\n",
           i8086::flagOF() ? 'O' : '-', i8086::flagDF() ? 'D' : '-',
           i8086::flagIF() ? 'I' : '-', i8086::flagTF() ? 'T' : '-',
           i8086::flagSF() ? 'S' : '-', i8086::flagZF() ? 'Z' : '-',
           i8086::flagAF() ? 'A' : '-', i8086::flagPF() ? 'P' : '-',
           i8086::flagCF() ? 'C' : '-');
}


void SerialDebug::cmdDump(char const * args)
{
  uint32_t addr = m_dumpAddr;
  uint32_t len  = 128;

  if (*args) {
    if (!parseAddress(args, &addr)) {
      write("bad address\r\n");
      return;
    }
    char const * sp = strchr(args, ' ');
    if (sp) {
      bool ok = false;
      uint32_t l = parseHex(sp, &ok);
      if (ok && l > 0)
        len = l;
    }
  }

  if (len > 1024)
    len = 1024;

  for (uint32_t i = 0; i < len; i += 16) {
    uint32_t a = addr + i;
    if (a >= RAM_SIZE)
      break;
    writeFmt("%05X  ", (unsigned)a);
    char asc[17];
    for (int j = 0; j < 16; ++j) {
      uint8_t b = (a + j < RAM_SIZE) ? m_machine->readMemDebug(a + j) : 0;
      writeFmt("%02X ", b);
      asc[j] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
    }
    asc[16] = 0;
    writeFmt(" |%s|\r\n", asc);
  }

  m_dumpAddr = addr + len;
}


void SerialDebug::cmdEnter(char const * args)
{
  uint32_t addr;
  if (!parseAddress(args, &addr)) {
    write("bad address\r\n");
    return;
  }

  char const * p = strchr(args, ' ');
  if (!p) {
    write("no bytes given\r\n");
    return;
  }

  int written = 0;

  while (p && *p) {
    while (*p == ' ')
      ++p;
    if (!*p)
      break;
    bool ok = false;
    uint32_t b = parseHex(p, &ok);
    if (!ok)
      break;
    if (addr + written < RAM_SIZE)
      m_machine->writeMemDebug(addr + written, (uint8_t)b);
    ++written;
    p = strchr(p, ' ');
  }

  writeFmt("%d byte(s) written at %05X\r\n", written, (unsigned)addr);
}


// f bb bb bb ...   search conventional memory for a byte sequence
//
// Needed because the interesting code in a DOS guest is often not on disk in any readable form
// -- QBASIC.EXE is PKLITE compressed, so the only place its real instructions exist is in RAM
// once DOS has unpacked it. Searching the live machine gets at them.
void SerialDebug::cmdFind(char const * args)
{
  uint8_t pattern[16];
  int patLen = 0;

  char const * p = args;
  while (*p && patLen < (int)sizeof(pattern)) {
    while (*p == ' ')
      ++p;
    if (!*p)
      break;
    bool ok = false;
    uint32_t b = parseHex(p, &ok);
    if (!ok)
      break;
    pattern[patLen++] = (uint8_t) b;
    char const * sp = strchr(p, ' ');
    if (!sp)
      break;
    p = sp;
  }

  if (patLen == 0) {
    write("usage: f bb bb bb ...\r\n");
    return;
  }

  // conventional memory only: that is where a DOS program and its data live
  constexpr uint32_t START = 0x00400;
  constexpr uint32_t END   = 0xa0000;

  auto mem = m_machine->memory();
  int hits = 0;

  for (uint32_t a = START; a + patLen <= END; ++a) {
    if (mem[a] != pattern[0])
      continue;
    int i = 1;
    while (i < patLen && mem[a + i] == pattern[i])
      ++i;
    if (i == patLen) {
      writeFmt("  %05X  (%04X:%04X)\r\n", (unsigned)a, (unsigned)(a >> 4), (unsigned)(a & 0x0f));
      if (++hits >= 24) {
        write("  ... more hits, stopping\r\n");
        break;
      }
    }
  }

  writeFmt("%d hit(s)\r\n", hits);
}


// p -- dump the framebuffer as a binary stream.
//
// The screen mirror can only show text modes, which is useless precisely when working on
// graphics. This writes a one line header and then the raw framebuffer, so the host can rebuild
// the picture exactly rather than guess at it from ASCII art. The guest is frozen while the
// monitor is open, so the frame is stable.
void SerialDebug::cmdScreenshot()
{
  auto ga = m_machine->graphicsAdapter();

  if (m_machine->EGAActive()) {

    int      w      = ga->getGraphWidth();
    int      h      = ga->getGraphHeight();
    int      stride = w / 8;
    uint32_t bytes  = (uint32_t) stride * h * 4;      // four planes per byte-column

    writeFmt("SCREENSHOT fmt=ega w=%d h=%d stride=%d bytes=%u pal=", w, h, stride, (unsigned)bytes);
    for (int i = 0; i < 16; ++i)
      writeFmt("%02x", m_machine->egaPalette()[i]);
    write("\r\n");

    auto ega = m_machine->ega();
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < stride; ++x) {
        uint32_t v = *ega->bankPtr((uint32_t)y * stride + x);
        uint8_t  b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        Serial.write(b, 4);
      }
      Serial.flush();
      vTaskDelay(1);        // let the idle task run; this is tens of KB down a 115200 line
    }

    write("\r\nSCREENSHOTEND\r\n");
    return;
  }

  auto em = ga->emulation();
  auto fb = m_machine->frameBuffer();

  if (fb && (em == GraphicsAdapter::Emulation::PC_Graphics_320x200_4Colors ||
             em == GraphicsAdapter::Emulation::PC_Graphics_640x200_2Colors)) {

    bool   four   = (em == GraphicsAdapter::Emulation::PC_Graphics_320x200_4Colors);
    int    w      = four ? 320 : 640;
    int    h      = 200;
    uint32_t bytes = 16384;                  // including the interleaved bank gaps

    writeFmt("SCREENSHOT fmt=%s w=%d h=%d stride=%d bytes=%u pal=\r\n",
             four ? "cga4" : "cga2", w, h, w / (four ? 4 : 8), (unsigned)bytes);

    for (uint32_t i = 0; i < bytes; i += 256) {
      Serial.write(fb + i, 256);
      Serial.flush();
      vTaskDelay(1);
    }

    write("\r\nSCREENSHOTEND\r\n");
    return;
  }

  // Tandy 16 colours: the whole 32 or 16 KB with its bank gaps, plus the 16 palette registers
  // (4 bit IRGB) after the palette mask has been applied, so the decoder only has to look them up
  if (fb && (em == GraphicsAdapter::Emulation::PC_Graphics_Tandy_320x200_16Colors ||
             em == GraphicsAdapter::Emulation::PC_Graphics_Tandy_160x200_16Colors)) {

    bool     mode9 = (em == GraphicsAdapter::Emulation::PC_Graphics_Tandy_320x200_16Colors);
    uint32_t bytes = mode9 ? 32768 : 16384;
    auto     arr   = m_machine->tandyArray();

    writeFmt("SCREENSHOT fmt=%s w=%d h=200 stride=%d bytes=%u pal=",
             mode9 ? "tandy9" : "tandy8", mode9 ? 320 : 160, mode9 ? 160 : 80, (unsigned)bytes);
    for (int i = 0; i < 16; ++i)
      writeFmt("%02x", arr[0x10 + (i & arr[1])]);
    write("\r\n");

    for (uint32_t i = 0; i < bytes; i += 256) {
      Serial.write(fb + i, 256);
      Serial.flush();
      vTaskDelay(1);
    }

    write("\r\nSCREENSHOTEND\r\n");
    return;
  }

  // Plantronics: both planes, 32 KB. The 640x200x4 palette is the CGA one, so its header carries
  // the colour register and the mode register for the decoder to pick the same four colours.
  if (fb && (em == GraphicsAdapter::Emulation::PC_Graphics_Plantronics_320x200_16Colors ||
             em == GraphicsAdapter::Emulation::PC_Graphics_Plantronics_640x200_4Colors)) {

    bool sixteen = (em == GraphicsAdapter::Emulation::PC_Graphics_Plantronics_320x200_16Colors);
    writeFmt("SCREENSHOT fmt=%s w=%d h=200 stride=80 bytes=32768 pal=%02x%02x\r\n",
             sixteen ? "cplus16" : "cplus4", sixteen ? 320 : 640,
             m_machine->cgaColorReg(), m_machine->cgaModeReg());
    for (uint32_t i = 0; i < 32768; i += 256) {
      Serial.write(fb + i, 256);
      Serial.flush();
      vTaskDelay(1);
    }
    write("\r\nSCREENSHOTEND\r\n");
    return;
  }

  // composite: the CGA memory as it is, plus which artifact table the adapter uses (0 = 640x200,
  // 1/2 = 320x200 palette 0/1)
  if (fb && em == GraphicsAdapter::Emulation::PC_Graphics_CGA_Composite) {
    int table = (m_machine->cgaModeReg() & 0x10) ? 0 : ((m_machine->cgaColorReg() & 0x20) ? 2 : 1);
    writeFmt("SCREENSHOT fmt=comp w=320 h=200 stride=80 bytes=16384 pal=%02x\r\n", table);
    for (uint32_t i = 0; i < 16384; i += 256) {
      Serial.write(fb + i, 256);
      Serial.flush();
      vTaskDelay(1);
    }
    write("\r\nSCREENSHOTEND\r\n");
    return;
  }

  write("text mode -- attach the screen mirror instead\r\n");
}


// x addr len -- stream a region of guest memory as raw binary.
//
// The hex dump is fine for a few lines but hopeless for reading a whole program: a DOS binary is
// hundreds of KB, and at three characters per byte the hex form is slower than the serial line
// deserves. This writes a header and then the bytes, so the host can pull a segment in seconds
// and disassemble it properly.
void SerialDebug::cmdExport(char const * args)
{
  uint32_t addr;
  if (!parseAddress(args, &addr)) {
    write("usage: x addr len\r\n");
    return;
  }

  char const * sp = strchr(args, ' ');
  bool ok = false;
  uint32_t len = sp ? parseHex(sp, &ok) : 0;
  if (!ok || len == 0) {
    write("usage: x addr len\r\n");
    return;
  }
  if (addr + len > RAM_SIZE)
    len = RAM_SIZE - addr;

  writeFmt("EXPORT addr=%05X bytes=%u\r\n", (unsigned)addr, (unsigned)len);

  auto mem = m_machine->memory();
  for (uint32_t i = 0; i < len; i += 256) {
    uint32_t n = (len - i < 256) ? (len - i) : 256;
    Serial.write(mem + addr + i, n);
    Serial.flush();
    vTaskDelay(1);
  }

  write("\r\nEXPORTEND\r\n");
}


// h -- the allocator's own view of every region it manages. MALLOC_CAP_32BIT covers them all:
// the byte-addressable DRAM regions and the leftover IRAM that is only usable for aligned 32 bit
// data. PSRAM is not in the heap on this machine (see setup()), so it does not appear. The
// "minimum free" column is the low water mark since boot, which shows how close a region has come
// to running out even when it looks roomy now.
void SerialDebug::cmdHeapMap()
{
  write("internal 8 bit  ");
  writeFmt("free %u, largest %u, low water %u\r\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  write("dma capable     ");
  writeFmt("free %u, largest %u\r\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_DMA),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  write("32 bit only     ");
  writeFmt("free %u  (IRAM leftovers, aligned word access only; the 0x4008/0x4009 region below)\r\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_32BIT) - heap_caps_get_free_size(MALLOC_CAP_8BIT)));
  write("--- per region (ESP-IDF heap_caps_print_heap_info) ---\r\n");
  fflush(stdout);
  heap_caps_print_heap_info(MALLOC_CAP_32BIT);
  fflush(stdout);
  write("--- end ---\r\n");
}


// v -- what the video side currently believes it is driving. Useful when the guest is provably
// writing the right thing but nothing reaches the monitor.
void SerialDebug::cmdVideoState()
{
  auto ga = m_machine->graphicsAdapter();

  writeFmt("emulation   %d\r\n", (int) ga->emulation());
  writeFmt("videoEnabled %d\r\n", (int) ga->videoEnabled());
  writeFmt("viewport    %dx%d\r\n", ga->viewPortWidth(), ga->viewPortHeight());
  writeFmt("text        %d cols, %d rows, %d scanlines/char\r\n",
           ga->getTextColumns(), ga->getTextRows(), ga->egaCharHeight());
  writeFmt("EGA active  %d, window %05X..%05X, oddEven %d\r\n",
           (int) m_machine->EGAActive(), (unsigned) m_machine->ega()->mapBase(),
           (unsigned) m_machine->ega()->mapEnd(), (int) m_machine->ega()->oddEven());
  // Total free is not the whole story: a big allocation needs one contiguous block, so a heap that
  // is large but chopped up still fails it. Report both, for internal and DMA capable memory.
  writeFmt("heap intern %d free, %d largest\r\n",
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  writeFmt("heap dma    %d free, %d largest\r\n",
           heap_caps_get_free_size(MALLOC_CAP_DMA),
           heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  writeFmt("heap psram  %d free, %d largest\r\n",
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  {
    // The SysReq menu is a text screen over the running guest: a 4000 byte cell buffer, the
    // 4096 byte font copy and the VGA line buffers, 8 lines of 640 bytes in DMA capable RAM, plus
    // 4000 bytes per open pop up. Opening it from a 320 wide mode also takes the 640x480 timing's
    // larger buffers, so this is a rough guide rather than a guarantee.
    size_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t freeTotal  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    writeFmt("sysreq menu %s\r\n",
             (dmaLargest >= 8 * 640 && freeTotal >= 24000) ? "has room to open" : "MAY NOT HAVE ROOM TO OPEN");
  }

  // Is the scanline interrupt still running? VSync is set inside it, so if the DMA has stopped
  // this never changes no matter how long we watch. Sampled for about 100ms, several frames.
  {
    bool first = ga->VSync();
    bool moved = false;
    for (int i = 0; i < 2000 && !moved; ++i) {
      if (ga->VSync() != first)
        moved = true;
      esp_rom_delay_us(50);
    }
    writeFmt("scanline ISR %s\r\n", moved ? "running" : "STOPPED");

    // Timing since the last "v": the longest run, the shortest gap between runs (a short one means
    // a run started late), and how many runs the DMA overtook -- each of those is a visible glitch.
    using fabgl::VGADirectController;
    uint32_t maxC = VGADirectController::s_isrMaxCycles, minP = VGADirectController::s_isrMinPeriod;
    writeFmt("scanline ISR longest %u us, shortest gap %u us, overtaken by DMA %u (since last v)\r\n",
             (unsigned)(maxC / 240), minP == 0xffffffff ? 0u : (unsigned)(minP / 240),
             (unsigned) VGADirectController::s_isrOverruns);
    VGADirectController::s_isrMaxCycles = 0;
    VGADirectController::s_isrMinPeriod = 0xffffffff;
    VGADirectController::s_isrOverruns  = 0;
  }

  // A picture that is generated correctly but shows black is usually the colour table: every
  // attribute resolving to the same raw value looks exactly like a dead screen.
  write("AC palette  ");
  for (int i = 0; i < 16; ++i)
    writeFmt("%02x ", ga->egaPaletteEntry(i));
  write("\r\n");

  write("raw pixels  ");
  for (int i = 0; i < 16; ++i)
    writeFmt("%02x ", (unsigned)(ga->egaRawPair(i) & 0xff));
  write("\r\n");

  // The mirror reads plane 0, so it proves the characters are there but says nothing about the
  // glyphs. If plane 2 is blank the monitor shows black while the mirror looks perfect.
  if (m_machine->EGAActive()) {
    write("glyph 'A'   ");
    for (int r = 0; r < 16; ++r)
      writeFmt("%02x ", m_machine->ega()->planeByte(2, 0x41 * 32 + r));
    write("\r\n");
    write("cell 0      ");
    for (int i = 0; i < 4; ++i)
      writeFmt("p%d=%02x ", i, m_machine->ega()->planeByte(i, 0));
    write("\r\n");
  }
}


// w reg value -- set a CPU register.
//
// Needed because patching memory is not always enough: a routine that returns a computed value
// in a register hands the caller that register, not whatever the value was also written to. The
// only way to change what the caller sees is to change the register at the point it is produced.
// The screen mirror can only send characters that appear in ASCII_TO_SCANCODE, which leaves out
// exactly the keys a full screen application is driven by: function keys, ALT combinations, the
// cursor pad. This injects a raw BIOS keyboard buffer word instead, so anything reachable from the
// keyboard is reachable from here. Useful words: 3b00..4400 = F1..F10, 5400..5d00 = shift+F1..F10,
// 1e00 = ALT+A, 2000 = ALT+D, 4800/5000/4b00/4d00 = cursor up/down/left/right, 1c0d = Enter.
void SerialDebug::cmdInjectKey(char const * args)
{
  int sent = 0;
  while (true) {
    while (*args == ' ')
      ++args;
    if (!*args)
      break;
    bool ok = false;
    uint32_t v = parseHex(args, &ok);
    if (!ok)
      break;
    m_machine->bios()->injectKey((uint16_t) v);
    ++sent;
    while (*args && *args != ' ')
      ++args;
  }

  if (sent == 0) {
    write("usage: k word [word ..]   e.g. 'k 5400' for shift+F1\r\n");
    return;
  }
  writeFmt("injected %d key word(s)\r\n", sent);
}


// Drop a blob straight into guest memory. "l addr len", then exactly len raw bytes on the wire.
// Nothing is echoed while the bytes are arriving, so the host can push a whole COM file in one go
// and then start it with "j", which together replace typing a program in through COPY CON.
void SerialDebug::cmdLoad(char const * args)
{
  uint32_t addr = 0;
  if (!parseAddress(args, &addr)) {
    write("usage: l addr len   then send exactly len raw bytes\r\n");
    return;
  }
  while (*args && *args != ' ')
    ++args;

  bool ok = false;
  uint32_t len = parseHex(args, &ok);
  if (!ok || len == 0 || len > 0x10000) {
    write("usage: l addr len   (len is hex, 1..10000)\r\n");
    return;
  }

  writeFmt("send %u bytes now\r\n", (unsigned) len);

  uint32_t got  = 0;
  uint32_t idle = millis();
  while (got < len) {
    if (Serial.available()) {
      m_machine->writeMemDebug(addr + got, (uint8_t) Serial.read());
      ++got;
      idle = millis();
    } else if (millis() - idle > 4000) {
      break;      // the host went away; report what arrived rather than hang the console
    }
  }

  writeFmt("%u of %u byte(s) loaded at %05X\r\n", (unsigned) got, (unsigned) len, (unsigned) addr);
}


// Point the CPU at an address and let it run. Takes seg:off, because anything worth starting this
// way cares which segment it runs in.
void SerialDebug::cmdJump(char const * args)
{
  uint32_t addr = 0;
  if (!parseAddress(args, &addr)) {
    write("usage: j seg:off\r\n");
    return;
  }

  uint16_t seg, off;
  if (strchr(args, ':')) {
    bool ok = false;
    seg = (uint16_t) parseHex(args, &ok);
    char const * p = strchr(args, ':') + 1;
    off = (uint16_t) parseHex(p, &ok);
  } else {
    seg = (uint16_t) (addr >> 4);
    off = (uint16_t) (addr & 0x0f);
  }

  i8086::setCS(seg);
  i8086::setIP(off);
  writeFmt("CS:IP set to %04X:%04X\r\n", seg, off);
}


// ---------------------------------------------------------------------------------------------
// USB serial routing
// ---------------------------------------------------------------------------------------------
//
// COM1 shares pins 3/1 with the USB serial console, so historically a machine got one or the
// other. When the console is off, an emulated port takes those pins directly through a real
// SerialPort and nothing here is involved. When the console is on it owns the UART, and this
// carries a chosen port's bytes in software instead: guest output is written to the host, and
// host input is handed to the chip with injectRx(). Both stay alive, which is the point -- a
// transfer or a terminal session does not cost you the debugger.
//
// While a port is routed, bytes that would have gone to the emulated keyboard go to that port
// instead. Ctrl+C Ctrl+D still reaches the monitor and postbox frames are still claimed first, so
// there is always a way back. Use 'k' from the monitor to type at the guest meanwhile.

void SerialDebug::bridgeTX(void * context, uint8_t value)
{
  Serial.write(value);
}


void SerialDebug::setUsbSerialRoute(int port)
{
  if (!m_machine)
    return;

  m_usbSerialRoute = port;
  m_machine->bridgeCOM(port, this, bridgeTX);
}


void SerialDebug::cmdUsbSerial(char const * args)
{
  while (*args == ' ')
    ++args;

  if (*args) {
    // a port with a physical UART behind it (the U1 header) is not the USB serial's to take
    int want = !strncmp(args, "com1", 4) ? 1 : (!strncmp(args, "com2", 4) ? 2 : 0);
    if (want && (want == 1 ? m_machine->com1() : m_machine->com2())->hasSerialPort()) {
      writeFmt("COM%d is on the U1 serial header, it cannot go over USB as well\r\n", want);
      return;
    }
    if      (!strncmp(args, "off",  3)) setUsbSerialRoute(0);
    else if (!strncmp(args, "com1", 4)) setUsbSerialRoute(1);
    else if (!strncmp(args, "com2", 4)) setUsbSerialRoute(2);
    else {
      write("usage: u [off|com1|com2]\r\n");
      return;
    }
  }

  writeFmt("USB serial carries: %s\r\n",
           m_usbSerialRoute == 1 ? "COM1" : (m_usbSerialRoute == 2 ? "COM2" : "nothing"));
}


void SerialDebug::cmdWriteReg(char const * args)
{
  char name[4] = { 0, 0, 0, 0 };
  int n = 0;
  while (*args == ' ')
    ++args;
  while (*args && *args != ' ' && n < 3) {
    char c = *args++;
    name[n++] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
  }

  bool ok = false;
  uint32_t v = parseHex(args, &ok);
  if (!ok || n == 0) {
    write("usage: w reg value   (ax bx cx dx si di bp sp cs ds es ss ip al ah)\r\n");
    return;
  }

  uint16_t w16 = (uint16_t) v;
  uint8_t  w8  = (uint8_t) v;

  if      (!strcmp(name, "ax")) i8086::setAX(w16);
  else if (!strcmp(name, "bx")) i8086::setBX(w16);
  else if (!strcmp(name, "cx")) i8086::setCX(w16);
  else if (!strcmp(name, "dx")) i8086::setDX(w16);
  else if (!strcmp(name, "di")) i8086::setDI(w16);
  else if (!strcmp(name, "sp")) i8086::setSP(w16);
  else if (!strcmp(name, "cs")) i8086::setCS(w16);
  else if (!strcmp(name, "ds")) i8086::setDS(w16);
  else if (!strcmp(name, "es")) i8086::setES(w16);
  else if (!strcmp(name, "ss")) i8086::setSS(w16);
  else if (!strcmp(name, "ip")) i8086::setIP(w16);
  else if (!strcmp(name, "al")) i8086::setAL(w8);
  else if (!strcmp(name, "ah")) i8086::setAH(w8);
  else {
    writeFmt("no such register \"%s\"\r\n", name);
    return;
  }

  writeFmt("%s = %04X\r\n", name, (unsigned) v);
}


void SerialDebug::cmdInPort(char const * args)
{
  bool ok = false;
  uint32_t port = parseHex(args, &ok);
  if (!ok) {
    write("bad port\r\n");
    return;
  }
  writeFmt("in  %04X = %02X\r\n", (unsigned)port, m_machine->readPortDebug(port));
}


void SerialDebug::cmdOutPort(char const * args)
{
  bool ok = false;
  uint32_t port = parseHex(args, &ok);
  char const * sp = strchr(args, ' ');
  bool ok2 = false;
  uint32_t val = sp ? parseHex(sp, &ok2) : 0;
  if (!ok || !ok2) {
    write("usage: o port val\r\n");
    return;
  }
  m_machine->writePortDebug(port, (uint8_t)val);
  writeFmt("out %04X <= %02X\r\n", (unsigned)port, (unsigned)(val & 0xff));
}


// Port trace. Arming it and then resuming with 'g' records what the guest does to a range of
// ports -- which registers a game programs, in which order and from where -- and 't' alone lists
// it afterwards. The buffer fills and then stops, so it keeps the first accesses after arming,
// which is where a mode set or a hardware probe happens.
void SerialDebug::cmdPortTrace(char const * args)
{
  if (!*args) {
    int count;
    uint32_t missed;
    auto e = m_machine->portTrace(&count, &missed);
    if (!e) {
      write("port trace is off\r\n");
      return;
    }
    for (int i = 0; i < count; ++i) {
      switch (e[i].kind) {
        case Machine::TraceKind::Out:
          writeFmt("%04X:%04X  out %04X <= %02X", e[i].cs, e[i].ip, e[i].port, e[i].value);
          break;
        case Machine::TraceKind::In:
          writeFmt("%04X:%04X  in  %04X  = %02X", e[i].cs, e[i].ip, e[i].port, e[i].value);
          break;
        case Machine::TraceKind::Int:
          writeFmt("%04X:%04X  int %02X  AX=%04X", e[i].cs, e[i].ip, e[i].value, e[i].port);
          break;
      }
      if (e[i].repeat > 1)
        writeFmt("  x%u", (unsigned) e[i].repeat);
      write("\r\n");
    }
    writeFmt("%d entries%s, %u more accesses after the buffer filled\r\n",
             count, count == PORTTRACE_ENTRIES ? " (full)" : "", (unsigned) missed);
    return;
  }

  if (strncmp(args, "off", 3) == 0) {
    m_machine->stopPortTrace();
    write("port trace off\r\n");
    return;
  }

  // t lo hi [r|w|rw] [int lo [hi]]
  char buf[64];
  strlcpy(buf, args, sizeof(buf));
  char * tok[8];
  int n = 0;
  for (char * t = strtok(buf, " "); t && n < 8; t = strtok(nullptr, " "))
    tok[n++] = t;

  bool ok = n >= 2;
  bool ok1 = false, ok2 = false;
  uint32_t lo = ok ? parseHex(tok[0], &ok1) : 0;
  uint32_t hi = ok ? parseHex(tok[1], &ok2) : 0;
  ok = ok && ok1 && ok2 && lo <= hi && hi <= 0xffff;

  bool reads = false, writes = true;
  int intLo = 1, intHi = 0;
  for (int i = 2; ok && i < n; ++i) {
    if (strcmp(tok[i], "r") == 0)       { reads = true;  writes = false; }
    else if (strcmp(tok[i], "w") == 0)  { reads = false; writes = true;  }
    else if (strcmp(tok[i], "rw") == 0) { reads = true;  writes = true;  }
    else if (strcmp(tok[i], "int") == 0 && i + 1 < n) {
      bool ok3 = false, ok4 = true;
      intLo = parseHex(tok[++i], &ok3);
      intHi = (i + 1 < n) ? parseHex(tok[++i], &ok4) : intLo;
      ok = ok3 && ok4 && intLo <= intHi && intHi <= 0xff;
    } else
      ok = false;
  }
  if (!ok) {
    write("usage: t lo hi [r|w|rw] [int lo [hi]]\r\n");
    return;
  }
  if (!m_machine->armPortTrace(lo, hi, reads, writes, intLo, intHi)) {
    write("no memory for the trace buffer\r\n");
    return;
  }
  writeFmt("tracing %s on %04X..%04X", reads && writes ? "reads and writes" : (reads ? "reads" : "writes"),
           (unsigned) lo, (unsigned) hi);
  if (intLo <= intHi)
    writeFmt(", and INT %02X..%02X", intLo, intHi);
  write(", 'g' to run\r\n");
}


// Free CPU time on the video core (the one the scanline interrupt runs on), for judging what else
// could run there -- an FM synthesiser, for one. A task at the lowest priority above idle reads the
// cycle counter in a tight loop: two readings only a few cycles apart mean it ran undisturbed, a
// longer gap means an interrupt or another task had the core. The undisturbed cycles, over the
// clock, are the share nothing else wanted. No calibration needed. The picture keeps being drawn
// while the monitor has the guest frozen, so this works from the monitor.
namespace {

struct CoreLoadRun {
  volatile bool     stop;
  volatile uint64_t freeCycles;
  volatile uint64_t totalCycles;
  volatile bool     done;
};

void IRAM_ATTR coreLoadTask(void * arg)
{
  auto run = (CoreLoadRun*) arg;
  uint32_t last = esp_cpu_get_cycle_count();
  uint64_t freeC = 0, totalC = 0;
  while (!run->stop) {
    uint32_t now = esp_cpu_get_cycle_count();
    uint32_t d   = now - last;
    last = now;
    totalC += d;
    if (d < 64)           // one pass of this loop is a few tens of cycles; more means preempted
      freeC += d;
  }
  run->freeCycles  = freeC;
  run->totalCycles = totalC;
  run->done        = true;
  vTaskDelete(nullptr);
}

}   // namespace


void SerialDebug::cmdCoreLoad(char const * args)
{
  int seconds = 2;
  if (*args) {
    bool ok = false;
    uint32_t v = parseHex(args, &ok);    // hex like everything else here; 1-9 read the same
    if (ok && v > 0 && v <= 0x3c)
      seconds = v;
  }
  int core = fabgl::CoreUsage::busiestCore();
  if (core < 0) {
    write("no video core configured\r\n");
    return;
  }

  static CoreLoadRun run;
  run.stop = false;
  run.done = false;
  TaskHandle_t h;
  if (xTaskCreatePinnedToCore(coreLoadTask, "coreload", 2048, &run, 1, &h, core) != pdPASS) {
    write("cannot start the measuring task\r\n");
    return;
  }
  writeFmt("measuring core %d for %d s ...\r\n", core, seconds);
  vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
  run.stop = true;
  while (!run.done)
    vTaskDelay(1);

  auto em = m_machine->graphicsAdapter()->emulation();
  uint32_t pct10 = run.totalCycles ? (uint32_t)(run.freeCycles * 1000 / run.totalCycles) : 0;
  writeFmt("core %d free: %u.%u %%  (%u of %u Mcycles), video emulation %d\r\n", core,
           (unsigned)(pct10 / 10), (unsigned)(pct10 % 10),
           (unsigned)(run.freeCycles / 1000000), (unsigned)(run.totalCycles / 1000000), (int)em);
}


// Video memory statistics: 'm on' zeroes and starts counting, 'g' lets the guest run, 'm' reports
// how many instructions it executed, how many video memory accesses it made and what share of the
// emulator core's cycles went into the video memory handlers (see Machine::s_vmStats).
void SerialDebug::cmdVideoStats(char const * args)
{
  static uint32_t startTicks;
  static int64_t  startTime;

  if (strncmp(args, "on", 2) == 0) {
    Machine::s_vmReads  = 0;
    Machine::s_vmWrites = 0;
    Machine::s_vmCycles = 0;
    Machine::s_stepCycles = 0;
    Machine::s_tickCycles = 0;
    startTicks          = m_machine->ticksCounter();
    startTime           = esp_timer_get_time();
    Machine::s_vmStats  = true;
    write("video memory statistics on; 'g' to run, then 'm' to report\r\n");
    return;
  }
  if (strncmp(args, "off", 3) == 0) {
    Machine::s_vmStats = false;
    write("video memory statistics off\r\n");
    return;
  }
  // the monitor itself stops the guest, so this is guest running time plus the monitor's; fine
  // for shares and rates when the guest ran for some seconds
  double   secs   = (esp_timer_get_time() - startTime) / 1e6;
  uint32_t instr  = m_machine->ticksCounter() - startTicks;
  double   cycles = secs * ESP.getCpuFreqMHz() * 1e6;
  writeFmt("%s, %.1f s: %u instructions (%.0f k/s), %u video reads (%.0f k/s), %u writes (%.0f k/s)\r\n",
           Machine::s_vmStats ? "counting" : "stopped", secs, (unsigned) instr, instr / secs / 1000,
           (unsigned) Machine::s_vmReads, Machine::s_vmReads / secs / 1000,
           (unsigned) Machine::s_vmWrites, Machine::s_vmWrites / secs / 1000);
  uint32_t n = Machine::s_vmReads + Machine::s_vmWrites;
  writeFmt("  %.1f%% of the emulator core's cycles in the video handlers, %u cycles per access\r\n",
           cycles > 0 ? Machine::s_vmCycles * 100.0 / cycles : 0.0,
           n ? (unsigned)(Machine::s_vmCycles / n) : 0u);
  writeFmt("  per instruction: %u cycles in step(), %u in tick(); %.1f%% of the core in step, %.1f%% in tick\r\n",
           instr ? (unsigned)(Machine::s_stepCycles / instr) : 0u,
           instr ? (unsigned)(Machine::s_tickCycles / instr) : 0u,
           cycles > 0 ? Machine::s_stepCycles * 100.0 / cycles : 0.0,
           cycles > 0 ? Machine::s_tickCycles * 100.0 / cycles : 0.0);
}


// Per opcode CPU profile: 'y on' allocates and starts, 'g' lets the guest run, 'y' lists the 24
// opcodes that took the most cycles, 'y off' stops and frees (see Machine::s_opProfile).
void SerialDebug::cmdOpProfile(char const * args)
{
  if (strncmp(args, "on", 2) == 0) {
    auto p = (Machine::OpProfile *) calloc(1, sizeof(Machine::OpProfile));
    if (!p) {
      write("no memory for the profile\r\n");
      return;
    }
    auto old = Machine::s_opProfile;
    Machine::s_opProfile = p;
    free(old);
    write("opcode profile on; 'g' to run, then 'y' to report\r\n");
    return;
  }
  if (strncmp(args, "off", 3) == 0) {
    auto old = Machine::s_opProfile;
    Machine::s_opProfile = nullptr;
    free(old);
    write("opcode profile off\r\n");
    return;
  }
  auto p = Machine::s_opProfile;
  if (!p) {
    write("profile is off ('y on' first)\r\n");
    return;
  }
  uint64_t totalCycles = 0, totalCount = 0;
  for (int i = 0; i < 256; ++i) {
    totalCycles += p->cycles[i];
    totalCount  += p->count[i];
  }
  writeFmt("%llu instructions, %llu cycles in step(), %u cycles on average\r\n",
           (unsigned long long) totalCount, (unsigned long long) totalCycles,
           totalCount ? (unsigned)(totalCycles / totalCount) : 0u);
  write(" op  cycles%  instr%  avg cycles\r\n");
  bool shown[256] = { };
  for (int n = 0; n < 24; ++n) {
    int best = -1;
    for (int i = 0; i < 256; ++i)
      if (!shown[i] && (best < 0 || p->cycles[i] > p->cycles[best]))
        best = i;
    if (best < 0 || p->cycles[best] == 0)
      break;
    shown[best] = true;
    writeFmt(" %02X  %5.1f%%  %6.2f%%  %5u\r\n", best,
             totalCycles ? p->cycles[best] * 100.0 / totalCycles : 0.0,
             totalCount ? p->count[best] * 100.0 / totalCount : 0.0,
             p->count[best] ? (unsigned)(p->cycles[best] / p->count[best]) : 0u);
  }
}


// OPL2 cost on this CPU: all nine channels keyed on with a two operator FM voice and sustained, the
// heaviest thing an AdLib does, generated at 16384 Hz for one second's worth of samples.
void SerialDebug::cmdOPLBench()
{
  constexpr int RATE  = 16384;
  constexpr int BLOCK = 256;

  auto chip = new DBOPL::Chip(false);
  if (!chip) {
    write("no memory for the chip\r\n");
    return;
  }
  uint32_t t0 = esp_cpu_get_cycle_count();
  DBOPL::InitTables();
  chip->Setup(RATE);
  uint32_t initCycles = esp_cpu_get_cycle_count() - t0;

  chip->WriteReg(0x01, 0x20);                        // waveform select enable
  for (int c = 0; c < 9; ++c) {
    int mod = (c / 3) * 8 + (c % 3), car = mod + 3;
    for (int op : { mod, car }) {
      chip->WriteReg(0x20 + op, 0x21);               // sustain, multiplier 1
      chip->WriteReg(0x60 + op, 0xf2);               // fast attack, slow decay
      chip->WriteReg(0x80 + op, 0x24);               // sustain level, release
      chip->WriteReg(0xe0 + op, c & 3);              // a mix of waveforms
    }
    chip->WriteReg(0x40 + mod, 0x18);                // modulator level
    chip->WriteReg(0x40 + car, 0x00);                // carrier at full volume
    chip->WriteReg(0xc0 + c, 0x0e);                  // feedback 7, FM
    int fnum = 0x157 + c * 23;
    chip->WriteReg(0xa0 + c, fnum & 0xff);
    chip->WriteReg(0xb0 + c, 0x20 | (4 << 2) | (fnum >> 8));   // key on, block 4
  }

  static int32_t buf[BLOCK];
  int64_t peak = 0;
  uint32_t start = esp_cpu_get_cycle_count();
  for (int n = 0; n < RATE; n += BLOCK) {
    chip->GenerateBlock2(BLOCK, buf);
    for (int i = 0; i < BLOCK; ++i)
      peak = buf[i] > peak ? buf[i] : (-buf[i] > peak ? -buf[i] : peak);
  }
  uint32_t cycles = esp_cpu_get_cycle_count() - start;

  uint32_t perSample = cycles / RATE;
  uint32_t mhz       = ESP.getCpuFreqMHz();
  writeFmt("OPL2 (DBOPL), 9 channels FM: %u cycles per sample, %u Mcycles for one second at %d Hz\r\n",
           (unsigned) perSample, (unsigned)(cycles / 1000000), RATE);
  writeFmt("  share of a %u MHz core:  16384 Hz %u%%,  32768 Hz %u%%,  49716 Hz %u%%\r\n", (unsigned) mhz,
           (unsigned)(perSample * 16384ull * 100 / (mhz * 1000000ull)),
           (unsigned)(perSample * 32768ull * 100 / (mhz * 1000000ull)),
           (unsigned)(perSample * 49716ull * 100 / (mhz * 1000000ull)));
  writeFmt("  chip struct %u bytes, tables + setup %u kcycles, peak output %d\r\n",
           (unsigned) sizeof(DBOPL::Chip), (unsigned)(initCycles / 1000), (int) peak);
  delete chip;
}


void SerialDebug::runMonitor()
{
  write("\r\n--- monitor, the guest is frozen. '?' for help, 'g' to resume ---\r\n");
  cmdRegisters();

  char line[128];

  while (true) {

    write("- ");
    if (!readLine(line, sizeof(line)))
      continue;

    char * p = line;
    while (*p == ' ')
      ++p;
    if (!*p)
      continue;

    char cmd = *p++;
    while (*p == ' ')
      ++p;

    switch (cmd) {
      case 'r': cmdRegisters();  break;
      case 'd': cmdDump(p);      break;
      case 'e': cmdEnter(p);     break;
      case 'f': cmdFind(p);      break;
      case 'p': cmdScreenshot(); break;
      case 'v': cmdVideoState(); break;
      case 'h': cmdHeapMap();    break;
      case 'w': cmdWriteReg(p);  break;
      case 'k': cmdInjectKey(p); break;
      case 'l': cmdLoad(p);      break;
      case 'j': cmdJump(p);      break;
      case 'u': cmdUsbSerial(p); break;

      // Ctrl+Alt+Del. The guest restarts from POST, which is the only way to get a
      // breakpoint in before the video option ROM initialises itself.
      case 'z':
        write("--- rebooting the guest ---\r\n");
        m_machine->trigReset();
        m_state    = State::Screen;
        m_lastHash = 0;
        return;
      case 'x': cmdExport(p);    break;

      case 'b':
      {
        uint32_t a;
        if (*p && parseAddress(p, &a)) {
          m_machine->setBreakpoint(a);
          writeFmt("breakpoint armed at %05X\r\n", (unsigned)a);
        } else {
          m_machine->setBreakpoint(0);
          write("breakpoint cleared\r\n");
        }
        break;
      }
      case 'i': cmdInPort(p);    break;
      case 'o': cmdOutPort(p);   break;
      case 't': cmdPortTrace(p); break;
      case 'c': cmdCoreLoad(p);  break;
      case 'a': cmdOPLBench();   break;
      case 'm': cmdVideoStats(p); break;
      case 'y': cmdOpProfile(p); break;
      case '?': help();          break;

      case 's':
      {
        // single step: the surest way to tell "the CPU is stopped" from "the CPU is looping"
        int n = 1;
        if (*p) {
          bool ok = false;
          uint32_t v = parseHex(p, &ok);
          if (ok && v > 0 && v <= 0x10000)
            n = (int) v;
        }
        for (int i = 0; i < n; ++i)
          m_machine->stepCPU();
        cmdRegisters();
        break;
      }

      case 'g':
        write("--- resumed ---\r\n");
        m_state    = State::Screen;
        m_lastHash = 0;
        return;

      case 'q':
        write("--- detached, the console is idle again ---\r\n");
        m_state    = State::Detached;
        m_escCount = 0;
        return;

      default:
        write("unknown command, '?' for help\r\n");
        break;
    }
  }
}
