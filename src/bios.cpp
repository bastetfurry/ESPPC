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



#include "bios.h"
#include "machine.h"
#include "emudevs/i8086.h"


using fabgl::i8086;


// To compile BIOS execute "cbios.sh"
static const uint8_t biosrom[] = {
  #include "biosrom.h"
};


BIOS::BIOS()
  : m_memory(nullptr)
{
}


void BIOS::init(Machine * machine)
{
  m_machine   = machine;
  m_memory    = m_machine->memory();
  m_i8042     = m_machine->getI8042();
  m_keyboard  = m_i8042->keyboard();
  m_mouse     = m_i8042->mouse();
  m_MC146818  = m_machine->getMC146818();

	// copy bios
  memcpy(m_memory + BIOS_ADDR, biosrom, sizeof(biosrom));

  // setup bootstrap code (starting from 0xFFFF0)
  // for example: "JMP FC00:0100"
  m_memory[0xffff0] = 0xea;
  m_memory[0xffff1] = BIOS_OFF & 0xff;
  m_memory[0xffff2] = BIOS_OFF >> 8;
  m_memory[0xffff3] = BIOS_SEG & 0xff;
  m_memory[0xffff4] = BIOS_SEG >> 8;
}


void BIOS::reset()
{
  m_kbdScancodeComp = 0;
  m_memory[BIOS_DATAAREA_ADDR + BIOS_NUMHD] = (bool)(m_machine->disk(2)) + (bool)(m_machine->disk(3));
}


// drive:
//   0 = floppy 0 (get address from INT 1E)
//   1 = floppy 1 (get address from INT 1E)
//   2 = HD 0     (get address from INT 41)
//   3 = HD 1     (get address from INT 46)
uint32_t BIOS::ebdaAddr() const
{
  return EBDA_SEG_FOR_KB(m_machine->ramSizeKB()) << 4;
}


uint32_t BIOS::getDriveMediaTableAddr(int drive)
{
  int intNum = drive < 2 ? 0x1e : (drive == 2 ? 0x41 : 0x46);
  uint16_t * intAddr = (uint16_t*)(m_memory + intNum * 4);
  return intAddr[0] + intAddr[1] * 16;
}


bool BIOS::checkDriveMediaType(int drive)
{
  if (drive < 2) {
    // FDD
    if (m_mediaType[drive] == mediaUnknown) {
      MediaType mt = mediaUnknown;
      int h = m_machine->diskHeads(drive);
      int t = m_machine->diskCylinders(drive);
      int s = m_machine->diskSectors(drive);
      if (h == 1 && t == 40 && s == 8)
        mt = floppy160KB;
      else if (h == 1 && t == 40 && s == 9)
        mt = floppy180KB;
      else if (h == 2 && t == 40 && s == 8)
        mt = floppy320KB;
      else if (h == 2 && t == 40 && s == 9)
        mt = floppy360KB;
      else if (h == 2 && t == 80 && s == 9)
        mt = floppy720KB;
      else if (h == 2 && t == 80 && s == 15)
        mt = floppy1M2K;
      else if (h == 2 && t == 80 && s == 18)
        mt = floppy1M44K;
      else if (h == 2 && t == 80 && s == 36)
        mt = floppy2M88K;
      setDriveMediaType(drive, mt);
    }
  } else {
    // HDD
    if (m_machine->disk(drive))
      setDriveMediaType(drive, HDD);
  }
  return m_mediaType[drive] != mediaUnknown;
}


void BIOS::setDriveMediaType(int drive, MediaType media)
{
  m_mediaType[drive] = media;

  if (drive < 2) {
    // FDD

    // updates BIOS data area
    uint8_t knownMedia     = 0x10;        // default set bit 4 (known media)
    uint8_t doubleStepping = 0x00;        // reset bit 5 (double stepping)
    uint8_t dataRate       = 0x00;
    uint8_t defs           = 0x00;
    switch (media) {
      case floppy160KB:
      case floppy180KB:
      case floppy320KB:
      case floppy360KB:
        doubleStepping     = 0x20;        // set bit 5 (double stepping)
        dataRate           = 0b01000000;  // 300 KBS
        defs               = 0b00000100;  // Known 360K media in 1.2MB drive
        break;
      case floppy720KB:
        dataRate           = 0b10000000;  // 250 KBS
        defs               = 0b00000111;  // 720K media in 720K drive or 1.44MB media in 1.44MB drive
        break;
      case floppy1M2K:
        dataRate           = 0b00000000;  // 500 KBS
        defs               = 0b00000101;  // Known 1.2MB media in 1.2MB drive
        break;
      case floppy1M44K:
        dataRate           = 0b00000000;  // 500 KBS
        defs               = 0b00000111;  // 720K media in 720K drive or 1.44MB media in 1.44MB drive
        break;
      case floppy2M88K:
        dataRate           = 0b11000000;  // 1 MBS
        defs               = 0b00000111;  // right?
        break;
      case mediaUnknown:
      default:
        knownMedia         = 0x00;        // reset bit 4 (known media)
        break;
    }
    if (m_memory && drive < 2) {
      // BIOS data area
      m_memory[BIOS_DATAAREA_ADDR + BIOS_DRIVE0MEDIATYPE + drive] = knownMedia | doubleStepping | dataRate | defs;
      //printf("%05X = %02X\n", BIOS_DATAAREA_ADDR + BIOS_DRIVE0MEDIATYPE + drive, m_memory[BIOS_DATAAREA_ADDR + BIOS_DRIVE0MEDIATYPE + drive]);

      // INT 1E
      uint32_t maddr = getDriveMediaTableAddr(drive);
      m_memory[maddr + 0x04] = m_machine->diskSectors(drive);

      // original INT 1E (returned in ES:DI, int 13h, serv 08h)
      m_memory[m_origInt1EAddr + 0x04] = m_machine->diskSectors(drive);
      m_memory[m_origInt1EAddr + 0x0b] = m_machine->diskCylinders(drive) - 1;
    }

  } else if (media == HDD) {

    // HDD

    // fill tables pointed by INT 41h or 46h
    uint32_t mtableAddr = getDriveMediaTableAddr(drive);
    *(uint16_t*)(m_memory + mtableAddr + 0x00) = m_machine->diskCylinders(drive);
    *(uint8_t*)(m_memory + mtableAddr + 0x02)  = m_machine->diskHeads(drive);
    *(uint8_t*)(m_memory + mtableAddr + 0x0e)  = m_machine->diskSectors(drive);

  }
}


