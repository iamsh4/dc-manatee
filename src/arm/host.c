/*
 * host.c - SH-4 -> ARM host command interface (0x4948-0x5C97).
 *
 * The SH-4 sd library copies a batch of up to 32 16-byte records to
 * g_host_cmd[] (0x13200) and sets host_cmd_pending (0x13400); the main loop
 * calls host_cmd_poll, which executes every slot whose code byte is nonzero.
 * Wire layout of each command: docs/research/host_protocol.md section 3
 * ("[n]" below = byte n of the 16-byte record; [1] is always 0 from the SH-4).
 *
 * host_cmd_exec is one big asm routine with two jump tables:
 *   0x4A44: 64 x "b case" for codes 0x00-0x3F
 *   0x4B44: 16 x "b case" for codes 0x80-0x8F
 * Every case ends with "mov pc, lr" back into host_cmd_poll, which stores the
 * low byte of r0 as the command's "result" into byte 1 of its command-log entry.
 * Many cases do not set r0 explicitly; the C returns what the asm leaves in r0
 * (traced through the callees in host_helpers.c / sequencer.c / fx.c: the
 * one-shot/stream parameter helpers never write r0, so it is still the port
 * value they masked or were given). Nothing on the SH-4
 * side reads the command log (0x14800 = drv_layout_table slot 0xD4, cached but
 * unused by the R9 library), so the result bytes are debugging aids only.
 *
 * Case bodies at 0x5A8C (0x82), 0x5BD8 (0x83), 0x5C94/0x5C98 (0x84) and
 * 0x5DCC-0x6167 (0x85-0x8F) are implemented in fx.c; they are dispatched here.
 */
#include "manatee.h"

/* Byte / little-endian word n of the 16-byte command record. */
#define CB(c, n) (((volatile const u8 *)(c))[n])
#define CW(c, n) (((volatile const u32 *)(c))[(n) >> 2])

#define BANK_TABLE      SRAM_PTR(volatile u32, 0x14000) /* {addr, size} x 16 per type */
#define BANK_SMSB(n)    BANK_TABLE[0x00 + (n) * 2]      /* 0x14000 MIDI sequence banks */
#define BANK_SOSB(n)    BANK_TABLE_SOSB[(n) * 2]        /* 0x14100 one-shot banks */
#define mono_flag       SRAM(u32, 0x13404)              /* 0 stereo, 0xFFFFFFFF mono (host 0x8A) */

/* Scratch word inside the code image (0x4994): log entry of the current command. */
static volatile u8 *cmd_log_cur;

/* ------------------------------------------------------------------------- */
/* Shared inline sequences                                                   */
/* ------------------------------------------------------------------------- */

/* |a - b| as computed by "subs; eormi #-1; addmi #1". */
static u32 absdiff(u32 a, u32 b)
{
    u32 d = a - b;
    return ((s32)d < 0) ? (u32)-(s32)d : d;
}

/*
 * Bank header check used by 0x01 (SMSB) and 0x11 (SOSB); inline in the asm
 * (0x4BB8, 0x5208). Returns 0 or the error bit: 2 bad magic, 8 version
 * byte != 1, 4 no "ENDB" at size-4.
 * Notes: the table entry is not tested for 0 - an empty slot reads the magic
 * from address 0 (an ARM vector, 0xEA......) and fails with 2.
 */
static u32 bank_check(const u8 *bank, u32 magic)
{
    if (*(const u32 *)bank != magic)
        return 2;
    if (bank[4] != 1)
        return 8;
    if (*(const u32 *)(bank + *(const u32 *)(bank + 8) - 4) != FX_ENDB_MAGIC)
        return 4;
    return 0;
}

/*
 * MIDI port status update after a sequencer call (inline 5 times: 0x4C08,
 * 0x4CD4, 0x4D64, 0x4DB4, 0x51A4). seq_state = SeqPlayer.state returned by
 * the sequencer (in r1). status = (status & 0xFC) | (state & 3);
 * err = g_seq.err; status bit7 = (err != 0). Returns r0 = port << 7
 * (the asm computes the PortState offset in r0 and leaves it there).
 * Notes: only the play command (0x01) clears g_seq.err afterwards.
 */
static u32 port_status_sync(u32 port, u32 seq_state)
{
    PortState *ps = &g_port[port];
    u32 st = (ps->status & 0xFC) | (seq_state & 3);
    u32 err = g_seq.err;

    ps->err = (u8)err;
    if (err & 0xFF)
        st |= PORT_FLG_TROUBLE;
    else
        st &= ~PORT_FLG_TROUBLE;
    ps->status = (u8)st;
    return port << 7;
}

/*
 * Key off the AICA channel of every driver voice (0..47) - of one port only
 * if port_only. clear: also zero the voice's flags..level word (frees it).
 * AICA write: ctl = (ctl & 0x7FF) | KYONEX (KYONB cleared, SA/PCMS/loop kept).
 * Inline at 0x4C84 (stop), 0x4D1C (pause, no clear), 0x5158 and 0x5974 (all).
 */
static void voices_kill(u32 port, int port_only, int clear)
{
    Voice *v = g_voice;
    volatile AicaChannel *ac = AICA_CH;
    int n;

    for (n = 48; n != 0; n--, v++, ac++) {
        if (port_only && v->port != port)
            continue;
        if (clear)
            *(volatile u32 *)v = 0;
        ac->ctl_sa_hi = (ac->ctl_sa_hi & 0x7FF) | AICA_KYONEX;
    }
}

/*
 * PCM player status update after a one-shot/stream play (0x5260, 0x55E4):
 * flags bit7 = (err != 0), exported err byte (status +0x1C) = err, err = 0.
 * Returns the new flags byte (left in r0 by the asm).
 */
static u32 pcm_status_sync(PcmPlayer *pl, volatile PortStatus *st)
{
    u32 err = pl->err;
    u32 f = pl->flags;

    f = err ? (f | 0x80) : (f & ~0x80u);
    pl->flags = (u8)f;
    st->err = (u8)err;
    pl->err = 0;
    return f & 0xFF;
}

