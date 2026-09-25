/*
 * event_ctl.c - MIDI event types 2..7: poly pressure, control change, program
 * change, channel pressure, pitch bend and the driver-private type 7.
 *
 * All entry points are reached from midi_event_dispatch's 8-way jump table at
 * 0x0A1C with r1 = ev, r2 = ch, r3 = port, r4 = flags (ev>>27), r5 = ch*0x48,
 * r6 = 0xC400 + port*0x480. Every handler first does r5 += r6 (the channel
 * record). In C they take the packed event and re-derive port/ch/data.
 *
 * Control change (0x185C) is a 128-entry branch table (0x1878) indexed by the
 * controller number; unhandled controllers go to a lone "mov pc,lr" (0x1A78).
 * Most sub-handlers only store a value in the MidiChannel for the next note-on;
 * volume, expression, pan, modulation, sustain, resonance, reverb send and the
 * FLV3/cutoff controllers also rewrite the AICA registers of every sounding
 * voice on the channel (voice loop over g_voice[0..47] matching port/ch).
 *
 * Handled controllers:
 *    1 mod          6 data entry   7 volume       10 pan       11 expression
 *   20-24 FLV0..4 ofs MSB   52-56 FLV0..4 ofs LSB (14-bit, 0x2000 = no change)
 *   25-28 filter EG FAR/FD1R/FD2R/FRR ofs           32 bank select (not CC0!)
 *   48 MIDI timing counter 0x13414                   64 damper
 *   70 filter on/off  71,85 resonance  72,86 release  73,87 attack
 *   74 cutoff (scale via 75)  75 cutoff range  76 DSP input (ISEL)
 *   77/78/79 (port 7 only) DSP output pan / level / effect program
 *   80 DSP effect slot pan     88 decay level  89 decay rate  90 unused store
 *   91 reverb send (IMXL)  96/97 data inc/dec  98-101 NRPN/RPN select
 *   110-117 cutoff with fixed scale 2^0..2^7    121 reset controllers (whole port)
 * Everything else, including CC0, CC120 and CC123 (all notes off), is ignored.
 */
#include "manatee.h"

/* ---- locals ------------------------------------------------------------- */

#define VOICE_ACTIVE(v)  (*(volatile u32 *)(v) != 0)      /* u32 at +0 */
#define SPLIT(v)         ((const ToneSplit *)(v)->split)
#define PORT_OF(v)       ((PortState *)(v)->port_state)

#define midi_timing_cnt  SRAM(u8, 0x13414)   /* CC48 counter, sdDrvGetMidiTimmingCounter */
#define drv_err_flags    SRAM(u8, 0x13420)   /* sdDrvGetErr bits; CC32 sets bit0 (bank/kit not loaded) */
#define fx_pan_mode      SRAM(u8, 0x13F1D)   /* bit4: 8 pan slots, bit5: 4 pan slots */
#define fx_pan_coef_base SRAM(u8, 0x13F1E)   /* first COEF index of the pan slots */

#define BANK_KIT_FIRST   66                  /* drum kits 0/1 = bank table entries 66/67 (0x14290) */
#define DEFAULT_KIT      0x4124u             /* built-in drum kit ("SMPB") */
#define VOL_CURVE        ((const u8 *)0x3F28u) /* = velocity curve 0 of the built-in bank */
#define FX_PAN_COEF_TAB  ((const u32 *)0x3C48u) /* 32 equal-power pan presets x 4 gains */

/* bank table entry n: [0] address, [1] size */
static inline volatile u32 *bank_entry(u32 n)
{
    return (volatile u32 *)(0x14080u + n * 8);
}

/* Image-embedded scratch words */
static u32 s_prog_saved_bank;  /* 0x13E8 */
static s32 s_bend;             /* 0x14B0 */

/*
 * MIDI pan 0..127 -> AICA 5-bit DIPAN/EFPAN (bit4 = left side, 3:0 = attenuation
 * of the other side). Inline in the asm at 0x1CC8, 0x10E0 (note-on) and 0x2478.
 */
static u32 pan_to_aica(u32 p)
{
    s32 d = (s32)(p >> 2) - 0x10;
    if (d < 0)
        d = (d ^ 0xFF) + 0x10;
    return (u32)d & 0x1F;
}

/*
 * Filter FLV3 recompute for the sounding voices of a channel.
 * Three identical copies in the asm: 0x1DE8 (CC23), 0x2030 (CC55) and 0x2390
 * (tail of CC74/CC110-117, its own Ghidra function ctl_apply_flv3).
 * FLV3 = split FLV3 + (flv_ofs[3]-0x2000) + cutoff, clamped to 8..0x1FF8.
 * Only FLV3 is refreshed on live voices; note-on applies the offsets to all five.
 */
