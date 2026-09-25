#!/usr/bin/env python3
"""Parse and validate SFPB (FX program) / SFOB (FX output) banks as the driver does.

Usage: python3 tools/dump_fx_bank.py FILE [OFFSET ...]
       python3 tools/dump_fx_bank.py FILE --scan        (find every SFPB/SFOB in FILE)
Format: docs/notes/fx_format.md. Checks mirror fx_prg_select / fx_cmd_set_out.
"""
import struct
import sys

RB_NEED = [0x3FFF, 0x7FFF, 0xFFFF, 0x1FFFF]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def pan_str(c):
    c &= 0x1F
    if c in (0, 0x10):
        return 'C'
    return ('R%d' if c < 0x10 else 'L%d') % (c & 0xF)


def dump(b, base):
    magic = b[base:base + 4]
    ver, size, count = u32(b, base + 4), u32(b, base + 8), u32(b, base + 12)
    errs = []
    if ver & 0xFF != 1:
        errs.append('version')
    if b[base + size - 4:base + size] != b'ENDB':
        errs.append('ENDB')
    print('%s @0x%X: version %d size 0x%X count %d %s' % (
        magic.decode(), base, ver, size, count, ('ERR ' + ','.join(errs)) if errs else 'ok'))
    offs = [u32(b, base + 0x10 + 4 * i) for i in range(count & 0xFF)]
    if magic == b'SFPB':
        for i, o in enumerate(offs):
            p = base + o
            name = b[p:p + 32].split(b'\0')[0].decode('latin-1').rstrip()
            rb, b21, b22, pan_mode, pan_base = b[p + 0x20:p + 0x25]
            coef = struct.unpack_from('<128I', b, p + 0x40)
            madrs = struct.unpack_from('<64I', b, p + 0x240)
            gap = struct.unpack_from('<64I', b, p + 0x340)
            mpro = struct.unpack_from('<512I', b, p + 0x440)
            steps = max([j // 4 + 1 for j, x in enumerate(mpro) if x] or [0])
            print('  prg %d @+0x%X "%s": rb_size %d (%d KB, need SFPW >= 0x%X) b21 %d b22 %d '
                  'pan_mode 0x%02X pan_base %d | COEF nz %d, MADRS nz %d, gap nz %d, MPRO steps %d%s' % (
                      i, o, name, rb, 16 << rb if rb < 4 else -1, RB_NEED[rb] if rb < 4 else -1,
                      b21, b22, pan_mode, pan_base, sum(1 for x in coef if x), sum(1 for x in madrs if x),
                      sum(1 for x in gap if x), steps,
                      '' if o + 0xC40 <= size - 4 else '  !! runs past ENDB'))
    elif magic == b'SFOB':
        # the driver only uses the low byte of offset[0] and assumes contiguous 0x20-byte sets
        tbl = base + b[base + 0x10]
        for i in range(count & 0xFF):
            e = tbl + i * 0x20
            outs = ['%d:%d/%s' % (n, b[e + 2 * n] & 0xF, pan_str(b[e + 2 * n + 1]))
                    for n in range(16) if b[e + 2 * n] & 0xF]
            print('  set %d @+0x%X (offset[%d]=0x%X): EFREG lev/pan %s' % (
                i, e - base, i, offs[i] if i < len(offs) else -1, ' '.join(outs) or '(all off)'))


def main():
    data = open(sys.argv[1], 'rb').read()
    if len(sys.argv) > 2 and sys.argv[2] == '--scan':
        bases = [i for i in range(0, len(data) - 16, 4) if data[i:i + 4] in (b'SFPB', b'SFOB')
                 and u32(data, i + 4) == 1 and 0x14 <= u32(data, i + 8) <= len(data) - i
                 and data[i + u32(data, i + 8) - 4:i + u32(data, i + 8)] == b'ENDB']
    elif len(sys.argv) > 2:
        bases = [int(x, 0) for x in sys.argv[2:]]
    else:
        bases = [0]
    for base in bases:
        dump(data, base)


if __name__ == '__main__':
    main()
