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
#include "ega.h"


// Disk images may live on the SD card (paths relative to the base directory) or in the
// on-flash SPIFFS partition (absolute paths, ie "/spiffs/rescue.img").
#define SPIFFS_MOUNT_PATH    "/spiffs"

#define RAM_SIZE             1048576    // must correspond to bios MEMSIZE
#define VIDEOMEMSIZE         65536

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
  bool egaBiosInstalled() const                { return m_egaBiosLoaded; }

  // Called from POST once the IVT is in place: points the option ROM's own INT 10h handler at
  // this BIOS's handler, so the ROM is present and hooked but never actually drives the hardware.
  void prepareVideoROM();
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

  // --- EGA ---
  // setEGAMode() takes a BIOS mode number (0x0d, 0x0e, 0x0f, 0x10) and returns false if it is
  // not an EGA mode or the planar video memory could not be allocated, in which case the caller
  // must fall back to the CGA path.
  bool setEGAMode(int biosMode);
  void leaveEGAMode();
  bool EGAActive() const               { return m_EGAActive; }
  EGA * ega()                          { return &m_EGA; }
  uint8_t const * egaPalette() const   { return m_EGAACPalette; }

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
  uint8_t readPortDebug(int address)            { return readPort(this, address); }
  void    writePortDebug(int address, uint8_t v) { writePort(this, address, v); }

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

  bool loadEGABios();
  void loadEGAFont(int charHeight);
  void setCGAMode();
  void setEGAAttributeReg(uint8_t value);
  uint8_t egaInputStatus1();
  void setCGA6845Register(uint8_t value);

  void setHGCMode();
  void setHGC6845Register(uint8_t value);

  static void writePort(void * context, int address, uint8_t value);
  static uint8_t readPort(void * context, int address);

  static void writeVideoMemory8(void * context, int address, uint8_t value);
  static void writeVideoMemory16(void * context, int address, uint16_t value);
  static uint8_t readVideoMemory8(void * context, int address);
  static uint16_t readVideoMemory16(void * context, int address);

  static bool interrupt(void * context, int num);

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
  uint16_t                 m_CGAVSyncQuery;

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
  uint8_t                  m_EGAMiscOutput;
  uint8_t                  m_EGASwitches;     // EGA configuration DIP block, read via 0x3c2

  SerialDebug              m_serialDebug;
  bool                     m_debugConsoleRequested;
  int                      m_usbSerialRoute;
  uint32_t                 m_breakAddr;
  char *                   m_egaRomPath;
  bool                     m_egaBiosLoaded;

  PC8250                   m_COM1;
  PC8250                   m_COM2;

};