// AH = select the helper function
void BIOS::helpersEntry()
{
  switch (i8086::AH()) {

    // AH = 0x00, perform some INT 9 tasks (keyboard interrupt handler)
    case 0x00:
      getKeyFromKeyboard();
      break;

    // AH = 0x01, get or extract key from keyboard buffer
    case 0x01:
      getKeyFromBuffer();
      break;

    // AH = 0x02, get shift flags or extended shift flags
    case 0x02:
      getKeyboardFlags();
      break;

    // AH = 0x03, set keyboard typematic rate and delay
    case 0x03:
      setKeyboardTypematicAndDelay();
      break;

    // AH = 0x05, store keyboard key data
    case 0x05:
      storeKeyboardKeyData();
      break;

    // AH = 0x06, pointing device interface
    case 0x06:
      pointingDeviceInterface();
      break;

    // AH = 0x07, synchronize system ticks with RTC
    case 0x07:
      syncTicksWithRTC();
      break;

    // AH = 0x08, set media drive parameters tables address
    //   ES:BX = int 1Eh address (in bios.asm) for floppy drivers
    case 0x08:
      m_origInt1EAddr = i8086::ES() * 16 + i8086::BX();
      // set initial media
      for (int i = 0; i < DISKCOUNT; ++i)
        setDriveMediaType(i, mediaUnknown);
      break;

    // AH = 0x09, report the configured conventional memory size
    //   returns AX = size in KB (excluding the 1KB EBDA), BX = EBDA segment
    case 0x09:
    {
      int kb = m_machine->ramSizeKB();
      i8086::setAX(kb - 1);                       // int12h reports usable KB, EBDA excluded
      i8086::setBX(EBDA_SEG_FOR_KB(kb));
      break;
    }

    // AH = 0x0A, report the configured boot drive
    //   returns AL = 0x00 fd0, 0x01 fd1, 0x80 hd0, 0x81 hd1
    case 0x0a:
      i8086::setAL((m_machine->bootDrive() & 1) | (m_machine->bootDrive() > 1 ? 0x80 : 0x00));
      break;

    // AH = 0x0D, prepare any video option ROM before POST hands control to it
    case 0x0d:
      m_machine->prepareVideoROM();
      break;

    // AH = 0x0E, is an EGA fitted?
    //   returns AL = 1 on a machine with the EGA BIOS ROM, AL = 0 on a CGA machine
    //
    // AH = 0x10, INT 1Ah AH=81h..85h, the Tandy SL/TL DAC calls. SI = the caller's AX.
    //   returns AX and CF as the Tandy BIOS does (see Machine::tandyDACBios)
    case 0x10:
      m_machine->tandyDACBios(i8086::SI());
      break;

    // AH = 0x11, from the DAC's IRQ 7 handler: returns AL = 1 when a block has ended (the handler
    // then issues the INT 15h AX=91FBh callout), AL = 0 for any other IRQ 7
    case 0x11:
      i8086::setAL(m_machine->tandyDACIrq() ? 1 : 0);
      break;

    // AH = 0x13, the text mode fast path of INT 10h: SI = the caller's AX (AH = 02h, 09h, 0Ah or
    // 0Eh). Returns CF = 1 when done (AX is the caller's again), CF = 0 for bios.asm to carry on
    // with its own code, which is what happens in the graphics modes.
    case 0x13:
    {
      uint16_t ax = i8086::SI();
      bool done = fastTextVideo(ax);
      i8086::setAX(ax);
      i8086::setFlagCF(done);
      break;
    }

    // AH = 0x12, at POST: DX = offset of that IRQ 7 handler in the BIOS segment
    case 0x12:
      m_machine->setTandyDACStub(i8086::DX());
      break;

    // AH = 0x0E, which adapter is fitted?
    //   returns AL = 0 CGA, 1 EGA (the EGA BIOS ROM is loaded), 2 Tandy 1000
    //
    // INT 10h AH=10h..12h are EGA calls. A CGA BIOS leaves the registers alone for them, and
    // that is how software tells the two apart, so bios.asm only answers them when this says so.
    // A Tandy answers AH=10h AL=00h..02h, its palette calls, and nothing else of the three.
    case 0x0e:
      i8086::setAL(m_machine->egaBiosInstalled() ? 1 : (m_machine->tandy() ? 2 : 0));
      break;

    // AH = 0x0B, video mode hook, called from int10_set_vm before it programs the CGA registers.
    //   AL = requested BIOS video mode
    //   returns AL = 1 when the emulator has set the mode up itself and the BIOS must skip its
    //               own mode programming, AL = 0 to carry on with the CGA path
    //
    // The EGA modes are handled here rather than in bios.asm because the adapter they need is
    // not a set of CRTC registers but a whole planar memory engine on the C++ side, and the
    // BIOS data area is easier to fill in from here than from the mode tables in the assembler.
    case 0x0b:
    {
      int mode = i8086::AL() & 0x7f;

      // On the EGA path the text modes belong to the EGA too: the glyphs live in plane 2 so the
      // guest can redefine them, which the CGA text path cannot do.
      //
      // Without the EGA ROM there is no EGA at all, so its graphics modes are refused too and
      // the request falls through to the CGA path, as it would on a real CGA.
      bool ega     = m_machine->egaBiosInstalled();
      bool egaText = ega && mode <= 0x03;

      // BL bit 7 is the caller's "keep video memory" (AL bit 7 of the request, see bios.asm)
      if (ega && (egaText || (mode >= 0x0d && mode <= 0x10)) &&
          m_machine->setEGAMode(mode, (i8086::BL() & 0x80) == 0)) {

        int columns  = (mode == 0x0d || (egaText && mode <= 0x01)) ? 40 : 80;
        int charH    = egaText ? 16 : ((mode >= 0x0f) ? 14 : 8);
        int pageSize = egaText ? 0x1000
                               : ((mode == 0x0d) ? 0x2000 : (mode == 0x0e ? 0x4000 : 0x8000));

        auto bda = m_memory + BIOS_DATAAREA_ADDR;

        bda[0x49]                     = (uint8_t) mode;          // current video mode
        *(uint16_t*)(bda + 0x4a)      = (uint16_t) columns;      // text columns
        *(uint16_t*)(bda + 0x4c)      = (uint16_t) pageSize;     // page size in bytes
        *(uint16_t*)(bda + 0x4e)      = 0;                       // page offset
        memset(bda + 0x50, 0, 16);                               // cursor position, all pages
        bda[0x62]                     = 0;                       // active display page
        bda[0x84]                     = 24;                      // text rows - 1
        bda[0x85]                     = (uint8_t) charH;         // scan lines per character

        i8086::setAL(1);
        break;
      }

      // Tandy 1000: modes 8 and 9. For any other mode this puts the video array back to its CGA
      // state and returns false, and the CGA path carries on as usual.
      // BL bit 7 is the caller's "keep video memory" (AL bit 7 of the request, see bios.asm).
      if (m_machine->tandy() && m_machine->setTandyMode(mode, (i8086::BL() & 0x80) == 0)) {

        bool mode9 = mode == 0x09;
        auto bda   = m_memory + BIOS_DATAAREA_ADDR;

        bda[0x49]                     = (uint8_t) mode;          // current video mode
        *(uint16_t*)(bda + 0x4a)      = mode9 ? 40 : 20;         // text columns
        *(uint16_t*)(bda + 0x4c)      = mode9 ? 0x8000 : 0x4000; // page size in bytes
        *(uint16_t*)(bda + 0x4e)      = 0;                       // page offset
        memset(bda + 0x50, 0, 16);                               // cursor position, all pages
        bda[0x62]                     = 0;                       // active display page
        bda[0x65]                     = mode9 ? 0x2b : 0x2a;     // 3D8h as programmed
        bda[0x66]                     = 0x30;                    // 3D9h as programmed
        bda[0x84]                     = 24;                      // text rows - 1
        bda[0x85]                     = 8;                       // scan lines per character
        bda[0x8a]                     = mode9 ? 0xf6 : 0x3f;     // Tandy: page register copy

        i8086::setAL(1);
        break;
      }

      // Not an EGA mode, or the planar memory could not be allocated. Either way the CGA path is
      // about to run, so any EGA mode still up has to be torn down first -- that is what gives
      // the CGA frame buffer its memory back.
      m_machine->leaveEGAMode();
      i8086::setAL(0);
      break;
    }

    // Draw a character in an EGA graphics mode. AL = character, BL = colour. Those modes have no
    // linear frame buffer, so the BIOS cannot plot a glyph the way it does for CGA.
    case 0x0c:
      m_machine->egaDrawChar(i8086::AL(), i8086::BL());
      break;

    default:
      break;

  };
}



// INT 10h text output done natively rather than by the emulated BIOS: set cursor (02h), write
// character and attribute (09h), write character (0Ah) and teletype (0Eh), in text modes 0-3 on
// every adapter. The CPU executed some 150 instructions of BIOS per call for these -- the dispatch
// chain, the mode checks, the address arithmetic and the CRTC update -- which made text output via
// the BIOS the slowest thing on screen (CheckIt: 1559 characters per second on the EGA machine).
//
// Video memory is written through the same handler the guest's own writes go through, so the CGA
// buffer, the EGA's odd/even planes and the Tandy behave exactly as they did with the asm; only the
// emulated instructions around each write are gone. The behaviour follows bios.asm, apart from
// two quirks of its teletype that are not kept: a line feed without scrolling left the hardware
// cursor behind until the next character, and the page checks were signed.
//
// The BIOS data area: 40:49 mode, 40:4A columns, 40:4C page size, 40:50 cursor (column, row) for
// eight pages, 40:62 active page, 40:84 rows - 1.
bool BIOS::fastTextVideo(uint16_t callerAX)
{
  auto bda  = m_memory + BIOS_DATAAREA_ADDR;
  int  mode = bda[0x49];
  if (mode > 3)
    return false;

  int service = callerAX >> 8;
  int page    = (service == 0x0e) ? bda[0x62] : i8086::BH();
  if (page > 7)
    return true;                 // no such page: the asm returned without doing anything too

  if (service == 0x02) {
    textSetCursor(page, i8086::DH(), i8086::DL());
    return true;
  }

  int      cols     = *(uint16_t*)(bda + 0x4a);
  uint32_t pageBase = 0xb8000 + (uint32_t)page * *(uint16_t*)(bda + 0x4c);
  uint8_t  col      = bda[0x50 + page * 2];
  uint8_t  row      = bda[0x51 + page * 2];
  uint32_t addr     = pageBase + ((uint32_t)row * cols + col) * 2;
  uint8_t  ch       = callerAX & 0xff;

  switch (service) {

    // character and attribute (BL), CX times, from the cursor on; the cursor does not move
    case 0x09:
      for (int n = i8086::CX(); n > 0; --n, addr += 2) {
        m_machine->writeMemDebug(addr, ch);
        m_machine->writeMemDebug(addr + 1, i8086::BL());
      }
      return true;

    // character only, CX times, the attributes left as they are
    case 0x0a:
      for (int n = i8086::CX(); n > 0; --n, addr += 2)
        m_machine->writeMemDebug(addr, ch);
      return true;

    // teletype on the active page: control characters move the cursor, anything else is written
    // (attribute unchanged) and advances it, wrapping and scrolling at the edges
    case 0x0e:
    {
      int lastRow = bda[0x84];
      switch (ch) {
        case 0x08:                           // backspace: blank the cell, step back
          m_machine->writeMemDebug(addr, 0x20);
          col = col ? col - 1 : 0;
          break;
        case 0x0d:                           // carriage return
          col = 0;
          break;
        case 0x0a:                           // line feed (and, as in bios.asm, back to column 0)
          col = 0;
          if (++row > lastRow) {
            row = lastRow;
            textScrollUp(page, 0x07);
          }
          break;
        default:
          m_machine->writeMemDebug(addr, ch);
          if (++col >= cols) {
            col = 0;
            if (++row > lastRow) {
              row = lastRow;
              textScrollUp(page, 0x07);
            }
          }
          break;
      }
      textSetCursor(page, row, col);
      return true;
    }
  }
  return false;
}


