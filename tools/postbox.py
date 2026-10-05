#!/usr/bin/env python3
"""Host side of the ESPPC postbox.

The postbox is a private channel between a program running on the emulated machine and this
script. The guest end is INT 0xFF with AX=0x55EE; this end speaks a small framed protocol over
the same USB serial the debug console uses, so a transfer never takes the debugger away and never
needs a COM port. See the note above class Machine in src/machine.h.

Wire format, host to emulator:   0x10 'P' <cmd> <len> <payload...>
and the answer:                  0x10 'p' <cmd> <len> <payload...>
"""

import argparse, glob, os, sys, time
import serial

DLE = 0x10


class Postbox:
    def __init__(self, port=None, baud=115200, timeout=1.0):
        if port is None:
            ports = sorted(glob.glob('/dev/ttyUSB*')) + sorted(glob.glob('/dev/ttyACM*'))
            if not ports:
                raise SystemExit('no serial port found; pass --port')
            port = ports[0]
        self.port = port
        self.s = serial.Serial(port, baud, timeout=timeout)
        time.sleep(0.3)

    # -- framing -------------------------------------------------------------------------------
    def _send(self, cmd, payload=b''):
        if len(payload) > 255:
            raise ValueError('payload too long for one frame')
        self.s.write(bytes([DLE, ord('P'), ord(cmd), len(payload)]) + payload)
        self.s.flush()

    def _reply(self, cmd, timeout=3.0):
        """Pull the next reply frame out of the stream, skipping console chatter."""
        deadline = time.time() + timeout
        buf = b''
        while time.time() < deadline:
            b = self.s.read(1)
            if not b:
                continue
            buf += b
            i = buf.find(bytes([DLE, ord('p')]))
            if i < 0:
                buf = buf[-1:]
                continue
            while len(buf) < i + 4:
                more = self.s.read(1)
                if not more:
                    break
                buf += more
            if len(buf) < i + 4:
                continue
            rcmd, n = buf[i + 2], buf[i + 3]
            while len(buf) < i + 4 + n:
                more = self.s.read(n - (len(buf) - i - 4))
                if not more:
                    break
                buf += more
            payload = buf[i + 4:i + 4 + n]
            if rcmd == ord(cmd):
                return payload
            buf = buf[i + 4 + n:]
        raise TimeoutError('no reply to %r' % cmd)

    def _call(self, cmd, payload=b''):
        self._send(cmd, payload)
        return self._reply(cmd)

    # -- session -------------------------------------------------------------------------------
    def open(self):
        self._call('O')

    def close(self):
        self._call('C')

    def query(self):
        p = self._call('Q')
        return {'host_open': bool(p[0]), 'waiting_for_guest': p[1], 'waiting_for_host': p[2]}

    # -- bytes ---------------------------------------------------------------------------------
    def write(self, data, timeout=30.0):
        """Push bytes to the guest, respecting how many the emulator actually took."""
        sent = 0
        deadline = time.time() + timeout
        while sent < len(data):
            chunk = data[sent:sent + 200]
            took = self._call('S', chunk)[0]
            sent += took
            if took < len(chunk):
                if time.time() > deadline:
                    raise TimeoutError('guest is not draining the postbox')
                time.sleep(0.02)
            else:
                deadline = time.time() + timeout
        return sent

    def read(self, want=200):
        return self._call('R', bytes([min(want, 200)]))


    # -- protocol with POSTBOX.COM --------------------------------------------------------------
    def expect(self, wanted, timeout=20.0):
        """Wait for one specific byte from the guest, returning any error detail with it."""
        deadline = time.time() + timeout
        seen = b''
        while time.time() < deadline:
            seen += self.read(200)
            if not seen:
                time.sleep(0.05)
                continue
            for i, b in enumerate(seen):
                if b == ord(wanted):
                    return seen[i + 1:]
                if b == ord('E'):
                    tail = seen[i + 1:]
                    while not tail and time.time() < deadline:
                        tail = self.read(4)
                    code = tail[0] if tail else 0
                    raise RuntimeError('guest reported DOS error %d (0x%02X)' % (code, code))
            time.sleep(0.05)
        raise TimeoutError('guest never sent %r (saw %r)' % (wanted, seen))

    def wait_ready(self, timeout=30.0):
        """POSTBOX.COM answers R once it has seen our session open."""
        return self.expect('R', timeout)

    def send_file(self, data, dosname, timeout=300.0):
        if len(dosname) > 79:
            raise ValueError('DOS filename too long')
        hdr = bytes([ord('F'), len(dosname)]) + dosname.encode('ascii') + \
              len(data).to_bytes(4, 'little')
        self.write(hdr, timeout=timeout)
        self.write(data, timeout=timeout)
        self.expect('K', timeout=timeout)

    def tell_guest(self, cmd):
        self.write(cmd.encode())


