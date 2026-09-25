/*
 * midi_in.c - external MIDI input (AICA MIDI-in FIFO -> event ring, port 7).
 *
 * On a retail Dreamcast nothing is attached to the AICA MIDI input; this path
 * exists for development hardware (sound authoring tools driving the driver
 * over MIDI). It is still fully wired up: the MIDI-in interrupt is enabled at
 * FIQ level 5 by irq_init().
 */
#include "manatee.h"

/*
 * Parser state. In the original these are words/bytes embedded in the code
 * image (0x1620-0x162B) and the state is the *address* of the code block that
 * consumes the next data byte (0 = waiting for a status byte).
 */
enum MidiInState {
    MIDI_IN_IDLE = 0,       /* 0x0000: discard data bytes */
    MIDI_IN_3B_DATA1 = 1,   /* 0x1668: first data byte of a 3-byte message */
    MIDI_IN_3B_DATA2 = 2,   /* 0x1678: second data byte -> emit */
    MIDI_IN_2B_DATA = 3,    /* 0x16AC: only data byte of a 2-byte message -> emit */
};

static u32 midi_in_sysex;        /* 0x1620: never set nonzero anywhere (vestigial) */
static u32 midi_in_state;        /* 0x1624 */
static u8  midi_in_status;       /* 0x1628: running status byte */
static u8  midi_in_data1;        /* 0x1629 */
static u8  midi_in_overflow;     /* 0x162B: FIFO overflowed, resync on next status byte */

/*
 * 0x16CC midi_event_push_ext
 * Confidence: high
 * Packs a channel message from the MIDI input into an event and appends it to
 * the ring: flags 0x1F, type = (status >> 4) & 7, port 7, channel = status & 15.
 * Notes: no ring-full check (see midi_event_pop). Uses the same ring as all
 * internal producers, so external MIDI is simply "port 7".
 */
void midi_event_push_ext(u32 status, u32 data1, u32 data2)
{
    u32 wr = evq_write_ofs;
    u32 ev = ((0xF8u | ((status >> 4) & 7)) << 24)
           | ((0x70u | (status & 0xF)) << 16)
           | ((data1 & 0xFF) << 8)
           | (data2 & 0xFF);

    *(volatile u32 *)(0xB000u + wr) = ev;
    evq_write_ofs = (wr + 4) & EVQ_MASK;
}

/*
 * 0x1724 (inside midi_in_isr) system message handling for status 0xF0-0xFF.
 * Confidence: high
 * F0 (SysEx start), F2-F6 and F7 (SysEx end) cancel running status; F1, F8-FF
 * (MTC quarter frame and real-time bytes) are ignored without disturbing it.
 * SysEx contents are therefore discarded as "data bytes with no state".
 * Notes: "cancel" only resets midi_in_state; midi_in_status is left as is.
 * Quirk (reproduced): F1 (MTC quarter frame) keeps the parser state, so its
 * data byte is taken as a running-status data byte of the previous channel
 * message (the same happens for a data byte following F8-FF, which is correct
 * MIDI behaviour for real-time bytes only).
 */
static void midi_in_system(u8 b)
{
    switch (b & 0xF) {
    case 0x0: case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7:
        midi_in_state = MIDI_IN_IDLE;
        break;
    default: /* 0x1, 0x8-0xF */
        break;
    }
}

/*
 * 0x15A4 midi_in_isr
 * Confidence: high
 * Called from the FIQ handler for the MIDI-in interrupt. Drains the FIFO and
 * runs a running-status MIDI parser; each complete channel message is pushed
 * into the event ring.
 * Notes:
 *  - The overflow flag (MIOVF) is tested before the data: on overflow the
 *    parser throws away data bytes until the next status byte.
 *  - The bit-7 test for resync is done on the raw register value; only bits
 *    7:0 are data, so that is equivalent to testing the byte.
 *  - midi_in_sysex is tested but never set, so SysEx bodies are discarded by
 *    the IDLE state instead.
 */
void midi_in_isr(void)
{
    for (;;) {
        u32 v = AICA_MIDI_IN;
        u8 b;

        if (v & AICA_MIDI_MIEMP)
            return;                         /* FIFO empty */
        if (v & AICA_MIDI_MIOVF) {
            midi_in_overflow = 0xFF;
            continue;
        }
        if (midi_in_overflow) {
            if (!(v & 0x80))
                continue;                   /* skip data until a status byte */
            midi_in_overflow = 0;
            midi_in_sysex = 0;
        }
        b = (u8)v;

        if (b & 0x80) {
            switch ((b & 0x70) >> 4) {
            case 0: case 1: case 2: case 3: case 6:   /* 8x 9x Ax Bx Ex */
                midi_in_status = b;
                midi_in_state = MIDI_IN_3B_DATA1;
                break;
            case 4: case 5:                           /* Cx Dx */
                midi_in_status = b;
                midi_in_state = MIDI_IN_2B_DATA;
                break;
            case 7:                                   /* Fx */
                midi_in_system(b);
                break;
            }
            continue;
        }

        if (midi_in_sysex)
            continue;
        switch (midi_in_state) {
        case MIDI_IN_IDLE:
            break;
        case MIDI_IN_3B_DATA1:
            midi_in_data1 = b;
            midi_in_state = MIDI_IN_3B_DATA2;
            break;
        case MIDI_IN_3B_DATA2:
            midi_event_push_ext(midi_in_status, midi_in_data1, b);
            midi_in_state = MIDI_IN_3B_DATA1;   /* running status */
            break;
        case MIDI_IN_2B_DATA:
            midi_event_push_ext(midi_in_status, b, 0);
            break;                              /* stays in 2-byte state */
        }
    }
}