/*
 * Fade parameter computation, 8-bit values (MIDI port vol/pan, 0x4E0C/0x4E94).
 * fade = [4..5] (ms?). interval = fade >> 3; step = |cur - target|; both are
 * halved together while step >= 2 and interval >= 2 (keeps the ratio,
 * limits the interval). fade == 0: interval 0 (jump), step = low byte of
 * 0xC000 (r0 still holds the port base) = 0.
 */
static u32 fade8_calc(u32 cur, u32 target, u32 fade, u32 *interval)
{
    u32 step = 0xC000, iv = fade;

    if (fade != 0) {
        iv >>= 3;
        step = absdiff(cur, target) & 0xFF;
        while ((step & 0xFE) && (iv & ~1u)) {
            step >>= 1;
            iv >>= 1;
        }
    }
    *interval = iv;
    return step;
}

/*
 * Fade computation for PCM player speed/pitch (one-shot 0x5370/0x5410,
 * stream 0x56E4/0x5794): 16-bit values, target = w & 0xFFFF, fade = w >> 16.
 * Quirk: the halving loop condition is inverted w.r.t. the MIDI port version
 * ("tst interval,#~1; bne exit"): it only halves while interval <= 1, so
 * for interval >= 2 the step is the raw |cur - target| (clamped to 0xFF).
 * fade == 0: r0 still holds the player base address -> step 0xFF.
 */
static u32 pcm_fade16_calc(u32 cur, u32 target, u32 fade, u32 *interval)
{
    u32 step = 0x11000, iv = fade;

    if (fade != 0) {
        iv >>= 3;
        step = absdiff(cur, target) & 0xFFFF;
        while ((step & ~1u) && !(iv & ~1u)) {
            step >>= 1;
            iv >>= 1;
        }
    }
    if (step & ~0xFFu)
        step = 0xFF;
    *interval = iv;
    return step;
}

/* ------------------------------------------------------------------------- */
/* 0x0n: MIDI (sequence) ports                                               */
/* ------------------------------------------------------------------------- */

/*
 * 0x4B9C hcmd_01_midi_play - sdMidiPlay: [2]=port [3]=SMSB bank [4]=song [5]=prio<<3
 * Confidence: high
 * Validates the SMSB bank (bank table 0x14000 + bank*8) and starts the
 * sequence with seq_play, then syncs PortState.status/err and clears g_seq.err.
 * Bank errors: status |= 0x80 (trouble), err_hi |= 2/8/4 (magic/version/end),
 * exported by the status export at +0x1F.
 * Result: success -> (port << 7) low byte; bank error -> the new err_hi byte.
 * Notes: port not range-checked here (seq_play masks it with 7, PortState
 * indexing does not).
 */
static u32 hcmd_01_midi_play(volatile HostCmd *c)
{
    u32 port = CB(c, 2), song = CB(c, 4), prio = CB(c, 5);
    const u8 *bank = (const u8 *)BANK_SMSB(CB(c, 3));
    u32 err = bank_check(bank, SMSB_MAGIC), r;
    PortState *ps = &g_port[port];

    if (err) {
        ps->status |= PORT_FLG_TROUBLE;
        ps->err_hi |= (u8)err;
        return ps->err_hi;
    }
    r = port_status_sync(port, seq_play(port, song, prio, bank));
    g_seq.err = 0;
    return r;
}

/*
 * 0x4C80 hcmd_02_midi_stop - sdMidiStop: [2]=port
 * Confidence: high
 * Keys off and frees every voice of the port, seq_stop, status sync.
 * Result: (port << 7) low byte.
 */
static u32 hcmd_02_midi_stop(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    voices_kill(port, 1, 1);
    return port_status_sync(port, seq_stop(port));
}

/*
 * 0x4D18 hcmd_03_midi_pause - sdMidiPause: [2]=port
 * Confidence: high
 * Keys off the port's voices but keeps their Voice records, seq_pause
 * (which also drops the port's queued note-ons), status sync.
 * Result: (port << 7) low byte.
 */
static u32 hcmd_03_midi_pause(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    voices_kill(port, 1, 0);
    return port_status_sync(port, seq_pause(port));
}

/*
 * 0x4DA4 hcmd_04_midi_continue - sdMidiContinue: [2]=port
 * Confidence: high
 * seq_continue (re-sends the held notes), status sync. Result: (port << 7) low byte.
 */
static u32 hcmd_04_midi_continue(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    return port_status_sync(port, seq_continue(port));
}

/*
 * 0x4DF0 hcmd_05_midi_set_vol - sdMidiSetVol: [2]=port [3]=vol+0x80 [4..5]=fade
 * Confidence: high
 * Sets PortState vol_target and the fade parameters (see fade8_calc);
 * port_vol_fade does the stepping. Result 0.
 */
static u32 hcmd_05_midi_set_vol(volatile HostCmd *c)
{
    u32 port = CB(c, 2), target = CB(c, 3), iv;
    PortState *ps = &g_port[port];
    u32 step = fade8_calc(ps->volume, target, CW(c, 4) & 0xFFFF, &iv);

    ps->vol_target = (u8)target;
    ps->vol_interval = (u8)iv;
    ps->vol_count = (u8)iv;
    ps->vol_step = (u8)step;
    return 0;
}

/*
 * 0x4E78 hcmd_06_midi_set_pan - sdMidiSetPan (lib-only): [2]=port [3]=pan+0x80 [4..5]=fade
 * Confidence: high
 * As 0x05 for the pan fields (+0x06..+0x0A). Result 0.
 */
static u32 hcmd_06_midi_set_pan(volatile HostCmd *c)
{
    u32 port = CB(c, 2), target = CB(c, 3), iv;
    PortState *ps = &g_port[port];
    u32 step = fade8_calc(ps->pan, target, CW(c, 4) & 0xFFFF, &iv);

    ps->pan_target = (u8)target;
    ps->pan_interval = (u8)iv;
    ps->pan_count = (u8)iv;
    ps->pan_step = (u8)step;
    return 0;
}

