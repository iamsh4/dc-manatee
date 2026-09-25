# One-shot banks (SOSB) and PCM streams (SPSR rings): formats and driver behaviour

Source: `src/arm/host_helpers.c` (0x6DFC-0x7118, 0x7884-0x82CC) and the host handlers in
host_cmd_exec (0x51F4 one-shot play, 0x5560 stream play). The per-tick monitoring is in
`port.c` (pcm_update 0x7178, shot_monitor 0x7350, stream_monitor 0x73C8, status export 0x6470).
The real banks checked are in `data/files/CINIT.DAT`: nested olnk entry 2, child 2 (SOSB at file
offset 0x220D80) and child 5 (SOSB at 0x2FAB40).

## 1. Work RAM

| Address | Content |
| --- | --- |
| 0x11000 | `PcmPlayer g_pcm_player[24]`, 0x58 bytes each. [0..7] = one-shot port 0..7, [8 + port*2 + ch] = stream port 0..7, channel 0/1. |
| 0x12080 | `PcmSlot g_pcm_slot[16]`, 12 bytes each: `{u8 prio, u8 kind, u8 pad[2], PcmPlayer *owner, AicaChannel *ch}`. Slot n owns AICA channel 48+n; `ch` = 0x801800 + n*0x80 (set by pcm_init). kind 1 = one-shot, 2 = stream ch0, 3 = stream ch1. owner 0 = free. |
| 0x13800 / 0x13A00 | Status records of one-shot ports / stream port*2+ch (see host_protocol.md 4.2). |
| 0x14100 / 0x14180 | Bank table: `{addr, size}` x 16 for SOSB banks / SPSR ring buffers. |

The driver reserves AICA channels 48-63 for these ports; channels 0-47 are MIDI voices.
The sd library allows 8 one-shot and 4 stream ports; the driver masks port numbers with 7 and has
room for 8 stream ports.

PcmPlayer fields used by the host commands (see manatee.h for the full list):

| Off | Field | Meaning |
| --- | --- | --- |
| 0x00 | flags | `PORT_FLG_*`: 1 play (channel reserved), 2 pause, 4 vol fade, 8 speed fade, 0x10 pan fade, 0x20 pitch fade, 0x80 error latched (by the host handler) |
| 0x01 | err | error bits of the last play/loop command (host handler copies to status +0x1C and clears) |
| 0x02/0x03 | prio, slot | priority (0..31) and PcmSlot index |
| 0x0C | pitch_base | AICA OCT/FNS of the sample (SOSP) or stream frequency (cmd) |
| 0x10 | pitch_ofs | linear pitch offset from speed+pitch (port.c) |
| 0x14-0x1F | fades | vol/pan interval, step, target, count; fx_lvl (0x1A), direct_lvl (0x1B), volume (0x1E), pan (0x1F); all +0x80 biased |
| 0x20/0x21 | tl, dipan | last values written to the channel |
| 0x22 | fx_base | FxCh base level (bits 7:4) |
| 0x23 | loop_mode | one-shot: 0 or host 0x1C mode+1; stream: LP flag of the last tick |
| 0x24-0x2B | tl_base, isel, imxl, pcms, pan_base, fx_ch, disdl_base, disdl | per-sample bases; fx_ch bit6 = FxCh override (bits 3:0 = ISEL) |
| 0x2C | loop_count | loop ends passed (stream: ring wraps) |
| 0x30 | start | sample / ring start address |
| 0x34 | ring_len | stream: ring size in bytes (clamped); one-shot: host 0x1C param |
| 0x38 | loops_left | one-shot: loop passes left (0 = forever); stream ch0: bank word `bank0 | bank1<<8 | stereo<<16` |
| 0x3C | loops_default | one-shot: SOSP loop count (read and written as a byte) |
| 0x40-0x57 | speed/pitch fades | written by the inline host handlers 0x17/0x18/0x27/0x28 |

## 2. SOSB one-shot bank

```
+0x00 u32 magic   "SOSB"            checked by the 0x11 handler (else status +0x1F |= 2)
+0x04 u32 version 1                 low byte checked (else |= 8)
+0x08 u32 size    bank size         u32 at size-4 must be "ENDB" (else |= 4)
+0x0C u32 count   number of entries shot_play rejects num > count (err 4) -- QUIRK: num == count
                                    is accepted and reads offset[count] = the first entry's tag
+0x10 u32 offset[count]             entry offset from the bank start
      SOSP entries (0x38 bytes each), then the sample data
      "ENDB"
```

