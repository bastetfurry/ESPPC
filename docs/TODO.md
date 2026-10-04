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

## Machine repositories: add machines from any URL

**Built on branch `machine-repository`** (F6 on the machine list, `src/repository.cpp`, the parser
in `src/repolist.cpp` with host tests in `tools/repotest/`), as specified below except where the
notes under "Open points" say otherwise. Still to do: the testing list at the end on the board, and
paging through a repository bigger than one page beyond typing the page URL.

Replaces the hardcoded machine list (`DefaultConfFile` in `src/mconf.h`, 13 fabglib.org entries
written into a fresh `mconfs.txt`) with a repository the user points the board at.

### The repository format

A repository is a URL, for example `http://foo.test/pcimages/1`. The board fetches
`<URL>/files.list` from it (a trailing `/` is tolerated, and a URL already ending in `files.list`
is used as is). The list is CSV, one machine per line, **at most 20 machines**:

```
"Name", "Full-Imagefile-URL", "Memory in KB", "Machine-Type", "Explanation"
"FreeDOS 1.3", "http://foo.test/pcimages/img/freedos.img", "640", "CGA Adlib", "FreeDOS only"
"Tandy DeskMate", "http://foo.test/pcimages/img/deskmate.img", "640", "Tandy TandyDAC", "DeskMate in autoboot"
```

- Fields may be quoted; `""` inside quotes is a literal quote. Blank lines and lines starting with
  `#` are skipped. A first line whose name field is literally `Name` is taken as a header and
  skipped, so the example above is valid as written.
- A repository with more machines splits itself into numbered pages (`.../1`, `.../2`, `.../3`);
  the user enters the page they want. Lines after the 20th are ignored, and the list says so.
- Memory: 64 to 704 in steps of 64, as in the machine editor. Anything else rejects that line.
- Machine type: one of `CGA`, `EGA`, `Tandy` (not case sensitive), then optionally `Adlib` and
  `TandyDAC`, in any order. Maps to `videoAdapter` 0 / 1 / 2, `adlib`, `tandyDAC`. `TandyDAC` on
  CGA or EGA also sets `tandySound`, since the DAC needs the sound chip. EGA leaves `egarom` empty,
  so the flash `ega.bin` is used, as it is today. Unknown words reject the line, so a typo does not
  quietly produce a different machine.
- A rejected line shows up in the list greyed out with the reason, rather than disappearing.
- The explanation field is truncated to 78 chars.

### On the board

- Machine selection screen: **F6 : Add from Repository**. It asks for the URL with the existing
  text input (the one behind "Type a Path or URL..."), prefilled with the last URL used, which is
  kept in Preferences next to the other host settings.
- Wi-Fi as for downloads today (`checkWiFi()`), then fetch the list into a PSRAM buffer, capped at
  a few KB, with the same HTTP/1.0 and abort handling as `downloadURL()`.
- Show the up to 20 entries (name, type, memory); Enter adds the selected one to `mconfs.txt`, and
  the list stays open to add more. An entry whose image URL is already in `mconfs.txt` asks first.
- The image itself is **not** downloaded here: the new machine stores the URL, and `getDisk()`
  fetches it on first boot, as it already does for URLs.
- Floppy or hard disk: the CSV does not say, and the entry needs `fd0` or `hd0` (and `boot hd0`)
  before anything is downloaded. Plan: an HTTP HEAD for the image's size when it is added; a
  standard floppy size (160K to 2.88M, the table in `Machine::autoDetectDriveGeometry`) means
  `fd0`, anything else `hd0` with `boot hd0`. No length in the answer means asking the user.
- Drop `DefaultConfFile`'s entries: a fresh `mconfs.txt` starts empty, and an empty machine list
  shows a hint pointing at F6. Existing `mconfs.txt` files are left alone.

### Open points, and how they were settled

- **Name collisions on the SD card.** `getDisk()` saves a download under the URL's base name in the
  SD root and reuses an existing file of that name. Adding a machine whose image would share a file
  with another machine's warns and asks; per host folders were not done, as they would make every
  existing URL machine download again.
- **HTTPS** works without a CA bundle: `HTTPClient::begin()` falls back to `setInsecure()`, so it
  is encrypted but not authenticated. There was internal RAM for it during setup (25-50 KB free
  while downloading).
- **Redirects across protocols** were the surprise: a server that moves http to https answers 301,
  and HTTPClient will not follow a redirect that changes the protocol. `httpOpen()` (src/httpopen.*)
  follows them itself, for the list, the size check and the image download.
- Relative image URLs: not allowed, the spec says full URLs.

### Downloads (getDisk)

Reworked along the way, since a 40 MB image showed every weakness:

- Resumable: the data goes to `<name>.part`, kept when an attempt fails, continued with a Range
  request (on the spot after a dropped connection, or at the next boot) and renamed when complete.
  The part is synced every 512 KB, since FAT only records a file's length on close or sync.