// The cursor of a page in the BIOS data area, and the CRTC's when it is the active page (row * cols
// + column, like bios.asm's set_crtc_cursor_pos). The CRTC goes through the port handler so its
// registers and the adapter's cursor stay in step, as with the guest's own OUTs.
void BIOS::textSetCursor(int page, int row, int col)
{
  auto bda = m_memory + BIOS_DATAAREA_ADDR;
  bda[0x50 + page * 2] = col;
  bda[0x51 + page * 2] = row;
  if (page != bda[0x62])
    return;
  uint16_t pos = row * *(uint16_t*)(bda + 0x4a) + col;
  m_machine->writePortDebug(0x3d4, 0x0e);
  m_machine->writePortDebug(0x3d5, pos >> 8);
  m_machine->writePortDebug(0x3d4, 0x0f);
  m_machine->writePortDebug(0x3d5, pos & 0xff);
}


// The whole page up one line, the new bottom line filled with character 0 in attr, as the teletype
// path of bios.asm does it (scroll_up_window with AL = 1 over the full screen).
void BIOS::textScrollUp(int page, int attr)
{
  auto     bda   = m_memory + BIOS_DATAAREA_ADDR;
  int      cols  = *(uint16_t*)(bda + 0x4a);
  int      rows  = bda[0x84] + 1;
  uint32_t base  = 0xb8000 + (uint32_t)page * *(uint16_t*)(bda + 0x4c);
  uint32_t bytes = (uint32_t)(rows - 1) * cols * 2;
  uint32_t line  = cols * 2;
  for (uint32_t i = 0; i < bytes; ++i)
    m_machine->writeMemDebug(base + i, m_machine->readMemDebug(base + line + i));
  for (uint32_t i = 0; i < line; i += 2) {
    m_machine->writeMemDebug(base + bytes + i, 0x00);
    m_machine->writeMemDebug(base + bytes + i + 1, attr);
  }
}


// convert keypad scancode to number, or -1 if not applicable
int convKeypadScancodeToNum(uint8_t scancode)
{
  // LUT to convert scancode 0x47 to 0x52
  static const int8_t CONV[12] = { 7, 8, 9, -1, 4, 5, 6, -1, 1, 2, 3, 0 };
  return scancode >= 0x47 && scancode <= 0x52 ? CONV[scancode - 0x47] : -1;
}


