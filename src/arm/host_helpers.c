/*
 * host_helpers.c - one-shot ("Shot", host commands 0x1x) and PCM-stream ("Pstm", host 0x2x)
 * port helpers called from host_cmd_exec (0x4998). Code ranges 0x6DFC-0x7118 and 0x7884-0x82CC.
 *
 * Data: PcmPlayer g_pcm_player[24] at 0x11000 ([0..7] one-shot ports, [8 + port*2 + ch] PCM
 * stream channels) and PcmSlot g_pcm_slot[16] at 0x12080 (owner of AICA channel 48+n).
 * Formats: docs/notes/oneshot_pstm_format.md.
 *
 * Register conventions of the asm (for the Ghidra side):
 *   r12 = PcmPlayer *, r10 = PcmSlot *, r11 = AICA channel registers, r0 = port (masked with 7).
 * Many routines leave through shared tails that pop the lr pushed by the *caller's* entry:
 *   0x81C4  err exit:    pl->err |= r4; return from the entry routine
 *   0x81D8  key-on exit: ch->ctl |= KYONEX|KYONB; return
 *   0x81EC  key-off exit (pcm_key_exec_off): ch->ctl = (ch->ctl & ~KYONB) | KYONEX; return
 * These are modelled as ordinary returns.
 */
#include "manatee.h"

/* byte access to a channel register, as the asm's strb/ldrb [r11,#off] */
#define CH_B(ch, off) (*(volatile u8 *)((u32)(ch) + (off)))
#define CH_DSP_SEND 0x20   /* IMXL7:4 ISEL3:0 */
#define CH_DIPAN    0x24   /* DIPAN4:0 */
#define CH_DISDL    0x25   /* DISDL (bits 11:8 of reg 0x24) */
#define CH_Q        0x28   /* LPOFF5 Q4:0 */
#define CH_TL       0x29   /* TL (bits 15:8 of reg 0x28) */

/* low byte of r4 in host_cmd_exec (r4 = &cmd_log_ofs = 0x1340C), see pcm_fx_ch_byte */
#define FX_CH_STALE_R4 0x0C

static PcmPlayer *shot_pl(u32 port) { return &g_shot_player[port & 7]; }
static PcmPlayer *pstm_pl(u32 port) { return &g_pstm_player[(port & 7) * 2]; }

/* u32 view of the first word of a slot (prio | kind<<8 | pad<<16), compared by pcm_slot_alloc */
static u32 slot_word(const PcmSlot *s) { return *(const u32 *)(const void *)s; }

/*
 * 0x8240 pcm_player_slot (also 0x70F4, same code with other scratch registers)
 * Confidence: high
 * r12 player -> r10 = &g_pcm_slot[pl->slot], r11 = that slot's AICA channel.
 * Notes: 0x70F4 clobbers r7/r9, 0x8240 clobbers r4/r9 (r9 = slot index).
 */
PcmSlot *pcm_player_slot(const PcmPlayer *pl)
{
    return &g_pcm_slot[pl->slot];
}

/*
 * 0x6EEC pcm_fade_calc
 * Confidence: high
 * Converts a fade request into (step, interval) for the 4 ms fade tick (pcm_update):
 * every `interval` ticks the value moves `step` towards the target.
 * In: r1 target, r7 current, r2 fade time in ms. Out: r3 step, r2 interval (ticks).
 * Notes: interval = fade/4, diff = |cur - target| & 0xFF; both are halved together until
 * diff <= 1 (then interval is clamped to 0xFF) or interval <= 1. fade == 0 gives 0/0.
 */
static u32 pcm_fade_calc(u32 target, u32 cur, u32 *interval)
{
    u32 ticks = *interval;
    u32 diff;

    if (ticks == 0)
        return 0;
    ticks >>= 2;
    diff = cur - target;                   /* rsbs */
    if ((s32)diff < 0)
        diff = 0 - diff;                   /* eormi -1 / addmi 1 */
    diff &= 0xFF;
    for (;;) {
        if ((diff & 0xFE) == 0) {          /* diff <= 1: done */
            if (ticks & 0xFFFFFF00u)
                ticks = 0xFF;
            break;
        }
        if ((ticks & 0xFFFFFFFEu) == 0)    /* interval <= 1: take big steps */
            break;
        diff >>= 1;
        ticks >>= 1;
    }
    *interval = ticks;
    return diff;
}

/* 0x6E30 body of shot_set_vol / pstm_set_vol for one player */
static void pcm_vol_set(PcmPlayer *pl, u32 vol, u32 fade)
{
    u32 interval = fade;
    u32 step;

    pl->flags |= PORT_FLG_CHG_VOL;
    step = pcm_fade_calc(vol, pl->volume, &interval);
    pl->vol_target = (u8)vol;
    pl->vol_interval = (u8)interval;
    pl->vol_count = (u8)interval;
    pl->vol_step = (u8)step;
    if (interval == 0)
        pl->volume = (u8)vol;              /* immediate */
}

/* 0x6EB8 body of shot_set_pan / pstm_set_pan for one player */
static void pcm_pan_set(PcmPlayer *pl, u32 pan, u32 fade)
{
    u32 interval = fade;
    u32 step;

    pl->flags |= PORT_FLG_CHG_PAN;
    step = pcm_fade_calc(pan, pl->pan, &interval);
    pl->pan_target = (u8)pan;
    pl->pan_interval = (u8)interval;
    pl->pan_count = (u8)interval;
    pl->pan_step = (u8)step;
    if (interval == 0)
        pl->pan = (u8)pan;
}

/*
 * 0x6DFC shot_set_vol (host 0x15 sdShotSetVol)
 * Confidence: high
 * r0 port, r1 volume (+0x80 biased), r2 fade time (ms). Starts a volume fade of the one-shot
 * player; the 4 ms tick (pcm_update) moves PcmPlayer.volume and rewrites TL.
 * Notes: works whether or not the port is playing. Shares its body (0x6E30) with pstm_set_vol;
 * the helper 0x6E64 (pcm_next_ch) returns from the whole routine because r0 has no channel bits.
 */
void shot_set_vol(u32 port, u32 vol, u32 fade)
{
    pcm_vol_set(shot_pl(port), vol, fade);
}

