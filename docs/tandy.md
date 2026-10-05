# Tandy 1000 graphics and sound

Reference and design notes for the planned "Tandy 1000" video option. The hardware facts were
cross-checked against the Tandy code in 86Box (`src/video/vid_tandy.c`, `src/machine/m_tandy.c`)
and DOSBox Staging (`src/ints/int10_modes.cpp`). Both were read as references only; no code was
taken from them.

Space Quest 3 is the test case for mode 9. Planet X3 (option 4 in its video menu) is the test case
for mode 8.

## How software recognises a Tandy

Software reads two BIOS bytes. It does not probe the sound chip, because the SN76496 is write
only and cannot be probed.

| address    | Tandy | ours today                  |
|------------|-------|-----------------------------|
| F000:FFFE  | FFh   | FCh (AT), copied from `mem_top` in bios.asm at POST |
| F000:C000  | 21h   | free: the BIOS image ends around F000:4200 |

Both can be patched from `Machine::prepareVideoROM()`, which runs after POST has copied `mem_top`,
and only on a Tandy machine.

Some software wants more than the single byte. Sierra's installers (KQ5's INSTALL.EXE, 1991) scan
**F000:C000-C3FF for the string "Tandy"**, which on the real ROM is part of the copyright notice
there. We write `!Tandy` at F000:C000, which satisfies both checks. The same installer then probes
for a monochrome adapter (CRTC register 0Fh read back at 3B4h/3B5h, bit 7 of 3BAh toggling), so a
Tandy machine does not answer the Hercules ports either. The INT 15h C0h configuration table (bios.asm, `MODEL`) also reports
the model; whether that matters is for the trace to show.

## Video memory

There is no separate video RAM. The video circuit shares the **top 128 KB of conventional
memory**, `base = RAM size - 128 KB`, which is 80000h on a 640 KB machine. That region is split
into eight 16 KB pages.

**Page register, port 3DFh (write only):**

| bits | 16 KB modes (bits 7-6 != 11) | 32 KB modes (bits 7-6 = 11) |
|------|------------------------------|-----------------------------|
| 2-0  | CRT page: what is displayed  | bits 2-1: CRT page, 32 KB aligned |
| 5-3  | CPU page: what B8000 shows   | bits 5-4: CPU page, 32 KB aligned |
| 7-6  | address mode                 | 11 = 32 KB modes (9, 0Ah) |

- The B8000 window is only an alias for the CPU page. It is 16 KB wide (mask 3FFFh) in the 16 KB
  modes and 32 KB wide (mask 7FFFh) in the 32 KB modes.
- Software can equally write the screen at its real address, 9C000h and so on.
- The BIOS mode set leaves **3Fh** (CRT and CPU page 7, 9C000h) for modes 0-8 and **F6h**
  (32 KB page 3 = pages 6-7, 98000h) for modes 9 and 0Ah. It keeps a copy in the BIOS data area.

### Plan: the top 64 KB lives in the internal video buffer

Route the guest's **90000h-9FFFFh** (pages 4-7) through the existing 64 KB internal buffer,
exactly as B0000h-BFFFFh is routed on the CGA machine: `setVideoMemStart(0x90000)` and a
Tandy branch in the `write/readVideoMemory8/16` callbacks. B8000h-BFFFFh then maps to
`buffer + cpuPage*16K - 10000h` under the window mask.

- The renderer only ever reads internal RAM, never PSRAM, so the core affinity and cache
  problems of guest RAM do not apply.
- It would need the CGA buffer back at 64 KB: since Hercules was removed it is 32 KB, covering
  B8000-BFFFF only.
- It needs 640 KB or less, since the machine has to have that memory top. The Tandy 1000 never had
  more.
- Pages 0-3 (80000h-8FFFFh) stay plain PSRAM. A game that points the **CRT** page there gets a
  one-time log line instead of a picture. The trace will show whether any test case does that.
