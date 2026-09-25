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


#include "machine.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <math.h>


#pragma GCC optimize ("O3")




// CGA Craphics Card Ports Bits

#define CGA_MODECONTROLREG_TEXT80         0x01   // 0 = 40x25, 1 = 80x25
#define CGA_MODECONTROLREG_GRAPHICS       0x02   // 0 = text,  1 = graphics
#define CGA_MODECONTROLREG_COLOR          0x04   // 0 = color, 1 = monochrome
#define CGA_MODECONTROLREG_ENABLED        0x08   // 0 = video off, 1 = video on
#define CGA_MODECONTROLREG_GRAPH640       0x10   // 0 = 320x200 graphics, 1 = 640x200 graphics
#define CGA_MODECONTROLREG_BIT7BLINK      0x20   // 0 = text mode bit 7 controls background, 1 = text mode bit 7 controls blinking

#define CGA_COLORCONTROLREG_BACKCOLR_MASK 0x0f   // mask for 320x200 background color index (on 640x200 is the foreground)
#define CGA_COLORCONTROLREG_HIGHINTENSITY 0x10   // select high intensity colors
#define CGA_COLORCONTROLREG_PALETTESEL    0x20   // 0 is Green, red and brown, 1 is Cyan, magenta and white


// Hercules (HGC) Ports Bits

#define HGC_MODECONTROLREG_GRAPHICS       0x02   // 0 = text mode, 1 = graphics mode
#define HGC_MODECONTROLREG_ENABLED        0x08   // 0 = video off, 1 = video on
#define HGC_MODECONTROLREG_BIT7BLINK      0x20   // 0 = text mode bit 7 controls background, 1 = text mode bit 7 controls blinking
#define HGC_MODECONTROLREG_GRAPHICSPAGE   0x80   // 0 = graphics mapped on page 0 (0xB0000), 1 = graphics mapped on page 1 (0xB8000)

#define HGC_CONFSWITCH_ALLOWGRAPHICSMODE  0x01   // 0 = prevents graphics mode, 1 = allows graphics mode
#define HGC_CONFSWITCH_ALLOWPAGE1         0x02   // 0 = prevents access to page 1, 1 = allows access to page 1


// I/O expander (based on MCP23S17) ports

#define EXTIO_CONFIG                    0x00e0   // configuration port (see EXTIO_CONFIG_.... flags)
// whole 8 bit ports handling
#define EXTIO_DIRA                      0x00e1   // port A direction (0 = input, 1 = output)
#define EXTIO_DIRB                      0x00e2   // port B direction (0 = input, 1 = output)
#define EXTIO_PULLUPA                   0x00e3   // port A pullup enable (0 = disabled, 1 = enabled)
#define EXTIO_PULLUPB                   0x00e4   // port B pullup enable (0 = disabled, 1 = enabled)
#define EXTIO_PORTA                     0x00e5   // port A read/write
#define EXTIO_PORTB                     0x00e6   // port B read/write
// single GPIO handling
#define EXTIO_GPIOSEL                   0x00e7   // GPIO selection (0..7 = PA0..PA7, 8..15 = PB0..PB8)
#define EXTIO_GPIOCONF                  0x00e8   // selected GPIO direction and pullup (0 = input, 1 = output, 2 = input with pullup)
#define EXTIO_GPIO                      0x00e9   // selected GPIO read or write (0 = low, 1 = high)

// I/O expander configuration bits
#define EXTIO_CONFIG_AVAILABLE            0x01   // 1 = external IO available, 0 = not available
#define EXTIO_CONFIG_INT_POLARITY         0x02   // 1 = positive polarity, 0 = negative polarity (default)



//////////////////////////////////////////////////////////////////////////////////////
// Machine


using fabgl::i8086;


uint8_t *         Machine::s_memory;
uint8_t *         Machine::s_videoMemory;


Machine::Machine() :
    #ifdef FABGL_EMULATED
    m_stepCallback(nullptr),
    #endif
    m_diskFilename(),
    m_disk(),
    m_diskChanged(),
    m_frameBuffer(nullptr),
    m_bootDrive(0),
    m_sysReqCallback(nullptr),
    m_baseDir(nullptr),
    m_ramSizeKB(640),
    m_EGAActive(false),
    m_EGAACIndex(0),
    m_EGAACFlipFlop(false),
    m_EGAMiscOutput(0),
    m_EGASwitches(0x09),
    m_debugConsoleRequested(false),
    m_breakAddr(0),
    m_egaRomPath(nullptr),
    m_egaBiosLoaded(false)
{
}


Machine::~Machine()
{
  for (int i = 0; i < DISKCOUNT; ++i) {
    free(m_diskFilename[i]);
    if (m_disk[i])
      fclose(m_disk[i]);
  }
  vTaskDelete(m_taskHandle);
  free(s_videoMemory);
}


// A real EGA carries its own BIOS ROM at C000:0000, and a good deal of software decides whether
// an EGA is fitted by looking for the 0x55AA option ROM signature there rather than by asking
// INT 10h. This emulator implements INT 10h inside the system BIOS, so there is no video BIOS
// code to put at C000 -- but the signature has to be there for that test to pass. The entry
// point is a far return, so anything that does call into it gets control straight back, and the
// block is checksummed the way a real option ROM is in case something verifies it.
static void installVideoROMStub(uint8_t * memory)
{
  constexpr uint32_t ADDR = 0xc0000;
  constexpr int      SIZE = 512;          // one block, the smallest a real option ROM can be

  memset(memory + ADDR, 0, SIZE);
  memory[ADDR + 0] = 0x55;
  memory[ADDR + 1] = 0xaa;
  memory[ADDR + 2] = SIZE / 512;
  memory[ADDR + 3] = 0xcb;                // retf

  uint8_t sum = 0;
  for (int i = 0; i < SIZE - 1; ++i)
    sum += memory[ADDR + i];
  memory[ADDR + SIZE - 1] = (uint8_t)(0u - sum);
}


