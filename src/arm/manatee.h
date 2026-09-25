/*
 * manatee.h - Soul Calibur (DC, USA) AICA ARM7DI sound driver ("manatee.drv",
 * SDRV container in CINIT.DAT, credits "1999.03.18:DIGITALMEDIA :Y.Kashima / K.Suyama").
 *
 * Logical C reconstruction of a hand-written ARM assembly driver. This is NOT a
 * matching decompilation: register-passing conventions of the original routines
 * are expressed as ordinary C parameters/returns, and state that the original kept
 * at fixed sound-RAM addresses is expressed as overlays at those addresses.
 *
 * Sound RAM map (ARM addresses):
 *   0x00000-0x082FF  driver image (code, literal pools, built-in tables/banks)
 *     0x000000C0     drv_layout_table: 8 work-area pointers published to the SH-4
 *     0x000000F8     drv_status: 0xFF while initialising, "SEGA" when running
 *     0x000000FC     midi_irq_flag
 *     0x00000368-3CB FIQ stack (inside the image)
 *   0x0B000 (below)  SVC stack top (stack grows down from 0xB000)
 *   0x0B000-0x0BFFF  evq_ring: 1024 x u32 MIDI event ring
 *   0x0C000-0x0C3FF  g_port[8]: MIDI port state (0x80 each)
 *   0x0C400-0x0E7FF  g_midi_ch[8][16]: MIDI channel state (0x48 each)
 *   0x0E800-0x0F3FF  g_voice[48]: voice state (0x40 each)
 *   0x10000-0x107FF  sequencer: g_seq_player[8] 0x10000, g_seq 0x10200 (+ overflow queue
 *                    0x10210), g_seq_note[48] 0x10300 (see sequencer.c)
 *   0x11000-0x1183F  g_pcm_player[8 one-shot + 16 stream] (0x58 each)
 *   0x12080-0x1213F  g_pcm_slot[16]: AICA channels 48-63
 *   0x13000-0x17FFF  host interface (cleared at boot):
 *     0x13000/04     event ring write/read offsets
 *     0x13200        host command slots (32 x 16 bytes, written by SH-4)
 *     0x13400        host_cmd_pending flag (SH-4 sets, driver clears)
 *     0x13600        port status published to the SH-4
 *     0x13E00, 0x13F00  see drv_layout_table
 *     0x14000        bank address table
 *     0x14800        host command history ring
 *   0x18000-0x1FFFFF free for sound banks downloaded by the SH-4
 */
#ifndef MANATEE_H
#define MANATEE_H

#include "types.h"
#include "aica.h"

#define SRAM(type, addr) (*(volatile type *)(u32)(addr))
#define SRAM_PTR(type, addr) ((type *)(u32)(addr))

/* ------------------------------------------------------------------------- */
/* Limits                                                                    */
/* ------------------------------------------------------------------------- */
#define NUM_PORTS        8    /* SDD_MIDI_PORT_MAX */
#define NUM_MIDI_CH      16
#define NUM_VOICES       48   /* AICA channels 0..47; 48..63 left for SH-4 streaming */
#define EXT_MIDI_PORT    7    /* external MIDI-in is routed to port 7 */

/* ------------------------------------------------------------------------- */
/* Packed MIDI event (event ring at 0xB000)                                  */
/*   [31:27] flags (0x1F for external MIDI in)                               */
/*   [26:24] type: 0 note-off 1 note-on 2 poly-pres 3 CC 4 prog 5 ch-pres    */
/*           6 pitch-bend 7 driver-private/system                            */
/*   [22:20] port   [19:16] MIDI channel   [15:8] data1   [7:0] data2         */
/* ------------------------------------------------------------------------- */
#define EV_FLAGS(ev)  (((ev) >> 27) & 0x1F)
#define EV_TYPE(ev)   (((ev) >> 24) & 7)
#define EV_PORT(ev)   (((ev) >> 20) & 7)
#define EV_CH(ev)     (((ev) >> 16) & 0xF)
#define EV_DATA1(ev)  (((ev) >> 8) & 0xFF)
#define EV_DATA2(ev)  ((ev) & 0xFF)

enum {
    EV_NOTE_OFF = 0, EV_NOTE_ON, EV_POLY_PRES, EV_CONTROL,
    EV_PROGRAM, EV_CH_PRES, EV_PITCH_BEND, EV_SYSTEM
};

#define EVQ_RING      SRAM_PTR(volatile u32, 0xB000)
#define EVQ_MASK      0xFFFu
#define evq_write_ofs SRAM(u32, 0x13000)
#define evq_read_ofs  SRAM(u32, 0x13004)

/* ------------------------------------------------------------------------- */
/* MIDI port state, 8 x 0x80 at 0xC000 (owner: port.c).                      */
/* Each of vol/pan/pitch/speed has a target set by a host command, a current */
/* value stepped towards it every 4 ms by the port_*_fade routines: every     */
/* <interval>+1 ticks the current value moves <step> (scaled) towards the     */
/* target; interval 0 = jump. Neutral value 0x80 / 0x8000 everywhere.         */
/* ------------------------------------------------------------------------- */
typedef struct PortState {
    u8  vol_target;  /* 0x00 host SetVol target (vol+0x80) */
    u8  volume;      /* 0x01 port volume offset, 0x80 = 0 (added to Voice.vol before TL) */
    u8  vol_interval;/* 0x02 fade: ticks between steps (0 = jump to target) */
    u8  vol_count;   /* 0x03 fade: tick countdown */
    u8  vol_step;    /* 0x04 fade: step per update */
    u8  vol_dirty;   /* 0x05 !=0: voice_update_4ms re-applies volume to this port's voices */
    u8  pan_target;  /* 0x06 */
    u8  pan;         /* 0x07 port pan offset, 0x80 = 0 (added to Voice.pan) */
    u8  pan_interval;/* 0x08 */
    u8  pan_count;   /* 0x09 */
    u8  pan_step;    /* 0x0A */
    u8  pan_dirty;   /* 0x0B !=0: voice_update_4ms re-applies pan */
    u32 pitch_target;/* 0x0C host SetPitch target (pitch+0x8000) */
    u32 pitch;       /* 0x10 port transpose in 1/256 semitone, biased by 0x8000 */
    u8  pitch_interval; /* 0x14 */
    u8  pitch_count; /* 0x15 */
    u8  pitch_step;  /* 0x16 step is pitch_step*2 */
    u8  pitch_dirty; /* 0x17 !=0: voice_update_4ms recomputes pitch */
    u32 speed_target;/* 0x18 host SetSpeed target (speed*4+0x8000) */
    u32 speed;       /* 0x1C current speed; SeqPlayer.tempo_scale = max(0, speed-0x4000) */
    u8  speed_interval; /* 0x20 */
    u8  speed_count; /* 0x21 */
    u8  speed_step;  /* 0x22 step is speed_step*16 */
    u8  _23;
    u8  direct_lvl;  /* 0x24 DISDL offset: ((x>>3)&0x1F)-16 added to tone DISDL; init 0x80 */
    u8  _25;
    u8  fx_send;     /* 0x26 IMXL offset: ((x>>3)&0x1F)-16 added to tone IMXL; init 0x80 */
    u8  _27;
    u8  status;      /* 0x28 PORT_FLG_* (SDD_PORT_FLG_*), exported to the SH-4 */
    u8  err;         /* 0x29 SDD_PORT_ERR_* of the last command (copied from g_seq.err) */
    u8  err_hi;      /* 0x2A more error bits set by the host play command (exported at +0x1F) */
    u8  _2b[0x80 - 0x2B];
} PortState;
SIZE_CHECK(PortState, 0x80);
OFFSET_CHECK(PortState, vol_dirty, 0x05);
OFFSET_CHECK(PortState, pan_target, 0x06);
OFFSET_CHECK(PortState, pan_dirty, 0x0B);
OFFSET_CHECK(PortState, pitch_target, 0x0C);
OFFSET_CHECK(PortState, pitch, 0x10);
OFFSET_CHECK(PortState, pitch_dirty, 0x17);
OFFSET_CHECK(PortState, speed_target, 0x18);
OFFSET_CHECK(PortState, speed, 0x1C);
OFFSET_CHECK(PortState, speed_step, 0x22);
OFFSET_CHECK(PortState, direct_lvl, 0x24);
OFFSET_CHECK(PortState, fx_send, 0x26);
OFFSET_CHECK(PortState, status, 0x28);
OFFSET_CHECK(PortState, err_hi, 0x2A);
/* PortState.status / PcmPlayer.flags / exported +0 flags (= sg_sd.h SDD_PORT_FLG_*) */
#define PORT_FLG_PLAY        0x01
#define PORT_FLG_PAUSE       0x02
#define PORT_FLG_CHG_VOL     0x04
#define PORT_FLG_CHG_SPEED   0x08
#define PORT_FLG_CHG_PAN     0x10
#define PORT_FLG_CHG_PITCH   0x20
#define PORT_FLG_TROUBLE     0x80
/* sg_sd.h SDD_PORT_ERR_* (g_seq.err, PortState.err) */
#define PORT_ERR_PRIORITY        0x02
#define PORT_ERR_REQUEST_NUM     0x04
#define PORT_ERR_MIDI_BUF        0x20  /* event ring full */
#define PORT_ERR_MIDI_SEQ_BUF    0x40  /* note-off overflow queue full */
#define PORT_ERR_SLOT            0x80  /* note-off queue full, a note was stolen */
#define g_port SRAM_PTR(PortState, 0xC000)

