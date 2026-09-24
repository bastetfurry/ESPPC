#!/usr/bin/env python3
"""
Grab a screenshot from a running PCEmulator machine over the serial debug console.

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


# An attribute picks a palette register, and the emulator renders that register's value through
# EGADefaultPalette[value & 0x0f] (see GraphicsAdapter::setupEGALUT). Decode the same way, or a
# dithered picture comes out looking like noise.
def palette_entry(v):
    return CGA16[v & 0x0f]


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


def decode_hgc(data, w, h):
    rows = []
    for y in range(h):
        row = bytearray()
        base = (y & 3) * 0x2000 + (y >> 2) * (w // 8)
        for x in range(w // 8):
            b = data[base + x]
            for k in range(8):
                row += bytes(CGA16[15] if (b >> (7 - k)) & 1 else CGA16[0])
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
        pal = [palette_entry(int(palhex[i * 2:i * 2 + 2], 16)) for i in range(16)] if palhex \
              else CGA16
        rows = decode_ega(data, w, h, stride, pal)
    elif fmt == 'hgc':
        rows = decode_hgc(data, w, h)
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
