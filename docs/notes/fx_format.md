# FX (AICA DSP) data formats and driver handling

Sources: driver code 0x0694-0x0858, 0x5A8C-0x6454 (src/arm/fx.c; fx_prg_select 0x5C98 is in
src/arm/voice.c), CC79/CC80 handlers 0x24D0/0x250C (src/arm/event_ctl.c), the built-in banks in
the image and the real banks in `data/files/CINIT.DAT`. `tools/dump_fx_bank.py FILE --scan`
parses/validates banks exactly like the driver. SDK names from `sg_sd.h` (Sega sd library 1.00.18):
SFPB = `SDE_DATA_TYPE_FX_PRG_BANK`, SFOB = `SDE_DATA_TYPE_FX_OUT_BANK`, SFPW = `SDE_DATA_TYPE_FX_PRG_WRK`.

## Where the driver finds them

The SH-4 library writes `{address, size}` pairs into the bank table (no host command is sent):

| Slot | Content | Default (init_bank_table) |
| --- | --- | --- |
| 0x14200/04 | SFPB | 0x2908 / 0xC58 (built-in "e-reverb" bank). If the address is 0 the driver also falls back to 0x2908. |
| 0x14280/84 | SFOB | 0x4234 / 0x38 (built-in) |
| 0x14288/8C | SFPW = DSP ring buffer, address / size in bytes | 0 / 0 (cleared) |

(0x14290/0x14298 are unrelated: MIDI program-bank entries 66/67 = drum kits, default 0x4124.)

## Common bank header (SFPB and SFOB)

| Off | Type | Meaning | Driver check |
| --- | --- | --- | --- |
| 0x00 | char[4] | "SFPB" / "SFOB" | else error 2 |
| 0x04 | u32 | version = 1 | low byte only, else error 8 |
| 0x08 | u32 | bank size in bytes (incl. trailing "ENDB") | "ENDB" must be at size-4, else error 4 |
| 0x0C | u32 | count (programs / output sets) | low byte only; index >= count -> error 0x10 |
| 0x10 | u32[count] | offset of each entry from the bank start (the CINIT SFOBs pad the table with a 0 word, entry at 0x18) | |
| size-4 | char[4] | "ENDB" | |

Error bits are ORed into the byte at 0x13420 (`sdDrvGetErr`) and returned. They are the SDK's
`SDD_DRV_ERR_*` values >> 3: 1 = BANK_NO_DOWNLOAD (here: SFPW missing), 2 = ILLEGAL_ID,
4 = ILLEGAL_END_ID, 8 = ILLEGAL_VER, 0x10 = ILLEGAL_NUM.

## SFPB entry = one DSP program (0xC40 bytes, `FxProgram` in manatee.h)

| Off | Size | Meaning |
| --- | --- | --- |
| 0x000 | 32 | name, NUL- or space-padded ("e-reverb", "dcsc2_outside.FPD" ...) |
| 0x020 | u8 | `rb_size`: required ring buffer, RBL code 0..3 = 16/32/64/128 KB. Written to RINGBUF bits 14:13; SFPW size must be >= `fx_rb_need_tab[rb_size]` (0x5DA0: 0x3FFF/0x7FFF/0xFFFF/0x1FFFF) |
| 0x021 | u8 | unused (0 in every known bank) |
| 0x022 | u8 | copied to 0x13F1C on load, otherwise unused (0 everywhere) |
| 0x023 | u8 | `pan_mode`: bit4 = program has 8 DSP pan slots, bit5 = 4 pan slots. Nonzero also waives the ring-buffer size check (0 everywhere) |
| 0x024 | u8 | `pan_coef_base`: first COEF index of the pan slots |
| 0x025 | 27 | unused (0) |
| 0x040 | 128 x u32 | COEF (13-bit signed coefficient << 3, e.g. 0x7FF8 = +0.999) -> 0x803000 |
| 0x240 | 64 x u32 | MADRS (ring-buffer word offsets) -> 0x803200 |
| 0x340 | 64 x u32 | not a register block (0x803300-0x8033FF); copied anyway, always 0 |
| 0x440 | 512 x u32 | MPRO, 128 steps x 4 16-bit words -> 0x803400 |

The loader copies 0x300 words from entry+0x40 to 0x803000 in one loop.

Pan slots (only used if `pan_mode` != 0; no known program has them): slot i (0..3 or 0..7) owns
COEF[base + i + k*slots], k = 0..3, a 2x2 mixing matrix. Host command 0x88 (cmd[2+i]) and MIDI CC80
(channel = slot, any port) write preset `(value >> 2) & 31` from `fx_pan_coef_tab` (0x3C48, 32 x 4
COEFs, roughly equal-power and not exactly symmetric: 0x7FF8 = 1.0 at the ends, 0x5998 = 0.7 at the
centre; see data_map.md). With both bits set the
host command uses 4 slots and CC80 uses 8.

## SFOB entry = one output set (0x20 bytes, `FxOutSet`)

16 x `{u8 lev, u8 pan}`, one per DSP output EFREG0..15 -> AICA 0x802000 + 4n:
`EFSDL` (bits 11:8) = lev & 15, `EFPAN` (bits 4:0) = pan (AICA sign/magnitude pan: 0x00/0x10 centre,
0x01..0x0F right, 0x11..0x1F left; 0x0F full right, 0x1F full left).

