/*
 * monitor.c - round-robin sampling of AICA channel envelope state.
 *
 * The AICA exposes the envelope (EG) and current address of exactly one
 * channel at a time, selected by MSLC. The driver advances MSLC by one channel
 * per 1 ms tick over the 48 driver voices, so each voice is observed about
 * every 48 ms. The result (Voice.level) lets the voice allocator find voices
 * that have decayed to silence.
 */
#include "manatee.h"

static u8 monitor_slot;   /* 0x8CC: channel currently selected in MSLC */

/*
 * 0x085C voice_monitor_sample
 * Confidence: high
 * Reads EG/SGC of the monitored channel and stores an audibility estimate in
 * g_voice[monitor_slot].level:
 *   SGC != 3 (attack/decay/sustain)       -> 0xFF
 *   SGC == 3 (release), EG >= 0x100       -> 0x00 (attenuated beyond ~96 dB: silent)
 *   SGC == 3 (release), EG <  0x100       -> 0xFF - EG
 * Notes: EG is attenuation (0 = loudest). The value read reflects the channel
 * selected by the *previous* voice_monitor_next call; the main loop always
 * calls sample before next so the pairing is consistent.
 */
void voice_monitor_sample(void)
{
    u32 eg = AICA_MON_EG;
    u32 lvl = (eg & 0x1F00) ? 0xFF : (eg & 0xFF);

    lvl ^= 0xFF;
    if (((eg >> 13) & 3) != 3)
        lvl = 0xFF;
    g_voice[monitor_slot].level = (u8)lvl;
}

/*
 * 0x08A0 voice_monitor_next
 * Confidence: high
 * Advances the monitored channel 0 -> 47 -> 0 and writes it to MSLC (byte
 * store to 0x80280D, i.e. bits 13:8 of the MSLC register; AFSEL stays 0 so the
 * monitor shows the envelope rather than the filter envelope).
 */
void voice_monitor_next(void)
{
    u32 n = monitor_slot + 1;

    if (n == NUM_VOICES)
        n = 0;
    monitor_slot = (u8)n;
    AICA_MSLC_B = (u8)(n & 0x3F);
}
