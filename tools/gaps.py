#!/usr/bin/env python3
"""Report drv_image ranges (>=16 bytes) not covered by disassembled instructions."""
import re
ins = set()
for l in open('out/ghidra/listing.txt'):
    m = re.match(r'\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+(\S+)', l)
    if m and not m.group(3).startswith(('??', 'undefined', 'db', 'ddw', 'dw', 'addr')):
        ins.add(int(m.group(1), 16))
runs = []; start = None
for a in range(0x100, 0x8300, 4):
    if a not in ins:
        if start is None: start = a
    elif start is not None:
        runs.append((start, a)); start = None
if start is not None: runs.append((start, 0x8300))
for s, e in runs:
    if e - s >= 16: print(f'{s:05x}-{e:05x} {e-s:6d}')
print(len(ins) * 4, 'bytes of instructions')
