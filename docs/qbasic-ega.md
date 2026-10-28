# Why QBasic would not use the EGA modes

Status: **resolved** (2026-09-25, commit ca8f8c8). QBasic 1.1 refused `SCREEN 7/8/9/10` with
"illegal function call" and never issued `INT 10h AH=00` for modes 0Dh..10h, while the EGA itself
worked (Planet X3's EGA build ran in mode 0Eh).

**The cause:** this BIOS answered `INT 10h AH=1Bh`, the VGA's functionality and state call, which no
EGA BIOS implements. QBasic's video runtime gates each mode on two bytes, `test [270Dh]` (16h for
SCREEN 7 and 8, 14h for 9, 09h for 10) and `test [270Ch],0Ch` for all of them. On the VGA path that
an answer to 1Bh selects, [270Ch] is 10h unless the static functionality table claims mode 12h,
which an EGA cannot do, and 10h AND 0Ch is zero: every mode refused. Unanswered, 1Bh sends QBasic
down its EGA path, which reads the switches through AH=12h BL=10h: 04h in both bytes, which SCREEN
7, 8 and 9 accept. SCREEN 10 still refuses, correctly, since it is the monochrome 640x350 mode.
It was AH=1Ah's mistake again, one function along (see below).

The rest of this file is the investigation as it stood the day before, kept for how it got there:
the methods (reading a PKLITE packed program out of a live machine) are reusable. Its conclusions
about the real EGA BIOS are superseded by the above.


## Getting at the code

`QBASIC.EXE` is **PKLITE compressed** (`PKLITE Copr. 1990-92 PKWARE Inc.` at file offset 0x30,
entry `CS:IP = FFF0:0100`, one relocation). Nothing useful disassembles from the file on disk: the
real instructions only exist once DOS has unpacked it into RAM.

Everything below was read out of a live machine through the serial debug console:

    f b8 00 1a cd 10        find "mov ax,1A00 / int 10h"   -> the detection routine
    d <addr> <len>          hex dump, then ndisasm -b16 -o <addr> on the host
    b <addr>                one-shot breakpoint
    s                       single step

**QBasic relocates between launches.** The detection routine has been seen at 0x4A636, 0x4A5E6
and a third offset about 0x52 away. *Always* locate it by signature on the run you are working
with. Hardcoding an address from a previous run silently patches the wrong instruction.


## QBasic's adapter detection

    mov ax,1a00 / int 10 / cmp al,1a / jnz fallback
      cmp bl,7  / cmp bl,8      -> VGA,  capability 0x40
      cmp bl,0b / cmp bl,0c     -> MCGA, capability 0x20
      xor ax,ax                 -> anything else, including EGA: no capability

    fallback:
      mov ah,12 / mov bl,10 / int 10      ; EGA configuration
      cmp bl,10                           ; unchanged -> no EGA, bail
      int 11                              ; equipment word
      and al,30 / add al,d0 / sbb bh,0 / jnz bail
      cmp cl,0b / 0a / 04 / 05  -> capability 0x10
      cmp cl,03 / 09            -> capability 0x08
      otherwise                 -> capability 0x04
      or bl,bl / jnz / or ax,4            ; BL = 0 (64 KB) also sets bit 2
      ...
      mov ah,0fa / xor bx,bx / int 10     ; mouse driver's EGA interface probe
      or bx,bx / jz / or dl,80
      mov al,dl
      mov [di+0ah],al                     ; <- capability byte cached here
      test al,7c                          ; any graphics adapter at all?
      ...
      mov [di+0bh],ah                     ; <- display type: 1 mono, 2, or 4

Note there is **no EGA display combination code in the first branch at all**. DCC 04/05 fall
straight into `xor ax,ax`.


## The one real fix so far

A genuine EGA BIOS does not implement `INT 10h AH=1Ah` -- that call arrived with the PS/2 in
1987, three years after the EGA. This BIOS answered it anyway, which sent QBasic into the
VGA/MCGA-only branch where EGA means "no capability", and the fallback EGA test was never
reached.

Reporting `AH=1Ah` unsupported (commit `bd337a6`) fixes that. QBasic now **does** detect the
card: traced calling `AH=12h BL=10h`, and a breakpoint on the store confirmed `AX=0008`.

It still refuses the screen modes, so something further gates them.


## Ruled out, with evidence

