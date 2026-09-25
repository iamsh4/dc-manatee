#!/usr/bin/env python3
"""Coverage audit: Ghidra functions vs C sources vs confidence ratings, plus orphan instructions."""
import re, glob, pathlib, json, sys
ROOT = pathlib.Path(__file__).resolve().parent.parent
idx = [l.rstrip('\n').split('\t') for l in open(ROOT/'out/ghidra/index.tsv')][1:]
csrc = ''.join(open(f).read() for f in glob.glob(str(ROOT/'src/arm/*.c')))
# addresses mentioned in C header comments: "0x09D4 name" or "0x9D4"
mentioned = {int(m, 16) for m in re.findall(r'\b0x([0-9A-Fa-f]{3,5})\b', csrc)}
headers = {int(m.group(1), 16) for m in re.finditer(r'^\s*\*\s*0x([0-9A-Fa-f]{3,5})\b', csrc, re.M)}
conf = {}
for f in glob.glob(str(ROOT/'tools/ghidra/annotations/*.json')):
    for fn in json.load(open(f)).get('functions', []):
        conf[int(fn['addr'], 16)] = fn.get('confidence')
missing_c, no_conf, unnamed = [], [], []
for e in idx:
    a = int(e[0], 16); name = e[1]
    if a < 0x20: continue          # vector stubs
    if name.startswith('FUN_'): unnamed.append((a, name))
    if a not in headers: missing_c.append((a, name, 'mentioned' if a in mentioned else 'absent'))
    if not conf.get(a): no_conf.append((a, name))
print(f'{len(idx)} functions; unnamed {len(unnamed)}; no C header comment {len(missing_c)}; no confidence {len(no_conf)}')
for a, n, how in missing_c: print(f'  no-header {a:05x} {n} ({how} in C)')
for a, n in no_conf: print(f'  no-conf   {a:05x} {n}')
# orphan instructions: in listing but not inside any function body
bodies = []
for f in glob.glob(str(ROOT/'out/ghidra/functions/*.txt')):
    head = open(f).readline(); body = open(f).readlines()[1]
    for s, e in re.findall(r'\[([0-9a-f]{8}), ([0-9a-f]{8})\]', body):
        bodies.append((int(s, 16), int(e, 16)))
def inbody(a): return any(s <= a <= e for s, e in bodies)
orph = []
for l in open(ROOT/'out/ghidra/listing.txt'):
    m = re.match(r'\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+([a-z]\S*)', l)
    if m and not m.group(3).startswith(('undefined', 'addr', 'ddw', 'dw', 'db', 'uint', 'int', 'char', 'byte', 'dword', 'pointer', 'ds')):
        a = int(m.group(1), 16)
        if a >= 0x20 and not inbody(a): orph.append(a)
runs = []
for a in orph:
    if runs and a == runs[-1][1] + 4: runs[-1][1] = a
    else: runs.append([a, a])
print(f'orphan instruction runs: {len(runs)}')
for s, e in runs: print(f'  {s:05x}-{e+3:05x}')
