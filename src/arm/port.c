/*
 * port.c - per-port fades, PCM player (one-shot / PCM stream) monitoring and the
 * status records exported to the SH-4.
 *
 * MIDI ports (PortState, 0xC000): host commands 0x05-0x08 set a target and a fade
 * (interval, step); port_*_fade move the current value one step towards the
 * target every <interval>+1 ticks of 4 ms and raise the matching "dirty" byte,
 * which voice_update_4ms consumes to re-apply volume/pan/pitch to the voices of
 * the port. The speed fade drives the sequencer's tempo_scale instead.
 *
 * PCM players (PcmPlayer, 0x11000): 8 one-shot ports and 16 PCM-stream channels,
 * each keyed on one of AICA channels 48-63 (PcmSlot table at 0x12080). Their
 * speed and pitch fades feed pcm_pitch_update; volume and pan fades are done in
 * pcm_update, which also writes TL/DIPAN/pitch straight to the AICA channel.
 */
#include "manatee.h"

extern const u8 note_oct_tab[];        /* 0x3564 */
extern const u8 fns_lo_tab[];          /* 0x3648 */
extern const u8 pan_aica_to_lin_a[32]; /* 0x7310 */
extern const u8 pan_lin_to_aica_a[32]; /* 0x7330 */
extern const u8 oct_flip_tab[16];      /* 0x7E38 */

#define AICA_CH_BYTE(ch, off) (((volatile u8 *)(ch))[off])

/*
 * 0x6454 port_clear_play
 * Confidence: high
 * Clears PORT_FLG_PLAY in the port status (end of sequence, from seq_step).
 */
void port_clear_play(u32 port)
{
    g_port[port].status &= ~PORT_FLG_PLAY;
}

/*
 * 0x6470 port_status_export
 * Confidence: high
 * Every 4 ms: publishes the port state for the sd library getters
 * (docs/research/host_protocol.md 4.2):
 *   0x13600 + i*0x20  MIDI port i      (PortState i, SeqPlayer i)
 *   0x13800 + i*0x20  one-shot port i  (PcmPlayer i)
 *   0x13A00 + j*0x20  stream channel j (PcmPlayer 8+j; j = port*2 + ch)
 * All 0x80/0x8000-biased values are exported unbiased (signed).
 * Notes: MIDI flags get 0x80 (TROUBLE) when either error byte is set; PCM flags
 * are copied as is. PCM position fields (+0x0C, +0x14, +0x18) are only refreshed
 * while the channel is keyed on. One-shot +0x14 = (LEA-LSA of the AICA channel)
 * * loop_count + CA, using only the LOW BYTE of loop_count (ldrb). Stream +0x14
 * = loop_count * ring length in samples + CA (length in bytes >>1 for 16-bit,
 * <<1 for ADPCM).
 */
