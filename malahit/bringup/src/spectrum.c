#include "spectrum.h"
#include "fft.h"
#include "ili9488.h"
#include "board.h"
#include "clock.h"

#define W   ILI9488_W       /* 480 */
#define H   ILI9488_H       /* 320 */

/* One column, in the panel's 18-bit format: three bytes a pixel with
 * the data in the top six bits of each. */
static uint8_t s_col[H * 3U];

/* Per-column maximum, in tenths of a dB. */
static int16_t s_peak_col[W];

static uint32_t s_last_us;

/*
 * Colours. Deliberately dim except for the trace: a panorama is looked
 * at for a long time, and the only thing that should be bright is the
 * thing being looked for.
 */
#define BG_R    0x00U
#define BG_G    0x00U
#define BG_B    0x18U

#define GRID_R  0x28U
#define GRID_G  0x28U
#define GRID_B  0x38U

#define CTR_R   0x60U
#define CTR_G   0x10U
#define CTR_B   0x10U

#define FILL_R  0x00U
#define FILL_G  0x48U
#define FILL_B  0x20U

#define TRACE_R 0x40U
#define TRACE_G 0xFCU
#define TRACE_B 0x50U

void spectrum_init(void)
{
    ili9488_fill_screen(BG_R, BG_G, BG_B);
}

/* dB to row. Row 0 is the top and 0 dBFS; the floor is the last row. */
static uint32_t db_to_row(int16_t db10)
{
    int32_t span = -SPECTRUM_FLOOR_DB10;            /* 1600 tenths */
    int32_t v    = -(int32_t)db10;                  /* 0 at the top */

    if (v < 0)     { v = 0; }
    if (v > span)  { v = span; }

    uint32_t row = (uint32_t)((v * (int32_t)(H - 1U)) / span);

    return row;
}

void spectrum_draw(const int16_t *db10)
{
    uint64_t t0 = board_cycles();

    /*
     * Fold FFT_N bins into W columns, keeping the loudest in each.
     *
     * 1024 into 480 is 2.13 bins a column, so the mapping is not a
     * clean integer and must not be treated as one: walking the bins
     * and computing their column is exact, whereas walking the columns
     * and computing a bin range would drop every third bin.
     */
    for (uint32_t x = 0; x < W; x++) {
        s_peak_col[x] = (int16_t)SPECTRUM_FLOOR_DB10;
    }

    for (uint32_t k = 0; k < FFT_N; k++) {
        uint32_t x = (k * W) / FFT_N;
        if (db10[k] > s_peak_col[x]) { s_peak_col[x] = db10[k]; }
    }

    for (uint32_t x = 0; x < W; x++) {
        uint32_t top = db_to_row(s_peak_col[x]);

        /*
         * Grid. Horizontal every 20 dB, which at half a decibel a pixel
         * is every 40 rows; vertical every 48 columns, which at 187.5 Hz
         * a bin and 2.13 bins a column is almost exactly every 19.2 kHz.
         * The centre column is the tuned frequency and gets its own
         * colour, because on a zero-IF receiver that line is where the
         * LO leakage lives and it should be recognisable as such.
         */
        int is_vgrid = ((x % 48U) == 0U);
        int is_ctr   = (x == W / 2U);

        for (uint32_t y = 0; y < H; y++) {
            uint8_t r, g, b;

            if (y == top) {
                r = TRACE_R; g = TRACE_G; b = TRACE_B;
            } else if (y > top) {
                r = FILL_R; g = FILL_G; b = FILL_B;
            } else if (is_ctr) {
                r = CTR_R; g = CTR_G; b = CTR_B;
            } else if (is_vgrid || ((y % 40U) == 0U)) {
                r = GRID_R; g = GRID_G; b = GRID_B;
            } else {
                r = BG_R; g = BG_G; b = BG_B;
            }

            s_col[y * 3U]      = r;
            s_col[y * 3U + 1U] = g;
            s_col[y * 3U + 2U] = b;
        }

        ili9488_blit((uint16_t)x, 0U, 1U, (uint16_t)H, s_col);
    }

    uint64_t t1 = board_cycles();

    s_last_us = (uint32_t)(((t1 - t0) * 1000000U) / clock_sysclk_hz);
}

uint32_t spectrum_last_us(void)
{
    return s_last_us;
}

void spectrum_peak(const int16_t *db10, uint32_t fs_hz,
                   spectrum_peak_t *out)
{
    uint32_t best  = 0;
    int      found = 0;

    /*
     * SKIP THE MIDDLE. The centre bins carry the mixer's LO leakage,
     * which step 5 measured at 4 dB ABOVE the wanted carrier - so a
     * peak search that included them would report the receiver's own
     * offset as the strongest signal, every time, whatever was on the
     * air.
     *
     * Four bins either side of centre is enough: fft_load() subtracts
     * the block mean, which removes most of the leakage, and the Hann
     * window confines what is left to its two-bin main lobe.
     */
    const uint32_t mid  = FFT_N / 2U;
    const uint32_t skip = 4U;

    for (uint32_t k = 0; k < FFT_N; k++) {
        if (k > mid - skip && k < mid + skip) { continue; }

        /* `found` rather than comparing against db10[best] from the
         * start: with best initialised to zero, "db10[k] > db10[best]"
         * compares against bin 0 instead of against nothing, and any
         * spectrum whose lowest bin happened to be the loudest would
         * report index 0 forever. */
        if (!found || db10[k] > db10[best]) { best = k; found = 1; }
    }

    out->index = best;
    out->db10  = db10[best];

    /*
     * Display index to frequency offset. Index mid is the tuned
     * frequency, each step is fs/N, and the arithmetic is signed
     * because half the display is below the tuning.
     */
    int32_t bins = (int32_t)best - (int32_t)mid;

    out->offset_hz = (int32_t)(((int64_t)bins * (int64_t)fs_hz)
                               / (int64_t)FFT_N);
}