static void ctl_apply_flv3(MidiChannel *mc, u32 port, u32 ch)
{
    u32 i;

    for (i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voice[i];
        s32 f;

        if (!VOICE_ACTIVE(v) || v->port != port || v->ch != ch)
            continue;
        f = (s32)SPLIT(v)->flv[3] + ((s32)mc->flv_ofs[3] - 0x2000) + mc->cutoff;
        if (f < 0)
            f = 0;
        if (f < 8)
            f = 8;
        if ((f + 8) & 0xE000)       /* only bits 15:13 are tested */
            f = 0x1FF8;
        AICA_CH[i].flv[3] = (u32)f;
    }
}

/* ---- trivial event types ------------------------------------------------ */

/*
 * 0x1368 ev_poly_pressure
 * Confidence: high
 * Polyphonic key pressure is ignored (single "mov pc,lr").
 */
void ev_poly_pressure(u32 ev)
{
}

/*
 * 0x13EC ev_channel_pressure
 * Confidence: high
 * Channel pressure is ignored (single "mov pc,lr").
 */
void ev_channel_pressure(u32 ev)
{
}

/*
 * 0x14B8 ev_system
 * Confidence: high
 * Event type 7 = status 0xF0-0xFF (type is status>>4 & 7) is ignored (single
 * "mov pc,lr"). The SH-4 library's sdMidiSetMes turns system messages into an
 * all-zero message anyway, so this is just the unused eighth table slot.
 */
void ev_system(u32 ev)
{
}

/* ---- program change / pitch bend ---------------------------------------- */

/*
 * 0x136C ev_program_change
 * Confidence: high
 * Re-resolves the bank from bank_no (table 0x14080, built-in 0x3E48 if the
 * entry's address is 0), then selects program data1 if it is < num_progs:
 * program, prog (ToneProgram*) and velcurve (curve table base) are updated.
 * An out-of-range program leaves everything as it was.
 * Notes: only affects later note-ons. Ignores drum_kit: on a drum channel the
 * kit pointer set by CC32 is replaced by melodic bank bank_no (0/1) here.
 * Unlike CC32 it does not check the entry's size word.
 */
void ev_program_change(u32 ev)
{
    MidiChannel *mc = MIDI_CH(EV_PORT(ev), EV_CH(ev));
    const BankHeader *bank;
    u32 prog = EV_DATA1(ev) & 0x7F;

    s_prog_saved_bank = mc->bank;
    bank = (const BankHeader *)bank_entry(mc->bank_no)[0];
    if (bank == 0)
        bank = DEFAULT_BANK;
    mc->bank = (u32)bank;

    if (prog >= bank->num_progs) {
        mc->bank = s_prog_saved_bank;
        return;
    }
    mc->program = (u8)prog;
    mc->prog = (u32)bank + *(const u32 *)((u32)bank + bank->prog_tab + prog * 4);
    mc->velcurve = (u32)bank + bank->velcurve_tab;
}

/*
 * 0x13F0 ev_pitch_bend
 * Confidence: high
 * bend = (data2<<7 | data1) - 0x2000 is stored in the channel, then every
 * sounding voice of the channel gets bend_ofs = (bend * range)>>5 & 0xFFFF
 * (range = bend_down for negative bends, bend_up otherwise; 1/256 semitone
 * units, so 0x2000*range>>5 = range semitones) and its pitch recomputed with
 * pitch_mod + bend_ofs + (port pitch - 0x8000) via voice_set_pitch (0x14BC).
 * Notes: the asm uses LSR #5 on the signed product; only the low 16 bits are
 * kept so the result equals an arithmetic shift. The bend is also kept in an
 * image word at 0x14B0 (only read back by this routine).
 */
void ev_pitch_bend(u32 ev)
{
    u32 port = EV_PORT(ev), ch = EV_CH(ev);
    MidiChannel *mc = MIDI_CH(port, ch);
    s32 bend = (s32)(((ev & 0x7F) << 7) | (EV_DATA1(ev) & 0x7F)) - 0x2000;
    u32 i;

    mc->bend = bend;
    s_bend = bend;
    for (i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voice[i];
        u32 range, ofs, pitch;

        if (!VOICE_ACTIVE(v) || v->port != port || v->ch != ch)
            continue;
        range = ((u32)s_bend & 0xFFFF0000u) ? v->bend_down : v->bend_up;
        ofs = ((u32)s_bend * range >> 5) & 0xFFFF;
        v->bend_ofs = ofs;
        pitch = (u32)v->pitch_mod + ofs + (PORT_OF(v)->pitch - 0x8000);
        voice_set_pitch(v, SPLIT(v), pitch);
    }
}

/* ---- control change sub-handlers ---------------------------------------- */

