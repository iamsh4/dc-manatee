/*
 * irq.c - exception vectors, FIQ handler and interrupt controller setup.
 *
 * The AICA drives only the ARM FIQ line. Its interrupt controller maps each
 * SCIPD source to a 3-bit level (SCILV0-2) which the handler reads back from
 * INTREQ. Used sources:
 *   Timer A  (bit 6) -> level 2 : the 1 ms / 4 ms driver clock
 *   MIDI in  (bit 3) -> level 5 : external MIDI input
 * The SH-4 never interrupts the ARM; it is polled through shared memory.
 */
#include "manatee.h"

volatile u8 tick4ms_flag;     /* 0x22C */
static u8   tick_sub4;        /* 0x22D: 1 ms ticks modulo 4 */
volatile u8 tick1ms_flag;     /* 0x22E */
static u32  fiq_count;        /* 0x230 */

#define TIMA_RELOAD      0xD4u  /* 0x31C: overflow after 256-0xD4 = 44 samples */
#define TICK_DRIFT_RELOAD 441u  /* 0x324 */
static u32 tick_drift_count = TICK_DRIFT_RELOAD;   /* 0x320 */

/*
 * 0x0000-0x001C vector table, 0x0234 exc_ignore
 * Confidence: high
 * Reset branches to drv_reset (0x100, see main.c). Undefined instruction, SWI,
 * prefetch abort, data abort, the 26-bit address exception slot (0x14) and IRQ
 * all go to exc_ignore, which does:
 *     sub lr, lr, #4 ; adr sp, save ; str lr, [sp] ; ldmia sp, {pc}^
 * i.e. return to LR-4 restoring CPSR (save word at 0x244; the exception
 * mode's banked sp is clobbered). Effect per exception (LR = fault + 4 for
 * undef/SWI/prefetch abort, + 8 for data abort/address exception, next + 4
 * for IRQ):
 *   IRQ                  : correct return to the interrupted instruction
 *   prefetch abort       : retries the faulting instruction (loops if persistent)
 *   undefined, SWI       : re-executes the same instruction -> loops forever
 *   data abort, addr exc : resumes *after* the faulting instruction (skips it)
 * IRQ is never raised by the AICA, so in practice this is a crash trap.
 * The FIQ vector branches to fiq_handler (0x248).
 * Expressed as a naked function; the vector table itself is in main.c.
 */
__attribute__((naked)) void exc_ignore(void)
{
    __asm__ volatile(
        "sub lr, lr, #4\n"
        "adr sp, 1f\n"
        "str lr, [sp]\n"
        "ldmia sp, {pc}^\n"
        "1: .word 0\n");
}

/*
 * 0x02A8 (inside fiq_handler) Timer A service.
 * Confidence: high
 * Timer A counts samples at 44.1 kHz; reloading 0xD4 gives an interrupt every
 * 44 samples = 0.99773 ms. tick1ms_flag is raised on *every* interrupt. For the
 * 4 ms clock, one interrupt out of every 441 is not counted, so the 4 ms tick
 * advances 440 times per 441 * 44 = 19404 samples = exactly 0.44 s, i.e. the
 * 4 ms tick is exact while the "1 ms" flag runs at ~1.0023 kHz.
 * When a 4 ms tick fires, tick1ms_flag is cleared again: the main loop's 4 ms
 * branch performs the 1 ms work as well.
 */
static void timer_a_service(void)
{
    AICA_TIMA = TIMA_RELOAD;          /* TACTL = 0: count every sample */
    AICA_SCIRE = AICA_INT_TIMER_A;
    tick1ms_flag = 0xFF;

    if (--tick_drift_count == 0) {
        tick_drift_count = TICK_DRIFT_RELOAD;
        return;                       /* drop this tick from the 4 ms clock */
    }
    tick_sub4 = (tick_sub4 + 1) & 3;
    if (tick_sub4 != 0)
        return;
    tick4ms_count++;                  /* visible to the SH-4 at 0x13418 */
    tick4ms_flag = 0xFF;
    tick1ms_flag = 0;
}

/*
 * 0x0248 fiq_handler
 * Confidence: high
 * Runs in FIQ mode with banked r8-r12 and its own stack inside the driver
 * image: "adr sp, 0x3CC" on every entry, LR-4 pushed at 0x3C8 and r0-r7 at
 * 0x3A8 on the MIDI path (0x368-0x3A7 is reserved but never reached).
 * Dispatches on the level reported by INTREQ & 7 through an 8-entry branch
 * table; only levels 2 and 5 do anything. The MIDI path saves r0-r7 because
 * midi_in_isr (and midi_event_push_ext) use them. Finally INTCLR is written
 * four times (presumably so the AICA reliably releases the FIQ line; irq_init
 * uses 8 writes) and "ldmia sp!, {pc}^" returns restoring CPSR.
 * Notes:
 *  - fiq_count (0x230) counts every FIQ, including spurious levels.
 *  - "mov r11, #0" at 0x254 is dead: r11_fiq is not read anywhere.
 *  - The MIDI path never writes SCIRE for bit 3 (the timer path acks bit 6);
 *    how the MIDI-in request is released is not visible in the driver.
 */
__attribute__((interrupt("FIQ"))) void fiq_handler(void)
{
    fiq_count++;

    switch (AICA_INTREQ & 7) {
    case 2:
        timer_a_service();
        break;
    case 5:
        midi_irq_count++;             /* 0x1341C */
        midi_irq_flag = 0xFFFFFFFFu;  /* 0xFC */
        midi_in_isr();
        break;
    default:                          /* 0,1,3,4,6,7: spurious */
        break;
    }

    AICA_INTCLR = 1;
    AICA_INTCLR = 1;
    AICA_INTCLR = 1;
    AICA_INTCLR = 1;
}

/*
 * 0x03CC irq_init
 * Confidence: high
 * Releases any pending FIQ (8 x INTCLR with 3 NOPs of delay each), assigns
 * interrupt levels, starts Timer A and enables its and the MIDI-in interrupt.
 * SCILVn bit k is bit n of the level of source k:
 *   SCILV0 = 0x18, SCILV1 = 0x50, SCILV2 = 0x08
 *   -> bit3 (MIDI in) = 1|0|4 = 5, bit4 (DMA) = 1|2|0 = 3, bit6 (Timer A) = 0|2|0 = 2
 * DMA end gets level 3 but is never enabled.
 */
void irq_init(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        AICA_INTCLR = 1;
        __asm__ volatile("mov r0, r0\n mov r0, r0\n mov r0, r0");
    }
    AICA_SCILV0 = 0x18;
    AICA_SCILV1 = 0x50;
    AICA_SCILV2 = 0x08;
    AICA_TIMA = TIMA_RELOAD;
    AICA_SCIEB = AICA_INT_MIDI_IN | AICA_INT_TIMER_A;   /* 0x48 */
    AICA_SCIRE = AICA_INT_MIDI_IN | AICA_INT_TIMER_A;
}
