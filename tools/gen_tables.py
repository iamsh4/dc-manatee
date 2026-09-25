#!/usr/bin/env python3
"""Generate src/arm/tables.c: constant tables extracted verbatim from bin/manatee_arm.bin.

Every table is emitted with its image address; see docs/notes/data_map.md for meaning/users.
Run from the repository root:  python3 tools/gen_tables.py
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMG = (ROOT / 'bin/manatee_arm.bin').read_bytes()

# (C name, address, element type, count, comment)
TABLES = [
    ('drv_layout_table', 0x00C0, 'u32', 8,
     'work-area pointers published to the SH-4 (read by the sd library after boot)'),
    ('builtin_sfpb', 0x2908, 'u8', 0xC58,
     'built-in FX program bank (SFPB, 1 program "e-reverb"); installed in 0x14200 by init_bank_table'),
    ('builtin_sfpb_size', 0x3560, 'u32', 1,
     'size word following builtin_sfpb (copied to 0x14204)'),
    ('note_oct_tab', 0x3564, 'u8', 0xE4,
     'note -> (octave << 4 | semitone), index = note + 0x60 (+transpose); octave is a signed nibble '
     '(0x8..0xF = -8..-1), 0x7F = out of range'),
    ('fns_lo_tab', 0x3648, 'u8', 0x600,
     'low 8 bits of AICA FNS for semitone*128 + fine (1/128 semitone); '
     'add 0x100 for each threshold FNS_HI_1/2/3 the index is >= (FNS = floor(1024*(2^(i/1536)-1)))'),
    ('fx_pan_coef_tab', 0x3C48, 'u32', 32 * 4,
     'DSP pan-matrix COEF presets, 32 positions x 4 coefficients (13-bit << 3, 0x7FF8 = 1.0); '
     'index = (7-bit pan >> 2): [0] = {0,1,1,0} ... [15]/[16] = {0,0.7,0,0.7} ... [31] = {1,0,0,1}; '
     'roughly equal-power, not exactly symmetric; see fx_cmd_set_dsp_pan'),
    ('builtin_smpb0', 0x3E48, 'u8', 0x2D8,
     'built-in MIDI program bank (SMPB, 2 programs: 16-bit square / 8-bit saw, samples inside); '
     'default of every MIDI channel (init_midi_ports); +0xE0 = volume/velocity curve'),
    ('builtin_smpb0_size', 0x4120, 'u32', 1, 'size word following builtin_smpb0'),
    ('builtin_smpb1', 0x4124, 'u8', 0x10C,
     'second built-in SMPB = default drum kit (1 split playing the AICA noise generator); installed in 0x14290/0x14298'),
    ('builtin_smpb1_size', 0x4230, 'u32', 1, 'size word following builtin_smpb1 (copied to 0x14294/0x1429C)'),
    ('builtin_sfob', 0x4234, 'u8', 0x38,
     'built-in FX output bank (SFOB, 1 set: EFREG0 full left, EFREG1 full right); installed in 0x14280'),
    ('builtin_sfob_size', 0x426C, 'u32', 1, 'size word following builtin_sfob (copied to 0x14284)'),
    ('seq_len_mul_a', 0x47A4, 'u32', 4, 'sequencer: multiplier selected by (op & 3), accumulates into track +0x2C'),
    ('seq_len_mul_b', 0x47B4, 'u32', 4, 'sequencer: multiplier selected by (op & 3), accumulates into track +0x28'),
    ('fx_rb_need_tab', 0x5DA0, 'u32', 4,
     'minimum accepted SFPW size for FX program ring-buffer size code 0..3 (16/32/64/128 KB minus 1; '
     'fx_prg_select rejects size < entry)'),
    ('pan_aica_to_lin_a', 0x7310, 'u8', 32, 'AICA pan code (sign/magnitude) -> linear 0..31 (0x10 = centre)'),
    ('pan_lin_to_aica_a', 0x7330, 'u8', 32, 'linear pan 0..31 -> AICA pan code'),
    ('pan_lin_to_aica_b', 0x7CD4, 'u8', 32, 'linear pan 0..31 -> AICA pan code (duplicate of pan_lin_to_aica_a)'),
    ('pan_aica_to_lin_b', 0x7CF4, 'u8', 32, 'AICA pan code -> linear 0..31 (duplicate of pan_aica_to_lin_a)'),
    ('oct_flip_tab', 0x7E38, 'u8', 16, 'OCT nibble two\'s complement <-> offset binary (x ^ 8)'),
]


def values(addr, typ, count):
    if typ == 'u8':
        return list(IMG[addr:addr + count])
    if typ == 'u16':
        return list(struct.unpack_from('<%dH' % count, IMG, addr))
    return list(struct.unpack_from('<%dI' % count, IMG, addr))


def fmt(v, typ):
    return {'u8': '0x%02X', 'u16': '0x%04X', 'u32': '0x%08X'}[typ] % v


def main():
    out = []
    out.append('/*\n * tables.c - constant data tables of the driver image, extracted verbatim from\n'
               ' * bin/manatee_arm.bin by tools/gen_tables.py (DO NOT EDIT BY HAND).\n'
               ' * Addresses are ARM sound-RAM addresses; see docs/notes/data_map.md.\n */\n')
    out.append('#include "manatee.h"\n')
    for name, addr, typ, count, cmt in TABLES:
        vals = values(addr, typ, count)
        out.append('\n/* 0x%04X-0x%04X: %s */' % (addr, addr + count * {'u8': 1, 'u16': 2, 'u32': 4}[typ] - 1, cmt))
        if count == 1:
            out.append('const %s %s = %s;' % (typ, name, fmt(vals[0], typ)))
            continue
        per = {'u8': 16, 'u16': 8, 'u32': 4}[typ]
        if name == 'fx_pan_coef_tab':
            out.append('const u32 %s[32][4] = {' % name)
            for i in range(32):
                out.append('    { %s },' % ', '.join(fmt(v, typ) for v in vals[i * 4:i * 4 + 4]))
        else:
            out.append('const %s %s[0x%X] = {' % (typ, name, count))
            for i in range(0, count, per):
                out.append('    ' + ', '.join(fmt(v, typ) for v in vals[i:i + per]) + ',')
        out.append('};')
    (ROOT / 'src/arm/tables.c').write_text('\n'.join(out) + '\n')
    print('wrote src/arm/tables.c (%d tables)' % len(TABLES), file=sys.stderr)


if __name__ == '__main__':
    main()
