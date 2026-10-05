# SPIFFS payload

Everything in **`data/fs/`** is packed into the on-flash `spiffs` partition (2,293,760 bytes at
0x1D0000) and flashed with:

```sh
pio run -t uploadfs
```

That is independent of the firmware: `pio run -t upload` does not touch it, and flashing this
does not touch the firmware.

This file is deliberately one level above `data/fs/` so it is not packed into the image:
PlatformIO packs `data_dir` wholesale and offers no exclude filter, so `data_dir` points at the
subdirectory instead (see `platformio.ini`).

## The EGA BIOS

`ega.bin` is IBM's EGA BIOS, the option ROM of the IBM Enhanced Graphics Adapter
(`ibm_6277356_ega_card_u44_27128.bin` in the 86Box ROM set, 16 KB, SHA-1
`3af56105530366669a8905c14439619bf6b76b25`). Machines set to `video ega` load it from
`/spiffs/ega.bin` unless their `egarom` tag names another file. See
[docs/ega-bios.md](../docs/ega-bios.md) for how it is used.

The copyright is IBM's. It is included here, as emulators such as 86Box have done for years,
because the EGA machines cannot run without it.

## The rescue disk

Drop a **1.44 MB HD floppy image** into `data/fs/`, for example `rescue.img` (exactly 1,474,560 bytes), then
reference it from a machine configuration by absolute path:

```
desc "Rescue" fd0 "/spiffs/rescue.img" boot fd0
```

1.44 MB is the largest standard floppy format that fits, and `Machine::autoDetectDriveGeometry()`
recognises it directly as 80 cylinders / 2 heads / 18 sectors — no `chs` override needed. Build one
with 86Box, DOSBox, or on Linux:

```sh
dd if=/dev/zero of=rescue.img bs=1024 count=1440
mkfs.fat -F 12 -n RESCUE rescue.img      # then make it bootable and copy files in
```

720 KB DD images work equally well if you prefer them.

## Size budget — measured, not estimated

SPIFFS yields only about **70%** of its partition to a single file, far less than its raw size
suggests. Measured with the real `mkspiffs` (`-p 256 -b 4096`):

| spiffs partition | 720 KB image | 1.44 MB image |
|------------------|--------------|---------------|
| 983,040 (the old 960 KB partition) | fails above ~690 KB | no |
| 2,031,616 | yes | no |
| **2,293,760 (current)** | **yes** | **yes** |

This is why `app0` is 1.75 MB rather than the stock 3 MB. If you ever grow the firmware past
1.75 MB you must shrink this partition to match, and a 1.44 MB image will stop fitting.

## Images here are read-only

`Machine::setDriveImage()` opens anything under `/spiffs/` with mode `"rb"`. SPIFFS is log
structured, so random writes into a large file are slow and wear the flash. The guest will see
write failures on this drive, like a write-protected floppy — which is what a rescue disk
should be. Keep writable disks on the SD card.