/*
 * 0x4F00 hcmd_07_midi_set_speed - sdMidiSetSpeed (lib-only):
 *   [2]=port [4..5]=speed*4+0x8000 [6..7]=fade
 * Confidence: high
 * target = speed & 0xFFF0; fade is masked with 0xFFF0 too (quirk: the same
 * literal is reused). interval = fade >> 3, step = |speed - target| & 0xFFF0,
 * halved together while both >= 2; step clamped to 0xFF. fade == 0: interval
 * 0, step 0xFF (r0 = 0xC000 base). Result 0.
 */
static u32 hcmd_07_midi_set_speed(volatile HostCmd *c)
{
    u32 port = CB(c, 2), w = CW(c, 4);
    PortState *ps = &g_port[port];
    u32 target = w & 0xFFF0, iv = (w >> 16) & 0xFFF0, step = 0xC000;

    if (iv != 0) {
        iv >>= 3;
        step = absdiff(ps->speed, target) & 0xFFF0;
        while ((step & ~1u) && (iv & ~1u)) {
            step >>= 1;
            iv >>= 1;
        }
    }
    if (step & ~0xFFu)
        step = 0xFF;
    ps->speed_target = target;
    ps->speed_interval = (u8)iv;
    ps->speed_count = (u8)iv;
    ps->speed_step = (u8)step;
    return 0;
}

/*
 * 0x4FA0 hcmd_08_midi_set_pitch - sdMidiSetPitch: [2]=port [4..5]=pitch+0x8000 [6..7]=fade
 * Confidence: high
 * target = pitch & 0xFFFE, interval = (fade & 0xFFFE) >> 3,
 * step = |pitch - target| & 0xFFFE. Unlike the other fades this is a
 * do-while: step and interval are always halved at least once (the stored
 * step is half the delta - port_pitch_fade applies step*2), and there is no
 * fade == 0 special case. Step clamped to 0xFF. Result 0.
 */
static u32 hcmd_08_midi_set_pitch(volatile HostCmd *c)
{
    u32 port = CB(c, 2), w = CW(c, 4);
    PortState *ps = &g_port[port];
    u32 target = w & 0xFFFE, iv = ((w >> 16) & 0xFFFE) >> 3;
    u32 step = absdiff(ps->pitch, target) & 0xFFFE;

    do {
        step >>= 1;
        iv >>= 1;
    } while ((step & ~1u) && (iv & ~1u));
    if (step & ~0xFFu)
        step = 0xFF;
    ps->pitch_target = target;
    ps->pitch_interval = (u8)iv;
    ps->pitch_count = (u8)iv;
    ps->pitch_step = (u8)step;
    return 0;
}

/*
 * 0x5038 hcmd_0a_midi_set_fx_lev - sdMidiSetFxLev: [2]=port [3]=lev+0x80
 * Confidence: high
 * PortState.fx_send = [3]. No dirty flag: affects notes keyed afterwards. Result 0.
 */
static u32 hcmd_0a_midi_set_fx_lev(volatile HostCmd *c)
{
    g_port[CB(c, 2)].fx_send = CB(c, 3);
    return 0;
}

/*
 * 0x505C hcmd_0b_midi_set_drct_lev - sdMidiSetDrctLev (lib-only): [2]=port [3]=lev+0x80
 * Confidence: high
 * PortState.direct_lvl = [3] (new notes only). Result 0.
 */
static u32 hcmd_0b_midi_set_drct_lev(volatile HostCmd *c)
{
    g_port[CB(c, 2)].direct_lvl = CB(c, 3);
    return 0;
}

/*
 * 0x5080 hcmd_0c_midi_break_loop - no sd 1.00.18 API: [2]=port
 * Confidence: high
 * seq_break_loop: SeqPlayer.loop_left = 0 (the running loop ends at its
 * next loop point). Result: port & 7 (left in r0 by the callee).
 */
static u32 hcmd_0c_midi_break_loop(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    seq_break_loop(port);
    return port & 7;
}

/*
 * 0x50B8 port_reset_prm (called by 0x0E with r4 = &g_port[port], r6 = &g_seq_player[port])
 * Confidence: high
 * Resets the port's host parameters: vol/pan target and current 0x80,
 * pitch/speed target and current 0x8000, all fade interval/count/step 0,
 * direct_lvl/fx_send 0x80, SeqPlayer.tempo_scale 0x4000 (1x). Returns 0.
 * Notes: the vol/pan/pitch dirty flags are not set, so sounding voices keep
 * their old volume/pan/pitch until something else marks the port dirty.
 * Single-port entry 0x5094 falls into this routine.
 */
static u32 port_reset_prm(PortState *ps, SeqPlayer *sp)
{
    ps->vol_target = 0x80;
    ps->volume = 0x80;
    ps->vol_interval = 0;
    ps->vol_count = 0;
    ps->vol_step = 0;
    ps->pan_target = 0x80;
    ps->pan = 0x80;
    ps->pan_interval = 0;
    ps->pan_count = 0;
    ps->pan_step = 0;
    ps->pitch_target = 0x8000;
    ps->pitch = 0x8000;
    ps->pitch_interval = 0;
    ps->pitch_count = 0;
    ps->pitch_step = 0;
    ps->speed_target = 0x8000;
    ps->speed = 0x8000;
    ps->speed_interval = 0;
    ps->speed_count = 0;
    ps->speed_step = 0;
    ps->direct_lvl = 0x80;
    ps->fx_send = 0x80;
    sp->tempo_scale = 0x4000;
    return 0;
}

/*
 * 0x5094 hcmd_0e_midi_reset_prm - sdMidiResetPrm / sdMidiResetAllPrm (lib-only):
 *   [2]=port, 0xFF = all 8 ports
 * Confidence: high
 * port_reset_prm for one or all ports. Result 0.
 * Notes: a single port is not range-checked (0x08-0xFE write past the tables).
 */
static u32 hcmd_0e_midi_reset_prm(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    if (port != 0xFF)
        return port_reset_prm(&g_port[port], &g_seq_player[port]);
    for (port = 0; port < NUM_PORTS; port++)
        port_reset_prm(&g_port[port], &g_seq_player[port]);
    return 0;
}