Driver quirks: only the low byte of `offset[0]` is used and sets are assumed to follow each other
at 0x20-byte intervals (offset[1..] ignored); host 0x83 always modifies set 0.

Host level/pan offsets (host 0x83 `sdSndSetFxOutPrm(out, pan, lev)`: cmd[2]=out, cmd[3]=lev+0x80,
cmd[4]=pan+0x80) are kept at 0x13450 + 2*out (s8 lev, s8 pan) and applied on every write:
`EFSDL = clamp(lev_ofs + lev*8, 0, 127) >> 3`, `EFPAN = lin2aica(clamp(pan_ofs + aica2lin(pan)*4, 0, 127) >> 2)`
where aica2lin maps the pan code to 0..31 (0 = full left, 0x10 = centre, 31 = full right).

## SFPW = the DSP ring buffer

Only address and size (0x14288/0x1428C). Requirements: address >= 0x18000 (signed test) and
size >= need[rb_size]. `RINGBUF = (addr >> 11 & 0xFFF) | RBL << 13` (RBP in 2 KB units, so the
buffer should be 2 KB aligned). fx_update_ringbuf (every 4 ms) recomputes RBL from the SFPW *size*
(>=128K -> 3, >=64K -> 2, >=32K -> 1, else 0; only size bits 15..21 are examined, so e.g. 4 MB counts
as < 32K), overriding the program's rb_size a tick after a load (only while the address is >= 0x18000).
The buffer is cleared with 0x60006000: in the DSP's 16-bit float memory format (sign, 4-bit exponent,
11-bit mantissa) 0x6000 is 0.0, whereas 0x0000 decodes to a non-zero value.

## Program change sequence

1. `fx_prg_select(n)` (host 0x84 = first half of `sdSndSetFxPrg(prg, out)`, entry 0x5C94 with
   cmd[2]; or CC79 = n+1 on MIDI port 7): validate, then queue `fx_prg_pending`, `fx_rb_pending`,
   `fx_load_req = 1`. Bug: a too-small SFPW reports `rb_size*4` (0/4/8/0xC) instead of error 1.
2. Host 0x82 (second half of `sdSndSetFxPrg`, also `sdSndSetFxOut`) selects the SFOB set.
3. Next 4 ms tick, `fx_prg_load_service` (skipped while 0x13F01 is set): fade DSP outputs out
   (16 steps), clear sends/MPRO/COEF/MADRS/TEMP, clear the ring buffer, write RINGBUF, 0x13476 = busy,
   copy COEF/MADRS/MPRO, clear the ring again, fade back in, 0x13474 = program number, busy = 0.
4. `fx_prg_clear` (host 0x85 `sdSndClearFxPrg`, CC79 = 0 on port 7, resets): fade out, clear DSP
   and ring, fade in; 0x13474 = 0xFF.

Readback: the driver keeps the current program in 0x13474 (0xFF = none) and the output set in
0x13475. The R9 library's sdSndGetFxPrg/GetFxOut read 0x134E0/0x134E1 instead - a mismatch between
this driver build and that library version (or those getters are unused by the game).

## 0x13F00 block (drv_layout_table[0])

| Addr | Meaning |
| --- | --- |
| 0x13F00 | external-control request (writer unknown; not the R9 sd library) |
| 0x13F01 | 0xFF when 0x13F00 != 0 and an SFPW is registered: loader and RINGBUF updates are suspended |
| 0x13F04 | 0x803000 (DSP register window base) |
| 0x13F08 | 0xC00 (window size: COEF..MPRO) |
| 0x13F0C | SFPW size, updated when RINGBUF is rewritten |
| 0x13F10 | shadow of AICA_RINGBUF |
| 0x13F1C/1D/1E | bytes +0x22/+0x23/+0x24 of the loaded program |
| 0x13F80 -> 0x13F81, 0x13F94 -> 0x13F95 | two more request -> ack byte pairs (0xFF/0), no other user |

This looks like a hook for an external tool (e.g. a DSP editor on a development system) to take
over the DSP; no writer of 0x13F00 is known.

## Known banks

| Where | Bank | Content |
| --- | --- | --- |
| image 0x2908 (also CINIT.DAT 0x219FA8 = driver copy) | SFPB, 1 program | "e-reverb": rb_size 3 (128 KB), 90 MPRO steps, 64 COEF, 31 MADRS |
| image 0x4234 (CINIT 0x21B8D4) | SFOB, 1 set | EFREG0 lev 15 full left, EFREG1 lev 15 full right |
| CINIT.DAT 0x362300 (olnk entry 2, child 6) | SFPB, 3 programs (offsets 0x1C/0xC5C/0x189C) | "dcsc2_outside.FPD" (18 steps), "dcsc2_inside.FPD" (38 steps), "dcsc2BGM.FPD" (90 steps, same structure as e-reverb); all rb_size 2 (64 KB) |
| CINIT.DAT 0x3647E0 (child 7) | SFOB, 1 set, offset 0x18 | EFREG0/2/4/6/14 full left, 1/3/5/7/15 full right, all lev 15 |
| CINIT.DAT 0x220D40 | SFOB, 1 set | EFREG0/2/4 left, 1/3/5 right |

All real banks validate against the driver checks; `pan_mode`, bytes 0x21/0x22 and the 0x340 gap are
0 in every program, and each SFOB has exactly one set, so the driver's offset/set-0 quirks never
matter in the game.