/*
 * 0x1A7C cc_modulation (CC1)
 * Confidence: high
 * mod = v; for every voice on the channel the split's LFO register is rescaled:
 * PLFOS' = ((PLFOS+1)*(v+1)-1)>>7, ALFOS' likewise, LFORE cleared.
 * Notes: the only voice loop that does not test the voice-active word, so idle
 * AICA channels whose stale port/ch match get an LFO write too (split may be 0:
 * reads the vector table; harmless for a silent channel).
 */
static void cc_modulation(MidiChannel *mc, u32 port, u32 ch, u32 val)
{
    u32 m, i;

    val &= 0x7F;
    mc->mod = (u8)val;
    m = val + 1;
    for (i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voice[i];
        u32 lfo, plfos, alfos;

        if (v->port != port || v->ch != ch)
            continue;
        lfo = SPLIT(v)->lfo;
        plfos = (((((lfo >> 5) & 7) + 1) * m - 1) >> 2) & 0xE0;
        alfos = ((((lfo & 7) + 1) * m - 1) >> 7) & 7;
        AICA_CH[i].lfo = ((lfo & 0xFF18) | plfos | alfos) & ~0x8000u;
    }
}

/*
 * 0x1B44 ctl_rpn_data_set
 * Confidence: high
 * Stores an RPN data value (bit7 set) and interprets it. The RPN number is taken
 * from rpn_ref_msb/lsb (+0x20/+0x21), which nothing writes, so it is always
 * "RPN 0" and every RPN data entry sets the pitch-bend range (bend_range = v|0x80).
 * If the reference were 0x7F/0x7F (RPN null) the RPN selection would be cleared.
 * Also the target of CC96/CC97 on a selected RPN.
 */
static void ctl_rpn_data_set(MidiChannel *mc, u32 d)
{
    mc->rpn_data = (u8)d;
    if (mc->rpn_ref_msb & 0x7F) {
        if (mc->rpn_ref_msb != 0x7F || mc->rpn_ref_lsb != 0x7F)
            return;
        mc->rpn_data &= 0x7F;
        return;
    }
    if (mc->rpn_ref_lsb & 0x7F)
        return;
    mc->bend_range = (u8)d;
}

/*
 * 0x1B90 ctl_nrpn_data_set
 * Confidence: high
 * NRPN data is only stored (no NRPN is interpreted).
 */
static void ctl_nrpn_data_set(MidiChannel *mc, u32 d)
{
    mc->nrpn_data = (u8)d;
}

/*
 * 0x1B20 cc_data_entry (CC6)
 * Confidence: high
 * NRPN selected -> store only; RPN selected -> ctl_rpn_data_set; else ignored.
 * NRPN wins if both select bits were somehow set.
 */
static void cc_data_entry(MidiChannel *mc, u32 val)
{
    u32 d = (val & 0x7F) | 0x80;

    if (mc->nrpn_data & 0x80) {
        ctl_nrpn_data_set(mc, d);
        return;
    }
    if (!(mc->rpn_data & 0x80))
        return;
    ctl_rpn_data_set(mc, d);
}

/*
 * 0x1BA0 ctl_apply_volume (shared tail of CC7 and CC11)
 * Confidence: high
 * level = (curve[a]+1)*(curve[b]+1)-1 >> 7 with the built-in curve at 0x3F28
 * (a/b = volume/expression). Then for every sounding voice of the channel:
 * vol = (velcurve[vel]+1)*(level+1)*(255-split.tl+1)-1 >> 14 (same formula as
 * note-on), TL = 255 - clamp(vol + port volume - 0x80, 0, 255), keeping the
 * low 6 bits (Q/LPOFF) of the register.
 */
static void ctl_apply_volume(MidiChannel *mc, u32 port, u32 ch, u32 a, u32 b)
{
    s32 port_vol = (s32)g_port[port].volume - 0x80;
    u32 i;

    mc->level = (u8)(((VOL_CURVE[a] + 1) * (VOL_CURVE[b] + 1) - 1) >> 7);
    for (i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voice[i];
        const u8 *curve;
        u32 vol, tl;
        s32 t;

        if (!VOICE_ACTIVE(v) || v->port != port || v->ch != ch)
            continue;
        curve = (const u8 *)(mc->velcurve + (u32)v->vel_curve * 128);
        vol = (curve[v->velocity] + 1) * (mc->level + 1);
        vol = ((vol * ((SPLIT(v)->tl ^ 0xFF) + 1) - 1) >> 14) & 0xFF;
        v->vol = (u8)vol;
        t = port_vol + (s32)vol;
        if (t < 0)
            t = 0;
        else if (t & 0x100)
            t = 0xFF;
        tl = ((u32)t ^ 0xFF) & 0xFF;
        AICA_CH[i].tl_q = (AICA_CH[i].tl_q & 0x3F) | (tl << 8);
    }
}

/*
 * 0x1B98 cc_volume (CC7)
 * Confidence: high
 */
