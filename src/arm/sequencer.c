/*
 * sequencer.c - MIDI sequence players (8 ports, 0x10000) and the timed note-off
 * queue (0x10300). The data format is described in docs/notes/sequence_format.md.
 *
 * Every 4 ms seq_tick first ages the pending note-offs of sequenced notes, then
 * lets each playing port parse as many events as are due. Parsed MIDI messages
 * are packed into the common 32-bit event format (manatee.h) and appended to the
 * event ring at 0xB000, from which midi_event_dispatch executes them. The port's
 * priority (host play command, prio<<3) goes into the event flag bits 31:27.
 *
 * Time base: a player keeps "delta" in units where 0x4000 elapses per 4 ms at
 * normal speed (tempo_scale, set by port_speed_fade from the port speed). One
 * sequence tick is tick_len = timebase * beat_len >> 4 of those units, with
 * timebase = 65536/ticks-per-beat and beat_len = ms per beat.
 *
 * The event ring has no common "push" helper; the sequencer uses its own
 * (seq_evq_push, 0x4900), which - unlike the other producers - detects a full
 * ring, but with an unmasked comparison (see there).
 */
#include "manatee.h"

extern const u32 seq_len_mul_a[4];  /* 0x47A4: {0x200,0x800,0x1000,0x2000} meta 0x88-0x8B */
extern const u32 seq_len_mul_b[4];  /* 0x47B4: {0x100,0x200,0x800,0x1000}  meta 0x8C-0x8F */

#define EV_PORT4(ev) (((ev) >> 20) & 0xF)   /* the asm compares 4 bits (0xF00000) */
#define SEQ_NOTE_END (g_seq_note + NUM_SEQ_NOTES)
#define SEQ_OVF_END  ((u32 *)0x10300u)     /* == &g_seq.ovf[SEQ_OVF_LEN] == g_seq_note */

/*
 * 0x4900 seq_evq_push
 * Confidence: high
 * Appends ev to the event ring unless it is full; returns 1 if stored, 0 if full.
 * Notes: asm interface r5 = ev, r6 = write offset, r4 = read offset - 4, r7 =
 * 0xB000, r8 = 0x13000; returns to lr on "full" and to lr+4 on success (the
 * callers put the full-handler branch right after the bl); clobbers r5 (mask)
 * and updates r6. The callers load r4 once per note-off scan (seq_tick phase 1)
 * or once per emitted event, and r6 once per scan (phase 1) or before every
 * push (emitter); here both offsets are re-read per push, which only differs if
 * an interrupt moved them in between. QUIRK: "full" is write == read-4 WITHOUT masking,
 * so when the read offset is 0 the ring is never seen as full and the 1024th
 * push makes write == read, i.e. the whole ring reads as empty.
 */
static int seq_evq_push(u32 ev)
{
    u32 wr = evq_write_ofs;

    if (evq_read_ofs - 4 == wr)
        return 0;
    *(volatile u32 *)(0xB000u + wr) = ev;
    evq_write_ofs = (wr + 4) & EVQ_MASK;
    return 1;
}

/*
 * 0x48D4 seq_add_delta
 * Confidence: high
 * Converts a delta time d (sequence ticks) to scaled units, adds the pending
 * meta 0x8C-0x8F extension and the (<= 0) remainder of the current delta, stores
 * the result as the new delta and saves the parse pointer. Returns the new delta.
 * Notes: asm r3 = d in/out, r11 = player, r12 = ptr. The "mlas ... mvnvs r3,#0"
 * saturation is dead code: MLAS does not change V on ARMv4 and V is always clear
 * at the call sites (last flag setter is a cmp with 0), so the product wraps.
 */
static s32 seq_add_delta(SeqPlayer *p, u32 d, const u8 *ptr)
{
    s32 t = (s32)(d * p->tick_len + p->delta_extra);

    p->delta_extra = 0;
    t += p->delta;
    p->delta = t;
    p->cur = ptr;
    return t;
}