/*
 * 0x6E14 pstm_set_vol (host 0x25 sdPstmSetVol)
 * Confidence: high
 * r0 = cmd[2] = port | 0x20 (ch0) | 0x10 (ch1), r1/r2 ch0 volume/fade, r4/r5 ch1 volume/fade.
 * Notes: 0x6E64 pcm_next_ch: if bit4 is set it clears it, advances r12 to the ch1 player and
 * moves r4/r5 to r1/r2; otherwise it pops the entry lr and returns from pstm_set_vol.
 */
void pstm_set_vol(u32 pmask, u32 vol0, u32 fade0, u32 vol1, u32 fade1)
{
    PcmPlayer *pl = pstm_pl(pmask);

    if (pmask & 0x20)
        pcm_vol_set(&pl[0], vol0, fade0);
    if (pmask & 0x10)
        pcm_vol_set(&pl[1], vol1, fade1);
}

/*
 * 0x6E84 shot_set_pan (host 0x16 sdShotSetPan)
 * Confidence: high
 * r0 port, r1 pan (+0x80 biased), r2 fade (ms). Pan counterpart of shot_set_vol (body 0x6EB8).
 */
void shot_set_pan(u32 port, u32 pan, u32 fade)
{
    pcm_pan_set(shot_pl(port), pan, fade);
}

/*
 * 0x6E9C pstm_set_pan (host 0x26 sdPstmSetPan)
 * Confidence: high
 * As pstm_set_vol for pan: r0 = port | 0x20 (ch0) | 0x10 (ch1), r1/r2 ch0, r4/r5 ch1.
 */
void pstm_set_pan(u32 pmask, u32 pan0, u32 fade0, u32 pan1, u32 fade1)
{
    PcmPlayer *pl = pstm_pl(pmask);

    if (pmask & 0x20)
        pcm_pan_set(&pl[0], pan0, fade0);
    if (pmask & 0x10)
        pcm_pan_set(&pl[1], pan1, fade1);
}

/*
 * 0x70D4 pcm_level_mix
 * Confidence: high
 * r7 = host level (+0x80 biased), r6 = base level 0..15 -> r7 = clamp((base*8 + lvl-0x80) >> 3, 0, 15).
 * Used for IMXL (fx send) and DISDL (direct level).
 */
u32 pcm_level_mix(u32 ofs, u32 base)
{
    s32 v = ((s32)(base << 3) + ((s32)ofs - 0x80)) >> 3;   /* asr */

    if (v < 0)
        v = 0;
    if (v > 15)
        v = 15;
    return (u32)v;
}

/* 0x7038-0x7050: IMXL = pcm_level_mix(fx_lvl, base>>4), keeping ISEL */
static void pcm_write_imxl(volatile AicaChannel *ch, u32 fx_lvl, u32 base)
{
    u32 lvl = pcm_level_mix(fx_lvl, base >> 4);

    CH_B(ch, CH_DSP_SEND) = (u8)((CH_B(ch, CH_DSP_SEND) & 0x0F) | (lvl << 4));
}

/* 0x6F88 body of the FxCh commands, one player (base = level & 0xF0, fx = the fx_ch byte computed
 * once at 0x6F84); returns r1, which the pass may replace (see pstm_set_fx_ch). */
static u32 pcm_fx_ch_set(PcmPlayer *pl, u32 in_ch, u32 base, u8 fx)
{
    u32 imxl;

    pl->fx_base = (u8)base;
    pl->fx_ch = fx;
    if (pl->flags & PORT_FLG_PLAY) {
        volatile AicaChannel *ch;
        u32 lvl;

        if (in_ch != 0xFF) {
            imxl = base;
        } else {
            in_ch = pl->isel;    /* QUIRK: r1 stays replaced for the stream's 2nd channel */
            imxl = pl->imxl;
        }
        ch = pcm_player_slot(pl)->ch;      /* 0x70F4 */
        lvl = pcm_level_mix(pl->fx_lvl, imxl >> 4);
        CH_B(ch, CH_DSP_SEND) = (u8)(in_ch + (lvl << 4));
    }
    return in_ch;
}

/* 0x6F78-0x6F84: fx_ch byte stored by every pass. in_ch == 0xFF stores whatever r4 holds:
 * host_cmd_exec left &cmd_log_ofs (0x1340C) there, so the byte is 0x0C (bit6 clear = "use the
 * sample's ISEL/IMXL"). The addne runs once, before the pass loop (0x6F88). */
static u8 pcm_fx_ch_byte(u32 in_ch)
{
    return (u8)(in_ch != 0xFF ? in_ch + 0x40 : FX_CH_STALE_R4);
}

/*
 * 0x6F4C shot_set_fx_ch (host 0x19 sdShotSetFxCh)
 * Confidence: high
 * r0 port, r1 DSP input channel (0xFF = back to the sample's), r2 base level (+0x80 biased;
 * only bits 7:4 are kept). Stores fx_ch = in_ch+0x40 / fx_base and, if the port is playing,
 * rewrites the channel's dsp_send byte = in_ch + (pcm_level_mix(fx_lvl, base>>4) << 4).
 * Notes: see pcm_fx_ch_byte for the stale-r4 byte stored when in_ch == 0xFF. in_ch is not masked:
 * values > 0x0F carry into IMXL (and fx_ch bit6 may end up clear, e.g. in_ch 0x40 -> 0x80).
 */
void shot_set_fx_ch(u32 port, u32 in_ch, u32 base_lvl)
{
    pcm_fx_ch_set(shot_pl(port), in_ch, base_lvl & ~0x0Fu, pcm_fx_ch_byte(in_ch));
}

/*
 * 0x6F64 pstm_set_fx_ch (host 0x29 sdPstmSetFxCh)
 * Confidence: high
 * As shot_set_fx_ch for both channels of a stream port (r8 = 2 passes, no channel mask).
 * Notes: both channels get the same fx_ch byte (r4 is computed once). QUIRK: with in_ch == 0xFF
 * and ch0 playing, r1 is replaced by ch0's ISEL inside the loop; if ch1 is playing too, its
 * dsp_send is then written as ch0.isel + (pcm_level_mix(fx_lvl, fx_base>>4) << 4), i.e. with the
 * FxCh base level instead of its own ISEL/IMXL, although its stored fx_ch (0x0C) says "no
 * override". The next pcm_apply_params on ch1 restores its own ISEL/IMXL (FxLev only rewrites IMXL).
 */
void pstm_set_fx_ch(u32 port, u32 in_ch, u32 base_lvl)
{
    PcmPlayer *pl = pstm_pl(port);
    u32 base = base_lvl & ~0x0Fu;
    u8 fx = pcm_fx_ch_byte(in_ch);

    in_ch = pcm_fx_ch_set(&pl[0], in_ch, base, fx);
    pcm_fx_ch_set(&pl[1], in_ch, base, fx);
}