/* ------------------------------------------------------------------------- */
/* MIDI channel state, 0x48 bytes, g_midi_ch[port*16 + ch] at 0xC400.        */
/* ------------------------------------------------------------------------- */
/* Offsets marked "ofs" are biased: 0x20 (5-bit EG rates), 0x40 (Q) or 0x2000 (FLV) = no change. */
typedef struct MidiChannel {
    u8  program;     /* 0x00 current program number */
    u8  bank_no;     /* 0x01 index into the bank table at 0x14080 (8 bytes per entry) */
    u8  mod;         /* 0x02 modulation depth: scales tone PLFOS/ALFOS at note-on */
    u8  bend_range;  /* 0x03 bit7 set: override tone bend range (both directions) with bits 6:0 */
    s32 bend;        /* 0x04 pitch bend, -0x2000..0x1FFF */
    u8  volume;      /* 0x08 init 100 */
    u8  expression;  /* 0x09 init 127 */
    u8  level;       /* 0x0A channel gain used at note-on (init 100) */
    u8  pan;         /* 0x0B bit7 set: use tone pan; else 0..127 MIDI pan (init 0xC0) */
    u8  _0c[4];
    u32 bank;        /* 0x10 const BankHeader * of the selected tone bank */
    u32 prog;        /* 0x14 const ToneProgram * of the current program */
    u32 velcurve;    /* 0x18 const u8 (*)[128] velocity curve tables of the bank */
    u8  _1c[4];
    u8  rpn_ref_msb; /* 0x20 compared by data entry as if it were the selected RPN MSB, but */
    u8  rpn_ref_lsb; /* 0x21   never written after init (CC99/101 write 0x22) -> always 0 */
    u8  param_msb;   /* 0x22 last RPN/NRPN parameter MSB (CC99 / CC101) */
    u8  param_lsb;   /* 0x23 last RPN/NRPN parameter LSB (CC98 / CC100) */
    u8  nrpn_data;   /* 0x24 bit7: NRPN selected (CC98/99); bits 6:0 last NRPN data entry */
    u8  rpn_data;    /* 0x25 bit7: RPN selected (CC100/101); bits 6:0 last RPN data entry */
    u8  sustain;     /* 0x26 bit6: damper on -> new voices get VOICE_HELD */
    u8  q_ofs;       /* 0x27 filter resonance ofs (0x40) */
    u16 flv_ofs[5];  /* 0x28 filter FLV0..4 ofs (0x2000) */
    u8  far_ofs;     /* 0x32 filter attack rate ofs (0x20) */
    u8  fd1r_ofs;    /* 0x33 */
    u8  fd2r_ofs;    /* 0x34 */
    u8  frr_ofs;     /* 0x35 */
    u8  fx_send;     /* 0x36 0: tone IMXL; else bits 7:4 replace the tone IMXL */
    u8  ar_ofs;      /* 0x37 amp EG attack rate ofs (0x20) */
    u8  dr_ofs;      /* 0x38 amp EG D1R *and* D2R ofs (0x20) */
    u8  dl_ofs;      /* 0x39 amp EG decay level ofs (0x20) */
    u8  cc90_ofs;    /* 0x3A CC90 (v>>1), init 0x20; never read by the driver */
    u8  rr_ofs;      /* 0x3B amp EG release rate ofs (0x20) */
    u8  drum_kit;    /* 0x3C bit0: drum channel (host cmd); CC32 then picks kit 0/1 = bank 66/67 */
    u8  lpoff;       /* 0x3D 0: tone LPOFF; else bit6 set -> filter on, clear -> filter off */
    u8  cutoff_range;/* 0x3E CC75: min(v,8)*4 = jump-table offset selecting CC74's scale */
    u8  _3f;
    s32 cutoff;      /* 0x40 added to every FLV; CC74/CC110-117 (v-64)<<k. Boot writes 0x40, CC121 0 */
    u8  _44[2];
    u8  isel;        /* 0x46 bits 7:4 != 0: tone ISEL; else DSP input select (init 0x7F) */
    u8  _47;
} MidiChannel;
SIZE_CHECK(MidiChannel, 0x48);
OFFSET_CHECK(MidiChannel, bend, 0x04);
OFFSET_CHECK(MidiChannel, bank, 0x10);
OFFSET_CHECK(MidiChannel, sustain, 0x26);
OFFSET_CHECK(MidiChannel, flv_ofs, 0x28);
OFFSET_CHECK(MidiChannel, rr_ofs, 0x3B);
OFFSET_CHECK(MidiChannel, rpn_ref_msb, 0x20);
OFFSET_CHECK(MidiChannel, nrpn_data, 0x24);
OFFSET_CHECK(MidiChannel, rpn_data, 0x25);
OFFSET_CHECK(MidiChannel, q_ofs, 0x27);
OFFSET_CHECK(MidiChannel, far_ofs, 0x32);
OFFSET_CHECK(MidiChannel, fx_send, 0x36);
OFFSET_CHECK(MidiChannel, cc90_ofs, 0x3A);
OFFSET_CHECK(MidiChannel, drum_kit, 0x3C);
OFFSET_CHECK(MidiChannel, lpoff, 0x3D);
OFFSET_CHECK(MidiChannel, cutoff_range, 0x3E);
OFFSET_CHECK(MidiChannel, level, 0x0A);
OFFSET_CHECK(MidiChannel, cutoff, 0x40);
OFFSET_CHECK(MidiChannel, isel, 0x46);
#define g_midi_ch SRAM_PTR(MidiChannel, 0xC400)
#define MIDI_CH(port, ch) (&g_midi_ch[(port) * NUM_MIDI_CH + (ch)])

