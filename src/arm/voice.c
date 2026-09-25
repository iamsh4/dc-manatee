/*
 * voice.c - note on/off, voice allocation, AICA channel programming and the
 * 4 ms per-voice update.
 *
 * Voice n (g_voice[n], 0xE800 + n*0x40) always drives AICA channel n; only
 * channels 0..47 are used. A note-on walks the channel's current program
 * (up to 4 layers), picks the first split of each layer whose key/velocity
 * window contains the note, allocates a voice for it and copies the split's
 * register images into the AICA channel, adjusted by the MIDI channel's
 * controller offsets and the port's volume/pan/pitch. Tone bank layout:
 * docs/notes/bank_format.md and the Bank/Tone structs in manatee.h.
 *
 * Pitch is carried as a 1/256-semitone value (u32, wrapping) and converted to
 * AICA OCT/FNS with two tables inside the image (voice_set_pitch).
 */
#include "manatee.h"

#define NUM_DRV_VOICES 48

/* Tables inside the driver image */
#define PITCH_OCT_TBL  SRAM_PTR(const u8, 0x3564) /* 256: (note+0x60-root) -> OCT<<4 | semitone */
#define PITCH_FNS_TBL  SRAM_PTR(const u8, 0x3648) /* 12*128: semitone*128 + frac/2 -> FNS low 8 bits */

static inline volatile AicaChannel *voice_aica(const Voice *v)
{
    return &AICA_CH[v - g_voice];
}

/* 5-bit EG/filter-rate clamp used all over note-on: <0 -> 0, >31 -> 31. */
static u32 clamp5(s32 x)
{
    if (x < 0)
        x = 0;
    if (x & 0xE0)
        x = 0x1F;
    return (u32)x;
}

/*
 * 0x0A44 note_off (entry 0x0A3C note_off_ev = dispatch case 0 computes
 * note = (ev>>8)&0x7F and falls through; note_on jumps to 0x0A44 for velocity 0)
 * Confidence: high
 * Releases every keyed voice (flags bit7) that plays port/ch/note. If the voice
 * is VOICE_HELD (damper was down at key-on) only the key bit is cleared and the
 * AICA channel keeps sounding (the sustain-pedal-off handler releases it).
 * Otherwise: age = 0, cancel a pending delayed key-on, level = 0xFF and
 * key the channel off (KYONEX with KYONB = 0, SA/PCMS/loop bits kept).
 * Notes: does not stop at the first match - all duplicates are released.
 */
void note_off(u32 port, u32 ch, u32 note)
{
    Voice *v = g_voice;
    volatile AicaChannel *ac = AICA_CH;
    int n;

    for (n = NUM_DRV_VOICES; n != 0; n--, v++, ac++) {
        if (!(v->flags & VOICE_KEYON) || v->port != port || v->ch != ch || v->note != note)
            continue;
        v->flags &= ~VOICE_KEYON;
        if (v->flags & VOICE_HELD)
            continue;
        v->age = 0;
        v->keyon_delay = 0;
        v->level = 0xFF;
        ac->ctl_sa_hi = (ac->ctl_sa_hi & 0x7FF) | AICA_KYONEX;
    }
}

/*
 * 0x1250 voice_alloc
 * Confidence: high
 * Picks a voice for a new note of priority prio (event flags). In the asm the
 * results are r6 = voice, r8 = AICA channel, r0 = 0 (ok) / 0xFF (no voice).
 * Rules, in voice order 0..47:
 *  - a completely idle voice (u32 at +0, i.e. flags and level, == 0) is taken at once;
 *  - voices with v->prio > prio are never stolen;
 *  - the first candidate is taken; after that a candidate replaces the current
 *    best depending on the best's level snapshot:
 *      best.level != 0 (best is audible or keyed & sampled): only voices with
 *        flags == 0 qualify; lower level wins; on equal level, prio >= best.prio wins;
 *      best.level == 0: a voice with flags == 0 wins outright; otherwise it wins
 *        if its prio <= best.prio and its age >= best.age (older voice).
 * Notes: the best record lives at 0x135C (voice, aica, level, prio, age, port);
 * the port byte is recorded but never used. "level" is maintained by
 * voice_monitor_sample (0xFF while not releasing, 0xFF-EG in release), so
 * among released voices the quietest is stolen. All comparisons are on bytes
 * (N flag of a byte subtraction = unsigned less-than).
 * BUG (reproduced): the record pointer r1 is left at 0x1368 after clearing the
 * record and is only reset to 0x135C when some voice passes the priority test.
 * If all 48 voices are busy with a higher priority, the result is read from
 * 0x1368/0x136C, i.e. the instruction words of ev_poly_pressure/ev_program_change
 * (0xE1A0F00E / 0xE0855006): non-zero, so r0 = 0 and note_on programs a bogus
 * "voice"/"AICA channel" at those addresses. Consequently the "no voice" result
 * (r0 = 0xFF, NULL here) is unreachable: if any voice passes the priority test
 * the first one is always recorded as best.
 */