static void cc_volume(MidiChannel *mc, u32 port, u32 ch, u32 val)
{
    mc->volume = (u8)val;
    ctl_apply_volume(mc, port, ch, val, mc->expression);
}

/*
 * 0x1D24 cc_expression (CC11)
 * Confidence: high
 */
static void cc_expression(MidiChannel *mc, u32 port, u32 ch, u32 val)
{
    mc->expression = (u8)val;
    ctl_apply_volume(mc, port, ch, val, mc->volume);
}

/*
 * 0x1C98 cc_pan (CC10)
 * Confidence: high
 * pan = v (bit7 clear -> channel pan now overrides the tone pan at note-on).
 * Live voices get DIPAN from clamp(v + port pan - 0x80, 0, 127) via a byte
 * store to AICA reg 0x24 (DISDL in the upper byte is untouched).
 */
static void cc_pan(MidiChannel *mc, u32 port, u32 ch, u32 val)
{
    s32 p;
    u32 dipan, i;

    mc->pan = (u8)val;
    p = (s32)val + ((s32)g_port[port].pan - 0x80);
    if (p < 0)
        p = 0;
    else if (p & 0x80)
        p = 0x7F;
    dipan = pan_to_aica((u32)p);
    for (i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voice[i];
        if (!VOICE_ACTIVE(v) || v->ch != ch || v->port != port)
            continue;
        *(volatile u8 *)&AICA_CH[i].direct = (u8)dipan;
    }
}

/*
 * 0x1D34/0x1D64/0x1D94/0x1DC4/0x1E84 cc_flv_msb (CC20..24 -> FLV0..4)
 * 0x1FBC/0x1FDC/0x1FFC/0x201C/0x20CC cc_flv_lsb (CC52..56)
 * Confidence: high
 * 14-bit offsets flv_ofs[n] = MSB<<7 | LSB (0x2000 = none), applied at note-on.
 * Each also forces lpoff = 0x7F (filter on for new notes). FLV3 (CC23/CC55)
 * is additionally pushed to the sounding voices.
 */
static void cc_flv_msb(MidiChannel *mc, u32 port, u32 ch, u32 n, u32 val)
{
    mc->flv_ofs[n] = (u16)(((val & 0x7F) << 7) | (mc->flv_ofs[n] & 0x7F));
    if (n == 3)
        ctl_apply_flv3(mc, port, ch);
    mc->lpoff = 0x7F;
}

static void cc_flv_lsb(MidiChannel *mc, u32 port, u32 ch, u32 n, u32 val)
{
    mc->flv_ofs[n] = (u16)((mc->flv_ofs[n] & 0xFF80) | (val & 0x7F));
    if (n == 3)
        ctl_apply_flv3(mc, port, ch);
    mc->lpoff = 0x7F;
}

/*
 * 0x1EB4/0x1ECC/0x1EE4/0x1EFC cc_fenv (CC25..28)
 * Confidence: high
 * Filter EG rate offsets far/fd1r/fd2r/frr = v>>1 (0x20 = none), note-on only.
 * Also forces lpoff = 0x7F.
 */
static void cc_fenv(MidiChannel *mc, u32 n, u32 val)
{
    (&mc->far_ofs)[n] = (u8)((val >> 1) & 0x3F);
    mc->lpoff = 0x7F;
}

/*
 * 0x1F14 cc_bank_select (CC32)
 * Confidence: high
 * Melodic channel: bank_no = v; if bank table entry v has no address or size,
 * drv_err_flags |= 1 (the bank is still selected; program change then falls
 * back to the built-in bank). Drum channel (drum_kit bit0): only 0/1 accepted,
 * kit = bank entry 66+v; if loaded, bank_no = v and bank = kit, else
 * bank = built-in kit 0x4124 and error bit 0. prog/velcurve are not refreshed.
 * Notes: CC0 (bank MSB) is ignored; the driver uses CC32 as its bank number.
 */
static void cc_bank_select(MidiChannel *mc, u32 val)
{
    volatile u32 *e;

    if (!(mc->drum_kit & 1)) {
        mc->bank_no = (u8)val;
        e = bank_entry(val);
        if (e[0] != 0 && e[1] != 0)
            return;
    } else {
        if (val >= 2)
            return;
        e = bank_entry(BANK_KIT_FIRST + val);
        if (e[0] != 0 && e[1] != 0) {
            mc->bank_no = (u8)val;
            mc->bank = e[0];
            return;
        }
        mc->bank = DEFAULT_KIT;
    }
    drv_err_flags |= 1;
}

/*
 * 0x1FA0 cc_midi_timing (CC48)
 * Confidence: high
 * Global byte at 0x13414: v == 0 clears it, any other value increments it.
 * Not read by the driver; the SH-4 library reads it (as a u32) with
 * sdDrvGetMidiTimmingCounter, so sequences use CC48 as a timing/sync marker.
 */