SOSP entry (`SospEntry` in manatee.h). Everything from +0x04 to +0x27 is an AICA channel register
image, 16 bits per register, copied verbatim (except SA, which is relocated by the bank address):

| Off | Size | AICA reg | Meaning | Driver use |
| --- | --- | --- | --- | --- |
| 0x00 | 4 | - | "SOSP" | not checked |
| 0x04 | 2 | 0x00 | KYONB=0, SSCTL, LPCTL, PCMS, SA[22:16] | + bank address (carry into SA[22:16]); no key-on bit |
| 0x06 | 2 | 0x04 | SA[15:0] | + bank address |
| 0x08 | 2 | 0x08 | LSA | copied |
| 0x0A | 2 | 0x0C | LEA | copied |
| 0x0C | 2 | 0x10 | D2R/D1R/AR | copied |
| 0x0E | 2 | 0x14 | LPSLNK/KRS/DL/RR | copied |
| 0x10 | 2 | 0x18 | OCT/FNS | copied; read back as pitch_base, re-written every tick with pitch_ofs |
| 0x12 | 2 | 0x1C | LFO | copied, then LFORE cleared |
| 0x14 | 1 | 0x20 | IMXL7:4 ISEL3:0 | copied; imxl/isel bases |
| 0x16 | 1 | 0x24 | DIPAN | copied; pan_base (DIPAN code) |
| 0x17 | 1 | 0x25 | DISDL | copied; disdl_base |
| 0x18 | 1 | 0x28 | LPOFF/Q | copied |
| 0x19 | 1 | 0x29 | TL | copied; tl_base |
| 0x1A | 10 | 0x2C-0x3C | FLV0-4 | copied |
| 0x24 | 2 | 0x40 | FAR/FD1R | copied |
| 0x26 | 2 | 0x44 | FD2R/FRR | copied |
| 0x28 | 1 | - | loop passes (0 = forever) | loops_default |
| 0x29 | 1 | - | ? | not read |
| 0x2A | 2 | - | ? (0x0054, 0x454D, 0x5649, ...) | not read |
| 0x2C | 4 | - | 0 | not read |
| 0x30 | 4 | - | = LEA (sample count) | not read |
| 0x34 | 4 | - | "ENDP" | not checked |

After copying, shot_play overrides the mix registers from the port state (pcm_apply_params):
TL = clamp(tl_base + 0x100 - 2*volume), DIPAN from pan_base moved by (pan-0x80)/4 on a linear
0..31 scale, DISDL = clamp(disdl_base + (direct_lvl-0x80)/8), IMXL = clamp(imxl + (fx_lvl-0x80)/8)
(or FxCh base level), ISEL = sample or FxCh override, pitch = pitch_base + pitch_ofs.
The loop count (loops_left) is loops_default, or modified by host command 0x1C (below).

Driver-side quirk: `pcms` is taken from bits 24:23 of the u32 at +0x04 (= SA[8:7]) instead of
PCMS (bits 8:7 of the halfword). It only feeds `cur_addr`, which nothing exports.

### Real banks (CINIT.DAT)

| | child 2 @0x220D80 | child 5 @0x2FAB40 |
| --- | --- | --- |
| size / count | 0x69DC8 / 61 | 0x677A8 / 53 |
| trailer | ENDB at size-4 | ENDB at size-4 |
| entries | at 0x104 + i*0x38, all "SOSP".."ENDP" | at 0xE4 + i*0x38 |
| sample data | 0xE60..0x69DA3 | 0xC80..0x6778D |
| ctl | 0x0100 + SA hi: PCMS 2 (ADPCM), no loop | same |
| LSA / LEA | 0 / length in samples (= +0x30) | same |
| AR / RR | 0x1F / 0x1F | 0x1F / 0x1F (one 0x15) |
| pitch | 0x7000 (OCT -2, 11025 Hz) | 0x71CD, 0x7341, 0x69CD, ... |
| dsp_send | 0xC2 (IMXL 12, ISEL 2) | 0xC0 / 0xC4 |
| DISDL / DIPAN | 0xF / 0 (centre) | 0xF / 0 |
| TL | 0 | 0x0A mostly, 0x02-0x11 |
| FLV / FEG | 0x1FF8 x5 / 0x1919 0x1919 | same |
| loops | 0 | 0 |

