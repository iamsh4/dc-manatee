# MIDI sequence data format (SMSB / SMSD) as played by the driver

Source: `seq_play` (0x747C), `seq_tick` (0x4270) and host command 0x01 (0x4B9C); C in
`src/arm/sequencer.c`. The format was checked by parsing every sequence in the real banks
(`CDATA.DAT` @0x80 and @0x77780, `CINIT.DAT` @0x21FB60 and @0x28AB60, 22 sequences). Each one
parses cleanly up to its 0x83 end marker, which is followed by the `ENDD` tag.

All multi-byte values inside the event stream are **big-endian**. Header words are little-endian u32.

## Bank (`SMSB`)

The bank is downloaded by the SH-4. Its address is in the bank table at `0x14000 + bank*8` = {addr, size}.

| Offset | Content |
| --- | --- |
| 0x00 | `"SMSB"`. Checked by the host play command, error bit 0x02 in `PortState.err_hi`. |
| 0x04 | Version. Must be 1 (error bit 0x08). |
| 0x08 | Bank size. `"ENDB"` must be at `size-4` (error bit 0x04). |
| 0x0C | Number of sequences. `song >= count` gives `SDD_PORT_ERR_REQUEST_NUM` (0x04). |
| 0x10 | `u32 offset[count]`: the offset of each `SMSD` from the bank start. |
| ... | The sequences, each ending with `"ENDD"`, then `"ENDB"`. |

Sequence *n* ends at `offset[n+1]`. There is no end entry for the last sequence: its `offset[count]`
slot is really the `"SMSD"` tag of sequence 0. The driver detects that value and uses `size-4`
(the position of ENDB) instead. The data parser stops at the end address, or earlier at meta 0x83.

## Sequence (`SMSD`)

| Offset | Content |
| --- | --- |
| 0x00 | `"SMSD"` (not checked) |
| 0x04 | `timebase` = 65536 / ticks-per-beat (0x88 = 136, so about 480 ticks per beat) |
| 0x08 | initial tempo = milliseconds per beat (e.g. 125; the data then sets the real tempo with 0x84) |
| 0x0C | event stream (a single track; all 16 MIDI channels are interleaved) |

## Timing

- Every 4 ms each playing port subtracts `tempo_scale` from its `delta` counter:
  - `tempo_scale` is 0x4000 at normal speed.
  - It comes from the port speed: `max(0, (speed & 0xFFFF) - 0x4000)`, where speed is
    0x8000-biased and the library sends `speed*4 + 0x8000` (so the rate is `1 + speed/4096`;
    speed <= -0x1000 stops the sequence). `port_speed_fade` only writes it on a fade step.
- One sequence tick is `tick_len = timebase * tempo >> 4` units. This gives
  `tick = tempo_ms / (65536/timebase)` ms, i.e. ticks-per-beat = 65536/timebase at every tempo.
- When the old `delta <= tempo_scale` (i.e. the new `delta <= 0`), events are parsed and executed
  until one of them carries a non-zero delta and the new delta is > 0. The new delta is
  `d * tick_len` (+ a pending long-delta prefix) plus the (<= 0) remainder, so no rounding error
  builds up. The test is **unsigned**: a negative stored delta (only possible after a ring-full
  abort, see below) makes the player wait until it wraps, about 17 minutes at 1x.
- Total beats (status +0x14): a countdown starts at `tempo`, drops by `tempo_scale >> 12` per 4 ms
  (4 at 1x), and adds `tempo` again for each beat counted.

## Events

Every event except the no-ops and the meta prefixes ends with a **delta time** to the next event.
It is 1 byte, or 2 bytes big-endian where a flag says so. Delta 0 means "execute the next event now".