/*
 * 0x5158 hcmd_0f_midi_stop_all - sdMidiStopAll (lib-only): no parameters
 * Confidence: high
 * Keys off and frees all 48 driver voices, then seq_stop + status sync for
 * ports 0..7. Result: 8 (the loop counter left in r0).
 */
static u32 hcmd_0f_midi_stop_all(volatile HostCmd *c)
{
    u32 port;

    voices_kill(0, 0, 1);
    for (port = 0; port < NUM_PORTS; port++)
        port_status_sync(port, seq_stop(port));
    return 8;
}

/* ------------------------------------------------------------------------- */
/* 0x1n: one-shot ports (PCM players 0..7)                                   */
/* ------------------------------------------------------------------------- */

/*
 * 0x51F4 hcmd_11_shot_play - sdShotPlay: [2]=port [3]=SOSB bank [4]=data [5]=prio<<3
 * Confidence: high
 * Validates the SOSB bank (0x14100 + bank*8) and starts the one-shot with
 * shot_play, then pcm_status_sync (flags bit7, g_shot_status[port].err).
 * Bank errors: player flags |= 0x80, g_shot_status[port].err_hi |= 2/8/4.
 * Result: success -> the new player flags byte; error -> the new err_hi byte.
 */
static u32 hcmd_11_shot_play(volatile HostCmd *c)
{
    u32 port = CB(c, 2);
    const u8 *bank = (const u8 *)BANK_SOSB(CB(c, 3));
    u32 err = bank_check(bank, SOSB_MAGIC);

    if (err) {
        g_shot_player[port].flags |= 0x80;
        g_shot_status[port].err_hi |= (u8)err;
        return g_shot_status[port].err_hi;
    }
    shot_play(port, CB(c, 4), CB(c, 5), (const SosbHeader *)bank);
    return pcm_status_sync(&g_shot_player[port], &g_shot_status[port]);
}

/*
 * 0x52E4 hcmd_12_shot_stop - sdShotStop (lib-only): [2]=port
 * 0x52F8 hcmd_13_shot_pause - no library API: [2]=port
 * 0x530C hcmd_14_shot_continue - no library API: [2]=port
 * Confidence: high
 * Plain calls; r0 is saved around the call, so the result is the port byte.
 */
static u32 hcmd_12_shot_stop(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    shot_stop(port);
    return port;
}

static u32 hcmd_13_shot_pause(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    shot_pause(port);
    return port;
}

static u32 hcmd_14_shot_continue(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    shot_continue(port);
    return port;
}

/*
 * 0x5320 hcmd_15_shot_set_vol - sdShotSetVol: [2]=port [3]=vol+0x80 [4..5]=fade
 * 0x5348 hcmd_16_shot_set_pan - sdShotSetPan (lib-only): [2]=port [3]=pan+0x80 [4..5]=fade
 * Confidence: high (dispatch) - the work is in the callee.
 * Result: port & 7 (the callee does "and r0,r0,#7"; pcm_fade_calc and
 * pcm_next_ch leave r0 alone - bit 4 is clear, so pcm_next_ch returns).
 */
static u32 hcmd_15_shot_set_vol(volatile HostCmd *c)
{
    shot_set_vol(CB(c, 2), CB(c, 3), CW(c, 4) & 0xFFFF);
    return CB(c, 2) & 7;
}

static u32 hcmd_16_shot_set_pan(volatile HostCmd *c)
{
    shot_set_pan(CB(c, 2), CB(c, 3), CW(c, 4) & 0xFFFF);
    return CB(c, 2) & 7;
}

/*
 * 0x5370 hcmd_17_shot_set_speed - sdShotSetSpeed (lib-only): [2]=port [4..5]=speed+0x8000 [6..7]=fade
 * 0x5410 hcmd_18_shot_set_pitch - sdShotSetPitch: [2]=port [4..5]=pitch+0x8000 [6..7]=fade
 * Confidence: high
 * Store target and fade parameters (pcm_fade16_calc) in PcmPlayer
 * speed_* (+0x40..) / pitch_* (+0x4C..); pcm_speed_fade/pcm_pitch_fade step
 * them. Result 0.
 */
static u32 hcmd_17_shot_set_speed(volatile HostCmd *c)
{
    PcmPlayer *pl = &g_shot_player[CB(c, 2)];
    u32 w = CW(c, 4), target = w & 0xFFFF, iv;
    u32 step = pcm_fade16_calc(pl->speed, target, w >> 16, &iv);

    pl->speed_target = target;
    pl->speed_interval = (u8)iv;
    pl->speed_count = (u8)iv;
    pl->speed_step = (u8)step;
    return 0;
}

static u32 hcmd_18_shot_set_pitch(volatile HostCmd *c)
{
    PcmPlayer *pl = &g_shot_player[CB(c, 2)];
    u32 w = CW(c, 4), target = w & 0xFFFF, iv;
    u32 step = pcm_fade16_calc(pl->pitch, target, w >> 16, &iv);

    pl->pitch_target = target;
    pl->pitch_interval = (u8)iv;
    pl->pitch_count = (u8)iv;
    pl->pitch_step = (u8)step;
    return 0;
}

/*
 * 0x54B0 hcmd_19_shot_set_fx_ch - sdShotSetFxCh (lib-only): [2]=port [3]=fx_in_ch [4]=base_lev+0x80
 * 0x54CC hcmd_1a_shot_set_fx_lev - sdShotSetFxLev: [2]=port [3]=lev+0x80
 * 0x54E8 hcmd_1b_shot_set_drct_lev - sdShotSetDrctLev (lib-only): [2]=port [3]=lev+0x80
 * 0x5504 hcmd_1c_shot_set_loop_mode - no library API: [2]=port [3]=mode 0..2 [4..7]=value
 * Confidence: high (dispatch). 0x1A/0x1B also load [4] into r2, which the
 * callees overwrite unread.
 * Result: port & 7 (callee's "and r0,r0,#7"; pcm_player_slot_a,
 * pcm_level_mix, pcm_fade_calc and shot_loops_apply do not touch r0).
 * 0x1C with mode [3] > 2: the callee branches to pcm_err_exit (0x81C4),
 * which loads r0 = the player's OLD err byte (before |= 0x20) and pops
 * host_cmd_exec's saved lr, returning straight to host_cmd_poll (stack
 * stays balanced) -> result = old g_shot_player[port & 7].err.
 */