/* 0x7010 body of the FxLev commands */
static void pcm_fx_lvl_set(PcmPlayer *pl, u32 lvl)
{
    pl->fx_lvl = (u8)lvl;
    if (pl->flags & PORT_FLG_PLAY) {
        u32 base = (pl->fx_ch & 0x40) ? pl->fx_base : pl->imxl;
        pcm_write_imxl(pcm_player_slot(pl)->ch, lvl, base);
    }
}

/*
 * 0x6FE0 shot_set_fx_lvl (host 0x1A sdShotSetFxLev)
 * Confidence: high
 * r0 port, r1 level (+0x80 biased). fx_lvl = r1; if playing, IMXL = pcm_level_mix(r1, base)
 * with base = fx_base if fx_ch bit6 (FxCh override) else the sample's IMXL.
 * Notes: 0x7038 (inside this body) is also the tail of pcm_apply_params.
 */
void shot_set_fx_lvl(u32 port, u32 lvl)
{
    pcm_fx_lvl_set(shot_pl(port), lvl);
}

/*
 * 0x6FF8 pstm_set_fx_lvl (host 0x2A sdPstmSetFxLev)
 * Confidence: high
 * As shot_set_fx_lvl for both channels of the stream port.
 */
void pstm_set_fx_lvl(u32 port, u32 lvl)
{
    PcmPlayer *pl = pstm_pl(port);

    pcm_fx_lvl_set(&pl[0], lvl);
    pcm_fx_lvl_set(&pl[1], lvl);
}

/* 0x7098 body of the DrctLev commands */
static void pcm_direct_lvl_set(PcmPlayer *pl, u32 lvl)
{
    pl->direct_lvl = (u8)lvl;
    if (pl->flags & PORT_FLG_PLAY) {
        volatile AicaChannel *ch = pcm_player_slot(pl)->ch;
        u32 v = pcm_level_mix(lvl, pl->disdl_base);

        pl->disdl = (u8)v;
        CH_B(ch, CH_DISDL) = (u8)v;
    }
}

/*
 * 0x7068 shot_set_direct_lvl (host 0x1B sdShotSetDrctLev)
 * Confidence: high
 * r0 port, r1 level (+0x80 biased). direct_lvl = r1; if playing, DISDL = pcm_level_mix(r1, disdl_base).
 */
void shot_set_direct_lvl(u32 port, u32 lvl)
{
    pcm_direct_lvl_set(shot_pl(port), lvl);
}

/*
 * 0x7080 pstm_set_direct_lvl (host 0x2B sdPstmSetDrctLev)
 * Confidence: high
 * As shot_set_direct_lvl for both channels of the stream port.
 */
void pstm_set_direct_lvl(u32 port, u32 lvl)
{
    PcmPlayer *pl = pstm_pl(port);

    pcm_direct_lvl_set(&pl[0], lvl);
    pcm_direct_lvl_set(&pl[1], lvl);
}

/* ------------------------------------------------------------------------- */
/* Channel allocation / release                                              */
/* ------------------------------------------------------------------------- */

/*
 * 0x81EC pcm_key_exec_off
 * Confidence: high
 * ch->ctl = (ch->ctl & ~KYONB) | KYONEX, then pops the entry lr (tail of the stop routines).
 * Notes: KYONEX executes the key state of *all* channels, so this also keys on any channel whose
 * KYONB was set without KYONEX (pstm_play relies on that in its mono path).
 */
void pcm_key_exec_off(volatile AicaChannel *ch)
{
    ch->ctl_sa_hi = (ch->ctl_sa_hi & ~AICA_KYONB) | AICA_KYONEX;
}

/* 0x81D8 key-on exit */
static void pcm_key_exec_on(volatile AicaChannel *ch)
{
    ch->ctl_sa_hi |= AICA_KYONEX | AICA_KYONB;
}

/*
 * 0x8210 pcm_slot_free
 * Confidence: high
 * r12 player, r10 slot, r6 = 0: frees the slot (owner and prio/kind word = 0) and zeroes the
 * player's prio, slot, pitch_base, tl/dipan, vol/pan fade counters and the words at +0x24
 * (tl_base, isel, imxl, pcms) and +0x28 (pan_base, fx_ch, disdl_base, disdl).
 * Notes: does NOT touch flags; callers clear PORT_FLG_PLAY and restore fx_ch themselves.
 */
void pcm_slot_free(PcmPlayer *pl, PcmSlot *s)
{
    s->owner = 0;
    s->prio = 0;
    s->kind = 0;
    s->_02[0] = s->_02[1] = 0;
    pl->pan_base = 0;
    pl->fx_ch = 0;
    pl->disdl_base = 0;
    pl->disdl = 0;
    pl->slot = 0;
    pl->prio = 0;
    pl->pitch_base = 0;
    pl->tl = 0;
    pl->dipan = 0;
    pl->pan_count = 0;
    pl->vol_count = 0;
    pl->tl_base = 0;
    pl->isel = 0;
    pl->imxl = 0;
    pl->pcms = 0;
}

/*
 * 0x8204 pcm_release
 * Confidence: high
 * loops_default = loops_left = start = 0, then falls into pcm_slot_free.
 * Notes: also used by shot_monitor (0x7350, port.c) at the end of a one-shot.
 */
void pcm_release(PcmPlayer *pl, PcmSlot *s)
{
    pl->loops_default = 0;
    pl->loops_left = 0;
    pl->start = 0;
    pcm_slot_free(pl, s);
}

/*
 * 0x7F50 shot_release
 * Confidence: high
 * r12 player, r10 slot, r11 channel, r6 = 0: pcm_release keeping fx_ch, clear PORT_FLG_PLAY,
 * clear KYONB of the channel (no KYONEX: the caller executes the key-off).
 */
void shot_release(PcmPlayer *pl, PcmSlot *s, volatile AicaChannel *ch)
{
    u8 keep_fx = pl->fx_ch;
    u8 flags = pl->flags;

    pcm_release(pl, s);
    pl->flags = flags & ~PORT_FLG_PLAY;
    pl->fx_ch = keep_fx;
    ch->ctl_sa_hi &= ~AICA_KYONB;
}