void Machine::init()
{
  srand((uint32_t)time(NULL));

  // to avoid PSRAM bug without -mfix-esp32-psram-cache-issue
  // core 0 can only work reliably with the lower 2 MB and core 1 only with the higher 2 MB.
  s_memory = (uint8_t*)(SOC_EXTRAM_DATA_LOW + (xPortGetCoreID() == 1 ? 2 * 1024 * 1024 : 0));

  s_videoMemory = (uint8_t*)heap_caps_malloc(VIDEOMEMSIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

  // Machines with more than 640KB put ordinary RAM where the IBM video window began, so the
  // CPU must stop routing 0xA0000-0xAFFFF to the video callbacks (which discard it anyway).
  // At 640KB or less the IBM behaviour is kept: that range reads 0xFF and swallows writes.
  i8086::setVideoMemStart(m_ramSizeKB > 640 ? 0xB0000 : 0xA0000);

  memset(s_memory, 0, RAM_SIZE);

  // A real EGA BIOS, if one is configured, replaces the stub entirely: it carries its own
  // signature and provides the INT 10h implementation the stub only pretends to stand for.
  m_egaBiosLoaded = loadEGABios();
  if (m_egaBiosLoaded) {
    // The ROM programs and tests the hardware during its own initialisation, so the planes have
    // to be there before POST calls it. The CGA buffer is released the same way an EGA mode set
    // would do it, and setCGAMode()/setHGCMode() stand down for as long as this is true.
    if (s_videoMemory) {
      heap_caps_free(s_videoMemory);
      s_videoMemory = nullptr;
    }
    if (m_EGA.allocVRAM(EGA_BANK_COUNT)) {
      m_EGA.reset();
      m_EGA.clearVRAM();
      m_EGAActive = true;
      printf("EGA BIOS: planar memory ready, internal heap free %d\n",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
      printf("EGA BIOS: no room for planar memory, falling back to CGA\n");
      s_videoMemory = (uint8_t*) heap_caps_malloc(VIDEOMEMSIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      m_frameBuffer = s_videoMemory;
      m_egaBiosLoaded = false;
    }
  }
  if (!m_egaBiosLoaded)
    installVideoROMStub(s_memory);

  m_soundGen.play(true);
  m_soundGen.attach(&m_sinWaveGen);

  m_i8042.init();
  m_i8042.setCallbacks(this, keyboardInterrupt, mouseInterrupt, resetMachine, sysReq);

  m_PIT8253.setCallbacks(this, PITChangeOut);
  m_PIT8253.reset();

  m_MC146818.init("PCEmulator");
  m_MC146818.setCallbacks(this, MC146818Interrupt);

  m_MCP23S17.begin();
  m_MCP23S17Sel = 0;

  m_BIOS.init(this);

  i8086::setCallbacks(this, readPort, writePort, writeVideoMemory8, writeVideoMemory16, readVideoMemory8, readVideoMemory16, interrupt);
  i8086::setMemory(s_memory);

  // Started from init(), which runs in the emulator task: the monitor reads guest RAM, and that
  // is PSRAM which only this core may touch (see the s_memory assignment above).
  if (m_debugConsoleRequested)
    m_serialDebug.begin(this);

  m_reset = true;
}


void Machine::setDriveImage(int drive, char const * filename, int cylinders, int heads, int sectors)
{
  if (m_disk[drive]) {
    fclose(m_disk[drive]);
    m_disk[drive] = nullptr;
  }

  if (m_diskFilename[drive]) {
    free(m_diskFilename[drive]);
    m_diskFilename[drive] = nullptr;
  }

  m_BIOS.setDriveMediaType(drive, mediaUnknown);

  m_diskCylinders[drive] = cylinders;
  m_diskHeads[drive]     = heads;
  m_diskSectors[drive]   = sectors;
  
  m_diskChanged[drive]   = true;

  if (filename) {
    m_diskFilename[drive] = strdup(filename);
    if (filename[0] == '/') {
      // Absolute path, used as-is rather than relative to the base directory. Images in the
      // SPIFFS partition are opened read-only on purpose: SPIFFS is log structured, so random
      // writes into a large file are slow and wear the flash. A rescue disk is read anyway.
      bool inFlash = strncmp(filename, SPIFFS_MOUNT_PATH "/", sizeof(SPIFFS_MOUNT_PATH "/") - 1) == 0;
      m_disk[drive] = fopen(filename, inFlash ? "rb" : "r+b");
    } else
      m_disk[drive] = FileBrowser(m_baseDir).openFile(filename, "r+b");
    if (m_disk[drive]) {

      // get image file size
      fseek(m_disk[drive], 0L, SEEK_END);
      m_diskSize[drive] = ftell(m_disk[drive]);

      // need to detect geometry?
      if (cylinders == 0 || heads == 0 || sectors == 0)
        autoDetectDriveGeometry(drive);
    }
  }
}


void Machine::autoDetectDriveGeometry(int drive)
{
  // well known floppy formats
  static const struct {
    uint16_t tracks;
    uint8_t  sectors;
    uint8_t  heads;
  } FLOPPYFORMATS[] = {
    { 40,  8, 1 },   //  163840 bytes (160K, 5.25 inch)
    { 40,  9, 1 },   //  184320 bytes (180K, 5.25 inch)
    { 40,  8, 2 },   //  327680 bytes (320K, 5.25 inch)
    { 40,  9, 2 },   //  368640 bytes (360K, 5.25 inch)
    { 80,  9, 2 },   //  737280 bytes (720K, 3.5 inch)
    { 80, 15, 2 },   // 1228800 bytes (1200K, 5.25 inch)
    { 80, 18, 2 },   // 1474560 bytes (1440K, 3.5 inch)
    { 80, 36, 2 },   // 2949120 bytes (2880K, 3.5 inch)
  };

  // look for well known floppy formats
  for (auto const & ff : FLOPPYFORMATS) {
    if (512 * ff.tracks * ff.sectors * ff.heads == m_diskSize[drive]) {
      m_diskCylinders[drive] = ff.tracks;
      m_diskHeads[drive]     = ff.heads;
      m_diskSectors[drive]   = ff.sectors;
      //printf("autoDetectDriveGeometry, found floppy: t=%d s=%d h=%d\n", ff.tracks, ff.sectors, ff.heads);
      return;
    }
  }

  // maybe an hard disk, try to calculate geometry (max 528MB, common lower end for BIOS and MSDOS: https://tldp.org/HOWTO/Large-Disk-HOWTO-4.html)
  constexpr int MAXCYLINDERS = 1024;  // Cylinders : 1...1024
  constexpr int MAXHEADS     = 16;    // Heads     : 1...16 (actual limit is 256)
  constexpr int MAXSECTORS   = 63;    // Sectors   : 1...63
  int c = 1, h = 1;
  int s = (int)(m_diskSize[drive] / 512);
  if (s > MAXSECTORS) {
    h = s / MAXSECTORS;
    s = MAXSECTORS;
  }
  if (h > MAXHEADS) {
    c = h / MAXHEADS;
    h = MAXHEADS;
  }
  if (c > MAXCYLINDERS)
    c = MAXCYLINDERS;
  m_diskCylinders[drive] = c;
  m_diskHeads[drive]     = h;
  m_diskSectors[drive]   = s;
  //printf("autoDetectDriveGeometry, found HD: c=%d h=%d s=%d (tot=%d filesz=%d)\n", m_diskCylinders[drive], m_diskHeads[drive], m_diskSectors[drive], 512 * m_diskCylinders[drive] * m_diskHeads[drive] * m_diskSectors[drive], (int)m_diskSize[drive]);

}


void Machine::setCOM1(SerialPort * serialPort)
{
  m_COM1.setCallbacks(this, COM1Interrupt);
  m_COM1.setSerialPort(serialPort);
}


void Machine::setCOM2(SerialPort * serialPort)
{
  m_COM2.setCallbacks(this, COM2Interrupt);
  m_COM2.setSerialPort(serialPort);
}


void Machine::reset()
{
  m_reset = false;

  m_ticksCounter = 0;

  m_CGAMemoryOffset = 0;
  m_CGAModeReg      = 0;
  m_CGAColorReg     = 0;
  m_CGAVSyncQuery   = 0;

  m_HGCMemoryOffset = 0;
  m_HGCModeReg      = 0;
  m_HGCSwitchReg    = 0;
  m_HGCVSyncQuery   = 0;

  m_speakerDataEnable = false;

  m_i8042.reset();

  m_PIC8259A.reset();
  m_PIC8259B.reset();

  m_PIT8253.reset();
  m_PIT8253.setGate(0, true);
  //m_PIT8253.setGate(1, true); // @TODO: timer 1 used for DRAM refresh, required to run?

  m_MC146818.reset();

  // a warm boot (Ctrl+Alt+Del jumps to the reset vector) has to come back on the CGA adapter,
  // otherwise POST runs with the EGA planes still allocated and no CGA frame buffer
  leaveEGAMode();

  m_COM1.reset();
  m_COM2.reset();

  memset(m_CGA6845, 0, sizeof(m_CGA6845));
  memset(m_HGC6845, 0, sizeof(m_HGC6845));

  m_BIOS.reset();

  i8086::reset();

  // set boot drive (0, 1, 0x80, 0x81)
  i8086::setDL((m_bootDrive & 1) | (m_bootDrive > 1 ? 0x80 : 0x00));
}


void Machine::run()
{
  xTaskCreatePinnedToCore(&runTask, "", 4000, this, 5, &m_taskHandle, CoreUsage::quietCore());
}


void Machine::runTask(void * pvParameters)
{
  auto m = (Machine*)pvParameters;

  m->init();

	while (true) {

    if (m->m_reset)
      m->reset();

    if (m->m_breakAddr &&
        (((uint32_t)i8086::CS() << 4) + i8086::IP()) == m->m_breakAddr) {
      m->m_breakAddr = 0;                 // one shot, so resuming does not trip instantly
      m->m_serialDebug.breakNow();
    }

    #ifdef FABGL_EMULATED
    taskEmuCheck();
    if (m->m_stepCallback)
      m->m_stepCallback(m);
    #endif

    i8086::step();
		m->tick();

	}
}


void Machine::setEGABios(char const * romPath)
{
  free(m_egaRomPath);
  m_egaRomPath = (romPath && romPath[0]) ? strdup(romPath) : nullptr;
}


// Loads the EGA BIOS option ROM into the guest's upper memory at C000:0000. Nothing is executed
// here: POST scans for the 0x55AA signature and far-calls the init entry, which is what lets the
// ROM hook INT 10h and set itself up, exactly as on real hardware.
bool Machine::loadEGABios()
{
  if (!m_egaRomPath)
    return false;

  FILE * f = fopen(m_egaRomPath, "rb");
  if (!f) {
    printf("EGA BIOS: cannot open %s\n", m_egaRomPath);
    return false;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  // C0000..C7FFF is the video ROM window; anything larger is not a video option ROM
  if (size < 512 || size > 32768) {
    printf("EGA BIOS: %s has a bad size (%ld)\n", m_egaRomPath, size);
    fclose(f);
    return false;
  }

  size_t got = fread(s_memory + 0xc0000, 1, size, f);
  fclose(f);

  uint8_t * rom = s_memory + 0xc0000;
  if (got != (size_t)size || rom[0] != 0x55 || rom[1] != 0xaa) {
    printf("EGA BIOS: %s is not an option ROM (read %u of %ld, sig %02X%02X)\n",
           m_egaRomPath, (unsigned)got, size, rom[0], rom[1]);
    memset(rom, 0, size > 0 ? size : 512);
    return false;
  }

  uint8_t sum = 0;
  for (int i = 0; i < rom[2] * 512; ++i)
    sum += rom[i];

  printf("EGA BIOS: loaded %s, %d blocks (%d bytes), checksum %s\n",
         m_egaRomPath, rom[2], rom[2] * 512, sum == 0 ? "ok" : "BAD");

  return true;
}


// The EGA BIOS hooks INT 10h to its own handler and then calls it to set the initial mode. That
// handler programs a real CRTC, Sequencer and Attribute Controller, none of which exist here, so
// it never comes back.
//
// We do not need it. There is already a working INT 10h in this BIOS, with the EGA modes wired
// to the planar engine. So let the ROM hook the vector -- anything inspecting IVT[10h], the save
// pointer table or the ROM signature sees a genuine EGA BIOS -- and redirect the first
// instruction of its handler to a far jump into ours. The card is real where it is looked at and
// ours where it is used.
//
// The hook site is found rather than hardcoded: every EGA BIOS installs its handler with
// "mov word [0x40], imm16", and the immediate is the handler's offset within the ROM.
void Machine::prepareVideoROM()
{
  if (!m_egaBiosLoaded)
    return;

  uint16_t ourIP = *(uint16_t*)(s_memory + 0x10 * 4);
  uint16_t ourCS = *(uint16_t*)(s_memory + 0x10 * 4 + 2);

  uint8_t * rom = s_memory + 0xc0000;

  for (int i = 0; i < 0x8000 - 6; ++i) {
    if (rom[i] == 0xc7 && rom[i + 1] == 0x06 && rom[i + 2] == 0x40 && rom[i + 3] == 0x00) {
      uint16_t handler = rom[i + 4] | (rom[i + 5] << 8);
      uint8_t * p = rom + handler;
      p[0] = 0xea;                          // jmp far ourCS:ourIP
      p[1] = (uint8_t) ourIP;
      p[2] = (uint8_t)(ourIP >> 8);
      p[3] = (uint8_t) ourCS;
      p[4] = (uint8_t)(ourCS >> 8);
      printf("EGA BIOS: INT 10h handler C000:%04X redirected to %04X:%04X\n",
             handler, ourCS, ourIP);
      return;
    }
  }

  printf("EGA BIOS: could not find where the ROM hooks INT 10h\n");
}


void Machine::stepCPU()
{
  i8086::step();
  tick();
}


void Machine::tick()
{
  ++m_ticksCounter;

  if ((m_ticksCounter & 0x03) == 0x03) {
  
    if (m_COM1.assigned())
      m_COM1.tick();
    if (m_COM2.assigned())
      m_COM2.tick();
      
    if ((m_ticksCounter & 0x7f) == 0x7f) {
      m_PIT8253.tick();
      // run keyboard controller every PIT tick (just to not overload CPU with continous checks)
      m_i8042.tick();
    }

    // Far rarer than the devices above: the console only has to notice bytes arriving on a
    // 115200 baud line, and while the monitor is open this call does not return until the user
    // resumes, which is what freezes the guest.
    if ((m_ticksCounter & 0x3fff) == 0x3fff && m_serialDebug.enabled())
      m_serialDebug.poll();

  }
  
  if (m_PIC8259A.pendingInterrupt() && i8086::IRQ(m_PIC8259A.pendingInterruptNum()))
    m_PIC8259A.ackPendingInterrupt();
  if (m_PIC8259B.pendingInterrupt() && i8086::IRQ(m_PIC8259B.pendingInterruptNum()))
    m_PIC8259B.ackPendingInterrupt();

}


void Machine::setCGA6845Register(uint8_t value)
{
  m_CGA6845[m_CGA6845SelectRegister] = value;

  switch (m_CGA6845SelectRegister) {

    // cursor start: (bits 5,6 = cursor blink and visibility control), bits 0..4 cursor start scanline
    case 0x0a:
      m_graphicsAdapter.setCursorVisible((m_CGA6845[0xa] >> 5) >= 2);
      // no break!
    // cursor end: bits 0..4 cursor end scanline
    case 0x0b:
      m_graphicsAdapter.setCursorShape(2 * (m_CGA6845[0xa] & 0x1f), 2 * (m_CGA6845[0xb] & 0x1f));
      break;

    // video memory start offset (0x0c = H, 0x0d = L)
    case 0x0c:
    case 0x0d:
      // In EGA text the start address is how the BIOS scrolls: it points the CRTC at a later
      // character cell rather than moving the screen contents. One address is one character,
      // which is also one plane offset, so the value is used as it stands.
      //
      // For the EGA graphics modes this is also how software page flips, but nothing has
      // exercised that yet, so it is left following the same path rather than special cased.
      if (m_EGAActive) {
        m_graphicsAdapter.setEGAStartAddress((((uint32_t)m_CGA6845[0xc] << 8) | m_CGA6845[0xd])
                                             & m_EGA.offsetMask());
        break;
      }
      m_CGAMemoryOffset = ((m_CGA6845[0xc] << 8) | m_CGA6845[0xd]) << 1;
      setCGAMode();
      break;

    // cursor position (0x0e = H and 0x0f = L)
    case 0x0e:
    case 0x0f:
    {
      int pos = (m_CGA6845[0xe] << 8) | m_CGA6845[0xf];
      m_graphicsAdapter.setCursorPos(pos / m_graphicsAdapter.getTextColumns(), pos % m_graphicsAdapter.getTextColumns());
      break;
    }

  }
}


void Machine::setCGAMode()
{
  // While an EGA mode is up the CGA frame buffer has been released to make room for the planar
  // planes, so every branch below would build m_frameBuffer out of a null pointer and hand it to
  // the scanline ISR. Any stray write to 0x3d8/0x3d9 from the guest would then crash the board.
  // Mode changes go through the BIOS hook, which tears EGA down first, so ignoring these here is
  // safe as well as necessary.
  if (m_EGAActive || !s_videoMemory)
    return;

  if ((m_CGAModeReg & CGA_MODECONTROLREG_ENABLED) == 0) {

    // video disabled
    //printf("CGA, video disabled\n");
    m_graphicsAdapter.enableVideo(false);

  } else if ((m_CGAModeReg & CGA_MODECONTROLREG_TEXT80) == 0 && (m_CGAModeReg & CGA_MODECONTROLREG_GRAPHICS) == 0) {

    // 40 column text mode
    //printf("CGA, 40 columns text mode\n");
    m_frameBuffer = s_videoMemory + 0x8000 + m_CGAMemoryOffset;
    m_graphicsAdapter.setVideoBuffer(m_frameBuffer);
    m_graphicsAdapter.setEmulation(GraphicsAdapter::Emulation::PC_Text_40x25_16Colors);
    m_graphicsAdapter.setBit7Blink(m_CGAModeReg & CGA_MODECONTROLREG_BIT7BLINK);
    m_graphicsAdapter.enableVideo(true);

  } else if ((m_CGAModeReg & CGA_MODECONTROLREG_TEXT80) && (m_CGAModeReg & CGA_MODECONTROLREG_GRAPHICS) == 0) {

    // 80 column text mode
    //printf("CGA, 80 columns text mode\n");
    m_frameBuffer = s_videoMemory + 0x8000 + m_CGAMemoryOffset;
    m_graphicsAdapter.setVideoBuffer(m_frameBuffer);
    m_graphicsAdapter.setEmulation(GraphicsAdapter::Emulation::PC_Text_80x25_16Colors);
    m_graphicsAdapter.setBit7Blink(m_CGAModeReg & CGA_MODECONTROLREG_BIT7BLINK);
    m_graphicsAdapter.enableVideo(true);

  } else if ((m_CGAModeReg & CGA_MODECONTROLREG_GRAPHICS) && (m_CGAModeReg & CGA_MODECONTROLREG_GRAPH640) == 0) {

    // 320x200 graphics
    //printf("CGA, 320x200 graphics mode\n");
    m_frameBuffer = s_videoMemory + 0x8000 + m_CGAMemoryOffset;
    m_graphicsAdapter.setVideoBuffer(m_frameBuffer);
    m_graphicsAdapter.setEmulation(GraphicsAdapter::Emulation::PC_Graphics_320x200_4Colors);
    int paletteIndex = (bool)(m_CGAColorReg & CGA_COLORCONTROLREG_PALETTESEL) * 2 + (bool)(m_CGAColorReg & CGA_COLORCONTROLREG_HIGHINTENSITY);
    m_graphicsAdapter.setPCGraphicsPaletteInUse(paletteIndex);
    m_graphicsAdapter.setPCGraphicsBackgroundColorIndex(m_CGAColorReg & CGA_COLORCONTROLREG_BACKCOLR_MASK);
    m_graphicsAdapter.enableVideo(true);

  } else if ((m_CGAModeReg & CGA_MODECONTROLREG_GRAPHICS) && (m_CGAModeReg & CGA_MODECONTROLREG_GRAPH640)) {

    // 640x200 graphics
    //printf("CGA, 640x200 graphics mode\n");
    m_frameBuffer = s_videoMemory + 0x8000 + m_CGAMemoryOffset;
    m_graphicsAdapter.setVideoBuffer(m_frameBuffer);
    m_graphicsAdapter.setEmulation(GraphicsAdapter::Emulation::PC_Graphics_640x200_2Colors);
    m_graphicsAdapter.setPCGraphicsForegroundColorIndex(m_CGAColorReg & CGA_COLORCONTROLREG_BACKCOLR_MASK);
    m_graphicsAdapter.enableVideo(true);

  }
}


// --- EGA ---------------------------------------------------------------------------------------
//
// The EGA's 128 KB of planar video memory and the 64 KB CGA/Hercules buffer cannot both be held
// in internal RAM on this board, so entering an EGA mode releases the CGA buffer and leaving one
// puts it back. The video memory callbacks tolerate s_videoMemory being null meanwhile: a guest
// write to 0xB8000 while an EGA graphics mode is up is discarded, which is also what a real EGA
// does, since that window is not mapped in modes 0Dh-10h.

// The EGA keeps its font in plane 2, which is why software can redefine the character set. The
// BIOS would normally load it there during a mode set; we do the same, from FabGL's 8x14 ROM
// font, 32 bytes per character as the hardware expects. Anything that later rewrites plane 2 --
// INT 10h AH=11h, or a program poking it directly -- changes what the renderer draws.
void Machine::loadEGAFont(int charHeight)
{
  auto const & f = (charHeight >= 16) ? fabgl::FONT_8x16 : fabgl::FONT_8x14;

  for (int ch = 0; ch < 256; ++ch) {
    for (int row = 0; row < 32; ++row) {
      uint8_t bits = (row < charHeight && row < f.height) ? f.data[ch * f.height + row] : 0x00;
      m_EGA.writePlaneByte(2, (uint32_t)ch * 32 + row, bits);
    }
  }
}


bool Machine::setEGAMode(int biosMode)
{
  // Text modes on the EGA path. These are not the CGA text modes: characters live in plane 0,
  // attributes in plane 1 and the glyphs in plane 2, so the renderer draws whatever font the
  // guest has loaded rather than a fixed one out of ROM.
  if (m_egaBiosLoaded && biosMode <= 0x03) {

    int textColumns = (biosMode <= 0x01) ? 40 : 80;


    m_graphicsAdapter.enableVideo(false);

    if (s_videoMemory) {
      heap_caps_free(s_videoMemory);
      s_videoMemory = nullptr;
    }

    if (!m_EGA.allocVRAM(EGA_BANK_COUNT)) {
      // The CGA buffer has already been released at this point, so it has to come back before
      // the caller falls through to the CGA path -- otherwise that path renders from a null
      // pointer and the video stays dark with no clue as to why.
      printf("EGA: cannot allocate planar memory for text mode, falling back to CGA\n");
      s_videoMemory = (uint8_t*) heap_caps_malloc(VIDEOMEMSIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      if (s_videoMemory)
        memset(s_videoMemory, 0, VIDEOMEMSIZE);
      m_frameBuffer = s_videoMemory;
      m_graphicsAdapter.enableVideo(true);
      return false;
    }

    m_EGAActive = true;
    m_EGA.reset();
    m_EGA.clearVRAM();

    // Program the card the way an EGA BIOS does for an alphanumeric mode. Without this the
    // registers are still at their power-on planar defaults and the guest's character writes go
    // down the four-plane path instead of being split into characters and attributes.
    m_EGA.selectSequencer(2); m_EGA.writeSequencer(0x03);   // map mask: planes 0 and 1
    m_EGA.selectSequencer(4); m_EGA.writeSequencer(0x03);   // memory mode: odd/even addressing
    m_EGA.selectGraphics(5);  m_EGA.writeGraphics(0x10);    // odd/even
    m_EGA.selectGraphics(6);  m_EGA.writeGraphics(0x0e);    // window at B8000, alphanumeric

    // after the registers, so the direct plane writes land where the renderer looks
    loadEGAFont(16);

    // Clear the text screen the way a BIOS mode set does: spaces on light grey over black.
    //
    // clearVRAM() above zeroes every plane, which leaves attribute 0 -- black on black. That is
    // invisible, and it is not obvious from the outside: the characters really are in plane 0,
    // so the serial screen mirror looks perfect while the monitor shows nothing at all. At boot
    // it was masked by POST's own "clear video memory", which writes 0x0700 through the CPU and
    // so sets the attributes; that runs once, so the first mode set after a graphics mode left
    // the screen permanently dark.
    for (uint32_t i = 0; i < (uint32_t)textColumns * 25; ++i) {
      m_EGA.writePlaneByte(0, i, 0x20);      // space
      m_EGA.writePlaneByte(1, i, 0x07);      // light grey on black
    }

    for (int i = 0; i < 16; ++i)
      m_EGAACPalette[i] = i;

    m_graphicsAdapter.setEGAVideoBuffer(m_EGA.banks(), textColumns, m_EGA.bankMask(), m_EGA.offsetMask());
    m_graphicsAdapter.setEGAStartAddress(0);
    m_graphicsAdapter.setEGACharHeight(16);
    m_graphicsAdapter.setEGATextColumns(textColumns);
    m_graphicsAdapter.setEGAPalette(m_EGAACPalette);
    m_graphicsAdapter.setBit7Blink(true);
    m_graphicsAdapter.setEmulation(GraphicsAdapter::Emulation::PC_Text_EGA_80x25_16Colors);
    m_graphicsAdapter.enableVideo(true);

    printf("EGA: text mode %02X, %dx25 with the font in plane 2\n", biosMode, textColumns);
    return true;
  }

  GraphicsAdapter::Emulation em;
  int stride;

  switch (biosMode) {
    case 0x0d: em = GraphicsAdapter::Emulation::PC_Graphics_EGA_320x200_16Colors; stride = 40; break;
    case 0x0e: em = GraphicsAdapter::Emulation::PC_Graphics_EGA_640x200_16Colors; stride = 80; break;
    case 0x0f: em = GraphicsAdapter::Emulation::PC_Graphics_EGA_640x350_2Colors;  stride = 80; break;
    case 0x10: em = GraphicsAdapter::Emulation::PC_Graphics_EGA_640x350_16Colors; stride = 80; break;
    default:   return false;
  }

  // Always the full four banks.
  //
  // Sizing this to what one page of the mode needs looked like a free 64 KB in 640x200, and it
  // is wrong: a mode's displayed page is not all the memory software uses. Mode 0Eh has a page
  // size of 0x4000 and a real EGA holds four of them, 0Dh has eight pages of 0x2000, and
  // software page-flips through them. With two banks everything past byte-column 16384 wrapped
  // onto page 0, which is what made Planet X3 glitch.
  //
  // Four banks give 32768 byte-columns per plane, so two pages in mode 0Eh and four in 0Dh --
  // still half of a real EGA's 64 KB per plane, but enough for the double buffering software
  // actually does.
  constexpr int banks = EGA_BANK_COUNT;

  // stop the scanline ISR before anything it reads is freed or reallocated
  m_graphicsAdapter.enableVideo(false);

  printf("EGA: mode %02X, internal heap free %d (largest %d)\n", biosMode,
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  if (s_videoMemory) {
    heap_caps_free(s_videoMemory);
    s_videoMemory = nullptr;
  }

  if (!m_EGA.allocVRAM(banks)) {
    // out of internal memory: put the CGA buffer back and let the caller use the CGA path
    printf("EGA: cannot allocate planar video memory, staying on CGA\n");
    s_videoMemory = (uint8_t*) heap_caps_malloc(VIDEOMEMSIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    m_frameBuffer = s_videoMemory;
    m_graphicsAdapter.enableVideo(true);
    return false;
  }

  m_EGAActive = true;

  m_EGA.reset();
  m_EGA.clearVRAM();

  // default EGA palette: attribute N selects colour N
  for (int i = 0; i < 16; ++i)
    m_EGAACPalette[i] = i;

  m_graphicsAdapter.setEGAVideoBuffer(m_EGA.banks(), stride, m_EGA.bankMask(), m_EGA.offsetMask());
  m_graphicsAdapter.setEGAStartAddress(0);
  m_graphicsAdapter.setEGAPalette(m_EGAACPalette);
  printf("EGA: planes allocated, internal heap free %d (largest %d)\n",
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  m_graphicsAdapter.setEmulation(em);
  m_graphicsAdapter.enableVideo(true);

  printf("EGA: video up, internal heap free %d (largest %d)\n",
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  return true;
}


void Machine::leaveEGAMode()
{
  if (!m_EGAActive)
    return;

  m_graphicsAdapter.enableVideo(false);

  m_EGA.freeVRAM();
  m_EGAActive = false;

  if (!s_videoMemory)
    s_videoMemory = (uint8_t*) heap_caps_malloc(VIDEOMEMSIZE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (s_videoMemory)
    memset(s_videoMemory, 0, VIDEOMEMSIZE);

  m_frameBuffer = s_videoMemory;

  // the caller (the BIOS mode set) programs the CGA registers next, which picks the mode back up
}


// Attribute Controller, port 0x3c0. One port carries both the index and the data, alternating,
// and reading the status register at 0x3da resets it to expecting an index.
void Machine::setEGAAttributeReg(uint8_t value)
{
  if (!m_EGAACFlipFlop) {
    m_EGAACIndex    = value & 0x1f;
    m_EGAACFlipFlop = true;
  } else {
    m_EGAACFlipFlop = false;
    if (m_EGAACIndex < 16) {
      m_EGAACPalette[m_EGAACIndex] = value & 0x3f;
      if (m_EGAActive)
        m_graphicsAdapter.setEGAPalette(m_EGAACPalette);
    }
  }
}


void Machine::setHGC6845Register(uint8_t value)
{
  m_HGC6845[m_HGC6845SelectRegister] = value;
  switch (m_HGC6845SelectRegister) {

    // cursor start: (bits 5,6 = cursor blink and visibility control), bits 0..4 cursor start scanline
    case 0x0a:
      m_graphicsAdapter.setCursorVisible((m_HGC6845[0xa] >> 5) >= 2);
    // cursor end: bits 0..4 cursor end scanline
    case 0x0b:
      m_graphicsAdapter.setCursorShape((m_HGC6845[0xa] & 0x1f), (m_HGC6845[0xb] & 0x1f));
      break;

    // video memory start offset (0x0c = H, 0x0d = L)
    case 0x0c:
    case 0x0d:
      m_HGCMemoryOffset = ((m_HGC6845[0xc] << 8) | m_HGC6845[0xd]) << 1;
      setHGCMode();
      break;

    // cursor position (0x0e = H and 0x0f = L)
    case 0x0e:
    case 0x0f:
    {
      int pos = (m_HGC6845[0xe] << 8) | m_HGC6845[0xf];
      m_graphicsAdapter.setCursorPos(pos / m_graphicsAdapter.getTextColumns(), pos % m_graphicsAdapter.getTextColumns());
      break;
    }

  }
}


void Machine::setHGCMode()
{
  // same reasoning as setCGAMode()
  if (m_EGAActive || !s_videoMemory)
    return;

  constexpr int HGC_OFFSET_PAGE0 = 0x0000;
  constexpr int HGC_OFFSET_PAGE1 = 0x8000;

  if ((m_HGCModeReg & HGC_MODECONTROLREG_ENABLED) == 0) {

    // video disabled
    //printf("Hercules, video disabled\n");
    m_graphicsAdapter.enableVideo(false);

  } else if ((m_HGCModeReg & HGC_MODECONTROLREG_GRAPHICS) == 0 || (m_HGCSwitchReg & HGC_CONFSWITCH_ALLOWGRAPHICSMODE) == 0) {

    // text mode
    //printf("Hercules, text mode\n");
    m_frameBuffer = s_videoMemory + HGC_OFFSET_PAGE0;
    m_graphicsAdapter.setVideoBuffer(m_frameBuffer);
    m_graphicsAdapter.setEmulation(GraphicsAdapter::Emulation::PC_Text_80x25_16Colors);
    m_graphicsAdapter.setBit7Blink(m_HGCModeReg & HGC_MODECONTROLREG_BIT7BLINK);
    m_graphicsAdapter.enableVideo(true);

  } else if ((m_HGCModeReg &HGC_MODECONTROLREG_GRAPHICS)) {

    // graphics mode
    //printf("Hercules, graphics mode\n");
    int offset = (m_HGCModeReg & HGC_MODECONTROLREG_GRAPHICSPAGE) && (m_HGCSwitchReg & HGC_CONFSWITCH_ALLOWPAGE1) ? HGC_OFFSET_PAGE1 : HGC_OFFSET_PAGE0;
    m_frameBuffer = s_videoMemory + offset;
    m_graphicsAdapter.setVideoBuffer(m_frameBuffer);
    m_graphicsAdapter.setEmulation(GraphicsAdapter::Emulation::PC_Graphics_HGC_720x348);
    m_graphicsAdapter.enableVideo(true);

  }

}


void Machine::writePort(void * context, int address, uint8_t value)
{
  auto m = (Machine*)context;

  switch (address) {

    // PIC8259A
    case 0x20:
    case 0x21:
      m->m_PIC8259A.write(address & 1, value);
      break;

    // PIC8259B
    case 0xa0:
    case 0xa1:
      m->m_PIC8259B.write(address & 1, value);
      break;

    // PIT8253
    case 0x0040:
    case 0x0041:
    case 0x0042:
    case 0x0043:
      m->m_PIT8253.write(address & 3, value);
      if ((address == 0x43 && (value >> 6) == 2) || address == 0x42)
        m->speakerSetFreq();
      break;

    // 8042 keyboard controller input
    case 0x0060:
      m->m_i8042.write(0, value);
      break;

    // PortB
    //   bit 1 : speaker data enable
    //   bit 0 : timer 2 gate
    case 0x0061:
      m->m_speakerDataEnable = value & 0x02;
      m->m_PIT8253.setGate(2, value & 0x01);
      m->speakerEnableDisable();
      break;

    // 8042 keyboard controller input
    case 0x0064:
      m->m_i8042.write(1, value);
      break;

    // MC146818 RTC & RAM
    case 0x0070:
    case 0x0071:
      m->m_MC146818.write(address & 1, value);
      break;
      
    // COM2
    case 0x2f8 ... 0x2ff:
      if (m->m_COM2.assigned())
        m->m_COM2.write(address, value);
      break;

    // EGA - Attribute Controller (index and data share the port)
    case 0x3c0:
      m->setEGAAttributeReg(value);
      break;

    // EGA - Miscellaneous Output Register
    case 0x3c2:
      m->m_EGAMiscOutput = value;
      break;

    // EGA - Sequencer
    case 0x3c4:
      m->m_EGA.selectSequencer(value);
      break;
    case 0x3c5:
      m->m_EGA.writeSequencer(value);
      break;

    // EGA - Graphics Controller
    case 0x3ce:
      m->m_EGA.selectGraphics(value);
      break;
    case 0x3cf:
      m->m_EGA.writeGraphics(value);
      break;

    // CGA - CRT 6845 - register selection register
    case 0x3d4:
      m->m_CGA6845SelectRegister = value;
      break;

    // CGA - CRT 6845 - selected register write
    case 0x3d5:
      m->setCGA6845Register(value);
      break;

    // CGA - Mode Control Register
    case 0x3d8:
      m->m_CGAModeReg = value;
      m->setCGAMode();
      break;

    // CGA - Color Select register
    case 0x3d9:
      m->m_CGAColorReg = value;
      m->setCGAMode();
      break;

    // Hercules (HGC) - CRT 6845 - register selection register
    case 0x3b4:
      m->m_HGC6845SelectRegister = value;
      break;

    // Hercules (HGC) - CRT 6845 - selected register write
    case 0x3b5:
      m->setHGC6845Register(value);
      break;

    // Hercules (HGC) - Display Mode Control Port
    case 0x3b8:
      m->m_HGCModeReg = value;
      m->setHGCMode();
      break;

    // Hercules (HGC) - Configuration Switch
    case 0x3bf:
      m->m_HGCSwitchReg = value;
      m->setHGCMode();
      break;
      
    // COM1
    case 0x3f8 ... 0x3ff:
      if (m->m_COM1.assigned())
        m->m_COM1.write(address, value);
      break;

    // I/O expander - Configuration
    case EXTIO_CONFIG:
      m->m_MCP23S17.setINTActiveHigh(value & EXTIO_CONFIG_INT_POLARITY);
      break;

    // I/O expander - Port A/B Direction
    case EXTIO_DIRA ... EXTIO_DIRB:
      m->m_MCP23S17.setPortDir(address - EXTIO_DIRA + MCP_PORTA, ~value);
      printf("dir %d = %02X\n", address - EXTIO_DIRA + MCP_PORTA, ~value);
      break;

    // I/O expander - Port A/B pullup
    case EXTIO_PULLUPA ... EXTIO_PULLUPB:
      m->m_MCP23S17.enablePortPullUp(address - EXTIO_PULLUPA + MCP_PORTA, value);
      break;

    // I/O expander - Port A/B write
    case EXTIO_PORTA ... EXTIO_PORTB:
      m->m_MCP23S17.writePort(address - EXTIO_PORTA + MCP_PORTA, value);
      printf("set %d = %02X\n", address - EXTIO_PORTA + MCP_PORTA, value);
      break;

    // I/O expander - GPIO selection
    case EXTIO_GPIOSEL:
      m->m_MCP23S17Sel = value & 0xf;
      break;

    // I/O expander - GPIO direction and pullup
    case EXTIO_GPIOCONF:
      m->m_MCP23S17.configureGPIO(m->m_MCP23S17Sel, value & 1 ? fabgl::MCPDir::Output : fabgl::MCPDir::Input, value & 2);
      break;

    // I/O expander - GPIO write
    case EXTIO_GPIO:
      m->m_MCP23S17.writeGPIO(m->m_MCP23S17Sel, value);
      break;

    default:
      //printf("OUT %04x=%02x\n", address, value);
      break;
  }
}


uint8_t Machine::readPort(void * context, int address)
{
  auto m = (Machine*)context;

  switch (address) {

    // PIC8259A
    case 0x0020:
    case 0x0021:
      return m->m_PIC8259A.read(address & 1);

    // PIC8259B
    case 0x00a0:
    case 0x00a1:
      return m->m_PIC8259B.read(address & 1);

    // PIT8253
    case 0x0040:
    case 0x0041:
    case 0x0042:
    case 0x0043:
      return m->m_PIT8253.read(address & 3);

    // 8042 keyboard controller output
    case 0x0060:
      return m->m_i8042.read(0);

    // Port B
    //   bit 5 : timer 2 out
    //   bit 4 : toggles every 15.085us (DMA refresh)
    //   bit 1 : speaker data enable
    //   bit 0 : timer 2 gate
    case 0x0061:
      return ((int)m->m_PIT8253.getOut(2) << 5) |    // bit 5
             (esp_timer_get_time() & 0x10)       |   // bit 4 (toggles every 16us)
             ((int)m->m_speakerDataEnable << 1)  |   // bit 1
             ((int)m->m_PIT8253.getGate(2));         // bit 0

    // I/O port
    case 0x0062:
      return 0x20 * m->m_PIT8253.getOut(2);  // bit 5 = timer 2 output

    // 8042 keyboard controller status register
    case 0x0064:
      return m->m_i8042.read(1);

    // MC146818 RTC & RAM
    case 0x0070:
    case 0x0071:
      return m->m_MC146818.read(address & 1);

    // COM2
    case 0x2f8 ... 0x2ff:
      return m->m_COM2.assigned() ? m->m_COM2.read(address) : 0;

    // CGA - CRT 6845 - register selection register
    case 0x3d4:
      return 0x00;  // not readable

    // CGA - CRT 6845 - selected register read
    case 0x3d5:
      return m->m_CGA6845SelectRegister >= 14 && m->m_CGA6845SelectRegister < 16 ? m->m_CGA6845[m->m_CGA6845SelectRegister] : 0x00;

    // EGA - Attribute Controller readback
    case 0x3c1:
      return m->m_EGAACIndex < 16 ? m->m_EGAACPalette[m->m_EGAACIndex] : 0x00;

    // EGA - Miscellaneous Output Register (readable at 0x3cc)
    case 0x3cc:
      return m->m_EGAMiscOutput;

    // EGA - Input Status Register 0.
    //
    // Bit 4 is the configuration switch sense: the BIOS picks a switch with bits 3-2 of the value
    // it last wrote to the Misc Output register and reads the answer back here. It assembles the
    // four answers into the switch block it then reports through INT 10h AH=12h, and dispatches
    // its own configuration through a jump table indexed by that value.
    //
    // The sense is NOT inverted relative to the value we want reported: feeding back the
    // complement made the BIOS read 0x06 out of a 0x09 switch block and take the wrong table
    // entry. m_EGASwitches is a card set up as "enhanced colour display, primary".
    case 0x3c2:
    {
      int sw = (m->m_EGAMiscOutput >> 2) & 0x03;
      bool on = (m->m_EGASwitches >> sw) & 1;
      return (on ? 0x10 : 0x00) | 0x0f;
    }

    // EGA - Sequencer
    case 0x3c5:
      return m->m_EGA.readSequencer();

    // EGA - Graphics Controller
    case 0x3cf:
      return m->m_EGA.readGraphics();

    // CGA - Color Select register
    // note: this register should be write-only, but some games do not work if it isn't readable
    case 0x3d9:
      return m->m_CGAColorReg;

    // CGA - Status Register
    // real vertical sync is too fast for our slowly emulated 8086, so
    // here it is just a fake, just to allow programs that check it to keep going anyway.
    case 0x3da:
      // on an EGA this read also puts the attribute controller back to expecting an index,
      // which is how every EGA mode set routine starts
      m->m_EGAACFlipFlop = false;
      m->m_CGAVSyncQuery += 1;
      return (m->m_CGAVSyncQuery & 0x7) != 0 ? 0x09 : 0x00; // "not VSync" (0x00) every 7 queries

    // Hercules (HGC) - register selection register
    case 0x3b4:
      return 0x00;  // not readable

    // Hercules (HGC) - selected register read
    case 0x3b5:
      return m->m_HGC6845SelectRegister >= 14 && m->m_HGC6845SelectRegister < 16 ? m->m_HGC6845[m->m_HGC6845SelectRegister] : 0x00;

    // Hercules (HGC) - Display Status Port
    // real vertical sync is too fast for our slowly emulated 8086, so
    // here it is just a fake, just to allow programs that check it to keep going anyway.
    case 0x3ba:
      m->m_HGCVSyncQuery += 1;
      return (m->m_HGCVSyncQuery & 0x7) != 0 ? 0x00 : 0x80; // "not VSync" (0x80) every 7 queries

    // COM1
    case 0x3f8 ... 0x3ff:
      return m->m_COM1.assigned() ? m->m_COM1.read(address) : 0;

    // I/O expander - Configuration
    case EXTIO_CONFIG:
      return (m->m_MCP23S17.available()        ? EXTIO_CONFIG_AVAILABLE    : 0) |
             (m->m_MCP23S17.getINTActiveHigh() ? EXTIO_CONFIG_INT_POLARITY : 0);

    // I/O expander - Port A/B Direction
    case EXTIO_DIRA ... EXTIO_DIRB:
      return m->m_MCP23S17.getPortDir(address - EXTIO_DIRA + MCP_PORTA);

    // I/O expander - Port A/B pullup
    case EXTIO_PULLUPA ... EXTIO_PULLUPB:
      return m->m_MCP23S17.getPortPullUp(address - EXTIO_PULLUPA + MCP_PORTA);

    // I/O expander - Port A/B read
    case EXTIO_PORTA ... EXTIO_PORTB:
      return m->m_MCP23S17.readPort(address - EXTIO_PORTA + MCP_PORTA);

    // I/O expander - GPIO selection
    case EXTIO_GPIOSEL:
      return m->m_MCP23S17Sel;

    // I/O expander - GPIO read
    case EXTIO_GPIO:
      return m->m_MCP23S17.readGPIO(m->m_MCP23S17Sel);

  }

  //printf("IN %04X\n", address);
  return 0xff;
}


void Machine::PITChangeOut(void * context, int timerIndex)
{
  auto m = (Machine*)context;

  // timer 0 trigged?
  if (timerIndex == 0 &&  m->m_PIT8253.getOut(0) == true) {
    // yes, report 8259A-IR0 (IRQ0, INT 08h)
    m->m_PIC8259A.signalInterrupt(0);
  }
}


// reset from 8042
bool Machine::resetMachine(void * context)
{
  auto m = (Machine*)context;
  m->trigReset();
  return true;
}

// SYSREQ (ALT + PRINTSCREEN)
bool Machine::sysReq(void * context)
{
  auto m = (Machine*)context;
  if (m->m_sysReqCallback)
    m->m_sysReqCallback();
  return true;
}


// 8259A-IR1 (IRQ1, INT 09h)
bool Machine::keyboardInterrupt(void * context)
{
  auto m = (Machine*)context;
  return m->m_PIC8259A.signalInterrupt(1);
}


// 8259B-IR4 (IRQ12, INT 074h)
bool Machine::mouseInterrupt(void * context)
{
  auto m = (Machine*)context;
  return m->m_PIC8259B.signalInterrupt(4);
}


// interrupt from MC146818, trig 8259B-IR0 (IRQ8, INT 70h)
bool Machine::MC146818Interrupt(void * context)
{
  auto m = (Machine*)context;
  return m->m_PIC8259B.signalInterrupt(0);
}


// interrupt from COM1, trig 8259A-IR4 (IRQ4, INT 0Ch)
bool Machine::COM1Interrupt(PC8250 * source, void * context)
{
  auto m = (Machine*)context;
  return source->getOut2() && m->m_PIC8259A.signalInterrupt(4);
}


// interrupt from COM2, trig 8259A-IR3 (IRQ3, INT 0Bh)
bool Machine::COM2Interrupt(PC8250 * source, void * context)
{
  auto m = (Machine*)context;
  return source->getOut2() && m->m_PIC8259A.signalInterrupt(3);
}


// The 0xA0000-0xAFFFF window belongs to the EGA when an EGA mode is up, and goes through the
// planar engine. 0xB0000 upwards is the CGA/Hercules buffer, which is released while EGA is
// active -- hence the null checks, which also make a stray write during a mode change harmless.

// When the EGA is live it answers whichever CPU window its Graphics Controller claims -- A0000
// for the graphics modes, B8000 for text -- so the routing follows the card rather than a fixed
// split. Outside that window the CGA/Hercules buffer still applies, when it exists.
void Machine::writeVideoMemory8(void * context, int address, uint8_t value)
{
  auto m = (Machine*)context;
  if (m->m_EGAActive && address >= (int)m->m_EGA.mapBase() && address < (int)m->m_EGA.mapEnd()) {
    m->m_EGA.write8(address - m->m_EGA.mapBase(), value);
    return;
  }
  if (address >= 0xb0000 && s_videoMemory)
    s_videoMemory[address - 0xb0000] = value;
}


void Machine::writeVideoMemory16(void * context, int address, uint16_t value)
{
  auto m = (Machine*)context;
  if (m->m_EGAActive && address >= (int)m->m_EGA.mapBase() && address < (int)m->m_EGA.mapEnd()) {
    m->m_EGA.write16(address - m->m_EGA.mapBase(), value);
    return;
  }
  if (address >= 0xb0000 && s_videoMemory)
    *(uint16_t*)(s_videoMemory + address - 0xb0000) = value;
}


uint8_t Machine::readVideoMemory8(void * context, int address)
{
  auto m = (Machine*)context;
  if (m->m_EGAActive && address >= (int)m->m_EGA.mapBase() && address < (int)m->m_EGA.mapEnd())
    return m->m_EGA.read8(address - m->m_EGA.mapBase());
  if (address >= 0xb0000 && s_videoMemory)
    return s_videoMemory[address - 0xb0000];
  return 0xff;
}


uint16_t Machine::readVideoMemory16(void * context, int address)
{
  auto m = (Machine*)context;
  if (m->m_EGAActive && address >= (int)m->m_EGA.mapBase() && address < (int)m->m_EGA.mapEnd())
    return m->m_EGA.read16(address - m->m_EGA.mapBase());
  if (address >= 0xb0000 && s_videoMemory)
    return *(uint16_t*)(s_videoMemory + address - 0xb0000);
  return 0xffff;
}


bool Machine::interrupt(void * context, int num)
{
  auto m = (Machine*)context;


  // emu interrupts callable only inside the BIOS segment
  if (i8086::CS() == BIOS_SEG) {
    switch (num) {

      // Put Char for debug (AL)
      case 0xf4:
        printf("%c", i8086::AX() & 0xff);
        return true;

      // BIOS helpers (AH = select helper function)
      case 0xf5:
        m->m_BIOS.helpersEntry();
        return true;

      // set or reset flag CF before IRET, replacing value in stack with current value
      case 0xf6:
      {
        auto savedFlags = (uint16_t*) (s_memory + i8086::SS() * 16 + (uint16_t)(i8086::SP() + 4));
        *savedFlags = (*savedFlags & 0xfffe) | i8086::flagCF();
        return true;
      }

      // set or reset flag ZF before IRET, replacing value in stack with current value
      case 0xf7:
      {
        auto savedFlags = (uint16_t*) (s_memory + i8086::SS() * 16 + (uint16_t)(i8086::SP() + 4));
        *savedFlags = (*savedFlags & 0xffbf) | (i8086::flagZF() << 6);
        return true;
      }

      // set or reset flag IF before IRET, replacing value in stack with current value
      case 0xf8:
      {
        auto savedFlags = (uint16_t*) (s_memory + i8086::SS() * 16 + (uint16_t)(i8086::SP() + 4));
        *savedFlags = (*savedFlags & 0xfdff) | (i8086::flagIF() << 9);
        return true;
      }

      // test point P0
      case 0xf9:
        printf("P0 AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X\n", i8086::AX(), i8086::BX(), i8086::CX(), i8086::DX(), i8086::DS());
        return true;

      // test point P1
      case 0xfa:
        printf("P1 AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X\n", i8086::AX(), i8086::BX(), i8086::CX(), i8086::DX(), i8086::DS());
        return true;

      // BIOS disk handler (INT 13h)
      case 0xfb:
        m->m_BIOS.diskHandlerEntry();
        return true;

      // BIOS video handler (INT 10h)
      case 0xfc:
        m->m_BIOS.videoHandlerEntry();
        return true;

    }
  }

  // not hanlded
  return false;
}


void Machine::speakerSetFreq()
{
  //printf("speakerSetFreq: count = %d\n", m_PIT8253.timerInfo(2).resetCount);
  int timerCount = m_PIT8253.timerInfo(2).resetCount;
  if (timerCount == 0)
    timerCount = 65536;
  int freq = PIT_TICK_FREQ / timerCount;
  //printf("   freq = %dHz\n", freq);
  m_sinWaveGen.setFrequency(freq);
}


void Machine::speakerEnableDisable()
{
  bool genEnabled = m_PIT8253.getGate(2);
  if (genEnabled && m_speakerDataEnable) {
    // speaker enabled
    m_sinWaveGen.enable(true);
  } else {
    // speaker disabled
    m_sinWaveGen.enable(false);
  }
}


void Machine::dumpMemory(char const * filename)
{
  constexpr int BLOCKLEN = 1024;
  auto file = FileBrowser(m_baseDir).openFile(filename, "wb");
  if (file) {
    for (int i = 0; i < 1048576; i += BLOCKLEN)
      fwrite(s_memory + i, 1, BLOCKLEN, file);
    fclose(file);
  }
}


void Machine::dumpInfo(char const * filename)
{
  auto file = FileBrowser(m_baseDir).openFile(filename, "wb");
  if (file) {
    // CPU state
    fprintf(file, " CS   DS   ES   SS\n");
    fprintf(file, "%04X %04X %04X %04X\n\n", i8086::CS(), i8086::DS(), i8086::ES(), i8086::SS());
    fprintf(file, " IP   AX   BX   CX   DX   SI   DI   BP   SP\n");
    fprintf(file, "%04X %04X %04X %04X %04X %04X %04X %04X %04X\n\n", i8086::IP(), i8086::AX(), i8086::BX(), i8086::CX(), i8086::DX(), i8086::SI(), i8086::DI(), i8086::BP(), i8086::SP());
    fprintf(file, "O D I T S Z A P C\n");
    fprintf(file, "%d %d %d %d %d %d %d %d %d\n\n", i8086::flagOF(), i8086::flagDF(), i8086::flagIF(), i8086::flagTF(), i8086::flagSF(), i8086::flagZF(), i8086::flagAF(), i8086::flagPF(), i8086::flagCF());
    fprintf(file, "CS+IP: %05X\n", i8086::CS() * 16 + i8086::IP());
    fprintf(file, "SS+SP: %05X\n\n", i8086::SS() * 16 + i8086::SP());
    fclose(file);
  }
}