// - update keyboard shift flags
// - adapt fabgl scancode to PC scancodes
// ret: true if the key need to be inserted into the keyboard buffer
bool BIOS::processScancode(int scancode, uint16_t * syscode)
{
  struct ScanCode2SysCode {
    uint8_t  scancode;
    uint16_t syscode[4];    // 0 = normal, 1 = shifted, 2 = control, 3 = alt
  };
  static const ScanCode2SysCode SCODE2SYSCODE[] = {
    { 0x29, 0x2960, 0x297e, 0xffff, 0x2900 }, { 0x02, 0x0231, 0x0221, 0xffff, 0x7800 }, { 0x03, 0x0332, 0x0340, 0x0300, 0x7900 },
    { 0x04, 0x0433, 0x0423, 0xffff, 0x7a00 }, { 0x05, 0x0534, 0x0524, 0xffff, 0x7b00 }, { 0x06, 0x0635, 0x0625, 0xffff, 0x7c00 },
    { 0x07, 0x0736, 0x075e, 0x071e, 0x7d00 }, { 0x08, 0x0837, 0x0826, 0xffff, 0x7e00 }, { 0x09, 0x0938, 0x092a, 0xffff, 0x7f00 },
    { 0x0a, 0x0a39, 0x0a28, 0xffff, 0x8000 }, { 0x0b, 0x0b30, 0x0b29, 0xffff, 0x8100 }, { 0x0c, 0x0c2d, 0x0c5f, 0x0c1f, 0x8200 },
    { 0x0d, 0x0d3d, 0x0d2b, 0xffff, 0x8300 }, { 0x0e, 0x0e08, 0x0e08, 0x0e7f, 0x0e00 }, { 0x0f, 0x0f09, 0x0f00, 0x9400, 0xa500 },
    { 0x10, 0x1071, 0x1051, 0x1011, 0x1000 }, { 0x11, 0x1177, 0x1157, 0x1117, 0x1100 }, { 0x12, 0x1265, 0x1245, 0x1205, 0x1200 },
    { 0x13, 0x1372, 0x1352, 0x1312, 0x1300 }, { 0x14, 0x1474, 0x1454, 0x1414, 0x1400 }, { 0x15, 0x1579, 0x1559, 0x1519, 0x1500 },
    { 0x16, 0x1675, 0x1655, 0x1615, 0x1600 }, { 0x17, 0x1769, 0x1749, 0x1709, 0x1700 }, { 0x18, 0x186f, 0x184f, 0x180f, 0x1800 },
    { 0x19, 0x1970, 0x1950, 0x1910, 0x1900 }, { 0x1a, 0x1a5b, 0x1a7b, 0x1a1b, 0x1a00 }, { 0x1b, 0x1b5d, 0x1b7d, 0x1b1d, 0x1b00 },
    { 0x2b, 0x2b5c, 0x2b7c, 0x2b1c, 0x2b00 }, { 0x1e, 0x1e61, 0x1e41, 0x1e01, 0x1e00 }, { 0x1f, 0x1f73, 0x1f53, 0x1f13, 0x1f00 },
    { 0x20, 0x2064, 0x2044, 0x2004, 0x2000 }, { 0x21, 0x2166, 0x2146, 0x2106, 0x2100 }, { 0x22, 0x2267, 0x2247, 0x2207, 0x2200 },
    { 0x23, 0x2368, 0x2348, 0x2308, 0x2300 }, { 0x24, 0x246a, 0x244a, 0x240a, 0x2400 }, { 0x25, 0x256b, 0x254b, 0x250b, 0x2500 },
    { 0x26, 0x266c, 0x264c, 0x260c, 0x2600 }, { 0x27, 0x273b, 0x273a, 0xffff, 0x2700 }, { 0x28, 0x2827, 0x2822, 0xffff, 0x2800 },
    { 0x2b, 0x2b5c, 0x2b7c, 0x2b1c, 0xffff }, { 0x1c, 0x1c0d, 0x1c0d, 0x1c0a, 0x1c00 }, { 0x56, 0x565c, 0x567c, 0xffff, 0xffff },
    { 0x2c, 0x2c7a, 0x2c5a, 0x2c1a, 0x2c00 }, { 0x2d, 0x2d78, 0x2d58, 0x2d18, 0x2d00 }, { 0x2e, 0x2e63, 0x2e43, 0x2e03, 0x2e00 },
    { 0x2f, 0x2f76, 0x2f56, 0x2f16, 0x2f00 }, { 0x30, 0x3062, 0x3042, 0x3002, 0x3000 }, { 0x31, 0x316e, 0x314e, 0x310e, 0x3100 },
    { 0x32, 0x326d, 0x324d, 0x320d, 0x3200 }, { 0x33, 0x332c, 0x333c, 0xffff, 0x3300 }, { 0x34, 0x342e, 0x343e, 0xffff, 0x3400 },
    { 0x35, 0x352f, 0x353f, 0xffff, 0x3500 }, { 0x39, 0x3920, 0x3920, 0x3920, 0x3920 }, { 0x47, 0x4700, 0x4737, 0x7700, 0xffff },
    { 0x4b, 0x4b00, 0x4b34, 0x7300, 0xffff }, { 0x4f, 0x4f00, 0x4f31, 0x7500, 0xffff }, { 0x48, 0x4800, 0x4838, 0x8d00, 0xffff },
    { 0x4c, 0x4c00, 0x4c35, 0x8f00, 0xffff }, { 0x50, 0x5000, 0x5032, 0x9100, 0xffff }, { 0x52, 0x5200, 0x5230, 0x9200, 0xffff },
    { 0x37, 0x372a, 0x372a, 0x9600, 0x3700 }, { 0x49, 0x4900, 0x4939, 0x8400, 0xffff }, { 0x4d, 0x4d00, 0x4d36, 0x7400, 0xffff },
    { 0x51, 0x5100, 0x5133, 0x7600, 0xffff }, { 0x53, 0x5300, 0x532e, 0x9300, 0xffff }, { 0x4a, 0x4a2d, 0x4a2d, 0x8e00, 0x4a00 },
    { 0x4e, 0x4e2b, 0x4e2b, 0x9000, 0x4e00 }, { 0x01, 0x011b, 0x011b, 0x011b, 0x0100 }, { 0x3b, 0x3b00, 0x5400, 0x5e00, 0x6800 },
    { 0x3c, 0x3c00, 0x5500, 0x5f00, 0x6900 }, { 0x3d, 0x3d00, 0x5600, 0x6000, 0x6a00 }, { 0x3e, 0x3e00, 0x5700, 0x6100, 0x6b00 },
    { 0x3f, 0x3f00, 0x5800, 0x6200, 0x6c00 }, { 0x40, 0x4000, 0x5900, 0x6300, 0x6d00 }, { 0x41, 0x4100, 0x5a00, 0x6400, 0x6e00 },
    { 0x42, 0x4200, 0x5b00, 0x6500, 0x6f00 }, { 0x43, 0x4300, 0x5c00, 0x6600, 0x7000 }, { 0x44, 0x4400, 0x5d00, 0x6700, 0x7100 },
    { 0x57, 0x8500, 0x8700, 0x8900, 0x8b00 }, { 0x58, 0x8600, 0x8800, 0x8a00, 0x8c00 },
    { 0x00, 0xffff, 0xffff, 0xffff, 0xffff } // ending code
  };
  static const ScanCode2SysCode ESCODE2SYSCODE[] = {
    { 0x52, 0x52e0, 0x52e0, 0x92e0, 0xa200 }, { 0x53, 0x53e0, 0x53e0, 0x93e0, 0xa300 }, { 0x4b, 0x4be0, 0x4be0, 0x73e0, 0x9b00 },
    { 0x47, 0x47e0, 0x47e0, 0x77e0, 0x9700 }, { 0x4f, 0x4fe0, 0x4fe0, 0x75e0, 0x9f00 }, { 0x48, 0x48e0, 0x48e0, 0x8de0, 0x9800 },
    { 0x50, 0x50e0, 0x50e0, 0x91e0, 0xa000 }, { 0x49, 0x49e0, 0x49e0, 0x84e0, 0x9900 }, { 0x51, 0x51e0, 0x51e0, 0x76e0, 0xa100 },
    { 0x4d, 0x4de0, 0x4de0, 0x74e0, 0x9d00 }, { 0x35, 0xe02f, 0xe02f, 0x9500, 0xa400 }, { 0x1c, 0xe00d, 0xe00d, 0xe00a, 0xa600 },
    { 0x37, 0xffff, 0xffff, 0x7200, 0xffff }, // CTRL + PRINTSCREEN
    { 0x46, 0xffff, 0xffff, 0x0000, 0xffff }, // CTRL + PAUSE (BREAK)
    { 0x00, 0xffff, 0xffff, 0xffff, 0xffff }  // ending code
  };

  //printf("  %02X\n", scancode);

  *syscode = 0xffff;

  // 3 = RALT, 2 = RCTRL, 1 = E0, 0 = E1
  uint8_t * mode = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDMODE;

  // save and reset e0 and e1 flags
  bool e0 = *mode & 0x02;
  bool e1 = *mode & 0x01;
  *mode &= 0xfc;

  // e0?
  if (scancode == 0xe0) {
    *mode |= 0x02;
    return false;
  }

  // e1?
  if (scancode == 0xe1) {
    *mode |= 0x01;
    return false;
  }

  bool down = !(scancode & 0x80); // down if bit 7 = 0
  scancode &= 0x7f;

  //printf("  e0 = %d, down = %d\n", e0, down);

  // 7 = INS ON, 6 = CAPS ON, 5 = NUMLCK ON, 4 = SCRLCK ON, 3 = ALT, 2 = CTRL, 1 = LSHIFT, 0 = RSHIFT
  uint8_t * flags1 = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDSHIFTFLAGS1;

  // 7 = INS, 6 = CAPS, 5 = NUMLCK, 4 = SCRLCK, 3 = CTRL+NUMLCK ON (PAUSE), 2 = SYSREQ, 1 = LALT, 0 = LCTRL
  uint8_t * flags2 = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDSHIFTFLAGS2;

  // 2 = CAPS LED, 1 = NUMLCK LED, 0 = SCRLCK LED
  uint8_t * LEDs   = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDLEDS;

  uint8_t * altKeypadEntry = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDALTKEYPADENTRY;

  if (e0) {
    // extended code (0xe0 ...)
    switch (scancode) {
      // RCTRL
      case 0x1d:
        *flags1  = (*flags1 & ~0x04) | (0x04 * down);  // bit 2 on flags1 (1 = down)
        *mode    = (*mode   & ~0x04) | (0x04 * down);  // bit 2 on mode   (1 = down)
        break;
      // RALT
      case 0x38:
        *flags1  = (*flags1 & ~0x08) | (0x08 * down);  // bit 3 on flags1 (1 = down)
        *mode    = (*mode   & ~0x08) | (0x08 * down);  // bit 3 on mode   (1 = down)
        break;
      // INSERT
      case 0x52:
        *flags1 ^= 0x80 * !down;                       // bit 7 on flags1 (toggle when up)
        *flags2  = (*flags2 & ~0x80) | (0x80 * down);  // bit 7 on flags2 (1 = down)
        break;
      // PRINTSCREEN or SYSREQ
      case 0x37:
        // not shifts, PRINTSCREEN
        if (down && (*flags1 & 0x0f) == 0)
          m_memory[BIOS_DATAAREA_ADDR + BIOS_PRINTSCREENFLAG] = 1;
        // ALT + PRINTSCREEN = SYSREQ
        else if (*flags1 & 0x08)
          *flags2 |= 0x04;
        break;
      // CTRL + BREAK (CTRL + PAUSE)
      case 0x46:
        emptyKbdBuffer();
        m_memory[BIOS_DATAAREA_ADDR + BIOS_CTRLBREAKFLAG] = 0x80;
        break;
      // bypass (e0 2a / e0 aa)
      case 0x2a:
        return false;
    }
  } else if (e1 || m_kbdScancodeComp > 0) {
    // extended code (0xe1 ...)
    if ((m_kbdScancodeComp == 0 && scancode == 0x1d) || (m_kbdScancodeComp == 1 && scancode == 0x45) || (m_kbdScancodeComp == 2 && scancode == 0x1d)) {
      ++m_kbdScancodeComp;
      return false;
    } else if (m_kbdScancodeComp == 3 && scancode == 0x45) {
      // PAUSE key completed (e1 1d 45 e1 9d c5)
      *flags2 |= 0x08;                                 // bit 3 on flags2 (always set)
      m_kbdScancodeComp = 0;
      return false;
    }
    m_kbdScancodeComp = 0;
  } else {
    // normal code
    m_kbdScancodeComp = 0;
    switch (scancode) {
      // LALT
      case 0x38:
        *flags1  = (*flags1 & ~0x08) | (0x08 * down);  // bit 3 on flags1 (1 = down)
        *flags2  = (*flags2 & ~0x02) | (0x02 * down);  // bit 1 on flags2 (1 = down)
        break;
      // LSHIFT
      case 0x2a:
        *flags1  = (*flags1 & ~0x02) | (0x02 * down);  // bit 1 on flags1 (1 = down)
        break;
      // RSHIFT
      case 0x36:
        *flags1  = (*flags1 & ~0x01) | (0x01 * down);  // bit 0 on flags1 (1 = down)
        break;
      // LCTRL
      case 0x1d:
        *flags1  = (*flags1 & ~0x04) | (0x04 * down);  // bit 2 on flags1 (1 = down)
        *flags2  = (*flags2 & ~0x01) | (0x01 * down);  // bit 0 on flags2 (1 = down)
        break;
      // SCROLLLOCK
      case 0x46:
        *flags1 ^= 0x10 * !down;                       // bit 4 on flags1 (toggle when up)
        *flags2  = (*flags2 & ~0x10) | (0x10 * down);  // bit 4 on flags2 (1 = down)
        *LEDs   ^= 0x01 * !down;                       // bit 0 on LEDs   (toggle when up)
        break;
      // NUMLOCK
      case 0x45:
        *flags1 ^= 0x20 * !down;                       // bit 5 on flags1 (toggle when up)
        *flags2  = (*flags2 & ~0x20) | (0x20 * down);  // bit 5 on flags2 (1 = down)
        *LEDs   ^= 0x02 * !down;                       // bit 1 on LEDs   (toggle when up)
        break;
      // CAPSLOCK
      case 0x3a:
        *flags1 ^= 0x40 * !down;                       // bit 6 on flags1 (toggle when up)
        *flags2  = (*flags2 & ~0x40) | (0x40 * down);  // bit 6 on flags2 (1 = down)
        *LEDs   ^= 0x04 * !down;                       // bit 2 on LEDs   (toggle when up)
        break;
      // KEYPAD INS (KEYPAD 0)
      case 0x52:
        // NUMLOCK = off ?, interpret keypad0 as INSERT toggle
        if ((*flags1 & 0x20) == 0)
          *flags1 ^= 0x80 * !down;                     // bit 7 on flags1 (toggle when up)
        break;
    }
  }

  bool lalt = *flags1 & 0x08;

  // manage LALT + KEYPAD NUM
  if (lalt && scancode != 0x38 && !e0) {
    // ALT was down, is this a keypad number?
    int num = convKeypadScancodeToNum(scancode);
    if (num >= 0) {
      // yes this is a keypad num, if down update altKeypadEntry
      if (down)
        *altKeypadEntry = (*altKeypadEntry * 10 + num) & 0xff;
      return false;
    } else {
      // no, back to normal case
      *altKeypadEntry = 0;
    }
  } else if (*altKeypadEntry > 0 && scancode == 0x38 && !down) {
    // ALT is up and altKeypadEntry contains a valid value, add it
    *syscode = *altKeypadEntry;  // high byte will be 0x00, low byte is the ascii value just typed
    *altKeypadEntry = 0;
    return true;
  }

  if (down) {
    bool shift    = *flags1 & 0x03;
    bool capslock = *flags1 & 0x40;
    bool numlock  = *flags1 & 0x20;
    bool ctrl     = *flags1 & 0x04;

    // CAPSLOCK enabled and letter
    if (capslock && ((scancode >= 0x10 && scancode <= 0x19) || (scancode >= 0x1e && scancode <= 0x26) || (scancode >= 0x2c && scancode <= 0x32)))
      shift = !shift;

    // NUMLOCK and keypad
    if (numlock && (scancode >= 0x47 && scancode <= 0x53))
      shift = !shift;

    // convert scancode to system code
    for (auto CONV = (e0 ? ESCODE2SYSCODE : SCODE2SYSCODE); CONV->scancode; ++CONV)
      if (CONV->scancode == scancode) {
        *syscode = CONV->syscode[shift ? 1 : ctrl ? 2 : lalt ? 3 : 0];
        return *syscode != 0xffff;
      }
  }

  return false;
}


