# ESPPC Code Review

**Target Hardware:** Espressif ESP32-D0WDQ6 (Revision 1 / WROVER with PSRAM)  
**Foundation:** FabGL 1.0.9 (PCEmulator example)  
**Review Subject:** Architecture, Performance Optimizations, Subsystems, Code Quality, and Portability Roadmap  
**Date:** September 2026  

---

## 1. Executive Summary

This repository represents a tour de force in embedded systems engineering and retrocomputing emulation. Taking FabGL's original proof-of-concept IBM PC/XT emulator and pushing it to run real-world PC games and software with **EGA**, **Tandy 1000 graphics and sound**, **Plantronics ColorPlus**, **CGA Composite Artifacting**, and **AdLib (OPL2 FM synthesis)** on the original ESP32 silicon is an extraordinary achievement.

The emulator achieves a **Topbench score of 17** (equivalent to an 8–10 MHz 80286 or a Turbo XT at 10–12 MHz), up from an initial score of 11. What makes this impressive is not merely raw clock speed, but how the emulator systematically maneuvers around severe hardware limitations:
- Original ESP32 PSRAM cache bugs (handled without the disabling `-mfix-esp32-psram-cache-issue` compiler flag)
- Tightly squeezed internal SRAM (where an EGA planar allocation must fit into fragmented blocks)
- Exhausted IRAM (<2 KB remaining), managed by selectively offloading cold opcodes to flash
- Multi-core load balancing, running OPL2 synthesis on the video core while keeping guest memory isolated on the emulator core

This review examines the design decisions, validates the optimizations, identifies several subtle bugs and edge cases, and provides recommendations for ongoing development and the planned platform split.

---

## 2. Architecture & Resource Engineering on the ESP32

```
                   ESP32 Dual Tensilica Xtensa LX6 @ 240 MHz
 ┌─────────────────────────────────────────┐  ┌─────────────────────────────────────────┐
 │               Core 0 (App)              │  │              Core 1 (Video)             │
 │                                         │  │                                         │
 │  ┌───────────────────────────────────┐  │  │  ┌───────────────────────────────────┐  │
 │  │        i8086 CPU Emulation        │  │  │  │   VGADirectController (ISR/DMA)   │  │
 │  │      (Batch of 16 instructions)   │  │  │  │   Scanline rendering (IRAM)       │  │
 │  └─────────────────┬─────────────────┘  │  │  └─────────────────┬─────────────────┘  │
 │                    │                    │  │                    │                    │
 │  ┌─────────────────▼─────────────────┐  │  │  ┌─────────────────▼─────────────────┐  │
 │  │  Peripherals & Interleaved Planar │  │  │  │       DBOPL Synthesizer Task      │  │
 │  │  EGA Engine (8x16KB Banked SRAM)  │  │  │  │       (Lowest priority, 16kHz)    │  │
 │  └─────────────────┬─────────────────┘  │  │  └─────────────────┬─────────────────┘  │
 └────────────────────┼────────────────────┘  └────────────────────┼────────────────────┘
                      │                                            │
         ┌────────────▼─────────────┐                 ┌────────────▼─────────────┐
         │ PSRAM Lower 2MB Window   │                 │ PSRAM Upper 2MB Window   │
         │ - 1MB Guest RAM          │                 │ - DBOPL Chip State       │
         │   (0x00000 - 0xFFFFF)    │                 │   (Placement new, 4.4KB) │
         └──────────────────────────┘                 └──────────────────────────┘
```

### 2.1 Dual-Core Work Distribution & PSRAM Errata Mitigation

The original ESP32 silicon has a notorious hardware errata: simultaneous access to external PSRAM across cores or between CPU instructions and data cache can lock the bus or corrupt cache lines. The standard Espressif workaround (`-mfix-esp32-psram-cache-issue`) inserts instruction paddings and fences that cripple CPU emulation performance by 30–50%.