/* ------------------------------------------------------------------------- */
/* Voice state, 48 x 0x40 at 0xE800. Voice n drives AICA channel n.          */
/* ------------------------------------------------------------------------- */
typedef struct Voice {
    u8  flags;       /* 0x00 VOICE_KEYON 0x80, VOICE_HELD 0x40 (damper held at note-on) */
    u8  _01[2];      /* 0x01 never written by the driver; part of the "voice free" word test */
    u8  level;       /* 0x03 0 at key-on, 0xFF at key-off; voice_monitor_sample: 0xFF unless
                        in release, in release 0xFF-EG (0 = silent). Voice is free when the
                        u32 at +0 (flags..level) is 0. */
    u8  port;        /* 0x04 */
    u8  ch;          /* 0x05 */
    u8  note;        /* 0x06 */
    u8  velocity;    /* 0x07 */
    u8  vol;         /* 0x08 0..255 gain from velocity curve x channel level x tone TL */
    u8  pan;         /* 0x09 0..127 MIDI-style pan before port pan offset */
    u8  bend_up;     /* 0x0A bend range (semitones) for positive bend */
    u8  bend_down;   /* 0x0B bend range for negative bend */
    u32 bend_ofs;    /* 0x0C (bend*range)>>5 & 0xFFFF: pitch offset in 1/256 semitone, 16-bit
                        two's complement (wraps harmlessly, see voice_set_pitch) */
    const struct ToneSplit *split; /* 0x10 split that was keyed */
    s32 pitch_mod;   /* 0x14 extra pitch offset (1/256 semitone); never written by the driver */
    struct PortState *port_state;  /* 0x18 the voice's port */
    u8  vel_curve;   /* 0x1C velocity curve index (copied from the split) */
    u8  _1d[0x38 - 0x1D];
    u32 keyon_delay; /* 0x38 4 ms ticks until the deferred KYONB (0 = keyed immediately) */
    u8  _3c[2];
    u8  prio;        /* 0x3E event flags (ev>>27) of the note-on: voice-steal priority */
    u8  age;         /* 0x3F 4 ms ticks while keyed/held (wraps at 256); 0 after key-off */
} Voice;
SIZE_CHECK(Voice, 0x40);
OFFSET_CHECK(Voice, level, 0x03);
OFFSET_CHECK(Voice, velocity, 0x07);
OFFSET_CHECK(Voice, bend_ofs, 0x0C);
OFFSET_CHECK(Voice, split, 0x10);
OFFSET_CHECK(Voice, port_state, 0x18);
OFFSET_CHECK(Voice, vel_curve, 0x1C);
OFFSET_CHECK(Voice, keyon_delay, 0x38);
OFFSET_CHECK(Voice, prio, 0x3E);
OFFSET_CHECK(Voice, age, 0x3F);
#define g_voice SRAM_PTR(Voice, 0xE800)
#define VOICE_KEYON 0x80
#define VOICE_HELD  0x40

/* ------------------------------------------------------------------------- */
/* Tone bank "SMPB" (see docs/notes/bank_format.md). All offsets are relative */
/* to the bank header. Built-in default bank at 0x3E48.                      */
/* ------------------------------------------------------------------------- */
#define SMPB_MAGIC 0x42504D53u  /* "SMPB" */
typedef struct BankHeader {
    u32 magic;        /* 0x00 "SMPB" */
    u32 version;      /* 0x04 1 */
    u32 size;         /* 0x08 total size; bank ends with "ENDB" at size-4 */
    u32 _0c;
    u32 prog_tab;     /* 0x10 offset of u32 prog_ofs[num_progs] */
    u32 num_progs;    /* 0x14 */
    u32 velcurve_tab; /* 0x18 offset of u8 curve[n][128] */
    u32 num_velcurves;/* 0x1C */
} BankHeader;
SIZE_CHECK(BankHeader, 0x20);

typedef struct ToneProgram {
    u32 layer[4];     /* 0x00 layer offsets, 0 = unused */
    u32 _10;          /* 0x10 not read by the driver */
} ToneProgram;
SIZE_CHECK(ToneProgram, 0x14);

typedef struct ToneLayer {
    u32 num_splits;   /* 0x00 bits 6:0 split count (0 -> note_on scans 2^32 splits: subs/bne loop), bit7 = layer muted */
    u32 splits;       /* 0x04 offset of ToneSplit[num_splits] */
    u32 keyon_delay;  /* 0x08 key-on delay in 4 ms ticks -> Voice.keyon_delay */
    u8  bend_up;      /* 0x0C */
    u8  bend_down;    /* 0x0D */
    u8  _0e[2];       /* 0x0E read into scratch but unused */
} ToneLayer;
SIZE_CHECK(ToneLayer, 0x10);

/* A split mirrors the AICA channel registers (16-bit fields). */
typedef struct ToneSplit {
    u16 ctl;          /* 0x00 AICA reg 0 layout: SSCTL/LPCTL/PCMS (0x780) + SA[22:16] (bank-relative) */
    u16 sa_lo;        /* 0x02 SA[15:0] (bank-relative) */
    u16 lsa;          /* 0x04 */
    u16 lea;          /* 0x06 */
    u16 env_ad;       /* 0x08 D2R15:11 D1R10:6 AR4:0 */
    u16 env_dr;       /* 0x0A LPSLNK14 KRS13:10 DL9:5 RR4:0 */
    u16 _0c;          /* 0x0C not read by the driver */
    u16 lfo;          /* 0x0E AICA LFO register (PLFOS/ALFOS scaled by MidiChannel.mod) */
    u8  dsp_send;     /* 0x10 IMXL7:4 ISEL3:0 */
    u8  _11;
    u8  dipan;        /* 0x12 AICA DIPAN (5 bits) */
    u8  disdl;        /* 0x13 DISDL in bits 3:0 */
    u8  q;            /* 0x14 Q4:0, LPOFF bit5 */
    u8  tl;           /* 0x15 total level (attenuation) */
    u16 flv[5];       /* 0x16 filter FLV0..4 */
    u16 fenv_a;       /* 0x20 FAR12:8 FD1R4:0 */
    u16 fenv_r;       /* 0x22 FD2R12:8 FRR4:0 */
    u8  key_lo;       /* 0x24 */
    u8  key_hi;       /* 0x25 */
    u8  root_key;     /* 0x26 bits 6:0 */
    s8  fine;         /* 0x27 fine tune, 1/256 semitone */
    u8  _28[2];
    u8  vel_curve;    /* 0x2A velocity curve index */
    u8  vel_lo;       /* 0x2B */
    u8  vel_hi;       /* 0x2C */
    u8  _2d[3];
} ToneSplit;
SIZE_CHECK(ToneSplit, 0x30);
OFFSET_CHECK(ToneSplit, flv, 0x16);
OFFSET_CHECK(ToneSplit, key_lo, 0x24);
OFFSET_CHECK(ToneSplit, vel_hi, 0x2C);

#define BANK_TABLE_PROG  SRAM_PTR(u32, 0x14080)  /* {bank address, size} x 16 */
#define DEFAULT_BANK     ((const BankHeader *)0x3E48u)

/* ------------------------------------------------------------------------- */
/* MIDI sequencer (sequencer.c). See docs/notes/sequence_format.md.          */
/* ------------------------------------------------------------------------- */
#define SMSB_MAGIC 0x42534D53u  /* "SMSB" sequence bank */
#define SMSD_MAGIC 0x44534D53u  /* "SMSD" one sequence inside a bank */

