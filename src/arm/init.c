/*
 * init.c - boot-time initialisation of AICA and driver work RAM.
 * All routines here are called once from drv_reset (some also from host
 * commands, see host.c).
 */
#include "manatee.h"

static void zero_words(u32 addr, u32 count)
{
    volatile u32 *p = (volatile u32 *)addr;
    while (count--)
        *p++ = 0;
}

/*
 * 0x043C clear_host_area
 * Confidence: high
 * Zeroes the whole host-interface/work area 0x13000-0x17FFF (0x1400 words)
 * and sets fx_prg_cur (0x13474) = 0xFF ("no FX program loaded").
 */
void clear_host_area(void)
{
    zero_words(0x13000, 0x1400);
    fx_prg_cur = 0xFF;
}

/*
 * 0x0464 init_bank_table
 * Confidence: high
 * Clears 32 words at 0x14000 and 32 words at 0x14080 (the first 16 {addr, size}
 * entries of the MIDI program-bank table), then installs the built-in banks of
 * the driver image, in this order:
 *   0x14290 = { 0x4124, size, 0x4124, size }  program-bank entries 66/67 = drum
 *             kits 0/1 (builtin_smpb1, size word at 0x4230)
 *   0x14200 = { 0x2908, *(u32 *)0x3560 }      fx_prg_bank: builtin_sfpb "e-reverb"
 *   0x14280 = { 0x4234, *(u32 *)0x426C }      fx_out_bank: builtin_sfob
 * Notes: the size words are the builtin_*_size constants that follow each
 * bank in the image (tables.c). Entries 66/67 lie outside the cleared range.
 */
void init_bank_table(void)
{
    volatile u32 *t;

    zero_words(0x14000, 0x20);
    zero_words(0x14080, 0x20);

    t = (volatile u32 *)0x14290;
    t[0] = 0x4124; t[1] = SRAM(u32, 0x4230);
    t[2] = 0x4124; t[3] = SRAM(u32, 0x4230);

    fx_prg_bank = 0x2908;
    fx_prg_bank_size = SRAM(u32, 0x3560);

    fx_out_bank = 0x4234;
    fx_out_bank_size = SRAM(u32, 0x426C);
}

/*
 * 0x04E8 evq_clear
 * Confidence: high
 * Zeroes the 4 KB MIDI event ring at 0xB000.
 */
void evq_clear(void)
{
    zero_words(0xB000, 0x400);
}

/*
 * 0x0504 init_midi_ports
 * Confidence: high
 * Zeroes 0xC000-0xE7FF (0xA00 words: g_port[8] + g_midi_ch[8*16]), then sets
 * the non-zero power-on defaults:
 *  - PortState x8: vol/pan target and current = 0x80, pitch/speed target and
 *    current = 0x8000 (all "neutral"), direct_lvl = fx_send = 0x80 (offset 0).
 *  - MidiChannel x128: volume 100, level 100, expression 127, isel 0x7F,
 *    pan 0xC0 (bit7: use the tone's pan), q_ofs 0x40, cutoff 0x40, all
 *    EG/filter-EG rate offsets 0x20 (FAR/FD1R/FD2R/FRR/AR/DR/DL/CC90/RR),
 *    flv_ofs[0..4] = 0x2000, and the built-in tone bank at 0x3E48 with its
 *    program 0 and velocity-curve table.
 * Notes:
 *  - The asm writes every field with byte stores except the 0x8000 port words
 *    and the three bank pointers. flv_ofs[i] = 0x2000 is a strb of 0x20 to the
 *    high byte (+0x29, +0x2B, ... +0x31) and cutoff = 0x40 a strb to its low
 *    byte; the other bytes are already 0 from the clear, so the word stores
 *    below give the same memory image.
 *  - The ports loop runs first, then the channels loop (store order within a
 *    record is irrelevant: nothing else runs during boot).
 *  - Program 0 is not bounds-checked against num_progs (the built-in bank has one).
 */
void init_midi_ports(void)
{
    PortState *port;
    MidiChannel *ch;
    const BankHeader *bank = DEFAULT_BANK;
    u32 i, n;

    zero_words(0xC000, 0xA00);

    for (n = 0, port = g_port; n < NUM_PORTS; n++, port++) {
        port->vol_target   = 0x80;
        port->volume       = 0x80;
        port->pan_target   = 0x80;
        port->pan          = 0x80;
        port->pitch_target = 0x8000;
        port->pitch        = 0x8000;
        port->speed_target = 0x8000;
        port->speed        = 0x8000;
        port->direct_lvl   = 0x80;
        port->fx_send      = 0x80;
    }

    for (n = 0, ch = g_midi_ch; n < NUM_PORTS * NUM_MIDI_CH; n++, ch++) {
        ch->volume     = 100;
        ch->level      = 100;
        ch->expression = 0x7F;
        ch->isel       = 0x7F;
        ch->pan        = 0xC0;
        ch->q_ofs      = 0x40;
        ch->cutoff     = 0x40;      /* strb to the low byte */
        ch->far_ofs    = 0x20;
        ch->fd1r_ofs   = 0x20;
        ch->fd2r_ofs   = 0x20;
        ch->frr_ofs    = 0x20;
        for (i = 0; i < 5; i++)
            ch->flv_ofs[i] = 0x2000; /* strb 0x20 to the high byte */
        ch->ar_ofs     = 0x20;
        ch->dr_ofs     = 0x20;
        ch->cc90_ofs   = 0x20;
        ch->rr_ofs     = 0x20;
        ch->dl_ofs     = 0x20;
        ch->bank       = (u32)bank;
        ch->prog       = (u32)bank + *(const u32 *)((u32)bank + bank->prog_tab); /* prog_ofs[0] */
        ch->velcurve   = (u32)bank + bank->velcurve_tab;
    }
}

/*
 * 0x0614 clear_voices
 * Confidence: high
 * Zeroes 0xE800-0xF7FF: room for 64 voices of 0x40 bytes although only 48 are used.
 */
void clear_voices(void)
{
    zero_words(0xE800, 0x400);
}

/*
 * 0x0638 midi_in_flush
 * Confidence: high
 * Four dummy reads of the MIDI-in register to drain the 4-byte input FIFO.
 */
void midi_in_flush(void)
{
    (void)AICA_MIDI_IN;
    (void)AICA_MIDI_IN;
    (void)AICA_MIDI_IN;
    (void)AICA_MIDI_IN;
}

/*
 * 0x0654 aica_silence_all
 * Confidence: high
 * Master volume 0, then for all 64 channels write KYONEX with KYONB = 0 and
 * SA = 0 (key off, executed immediately) and RR = 0x1F (instant release).
 * Notes: the KYONEX write on each channel re-executes key on/off for *all*
 * channels; harmless here since all are being keyed off.
 */
void aica_silence_all(void)
{
    u32 i;

    AICA_MVOL = 0;
    for (i = 0; i < AICA_NUM_CHANNELS; i++) {
        AICA_CH[i].ctl_sa_hi = AICA_KYONEX;
        AICA_CH[i].env_dr = 0x1F;
    }
}

/*
 * 0x08D0 dsp_out_clear
 * Confidence: high
 * Zeroes the 18 DSP output mixer registers at 0x802000 (EFSDL/EFPAN for the
 * 16 EFREG outputs and the 2 EXTS/CDDA inputs), muting the effect returns.
 */
void dsp_out_clear(void)
{
    u32 i;

    for (i = 0; i < 18; i++)
        AICA_DSP_OUT(i) = 0;
}
