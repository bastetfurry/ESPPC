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


#pragma once


#include "fabgl.h"
#include "emudevs/graphicsadapter.h"
#include "emudevs/i8086.h"
#include "emudevs/PIC8259.h"
#include "emudevs/PIT8253.h"
#include "emudevs/i8042.h"
#include "emudevs/MC146818.h"
#include "emudevs/PC8250.h"
#include "devdrivers/MCP23S17.h"

#include "bios.h"
#include "serialdbg.h"
#include "sn76496.h"
#include "tandydac.h"
#include "ega.h"


// Disk images may live on the SD card (paths relative to the base directory) or in the
// on-flash SPIFFS partition (absolute paths, ie "/spiffs/rescue.img").
#define SPIFFS_MOUNT_PATH    "/spiffs"

#define RAM_SIZE             1048576    // must correspond to bios MEMSIZE
#define VIDEOMEMSIZE         65536
#define PORTTRACE_ENTRIES    512        // debug monitor port trace, see Machine::armPortTrace()
#define CGA_BORROWED_SIZE    16384      // a real CGA's video RAM, and one EGA plane bank

// maximum number of serial ports
#define SERIALPORTS          2



using fabgl::GraphicsAdapter;
using fabgl::PIC8259;
using fabgl::PIT8253;
using fabgl::i8042;
using fabgl::MC146818;
using fabgl::MCP23S17;
using fabgl::SerialPort;
using fabgl::PC8250;


#ifdef FABGL_EMULATED
typedef void (*StepCallback)(void *);
#endif


typedef void (*SysReqCallback)();



// Scanlines the EGA BIOS POST expects to count between vertical retraces. 350 is the enhanced
// colour display; a CGA class display would be 200.
#define EGA_POST_SCANLINES 350


// Postbox: a private channel between guest software and whatever is on the host serial line.
//
// Guest side is INT 0xFF with AX = 0x55EE, which is deliberately not a real BIOS or DOS service,
// so ordinary programs cannot collide with it. BX selects the function:
//
//   BX = 0  ReadyToAcceptConnection  -> AX = 0x55EE (proves a postbox is listening),
//                                       BL = protocol version, CX != 0 if a host session is open,
//                                       DX = bytes waiting to be received
//   BX = 1  SendByte                 <- CL = byte to send. CF set if the buffer is full.
//   BX = 2  ReceiveByte              -> AL = byte, CF clear. CF set if nothing is waiting.
//
// AX comes back as 0x55EE on every call, so a guest can tell a real postbox from an unhandled
// interrupt. Both directions are small rings: the host is far faster than the guest, and a
// transfer is expected to be flow controlled by the guest polling.
#define POSTBOX_SIGNATURE 0x55ee
#define POSTBOX_VERSION   1
#define POSTBOX_BUFSIZE   1024


class PostboxRing {
public:
  PostboxRing() : m_head(0), m_tail(0) { }

  void  clear()            { m_head = m_tail = 0; }
  bool  empty() const      { return m_head == m_tail; }
  int   count() const      { return (m_head - m_tail + POSTBOX_BUFSIZE) % POSTBOX_BUFSIZE; }
  bool  full() const       { return count() == POSTBOX_BUFSIZE - 1; }

  bool push(uint8_t v)
  {
    if (full())
      return false;
    m_buf[m_head] = v;
    m_head = (m_head + 1) % POSTBOX_BUFSIZE;
    return true;
  }

  bool pop(uint8_t * v)
  {
    if (empty())
      return false;
    *v = m_buf[m_tail];
    m_tail = (m_tail + 1) % POSTBOX_BUFSIZE;
    return true;
  }

private:
  uint8_t m_buf[POSTBOX_BUFSIZE];
  int     m_head;
  int     m_tail;
};


class Machine {

public:
  Machine();
  ~Machine();

  void setBaseDirectory(char const * value)    { m_baseDir = value; }

