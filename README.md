# ESPPC

An IBM PC/XT on the ESP32: 8086 at roughly turbo XT speed, up to 704 KB of RAM, CGA, EGA or
Tandy 1000 video on a VGA monitor, PC speaker, Tandy sound and an AdLib, floppy and hard disk
images on the SD card or downloaded over Wi-Fi.

ESPPC started as the PCEmulator example of [FabGL](https://github.com/fdivitto/FabGL) by Fabrizio
Di Vittorio and has grown well beyond it with the help of Claude.
FabGL is still underneath it for VGA output, PS/2, sound and the emulated chips,
vendored in `lib/FabGL` with every local change listed in
`lib/FabGL/VENDORED.md`.

## Why?

I had the board lying around and wanted to see how far it could be taken. Also, the example project this is based on
had a bunch of annoying bugs, which are squashed now.

I could have written all of it myself, but PC emulation has been done to death already, there is nothing new to invent here,
and I don't think most people will mind an enhanced port done by an AI under watchful human eyes. And Claude did a very good job here. If I had done all that the old fashioned way, it would have taken me weeks of full time work. Maybe I could have gotten this to a Topbench score of 18 or 19, but sometimes good enough is good enough.

Also, this is an old MCU. The first time I saw this exact chip run emulation was on the venerable ODROID-GO, and there are other projects out there for better MCUs; heck, someone made a 386 fit into an RP2350 with PSRAM. But if you have such an old ESP32 with VGA and an SD card lying around, take this project for a spin. It is still good enough for some old games, or for writing your novel on if you want. If you can get such a board cheaply, grab it, but don't pay more than around 30€ for one, including taxes and shipping.

Pull requests are welcome of course.

## What it emulates

- **CPU:** 8086, Topbench 17 (a turbo XT at 10–12 MHz; up from 11 in the original example).
  Checked against the SingleStepTests 8088 suite, see `tools/cputest/`.
- **Memory:** 64 to 704 KB of conventional memory. 704 KB is the classic non-IBM XT
  configuration: video moves up to B000 and the A000 segment becomes ordinary RAM.
- **Video:** per machine:
  - **CGA**, with the Plantronics ColorPlus modes, and optionally a **composite** monitor that
    shows the NTSC artifact colours (160x200 in 16 colours for games that offer it). The 6845 is
    followed closely enough for the tweaked text modes (80x100, the 160x100x16 of Paku Paku and
    others), hardware scrolling, display pages, the border colour and 59.92 Hz retrace timing.
  - **EGA**, driven by a real IBM EGA BIOS option ROM (`data/fs/ega.bin`, see `data/README.md`):
    the 200 and 350 line modes, split screen, the 6 bit palette.
  - **Tandy 1000**, with its 16 colour modes 8 and 9 and its three voice sound chip.
- **Sound:** PC speaker; the Tandy SN76496 at C0h (also as a TNDY style card on CGA or EGA); the
  Tandy SL/TL **DAC** for Sierra speech and effects; an **AdLib** (OPL2, DOSBox-X's DBOPL),
  synthesised on the second core.
- **Chipset:** 8259 PIC, 8253 PIT, 8237 DMA controller (with the DRAM refresh counting on channel 0),
  8042 keyboard controller, an MC146818 RTC (set from NTP at
  power on), or instead the MM58174 clock SCO Xenix reads, or no clock at all; two 8250 serial
  ports, a PS/2 mouse.
- **Disks:** two floppies and two hard disks as image files. Geometry is guessed from the image
  size or given by hand. Images can be on the SD card, built into the flash file system (read only), or a URL
  that is downloaded to the card on first boot. DOS and games go through the BIOS; for systems
  with their own disk drivers (SCO Xenix 86) a machine can also have the NEC uPD765 floppy
  controller and the IBM XT hard disk controller, with DMA.

## Hardware

Developed on the **Olimex ESP32-SBC-FabGL**: ESP32 WROVER with PSRAM, SD card slot, VGA, PS/2
keyboard and mouse, audio out, and the U1 header for a serial port. Any ESP32 board with PSRAM, an
SD card slot and FabGL's VGA wiring (such as the TTGO VGA32 v1.4) should run it, but only the
Olimex board has been tested.

The board needs at least 4 MB **PSRAM** (the guest's RAM lives there) and **4 MB of flash**.

## Build & flash

**Without building anything:** open [the installer](https://bastetfurry.github.io/ESPPC/) in
Firefox, Chrome or Edge, plug in the board and click Install. It flashes the latest release, firmware and
flash file system.

To build it yourself, with [PlatformIO](https://platformio.org/):

```sh
pio run -t upload       # the firmware (~/.platformio/penv/bin/pio if pio is not on the PATH)
pio run -t uploadfs     # the flash file system: the EGA BIOS and an optional rescue disk
pio device monitor      # serial console, 115200 baud
```

The two uploads are independent: flashing the firmware leaves the file system alone and the other
way round. `data/README.md` explains what goes into the file system and how much fits.

Two settings in `platformio.ini` are important; keep them as they are:

- **PSRAM is not enabled at build time.** `setup()` brings it up itself with
  `esp_spiram_init()`, because `-DBOARD_HAS_PSRAM` would add gcc's
  `-mfix-esp32-psram-cache-issue` workaround, which slows the emulator down far too much.
- **`platform = espressif32@6.7.0`** (arduino-esp32 2.0.17, ESP-IDF 4.4). `esp_spiram_init_cache()`
  is gone in ESP-IDF 5.

The other environments in `platformio.ini` (`psrambench`, `egabench`, `sdbench`, `dltest`,
`uarttest`) are test programs for the board, not the emulator.

## Using it

At power on the board runs a POST screen, sets the clock over Wi-Fi and shows the machine list.
The first time, it asks for a Wi-Fi network and its password; both are kept in the flash.

**Machine list**

| Key | |
|-----|---|
| Enter | boot the selected machine |
| F2 / F3 | edit it / set up a new one |
| Del | remove it from the list (the images stay on the card) |
| F4 | file browser: rename, copy, paste, new folder, delete on the SD card |
| F5 | advanced settings: time server, time zone, auto boot countdown, drive LED |
| F6 | add machines from a repository (below) |

The **machine editor** sets the disk images, boot drive, memory, video adapter, sound cards and
serial ports, with a line of help for each field. **F9** creates an empty hard disk image of a
chosen size, **F10** saves.

**Inside a machine, Alt+PrtScr (SysReq)** opens the system menu: change or eject the floppy in A:
or B:, switch a CGA machine between an RGB and a composite monitor, restart the guest, restart the
board, or go back to the guest.

### SD card

`mconfs.txt` in the root of the card holds the machine list, one machine per line. The editor
writes it, but it is plain text:

```
desc "FreeDOS" hd0 "/SD/dos/freedos.img" boot hd0 ram 640 video ega adlib 1
desc "Rescue" fd0 "/spiffs/rescue.img" boot fd0
```

| Tag | |
|-----|---|
| `desc` | the name in the machine list |
| `fd0` `fd1` `hd0` `hd1` | disk images: a path on the card, `/spiffs/...` for the flash, or an `http(s)://` URL |
| `chs0` `chs1` | hard disk geometry as `cylinders,heads,sectors`, if the guess is wrong |
| `boot` | `fd0` (default), `fd1`, `hd0` or `hd1` |
| `ram` | KB of conventional memory, 64 to 704 in steps of 64, default 640 |
| `video` | `cga` (default), `ega` or `tandy` |
| `egarom` | another EGA BIOS than `/spiffs/ega.bin` |
| `composite` | `1` for a composite colour monitor on CGA |
| `tandysound` | `1` for the SN76496 on CGA or EGA (Tandy video always has it) |
| `tandydac` | `1` for the Tandy SL/TL DAC |
| `adlib` | `1` for an AdLib at 388h |
| `hwdisk` | `1` adds the floppy and XT hard disk controllers for systems with their own disk drivers |
| `clock` | `at` (default, MC146818), `mm58174` (the clock SCO Xenix 86 reads) or `none` |
| `usbserial` | `com1` or `com2`: that port goes over the USB serial |
| `hwserial` | `com1` or `com2`: that port is on the U1 header (pin 5 TXD, 6 RXD, 3 GND, 3.3 V only) |
| `enabledebug` | `1` for the serial debug console |

The comment at the top of `src/mconf.h` has more examples.

You can install a full OS with nothing but the board and your favourite XT compatible OS as a set of disk images.

On Linux, at least, you can mount the hard disk image to fill it with software or to make backups. Don't forget the `-w` option when using the GNOME Image Mounter.

### Machine repositories

A repository is a web server with a `files.list` of ready made machines. **F6** asks for its URL,
shows what it offers, and adds the chosen machines; their images are downloaded on first boot.
`http://lionscade.de/pcimages` has FreeDOS and SvarDOS to start with. How to offer one is in
[docs/repositories.md](docs/repositories.md).

## Serial console and tools

With `enabledebug 1`, the USB serial line becomes a debug console. **Esc Esc Esc** mirrors the
text screen to the terminal and types into the guest; **Ctrl+C Ctrl+D** freezes the guest and
opens a monitor: registers, memory dump and edit, I/O ports, breakpoints, single stepping, port
traces, screenshots, a heap map and a per opcode CPU profile. `?` lists the commands, `g` goes
back to the guest. Without the console, the USB serial line is the guest's COM1.

- `tools/screenshot.py` saves the guest's screen as a PNG over the console.
- `tools/postbox.py` and `tools/postbox.com` move files between the PC and the guest through a
  private channel on the same line, no COM port needed. See the note above `class Machine` in
  `src/machine.h`.
- `tools/cputest/` runs the 8086 core on the PC against the SingleStepTests suite.
- `tests/` holds the emulator's tests: small boot floppies of their own (timer interrupts, CGA
  retrace timing, display pages, the CGA's picture, the keyboard, a reboot from inside an
  interrupt), booted headless by `tests/run.sh` with `pctool`. GitHub runs them on every push,
  with a check that `src/biosrom.h` matches `tools/bios.asm`.
- `host/` builds the whole emulator on a Linux PC (`make -C host`): `host/pctool` boots a machine
  from disk images headless and runs a script, typing on the keyboard, dumping the text screen
  and memory, and writing screenshots from the emulator's own renderers. Time is virtual, so a run
  repeats exactly and takes a fraction of a second. The comment at the top of `host/main.cpp`
  lists the commands.

You can also use that same line as a normal serial line. I tested it with a DOS terminal program, with my Linux box running a login console on ttyUSB0. The only drawback I found was that TERM90 doesn't like Unicode; getting that to work is beyond this document, though.

## The BIOS

The machine's BIOS is 8086tiny's, much extended. `src/biosrom.h` is generated from
`tools/bios.asm` and checked in, so a normal build needs no assembler. After editing the
assembly:

```sh
tools/cbios.sh          # needs nasm and xxd
```

## Layout

```
src/main.cpp            setup, Wi-Fi and clock, the machine list, the SysReq menu
src/machine.*           the PC: CPU, chipset, video, disks, timing
src/bios.*              BIOS services done natively; biosrom.h is built from tools/bios.asm
src/ega.*               the EGA planar memory engine
src/fdc.* hdc.*         the floppy and XT hard disk controllers ("hwdisk")
src/mm58174.*           the MM58174 clock ("clock mm58174")
src/sn76496.* tandydac.* adlib.* opl/   sound: Tandy chip, Tandy DAC, AdLib (DBOPL)
src/textui.* setupui.*  the host's text mode screens: POST, machine editor, file browser
src/mconf.*             mconfs.txt
src/repository.* repolist.* httpopen.*   machine repositories and downloads
src/serialdbg.*         the serial debug console and monitor
lib/FabGL/              FabGL 1.0.9, vendored and changed, see VENDORED.md
data/fs/                the flash file system: EGA BIOS, rescue disk
tools/                  BIOS source, CPU tests, host side tools
host/                   the emulator built on a PC, headless (pctool)
docs/                   design notes and the to do list
```

## Licences

ESPPC is GPL v3 or later, as FabGL is (see `LICENSE`). The AdLib emulation is DOSBox-X's DBOPL (GPL v2 or later). The BIOS
is based on 8086tiny's (MIT). `data/fs/ega.bin` is IBM's EGA BIOS, included as emulators such as
86Box have done for years; its copyright is IBM's.