static void cc_midi_timing(u32 val)
{
    midi_timing_cnt = val ? (u8)(midi_timing_cnt + 1) : 0;
}

/*
 * 0x20EC cc_damper (CC64)
 * Confidence: high
 * sustain = v & 0x40. Pedal down: VOICE_HELD on every sounding voice of the
 * channel. Pedal up: clear VOICE_HELD; voices already released by note-off
 * (KEYON clear) get age = 0 and are keyed off (KYONEX, KYONB clear).
 * Notes: pedal up keys off every active voice of the channel whose KEYON is
 * clear, HELD or not, so voices already in release (pedal never down, or
 * released before the pedal) get a second key-off. Pedal down also marks
 * already-released voices HELD. The key-off mask 0x3FF also clears SSCTL
 * (CC121 uses 0xBFFF and keeps it).
 */
static void cc_damper(MidiChannel *mc, u32 port, u32 ch, u32 val)
{
    u32 s = val & 0x40, i;

    mc->sustain = (u8)s;
    for (i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voice[i];

        if (!VOICE_ACTIVE(v) || v->port != port || v->ch != ch)
            continue;
        if (s) {
            v->flags |= VOICE_HELD;
            continue;
        }
        v->flags &= ~VOICE_HELD;
        if (v->flags & VOICE_KEYON)
            continue;
        v->age = 0;
        AICA_CH[i].ctl_sa_hi = (AICA_CH[i].ctl_sa_hi & 0x3FF) | AICA_KYONEX;
    }
}

/*
 * 0x21C8 cc_filter_switch (CC70)
 * Confidence: high
 * lpoff = v: 0 -> tone LPOFF, 1..63 -> filter off, 64..127 -> filter on (at note-on).
 */
static void cc_filter_switch(MidiChannel *mc, u32 val)
{
    mc->lpoff = (u8)(val & 0x7F);
}

/*
 * 0x21D4 cc_resonance (CC71, CC85)
 * Confidence: high
 * q_ofs = v; live voices: Q = clamp(split.q&0x1F + v-64, 0, 31), byte-written
 * to AICA reg 0x28 keeping LPOFF (VOFF is cleared by the byte store).
 */
static void cc_resonance(MidiChannel *mc, u32 port, u32 ch, u32 val)
{
    s32 d;
    u32 i;

    val &= 0x7F;
    mc->q_ofs = (u8)val;
    d = (s32)val - 0x40;
    for (i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voice[i];
        u32 lpoff;
        s32 q;

        if (!VOICE_ACTIVE(v) || v->port != port || v->ch != ch)
            continue;
        lpoff = *(volatile u8 *)&AICA_CH[i].tl_q & 0x20;
        q = (s32)(SPLIT(v)->q & 0x1F) + d;
        if (q < 0)
            q = 0;
        if (q & 0xE0)
            q = 0x1F;
        *(volatile u8 *)&AICA_CH[i].tl_q = (u8)((u32)q | lpoff);
    }
}

/*
 * 0x2254 CC72/86 -> rr_ofs, 0x2264 CC73/87 -> ar_ofs, 0x2424 CC88 -> dl_ofs,
 * 0x2434 CC89 -> dr_ofs, 0x2444 CC90 -> cc90_ofs (unused)
 * Confidence: high
 * Amplitude EG offsets = v>>1 (0x20 = none), applied at note-on only.
 */
static u8 cc_eg_value(u32 val)
{
    return (u8)((val >> 1) & 0x3F);
}

/*
 * 0x22DC..0x2380 cc_cutoff_k (CC110..117, k = 0..7)
 * Confidence: high
 * cutoff = (v - 64) << k, then FLV3 of live voices is refreshed (0x2390).
 */
static void cc_cutoff_k(MidiChannel *mc, u32 port, u32 ch, u32 k, u32 val)
{
    mc->cutoff = ((s32)(val & 0x7F) << k) - (0x40 << k);
    ctl_apply_flv3(mc, port, ch);
}

/*
 * 0x2274 cc_cutoff (CC74)
 * Confidence: high
 * Branches through the 9-entry table at 0x2284 by cutoff_range (CC75):
 * range 0 (default) and 8 -> k=7, range n (1..7) -> k=n-1.
 */
static void cc_cutoff(MidiChannel *mc, u32 port, u32 ch, u32 val)
{
    u32 r = mc->cutoff_range >> 2;
    u32 k = (r == 0 || r >= 8) ? 7 : r - 1;

    cc_cutoff_k(mc, port, ch, k, val);
}

/*
 * 0x22C4 cc_cutoff_range (CC75)
 * Confidence: high
 * cutoff_range = min(v, 8) * 4 (a jump-table byte offset). Not reset by CC121.
 */
static void cc_cutoff_range(MidiChannel *mc, u32 val)
{
    val &= 0x7F;
    if (val > 8)
        val = 8;
    mc->cutoff_range = (u8)(val << 2);
}

