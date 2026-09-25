/*
 * event.c - the MIDI event ring (0xB000) consumer side.
 *
 * Every producer (external MIDI-in, the sequencer, host commands) packs a MIDI
 * message into a 32-bit event (layout in manatee.h) and appends it to the ring.
 * The main loop drains up to four events per pass.
 */
#include "manatee.h"

/*
 * 0x1564 midi_event_pop
 * Confidence: high
 * Returns the next event from the ring, or 0 if the ring is empty.
 * Notes: the original returns the event in r1 (r0 = 0xB000 [+ old read
 * offset], r2-r4 clobbered; unused by the caller). Offsets are byte offsets
 * masked with 0xFFF, i.e. 1024 entries. A stored event of 0 is
 * indistinguishable from "empty" (it is consumed and ignored). Only the
 * sequencer (seq_evq_push) checks for a full ring; midi_event_push_ext and the
 * host push do not, and a producer that laps the reader makes the ring look
 * empty, losing 1024 events.
 */
u32 midi_event_pop(void)
{
    u32 rd = evq_read_ofs;
    u32 ev;

    if (evq_write_ofs == rd)
        return 0;
    ev = *(volatile u32 *)(0xB000u + rd);
    evq_read_ofs = (rd + 4) & EVQ_MASK;
    return ev;
}

/*
 * 0x09A8 voice_service_x4
 * Confidence: high
 * Called on every main-loop pass (not tick driven): executes up to four queued
 * events. midi_event_dispatch ignores the 0 returned for an empty ring.
 */
void voice_service_x4(void)
{
    midi_event_dispatch(midi_event_pop());
    midi_event_dispatch(midi_event_pop());
    midi_event_dispatch(midi_event_pop());
    midi_event_dispatch(midi_event_pop());
}

/*
 * 0x09D4 midi_event_dispatch
 * Confidence: high
 * Executes one packed event (0 = ring empty, ignored). Decodes type, port,
 * channel and flags and jumps through the 8-entry branch table at 0xA1C.
 * Notes: the asm precomputes r5 = ch*0x48, r6 = 0xC400 + port*0x480 (so
 * r5+r6 = MIDI_CH(port, ch)), r4 = flags (ev>>27, used as the note-on voice
 * priority) and leaves r1 = ev, r2 = ch, r3 = port for the handlers; here every
 * handler re-derives what it needs from ev. Types 2 (poly pressure),
 * 5 (channel pressure) and 7 (system) are bare `mov pc,lr` in this driver.
 */
void midi_event_dispatch(u32 ev)
{
    if (ev == 0)
        return;
    switch (EV_TYPE(ev)) {
    case EV_NOTE_OFF:   note_off(EV_PORT(ev), EV_CH(ev), EV_DATA1(ev) & 0x7F); break; /* 0xA3C */
    case EV_NOTE_ON:    note_on(ev);             break; /* 0xAD8 */
    case EV_POLY_PRES:  ev_poly_pressure(ev);    break; /* 0x1368 */
    case EV_CONTROL:    ev_control_change(ev);   break; /* 0x185C */
    case EV_PROGRAM:    ev_program_change(ev);   break; /* 0x136C */
    case EV_CH_PRES:    ev_channel_pressure(ev); break; /* 0x13EC */
    case EV_PITCH_BEND: ev_pitch_bend(ev);       break; /* 0x13F0 */
    case EV_SYSTEM:     ev_system(ev);           break; /* 0x14B8 */
    }
}
