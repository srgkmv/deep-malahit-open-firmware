#ifndef MALAHIT_SPECTRUM_H
#define MALAHIT_SPECTRUM_H

#include <stdint.h>

/*
 * The panoramic display: 1024 FFT bins across 480 columns of ILI9488.
 *
 * COLUMN BY COLUMN, NOT A FRAMEBUFFER, and the reason is worth stating
 * because the obvious choice is the other one. A full 480 x 320 frame in
 * the panel's own 18-bit format is 460 800 bytes - nearly all of AXI
 * SRAM, and PORTING.md section 3.1 treats that cost as a real design
 * decision rather than an assumption.
 *
 * It buys almost nothing. The bus is the limit either way: 460 kB at
 * 30 MHz is about 123 ms whether it leaves in one transfer or in 480,
 * because the per-transfer overhead of a window set and a RAMWR is a
 * dozen bytes against 960 bytes of pixels. Measured, the difference is
 * a few percent. So the display is built one 320-pixel column at a time
 * into a 960-byte buffer, and the 460 kB stays available for the DSP
 * tree that arrives in step 8.
 *
 * The vertical scale runs 0 dBFS at the top to SPECTRUM_FLOOR_DB10 at
 * the bottom, which at 320 pixels is half a decibel a pixel.
 */

#define SPECTRUM_FLOOR_DB10   (-1600)   /* -160.0 dBFS at the bottom row */

void spectrum_init(void);

/*
 * Draw one frame. `db10` is FFT_N entries in DISPLAY order - lowest
 * frequency first, tuned frequency in the middle - which is what
 * fft_spectrum_db10() produces.
 *
 * Several bins share each column, so each column shows the LOUDEST of
 * its bins. Averaging would be wrong: a narrow carrier two bins wide
 * averaged against its neighbours sinks into the noise, and a panorama
 * is read to find signals, not to measure their power.
 */
void spectrum_draw(const int16_t *db10);

/* How long the last spectrum_draw() took, in microseconds. The bus is
 * the limit, so this is really a measurement of the panel. */
uint32_t spectrum_last_us(void);

/*
 * Peak finding, for the console - which is what makes the display
 * verifiable without looking at it. Returns the display index of the
 * strongest bin, its level in tenths of a dB, and the frequency offset
 * from the tuned frequency in Hz.
 */
typedef struct {
    uint32_t index;         /* 0..FFT_N-1, display order    */
    int16_t  db10;
    int32_t  offset_hz;     /* signed, relative to tuning   */
} spectrum_peak_t;

void spectrum_peak(const int16_t *db10, uint32_t fs_hz,
                   spectrum_peak_t *out);

#endif /* MALAHIT_SPECTRUM_H */