/*
 * 0x7EA4 pstm_release
 * Confidence: high
 * r12 = channel-0 player of a stream, r10/r11 its slot/channel. For ch0 and then (if it is
 * playing) ch1: pcm_slot_free keeping fx_ch, clear KYONB, loop_mode = start = ring_len = 0,
 * pan_base = 0x1F (ch0) / 0x0F (ch1), disdl_base = 0x0F, clear PORT_FLG_PLAY.
 * Returns the last channel handled (r11) for the caller's key-off. Side effect of the asm: r12
 * is always left at the player after the first one (pl + 0x58), which pcm_slot_alloc's failure
 * path relies on (see there).
 * Notes: QUIRK: always assumes r12 is ch0. pcm_slot_alloc also calls it for a stolen stream
 * *ch1* player: then ch1 gets pan_base 0x1F and the following player (next port's ch0, or past
 * the array for port 7) is released too if it is playing. No KYONEX here.
 */
volatile AicaChannel *pstm_release(PcmPlayer *pl, PcmSlot *s, volatile AicaChannel *ch)
{
    u32 pan = 0x1F;

    for (;;) {
        u8 keep_fx = pl->fx_ch;

        pcm_slot_free(pl, s);
        pl->fx_ch = keep_fx;
        ch->ctl_sa_hi &= ~AICA_KYONB;
        pl->loop_mode = 0;
        pl->start = 0;
        pl->ring_len = 0;
        pl->pan_base = (u8)pan;
        pl->disdl_base = 0x0F;
        pl->flags &= ~PORT_FLG_PLAY;
        if (pan == 0x0F)
            return ch;
        pan = 0x0F;
        pl++;
        if (!(pl->flags & PORT_FLG_PLAY))
            return ch;
        s = pcm_player_slot(pl);
        ch = s->ch;
    }
}

/*
 * 0x7B68 pcm_slot_alloc
 * Confidence: high
 * r12 player, r5 priority (0..31), r0 kind (1 shot, 2 stream ch0, 3 stream ch1).
 * Returns r10 slot (r11 = its channel, r9 index); on failure ORs an error into the player's err and
 * leaves through 0x81C4 (returning from the caller) - modelled as a 0 return.
 *  - player already playing: if pl->prio != 0, prio != 0 and prio < pl->prio -> PCM_ERR_PRIORITY;
 *    otherwise it re-uses (steals) its own slot.
 *  - else the first slot with owner == 0.
 *  - else the victim: scanning slots 15..0, a slot with prio 0 is taken at once; otherwise the
 *    lowest prio, ties -> lowest (prio | kind<<8) word, then lowest index. prio 0 always steals;
 *    prio > victim steals; prio < victim fails; equal prio compares (prio + kind) with the
 *    victim word (prio | kind<<8), i.e. always fails (QUIRK: probably meant kind<<8).
 *  - failure: PCM_ERR_NO_CHANNEL; for kind 3 the stream's ch0 (already allocated by pstm_play)
 *    is released first (pstm_release, KYONB cleared, no KYONEX). pstm_release returns with
 *    r12 = ch0 + 0x58, so the error lands on ch1 again (not ch0).
 *  - steal: the owner is released (shot_release for kind 1, pstm_release otherwise) and its
 *    channel keyed off (KYONEX).
 *  - claim: pl->prio/slot, slot prio/kind/owner, PORT_FLG_PLAY, AICA MSLC = 48+slot.
 * Notes: QUIRK: when the victim comes from the lowest-priority scan (not the prio-0 early exit),
 * r9 is -1 after the loop, so pl->slot becomes 0xFF (slot entry and channel are still right, but
 * later look-ups via pl->slot index g_pcm_slot[255] at 0x12C74, past the array). MSLC also gets
 * 0x2F, i.e. MIDI voice channel 47 is selected for the dummy MON_EG/MON_CA reads.
 */
PcmSlot *pcm_slot_alloc(PcmPlayer *pl, u32 prio, u32 kind)
{
    PcmSlot *s;
    s32 idx;

    if (pl->flags & PORT_FLG_PLAY) {
        u32 cur = pl->prio;
        if (cur != 0 && prio != 0 && prio < cur) {
            pl->err |= PCM_ERR_PRIORITY;
            return 0;
        }
        idx = pl->slot;                    /* 0x7C54 */
        s = &g_pcm_slot[idx];
        goto steal;
    }
    for (idx = 0; idx < NUM_PCM_SLOTS; idx++) {
        s = &g_pcm_slot[idx];
        if (s->owner == 0)
            goto claim;
    }
    {
        u32 min = 0xFF;
        PcmSlot *victim = &g_pcm_slot[NUM_PCM_SLOTS];
        for (idx = NUM_PCM_SLOTS - 1; idx >= 0; idx--) {
            s = &g_pcm_slot[idx];
            if (s->prio == 0)
                goto steal;                /* idx valid here */
            if (s->prio < min) {
                min = s->prio;
                victim = s;
            } else if (s->prio == min && slot_word(s) <= slot_word(victim)) {
                victim = s;
            }
        }
        /* idx == -1 here */
        if (prio == 0 || prio > min ||
            (prio == min && prio + kind >= slot_word(victim))) {
            s = victim;
            goto steal;                    /* QUIRK: claims with idx = -1 */
        }
        if (kind == PCM_KIND_PSTM_R) {
            PcmPlayer *ch0 = pl - 1;
            PcmSlot *s0 = pcm_player_slot(ch0);
            pstm_release(ch0, s0, s0->ch); /* leaves r12 = ch0 + 0x58 = pl */
        }
        pl->err |= PCM_ERR_NO_CHANNEL;
        return 0;
    }

steal: {
        PcmPlayer *old = s->owner;
        volatile AicaChannel *och = s->ch;
        if (s->kind == PCM_KIND_SHOT)
            shot_release(old, s, och);     /* 0x7F50 */
        else
            och = pstm_release(old, s, och); /* 0x7EA4 */
        och->ctl_sa_hi = (och->ctl_sa_hi & ~AICA_KYONB) | AICA_KYONEX;
    }
claim:
    pl->prio = (u8)prio;
    pl->slot = (u8)idx;
    s->prio = (u8)prio;
    s->kind = (u8)kind;
    s->owner = pl;
    pl->flags |= PORT_FLG_PLAY;
    AICA_MSLC_B = (u8)(idx + 0x30);        /* monitor this channel (MON_EG/MON_CA) */
    return s;
}

/* ------------------------------------------------------------------------- */
/* Parameter application                                                     */
/* ------------------------------------------------------------------------- */

