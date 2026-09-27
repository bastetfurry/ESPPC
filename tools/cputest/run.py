#!/usr/bin/env python3
"""
Run FabGL's i8086 core against the SingleStepTests 8088 suite (https://github.com/SingleStepTests/8088,
MIT licence, hardware-generated on a real 8088).

    g++ -O2 -shared -fPIC -std=gnu++17 -w -I tools/cputest/stub -I lib/FabGL/src/emudevs \\
        -o /tmp/libcpu.so tools/cputest/harness.cpp lib/FabGL/src/emudevs/i8086.cpp
    tools/cputest/run.py /tmp/libcpu.so <dir with v2/*.json.gz and metadata.json> [opcode files...]

Each test sets the registers and the memory it lists, runs one instruction (harness.cpp: prefixes
and REP iterations are separate steps of the core, so it steps until IP leaves the instruction),
and compares registers, flags -- masked with metadata.json's undefined flags -- and the memory the
test says changed. Results are counted per test file; --out writes one example failure per file.

Known difference by design: the core does not wrap addresses at 1 MB (segment * 16 + offset), the
8088 does. The harness mirrors every byte a test sets 1 MB up, so the core finds the same memory
there; tests whose writes only landed in the mirror count as passed and are reported as "wrap".
"""

import ctypes
import gzip
import json
import multiprocessing
import os
import sys

REGS = ['ax', 'bx', 'cx', 'dx', 'cs', 'ss', 'ds', 'es', 'sp', 'bp', 'si', 'di', 'ip', 'flags']

PREFIXES = (0x26, 0x2e, 0x36, 0x3e, 0xf0, 0xf2, 0xf3)
# Instructions after which the core executes the next one in the same step, as the 8086 holds
# interrupts off for one instruction after them: POP ES/SS/DS, MOV SS, and here also the flag
# instructions. The harness puts a NOP after them and expects IP one further.
INHIBIT = (0x07, 0x17, 0x1f, 0x8e, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd)
FLAGNAMES = [(0x001, 'C'), (0x004, 'P'), (0x010, 'A'), (0x040, 'Z'), (0x080, 'S'), (0x100, 'T'),
             (0x200, 'I'), (0x400, 'D'), (0x800, 'O')]

LIB = None
META = None


def flag_diff(a, b):
    return ''.join(n for bit, n in FLAGNAMES if (a ^ b) & bit)


def lookup_meta(name):
    """(status, flags mask) for a test file name like '40.json.gz' or '80.1.json.gz'"""
    parts = name.split('.')
    op = parts[0].upper()
    entry = META['opcodes'].get(op, {})
    if len(parts) > 3 and 'reg' in entry:
        entry = entry['reg'].get(parts[1], {})
    return entry.get('status', 'normal'), entry.get('flags-mask', 0xffff)


