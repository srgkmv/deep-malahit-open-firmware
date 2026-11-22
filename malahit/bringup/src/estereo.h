#ifndef MALAHIT_ESTEREO_H
#define MALAHIT_ESTEREO_H

#include <stdint.h>

/*
 * THE FM STEREO DECODER - 22/11/2026, the owner's: upstream's WFM is mono
 * by design (demod_am.h, its WFM note), and this is not upstream's.
 *
 * It does not replace upstream's WFM path; it ADDS to it. Upstream's
 * output is the mono sum, M = (L + R) / 2, with everything upstream does
 * to it - squelch, de-emphasis, the 15/8/4 kHz low-pass, its AGC, the
 * short mutes, the pips. This decodes the difference, S = (L - R) / 2,
 * out of the same multiplex, through the SAME de-emphasis and low-pass,
 * and hands back L = M + gS and R = M - gS, where g is the gain
 * upstream's AGC applied to M in this very block - found by regressing
 * upstream's output on this file's own copy of M. So a squelch, a mute
 * or an AGC step in upstream's path does the same to S, with no copy of
 * any of them here.
 *
 * The 38 kHz reference is the pilot's phase doubled. The pilot's phasor
 * is the multiplex correlated with a 19 kHz oscillator over each block
 * and smoothed across blocks; its scatter from block to block is the
 * pilot's noise, and the ratio of the two is what decides stereo: below
 * ESTEREO_SNR_OFF_DB mono, above ESTEREO_SNR_FULL_DB full stereo, a blend
 * in between - so a weak station fades to mono instead of to hiss.
 *
 * Plain C, no peripheral: the deepsdr-ui bench compiles this file as it
 * is, behind upstream's real demod_wfm_process_raw().
 *
 * Both entry points run in the block interrupt, in this order, once per
 * 512-frame call of upstream's WFM hook:
 *   estereo_mpx()     the raw multiplex, from where upstream hands it to
 *                     the RDS decoder (demod.c wraps rds_feed())
 *   estereo_aplica()  upstream's stereo frames, in the audio sink
 */
#define ESTEREO_FS_HZ        192000.0f
#define ESTEREO_N_MAX        512U
#define ESTEREO_SNR_OFF_DB   12.0f      /* pilot SNR below which: mono */
#define ESTEREO_SNR_FULL_DB  24.0f      /* and above which: full stereo */
/* And a pilot at all: its deviation, which BS.450 puts at 8-10 % of the
 * 75 kHz - 6 to 7.5 kHz. The SNR alone is not enough: a 1 kHz tone's 19th
 * harmonic out of the discriminator is a perfectly steady 19 kHz line, so
 * its "SNR" is high, and it is a hundred times too weak to be a pilot. */
#define ESTEREO_PILOTO_MIN_HZ 2500.0f

void estereo_reset(void);
/* The same, from the display loop: done by the next estereo_mpx(), in the
 * block interrupt, so no block is ever half reset. */
void estereo_pide_reset(void);
void estereo_mpx(const float *mpx, uint32_t n);
/* Upstream's n stereo frames in `src`, the stereo ones out in `dst`.
 * Returns 0 - and writes nothing - when there is no stereo to make (no
 * pilot, the switch off, or not this call's multiplex): then the caller
 * copies `src` itself. One read of `src` and one write of `dst` a frame:
 * `dst` is the SAI ring, uncached. */
int  estereo_aplica(const int16_t *src, int16_t *dst, uint32_t n);

/* The owner's switch: 0 forces mono, 1 decodes when there is a pilot. */
void    estereo_pon(uint8_t on);
uint8_t estereo_puesto(void);

/* What it is doing, for the header and the console. */
typedef struct {
    uint8_t piloto;     /* 1 = a pilot above ESTEREO_SNR_OFF_DB */
    float   snr_db;     /* the pilot's, from block to block */
    float   mezcla;     /* 0 mono .. 1 full stereo, what is applied */
    float   ganancia;   /* g: upstream's gain on M, as found */
    float   piloto_hz;  /* the pilot's frequency as this oscillator sees it */
    float   piloto_dev_hz; /* the pilot's deviation, Hz (6-7.5 kHz on air) */
    float   resto_db;   /* how far upstream's M is from this file's copy of
                           it after g: the match S depends on, in dB */
    uint32_t ciclos_mpx, ciclos_aplica;   /* the last block's, CPU cycles */
} estereo_estado_t;
void estereo_estado(estereo_estado_t *out);

#endif /* MALAHIT_ESTEREO_H */