Every run below carried a positive control: `T.BAS` starts with `SCREEN 1`, which produces
`VID04` on the serial line from `bios.asm`'s mode-set debug print. Without that control, "no EGA
modes attempted" is indistinguishable from "the probe never ran" -- which did happen, twice. An
earlier sweep was run *before* the control existed and had to be discarded and redone.

**The whole `AH=12h BL=10h` answer space.** All controls passed, no EGA mode attempted in any case:

| `CL` (switch settings) | `BL` (memory size) | result |
|---|---|---|
| 09, 03 (capability 0x08) | 1 | none |
| 0B, 0A, 04, 05 (capability 0x10) | 1 | none |
| 00 (capability 0x04) | 1 | none |
| 0B | 0 (64 KB) | none |
| 09, 0B, 03 | 3 (256 KB) | none |
| 09 | 2 (192 KB) | none |

`BH` is 0 (colour) throughout. So switch settings, capability class and reported memory size are
all irrelevant to the decision.

**The save pointer table at 40:A8 being non-zero.** Pointed it at zeroed BDA space; no change.
(This does not rule out QBasic *following* the table and wanting real contents.)


## NOT ruled out: forcing the capability byte

Two attempts, both methodologically broken. Neither result means anything.

1. Patched the **stored** byte at `[di+0ah]` after the store executed. The routine returns the
   capability in **AL**, and only the cache path (`mov al,[di+0ah]` on a later call) re-reads
   memory, so the caller never saw the patched value.

2. Patched the instruction `mov al,dl` (the 2 bytes before the store) to `mov al,imm` -- correct
   approach, but at an address hardcoded from an earlier run. QBasic had relocated, so it
   overwrote an unrelated instruction.

**The experiment to actually run:** locate the routine by signature each run; the store is at
`site + 0xDA` and `mov al,dl` is 2 bytes before it. Break there, patch those 2 bytes to
`B0 <cap>`, resume, and see which capability value (0x10, 0x20, 0x40, 0x7F) unlocks the modes.
If none do, the capability byte is not the gate and the search moves downstream.


## The downstream chain, mapped

- `[0x1aa5]` (DS-relative in QBasic's data segment) holds the **BIOS video mode number**.
- The mode is set by `mov al,[0x1aa5] / mov ah,0 / call <wrapper>`.
- The wrapper is a generic `INT 10h` thunk: `push bp / push si / push di / int 10h / pop ...`.
  It is called **near**, so on the stack at the `INT`: `IP CS FLAGS di si bp` then the near
  return offset, with the segment being the wrapper's own `CS`.
- Immediately above the wrapper sits `cmp bl,0dh` and `mov dx,3ce / mov ax,105 / call ...` --
  QBasic writing the **Graphics Controller**. It has working EGA planar code; it simply will not
  enable it for us.
- There is **no `mov [0x1aa5],al`** anywhere in memory (`f a2 a5 1a` -> 0 hits), so the variable
  is written through some other addressing form. Finding that write is the most likely location
  of the `SCREEN` validation.


## Patching the BIOS ROM in guest RAM (fast iteration)

Reflashing is about four minutes per experiment; patching the ROM where it sits in guest memory
is about one. The ROM is loaded at guest linear `0xF0100 + <file offset>`, and the file offsets
come from the nasm listing (`nasm tools/bios.asm -l out.lst`).

For the `AH=12h BL=10h` answer the immediates are:

| register | guest address |
|----------|---------------|
| `BH`     | `F0E01`       |
| `BL`     | `F0E03`       |
| `CH`     | `F0E05`       |
| `CL`     | `F0E07`       |

Verify with `d f0dfa 20`; the sequence reads `80 FB 10 75 08 B7 00 B3 01 B5 00 B1 09 CF`.


## Traps worth remembering

- **`x` (binary export) has no flow control.** A 192 KB transfer at 115200 silently drops bytes
  and shifts everything after them, while still returning the expected byte count -- a corrupt
  dump looks like a good one. Verify large dumps against a known signature, or stay small and use
  `d`.
- **Guarantee the console state before typing at the guest.** Text typed while the console is in
  the monitor goes to the monitor's command parser, which answers `unknown command` and looks
  exactly like a machine that is ignoring you.
- **Always give a guest-side probe a positive control.**


## What QBasic actually complains about

Asked directly, with a program that traps the error and prints it:

    ON ERROR GOTO H
    FOR M = 7 TO 10 : X = M : SCREEN M : SCREEN 0 : PRINT "MODE"; X; "OK" : NEXT M
    SYSTEM
    H: SCREEN 0 : PRINT "MODE"; X; "ERR"; ERR : RESUME NEXT

gives **`ERR 5`, "Illegal function call", for all four modes**. So this is QBasic's own mode
validation rejecting the number outright. It is not a memory failure, not a BIOS call failing,
and nothing reaches `INT 10h`.

This should have been the first question asked; a lot of inference from silence could have been
skipped.

### Why the detection answer cannot be the whole story

86Box with an EGA configured as "enhanced colour display, primary" presents switch settings 09.
Running QBasic's routine by hand on that input gives capability 0x08 and display type 4 -- which
is **exactly what we produce**, confirmed by breakpoint (`AX=0008` at the store). Yet QBasic
allows SCREEN 7/8/9 there and refuses them here.

Same detection inputs, same computed values, different outcome. So whatever gates the modes is
downstream of the adapter detection entirely, and no amount of tuning the `AH=12h` answer can
reach it. That is consistent with the sweep table above.

### False lead, recorded so it is not chased again

Searching for QBasic's SCREEN-to-BIOS-mode table via the byte sequence `0D 0E 0F 10` hits
0x2EBA7, which looks promising because it sits right beside the mode-set code. It is not a mode
table: it is part of a sequential `00..10` run followed by `2A 00 2A ... 3F ... 15`, the standard
VGA DAC palette. QBasic carries a full VGA palette table near its video code.


## Using a real EGA BIOS instead

86Box with a fully emulated EGA card runs QBasic's `SCREEN 7/8/9` correctly (and refuses 10, which
wants a monochrome display -- consistent with the disassembly, where `BH` from `AH=12h` selects
the mono branch). So a real EGA BIOS demonstrably satisfies whatever we are missing.