The entries match the layout above exactly (0x38 bytes, SA offsets bank-relative, data packed
after the entry table with small gaps). None of them loops (LPCTL = 0), so the loop count and
host 0x1C are irrelevant for this game's banks: the sample ends, shot_monitor sees the LP (end)
flag and releases the channel.

## 3. PCM stream (Pstm) rings

No header: an SPSR bank is just `{addr, size}` in the table at 0x14180. The SH-4 DMAs wave data
into it (sdPstmTransferWaveData) and reads the play position from the status record.

Host 0x21 (handler 0x5560 -> pstm_play 0x79BC):

| cmd | Driver register | Meaning |
| --- | --- | --- |
| [2] bits 2:0 | r0 port | port (masked 7) |
| [2] bits 5:4 | r1 fmt | AICA PCMS: 0 16-bit, 1 8-bit, 2/3 ADPCM (forced to 3 = long-stream ADPCM) |
| [2] bit 7 | r2 bit16 | stereo (2 channels) |
| [3] | r3 tl | base volume byte = TL attenuation (library: 0xFF - 2*vol, 0x7F -> 0) |
| [4..5] | r4 freq | raw OCT/FNS |
| [6] | r5 | prio<<3 |
| [7] / [8] | r2 bits 7:0 / 15:8 | SPSR bank of ch0 / ch1 (ch1 only when stereo) |

Per channel the driver programs a looping channel over the whole ring:

| Reg | Value |
| --- | --- |
| SA | ring address; KYONB + LPCTL + PCMS<<7 |
| LSA / LEA | 0 / ring size in samples: 16-bit size/2 (size clamped to 0x1F000), 8-bit size (0xF000), ADPCM size*2 (0x7000) |
| AR / D1R / D2R | 0x1F / 0 / 0 |
| DL / RR | 0 / 0x15 |
| LFO | 0x8000 (LFORE) |
| Q / LPOFF | 4 / filter off |
| FLV0-4 | 0x1FF8 |
| FAR..FRR | 0x1F each |
| DIPAN base | mono 0x00 (centre), stereo ch0 0x1F (full left), ch1 0x0F (full right) |
| DISDL base / IMXL base | 0x0F / 0 |

Both channels are keyed on with one KYONEX after the last one. The clamps are the same as the
SH-4 library's (host_protocol.md 5.4), so the library's size argument maps 1:1 to LEA.
Every tick stream_monitor counts ring wraps (LP flag) in loop_count and exports
CA (status +0x18), wraps (+0x0C) and wraps * ring samples + CA (+0x14).

Pan: the library's per-channel pan' (host_protocol.md 3.1) is PcmPlayer.pan; the driver moves
the channel's DIPAN by (pan'-0x80)/4 on the linear 0..31 scale from its base (left/right edge),
which is why the library halves and re-centres the stereo pans.

## 4. Host command -> helper map

| Code | API | Handler | Helper (registers) |
| --- | --- | --- | --- |
| 0x11 | sdShotPlay | 0x51F4 (SOSB checks) | shot_play 0x7884 (r0 port, r1 num, r2 prio<<3, r3 SOSB) |
| 0x12 | sdShotStop | 0x52E4 | shot_stop 0x7E48 (r0) |
| 0x13 | (pause) | 0x52F8 | shot_pause 0x7FFC (r0) |
| 0x14 | (continue) | 0x530C | shot_continue 0x8058 (r0) |
| 0x15 | sdShotSetVol | 0x5320 | shot_set_vol 0x6DFC (r0, r1 vol, r2 fade) |
| 0x16 | sdShotSetPan | 0x5348 | shot_set_pan 0x6E84 |
| 0x17/0x18 | SetSpeed/SetPitch | 0x5370/0x5410 inline | - |
| 0x19 | sdShotSetFxCh | 0x54B0 | shot_set_fx_ch 0x6F4C (r0, r1 in_ch, r2 base) |
| 0x1A | sdShotSetFxLev | 0x54CC | shot_set_fx_lvl 0x6FE0 |
| 0x1B | sdShotSetDrctLev | 0x54E8 | shot_set_direct_lvl 0x7068 |
| 0x1C | (none in sd 1.00.18) | 0x5504 | shot_set_loop_mode 0x8258 (r0, r1 = cmd[3] mode 0..2, r2 = cmd[4..7] param) |
| 0x1E | sdShotResetPrm | 0x5520 | shot_reset_prm 0x809C / 0xFF: shot_reset_prm_all 0x8090 |
| 0x1F | sdShotStopAll | 0x554C | shot_stop_all 0x7F18 |
| 0x21 | sdPstmPlay | 0x5560 | pstm_play 0x79BC (see 3) |
| 0x22 | sdPstmStop | 0x5638 | pstm_stop 0x7E78 |
| 0x25 | sdPstmSetVol | 0x567C | pstm_set_vol 0x6E14 (r0 = cmd[2] incl. mask 0x20 ch0 / 0x10 ch1, r1 [3], r2 [4..5], r4 [7], r5 [8..9]) |
| 0x26 | sdPstmSetPan | 0x56B0 | pstm_set_pan 0x6E9C (same registers) |
| 0x27/0x28 | SetSpeed/SetPitch | 0x56E4/0x5794 inline (both channels) | - |
| 0x29 | sdPstmSetFxCh | 0x5844 | pstm_set_fx_ch 0x6F64 |
| 0x2A | sdPstmSetFxLev | 0x5860 | pstm_set_fx_lvl 0x6FF8 |
| 0x2B | sdPstmSetDrctLev | 0x5878 | pstm_set_direct_lvl 0x7080 |
| 0x2E | sdPstmResetPrm | 0x5890 | pstm_reset_prm 0x80EC / 0xFF: pstm_reset_prm_all 0x80E0 |
| 0x2F | sdPstmStopAll | 0x58BC | pstm_stop_all 0x7F80 |
| 0x80 | sdSndStopAll | 0x5968 | [3] -> shot_stop_all, [4] -> pstm_stop_all |

