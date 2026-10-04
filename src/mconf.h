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



/*

** Disks configuration text format **

Each line contains a machine configuration, which includes description and disks images location. Location can be a URL or a local path.

Allowed tags:

"desc" : Textual description of the configuration
"fd0"  : Floppy drive 0 (A) filename or URL
"fd1"  : Floppy drive 1 (B) filename or URL
"hd0"  : Hard disk drive 0 filename or URL
"hd1"  : Hard disk drive 1 filename or URL
"chs0" : Hard disk 0 geometry (Cylinders,Heads,Sectors)
"chs1" : Hard disk 1 geometry (Cylinders,Heads,Sectors)
"boot" : Boot drive. Values: fd0, fd1, hd0, hd1. Default is fd0
"ram"  : Conventional memory in KB, 64 to 704 in steps of 64. Default is 640.
         704 is the classic non-IBM XT configuration: video starts at 0xB0000, so 0xA0000-0xAFFFF
         is ordinary RAM. Leave at 640 unless a program needs the extra memory, since some software
         probes 0xA0000 to detect a graphics adapter.

A filename may be an absolute path, which is used exactly as given: "/SD/disks/disk1.img" for the
SD card, or "/spiffs/rescue.img" for an image built into the flash (see data/README.md). The
machine editor always writes absolute paths, because an image is not necessarily on the SD card.

A filename without a leading slash is relative to SD_MOUNT_PATH, which is how older configuration
files are written and still works. For example, if "disks/disk1.img" is specified and
SD_MOUNT_PATH="/SD", then it loads "/SD/disks/disk1.img".

Images under the SPIFFS mount are opened read-only, see Machine::setDriveImage().


Examples:

Download first floppy image from "http://www.fabglib.org/downloads/A_freedos.img" and first hard disk image from "http://www.fabglib.org/downloads/C_dosdev.img". Boot from floppy:
    desc "FreeDOS (A:) + DOS Programming Tools (C:)"  fd0 "http://www.fabglib.org/downloads/A_freedos.img"  hd0 "http://www.fabglib.org/downloads/C_dosdev.img"

First hard disk is "HDD_10M.IMG", having 306 cylinders, 4 heads and 17 sectors. Boot from hard disk:
    desc "My Own MSDOS" hd0 HDD_10M.IMG chs0 306,4,17 boot hd0

First floppy drive is "TESTBOOT.IMG". Boot from first floppy:
    desc "Floppy Only"  fd0 TESTBOOT.IMG boot fd0

Boot from disk1.img located inside "msdos3" folder:
    desc "Machine1" fd0 msdos3/disk1.img

*/


#pragma once


#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "bios.h"
#include "textui.h"


#define SD_MOUNT_PATH         "/SD"

#define MACHINE_CONF_FILENAME "mconfs.txt"

#define NL                    "\r\n"




////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MachineConf


struct MachineConfItem {
  MachineConfItem * next;
  char            * desc;
  char            * disk[DISKCOUNT];        // path relative to mounting path
  uint16_t          cylinders[DISKCOUNT];
  uint16_t          heads[DISKCOUNT];
  uint16_t          sectors[DISKCOUNT];
  uint8_t           bootDrive;
  uint16_t          ramSizeKB;                // conventional memory in KB (64..704, step 64)
  bool              debugConsole;             // "enabledebug": serial debug console instead of COM1
  uint8_t           usbSerial;                // "usbserial": 0 = off, 1 = COM1, 2 = COM2
  bool              usbSerialSet;             // whether the file said so, see applyUsbSerial()
  uint8_t           hwSerial;                 // "hwserial": port on the U1 header, 0 = off, 1 = COM1, 2 = COM2
  uint8_t           videoAdapter;             // "video": 0 = CGA (default), 1 = EGA, 2 = Tandy 1000
  char            * egaRom;                   // "egarom": path to the EGA BIOS option ROM
  bool              tandySound;               // "tandysound": SN76496 at C0h without the Tandy video
  bool              tandyDAC;                 // "tandydac": the SL/TL DAC, needs the sound chip
  bool              composite;                // "composite": a composite colour monitor on the CGA
  bool              adlib;                    // "adlib": an AdLib (OPL2) at 388h

  MachineConfItem()
    : next(nullptr),
      desc(nullptr),
      disk(),
      cylinders(),
      heads(),
      sectors(),
      bootDrive(0),
      ramSizeKB(640),
      debugConsole(false),
      usbSerial(0),
      usbSerialSet(false),
      hwSerial(0),
      videoAdapter(0),
      egaRom(nullptr),
      tandySound(false),
      tandyDAC(false),
      composite(false),
      adlib(false)
  {
  }

  ~MachineConfItem()
  {
    free(egaRom);
    free(desc);
    for (int i = 0; i < DISKCOUNT; ++i)
      free(disk[i]);
  }

  bool isValid()                                { return desc != nullptr; }

  void setDesc(char const * value)              { free(desc); desc = strdup(value); }
  void setDisk(int index, char const * value)   { free(disk[index]); disk[index] = strdup(value); }
  void setCHS(int index, char const * value)    { sscanf(value, "%hu,%hu,%hu", &cylinders[index], &heads[index], &sectors[index]); }