// ret false on buffer full
bool BIOS::storeKeyInKbdBuffer(uint16_t syscode)
{
  // check space in BIOS keyboard buffer
  auto head = (uint16_t*)(m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDBUFHEAD);
  auto tail = (uint16_t*)(m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDBUFTAIL);
  if (*head - 2 != *tail && (*head != BIOS_KBDBUF || *tail != BIOS_KBDBUF + 30)) {
    // insert key into the keyboard buffer
    *(uint16_t*)(m_memory + BIOS_DATAAREA_ADDR + *tail) = syscode;
    *tail = (*tail == BIOS_KBDBUF + 30 ? BIOS_KBDBUF : *tail + 2);
    return true;  // success
  }
  // buffer full
  //printf("kbd buffer full\n");
  return false;
}


// perform some INT 9 tasks (keyboard interrupt handler)
// intput AL:
//    scancode as read from port 0x60
// output AH:
//    0 : normal key
//    2 : CTRL + ALT + DEL
//    3 : PRINTSCREEN
//    4 : CTRL-BREAK  (CTRL + PAUSE)
//    5 : SYSREQ      (ALT + PRINTSCREEN), down (AL = 0), up (AL = 1)
void BIOS::getKeyFromKeyboard()
{
  uint8_t * flags1 = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDSHIFTFLAGS1;
  uint8_t * flags2 = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDSHIFTFLAGS2;
  // saves current pause state
  bool onPause = *flags2 & 0x08;
  // update keyboard decoding state
  uint16_t syscode; // low byte = ASCII value, high byte = scancode
  bool toAdd = processScancode(i8086::AL(), &syscode);
  if (toAdd) {
    if (onPause) {
      // just disable pause state and discard key
      *flags2 &= ~0x08;
    } else {
      // we need to add this key to the keyboard buffer
      //printf("kbd store %04X\n", syscode);
      if (!storeKeyInKbdBuffer(syscode)) {
        // buffer full
        //printf("kbd buffer full\n");
      }
    }
  }
  // check for special syskeys
  if ((*flags1 & 0x04) && (*flags1 & 0x08) && (syscode == 0x53e0 || syscode == 0x93e0 || syscode == 0xa300)) {
    i8086::setAH(2);
  } else if (m_memory[BIOS_DATAAREA_ADDR + BIOS_PRINTSCREENFLAG] == 1) {
    i8086::setAH(3);
  } else if (syscode == 0x0000) {
    i8086::setAH(4);
  } else if (*flags2 & 0x04) {
    i8086::setAH(5);
    i8086::setAL((bool)i8086::AL());
  } else
    i8086::setAH(0);
}


// get or extract key from keyboard buffer
// updates keyboard LEDs
// input AL:
//    bit 0 : 0 = check only, 1 = extract
//    bit 1 : 0 = do not filter, 1 = filter extended keys
// output:
//    AX : ASCII (AL) and scancode (AH)
//    ZF : 0 = key present, 1 = key not present
void BIOS::getKeyFromBuffer()
{
  // this LUT transform extended keyboard syskeys to XT syscodes
  struct Ext2XT {
    uint16_t esyscode;
    uint16_t xsyscode;  // 0xffff = don't return
  };
  static const Ext2XT EXT2XT[] = {
    { 0x2900, 0xffff }, { 0x0e00, 0xffff }, { 0x9400, 0xffff }, { 0xa500, 0xffff }, { 0x1a00, 0xffff }, { 0x1b00, 0xffff }, { 0x2b00, 0xffff },
    { 0x2700, 0xffff }, { 0x2800, 0xffff }, { 0x1c00, 0xffff }, { 0x3300, 0xffff }, { 0x3400, 0xffff }, { 0x3500, 0xffff }, { 0x52e0, 0x5200 },
    { 0x92e0, 0xffff }, { 0xa200, 0xffff }, { 0x53e0, 0x5300 }, { 0x93e0, 0xffff }, { 0xa300, 0xffff }, { 0x4be0, 0x4b00 }, { 0x73e0, 0x7300 },
    { 0x9b00, 0xffff }, { 0x47e0, 0x4700 }, { 0x77e0, 0x7700 }, { 0x9700, 0xffff }, { 0x4fe0, 0x4f00 }, { 0x75e0, 0x7500 }, { 0x9f00, 0xffff },
    { 0x48e0, 0x4800 }, { 0x8de0, 0xffff }, { 0x9800, 0xffff }, { 0x50e0, 0x5000 }, { 0x91e0, 0xffff }, { 0xa000, 0xffff }, { 0x49e0, 0x4900 },
    { 0x84e0, 0x8400 }, { 0x9900, 0xffff }, { 0x51e0, 0x5100 }, { 0x76e0, 0x7600 }, { 0xa100, 0xffff }, { 0x4de0, 0x4d00 }, { 0x74e0, 0x7400 },
    { 0x9d00, 0xffff }, { 0xe02f, 0x352f }, { 0x9500, 0xffff }, { 0xa400, 0xffff }, { 0x8d00, 0xffff }, { 0x8f00, 0xffff }, { 0x9100, 0xffff },
    { 0x9200, 0xffff }, { 0x9600, 0xffff }, { 0x3700, 0xffff }, { 0x9300, 0xffff }, { 0x8e00, 0xffff }, { 0x4a00, 0xffff }, { 0x9000, 0xffff },
    { 0x4e00, 0xffff }, { 0xe00d, 0x1c0d }, { 0xe00a, 0x1c0a }, { 0xa600, 0xffff }, { 0x0100, 0xffff }, { 0x8500, 0xffff }, { 0x8700, 0xffff },
    { 0x8900, 0xffff }, { 0x8b00, 0xffff }, { 0x8600, 0xffff }, { 0x8800, 0xffff }, { 0x8a00, 0xffff }, { 0x8c00, 0xffff },
  };

  // return value is not valid (ZF = 1)
  i8086::setFlagZF(1);

  auto head = (uint16_t*)(m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDBUFHEAD);
  auto tail = (uint16_t*)(m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDBUFTAIL);

  if (*head != *tail) {

    // get key from buffer head
    uint16_t k = * (uint16_t*) (m_memory + BIOS_DATAAREA_ADDR + *head);

    bool filtered = false;

    // filter extended keys?
    if (i8086::AL() & 0x02) {
      for (int i = 0; i < sizeof(EXT2XT) / sizeof(Ext2XT); ++i) {
        if (EXT2XT[i].esyscode == k) {
          if (EXT2XT[i].xsyscode == 0xffff)
            filtered = true;        // don't return
          else
            k = EXT2XT[i].xsyscode; // replace
          break;
        }
      }
    }

    // remove from buffer?
    if (filtered || (i8086::AL() & 0x01))
      *head = (*head == BIOS_KBDBUF + 30 ? BIOS_KBDBUF : *head + 2);

    if (!filtered) {
      // return value is valid (ZF = 0)
      i8086::setFlagZF(0);
      i8086::setAX(k);
    }

  }

  // update LEDs
  bool numLockLED, capsLockLED, scrollLockLED;
  m_keyboard->getLEDs(&numLockLED, &capsLockLED, &scrollLockLED);
  uint8_t LEDs = m_memory[BIOS_DATAAREA_ADDR + BIOS_KBDLEDS];
  if (numLockLED != (bool)(LEDs & 0x02) || capsLockLED != (bool)(LEDs & 0x04) || scrollLockLED != (bool)(LEDs & 0x01))
    m_keyboard->setLEDs((bool)(LEDs & 0x02), (bool)(LEDs & 0x04), (bool)(LEDs & 0x01));
}


void BIOS::emptyKbdBuffer()
{
  auto head = (uint16_t*)(m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDBUFHEAD);
  auto tail = (uint16_t*)(m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDBUFTAIL);
  *tail = *head;
}


// get shift flags or extended shift flags
// input AL:
//   bit 0 : 0 = normal flags in AL, 1 = normal flags in AL and extended flags in AH
// output AL or AX
void BIOS::getKeyboardFlags()
{
  uint8_t * flags1 = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDSHIFTFLAGS1;
  uint8_t * flags2 = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDSHIFTFLAGS2;
  uint8_t * mode   = m_memory + BIOS_DATAAREA_ADDR + BIOS_KBDMODE;
  if (i8086::AL() & 1)
    i8086::setAH( (*flags2 & 0xf3) | (*mode & 0x0c) );
  i8086::setAL(*flags1);
}


// inputs:
//    AL : service (0x05 = set typematic rate and delay)
//    BH : delay value (as expected by BIOS, 0 = 250ms, 1 = 500ms, etc...)
//    BL : typematic rate (as expected by BIOS, 0 = 30, 1 = 26.7, etc...)
// notes:
//    this method doesn't use 0x60 and 0x64 ports, but directly interfaces to the Keyboard object
void BIOS::setKeyboardTypematicAndDelay()
{
  if (i8086::AL() == 0x05) {
    // send command "set typematic rate and delay" (0xF3) to the keyboard and wait for ACK (0xFA)
    if (!m_keyboard->sendCommand(0xF3, 0xFA))
      return;
    // send parameters
    m_keyboard->sendCommand(i8086::BL() | (i8086::BH() << 5), 0xFA);
  }
}