void port_status_export(void)
{
    PortState *ps = g_port;
    SeqPlayer *sp = g_seq_player;
    PcmPlayer *pl;
    volatile PortStatus *st = g_midi_status;
    int i;
    u8 f;

    for (i = 0; i < NUM_PORTS; i++, ps++, sp++, st++) {
        st->vol = (s8)(ps->volume - 0x80);
        st->pan = (s8)(ps->pan - 0x80);
        st->pitch = (s32)(ps->pitch - 0x8000);
        st->speed = (s32)(ps->speed - 0x8000);
        st->direct_lvl = (s8)(ps->direct_lvl - 0x80);
        st->fx_lvl = (s8)(ps->fx_send - 0x80);
        st->cur_adr = (s32)(sp->cur - sp->base);
        st->total = sp->beats;
        st->loops = sp->loop_iter;
        st->err = ps->err;
        st->err_hi = ps->err_hi;
        f = ps->status;
        if (ps->err | ps->err_hi)
            f |= PORT_FLG_TROUBLE;
        st->flags = f;
    }

    pl = g_shot_player;
    st = g_shot_status;
    for (i = 0; i < NUM_SHOT_PORTS; i++, pl++, st++) {
        st->vol = (s8)(pl->volume - 0x80);
        st->pan = (s8)(pl->pan - 0x80);
        st->speed = (s32)(pl->speed - 0x8000);
        st->pitch = (s32)(pl->pitch - 0x8000);
        st->direct_lvl = (s8)(pl->direct_lvl - 0x80);
        st->fx_lvl = (s8)(pl->fx_lvl - 0x80);
        st->flags = pl->flags;
        if (pl->flags & PORT_FLG_PLAY) {
            volatile AicaChannel *ch = &AICA_CH[pl->slot + 0x30];
            st->cur_adr = (s32)pl->cur_pos;
            st->loops = pl->loops_left;
            st->total = (ch->lea - ch->lsa) * (u8)pl->loop_count + pl->cur_pos;
        }
    }

    pl = g_pstm_player;
    st = g_pstm_status;
    for (i = 0; i < NUM_PSTM_CH; i++, pl++, st++) {
        st->vol = (s8)(pl->volume - 0x80);
        st->pan = (s8)(pl->pan - 0x80);
        st->speed = (s32)(pl->speed - 0x8000);
        st->pitch = (s32)(pl->pitch - 0x8000);
        st->direct_lvl = (s8)(pl->direct_lvl - 0x80);
        st->fx_lvl = (s8)(pl->fx_lvl - 0x80);
        st->flags = pl->flags;
        if (pl->flags & PORT_FLG_PLAY) {
            u32 len = pl->ring_len;
            st->cur_adr = (s32)pl->cur_pos;
            st->loops = pl->loop_count;
            if ((pl->pcms & 3) == 0)
                len >>= 1;              /* 16-bit: bytes -> samples */
            else if (pl->pcms & 2)
                len <<= 1;              /* ADPCM */
            st->total = pl->loop_count * len + pl->cur_pos;
        }
    }
}

/*
 * 0x6698 ticks_since_cmd_inc
 * Confidence: high
 * ticks_since_cmd (0x13410)++ every 4 ms. host_cmd_exec stores it in the
 * command history record (+0x0C) and resets it, so the log shows the 4 ms
 * ticks between host commands.
 */
void ticks_since_cmd_inc(void)
{
    ticks_since_cmd++;
}

/* Result of one fade update */
enum { FADE_DONE, FADE_WAIT, FADE_STEP };

/*
 * Common body of the u8 fades (port volume 0x66B8 and pan 0x6758).
 * current == target -> DONE. Else if the countdown is non-zero it is decremented
 * (WAIT). Else the countdown is reloaded with interval and the value moves by
 * step towards the target, clamped at the target (and at 0 when decreasing);
 * interval 0 jumps to the target (STEP).
 */
static int fade_u8(u8 *cur, u8 target, u8 *count, u8 interval, u8 step)
{
    u32 c = *cur, t = target;

    if (c == t)
        return FADE_DONE;
    if (*count != 0) {
        (*count)--;
        return FADE_WAIT;
    }
    *count = interval;
    if (interval == 0) {
        c = t;
    } else if (c < t) {
        c += step;
        if (c >= t)
            c = t;
    } else {
        c = (c >= step) ? c - step : 0;
        if (t >= c)
            c = t;
    }
    *cur = (u8)c;
    return FADE_STEP;
}

/*
 * Common body of the u32 fades (port speed/pitch, PCM speed/pitch), as fade_u8.
 * down_bug reproduces "subs; movcs r3,#0" (0x693C, 0x69E4): when decreasing,
 * the result is forced to 0 exactly when there was NO borrow, so a normal
 * downward step jumps straight to the target; if cur < step the value wraps to
 * cur-step (~0xFFFFxxxx, target >= it is false) and is stored; the next step
 * (from that huge value) then lands on the target.
 */
static int fade_u32(u32 *cur, u32 target, u8 *count, u8 interval, u32 step, int down_bug)
{
    u32 c = *cur;

    if (c == target)
        return FADE_DONE;
    if (*count != 0) {
        (*count)--;
        return FADE_WAIT;
    }
    *count = interval;
    if (interval == 0) {
        c = target;
    } else if (c < target) {
        c += step;
        if (c >= target)
            c = target;
    } else {
        if (down_bug)
            c = (c >= step) ? 0 : c - step;
        else
            c = (c >= step) ? c - step : 0;
        if (target >= c)
            c = target;
    }
    *cur = c;
    return FADE_STEP;
}