- DOS code that runs from 90000h-9FFFFh becomes a little slower, because those accesses go
  through a callback.
- INT 12h reports the configured RAM, the full 640 KB. The original Tandy 1000 and the SX/EX/HX
  took their video pages out of the 640 KB, but the SL/TL/TX have 768 KB with the top 128 KB,
  above 640 KB, reserved for video, and DOS keeps all 640 KB. Here the pages live in the internal
  buffer and never overlap DOS memory, so there is nothing to reserve. If aliasing is added, it
  should follow the SL/TL layout, with the pages above 640 KB.

## Registers

The CGA registers keep their meaning: CRTC at 3D4h/3D5h, mode control at 3D8h, colour select at
3D9h, status at 3DAh (read). The additions are:

- **3DAh (write)**: video array index (`& 1Fh`)
- **3DEh (write)**: video array data
  - reg 1: palette mask, 0Fh after a mode set; the pixel value is ANDed with it
  - reg 2: border colour (low 4 bits)
  - reg 3: mode control. **Bit 4 = 16 colour graphics** (modes 8 and 9), **bit 3 = 640x200x4**
    (mode 0Ah)
  - reg 5, reg 8: cleared by the BIOS (extended mapping, monitor mode)
  - reg 10h-1Fh: the 16 palette entries, 4 bit IRGB, identity after a mode set
- **3DFh (write)**: page register, see above

Mode 8 and mode 9 differ only in 3D8h bit 0 (the high resolution clock), in the CRTC values, and in
how many scanlines make up one character row.

The palette registers apply in **every** mode, the CGA and text modes included, because every
pixel goes through them. Identity after a mode set means today's CGA renderers are correct until a
program reprograms the palette. Honouring that in the CGA modes is a later refinement: the CGA
LUT would be built through the palette.

## Modes

The BIOS mode set, per DOSBox:

| mode | size       | 3D8h | 3D9h | array reg 3 | page reg | CRTC R0/R1/R4/R6/R9 | memory |
|------|------------|------|------|-------------|----------|---------------------|--------|
| 8    | 160x200x16 | 2Ah  | 30h  | 14h         | 3Fh      | 56/40/127/100/1     | 16 KB: 2 banks, 80 bytes a line |
| 9    | 320x200x16 | 2Bh  | 30h  | 14h         | F6h      | 113/80/63/50/3      | 32 KB: 4 banks, 160 bytes a line |
| 0Ah  | 640x200x4  | 3Bh  | 3Fh  | 0Ch         | F6h      | 113/80/63/50/3      | 32 KB: 4 banks, 160 bytes a line |

Other CRTC values: R2 = R1 + 1, R3 = 0Ah, R5 = 6, R7 = R6 + (R4 - R6) / 2 - 1. The mode set also
writes array reg 1 = 0Fh, reg 2 = 0, the identity palette to reg 10h-1Fh, and reg 5 = reg 8 = 0.

**Pixel layout:** a line starts at `(line % rows) * 2000h + (line / rows) * bytesPerLine` within
the CRT page, where `rows` = R9 + 1, so 2 for mode 8 and 4 for mode 9. Every byte holds two pixels,
high nibble first.

- Mode 9: one pixel is one output pixel at 320 wide.
- Mode 8: each pixel is two output pixels wide at 320.
- Mode 0Ah: 86Box calls its implementation "a complete guess", so it is left out of round one.

### Renderers

Both renderers run on the QVGA 320x240 timing, like CGA 320x200, and are table driven:

- mode 9: `uint16_t LUT[256]`, two raw pixels per source byte
- mode 8: `uint32_t LUT[256]`, four raw pixels per source byte; the same shape as the CGA
  320x200 renderer (1 KB)

The tables are rebuilt whenever the palette, the mask or the border changes. Together that is
roughly 150 bytes of IRAM, against about 2.9 KB free.

## BIOS

