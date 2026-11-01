#ifndef MALAHIT_FFT_H
#define MALAHIT_FFT_H

#include <stdint.h>

/*
 * A complex radix-2 FFT, sized for a panoramic display.
 *
 * COMPLEX, not real. The receiver is zero-IF and delivers I and Q, so
 * positive and negative frequencies are different signals - a carrier
 * 20 kHz above the tuned frequency and one 20 kHz below are not the
 * same thing and must not fold onto each other. A real FFT would throw
 * away exactly the information the quadrature front end was built to
 * provide. Bin 0 is the tuned frequency itself, bins 1..N/2-1 are above
 * it and bins N/2+1..N-1 are below.
 *
 * Single precision. This part has a double-precision FPU, so both are
 * in hardware, and float halves the memory traffic for an array that is
 * read and written log2(N) times. The dynamic range of a 1024-point
 * float FFT is far beyond what a 16-bit ADC feeding it can produce.
 */
#define FFT_N       1024U
#define FFT_LOG2N   10U

/*
 * Build the twiddle and window tables. Must be called before fft_run().
 *
 * NO LIBM IS AVAILABLE - this image links -nostdlib, deliberately, so
 * there is no cos(), no sin() and no sqrt(). The tables are built from
 * the half-angle identities instead:
 *
 *     cos(t/2) = sqrt((1 + cos t) / 2)
 *     sin(t/2) = sqrt((1 - cos t) / 2)
 *
 * starting from cos(pi) = -1 and halving until the angle is 2*pi/N.
 * Every angle a power-of-two FFT needs is reachable that way, exactly,
 * which is the happy accident that makes this cheap. The rest of the
 * table comes from repeated complex rotation; over N/2 steps in double
 * precision the accumulated error is around 1e-13, which is nothing
 * against the 16 bits arriving from the codec.
 */
void fft_init(void);

/*
 * In-place FFT of `re`/`im`, each FFT_N long. Decimation in time, so
 * the input is bit-reversed on the way in and the output comes out in
 * natural order.
 */
void fft_run(float *re, float *im);

/*
 * Load a capture into the FFT arrays, windowed and scaled.
 *
 * `iq` is interleaved as the SAI delivers it - SLOT 0 IS Q, SLOT 1 IS I
 * on this board, see iqmeas.h - and the DC offset is removed first.
 * Removing it matters here more than anywhere else: the mixer's LO
 * leakage measured 4 dB ABOVE the wanted carrier in step 5, so left in
 * place it would be the tallest thing on the display and its window
 * skirts would bury small signals either side of centre.
 */
void fft_load(const int16_t *iq, float *re, float *im);

/*
 * Magnitude of each bin in tenths of a dB relative to full scale,
 * written to `db10`, which must be FFT_N long. Bin order is rotated so
 * that index 0 is the LOWEST frequency and index N-1 the highest, with
 * the tuned frequency in the middle at index N/2 - which is how a
 * panoramic display is read, and not how an FFT comes out.
 */
void fft_spectrum_db10(const float *re, const float *im, int16_t *db10);

/* log2 for floats, by taking the exponent apart. Public because the
 * display uses it for scaling too. */
float fft_log2f(float x);

#endif /* MALAHIT_FFT_H */
