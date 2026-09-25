/*
 * fx.c - AICA DSP effects: FX program banks (SFPB), FX output banks (SFOB), the
 * DSP ring buffer (SFPW), and the small host commands that live in the same
 * part of the image (0x5A8C-0x6470).
 *
 * Data formats: docs/notes/fx_format.md.  Overview:
 *   - fx_prg_select() validates a program of the current SFPB and queues it
 *     (fx_prg_pending / fx_rb_pending / fx_load_req).
 *   - fx_prg_load_service() (main loop, every 4 ms) performs the queued load:
 *     fade the 16 DSP outputs out, clear the DSP, clear the ring buffer,
 *     program RINGBUF, copy COEF/MADRS/MPRO, clear the ring buffer again and
 *     fade the outputs back in.
 *   - fx_cmd_set_out()/fx_cmd_set_out_prm() program EFSDL/EFPAN of the 16 DSP
 *     outputs (0x802000) from an SFOB set plus per-output host offsets.
 *   - fx_update_ringbuf() keeps RINGBUF in sync with the SFPW bank entry.
 *
 * DSP ring-buffer memory stores 16-bit "floats" (sign, 4-bit exponent,
 * 11-bit mantissa). 0x0000 is NOT zero in that format (exponent 0 means the
 * implicit leading 1 is present), 0x6000 is (exponent >= 11, mantissa 0);
 * that is why fx_ringbuf_clear fills with 0x60006000.
 */
#include "manatee.h"

/* ---- AICA DSP output mixer, byte access (EFPAN at +0, EFSDL at +1) ---- */
#define DSP_OUT_PAN_B(n)  AICA_REG8(0x2000 + (n) * 4)
#define DSP_OUT_LEV_B(n)  AICA_REG8(0x2001 + (n) * 4)
#define NUM_DSP_OUT       16

/* ---- state words embedded in the driver image ---- */
static u32 fx_rb_dirty;         /* 0x0858: 0xFF when the RBP part of RINGBUF changed */
#define fx_prg_sel SRAM(u8, 0x5DC8)  /* program number accepted by fx_prg_select (voice.c) */
static u32 fx_out_save[16];     /* 0x63F4: DSP output registers saved by the fade-out */

/* The fade loops wait 255 iterations of "mov r0,r0; subs; bne" (~3 cycles each). */
static void fx_delay(void)
{
    volatile u32 n = 0xFF;
    do {
    } while (--n);
}

/*
 * 0x0694 fx_status_init
 * Confidence: high
 * Boot: publishes the DSP register window (fx_dsp_reg_base = 0x803000,
 * fx_dsp_reg_size = 0xC00: COEF, MADRS, MPRO) and the RBP of the SFPW
 * address in the 0x13F00 block (drv_layout_table[0]).
 * Notes: provisional name was fx_init_ringbuf; it does not touch the
 * RINGBUF register. At boot fx_wrk_addr is still 0 (clear_host_area), so
 * fx_ringbuf_reg becomes 0. Called after clear_host_area/init_bank_table.
 */
void fx_status_init(void)
{
    fx_dsp_reg_base = 0x00803000;
    fx_dsp_reg_size = 0xC00;
    fx_ringbuf_reg = (fx_wrk_addr >> 11) & 0xFFF;
}

/*
 * 0x06D4 fx_ext_flags_update
 * Confidence: high
 * Every 4 ms: mirrors three request bytes of the 0x13F00 block into
 * acknowledge bytes:
 *   fx_ext_active = (fx_ext_req && fx_wrk_addr) ? 0xFF : 0
 *   ext_ack2      = ext_req2 ? 0xFF : 0
 *   ext_ack3      = ext_req3 ? 0xFF : 0
 * While fx_ext_active is set, fx_update_ringbuf and fx_prg_load_service do
 * nothing, i.e. something else (an SH-4 side tool?) owns the DSP.
 * Notes: provisional name fx_update_flags. Nothing in the R9 sd library
 * reads/writes 0x13F00..; who sets the requests is unknown, and ext_req2/3
 * have no other user in the driver. The "movne r1,#0" in the clear paths
 * never executes (Z is always set there) but r1 is already 0.
 */
