# Why QBasic still will not use the EGA modes

Status: **unresolved**. The EGA itself works -- Planet X3's EGA build runs correctly in mode 0Eh
-- but QBasic 1.1 refuses `SCREEN 7/8/9/10` and never issues `INT 10h AH=00` for modes 0Dh..10h.

This is the state of the investigation so it can be picked up without re-deriving it.


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

**The `AH=12h` answer.** Swept `CL` across all three capability classes (09, 0B, 00, 03, 06) plus
`BL=0` variants. No change in any case.

That sweep is only trustworthy because the probe carries a positive control: `T.BAS` starts with
`SCREEN 1`, which produces `VID04` on the serial line from `bios.asm`'s mode-set debug print. If
the control is absent, "no EGA modes attempted" is indistinguishable from "the probe never ran" --
which did happen, twice.


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


## Is this worth more effort?

The thing other software would plausibly share is the `AH=1Ah` short-circuit, and that is already
found and fixed. Whatever still gates QBasic may be specific to its own interpreter, in which case
this is a rabbit hole rather than a class of bug. Worth weighing before spending more on it.
