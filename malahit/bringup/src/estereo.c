#include <math.h>
#include <string.h>
#include "estereo.h"
#include "arm_math.h"      /* CMSIS-DSP: the biquads, as upstream's path uses them */
#include "demod_am.h"      /* ../../User - demod_am_get_audio_bw() */

/*
 * See estereo.h for what this is and why it is built round upstream's
 * mono path instead of beside it.
 *
 * THE MULTIPLEX, as broadcast (ITU-R BS.450):
 *
 *     m(t) = (L+R)/2 + p sin(wt) + (L-R)/2 sin(2wt) [+ RDS at 3w]
 *
 * with w = 2 pi 19 kHz. Correlating m with e^{-j theta}, theta the local
 * 19 kHz oscillator's phase, gives (p/2) e^{j psi} over a block where the
 * pilot is cos(theta + psi); the pilot is then cos(theta_p), and
 * sin(2wt) - the subcarrier, in phase with the pilot by the standard - is
 * cos(2 theta_p + pi/2) = -Im(e^{j 2 theta_p}). Multiplying m by twice
 * that leaves (L-R)/2 at baseband, and the products at 19, 23-53, 57 and
 * 76 kHz go in the same low-pass that takes the pilot out of M.
 *
 * The discriminator's sign does not matter: inverted, m is -m, the pilot's
 * phase moves by pi, the reference by 2 pi - not at all - so S is -S and
 * M is -M, and L and R are both inverted, not swapped.
 */

/* Upstream's WFM de-emphasis and audio low-pass, demod_am.c: the same
 * numbers, because S has to go through exactly what M goes through or
 * the two do not cancel in the wrong channel. Copied, not shared: they
 * are static there. If upstream changes them, the bench's separation
 * figure (deepsdr-ui `make wfmestereo`) is what notices. */
#define ALPF_STAGES  2U
static const float32_t k_alpf[3][ALPF_STAGES * 5U] = {
    {   /* AUDIO_BW_4K0: WFM_ALPF_WIDE_COEFFS, 15 kHz */
        0.0020570668f, 0.0041141336f, 0.0020570668f,  1.2287186181f, -0.3932293820f,
        1.0000000000f, 2.0000000000f, 1.0000000000f,  1.4942806865f, -0.6943470431f
    },
    {   /* AUDIO_BW_2K3: WFM_ALPF_NORM_COEFFS, 8 kHz */
        0.0002131387f, 0.0004262775f, 0.0002131387f,  1.5590543011f, -0.6140517819f,
        1.0000000000f, 2.0000000000f, 1.0000000000f,  1.7577536095f, -0.8197604429f
    },
    {   /* AUDIO_BW_1K8: WFM_ALPF_NARROW_COEFFS, 4 kHz */
        0.0000155517f, 0.0000311034f, 0.0000155517f,  1.7695043485f, -0.7847733318f,
        1.0000000000f, 2.0000000000f, 1.0000000000f,  1.8885559539f, -0.9048522288f
    },
};
#define DEEMPH_ALPHA  0.9010751057f     /* WFM_DEEMPH_ALPHA: 50 us at 192 kHz */
/*
 * AND UPSTREAM'S DC BLOCKER, WFM_DCB_R - found by the bench, 22/11/2026.
 * It acts on the multiplex BEFORE this file sees it, so M has been
 * through it and S - which was up at 38 kHz then - has not: a 15 Hz
 * high-pass is 0.9 degrees of phase at 1 kHz, and that alone held the
 * separation at 40 dB (57 at 3 kHz). S gets the same high-pass after
 * its demodulation, and the two match again.
 */
#define DCB_R         0.9995f

#define PILOTO_HZ     19000.0f
/*
 * THE PILOT IS FOUND THROUGH A 200 Hz LOW-PASS, not a block average -
 * 22/11/2026, from the bench. A 512-sample average is a sinc whose
 * sidelobes let the programme in: the nearest of it is 4 kHz from the
 * pilot (15 kHz of L+R, 23 kHz of L-R), and through the sinc it held a
 * NOISELESS pilot's SNR at 29 dB - loud music would have moved the blend
 * on a strong station. Two poles at 200 Hz put that 4 kHz 52 dB down, so
 * what the SNR measures is noise.
 */
#define PILOTO_LP_HZ  200.0f
/* What a hertz of deviation is in the multiplex: upstream's discriminator
 * is atan2() in radians a sample times WFM_DISC_GAIN (7300), so one hertz
 * is 7300 x 2 pi / 192000 = 0.239. Copied, as the filters are. */