/* One sequence player per MIDI port, 8 x 0x40 at 0x10000. */
typedef struct SeqPlayer {
    u8  prio;          /* 0x00 priority<<3 (low 3 bits cleared); ORed into event bits 31:27 */
    u8  state;         /* 0x01 SEQ_PLAYING | SEQ_PAUSED */
    u8  call_count;    /* 0x02 meta 0x81: events left before returning to call_ret */
    u8  loop_left;     /* 0x03 meta 0x82: loop-backs left, 0x80 = infinite, 0 = leave loop */
    const u8 *end;     /* 0x04 end of this sequence's data */
    u32 timebase;      /* 0x08 SMSD +4: 65536 / ticks-per-beat */
    u32 tick_len;      /* 0x0C timebase * beat_len >> 4: one delta tick in tempo_scale units */
    const u8 *cur;     /* 0x10 next event */
    const u8 *base;    /* 0x14 SMSD header: base of meta-0x81 offsets and of the exported CurAdr */
    u32 loop_iter;     /* 0x18 loop-backs taken (exported at status +0x0C) */
    const u8 *call_ret;/* 0x1C meta 0x81 return address */
    const u8 *loop_start; /* 0x20 meta 0x82 loop start (0 = no loop active) */
    s32 delta;         /* 0x24 time to the next event, counts down by tempo_scale per 4 ms */
    u32 delta_extra;   /* 0x28 meta 0x8C-0x8F: added to the next non-zero delta */
    u32 gate_extra;    /* 0x2C meta 0x88-0x8B: added to the next note's gate time */
    u32 beats;         /* 0x30 beats played (TotalBeatTime, status +0x14) */
    u32 beat_len;      /* 0x34 tempo = ms per beat (SMSD +8, meta 0x84) */
    u32 beat_count;    /* 0x38 beat countdown: -= tempo_scale>>12 (4 at 1x) per 4 ms */
    u32 tempo_scale;   /* 0x3C 0x4000 = 1x; set from PortState.speed by port_speed_fade */
} SeqPlayer;
SIZE_CHECK(SeqPlayer, 0x40);
OFFSET_CHECK(SeqPlayer, end, 0x04);
OFFSET_CHECK(SeqPlayer, cur, 0x10);
OFFSET_CHECK(SeqPlayer, loop_start, 0x20);
OFFSET_CHECK(SeqPlayer, delta, 0x24);
OFFSET_CHECK(SeqPlayer, beats, 0x30);
OFFSET_CHECK(SeqPlayer, tempo_scale, 0x3C);
#define g_seq_player SRAM_PTR(SeqPlayer, 0x10000)
#define SEQ_PLAYING 0x01
#define SEQ_PAUSED  0x02

/* Sequencer globals + overflow event queue at 0x10200. */
#define SEQ_OVF_LEN 0x3C
typedef struct SeqGlobals {
    u8  err;           /* 0x00 PORT_ERR_* accumulated; host commands copy it to PortState.err */
    u8  stalled;       /* 0x01 1 = overflow queue full: seq_tick does nothing until seq_init */
    u8  resume_port;   /* 0x02 port seq_tick resumes at after an event-ring-full abort */
    u8  _03;
    SeqPlayer *resume; /* 0x04 &g_seq_player[resume_port] */
    u32 *ovf_wr;       /* 0x08 overflow queue write pointer */
    u32 *ovf_rd;       /* 0x0C overflow queue read pointer */
    u32 ovf[SEQ_OVF_LEN]; /* 0x10 events that did not fit in the ring (0 = free) */
} SeqGlobals;
SIZE_CHECK(SeqGlobals, 0x100);
OFFSET_CHECK(SeqGlobals, resume, 0x04);
OFFSET_CHECK(SeqGlobals, ovf, 0x10);
#define g_seq (*SRAM_PTR(SeqGlobals, 0x10200))

/* Pending note-offs of sequenced notes, 48 x 12 at 0x10300. */
#define NUM_SEQ_NOTES 48
typedef struct SeqNote {
    u32 ev;            /* 0x00 note-off event (0 = free); bit 24 set = note-on to re-send (seq_continue) */
    u32 age;           /* 0x04 4 ms ticks since queued */
    s32 time;          /* 0x08 remaining gate time, -= tempo_scale per 4 ms */
} SeqNote;
SIZE_CHECK(SeqNote, 12);
#define g_seq_note SRAM_PTR(SeqNote, 0x10300)
#define SEQ_NOTE_RETRIGGER 0x01000000u

/* ------------------------------------------------------------------------- */
/* PCM players (one-shot and PCM-stream ports) on AICA channels 48-63.       */
/* 24 x 0x58 at 0x11000: [0..7] one-shot ports, [8..23] PCM-stream port p     */
/* channel c = 8 + p*2 + c. Fades/monitoring in port.c; start/stop in the     */
/* host command code (0x7884-0x82CC).                                         */
/* ------------------------------------------------------------------------- */
#define NUM_SHOT_PORTS  8
#define NUM_PSTM_CH     16
#define NUM_PCM_SLOTS   16
typedef struct PcmPlayer {
    u8  flags;         /* 0x00 PORT_FLG_*; bit0 = channel keyed on */
    u8  err;           /* 0x01 PCM_ERR_* of the last play/loop command; host copies it to status +0x1C */
    u8  prio;          /* 0x02 */
    u8  slot;          /* 0x03 g_pcm_slot index; AICA channel 48+slot */
    u32 cur_pos;       /* 0x04 AICA CA (samples), exported at status +0x18 */
    u32 cur_addr;      /* 0x08 start + CA scaled by pcms (see stream_monitor quirk) */
    u32 pitch_base;    /* 0x0C AICA OCT/FNS register value of the sample */
    s32 pitch_ofs;     /* 0x10 linear pitch offset (oct<<10 | fns), from speed + pitch */
    u8  vol_interval;  /* 0x14 */
    u8  vol_step;      /* 0x15 */
    u8  pan_interval;  /* 0x16 */
    u8  pan_step;      /* 0x17 */
    u8  vol_target;    /* 0x18 */
    u8  pan_target;    /* 0x19 */
    u8  fx_lvl;        /* 0x1A exported at +0x10 */
    u8  direct_lvl;    /* 0x1B exported at +0x11 */
    u8  vol_count;     /* 0x1C */
    u8  pan_count;     /* 0x1D */
    u8  volume;        /* 0x1E 0x80 = unity */
    u8  pan;           /* 0x1F 0x80 = centre */
    u8  tl;            /* 0x20 last TL written to the channel */
    u8  dipan;         /* 0x21 last DIPAN written */
    u8  fx_base;       /* 0x22 FxCh base level (bits 7:4) used instead of imxl when fx_ch bit6 */
    u8  loop_mode;     /* 0x23 one-shot: 0 = SOSP loop count, 1..3 = host 0x1C mode+1 (shot_loops_apply);
                          stream: LP flag of the last tick */
    u8  tl_base;       /* 0x24 TL of the sample (streams: base volume) */
    u8  isel;          /* 0x25 ISEL of the sample (SOSP dsp_send bits 3:0) */
    u8  imxl;          /* 0x26 IMXL of the sample in bits 7:4 (SOSP dsp_send & 0xF0; streams 0) */
    u8  pcms;          /* 0x27 AICA PCMS: 0 16-bit, 1 8-bit, 2/3 ADPCM */
    u8  pan_base;      /* 0x28 AICA DIPAN of the sample (streams: 0x1F left, 0x0F right) */
    u8  fx_ch;         /* 0x29 host FxCh: bit6 set = override, bits 3:0 = DSP input (ISEL); 0x80 after reset */
    u8  disdl_base;    /* 0x2A DISDL of the sample (streams 0x0F) */
    u8  disdl;         /* 0x2B last DISDL written */
    u32 loop_count;    /* 0x2C loop ends passed (AICA LP flag) */
    u32 start;         /* 0x30 sample / ring start address */
    u32 ring_len;      /* 0x34 stream: ring length in bytes (clamped); one-shot: host 0x1C loop param */
    u32 loops_left;    /* 0x38 one-shot: loop passes before key-off (0 = forever);
                          stream ch0: bank word = SPSR bank ch0 | ch1<<8 | stereo<<16 (pstm_play) */
    u32 loops_default; /* 0x3C one-shot: SOSP loop count (SospEntry.loops) */
    u32 speed_target;  /* 0x40 speed+0x8000 */
    u32 speed;         /* 0x44 */
    u8  speed_interval;/* 0x48 */
    u8  speed_count;   /* 0x49 */
    u8  speed_step;    /* 0x4A step is speed_step*2 */
    u8  pitch_dirty;   /* 0x4B 0xFF: pcm_pitch_update recomputes pitch_ofs */
    u32 pitch_target;  /* 0x4C pitch+0x8000 */
    u32 pitch;         /* 0x50 */
    u8  pitch_interval;/* 0x54 */
    u8  pitch_count;   /* 0x55 */
    u8  pitch_step;    /* 0x56 step is pitch_step*2 */
    u8  _57;
} PcmPlayer;
SIZE_CHECK(PcmPlayer, 0x58);
OFFSET_CHECK(PcmPlayer, pitch_ofs, 0x10);
OFFSET_CHECK(PcmPlayer, vol_target, 0x18);
OFFSET_CHECK(PcmPlayer, volume, 0x1E);
OFFSET_CHECK(PcmPlayer, tl_base, 0x24);
OFFSET_CHECK(PcmPlayer, pan_base, 0x28);
OFFSET_CHECK(PcmPlayer, loop_count, 0x2C);
OFFSET_CHECK(PcmPlayer, loops_left, 0x38);
OFFSET_CHECK(PcmPlayer, err, 0x01);
OFFSET_CHECK(PcmPlayer, fx_base, 0x22);
OFFSET_CHECK(PcmPlayer, isel, 0x25);
OFFSET_CHECK(PcmPlayer, fx_ch, 0x29);
OFFSET_CHECK(PcmPlayer, disdl, 0x2B);
OFFSET_CHECK(PcmPlayer, ring_len, 0x34);
OFFSET_CHECK(PcmPlayer, loops_default, 0x3C);
OFFSET_CHECK(PcmPlayer, speed_target, 0x40);
OFFSET_CHECK(PcmPlayer, pitch_dirty, 0x4B);
OFFSET_CHECK(PcmPlayer, pitch_target, 0x4C);
OFFSET_CHECK(PcmPlayer, pitch_step, 0x56);
#define g_pcm_player SRAM_PTR(PcmPlayer, 0x11000)
#define g_shot_player (&g_pcm_player[0])
#define g_pstm_player (&g_pcm_player[NUM_SHOT_PORTS])