// inputs:
//    CL : ASCII character
//    CH : scan code
// outputs:
//    AL : 0 = no error, 1 = keyboard buffer full
//    CF : 0 = no error, 1 = keyboard buffer full
void BIOS::storeKeyboardKeyData()
{
  bool r = storeKeyInKbdBuffer(i8086::CX());
  i8086::setAL(!r);
  i8086::setFlagCF(!r);
}


// Implements all services of "INT 15 Function C2h"
// inputs:
//    AL : subfunction
//    .. : depends by the subfunction
// outputs:
//    AH : 0 = success, >0 = error (see "INT 15h Function C2h - Pointing Device Interface")
//    CF : 0 = successful, 1 = unsuccessful
//    .. : depends by the subfunction
void BIOS::pointingDeviceInterface()
{
  if (m_mouse->isMouseAvailable()) {

    i8086::setAH(0x00);
    i8086::setFlagCF(0);

    switch (i8086::AL()) {

      // Enable/disable pointing device
      // inputs:
      //    AL : 0x00
      //    BH : 0 = disable, 1 = enable
      case 0x00:
        m_i8042->enableMouse(i8086::BH());
        break;

      // Reset pointing device
      // inputs:
      //    AL : 0x01
      // outputs:
      //    BH : Device ID
      case 0x01:
        m_i8042->enableMouse(false);             // mouse disabled
        m_mouse->setSampleRate(100);             // 100 reports/second
        m_mouse->setResolution(2);               // 4 counts/millimeter
        m_mouse->setScaling(1);                  // 1:1 scaling
        i8086::setBH(m_mouse->deviceID() & 0xff);
        break;

      // Set sample rate
      // inputs:
      //    AL : 0x02
      //    BH : Sample rate
      case 0x02:
        m_mouse->setSampleRate(i8086::BH());
        break;

      // Set resolution
      // inputs:
      //    AL : 0x03
      //    BH : Resolution value
      case 0x03:
        m_mouse->setResolution(i8086::BH());
        break;

      // Read device type
      // inputs:
      //    AL : 0x04
      case 0x04:
        i8086::setBH(m_mouse->deviceID() & 0xff);
        break;

      // Initialize pointing device interface
      // inputs:
      //    AL : 0x05
      //    BH : Data package size (1-8, in bytes)
      //         note: this value is acqually ignored because we get actual packet size from Mouse object
      case 0x05:
      {
        m_i8042->enableMouse(false);              // mouse disabled
        m_mouse->setSampleRate(100);              // 100 reports/second
        m_mouse->setResolution(2);                // 4 counts/millimeter
        m_mouse->setScaling(1);                   // 1:1 scaling
        uint8_t * EBDA = m_memory + ebdaAddr();
        EBDA[EBDA_DRIVER_OFFSET] = 0x0000;
        EBDA[EBDA_DRIVER_SEG]    = 0x0000;
        EBDA[EBDA_FLAGS1]        = 0x00;
        EBDA[EBDA_FLAGS2]        = m_mouse->getPacketSize(); // instead of i8086::BH()!!
        break;
      }

      // Set scaling or get status
      // inputs:
      //    AL : 0x06
      //    BH : subfunction
      case 0x06:
        switch (i8086::BH()) {
          // Set scaling factor to 1:1
          // inputs:
          //    BH : 0x01
          case 0x01:
            m_mouse->setScaling(1);
            break;
          // Set scaling factor to 2:1
          // inputs:
          //    BH : 0x02
          case 0x02:
            m_mouse->setScaling(2);
            break;
          default:
            // not implements
            printf("Pointing device function 06:%02X not implemented\n", i8086::BH());
            i8086::setAH(0x86);
            i8086::setFlagCF(1);
            break;
        }
        break;

      // Set pointing device handler address
      // inputs:
      //    AL = 0x07
      //    ES:BX : Pointer to application-program's device driver
      case 0x07:
      {
        uint8_t * EBDA = m_memory + ebdaAddr();
        *(uint16_t*)(EBDA + EBDA_DRIVER_OFFSET) = i8086::BX();
        *(uint16_t*)(EBDA + EBDA_DRIVER_SEG)    = i8086::ES();
        EBDA[EBDA_FLAGS2] |= 0x80;  // set handler installed flag
        break;
      }

      default:
        // not implements
        printf("Pointing device function %02X not implemented\n", i8086::AL());
        i8086::setAH(0x86);
        i8086::setFlagCF(1);
        break;
    }

  } else {
    // mouse not available
    i8086::setAH(0x03);   // 0x03 = interface error
    i8086::setFlagCF(1);
  }
}


// convert packed BCD to decimal
static uint8_t BCDtoByte(uint8_t v)
{
  return (v & 0x0F) + (v >> 4) * 10;
}


// synchronize system ticks with RTC
void BIOS::syncTicksWithRTC()
{
  m_MC146818->updateTime();
  int ss = BCDtoByte(m_MC146818->reg(0x00));
  int mm = BCDtoByte(m_MC146818->reg(0x02));
  int hh = BCDtoByte(m_MC146818->reg(0x04));
  int totSecs = ss + mm * 60 + hh * 3600 + 1000;
  int64_t pitTicks = (int64_t)totSecs * PIT_TICK_FREQ;
  *(uint32_t*)(m_memory + BIOS_DATAAREA_ADDR + BIOS_SYSTICKS) = (uint32_t) (pitTicks / 65536);
}


// INT 13 services
void BIOS::diskHandlerEntry()
{
  if (i8086::DL() < 2)
    diskHandler_floppy();
  else
    diskHandler_HD();
}


