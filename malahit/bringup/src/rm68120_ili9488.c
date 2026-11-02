/*
 * The three functions gfx2 needs from a panel, over the ILI9488.
 * See ../../compat/rm68120_ili9488.h for why it is only three.
 */
#include "stm32h7xx.h"
#include "rm68120_ili9488.h"
#include "ili9488.h"
#include "spi3.h"
#include "board.h"
#include "clock.h"

/*
 * The staging buffer, and why this size.
 *
 * gfx2 composes a band at a time - GFX2_BAND_H rows of the full screen
 * width - and hands it over in one window. At 480 wide and the band
 * height this port builds with, the largest window is 480 x BAND_H
 * pixels, three bytes each.
 *
 * Sized for 32 rows so the band height can be raised without this
 * becoming the thing that stops it: 480 x 32 x 3 is 46 kB, which on a
 * board with 512 kB of AXI SRAM is affordable, and the band is the one
 * buffer whose size directly decides how many times gfx2 re-runs the
 * drawing code per frame.
 *
 * A window larger than this does not break: rm68120_dato() flushes
 * whenever the buffer fills, and set_window() re-arms. It just costs
 * more transfers.
 */
#define STAGE_ROWS      32U
#define STAGE_BYTES     (480U * STAGE_ROWS * 3U)

static uint8_t s_stage[STAGE_BYTES];

volatile uint32_t g_lcd_ciclos;     /* bus time, accumulated by gfx2 */

uint8_t *g_rm_put = s_stage;
uint8_t *g_rm_end = s_stage;        /* empty until a window is set */

/* Where the pixels flushed so far are going, and what is left. */
static uint16_t s_x0, s_y0, s_x1, s_y1;
static uint32_t s_pending;          /* pixels still to come in this window */
static uint16_t s_row;              /* next row to receive, absolute */

void rm68120_flush(void)
{
    uint32_t bytes = (uint32_t)(g_rm_put - s_stage);

    if (bytes == 0U) { return; }

    uint32_t px   = bytes / 3U;
    uint32_t w    = (uint32_t)(s_x1 - s_x0) + 1U;

    /*
     * Whole rows only, which is how gfx2 always fills a window - it
     * walks the band row by row - so this is an identity in practice
     * and a guard against a partial row in theory. A partial row would
     * have to be carried over, and nothing upstream produces one.
     */
    uint32_t rows = px / w;

    if (rows > 0U) {
        ili9488_blit(s_x0, s_row, (uint16_t)w, (uint16_t)rows, s_stage);
        s_row = (uint16_t)(s_row + rows);
    }

    g_rm_put = s_stage;
}

void rm68120_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    /* Anything staged belongs to the previous window. */
    rm68120_flush();

    s_x0 = x0; s_y0 = y0; s_x1 = x1; s_y1 = y1;
    s_row = y0;

    uint32_t w = (uint32_t)(x1 - x0) + 1U;
    uint32_t h = (uint32_t)(y1 - y0) + 1U;

    s_pending = w * h;

    /*
     * Arm the buffer for at most a whole number of rows, so a flush
     * never splits one. Everything beyond that simply flushes earlier.
     */
    uint32_t cap_rows = STAGE_BYTES / (w * 3U);

    if (cap_rows == 0U) { cap_rows = 1U; }      /* absurdly wide window */
    if (cap_rows > h)   { cap_rows = h; }

    g_rm_put = s_stage;
    g_rm_end = s_stage + (w * cap_rows * 3U);
}

uint32_t rm68120_ciclo(void)
{
    /*
     * gfx2 uses this only to account for bus time, subtracting one
     * reading from another - so what matters is that it counts
     * monotonically at a known rate, not what the origin is.
     *
     * board_cycles() is the SysTick-derived count the rest of this port
     * uses; truncating it to 32 bits is fine because gfx2 only ever
     * looks at differences, and at 480 MHz a 32-bit count wraps every
     * nine seconds - far longer than a band takes to draw.
     */
    return (uint32_t)board_cycles();
}
