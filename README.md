# PCEmulator (PlatformIO)

IBM PC/XT emulator (i8086, CGA/Hercules, 8259 PIC, 8253 PIT, 8042 keyboard controller,
MC146818 RTC, 8250 serial) running on an ESP32, built on
[FabGL](https://github.com/fdivitto/FabGL) by Fabrizio Di Vittorio.

This is the FabGL `examples/VGA/PCEmulator` sketch converted from the Arduino IDE to
PlatformIO / VSCode. GPL v3, see the headers in `src/`.

## Hardware

- ESP32 **with PSRAM** soldered and an SD-CARD slot — e.g. TTGO VGA32 v1.4 or the FabGL
  Development Board with a WROVER module
- VGA monitor and a PS/2 keyboard (mouse optional)
- 4MB flash

## Build & flash

```sh
pio run              # build
pio run -t upload    # build + flash
pio device monitor   # serial console, 115200 baud
```

If `pio` is not on your PATH, the VSCode PlatformIO extension ships it at
`~/.platformio/penv/bin/pio`.

The first build downloads the espressif32 6.7.0 toolchain and FabGL 1.0.9.

## Important: do not enable PSRAM in the build

`setup()` initializes SPIRAM at runtime via `esp_spiram_init()` / `esp_spiram_init_cache()`.
This is deliberate: enabling PSRAM in the build (`-DBOARD_HAS_PSRAM`) pulls in gcc's
`-mfix-esp32-psram-cache-issue` workaround, which slows the CPU down too much for the emulator.
The emulator only uses ~2MB of PSRAM, so the PSRAM cache bug does not apply.

The `platform = espressif32@6.7.0` pin is also load-bearing: `esp_spiram.h` and
`esp_spiram_init_cache()` were removed in ESP-IDF 5.x (arduino-esp32 3.x).

## SD card

Machine configurations live in `mconfs.txt` in the SD card root; it is created with a set of
defaults on first run. Each line describes one machine (description, disk images, geometry, boot
drive) — see the comment block at the top of `src/mconf.h` for the format. Disk images may be
local paths relative to `/SD` or `http(s)://` URLs, which are downloaded over WiFi on first use.

## Usage

- On boot, pick a machine configuration and press **Run**.
- **ALT + PrintScreen** (SYSREQ) inside the emulator opens a menu to restart the emulator,
  reset the machine, or mount a floppy image.

## Rebuilding the BIOS

`src/biosrom.h` is generated from `tools/bios.asm` and is checked in, so a normal build does not
need it. After editing the assembly:

```sh
tools/cbios.sh       # requires nasm and xxd
```

## Layout

```
platformio.ini    build configuration (board, partitions)
partitions.csv    3MB app partition ("Huge APP" equivalent)
src/main.cpp      entry point (was PCEmulator.ino)
src/machine.*     machine: CPU, chipset, video, disks
src/bios.*        BIOS services; biosrom.h is generated from tools/bios.asm
src/mconf.*       machine configuration file parsing and the setup dialogs
tools/            bios.asm and its build script
lib/FabGL/        vendored FabGL 1.0.9 (see lib/FabGL/VENDORED.md)
```

## FabGL is vendored, not fetched

`lib/FabGL/` holds FabGL 1.0.9's `src/` tree verbatim, and there is no `lib_deps` entry.
PlatformIO compiles anything under `lib/` automatically.

This is deliberate. The project modifies emulator internals, and a fetched dependency lives in
`.pio/libdeps/`, which is gitignored and destroyed by `pio run -t fullclean` — edits made there
would vanish silently. Vendoring also means the build needs no network.

Record any change to that tree in `lib/FabGL/VENDORED.md`, which currently documents it as
byte-identical to upstream, so a future version bump knows what to preserve.

## Local changes vs the upstream FabGL example

This project started as a byte-identical copy of `FabGL/examples/VGA/PCEmulator` and has since
diverged. **`src/biosrom.h` is generated from the modified `tools/bios.asm`** — regenerating either
from a stock FabGL checkout silently reverts the boot-failure check below.

Unchanged from upstream: `src/machine.cpp`, `src/machine.h`, `src/bios.cpp`, `src/bios.h`,
`src/mconf.h`, `partitions.csv`.

### `src/main.cpp` (was `PCEmulator.ino`)

- Converted for PlatformIO: added `#include <Arduino.h>`, added the forward declarations the Arduino
  preprocessor used to generate, and replaced the "Huge APP" `#pragma message`.
- **SysReq → Mount Disk keeps the folder.** `fileSelector()` hands back the directory and the bare
  filename separately, while `setDriveImage()` resolves against `SD_MOUNT_PATH`. The original passed
  only the filename, so any image inside a folder was looked up in the mount root and failed to
  open. Both directions are now handled, using the same "bypass `/SD/`" rule as `editConfigDialog()`
  in `src/mconf.cpp`, so the selector also reopens in the folder the mounted disk lives in.
- **SysReq → Eject Floppy** added. Lists both drives with their current image, then calls
  `setDriveImage(drive, nullptr)` so the guest sees a media change. Floppies only.

### `src/mconf.cpp`

- **Hard disk image creation is no longer pinned to the video refresh.** The progress form was
  updated once per 4 KB chunk; because its label is auto-sized and drawing queues one primitive per
  character into a queue drained once per vertical sync, an 80 MB image spent minutes stalled on
  vblank rather than on the card. It now refreshes only when the percentage changes, and writes in
  32 KB chunks. Also guards a failed `openFile()` (previously `fwrite` to a null handle) and a short
  write (previously an infinite loop when the card filled up).
- **The config dialog warns about an unbootable boot drive.** `bootDrive` defaults to 0 (`fd0`), so
  filling in only a hard disk and pressing Save produced a machine that could not boot, with no
  indication. `saveAndQuit()` now says so, naming the drive. A warning rather than a refusal,
  because booting a floppy mounted later via SYSREQ is legitimate.

### Selectable conventional memory (64 KB – 704 KB)

The machine editor has a **Memory** dropdown, and the config file a `ram <KB>` tag, offering
64 KB to 704 KB in 64 KB steps. **The default is 640 KB**, which behaves exactly as stock FabGL.

704 KB is the classic non-IBM XT configuration. The video window only needs 0xB0000-0xBFFFF, so
at sizes above 640 KB the CPU's video trap moves from 0xA0000 to 0xB0000 and 0xA0000-0xAFFFF
becomes ordinary RAM — direct array access, no callbacks, no cost. At 640 KB and below the window
stays at 0xA0000 and that range reads 0xFF as IBM hardware does, because some software probes
0xA0000 to detect a graphics adapter.

Moving parts, which must stay in sync:

- `i8086::setVideoMemStart()` in the vendored FabGL (see `lib/FabGL/VENDORED.md`)
- `Machine::setRamSize()`, which applies it in `Machine::init()`
- BIOS helper `AH=0x09`, which POST calls to fill in `[cs:memsize]` and `[cs:ebda_seg]` before
  the BIOS data area is copied to 40:0 — so both `INT 12h` and 40:13h report the right size
- the EBDA, which always sits in the top 1 KB of conventional memory, hence `EBDA_SEG_FOR_KB()`

Known limitation: selecting *less* than 640 KB changes only what the BIOS reports. The emulator's
RAM array is always a full 1 MB, so memory above the configured size is still physically readable
rather than reading as absent. Software that trusts `INT 12h` behaves correctly; software that
probes memory directly will still find it.

### `tools/bios.asm` → `src/biosrom.h`

- **The boot drive comes from the emulator (helper `AH=0x0A`), not from DL.** `bios_entry` used to
  take it from DL, which only holds it on a cold boot: a warm boot reached by jumping to the reset
  vector (Ctrl+Alt+Del) arrives with whatever DL the guest left behind. The read then failed, and
  the old code jumped to 0:7C00 anyway and re-ran the boot sector still in RAM from the previous
  boot — so it appeared to work. Adding the boot-failure check above turned that latent bug into a
  visible `NO BOOT DEVICE`.

- **POST enables the 8042 keyboard controller (`0xAE`).** Software brackets its keyboard handling
  with 8042 commands `0xAD` (disable) and `0xAE` (enable), and a guest that reboots by jumping to
  the reset vector — which is what Ctrl+Alt+Del does in DOS — can get there between the two.
  Nothing resets the controller on that path (no `0xFE` is written, so `Machine::reset()` never
  runs), so the keyboard stayed disabled forever and the machine came back up with a dead
  keyboard. Confirmed by instrumenting the 8042 command stream.

- **`NO BOOT DEVICE` is reported instead of jumping into nothing.** The boot code issued its
  `int 13h` and then ran `jmp 0:0x7c00` unconditionally, so any failed read jumped into whatever
  happened to be at 0x7C00 — a blinking cursor with no message and no disk activity. It now checks
  CF and the 0xAA55 signature, prints a message via INT 10h teletype naming the likely cause, and
  idles with interrupts enabled so SYSREQ still works and a disk can be mounted.

Regenerate the ROM after editing the assembly:

```sh
tools/cbios.sh       # requires nasm and xxd
```

`nasm` 3.02 reproduces the unmodified ROM byte-for-byte, so a rebuild introduces only your own
changes. Nothing in the ROM is position-locked: the `absolute` blocks are `resb` RAM labels that
emit no bytes, and `m_origInt1EAddr` is discovered at runtime from ES:BX (`src/bios.cpp`), not
hardcoded.