- A failed download no longer restarts into the same machine (an endless loop with Auto Boot); it
  asks "Try Again / Machine List", and the list comes up after the restart.
- Wi-Fi that drops during a download is reconnected between attempts.
- The progress box shows MB, KB/s and the time left; the serial line gets a line every 10 s.
- **The SD card now runs at 20 MHz from the start.** Below 20 MHz this card wrote 30 times slower
  (45 against 1400 KB/s, in any order: `pio run -e sdbench`), and FabGL kept 19 MHz until after the
  downloads. Writes go in 16 KB sector aligned blocks from internal (DMA capable) RAM.
- **Wi-Fi transmits at 8.5 dBm instead of 19.5.** At full power the 3.3V rail dipped on transmit
  bursts (board on a USB 3 port): during downloads Wi-Fi went deaf every few minutes while still
  reporting itself connected, and the SD card stopped answering mid-transfer until powered off.
  At half power 26 MB ran through without a stall at 180 KB/s, with the signal unchanged around
  -50 dBm. A board far from its access point may need more; a bulk capacitor on the 3.3V rail
  would be the hardware fix. Stalls are still handled: after 10 s without data, or a refused
  request, the Wi-Fi association is renewed, and drops log their disconnect reason.
- The free internal RAM matters as well: TLS takes about 60 KB of the ~98 KB free, and the Wi-Fi
  driver's receive buffers come out of the rest, so the download's own buffers stay small (4 KB
  block, 8 KB task stack, 2.5 KB of it unused).
- The network ceiling measured before the power fix (100-170 KB/s, `pio run -e dltest`) was partly
  lost packets; the lwIP and Wi-Fi buffer sizes compiled into the framework (TCP window 5760) are
  the next limit. A 40 MB image takes about 4 minutes.
- Seen once, not reproduced: a LoadProhibited in lwIP's `poll_tcp` (tcp_slowtmr) during a download.
- The download runs in a task on core 0, next to Wi-Fi: on core 1 it starved the VGA interrupt
  into visible glitches.
- **A reset in the middle of an SD transfer leaves the card stuck** until it loses power: it keeps
  its power over a reset of the ESP32, and then answers nothing, not even CMD0 (reproduced with a
  timer restart while writing; a restart while idle is harmless, with or without Wi-Fi). Software
  restarts now flush the guest's disks and unmount the card first (the shutdown handler), and the
  mount at boot tries to talk a stuck card round (`src/sdrecover.cpp`: stop token, CMD12, clocks),
  which worked in most tries. It deliberately does not finish a half sent data block, as that
  would write filler into whatever sector was being written. If nothing helps, the POST says to
  unplug the board.

### Testing

A local repository first: a folder with `files.list` and a few small images, served from the PC
with `python3 -m http.server` (which answers HTTP/1.0, like the board asks). Cover: a valid list,
more than 20 lines, a header line, bad memory and type values, quoting, a missing `files.list`, an
image without Content-Length, Esc during the list fetch, and a floppy and a hard disk image.

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

## Cleanup before publishing

- **README rewrite.** It still describes the FabGL example: "CGA/Hercules", the TTGO VGA32, "press
  Run". Missing: the Olimex ESP32-SBC-FabGL, EGA / Tandy / Plantronics / composite / AdLib / Tandy
  DAC, the setup screens and SysReq menu as they are now, the `mconfs.txt` tags, the debug console
  and postbox, and speed figures. It must say where `ega.bin` comes from: it is not in the repo
  (`*.bin` is ignored) and not redistributable, so without a note EGA fails for everyone else.
- **Tests in the build.** One script that builds the firmware and runs the SingleStepTests harness
  (`tools/cputest/run.py`), usable as a pre-commit hook and later as CI once there is a remote.
- **Rescue disk** on FreeDOS instead of retail DOS, so it can be committed (see `.gitignore`).
  The maintainer is building it in 86Box.
- Then a remote, and publish.

## A speed limit for the guest

At Topbench 17 the emulator is a turbo XT, and early games that time themselves by the CPU run too
fast. A per machine setting (full speed, or about 4.77 MHz) that paces instruction batches against
the emulated PIT clock, not host time. The planned game port needs that same clock for its
one-shots.

## Save states

For long games without a save option: from the SysReq menu, write guest RAM (1 MB from PSRAM) and
every device's state (CPU, PIC, PIT, 8042, RTC, video adapter and its memory, EGA planes, sound
chips, DAC, floppy/disk state) to the SD card, and restore it. The work is in the devices: each one
needs a versioned save and load of its state, and the vendored FabGL ones have none. Possibly one
for the port (see [platform-split.md](platform-split.md)), where the device code gets restructured
anyway.