void BIOS::diskHandler_floppy()
{
  int drive   = i8086::DL();
  int service = i8086::AH();

  //printf("INT 13h, FDD (%d), service %02X\n", drive, service);

  if (m_machine->disk(drive) == nullptr) {
    // invalid drive
    diskHandler_floppyExit(0x80, true);
    return;
  }

  // read, write, verify and format light the on-screen drive LED, if it is enabled
  if (service >= 0x02 && service <= 0x05)
    m_machine->graphicsAdapter()->driveActivity(drive);

  switch (service) {

    // Reset Diskette System
    case 0x00:
      diskHandler_floppyExit(m_mediaType[drive] == mediaUnknown ? 0x06 : 0x00, true);
      return;

    // Read Diskette Status
    case 0x01:
      diskHandler_floppyExit(m_memory[BIOS_DATAAREA_ADDR + BIOS_DISKLASTSTATUS], false);
      m_memory[BIOS_DATAAREA_ADDR + BIOS_DISKLASTSTATUS] = 0; // this function resets BIOS_DISKLASTSTATUS
      return;

    // Read Diskette Sectors
    case 0x02:
    // Write Diskette Sectors
    case 0x03:
    // Verify Diskette Sectors
    case 0x04:
    {
      if (!checkDriveMediaType(drive)) {
        diskHandler_floppyExit(6, true);
        return;
      }
      uint32_t pos, dest, count;
      if (!diskHandler_calcAbsAddr(drive, &pos, &dest, &count)) {
        diskHandler_floppyExit(4, true);  // sector not found
        return;
      }
      size_t sects = i8086::AL();
      if (service != 0x04)
        sects = diskHandler_transfer(drive, service == 0x02, pos, dest, count) / 512;
      i8086::setAL(sects);
      diskHandler_floppyExit(sects == 0 ? 4 : 0, true);
      m_machine->resetDiskChanged(drive);
      return;
    }

    // Format Diskette Track
    case 0x05:
    {
      int sectsCountToFormat = i8086::AL();
      int track              = i8086::CH();
      int head               = i8086::DH();
      uint32_t tableAddr     = i8086::ES() * 16 + i8086::BX();

      int SPT = m_machine->diskSectors(drive);
      int tracksCount = m_machine->diskCylinders(drive);

      uint8_t fillByte = m_memory[getDriveMediaTableAddr(drive) + 8];

      uint8_t * buf = (uint8_t*) malloc(512);
      memset(buf, fillByte, 512);

      for (int i = 0; i < sectsCountToFormat; ++i) {
        int ttrack  = m_memory[tableAddr++];
        int thead   = m_memory[tableAddr++];
        int tsect   = m_memory[tableAddr++];
        int tsectSz = 128 << m_memory[tableAddr++];
        if (ttrack != track || thead > 1 || tsect > SPT || tsectSz != 512 || track >= tracksCount) {
          // error
          free(buf);
          diskHandler_floppyExit(0x04, true);
          return;
        }
        fseek(m_machine->disk(drive), 512 * ((track * 2 + head) * SPT + (tsect - 1)), 0);
        fwrite(buf, 1, 512, m_machine->disk(drive));
        //printf("  track=%d head=%d sect=%d pos=%d fill=%02X\n", track, head, tsect, 512 * ((track * 2 + head) * SPT + (tsect - 1)), fillByte);
      }
      free(buf);
      diskHandler_floppyExit(0x00, true);
      m_machine->resetDiskChanged(drive);
      return;
    }

    // Read Drive Parameters
    case 0x08:
      i8086::setAX(0x0000);
      i8086::setBH(0x00);
      if (!checkDriveMediaType(drive)) {
        i8086::setCX(0x0000);
        i8086::setDX(0x0000);
        i8086::setES(0x0000);
        i8086::setDI(0x0000);
        diskHandler_floppyExit(6, true);
        return;
      }
      switch (m_mediaType[drive]) {
        case floppy160KB:
        case floppy180KB:
        case floppy320KB:
          // @TODO: check this
          i8086::setBL(0x01);
          break;
        case floppy360KB:
          i8086::setBL(0x01);
          break;
        case floppy720KB:
          i8086::setBL(0x03);
          break;
        case floppy1M2K:
          i8086::setBL(0x02);
          break;
        case floppy1M44K:
          i8086::setBL(0x04);
          break;
        case floppy2M88K:
          i8086::setBL(0x05);
          break;
        case mediaUnknown:
        default:
          // not possible here
          break;
      }
      i8086::setCH(m_machine->diskCylinders(drive) - 1);                      // max usable track number
      i8086::setCL(m_machine->diskSectors(drive));                            // max usable sector number
      i8086::setDH(m_machine->diskHeads(drive) - 1);                          // max usable head number
      i8086::setDL((bool)(m_machine->disk(0)) + (bool)(m_machine->disk(1)));  // number of diskette installed
      // Pointer to Diskette Parameters table for the maximum media type supported on the specified drive
      i8086::setES(BIOS_SEG);
      i8086::setDI(m_origInt1EAddr - BIOS_SEG * 16);
      diskHandler_floppyExit(0, true);
      //printf("  INT 13h, FDD, 08h: BL=%d, CH=%d, CL=%d, DL=%d\n", i8086::BL(), i8086::CH(), i8086::CL(), i8086::DL());
      return;

    // Read Drive Type
    case 0x15:
      diskHandler_floppyExit(0, true);
      i8086::setAH(m_machine->disk(drive) ? 0x02 : 0x00);
      return;

    // Detect Media Change
    case 0x16:
      diskHandler_floppyExit(m_machine->diskChanged(drive) ? 0x06 : 0x00, true);
      return;

    // Set Diskette Type
    case 0x17:
      switch (i8086::AL()) {

        // 320K/360K
        case 0x01:
          diskHandler_floppyExit(m_mediaType[drive] != floppy360KB &&
                                 m_mediaType[drive] != floppy320KB &&
                                 m_mediaType[drive] != floppy180KB &&
                                 m_mediaType[drive] != floppy160KB, true);
          break;

        // 360K
        case 0x02:
          diskHandler_floppyExit(m_mediaType[drive] != floppy360KB, true);
          break;

        // 1.2MB
        case 0x03:
          diskHandler_floppyExit(m_mediaType[drive] != floppy1M2K, true);
          break;

        // 720KB
        case 0x04:
          diskHandler_floppyExit(m_mediaType[drive] != floppy720KB, true);
          break;

        // error
        default:
          diskHandler_floppyExit(1, true);
          break;

      }
      m_machine->resetDiskChanged(drive);
      return;

    // Set Media Type for Format
    case 0x18:
    {
      // check if proposed media type matches with current
      int propTracks = i8086::CH();
      int propSPT    = i8086::CL();
      int tracks     = m_machine->diskCylinders(drive) - 1;
      int SPT        = m_machine->diskSectors(drive);
      if (propTracks == tracks && propSPT == SPT) {
        // match ok
        diskHandler_floppyExit(0x00, true);
        i8086::setES(BIOS_SEG);
        i8086::setDI(m_origInt1EAddr - BIOS_SEG * 16);
      } else {
        // not supported
        diskHandler_floppyExit(0x0c, true);
        printf("  INT 13h, FDD, 18h: unsupported media type, t=%d (%d), s=%d (%d)\n", propTracks, tracks, propSPT, SPT);
      }
      m_machine->resetDiskChanged(drive);
      return;
    }

    default:
      // invalid function
      diskHandler_floppyExit(1, true);
      printf("  INT 13h, FDD, invalid service %02X\n", service);
      return;

  }
}


// CH       : low 8 bits of track number
// CL 6...7 : high 2 bits of track number
// CL 0...5 : sector number
// DH       : head number
// AL       : number of sectors to read
// ES:BX    : destination address
bool BIOS::diskHandler_calcAbsAddr(int drive, uint32_t * pos, uint32_t * dest, uint32_t * count)
{
  int sectorsPerTrack = m_machine->diskSectors(drive);
  int heads           = m_machine->diskHeads(drive);
  int track  = i8086::CH() | (((uint16_t)i8086::CL() & 0xc0) << 2);
  int sector = i8086::CL() & 0x3f;
  int head   = i8086::DH();
  // sectors count from 1: sector 0 made pos wrap to 0xFFFFFE00, the fseek failed, and the transfer
  // silently used whatever position the file was left at. A head past the geometry would alias
  // into the next cylinder.
  if (sector < 1 || sector > sectorsPerTrack || head >= heads)
    return false;
  *pos   = 512 * ((track * heads + head) * sectorsPerTrack + (sector - 1));
  *dest  = i8086::ES() * 16 + i8086::BX();
  *count = i8086::AL() * 512;
  return true;
}


// Moves count bytes between the image at pos and guest memory at dest, as the DMA would. Only
// memory below the video window is taken directly; the rest of the transfer is open bus, so a read
// into it is dropped and a write from it stores 0xFF. Straight to m_memory, a transfer there went
// past the video adapter, overwrote the ROMs, or past 1MB ran off the end of guest RAM.
size_t BIOS::diskHandler_transfer(int drive, bool read, uint32_t pos, uint32_t dest, uint32_t count)
{
  FILE * file = m_machine->disk(drive);
  uint32_t ramEnd = i8086::videoMemStart();
  uint32_t direct = dest < ramEnd ? fabgl::tmin<uint32_t>(count, ramEnd - dest) : 0;
  size_t done = 0;
  fseek(file, pos, SEEK_SET);
  if (direct)
    done = read ? fread(m_memory + dest, 1, direct, file) : fwrite(m_memory + dest, 1, direct, file);
  if (done < direct)
    return done;
  uint32_t rest = count - direct;
  if (read) {
    // nothing to store, but the sectors must still exist on the image
    uint64_t size = m_machine->diskSize(drive);
    uint64_t from = (uint64_t) pos + direct;
    return done + (from >= size ? 0 : (size_t) fabgl::tmin<uint64_t>(rest, size - from));
  }
  for (; rest && fputc(0xFF, file) != EOF; --rest)
    ++done;
  return done;
}


void BIOS::diskHandler_floppyExit(uint8_t err, bool setErrStat)
{
  i8086::setAH(err);
  i8086::setFlagCF(err ? 1 : 0);
  if (setErrStat)
    m_memory[BIOS_DATAAREA_ADDR + BIOS_DISKLASTSTATUS] = err;

  /*
  if (err > 0)
    printf("  err = %d\n", err);
    */
}