/*
 * 0x2454 cc_dsp_input (CC76)
 * Confidence: high
 * isel = (v & 0x70) ? 0x7F (use tone ISEL) : v & 0xF (DSP MIXS input for new notes).
 */
static void cc_dsp_input(MidiChannel *mc, u32 val)
{
    mc->isel = (val & 0x70) ? 0x7F : (u8)(val & 0xF);
}

/*
 * 0x246C cc_dsp_out_pan (CC77), 0x24A4 cc_dsp_out_level (CC78)
 * Confidence: high
 * Port 7 only. MIDI channel n (0..15) addresses DSP output slot EFREG n:
 * CC77 byte-writes EFPAN (pan_to_aica(v)) to AICA 0x2000+n*4,
 * CC78 byte-writes EFSDL = v>>3 to AICA 0x2001+n*4.
 */
static void cc_dsp_out_pan(u32 port, u32 ch, u32 val)
{
    if (port != EXT_MIDI_PORT)
        return;
    AICA_REG8(0x2000 + (ch & 0xF) * 4) = (u8)pan_to_aica(val);
}

static void cc_dsp_out_level(u32 port, u32 ch, u32 val)
{
    if (port != EXT_MIDI_PORT)
        return;
    AICA_REG8(0x2001 + (ch & 0xF) * 4) = (u8)((val >> 3) & 0xF);
}

/*
 * 0x24D0 cc_dsp_program (CC79)
 * Confidence: high
 * Port 7 only (any channel): v != 0 loads DSP effect program v-1 via
 * fx_prg_select (0x5C98), v == 0 stops the effect via fx_prg_clear (0x5DCC).
 * Same as host commands 0x84/0x85 (sdSndSetFxPrg / sdSndClearFxPrg).
 */
static void cc_dsp_program(u32 port, u32 val)
{
    if (port != EXT_MIDI_PORT)
        return;
    val &= 0x7F;
    if (val)
        fx_prg_select(val - 1);
    else
        fx_prg_clear();
}

/*
 * 0x250C cc_fx_dsp_pan (CC80)
 * Confidence: high
 * Pan of a DSP effect slot. Only if the loaded effect program has pan slots
 * (fx_pan_mode 0x13F1D: bit4 = 8 slots, stride 8 coefs; else bit5 = 4 slots,
 * stride 4). The MIDI channel selects the slot (ch&7 or ch&3); equal-power pan
 * preset v>>2 (0..31) of fx_pan_coef_tab (0x3C48, 4 gains each) is written to
 * COEF[fx_pan_coef_base + slot + k*stride], k = 0..3. Same copy as host command
 * 0x88 (fx_cmd_set_dsp_pan, 0x5F54) does for all slots. Any port.
 */
static void cc_fx_dsp_pan(u32 ch, u32 val)
{
    u32 fl = fx_pan_mode, slot, stride, k;
    const u32 *src;
    volatile u32 *dst;

    if (!(fl & 0x30))
        return;
    if (!(fl & 0x10)) {
        slot = ch & 3;
        stride = 4;
    } else {
        slot = ch & 7;
        stride = 8;
    }
    src = FX_PAN_COEF_TAB + (val & 0x7C);   /* (v&0x7C)*4 bytes = 16 bytes per preset */
    dst = &AICA_DSP_COEF(fx_pan_coef_base) + slot;
    for (k = 0; k < 4; k++)
        dst[k * stride] = src[k];
}

/*
 * 0x257C cc_reverb_send (CC91)
 * Confidence: high
 * v != 0: fx_send = 2v; IMXL = clamp((2v>>4) + port fx_send ofs, 0, 15) is
 * written with the *tone* ISEL (a CC76 override is lost) to live voices.
 * v == 0 (0x2620): fx_send = 0 (new notes use the tone IMXL) and live voices
 * get IMXL forced to 0.
 * Notes: the v == 0 path computes tone IMXL + port ofs - 0x20 (sic, not
 * -0x10), clamps it with a carry test instead of a sign test and then ANDs the
 * result with 0 ("and r1,r1,#0", probably meant #0xF), so live sends are
 * muted rather than restored to the tone default. ISEL is kept from the
 * split in both paths.
 */
static void cc_reverb_send(MidiChannel *mc, u32 port, u32 ch, u32 val)
{
    s32 lvl;
    u32 i;

    if (val & 0xFF) {
        val <<= 1;
        mc->fx_send = (u8)val;
        lvl = (s32)(val >> 4) + (s32)((g_port[port].fx_send >> 3) & 0x1F) - 0x10;
        if (lvl < 0)
            lvl = 0;
        else if (lvl & 0x10)
            lvl = 0xF;
        lvl &= 0xF;
    } else {
        mc->fx_send = 0;
        lvl = 0;        /* see Notes */
    }
    for (i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voice[i];

        if (!VOICE_ACTIVE(v) || v->port != port || v->ch != ch)
            continue;
        *(volatile u8 *)&AICA_CH[i].dsp_send =
            (u8)((SPLIT(v)->dsp_send & 0xF) | ((u32)lvl << 4));
    }
}

