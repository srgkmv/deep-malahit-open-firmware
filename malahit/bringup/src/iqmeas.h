#ifndef MALAHIT_IQMEAS_H
#define MALAHIT_IQMEAS_H

#include <stdint.h>

/*
 * Measuring one frequency in a captured I/Q block, exactly.
 *
 * WHY A SINGLE BIN AND NOT AN FFT. Everything step 5 has to establish -
 * did the LO land where it was told, which way round is the spectrum,
 * how much signal does each band filter pass, does the gain control do
 * anything - is a question about the amplitude at ONE known frequency.
 * An FFT would answer it along with 8191 questions nobody asked, and it
 * would need a twiddle table, a bit-reversal and a window before it
 * answered anything at all.
 *
 * THE TRICK THAT MAKES IT EXACT. The measured bin is fixed at Fs/64, so
 * the reference phasor advances by exactly one sixty-fourth of a turn
 * per sample - which is exactly the 64-entry sine table below, with no
 * interpolation and no rounding. At 192 kHz that puts the bin at
 * 3000.00 Hz. Give the capture a length that is a multiple of 64 and
 * the measurement is fully coherent: no window, no leakage from DC, no
 * scalloping, and a stationary carrier integrates up linearly while
 * noise integrates as the square root.
 *
 * So the plan is to tune the receiver deliberately 3 kHz away from the
 * test carrier and measure that bin. Nothing here needs libm, which
 * matters because this image links -nostdlib.
 */

#define IQ_BIN_DIVISOR  64U     /* measured bin = Fs / 64 */

/* One cycle of a sine in 64 steps, amplitude 12000. Shared with the
 * tone generator in main.c so there is one table and one scale. */
extern const int16_t iq_sin64[64];

typedef struct {
    uint64_t mag_pos;       /* |X(+Fs/64)|  */
    uint64_t mag_neg;       /* |X(-Fs/64)|  - the image */
    uint64_t mag_dc;        /* |X(0)|       - LO leakage and mixer offset */
    int32_t  image_db10;    /* 10 x dB of mag_pos / mag_neg */
    int32_t  dc_db10;       /* 10 x dB of mag_pos / mag_dc */
} iq_result_t;

/*
 * Measure a capture. `iq` is interleaved as the SAI delivers it: SLOT 0 IS I AND
 * SLOT 1 IS Q, which is upstream's order and is true here only because
 * the codec's ADCPHS bit is set to make it so - the schematic wires
 * AUDIO_Q to the left channel. See nau8822.h. Getting that pair the
 * wrong way round conjugates the spectrum - it swaps mag_pos and
 * mag_neg - which looks like an image rejection failure rather than
 * like a wiring assumption.
 *
 * frames should be a multiple of 64 for the measurement to be coherent;
 * anything else still works but leaks.
 */
void iq_measure(const int16_t *iq, uint32_t frames, iq_result_t *out);

/*
 * 10 x 20log10(num/den), in integer arithmetic. Returns -9990 for a
 * zero numerator and +9990 for a zero denominator, so a ratio against
 * nothing prints as an obvious sentinel instead of dividing by zero.
 */
int32_t iq_db10(uint64_t num, uint64_t den);

/*
 * The same thing relative to digital full scale, which is the only
 * reference that means anything to a reader.
 *
 * iq_db10(mag, 1) gives numbers like "214 dB", which is arithmetically
 * true and useless: it is 20log10 of an accumulator whose scale depends
 * on the capture length and the table amplitude. Measured against what
 * a full-scale carrier would have produced, the same figure becomes
 * -46 dBFS and can be compared with a datasheet, with another run, or
 * with the next band.
 *
 * Full scale for this measurement is frames x 32768 x 12000: every
 * sample at the top of the 16-bit range, multiplied by the table.
 */
int32_t iq_dbfs10(uint64_t mag, uint32_t frames);

/* Print helper: dB with one decimal, as a sign and two integers,
 * because console_printf() has no %f and should not acquire one. */
#define IQ_DB_INT(d10)   ((d10) / 10)
#define IQ_DB_FRAC(d10)  (((d10) < 0 ? -(d10) : (d10)) % 10)

/*
 * The strongest bin of a finished spectrum, which is what makes a
 * picture verifiable without looking at it.
 *
 * `db` is as fft_compute_db_iq() leaves it: FFTSHIFT ORDER, index 0 the
 * lowest frequency and index bins/2 the tuned one.
 *
 * This is the last thing left of the stand-in panadapter this port
 * carried until User/spectrum.c was grafted. It stays because nothing
 * upstream exposes a peak finder - the real UI reads its own cursor -
 * and because a console line saying where the peak is has caught more
 * mistakes in this port than the screen has.
 */
typedef struct {
    uint32_t index;
    float    db;
    int32_t  offset_hz;     /* signed, relative to the tuned frequency */
} iq_peak_t;

void iq_peak(const float *db, uint32_t bins, uint32_t fs_hz,
             iq_peak_t *out);

#endif /* MALAHIT_IQMEAS_H */