/*
 * 0x4378-0x48D0 (body of seq_tick) seq_step
 * Confidence: high
 * Runs one playing sequence player for one 4 ms tick: advances the beat counter,
 * counts the delta down and, when it expires, parses and emits events until the
 * next non-zero delta. Returns 1 if seq_tick must return immediately (event ring
 * full or overflow queue full), else 0.
 * Notes: see docs/notes/sequence_format.md for the byte format. Every path
 * (both jump tables 0x45CC / 0x4650, all 16 meta and 7 channel entries) was
 * checked against the disassembly. Quirks kept:
 *  - the delta countdown test is UNSIGNED (subs/bhi at 0x43B8): a negative
 *    stored delta counts as huge, so the player sleeps until it wraps
 *    (~2^32/tempo_scale ticks, ~17.5 min at 1x). Normal parsing never stores
 *    a delta <= 0, but the ring-full exit calls seq_add_delta unconditionally,
 *    e.g. on an event with delta 0 inside a chord, which stores the (usually
 *    negative) remainder.
 *  - a raw 0x9n / 0xFn status byte is skipped as a 1-byte no-op (notes use the
 *    compact <0x80 form); 0x80 and 0x85-0x87 are 1-byte no-ops too.
 *  - a zero delta does not consume delta_extra (meta 0x8C-0x8F); it waits for the
 *    next non-zero delta.
 *  - the "mvnvs" saturation after each MLAS is dead (see seq_add_delta).
 *  - note-off queue full: the victim is the LAST entry with the lowest priority
 *    (ages are tracked but never influence the choice, the "oldest" test only
 *    updates the best age); a priority-0 entry is taken at once.
 *  - if the overflow queue is also full at that point the asm returns with an
 *    unbalanced stack: 0x4438 pops the port number pushed at 0x4508 into lr
 *    and does mov pc,lr, i.e. jumps to address 0..7 (ARM7 ignores pc bits 1:0:
 *    ports 0-3 -> reset vector, 4-7 -> undefined-instruction vector), with the
 *    real return address left on the stack. Here it just stalls and returns.
 *  - ring full while emitting and the overflow queue empty: the new event is
 *    DROPPED (only events already waiting in the overflow queue are preserved,
 *    and the new one is appended only if the drain was interrupted).
 *  - overflow pointers wrap with ">" (not ">=") 0x10300 when emitting, so the
 *    pointer may reach 0x10300 = g_seq_note[0].ev and treat it as a queue entry
 *    (only the "steal" path wraps with ">=").
 *  - if the ring fills while draining, the read pointer is not saved; the drained
 *    entries are already zeroed, so the next drain stops at the first zero and
 *    the entries behind it are stranded until the writer wraps onto them, which
 *    then triggers the MIDI_SEQ_BUF stall.
 *  - ring full after a note: its note-off is already queued in g_seq_note
 *    although the note-on may have been dropped (harmless extra note-off).
 *  - a meta 0x81 call counts every event that reaches the delta stage
 *    (notes, channel messages, 0x82, 0x84 - also one dropped on a full ring);
 *    call_count 0 never returns.
 */
