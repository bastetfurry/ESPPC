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

Replaces the hardcoded machine list (`DefaultConfFile` in `src/mconf.h`, 13 fabglib.org entries
written into a fresh `mconfs.txt`) with a repository the user points the board at.

### The repository format

A repository is a URL, for example `http://foo.test/pcimages/1`. The board fetches
`<URL>/files.list` from it (a trailing `/` is tolerated, and a URL already ending in `files.list`
is used as is). The list is CSV, one machine per line, **at most 20 machines**:

```
"Name", "Full-Imagefile-URL", "Memory in KB", "Machine-Type"
"FreeDOS 1.3", "http://foo.test/pcimages/img/freedos.img", "640", "CGA Adlib"
"Tandy DeskMate", "http://foo.test/pcimages/img/deskmate.img", "640", "Tandy TandyDAC"
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

### Open points

- **Name collisions on the SD card.** `getDisk()` saves a download under the URL's base name in the
  SD root and reuses an existing file of that name, so `freedos.img` from two repositories would
  silently share one image. Either save under a per-host folder, or warn when adding.
- **HTTPS.** Whether `HTTPClient::begin(url)` with an `https://` URL connects at all on this
  framework without a CA bundle or `setInsecure()`, and whether there is internal RAM for TLS
  (tens of KB) at that point in setup. Plain HTTP works; HTTPS needs a test.
- Whether relative image URLs (relative to the list) are worth allowing. The spec says full URLs.

### Testing

A local repository first: a folder with `files.list` and a few small images, served from the PC
with `python3 -m http.server` (which answers HTTP/1.0, like the board asks). Cover: a valid list,
more than 20 lines, a header line, bad memory and type values, quoting, a missing `files.list`, an
image without Content-Length, Esc during the list fetch, and a floppy and a hard disk image.