/*
 * 0x66B8 port_vol_fade
 * Confidence: high
 * Volume fade of the 8 MIDI ports (vol_target/volume/vol_interval/vol_count/
 * vol_step at +0..+4). On a step sets vol_dirty and PORT_FLG_CHG_VOL; clears
 * CHG_VOL when the target is reached.
 */
void port_vol_fade(void)
{
    PortState *ps = g_port;
    int i;

    for (i = 0; i < NUM_PORTS; i++, ps++) {
        u8 st = ps->status;
        switch (fade_u8(&ps->volume, ps->vol_target, &ps->vol_count, ps->vol_interval, ps->vol_step)) {
        case FADE_DONE:
            st &= ~PORT_FLG_CHG_VOL;
            break;
        case FADE_STEP:
            ps->vol_dirty = 1;
            st |= PORT_FLG_CHG_VOL;
            break;
        }
        ps->status = st;
    }
}

/*
 * 0x6758 port_pan_fade
 * Confidence: high
 * Pan fade of the 8 MIDI ports (+6..+0xA), sets pan_dirty on a step.
 * Notes: QUIRK: while fading it sets PORT_FLG_CHG_VOL (0x04, copy-paste from the
 * volume fade) instead of CHG_PAN; CHG_PAN (0x10) is only ever cleared here.
 */
void port_pan_fade(void)
{
    PortState *ps = g_port;
    int i;

    for (i = 0; i < NUM_PORTS; i++, ps++) {
        u8 st = ps->status;
        switch (fade_u8(&ps->pan, ps->pan_target, &ps->pan_count, ps->pan_interval, ps->pan_step)) {
        case FADE_DONE:
            st &= ~PORT_FLG_CHG_PAN;
            break;
        case FADE_STEP:
            ps->pan_dirty = 1;
            st |= PORT_FLG_CHG_VOL;     /* sic */
            break;
        }
        ps->status = st;
    }
}

/*
 * 0x67F8 port_speed_fade
 * Confidence: high
 * Speed fade of the 8 MIDI ports (+0x18..+0x22, step = speed_step*16). After
 * each step the sequencer tempo of the port is set:
 *   tempo_scale = max(0, (speed & 0xFFFF) - 0x8000 + 0x4000)
 * i.e. 0x8000 -> 0x4000 = 1x (the library sends speed*4 + 0x8000, so tempo =
 * 1 + speed/4096; speeds below -0x1000 stop the sequence).
 * Notes: tempo_scale is only written on a step, never while waiting/done.
 */
void port_speed_fade(void)
{
    PortState *ps = g_port;
    SeqPlayer *sp = g_seq_player;
    int i;

    for (i = 0; i < NUM_PORTS; i++, ps++, sp++) {
        u8 st = ps->status;
        switch (fade_u32(&ps->speed, ps->speed_target, &ps->speed_count, ps->speed_interval,
                         (u32)ps->speed_step << 4, 0)) {
        case FADE_DONE:
            st &= ~PORT_FLG_CHG_SPEED;
            break;
        case FADE_STEP: {
            s32 scale = (s32)(ps->speed & 0xFFFF) - 0x8000 + 0x4000;
            sp->tempo_scale = scale < 0 ? 0 : (u32)scale;
            st |= PORT_FLG_CHG_SPEED;
            break;
        }
        }
        ps->status = st;
    }
}

/*
 * 0x68C4 port_pitch_fade
 * Confidence: high
 * Pitch fade of the 8 MIDI ports (+0x0C..+0x16, step = pitch_step*2, 1/256
 * semitone units). On a step sets pitch_dirty and PORT_FLG_CHG_PITCH.
 * Notes: QUIRK: downward fades jump to the target on the first step (movcs bug,
 * see fade_u32).
 */
void port_pitch_fade(void)
{
    PortState *ps = g_port;
    int i;

    for (i = 0; i < NUM_PORTS; i++, ps++) {
        u8 st = ps->status;
        switch (fade_u32(&ps->pitch, ps->pitch_target, &ps->pitch_count, ps->pitch_interval,
                         (u32)ps->pitch_step << 1, 1)) {
        case FADE_DONE:
            st &= ~PORT_FLG_CHG_PITCH;
            break;
        case FADE_STEP:
            ps->pitch_dirty = 1;
            st |= PORT_FLG_CHG_PITCH;
            break;
        }
        ps->status = st;
    }
}