def run_file(path):
    global LIB
    name = os.path.basename(path)
    status, mask = lookup_meta(name)
    tests = json.load(gzip.open(path))
    lib = LIB
    res = {'file': name, 'status': status, 'total': len(tests), 'fail': 0, 'regs': 0, 'flags': 0,
           'mem': 0, 'wrap': 0, 'flagbits': {}, 'example': None}
    for t in tests:
        ini, fin = t['initial'], t['final']
        touched = [a for a, _ in ini['ram']] + [a for a, _ in fin['ram']]
        wraps = False
        lib.h_reset()
        for i, r in enumerate(REGS):
            lib.h_set(i, ini['regs'][r])
        # every byte also 1 MB up: the 8088 wraps addresses at 1 MB and the core does not, so
        # anything it reaches above FFFFFh finds the same memory there (see the module comment)
        for a, v in ini['ram']:
            lib.h_poke(a, v)
            if a < 0x10000:
                lib.h_poke(a + 0x100000, v)
        rep = any(b in (0xf2, 0xf3) for b in t['bytes'])
        op = next((b for b in t['bytes'] if b not in PREFIXES), 0)
        extra_ip = 0
        if op in INHIBIT:
            nop = ((ini['regs']['cs'] << 4) + ini['regs']['ip'] + len(t['bytes'])) & 0xfffff
            lib.h_poke(nop, 0x90)
            if nop < 0x10000:
                lib.h_poke(nop + 0x100000, 0x90)
            touched.append(nop)
            extra_ip = 1
        # exactly the prefixes plus the instruction, except for a REP'd string instruction, which
        # the core runs one iteration per step: that one keeps going while IP stays on it (a jump
        # into its own bytes must not be stepped again, hence no looping otherwise)
        nprefix = next((i for i, b in enumerate(t['bytes']) if b not in PREFIXES), 0)
        stringop = op in (0xa4, 0xa5, 0xa6, 0xa7, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0x6c, 0x6d, 0x6e, 0x6f)
        lib.h_run(len(t['bytes']), 140000 if (rep and stringop) else nprefix + 1)

        problems = []
        for i, r in enumerate(REGS):
            want = fin['regs'].get(r, ini['regs'][r])
            if r == 'ip':
                want = (want + extra_ip) & 0xffff
            got = lib.h_get(i)
            if r == 'flags':
                if (want & mask) != (got & mask):
                    d = flag_diff(want & mask, got & mask)
                    problems.append(('flags', 'flags %04X want %04X (%s)' % (got, want, d)))
                    for ch in d:
                        res['flagbits'][ch] = res['flagbits'].get(ch, 0) + 1
            elif want != got:
                problems.append(('regs', '%s %04X want %04X' % (r, got, want)))
        for a, v in fin['ram']:
            got = lib.h_peek(a)
            if got != v:
                if a < 0x10000 and lib.h_peek(a + 0x100000) == v:
                    wraps = True          # written 1 MB up: correct but for the missing wrap
                    continue
                problems.append(('mem', '[%05X] %02X want %02X' % (a, got, v)))

        if wraps and not problems:
            res['wrap'] += 1              # passed only thanks to the mirror
        if problems:
            res['fail'] += 1
            if wraps:
                res['wrap'] += 1
            else:
                for kind in set(k for k, _ in problems):
                    res[kind] += 1
            if res['example'] is None and not wraps:
                res['example'] = {'name': t['name'], 'bytes': ' '.join('%02X' % b for b in t['bytes']),
                                  'problems': [p for _, p in problems][:6]}
        for a in touched:
            lib.h_poke(a, 0)
            if a < 0x10000:
                lib.h_poke(a + 0x100000, 0)
    return res


def init(libpath, metapath):
    global LIB, META
    LIB = ctypes.CDLL(libpath)
    LIB.h_get.restype = ctypes.c_uint16
    LIB.h_peek.restype = ctypes.c_uint8
    LIB.h_init()
    META = json.load(open(metapath))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out = next((a.split('=', 1)[1] for a in sys.argv[1:] if a.startswith('--out=')), None)
    libpath, testdir = os.path.abspath(args[0]), args[1]
    metapath = os.path.join(testdir, 'metadata.json')
    if len(args) > 2:
        files = [os.path.join(testdir, f if f.endswith('.json.gz') else f + '.json.gz') for f in args[2:]]
    else:
        files = sorted(os.path.join(testdir, f) for f in os.listdir(testdir) if f.endswith('.json.gz'))

    with multiprocessing.Pool(os.cpu_count(), initializer=init, initargs=(libpath, metapath)) as pool:
        results = pool.map(run_file, files, chunksize=1)

    total = sum(r['total'] for r in results)
    fails = sum(r['fail'] for r in results)
    print('%d files, %d tests, %d failed (%.2f%%)' % (len(results), total, fails, 100.0 * fails / max(total, 1)))
    print('%-14s %-12s %6s %6s %6s %6s %6s  %s' % ('file', 'status', 'fail', 'regs', 'flags', 'mem', 'wrap', 'flag bits'))
    for r in results:
        if r['fail']:
            bits = ' '.join('%s:%d' % kv for kv in sorted(r['flagbits'].items()))
            print('%-14s %-12s %6d %6d %6d %6d %6d  %s' % (r['file'].replace('.json.gz', ''), r['status'], r['fail'],
                                                        r['regs'], r['flags'], r['mem'], r['wrap'], bits))
    if out:
        with open(out, 'w') as f:
            json.dump(results, f, indent=1)


if __name__ == '__main__':
    main()