/* AICA channel 48+n allocation, 16 x 12 at 0x12080. */
typedef struct PcmSlot {
    u8  prio;          /* 0x00 priority of the owner (0 = free) */
    u8  kind;          /* 0x01 owner type (1 one-shot, 2/3 stream) */
    u8  _02[2];
    PcmPlayer *owner;  /* 0x04 0 = free */
    volatile AicaChannel *ch; /* 0x08 &AICA_CH[48+n] */
} PcmSlot;
SIZE_CHECK(PcmSlot, 12);
#define g_pcm_slot SRAM_PTR(PcmSlot, 0x12080)
#define PCM_KIND_SHOT   1   /* PcmSlot.kind: one-shot port */
#define PCM_KIND_PSTM_L 2   /* PCM stream channel 0 */
#define PCM_KIND_PSTM_R 3   /* PCM stream channel 1 (stereo) */
/* PcmPlayer.err bits (host copies them to status +0x1C; names from use, cf. SDD_PORT_ERR_*) */
#define PCM_ERR_NO_RING     0x01  /* pstm_play: SPSR ring-buffer bank address is 0 */
#define PCM_ERR_PRIORITY    0x02  /* port busy with a higher priority */
#define PCM_ERR_REQUEST_NUM 0x04  /* shot_play: data number > SOSB count */
#define PCM_ERR_PARAM       0x20  /* host 0x1C: mode > 2 */
#define PCM_ERR_NO_CHANNEL  0x80  /* no AICA channel 48-63 could be taken */

/* One-shot bank "SOSB" (docs/notes/oneshot_pstm_format.md). Offsets are relative to
 * the bank header; the host 0x11 handler checks magic, version byte and "ENDB" at size-4. */
#define SOSB_MAGIC 0x42534F53u  /* "SOSB" */
#define SOSP_MAGIC 0x50534F53u  /* "SOSP" (entry tag, never checked by the driver) */
#define ENDP_MAGIC 0x50444E45u  /* "ENDP" (entry end tag, never checked) */
typedef struct SosbHeader {
    u32 magic;        /* 0x00 "SOSB" */
    u32 version;      /* 0x04 low byte must be 1 */
    u32 size;         /* 0x08 bank size incl. the trailing "ENDB" */
    u32 count;        /* 0x0C number of entries; shot_play accepts num <= count (off by one) */
    u32 offset[1];    /* 0x10 offset[count] of each SospEntry from the bank start */
} SosbHeader;
/* A SOSP entry is an AICA channel register image (16-bit fields) plus a few extras. */
typedef struct SospEntry {
    u32 magic;        /* 0x00 "SOSP" */
    u16 ctl;          /* 0x04 AICA reg 0: LPCTL/PCMS + SA[22:16] (bank-relative) */
    u16 sa_lo;        /* 0x06 SA[15:0] (bank-relative) */
    u16 lsa;          /* 0x08 -> reg 0x08 */
    u16 lea;          /* 0x0A -> reg 0x0C */
    u16 env_ad;       /* 0x0C -> reg 0x10 */
    u16 env_dr;       /* 0x0E -> reg 0x14 */
    u16 pitch;        /* 0x10 -> reg 0x18 OCT/FNS (also PcmPlayer.pitch_base) */
    u16 lfo;          /* 0x12 -> reg 0x1C */
    u8  dsp_send;     /* 0x14 -> reg 0x20: IMXL7:4 ISEL3:0 (PcmPlayer.imxl/isel) */
    u8  _15;
    u8  dipan;        /* 0x16 -> reg 0x24 DIPAN (PcmPlayer.pan_base) */
    u8  disdl;        /* 0x17 -> reg 0x25 DISDL (PcmPlayer.disdl_base) */
    u8  q;            /* 0x18 -> reg 0x28 Q/LPOFF */
    u8  tl;           /* 0x19 -> reg 0x29 TL (PcmPlayer.tl_base) */
    u16 flv[5];       /* 0x1A -> regs 0x2C-0x3C */
    u16 fenv_a;       /* 0x24 -> reg 0x40 */
    u16 fenv_r;       /* 0x26 -> reg 0x44 */
    u8  loops;        /* 0x28 loop passes (0 = loop forever) -> PcmPlayer.loops_default */
    u8  _29;
    u16 _2a;          /* 0x2A not read by the driver (0x5400, 0x4D45, ...) */
    u32 _2c;          /* 0x2C not read (0) */
    u32 length;       /* 0x30 not read; = lea in every entry seen */
    u32 end_magic;    /* 0x34 "ENDP" */
} SospEntry;
SIZE_CHECK(SospEntry, 0x38);
OFFSET_CHECK(SospEntry, dsp_send, 0x14);
OFFSET_CHECK(SospEntry, tl, 0x19);
OFFSET_CHECK(SospEntry, loops, 0x28);
#define BANK_TABLE_SOSB  SRAM_PTR(u32, 0x14100)  /* {SOSB address, size} x 16 */
#define BANK_TABLE_SPSR  SRAM_PTR(u32, 0x14180)  /* {PCM stream ring address, size} x 16 */