static int seq_step(SeqPlayer *p, u32 port)
{
    const u8 *ptr = p->cur;
    u32 scale = p->tempo_scale;
    u32 b, d, c, ev, hi, n, gate;
    s32 t;
    SeqNote *s;

    /* beat counter (TotalBeatTime): beat_count -= scale>>12 (4 at 1x) per 4 ms */
    b = p->beat_count;
    if (b > (scale >> 12)) {
        b -= scale >> 12;
    } else {
        u32 beats = p->beats;
        b -= scale >> 12;
        do {
            beats++;
            b += p->beat_len;
        } while ((s32)b < 0);
        p->beats = beats;
    }
    p->beat_count = b;

    d = (u32)p->delta;
    p->delta = (s32)(d - scale);
    if (d > scale)                      /* unsigned (bhi) */
        return 0;

next:                                   /* 0x43C4: count an event for meta 0x81 */
    if (p->call_count != 0) {
        if (p->call_count == 1)
            ptr = p->call_ret;
        p->call_count--;
    }
parse:                                  /* 0x43E0 */
    if (ptr >= p->end)
        goto end_seq;
    p->cur = ptr;
    c = *ptr++;

    if (!(c & 0x80)) {
        /* 0x444C: note  0 ll w cccc  note vel gate[ll+1] delta[1+w] */
        hi = ((u32)(p->prio | EV_NOTE_ON) << 8) | (port << 4) | (c & 0x0F);
        n = *ptr++;
        ev = (hi << 16) | (n << 8) | *ptr++;
        gate = 0;
        for (n = c >> 5; ; n--) {
            gate |= *ptr++;
            if (n == 0)
                break;
            gate <<= 8;
        }
        t = (s32)(gate * p->tick_len + p->gate_extra) + p->delta;
        p->gate_extra = 0;
        d = *ptr++;
        if (c & 0x10)
            d = (d << 8) | *ptr++;

        /* queue the matching note-off (type 0) */
        for (s = g_seq_note; s < SEQ_NOTE_END; s++)
            if (s->ev == 0)
                goto have_slot;
        g_seq.err |= PORT_ERR_SLOT;
        {
            SeqNote *victim = g_seq_note, *q;
            u32 best = 0x1F, best_age = 0, pr;
            u32 *wp;

            for (q = g_seq_note; q < SEQ_NOTE_END; q++) {
                pr = q->ev >> 27;
                if (pr == 0) {
                    victim = q;
                    break;
                }
                if (pr == best) {
                    if (q->age > best_age)
                        best_age = q->age;
                    victim = q;          /* unconditional in the asm */
                } else if (pr < best) {
                    victim = q;
                    best = pr;
                    best_age = q->age;
                }
            }
            wp = g_seq.ovf_wr;
            if (wp >= SEQ_OVF_END)
                wp = g_seq.ovf;
            if (*wp != 0) {
                /* 0x48C4 with r10 still pushed: see the stack-imbalance quirk */
                g_seq.stalled = 1;
                g_seq.err |= PORT_ERR_MIDI_BUF | PORT_ERR_MIDI_SEQ_BUF;
                return 1;
            }
            *wp++ = victim->ev & ~SEQ_NOTE_RETRIGGER;   /* its note-off goes out first */
            g_seq.ovf_wr = wp;
            s = victim;
        }
have_slot:
        s->ev = ev & ~SEQ_NOTE_RETRIGGER;               /* note-on -> note-off */
        s->time = t;
        s->age = 1;
        goto emit;
    }

    if (c < 0x90) {
        /* 0x4638: meta events, jump table at 0x4650 */
        switch (c) {
        case 0x81:                                      /* 0x4690 call: hi lo n */
            n = (u32)ptr[0] << 8 | ptr[1];
            p->call_ret = ptr + 3;
            p->call_count = ptr[2];
            ptr = p->base + n;
            goto parse;
        case 0x82:                                      /* 0x46B4 loop start/end: cnt delta[1+(cnt>>7)] */
            if (p->loop_start == 0) {
                p->loop_start = ptr;
                p->loop_iter = 0;
                c = *ptr++;
                n = c & 0x7F;
                p->loop_left = n ? n : 0x80;            /* 0 = forever */
            } else if (p->loop_left == 0) {
                p->loop_start = 0;                      /* loop done, fall through */
                c = *ptr++;
            } else {
                ptr = p->loop_start;                    /* jump back, re-read the start operand */
                c = *ptr++;
                p->loop_iter++;
                if (p->loop_left != 0x80)
                    p->loop_left--;
            }
            d = *ptr++;
            if (c & 0x80)
                d = (d << 8) | *ptr++;
            goto after_emit;
        case 0x83:                                      /* 0x47C4 end of sequence */
            goto end_seq;
        case 0x84:                                      /* 0x472C tempo: BE16 ms/beat, delta[1] */
            n = (u32)ptr[0] << 8 | ptr[1];
            ptr += 2;
            p->beat_len = n;
            p->tick_len = p->timebase * n >> 4;
            d = *ptr++;
            goto after_emit;
        case 0x88: case 0x89: case 0x8A: case 0x8B:     /* 0x4754 long gate prefix */
            p->gate_extra += p->tick_len * seq_len_mul_a[c & 3];
            goto parse;
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:     /* 0x477C long delta prefix */
            p->delta_extra += p->tick_len * seq_len_mul_b[c & 3];
            goto parse;
        default:                                        /* 0x80, 0x85-0x87: no-op */
            goto parse;
        }
    }

    /* 0x45A8: channel messages, jump table at 0x45CC indexed by (c-0x90)>>4 */
    hi = ((u32)(p->prio | ((c >> 4) & 7)) << 8) | (port << 4) | (c & 0x0F);
    switch ((c - 0x90) >> 4) {
    case 1:                                             /* 0xAn poly pressure */
    case 2:                                             /* 0xBn control change */
        n = *ptr++;                                     /* bit7 = 2-byte delta */
        ev = (hi << 16) | ((n & 0x7F) << 8) | *ptr++;
        d = *ptr++;
        if (n & 0x80)
            d = (d << 8) | *ptr++;
        break;
    case 3:                                             /* 0xCn program change */
    case 4:                                             /* 0xDn channel pressure */
        n = *ptr++;
        ev = (hi << 16) | ((n & 0x7F) << 8);
        d = *ptr++;
        if (n & 0x80)
            d = (d << 8) | *ptr++;
        break;
    case 5:                                             /* 0xEn pitch bend: one byte -> data2 */
        ev = (hi << 16) | *ptr++;
        d = *ptr++;
        break;
    default:                                            /* 0x9n, 0xFn: no-op */
        goto parse;
    }

emit:                                                   /* 0x4814 */
    {
        u32 *rp = g_seq.ovf_rd;
        u32 e;

        for (;;) {
            if (rp > SEQ_OVF_END)                       /* QUIRK: '>' */
                rp = g_seq.ovf;
            e = *rp;
            if (e == 0) {                               /* queue drained: now the new event */
                g_seq.ovf_rd = rp;
                e = ev;
                ev = 0;
            }
            if (!seq_evq_push(e))
                goto ring_full;
            if (ev == 0)
                break;
            *rp++ = 0;
        }
    }
after_emit:                                             /* 0x4874 */
    if ((s32)d <= 0)
        goto next;
    if (seq_add_delta(p, d, ptr) <= 0)
        goto next;
    return 0;

ring_full:                                              /* 0x488C */
    {
        u32 *wp = g_seq.ovf_wr;

        if (wp > SEQ_OVF_END)                           /* QUIRK: '>' */
            wp = g_seq.ovf;
        if (*wp != 0) {                                 /* 0x48C4 */
            g_seq.stalled = 1;
            g_seq.err |= PORT_ERR_MIDI_BUF | PORT_ERR_MIDI_SEQ_BUF;
            return 1;
        }
        if (ev != 0)
            *wp++ = ev;
        g_seq.ovf_wr = wp;
    }
    seq_add_delta(p, d, ptr);
    g_seq.resume_port = (u8)port;
    g_seq.resume = p;
    g_seq.err |= PORT_ERR_MIDI_BUF;
    return 1;

end_seq:                                                /* 0x47C4 */
    *(u32 *)&p->prio = 0;               /* prio, state, call_count, loop_left */
    p->end = 0;
    p->timebase = 0;
    p->tick_len = 0;
    p->loop_left = 0;
    p->loop_iter = 0;
    p->call_ret = 0;
    p->loop_start = 0;
    p->delta = 0;
    p->delta_extra = 0;
    p->gate_extra = 0;
    p->beats = 0;
    p->beat_len = 0;
    p->beat_count = 0;
    port_clear_play(port);
    return 0;
}