/*
 * 0x26BC cc_data_inc (CC96)
 * Confidence: high
 * Selected NRPN: nrpn_data + 1 ("adds/movcs #0xFF" can never saturate a byte,
 * so 0xFF wraps to 0x00 and the select bit is dropped).
 * Selected RPN: "add r1,r1,#1" has no S, so the following "movcs r1,#0xFF"
 * tests a stale carry. Every instruction since midi_event_pop's
 * "cmp write,read" leaves C unchanged (movs/tst with unrotated immediates or
 * plain register operands), so C = (write_ofs >= read_ofs) at pop time: set
 * unless the ring index had wrapped. Carry set -> the value is 0xFF outright
 * (not rpn_data+1), i.e. every increment sets the bend range to 127
 * semitones; carry clear -> rpn_data + 1 (0x100 stores as 0x00).
 * Notes: the C reconstructs the old read offset as read_ofs - 4; this is exact
 * unless the MIDI-in ISR overran the ring between the pop and this point.
 */
static void cc_data_inc(MidiChannel *mc)
{
    u32 d, carry;

    if (mc->nrpn_data & 0x80) {
        ctl_nrpn_data_set(mc, (u8)(mc->nrpn_data + 1));
        return;
    }
    if (!(mc->rpn_data & 0x80))
        return;
    carry = evq_write_ofs >= ((evq_read_ofs - 4) & 0xFFF);  /* stale C flag */
    d = carry ? 0xFF : (u8)(mc->rpn_data + 1);   /* strb truncates 0x100 */
    ctl_rpn_data_set(mc, d);
}

/*
 * 0x26F0 cc_data_dec (CC97)
 * Confidence: high
 * Selected NRPN: nrpn_data = max((nrpn_data&0x7F)-1, 0) | 0x80.
 * Selected RPN: BUG - the value decremented is again nrpn_data's (r1 still holds
 * +0x24), not rpn_data: rpn = max((nrpn_data&0x7F)-1, 0) | 0x80.
 */
static void cc_data_dec(MidiChannel *mc)
{
    s32 d;

    if (mc->nrpn_data & 0x80) {
        d = (s32)(mc->nrpn_data & 0x7F) - 1;
        if (d < 0)
            d = 0;
        ctl_nrpn_data_set(mc, (u32)d | 0x80);
        return;
    }
    if (!(mc->rpn_data & 0x80))
        return;
    d = (s32)(mc->nrpn_data & 0x7F) - 1;   /* sic */
    if (d < 0)
        d = 0;
    ctl_rpn_data_set(mc, (u32)d | 0x80);
}

/*
 * 0x2734 CC98 / 0x2754 CC99 (NRPN LSB/MSB), 0x2774 CC100 / 0x2794 CC101 (RPN LSB/MSB)
 * Confidence: high
 * Store the parameter byte and set the select bit of one data byte while
 * clearing the other. The stored number is never used (see ctl_rpn_data_set).
 */
static void cc_param_select(MidiChannel *mc, int is_msb, int is_rpn, u32 val)
{
    if (is_msb)
        mc->param_msb = (u8)val;
    else
        mc->param_lsb = (u8)val;
    if (is_rpn) {
        mc->rpn_data |= 0x80;
        mc->nrpn_data &= 0x7F;
    } else {
        mc->nrpn_data |= 0x80;
        mc->rpn_data &= 0x7F;
    }
}

/*
 * 0x27B4 cc_reset_all (CC121)
 * Confidence: high
 * Resets the controllers of ALL 16 channels of the port (r6 is the port's
 * channel base, not the channel): volume/level 100, expression 127, isel 0x7F,
 * pan 0xC0 (tone pan), q_ofs 0x40, EG/filter-EG offsets 0x20, flv_ofs 0x2000,
 * cutoff 0, bend_range 0, bend 0, mod 0, lpoff 0, sustain 0, fx_send 0.
 * Kept: program/bank, RPN/NRPN state, drum_kit, cutoff_range, cc... _0c/_1c.
 * Then every sounding voice of the port loses VOICE_HELD and, if already
 * released, is keyed off (age = 0). Live volume/pan/pitch are not refreshed.
 * Notes: level is set to 100 directly rather than recomputed from the curve
 * (curve(100)*curve(127) gives 119).
 */