static Voice *voice_alloc(u32 prio, volatile AicaChannel **ac_out)
{
    Voice *best = 0;
    int record_ptr_ok = 0;
    u8 best_level = 0, best_prio = 0, best_age = 0;
    Voice *v = g_voice;
    int n;

    for (n = NUM_DRV_VOICES; n != 0; n--, v++) {
        if (*(volatile u32 *)v == 0) {
            *ac_out = voice_aica(v);
            return v;
        }
        if (prio < v->prio)
            continue;
        record_ptr_ok = 1;
        if (best) {
            if (best_level != 0) {
                if (v->flags != 0)
                    continue;
                if (best_level == v->level) {
                    if (v->prio < best_prio)
                        continue;
                } else if (best_level < v->level) {
                    continue;
                }
            } else if (v->flags != 0) {
                if (best_prio < v->prio)
                    continue;
                if (v->age < best_age)
                    continue;
            }
        }
        best = v;
        best_level = v->level;
        best_prio = v->prio;
        best_age = v->age;
    }
    if (!record_ptr_ok) {                 /* see BUG above */
        *ac_out = *(volatile AicaChannel *const *)0x136Cu;
        return *(Voice *const *)0x1368u;
    }
    if (best)
        *ac_out = voice_aica(best);
    return best;
}

/*
 * 0x14BC voice_set_pitch
 * Confidence: high
 * Converts pitch (1/256 semitone offset from the split's root, wrapping u32)
 * plus the split's fine tune into AICA OCT/FNS and writes the channel pitch
 * register if it changed. In the asm: r5 = voice, r6 = split, r7 = pitch,
 * r8 = AICA channel (voice_set_pitch_ac takes it explicitly).
 *   idx  = (pitch>>8) + note + 0x60 - root, mod 256 -> PITCH_OCT_TBL gives
 *          OCT (4-bit signed, bits 7:4) and semitone (bits 3:0);
 *   fidx = semitone*128 + (pitch & 0xFE)/2 -> PITCH_FNS_TBL gives FNS bits 7:0,
 *          bits 9:8 come from comparing fidx with 0x1EF/0x383/0x4D9.
 * Notes: pitch>>8 is a logical shift but the &0xFF makes negative offsets
 * (and the 16-bit wrapped Voice.bend_ofs) come out right modulo 256 semitones.
 */
static void voice_set_pitch_ac(Voice *v, volatile AicaChannel *ac, const ToneSplit *sp, u32 pitch)
{
    u32 fine = *(const u8 *)&sp->fine;
    u32 t, oct, fidx, fns, val;

    if (fine & 0x80)
        fine |= 0xFFFFFF00u;
    pitch += fine;
    t = PITCH_OCT_TBL[((pitch >> 8) + v->note + 0x60 - (sp->root_key & 0x7F)) & 0xFF];
    oct = (t << 7) & 0x7800;
    fidx = (((t << 8) & 0xF00) | (pitch & 0xFE)) >> 1;
    fns = PITCH_FNS_TBL[fidx];
    if (fidx >= 0x4D9) fns += 0x100;
    if (fidx >= 0x383) fns += 0x100;
    if (fidx >= 0x1EF) fns += 0x100;
    val = oct | fns;
    if (ac->pitch != val)
        ac->pitch = val;
}