/*
 * 0x4270 seq_tick
 * Confidence: high
 * Every 4 ms (main loop). Does nothing while g_seq.stalled == 1.
 * 1) Pending note-offs (48 x SeqNote at 0x10300) of non-paused ports: time -=
 *    tempo_scale of the port; when it expires the note-off is pushed to the ring
 *    and the entry freed. Entries marked SEQ_NOTE_RETRIGGER (by seq_continue when
 *    the ring was full) push their note-on (the ev as stored) first and then
 *    become plain note-offs; if they expire before that they are just dropped.
 * 2) Sequence players 0..7 whose state is exactly SEQ_PLAYING (paused players
 *    are skipped), starting at g_seq.resume_port (set when the previous tick
 *    aborted on a full ring), see seq_step.
 * Notes: ring full in phase 1 sets PORT_ERR_MIDI_BUF and returns at once (no
 * sequencer step this tick). For a retrigger entry the entry is cleared first
 * (its note-on and its gate are lost); an expired entry is kept for the next
 * tick. The code after 0x4350 compares the entry pointer against the loop
 * counter (r1) instead of an end address, so the intended scan that clears the
 * remaining retrigger entries (0x4360) is unreachable.
 * After a ring-full abort in phase 2 the next tick starts at resume_port: the
 * ports before it are not run in that tick (they lose 4 ms against the others).
 */