  void setDriveImage(int drive, char const * filename, int cylinders = 0, int heads = 0, int sectors = 0);
  
  bool diskChanged(int drive)                  { return m_diskChanged[drive]; }
  void resetDiskChanged(int drive)             { m_diskChanged[drive] = false; }

  void setBootDrive(int drive)                 { m_bootDrive = drive; }
  int bootDrive()                              { return m_bootDrive; }

  // conventional memory in KB (64..704, in 64KB steps). Above 640KB the video window moves
  // from 0xA0000 to 0xB0000 so that 0xA0000-0xAFFFF becomes ordinary RAM.
  void setRamSize(int kb)                      { m_ramSizeKB = kb; }

  // Selects the EGA path: a real EGA BIOS option ROM is loaded at C000:0000 and POST lets it
  // initialise itself, instead of the CGA/Hercules adapter with EGA modes bolted onto the system
  // BIOS. romPath is copied. Must be called before run().
  void setEGABios(char const * romPath);

  // Tandy 1000 video and sound on top of the CGA: the 16 colour modes 8 and 9, the video array
  // (palette) at 3DAh/3DEh, the page register at 3DFh, and the SN76496 at C0h. Must be called
  // before run().
  void setTandy(bool value)                    { m_tandy = value; }
  bool tandy() const                           { return m_tandy; }

  // The SN76496 alone, as on the TNDY style sound cards, for a machine without the Tandy video.
  // setTandy() implies it. Must be called before run().
  void setTandySound(bool value)               { m_tandySound = value; }
  bool tandySound() const                      { return m_tandySound || m_tandy; }

  // The Tandy 1000 SL/TL DAC next to the sound chip (see tandydac.h), reached through the BIOS,
  // INT 1Ah AH=81h..85h. Needs the sound chip. Must be called before run().
  void setTandyDAC(bool value)                 { m_tandyDACRequested = value; }
  bool tandyDAC() const                        { return m_tandyDACRequested && tandySound(); }

  // BIOS side of the DAC: INT 1Ah AH=81h..85h with the caller's AX (the other inputs are still in
  // their registers), the IRQ 7 stub's question "has a block ended?", and where that stub is.
  void tandyDACBios(uint16_t callerAX);
  bool tandyDACIrq();
  void setTandyDACStub(uint16_t offset)        { m_dacStubIP = offset; }

  // BIOS side of the Tandy: a mode set for BIOS modes 8 and 9 (returns false for any other mode,
  // after putting the video array back to its CGA state), and the palette calls INT 10h AH=10h.
  bool setTandyMode(int biosMode, bool clear);
  void setTandyPaletteReg(int index, uint8_t value);
  void setTandyBorder(uint8_t value)           { m_tandyArray[2] = value & 0x0f; }
  uint8_t const * tandyArray() const           { return m_tandyArray; }   // for the monitor
  bool egaBiosInstalled() const                { return m_egaBiosLoaded; }

  // Called from POST once the IVT is in place: points the option ROM's own INT 10h handler at
  // this BIOS's handler, so the ROM is present and hooked but never actually drives the hardware.
  void prepareVideoROM();

  // Makes the next CGA register write set the mode up in full, even if it repeats the values the
  // CGA had before. Needed after anything else has driven the graphics adapter.
  void invalidateCGAMode()                     { m_CGAModeCurrentValid = false; }
  int ramSizeKB()                              { return m_ramSizeKB; }

  void setSysReqCallback(SysReqCallback value) { m_sysReqCallback = value; }
  
  PC8250 * com1()                      { return &m_COM1; }
  PC8250 * com2()                      { return &m_COM2; }

  // Carry an emulated COM port in software instead of over real pins, or 0 to stop. This goes
  // through the machine rather than straight to the chip because a port needs its interrupt
  // callback registered as well, exactly as setCOM1() does it -- without that the chip is ticked
  // but can never raise IRQ4, and nothing it transmits gets anywhere.
  void bridgeCOM(int port, void * context, PC8250::TXCallback txCallback);