/* Public form: the AICA channel is the voice's own (always true except for the
 * voice_alloc bug case, where note_on uses voice_set_pitch_ac directly). */
void voice_set_pitch(Voice *v, const ToneSplit *sp, u32 pitch)
{
    voice_set_pitch_ac(v, voice_aica(v), sp, pitch);
}

/* FLVn = tone + (ofs - 0x2000) + cutoff, clamped to [8, 0x1FF8]. The asm clamps
 * negatives to 8 for FLV0/1 and to 0 for FLV2..4, but the following "< 8 -> 8"
 * makes both identical. Quirk kept: the upper clamp only tests bits 15:13 of
 * (v + 8), so v >= 0x10000 slips through (register keeps the low 13 bits). */
static u32 flv_calc(u32 tone, u32 ofs, s32 cutoff)
{
    s32 x = (s32)(tone & 0xFFFF) + (s32)((ofs & 0xFFFF) - 0x2000) + cutoff;

    if (x < 8)
        x = 8;
    if ((x + 8) & 0xE000)
        x = 0x1FF8;
    return (u32)x;
}

/*
 * Program one AICA channel for a split (0xBC8-0x11E8, the body of the note-on
 * split loop after a successful voice_alloc). Register write order kept:
 * CTL(key off) SA LSA LEA AD DR LFO DSP FLV0-4 FENV TL/Q DIRECT PITCH LFO CTL.
 */
