#ifndef MALAHIT_PANBRINGUP_H
#define MALAHIT_PANBRINGUP_H

#include <stdint.h>

/*
 * A BRING-UP STAND-IN FOR User/spectrum.c, AND NOTHING MORE.
 *
 * This file used to be called spectrum.c, which was a mistake in two
 * ways. Upstream already has User/spectrum.c - 2078 lines of panadapter
 * with heat-map palettes, partial redraw, per-column ceilings and band
 * labels - so the name collided outright with the thing this port
 * exists to bring over.
 *
 * It is not grafted yet, and the reason is not laziness. It is built on
 * gfx2.h, whose implementation includes rm68120_exmc.h: the DEEPSDR's
 * parallel panel. It also hard-codes SPEC_MAX_W = 800 and 240 rows.
 * Pulling it in means grafting the whole gfx2 surface layer,
 * re-pointing it at an ILI9488 on SPI, and doing the 800 -> 480
 * relayout that PORTING.md section 3.1 calls the largest single piece
 * of work in the port. That is step 9, not step 7.
 *
 * So this stays a small stand-in whose only job is to make the spectrum
 * visible while the signal chain underneath it is grafted, and it is
 * named so nobody mistakes it for the real one. When User/spectrum.c
 * arrives, this file goes.
 *
 * THE TRANSFORM IS NOT A STAND-IN ANY MORE: User/fft.c does it.
 */

/*
 * Upstream's dB is UNCALIBRATED, and these two numbers say so.
 *
 * fft.c computes 3.0103 * log2(power) straight from windowed int16
 * samples, with no reference - so a full-scale carrier is not 0 dB, it
 * is whatever the transform size and the window happen to give.
 * Worked through for 512 points and Hann: a full-scale tone puts
 * 32768 * 0.5 * 512 into its bin, so its "dB" is about 138, and the
 * measured noise floor sits near 40.
 *
 * Hence a window of 20..140 across the 320 rows, which is 0.375 dB a
 * pixel. These are display constants, not calibration: anything that
 * needs a real level in dBFS should measure it, as iqmeas.c does.
 */
#define PAN_TOP_DB      140.0f
#define PAN_FLOOR_DB     20.0f

void pan_init(void);

/*
 * Draw one frame. `db` is `bins` values as fft_compute_db_iq() leaves
 * them: FFTSHIFT ORDER ALREADY DONE - index 0 is -Fs/2, index bins/2 is
 * the tuned frequency, the last is just under +Fs/2.
 *
 * This file does not rotate them. The hand-written transform it
 * replaced did, and getting that wrong is invisible until a signal
 * appears on the wrong side of the dial.
 *
 * Several bins share each column, so each column shows the LOUDEST of
 * its bins. Averaging would be wrong: a narrow carrier two bins wide
 * averaged against its neighbours sinks into the noise, and a panorama
 * is read to find signals, not to measure their power.
 */
void pan_draw(const float *db, uint32_t bins);

/* How long the last pan_draw() took, in microseconds. The bus is the
 * limit, so this is really a measurement of the panel. */
uint32_t pan_last_us(void);

/*
 * Peak finding, for the console - which is what makes the display
 * verifiable without looking at it.
 */
typedef struct {
    uint32_t index;         /* 0..bins-1, display order   */
    float    db;
    int32_t  offset_hz;     /* signed, relative to tuning */
} pan_peak_t;

void pan_peak(const float *db, uint32_t bins, uint32_t fs_hz,
              pan_peak_t *out);

#endif /* MALAHIT_PANBRINGUP_H */