- The mode hook (helper AH=0Bh) takes modes 8 and 9 on a Tandy machine. It programs the registers
  above and fills the data area: mode, columns 20 or 40, page size, and the page register copy.
- INT 10h AH=0Ch/0Dh (write and read pixel) and character drawing in modes 8 and 9.
- INT 10h AH=10h AL=00h-02h: the PCjr/Tandy palette calls. The pure CGA gate added in 0f2a1d9 asks
  helper AH=0Eh "is an EGA fitted". It has to become "which adapter", so that a Tandy answers
  these calls and still refuses AH=12h.
- The BIOS data area keeps the CGA equipment bits (10b, 80x25 colour).

## Sound: SN76496 at C0h

The chip is clocked at 3.579545 MHz. 86Box decodes C0h-C7h.

- A byte with bit 7 set is a **latch**: bits 6-5 pick the channel, bit 4 picks volume (1) or
  tone/noise (0), and bits 3-0 are data.
- A byte with bit 7 clear carries the **upper 6 bits** of the latched tone channel's 10 bit
  divider.
- Tone: `f = 3579545 / (32 * N)`. Volume: 4 bit attenuation in 2 dB steps, 15 = off.
- Noise: bit 2 picks white (1) or periodic (0). Bits 1-0 pick the shift rate: N = 512, 1024,
  2048, or tone channel 3's divider.

For output there are three `SquareWaveformGenerator` and one `NoiseWaveformGenerator` on the
existing `SoundGenerator`, next to the PC speaker's sine, with a 16 entry attenuation table mapped
to FabGL's volume range (0-127). Periodic noise can be approximated.

Unhandled port writes are already dropped, so games that write C0h do no harm today even without
sound.

## Tracing a program

The monitor's `t` command records port accesses and software interrupts, and
`tools/tracesession.py` drives a whole session. For example:

    tools/tracesession.py --trace "3d0 3df int 10 1a" --type 'cd \sierra\sq3' --type 'sq' \
        --run 60 --out sq3-video.trace
    tools/tracesession.py --trace "c0 c7" ... --out sq3-sound.trace

A few things to know when reading the output:

- INT entries show the caller's CS:IP and AX.
- A repeated identical access from the same place collapses into one line with `xN`.
- The buffer keeps the **first** 512 entries after arming, which is where mode sets and probes
  happen.

## What Space Quest 3 does (traced 2026-09-27)

The traces were taken on the CGA machine with SQ3's Tandy drivers configured, from the start of the
game to the dumpster scene.

**Video.** SQ3 made only four hardware-relevant calls:

    INT 10h AH=0Fh            read the current mode (to restore it at exit)
    INT 10h AX=0089h          set mode 9, bit 7 = keep video memory
    INT 10h AH=0Bh            background colour black
    out 3D8h <= 0Bh           mode control: graphics, high resolution clock, video on

- It never writes 3DAh/3DEh, 3DFh or the CRTC. **The BIOS mode set has to program all of them**:
  palette identity, mask 0Fh, array reg 3 = 14h, page register F6h, and the CRTC.
- It draws through **B8000h in the 32 KB four bank layout**: B800:0000, 2000, 4000 and 6000 all
  held picture data. It does not use the real RAM at 98000h.
- It never looked at F000:FFFE or F000:C000 (they read FCh and 00h, and it ran anyway), because
  the driver comes from its configuration.
- It made no INT 11h or INT 12h calls. On the CGA machine, 98000h-9FFFFh held DOS data (the
  COMMAND.COM transient), which is fine: the video memory is the separate buffer behind B8000h,
  so a Tandy machine keeps the full 640 KB, like a Tandy 1000 SL/TL/TX.

So round one does not need the 9xxxx aliasing: B8000h in the existing buffer, with the page
register stored but not acted on, is enough for SQ3. The aliasing can follow if a game flips pages.

**Sound.** About 130 writes a second, all to port C0h, in plain SN76496 protocol:

- volume latches for channels 0-2 (software envelopes, so most writes are volume)
- tone latches followed by one data byte, e.g. ABh 23h = channel 1, N = 23Bh, 196 Hz
- no noise channel in the 20 s traced, and no INT 1Ah calls

## Status and to do

Round one works on hardware (2026-09-27): Space Quest 3 (mode 9) and Planet X3 (mode 8) run with
their Tandy graphics and sound.

Still to do:

- **Page register 3DFh**: stored and copied to 40:8A, but neither the display (CRT page) nor the
  B8000 window (CPU page) follows it. Games that page flip need it, and so does software that
  writes the screen at its real address (9C000h and so on), which needs the 9xxxx aliasing
  described above, with the pages above 640 KB as on the SL/TL.
- **CRTC start address** in the Tandy modes (hardware scrolling). It moves all four banks at
  once, so the renderers need a wrapping offset per bank.
- **The rest of the video array**: the border colour (reg 2) is stored but not drawn, and reg 5
  (extended mapping) and reg 8 (monitor mode) are ignored.
- **Palette registers in the CGA and text modes**: every pixel goes through them on a real
  Tandy. Identity after a mode set, so only programs that reprogram them notice.
- **Mode 0Ah** (640x200x4), whose memory layout even 86Box calls a guess.
- **BIOS text** (INT 10h AH=09h/0Ah/0Eh) in modes 8 and 9.
- **SN76496 noise**: the shift rate and the periodic mode (FabGL's noise generator is plain white
  noise).

## The SL/TL DAC

The Tandy 1000 SL/TL/RL add a DMA fed DAC to the PSSJ chip: ports C4h-C7h, DMA 1, IRQ 7. It is a
per-machine option ("tandydac 1", needs the sound chip). Software reaches it two ways, and both are
emulated (src/tandydac.*):

**Through the BIOS** (KQ5's TANDY.DRV, "Tandy 1000 SL, TL, HL, RL series"):

- `INT 1Ah AH=81h`: detect. "Present" means AX comes back with the high nibble clear (the base
  port, C4h); CF set means busy. Without the DAC the call fails as before.
- `AH=85h` reset, `AH=83h` play ES:BX, CX bytes, DX = clock divider (rate = 3579545 / DX),
  AL = volume; `AH=84h` stop (still with the callout, like the real BIOS); `AH=82h` record is
  refused.
- End of a block: the BIOS points IRQ 7 at its own handler for the block, keeping the old vector.
  When the block has been heard, the handler restores the vector and issues `INT 15h AX=91FBh`,
  the "device done" callout the driver hooks INT 15h for.

**Directly** (a MOD player, traced 2026-09-27): INT 1Ah AX=8100h only to find the port, then

    out 0Ah <= 05, 0Bh <= 59h, 0Ch, 03h count, 02h address, 83h page, 0Ah <= 01   (DMA 1, auto-init)
    out C6h <= divider low, C7h <= high, C4h <= 17h then 1Fh                      (playback, DMA on)
    in C7h, out C7h <= (low 5 bits kept) | volume << 5                             (amplitude)

and it then polls DMA channel 1's current address (port 02h) to mix behind the play position. So
the machine has a minimal 8237 when the DAC is fitted: all four channels' registers, the flip-flop,
masks, modes, status and page registers, with only channel 1 moving. Its current address and count
follow what the DAC has actually played. The DAC's registers read back (the player relies on C7h
doing so), playback runs while C4h says playback with DMA and channel 1 is unmasked, and every
terminal count flags C4h bit 3 (cleared by reading C4h) and raises IRQ 7.

Samples are copied from guest RAM into a 2 KB ring by the emulator task, only about 15 ms ahead of
playback, because players mix into the part of their buffer that has just been played.

Not modelled: C5h (direct output and the ADC), recording, DMA channels other than 1 transferring,
and the demand/block DMA modes (everything is played as single transfers at the DAC's rate).