/*
 * 0x6970 pcm_speed_fade
 * Confidence: high
 * Speed fade of all 24 PCM players (+0x40..+0x4A, step*2). On a step sets
 * pitch_dirty = 0xFF (pcm_pitch_update) and PORT_FLG_CHG_SPEED.
 * Notes: QUIRK: same downward "movcs" bug as port_pitch_fade.
 */
void pcm_speed_fade(void)
{
    PcmPlayer *pl = g_pcm_player;
    int i;

    for (i = 0; i < NUM_SHOT_PORTS + NUM_PSTM_CH; i++, pl++) {
        u8 f = pl->flags;
        switch (fade_u32(&pl->speed, pl->speed_target, &pl->speed_count, pl->speed_interval,
                         (u32)pl->speed_step << 1, 1)) {
        case FADE_DONE:
            f &= ~PORT_FLG_CHG_SPEED;
            break;
        case FADE_STEP:
            pl->pitch_dirty = 0xFF;
            f |= PORT_FLG_CHG_SPEED;
            break;
        }
        pl->flags = f;
    }
}

/*
 * 0x6A20 pcm_pitch_fade
 * Confidence: high
 * Pitch fade of all 24 PCM players (+0x4C..+0x56, step*2), correct clamping.
 * On a step sets pitch_dirty = 0xFF and PORT_FLG_CHG_PITCH.
 */
void pcm_pitch_fade(void)
{
    PcmPlayer *pl = g_pcm_player;
    int i;

    for (i = 0; i < NUM_SHOT_PORTS + NUM_PSTM_CH; i++, pl++) {
        u8 f = pl->flags;
        switch (fade_u32(&pl->pitch, pl->pitch_target, &pl->pitch_count, pl->pitch_interval,
                         (u32)pl->pitch_step << 1, 0)) {
        case FADE_DONE:
            f &= ~PORT_FLG_CHG_PITCH;
            break;
        case FADE_STEP:
            pl->pitch_dirty = 0xFF;
            f |= PORT_FLG_CHG_PITCH;
            break;
        }
        pl->flags = f;
    }
}

/*
 * Body of pcm_pitch_update (inlined twice in the asm): converts a 0x8000-biased
 * pitch in 1/256 semitone to a signed "linear" AICA pitch offset (octave<<10 |
 * FNS) with the note/FNS tables used for voice pitch.
 *   x = v - 0x8000; negative x becomes (~x) | 0x8000 (magnitude-1, sign bit 15)
 *   e = note_oct_tab[0x60 + (x>>8 & 0x7F)]  (octave<<4 | semitone; 0x7F = range)
 *   fns = fns_lo_tab[semitone*128 + (x>>1 & 0x7F)] + 0x100 per threshold
 *   r = (octave << 10) | fns, or 0x1FFF if out of range; negated if negative.
 */
static s32 pitch_to_linear(u32 v)
{
    u32 x = v - 0x8000, hi, e, idx, fns, r;

    if ((s32)x < 0)
        x = ~x | 0x8000;
    hi = x >> 8;
    e = note_oct_tab[(hi & 0x7F) + 0x60];
    if (e == 0x7F) {
        r = 0x1FFF;
    } else {
        idx = ((x >> 1) & 0x7F) | ((e & 0xF) << 7);
        fns = fns_lo_tab[idx];
        if (idx >= 0x4D9)
            fns += 0x100;
        if (idx >= 0x383)
            fns += 0x100;
        if (idx >= 0x1EF)
            fns += 0x100;
        r = ((e & 0xF0) << 6) | fns;
    }
    if (hi & 0x80)
        r = ~r + 1;
    return (s32)r;
}

/*
 * 0x6AD4 pcm_pitch_update
 * Confidence: high
 * For every PCM player with pitch_dirty set: clears it and sets
 *   pitch_ofs = pitch_to_linear(speed) + pitch_to_linear(pitch)
 * (speed and pitch both just transpose a PCM player). pcm_update adds pitch_ofs
 * to the sample's base pitch when it writes the AICA pitch register.
 * Notes: the speed term is parked in an image word (0x6C40) between the two
 * conversions.
 */