void fx_ext_flags_update(void)
{
    fx_ext_active = (fx_ext_req != 0 && fx_wrk_addr != 0) ? 0xFF : 0;
    ext_ack2 = ext_req2 ? 0xFF : 0;
    ext_ack3 = ext_req3 ? 0xFF : 0;
}

/*
 * 0x076C fx_update_ringbuf
 * Confidence: high
 * Every 4 ms (skipped while fx_ext_active): if an SFPW ring buffer at or
 * above 0x18000 is registered, recompute AICA_RINGBUF from it and write the
 * register when it changed:
 *   RBP (bits 11:0)  = addr >> 11                 (2 KB units)
 *   RBL (bits 14:13) = size >= 128K ? 3 : >= 64K ? 2 : >= 32K ? 1 : 0
 * The shadow is fx_ringbuf_reg (0x13F10); fx_wrk_size_pub gets a copy of the
 * size whenever the register is written.
 * Notes: an RBP change only sets the image-embedded fx_rb_dirty flag; the
 * register is written after the RBL comparison (both changes are then
 * written together). The size test only looks at bits 15..21 of the size
 * (size >> 14 & 0xFE), e.g. a 4 MB size would count as "< 32 KB".
 * The RBL computed here comes from the SFPW size, not from the program's
 * rb_size code, so it overrides the RBL that fx_prg_load_service wrote
 * (fx_rb_pending) one tick later whenever the two disagree.
 * The address compare is "cmp; movmi": it tests only the N flag of
 * addr - 0x18000, so addresses 0x80018000.. also return (irrelevant for
 * real 2 MB sound-RAM addresses).
 */
void fx_update_ringbuf(void)
{
    u32 addr, rbp, rbl, s;

    if (fx_ext_active)
        return;
    addr = fx_wrk_addr;
    if ((s32)(addr - 0x18000) < 0)        /* N flag only (movmi) */
        return;

    rbp = (addr >> 11) & 0xFFF;
    if ((fx_ringbuf_reg & 0xFFF) != rbp) {
        fx_rb_dirty = 0xFF;
        /* asm: r4 = (reg & 0xFFF) & 0x6000 == 0, so RBL is dropped here
           and restored by the RBL comparison below */
        fx_ringbuf_reg = rbp;
    }

    s = fx_wrk_size >> 14;                  /* size in 16 KB units */
    if (s & 0xF8)
        rbl = 0x6000;                       /* RBL 3: 64K words = 128 KB */
    else if (s & 0x4)
        rbl = 0x4000;                       /* RBL 2: 64 KB */
    else if (s & 0x2)
        rbl = 0x2000;                       /* RBL 1: 32 KB */
    else
        rbl = 0;                            /* RBL 0: 16 KB */

    if ((fx_ringbuf_reg & 0x6000) != rbl) {
        fx_ringbuf_reg = (fx_ringbuf_reg & 0xFFF) | rbl;
    } else if (!(fx_rb_dirty & 0xFF)) {
        return;
    }
    fx_rb_dirty = 0;
    AICA_RINGBUF = fx_ringbuf_reg;
    fx_wrk_size_pub = fx_wrk_size;
}

/*
 * Level/pan combination shared (inlined twice) by fx_cmd_set_out (0x5B14..)
 * and fx_cmd_set_out_prm (0x5C10..).
 *   level: lev_ofs + EFSDL*8, clamped 0..127, >> 3        -> EFSDL (bits 11:8)
 *   pan:   AICA pan code -> linear 0..31 (0x10 = centre), pan_ofs + lin*4,
 *          clamped 0..127, >> 2, back to an AICA pan code  -> EFPAN (bits 4:0)
 * The pan conversion: code 0x10 -> 0; code c with bit4 set (left side) ->
 * ((c - 0x10) ^ 0xFF) + 0x10; then + 0x10, & 0x1F. Inverse: x - 0x10, if
 * negative ^0xFF + 0x10, & 0x1F. (Same mapping as pan_aica_to_lin_a /
 * pan_lin_to_aica_a, computed instead of looked up.)
 */