class Monitor:
    """The serial debug monitor, used only by the bootstrap.

    The monitor stops the CPU while it has the line, so postbox frames are not parsed while its
    prompt is up -- resume with go() before opening a session.
    """

    def __init__(self, ser):
        self.s = ser

    def _drain(self):
        self.s.reset_input_buffer()

    def in_monitor(self):
        self._drain()
        self.s.write(b'r\r'); self.s.flush()
        time.sleep(1.8)
        return b'ticks=' in self.s.read(200000)

    def attach(self):
        if self.in_monitor():
            return True
        self.s.write(b'\x03\x04'); self.s.flush(); time.sleep(2.0); self.s.read(300000)
        if self.in_monitor():
            return True
        # from a detached console the screen has to be attached first
        self.s.write(b'\x1b\x1b\x1b'); self.s.flush(); time.sleep(2.0); self.s.read(300000)
        self.s.write(b'\x03\x04'); self.s.flush(); time.sleep(2.0); self.s.read(300000)
        return self.in_monitor()

    def cmd(self, text, wait=2.0):
        self._drain()
        self.s.write(text.encode() + b'\r'); self.s.flush()
        time.sleep(wait)
        return self.s.read(300000).decode('utf-8', 'replace').strip()

    def load(self, linear, blob):
        self._drain()
        self.s.write(('l %x %x\r' % (linear, len(blob))).encode()); self.s.flush()
        time.sleep(1.5)
        self.s.read(60000)
        self.s.write(blob); self.s.flush()
        time.sleep(1.5 + len(blob) / 8000.0)
        return self.s.read(60000).decode('utf-8', 'replace').strip()

    def go(self):
        self.s.write(b'g\r'); self.s.flush(); time.sleep(1.2); self._drain()

    def reboot(self):
        self.s.write(b'z\r'); self.s.flush(); time.sleep(2.0)


def bootstrap(pb, com_path, seg, dosname):
    """Drop POSTBOX.COM onto the guest disk without it being there already.

    The image is injected into memory with the debugger and started there, then used to write
    itself to disk. It is entered with no PSP and no way back into COMMAND.COM, so it is told to
    park afterwards and the guest is rebooted.
    """
    blob = open(com_path, 'rb').read()
    mon = Monitor(pb.s)
    if not mon.attach():
        raise SystemExit('could not reach the debug monitor')

    linear = seg * 16 + 0x100          # .COM images start at offset 0x100
    print('loading %d bytes at %04X:0100 ...' % (len(blob), seg))
    print(' ', mon.load(linear, blob))
    print(' ', mon.cmd('j %04x:0100' % seg))
    mon.go()

    print('opening the postbox and waiting for the injected copy ...')
    pb.open()
    pb.wait_ready()
    print('running; writing %s to the guest disk ...' % dosname)
    pb.send_file(blob, dosname)
    print('written. parking it and rebooting the guest.')
    pb.tell_guest('P')
    time.sleep(0.5)
    pb.close()
    if not mon.attach():
        raise SystemExit('dropped the file but could not reach the monitor to reboot')
    mon.reboot()
    print('done -- %s is on the disk; run it from the prompt before sending files.' % dosname)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port')
    ap.add_argument('--baud', type=int, default=115200)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('status', help='report whether a postbox is listening')
    p = sub.add_parser('loopback', help='push bytes in and read whatever the guest sent back')
    p.add_argument('--text', default='postbox')

    p = sub.add_parser('bootstrap', help='drop POSTBOX.COM onto the guest disk using the debugger')
    p.add_argument('--com', default=os.path.join(os.path.dirname(__file__), 'postbox.com'))
    p.add_argument('--seg', default='3000', help='segment to inject at (hex, default 3000)')
    p.add_argument('--name', default='POSTBOX.COM', help='name to write on the guest')

    p = sub.add_parser('send', help='send a file to POSTBOX.COM running on the guest')
    p.add_argument('file')
    p.add_argument('--name', help='DOS name to write (default: the basename, uppercased)')
    p.add_argument('--keep-running', action='store_true',
                   help='leave POSTBOX.COM waiting for another file instead of exiting it')

    a = ap.parse_args()
    pb = Postbox(a.port, a.baud)

    if a.cmd == 'status':
        pb.open()
        print('port      :', pb.port)
        print('status    :', pb.query())
        pb.close()
        print('closed    :', pb.query())

    elif a.cmd == 'loopback':
        pb.open()
        pb.write(a.text.encode())
        print('after write:', pb.query())
        print('read back  :', pb.read())
        pb.close()

    elif a.cmd == 'bootstrap':
        bootstrap(pb, a.com, int(a.seg, 16), a.name)

    elif a.cmd == 'send':
        data = open(a.file, 'rb').read()
        name = a.name or os.path.basename(a.file).upper()
        pb.open()
        print('waiting for POSTBOX.COM on the guest ...')
        pb.wait_ready()
        print('sending %s (%d bytes) as %s ...' % (a.file, len(data), name))
        pb.send_file(data, name)
        print('written.')
        if not a.keep_running:
            pb.tell_guest('X')
        pb.close()


if __name__ == '__main__':
    main()