/*
 * 0x7DEC pcm_apply_pitch
 * Confidence: high
 * r4 = AICA OCT/FNS value, r12 player, r11 channel: converts to a linear pitch
 * (oct+8)<<10 | FNS, adds pl->pitch_offset (speed + pitch, from pcm_pitch_update), converts back and
 * writes the channel PITCH register.
 * Notes: FNS is taken as 10 bits (mask 0x3FF). The octave is extracted with lsr, so a negative
 * sum becomes a huge value and clamps to 15 = OCT 7 (QUIRK: the `movlt 0` is dead code).
 * Also the tail of pcm_update (0x730C, called every tick for playing PCM players).
 */
void pcm_apply_pitch(PcmPlayer *pl, volatile AicaChannel *ch, u32 pitch_base)
{
    u32 lin = (pitch_base & 0x3FF) | ((u32)oct_flip_tab[pitch_base >> 11] << 10);
    u32 fns, oct;

    lin += (u32)pl->pitch_ofs;
    fns = lin & 0x3FF;
    oct = lin >> 10;
    if (oct > 15)
        oct = 15;
    ch->pitch = fns | ((u32)oct_flip_tab[oct] << 11);
}

/*
 * 0x7D14 pcm_apply_params
 * Confidence: high
 * r12 player, r11 channel, r3 tl (sample TL / stream base volume), r2 pan_base (AICA DIPAN code),
 * r4 pitch base. Writes the current mix state of the player to its channel:
 *   loop_count = 0
 *   TL    = clamp(tl + 0x100 - 2*volume, 0, 255)                     -> reg byte 0x29, pl->tl
 *   DIPAN = pan_lin_to_aica[clamp(pan_aica_to_lin[pan_base] + pan/4 - 0x20, 0, 31)] -> 0x24, pl->dipan
 *   pitch (pcm_apply_pitch), DISDL = pcm_level_mix(direct_lvl, disdl_base) -> 0x25, pl->disdl
 *   ISEL  = fx_ch (if bit6) else isel -> byte 0x20; IMXL = pcm_level_mix(fx_lvl, fx_base or imxl)
 * Clears PORT_FLG_CHG_VOL / CHG_PAN when no fade is running (interval 0).
 * Notes: ends by branching into the FxLev body at 0x7038 with r8 = 1 (returns via its pop).
 * With the fx_ch override the whole fx_ch byte (incl. bit6) is written to the dsp_send byte
 * first; the IMXL write that follows replaces bits 7:4 again.
 */
void pcm_apply_params(PcmPlayer *pl, volatile AicaChannel *ch, u32 tl, u32 pan_base, u32 pitch)
{
    s32 v;
    u32 base;

    pl->loop_count = 0;
    v = (s32)tl + (0x100 - 2 * (s32)pl->volume);
    if (v < 0)
        v = 0;
    if (v > 0xFF)
        v = 0xFF;
    CH_B(ch, CH_TL) = (u8)v;
    pl->tl = (u8)v;
    if (pl->vol_interval == 0)
        pl->flags &= ~PORT_FLG_CHG_VOL;

    v = (s32)pan_aica_to_lin_b[pan_base] + ((s32)(pl->pan >> 2) - 0x20);
    if (v < 0)
        v = 0;
    if (v > 0x1F)
        v = 0x1F;
    CH_B(ch, CH_DIPAN) = pan_lin_to_aica_b[v];
    pl->dipan = pan_lin_to_aica_b[v];
    if (pl->pan_interval == 0)
        pl->flags &= ~PORT_FLG_CHG_PAN;

    pcm_apply_pitch(pl, ch, pitch);

    v = (s32)pcm_level_mix(pl->direct_lvl, pl->disdl_base);
    pl->disdl = (u8)v;
    CH_B(ch, CH_DISDL) = (u8)v;

    if (pl->fx_ch & 0x40) {
        CH_B(ch, CH_DSP_SEND) = pl->fx_ch;
        base = pl->fx_base;
    } else {
        CH_B(ch, CH_DSP_SEND) = pl->isel;
        base = pl->imxl;
    }
    pcm_write_imxl(ch, pl->fx_lvl, base);
}

/* ------------------------------------------------------------------------- */
/* One-shot ports                                                            */
/* ------------------------------------------------------------------------- */

/*
 * 0x828C shot_loops_apply
 * Confidence: high
 * r12 player, r1 mode, r2 param: loops_left from the SOSP loop count and the host 0x1C setting,
 * via the jump table at 0x8298: 0 default, 1 default + param, 2 (default * param) >> 8, 3 param.
 * Notes: mode > 3 would jump through the code after the table (cannot happen: 0x8258 limits it).
 */
void shot_loops_apply(PcmPlayer *pl, u32 mode, u32 param)
{
    u32 def = *(const u8 *)&pl->loops_default;   /* ldrb */
    u32 n;

    switch (mode) {
    case 0:  n = def; break;
    case 1:  n = def + param; break;
    case 2:  n = (def * param) >> 8; break;
    default: n = param; break;
    }
    pl->loops_left = n;
}

/*
 * 0x8258 shot_set_loop_mode (host 0x1C; no sd 1.00.18 API sends it)
 * Confidence: high
 * r0 port, r1 mode 0..2 (cmd[3]), r2 param (cmd[4..7] u32). Stores loop_mode = mode+1 and
 * ring_len (= loop param for one-shots) = param; if the player's flags byte is non-zero the new
 * loops_left is applied at once (shot_loops_apply), otherwise at the next shot_play.
 * mode > 2 -> PCM_ERR_PARAM (0x20).
 * Notes: no lr push; the error exit 0x81C4 pops the lr the host handler pushed, i.e. returns
 * straight to host_cmd_exec's caller path (same effect). Tests the whole flags byte (paused but
 * stopped players also get the update). Mode is reset to 0 only by shot_reset_prm.
 */
void shot_set_loop_mode(u32 port, u32 mode, u32 param)
{
    PcmPlayer *pl = shot_pl(port);

    if (mode > 2) {
        pl->err |= PCM_ERR_PARAM;
        return;
    }
    mode++;
    pl->ring_len = param;
    pl->loop_mode = (u8)mode;
    if (pl->flags == 0)
        return;
    shot_loops_apply(pl, mode, param);
}

