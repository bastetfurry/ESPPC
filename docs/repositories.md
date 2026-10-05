# Machine repositories

A repository lets the board add ready made machines from a web server: **F6 : Add from
Repository** on the machine list asks for its URL, shows the machines it offers, and adds the
chosen ones to `mconfs.txt`. Their disk images are downloaded on each machine's first boot.

The first part of this page is for whoever offers a repository; the rest describes how the board
handles it. Merged 2026-10-04 (9db4ea9). Open points are in [TODO.md](TODO.md).


## Offering a repository

A repository is a URL, for example `https://example.org/pcimages`. The board fetches
`<URL>/files.list` from it. A trailing `/` is fine, and a URL that already ends in `files.list` is
used as it is. Plain http and https both work, and so do redirects between them.

`files.list` is CSV, one machine per line, **at most 20 machines**:

```
"Name", "Full-Imagefile-URL", "Memory in KB", "Machine-Type", "Explanation"
"FreeDOS 1.4", "https://example.org/pcimages/FreeDOS_XT.img", "640", "EGA Adlib", "FreeDOS only"
"Tandy DeskMate", "https://example.org/pcimages/deskmate.img", "640", "Tandy TandyDAC", "DeskMate in autoboot"
```

- **Name:** what the machine list shows; up to 43 characters are used.
- **Image URL:** a full `http://` or `https://` URL of the disk image, without blanks or quotes.
  The board saves it on the SD card under its file name (the last part of the URL), so give
  different images different file names.
- **Memory:** 64 to 704 KB, in steps of 64.
- **Machine type:** one of `CGA`, `EGA`, `Tandy`, then optionally `Adlib` and `TandyDAC`, in any
  order, case does not matter. `TandyDAC` on CGA or EGA also gets the Tandy sound chip, which the
  DAC needs. Unknown words reject the line, so a typo does not quietly give a different machine.
- **Explanation:** optional, shown under the list when the line is selected, so users know what to
  expect from the image. Up to 78 characters.

Fields may be quoted; `""` inside quotes is a literal quote. Blank lines and lines starting with
`#` are skipped, and a first line whose name is `Name` is taken as a header. A repository with more
than 20 machines splits itself into numbered pages (`.../1`, `.../2`); lines after the 20th are
ignored, and the list says so.

**Floppy or hard disk** follows from the image size: a standard floppy size (160K, 180K, 320K,
360K, 720K, 1.2M, 1.44M, 2.88M) becomes drive A:, anything else the hard disk C:, booted from.
The server should answer HEAD requests with a Content-Length, otherwise the user is asked. Range
requests let interrupted downloads resume; Apache and most servers do them for static files.

**Hard disk geometry** comes from the image's partition table, so an image partitioned in another
emulator or on real hardware boots as it is (86Box's 963/5/17 MFM disk does). A disk without a
partition table gets a geometry worked out from its size.


## On the board

- The repository URL is asked with the usual text input, prefilled with the last one used (kept in
  Preferences). Enter adds the selected machine, and the list stays open to add more. Lines that
  cannot be used are shown greyed out with the reason.
- A machine whose image URL is already in `mconfs.txt` asks before it is added again, and so does
  one whose image would share a file name on the SD card with another machine's image.
- A fresh `mconfs.txt` starts empty, with a hint pointing at F6. The old built-in list of
  fabglib.org images is gone; existing `mconfs.txt` files are left alone.
- Code: `src/repository.cpp` (fetching, the screen), `src/repolist.cpp` (the parser, with host
  tests in `tools/repotest/`), `src/httpopen.cpp` (requests that follow redirects across http and
  https, which HTTPClient refuses).


## Downloads

A 40 MB image over https showed every weakness the image download had, so `downloadURL()` in
`src/main.cpp` was reworked:

- **Resumable.** The data goes to `<name>.part`, which a failed attempt keeps. It is continued
  with a Range request, on the spot after a dropped connection or at the next boot, and renamed
  when complete. The part is synced every 512 KB, since FAT only records a file's length on close
  or sync.
- **No restart loop.** A download that gives up asks "Try Again / Machine List"; the list comes up
  after the restart instead of the same download with Auto Boot.
- **Progress.** MB, KB/s, time left, Wi-Fi signal and lost connections in the box, and a line
  every 10 s on the serial console, with the free internal RAM.
- **SD card at 20 MHz from the start.** Below 20 MHz this card wrote 30 times slower (45 against
  1400 KB/s, in any order: `pio run -e sdbench`); FabGL kept 19 MHz until after the downloads.
  Writes go in 4 KB sector aligned blocks from DMA capable internal RAM.
- **On core 0**, next to Wi-Fi: on core 1 the download starved the VGA interrupt into glitches.
- **Wi-Fi at 8.5 dBm instead of 19.5**, for everything Wi-Fi does. At full power the 3.3V rail
  dipped on transmit bursts (board on a USB 3 port): Wi-Fi went deaf every few minutes while still
  reporting itself connected, and the SD card stopped answering mid-transfer until powered off.
  At half power 26 MB ran through without a stall at 180 KB/s, the signal still around -50 dBm. A
  bulk capacitor on the 3.3V rail would be the hardware fix. Stalls are still handled: after 10 s
  without data, or a refused request, the Wi-Fi association is renewed, and drops log their reason.
- **Internal RAM** is tight: TLS takes about 60 KB of the ~98 KB free, and the Wi-Fi driver's
  receive buffers come out of the rest. So the download's own buffers stay small (4 KB block, an
  8 KB task stack of which 2.5 KB stay unused); with 28 KB more it lost packets and stalled.
- **SD card safety.** A reset in the middle of an SD transfer leaves the card stuck until it loses
  power: it keeps its power over a reset of the ESP32 and then answers nothing, not even CMD0
  (reproduced; a restart while idle is harmless). Software restarts flush the guest's disks and
  unmount the card first (the shutdown handler), and the mount at boot tries to talk a stuck card
  round (`src/sdrecover.cpp`). It deliberately does not finish a half sent data block, which would
  write filler into whatever sector was being written. If nothing helps, the POST says to unplug
  the board.
- **HTTPS** works without a CA bundle: HTTPClient falls back to `setInsecure()`, so the transfer is
  encrypted but the server is not authenticated.
- `pio run -e dltest` measures the network without SD card and VGA (edit its URLs first).