#define MPX_POR_HZ    (7300.0f * 6.28318531f / ESTEREO_FS_HZ)
#define SUAVE         0.15f             /* the phasor across blocks: ~17 ms */
#define MEZCLA_PASO   0.05f             /* the blend moves 5 % a block at most */

typedef struct {
    arm_biquad_casd_df1_inst_f32 alpf;
    float32_t estado[ALPF_STAGES * 4U];
    float     deemph;
    float     dcb_x1, dcb_y1;           /* S only: see DCB_R */
} ruta_t;

/*
 * IN THE DTCM, WHAT IS TOUCHED EVERY SAMPLE - 22/11/2026, measured. This
 * image runs with the D-cache off (WORKING.md, "The caches"), so every
 * float in AXI SRAM is a slow bus access: in AXI the decoder cost 6 ms
 * of the 42.7 ms block and the screen dropped to 9 frames a second.
 * Upstream keeps its own hot state in .tcmram for the same reason, and
 * this port puts that section in the DTCM (stm32h743_*.ld) - NOLOAD,
 * zeroed by the startup, so nothing with an initialiser goes there.
 */
#define TCM __attribute__((section(".tcmram")))

static ruta_t   s_m TCM, s_s TCM;       /* our copy of M, and S */
static uint8_t  s_bw = 0xFFU;           /* the audio_bw the filters are on */

static float    s_zr = 1.0f, s_zi;      /* the 19 kHz oscillator, e^{j theta} */
static float    s_lpa;                  /* the pilot low-pass's coefficient */
static float    s_i1, s_q1, s_i2, s_q2; /* its two poles, I and Q */
static float    s_rr, s_ri;             /* its step, e^{j w/fs} */
static float    s_pr, s_pi;             /* the smoothed pilot phasor */
static float    s_ruido = 1e-12f;       /* its scatter, smoothed */
static float    s_ur = 1.0f, s_ui;      /* (P/|P|)^2: the reference's phase */
static float    s_snr_db;
static float    s_mezcla;
static float    s_g;
static float    s_piloto_hz = PILOTO_HZ;
static float    s_resto_db;
static uint32_t s_cic_mpx, s_cic_apl;   /* cycles, for the report */

/* The cycle counter, where there is one: the radio's DWT; 0 on a PC. */
#if defined(__ARM_ARCH)
#include "stm32h7xx.h"
#define CICLOS()  (DWT->CYCCNT)
#else
#define CICLOS()  0U
#endif
static float    s_dev_hz;               /* the pilot's deviation */
static float    s_fase_ant;
static uint8_t  s_on = 1U;
static volatile uint8_t s_pide_reset;

static float    s_mb[ESTEREO_N_MAX] TCM, s_sb[ESTEREO_N_MAX] TCM;
static uint32_t s_listo;                /* frames of this call in s_mb/s_sb */

static void ruta_init(ruta_t *r, uint8_t bw)
{
    memset(r->estado, 0, sizeof r->estado);
    r->deemph = 0.0f;
    r->dcb_x1 = r->dcb_y1 = 0.0f;
    arm_biquad_cascade_df1_init_f32(&r->alpf, ALPF_STAGES,
                                    (float32_t *)k_alpf[bw], r->estado);
}

void estereo_reset(void)
{
    float w = 2.0f * 3.14159265358979f * PILOTO_HZ / ESTEREO_FS_HZ;

    s_rr = cosf(w);
    s_ri = sinf(w);
    s_lpa = 1.0f - expf(-2.0f * 3.14159265358979f * PILOTO_LP_HZ / ESTEREO_FS_HZ);
    s_i1 = s_q1 = s_i2 = s_q2 = 0.0f;
    s_zr = 1.0f; s_zi = 0.0f;
    s_pr = s_pi = 0.0f;
    s_ruido = 1e-12f;
    s_ur = 1.0f; s_ui = 0.0f;
    s_snr_db = 0.0f;
    s_mezcla = 0.0f;
    s_g = 0.0f;
    s_piloto_hz = PILOTO_HZ;
    s_fase_ant = 0.0f;
    s_bw = 0xFFU;
    s_listo = 0U;
}

void    estereo_pon(uint8_t on) { s_on = on ? 1U : 0U; }
void    estereo_pide_reset(void) { s_pide_reset = 1U; }
uint8_t estereo_puesto(void)    { return s_on; }