  void setCOM1(SerialPort * serialPort);
  
  void setCOM2(SerialPort * serialPort);

  void run();

  void trigReset()                     { m_reset = true; }

  uint32_t ticksCounter()              { return m_ticksCounter; }

  i8042 * getI8042()                   { return &m_i8042; }

  MC146818 * getMC146818()             { return &m_MC146818; }

  uint8_t * memory()                   { return s_memory; }

  uint8_t * videoMemory()              { return s_videoMemory; }

  uint8_t * frameBuffer()              { return m_frameBuffer; }

  GraphicsAdapter * graphicsAdapter()  { return &m_graphicsAdapter; }

  BIOS * bios()                        { return &m_BIOS; }

  // debug monitor introspection
  uint32_t ticksCounter() const        { return m_ticksCounter; }
  bool     resetPending() const        { return m_reset; }

  // --- postbox, see the note above the class ---
  // toHost holds what the guest has sent; toGuest what the host has queued for it.
  PostboxRing & postboxToHost()        { return m_postboxToHost; }
  PostboxRing & postboxToGuest()       { return m_postboxToGuest; }
  bool postboxHostOpen() const         { return m_postboxHostOpen; }
  void setPostboxHostOpen(bool v)      { m_postboxHostOpen = v; }
  void     stepCPU();

  // One-shot breakpoint on a linear address, for the debug monitor. Disarmed costs one compare
  // against zero per instruction; armed, the monitor takes over the moment CS:IP matches.
  void     setBreakpoint(uint32_t linear)  { m_breakAddr = linear; }
  uint32_t breakpoint() const              { return m_breakAddr; }

  // Port trace for the debug monitor: records the guest's accesses to ports lo..hi, with the CS:IP
  // they came from, until the buffer is full. A run of identical accesses from the same place is
  // one entry with a repeat count, so a polling loop does not fill it. Disarmed it costs one
  // pointer test per port access; the buffer only exists while armed.
  //
  // It can record software interrupts intLo..intHi as well (intLo > intHi leaves them out): for
  // those, port holds AX and value the interrupt number, and CS:IP is the caller's.
  enum class TraceKind : uint8_t { In, Out, Int };
  struct PortTraceEntry {
    uint16_t  cs, ip, port, repeat;
    uint8_t   value;
    TraceKind kind;
  };
  bool armPortTrace(int lo, int hi, bool reads, bool writes, int intLo = 1, int intHi = 0);
  void stopPortTrace();
  PortTraceEntry const * portTrace(int * count, uint32_t * missed) const
                                       { *count = m_portTraceCount; *missed = m_portTraceMissed; return m_portTrace; }

  // --- EGA ---
  // setEGAMode() takes a BIOS mode number (0x0d, 0x0e, 0x0f, 0x10) and returns false if it is
  // not an EGA mode or the planar video memory could not be allocated, in which case the caller
  // must fall back to the CGA path.
  bool setEGAMode(int biosMode);

  // Draw one character into the planes for an EGA graphics mode, at the BIOS cursor.
  // The BIOS asks for this because there is no linear frame buffer to poke in these modes.
  void egaDrawChar(uint8_t ch, uint8_t colour);

  // BIOS pixel services for the EGA graphics modes; false / -1 when the mode is not one.
  bool egaWritePixel(int x, int y, uint8_t colour, bool xored);
  int  egaReadPixel(int x, int y);
  void leaveEGAMode();
  bool EGAActive() const               { return m_EGAActive; }
  EGA * ega()                          { return &m_EGA; }
  uint8_t const * egaPalette() const   { return m_EGAACPalette; }

  // The attribute controller's palette registers, for the BIOS palette calls (INT 10h AH=10h).
  // value is 6 bit rgbRGB, as on the port.
  void setEGAPaletteReg(int index, uint8_t value);