/* ------------------------------------------------------------------------- */
/* Port status records exported to the SH-4 every 4 ms (port_status_export). */
/* Read by the sd library getters (docs/research/host_protocol.md 4.2).   */
/* ------------------------------------------------------------------------- */
typedef struct PortStatus {
    u8  flags;         /* 0x00 PORT_FLG_*; MIDI: |0x80 when an error is latched */
    s8  vol;           /* 0x01 */
    s8  pan;           /* 0x02 */
    u8  _03;
    s32 speed;         /* 0x04 MIDI: speed*4 (library divides by 4) */
    s32 pitch;         /* 0x08 */
    u32 loops;         /* 0x0C MIDI: SeqPlayer.loop_iter, shot: loops_left, stream: loop_count; unused by the library */
    s8  fx_lvl;        /* 0x10 */
    s8  direct_lvl;    /* 0x11 */
    u8  _12[2];
    u32 total;         /* 0x14 MIDI: beats; PCM: sample frames played */
    s32 cur_adr;       /* 0x18 MIDI: offset of the next event from the SMSD header; PCM: CA */
    u8  err;           /* 0x1C MIDI only (SDD_PORT_ERR_*) */
    u8  _1d[2];
    u8  err_hi;        /* 0x1F MIDI only (PortState.err_hi) */
} PortStatus;
SIZE_CHECK(PortStatus, 0x20);
OFFSET_CHECK(PortStatus, speed, 0x04);
OFFSET_CHECK(PortStatus, fx_lvl, 0x10);
OFFSET_CHECK(PortStatus, total, 0x14);
OFFSET_CHECK(PortStatus, err, 0x1C);
#define g_midi_status SRAM_PTR(volatile PortStatus, 0x13600)
#define g_shot_status SRAM_PTR(volatile PortStatus, 0x13800)
#define g_pstm_status SRAM_PTR(volatile PortStatus, 0x13A00) /* [port*2 + ch] */
#define ticks_since_cmd SRAM(u32, 0x13410) /* ++ every 4 ms; logged and cleared by host_cmd_exec */

/* ------------------------------------------------------------------------- */
/* Host interface                                                            */
/* ------------------------------------------------------------------------- */
#define NUM_HOST_CMD_SLOTS 32
typedef struct HostCmd {
    u8  code;        /* 0 = empty slot */
    u8  arg[15];
} HostCmd;
SIZE_CHECK(HostCmd, 16);
#define g_host_cmd       SRAM_PTR(volatile HostCmd, 0x13200)
#define host_cmd_pending SRAM(u8, 0x13400)
#define cmd_log_ofs      SRAM(u32, 0x1340C)
#define CMD_LOG          SRAM_PTR(volatile u32, 0x14800)

#define tick4ms_count    SRAM(u32, 0x13418)
#define midi_irq_count   SRAM(u32, 0x1341C)
#define master_vol_shadow SRAM(u8, 0x13424)

/* Values inside the driver image */
#define drv_status       SRAM(u32, 0xF8)
#define midi_irq_flag    SRAM(u32, 0xFC)

/* ------------------------------------------------------------------------- */
/* Effects (owner: fx.c, format notes: docs/notes/fx_format.md)              */
/*   SFPB = FX program bank (DSP microprogram + COEF + MADRS per program)    */
/*   SFOB = FX output bank (EFSDL/EFPAN of the 16 DSP outputs, per "out")    */
/*   SFPW = FX work area = DSP ring buffer (address/size only)               */
/* ------------------------------------------------------------------------- */
#define FX_SFPB_MAGIC 0x42504653u  /* "SFPB" */
#define FX_SFOB_MAGIC 0x424F4653u  /* "SFOB" */
#define FX_ENDB_MAGIC 0x42444E45u  /* "ENDB" (last word of every bank, at size-4) */

typedef struct FxBankHeader {    /* common header of SFPB and SFOB */
    u32 magic;        /* 0x00 "SFPB" / "SFOB" */
    u32 version;      /* 0x04 only the low byte is checked (== 1) */
    u32 size;         /* 0x08 bank size incl. the trailing "ENDB" */
    u32 count;        /* 0x0C number of programs / output sets (low byte checked) */
    u32 offset[1];    /* 0x10 offset[count] from the bank start */
} FxBankHeader;

typedef struct FxProgram {       /* SFPB entry, 0xC40 bytes */
    char name[32];    /* 0x000 e.g. "e-reverb", "dcsc2_outside.FPD" */
    u8  rb_size;      /* 0x020 RBL code 0..3 = 16/32/64/128 KB ring buffer */
    u8  _21;
    u8  _22;          /* 0x022 copied to fx_prg_b22 (0x13F1C), never used */
    u8  pan_mode;     /* 0x023 bit4: 8 DSP pan slots, bit5: 4 slots; nonzero = no ring buffer needed */
    u8  pan_coef_base;/* 0x024 first COEF of the pan slots */
    u8  _25[0x40 - 0x25];
    u32 coef[128];    /* 0x040 -> 0x803000 COEF */
    u32 madrs[64];    /* 0x240 -> 0x803200 MADRS */
    u32 _gap[64];     /* 0x340 -> 0x803300 (no registers; copied anyway) */
    u32 mpro[512];    /* 0x440 -> 0x803400 MPRO (128 steps x 4 words) */
} FxProgram;
SIZE_CHECK(FxProgram, 0xC40);
OFFSET_CHECK(FxProgram, pan_mode, 0x23);
OFFSET_CHECK(FxProgram, coef, 0x40);
OFFSET_CHECK(FxProgram, mpro, 0x440);

typedef struct FxOutEntry {      /* one DSP output (EFREG n) in an SFOB set */
    u8  lev;          /* bits 3:0 EFSDL */
    u8  pan;          /* bits 4:0 EFPAN (AICA sign/magnitude pan) */
} FxOutEntry;
typedef struct FxOutSet { FxOutEntry out[16]; } FxOutSet;  /* 0x20 bytes */
SIZE_CHECK(FxOutSet, 0x20);

/* bank table slots (written by the SH-4 sd library, defaults by init_bank_table) */
#define fx_prg_bank      SRAM(u32, 0x14200)  /* SFPB address (0 -> built-in 0x2908) */
#define fx_prg_bank_size SRAM(u32, 0x14204)
#define fx_out_bank      SRAM(u32, 0x14280)  /* SFOB address (default built-in 0x4234) */
#define fx_out_bank_size SRAM(u32, 0x14284)
#define fx_wrk_addr      SRAM(u32, 0x14288)  /* SFPW: DSP ring buffer address (>= 0x18000) */
#define fx_wrk_size      SRAM(u32, 0x1428C)  /* SFPW size in bytes */