static void voice_start(Voice *v, volatile AicaChannel *ac, MidiChannel *mc, PortState *ps,
                        const ToneLayer *ly, const ToneSplit *sp,
                        u32 port, u32 ch, u32 note, u32 vel, u32 prio)
{
    u32 ctl, sa, x, a, b, q, lp, tl, mod, g, pan, dl;
    s32 s;
    const u8 *curve;

    ac->ctl_sa_hi = AICA_KYONEX;          /* key off, clear SA/PCMS/loop */
    v->keyon_delay = ly->keyon_delay;
    v->split = sp;
    if (mc->bend_range & 0x80) {
        v->bend_up = v->bend_down = mc->bend_range & 0x7F;
    } else {
        v->bend_up = ly->bend_up;
        v->bend_down = ly->bend_down;
    }
    v->note = note;

    /* start address: bank-relative 23-bit SA split over ctl[6:0] and sa_lo */
    sa = ((((u32)sp->ctl & 0x7F) << 16) | sp->sa_lo) + mc->bank;
    ac->sa_lo = sa & 0xFFFF;
    ctl = (sp->ctl & 0x780) | ((sa >> 16) & 0x7F);   /* scratch 0x1230 */
    ac->lsa = sp->lsa;
    ac->lea = sp->lea;

    /* amplitude EG (quirk: D2R uses the D1R offset 0x38; 0x3A is never read) */
    x = sp->env_ad;
    a = clamp5((s32)(x & 0x1F) + mc->ar_ofs - 0x20);
    b = clamp5((s32)((x >> 6) & 0x1F) + mc->dr_ofs - 0x20);
    q = clamp5((s32)((x >> 11) & 0x1F) + mc->dr_ofs - 0x20);
    ac->env_ad = a | ((b | (q << 5)) << 6);
    x = sp->env_dr;
    a = clamp5((s32)(x & 0x1F) + mc->rr_ofs - 0x20);
    b = clamp5((s32)((x >> 5) & 0x1F) + mc->dl_ofs - 0x20);
    ac->env_dr = (x & 0x7C00) | (b << 5) | a;

    /* LFO: pitch/amp depth scaled by the modulation wheel */
    x = sp->lfo;
    mod = mc->mod + 1;
    x = (x & 0xFF1F) | (((((x >> 5) & 7) + 1) * mod - 1) >> 2 & 0xE0);
    x = (x & 0xFFF8) | (((((x & 7) + 1) * mod - 1) >> 7) & 7);
    ac->lfo = x;

    /* DSP send: IMXL = (channel override or tone) + port offset, ISEL */
    s = (s32)((ps->fx_send >> 3) & 0x1F) - 0x10;
    x = mc->fx_send ? mc->fx_send : sp->dsp_send;
    s += (s32)((x & 0xF0) >> 4);
    if (s < 0)
        s = 0;
    if (s & 0xF0)
        s = 0xF;
    x = (mc->isel & 0xF0) ? sp->dsp_send : mc->isel;
    ac->dsp_send = (x & 0xF) | ((u32)(s & 0xF) << 4);

    /* resonance + LPOFF (written with TL below; scratch 0x1234) */
    q = clamp5((s32)(sp->q & 0x1F) + mc->q_ofs - 0x40);
    if (mc->lpoff == 0)
        lp = sp->q & 0x20;
    else
        lp = (mc->lpoff & 0x40) ? 0 : 0x20;
    q |= lp;

    /* filter levels */
    ac->flv[0] = flv_calc(sp->flv[0], mc->flv_ofs[0], mc->cutoff);
    ac->flv[1] = flv_calc(sp->flv[1], mc->flv_ofs[1], mc->cutoff);
    ac->flv[2] = flv_calc(sp->flv[2], mc->flv_ofs[2], mc->cutoff);
    ac->flv[3] = flv_calc(sp->flv[3], mc->flv_ofs[3], mc->cutoff);
    ac->flv[4] = flv_calc(sp->flv[4], mc->flv_ofs[4], mc->cutoff);

    /* filter EG */
    x = sp->fenv_a;
    a = clamp5((s32)((x >> 8) & 0x1F) + mc->far_ofs - 0x20);
    b = clamp5((s32)(x & 0x1F) + mc->fd1r_ofs - 0x20);
    ac->fenv_a = b | (a << 8);
    x = sp->fenv_r;
    a = clamp5((s32)((x >> 8) & 0x1F) + mc->fd2r_ofs - 0x20);
    b = clamp5((s32)(x & 0x1F) + mc->frr_ofs - 0x20);
    ac->fenv_r = b | (a << 8);

    /* level: velocity curve x channel level x (256 - tone TL), 0..255 */
    v->vel_curve = sp->vel_curve;
    curve = (const u8 *)(mc->velcurve + (u32)sp->vel_curve * 128);
    v->velocity = vel;
    g = ((curve[vel] + 1) * (mc->level + 1) * (256 - sp->tl) - 1) >> 14 & 0xFF;
    v->vol = g;
    v->port_state = ps;
    s = (s32)ps->volume - 0x80 + (s32)g;
    if (s < 0)
        s = 0;
    else if (s & 0x100)
        s = 0xFF;
    tl = (s ^ 0xFF) & 0xFF;
    ac->tl_q = (q & 0x3F) | (tl << 8);

    /* pan: tone DIPAN -> 0..127 unless the channel pan overrides it */
    pan = sp->dipan & 0x1F;
    if (pan == 0x10)
        pan = 0;
    if (pan & 0x10)
        pan = (pan - 0x10) ^ 0xFF;
    pan = (pan + 0x10) << 2;
    if (!(mc->pan & 0x80))
        pan = mc->pan;
    pan &= 0x7F;
    v->pan = pan;
    s = (s32)ps->pan - 0x80 + (s32)pan;
    if (s < 0)
        s = 0;
    if (s & 0x80)
        s = 0x7F;
    s = (s >> 2) - 0x10;
    if (s < 0)
        s = (s ^ 0xFF) + 0x10;
    pan = s & 0x1F;
    s = (s32)((ps->direct_lvl >> 3) & 0x1F) - 0x10 + (s32)(sp->disdl & 0xF);
    if (s < 0)
        s = 0;
    if (s & 0x10)
        s = 0xF;
    dl = s & 0xF;
    ac->direct = pan | (dl << 8);

    v->port = port;
    v->prio = prio;
    v->ch = ch;

    /* pitch: port transpose + bend (range by direction) */
    x = (u32)v->pitch_mod + ps->pitch - 0x8000;
    a = (mc->bend & 0xFFFF0000) ? v->bend_down : v->bend_up;
    v->bend_ofs = ((u32)mc->bend * a >> 5) & 0xFFFF;
    voice_set_pitch_ac(v, ac, sp, x + v->bend_ofs);

    /* key on (unless delayed; voice_update_4ms keys it later) */
    v->flags |= VOICE_KEYON;
    if (mc->sustain & 0x40)
        v->flags |= VOICE_HELD;
    v->age = 1;
    v->level = 0;
    ctl &= 0x7FF;
    if (v->keyon_delay == 0)
        ctl |= AICA_KYONEX | AICA_KYONB;
    ac->lfo &= ~0x8000u;                  /* LFORE off */
    ac->ctl_sa_hi = ctl;
}