The codebase implements an elegant architectural solution:
1. **Manual SPIRAM Init:** In [`src/main.cpp`](../src/main.cpp#L775-L784), PSRAM is initialized manually at runtime via `esp_spiram_init()` and `esp_spiram_init_cache()`, explicitly avoiding `-DBOARD_HAS_PSRAM` in [`platformio.ini`](../platformio.ini#L40-L43).
2. **Core Spatial Partitioning:** In [`Machine::init()`](../src/machine.cpp#L164-L167):
   ```cpp
   s_memory = (uint8_t*)(SOC_EXTRAM_DATA_LOW + (xPortGetCoreID() == 1 ? 2 * 1024 * 1024 : 0));
   ```
   Core 0 exclusively touches the lower 2 MB half for guest RAM, while Core 1 exclusively touches the upper 2 MB half (used by the AdLib DBOPL state in [`src/adlib.cpp`](../src/adlib.cpp#L146)). Cross-core PSRAM thrashing is eliminated by design.

### 2.2 IRAM Budgeting & Cold Code Offloading

Instruction RAM (`.iram0.text`) on the ESP32 is capped at 128 KB. Because guest memory accesses in PSRAM compete with flash code fetches across the shared internal bus cache, running hot emulator paths from flash incurs catastrophic cache refill penalties.

Key optimizations implemented:
- In [`lib/FabGL/src/emudevs/i8086.cpp`](../lib/FabGL/src/emudevs/i8086.cpp#L713), [`i8086::step()`](../lib/FabGL/src/emudevs/i8086.cpp#L713) and [`i8086::stepEx()`](../lib/FabGL/src/emudevs/i8086.cpp#L1204) are decorated with `IRAM_ATTR`.
- To prevent `stepEx()` (21 KB) from overflowing IRAM, commit `e71839f` created [`i8086::stepExCold()`](../lib/FabGL/src/emudevs/i8086.cpp#L1935): rare opcodes (DAA, DAS, AAA, AAS, INT 3, INTO, AAM, AAD, SALC, HLT, ESC/x87, LOCK) are offloaded to flash with `__attribute__((noinline))`.
- As verified via `xtensa-esp32-elf-size`, `.iram0.text` sits at **129,355 bytes**, leaving **~1.7 KB of IRAM headroom**. This is razor-sharp budgeting.

### 2.3 Flash Bandwidth Optimization

[`platformio.ini`](../platformio.ini#L38) sets `board_build.flash_mode = qio` at 80 MHz. Because cold instruction fetches and data tables in flash share the cache controller with PSRAM, QIO doubles SPI read bandwidth over DIO, cutting Topbench's opcode exercise from 1383 µs to 1077 µs.

---

## 3. Core Subsystem Evaluations

### 3.1 CPU Core (`i8086.cpp` / `i8086.h`)

#### Instruction Batching
In [`Machine::runTask()`](../src/machine.cpp#L484-L536), instructions are executed in batches of 16 (`BATCH = 16`). Calling peripheral housekeeping ([`Machine::tick()`](../src/machine.cpp#L664)) after every single instruction previously cost 91 cycles vs 318 cycles for the instruction itself. Amortizing `tick()` across 16 instructions regained roughly 20% CPU throughput.

#### Fast Paths in `step()`
Common instructions are decoded and executed directly in `step()` via a lookup table [`optcodes[]`](../lib/FabGL/src/emudevs/i8086.cpp#L544), bypassing the heavy ModR/M and micro-function decoding in `stepEx()`:
- `INC`/`DEC` 16-bit registers (Case 19)
- `MOV` register/memory 8-bit and 16-bit (Case 21)
- Immediate ALU operations (Case 22: `ADD`, `OR`, `ADC`, `SBB`, `AND`, `SUB`, `XOR`, `CMP`)
- Single-bit shifts/rotates (Case 23)
- Immediate `MOV` to r/m (Case 25)

#### String Operation Pointer Loops
In [`lib/FabGL/src/emudevs/i8086.cpp`](../lib/FabGL/src/emudevs/i8086.cpp#L1113-L1139), `REP MOVSB` and `REP STOSB` forward operations over plain guest RAM bypass the per-byte `RMEM8`/`WMEM8` callbacks:
```cpp
if (plain) {
  uint8_t * d = s_memory + dst;
  if (kind == 0) {
    uint8_t const * s = s_memory + src;
    for (int32_t i = 0; i < n; ++i)
      d[i] = s[i];
    regs16[REG_SI] += n;
  } else { ... }
}
```
Because the loop executes strictly in ascending order, overlapping memory copies (commonly used in DOS graphics for run-length expansion) behave identically to a physical 8086.

#### Verification Against Hardware Traces
The CPU modifications are validated using [`tools/cputest/run.py`](../tools/cputest/run.py) and [`tools/cputest/harness.cpp`](../tools/cputest/harness.cpp), driving the core against the hardware-verified **SingleStepTests 8088** test suite. This provides high confidence that speedups did not introduce instruction or flag discrepancies.

---

### 3.2 Video Subsystem (`ega.cpp`, `graphicsadapter.cpp`, `machine.cpp`)

#### Interleaved Planar EGA Engine
Traditional EGA emulation maintains 4 separate plane buffers (`plane[4][65536]`). Every CPU write requires 4 loads, 4 ALU operations, and 4 stores.
In [`src/ega.h`](../src/ega.h#L15-L20) and [`src/ega.cpp`](../src/ega.cpp), the 4 planes are interleaved into a single `uint32_t` per byte-column (Plane $N$ resides in byte-lane $N$):
```
[ Byte Lane 3: Plane 3 ] [ Byte Lane 2: Plane 2 ] [ Byte Lane 1: Plane 1 ] [ Byte Lane 0: Plane 0 ]
```
- Latch reads load all 4 planes in a single 32-bit read: `m_latch = *p;`
- Graphics Controller Set/Reset, ALU, Bit Mask, and Map Mask operations run as single 32-bit branchless operations across all 4 planes simultaneously.
- A fast path [`m_simpleWrite`](../src/ega.h#L173) optimizes solid fills (`rep stosw` in Write Mode 0):
  ```cpp
  if (m_simpleWrite) {
    *p = (uint32_t)value * 0x01010101u;
    return;
  }
  ```

#### Banked Memory Allocation (Surviving Heap Fragmentation)
A real EGA has 64 KB per plane (256 KB total). On the ESP32, 128 KB of internal SRAM is the maximum feasible. Rather than allocating 4 contiguous 32 KB blocks (which repeatedly failed due to internal heap fragmentation), [`EGA::allocVRAM()`](../src/ega.cpp#L36-L70) splits the buffer into **8 banks of 16 KB** ([`EGA_BANK_ENTRIES = 4096`](../src/ega.h#L46)). Banks are allocated once and retained across mode changes, preventing heap churn.

#### CGA Optimization & Write Skipping
In [`Machine::setCGAMode()`](../src/machine.cpp#L1180-L1188), register writes to 0x3D8/0x3D9/0x3D4 are checked against current state. Games built on the SCUMM engine (e.g. *Monkey Island*) rewrite video registers on every frame; skipping redundant reconfiguration saves 1–2% of CPU time.

#### Extended Display Modes
- **Plantronics ColorPlus:** Port `0x3DD` supports 320x200 16-color and 640x200 4-color modes, including CPU plane flipping (`address ^= 0x4000` in [`Machine::writeVideoMemory8Impl`](../src/machine.cpp#L2377)).
- **CGA Composite Artifacting:** Realistic NTSC artifact color decoding for 320x200 and 640x200 modes via [`GraphicsAdapter::setCompositeTable`](../lib/FabGL/src/emudevs/graphicsadapter.cpp).
- **Tandy 1000 Graphics:** Modes 8 (160x200x16) and 9 (320x200x16), driven via the 3DAh/3DEh video array and palette mask.
- **On-Screen Drive LED:** In [`lib/FabGL/src/emudevs/graphicsadapter.cpp`](../lib/FabGL/src/emudevs/graphicsadapter.cpp), scanline rendering injects a drive activity indicator directly into the scanline buffer without disturbing video memory.

---

### 3.3 Audio Subsystem (`adlib.cpp`, `sn76496.cpp`, `tandydac.cpp`)

#### AdLib (OPL2 / YM3812)
- Ported DOSBox-X's **DBOPL** FM synthesizer into [`src/opl/`](../src/opl/).
- Tables are precomputed into flash ([`src/opl/dbopl_tables.h`](../src/opl/dbopl_tables.h)).
- The [`AdLib::Voice`](../src/adlib.h#L52) synthesizer runs in a FreeRTOS worker task pinned to the **video core** (`CoreUsage::busiestCore()`) at priority 1.
- Synthesized at native 16,384 Hz into a 1024-sample ring buffer, matching FabGL's `SoundGenerator` sample rate without resampling overhead.
- Timer 1 and Timer 2 (ports 0x388/0x389) are accurately emulated on the 8086 host-time side for sound card detection loops.

#### Tandy Sound & SL/TL DAC
- **SN76496:** 3 square-wave tone channels + 1 noise generator mapped to FabGL sound voices.
- **Tandy SL/TL DAC:** Supports both BIOS calls (`INT 1Ah AH=81h..85h`) and direct hardware register access (ports `0xC4-0xC7`, minimal 8237 DMA Channel 1 controller, IRQ 7). Samples are streamed from PSRAM into a 2 KB internal ring buffer with a tight 15 ms lookahead, allowing MOD players to mix audio directly behind playback without audible glitches.

---

### 3.4 BIOS & System Integration

#### POST & Boot Architecture (`bios.asm` / `bios.cpp`)
- BIOS code was converted from a rigid image into a maintainable NASM source ([`tools/bios.asm`](../tools/bios.asm)) compiled to [`src/biosrom.h`](../src/biosrom.h).
- Warm boots (`Ctrl+Alt+Del`) properly reset the 8042 keyboard controller (`0xAE`) and fetch the boot drive via helper `AH=0x0A`, avoiding hung boots.
- Unbootable disks or missing AA55 boot signatures report descriptive errors (`NO BOOT DEVICE`) instead of jumping to 0000:7C00 and crashing.

#### Fast Native Text Video (`fastTextVideo`)
In [`src/bios.cpp`](../src/bios.cpp#L418-L494), BIOS `INT 10h` services `AH=02h` (set cursor), `09h` (write char/attr), `0Ah` (write char), and `0Eh` (teletype output) are intercepted and handled directly in native C++. By bypassing emulated 8086 instruction execution for BIOS text output, DOS prompt scrolling and text-heavy DOS applications run orders of magnitude faster.

#### Selectable Conventional Memory
Supports 64 KB to 704 KB in 64 KB steps. For 704 KB, the video window start is moved from `0xA0000` to `0xB0000` via [`i8086::setVideoMemStart()`](../lib/FabGL/src/emudevs/i8086.h#L52), transforming `0xA0000–0xAFFFF` into physical guest RAM while updating BDA `40:13h` and the EBDA base address.

#### Serial Debugging & Postbox Communication
- **`SerialDebug` ([`src/serialdbg.cpp`](../src/serialdbg.cpp)):** Line-based remote debugger providing register inspection, memory search/dump, execution tracing, and binary frame grabs ([`tools/screenshot.py`](../tools/screenshot.py)).
- **Postbox ([`src/machine.h`](../src/machine.h#L89-L106)):** A framed host-guest communication protocol on `INT 0xFF AX=0x55EE` paired with [`tools/postbox.py`](../tools/postbox.py) and [`tools/postbox.asm`](../tools/postbox.asm), enabling bidirectional file transfers without network or disk swapping.

#### Award-Style `TextUI`
Replaced FabGL's heavy graphical UI with [`src/textui.cpp`](../src/textui.cpp) and [`src/setupui.cpp`](../src/setupui.cpp). Reuses the emulator's 80x25 character/attribute text buffer, saving ~50 KB of RAM and 16 KB of IRAM while rendering a clean, authentic BIOS setup utility.

---

## 4. Bugs, Edge Cases & Safety Findings

### 4.1 [BUG] `downloadURL()` Flags Incomplete Downloads as Successful

**Location:** [`src/main.cpp`](../src/main.cpp#L304-L320)  
```cpp
while (http.connected() && (len > 0 || len == -1)) {
  size_t size = stream->available();
  if (size) {
    int c = stream->readBytes(buf, size);
    auto wr = fwrite(buf, 1, c, file);
    if (wr != c) {
      dsize = 0;
      break;  // writing failure!
    }
    dsize += c;
    if (len > 0)
      len -= c;
    if (!p.update(tlen > 0 ? (int)((int64_t)dsize * 100 / tlen) : 0, ...))
      break;
  }
}
downloadOK = (len == 0 || (len == -1 && dsize > 0));
```

**Impact:**  
When an HTTP server serves a disk image using chunked transfer encoding (`Transfer-Encoding: chunked`), `http.getSize()` returns `-1`, setting `len = -1`.  
If the connection is prematurely dropped or if the user presses **Esc** to abort the download (`p.update` returns false), the loop breaks. At that moment:
- `len` is still `-1`
- `dsize > 0` is `true` (any bytes received so far)

`downloadOK` evaluates to **`true`**, treating a corrupt or partially downloaded disk image as complete.

**Recommendation:**  
Track an explicit abort/completion flag:
```cpp
bool aborted = false;
...
if (!p.update(...)) {
  aborted = true;
  break;
}
...
downloadOK = !aborted && (tlen > 0 ? (len == 0) : (!http.connected() && dsize > 0));
```
Even better: if `tlen == -1`, verify whether the stream encountered a clean EOF (`stream->available() == 0 && !http.connected()`).

---

### 4.2 [BUG] INT 13h Sector 0 Arithmetic Underflow

**Location:** [`src/bios.cpp`](../src/bios.cpp#L1369-L1382)  
```cpp
bool BIOS::diskHandler_calcAbsAddr(int drive, uint32_t * pos, uint32_t * dest, uint32_t * count)
{
  int sectorsPerTrack = m_machine->diskSectors(drive);
  int heads           = m_machine->diskHeads(drive);
  int track  = i8086::CH() | (((uint16_t)i8086::CL() & 0xc0) << 2);
  int sector = i8086::CL() & 0x3f;
  int head   = i8086::DH();
  if (sector > sectorsPerTrack)
    return false;
  *pos   = 512 * ((track * heads + head) * sectorsPerTrack + (sector - 1));
  *dest  = i8086::ES() * 16 + i8086::BX();
  *count = i8086::AL() * 512;
  return true;
}
```

**Impact:**  
Under the IBM PC BIOS specification for INT 13h, sector numbers are **1-based** (1 to $N$). If a guest program issues a request with `sector == 0` (e.g., buggy bootloaders or disk copy tools):
- `sector > sectorsPerTrack` is `false` (0 is not greater than sectors per track)
- `sector - 1` becomes `-1`
- If `track == 0` and `head == 0`, `*pos` evaluates to `512 * (-1) = 0xFFFFFE00` (or negative in signed arithmetic).
- `fseek(file, pos, SEEK_SET)` seeks to an invalid location past EOF or fails silently.

**Recommendation:**  
Add lower bound validation:
```cpp
if (sector < 1 || sector > sectorsPerTrack)
  return false;
```
Additionally, `track` and `head` should be validated against `m_machine->diskCylinders(drive)` and `heads`.

---

### 4.3 [EDGE CASE] Potential Buffer Overflow in Multi-Sector INT 13h Reads Near 1 MB

**Location:** [`src/bios.cpp`](../src/bios.cpp#L1186-L1189) & [`src/bios.cpp`](../src/bios.cpp#L1449-L1451)  
```cpp
sects = service == 0x02 ?
          fread(m_memory + dest, 1, count, m_machine->disk(drive)) :
          fwrite(m_memory + dest, 1, count, m_machine->disk(drive));
```

**Impact:**  
`dest` is calculated as `ES * 16 + BX`. `count` is `AL * 512` (up to 128 KB for 256 sectors).  
If `dest + count > 0x100000` (1,048,576 bytes), `fread()` writes past the end of the `s_memory` buffer into unmapped or heap-allocated memory in PSRAM.

**Recommendation:**  
Clamp the read/write length to the allocated RAM bounds:
```cpp
if (dest >= RAM_SIZE) {
  diskHandler_floppyExit(0x02, true); // address mark not found / invalid address
  return;
}
if (dest + count > RAM_SIZE)
  count = RAM_SIZE - dest;
```

---

### 4.4 [VULNERABILITY / STABILITY] `runTask` FreeRTOS Stack Size vs SysReq File Dialogs

**Location:** [`src/machine.cpp`](../src/machine.cpp#L473)  
```cpp
void Machine::run()
{
  xTaskCreatePinnedToCore(&runTask, "", 4000, this, 5, &m_taskHandle, CoreUsage::quietCore());
}
```

**Impact:**  
On ESP-IDF, `usStackDepth` in `xTaskCreatePinnedToCore` is measured in **bytes**, not words.  
When the user presses `Alt+PrintScreen` (SysReq):
1. `i8042` invokes [`Machine::sysReq()`](../src/machine.cpp#L2291).
2. Runs inside `runTask()`, calling [`sysReqCallback()`](../src/main.cpp#L615).
3. If the user mounts a floppy, [`setupFileDialog()`](../src/setupui.cpp#L182) is invoked.
4. `setupFileDialog` instantiates `DirList dl` containing `FileBrowser fb`.
5. `FileBrowser` performs VFS and SD-card FatFs directory traversal. FatFs on ESP-IDF can consume 1.8 KB to 2.5 KB of stack space for directory sector buffers, path resolution, and recursive reentrancy.

Coupled with the active call stack of `runTask` -> `tick` -> `i8042::tick` -> `sysReq`, a 4000-byte stack is dangerously close to overflowing, which can cause silent memory corruption or an unexplained FreeRTOS crash.

**Recommendation:**  
Increase `runTask` stack size to at least **6144 or 8192 bytes**. The emulator core has ample DRAM for task stacks.

---

### 4.5 [DOCUMENTATION DRIFT] `lib/FabGL/VENDORED.md` Inaccuracy

**Location:** [`lib/FabGL/VENDORED.md`](../lib/FabGL/VENDORED.md#L27-L29)  
`VENDORED.md` lists only two modified files:
- `src/emudevs/i8086.cpp` / `.h` (`s_videoMemStart`)
- `src/emudevs/i8042.cpp` (`m_keyboard->resumePort()`)

In reality, git history reveals **13 modified files** in `lib/FabGL/` with **1,562 insertions and 548 deletions**, including major alterations to:
- `graphicsadapter.cpp` / `.h` (Plantronics, Tandy modes, Composite LUT, Drive LED, EGA text/graphics)
- `PC8250.cpp` / `.h` (Software UART bridging)
- `vgadirectcontroller.cpp` / `.h` (Drive LED hook)
- `displaycontroller.cpp` (Queue size adjustments)

**Recommendation:**  
Update `lib/FabGL/VENDORED.md` to document all local changes, or execute Step 2 of [`docs/platform-split.md`](../docs/platform-split.md#L93-L96) to extract these modified classes into project-owned directories (`core/` and `display/`).

---

### 4.6 [RESOURCE HAZARD] SysReq Resolution Switch Without RAM Verification

**Location:** Described in [`docs/TODO.md`](../docs/TODO.md#L6-L14)  
Opening the SysReq menu triggers a video mode switch to 640x480 text mode. If the guest was running in a 320-wide mode (e.g. CGA 320x200 or Tandy mode 9), FabGL allocates new scanline DMA buffers and descriptors (~14 KB additional internal RAM). If internal RAM is heavily fragmented or exhausted by the sound card, FabGL hangs.

**Recommendation:**  
Before calling `ui.begin()` in `sysReqCallback()`, calculate needed DMA and line buffer RAM based on `machine->graphicsAdapter()->emulation()` and verify against `heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`. If insufficient, refuse to open the menu with a serial log warning rather than locking up the system.

---

## 5. Code Quality & Engineering Best Practices

### Strengths
1. **Exceptional In-Code Explanations:** The codebase is filled with clear, educational comments explaining the *rationale* behind non-obvious code (e.g., why 8042 command `0xAE` is needed in POST, why the switch sense is not inverted in EGA port 0x3C2, why QIO flash mode is critical).
2. **Strict Hardware Fidelity:** Hardware corner cases are respected—such as odd/even video addressing in EGA text mode, NTSC color burst suppression in CGA mode 5, and the 15 ms sound lookahead to match guest MOD mixer buffers.
3. **High Diagnostic Visibility:** The integration of the serial debug monitor, port tracer, memory dumper, and screenshot utility turns an embedded target into a fully observable development environment.
4. **Clean Decoupling of Optional Hardware:** Systems like the AdLib synth and Tandy DAC can be toggled on/off without polluting baseline CGA/XT emulation performance.

### Minor Code Smells & Nitpicks
1. **Magic Numbers in Ports & Registers:** While major ports are well-commented, several peripheral registers in `machine.cpp` (e.g. `0x61` bits, `0x3DF` page bits) would benefit from symbolic constants (`constexpr uint8_t PORT_B_SPEAKER_GATE = 0x01;`).
2. **Global Static Pointers in CPU Core:** `s_memory`, `regs8`, `regs16`, and flags in `i8086.cpp` are file-scope static variables. While this maximizes execution speed by allowing absolute addressing, it prevents running multiple instances of the CPU core concurrently (relevant for the upcoming headless testing harness in `platform-split.md`).

---

## 6. Portability & Platform Split Review (`docs/platform-split.md`)

The roadmap to target the **Raspberry Pi RP2350** alongside the ESP32 WROVER is well conceived. The core principles (zero virtual call overhead, compile-time platform selection via inline headers, host Linux testing) are sound.

### Key Considerations for RP2350:
1. **Memory Split (The 520 KB SRAM Constraint):**  
   The RP2350 has 520 KB of on-chip SRAM. Running 640 KB of guest RAM plus video buffers requires external QSPI/Octal PSRAM. Because external PSRAM is accessed via the RP2350 cache, placing the lowest 64 KB of guest RAM (IVT, BDA, DOS data) and video buffers in internal SRAM while mapping upper RAM to PSRAM will be vital to matching ESP32 speeds.
2. **Video Backend:**  
   The RP2350's PIO state machines can synthesize VGA (RGB565 via resistor ladder) or HSTX for direct DVI/HDMI. Shifting from FabGL's 6-bit packed format to RGB565 will unlock true 16-color and 256-color palettes (VGA/MCGA), making the display abstraction layer in Step 9 the most rewarding refactor.
3. **Decoupling FabGL:**  
   FabGL is unmaintained and tightly coupled to the ESP32. Migrating `i8086`, `PIC8259`, `PIT8253`, `i8042`, `PC8250`, and `MC146818` into a standalone `core/` folder (Step 2) should be prioritized immediately.

---

## 7. Recommended Action Plan

| Priority | Task | Affected Files | Effort |
| :---: | :--- | :--- | :---: |
| **High** | **Fix `downloadURL` truncated download bug** by checking abort status and clean EOF | [`src/main.cpp`](../src/main.cpp) | 30 mins |
| **High** | **Fix INT 13h sector 0 underflow** and add cylinder/head bounds checks | [`src/bios.cpp`](../src/bios.cpp) | 30 mins |
| **High** | **Increase `runTask` stack size** from 4000 to 8192 bytes | [`src/machine.cpp`](../src/machine.cpp) | 10 mins |
| **Medium** | **Guard SysReq menu memory** against current video mode DMA requirements | [`src/main.cpp`](../src/main.cpp), [`src/textui.cpp`](../src/textui.cpp) | 2 hours |
| **Medium** | **Update `lib/FabGL/VENDORED.md`** or extract devices to `core/` | [`lib/FabGL/VENDORED.md`](../lib/FabGL/VENDORED.md) | 1 hour |
| **Low** | **Clamp multi-sector INT 13h buffer addresses** to 1 MB limit | [`src/bios.cpp`](../src/bios.cpp) | 30 mins |

---

## Annotations by Claude (added 2026-09-30, not part of Gemini's review)

> Everything above this line is Gemini's review as it was delivered, with one change: its links
> were absolute `file:///` paths into the maintainer's checkout and were rewritten as paths
> relative to this file. This section was added afterwards by Claude (Claude Code, Opus 5.5),
> which checked each finding against the code and acted on it with the maintainer.

### Findings, as checked

| # | Verdict | What was done | Commit |
|---|---------|---------------|--------|
| 4.1 | **Confirmed, and worse than described.** Besides the aborted download passing as complete, `HTTPClient::getStreamPtr()` is the raw socket and does not decode chunked transfer encoding, so a chunked "successful" download would have written the chunk headers into the image. | `downloadURL()` asks for HTTP/1.0, so servers do not send chunked bodies, and fails on Esc, a write error or a missing stream; the caller then deletes the partial file. Remaining limit: with no Content-Length, a dropped connection looks like the end of the body. | `f5e7476` |
| 4.2 | **Confirmed.** The consequence was not a seek past EOF: the `fseek` to 0xFFFFFE00 failed, and the transfer silently used whatever position the file was left at, returning the wrong sectors as a success. | `diskHandler_calcAbsAddr()` rejects sector 0 and a head beyond the geometry ("sector not found"). A cylinder past the end already failed through `fread` at EOF. | `f5e7476` |
| 4.3 | **Harmless as described**: the worst case ends about 128 KB past guest RAM, in the core 0 half of PSRAM, where nothing else lives. A related real issue: transfers went straight into `m_memory`, bypassing the video adapter and overwriting the ROM images. | New `BIOS::diskHandler_transfer()`: only memory below the video window (A0000h, or B0000h with 704 KB) is transferred directly; the rest is open bus, so a disk read into it is dropped and a disk write from it stores 0xFF. Needed a one line `i8086::videoMemStart()` getter. | `f5e7476` |
| 4.4 | **Plausible, then measured.** The chain exists (the SysReq menu runs on the emulator task, and the floppy dialog walks the SD card). The overflow would not be silent: the framework has the end of stack watchpoint and the canary check on, so it would panic at once. | On the maintainer's decision, a guard rather than a bigger stack (internal RAM is scarce): the file dialog refuses to open with less than 3072 bytes left and logs its use. Measured on the board: 3588 bytes free on entry, about 2530 used by the dialog. More stack is left for the planned port. | `f5e7476` |
| 4.5 | **Confirmed.** One detail was wrong: the `displaycontroller.cpp` change moves code out of IRAM, it does not adjust queue sizes. | `lib/FabGL/VENDORED.md` now lists every locally changed file. | `f5e7476` |
| 4.6 | Already known and parked in `docs/TODO.md`. | Nothing new. | — |

### Not checked

Sections 2, 3, 5 and 6 (architecture, subsystems, code quality, portability) were read but not
verified claim by claim; nothing in them looked wrong on reading. The RP2350 notes in section 6
belong with `docs/platform-split.md`.