void seq_tick(void)
{
    SeqNote *s;
    SeqPlayer *p;
    u32 port;

    if (g_seq.stalled == 1)
        return;

    for (s = g_seq_note; s < SEQ_NOTE_END; s++) {
        u32 ev = s->ev;
        s32 t;

        if (ev == 0)
            continue;
        p = &g_seq_player[EV_PORT4(ev)];
        if (p->state & SEQ_PAUSED)
            continue;
        t = s->time - (s32)p->tempo_scale;
        if (s->time > (s32)p->tempo_scale) {   /* subs + ble: signed compare */
            if (ev & SEQ_NOTE_RETRIGGER) {
                if (!seq_evq_push(ev)) {
                    s->ev = 0;
                    s->time = 0;
                    s->age = 0;
                    goto ring_full;
                }
                s->ev &= ~SEQ_NOTE_RETRIGGER;
            }
            s->time = t;
            s->age++;
        } else {
            if (!(ev & SEQ_NOTE_RETRIGGER) && !seq_evq_push(ev))
                goto ring_full;
            s->ev = 0;
            s->time = 0;
            s->age = 0;
        }
    }

    port = g_seq.resume_port;
    p = g_seq.resume;
    for (;;) {
        if (p->state == SEQ_PLAYING && seq_step(p, port))
            return;
        if (port == 7)
            break;
        port++;
        p++;
    }
    g_seq.resume_port = 0;
    g_seq.resume = g_seq_player;
    return;

ring_full:
    g_seq.err |= PORT_ERR_MIDI_BUF;
}

/*
 * 0x742C seq_init
 * Confidence: high
 * Clears the whole sequencer area 0x10000-0x107FF (players, globals, overflow
 * queue, note-off queue), sets every player's tempo_scale to 0x4000 (1x), points
 * both overflow queue pointers at 0x10210 and the resume pointer at player 0.
 * Called at boot and by host_cmd_exec (0x60BC, 0x613C).
 */
void seq_init(void)
{
    u32 *w = (u32 *)0x10000u;
    int i;

    for (i = 0; i < 0x800 / 4; i++)
        w[i] = 0;
    for (i = 0; i < NUM_PORTS; i++)
        g_seq_player[i].tempo_scale = 0x4000;
    g_seq.ovf_wr = g_seq.ovf;
    g_seq.ovf_rd = g_seq.ovf;
    g_seq.resume = g_seq_player;
}

/*
 * 0x7618 (tail shared by seq_stop_player and seq_pause) seq_purge_port
 * Confidence: high
 * Zeroes every note-on event of this port still waiting in the event ring (in
 * place; a zero event is ignored by the dispatcher) and keys off all voices of
 * the port (voices_key_off_port, 0x17F4). Returns p->state.
 * Notes: the scan is a do-while from the read to the write offset, so with an
 * empty ring it walks all 1024 slots (harmless: only stale entries are hit).
 */
