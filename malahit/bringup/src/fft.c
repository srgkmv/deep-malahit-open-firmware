#include "fft.h"

static float s_twr[FFT_N / 2U];     /* cos(-2*pi*k/N) */
static float s_twi[FFT_N / 2U];     /* sin(-2*pi*k/N) */
static float s_win[FFT_N];          /* Hann */
static uint16_t s_rev[FFT_N];       /* bit-reversal permutation */

/*
 * sqrt in double, by Newton, because there is no libm.
 *
 * The compiler would happily emit the Cortex-M7's own VSQRT.F64 for a
 * call to sqrt(), but only with the right combination of -ffast-math
 * style flags; without them it emits a call to a libm this image does
 * not link. Newton from a seed of 1 converges in a handful of
 * iterations for the arguments here, which are all in [0, 2], and this
 * runs exactly nine times in the life of the program.
 */
static double dsqrt(double x)
{
    if (x <= 0.0) { return 0.0; }

    double r = 1.0;

    for (uint32_t i = 0; i < 24U; i++) {
        r = 0.5 * (r + x / r);
    }
    return r;
}

float fft_log2f(float x)
{
    /*
     * Pull the exponent out of the IEEE-754 bits and approximate the
     * rest on the mantissa, which is in [1, 2).
     *
     * THE POLYNOMIAL IS FITTED, NOT REMEMBERED. The first version of
     * this function carried a cubic written from memory, and it was
     * wrong where it mattered least visibly and most: exact at powers
     * of two, so log2(1), log2(8) and log2(0.5) all came out perfect,
     * and 1.5 bits out at the top of the mantissa range. Every spectrum
     * level was then 4.5 dB low, uniformly, which is exactly the kind
     * of error that gets absorbed into a "calibration constant" and
     * never found.
     *
     * This one is a least-squares degree-4 fit on [1, 2) with a
     * measured maximum error of 2e-4 bits - about 0.0006 dB once
     * scaled, against a display that resolves about 0.75 dB a pixel.
     */
    union { float f; uint32_t u; } v;

    if (x <= 0.0f) { return -200.0f; }

    v.f = x;

    int32_t e = (int32_t)((v.u >> 23) & 0xFFU) - 127;

    v.u = (v.u & 0x007FFFFFU) | 0x3F800000U;    /* mantissa into [1,2) */

    float m = v.f;
    float p = ((((-0.079150366f * m + 0.628815729f) * m
                 - 2.081060203f) * m + 4.028372767f) * m)
              - 2.496773768f;

    return (float)e + p;
}

void fft_init(void)
{
    /*
     * Halve pi down to 2*pi/N. Nine halvings for N = 1024:
     * pi, pi/2, pi/4 ... pi/512 = 2*pi/1024.
     */
    double c = -1.0;        /* cos(pi) */
    double s =  0.0;        /* sin(pi) */

    for (uint32_t k = 0; k < FFT_LOG2N - 1U; k++) {
        double cn = dsqrt((1.0 + c) * 0.5);
        double sn = dsqrt((1.0 - c) * 0.5);
        c = cn;
        s = sn;
    }
    /* c, s now hold cos and sin of 2*pi/N, both positive. */

    /*
     * Twiddles are e^(-j2*pi*k/N), so the imaginary part is negative.
     * Built by repeated rotation in double, then stored as float.
     */
    double wr = 1.0, wi = 0.0;

    for (uint32_t k = 0; k < FFT_N / 2U; k++) {
        s_twr[k] = (float)wr;
        s_twi[k] = (float)(-wi);

        double nr = wr * c - wi * s;
        double ni = wr * s + wi * c;
        wr = nr;
        wi = ni;
    }

    /*
     * Hann window, from the same recurrence - 0.5 - 0.5*cos(2*pi*n/N).
     *
     * A window is not optional here. The capture is a rectangular slice
     * of a continuous signal, and a carrier that does not land exactly
     * on a bin centre spreads skirts across the whole display at about
     * -13 dB without one. Hann puts them at -31 dB and falling, which
     * is the difference between a spectrum and a smear. It costs
     * resolution - the main lobe is two bins wide instead of one - and
     * that is a good trade for a panoramic display.
     */
    wr = 1.0; wi = 0.0;
    for (uint32_t n = 0; n < FFT_N; n++) {
        s_win[n] = (float)(0.5 - 0.5 * wr);

        double nr = wr * c - wi * s;
        double ni = wr * s + wi * c;
        wr = nr;
        wi = ni;
    }

    /* Bit-reversal permutation, computed once rather than per transform. */
    for (uint32_t i = 0; i < FFT_N; i++) {
        uint32_t r = 0;
        uint32_t v = i;
        for (uint32_t b = 0; b < FFT_LOG2N; b++) {
            r = (r << 1) | (v & 1U);
            v >>= 1;
        }
        s_rev[i] = (uint16_t)r;
    }
}