Usable ROMs exist in the 86Box repository, e.g. `ibm_6277356_ega_card_u44_27128.bin` and
`108281-001.bin`: 16 KB, valid `55AA` option ROM header, 32 blocks, checksum zero.

**This is not a drop-in.** What it would take:

1. POST has to scan for option ROMs -- C000..C7FF in 2 KB steps, check `55AA`, verify the
   checksum, `call far` to offset 3. `bios.asm` does not do this today. Small piece of work.
2. The BIOS then hooks INT 10h itself and sets up the save pointer table, the INT 1Fh/43h font
   pointers, the BDA fields and the character generator -- all correctly, replacing a pile of our
   hand-rolled approximations. That is the attraction.
3. But it drives the hardware through **real registers**, so the emulation has to be accurate
   enough to be driven by firmware rather than told what to do:
   - **CRTC**: we have a CGA 6845, not an EGA CRTC. The real BIOS programs around 25 registers,
     and the display mode would have to be *derived* from them instead of being handed to us by
     the `AH=0Bh` hook in `helpersEntry`.
   - **Sequencer**: clocking mode (8/9 dot, dot clock divided by two) and memory mode (odd/even,
     chain) become live rather than stored and ignored.
   - **Attribute Controller**: all 20 registers, including mode control.
4. **Text modes would go through the EGA as well**, and this is the big one. A real EGA BIOS sets
   modes 0..3 on the EGA: characters in plane 0, attributes in plane 1, and the font loaded into
   plane 2, with odd/even addressing. The entire CGA text path at 0xB8000 gets bypassed, so we
   would need a full EGA text renderer reading glyphs out of plane 2. That is exactly the
   compatibility machinery this project deliberately sidestepped by keeping the CGA path intact.

So it is a rewrite of the video subsystem, and it forces solving the EGA text problem that was
avoided on purpose at the start.

**Licensing:** these ROMs are IBM copyright 1984 and cannot be committed, the same constraint as
the DOS disk images. They would have to be user supplied on the SD card or in SPIFFS, with a
configuration option pointing at one.

**Memory:** no internal RAM cost. 16 KB at guest 0xC0000 sits in the UMA hole, which is already
part of the 1 MB of PSRAM.

**Order of work, if it is wanted:** find the actual gate first. If it turns out to be one more
INT 10h call or BDA field, it is cheap to add and the rewrite is unnecessary. Doing the rewrite
without knowing the cause risks a large change that does not even fix the symptom.


## Is this worth more effort?

The thing other software would plausibly share is the `AH=1Ah` short-circuit, and that is already
found and fixed. Whatever still gates QBasic may be specific to its own interpreter, in which case
this is a rabbit hole rather than a class of bug. Worth weighing before spending more on it.