static u32 seq_purge_port(SeqPlayer *p, u32 port)
{
    u32 wr = evq_write_ofs;
    u32 rd = evq_read_ofs;

    do {
        u32 e = EVQ_RING[rd / 4];
        if ((e & 0x07000000) == 0x01000000 && EV_PORT4(e) == port)
            EVQ_RING[rd / 4] = 0;
        rd = (rd + 4) & EVQ_MASK;
    } while (rd != wr);
    voices_key_off_port(port);
    return p->state;
}

/*
 * 0x747C seq_play
 * Confidence: high
 * Host command 0x01 (sdMidiPlay): start sequence 'song' of the SMSB bank at
 * 'smsb' on 'port' with priority prio (= prio<<3 from the host; low 3 bits are
 * cleared). Returns the new player state (1).
 * If the port is playing, a new request with lower non-zero priority than a
 * non-zero current priority fails with PORT_ERR_PRIORITY; otherwise the current
 * sequence is stopped (seq_stop_player).
 * The song's end is the next song's offset, or - for the last song, whose
 * "next offset" slot is the "SMSD" tag of the first song - the bank's ENDB.
 * Notes: asm r0 = port, r1 = song, r2 = prio, r12 = bank; returns r1. The
 * bank magic/version/ENDB are checked by the caller (0x4B9C).
 * QUIRK: on PORT_ERR_REQUEST_NUM (song >= count) the asm returns the byte at
 * r11+1 before r11 was set up; in host_cmd_exec r11 is host_cmd_poll's slot
 * counter (0x20..1), so the "state" is a byte of the vector table / version
 * (e.g. slot 0 -> byte 0x21 = 0x01 -> the port is reported as playing). Here
 * the port's real state is returned.
 * Delta, loop and call state are not reset: they are expected to be clear
 * (seq_init, end of sequence or seq_stop_player).
 */
u32 seq_play(u32 port, u32 song, u32 prio, const u8 *smsb)
{
    SeqPlayer *p = &g_seq_player[port & 7];
    const u32 *ofs;
    const u8 *sd;
    u32 next;

    if (song >= ((const u32 *)smsb)[3]) {
        g_seq.err |= PORT_ERR_REQUEST_NUM;
        return p->state;                    /* see QUIRK */
    }
    port &= 7;
    prio &= ~7u;
    if (p->state & SEQ_PLAYING) {           /* asm: unaligned ldr [p+1] then tst #1 */
        if (prio != 0 && p->prio != 0 && prio < p->prio) {
            g_seq.err |= PORT_ERR_PRIORITY;
            return p->state;
        }
        seq_stop_player(p, port);
    }
    p->prio = (u8)prio;
    ofs = (const u32 *)(smsb + 0x10) + song;
    next = ofs[1];
    if (next == SMSD_MAGIC)
        next = ((const u32 *)smsb)[2] - 4;
    p->end = smsb + next;
    sd = smsb + ofs[0];
    p->base = sd;
    p->timebase = ((const u32 *)sd)[1];
    p->beat_len = ((const u32 *)sd)[2];
    p->beat_count = p->beat_len;
    p->tick_len = p->timebase * p->beat_len >> 4;
    p->beats = 0;
    p->cur = sd + 12;
    p->state = SEQ_PLAYING;
    return 1;
}

/*
 * 0x7564 seq_stop
 * Confidence: high
 * Host command 0x02 (sdMidiStop), also used by 0x519C and 0x59B8: selects the
 * player of 'port' and falls into seq_stop_player.
 */
u32 seq_stop(u32 port)
{
    port &= 7;
    return seq_stop_player(&g_seq_player[port], port);
}

/*
 * 0x7574 seq_stop_player
 * Confidence: high
 * Stops player p (of 'port'): clears the playing bit and the playback fields,
 * frees this port's pending note-offs WITHOUT sending them, then purges its
 * queued note-ons from the ring and keys off its voices (seq_purge_port).
 * Returns p->state (the paused bit survives).
 * Notes: asm r11 = p, r0 = port. Not cleared: cur/base (still exported),
 * loop_iter, beats, beat_count, tempo_scale.
 */
