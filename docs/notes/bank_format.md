# MIDI tone bank format ("SMPB")

Derived from the note-on path of the ARM driver (`note_on` 0x0AD8, `voice_start`
body 0x0BC8-0x11E8, `voice_set_pitch` 0x14BC), the program-change handler (0x136C),
`init_midi_ports` (0x0504) and the built-in default bank at 0x3E48 in the driver
image. C structs: `BankHeader`, `ToneProgram`, `ToneLayer`, `ToneSplit` in
`src/arm/manatee.h`.

All offsets inside a bank are **relative to the bank header**. The driver adds
`MidiChannel.bank` (the absolute sound-RAM address of the header) when it
dereferences them, including the sample start address, so a bank is fully
relocatable.

## Where banks come from

- Bank table: `0x14080 + n*8 = {address, size}`, n = 0..15 (SH-4 downloads,
  see `docs/research/host_protocol.md`). Program change (0x136C) takes
  `n = MidiChannel.bank_no` (+0x01); address 0 selects the built-in bank at 0x3E48.
- Program change: if `program < num_progs` then
  `MidiChannel.prog = bank + prog_ofs[program]`,
  `MidiChannel.velcurve = bank + velcurve_tab`, `MidiChannel.bank = bank`;
  otherwise the previous bank pointer is restored and nothing changes.
- `init_midi_ports` points every channel at program 0 of the built-in bank.

## Header (0x20 bytes)

| Off | Type | Meaning |
| --- | --- | --- |
| 0x00 | char[4] | `"SMPB"` |
| 0x04 | u32 | version, 1 |
| 0x08 | u32 | total size; the bank ends with `"ENDB"` at `size-4` (built-in: followed by the size again) |
| 0x0C | u32 | 0 (not read by the note path) |
| 0x10 | u32 | offset of `u32 prog_ofs[num_progs]` |
| 0x14 | u32 | `num_progs` |
| 0x18 | u32 | offset of the velocity curves, `u8 curve[num_velcurves][128]` |
| 0x1C | u32 | `num_velcurves` (not read by the driver) |

The driver never checks magic/version/ENDB for SMPB banks (unlike SFPB, see
`dsp_program_select`).

## Program (0x14 bytes)

| Off | Type | Meaning |
| --- | --- | --- |
| 0x00 | u32[4] | layer offsets; 0 = layer unused |
| 0x10 | u32 | unknown, not read (0 in the built-in bank) |

A note-on sounds **every** used layer (up to 4 voices per note).

## Layer (0x10 bytes)

| Off | Type | Meaning |
| --- | --- | --- |
| 0x00 | u32 | bits 6:0 number of splits; bit 7 = layer muted (skipped). Count 0 is not special-cased: the dbra-style loop would run 2^32 times |
| 0x04 | u32 | offset of `ToneSplit[n]` |
| 0x08 | u32 | key-on delay in 4 ms ticks (0 = key on immediately). The channel is programmed at note-on but KYONB is only set by `voice_update_4ms` when the counter expires; a note-off before that cancels it |
| 0x0C | u8 | pitch-bend range up (semitones), used for bend >= 0 |
| 0x0D | u8 | pitch-bend range down, used for bend < 0 |
| 0x0E | u8[2] | copied into note-on scratch (0x121E/F) but never used |

`MidiChannel.bend_range` (+0x03) with bit 7 set overrides both ranges with
bits 6:0 (RPN 0).

## Split (0x30 bytes)

Within a layer, the **first** split whose key and velocity windows contain the
note is used (only one per layer). If no voice can be allocated for it, the scan
continues with the following splits of the same layer.

The first 0x24 bytes are 16-bit images of AICA channel registers.