Host 0x1C modes (loops_left computed by shot_loops_apply, now if the port has any flag set,
otherwise at the next play): 0 -> loops_default + param, 1 -> loops_default * param / 256,
2 -> param. Stored as loop_mode = mode+1; mode > 2 -> err 0x20.

Error bits (PcmPlayer.err -> status +0x1C): 0x01 stream ring bank address 0, 0x02 priority
(port playing with a higher priority), 0x04 data number > count, 0x20 bad 0x1C mode,
0x80 no AICA channel. The 0x11 handler reports bad banks in status +0x1F: 0x02 bad magic,
0x04 no "ENDB", 0x08 bad version (the DRV_ERR_* bit values).

## 5. Channel allocation (pcm_slot_alloc 0x7B68)

1. Player already playing: re-use (retrigger) its own slot unless both priorities are non-zero
   and the new one is lower (err 2).
2. First free slot.
3. All busy: slots 15..0 are scanned; a prio-0 slot is taken immediately; otherwise the victim
   is the lowest prio (ties: lower kind, then lower index). A new prio of 0 or higher than the
   victim's steals it; lower fails (err 0x80); equal also fails (the compare adds kind instead of
   kind<<8). If this was a stream's ch1, ch0 (already programmed, KYONB set but not executed) is
   released first; the error bit still lands on ch1 (pstm_release leaves r12 = ch0 + 0x58).
4. Stealing stops the owner (one-shot: release; stream: both channels) and keys it off.

## 6. Quirks worth knowing

- pcm_slot_alloc: when the victim comes from the priority scan (step 3, not the prio-0 early
  exit) the stored `pl->slot` is 0xFF (r9 = -1 after the loop). Slot entry and the first channel
  programming are right, but every later look-up by pl->slot (tick, stop, fades) indexes past
  g_pcm_slot (entry 255 at 0x12C74), and MSLC is set to 0x2F (MIDI voice channel 47).
- pstm_release assumes channel 0; stealing a stream's channel 1 also releases the next player.
- pstm_play mono on a port whose ch1 is still playing frees ch1's slot but leaves its PLAY flag.
- A stream ch1 with no ring bank leaves ch0 allocated with KYONB set but not executed; the next
  KYONEX anywhere (e.g. a MIDI note) starts it.
- shot_pause keys the sample off and shot_continue keys it on again from the start (no real pause).
- shot_reset_prm_all resets 16 players from 0x11000: the 8 one-shots and stream ports 0-3
  (clearing their ring_len and loop_mode).
- Reset paths write IMXL = 0 (imxl shifted twice).
- FxCh with in_ch 0xFF stores the low byte of a stale r4 (0x0C from host_cmd_exec, i.e. "no
  override") into fx_ch of every channel. For streams with both channels playing, r1 is replaced by
  ch0's ISEL during the first pass, so ch1's dsp_send register gets ch0's ISEL and IMXL from the
  FxCh base level (register only; its fx_ch byte still says "no override").
- pcm_apply_pitch clamps negative (below-range) pitch to the top octave.
