# Splitting the emulator into a portable core and a platform layer

A plan (2026-09-27). Step 1 exists in a different form since 2026-10-28, see "The host build
today" below; the rest has not started. The goal is a second hardware target -- an RP2350 board, next to
the ESP32 WROVER-E one -- without forking the emulator. Every step below leaves the ESP32 build
working exactly as before, and each one is checked on the board before the next.

## Principles

- **No behaviour change per step.** A step moves code or puts an interface in front of it; it does
  not fix or improve anything on the side. Fixes get their own commits.
- **No speed loss on the ESP32.** The hot paths -- the CPU core, the video memory callbacks, the
  scanline renderers, the device ticks -- must not go through virtual calls. The platform is chosen
  at compile time: an interface is a header with the declarations, and each platform provides the
  definitions (or inline implementations). `IRAM_ATTR` becomes a platform macro (`PLATFORM_HOT`, or
  similar) that means IRAM on the ESP32 and a RAM section on the RP2350.
- **Measured after every step:** tools/cputest (no test worse), a boot of each machine type,
  CheckIt or Topbench against the numbers in the emulator-speed notes, and PX3's fade-in.
- **A third platform early: the host (Linux).** A headless host build -- BIOS, devices, a disk
  image, a text screen dump -- gives the interfaces a second implementation long before RP2350
  hardware exists, and makes automated boot tests possible (boot DOS from an image, compare the
  text screen). It exists now, as `host/` (below).

## The host build today

`host/` (`make -C host`, 2026-10-28) builds the emulator's own sources on Linux into `pctool`, which
boots a machine from disk images and runs a script at emulated times: typing through the 8042,
key presses, text screen and memory dumps, register dumps, and screenshots drawn by the real
scanline renderers. Time is virtual, advanced by the instructions run, so runs are repeatable and
fast. See the comment at the top of `host/main.cpp`.

It takes a shortcut past the plan: instead of interfaces, `host/stub/` stands in for the headers
the code includes (`fabgl.h`, `esp_timer.h`, `esp_heap_caps.h`, `soc/soc.h`, FreeRTOS, NVS,
MCP23S17), and `host/host.cpp` implements them: the virtual clock and esp_timer, the one task, a
PS/2 keyboard queue, the PSRAM window, FabGL's fonts. The emulator sources are compiled unchanged,
so it tests exactly what runs on the board. Not there: sound (the AdLib task is refused), the
mouse, serial ports, the serial debug console, the setup screens.

So step 1 and the headless half of step 12 are usable already; the interfaces still have to be
drawn out of the stubs when the split proper starts, and the stubs then become the host's
implementation of them. Two things the host build found that matter for the split: the 8086 core
keeps its registers' address as a 32 bit offset from guest memory, so guest RAM has to lie within
2 GB of the core's static data (host.cpp puts it in the binary's data); and the emulator relied
on real time jitter, which the virtual clock does not have, to deliver interrupts (fixed in
05947ad).

## Where things stand today

Portable already, or nearly: the CPU core (i8086.cpp -- it runs on the host), the PIC, the EGA
planar engine (ega.cpp), the BIOS (bios.cpp and bios.asm), the Tandy, Plantronics and composite logic
in the machine, DBOPL, the SN76496 and the Tandy DAC state machines, the machine configuration
parser (mconf), the port and interrupt trace.

Tied to the platform:

| area | where | what ties it |
|---|---|---|
| time | PIT8253, MC146818, PC8250, graphicsadapter, machine | `esp_timer_get_time`, `FRC1Timer`, `localtime` |
| hot code placement | everywhere in the hot paths | `IRAM_ATTR` |
| memory | machine, graphicsadapter, textui, adlib | `heap_caps_malloc(INTERNAL)`, the PSRAM halves per core, `SOC_EXTRAM_DATA_LOW` |
| video output | graphicsadapter (FabGL's VGADirectController) | RGB222 plus sync in one byte, the x ^ 2 byte swizzle, FabGL modelines |
| input | i8042 | FabGL's PS2Controller, Keyboard and Mouse (ULP coprocessor) |
| sound | machine, sn76496, tandydac, adlib | FabGL's SoundGenerator and WaveformGenerator (DAC on GPIO25) |
| serial ports | PC8250, machine, serialdbg | FabGL's SerialPort, the ESP32 UARTs, Arduino `Serial` |
| tasks and cores | machine (runTask), adlib, serialdbg | FreeRTOS, `CoreUsage`, core pinning |
| storage | main, mconf, machine | SPIFFS, SD via FabGL's FileBrowser, `fopen` on VFS paths |
| settings | main, setupui | Arduino `Preferences` (NVS) |
| network | main | Wi-Fi, SNTP, HTTP downloads |
| host screens | textui, setupui, main | drawn with the machine's GraphicsAdapter in text mode |

## The interfaces

Described by what they have to offer, not by code.

1. **Time.** A monotonic microsecond clock (PIT, 8250 timing, the EGA retrace model, the drive LED,
   the AdLib timers), the wall clock for the RTC and POST, and an optional cycle counter for the
   monitor's profiling commands.
2. **Memory.** The guest RAM region and how it is reached (on the ESP32 one PSRAM half; on the
   RP2350 an SRAM/PSRAM split is likely, see the open questions), "fast" allocations for what the
   video interrupt reads (line tables, video buffers, the EGA planes), and memory the second core
   may own (today the AdLib chip in the video core's PSRAM half).
3. **Display.** The biggest one, in two halves:
   - *renderers* (portable): today's scanline renderers per mode, producing a line of pixels from
     the guest's video memory and palette. They are table driven already; the tables are built
     from colours through a platform supplied *pixel encoder* (RGB -> the native pixel, 1 or 2
     bytes) and write through a platform supplied *store* (the ESP32's x ^ 2 swizzle lives there).
   - *display backend* (platform): timings, line buffers, how lines reach the pins -- FabGL's
     VGADirectController on the ESP32; PIO (VGA, RGB565) or HSTX (DVI) on the RP2350 -- and the
     overlay hook the drive LED uses.
   The mode table (resolution, timing, lines per call) moves from GraphicsAdapter into the portable
   part, with the backend mapping each entry to a timing it can produce.
4. **Input.** Two PS/2 style byte channels (keyboard, mouse) as the i8042 sees them: bytes in,
   bytes out, "device present". The ESP32 backend wraps FabGL's PS2Controller; the RP2350 one
   would use PIO, or translate USB HID into PS/2 codes. The host screens read the same channel
   through a small key decoder.
5. **Audio.** A set of voices pulled at the output rate (PC speaker, SN76496, Tandy DAC, AdLib
   ring, Covox should it ever return) and an output backend that mixes and plays them -- FabGL's
   SoundGenerator on the ESP32, PWM or I2S via PIO on the RP2350. Plus the "producer on the other
   core at low priority" facility the AdLib synthesiser needs.
6. **Serial.** Byte streams for COM1/COM2 (real UART pins, USB CDC, the debug console bridge) and
   one for the debug console itself.
7. **Storage and settings.** Files for disk images and configuration (plain stdio on top of each
   platform's file system: ESP32 VFS, FatFs on the RP2350), and a small key/value store for host
   settings (NVS today, a flash sector on the RP2350).
8. **Tasks and cores.** "Run the emulator loop on core A", "run the display on core B", "run this
   producer on core B at low priority", "reboot the host".
9. **Network (optional).** NTP and HTTP downloads as a capability the platform may lack; the
   RP2350 without a CYW43 simply reports it absent.

## Steps

Ordered so that each is small, testable on the board, and makes the next one easier.

1. **Host platform, headless, CPU and BIOS only.** Grow tools/cputest's build into a host target that
   boots the BIOS with a disk image and dumps the text screen. Everything it needs becomes the first
   draft of the interfaces (time, memory, storage). *About a day.* Done as `host/`, with stub headers
   instead of interfaces (see above).
2. **Move the modified FabGL devices out of lib/FabGL.** i8086, PIC, PIT, i8042, 8250, RTC and
   GraphicsAdapter are heavily ESPPC specific by now; they move to a `core/` (and, for
   GraphicsAdapter, `display/`) directory unchanged, so later steps edit project code, not a vendored
   library. *An hour or two, mechanical.*
3. **Time and hot placement.** One time header, `PLATFORM_HOT` instead of `IRAM_ATTR`; PIT, RTC,
   8250 and the machine use them. *Half a day.*
4. **Memory.** The allocation calls and the guest RAM layout behind one header; the PSRAM half
   logic becomes the ESP32 implementation. *Half a day.*
5. **Storage and settings.** Preferences and the SPIFFS/SD mounting behind the storage interface.
   *Half a day.*
6. **Audio.** Our own voice interface in place of FabGL's WaveformGenerator; an ESP32 backend that
   bridges to SoundGenerator. The AdLib producer task through the tasks interface. *A day.*
7. **Input.** The i8042 talks to the input channels; the ESP32 backend wraps PS2Controller. The host
   screens read keys through the same channel. *A day.*
8. **Serial.** PC8250 and the debug console on byte stream interfaces. *Half a day.*
9. **Display.** Renderers, pixel encoder, store and mode table into the portable part;
   VGADirectController behind the backend interface. The biggest and riskiest step, done mode family
   by mode family (CGA text, CGA graphics, EGA, Tandy, Plantronics, composite), each checked on
   screen. *Two to four days.*
10. **Tasks and main loop.** runTask, init order and the core pinning into the platform; the machine
    exposes "run a batch" and "tick". *Half a day.*
11. **Host screens.** TextUI and the setup screens on top of the display (text mode) and input
    interfaces instead of GraphicsAdapter and PS2Controller directly. *A day.*
12. **Host platform complete.** With a trivial display (the text screen as text, graphics as PNG)
    and file backed storage, the host target boots DOS from an image headlessly: automated
    regression tests for everything above the hardware. *A day.*

After that the RP2350 port is "write the platform layer": display backend (PIO VGA or HSTX DVI),
input (PIO PS/2 or USB HID), audio (PWM or I2S), storage (SD over PIO/SPI with FatFs), settings
(flash), tasks (bare metal per core or FreeRTOS SMP), time (the hardware timer).

## More targets (added 2026-10-04)

The same split serves two more platforms, which is a good test of whether the line is clean:

- **ESP32-S3.** FabGL does not run on it (no I2S parallel mode as the ESP32 has it, a different
  ULP, different peripherals), so here the split is not optional: everything FabGL does today has
  to come from our own backends. VGA through the S3's LCD_CAM peripheral (as the S3 VGA libraries
  do it), keyboard by USB OTG HID or PS/2 through GPIO, Wi-Fi as on the ESP32. More internal RAM
  and octal PSRAM than the WROVER, so the memory and speed picture changes too.
- **SDL on the host.** Step 12's host platform with interactive backends: the display in a window,
  keyboard from SDL events, sound through SDL audio, disks and settings as plain files. For trying
  changes without flashing, for debugging with a real debugger, and for the boot tests. The
  headless host stays for automated runs.
- **No Wi-Fi.** An RP2350 board without a radio module (or with one added separately) reports the
  network capability absent: no NTP (the clock from the RTC or a fixed date), no repositories or
  URL disks, and the setup screens hide what needs it. The repository list and the downloads
  already live in their own files (`src/repository.cpp`, `src/httpopen.cpp`, `downloadURL()`),
  so they move behind the network interface as one piece.

## Open questions, for later

- **RP2350 video:** VGA through a resistor DAC (PIO, e.g. RGB565 -- MCGA and beyond become possible)
  or DVI/HDMI through HSTX (digital, fits modern party monitors), or both.
- **RP2350 guest RAM:** 520 KB of SRAM cannot hold 640 KB of guest RAM plus video memory plus the
  emulator. A split is needed -- low memory, stack and video buffers in SRAM, the rest in QSPI PSRAM
  behind the 16 KB cache -- and it decides a good part of the speed.
- **FabGL on the ESP32:** keep it as the ESP32 backend (the least work), or take over the few parts
  still used (VGADirectController, PS2Controller, SoundGenerator) and drop the library; it is
  unmaintained. GPLv3 either way.
- **The ESP32's colour depth:** FabGL packs 2 bits per colour and both syncs into one byte, hence 64
  colours. The ESP32's I2S parallel mode can also clock out 16 bits (bitluni's ESP32Lib does 14 bit
  colour that way), so a custom WROVER-E board with more DAC resistors could show far more colours.
  With the display split done, that is one more backend -- at the price of twice the line buffer
  traffic on the video core.