| Off | Type | AICA reg | Meaning / how it is applied |
| --- | --- | --- | --- |
| 0x00 | u16 | +0x00 | bits 10:7 SSCTL/LPCTL/PCMS kept; bits 6:0 = SA[22:16] (bank-relative) |
| 0x02 | u16 | +0x04 | SA[15:0] (bank-relative). SA = ((ctl&0x7F)<<16 \| sa_lo) + bank |
| 0x04 | u16 | +0x08 | LSA (samples, relative to SA) - copied as is |
| 0x06 | u16 | +0x0C | LEA - copied as is |
| 0x08 | u16 | +0x10 | AR 4:0, D1R 10:6, D2R 15:11. Each + (ch offset - 0x20), clamped 0..31: AR by ch+0x37, D1R **and** D2R by ch+0x38. Bit 5 dropped |
| 0x0A | u16 | +0x14 | RR 4:0 (+ch 0x3B), DL 9:5 (+ch 0x39), LPSLNK/KRS 14:10 kept |
| 0x0C | u16 | - | not read by the driver |
| 0x0E | u16 | +0x1C | LFO. PLFOS (7:5) and ALFOS (2:0) scaled by the modulation wheel: `((d+1)*(mod+1)-1) >> 7`; LFORE cleared again right before key-on |
| 0x10 | u8 | +0x20 | IMXL 7:4, ISEL 3:0. IMXL replaced by ch+0x36 bits 7:4 if ch+0x36 != 0, then + port offset ((port+0x26 >> 3) & 0x1F) - 16, clamp 0..15. ISEL replaced by ch+0x46 & 0xF if (ch+0x46 & 0xF0) == 0 |
| 0x11 | u8 | - | not read |
| 0x12 | u8 | +0x24 | DIPAN (5 bits). Converted to 0..127 (0x10 is treated as centre), replaced by channel pan (ch+0x0B) unless its bit 7 is set, + port pan (+0x07) - 0x80, converted back |
| 0x13 | u8 | +0x24 | DISDL in bits 3:0, + ((port+0x24 >> 3) & 0x1F) - 16, clamp 0..15 |
| 0x14 | u8 | +0x28 | Q 4:0 (+ ch+0x27 - 0x40, clamp 0..31), LPOFF bit 5 (overridden by ch+0x3D: 0 = tone, else bit6 set -> filter on) |
| 0x15 | u8 | +0x28 | TL (attenuation), used as a gain factor `256 - TL` (see level) |
| 0x16 | u16[5] | +0x2C..+0x3C | FLV0..4 (13 bits), each + (ch u16 at 0x28+2i - 0x2000) + ch s32 cutoff (+0x40), clamp 8..0x1FF8 |
| 0x20 | u16 | +0x40 | FD1R 4:0 (+ch 0x33), FAR 12:8 (+ch 0x32) |
| 0x22 | u16 | +0x44 | FRR 4:0 (+ch 0x35), FD2R 12:8 (+ch 0x34) |
| 0x24 | u8 | | key low (inclusive) |
| 0x25 | u8 | | key high (inclusive) |
| 0x26 | u8 | | root key (bits 6:0) |
| 0x27 | s8 | | fine tune in 1/256 semitone |
| 0x28 | u8[2] | | not read |
| 0x2A | u8 | | velocity curve index (into the bank's curve table) |
| 0x2B | u8 | | velocity low (inclusive) |
| 0x2C | u8 | | velocity high (inclusive) |
| 0x2D | u8[3] | | padding |

### Level

```
gain = ((curve[vel_curve][vel] + 1) * (MidiChannel.level + 1) * (256 - TL) - 1) >> 14   // 0..255
TL_reg = 0xFF - clamp(gain + PortState.volume - 0x80, 0, 255)
```

`gain` is kept in `Voice.vol` so that port volume changes can be re-applied
every 4 ms (`voice_update_4ms`).

### Pitch

The pitch is a 1/256-semitone value relative to the root key:

```
p   = PortState.pitch - 0x8000 + Voice.pitch_mod + ((bend * range) >> 5 & 0xFFFF) + fine
idx = ((p >> 8) + note + 0x60 - root) & 0xFF        -> table 0x3564: OCT<<4 | semitone
f   = semitone*128 + (p & 0xFE)/2                   -> table 0x3648: FNS bits 7:0,
      FNS bits 9:8 = (f >= 0x1EF) + (f >= 0x383) + (f >= 0x4D9)
```

At note == root and p == 0 the result is OCT 0, FNS 0, i.e. the sample plays at
its recorded rate (44.1 kHz base). Bend: +-0x2000 * range >> 5 = +-256*range,
i.e. `range` semitones.

## Built-in bank (0x3E48, 0x2D8 bytes)

- 2 programs, 1 velocity curve (a concave curve 0x00..0x7F).
- Program 0: one layer, one split: 16-bit PCM, loop, LEA 0x54, root 72,
  bend up 2 / down 12. Program 1: one split, 8-bit PCM, LEA 0xA9, root 60.
- Both: AR 31, RR 31, IMXL 15 ISEL 0, DISDL 15, Q 4, FLV all 0x1FF8,
  LFO 0x42F0, full key/velocity range.