/* driver-private FX state in the host area */
#define fx_out_table     SRAM(u32, 0x13008)  /* SFOB base + offset[0]: FxOutSet[] of the bank */
#define fx_prg_pending   SRAM(u32, 0x1300C)  /* FxProgram * queued by fx_prg_select */
#define fx_rb_pending    SRAM(u32, 0x13010)  /* RINGBUF value for the queued program */
#define fx_load_req      SRAM(u8,  0x13014)  /* 1 = fx_prg_load_service must load it */
#define drv_err          SRAM(u8,  0x13420)  /* error bits, sdDrvGetErr (lib shifts <<3?) */
#define DRV_ERR_NO_DOWNLOAD 0x01  /* SFPW missing/too small (FX) */
#define DRV_ERR_ILLEGAL_ID  0x02  /* bad magic */
#define DRV_ERR_ILLEGAL_END 0x04  /* no "ENDB" at size-4 */
#define DRV_ERR_ILLEGAL_VER 0x08  /* version byte != 1 */
#define DRV_ERR_ILLEGAL_NUM 0x10  /* index >= count */
#define FX_OUT_PRM       SRAM_PTR(volatile s8, 0x13450) /* [16][2] {lev, pan} offsets (host 0x83) */
#define fx_prg_cur       SRAM(u8,  0x13474)  /* loaded FX program, 0xFF = none */
#define fx_out_cur       SRAM(u8,  0x13475)  /* selected SFOB set */
#define fx_busy          SRAM(u8,  0x13476)  /* 0xFF while a program is being loaded */

/* 0x13F00 block (drv_layout_table[0]); the R9 sd library never reads it */
#define fx_ext_req       SRAM(u8,  0x13F00)  /* external DSP control request (writer unknown) */
#define fx_ext_active    SRAM(u8,  0x13F01)  /* 0xFF: req && SFPW set -> driver leaves DSP alone */
#define fx_dsp_reg_base  SRAM(u32, 0x13F04)  /* 0x803000 */
#define fx_dsp_reg_size  SRAM(u32, 0x13F08)  /* 0xC00 (COEF..MPRO) */
#define fx_wrk_size_pub  SRAM(u32, 0x13F0C)  /* copy of fx_wrk_size when RINGBUF is (re)written */
#define fx_ringbuf_reg   SRAM(u32, 0x13F10)  /* current AICA_RINGBUF value */
#define fx_prg_b22       SRAM(u8,  0x13F1C)  /* FxProgram._22 of the loaded program */
#define fx_pan_mode      SRAM(u8,  0x13F1D)  /* FxProgram.pan_mode of the loaded program */
#define fx_pan_coef_base SRAM(u8,  0x13F1E)  /* FxProgram.pan_coef_base */
#define ext_req2         SRAM(u8,  0x13F80)  /* mirrored to ext_ack2 by fx_ext_flags_update */
#define ext_ack2         SRAM(u8,  0x13F81)
#define ext_req3         SRAM(u8,  0x13F94)
#define ext_ack3         SRAM(u8,  0x13F95)

/* ------------------------------------------------------------------------- */
/* Prototypes (grouped by source file)                                       */
/* ------------------------------------------------------------------------- */
/* main.c */
void drv_reset(void);
/* irq.c */
void fiq_handler(void);
void irq_init(void);
extern volatile u8 tick1ms_flag, tick4ms_flag;
/* init.c */
void clear_host_area(void);
void init_bank_table(void);
void evq_clear(void);
void init_midi_ports(void);
void clear_voices(void);
void midi_in_flush(void);
void aica_silence_all(void);
void dsp_out_clear(void);
/* midi_in.c */
void midi_in_isr(void);
void midi_event_push_ext(u32 status, u32 data1, u32 data2);
/* event.c */
u32  midi_event_pop(void);
void midi_event_dispatch(u32 ev);
void voice_service_x4(void);
/* monitor.c */
void voice_monitor_sample(void);
void voice_monitor_next(void);
/* voice.c */
void voices_key_off_all(void);
void voices_key_off_port(u32 port);
void note_off(u32 port, u32 ch, u32 note);
void note_on(u32 ev);
void voice_set_pitch(Voice *v, const ToneSplit *sp, u32 pitch);
void voice_update_4ms(void);
/* event_ctl.c - handlers for event types 2..7 (called from midi_event_dispatch).
 * All take the packed event; port/ch/data are re-derived with the EV_* macros.
 * In the asm they receive r1 = ev, r2 = ch, r3 = port, r4 = flags,
 * r5 = ch*0x48, r6 = 0xC400 + port*0x480 (r5+r6 = MIDI_CH(port, ch)). */
void ev_poly_pressure(u32 ev);    /* 0x1368: just returns */
void ev_control_change(u32 ev);   /* 0x185C */
void ev_program_change(u32 ev);   /* 0x136C */
void ev_channel_pressure(u32 ev); /* 0x13EC: just returns */
void ev_pitch_bend(u32 ev);       /* 0x13F0: calls voice_set_pitch */
void ev_system(u32 ev);           /* 0x14B8: just returns */
void ctl_reset_port(u32 port);    /* 0x28C8: unreferenced alt. entry of CC121 */
/* sequencer.c - MIDI sequence players (called by host_cmd_exec). All take the
 * port number (masked with 7) and return SeqPlayer.state (the asm returns it in r1;
 * host_cmd_exec copies its low 2 bits into PortState.status and g_seq.err into
 * PortState.err). */
