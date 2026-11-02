#include "panbringup.h"
#include "ili9488.h"
#include "board.h"
#include "clock.h"

#define W   ILI9488_W       /* 480 */
#define H   ILI9488_H       /* 320 */

/* One column, in the panel's 18-bit format: three bytes a pixel with
 * the data in the top six bits of each. */
static uint8_t s_col[H * 3U];

/* Per-column maximum, in upstream's uncalibrated dB. */
static float s_peak_col[W];

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

void pan_init(void)
{
    ili9488_fill_screen(BG_R, BG_G, BG_B);
}

/* dB to row. Row 0 is the top and PAN_TOP_DB; the floor is the last. */
static uint32_t db_to_row(float db)
{
    const float span = PAN_TOP_DB - PAN_FLOOR_DB;

    float v = (PAN_TOP_DB - db) / span;         /* 0 at the top */

    if (v < 0.0f) { v = 0.0f; }
    if (v > 1.0f) { v = 1.0f; }

    return (uint32_t)(v * (float)(H - 1U));
}

void pan_draw(const float *db, uint32_t bins)
{
    uint64_t t0 = board_cycles();

    if (bins == 0U) { return; }

    /*
     * Fold `bins` bins into W columns, keeping the loudest in each.
     *
     * 512 bins into 480 columns is 1.07 bins a column, so most columns
     * take one bin and some take two. Walking the BINS and computing
     * their column is exact; walking the columns and computing a bin
     * range would skip bins whenever the ratio is not an integer, which
     * at 1.07 would drop one column in sixteen - a narrow carrier
     * landing in one of those simply would not appear.
     */
    for (uint32_t x = 0; x < W; x++) {
        s_peak_col[x] = PAN_FLOOR_DB;
    }

    for (uint32_t k = 0; k < bins; k++) {
        uint32_t x = (k * W) / bins;
        if (x < W && db[k] > s_peak_col[x]) { s_peak_col[x] = db[k]; }
    }

    for (uint32_t x = 0; x < W; x++) {
        uint32_t top = db_to_row(s_peak_col[x]);

        /*
         * Grid. Horizontal every 40 rows, which at 0.375 dB a pixel is
         * every 15 dB; vertical every 48 columns, which over a 192 kHz
         * span is every 19.2 kHz. The centre column is the tuned
         * frequency and gets its own colour, because on a zero-IF
         * receiver that line is where the LO leakage lives and it
         * should be recognisable as such.
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

uint32_t pan_last_us(void)
{
    return s_last_us;
}

void pan_peak(const float *db, uint32_t bins, uint32_t fs_hz,
              pan_peak_t *out)
{
    uint32_t best  = 0;
    int      found = 0;

    /*
     * SKIP THE MIDDLE, STILL.
     *
     * Upstream's fft.c already blanks the DC bin and its four
     * neighbours - FFT_IQ_DC_BLANK, which replaces them with the
     * average of the bins four places either side - so the LO spike no
     * longer towers over the display. That is cosmetic, by its own
     * comment, and this search is not cosmetic: the blanking fills
     * those bins with a plausible value rather than a small one, so a
     * peak search that trusted them could still land in the middle.
     *
     * Six bins either side covers the blanked five plus a margin.
     */
    const uint32_t mid  = bins / 2U;
    const uint32_t skip = 6U;

    for (uint32_t k = 0; k < bins; k++) {
        if (k + skip > mid && k < mid + skip) { continue; }

        if (!found || db[k] > db[best]) { best = k; found = 1; }
    }

    out->index = best;
    out->db    = db[best];

    int32_t off = (int32_t)best - (int32_t)mid;

    out->offset_hz = (int32_t)(((int64_t)off * (int64_t)fs_hz)
                               / (int64_t)bins);
}