u32 seq_stop_player(SeqPlayer *p, u32 port)
{
    SeqNote *s;

    p->state &= ~SEQ_PLAYING;
    p->prio = 0;
    p->call_count = 0;
    p->end = 0;
    p->timebase = 0;
    p->tick_len = 0;
    p->loop_left = 0;
    p->call_ret = 0;
    p->loop_start = 0;
    p->delta = 0;
    p->delta_extra = 0;
    p->gate_extra = 0;
    p->beat_len = 0;
    for (s = g_seq_note; s < SEQ_NOTE_END; s++) {
        u32 ev = s->ev;
        if (ev != 0 && EV_PORT4(ev) == port) {
            s->ev = 0;
            s->age = 0;
            s->time = 0;
        }
    }
    return seq_purge_port(p, port);
}

/*
 * 0x75F4 seq_pause
 * Confidence: high
 * Host command 0x03 (sdMidiPause): sets SEQ_PAUSED (even if not playing). If it
 * was playing: purges the port's note-ons from the ring and keys off its voices.
 * The pending note-offs stay queued with their timers frozen (seq_tick skips
 * paused ports) so seq_continue can re-trigger them. Returns the new state.
 */
u32 seq_pause(u32 port)
{
    SeqPlayer *p;
    u8 old;

    port &= 7;
    p = &g_seq_player[port];
    old = p->state;
    p->state = old | SEQ_PAUSED;
    if (!(old & SEQ_PLAYING))
        return p->state;
    return seq_purge_port(p, port);
}

/*
 * 0x7674 seq_continue
 * Confidence: high
 * Host command 0x04 (sdMidiContinue): if paused, clears SEQ_PAUSED; if also
 * playing, re-sends a note-on (the pending note-off with bit 24 set, i.e. type
 * 1, original velocity) for every note of this port still waiting in the
 * note-off queue, so held notes sound again. Returns the new state.
 * If the ring is (or becomes) full, this and all remaining entries of the port
 * get SEQ_NOTE_RETRIGGER set in the queue (seq_tick sends them later) and
 * PORT_ERR_MIDI_BUF is raised.
 * Notes: QUIRK: the ring write offset is only published when every note-on
 * fitted (0x76CC); when the ring fills part-way the note-ons already written
 * are discarded, and their notes are not marked for retrigger. Same unmasked
 * full test as seq_evq_push.
 */
u32 seq_continue(u32 port)
{
    SeqPlayer *p;
    SeqNote *s;
    u32 st, wr, full;

    port &= 7;
    p = &g_seq_player[port];
    st = p->state;
    if (!(st & SEQ_PAUSED))
        return st;
    st &= ~SEQ_PAUSED;
    p->state = (u8)st;
    if (!(st & SEQ_PLAYING))
        return st;

    wr = evq_write_ofs;
    full = evq_read_ofs - 4;
    s = g_seq_note;
    if (full != wr) {
        for (; s < SEQ_NOTE_END; s++) {
            u32 ev = s->ev;
            if (ev == 0 || EV_PORT4(ev) != port)
                continue;
            ev |= SEQ_NOTE_RETRIGGER;
            if (full == wr) {
                s->ev = ev;
                goto mark_rest;
            }
            EVQ_RING[wr / 4] = ev;
            wr = (wr + 4) & EVQ_MASK;
        }
        evq_write_ofs = wr;
        return p->state;
    }
mark_rest:
    for (; s < SEQ_NOTE_END; s++) {
        u32 ev = s->ev;
        if (ev != 0 && EV_PORT4(ev) == port)
            s->ev = ev | SEQ_NOTE_RETRIGGER;
    }
    g_seq.err |= PORT_ERR_MIDI_BUF;
    return p->state;
}

/*
 * 0x7744 seq_break_loop
 * Confidence: high
 * Host command 0x0C (not emitted by sd 1.00.18): sets loop_left = 0 so the
 * current meta-0x82 loop is left at its next end marker (e.g. to play a song's
 * ending). Returns nothing (r1 is left = 0).
 */
void seq_break_loop(u32 port)
{
    g_seq_player[port & 7].loop_left = 0;
}