static u32 fx_out_value(u32 lev, u32 pan, s32 lev_ofs, s32 pan_ofs)
{
    s32 l, p;
    u32 c;

    l = lev_ofs + (s32)((lev & 0xF) << 3);
    if (l < 0)
        l = 0;
    if (l & 0x80)
        l = 0x7F;
    l = (l << 5) & 0xF00;                   /* (l >> 3) << 8 */

    c = pan;
    if (c == 0x10)
        c = 0;
    if (c & 0x10)
        c = (c - 0x10) ^ 0xFF;
    c = (c + 0x10) & 0x1F;                  /* linear, 0x10 = centre */

    p = pan_ofs + (s32)(c << 2);
    if (p < 0)
        p = 0;
    if (p & 0x80)
        p = 0x7F;
    p = (p >> 2) & 0x1F;
    p -= 0x10;
    if (p < 0)
        p = (p ^ 0xFF) + 0x10;
    return (u32)l | ((u32)p & 0x1F);
}

/*
 * 0x5A8C fx_cmd_set_out (host command 0x82, sdSndSetFxOut / second half of
 * sdSndSetFxPrg)
 * Confidence: high
 * cmd[2] = output-set number. Validates the SFOB at fx_out_bank (magic
 * "SFOB" -> else err 2, version byte 1 -> else 8, "ENDB" at size-4 -> else
 * 4, number < count byte -> else 0x10), stores the number in fx_out_cur and
 * the set table base in fx_out_table, then writes all 16 DSP output
 * registers from set[number] combined with the host offsets in FX_OUT_PRM.
 * Returns 0, or (drv_err | err) after ORing err into drv_err.
 * Notes: fx_out_bank is not checked for 0 (the magic read from address 0
 * then fails with err 2). Only the LOW BYTE of offset[0] is used
 * ("ldrb r2,[r1,#0x10]") and the sets are assumed to be contiguous 0x20-byte
 * records after it; offset[1..] are ignored. Works for all known banks
 * (offset[0] = 0x14 / 0x18). Only one output set exists in every known SFOB.
 */
u32 fx_cmd_set_out(volatile HostCmd *cmd)
{
    u32 num = cmd->arg[1];
    const u8 *bank = (const u8 *)fx_out_bank;
    const FxBankHeader *h = (const FxBankHeader *)bank;
    const FxOutEntry *e;
    u32 err, i;

    if (h->magic != FX_SFOB_MAGIC)
        err = DRV_ERR_ILLEGAL_ID;
    else if ((h->version & 0xFF) != 1)
        err = DRV_ERR_ILLEGAL_VER;
    else if (*(const u32 *)(bank + h->size - 4) != FX_ENDB_MAGIC)
        err = DRV_ERR_ILLEGAL_END;
    else if (num >= (h->count & 0xFF))
        err = DRV_ERR_ILLEGAL_NUM;
    else {
        fx_out_cur = (u8)num;
        fx_out_table = (u32)bank + bank[0x10];          /* low byte of offset[0] only */
        e = (const FxOutEntry *)(fx_out_table + num * sizeof(FxOutSet));
        for (i = 0; i < NUM_DSP_OUT; i++)
            AICA_DSP_OUT(i) = fx_out_value(e[i].lev, e[i].pan,
                                           FX_OUT_PRM[i * 2], FX_OUT_PRM[i * 2 + 1]);
        return 0;
    }
    drv_err |= err;
    return drv_err;
}

/*
 * 0x5BD8 fx_cmd_set_out_prm (host command 0x83, sdSndSetFxOutPrm)
 * Confidence: high
 * cmd[2] & 15 = DSP output n, cmd[3] = level offset + 0x80, cmd[4] = pan
 * offset + 0x80. Stores the offsets (s8) in FX_OUT_PRM[n] and rewrites
 * output n from the SFOB entry and the new offsets. Returns 0.
 * Notes: the SFOB entry used is fx_out_table + n*2, i.e. always output set
 * 0, not the selected set fx_out_cur (bug; harmless with 1-set banks).
 * If no SFOB set was ever selected (fx_out_table == 0) the offsets are NOT
 * stored and the routine returns n (r0 still holds cmd[2] & 15).
 * Level offset unit: 1/8 EFSDL step (the library passes lev unshifted for
 * driver build >= 0x27); pan offset unit: 1/4 linear pan step.
 */