/*
 * 0x7884 shot_play (host 0x11 sdShotPlay)
 * Confidence: high
 * r0 port, r1 data number, r2 priority<<3, r3 SOSB bank header (already checked by the host
 * handler: magic, version byte 1, "ENDB" at size-4).
 *  num > count -> PCM_ERR_REQUEST_NUM (QUIRK: num == count accepted and reads offset[count],
 *  which is the first entry's "SOSP" tag). Allocates an AICA channel (kind 1), copies the SOSP
 *  register image to the channel (SA relocated by the bank address), latches the per-sample
 *  bases (tl_base, pan_base, isel/imxl, disdl_base, pcms, pitch_base = PITCH read back),
 *  applies the port mix (pcm_apply_params), sets loops_left (shot_loops_apply with the
 *  port's loop_mode), clears PAUSE and LFORE, dummy-reads MON_EG/MON_CA and keys on.
 * Notes: QUIRKs: writes SA_LO = 0x8000 right after the allocation (overwritten below);
 * pcms is taken from bits 24:23 of the u32 at SOSP+4, i.e. SA_LO bits 8:7, not PCMS
 * (bits 8:7 of ctl) - the value only feeds cur_addr (pcm_update), which the SH-4 never reads.
 */
void shot_play(u32 port, u32 num, u32 prio, const SosbHeader *bank)
{
    PcmPlayer *pl = shot_pl(port);
    PcmSlot *s;
    volatile AicaChannel *ch;
    const SospEntry *e;
    const u16 *src;
    volatile u32 *dst;
    u32 w;
    int i;

    if (num > bank->count) {
        pl->err |= PCM_ERR_REQUEST_NUM;
        return;
    }
    s = pcm_slot_alloc(pl, prio >> 3, PCM_KIND_SHOT);
    if (!s)
        return;
    ch = s->ch;
    ch->sa_lo = 0x8000;                    /* QUIRK, overwritten below */

    e = (const SospEntry *)((u32)bank + bank->offset[num]);
    w = (((u32)e->ctl << 16) | e->sa_lo) + (u32)bank;   /* ldr + ror #16 + bank */
    pl->cur_addr = w & 0x7FFFFF;
    pl->start = w & 0x7FFFFF;
    pl->cur_pos = 0;
    pl->loop_count = 0;

    ch->ctl_sa_hi = w >> 16;               /* flags + SA[22:16] (no KYONB) */
    ch->sa_lo = w & 0xFFFF;
    src = &e->lsa;                         /* 16 halfwords -> regs 0x08..0x44 */
    dst = &ch->lsa;
    for (i = 0; i < 16; i++)
        dst[i] = src[i];

    pl->disdl_base = e->disdl;
    pl->isel = e->dsp_send & 0x0F;
    pl->imxl = e->dsp_send & 0xF0;
    pl->pcms = (u8)((*(const u32 *)(const void *)&e->ctl & 0x1800000u) >> 23); /* QUIRK */
    pl->pan_base = e->dipan;
    pl->tl_base = e->tl;
    pl->pitch_base = ch->pitch;            /* read back from the AICA */
    pcm_apply_params(pl, ch, e->tl, e->dipan, pl->pitch_base);

    *(u8 *)&pl->loops_default = e->loops; /* strb: bytes 1-3 stay 0 (only ever zeroed by word) */
    shot_loops_apply(pl, pl->loop_mode, pl->ring_len);
    pl->flags &= ~PORT_FLG_PAUSE;
    ch->lfo &= ~0x8000u;                   /* LFORE off */
    (void)AICA_MON_EG;
    (void)AICA_MON_CA;
    pcm_key_exec_on(ch);
}

/*
 * 0x7E48 shot_stop (host 0x12 sdShotStop)
 * Confidence: high
 * r0 port: if playing, shot_release + KYONEX key-off.
 */
void shot_stop(u32 port)
{
    PcmPlayer *pl = shot_pl(port);
    PcmSlot *s;

    if (!(pl->flags & PORT_FLG_PLAY))
        return;
    s = pcm_player_slot(pl);
    shot_release(pl, s, s->ch);
    pcm_key_exec_off(s->ch);
}

/*
 * 0x7F18 shot_stop_all (host 0x1F sdShotStopAll, host 0x80 with cmd[3] != 0)
 * Confidence: high
 * Releases every slot owned by a one-shot (kind 1), then one KYONEX key-off (through slot 15's
 * channel register).
 */
void shot_stop_all(void)
{
    volatile AicaChannel *ch = 0;
    int i;

    for (i = 0; i < NUM_PCM_SLOTS; i++) {
        PcmSlot *s = &g_pcm_slot[i];
        ch = s->ch;
        if (s->kind == PCM_KIND_SHOT)
            shot_release(s->owner, s, ch);
    }
    pcm_key_exec_off(ch);
}

/*
 * 0x7FFC shot_pause (host 0x13; no sd 1.00.18 API sends it)
 * Confidence: high
 * r0 port: sets PORT_FLG_PAUSE; if playing, keys the channel off (release phase). If this was the
 * last loop pass (loops_left == 1) the slot is released and PLAY cleared as well.
 * Notes: not a real pause - the sample restarts from its beginning on shot_continue. The PLAY flag
 * (and the slot) stay while paused, so the channel stays reserved.
 */
void shot_pause(u32 port)
{
    PcmPlayer *pl = shot_pl(port);
    u8 flags = pl->flags;
    u8 nflags = flags | PORT_FLG_PAUSE;
    PcmSlot *s;

    pl->flags = nflags;
    if (!(flags & PORT_FLG_PLAY))
        return;
    s = pcm_player_slot(pl);
    if (pl->loops_left == 1) {
        u8 keep_fx = pl->fx_ch;
        pcm_release(pl, s);
        pl->flags = nflags & ~PORT_FLG_PLAY;
        pl->fx_ch = keep_fx;
    }
    pcm_key_exec_off(s->ch);
}

/*
 * 0x8058 shot_continue (host 0x14; no sd 1.00.18 API sends it)
 * Confidence: high
 * r0 port: if paused, clear PAUSE; if still playing, key the channel on again (KYONEX|KYONB),
 * which restarts the sample from its start address.
 */
void shot_continue(u32 port)
{
    PcmPlayer *pl = shot_pl(port);
    u8 flags = pl->flags;

    if (!(flags & PORT_FLG_PAUSE))
        return;
    flags &= ~PORT_FLG_PAUSE;
    pl->flags = flags;
    if (!(flags & PORT_FLG_PLAY))
        return;
    pcm_key_exec_on(pcm_player_slot(pl)->ch);
}

/* ------------------------------------------------------------------------- */
/* Reset parameters                                                          */
/* ------------------------------------------------------------------------- */

