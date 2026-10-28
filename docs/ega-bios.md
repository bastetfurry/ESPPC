# Running a real EGA BIOS

A second video path, selected per machine. The CGA adapter stays the default and is untouched; `video ega` loads a real EGA BIOS option ROM at C000:0000 and lets it drive the
hardware, the way a real card does.

Chosen in the machine editor with the **Video** dropdown, or in `mconfs.txt`:

    video ega                       use the EGA path
    egarom "/SD/other-ega.bin"      optional, defaults to /spiffs/ega.bin

The ROM shipped in `data/fs/ega.bin` is the IBM EGA BIOS (`ibm_6277356_ega_card_u44_27128.bin`
from the 86Box ROM set): 16 KB, valid `55AA` header, 32 blocks, checksum zero.


## Status: the ROM initialises, and the machine boots

The approach is deliberately "fake it": the ROM is real where it is *looked at*, and ours where it
is *used*. POST calls the ROM's init, the init hooks INT 10h to its own handler, and the emulator
has already rewritten the first instruction of that handler into a far jump back into this BIOS.
The ROM therefore never drives hardware that is not emulated, while everything that inspects the
machine -- the option ROM signature, IVT[10h] pointing into C000, the save pointer table, the
font pointers -- sees a genuine EGA BIOS.

What a boot now looks like:

    EGA BIOS: loaded /spiffs/ega.bin, 32 blocks (16384 bytes), checksum ok
    EGA BIOS: planar memory ready
    EGA BIOS: INT 10h handler C000:0CD7 redirected to F000:0455
    ROM<  VID07  VID03  VID01  >

The init calls INT 10h twice to set its initial mode; both land in this BIOS (VID07, VID03) and
it returns. The redirect target is wherever the handler sits in this BIOS, so it moves when
bios.asm changes.

The ROM's self test passes. It used to beep one long and three short, the IBM code for an EGA
adapter failure: the planes were reallocated on every mode change, the heap fragmented, and the
ROM's test of mode 0Eh fell back to CGA. Since 2026-09-26 the planes are allocated once, while the
heap is still whole, and kept.

State it leaves behind, read out of the running machine:

| field | value |
|-------|-------|
| IVT[10h] | C000:0CD7 (redirected on entry into this BIOS, F000:0455 today) |
| IVT[1Fh] | C000:3560, graphics font |
| 40:A8 | C000:010C, save pointer table |
| 40:87 / 88 / 89 / 8A | 04 / 09 / 81 / 04 |

**It did not fix QBasic on its own.** With a real EGA BIOS present, hooked, and having populated
all of the above, QBasic still refused SCREEN 7..10 with error 5, so the gate was not "is a real
EGA BIOS installed", which was the hypothesis behind adopting one. The gate turned out to be this
BIOS answering INT 10h AH=1Bh, a VGA call; see [qbasic-ega.md](qbasic-ega.md).


## Where the ROM's own code stops, if it is ever let loose

Working end to end:

- `Machine::loadEGABios()` reads the ROM into guest memory at C000:0000 and verifies the option
  ROM header and checksum in C++ before the guest ever sees it.
- The planar video memory is allocated **before** POST, because the ROM programs and tests the
  hardware during its own init. The CGA frame buffer is released at that point and
  `setCGAMode()` stands down.
- POST scans C000..C780 in 2 KB steps for `55AA` and far-calls the init entry at offset 3.
  Markers `ROM<` and `>` are printed either side, so a ROM that hangs inside its own
  initialisation can be told from one that never ran.
- The init **runs real code**: it resets the attribute controller flip-flop through 3DA/3BA,
  hooks INT 10h at IVT[10h] to its own handler at C000:0CD7, chains the old vector to INT 42h,
  installs the save pointer table at 40:A8, sets the INT 1Fh graphics font pointer, writes
  40:87, and reads the configuration switches.
- Input Status 0 (port 0x3C2 read) implements the switch sense: the BIOS selects a switch with
  bits 3-2 of the last Misc Output write and reads the answer in bit 4.

Where it stops: `C000:008B` calls `C000:00F3`, which masks the switch value to four bits and
dispatches through a **jump table at C000:0128**:

| switch | target |   | switch | target |
|--------|--------|---|--------|--------|
| 0 | 0173 | | 8 | 01C7 |
| 1 | 017E | | 9 | 01D2 |
| 2 | 017E | | A | 01DD |
| 3 | 0189 | | B | 01F1 |
| 4 | 0194 | | C | 0204 |
| 5 | 01A8 | | D | 0204 |
| 6 | 01BC | | E | 0204 |
| 7 | 01C7 | | F | 0204 |

Control never returns from that call when the ROM's own INT 10h is left in place. Those routines program the CRTC, Sequencer and Attribute
Controller for the detected display, and the emulation does not implement them: 3D4/3D5 is still
a CGA 6845, the Sequencer's clocking and memory mode registers are stored and ignored, and the
Attribute Controller only implements the palette. The machine boot loops as a result.


## Notes from getting this far

**The switch sense is not inverted** relative to the value to be reported. Feeding back the
complement made the BIOS read 0x06 out of a 0x09 switch block and dispatch through the wrong
table entry. `m_EGASwitches` holds the block directly.

**Do not checksum the ROM in the guest.** POST originally summed the 16 KB image the way a real
BIOS does. At roughly 380 iterations per second of emulated loop that takes about six minutes per
boot. The C++ side already verifies the checksum before placing the ROM, so the guest would only
be re-deriving a result it was handed; the signature check is enough there.

**The banner is not the end of POST.** `SerialDebug::begin()` prints from `Machine::init()`,
which runs *before* the guest executes anything. A capture that stops at the banner sees nothing
of POST.

**A boot loop is easy to debug here**, because POST runs over and over: attach whenever, arm a
breakpoint, and the next pass through hits it. That is how the failure was bisected to a single
`call`.


## What is needed next

A real EGA register file, so firmware can drive the hardware instead of being told what to do:

1. **CRTC** as an EGA CRTC rather than a CGA 6845, with the display mode *derived* from the
   register values instead of handed over by the `AH=0Bh` helper.
2. **Sequencer** clocking mode and memory mode made live, not just stored.
3. **Attribute Controller** in full: mode control, overscan, colour plane enable, pixel panning.
4. ~~**EGA text rendering**~~, done: the text modes are drawn from the planes as the card keeps
   them, characters in plane 0, attributes in plane 1 and the font in plane 2
   (`PC_Text_EGA_80x25_16Colors`), with the 43 line mode and the split screen.

The CGA path is unaffected throughout and remains the default.
