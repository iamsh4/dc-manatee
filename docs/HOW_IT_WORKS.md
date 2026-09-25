# How the Manatee sound driver works

This document explains **Manatee**, the AICA (ARM7DI) sound driver Sega shipped with the Katana SDK and
that many Dreamcast games load into the sound processor. The build analysed here is the one shipped in
Soul Calibur (Dreamcast, USA, T1401N): v1.1 build 0x3F, dated 1999-03-18. Concepts apply to Manatee in
general; version-specific details (bugs, codes, layouts) may differ in other games' builds. A browsable,
illustrated version is in `site/index.html`. It is based on a complete logical decompilation of the driver (`src/arm/*.c`), the
annotated Ghidra project (`ghidra/Manatee.gpr` (built by `make ghidra`)) and the SH-4 side of Sega's sound library
(`docs/research/host_protocol.md`). Detailed data formats are in `docs/notes/`.

## 1. What it is

| | |
| --- | --- |
| File | `CINIT.DAT`, top-level olnk entry 2, child 0: an `SDRV` container (32-byte header + 0x8300-byte image). `tools/extract_sdrv.py` extracts it to `bin/manatee_arm.bin`. |
| SDK name | `manatee.drv` (the name the game's executable refers to); Sega "sd" sound library 1.00.18 driver |
| Version | 1.1 build 0x3F (bytes at image offset 0x20) |
| Credits | `1998,(C)SEGA ENTERPRISES` / `1999.03.18:DIGITALMEDIA :Y.Kashima / K.Suyama` |
| CPU | ARM7DI (ARMv3, no Thumb, no halfword loads) inside the AICA (nominally 22.58 MHz, heavily slowed by sound-RAM contention), running from the AICA's 2 MB sound RAM |
| Language | Hand-written ARM assembly: no ABI, values passed in arbitrary registers, scratch variables embedded in the code image, routines that fall into each other or return to `lr+4` |

The driver is a small real-time operating kernel for the AICA. It plays:

- **MIDI** music and effects through 8 "MIDI ports" (sequencer players driving 16 MIDI channels
  each), using tone banks (`SMPB`) and sequence banks (`SMSB`), on AICA channels 0-47;
- **one-shot** sampled sound effects through 8 one-shot ports (`SOSB` banks), and
- **PCM streams** (up to 4 mono/stereo streams from ring buffers the SH-4 keeps filling), both on
  AICA channels 48-63;
- the AICA **DSP** effect programs (`SFPB`, reverbs) and their output mix (`SFOB`).

How the game divides its material between these is outside the driver, but the evidence points one way: the
music files (`*.P16/P08/P04`, raw 16/8/4-bit PCM/ADPCM left/right pairs) match what the PCM stream ports play,
and `CINIT.DAT` carries two one-shot banks (61 and 53 samples, all ADPCM) and small MIDI sequence/tone banks.
This split is an inference, not verified by tracing the SH-4 game code.

## 2. Boot and the SH-4 relationship

The SH-4 library (`sdDrvInit`) holds the ARM in reset (AICA register 0x2C00 bit 0), fills all of
sound RAM with the word `"SEGA"`, copies the driver image to sound RAM address 0 (also copying 0x20
bytes past the image, because it uses a size that includes the container header) and releases the
reset. There is no handshake; the SH-4 just waits and reads the version.

The ARM starts at the reset vector (0x0000 `b drv_reset`):

1. `drv_reset` (0x100): SVC stack at 0xB000, FIQ masked, `drv_status` (0xF8) = 0xFF.
2. Silence all 64 AICA channels (key off, instant release), master volume 0.
3. Clear all work RAM: host area 0x13000-0x17FFF, event ring, port/channel/voice state; install the
   built-in banks in the bank table; mute the DSP outputs; clear the DSP; initialise the sequencer and
   the PCM players; initialise FX status.
4. Master volume 15, drain the MIDI-in FIFO, program the interrupt controller and start Timer A.
5. `drv_status` = `"SEGA"`, unmask FIQ, enter the main loop, forever.

## 3. Execution model: one interrupt, one loop

```
                FIQ (only interrupt the AICA gives the ARM)
                 |-- level 2: Timer A every 44 samples  -> tick1ms_flag, tick4ms_flag
                 '-- level 5: MIDI-in FIFO               -> parse bytes -> event ring (port 7)

 main loop (drv_main):
   every pass:        pop and execute up to 4 MIDI events from the ring
   1 ms flag:         sample one voice's envelope (round robin over 48 voices)
   4 ms flag:         sample envelope
                      execute host commands (if the SH-4 raised the pending flag)
                      PCM players (one-shots/streams): fades, loop/stop detection
                      sequencer: advance 8 MIDI ports, push due events to the ring
                      port fades: speed, volume, pan, pitch
                      voices: delayed key-ons, re-apply port volume/pan/pitch
                      export port status for the SH-4
                      PCM speed/pitch fades, pitch registers
                      DSP ring buffer / FX status, queued DSP program load
                      advance the envelope monitor
```

- **Timing** (`irq.c`). Timer A counts samples at 44.1 kHz; a reload of 0xD4 fires every 44 samples
  (0.998 ms). The "1 ms" flag is raised on every interrupt. For the 4 ms clock, one interrupt in 441
  is not counted, so 440 counted ticks take 441 x 44 = 19404 samples = exactly 0.44 s: the 4 ms tick
  that drives all musical timing is exact. `tick4ms_count` (0x13418) is the "execute counter" the
  SH-4 can read with `sdDrvGetExecuteCounter`.
- The FIQ handler runs with the banked FIQ registers and a private 0x60-byte stack inside the image,
  and writes the AICA `INTCLR` register four times before returning (the chip needs repeated writes
  to reliably drop the line).
- All other exception vectors go to a stub that returns to `lr-4`; for an undefined instruction or
  SWI that re-executes the faulting instruction forever, so it is effectively a crash trap.
- The SH-4 never interrupts the ARM. Everything between the two CPUs is polled shared memory.

## 4. Memory map (sound RAM as seen by the ARM)

| Address | Size | Content |
| --- | --- | --- |
| 0x00000 | 0x8300 | driver image: vectors, header, code, literal pools, built-in banks and tables (`docs/notes/data_map.md`) |
| 0x000C0 | 0x20 | `drv_layout_table`: work-area addresses the SH-4 library reads (0x13F00, 0x13600, 0x13400, 0x14000, 0x13200, 0x14800, 0x13E00, 0x100) |
| 0x000F8 | 4 | `drv_status`: 0xFF initialising, `"SEGA"` running |
| ...-0x0AFFF | | SVC stack (grows down from 0xB000) |
| 0x0B000 | 4 KB | MIDI event ring, 1024 x u32 |
| 0x0C000 | 8 x 0x80 | `PortState`: per MIDI port volume/pan/pitch/speed with fades, status, errors |
| 0x0C400 | 128 x 0x48 | `MidiChannel`: 8 ports x 16 channels of controller state |
| 0x0E800 | 48 x 0x40 | `Voice`: voice n drives AICA channel n |
| 0x10000 | 8 x 0x40 | `SeqPlayer`: sequence player per MIDI port |
| 0x10200 | | sequencer globals and a 60-entry overflow event queue |
| 0x10300 | 48 x 12 | pending sequencer note-offs |
| 0x11000 | 24 x 0x58 | `PcmPlayer`: 8 one-shot ports, then stream port p channel c at 8+2p+c |
| 0x12080 | 16 x 12 | `PcmSlot`: ownership of AICA channels 48-63 |
| 0x13000 | | ring offsets, FX load request, misc |
| 0x13200 | 32 x 16 | host command slots (written by the SH-4) |
| 0x13400 | | system block: command-pending flag, counters, mono flag, driver error (0x13420), FX state |
| 0x13600 / 0x13800 / 0x13A00 | 0x20 each | port status records for MIDI / one-shot / stream ports (read by `sd*GetStat`) |
| 0x13F00 | | FX/DSP external request block |
| 0x14000 | | bank table: `{addr,size}` pairs written by the SH-4: +0 SMSB, +0x80 SMPB, +0x100 SOSB, +0x180 stream rings, 0x14200 SFPB, 0x14280 SFOB, 0x14288 SFPW (DSP ring buffer) |
| 0x14800 | 2 KB | host command history ring (debug log, never read by the SH-4) |
| 0x18000- | | free for banks, sample data, stream rings and the DSP ring buffer (managed entirely by the SH-4) |

## 5. The host interface

**Downloads.** Banks are copied into sound RAM by the SH-4 (CPU or G2 DMA) and announced only by
writing `{address, size}` into the bank table; no command is sent. The driver validates a bank when
it is used: magic tag, version byte 1, and the `"ENDB"` tag at `size-4`.

**Commands.** Once per frame the SH-4 library (`sdSysServer`) copies up to 32 queued 16-byte
commands to 0x13200 and sets byte 0x13400 = 1. Every 4 ms the driver checks that byte; if set,
`host_cmd_poll` scans all 32 slots, executes every slot whose code byte is non-zero, logs it to the
history ring at 0x14800, clears the slot and finally clears 0x13400 (which is what lets the SH-4 send
the next batch). The low nibble of a command code selects the operation, the high nibble the port
family:

| Codes | Family | Operations (low nibble) |
| --- | --- | --- |
| 0x01-0x0F | MIDI ports | 1 play, 2 stop, 3 pause, 4 continue, 5 volume, 6 pan, 7 speed, 8 pitch, A fx level, B direct level, C break loop, E reset parameters, F stop all |
| 0x11-0x1F | one-shot ports | same, plus C loop mode |
| 0x21-0x2F | PCM stream ports | same (1 play takes format, frequency, rings) |
| 0x35-0x3E | GD-DA (CD audio into the AICA) | 5 volume, 6 pan, E reset |
| 0x80-0x8F | global | 80 stop all, 81 master volume, 82/83 FX output set/params, 84 select FX program, 85 clear FX, 86 send a raw MIDI event, 87 drum mode, 88 DSP pan positions, 8A mono/stereo, 8E re-initialise, 8F reboot |

Levels travel as signed value + 0x80, pitch and speed as value + 0x8000, fades as a time in ms that
the driver converts to a step and an interval in 4 ms ticks. Full layouts: `docs/research/host_protocol.md`
and the per-command comments in `src/arm/host.c`.

**Status.** Every 4 ms `port_status_export` writes a 32-byte status record per port (flags
play/pause/fading, volume, pan, speed, pitch, levels, total beats or samples, current address, error
bits) which the SH-4's `sdMidiGetStat` etc. read. Bank/driver errors go to 0x13420 (`sdDrvGetErr`);
the bit values are exactly the SDK's `SDD_DRV_ERR_*` constants shifted right by 3.

## 6. The event ring: one MIDI engine for everything

Every musical action is expressed as a packed 32-bit MIDI event and appended to the ring at 0xB000:

```
[31:27] priority/flags  [26:24] type  [22:20] port  [19:16] MIDI channel  [15:8] data1  [7:0] data2
type: 0 note-off, 1 note-on, 2 poly pressure, 3 control change, 4 program change,
      5 channel pressure, 6 pitch bend, 7 system
```

Producers: the sequencer (ports 0-7), host command 0x86 (`sdMidiSendMes`, raw MIDI from the game)
and the MIDI-in interrupt (port 7). The only consumer is the main loop, which executes up to four
events per pass through `midi_event_dispatch`. The sequencer checks for a full ring (and spills into
its own overflow queue); the other producers do not.

## 7. The MIDI engine

**Channels** (`MidiChannel`, `src/arm/event_ctl.c`) hold standard controller state (volume,
expression, pan, modulation, sustain, bend and bend range, RPN/NRPN) plus a set of Sega-specific
controllers that offset the tone's AICA parameters directly: filter levels FLV0-4 (CC20-24/52-56),
filter envelope (CC25-28), cutoff (CC74 with CC75 range, CC110-117), resonance (CC71/85), envelope
attack/decay/release (CC72/73/86-89), DSP input select and send (CC76/91), and on port 7 the DSP
output mix and effect program (CC77-79). Bank select is CC32 (CC0 is ignored); CC48 drives the
"MIDI timing counter" the SH-4 can read. Poly/channel pressure and system messages are ignored.

**Tone banks** (`SMPB`, `docs/notes/bank_format.md`): program -> up to 4 layers -> splits. A split
is a key/velocity window plus a 16-bit image of every AICA channel register (sample address, loop,
envelopes, LFO, filter, pan, send levels), a root key and fine tune.

**Note-on** (`note_on`, `src/arm/voice.c`): for each layer of the channel's program, the first split
whose window contains the note gets a voice. The voice's AICA channel is programmed from the split's
register image, with every parameter adjusted by the channel's controller offsets and the port's
volume/pan/levels, and keyed on (or keyed on later if the layer has a key-on delay).

- *Level:* `gain = (curve[velocity] + 1) * (channel level + 1) * (256 - TL) >> 14`, then
  `TL = 0xFF - clamp(gain + port volume - 0x80)`.
- *Pitch:* a 1/256-semitone offset from the root key (port pitch + bend * range + fine tune) is
  turned into AICA OCT/FNS with two tables in the image (note -> octave/semitone, semitone fraction
  -> FNS).

**Voice allocation** (`voice_alloc`): a completely idle voice is taken at once; voices with a higher
priority than the new note are never stolen; otherwise released voices are preferred (the quietest wins),
then lower priority and older voices. Because of the bug in section 12 the allocator never actually reports
"no voice". "Quietness" comes from the envelope monitor: the AICA shows the envelope of one channel at a
time, and the driver advances that monitor channel every millisecond, so each voice is observed
every 48 ms.

**Every 4 ms** `voice_update_4ms` executes delayed key-ons and re-applies port volume, pan and pitch
changes (fades) to the sounding voices.

## 8. The sequencer

Sequences (`SMSB` banks of `SMSD` sequences, `docs/notes/sequence_format.md`) are a single stream of
events with big-endian operands. Notes use a compact form (note, velocity, gate length, delta) and
the driver generates the note-off itself from a 48-entry pending note-off queue. Meta events provide
a subroutine call (play n events elsewhere), a one-level loop, tempo changes and gate/delta
extensions. Timing: every 4 ms each port subtracts its speed (0x4000 = 1x) from a delta counter;
one tick is `timebase * tempo >> 4` units, which keeps ticks-per-beat constant at any tempo and
accumulates no rounding error. Each port has a priority that becomes the priority of its notes in
voice allocation, so sound-effect ports can pre-empt music.

## 9. One-shots and PCM streams

AICA channels 48-63 form a pool of 16 slots shared by the one-shot and stream players, allocated by
priority (`pcm_slot_alloc`).

- **One-shot** (`SOSB`, `docs/notes/oneshot_pstm_format.md`): each entry is a ready-made AICA channel
  register image (the sample address is relocated by the bank address) plus a loop count. Play copies
  it to the channel, applies the port's volume/pan/levels/pitch and keys on. Every 4 ms the player
  watches its channel (through the same envelope monitor) to count loops and detect the end.
- **PCM stream**: the SH-4 keeps a ring buffer in sound RAM filled; the driver programs a looping
  channel over the whole ring (ADPCM uses the AICA's long-stream mode), keys both channels of a
  stereo pair on together, and exports the current play position so the SH-4 knows where it may
  write. This is how the game's streamed music is played.

## 10. Effects (DSP)

An `SFPB` bank holds DSP programs (the game uses three reverbs: "outside", "inside" and "BGM"; the
driver has a built-in "e-reverb" at 0x2908). Selecting a program (host 0x84 or CC79) validates it and
queues it; on the next 4 ms tick the loader fades the DSP outputs out, clears the DSP ring buffer
(`SFPW`, a work area the SH-4 allocates, filled with the DSP's floating-point zero 0x60006000), copies
COEF/MADRS/MPRO to the DSP in one 0x300-word loop, points RINGBUF at the work area and fades the
outputs back in. `SFOB` banks hold output mixes (level and pan for each of the 16 DSP outputs). The
format details are in `docs/notes/fx_format.md`.

## 11. MIDI input

The AICA's MIDI-in port is not connected on a retail Dreamcast; the driver nevertheless enables its
interrupt. The FIQ drains the FIFO through a running-status MIDI parser and pushes channel messages
into the event ring as port 7. This is presumably how the sound designers auditioned and tweaked
tones and effects live from an authoring tool on development hardware — which also explains why the
DSP output/program controllers (CC77-79) only work on port 7.

## 12. Bugs and quirks in the original

The decompilation reproduces the original's behaviour, including its bugs. The notable ones:

- **Voice allocation with all voices busy** (`voice_alloc`, 0x1250): the pointer to the "best
  candidate" record is left just past the record and only reset when some voice passes the priority
  test. If all 48 voices have a higher priority than the new note, the driver reads two of its own
  instruction words as the chosen voice and AICA channel and "succeeds", programming garbage
  addresses.
- **FX program with too small a ring buffer** (`fx_prg_select`, 0x5D58): the error code reported is
  the table index (0/4/8/12) instead of "not downloaded", so the error is usually misreported or lost.
- **Host command log**: every empty slot also writes a result byte, so the last command of each batch
  has its logged result overwritten with 0 (harmless: nothing reads the log).
- **RPN**: data entry never compares the RPN number, so any RPN data entry sets the pitch-bend range.
- **Sequencer overflow**: when both the note-off queue and the overflow queue are full, the steal path
  leaves a register on the stack and "returns" to address `<port number>` (ports 0-3 land on the reset
  vector, 4-7 on the undefined-instruction vector).
- **Sequencer stall**: if the ring fills while a port handles a zero-delay event, its delay counter goes
  negative and is tested unsigned, so that port stops for about 17 minutes.
- **PCM slot stealing** (`pcm_slot_alloc`): a slot stolen through the priority scan is recorded as slot
  0xFF (the loop counter), so later lookups index far past the 16-entry slot table.
- **FX readback**: the driver keeps the current FX program/output at 0x13474/0x13475, while the game's sd
  library reads 0x134E0/0x134E1.
- Many smaller ones (downward fades that jump to the target, a pan fade that sets the volume-change
  flag, byte-wide counters that wrap, off-by-one bank index checks, …) are commented at the place they
  occur in the C source and in Ghidra.

## 13. Source map

| File | Content |
| --- | --- |
| `src/arm/aica.h`, `types.h` | AICA register definitions, fixed-width types |
| `src/arm/manatee.h` | memory map, all work-RAM structures (with offset checks), prototypes |
| `main.c` | vectors, reset, boot sequence, main loop |
| `irq.c` | exception stub, FIQ handler, Timer A clock, interrupt controller setup |
| `init.c` | boot-time clearing and defaults |
| `midi_in.c` | MIDI-in parser |
| `event.c` | event ring consumer and dispatcher |
| `voice.c` | note on/off, voice allocation, pitch, per-voice update, FX program select |
| `event_ctl.c` | controllers, program change, pitch bend |
| `monitor.c` | envelope monitor round robin |
| `sequencer.c` | sequence players, note-off queue, overflow queue |
| `port.c` | port fades, PCM player tick, status export |
| `host.c` | host command poll and dispatcher |
| `host_helpers.c` | one-shot and PCM stream commands, slot allocation |
| `fx.c` | DSP program loading, output mix, global host commands |
| `tables.c` | tables and built-in banks extracted from the image (`tools/gen_tables.py`) |

`make` compiles every file for ARMv4 (`clang --target=arm-none-eabi`); `make link` links the whole
driver, proving every call resolves. The reconstruction is logical, not matching: register-passing
conventions become C parameters and image-embedded scratch words become statics.

Each function carries a `Confidence:` rating in its header comment (and in its Ghidra plate):
**high** = every path checked against the disassembly, **medium** = structure right, some details
inferred, **low** = partial. See section 14 for the tally.

## 14. Confidence and open questions

Every file was decompiled by one agent and then re-derived path by path from the disassembly by an
independent reviewer, who fixed discrepancies (e.g. CC96's dependence on a stale carry flag, the 8-bit
stream position, 18 host-command result values, a delay loop off by one) and set the ratings.

| | |
| --- | --- |
| Ghidra functions | 185, all named, register-accurate signatures, no generic names left in reviewed code |
| C function headers | 181: **180 high**, 1 medium (`ctl_reset_port` 0x28C8, an unreferenced alternate entry) |
| Build | `make` (ARMv4, no warnings) and `make link` (no unresolved or duplicate symbols) |
| Tables | all 20 tables in `tables.c` match the image byte for byte |

Open questions (not answerable from the driver code):

- how the MIDI-in interrupt is released without an explicit acknowledge, and why INTCLR is written 4/8 times;
- who writes the "external FX request" bytes at 0x13F00/0x13F80/0x13F94 (not the game's sd library);
- whether anything reads `sndram_free_base` (0x13408);
- what the game actually does with the driver (which songs, reverbs and streams when): SH-4 game code, not traced;
- how other Manatee builds differ from this one.