void pcm_pitch_update(void)
{
    PcmPlayer *pl = g_pcm_player;
    int i;

    for (i = 0; i < NUM_SHOT_PORTS + NUM_PSTM_CH; i++, pl++) {
        s32 speed_term;
        if (pl->pitch_dirty == 0)
            continue;
        pl->pitch_dirty = 0;
        speed_term = pitch_to_linear(pl->speed);
        pl->pitch_ofs = pitch_to_linear(pl->pitch) + speed_term;
    }
}

/*
 * 0x7178 pcm_update
 * Confidence: high
 * Per PCM player every 4 ms (from pcm_tick). If keyed on, selects the player's
 * AICA channel for monitoring (MSLC = 48+slot) so that shot_monitor /
 * stream_monitor can read its EG/CA. Then runs the volume fade (flags bit2,
 * +0x14/15/18/1C/1E) and the pan fade (bit4, +0x16/17/19/1D/1F) and, when keyed
 * on, writes the results to the channel:
 *   TL (byte +0x29)    = clamp(0x100 - 2*volume + tl_base, 0, 255)   -> pl->tl
 *   DIPAN (byte +0x24) = lin_to_aica[clamp(aica_to_lin[pan_base] + pan/4 - 32, 0, 31)] -> pl->dipan
 * and finally the pitch register (pcm_apply_pitch, host_helpers.c). Returns the new flags, which
 * pcm_tick stores back.
 * Notes: the fade logic differs from the port fades: it counts down first and
 * only compares with the target when the count reaches 0; a count of 0 on entry
 * means "jump to target". QUIRK: decreasing has no clamp at 0, so if volume/pan
 * < step (and > target) the u32 result wraps and its low byte is stored (e.g.
 * 5 - 0x10 -> 0xF5), and the fade continues from there; the TL/DIPAN of that
 * tick are computed from the full wrapped u32 (-> TL 0xFF, pan fully one side). When the step lands
 * exactly on or past the target the count is not reloaded, so the next tick
 * finishes the fade via the "count == 0" path. asm: r12 = player, r9 = 0x802800,
 * leaves r10/r11 = slot entry / AICA channel for the callers.
 */
u32 pcm_update(PcmPlayer *pl)
{
    u32 f = pl->flags;
    volatile AicaChannel *ch = 0;
    u32 v, t;
    s32 x;

    if (f & PORT_FLG_PLAY) {
        ch = g_pcm_slot[pl->slot].ch;
        AICA_MSLC_B = pl->slot + 0x30;
    }

    if (f & PORT_FLG_CHG_VOL) {
        if (pl->vol_count == 0) {
            f &= ~PORT_FLG_CHG_VOL;
            v = pl->vol_target;
            goto set_vol;
        }
        if (--pl->vol_count != 0)
            goto pan;
        v = pl->volume;
        t = pl->vol_target;
        if (v == t) {
            f &= ~PORT_FLG_CHG_VOL;
            goto pan;
        }
        if (v > t) {
            v -= pl->vol_step;
            if (v > t)
                pl->vol_count = pl->vol_interval;
            else if (v < t)
                v = t;
        } else {
            v += pl->vol_step;
            if (v < t)
                pl->vol_count = pl->vol_interval;
            else if (v > t)
                v = t;
        }
set_vol:
        pl->volume = (u8)v;
        if (f & PORT_FLG_PLAY) {
            x = (s32)(0x100 - v * 2 + pl->tl_base);     /* full (possibly wrapped) v */
            if (x < 0)
                x = 0;
            if (x > 0xFF)
                x = 0xFF;
            AICA_CH_BYTE(ch, 0x29) = (u8)x;
            pl->tl = (u8)x;
        }
    }
pan:
    if (f & PORT_FLG_CHG_PAN) {
        if (pl->pan_count == 0) {
            f &= ~PORT_FLG_CHG_PAN;
            v = pl->pan_target;
            goto set_pan;
        }
        if (--pl->pan_count != 0)
            goto pitch;
        v = pl->pan;
        t = pl->pan_target;
        if (v == t) {
            f &= ~PORT_FLG_CHG_PAN;
            goto pitch;
        }
        if (v > t) {
            v -= pl->pan_step;
            if (v > t)
                pl->pan_count = pl->pan_interval;
            else if (v < t)
                v = t;
        } else {
            v += pl->pan_step;
            if (v < t)
                pl->pan_count = pl->pan_interval;
            else if (v > t)
                v = t;
        }
set_pan:
        pl->pan = (u8)v;
        if (!(f & PORT_FLG_PLAY))
            return f;
        x = (s32)((v >> 2) - 0x20 + pan_aica_to_lin_a[pl->pan_base]); /* full v */
        if (x < 0)
            x = 0;
        if (x > 0x1F)
            x = 0x1F;
        AICA_CH_BYTE(ch, 0x24) = pan_lin_to_aica_a[x];
        pl->dipan = pan_lin_to_aica_a[x];
    }
pitch:
    if (f & PORT_FLG_PLAY)
        pcm_apply_pitch(pl, ch, pl->pitch_base);   /* 0x7DEC, tail call at 0x730C */
    return f;
}