/*
 * 0x0AD8 note_on (midi_event_dispatch case 1)
 * Confidence: high
 * Velocity 0 -> note_off. Otherwise, for each of the program's 4 layers
 * (ToneProgram.layer[i] != 0, layer not muted by bit7 of num_splits), scans
 * the layer's splits for key_lo <= note <= key_hi and vel_lo <= vel <= vel_hi.
 * For a matching split a voice is allocated (voice_alloc with the event's
 * flag bits as priority); if that fails the scan would continue with the next
 * split (dead path: voice_alloc never fails, see its BUG note), otherwise the
 * voice is programmed (voice_start) and the layer is done.
 * Notes:
 *  - the original keeps port/prio/ch/note/vel and layer fields in scratch words
 *    at 0x1210-0x124C inside the image (see Ghidra labels).
 *  - the split count uses a decrement-and-branch-if-not-zero loop, so a layer
 *    with num_splits & 0x7F == 0 would scan 2^32 splits (reproduced).
 *  - a stolen voice keeps a stale VOICE_HELD bit (flags are OR-ed, not set).
 *  - register use across blocks checked: voice_alloc clobbers only r0/r1/r3/
 *    r6/r8/r10 and voice_set_pitch r1-r4/r7 (r2/r5/r6 saved around it), so the
 *    split pointer r2, split count r11, layer count r12 and channel r5 survive.
 *  - DIPAN/DISDL are read with an unaligned ldr at split+0x12 (ARM7 rotates the
 *    aligned word), which yields exactly the u16 at +0x12.
 */
void note_on(u32 ev)
{
    u32 port = EV_PORT(ev) & 0xF;
    u32 ch = EV_CH(ev);
    u32 note = (ev >> 8) & 0x7F;
    u32 vel = ev & 0x7F;
    u32 prio = EV_FLAGS(ev);
    MidiChannel *mc = MIDI_CH(EV_PORT(ev), ch);
    PortState *ps = &g_port[port];
    const u32 *layer_ofs;
    int i;

    if (vel == 0) {
        note_off(EV_PORT(ev), ch, note);
        return;
    }
    layer_ofs = (const u32 *)mc->prog;           /* scratch 0x1224 */
    for (i = 0; i < 4; i++, layer_ofs++) {
        const ToneLayer *ly;
        const ToneSplit *sp;
        u32 n;

        if (*layer_ofs == 0)
            continue;
        ly = (const ToneLayer *)(*layer_ofs + mc->bank);
        if (ly->num_splits & 0x80)
            continue;
        n = ly->num_splits & 0x7F;
        sp = (const ToneSplit *)(ly->splits + mc->bank);
        do {
            if (note >= sp->key_lo && note <= sp->key_hi &&
                vel >= sp->vel_lo && vel <= sp->vel_hi) {
                volatile AicaChannel *ac = 0;
                Voice *v = voice_alloc(prio, &ac);
                if (v) {
                    voice_start(v, ac, mc, ps, ly, sp, port, ch, note, vel, prio);
                    break;
                }
            }
            sp++;
        } while (--n);
    }
}

