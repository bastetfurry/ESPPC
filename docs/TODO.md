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
