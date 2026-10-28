# To do

Ideas and known gaps that are not being worked on yet. Details for the Tandy 1000 are in
[tandy.md](tandy.md), under "Status and to do".

## A memory check for the SysReq menu that knows the video mode

Opening the menu switches to 640x480 text. With too little internal RAM for that, FabGL hangs
instead of failing (seen with the AdLib before its chip moved to PSRAM). A fixed threshold does not
work: from a 640 wide mode (text, EGA 640) the menu only needs its cell buffer and font, about 8 KB,
while from a 320 wide mode it also needs the 640x480 timing's line buffers and DMA descriptors,
roughly 14 KB more (37.6 KB free in SQ3's mode 0Dh against 23.6 KB at the DOS prompt, and the menu
opened in both with 16 KB to spare). A proper check would estimate the need from the current mode
and refuse with a log line rather than hang. The monitor's `v` line is only a rough guide so far.

## Machine repositories: what is left

Repositories are done and merged, see [repositories.md](repositories.md). Still open:

- **Untested on the board:** a list with more than 20 lines, a missing `files.list`, an image
  without Content-Length (the "floppy or hard disk?" question), Esc during the list fetch and
  during a download, the "Try Again / Machine List" question after a failed download, and a floppy
  image from a repository.
- **Paging:** a repository with more than 20 machines is only reachable by typing the page URL
  (`.../2`). A key to go to the next page would be nicer.
- **Faster downloads:** about 180 KB/s now. The next limit is the lwIP and Wi-Fi buffer sizes the
  Arduino framework is compiled with (TCP window 5760), which means building those libraries
  ourselves.
- Seen once, not reproduced: a LoadProhibited in lwIP's `poll_tcp` (tcp_slowtmr) during a
  download, before the Wi-Fi power fix.

## The 16550 UART, waiting for another modem

On branch `serial-16550` (76023a9, not merged): the emulated UART as a 16550A with a 2 KB receive
ring, THRE pacing from the host UART, traffic counters (monitor `n` and the SysReq menu), and the
IRAM margin check in the build. The ESP8266 softmodem on U1 showed framing errors and lost pings
even at 2400 baud, so the user is getting a different modem before this is tested further. Note:
`main` has moved on since (download task, Wi-Fi power), so the branch needs a rebase or merge.

## A clean line between the emulator and FabGL

Parked 2026-10-04, for porting. The emulator should not reach into FabGL (or the ESP32, or
Arduino) directly but through a platform layer of its own, so that other targets only need that
layer written: an RP2350 board (no Wi-Fi unless added separately), an ESP32-S3 (where FabGL does
not run at all), and an SDL build on the PC for local tests. The plan, its steps and the ground
rules (no speed loss on the ESP32, no behaviour change per step) are in
[platform-split.md](platform-split.md), now with the S3 and SDL as targets. The headless host build
exists since 2026-10-28 (`host/`, `pctool`), on stub headers rather than interfaces.

## Numpad as a joypad, and later a joypad as keys

### Numpad to game port (this board)

A digital two button joystick on the game port, played on the numpad:

- **Directions:** 1 to 9 as on the keypad; 5 is neutral. The state is worked out from the set of
  keys held, not from the last key: left = any of 1/4/7, right = 3/6/9, up = 7/8/9, down = 1/2/3,
  and each axis is `right - left` (`down - up`). So 4 + 6 cancel to centre, 7 + 3 as well, and
  typematic repeats change nothing.
- **Buttons:** numpad 0 is button 1 (primary fire), numpad Enter is button 2.
- **Catching the keys:** at scancode level, in `i8042::tick()` before the set 2 to set 1
  conversion, through a filter callback the machine installs (a vendored FabGL change, record it in
  `VENDORED.md`). Keypad keys arrive without the `E0` prefix (`70 69 72 7A 6B 73 74 6C 75 7D`,
  break `F0 xx`); the grey arrow keys and numpad Enter carry `E0` (Enter is `E0 5A`). So the grey
  arrows stay arrows. Caught keys never reach the guest; the fake shift codes some keyboards wrap
  around keypad keys pass through untouched. Whether NumLock is on does not matter.