/*
 * 0x6C4C voice_update_4ms
 * Confidence: high
 * Called from the main loop on every 4 ms tick. For each non-idle voice:
 *  - age++ while keyed or held (u8, wraps);
 *  - if its port's vol_dirty: recompute TL from Voice.vol + port volume;
 *  - if pan_dirty: recompute DIPAN from Voice.pan + port pan;
 *  - if pitch_dirty: recompute pitch (port pitch + pitch_mod + bend_ofs);
 *  - count down keyon_delay and key the channel on (KYONEX|KYONB) at 0.
 * Then clears the three dirty flags of all 8 ports.
 * Notes: the port pointer comes from the 16-entry table at 0x6DB4
 * (0xC000 + i*0x80; entries 8..15 alias g_midi_ch and are never hit).
 * Quirk: the TL rewrite masks the old TL/Q register with 0x1F, so it clears
 * LPOFF (bit5) and VOFF (bit6): a port volume change re-enables the filter on
 * tones that had it off.
 */
void voice_update_4ms(void)
{
    Voice *v = g_voice;
    volatile AicaChannel *ac = AICA_CH;
    int n;

    for (n = NUM_DRV_VOICES; n != 0; n--, v++, ac++) {
        PortState *ps;
        s32 s;

        if (*(volatile u32 *)v == 0)
            continue;
        if (v->flags & (VOICE_KEYON | VOICE_HELD))
            v->age++;
        ps = &g_port[v->port];
        if (ps->vol_dirty) {
            s = (s32)v->vol + (s32)ps->volume - 0x80;
            if (s < 0)
                s = 0;
            else if (s & 0x100)
                s = 0xFF;
            ac->tl_q = (ac->tl_q & 0x1F) | ((u32)((s ^ 0xFF) & 0xFF) << 8);
        }
        if (ps->pan_dirty) {
            s = (s32)v->pan + (s32)ps->pan - 0x80;
            if (s < 0)
                s = 0;
            else if (s & 0x80)
                s = 0x7F;
            s = (s >> 2) - 0x10;
            if (s < 0)
                s = (s ^ 0xFF) + 0x10;
            ac->direct = (ac->direct & 0xFF00) | (u32)(s & 0x1F);
        }
        if (ps->pitch_dirty)
            voice_set_pitch_ac(v, ac, v->split,
                            ps->pitch - 0x8000 + (u32)v->pitch_mod + v->bend_ofs);
        if (v->keyon_delay != 0 && --v->keyon_delay == 0)
            ac->ctl_sa_hi |= AICA_KYONEX | AICA_KYONB;
    }
    for (n = 0; n < NUM_PORTS; n++) {
        g_port[n].vol_dirty = 0;
        g_port[n].pan_dirty = 0;
        g_port[n].pitch_dirty = 0;
    }
}

/*
 * 0x17B0 voices_key_off_all
 * Confidence: high
 * Clears VOICE_KEYON on all 48 voices and keys every channel off (KYONEX,
 * KYONB = 0, SA/PCMS/loop bits kept). Unlike note_off it ignores VOICE_HELD
 * and does not touch level/age/keyon_delay (a pending delayed key-on still
 * fires). No callers found by Ghidra (reached through a table or dead).
 */
void voices_key_off_all(void)
{
    Voice *v = g_voice;
    volatile AicaChannel *ac = AICA_CH;
    int n;

    for (n = NUM_DRV_VOICES; n != 0; n--, v++, ac++) {
        v->flags &= ~VOICE_KEYON;
        ac->ctl_sa_hi = (ac->ctl_sa_hi & 0x7FF) | AICA_KYONEX;
    }
}

/*
 * 0x17F4 voices_key_off_port
 * Confidence: high
 * Same as voices_key_off_all but only for voices whose port == port (r0).
 * Called from the sequencer at 0x7664.
 */
void voices_key_off_port(u32 port)
{
    Voice *v = g_voice;
    volatile AicaChannel *ac = AICA_CH;
    int n;

    for (n = NUM_DRV_VOICES; n != 0; n--, v++, ac++) {
        if (v->port != port)
            continue;
        v->flags &= ~VOICE_KEYON;
        ac->ctl_sa_hi = (ac->ctl_sa_hi & 0x7FF) | AICA_KYONEX;
    }
}

/* ------------------------------------------------------------------------- */
/* DSP effect program selection (lives at 0x5C98 among the fx routines; kept  */
/* here because it was assigned with the dispatcher; may move to fx.c)        */
/* ------------------------------------------------------------------------- */
#define fx_prg_sel      SRAM(u8, 0x5DC8)            /* last accepted index, copied to fx_prg_cur on load */
#define FX_RB_NEED_TAB  SRAM_PTR(const u32, 0x5DA0) /* {0x3FFF, 0x7FFF, 0xFFFF, 0x1FFFF} per RBL */