/*
 * 0x815C pcm_reset_fields
 * Confidence: high
 * Host-parameter defaults of one player: speed/pitch target and current = 0x8000, all fade
 * state 0, pitch_ofs 0, vol/pan targets and current = 0x80, fx_lvl = direct_lvl = 0x80,
 * fx_ch = 0x80 (no override); clears CHG_VOL/CHG_PAN (CHG_SPEED/CHG_PITCH are left set).
 * Notes: the asm stores 0 words and then 0x80 into byte 1 (e.g. +0x41) to get 0x8000. Its tail
 * (0x81B4) decrements the caller's loop counter r7 and returns from the entry when it hits 0.
 */
static void pcm_reset_fields(PcmPlayer *pl)
{
    pl->speed_target = 0x8000;
    pl->speed = 0x8000;
    pl->pitch_target = 0x8000;
    pl->pitch = 0x8000;
    pl->speed_interval = pl->speed_count = pl->speed_step = pl->pitch_dirty = 0;
    pl->pitch_interval = pl->pitch_count = pl->pitch_step = pl->_57 = 0;
    pl->pitch_ofs = 0;
    pl->vol_interval = pl->vol_step = pl->pan_interval = pl->pan_step = 0;
    pl->vol_count = pl->pan_count = 0;
    pl->vol_target = pl->pan_target = pl->fx_lvl = pl->direct_lvl = 0x80;
    pl->fx_ch = 0x80;
    pl->pan = 0x80;
    pl->volume = 0x80;
    pl->flags &= ~(PORT_FLG_CHG_VOL | PORT_FLG_CHG_PAN);
}

/*
 * 0x8128 pcm_reset_ch_regs
 * Confidence: high
 * For a playing player (r11 = its channel): TL = tl_base, DIPAN = pan_base, IMXL from imxl,
 * DISDL = disdl_base, then falls into pcm_reset_fields.
 * Notes: QUIRK: imxl is already stored in bits 7:4 but is shifted left by 4 again, so the byte
 * store leaves IMXL = 0 (fx send muted) until the next FxLev/FxCh command or play.
 * ISEL is kept; pl->disdl is not updated.
 */
static void pcm_reset_ch_regs(PcmPlayer *pl, volatile AicaChannel *ch)
{
    CH_B(ch, CH_TL) = pl->tl_base;
    pl->tl = pl->tl_base;
    CH_B(ch, CH_DIPAN) = pl->pan_base;
    pl->dipan = pl->pan_base;
    CH_B(ch, CH_DSP_SEND) = (u8)((CH_B(ch, CH_DSP_SEND) & 0x0F) | (pl->imxl << 4));
    CH_B(ch, CH_DISDL) = pl->disdl_base;
    pcm_reset_fields(pl);
}

static void pcm_reset_prm(PcmPlayer *pl, u32 n, int clear_loop)
{
    while (n--) {
        if (clear_loop) {                  /* shot variants only (0x80B8) */
            pl->ring_len = 0;
            pl->loop_mode = 0;
        }
        if (pl->flags & PORT_FLG_PLAY)
            pcm_reset_ch_regs(pl, pcm_player_slot(pl)->ch);
        else
            pcm_reset_fields(pl);
        pl++;
    }
}

/*
 * 0x809C shot_reset_prm (host 0x1E sdShotResetPrm, cmd[2] != 0xFF)
 * Confidence: high
 * r0 port: loop param/mode = 0 and parameter defaults (pcm_reset_ch_regs / pcm_reset_fields).
 */
void shot_reset_prm(u32 port)
{
    pcm_reset_prm(shot_pl(port), 1, 1);
}

/*
 * 0x8090 shot_reset_prm_all (host 0x1E sdShotResetAllPrm, cmd[2] == 0xFF)
 * Confidence: high
 * As shot_reset_prm for 16 players starting at 0x11000.
 * Notes: QUIRK: there are only 8 one-shot players; the other 8 are the stream channels of
 * ports 0-3, whose ring_len and loop_mode (LP flag) get zeroed too (breaks the SH-4 total
 * sample count of a playing stream: status +0x14 uses ring_len).
 */
void shot_reset_prm_all(void)
{
    pcm_reset_prm(g_shot_player, 16, 1);
}

/*
 * 0x80EC pstm_reset_prm (host 0x2E sdPstmResetPrm, cmd[2] != 0xFF)
 * Confidence: high
 * r0 port: parameter defaults for both channels of the stream port.
 */
void pstm_reset_prm(u32 port)
{
    pcm_reset_prm(pstm_pl(port), 2, 0);
}

/*
 * 0x80E0 pstm_reset_prm_all (host 0x2E sdPstmResetAllPrm, cmd[2] == 0xFF)
 * Confidence: high
 * Parameter defaults for all 16 stream channels.
 */
void pstm_reset_prm_all(void)
{
    pcm_reset_prm(g_pstm_player, 16, 0);
}

/* ------------------------------------------------------------------------- */
/* PCM-stream ports                                                          */
/* ------------------------------------------------------------------------- */

/*
 * 0x79BC pstm_play (host 0x21 sdPstmPlay)
 * Confidence: high
 * The host handler (0x5560) passes r0 port = cmd[2]&7 (&0xF), r1 fmt = (cmd[2]>>4)&3 (AICA PCMS),
 * r2 banks = cmd[7] | cmd[8]<<8 | stereo<<16 (cmd[2] bit7; cmd[8] only if stereo),
 * r3 tl = cmd[3] (base-volume byte, 0xFF - 2*vol), r4 freq = cmd[4..5] (AICA OCT/FNS),
 * r5 prio<<3 = cmd[6].
 * For ch0 (and ch1 if stereo): allocate a channel (kind 2 / 3), look up the SPSR ring bank
 * (0x14180 + bank*8 = {addr, size}; addr 0 -> PCM_ERR_NO_RING), clamp the ring size per format
 * (16-bit 0x1F000, 8-bit 0xF000, ADPCM 0x7000 and PCMS forced to 3 = long-stream ADPCM) and
 * program a looping channel: SA = LSA-base = ring, LSA 0, LEA = ring size in samples,
 * KYONB|LPCTL, AR 0x1F, RR 0x15, LPOFF + Q 4, FLV 0x1FF8, FEG rates 0x1F, LFORE, then
 * pcm_apply_params with pan_base 0x00 (mono), 0x1F / 0x0F (stereo left / right).
 * Both channels are keyed on together by one KYONEX after the last one.
 * Notes: pl->loops_left of ch0 keeps the bank word. QUIRKs: SA_LO = 0x8000 transient write;
 * a missing ring frees the slot but leaves the prio byte, and for ch1 leaves ch0 allocated with
 * KYONB set but never executed (any later KYONEX starts it). Mono play on a port whose ch1 is
 * still playing frees ch1's slot (pcm_slot_free + key-off; that KYONEX also starts ch0) but does
 * not clear ch1's PLAY flag, so ch1 stays "playing" on slot 0.
 */
