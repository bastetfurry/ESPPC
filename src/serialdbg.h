/*
  Serial debug console for the PCEmulator machine.

  Local to this project -- not part of FabGL.

  Gives a host on the USB serial line a way into a running machine: mirror the text screen, type
  at it, and drop into a small monitor that can look at and change guest memory and I/O ports.
  It is a debugging aid, deliberately line based and dumb -- no TUI, no cursor addressing beyond
  a home-and-repaint, nothing that needs a terminal emulator to be in any particular mode.

  It is off unless the machine's configuration says "enabledebug 1", because it is mutually
  exclusive with COM1: PC8250 puts COM1 on UART2 at GPIO 3/1, which are the same pins the USB
  serial console uses, so whichever is attached takes the line away from the other.

  States:

    Detached  the console is watching for three ESCs in a row and nothing else. The guest runs
              normally and nothing is sent to the host.
    Screen    the text screen is mirrored to the host and host keystrokes are injected into the
              guest's keyboard buffer.
    Monitor   the guest is frozen and the host is talking to the monitor.

  Transitions follow what the host types:

    Detached --[ESC ESC ESC]--> Screen --[Ctrl+C Ctrl+D]--> Monitor
    Monitor  --[g]--> Screen        Monitor --[q]--> Detached

  poll() is called from Machine::tick(), so it runs in the emulator task. That matters: guest RAM
  is PSRAM, and the emulator's core may only touch its own half of it (see Machine::init), so the
  monitor has to read memory from that task and no other.
 */


#pragma once


#include <stdint.h>


class Machine;


class SerialDebug {

public:

  SerialDebug();

  // machine may be nullptr, which leaves the console permanently disabled
  void begin(Machine * machine);

  bool enabled() const              { return m_machine != nullptr; }

  // called from Machine::tick(); cheap when detached, blocks while the monitor has the CPU
  void poll();


private:

  enum class State { Detached, Screen, Monitor };

  // --- screen mirroring ---
  void refreshScreen(bool force);
  bool textMode(uint8_t const * * buffer, int * columns, int * rows);
  static char cp437ToASCII(uint8_t c);

  // --- key injection ---
  void sendKeyToGuest(uint8_t ch);

  // --- monitor ---
  void runMonitor();
  bool readLine(char * buf, int bufSize);
  void cmdRegisters();
  void cmdDump(char const * args);
  void cmdEnter(char const * args);
  void cmdInPort(char const * args);
  void cmdOutPort(char const * args);
  void help();

  static bool parseAddress(char const * s, uint32_t * addr);
  static uint32_t parseHex(char const * s, bool * ok = nullptr);

  void write(char const * s);
  void writeFmt(char const * fmt, ...);

  Machine *  m_machine;
  State      m_state;

  int        m_escCount;        // consecutive ESCs seen while detached
  bool       m_pendingCtrlC;    // Ctrl+C held back, waiting to see if Ctrl+D follows
  bool       m_lastWasCR;       // so a CRLF pair from the host is one keystroke, not two

  uint32_t   m_lastRefreshMS;
  uint32_t   m_lastHash;

  uint32_t   m_dumpAddr;        // where an argument-less "d" carries on from

};