void BIOS::diskHandler_HD()
{
  int drive   = (i8086::DL() & 1) + 2;  // 2 = HD0, 3 = HD1
  int service = i8086::AH();

  //printf("INT 13h, HDD (%d), service %02X [AH=%02X DL=%02X]\n", drive, service, i8086::AH(), i8086::DL());

  if (m_machine->disk(drive) == nullptr || i8086::DL() > 0x81) {
    // invalid drive
    diskHandler_HDExit(0x80, true);
    return;
  }

  // read, write, verify and format light the on-screen drive LED, if it is enabled
  if (service >= 0x02 && service <= 0x05)
    m_machine->graphicsAdapter()->driveActivity(drive);

  switch (service) {

    // Reset Fixed Disk System
    case 0x00:
      diskHandler_HDExit(checkDriveMediaType(drive) ? 0x00 : 0x80, true);
      return;

    // Read Disk Status
    case 0x01:
      diskHandler_HDExit(m_memory[BIOS_DATAAREA_ADDR + BIOS_HDLASTSTATUS], false);
      m_memory[BIOS_DATAAREA_ADDR + BIOS_HDLASTSTATUS] = 0; // this function resets BIOS_HDLASTSTATUS
      return;

    // Read Fixed Disk Sectors
    case 0x02:
    // Write Fixed Disk Sectors
    case 0x03:
    // Verify Fixed Disk Sectors
    case 0x04:
    {
      if (!checkDriveMediaType(drive)) {
        diskHandler_HDExit(0x80, true);
        return;
      }
      uint32_t pos, dest, count;
      if (!diskHandler_calcAbsAddr(drive, &pos, &dest, &count)) {
        diskHandler_HDExit(4, true);  // sector not found
        return;
      }
      size_t sects = i8086::AL();
      if (service != 0x04)
        sects = diskHandler_transfer(drive, service == 0x02, pos, dest, count) / 512;
      i8086::setAL(sects);
      diskHandler_HDExit(sects == 0 ? 4 : 0, true);
      return;
    }

    // Format Disk Cylinder
    case 0x05:
      diskHandler_HDExit(0x00, true);
      return;

    // Read Drive Parameters
    case 0x08:
      i8086::setAL(0x00);
      if (checkDriveMediaType(drive)) {
        uint16_t maxUsableCylNum  = m_machine->diskCylinders(drive) - 1;
        uint8_t  maxUsableSecNum  = m_machine->diskSectors(drive);
        uint8_t  maxUsableHeadNum = m_machine->diskHeads(drive) - 1;
        i8086::setCH(maxUsableCylNum & 0xff);                                       // Maximum usable cylinder number (low 8 bits)
        i8086::setCL(((maxUsableCylNum >> 2) & 0xc0) |                              // Bits 7-6 = Maximum usable cylinder number (high 2 bits)
                     (maxUsableSecNum & 0x3f));                                     // Bits 5-0 = Maximum usable sector number
        i8086::setDH(maxUsableHeadNum);                                             // Maximum usable head number
        i8086::setDL((bool)(m_machine->disk(2)) + (bool)(m_machine->disk(3)));      // Number of drives (@TODO: correct? Docs not clear)
        // Address of Fixed Disk Parameters table
        // *** note: some texts tell ES:DI should return a pointer to parameters table. IBM docs don't. Actually
        //           returning ES:DI may crash old MSDOS versions!
        //i8086::setES(BIOS_SEG);
        //i8086::setDI(getDriveMediaTableAddr(drive) - BIOS_SEG * 16);
/*
        printf("CH = %02X CL = %02X DH = %02X DL = %02X\n", i8086::CH(), i8086::CL(), i8086::DH(), i8086::DL());
        printf("ES = %04X  DI = %04X\n", i8086::ES(), i8086::DI());
        printf("ES:DI [00-01] = %d\n", (int) *(uint16_t*)(m_memory + i8086::ES() * 16 + i8086::DI() + 0x00));
        printf("ES:DI    [02] = %d\n", (int) *(uint8_t*)(m_memory + i8086::ES() * 16 + i8086::DI() + 0x02));
        printf("ES:DI    [0e] = %d\n", (int) *(uint8_t*)(m_memory + i8086::ES() * 16 + i8086::DI() + 0x0e));
*/
        diskHandler_HDExit(0x00, true);
      } else {
        i8086::setCX(0x0000);
        i8086::setDX(0x0000);
        diskHandler_HDExit(0x80, true);
      }
      return;

    // Initialize Drive Parameters
    case 0x09:
    // Seek to Cylinder
    case 0x0c:
    // Recalibrate Drive
    case 0x11:
    // Controller Internal Diagnostic
    case 0x14:
      diskHandler_HDExit(checkDriveMediaType(drive) ? 0x00 : 0x80, true);
      return;

    // Test for Drive Ready
    case 0x10:
      diskHandler_HDExit(checkDriveMediaType(drive) ? 0x00 : 0xAA, true);
      return;

    // Read Disk Type
    case 0x15:
      if (checkDriveMediaType(drive)) {
        diskHandler_HDExit(0x00, true);
        i8086::setAH(0x03);// drive present
        auto sectors = m_machine->diskSize(drive) / 512;
        i8086::setDX(sectors & 0xffff);
        i8086::setCX(sectors >> 16);
      } else {
        i8086::setAX(0x0000);
        i8086::setCX(0x0000);
        i8086::setDX(0x0000);
        diskHandler_HDExit(0x00, true); // yes, it is 0x00!
      }
      return;

    default:
      // invalid function
      printf("INT 13h, HDD, invalid service %02X\n", service);
      diskHandler_HDExit(1, true);
      return;

  }
}


void BIOS::diskHandler_HDExit(uint8_t err, bool setErrStat)
{
  i8086::setAH(err);
  i8086::setFlagCF(err ? 1 : 0);
  if (setErrStat)
    m_memory[BIOS_DATAAREA_ADDR + BIOS_HDLASTSTATUS] = err;
}


// Offset into the frame buffer of pixel x, y in a Tandy 16 colour mode, or 0xffffffff if the adapter
// is not in one or the pixel is off the screen
static uint32_t tandyPixelAddress(GraphicsAdapter::Emulation emulation, int x, int y)
{
  if (y < 0 || y >= 200 || x < 0)
    return 0xffffffff;
  if (emulation == GraphicsAdapter::Emulation::PC_Graphics_Tandy_320x200_16Colors && x < 320)
    return ((y & 3) << 13) + (y >> 2) * 160 + (x >> 1);
  if (emulation == GraphicsAdapter::Emulation::PC_Graphics_Tandy_160x200_16Colors && x < 160)
    return ((y & 1) << 13) + (y >> 1) * 80 + (x >> 1);
  return 0xffffffff;
}


void BIOS::videoHandlerEntry()
{
  auto ga = m_machine->graphicsAdapter();
  auto fb = m_machine->frameBuffer();

  switch (i8086::AH()) {

    // Write Pixel
    case 0x0c:
      // The EGA graphics modes keep pixels in four bit planes, so they cannot go through the
      // frame buffer cases below.
      if (m_machine->egaWritePixel(i8086::CX(), i8086::DX(),
                                   i8086::AL() & 0x0f, i8086::AL() & 0x80))
        break;

      switch (ga->emulation()) {

        case GraphicsAdapter::Emulation::PC_Graphics_320x200_4Colors:
        {
          constexpr uint32_t rowlen = 320 / 4;
          const uint8_t  value      = i8086::AL() & 0b11;
          const bool     xored      = i8086::AL() & 0x80;
          const uint32_t col        = i8086::CX();
          const uint32_t row        = i8086::DX();
          const uint32_t addr       = (row >> 1) * rowlen + (col >> 2) + (row & 1) * 0x2000;
          const uint32_t shift      = 6 - (col & 3) * 2;
          if (xored)
            fb[addr] ^= value << shift;
          else
            fb[addr]  = (fb[addr] & ~(0b11 << shift)) | (value << shift);
          break;
        }

        // Tandy 16 colours: a nibble per pixel, high nibble first, the lines of a character row
        // in 8 KB banks (four in 320x200, two in 160x200)
        case GraphicsAdapter::Emulation::PC_Graphics_Tandy_320x200_16Colors:
        case GraphicsAdapter::Emulation::PC_Graphics_Tandy_160x200_16Colors:
        {
          uint32_t addr = tandyPixelAddress(ga->emulation(), i8086::CX(), i8086::DX());
          if (addr == 0xffffffff)
            break;
          const uint8_t shift = (i8086::CX() & 1) ? 0 : 4;
          const uint8_t value = (i8086::AL() & 0x0f) << shift;
          if (i8086::AL() & 0x80)
            fb[addr] ^= value;
          else
            fb[addr]  = (fb[addr] & ~(0x0f << shift)) | value;
          break;
        }

        case GraphicsAdapter::Emulation::PC_Graphics_640x200_2Colors:
          printf("INT 10h, write pixel, unsupported 640x200x2 resolution\n");
          break;


        default:
          printf("INT 10h, write pixel, unsupported resolution\n");
          break;

      }
      break;

    // Read Pixel
    case 0x0d:
    {
      int colour = m_machine->egaReadPixel(i8086::CX(), i8086::DX());
      if (colour >= 0) {
        i8086::setAL((uint8_t) colour);
        break;
      }
      uint32_t addr = tandyPixelAddress(ga->emulation(), i8086::CX(), i8086::DX());
      if (addr != 0xffffffff) {
        i8086::setAL((fb[addr] >> ((i8086::CX() & 1) ? 0 : 4)) & 0x0f);
        break;
      }
      printf("INT 10h, read pixel, unsupported resolution\n");
      break;
    }

    // EGA palette registers. The ROM side only forwards AL = 00..03 here; the DAC calls it
    // handles itself. On a Tandy 1000 the same calls (AL = 00..02) program its video array.
    case 0x10:
      if (m_machine->tandy()) {
        switch (i8086::AL()) {
          case 0x00:                     // BL = palette register, BH = colour (4 bit IRGB)
            if (i8086::BL() < 16)
              m_machine->setTandyPaletteReg(i8086::BL(), i8086::BH());
            break;
          case 0x01:                     // BH = border colour
            m_machine->setTandyBorder(i8086::BH());
            break;
          case 0x02:                     // ES:DX = 16 palette registers, then the border
          {
            uint32_t addr = ((uint32_t)i8086::ES() << 4) + i8086::DX();
            for (int i = 0; i < 16; ++i)
              m_machine->setTandyPaletteReg(i, m_memory[(addr + i) & 0xfffff]);
            m_machine->setTandyBorder(m_memory[(addr + 16) & 0xfffff]);
            break;
          }
        }
        break;
      }
      switch (i8086::AL()) {

        // Set one palette register: BL = register, BH = 6 bit rgbRGB value
        case 0x00:
          if (i8086::BL() < 16)
            m_machine->setEGAPaletteReg(i8086::BL(), i8086::BH());
          break;

        // Set the overscan (border) register. Nothing draws a border, so there is nothing to do.
        case 0x01:
          break;

        // Set all sixteen palette registers and the overscan register from the 17 bytes at ES:DX
        case 0x02:
        {
          uint32_t addr = ((uint32_t)i8086::ES() << 4) + i8086::DX();
          for (int i = 0; i < 16; ++i)
            m_machine->setEGAPaletteReg(i, m_memory[(addr + i) & 0xfffff]);
          break;
        }

        // BL = 0: attribute bit 7 is background intensity, BL = 1: it blinks
        case 0x03:
          ga->setBit7Blink(i8086::BL() & 1);
          break;

      }
      break;

    default:
      printf("unsupported INT 10h, AX = %04X\n", i8086::AX());
      break;

  }
}