static u32 hcmd_19_shot_set_fx_ch(volatile HostCmd *c)
{
    shot_set_fx_ch(CB(c, 2), CB(c, 3), CB(c, 4));
    return CB(c, 2) & 7;
}

static u32 hcmd_1a_shot_set_fx_lev(volatile HostCmd *c)
{
    shot_set_fx_lvl(CB(c, 2), CB(c, 3));
    return CB(c, 2) & 7;
}

static u32 hcmd_1b_shot_set_drct_lev(volatile HostCmd *c)
{
    shot_set_direct_lvl(CB(c, 2), CB(c, 3));
    return CB(c, 2) & 7;
}

static u32 hcmd_1c_shot_set_loop_mode(volatile HostCmd *c)
{
    u32 port = CB(c, 2) & 7, mode = CB(c, 3);
    u32 old_err = g_shot_player[port].err;

    shot_set_loop_mode(CB(c, 2), mode, CW(c, 4));
    return (mode > 2) ? old_err : port;
}

/*
 * 0x5520 hcmd_1e_shot_reset_prm - sdShotResetPrm / ResetAllPrm (lib-only): [2]=port / 0xFF
 * 0x554C hcmd_1f_shot_stop_all - sdShotStopAll: no parameters
 * Confidence: high (dispatch).
 * Result 0x1E: 0xFF for all ports, else port & 7 (r0 untouched by the
 * reset loop). 0x1F: shot_stop_all scans the 16 PCM slots with
 * "ldrb r0,[slot,#1]" (kind), so r0 = g_pcm_slot[15].kind as read before
 * that slot is released (released slots are cleared only after the load).
 */
static u32 hcmd_1e_shot_reset_prm(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    if (port == 0xFF) {
        shot_reset_prm_all();
        return port;
    }
    shot_reset_prm(port);
    return port & 7;
}

static u32 hcmd_1f_shot_stop_all(volatile HostCmd *c)
{
    u32 kind15 = g_pcm_slot[NUM_PCM_SLOTS - 1].kind;

    shot_stop_all();
    return kind15;
}

/* ------------------------------------------------------------------------- */
/* 0x2n: PCM stream ports (PCM players 8 + port*2 + ch)                      */
/* ------------------------------------------------------------------------- */

/*
 * Clears the play/pause/vol/speed flags (bits 0,1,2,4; mask 0xE8) of both
 * exported channel records of a stream port (0x55A8 and 0x5648).
 * Quirk: the asm first computes "flags & 0x17" of the callee's r1/r2 and ORs
 * it into a register that is immediately reloaded - dead code, probably
 * meant to copy the new state into the exported flags.
 */
static void pstm_status_clear(u32 port)
{
    volatile PortStatus *st = &g_pstm_status[port * 2];

    st[0].flags &= 0xE8;
    st[1].flags &= 0xE8;
}

/*
 * 0x5560 hcmd_21_pstm_play - sdPstmPlay: [2]=port|0x80 stereo|fmt<<4 [3]=base vol
 *   [4..5]=freq (AICA OCT/FNS) [6]=prio<<3 [7]=SPSR bank ch0 [8]=SPSR bank ch1
 * Confidence: high
 * Calls pstm_play(port = [2]&0xF, fmt = ([2]>>4)&3, banks, [3], freq, [6])
 * with banks = [7] | [8]<<8 | 0x10000 if stereo ([8] forced 0 for mono).
 * Then clears the exported flags (pstm_status_clear) and runs
 * pcm_status_sync for both channel players. Result 0.
 */
static u32 hcmd_21_pstm_play(volatile HostCmd *c)
{
    u32 b2 = CB(c, 2), port = b2 & 0xF, fmt = (b2 >> 4) & 3;
    u32 bank1 = (b2 & 0x80) ? CB(c, 8) : 0;
    u32 banks = CB(c, 7) | (bank1 << 8) | ((b2 & 0x80) << 9);
    PcmPlayer *pl = &g_pstm_player[port * 2];

    pstm_play(port, fmt, banks, CB(c, 3), CW(c, 4) & 0xFFFF, CB(c, 6));
    pstm_status_clear(port);
    pcm_status_sync(&pl[0], &g_pstm_status[port * 2]);
    pcm_status_sync(&pl[1], &g_pstm_status[port * 2 + 1]);
    return 0;
}

/*
 * 0x5638 hcmd_22_pstm_stop - sdPstmStop: [2]=port
 * Confidence: high
 * pstm_stop(port), then pstm_status_clear. Result: the port byte (r0 is
 * saved around the call).
 */
static u32 hcmd_22_pstm_stop(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    pstm_stop(port);
    pstm_status_clear(port);
    return port;
}

/*
 * 0x567C hcmd_25_pstm_set_vol - sdPstmSetVol: [2]=port|mask(0x20 ch0,0x10 ch1)
 *   [3]=vol0+0x80 [4..5]=fade0 [7]=vol1+0x80 [8..9]=fade1
 * 0x56B0 hcmd_26_pstm_set_pan - sdPstmSetPan: same layout with pan values
 * Confidence: high (dispatch). The callee takes the whole [2] byte (port
 * and channel mask).
 * Result: [2] & ~0x10 - r0 is the whole [2] byte; pcm_next_ch clears bit 4
 * (ch1 selected) before doing channel 1 and returns when it is clear.
 */
static u32 hcmd_25_pstm_set_vol(volatile HostCmd *c)
{
    pstm_set_vol(CB(c, 2), CB(c, 3), CW(c, 4) & 0xFFFF, CB(c, 7), CW(c, 8) & 0xFFFF);
    return CB(c, 2) & ~0x10u;
}

