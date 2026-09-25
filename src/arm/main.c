/*
 * main.c - exception vectors, reset entry, boot sequence and the main loop.
 *
 * The SH-4 (sdDrvInit) holds the ARM in reset, fills all of sound RAM with
 * "SEGA", copies this image to address 0 and releases reset. There is no
 * handshake: the SH-4 simply waits and then reads the version at 0x20. The
 * driver nevertheless publishes drv_status (0xF8): 0xFF while initialising,
 * "SEGA" once the main loop is about to start.
 *
 * Everything runs in the main loop except the FIQ handler (irq.c), which only
 * raises tick flags and parses MIDI input. The loop:
 *   every pass     : execute up to 4 queued MIDI events (voice_service_x4)
 *   1 ms flag      : sample one voice envelope, advance the monitor
 *   4 ms flag      : sample, host commands, sequencer, fades, voices,
 *                    status export, FX, advance the monitor
 */
#include "manatee.h"

/*
 * 0x0000 vector table
 * Confidence: high
 * All vectors except reset and FIQ go to exc_ignore (irq.c). 0x14 is the
 * ARMv3 26-bit address-exception slot. 0x20-0xFF hold the version/credit
 * header and drv_layout_table (see data_map.md), then reset code at 0x100.
 */
__attribute__((naked, section(".vectors"))) void vectors(void)
{
    __asm__ volatile(
        "b drv_reset\n"      /* 0x00 reset */
        "b exc_ignore\n"     /* 0x04 undefined instruction */
        "b exc_ignore\n"     /* 0x08 SWI */
        "b exc_ignore\n"     /* 0x0C prefetch abort */
        "b exc_ignore\n"     /* 0x10 data abort */
        "b exc_ignore\n"     /* 0x14 address exception (26-bit) */
        "b exc_ignore\n"     /* 0x18 IRQ (never raised by the AICA) */
        "b fiq_handler\n");  /* 0x1C FIQ */
}

void drv_main(void) __attribute__((noreturn, used));

/*
 * 0x0100 drv_reset (first four instructions)
 * Confidence: high
 * SVC stack at 0xB000 (just below the event ring, growing down into free RAM
 * above the image), FIQ masked until initialisation is complete.
 */
__attribute__((naked, noreturn)) void drv_reset(void)
{
    __asm__ volatile(
        "mov sp, #0xB000\n"
        "mrs r10, cpsr\n"
        "orr r10, r10, #0x40\n"   /* F bit: mask FIQ */
        "msr cpsr_cf, r10\n"
        "b drv_main\n");
}

static inline void fiq_enable(void)
{
    u32 cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    cpsr &= ~0x40u;
    __asm__ volatile("msr cpsr_cf, %0" : : "r"(cpsr));
}

/*
 * 0x0110-0x0224 drv_reset (body) - boot sequence and main loop
 * Confidence: high
 * Boot order matters in two places: the AICA is silenced and all work RAM
 * cleared before anything else, and irq_init (which starts Timer A and the
 * MIDI-in interrupt) runs last, just before FIQ is unmasked.
 * Main loop (0x194-0x224): voice_service_x4 on every pass; if tick1ms_flag is
 * set it is cleared, the monitor sampled/advanced and the loop restarts
 * *without* looking at tick4ms_flag (the FIQ clears tick1ms_flag whenever it
 * raises tick4ms_flag, so the 4 ms work is never starved). Otherwise, if
 * tick4ms_flag is set it is cleared and the 4 ms chain runs in exactly the
 * order below. Flags are bytes tested with "tst #0xFF".
 * Notes:
 *  - master volume goes 0 -> 0xF only after the voices are silenced.
 *  - sndram_free_base (0x13408) = 0x18000 is written but no ARM code reads it;
 *    0x18000 is also the minimum address fx_prg_select accepts for the DSP
 *    ring buffer, i.e. the start of SH-4-managed bank memory.
 *  - "SEGA" is loaded from the credit string at 0x38 rather than as a literal.
 */
void drv_main(void)
{
    drv_status = 0xFF;
    AICA_MVOL = 0;

    aica_silence_all();         /* 0x0654 */
    clear_host_area();          /* 0x043C */
    init_bank_table();          /* 0x0464 */
    evq_clear();                /* 0x04E8 */
    init_midi_ports();          /* 0x0504 */
    clear_voices();             /* 0x0614 */
    dsp_out_clear();            /* 0x08D0 */
    fx_prg_clear();             /* 0x5DCC */
    seq_init();                 /* 0x742C */
    pcm_init();                 /* 0x7778 */
    fx_status_init();           /* 0x0694 */

    AICA_MVOL = 0xF;
    master_vol_shadow = 0xF;
    midi_in_flush();            /* 0x0638 */
    irq_init();                 /* 0x03CC */

    SRAM(u32, 0x13408) = 0x18000;             /* sndram_free_base */
    drv_status = *(const u32 *)0x38;          /* "SEGA" from the credits */
    fiq_enable();

    for (;;) {
        voice_service_x4();                   /* 0x09A8, every pass */

        if (tick1ms_flag) {
            tick1ms_flag = 0;
            voice_monitor_sample();
            voice_monitor_next();
            continue;
        }
        if (!tick4ms_flag)
            continue;
        tick4ms_flag = 0;

        voice_monitor_sample();
        if (host_cmd_pending)
            host_cmd_poll();                  /* 0x4948 */
        pcm_tick();                           /* 0x711C one-shot / stream players */
        seq_tick();                           /* 0x4270 MIDI sequencer */
        port_speed_fade();                    /* 0x67F8 */
        port_vol_fade();                      /* 0x66B8 */
        port_pan_fade();                      /* 0x6758 */
        port_pitch_fade();                    /* 0x68C4 */
        voice_update_4ms();                   /* 0x6C4C */
        port_status_export();                 /* 0x6470 -> 0x13600.. for the SH-4 */
        ticks_since_cmd_inc();                /* 0x6698 */
        pcm_speed_fade();                     /* 0x6970 */
        pcm_pitch_fade();                     /* 0x6A20 */
        pcm_pitch_update();                   /* 0x6AD4 */
        fx_update_ringbuf();                  /* 0x076C */
        fx_ext_flags_update();                /* 0x06D4 */
        fx_prg_load_service();                /* 0x61D4 */
        voice_monitor_next();
    }
}