void estereo_mpx(const float *mpx, uint32_t n)
{
    uint32_t c0 = CICLOS();
    float    zr = s_zr, zi = s_zi, ar = 0.0f, ai = 0.0f;
    float    i1 = s_i1, q1 = s_q1, i2 = s_i2, q2 = s_q2, a = s_lpa;
    /* Locals, not the statics: the loop stores floats, and the compiler
     * cannot know they are not these - it would reload all four on
     * every sample. */
    float    rr = s_rr, ri = s_ri, ur = s_ur, ui = s_ui;
    uint32_t k;
    uint8_t  bw = (uint8_t)demod_am_get_audio_bw();

    if (s_pide_reset) {
        s_pide_reset = 0U;
        estereo_reset();
        zr = s_zr; zi = s_zi;
        i1 = q1 = i2 = q2 = 0.0f;
        a = s_lpa; rr = s_rr; ri = s_ri; ur = s_ur; ui = s_ui;
    }
    if (!s_on) {                        /* mono by the owner's switch: no cost */
        s_mezcla = 0.0f;
        s_listo  = 0U;
        s_cic_mpx = CICLOS() - c0;
        return;
    }
    if (n > ESTEREO_N_MAX) { n = ESTEREO_N_MAX; }
    if (bw > 2U) { bw = 0U; }
    if (bw != s_bw) {                   /* a new low-pass: both paths from zero */
        ruta_init(&s_m, bw);
        ruta_init(&s_s, bw);
        s_bw = bw;
    }

    /*
     * One pass: the pilot's correlation for the NEXT block's reference,
     * and S for this one with the reference the blocks before it found -
     * the phase moves a fraction of a degree a block, so a block late is
     * nothing.
     */
    for (k = 0; k < n; k++) {
        float x = mpx[k];
        float wr, wi, r;

        i1 += a * ( x * zr - i1);
        q1 += a * (-x * zi - q1);
        i2 += a * (i1 - i2);
        q2 += a * (q1 - q2);
        ar += i2;
        ai += q2;
        /* e^{j 2 theta} times (P/|P|)^2, and the subcarrier is -Im of it */
        wr = zr * zr - zi * zi;
        wi = 2.0f * zr * zi;
        r  = -(wr * ui + wi * ur);
        s_sb[k] = 2.0f * r * x;
        s_mb[k] = x;
        {
            float t = zr * rr - zi * ri;
            zi = zr * ri + zi * rr;
            zr = t;
        }
    }
    s_i1 = i1; s_q1 = q1; s_i2 = i2; s_q2 = q2;
    {   /* keep the oscillator on the unit circle */
        float g = 1.5f - 0.5f * (zr * zr + zi * zi);
        s_zr = zr * g;
        s_zi = zi * g;
    }

    /* The pilot: this block's phasor (the low-passed one, averaged over
     * the block), the smoothed one, the scatter. */
    {
        float inv = 1.0f / (float)n, br = ar * inv, bi = ai * inv;
        float dr = br - s_pr, di = bi - s_pi, mag2;

        s_ruido += SUAVE * ((dr * dr + di * di) - s_ruido);
        s_pr += SUAVE * dr;
        s_pi += SUAVE * di;
        mag2 = s_pr * s_pr + s_pi * s_pi;
        s_snr_db = 10.0f * log10f((mag2 + 1e-12f) / (s_ruido + 1e-12f));
        /* the phasor is half the pilot's amplitude */
        s_dev_hz = 2.0f * sqrtf(mag2) / MPX_POR_HZ;
        if (mag2 > 1e-12f) {
            float m = 1.0f / sqrtf(mag2), pr = s_pr * m, pim = s_pi * m;
            float fase = atan2f(pim, pr);
            float df = fase - s_fase_ant;

            s_ur = pr * pr - pim * pim;
            s_ui = 2.0f * pr * pim;

            /* how fast the phasor turns: the pilot's offset from 19 kHz */
            if (df >  3.14159265f) { df -= 6.28318531f; }
            if (df < -3.14159265f) { df += 6.28318531f; }
            s_piloto_hz = PILOTO_HZ + df * ESTEREO_FS_HZ / (6.28318531f * (float)n);
            s_fase_ant = fase;
        }
    }

    /* S through upstream's DC blocker, which M has had already; then M
     * and S through its de-emphasis and low-pass, in its order. */
    {
        float ym = s_m.deemph, ys = s_s.deemph;
        float x1 = s_s.dcb_x1, y1 = s_s.dcb_y1;

        for (k = 0; k < n; k++) {
            float d = s_sb[k] - x1 + DCB_R * y1;

            x1 = s_sb[k];
            y1 = d;
            ym = DEEMPH_ALPHA * ym + (1.0f - DEEMPH_ALPHA) * s_mb[k];
            ys = DEEMPH_ALPHA * ys + (1.0f - DEEMPH_ALPHA) * d;
            s_mb[k] = ym;
            s_sb[k] = ys;
        }
        s_s.dcb_x1 = x1;
        s_s.dcb_y1 = y1;
        s_m.deemph = ym;
        s_s.deemph = ys;
    }
    arm_biquad_cascade_df1_f32(&s_m.alpf, s_mb, s_mb, n);
    arm_biquad_cascade_df1_f32(&s_s.alpf, s_sb, s_sb, n);

    /* How much stereo: the pilot's SNR, mapped and slewed. */
    {
        float objetivo = (s_snr_db - ESTEREO_SNR_OFF_DB)
                       / (ESTEREO_SNR_FULL_DB - ESTEREO_SNR_OFF_DB);

        if (objetivo < 0.0f) { objetivo = 0.0f; }
        if (objetivo > 1.0f) { objetivo = 1.0f; }
        if (!s_on || s_dev_hz < ESTEREO_PILOTO_MIN_HZ) { objetivo = 0.0f; }
        if (objetivo > s_mezcla + MEZCLA_PASO)      { s_mezcla += MEZCLA_PASO; }
        else if (objetivo < s_mezcla - MEZCLA_PASO) { s_mezcla -= MEZCLA_PASO; }
        else                                        { s_mezcla = objetivo; }
    }
    s_listo = n;
    s_cic_mpx = CICLOS() - c0;
}

