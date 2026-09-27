# To do

Ideas and known gaps that are not being worked on yet. Details for the Tandy 1000 are in
[tandy.md](tandy.md), under "Status and to do".

## Virtual drive LED

The Olimex ESP32-SBC-FabGL has no free LED for disk activity, so draw one on screen instead: a
3x3 pixel square in the lower right corner that lights up green while disk I/O is happening.

- A host setting on the Advanced Settings page (kept in Preferences like the NTP server and the
  time zone), off by default.
- "I/O" means any floppy or hard disk transfer: the BIOS disk handler (INT 13h through
  BIOS::diskHandler_*) and the XTIDE ports. Keep it lit for a short moment after each access
  (some tens of ms) so single sector reads are visible.
- Every video mode needs it, so it probably belongs in the scanline path: after the mode's
  renderer has filled the last three visible lines, overwrite three raw pixels at the right edge.
  That is IRAM (about 2.6 KB free), so keep it to a flag test per scanline and only do work on the
  last three lines. The viewport size differs per mode (320, 640 or 720 wide, 200 to 400 lines).
- It must not end up in the guest's video memory or in the monitor's screenshots.