- **Switching it on:** a per machine option (`numpadjoy` in `mconfs.txt`, a checkbox in the
  editor), plus a row in the SysReq menu to toggle it for the session, like "Monitor". Games that
  want the numpad for numbers are then only one toggle away.
- **Game port at 201h:** nothing answers there today. A write starts the four one-shots, a read
  gives the timer bits 0-3 (X1, Y1, X2, Y2) and the buttons in bits 4-7, active low. A digital
  stick puts each axis at minimum, centre or maximum, so the one-shot lengths are roughly 25 us,
  about 575 us (a 50k pot) and about 1.1 ms. They must run on the clock the guest sees (the PIT's
  time base), not host time, or games that count loop iterations will calibrate differently at
  every emulator speed. The second stick's axes read as not connected: with no pot the one-shot never
  ends, so those bits stay 1 and software times out and sees one stick. Its buttons stay released.
- Optional: INT 15h AH=84h (read joystick) in `tools/bios.asm`. The original XT BIOS has no such
  call and most games read 201h directly, so this can wait.
- **Test:** a game port test program (CheckIt has one, and so do many games' setup screens), then
  a game that calibrates at start and one that does not.

### Joypad to keys (later boards, when there are pins to spare)

The other way round: a physical pad (DB9 digital, or a SNES/NES style shift register pad, which
needs only three pins) mapped to keyboard keys, for games with no joystick support.

- The mapping is per machine: each pad input to a key, stored as set 2 scancodes in `mconfs.txt`
  (arrows and Space/Ctrl/Alt as the default).
- The keys have to go into the **8042 scancode stream**, not into the BIOS buffer through
  `BIOS::injectKey()`, because games that hook INT 9 or read port 60h never see the latter. That
  wants an injection path in `i8042` next to the filter above (the serial console's key injection
  has the same limit and could move to it).
- A pad on such a board could just as well drive the game port directly; the per machine option
  then chooses between game port and keys.
- Belongs to the port, see [platform-split.md](platform-split.md).

## After publishing

Published on GitHub 2026-10-05 (v0.4), with the release workflow and the browser installer.

- **README:** still missing speed figures beyond Topbench, and screenshots (`pctool shot=` can
  make them now, from the emulator's own renderers).
- **Tests in the build.** One script that builds the firmware and runs the SingleStepTests harness
  (`tools/cputest/run.py`) and a few `pctool` boots (DOS from an image, a text screen to compare),
  usable as a pre-commit hook and in the GitHub workflow. Today the workflow only builds.
- **Rescue disk** on FreeDOS instead of retail DOS, so it can be committed (see `.gitignore`).
  The maintainer is building it in 86Box.

## CGA: what is left

After the CGA work of 2026-10-28 (retrace timing, start address, border, the 6845's text geometry),
measured against the CGA Compatibility Tester:

- **Colour changes in mid frame** (raster bars, the tester's horizontal retrace test): left out on
  purpose. California Games is the one known user, and only at exactly 4.77 MHz. It would need the
  3D9h writes recorded per line from the raster model, and IRAM for the lookup in the renderers.
- **Wider than 640 pixels** (the tester's 90x30): cut to 80 columns and 25 rows; it would need a
  720 wide output timing.
- **Side borders:** the border shows above and below the picture only, because our modes fill the
  width of their output timing.
- **The EGA's overscan register** (attribute controller 11h): the EGA's border stays black.
- **IRAM** is down to about 350 bytes; the next renderer change has to find its own room.

## A speed limit for the guest

At Topbench 17 the emulator is a turbo XT, and early games that time themselves by the CPU run too
fast. A per machine setting (full speed, or about 4.77 MHz) that paces instruction batches against
the emulated PIT clock, not host time. The planned game port needs that same clock for its
one-shots. (Games that time themselves by the CGA's vertical retrace already run at the right
speed, since the status register follows a 59.92 Hz raster.)

## Save states

For long games without a save option: from the SysReq menu, write guest RAM (1 MB from PSRAM) and
every device's state (CPU, PIC, PIT, 8042, RTC, video adapter and its memory, EGA planes, sound
chips, DAC, floppy/disk state) to the SD card, and restore it. The work is in the devices: each one
needs a versioned save and load of its state, and the vendored FabGL ones have none. Possibly one
for the port (see [platform-split.md](platform-split.md)), where the device code gets restructured
anyway.
