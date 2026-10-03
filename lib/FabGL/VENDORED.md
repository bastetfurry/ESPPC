# Vendored FabGL

This is the `src/` tree of **FabGL 1.0.9** by Fabrizio Di Vittorio, copied here verbatim and
placed under this project's control. GPL v3 — see `LICENSE`.

- Upstream: https://github.com/fdivitto/FabGL  (tag v1.0.9)
- Vendored on: 2026-09-23
- Only `src/`, `library.json`, `library.properties` and `LICENSE` were taken. Upstream's `docs/`
  (63 MB) and `examples/` (45 MB) are deliberately omitted.

## Why it is vendored rather than a lib_deps entry

This project modifies emulator internals. A dependency fetched by PlatformIO lands in
`.pio/libdeps/`, which is gitignored and destroyed by `pio run -t fullclean`, so any edit made
there would vanish silently. Vendoring makes those changes durable and reviewable, and lets the
project build with no network access.

Partial vendoring is not possible: copying only `emudevs/` into `src/` while FabGL is still a
`lib_deps` entry compiles both copies and fails to link with duplicate symbols.

## Local modifications

Record every change here as it is made, so a future upstream merge knows what to preserve:

| File | Change | Why |
|------|--------|-----|
| `src/emudevs/i8086.cpp`, `src/emudevs/i8086.h` | `VIDEOMEM_START` constant replaced by the runtime `i8086::s_videoMemStart` / `setVideoMemStart()` / `videoMemStart()`, defaulting to 0xA0000 | The machine's video memory only covers 0xB0000-0xBFFFF (`Machine::writeVideoMemory8` discards anything lower), so trapping from 0xA0000 leaves a dead 64KB hole that reads 0xFF and swallows writes. A machine configured above 640KB moves the window to 0xB0000, turning that hole into ordinary RAM for 704KB conventional. It must stay runtime-selectable so 640KB machines keep IBM-compatible behaviour. **Paired with `Machine::setRamSize()` and the BIOS helper AH=0x09 in `src/bios.cpp` / `tools/bios.asm`.** |
| `src/emudevs/i8042.cpp` | `reset()` now calls `m_keyboard->resumePort()` so the physical keyboard port matches the command byte it installs (the mouse port is deliberately left alone — see the comment in the code) | Upstream assigns `m_commandByte` directly, bypassing `updateCommandByte()` — the only thing that suspends/resumes the real ports. A guest that had suspended the keyboard (`CMDBYTE_DISABLE_KEYBOARD`, routine in BIOSes) and then issued a soft reset (8042 command 0xFE, ie Ctrl-Alt-Del) left the byte saying "enabled" while the port stayed suspended. `updateCommandByte()` only acts on a *change*, so `resumePort()` was never called again and the keyboard was dead until a power cycle. |
| `src/emudevs/i8086.cpp`, `src/emudevs/i8086.h` | `step()` and `stepEx()` in IRAM, register accessors inlined; fast paths in `step()` for common opcodes and for MOVS/STOS/LODS, with a pointer loop for REP over plain RAM; rare opcodes moved to `stepExCold()` in flash | Speed (Topbench 11 to 17). Checked against the SingleStepTests 8088 suite with `tools/cputest/`. |
| `src/emudevs/i8042.cpp`, `src/emudevs/PIC8259.cpp`, `src/emudevs/PIT8253.cpp` | `tick()` and the interrupt path marked `IRAM_ATTR` | They run on every instruction batch; from flash they compete with PSRAM for the cache. |
| `src/emudevs/PC8250.cpp`, `src/emudevs/PC8250.h` | Bridged mode: `setBridge()`, `clearBridge()`, `injectRx()`, modem lines reading as a ready DTE; `setSerialPort()` takes the UART index (upstream hardcodes UART2), `hasSerialPort()` | Lets COM1/COM2 run over the USB serial while the debug console owns the UART pins, and lets a second port sit on UART1 (the U1 header) next to one on UART2. |
| `src/emudevs/graphicsadapter.cpp`, `src/emudevs/graphicsadapter.h` | Most of the video work: EGA (planar engine hookup, text mode with the font in plane 2, 350 line modes, split screen, resident planes, 6 bit palette), CGA mode 5 palette and skipped no-op register writes, Tandy modes 8 and 9, Plantronics ColorPlus, composite colour, drive LED, faster renderers, font fallback to flash; Hercules removed | The emulator's video adapters. See the git log of these files for the reasoning behind each. |
| `src/dispdrivers/vgadirectcontroller.cpp`, `src/dispdrivers/vgadirectcontroller.h` | Scanline interrupt timing counters (`s_isrMaxCycles`, `s_isrMinPeriod`, `s_isrOverruns`) | Read by the debug monitor's `v` command to catch renderer overruns. |
| `src/displaycontroller.cpp`, `src/fabutils.cpp` | `IRAM_ATTR` removed from the primitive machinery and `Rect` helpers | Only FabGL's paletted controllers ran them from an interrupt, and this build uses none; frees about 8 KB of IRAM. |

## Checking against upstream

```sh
git clone --depth 1 --branch v1.0.9 https://github.com/fdivitto/FabGL.git /tmp/fabgl-upstream
diff -rq lib/FabGL/src /tmp/fabgl-upstream/src
```