void fft_load(const int16_t *iq, float *re, float *im)
{
    /*
     * REMOVE DC FIRST, AND MEAN IT.
     *
     * Step 5 measured the mixer's LO leakage at 4 dB ABOVE the wanted
     * carrier. Left in, it is the tallest line on the display, and
     * worse, the window's skirts around something that large bury
     * everything within a few bins of centre - which is where a
     * zero-IF receiver puts whatever it is tuned to.
     *
     * Subtracting the block mean is the cheap version and is enough for
     * a display. It is not enough for a demodulator, which will want
     * the tuner's own DC calibration or a high-pass in the DSP chain.
     */
    int32_t sq = 0, si = 0;

    for (uint32_t n = 0; n < FFT_N; n++) {
        sq += iq[n * 2U];
        si += iq[n * 2U + 1U];
    }

    float mq = (float)sq / (float)FFT_N;
    float mi = (float)si / (float)FFT_N;

    for (uint32_t n = 0; n < FFT_N; n++) {
        float w = s_win[n];
        im[n] = ((float)iq[n * 2U]      - mq) * w;   /* slot 0 is Q */
        re[n] = ((float)iq[n * 2U + 1U] - mi) * w;   /* slot 1 is I */
    }
}

void fft_run(float *re, float *im)
{
    /* Reorder into bit-reversed positions, swapping each pair once. */
    for (uint32_t i = 0; i < FFT_N; i++) {
        uint32_t j = s_rev[i];
        if (j > i) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    /*
     * Decimation in time. `step` is the half-size of the current
     * butterfly group, and the twiddle index strides through the table
     * so that no multiply is needed to find it.
     */
    for (uint32_t step = 1U; step < FFT_N; step <<= 1) {
        uint32_t stride = (FFT_N / 2U) / step;

        for (uint32_t base = 0; base < FFT_N; base += step * 2U) {
            uint32_t tw = 0;

            for (uint32_t k = 0; k < step; k++) {
                uint32_t a = base + k;
                uint32_t b = a + step;

                float wr = s_twr[tw];
                float wi = s_twi[tw];
                tw += stride;

                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;

                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] = re[a] + xr;
                im[a] = im[a] + xi;
            }
        }
    }
}

void fft_spectrum_db10(const float *re, const float *im, int16_t *db10)
{
    /*
     * Full scale reference. A carrier at digital full scale, windowed
     * by Hann - whose coherent gain is 0.5 - and summed over N bins
     * gives 32768 * 0.5 * N in its own bin. Scaling to that makes the
     * numbers dBFS and directly comparable with the single-bin
     * measurements in iqmeas.c.
     */
    const float full = 32768.0f * 0.5f * (float)FFT_N;
    const float ref2 = full * full;

    for (uint32_t k = 0; k < FFT_N; k++) {
        /*
         * ROTATE AS WE GO: output index 0 must be the LOWEST frequency.
         *
         * An FFT puts DC in bin 0 and the negative frequencies in the
         * upper half, which is the wrong order for anything anyone
         * looks at. A panoramic display reads left to right from low to
         * high with the tuned frequency in the middle, so bin N/2+k of
         * the transform belongs at index k of the display and bin k
         * belongs at index N/2+k.
         */
        uint32_t src = (k + FFT_N / 2U) & (FFT_N - 1U);

        float p = re[src] * re[src] + im[src] * im[src];

        /*
         * 10*log10(p/ref2) in tenths of a dB:
         *   10 log10(x) = 3.0103 log2(x), so tenths are 30.103 log2(x).
         * A floor of -1800 (that is -180 dB) keeps an exactly zero bin
         * from running away.
         */
        float d = (p > 0.0f)
                ? (30.103f * fft_log2f(p / ref2))
                : -1800.0f;

        if (d < -1800.0f) { d = -1800.0f; }
        if (d >  200.0f)  { d =  200.0f; }

        db10[k] = (int16_t)d;
    }
}