  // Enables the serial debug console (see serialdbg.h). Must be called before run(), and only
  // when COM1 is left unattached -- they share the same pins.
  void enableDebugConsole()            { m_debugConsoleRequested = true; }

  // Which emulated COM port the USB serial carries. Only used when the debug console owns
  // the UART: without it a port takes the pins directly. Also reachable at runtime from the
  // monitor's "u" command.
  // Recorded here rather than passed straight on, because the console is only started from
  // init(), which runs later and in the emulator task.
  void setUsbSerialRoute(int port)     { m_usbSerialRoute = port; }

  // I/O port access for the debug monitor, routed through the same handlers the CPU uses
  // (past the port trace, so the monitor's own accesses do not show up in it)
  uint8_t readPortDebug(int address)            { return readPortHW(this, address); }
  void    writePortDebug(int address, uint8_t v) { writePortHW(this, address, v); }

  // Memory access for the debug monitor. Video memory is not part of s_memory -- the CPU routes
  // the video window to separate callbacks -- so reading s_memory directly there shows zeros
  // instead of the screen. Mirror what i8086::RMEM8/WMEM8 do so the monitor sees what the CPU
  // would see. videoMemStart follows setRamSize(), same as in init().
  int videoMemStartDebug() const                { return m_ramSizeKB > 640 ? 0xB0000 : 0xA0000; }

  uint8_t readMemDebug(int address)
  {
    if (address >= videoMemStartDebug() && address < 0xC0000)
      return readVideoMemory8(this, address);
    return s_memory[address];
  }

  void writeMemDebug(int address, uint8_t value)
  {
    if (address >= videoMemStartDebug() && address < 0xC0000)
      writeVideoMemory8(this, address, value);
    else
      s_memory[address] = value;
  }

  FILE * disk(int index)               { return m_disk[index]; }
  char const * diskFilename(int index) { return m_diskFilename[index]; }
  uint64_t diskSize(int index)         { return m_diskSize[index]; }
  uint16_t diskCylinders(int index)    { return m_diskCylinders[index]; }
  uint8_t diskHeads(int index)         { return m_diskHeads[index]; }
  uint8_t diskSectors(int index)       { return m_diskSectors[index]; }

  void dumpMemory(char const * filename);
  void dumpInfo(char const * filename);

  #ifdef FABGL_EMULATED
  void setStepCallback(StepCallback value)  { m_stepCallback = value; }
  #endif

private:


  static void runTask(void * pvParameters);

  void init();
  void reset();

  void tick();

  bool egaPixelOffset(int x, int y, uint32_t * off, uint8_t * mask);
  bool loadEGABios();
  void loadEGAFont(int charHeight);
  void setCGAMode();
  void writeTandyArray(uint8_t value);
  void hookTandyDACIrq();
  void writeDMA(int port, uint8_t value);
  uint8_t readDMA(int port);
  void writeTandyDACPort(int port, uint8_t value);
  uint8_t readTandyDACPort(int port);
  void updateTandyDACDMA();
  void tandyDACTerminalCount(int count);
  void unhookTandyDACIrq();
  void endTandyDACBlock();
  void resetTandyArray();
  void setEGAAttributeReg(uint8_t value);
  void setEGADefaultPalette(bool line200);
  uint8_t egaInputStatus1();
  void setCGA6845Register(uint8_t value);

  void setHGCMode();
  void setHGC6845Register(uint8_t value);

  static void writePort(void * context, int address, uint8_t value);
  static uint8_t readPort(void * context, int address);
  static uint8_t readPortHW(void * context, int address);
  static void writePortHW(void * context, int address, uint8_t value);
  void tracePort(int address, uint8_t value, TraceKind kind);

  static void writeVideoMemory8(void * context, int address, uint8_t value);
  static void writeVideoMemory16(void * context, int address, uint16_t value);
  static uint8_t readVideoMemory8(void * context, int address);
  static uint16_t readVideoMemory16(void * context, int address);

