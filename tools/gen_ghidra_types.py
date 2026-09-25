#!/usr/bin/env python3
"""Extract struct/enum/typedef declarations from src/arm/manatee.h for Ghidra's C parser.

Preprocesses the header for the ARM target (layout asserts disabled via -DGHIDRA_TYPES),
keeps only top-level type declarations and writes build/ghidra_types.h.
"""
import pathlib, re, subprocess, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKIP = set()

def statements(text):
    depth, cur = 0, []
    for ch in text:
        cur.append(ch)
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
        elif ch == ';' and depth == 0:
            yield ''.join(cur).strip(); cur = []

def main():
    pp = subprocess.run(['clang', '-E', '-P', '--target=arm-none-eabi', '-march=armv4', '-ffreestanding',
                         '-DGHIDRA_TYPES', '-I', str(ROOT / 'src/arm'), str(ROOT / 'src/arm/manatee.h')],
                        check=True, capture_output=True, text=True).stdout
    keep = []
    for st in statements(pp):
        st = re.sub(r'\s+', ' ', st)
        if st.startswith('typedef char static_assert_'):
            continue
        is_type = st.startswith(('typedef', 'struct', 'enum', 'union'))
        if not is_type or st.startswith('typedef __'):
            continue
        if re.match(r'typedef [\w ]+ u?int(_least|_fast|max|ptr)?\d*_t;$', st) or \
           re.match(r'typedef (u?int\d+_t) [us]\d+;$', st):
            continue
        # drop function prototypes / function pointer typedefs
        head = st.split('{')[0]
        if '(' in head and '{' not in st:
            continue
        if any(re.search(r'\b%s\b' % s, head) for s in SKIP):
            continue
        keep.append(st)
    out = ROOT / 'build/ghidra_types.h'
    out.parent.mkdir(exist_ok=True)
    base = ['typedef unsigned char u8;', 'typedef signed char s8;', 'typedef unsigned short u16;',
            'typedef short s16;', 'typedef unsigned int u32;', 'typedef int s32;']
    out.write_text('\n'.join(base + keep) + '\n')
    print(f'{len(keep)} declarations -> {out}')

if __name__ == '__main__':
    sys.exit(main())