/*
 * 0x7350 shot_monitor
 * Confidence: high
 * One-shot player that is keyed on (flags bit0), after pcm_update selected its
 * channel for monitoring. Waits ~30 reads for the monitor to settle, then reads
 * the AICA LP flag (MON_EG bit 15, set when the loop end / sample end was passed):
 *  - LP set, channel looping (LPCTL) and loops_left == 0 (forever) or still > 0
 *    after decrementing: loop_count++.
 *  - LP set otherwise (one-shot ended, or last loop done): key off the channel,
 *    release the player and its slot (0x8204, preserving loop_mode and fx_ch) and clear
 *    flags bit0. Returns immediately.
 * Then (not released) updates cur_pos/cur_addr from MON_CA (see stream_monitor).
 * Returns the new flags.
 * Notes: the settle loop re-reads pl->start 30 times (ldr r2,[r12,#0x30]); r2 is
 * then used as the start address for cur_addr. Release (0x8204/0x8210, PCM area)
 * is reproduced inline.
 */
static void pcm_store_pos(PcmPlayer *pl, u32 start);

u32 shot_monitor(PcmPlayer *pl, u32 f)
{
    PcmSlot *slot = &g_pcm_slot[pl->slot];
    volatile AicaChannel *ch = slot->ch;
    u32 start = 0;
    int i;

    for (i = 0; i < 30; i++)
        start = *(volatile u32 *)&pl->start;
    if (AICA_MON_EG & 0x8000) {
        if (ch->ctl_sa_hi & AICA_LPCTL) {
            if (pl->loops_left == 0 || --pl->loops_left != 0) {
                pl->loop_count++;
                pcm_store_pos(pl, start);
                return f;
            }
        }
        ch->ctl_sa_hi = (ch->ctl_sa_hi | AICA_KYONEX) & ~AICA_KYONB;
        {
            u8 keep_mode = pl->loop_mode, keep_fx_ch = pl->fx_ch;
            pcm_release(pl, slot);      /* 0x8204 -> 0x8210 (host_helpers.c) */
            pl->loop_mode = keep_mode;
            pl->fx_ch = keep_fx_ch;
        }
        return f & ~PORT_FLG_PLAY;
    }
    pcm_store_pos(pl, start);
    return f;
}

/*
 * 0x73F4 (tail shared by shot_monitor and stream_monitor) pcm_store_pos
 * Confidence: high
 * cur = MON_CA; if non-zero: cur_pos = cur and cur_addr = start + cur scaled by
 * pcms (>1: cur*2, 0: cur/2, 1: see BUG).
 * Notes: QUIRK: the scaling is inverted for a samples->bytes conversion
 * (16-bit gives cur/2, ADPCM cur*2). BUG: for pcms == 1 (8-bit) neither movgt
 * nor movlt fires, so r1 still holds pcms and cur_addr = start + 1 (not
 * start + cur). cur_addr is not exported by
 * port_status_export (the SH-4 converts CA itself), so only readers of +0x08 in
 * the PCM area are affected.
 */