  static bool interrupt(void * context, int num);
  static bool interruptHandler(void * context, int num);

  static void PITChangeOut(void * context, int timerIndex);

  static bool MC146818Interrupt(void * context);
  
  static bool COM1Interrupt(PC8250 * source, void * context);
  static bool COM2Interrupt(PC8250 * source, void * context);

  static bool keyboardInterrupt(void * context);
  static bool mouseInterrupt(void * context);
  static bool resetMachine(void * context);
  static bool sysReq(void * context);

  void speakerSetFreq();
  void speakerEnableDisable();

  void autoDetectDriveGeometry(int drive);


  #ifdef FABGL_EMULATED
  StepCallback             m_stepCallback;
  #endif

  bool                     m_reset;

  PostboxRing              m_postboxToHost;
  PostboxRing              m_postboxToGuest;
  bool                     m_postboxHostOpen;

  int                      m_ramSizeKB;

  GraphicsAdapter          m_graphicsAdapter;

  BIOS                     m_BIOS;

  // 0, 1 = floppy
  // >= 2 = hard disk
  char *                   m_diskFilename[DISKCOUNT];
  bool                     m_diskChanged[DISKCOUNT];
  FILE *                   m_disk[DISKCOUNT];
  uint64_t                 m_diskSize[DISKCOUNT];
  uint16_t                 m_diskCylinders[DISKCOUNT];
  uint8_t                  m_diskHeads[DISKCOUNT];
  uint8_t                  m_diskSectors[DISKCOUNT];

  static uint8_t *         s_memory;
  static uint8_t *         s_videoMemory;
  uint8_t *                m_frameBuffer;
  // On the EGA machine, the plane bank a CGA mode (4, 5, 6) uses as its 16 KB of video RAM while
  // the EGA is not active; null otherwise. See leaveEGAMode().
  uint8_t *                m_CGABorrowed;

  // 8259 Programmable Interrupt Controllers
  PIC8259                  m_PIC8259A;  // master
  PIC8259                  m_PIC8259B;  // slave

  // 8253 Programmable Interval Timers
  // pin connections of PIT8253 on the IBM XT:
  //    gate-0 = gate-1 = +5V
  //    gate-2 = TIM2GATESPK
  //    clk-0  = clk-1  = clk-2 = 1193182 Hz
  //    out-0  = IRQ0
  //    out-1  = RAM refresh
  //    out-2  = speaker
  PIT8253                  m_PIT8253;

  // 8042 PS/2 Keyboard Controller
  i8042                    m_i8042;

  TaskHandle_t             m_taskHandle;

  uint32_t                 m_ticksCounter;

  // CGA
  uint8_t                  m_CGA6845SelectRegister;
  uint8_t                  m_CGA6845[18];
  uint16_t                 m_CGAMemoryOffset;
  uint8_t                  m_CGAModeReg;
  uint8_t                  m_CGAColorReg;
  // What setCGAMode() last set the adapter up from, so it can skip a write that changes nothing
  // (SCUMM games rewrite the mode and colour registers every frame). Cleared by invalidateCGAMode()
  // whenever something else -- EGA, Hercules, the system menu -- has had the adapter since.
  uint8_t                  m_CGAModeRegCurrent;
  uint8_t                  m_CGAColorRegCurrent;
  uint16_t                 m_CGAMemoryOffsetCurrent;
  bool                     m_CGAModeCurrentValid;
  uint16_t                 m_CGAVSyncQuery;

  // Tandy 1000 (see setTandy())
  bool                     m_tandy;
  bool                     m_tandySound;
  uint8_t                  m_tandyArrayIndex;   // 3DAh
  uint8_t                  m_tandyArray[32];    // 3DEh: 1 mask, 2 border, 3 mode, 10h-1Fh palette
  uint8_t                  m_tandyPage;         // 3DFh, stored only: see docs/tandy.md
  SN76496                  m_sn76496;
  bool                     m_tandyDACRequested;
  TandyDAC                 m_tandyDAC;
  uint16_t                 m_dacStubIP;         // BIOS IRQ 7 handler for the DAC, at F000:this
  uint32_t                 m_dacSavedVector;    // the IRQ 7 vector it replaced
  bool                     m_dacHooked;
  bool                     m_dacBlockEnded;     // IRQ 7 raised for it, not yet handled