  // "video ega" selects the EGA path, which needs a real EGA BIOS option ROM, and "video tandy" the
  // Tandy 1000 graphics and sound. Anything else, including the tag being absent, leaves the machine
  // on the original CGA adapter.
  void setVideoAdapter(char const * value)      {
    char c = value ? (value[0] | 0x20) : 0;
    videoAdapter = c == 'e' ? 1 : (c == 't' ? 2 : 0);
  }

  void setEgaRom(char const * value)            {
    if (egaRom)
      free(egaRom);
    egaRom = value && value[0] ? strdup(value) : nullptr;
  }

  // "tandysound 1" adds the Tandy 1000's SN76496 sound chip at port C0h to any machine, as the TNDY
  // style sound cards do. The Tandy 1000 video option has it anyway.
  void setTandySound(char const * value)        {
    tandySound = value && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                           value[0] == 't' || value[0] == 'T');
  }

  // "tandydac 1" adds the Tandy 1000 SL/TL's DAC, played through the BIOS (INT 1Ah AH=81h..85h).
  // It sits next to the sound chip, so it only takes effect with Tandy video or "tandysound 1".
  void setTandyDAC(char const * value)          {
    tandyDAC = value && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                         value[0] == 't' || value[0] == 'T');
  }

  // "composite 1": the CGA machine drives a composite colour monitor, so graphics modes with the
  // colour burst on show NTSC artifact colours
  void setComposite(char const * value)         {
    composite = value && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                          value[0] == 't' || value[0] == 'T');
  }

  // "adlib 1": an AdLib card, the OPL2 FM synthesiser at ports 388h/389h
  void setAdLib(char const * value)             {
    adlib = value && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                      value[0] == 't' || value[0] == 'T');
  }

  // "enabledebug 1" turns the serial debug console on. It is off unless asked for, because it
  // takes over the USB serial pins that COM1 would otherwise use (see setup() in main.cpp).
  void setDebugConsole(char const * value)      {
    debugConsole = value && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                             value[0] == 't' || value[0] == 'T');
  }

  // "usbserial com1" or "com2" puts that emulated port on the USB serial line; "off" leaves it
  // alone. How it gets there depends on the debug console: with the console off the port takes the
  // UART pins directly, and with it on the console carries the port in software instead, so both
  // work at once. See SerialDebug::setUsbSerialRoute().
  void setUsbSerial(char const * value)         {
    usbSerialSet = true;
    if      (value && (value[0] == '1' || !strncasecmp(value, "com1", 4))) usbSerial = 1;
    else if (value && (value[0] == '2' || !strncasecmp(value, "com2", 4))) usbSerial = 2;
    else                                                                  usbSerial = 0;
  }

  // What the machine should actually do when nothing was written down. A machine without the debug
  // console has always had COM1 on the USB serial, so that stays the default; one with the console
  // has always had no COM port at all, and silently bridging one would put guest output into the
  // middle of the console stream. The default never takes the port the U1 header already has.
  uint8_t effectiveUsbSerial() const            {
    if (usbSerialSet)
      return usbSerial;
    return (debugConsole || hwSerial == 1) ? 0 : 1;
  }

  // "hwserial com1" or "com2" puts that emulated port on the U1 header (TXD GPIO2, RXD GPI34,
  // 3.3V levels) through UART1; "off" or no tag leaves the header unused.
  void setHwSerial(char const * value)          {
    if      (value && (value[0] == '1' || !strncasecmp(value, "com1", 4))) hwSerial = 1;
    else if (value && (value[0] == '2' || !strncasecmp(value, "com2", 4))) hwSerial = 2;
    else                                                                  hwSerial = 0;
  }

  // Both the USB serial and the U1 header asked for the same COM port. Only one can have it: the
  // header wins, see setup() in main.cpp.
  bool serialConflict() const                   {
    return hwSerial != 0 && hwSerial == effectiveUsbSerial();
  }

  void setRamSize(char const * value)           {
    int kb = atoi(value);
    // accept only the sizes the dialog offers, fall back to the IBM default
    ramSizeKB = (kb >= 64 && kb <= 704 && (kb % 64) == 0) ? kb : 640;
  }

  void setBootDrive(char const * value)         {
    for (int i = 0; i < DISKCOUNT; ++i)
      if (strcmp(value, driveIndexToStr(i)) == 0) {
        bootDrive = i;
        return;
      }
  }

  static char const * driveIndexToStr(int index) {
    static char const * DRVNAME[DISKCOUNT] = { "fd0", "fd1", "hd0", "hd1" };
    return DRVNAME[index];
  }
};


class MachineConf {
public:
  MachineConf();
  ~MachineConf();

  void loadFromFile(FILE * file);
  void saveToFile(FILE * file);

  bool deleteItem(int index);

  void addItem(MachineConfItem * item);
  void insertItem(int insertingPoint, MachineConfItem * item);

  MachineConfItem * getFirstItem()     { return m_itemsList; }
  MachineConfItem * getItem(int index);

private:

  void cleanUp();

  MachineConfItem * m_itemsList;

};



////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////
// helpers


void loadMachineConfiguration(MachineConf * mconf);

void saveMachineConfiguration(MachineConf * mconf);

// diskType: 0 = 320K, 1 = 360K, 2 = 720K, 3 = 1.2M, 4 = 1.44M, 5 = 2.88M
bool createFATFloppyImage(TextUI * ui, int diskType, char const * directory, char const * filename);