u32 fx_cmd_set_out_prm(volatile HostCmd *cmd)
{
    u32 n = cmd->arg[1] & 0xF;
    const FxOutEntry *e;
    s32 lev_ofs, pan_ofs;

    if (fx_out_table == 0)
        return n;
    e = (const FxOutEntry *)fx_out_table + n;       /* set 0 regardless of fx_out_cur */
    lev_ofs = (s32)cmd->arg[2] - 0x80;
    pan_ofs = (s32)cmd->arg[3] - 0x80;
    FX_OUT_PRM[n * 2] = (s8)lev_ofs;
    FX_OUT_PRM[n * 2 + 1] = (s8)pan_ofs;
    AICA_DSP_OUT(n) = fx_out_value(e->lev, e->pan, lev_ofs, pan_ofs);
    return 0;
}

/*
 * 0x5C98 fx_prg_select is implemented in voice.c (it is reached from the CC
 * path there). Host command 0x84 enters it at 0x5C94 with prg = cmd[2]
 * (host.c). Quirk (asm 0x5D58, reproduced in voice.c): when the SFPW is
 * smaller than fx_rb_need_tab[rb_size] the error exit is taken with
 * r1 = rb_size * 4 instead of 1, so drv_err gets 0 / 4 / 8 / 0xC for
 * rb_size 0..3 (i.e. an "illegal end id"/"illegal version" report, or no
 * error at all for rb_size 0). See docs/notes/fx_format.md.
 */

/*
 * 0x5DCC fx_prg_clear (host command 0x85 sdSndClearFxPrg; CC79 = 0 on port 7;
 * drv_reset; host 0x80 with cmd[6] != 0; host 0x8E/0x8F)
 * Confidence: high
 * Fades the DSP outputs out, clears the DSP (sends, MPRO, COEF, MADRS,
 * TEMP; fx_prg_cur = 0xFF), fills the ring buffer with DSP zero, and fades
 * the outputs back to their previous levels. Returns 0.
 * Notes: the controllers code calls this dsp_program_off().
 */
u32 fx_prg_clear(void)
{
    fx_out_fade_out();
    fx_dsp_clear();
    fx_ringbuf_clear();
    fx_out_fade_in();
    return 0;
}

/*
 * 0x5DF0 fx_dsp_clear
 * Confidence: high
 * Zeroes the DSP send (IMXL/ISEL, reg +0x20) of all 64 channels, all 512
 * MPRO words, 128 COEF, 64 MADRS and 256 TEMP words, and sets fx_prg_cur to
 * 0xFF (no program).
 * Notes: channels 48..63 (not driver voices) lose their DSP send as well.
 * MEMS/MIXS/EFREG are not touched.
 */
void fx_dsp_clear(void)
{
    u32 i;

    for (i = 0; i < AICA_NUM_CHANNELS; i++)
        AICA_CH[i].dsp_send = 0;
    for (i = 0; i < 0x200; i++)
        AICA_DSP_MPRO(i) = 0;
    for (i = 0; i < 0x80; i++)
        AICA_DSP_COEF(i) = 0;
    for (i = 0; i < 0x40; i++)
        AICA_DSP_MADRS(i) = 0;
    for (i = 0; i < 0x100; i++)
        AICA_DSP_TEMP(i) = 0;
    fx_prg_cur = 0xFF;
}

/*
 * 0x5E70 host_cmd_push_event (host command 0x86)
 * Confidence: high
 * Appends the packed MIDI event in cmd bytes 4..7 (little-endian word) to
 * the event ring at 0xB000 (no full check). Returns 0.
 * Notes: not an effects routine; lives in this part of the image.
 */
u32 host_cmd_push_event(volatile HostCmd *cmd)
{
    u32 ev = *(volatile u32 *)&cmd->arg[3];
    u32 w = evq_write_ofs;

    *(volatile u32 *)(0xB000u + w) = ev;
    evq_write_ofs = (w + 4) & EVQ_MASK;
    return 0;
}