static int16_t sat16(float v)
{
    if (v >  32767.0f) { return  32767; }
    if (v < -32768.0f) { return -32768; }
    return (int16_t)v;
}

int estereo_aplica(const int16_t *src, int16_t *dst, uint32_t n)
{
    uint32_t c0 = CICLOS();
    float    om = 0.0f, mm = 0.0f, oo = 0.0f, gs;
    uint32_t k;

    if (s_listo == 0U || s_listo != n) { s_listo = 0U; return 0; }   /* not this call's */
    s_listo = 0U;

    /* g: upstream's output, left channel, against our M - and, from the
     * same three sums, the residual: what of upstream's M our copy does
     * not explain, sum (o - gm)^2 = oo - 2g om + g^2 mm. A silent block -
     * squelch, a mute - gives g = 0, and S goes with it. */
    for (k = 0; k < n; k++) {
        float o = (float)src[2U * k], m = s_mb[k];

        om += o * m;
        mm += m * m;
        oo += o * o;
    }
    s_g = (mm > 1e-6f) ? (om / mm) : 0.0f;
    if (s_g < 0.0f) { s_g = 0.0f; }
    {
        float e2 = oo - 2.0f * s_g * om + s_g * s_g * mm;

        if (e2 < 0.0f) { e2 = 0.0f; }
        s_resto_db = 10.0f * log10f((e2 + 1e-3f) / (oo + 1e-3f));
    }

    if (s_mezcla <= 0.0f) { s_cic_apl = CICLOS() - c0; return 0; }
    gs = s_g * s_mezcla;
    for (k = 0; k < n; k++) {
        float m = (float)src[2U * k], d = gs * s_sb[k];

        dst[2U * k]      = sat16(m + d);
        dst[2U * k + 1U] = sat16(m - d);
    }
    s_cic_apl = CICLOS() - c0;
    return 1;
}

void estereo_estado(estereo_estado_t *out)
{
    out->piloto    = (uint8_t)(s_snr_db >= ESTEREO_SNR_OFF_DB
                               && s_dev_hz >= ESTEREO_PILOTO_MIN_HZ);
    out->piloto_dev_hz = s_dev_hz;
    out->snr_db    = s_snr_db;
    out->mezcla    = s_mezcla;
    out->ganancia  = s_g;
    out->piloto_hz = s_piloto_hz;
    out->resto_db  = s_resto_db;
    out->ciclos_mpx    = s_cic_mpx;
    out->ciclos_aplica = s_cic_apl;
}