/*
 * 0x5C98 fx_prg_select
 * Confidence: high
 * Validates program `prg` of the current SFPB effect bank and queues it for
 * the main-loop DSP loader (0x61D4). Called from cc_dsp_program (0x24EC,
 * port 7 only) with value-1; value 0 calls 0x5DCC (DSP off) instead.
 * (Host command 0x84 enters 1 instruction earlier at 0x5C94 with r0 = cmd[2].)
 * Checks, each ORing an error bit into drv_err (0x13420) and returning it:
 *   magic != "SFPB" -> 2; version byte != 1 -> 8; no "ENDB" at size-4 -> 4;
 *   prg >= count (low byte of +0x0C, unsigned) -> 0x10; SFPW ring buffer
 *   missing or below 0x18000 -> 1; (pan_mode & 0x30) == 0 and ring size <
 *   need[RBL] -> rb_size*4 (bug, see below).
 * The SH-4 library's SDD_DRV_ERR_* codes are exactly these bits << 3
 * (0x08 not downloaded, 0x10 illegal id, 0x20 illegal end id, 0x40 illegal
 * version, 0x80 illegal number).
 * On success: fx_prg_sel = prg, fx_rb_pending = RBP (addr>>11 & 0xFFF) |
 * (RBL&3)<<13, fx_prg_pending = entry, fx_load_req = 1; returns 0.
 * Notes: fx_prg_sel is written as soon as prg passes the count check, i.e.
 * also when the SFPW checks then fail. RBL (entry byte +0x20) is not range-
 * checked; values > 3 read past the 4-entry need table. The ring address/size
 * checks are "cmp; bmi" (N flag of the difference, not a true signed <); the
 * C reproduces that. The error return is drv_err (byte) | err as a full word,
 * so it can exceed 0xFF in the bug case while drv_err keeps the low byte.
 */
u32 fx_prg_select(u32 prg)
{
    const FxBankHeader *bank = (const FxBankHeader *)fx_prg_bank;
    const FxProgram *ent;
    u32 err, rbl, rb_addr, need, r;

    if (bank == 0)
        bank = (const FxBankHeader *)0x2908u;
    if (bank->magic != FX_SFPB_MAGIC) {
        err = DRV_ERR_ILLEGAL_ID;
    } else if ((u8)bank->version != 1) {
        err = DRV_ERR_ILLEGAL_VER;
    } else if (*(const u32 *)((u32)bank + bank->size - 4) != FX_ENDB_MAGIC) {
        err = DRV_ERR_ILLEGAL_END;
    } else if (prg >= (u8)bank->count) {
        err = DRV_ERR_ILLEGAL_NUM;
    } else {
        fx_prg_sel = (u8)prg;
        ent = (const FxProgram *)((u32)bank + bank->offset[prg]);
        rbl = ent->rb_size;
        need = FX_RB_NEED_TAB[rbl];
        rb_addr = fx_wrk_addr;
        if (rb_addr == 0 || (s32)(rb_addr - 0x18000) < 0) {
            err = DRV_ERR_NO_DOWNLOAD;
        } else if (!(ent->pan_mode & 0x30) && (s32)(fx_wrk_size - need) < 0) {
            /* BUG (0x5D58): the asm exits with r1 still holding rb_size*4 (the
             * need-table index) instead of 1, so drv_err gets 0/4/8/0xC for
             * rb_size 0..3: nothing, or the "illegal end" and/or "illegal
             * version" bits. With rb_size 0 the call reports no error at all
             * (returns the old drv_err) but queues nothing. */
            err = rbl * 4;
        } else {
            fx_rb_pending = ((rb_addr >> 11) & 0xFFF) | ((rbl * 4 & 0xC) << 11);
            fx_prg_pending = (u32)ent;
            fx_load_req = 1;
            return 0;
        }
    }
    r = drv_err | err;
    drv_err = (u8)r;
    return r;
}