static u32 hcmd_26_pstm_set_pan(volatile HostCmd *c)
{
    pstm_set_pan(CB(c, 2), CB(c, 3), CW(c, 4) & 0xFFFF, CB(c, 7), CW(c, 8) & 0xFFFF);
    return CB(c, 2) & ~0x10u;
}

/*
 * 0x56E4 hcmd_27_pstm_set_speed - sdPstmSetSpeed (lib-only): [2]=port [4..5]=speed+0x8000 [6..7]=fade
 * 0x5794 hcmd_28_pstm_set_pitch - sdPstmSetPitch (lib-only): same layout
 * Confidence: high
 * As 0x17/0x18, computed from channel 0's current value and stored into
 * both channel players of the port. Result 0.
 * Notes: port not masked; [2] carrying channel-mask bits would index far
 * outside the player table.
 */
static u32 hcmd_27_pstm_set_speed(volatile HostCmd *c)
{
    PcmPlayer *pl = &g_pstm_player[CB(c, 2) * 2];
    u32 w = CW(c, 4), target = w & 0xFFFF, iv;
    u32 step = pcm_fade16_calc(pl[0].speed, target, w >> 16, &iv);
    int i;

    for (i = 0; i < 2; i++) {
        pl[i].speed_target = target;
        pl[i].speed_interval = (u8)iv;
        pl[i].speed_count = (u8)iv;
        pl[i].speed_step = (u8)step;
    }
    return 0;
}

static u32 hcmd_28_pstm_set_pitch(volatile HostCmd *c)
{
    PcmPlayer *pl = &g_pstm_player[CB(c, 2) * 2];
    u32 w = CW(c, 4), target = w & 0xFFFF, iv;
    u32 step = pcm_fade16_calc(pl[0].pitch, target, w >> 16, &iv);
    int i;

    for (i = 0; i < 2; i++) {
        pl[i].pitch_target = target;
        pl[i].pitch_interval = (u8)iv;
        pl[i].pitch_count = (u8)iv;
        pl[i].pitch_step = (u8)step;
    }
    return 0;
}

/*
 * 0x5844 hcmd_29_pstm_set_fx_ch - sdPstmSetFxCh: [2]=port [3]=fx_in_ch [4]=base_lev+0x80
 * 0x5860 hcmd_2a_pstm_set_fx_lev - sdPstmSetFxLev: [2]=port [3]=lev+0x80
 * 0x5878 hcmd_2b_pstm_set_drct_lev - sdPstmSetDrctLev (lib-only): [2]=port [3]=lev+0x80
 * 0x5890 hcmd_2e_pstm_reset_prm - sdPstmResetPrm / ResetAllPrm (lib-only): [2]=port / 0xFF
 * 0x58BC hcmd_2f_pstm_stop_all - sdPstmStopAll (lib-only)
 * Confidence: high (dispatch).
 * Result 0x29-0x2B, 0x2E: the unmodified [2] byte (the stream helpers mask
 * the port into r8, not r0). 0x2F: g_pcm_slot[15].kind as read by the slot
 * scan (as 0x1F).
 */
static u32 hcmd_29_pstm_set_fx_ch(volatile HostCmd *c)
{
    pstm_set_fx_ch(CB(c, 2), CB(c, 3), CB(c, 4));
    return CB(c, 2);
}

static u32 hcmd_2a_pstm_set_fx_lev(volatile HostCmd *c)
{
    pstm_set_fx_lvl(CB(c, 2), CB(c, 3));
    return CB(c, 2);
}

static u32 hcmd_2b_pstm_set_drct_lev(volatile HostCmd *c)
{
    pstm_set_direct_lvl(CB(c, 2), CB(c, 3));
    return CB(c, 2);
}

static u32 hcmd_2e_pstm_reset_prm(volatile HostCmd *c)
{
    u32 port = CB(c, 2);

    if (port == 0xFF)
        pstm_reset_prm_all();
    else
        pstm_reset_prm(port);
    return port;
}

static u32 hcmd_2f_pstm_stop_all(volatile HostCmd *c)
{
    u32 kind15 = g_pcm_slot[NUM_PCM_SLOTS - 1].kind;

    pstm_stop_all();
    return kind15;
}

/* ------------------------------------------------------------------------- */
/* 0x3n: GD-DA (CD audio = AICA EXTS inputs 0/1 = DSP output regs 16/17)     */
/* ------------------------------------------------------------------------- */

/*
 * 0x58D4 hcmd_35_gdda_set_vol - sdGddaSetVol (lib-only): [2]=l [3]=r (lib sends vol*2)
 * Confidence: high
 * Byte writes of [2]>>4 / [3]>>4 to the high byte of DSP_OUT(16)/(17)
 * (0x802041/0x802045): EFSDL = vol>>4, bits 15:12 cleared. Result 0.
 */
static u32 hcmd_35_gdda_set_vol(volatile HostCmd *c)
{
    AICA_REG8(0x2041) = CB(c, 2) >> 4;
    AICA_REG8(0x2045) = CB(c, 3) >> 4;
    return 0;
}

/* 0x80-biased pan -> AICA 5-bit sign/magnitude pan (0x5904). */
static u32 pan_to_efpan(u32 v)
{
    s32 p = (s32)(v >> 3) - 0x10;

    if (p < 0)
        p = (p ^ 0xFF) + 0x10;
    return (u32)p & 0x1F;
}

/*
 * 0x58FC hcmd_36_gdda_set_pan - sdGddaSetPan (lib-only): [2]=l+0x80 [3]=r+0x80
 * Confidence: high
 * Byte writes of the AICA sign/magnitude pan to the low byte of
 * DSP_OUT(16)/(17) (EFPAN; bits 7:5 cleared). Result 0.
 */
static u32 hcmd_36_gdda_set_pan(volatile HostCmd *c)
{
    AICA_REG8(0x2040) = (u8)pan_to_efpan(CB(c, 2));
    AICA_REG8(0x2044) = (u8)pan_to_efpan(CB(c, 3));
    return 0;
}

/*
 * 0x5944 hcmd_3e_gdda_reset_prm - sdGddaResetPrm (lib-only): no parameters
 * Confidence: high
 * DSP_OUT(16) = DSP_OUT(17) = 0 (CD audio muted). Result 0.
 */