| First byte | Bytes | Meaning |
| --- | --- | --- |
| `0 ll w cccc` (0x00-0x7F) | `note vel gate[ll+1] delta[1+w]` | **Note**. Emits a note-on (ch `cccc`, `note`, `vel`) and queues its note-off after `gate` ticks (1-4 bytes BE). `w` = 2-byte delta. |
| 0x80 | 1 | no-op |
| 0x81 | `hi lo n` | **Call**: continue at `SMSD + (hi<<8 \| lo)`, play `n` events there, then return after the operands. Every event that goes through the delta stage counts (notes, channel messages, 0x82, 0x84); no-ops, 0x81 and 0x88-0x8F do not. `n` = 0 never returns. No nesting (a second 0x81 overwrites the return point). |
| 0x82 | `cnt delta[1+(cnt>>7)]` | **Loop marker** (one level). The first 0x82 marks the loop start and sets the count `cnt & 0x7F` (0 = forever). Each later 0x82 is a loop end: while the count is > 0 it jumps back to just after the start marker, decrements the count, bumps `loop_iter` (status +0x0C) and re-reads the *start* marker's cnt/delta. At count 0 it falls through and clears the loop. The body therefore plays `cnt + 1` times. Host command 0x0C (`seq_break_loop`) zeroes the count. |
| 0x83 | 1 | **End of sequence.** Clears the player and `PortState` PLAY. |
| 0x84 | `hi lo delta[1]` | **Tempo** = ms per beat (BE16). |
| 0x85-0x87 | 1 | no-op |
| 0x88-0x8B | 1 | Long gate prefix: adds `{0x200, 0x800, 0x1000, 0x2000}[b&3]` ticks to the next note's gate (table 0x47A4). Prefixes accumulate. |
| 0x8C-0x8F | 1 | Long delta prefix: adds `{0x100, 0x200, 0x800, 0x1000}[b&3]` ticks to the next **non-zero** delta of any event (table 0x47B4). Prefixes accumulate. |
| 0x9n | 1 | no-op. Raw note-on status is not used; notes use the compact form. |
| 0xAn / 0xBn | `d1 d2 delta` | Poly pressure / control change. `d1` bit 7 = 2-byte delta; `d1 & 0x7F` is the data byte. |
| 0xCn / 0xDn | `d1 delta` | Program change / channel pressure. `d1` bit 7 = 2-byte delta. |
| 0xEn | `v delta[1]` | Pitch bend. The event gets data1 = 0 and data2 = `v`. |
| 0xFn | 1 | no-op |

## Emitted events

- The port is part of the 32-bit ring event (manatee.h), in bits 22:20.
- Event bits 31:27 = port priority (the host sends `prio<<3`; `seq_play` clears its low 3 bits).
- Note-offs are velocity-preserving copies of the note-on with the type bit cleared. They wait in
  the 48-entry queue at 0x10300 and count down by `tempo_scale` every 4 ms (frozen while paused).
- The gate counts from the note's own time: `gate * tick_len` (+ long-gate prefix) plus the
  current (<= 0) delta remainder.
- When the queue is full (error 0x80), a note is stolen: the first entry with priority 0,
  otherwise the **last** entry with the lowest priority (ages are tracked but not used). Its
  note-off goes into the 60-entry overflow queue at 0x10210 and is sent just before the new
  note-on. If that queue is full too, the sequencer stalls (error 0x60, see below) and the asm
  returns through an unbalanced stack (jumps to address = port number).
- Every emitted event first drains the overflow queue into the event ring. If the ring fills
  (error 0x20): an event interrupted while draining is appended to the overflow queue, but a new
  event that was being pushed directly (queue already empty) is **dropped**; the player's
  position and delta are saved and the next tick resumes at this port (lower ports skip that
  tick). If the overflow queue is full, `g_seq.stalled` = 1 (error 0x60) and the sequencer does
  nothing until `seq_init` (host 0x8E/0x8F or reboot).

## Status seen by the SH-4 (0x13600 + port*0x20)

| Offset | Content |
| --- | --- |
| +0x0C | `loop_iter` |
| +0x14 | total beats |
| +0x18 | offset of the next event from the SMSD header (`sdMidiGetCurAdr`) |

The rest of the record is described in `manatee.h` (`PortStatus`).
