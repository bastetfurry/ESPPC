/*
  Serial debug console -- see serialdbg.h for what it is and how it is reached.
 */


#include <Arduino.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "fabgl.h"

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
  write(" PCEmulator: machine booted, serial debug console on\r\n");
  write("=====================================================\r\n");
  write("  ESC ESC ESC   attach to the text screen\r\n");
  write("  Ctrl+C Ctrl+D once attached, drop into the monitor\r\n");
  write("\r\n");
  write("COM1 is not attached on this machine, because the debug\r\n");
  write("console needs the same pins. Clear \"enabledebug\" in the\r\n");
  write("machine configuration to get COM1 back.\r\n");
  write("\r\n");
}


// ---------------------------------------------------------------------------------------------
// state machine
// ---------------------------------------------------------------------------------------------

void SerialDebug::poll()
{
  if (!m_machine)
    return;

  while (Serial.available()) {

    uint8_t ch = (uint8_t) Serial.read();

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
        if (ch == 0x03)
          m_pendingCtrlC = true;      // hold it back until we know what follows
        else
          sendKeyToGuest(ch);
        break;

      case State::Monitor:
        // runMonitor() owns the line while it is active, so this is unreachable
        break;
    }
  }

  if (m_state == State::Screen)
    refreshScreen(false);
}


// ---------------------------------------------------------------------------------------------
// screen mirroring
// ---------------------------------------------------------------------------------------------

bool SerialDebug::textMode(uint8_t const * * buffer, int * columns, int * rows)
{
  auto ga = m_machine->graphicsAdapter();
  auto em = ga->emulation();

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

  if (!textMode(&fb, &columns, &rows)) {
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
  uint32_t hash = 2166136261u;
  for (int i = 0; i < columns * rows * 2; i += 2) {
    hash = (hash ^ fb[i]) * 16777619u;
    hash = (hash ^ fb[i + 1]) * 16777619u;
  }

  if (hash == m_lastHash && !force)
    return;
  m_lastHash = hash;

  char line[132];

  write("\x1b[H");
  for (int r = 0; r < rows; ++r) {
    int n = 0;
    for (int c = 0; c < columns && n < (int)sizeof(line) - 1; ++c)
      line[n++] = cp437ToASCII(fb[(r * columns + c) * 2]);
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
  write("  f bb bb ..        search conventional memory for a byte sequence\r\n");
  write("  i port            read an I/O port\r\n");
  write("  o port val        write an I/O port\r\n");
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
      case 'i': cmdInPort(p);    break;
      case 'o': cmdOutPort(p);   break;
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
