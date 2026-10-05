#!/usr/bin/env python3
"""
Grab a screenshot from a running ESPPC machine over the serial debug console.

The monitor's "p" command writes a header line and then the raw framebuffer. This attaches,
drops into the monitor (which freezes the guest, so the frame is stable), asks for the dump,
decodes it and writes a PNG.

    tools/screenshot.py out.png

Requires the machine to have "enabledebug 1" in mconfs.txt.
"""

import glob
import re
import struct
import sys
import time
import zlib

import serial


# An attribute picks a palette register, and the emulator renders that register's 6 bit value the
# way the monitor would (see GraphicsAdapter::setupEGALUT): in the 200 line modes bits 2-0 are
# RGB and bit 4 intensity, in the 350 line modes it is rgbRGB, two thirds for the capitals and
# one third for the rest. Decode the same way, or a dithered picture comes out looking like noise.
def palette_entry(v, h=350):
    if h <= 200:
        return CGA16[(v & 0x07) | ((v >> 1) & 0x08)]
    lvl = (0, 85, 170, 255)
    return (lvl[((v >> 1) & 2) | ((v >> 5) & 1)],
            lvl[(v & 2) | ((v >> 4) & 1)],
            lvl[((v << 1) & 2) | ((v >> 3) & 1)])


CGA16 = [(0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170),
         (170, 0, 0), (170, 0, 170), (170, 85, 0), (170, 170, 170),
         (85, 85, 85), (85, 85, 255), (85, 255, 85), (85, 255, 255),
         (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255)]


def write_png(path, width, height, rows_rgb):
    raw = b''.join(b'\x00' + bytes(r) for r in rows_rgb)

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


def decode_ega(data, w, h, stride, palette):
    rows = []
    for y in range(h):
        row = bytearray()
        base = y * stride * 4
        for x in range(stride):
            p0, p1, p2, p3 = data[base + x * 4: base + x * 4 + 4]
            for k in range(8):
                bit = 7 - k
                idx = (((p0 >> bit) & 1)
                       | (((p1 >> bit) & 1) << 1)
                       | (((p2 >> bit) & 1) << 2)
                       | (((p3 >> bit) & 1) << 3))
                row += bytes(palette[idx])
        rows.append(row)
    return rows


