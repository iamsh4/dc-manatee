/*
 * aica.h - AICA sound chip registers as seen from the ARM7DI side.
 *
 * ARM memory map:
 *   0x00000000-0x001FFFFF  sound RAM (2 MB)
 *   0x00800000-0x0080FFFF  AICA registers
 *
 * Bit layouts follow the AICA register documentation / MAME aica.cpp.
 * Register writes from the driver are 32-bit stores; only the low 16 bits exist.
 */
#ifndef AICA_H
#define AICA_H

#include "types.h"

#define AICA_REG(off) (*(volatile u32 *)(0x00800000u + (off)))
#define AICA_REG8(off) (*(volatile u8 *)(0x00800000u + (off)))

/* ---- Per-channel registers: 64 channels, stride 0x80 ---- */
#define AICA_NUM_CHANNELS 64

typedef struct AicaChannel {
    u32 ctl_sa_hi;   /* 0x00 KYONEX15 KYONB14 SSCTL10 LPCTL9 PCMS8:7 SA22:16 */
    u32 sa_lo;       /* 0x04 SA15:0 */
    u32 lsa;         /* 0x08 loop start (samples) */
    u32 lea;         /* 0x0C loop end (samples) */
    u32 env_ad;      /* 0x10 D2R15:11 D1R10:6 AR4:0 */
    u32 env_dr;      /* 0x14 LPSLNK14 KRS13:10 DL9:5 RR4:0 */
    u32 pitch;       /* 0x18 OCT14:11 FNS10:0 */
    u32 lfo;         /* 0x1C LFORE15 LFOF14:10 PLFOWS9:8 PLFOS7:5 ALFOWS4:3 ALFOS2:0 */
    u32 dsp_send;    /* 0x20 IMXL7:4 ISEL3:0 */
    u32 direct;      /* 0x24 DISDL11:8 DIPAN4:0 */
    u32 tl_q;        /* 0x28 TL15:8 VOFF6 LPOFF5 Q4:0 */
    u32 flv[5];      /* 0x2C-0x3C filter levels */
    u32 fenv_a;      /* 0x40 FAR12:8 FD1R4:0 */
    u32 fenv_r;      /* 0x44 FD2R12:8 FRR4:0 */
    u32 _pad[14];
} AicaChannel;

#define AICA_CH ((volatile AicaChannel *)0x00800000u)

/* ctl_sa_hi bits */
#define AICA_KYONEX   0x8000u  /* execute key on/off for all channels */
#define AICA_KYONB    0x4000u  /* key on */
#define AICA_SSCTL    0x0400u
#define AICA_LPCTL    0x0200u  /* loop enable */
#define AICA_PCMS_MASK 0x0180u
#define AICA_SA_HI_MASK 0x007Fu

/* ---- DSP output mixer (EFSDL/EFPAN) for 16 EFREG + 2 EXTS ---- */
#define AICA_DSP_OUT(n)  AICA_REG(0x2000 + (n) * 4)

/* ---- Common registers ---- */
#define AICA_MVOL     AICA_REG(0x2800)  /* MONO15 MEM8MB9 DAC18B8 VER7:4 MVOL3:0 */
#define AICA_RINGBUF  AICA_REG(0x2804)  /* RBL14:13 RBP12:0 (RBP in 2 KB units = addr>>11) */
#define AICA_MIDI_IN  AICA_REG(0x2808)  /* MOFUL12 MOEMP11 MIOVF10 MIFUL9 MIEMP8 MIBUF7:0 */
#define AICA_MSLC     AICA_REG(0x280C)  /* AFSEL14 MSLC13:8 MOBUF7:0 */
#define AICA_MSLC_B   AICA_REG8(0x280D) /* byte access to MSLC */
#define AICA_MON_EG   AICA_REG(0x2810)  /* LP15 SGC14:13 EG12:0 of monitored channel */
#define AICA_MON_CA   AICA_REG(0x2814)  /* current address of monitored channel */
#define AICA_TIMA     AICA_REG(0x2890)  /* TACTL10:8 TIMA7:0 */
#define AICA_TIMB     AICA_REG(0x2894)
#define AICA_TIMC     AICA_REG(0x2898)
#define AICA_SCIEB    AICA_REG(0x289C)  /* ARM interrupt enable */
#define AICA_SCIPD    AICA_REG(0x28A0)  /* ARM interrupt pending */
#define AICA_SCIRE    AICA_REG(0x28A4)  /* ARM interrupt reset */
#define AICA_SCILV0   AICA_REG8(0x28A8)
#define AICA_SCILV1   AICA_REG8(0x28AC)
#define AICA_SCILV2   AICA_REG8(0x28B0)
#define AICA_MCIEB    AICA_REG(0x28B4)  /* SH-4 interrupt enable */
#define AICA_MCIPD    AICA_REG(0x28B8)
#define AICA_MCIRE    AICA_REG(0x28BC)
#define AICA_INTREQ   AICA_REG(0x2D00)  /* current FIQ level L2:0 */
#define AICA_INTCLR   AICA_REG(0x2D04)  /* write 1 to release FIQ */

/* MIDI_IN bits */
#define AICA_MIDI_MIEMP 0x0100u
#define AICA_MIDI_MIOVF 0x0400u

/* SCIEB/SCIPD bit numbers */
#define AICA_INT_MIDI_IN  (1u << 3)
#define AICA_INT_DMA      (1u << 4)
#define AICA_INT_HOST      (1u << 5)
#define AICA_INT_TIMER_A  (1u << 6)
#define AICA_INT_TIMER_B  (1u << 7)
#define AICA_INT_TIMER_C  (1u << 8)

/* ---- DSP ---- */
#define AICA_DSP_COEF(n)  AICA_REG(0x3000 + (n) * 4)  /* 128 x 13-bit */
#define AICA_DSP_MADRS(n) AICA_REG(0x3200 + (n) * 4)  /* 64 x 16-bit */
#define AICA_DSP_MPRO(n)  AICA_REG(0x3400 + (n) * 4)  /* 128 steps x 4 x 16-bit words */
#define AICA_DSP_TEMP(n)  AICA_REG(0x4000 + (n) * 4)
#define AICA_DSP_MEMS(n)  AICA_REG(0x4400 + (n) * 4)
#define AICA_DSP_MIXS(n)  AICA_REG(0x4500 + (n) * 4)
#define AICA_DSP_EFREG(n) AICA_REG(0x4580 + (n) * 4)
#define AICA_DSP_EXTS(n)  AICA_REG(0x45C0 + (n) * 4)

#endif
