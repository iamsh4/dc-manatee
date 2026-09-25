#!/usr/bin/env python3
"""Extract the Manatee ARM sound driver (SDRV) from a Dreamcast game's data archive.

Reference build: Soul Calibur (USA, T1401N), file CINIT.DAT. Top-level olnk entry 2 is a
nested olnk archive whose child 0 is the SDRV container (32-byte header + ARM7DI image that
is loaded at AICA sound RAM 0x0). Offsets are found by walking the olnk headers.

usage: python3 tools/extract_sdrv.py path/to/CINIT.DAT
       python3 tools/extract_sdrv.py path/to/container.bin   (an already-extracted 'SDRV' container)
Writes bin/sdrv_container.bin and bin/manatee_arm.bin and checks their SHA-256.
"""
import hashlib, pathlib, struct, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXPECTED = {
    'sdrv_container.bin': '12c461d78bcf70e73dccb309979fbf2aa8534112270384fb45b0a0d1b8e85afc',
    'manatee_arm.bin': '44f3ab80428e7d5c1f619af6c39ae038028779de232a0cb27ce4911ea15cd9c6',
}


def olnk_entry(buf, base, idx):
    count, magic, payload, _ = struct.unpack_from('<I4sII', buf, base)
    if magic != b'olnk':
        raise SystemExit(f'not an olnk archive at {base:#x}')
    if idx >= count:
        raise SystemExit(f'olnk archive at {base:#x} has no entry {idx}')
    off, size = struct.unpack_from('<II', buf, base + 0x10 + 8 * idx)
    return base + payload + off, size


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    data = pathlib.Path(sys.argv[1]).read_bytes()
    if data[:4] == b'SDRV':
        start = 0
    else:
        nested, _ = olnk_entry(data, 0, 2)
        start, _ = olnk_entry(data, nested, 0)
        if data[start:start + 4] != b'SDRV':
            raise SystemExit('no SDRV container where expected (not the reference CINIT.DAT?)')
    size = struct.unpack_from('<I', data, start + 8)[0]
    cont = data[start:start + size]
    image = cont[0x20:]

    out = ROOT / 'bin'
    out.mkdir(exist_ok=True)
    ok = True
    for name, blob in (('sdrv_container.bin', cont), ('manatee_arm.bin', image)):
        (out / name).write_bytes(blob)
        digest = hashlib.sha256(blob).hexdigest()
        match = digest == EXPECTED[name]
        ok &= match
        print(f'{name:20s} {len(blob):#07x} bytes  sha256 {digest}  {"OK" if match else "MISMATCH"}')
    if not ok:
        print('warning: this is not the reference build (v1.1 build 0x3F); '
              'the decompilation and annotations may not match it', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