void pstm_play(u32 port, u32 fmt, u32 banks, u32 tl, u32 freq, u32 prio)
{
    PcmPlayer *pl0 = pstm_pl(port);
    PcmPlayer *pl = pl0;
    u32 kind = PCM_KIND_PSTM_L;
    u32 pan;

    pl->loops_left = banks;
    pan = (banks & 0x10000) ? 0x1F : 0x00;
    prio >>= 3;
    for (;;) {
        PcmSlot *s = pcm_slot_alloc(pl, prio, kind);
        volatile AicaChannel *ch;
        u32 bank, addr, size, lea;

        if (!s)
            return;
        ch = s->ch;
        ch->sa_lo = 0x8000;                /* QUIRK, overwritten below */
        pl->pcms = (u8)fmt;
        pl->tl_base = (u8)tl;
        pl->pitch_base = freq;
        pl->imxl = 0;
        bank = (kind != PCM_KIND_PSTM_L) ? (pl0->loops_left >> 8) & 0xFF   /* [r12-0x1F] */
                                         : pl0->loops_left & 0xFF;
        addr = BANK_TABLE_SPSR[bank * 2];
        if (addr == 0) {
            pl->flags &= ~PORT_FLG_PLAY;
            s->kind = 0;
            s->owner = 0;
            pl->err |= PCM_ERR_NO_RING;
            return;
        }
        pl->start = addr;
        size = BANK_TABLE_SPSR[bank * 2 + 1];
        if ((s32)fmt > 1) {                /* ADPCM */
            if ((s32)size > 0x7000)
                size = 0x7000;
            lea = size << 1;
            fmt = 3;
        } else if ((s32)fmt < 1) {         /* 16-bit */
            if ((s32)size > 0x1F000)
                size = 0x1F000;
            lea = size >> 1;
        } else {                           /* 8-bit */
            if ((s32)size > 0xF000)
                size = 0xF000;
            lea = size;
        }
        ch->lea = lea;
        pl->ring_len = size;
        pl->cur_addr = pl->start;
        ch->ctl_sa_hi = (addr >> 16) | AICA_KYONB | AICA_LPCTL | (fmt << 7);
        ch->flv[0] = ch->flv[1] = ch->flv[2] = ch->flv[3] = ch->flv[4] = 0x1FF8;
        ch->sa_lo = addr & 0xFFFF;
        CH_B(ch, CH_Q) = 0x24;             /* LPOFF, Q = 4 */
        ch->lsa = 0;
        ch->env_ad = 0x1F;                 /* AR 31 */
        ch->lfo = 0x8000;                  /* LFORE */
        ch->fenv_a = 0x1F;
        ch->fenv_r = 0x1F;
        pl->cur_pos = 0;
        pl->loop_count = 0;
        pl->pan_base = (u8)pan;
        ch->env_dr = 0x1F - 0x0A;          /* RR 0x15 */
        pcm_apply_params(pl, ch, tl, pan, freq);
        (void)AICA_MON_EG;
        (void)AICA_MON_CA;
        if (pan == 0x0F) {                 /* ch1 done: key both on */
            pcm_key_exec_on(ch);
            return;
        }
        pl++;
        if (pan > 0x0F) {                  /* stereo: now ch1 */
            pan = 0x0F;
            kind = PCM_KIND_PSTM_R;
            continue;
        }
        /* mono */
        if (!(pl->flags & PORT_FLG_PLAY)) {
            pcm_key_exec_on(ch);
            return;
        }
        {
            PcmSlot *s1 = pcm_player_slot(pl);
            u8 keep_fx = pl->fx_ch;
            pcm_slot_free(pl, s1);         /* QUIRK: PLAY flag of ch1 left set */
            pl->pan_base = 0x0F;
            pl->fx_ch = keep_fx;
            pcm_key_exec_off(s1->ch);      /* KYONEX also starts ch0 */
            return;
        }
    }
}

/*
 * 0x7E78 pstm_stop (host 0x22 sdPstmStop)
 * Confidence: high
 * r0 port: if ch0 is playing, pstm_release (ch0 and ch1) and KYONEX key-off.
 * Notes: a stale ch1 (see pstm_play) is not stopped when ch0 is idle.
 */
void pstm_stop(u32 port)
{
    PcmPlayer *pl = pstm_pl(port);
    PcmSlot *s;

    if (!(pl->flags & PORT_FLG_PLAY))
        return;
    s = pcm_player_slot(pl);
    pcm_key_exec_off(pstm_release(pl, s, s->ch));
}

/*
 * 0x7F80 pstm_stop_all (host 0x2F sdPstmStopAll, host 0x80 with cmd[4] != 0)
 * Confidence: high
 * For every slot owned by a stream channel (kind 2/3): pcm_slot_free keeping fx_ch,
 * disdl_base = 0x0F, clear PLAY and KYONB, loop_mode = start = ring_len = 0,
 * pan_base = 0x1F (kind 2) / 0x0F (kind 3). Then one KYONEX key-off (slot 15's register).
 */
void pstm_stop_all(void)
{
    volatile AicaChannel *ch = 0;
    int i;

    for (i = 0; i < NUM_PCM_SLOTS; i++) {
        PcmSlot *s = &g_pcm_slot[i];
        PcmPlayer *pl;
        u32 pan;
        u8 keep_fx;

        ch = s->ch;
        if (s->kind < PCM_KIND_PSTM_L)
            continue;
        pan = (s->kind == PCM_KIND_PSTM_L) ? 0x1F : 0x0F;
        pl = s->owner;
        keep_fx = pl->fx_ch;
        pcm_slot_free(pl, s);
        pl->fx_ch = keep_fx;
        pl->disdl_base = 0x0F;
        pl->flags &= ~PORT_FLG_PLAY;
        ch->ctl_sa_hi &= ~AICA_KYONB;
        pl->loop_mode = 0;
        pl->start = 0;
        pl->ring_len = 0;
        pl->pan_base = (u8)pan;
    }
    pcm_key_exec_off(ch);
}