def decode_cga(data, w, h, fmt):
    # CGA graphics memory interleaves even and odd scanlines 0x2000 apart
    rows = []
    for y in range(h):
        row = bytearray()
        base = (y & 1) * 0x2000 + (y >> 1) * (w // (4 if fmt == 'cga4' else 8))
        if fmt == 'cga4':
            for x in range(w // 4):
                b = data[base + x]
                for k in range(4):
                    idx = (b >> (6 - k * 2)) & 3
                    row += bytes(CGA16[[0, 11, 13, 15][idx]])
        else:
            for x in range(w // 8):
                b = data[base + x]
                for k in range(8):
                    row += bytes(CGA16[15] if (b >> (7 - k)) & 1 else CGA16[0])
        rows.append(row)
    return rows


def decode_tandy(data, w, h, fmt, pal):
    # Tandy 16 colours: two pixels a byte, high nibble first, through the palette registers
    # (4 bit IRGB, a CGA colour). Mode 9 spreads a character row's four lines over four 8 KB banks,
    # mode 8 its two lines over two, and every mode 8 pixel is shown twice as wide.
    rows_per_char, stride = (4, 160) if fmt == 'tandy9' else (2, 80)
    rows = []
    for y in range(h):
        row = bytearray()
        base = (y % rows_per_char) * 0x2000 + (y // rows_per_char) * stride
        for x in range(stride):
            b = data[base + x]
            for nib in (b >> 4, b & 15):
                rgb = bytes(CGA16[pal[nib] & 15])
                row += rgb if fmt == 'tandy9' else rgb * 2
        rows.append(row)
    return rows


def decode_cplus(data, w, h, fmt, colreg, modereg):
    # Plantronics ColorPlus: plane 0 at offset 0, plane 1 at 0x4000, each in the CGA line layout.
    # 320x200x16 takes 2 bits a pixel from each plane, CGA colour (v & 7) << 1 | v >> 3; 640x200x4
    # takes 1 bit from each and goes through the CGA mode 4/5 palette.
    if fmt == 'cplus4':
        intense = 8 if colreg & 0x10 else 0
        if modereg & 0x04:
            three = [3, 4, 7]
        elif colreg & 0x20:
            three = [3, 5, 7]
        else:
            three = [2, 4, 6]
        pal4 = [colreg & 15] + [c + intense for c in three]
    rows = []
    for y in range(h):
        row = bytearray()
        base = (y & 1) * 0x2000 + (y >> 1) * 80
        for x in range(80):
            a, b = data[base + x], data[0x4000 + base + x]
            if fmt == 'cplus16':
                for k in range(4):
                    v = ((a >> (6 - 2 * k)) & 3) | (((b >> (6 - 2 * k)) & 3) << 2)
                    row += bytes(CGA16[((v & 7) << 1) | (v >> 3)])
            else:
                for k in range(8):
                    v = ((a >> (7 - k)) & 1) | (((b >> (7 - k)) & 1) << 1)
                    row += bytes(CGA16[pal4[v]])
        rows.append(row)
    return rows


# CGA composite artifact colours, as in GraphicsAdapter (the Wikipedia article's pictures):
# 640x200 dots, 320x200 palette 0, 320x200 palette 1; indexed by a nibble, leftmost dot high
COMPOSITE = [
    [0x000000, 0x006e31, 0x3109ff, 0x008aff, 0xa70031, 0x767676, 0xec11ff, 0xbb92ff,
     0x315a00, 0x00db00, 0x767676, 0x45f7bb, 0xec6300, 0xbbe400, 0xff7fbb, 0xffffff],
    [0x000000, 0x0071d1, 0x0019ac, 0x0071f1, 0x954f00, 0x6dd441, 0xa27b1c, 0x74d461,
     0xb82100, 0x90a69c, 0xc54e76, 0x97a6bb, 0xf36800, 0xcbed26, 0xff9501, 0xd2ed46],
    [0x000000, 0x009aff, 0x0042ff, 0x0090ff, 0xaa4c00, 0x84fad2, 0xb9a2ad, 0x96f0ff,
     0xcd1f00, 0xa7cdff, 0xdc75ff, 0xb9c3ff, 0xff5c00, 0xedffcc, 0xffb2a6, 0xffffff],
]


def decode_composite(data, table):
    # every nibble of the CGA memory is one colour, two pixels wide at 320
    rows = []
    for y in range(200):
        row = bytearray()
        base = (y & 1) * 0x2000 + (y >> 1) * 80
        for x in range(80):
            b = data[base + x]
            for nib in (b >> 4, b & 15):
                c = COMPOSITE[table][nib]
                row += bytes((c >> 16, (c >> 8) & 255, c & 255)) * 2
        rows.append(row)
    return rows


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'screenshot.png'
    port = sorted(glob.glob('/dev/ttyUSB*'))[0]
    s = serial.Serial(port, 115200, timeout=1.0)
    time.sleep(0.3)
    s.reset_input_buffer()

    # attach, then into the monitor (which freezes the guest)
    s.write(b'\x1b\x1b\x1b')
    s.flush()
    time.sleep(2.0)
    s.reset_input_buffer()
    s.write(b'\x03\x04')
    s.flush()
    time.sleep(2.0)
    s.reset_input_buffer()

    s.write(b'p\r')
    s.flush()

    # find the header line
    header = b''
    deadline = time.time() + 15
    while time.time() < deadline:
        header += s.read(1)
        if header.endswith(b'\n') and b'SCREENSHOT ' in header:
            break
    m = re.search(rb'SCREENSHOT fmt=(\w+) w=(\d+) h=(\d+) stride=(\d+) bytes=(\d+) pal=([0-9a-f]*)',
                  header)
    if not m:
        print('no screenshot header; guest may be in a text mode')
        print(header.decode('utf-8', 'replace')[-300:])
        s.write(b'g\r')
        s.flush()
        return 1

    fmt = m.group(1).decode()
    w, h, stride, nbytes = (int(m.group(i)) for i in (2, 3, 4, 5))
    palhex = m.group(6).decode()
    print(f'{fmt} {w}x{h} stride={stride} {nbytes} bytes')

    data = b''
    deadline = time.time() + 60
    while len(data) < nbytes and time.time() < deadline:
        data += s.read(nbytes - len(data))
    if len(data) < nbytes:
        print(f'short read: {len(data)} of {nbytes}')
        return 1

    if fmt == 'ega':
        pal = [palette_entry(int(palhex[i * 2:i * 2 + 2], 16), h) for i in range(16)] if palhex \
              else CGA16
        rows = decode_ega(data, w, h, stride, pal)
    elif fmt == 'comp':
        rows = decode_composite(data, int(palhex[0:2], 16))
    elif fmt.startswith('cplus'):
        rows = decode_cplus(data, w, h, fmt, int(palhex[0:2], 16), int(palhex[2:4], 16))
    elif fmt.startswith('tandy'):
        pal = [int(palhex[i * 2:i * 2 + 2], 16) for i in range(16)]
        rows = decode_tandy(data, w, h, fmt, pal)
        w = 320
    else:
        rows = decode_cga(data, w, h, fmt)

    write_png(out, w, h, rows)
    print('wrote', out)

    s.write(b'\r')
    s.flush()
    time.sleep(0.5)
    s.write(b'g\r')      # let the guest run again
    s.flush()
    return 0


if __name__ == '__main__':
    sys.exit(main())