/*
 * 0x5EA0 host_cmd_drum_mode (host command 0x87; the R9 library never sends it)
 * Confidence: high
 * cmd[2] bit0 = on/off, cmd[3] & 7 = port. Acts on MIDI channel 10 (index
 * 9) of the port: off -> drum_kit = 0. On -> drum_kit = 1; if drum kit bank
 * 66 (0x14290 {addr,size}) has a nonzero address and size: bank_no = 0,
 * bank = that address; otherwise bank = the built-in kit 0x4124. Then
 * prog = first program and velcurve = curve table of the bank.
 * Returns r0 = the channel pointer (only its low byte is logged).
 * Notes: BUG in the fallback path (0x5F48): it stores 0x4124 in bank but
 * then branches to the common tail (0x5F24), which computes prog/velcurve
 * from r2 = the (zero or unsized) bank-66 address instead of 0x4124. With
 * address 0 that dereferences the exception vectors (mem[0x10] =
 * 0xEA000087 ...), giving garbage prog/velcurve pointers. bank_no is not
 * cleared on the fallback path. Not an effects routine.
 */
u32 host_cmd_drum_mode(volatile HostCmd *cmd)
{
    MidiChannel *mc = MIDI_CH(cmd->arg[2] & 7, 9);
    volatile u32 *kit = SRAM_PTR(volatile u32, 0x14290);
    u32 base = kit[0];                          /* r2 */

    if (!(cmd->arg[1] & 1)) {
        mc->drum_kit = 0;
        return (u32)mc;
    }
    mc->drum_kit = 1;
    if (base != 0 && kit[1] != 0) {
        mc->bank_no = 0;
        mc->bank = base;
    } else {
        mc->bank = 0x4124u;                     /* builtin_smpb1 ... */
        /* ... but the tail below still uses base (bug, see Notes) */
    }
    mc->prog = base + *(const u32 *)(base + *(const u32 *)(base + 0x10));
    mc->velcurve = base + *(const u32 *)(base + 0x18);
    return (u32)mc;
}

/*
 * 0x5F54 fx_cmd_set_dsp_pan (host command 0x88; not sent by the R9 library)
 * Confidence: high
 * For programs with pan slots (fx_pan_mode & 0x30): cmd[2..] hold one byte
 * per slot (8 slots if bit5 clear, 4 slots if bit5 set). A byte with bit7
 * set selects preset (b >> 2) & 31 of fx_pan_coef_tab, whose 4 words are
 * written to COEF[base + slot + k*stride], k = 0..3, stride = number of
 * slots, base = fx_pan_coef_base. Bytes with bit7 clear leave the slot.
 * Notes: fx_pan_mode is read with an unaligned "ldr" from 0x13F1D; on the
 * ARM7 this rotates the word at 0x13F1C so the low byte is 0x13F1D - i.e.
 * it behaves like a byte load. Priority differs from the MIDI version
 * (CC80, 0x250C): here bit5 wins (4 slots), there bit4 wins (8 slots).
 * Returns r0 as left by the loop (the 0x13F1D pointer when there are no
 * slots, else the last byte or preset address) - only the low byte is logged.
 */
u32 fx_cmd_set_dsp_pan(volatile HostCmd *cmd)
{
    u32 mode = fx_pan_mode, nslot, i, k, r0 = 0x13F1D;
    volatile u32 *coef;
    const u32 *src;

    if (!(mode & 0x30))
        return r0;
    nslot = (mode & 0x20) ? 4 : 8;              /* stride == number of slots */
    coef = &AICA_DSP_COEF(fx_pan_coef_base);
    for (i = 0; i < nslot; i++) {
        r0 = cmd->arg[1 + i];
        if (r0 & 0x80) {
            src = fx_pan_coef_tab[(r0 & 0x7C) >> 2];
            r0 = (u32)src;
            for (k = 0; k < 4; k++)
                coef[i + k * nslot] = src[k];
        }
    }
    return r0;
}

/*
 * 0x6040 host_cmd_nop89 (host command 0x89)
 * Confidence: high
 * Just returns. r0 still holds host_cmd_exec's table offset
 * ((0x89 << 2) & 0x1FF = 0x24), which is what gets logged.
 */
u32 host_cmd_nop89(void)
{
    return 0x24;
}

/*
 * 0x6044 host_cmd_mono (host command 0x8A; not sent by the R9 library)
 * Confidence: high
 * cmd[2] == 0: stereo: mono_flag (0x13404) = 0, MVOL = master volume.
 * cmd[2] != 0: mono: mono_flag = -1, MVOL = MONO (bit15) | min(master, 13).
 * Returns 0. Master volume (0x13424) is set by host 0x81, which also
 * applies the clamp when mono_flag is set.
 */
