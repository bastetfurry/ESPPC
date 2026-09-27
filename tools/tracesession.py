#!/usr/bin/env python3
"""
Record what a guest program does to the video (or any other) hardware, in one serial session.

Opening the port resets the board, so everything happens in a single process: wait for the
machine to boot, attach, arm the monitor's port trace, resume, type the commands that start the
program, let it run, then freeze the guest and save the trace.

    tools/tracesession.py --trace "3d0 3df int 10 1a" \\
        --type 'cd \\sierra\\sq3' --type 'sq' --run 60 --out sq3.trace

Each --type is typed at the guest followed by Enter. --run is how long the program gets before
the trace is read back. Two files come out: the trace (--out) and the whole serial stream next to
it (.log), which also carries the emulator's own messages, such as the BIOS's VIDxx line for every
mode set.

The machine needs "enabledebug 1" in mconfs.txt, and the trace arguments are those of the
monitor's 't' command:  lo hi [r|w|rw] [int lo [hi]].
"""

import argparse
import glob
import re
import sys
import time

import serial


def read_for(s, seconds, log):
    """Read whatever arrives for this long, log it, and return it."""
    data = b''
    end = time.time() + seconds
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            data += chunk
            log.write(chunk)
    return data


def read_until(s, marker, timeout, log):
    data = b''
    end = time.time() + timeout
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            data += chunk
            log.write(chunk)
            if marker in data:
                return data, True
    return data, False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--trace', required=True, help="arguments for the monitor's 't' command")
    ap.add_argument('--type', action='append', default=[], help='a line to type at the guest (repeatable)')
    ap.add_argument('--boot', type=float, default=25.0,
                    help='seconds from the boot banner to a DOS prompt (default 25)')
    ap.add_argument('--run', type=float, default=60.0, help='seconds to let the program run (default 60)')
    ap.add_argument('--out', default='trace.txt')
    ap.add_argument('--attach', action='store_true',
                    help='attach to the machine as it is running instead of resetting it and waiting '
                         'for the boot (then start the program yourself once the trace is armed)')
    args = ap.parse_args()

    ports = sorted(glob.glob('/dev/ttyUSB*'))
    if not ports:
        print('no /dev/ttyUSB*; is the board connected? (lsusb should show 1a86:7523)')
        return 1

    log_path = args.out.rsplit('.', 1)[0] + '.log'
    with open(log_path, 'wb') as log:
        s = serial.Serial(ports[0], 115200, timeout=0.2)
        if args.attach:
            # Opening the port can still reset the board. If a boot banner turns up, it did.
            data, reset = read_until(s, b'machine booted', 4, log)
            if reset or b'rst:' in data:
                print('opening the port reset the board after all; waiting for the boot')
                args.attach = False
                if not reset:
                    read_until(s, b'machine booted', 90, log)
                read_for(s, args.boot, log)
        else:
            # Opening the port usually resets the board, but not reliably when another process has
            # only just closed it, so pulse RTS as well.
            s.rts = True
            time.sleep(0.1)
            s.rts = False
            print('waiting for the machine to boot ...')
            _, ok = read_until(s, b'machine booted', 90, log)
            if not ok:
                print('no boot banner; is "enabledebug 1" set for the machine that boots?')
                return 1
            print(f'booted, giving DOS {args.boot:.0f} s')
            read_for(s, args.boot, log)

        # attach, monitor, arm, resume -- in that order, so nothing typed lands in the wrong state
        s.write(b'\x1b\x1b\x1b'); s.flush(); read_for(s, 2.0, log)
        s.write(b'\x03\x04');     s.flush(); read_for(s, 2.0, log)
        s.write(b't ' + args.trace.encode() + b'\r'); s.flush()
        reply = read_for(s, 1.5, log)
        if b'tracing' not in reply:
            print('the monitor did not arm the trace:')
            print(reply.decode('latin1')[-400:])
            return 1
        s.write(b'g\r'); s.flush(); read_for(s, 2.0, log)

        for line in args.type:
            print('typing:', line)
            for ch in line.encode() + b'\r':
                s.write(bytes([ch])); s.flush()
                time.sleep(0.05)
            read_for(s, 3.0, log)

        if args.attach and not args.type:
            print('ARMED: start the program now')
        print(f'letting it run for {args.run:.0f} s')
        read_for(s, args.run, log)

        s.write(b'\x03\x04'); s.flush(); read_for(s, 2.0, log)
        s.write(b't\r'); s.flush()
        data, ok = read_until(s, b'accesses after the buffer filled', 60, log)
        read_for(s, 0.5, log)
        s.write(b'g\r'); s.flush()

    text = data.decode('latin1').replace('\r', '')
    lines = [l for l in text.split('\n')
             if re.match(r'[0-9A-F]{4}:[0-9A-F]{4}  ', l) or 'accesses after the buffer filled' in l]
    with open(args.out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'wrote {args.out} ({len(lines)} lines) and {log_path}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