static void pcm_store_pos(PcmPlayer *pl, u32 start)
{
    u32 cur = AICA_MON_CA, v;

    if (cur == 0)
        return;
    if (pl->pcms > 1)
        v = cur << 1;
    else if (pl->pcms < 1)
        v = cur >> 1;
    else
        v = pl->pcms;                   /* BUG: == 1, the CA is not added */
    pl->cur_addr = v + start;
    pl->cur_pos = cur;
}

/*
 * 0x73C8 stream_monitor
 * Confidence: high
 * PCM-stream channel that is keyed on: after the settle loop, stores the LP flag
 * (MON_EG >> 15) in loop_mode, counts a ring wrap (loop_count++) when set, then
 * updates cur_pos/cur_addr (pcm_store_pos). Streams never stop by themselves.
 */
void stream_monitor(PcmPlayer *pl)
{
    u32 start = 0, lp;
    int i;

    for (i = 0; i < 30; i++)
        start = *(volatile u32 *)&pl->start;
    lp = AICA_MON_EG >> 15;
    pl->loop_mode = (u8)lp;
    if (lp)
        pl->loop_count++;
    pcm_store_pos(pl, start);
}

/*
 * 0x711C pcm_tick
 * Confidence: high
 * Every 4 ms: for the 8 one-shot players pcm_update then, if still keyed on,
 * shot_monitor; for the 16 stream channels pcm_update then, if keyed on,
 * stream_monitor. The flags returned by pcm_update/shot_monitor are stored back.
 */
void pcm_tick(void)
{
    PcmPlayer *pl = g_pcm_player;
    u32 f;
    int i;

    for (i = 0; i < NUM_SHOT_PORTS; i++, pl++) {
        f = pcm_update(pl);
        if (f & PORT_FLG_PLAY)
            f = shot_monitor(pl, f);
        pl->flags = (u8)f;
    }
    for (i = 0; i < NUM_PSTM_CH; i++, pl++) {
        f = pcm_update(pl);
        pl->flags = (u8)f;
        if (f & PORT_FLG_PLAY)
            stream_monitor(pl);
    }
}

/*
 * 0x7778 pcm_init
 * Confidence: high
 * Boot: clears the 24 PCM players and sets vol_target, pan_target, fx_lvl,
 * direct_lvl (one word at +0x18), volume, pan, speed and pitch (0x40/0x44/0x4C/0x50) to neutral 0x80/0x8000
 * (byte 1 of each u32 = 0x80) and fx_ch = 0x80. Stream channels additionally get
 * pan_base = 0x1F (even = left) / 0x0F (odd = right) and disdl_base = 0x0F. Then
 * clears the 16 AICA slots and points slot n at AICA channel 48+n.
 * Notes: QUIRK: each clear loop writes 0x5C bytes (offsets 0x58..0 inclusive),
 * i.e. also the first word of the next record; for the last record that is the
 * word at 0x11840, just past the array.
 */
void pcm_init(void)
{
    PcmPlayer *pl = g_pcm_player;
    u32 pan = 0x1F;
    int i, j;

    for (i = 0; i < NUM_SHOT_PORTS + NUM_PSTM_CH; i++, pl++) {
        for (j = 0x58; j >= 0; j -= 4)
            *(u32 *)((u8 *)pl + j) = 0;
        if (i >= NUM_SHOT_PORTS) {
            pl->pan_base = (u8)pan;
            pl->disdl_base = 0x0F;
            pan ^= 0x10;
        }
        *(u32 *)&pl->vol_target = 0x80808080;  /* vol/pan targets, fx_lvl, direct_lvl */
        pl->pan = 0x80;
        pl->volume = 0x80;
        pl->fx_ch = 0x80;
        ((u8 *)&pl->speed_target)[1] = 0x80;
        ((u8 *)&pl->speed)[1] = 0x80;
        ((u8 *)&pl->pitch_target)[1] = 0x80;
        ((u8 *)&pl->pitch)[1] = 0x80;
    }
    for (i = 0; i < NUM_PCM_SLOTS; i++) {
        g_pcm_slot[i].prio = 0;
        g_pcm_slot[i].kind = 0;
        g_pcm_slot[i]._02[0] = 0;
        g_pcm_slot[i]._02[1] = 0;
        g_pcm_slot[i].owner = 0;
        g_pcm_slot[i].ch = &AICA_CH[48 + i];
    }
}