u32 host_cmd_mono(volatile HostCmd *cmd)
{
    u32 vol = master_vol_shadow;

    if (cmd->arg[1] == 0) {
        SRAM(u32, 0x13404) = 0;
        AICA_MVOL = vol;
    } else {
        SRAM(u32, 0x13404) = 0xFFFFFFFFu;
        if (vol > 13)
            vol = 13;
        AICA_MVOL = 0x8000 | vol;
    }
    return 0;
}

/* FIQ mask/unmask via CPSR bit 6 (mrs/orr/msr in the asm). */
static inline void fiq_disable(void)
{
    u32 r;
    __asm__ volatile("mrs %0, cpsr\n\torr %0, %0, #0x40\n\tmsr cpsr_c, %0" : "=r"(r) :: "memory");
}
static inline void fiq_enable(void)
{
    u32 r;
    __asm__ volatile("mrs %0, cpsr\n\tbic %0, %0, #0x40\n\tmsr cpsr_c, %0" : "=r"(r) :: "memory");
}



/* shared body of 0x6094 / 0x60D8 */
static u32 drv_soft_reset(void)
{
    aica_silence_all();
    clear_host_area();
    init_bank_table();
    evq_clear();
    init_midi_ports();
    clear_voices();
    seq_init();
    return fx_prg_clear();
}

/*
 * 0x6094 host_cmd_reinit (host command 0x8E; not sent by the R9 library)
 * Confidence: high
 * With FIQ masked: silence all channels, clear the host area, reinstall the
 * default banks, clear the event ring, port/channel state and voices, seq_init,
 * clear the FX program. FIQ is re-enabled. Returns 0 (from fx_prg_clear).
 * Notes: unlike drv_reset it does not call dsp_out_clear (0x8D0),
 * fx_status_init, 0x7778 or irq_init; MVOL and drv_status are untouched.
 * The asm uses r10 as scratch for the CPSR, clobbering the command pointer
 * that host_cmd_poll keeps on the stack (harmless).
 */
u32 host_cmd_reinit(void)
{
    u32 r;

    fiq_disable();
    r = drv_soft_reset();
    fiq_enable();
    return r;
}

/*
 * 0x60D8 host_cmd_reboot (host command 0x8F; not sent by the R9 library)
 * Confidence: high
 * Only if cmd[4..7] spell "SEGA": FIQ masked, drv_status = 0xFF000000, the
 * same reinit as host 0x8E, then drv_status = 0xFFFFFFFF and the ARM spins
 * forever (nop loop) with FIQ still masked - presumably so that the SH-4
 * can stop the ARM and download a new driver. Otherwise returns (r0 = the
 * last byte compared).
 */
void host_cmd_reboot(volatile HostCmd *cmd)
{
    if (cmd->arg[3] != 'S' || cmd->arg[4] != 'E' || cmd->arg[5] != 'G' || cmd->arg[6] != 'A')
        return;
    fiq_disable();
    drv_status = 0xFF000000u;
    drv_soft_reset();
    drv_status = 0xFFFFFFFFu;
    for (;;)
        __asm__ volatile("nop");
}

/*
 * 0x61D4 fx_prg_load_service
 * Confidence: high
 * Main loop, every 4 ms. If no external DSP control (fx_ext_active == 0) and
 * a load is queued (fx_load_req): clears the request, fades the outputs
 * out, clears the DSP and the ring buffer, writes RINGBUF = fx_rb_pending
 * (if nonzero), sets fx_busy = 0xFF, republishes fx_dsp_reg_base/size and
 * fx_ringbuf_reg, latches the program's pan-slot bytes (fx_pan_coef_base =
 * +0x24, fx_prg_b22 = +0x22, fx_pan_mode = +0x23), copies 0x300 words from
 * program +0x40 to 0x803000..0x803BFF (COEF, MADRS, 64 unused words, MPRO),
 * clears the ring buffer again, fades the outputs in, then fx_prg_cur =
 * fx_prg_sel and fx_busy = 0.
 * Notes: fx_load_req is cleared with a 32-bit store (also zeroes 0x13015-17).
 * The DSP keeps running from an all-zero program while the new one is
 * copied; outputs are faded out during that time. Each fade takes 16 steps
 * of 255 delay iterations: roughly 14k instructions per fade (sub-ms at the
 * nominal ARM clock; the real time depends on AICA RAM wait states).
 */