void seq_init(void);                                        /* 0x742C boot + host */
u32  seq_play(u32 port, u32 song, u32 prio, const u8 *smsb); /* 0x747C (r0,r1,r2,r12) */
u32  seq_stop(u32 port);                                    /* 0x7564 */
u32  seq_stop_player(SeqPlayer *p, u32 port);               /* 0x7574 (r11 = p, r0 = port) */
u32  seq_pause(u32 port);                                   /* 0x75F4 */
u32  seq_continue(u32 port);                                /* 0x7674 */
void seq_break_loop(u32 port);                              /* 0x7744 */
void seq_tick(void);                                        /* 0x4270 every 4 ms */
/* port.c - fades, PCM player monitoring, status export (all every 4 ms unless noted) */
void port_clear_play(u32 port);                             /* 0x6454 */
void port_status_export(void);                              /* 0x6470 */
void ticks_since_cmd_inc(void);                             /* 0x6698 */
void port_vol_fade(void);                                   /* 0x66B8 */
void port_pan_fade(void);                                   /* 0x6758 */
void port_speed_fade(void);                                 /* 0x67F8 */
void port_pitch_fade(void);                                 /* 0x68C4 */
void pcm_speed_fade(void);                                  /* 0x6970 */
void pcm_pitch_fade(void);                                  /* 0x6A20 */
void pcm_pitch_update(void);                                /* 0x6AD4 */
void pcm_tick(void);                                        /* 0x711C */
u32  pcm_update(PcmPlayer *pl);                             /* 0x7178 */
u32  shot_monitor(PcmPlayer *pl, u32 flags);                /* 0x7350 */
void stream_monitor(PcmPlayer *pl);                         /* 0x73C8 */
void pcm_init(void);                                        /* 0x7778 boot */
/* fx.c - AICA DSP effects (SFPB/SFOB/SFPW) and the host commands in 0x5DCC-0x6470 */
void fx_status_init(void);              /* 0x0694 */
void fx_ext_flags_update(void);         /* 0x06D4 */
void fx_update_ringbuf(void);           /* 0x076C */
u32  fx_cmd_set_out(volatile HostCmd *cmd);     /* 0x5A8C host 0x82 sdSndSetFxOut */
u32  fx_cmd_set_out_prm(volatile HostCmd *cmd); /* 0x5BD8 host 0x83 sdSndSetFxOutPrm */
u32  fx_prg_select(u32 prg);            /* 0x5C98 (host 0x84 enters at 0x5C94 with cmd[2]) */
u32  fx_prg_clear(void);                /* 0x5DCC host 0x85 sdSndClearFxPrg */
void fx_dsp_clear(void);                /* 0x5DF0 */
u32  host_cmd_push_event(volatile HostCmd *cmd);  /* 0x5E70 host 0x86 */
u32  host_cmd_drum_mode(volatile HostCmd *cmd);   /* 0x5EA0 host 0x87 */
u32  fx_cmd_set_dsp_pan(volatile HostCmd *cmd);   /* 0x5F54 host 0x88 */
u32  host_cmd_nop89(void);                        /* 0x6040 host 0x89 */
u32  host_cmd_mono(volatile HostCmd *cmd);        /* 0x6044 host 0x8A */
u32  host_cmd_reinit(void);                       /* 0x6094 host 0x8E */
void host_cmd_reboot(volatile HostCmd *cmd);      /* 0x60D8 host 0x8F ("SEGA": reinit + hang) */
void fx_prg_load_service(void);         /* 0x61D4 every 4 ms */
void fx_ringbuf_clear(void);            /* 0x62EC */
void fx_out_fade_out(void);             /* 0x6328 */
void fx_out_fade_in(void);              /* 0x6388 */
/* tables.c - constant tables extracted from the image (tools/gen_tables.py) */
extern const u32 drv_layout_table[8];   /* 0x00C0 */
extern const u8  builtin_sfpb[0xC58];   /* 0x2908 SFPB "e-reverb" */
extern const u32 builtin_sfpb_size;     /* 0x3560 */
extern const u8  note_oct_tab[0xE4];    /* 0x3564 (note+0x60) -> oct<<4|semitone, 0x7F invalid */
extern const u8  fns_lo_tab[0x600];     /* 0x3648 semitone*128+fine -> FNS[7:0] */
#define FNS_HI_1 0x1EF                  /* fns_lo_tab index thresholds: +0x100 each */
#define FNS_HI_2 0x383
#define FNS_HI_3 0x4D9
extern const u32 fx_pan_coef_tab[32][4];/* 0x3C48 DSP pan-matrix COEF presets */
extern const u8  builtin_smpb0[0x2D8];  /* 0x3E48 default tone bank */
#define builtin_vol_curve (builtin_smpb0 + 0xE0)  /* 0x3F28 curve 0, also the CC7/CC11 curve */
extern const u32 builtin_smpb0_size;    /* 0x4120 */
extern const u8  builtin_smpb1[0x10C];  /* 0x4124 default drum kit */
extern const u32 builtin_smpb1_size;    /* 0x4230 */
extern const u8  builtin_sfob[0x38];    /* 0x4234 SFOB */
extern const u32 builtin_sfob_size;     /* 0x426C */
extern const u32 seq_len_mul_a[4];      /* 0x47A4 */
extern const u32 seq_len_mul_b[4];      /* 0x47B4 */
extern const u32 fx_rb_need_tab[4];     /* 0x5DA0 */
extern const u8  pan_aica_to_lin_a[32]; /* 0x7310 */
extern const u8  pan_lin_to_aica_a[32]; /* 0x7330 */
extern const u8  pan_lin_to_aica_b[32]; /* 0x7CD4 */
extern const u8  pan_aica_to_lin_b[32]; /* 0x7CF4 */
extern const u8  oct_flip_tab[16];      /* 0x7E38 */
/* host_helpers.c - one-shot (host 0x1x) and PCM-stream (host 0x2x) port commands, 0x6DFC-0x7118
 * and 0x7884-0x82CC. port is masked with 7. Stream "pmask" = cmd[2]: bits 2:0 port, bit5 = ch0,
 * bit4 = ch1 (sdPstmSetVol/Pan). Errors are ORed into PcmPlayer.err (the host handler exports them). */
void shot_set_vol(u32 port, u32 vol, u32 fade);                 /* 0x6DFC host 0x15 */
void pstm_set_vol(u32 pmask, u32 vol0, u32 fade0, u32 vol1, u32 fade1); /* 0x6E14 host 0x25 */
void shot_set_pan(u32 port, u32 pan, u32 fade);                 /* 0x6E84 host 0x16 */
void pstm_set_pan(u32 pmask, u32 pan0, u32 fade0, u32 pan1, u32 fade1); /* 0x6E9C host 0x26 */
void shot_set_fx_ch(u32 port, u32 in_ch, u32 base_lvl);         /* 0x6F4C host 0x19 */
void pstm_set_fx_ch(u32 port, u32 in_ch, u32 base_lvl);         /* 0x6F64 host 0x29 */
void shot_set_fx_lvl(u32 port, u32 lvl);                        /* 0x6FE0 host 0x1A */
void pstm_set_fx_lvl(u32 port, u32 lvl);                        /* 0x6FF8 host 0x2A */
void shot_set_direct_lvl(u32 port, u32 lvl);                    /* 0x7068 host 0x1B */
void pstm_set_direct_lvl(u32 port, u32 lvl);                    /* 0x7080 host 0x2B */
u32  pcm_level_mix(u32 ofs, u32 base);                          /* 0x70D4 -> 0..15 */
void shot_play(u32 port, u32 num, u32 prio, const SosbHeader *bank); /* 0x7884 host 0x11 */
void pstm_play(u32 port, u32 fmt, u32 banks, u32 tl, u32 freq, u32 prio); /* 0x79BC host 0x21 */
PcmSlot *pcm_slot_alloc(PcmPlayer *pl, u32 prio, u32 kind);    /* 0x7B68, 0 = failed (err set) */
void pcm_apply_params(PcmPlayer *pl, volatile AicaChannel *ch, u32 tl, u32 pan_base, u32 pitch); /* 0x7D14 */
void pcm_apply_pitch(PcmPlayer *pl, volatile AicaChannel *ch, u32 pitch_base); /* 0x7DEC (pcm_update tail) */
void shot_stop(u32 port);                                       /* 0x7E48 host 0x12 */
void pstm_stop(u32 port);                                       /* 0x7E78 host 0x22 */
volatile AicaChannel *pstm_release(PcmPlayer *pl, PcmSlot *s, volatile AicaChannel *ch); /* 0x7EA4 ch0+ch1 */
void shot_stop_all(void);                                       /* 0x7F18 host 0x1F / 0x80 */
void shot_release(PcmPlayer *pl, PcmSlot *s, volatile AicaChannel *ch); /* 0x7F50 */
void pstm_stop_all(void);                                       /* 0x7F80 host 0x2F / 0x80 */
void shot_pause(u32 port);                                      /* 0x7FFC host 0x13 */
void shot_continue(u32 port);                                   /* 0x8058 host 0x14 */
void shot_reset_prm_all(void);                                  /* 0x8090 host 0x1E port 0xFF */
void shot_reset_prm(u32 port);                                  /* 0x809C host 0x1E */
void pstm_reset_prm_all(void);                                  /* 0x80E0 host 0x2E port 0xFF */
void pstm_reset_prm(u32 port);                                  /* 0x80EC host 0x2E */
void pcm_key_exec_off(volatile AicaChannel *ch);                /* 0x81EC KYONB=0 + KYONEX */
void pcm_release(PcmPlayer *pl, PcmSlot *s);                    /* 0x8204 free slot + clear fields */
void pcm_slot_free(PcmPlayer *pl, PcmSlot *s);                  /* 0x8210 (flags untouched) */
PcmSlot *pcm_player_slot(const PcmPlayer *pl);                  /* 0x8240 (also 0x70F4): r10, r11 = ->ch */
void shot_set_loop_mode(u32 port, u32 mode, u32 param);         /* 0x8258 host 0x1C */
void shot_loops_apply(PcmPlayer *pl, u32 mode, u32 param);      /* 0x828C */
/* host.c */
void host_cmd_poll(void);
u32  host_cmd_exec(u32 code, volatile HostCmd *cmd);

#endif