static void cc_reset_all(u32 port)
{
    MidiChannel *mc = MIDI_CH(port, 0);
    u32 n, i;

    for (n = 0; n < NUM_MIDI_CH; n++, mc++) {
        mc->volume = 100;
        mc->level = 100;
        mc->expression = 127;
        mc->isel = 127;
        mc->pan = 0xC0;
        mc->q_ofs = 0x40;
        mc->far_ofs = mc->fd1r_ofs = mc->fd2r_ofs = mc->frr_ofs = 0x20;
        for (i = 0; i < 5; i++)
            mc->flv_ofs[i] = 0x2000;
        mc->ar_ofs = mc->dr_ofs = mc->cc90_ofs = mc->rr_ofs = mc->dl_ofs = 0x20;
        mc->cutoff = 0;
        mc->bend_range = 0;
        mc->bend = 0;
        mc->mod = 0;
        mc->lpoff = 0;
        mc->sustain = 0;
        mc->fx_send = 0;
    }
    for (i = 0; i < NUM_VOICES; i++) {
        Voice *v = &g_voice[i];

        if (!VOICE_ACTIVE(v) || v->port != port)
            continue;
        v->flags &= ~VOICE_HELD;
        if (v->flags & VOICE_KEYON)
            continue;
        v->age = 0;
        AICA_CH[i].ctl_sa_hi = (AICA_CH[i].ctl_sa_hi & 0xBFFF) | AICA_KYONEX;
    }
}

/*
 * 0x28C8 ctl_reset_port
 * Confidence: medium
 * Unreferenced alternative entry of cc_reset_all taking the port in r0
 * (r6 = 0xC400 + (r0&7)*0x480, then "b 0x27B4"). Ghidra had it as data.
 * Notes: it does not set r3, so the voice loop would compare against whatever
 * port the caller left in r3; the C uses the same port for both.
 */
void ctl_reset_port(u32 port)
{
    cc_reset_all(port & 7);
}

/* ---- control change dispatcher ------------------------------------------ */

/*
 * 0x185C ev_control_change
 * Confidence: high
 * Branch table at 0x1878 (128 x "b handler") indexed by data1 & 0x7F, with
 * val = data2 & 0x7F. Unlisted controllers hit "mov pc,lr" at 0x1A78.
 */
void ev_control_change(u32 ev)
{
    u32 port = EV_PORT(ev), ch = EV_CH(ev);
    MidiChannel *mc = MIDI_CH(port, ch);
    u32 cc = EV_DATA1(ev) & 0x7F;
    u32 val = ev & 0x7F;

    switch (cc) {
    case 1:   cc_modulation(mc, port, ch, val); break;
    case 6:   cc_data_entry(mc, val); break;
    case 7:   cc_volume(mc, port, ch, val); break;
    case 10:  cc_pan(mc, port, ch, val); break;
    case 11:  cc_expression(mc, port, ch, val); break;
    case 20: case 21: case 22: case 23: case 24:
        cc_flv_msb(mc, port, ch, cc - 20, val); break;
    case 25: case 26: case 27: case 28:
        cc_fenv(mc, cc - 25, val); break;
    case 32:  cc_bank_select(mc, val); break;
    case 48:  cc_midi_timing(val); break;
    case 52: case 53: case 54: case 55: case 56:
        cc_flv_lsb(mc, port, ch, cc - 52, val); break;
    case 64:  cc_damper(mc, port, ch, val); break;
    case 70:  cc_filter_switch(mc, val); break;
    case 71: case 85:
        cc_resonance(mc, port, ch, val); break;
    case 72: case 86:
        mc->rr_ofs = cc_eg_value(val); break;
    case 73: case 87:
        mc->ar_ofs = cc_eg_value(val); break;
    case 74:  cc_cutoff(mc, port, ch, val); break;
    case 75:  cc_cutoff_range(mc, val); break;
    case 76:  cc_dsp_input(mc, val); break;
    case 77:  cc_dsp_out_pan(port, ch, val); break;
    case 78:  cc_dsp_out_level(port, ch, val); break;
    case 79:  cc_dsp_program(port, val); break;
    case 80:  cc_fx_dsp_pan(ch, val); break;
    case 88:  mc->dl_ofs = cc_eg_value(val); break;
    case 89:  mc->dr_ofs = cc_eg_value(val); break;
    case 90:  mc->cc90_ofs = cc_eg_value(val); break;
    case 91:  cc_reverb_send(mc, port, ch, val); break;
    case 96:  cc_data_inc(mc); break;
    case 97:  cc_data_dec(mc); break;
    case 98:  cc_param_select(mc, 0, 0, val); break;
    case 99:  cc_param_select(mc, 1, 0, val); break;
    case 100: cc_param_select(mc, 0, 1, val); break;
    case 101: cc_param_select(mc, 1, 1, val); break;
    case 110: case 111: case 112: case 113:
    case 114: case 115: case 116: case 117:
        cc_cutoff_k(mc, port, ch, cc - 110, val); break;
    case 121: cc_reset_all(port); break;
    default:  break;                      /* 0x1A78 */
    }
}