void fx_prg_load_service(void)
{
    const u32 *src;
    u32 i;

    if (fx_ext_active)
        return;
    if (!fx_load_req)
        return;
    SRAM(u32, 0x13014) = 0;                 /* word store clears fx_load_req */

    fx_out_fade_out();
    fx_dsp_clear();
    fx_ringbuf_clear();
    if (fx_rb_pending != 0)
        AICA_RINGBUF = fx_rb_pending;
    fx_busy = 0xFF;

    fx_dsp_reg_base = 0x00803000;
    fx_dsp_reg_size = 0xC00;
    fx_ringbuf_reg = fx_rb_pending;
    {
        const FxProgram *p = (const FxProgram *)fx_prg_pending;
        fx_pan_coef_base = p->pan_coef_base;
        fx_prg_b22 = p->_22;
        fx_pan_mode = p->pan_mode;
        src = p->coef;
    }
    for (i = 0; i < 0x300; i++)             /* COEF, MADRS, gap, MPRO */
        AICA_DSP_COEF(i) = src[i];

    fx_ringbuf_clear();
    fx_out_fade_in();
    fx_prg_cur = fx_prg_sel;
    fx_busy = 0;
}

/*
 * 0x62EC fx_ringbuf_clear
 * Confidence: high
 * Fills the SFPW ring buffer (fx_wrk_addr, fx_wrk_size bytes) with
 * 0x60006000, i.e. 16-bit DSP-float zeros. Does nothing if both the address
 * and the size are 0.
 * Notes: do/while loop on size>>2: a nonzero address with size < 4 wraps the
 * counter and overwrites all of memory; address 0 with a nonzero size wipes
 * the driver itself. Neither happens with valid SFPW entries.
 */
void fx_ringbuf_clear(void)
{
    volatile u32 *p = (volatile u32 *)fx_wrk_addr;
    u32 n = fx_wrk_size;

    if ((u32)p == 0 && n == 0)
        return;
    n >>= 2;
    do {
        *p++ = 0x60006000u;
    } while (--n);
}

/*
 * 0x6328 fx_out_fade_out
 * Confidence: high
 * Saves the 16 DSP output registers (0x802000..0x80203C) in fx_out_save,
 * then 16 times: decrement every nonzero EFSDL byte (+1 of each register)
 * and wait 255 loop iterations. Ends with all 16 EFSDL = 0.
 * Notes: EXTS/CDDA outputs (0x802040/44) are not faded.
 */
void fx_out_fade_out(void)
{
    u32 i, step, l;

    for (i = 0; i < NUM_DSP_OUT; i++)
        fx_out_save[i] = AICA_DSP_OUT(i);
    for (step = 0; step < 16; step++) {
        for (i = 0; i < NUM_DSP_OUT; i++) {
            l = DSP_OUT_LEV_B(i);
            if (l != 0)
                DSP_OUT_LEV_B(i) = (u8)(l - 1);
        }
        fx_delay();
    }
}

/*
 * 0x6388 fx_out_fade_in
 * Confidence: high
 * Restores the EFPAN bytes from fx_out_save, then for level 0..15: every
 * output whose saved EFSDL >= level gets EFSDL = level; wait 255 iterations.
 * Ends with the saved levels.
 */
void fx_out_fade_in(void)
{
    u32 i, lev;

    for (i = 0; i < NUM_DSP_OUT; i++)
        DSP_OUT_PAN_B(i) = (u8)fx_out_save[i];
    for (lev = 0; lev < 16; lev++) {
        for (i = 0; i < NUM_DSP_OUT; i++)
            if (((fx_out_save[i] >> 8) & 0xFF) >= lev)
                DSP_OUT_LEV_B(i) = (u8)lev;
        fx_delay();
    }
}

/* 0x6454 port_clear_play (the last routine before 0x6470) is implemented in port.c. */
