#!/usr/bin/env python3
"""
Compare two run.py --out results: every test file whose failure count went up is a regression.
    tools/cputest/compare.py baseline.json new.json
"""
import json, sys

old = {r['file']: r['fail'] for r in json.load(open(sys.argv[1]))}
new = {r['file']: r['fail'] for r in json.load(open(sys.argv[2]))}
worse  = [(f, old.get(f, 0), n) for f, n in new.items() if n > old.get(f, 0)]
better = [(f, old.get(f, 0), n) for f, n in new.items() if n < old.get(f, 0)]
print('total failures: %d -> %d' % (sum(old.values()), sum(new.values())))
for f, o, n in sorted(worse):
    print('  WORSE  %-14s %6d -> %6d' % (f, o, n))
for f, o, n in sorted(better):
    print('  better %-14s %6d -> %6d' % (f, o, n))
sys.exit(1 if worse else 0)