  // The DAC's own registers, C4h-C7h, for software that programs it directly
  uint8_t                  m_dacMode;           // C4h: bits 1-0 = 3 playback, bits 3-2 = 11 DMA on
  uint16_t                 m_dacDivider;        // C6h, C7h bits 3-0: rate = 3579545 / divider
  uint8_t                  m_dacAmplitude;      // C7h bits 7-5
  bool                     m_dacIrqFlag;        // terminal count reached, C4h bit 3 on read

  // A minimal 8237 DMA controller, on machines with the Tandy DAC only. The registers of all four
  // channels are kept so software can program and read them, but only channel 1 moves, driven by
  // the DAC's playback (see TandyDAC).
  struct DMAChannel {
    uint16_t baseAddr, baseCount;
    uint16_t curAddr, curCount;
    uint8_t  page;
    uint8_t  mode;
    bool     masked;
  };
  DMAChannel               m_dma[4];
  bool                     m_dmaFlipFlop;       // false = the next byte is the low one
  uint8_t                  m_dmaStatus;         // bits 3-0 terminal count reached, cleared on read

  // Hercules
  uint8_t                  m_HGC6845SelectRegister;
  uint8_t                  m_HGC6845[18];
  uint16_t                 m_HGCMemoryOffset;
  uint8_t                  m_HGCModeReg;
  uint8_t                  m_HGCSwitchReg;
  uint16_t                 m_HGCVSyncQuery;

  // speaker/audio
  bool                     m_speakerDataEnable;
  SoundGenerator           m_soundGen;
  SineWaveformGenerator    m_sinWaveGen;

  // CMOS & RTC
  MC146818                 m_MC146818;

  // extended I/O (MCP23S17)
  MCP23S17                 m_MCP23S17;
  uint8_t                  m_MCP23S17Sel;

  uint8_t                  m_bootDrive;

  SysReqCallback           m_sysReqCallback;

  char const *             m_baseDir;
  
  // serial ports
  EGA                      m_EGA;
  bool                     m_EGAActive;
  uint8_t                  m_EGAACIndex;      // attribute controller index
  bool                     m_EGAACFlipFlop;   // false = next 0x3c0 write is an index
  uint8_t                  m_EGAACPalette[16];
  uint8_t                  m_EGAACColorPlaneEnable;  // AC register 0x12, incl. the status mux
  uint32_t                 m_EGAFramePos;            // raster position, in status register reads
  int64_t                  m_EGARetraceUs;           // esp_timer time the last vertical retrace began
  int                      m_EGAStride;              // byte-columns per scanline, to re-register
  uint8_t                  m_EGAMiscOutput;
  uint8_t                  m_EGASwitches;     // EGA configuration DIP block, read via 0x3c2

  SerialDebug              m_serialDebug;
  bool                     m_debugConsoleRequested;
  int                      m_usbSerialRoute;
  uint32_t                 m_breakAddr;
  PortTraceEntry *         m_portTrace;          // null while the trace is off
  int                      m_portTraceCount;
  uint32_t                 m_portTraceMissed;    // accesses that came after the buffer filled
  uint16_t                 m_portTraceLo;
  uint16_t                 m_portTraceHi;
  bool                     m_portTraceReads;
  bool                     m_portTraceWrites;
  int                      m_intTraceLo;
  int                      m_intTraceHi;
  char *                   m_egaRomPath;
  bool                     m_egaBiosLoaded;

  PC8250                   m_COM1;
  PC8250                   m_COM2;

};