static u32 hcmd_3e_gdda_reset_prm(volatile HostCmd *c)
{
    AICA_DSP_OUT(16) = 0;
    AICA_DSP_OUT(17) = 0;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 0x8n: global                                                              */
/* ------------------------------------------------------------------------- */

/*
 * 0x5968 hcmd_80_snd_stop_all - sdSndStopAll: [2]=[3]=[4]=[5]=1 [6]=[7]=0
 * Confidence: high
 * Each nonzero byte enables one part:
 *   [2] MIDI: key off + free all 48 voices, seq_stop for ports 0..7
 *       (no PortState status sync, unlike 0x0F)
 *   [3] one-shots: shot_stop_all (as 0x1F)
 *   [4] streams: pstm_stop_all (as 0x2F)
 *   [5] GD-DA: DSP_OUT(16) = DSP_OUT(17) = 0 (as 0x3E)
 *   [6] FX program: fx_prg_clear (as 0x85)
 *   [7] DSP outputs: DSP_OUT(0..15) = 0 (EFSDL/EFPAN of all 16 EFREGs)
 * Result 0 (r0 = [7] = 0 or explicitly 0).
 */
static u32 hcmd_80_snd_stop_all(volatile HostCmd *c)
{
    u32 port, i;

    if (CB(c, 2)) {
        voices_kill(0, 0, 1);
        for (port = 0; port < NUM_PORTS; port++)
            seq_stop(port);
    }
    if (CB(c, 3))
        shot_stop_all();
    if (CB(c, 4))
        pstm_stop_all();
    if (CB(c, 5)) {
        AICA_DSP_OUT(16) = 0;
        AICA_DSP_OUT(17) = 0;
    }
    if (CB(c, 6))
        fx_prg_clear();
    if (!CB(c, 7))
        return 0;
    for (i = 0; i < 16; i++)
        AICA_DSP_OUT(i) = 0;
    return 0;
}

/*
 * 0x5A50 hcmd_81_snd_set_master_vol - sdSndSetMasterVol: [2]=(vol<<4)&0xFF
 * Confidence: high
 * master_vol_shadow = ([2] >> 4) & 0xF; in mono mode (host 0x8A) clamped
 * to 13 before the write (the shadow keeps the unclamped value).
 * Byte write to MVOL: MVOL = vol, VER bits 7:4 = 0, the MONO bit (bit 15,
 * other byte) is kept. Result 0.
 */
static u32 hcmd_81_snd_set_master_vol(volatile HostCmd *c)
{
    u32 vol = (CB(c, 2) >> 4) & 0xF;

    master_vol_shadow = (u8)vol;
    if ((mono_flag & 0xFF) && vol > 13)
        vol = 13;
    AICA_REG8(0x2800) = (u8)vol;
    return 0;
}

/*
 * Result of host 0x8F when host_cmd_reboot returns: it compares [4..7] with
 * 'S','E','G','A' using "ldrb r0; cmp; movne pc,lr", so r0 = the first byte
 * that does not match.
 */
static u32 hcmd_8f_result(volatile HostCmd *c)
{
    static const u8 sega[4] = { 'S', 'E', 'G', 'A' };
    u32 i;

    for (i = 0; i < 3; i++)
        if (CB(c, 4 + i) != sega[i])
            break;
    return CB(c, 4 + i);
}

/* ------------------------------------------------------------------------- */
/* Dispatcher                                                                */
/* ------------------------------------------------------------------------- */

/*
 * 0x4998 host_cmd_exec (code in r0, command record in r10; result in r0)
 * Confidence: high
 * 1. Logs the command: the 16-byte log entry at 0x14800 + cmd_log_ofs gets
 *    command bytes 0..11 and, as the 4th word, ticks_since_cmd (4 ms ticks
 *    since the previous command), which is then cleared. cmd_log_cur (code
 *    image word 0x4994) remembers the entry for host_cmd_poll. The offset
 *    advances by 16 modulo 0x800 (128-entry ring) and the next entry is
 *    filled with 0xFFFFFFFF as an end marker.
 * 2. Dispatches: codes 0x00-0x3F through the table at 0x4A44, 0x80-0x8F
 *    through 0x4B44 (index (code*4) & 0x1FF, checked < 0x40). Codes
 *    0x40-0x7F and 0x90-0xFF return immediately.
 * Result (low byte logged): see the cases. Unused table entries and
 * rejected codes return r0 = the table offset: code*4 (& 0x1FF for >= 0x80).
 */
u32 host_cmd_exec(u32 code, volatile HostCmd *cmd)
{
    u32 ofs = cmd_log_ofs, off;
    volatile u32 *e = (volatile u32 *)(0x14800u + ofs);
    volatile const u32 *src = (volatile const u32 *)cmd;

    cmd_log_cur = (volatile u8 *)e;
    e[0] = src[0];
    e[1] = src[1];
    e[2] = src[2];
    e[3] = ticks_since_cmd;
    ticks_since_cmd = 0;
    ofs = (ofs + 16) & 0x7FF;
    cmd_log_ofs = ofs;
    e = (volatile u32 *)(0x14800u + ofs);
    e[0] = e[1] = e[2] = e[3] = 0xFFFFFFFFu;

    code &= 0xFF;
    off = code << 2;
    if (off < 0x200) {
        if (off >= 0x100)
            return off;                             /* 0x40-0x7F */
        switch (code) {
        case 0x00: return off;                      /* empty/null (sdSetNullHostCmd): never reached from poll */
        case 0x01: return hcmd_01_midi_play(cmd);
        case 0x02: return hcmd_02_midi_stop(cmd);
        case 0x03: return hcmd_03_midi_pause(cmd);
        case 0x04: return hcmd_04_midi_continue(cmd);
        case 0x05: return hcmd_05_midi_set_vol(cmd);
        case 0x06: return hcmd_06_midi_set_pan(cmd);
        case 0x07: return hcmd_07_midi_set_speed(cmd);
        case 0x08: return hcmd_08_midi_set_pitch(cmd);
        case 0x09: return off;                      /* MIDI FxCh: unused (mov pc,lr at 0x4B98) */
        case 0x0A: return hcmd_0a_midi_set_fx_lev(cmd);
        case 0x0B: return hcmd_0b_midi_set_drct_lev(cmd);
        case 0x0C: return hcmd_0c_midi_break_loop(cmd);
        case 0x0D: return off;                      /* unused */
        case 0x0E: return hcmd_0e_midi_reset_prm(cmd);
        case 0x0F: return hcmd_0f_midi_stop_all(cmd);
        case 0x10: return off;                      /* unused */
        case 0x11: return hcmd_11_shot_play(cmd);
        case 0x12: return hcmd_12_shot_stop(cmd);
        case 0x13: return hcmd_13_shot_pause(cmd);
        case 0x14: return hcmd_14_shot_continue(cmd);
        case 0x15: return hcmd_15_shot_set_vol(cmd);
        case 0x16: return hcmd_16_shot_set_pan(cmd);
        case 0x17: return hcmd_17_shot_set_speed(cmd);
        case 0x18: return hcmd_18_shot_set_pitch(cmd);
        case 0x19: return hcmd_19_shot_set_fx_ch(cmd);
        case 0x1A: return hcmd_1a_shot_set_fx_lev(cmd);
        case 0x1B: return hcmd_1b_shot_set_drct_lev(cmd);
        case 0x1C: return hcmd_1c_shot_set_loop_mode(cmd);
        case 0x1D: return off;                      /* unused */
        case 0x1E: return hcmd_1e_shot_reset_prm(cmd);
        case 0x1F: return hcmd_1f_shot_stop_all(cmd);
        case 0x20: return off;                      /* unused */
        case 0x21: return hcmd_21_pstm_play(cmd);
        case 0x22: return hcmd_22_pstm_stop(cmd);
        case 0x23: return off;                      /* PSTM pause: unused */
        case 0x24: return off;                      /* PSTM continue: unused */
        case 0x25: return hcmd_25_pstm_set_vol(cmd);
        case 0x26: return hcmd_26_pstm_set_pan(cmd);
        case 0x27: return hcmd_27_pstm_set_speed(cmd);
        case 0x28: return hcmd_28_pstm_set_pitch(cmd);
        case 0x29: return hcmd_29_pstm_set_fx_ch(cmd);
        case 0x2A: return hcmd_2a_pstm_set_fx_lev(cmd);
        case 0x2B: return hcmd_2b_pstm_set_drct_lev(cmd);
        case 0x2C: return off;                      /* unused */
        case 0x2D: return off;                      /* unused */
        case 0x2E: return hcmd_2e_pstm_reset_prm(cmd);
        case 0x2F: return hcmd_2f_pstm_stop_all(cmd);
        case 0x35: return hcmd_35_gdda_set_vol(cmd);
        case 0x36: return hcmd_36_gdda_set_pan(cmd);
        case 0x3E: return hcmd_3e_gdda_reset_prm(cmd);
        default:   return off;                      /* 0x30-0x34, 0x37-0x3D, 0x3F: unused */
        }
    }
    off &= 0x1FF;
    if (off >= 0x40)
        return off;                                 /* 0x90-0xFF */
    switch (code) {
    case 0x80: return hcmd_80_snd_stop_all(cmd);
    case 0x81: return hcmd_81_snd_set_master_vol(cmd);
    case 0x82: return fx_cmd_set_out(cmd);          /* sdSndSetFxOut / 2nd part of sdSndSetFxPrg */
    case 0x83: return fx_cmd_set_out_prm(cmd);      /* sdSndSetFxOutPrm */
    case 0x84: return fx_prg_select(CB(cmd, 2));    /* sdSndSetFxPrg: 0x5C94 loads [2], falls into 0x5C98 */
    case 0x85: return fx_prg_clear();               /* sdSndClearFxPrg */
    case 0x86: return host_cmd_push_event(cmd);     /* sdMidiSendMes: raw event word [4..7] */
    case 0x87: return host_cmd_drum_mode(cmd);      /* no library API */
    case 0x88: return fx_cmd_set_dsp_pan(cmd);      /* sdQsndSetPos */
    case 0x89: host_cmd_nop89(); return off;        /* 0x6040 mov pc,lr: r0 = 0x24 */
    case 0x8A: return host_cmd_mono(cmd);           /* sdSndSetSpace */
    case 0x8E: return host_cmd_reinit();            /* no library API: soft reset */
    case 0x8F: host_cmd_reboot(cmd);                /* no library API: "SEGA" -> reset + hang */
               return hcmd_8f_result(cmd);          /* only reached if [4..7] != "SEGA" */
    default:   return off;                          /* 0x8B-0x8D: unused */
    }
}

/*
 * 0x4948 host_cmd_poll
 * Confidence: high
 * Called from the main loop when host_cmd_pending (0x13400) is set. Scans
 * all 32 slots at 0x13200: a slot with a nonzero code byte is executed by
 * host_cmd_exec(code, slot) (r0, r10). For EVERY slot the low byte of r0 is
 * then stored to byte 1 of the log entry cmd_log_cur, and the slot's code
 * byte is cleared (bytes 1..15 are left as they are). Finally
 * host_cmd_pending = 0.
 * Notes: for an empty slot r0 is 0 (the code just tested), so every empty
 * slot after the last command overwrites that command's logged result with
 * 0 - in a normal batch (< 32 commands) the last result is always lost.
 * Before the first command ever, cmd_log_cur is 0 and such a write would hit
 * byte 1 of the reset vector. After host 0x8E the entry pointed to has
 * been cleared by clear_host_area (still written).
 */
void host_cmd_poll(void)
{
    volatile HostCmd *slot = g_host_cmd;
    u32 n, r;

    for (n = NUM_HOST_CMD_SLOTS; n != 0; n--, slot++) {
        r = slot->code;
        if (r != 0)
            r = host_cmd_exec(r, slot);
        cmd_log_cur[1] = (u8)r;
        slot->code = 0;
    }
    host_cmd_pending = 0;
}
