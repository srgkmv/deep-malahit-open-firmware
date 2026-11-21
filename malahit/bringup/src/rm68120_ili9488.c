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
 * THE STAGING BUFFER IS THE PANEL DRIVER'S NOW.
 *
 * It used to be a 46 kB array here, sized for 32 rows of 480 at three
 * bytes a pixel, flushed in one synchronous transfer whenever it filled.
 * Since 21/11/2026 the same 46 kB are ili9488.c's block queue - eight
 * slots of four full rows - and this file composes straight into the
 * slot the queue hands out (ili9488_q_slot()) and queues it when it is
 * full or the window is done (ili9488_q_push()). The SPI interrupt sends
 * the queued blocks by DMA while gfx2 composes the next ones.
 *
 * A window taller than a slot is simply several blocks, each under its
 * own window command - which is what a flush of a full buffer always
 * was here.
 */
volatile uint32_t g_lcd_ciclos;     /* bus time, accumulated by gfx2 */

static uint8_t s_none[3];           /* where an unarmed window points */

uint8_t *g_rm_put = s_none;
uint8_t *g_rm_end = s_none;         /* empty until a window is set */

static uint8_t *s_buf = s_none;     /* the slot being composed into */

/* Where the pixels flushed so far are going. */
static uint16_t s_x0, s_y0, s_x1, s_y1;
static uint16_t s_row;              /* next row to receive, absolute */

/*
 * Point the inline rm68120_dato() at a fresh slot, and say where it
 * ends.
 *
 * THE END IS THE WINDOW'S LAST ROW WHEN THAT COMES FIRST: the window's
 * own last pixel triggers its flush, so the final block of a window is
 * queued as it is finished - not left staged until somebody's NEXT
 * set_window, as the tail of a tall gfx_fill_rect() used to be.
 *
 * A COMPLETE WINDOW ARMS NOTHING, so it does not wait for a slot it will
 * never fill - the display loop would otherwise stall at the end of
 * every window whenever the queue is full, which is most of the time.
 * Pixels written past the end of a window are dropped; nothing upstream
 * writes any, and the old behaviour - sending them as rows below the
 * window - was not what a panel does with them either.
 */
static void arm(void)
{
    if (s_row > s_y1) {
        g_rm_put = s_none;
        g_rm_end = s_none;
        s_buf    = s_none;
        return;
    }

    uint32_t cap;
    uint32_t w    = (uint32_t)(s_x1 - s_x0) + 1U;
    uint32_t left = (uint32_t)(s_y1 - s_row) + 1U;

    s_buf = ili9488_q_slot(&cap);

    uint32_t rows = cap / (w * 3U);    /* >= 4: a slot is four full rows */

    if (rows == 0U)  { rows = 1U; }     /* cannot happen at w <= 480 */
    if (left < rows) { rows = left; }

    g_rm_put = s_buf;
    g_rm_end = s_buf + (w * rows * 3U);
}

void rm68120_flush(void)
{
    uint32_t bytes = (uint32_t)(g_rm_put - s_buf);

    if (s_buf == s_none || bytes == 0U) { return; }

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
        /* Returns as soon as the block is queued. */
        ili9488_q_push(s_x0, s_row, (uint16_t)w, (uint16_t)rows);
        s_row = (uint16_t)(s_row + rows);
    }

    arm();
}

void rm68120_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    /* Anything staged belongs to the previous window. */
    rm68120_flush();

    s_x0 = x0; s_y0 = y0; s_x1 = x1; s_y1 = y1;
    s_row = y0;

    /*
     * An inverted window would make w wrap to four billion in arm(). gfx
     * clips before it gets here, so this is a guard and not a feature:
     * nothing is armed, and every pixel sent to it is dropped by the
     * bounds test in rm68120_dato(). So is a window wider than the
     * panel, which a slot of four full rows could not hold one row of.
     */
    if (x1 < x0 || y1 < y0 || (uint32_t)(x1 - x0) >= ILI9488_W) {
        g_rm_put = s_none;
        g_rm_end = s_none;
        s_buf    = s_none;
        return;
    }

    /* Arm for at most a whole number of rows, so a flush never splits
     * one. Everything beyond that simply flushes earlier. */
    arm();
}

void rm68120_write_data(uint16_t rgb565)
{
    rm68120_dato(rgb565);
    rm68120_flush();
}

void rm68120_fill_screen(uint16_t rgb565)
{
    /* Whatever is staged was drawn first, so it goes first. */
    rm68120_flush();

    ili9488_fill_rect(0, 0, ILI9488_W, ILI9488_H,
                      (uint8_t)((rgb565 >> 8) & 0xF8U),
                      (uint8_t)((rgb565 >> 3) & 0xFCU),
                      (uint8_t)((rgb565 << 3) & 0xF8U));
}

/* ------------------------------------------------------------------ */
/* lcd_dma - see compat/lcd_dma_ili9488.h                             */
/* ------------------------------------------------------------------ */

void lcd_dma_init(void) { (void)ili9488_dma_init(0); }

void lcd_dma_pon(uint8_t on) { ili9488_dma_enable(on); }

uint8_t lcd_dma_hay(void) { return (uint8_t)ili9488_dma_on(); }

/*
 * Nothing of the CALLER'S is ever in flight here, so there is nothing
 * to wait for: lcd_dma_manda() has converted every pixel into the
 * staging buffer before it returns, and the buffer it was given is free
 * the moment it does. What is still going out is this driver's own
 * copy, and the driver collects that itself before anything else
 * touches the bus. See compat/lcd_dma_ili9488.h.
 */
uint8_t lcd_dma_espera(void) { return 1U; }

/* Transfers that failed plus queues that stalled: either one switches
 * the DMA off for the rest of the run. */
uint16_t lcd_dma_fallos(void)
{
    uint32_t n = spi3_dma_failures() + ili9488_q_stalls();

    return (uint16_t)((n > 0xFFFFU) ? 0xFFFFU : n);
}

uint8_t lcd_dma_manda(const uint16_t *px, uint32_t n)
{
    if (px == 0 || n == 0U) { return 0U; }

    /*
     * Through rm68120_dato() rather than straight to SPI, so the
     * window that set_window() armed is respected and there is exactly
     * one path from a pixel to the panel. The staging buffer flushes
     * itself whenever it fills, and at the window's last pixel.
     */
    for (uint32_t i = 0; i < n; i++) { rm68120_dato(px[i]); }
    rm68120_flush();

    return 1U;
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
     *
     * With the DMA on, what it measures around a band is the CPU's
     * conversion plus any wait for a free slot in the queue - not this
     * band's time on the wire, which overlaps whatever comes next.
     */
    return (uint32_t)board_cycles();
}
