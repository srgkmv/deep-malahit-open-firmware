#ifndef MALAHIT_ILI9488_H
#define MALAHIT_ILI9488_H

#include <stdint.h>

/*
 * ILI9488, 480x320, over 4-wire SPI.
 *
 * THE QUESTION THIS FILE EXISTS TO ANSWER.
 *
 * The ILI9488 is widely reported to accept only 18-bit pixels - three
 * bytes each - when it is driven over SPI, even though it handles 16-bit
 * RGB565 quite happily on its parallel bus. If that is true here it sets
 * the ceiling for everything above: a full frame becomes
 *
 *     480 x 320 x 3 = 460 800 bytes
 *
 * instead of 307 200, and at any plausible SPI clock that is the
 * difference between a usable waterfall and a slideshow. It also decides
 * whether a framebuffer in AXI SRAM can be RGB565 (307 kB, comfortable)
 * or has to be RGB666 (461 kB, still fits, but less room for everything
 * else).
 *
 * Nobody should take this on faith, including from the many libraries
 * that assert it. ili9488_test_pattern() draws the same picture in both
 * formats so the panel itself can settle it: whichever one looks right
 * is the one this panel supports.
 */

#define ILI9488_W   480U
#define ILI9488_H   320U

typedef enum {
    ILI9488_FMT_18BIT = 0,   /* COLMOD 0x66, three bytes per pixel */
    ILI9488_FMT_16BIT = 1    /* COLMOD 0x55, two bytes per pixel   */
} ili9488_fmt_t;

/* Hardware reset, init sequence, display on. Leaves the panel in
 * landscape with the requested pixel format. */
void ili9488_init(ili9488_fmt_t fmt);

/* Switch pixel format on a panel that is already up. */
void ili9488_set_format(ili9488_fmt_t fmt);
ili9488_fmt_t ili9488_format(void);

/* Address window for the next pixel write, inclusive. */
void ili9488_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* Solid rectangle in 24-bit colour; the driver reduces it to whatever
 * format is current. */
void ili9488_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       uint8_t r, uint8_t g, uint8_t b);

void ili9488_fill_screen(uint8_t r, uint8_t g, uint8_t b);

/*
 * Blit a prepared pixel block in one transfer. `px` is already in the
 * panel's format - three bytes a pixel at 18 bits, data in the top six
 * bits of each byte. See the comment in ili9488.c for why a spectrum
 * cannot be drawn with fill_rect().
 */
void ili9488_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  const uint8_t *px);

/*
 * THE BLOCK QUEUE: blits by DMA that return before the pixels have gone.
 *
 * ili9488_q_slot() hands out the buffer to compose the next block in -
 * three bytes a pixel, as for ili9488_blit() - waiting only if every
 * slot is queued; ili9488_q_push() queues what was composed there as a
 * w x h block at (x, y) and returns at once. SPI3's end-of-transfer
 * interrupt then sends the queued blocks one after another by DMA, each
 * under its own window and RAMWR, while the caller composes the next -
 * and keeps sending through the AM block interrupt, which it outranks.
 *
 * With the DMA off or failed, q_push() sends the block polled before it
 * returns, so callers do not change.
 *
 * Every other function in this driver waits for the queue to empty
 * before it touches the bus, so nothing can land among a block's pixels.
 */
uint8_t *ili9488_q_slot(uint32_t *cap);
void     ili9488_q_push(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* Wait for every queued block to have gone, CS raised. */
void ili9488_sync(void);

/* The deepest the queue has been since the last call (then reset), and
 * the times it stopped moving - each of which switched the DMA off. */
uint32_t ili9488_q_depth_max(void);
uint32_t ili9488_q_stalls(void);

/*
 * Set up the DMA (spi3_dma_init()) and prove it before anything is sent
 * through it: 256 bytes go out by DMA with CS HIGH - the panel is not
 * listening - and they must finish, without an error, and take at least
 * half the time their bits need on the wire (a transfer that "finishes"
 * faster than that did not happen). Only then is the queue allowed to
 * use it, and its interrupt armed. Returns 0, or negative with the DMA
 * left off and every blit polled, exactly as before. `test_us` gets the
 * probe's time if not NULL.
 */
int ili9488_dma_init(uint32_t *test_us);

/* The switch, for A/B measurement: off sends everything polled. On only
 * takes effect if the self-test passed and nothing has failed since. */
void ili9488_dma_enable(int on);

/* 1 if queued blocks go by DMA. */
int ili9488_dma_on(void);

/*
 * Eight colour bars with a one-pixel white border, drawn left to right:
 * black, red, green, blue, yellow, cyan, magenta, white.
 *
 * Chosen so a format mismatch is unmistakable rather than subtle. If the
 * panel is reading three bytes where two were sent, the bars come out in
 * the wrong colours AND at the wrong widths, and the picture walks
 * sideways down the screen instead of standing in neat columns.
 */
void ili9488_test_pattern(void);

/*
 * Ask the panel a question and get its own answer.
 *
 * ILI9488 read frames are: command byte with D/CX low, then one DUMMY
 * byte, then the data, all with D/CX high and CS held low throughout.
 * Returns up to 4 bytes, dummy already discarded.
 *
 * The two worth asking:
 *   0x0C  RDDCOLMOD - the pixel format the panel believes it is in.
 *         0x06 means 18 bits per pixel, 0x05 means 16.
 *   0x04  RDDID     - manufacturer and device ID, which proves the read
 *         path works before any answer from it is trusted.
 */
uint32_t ili9488_read_reg(uint8_t cmd, uint32_t nbytes);

/*
 * The same read, but handing back every byte exactly as it arrived,
 * dummy and all.
 *
 * How many dummy bytes precede the answer is not the same for every
 * command on this part, and guessing wrong turns a useful register
 * into a plausible-looking zero - which is what happened: RDDID came
 * back as 0x403300 and RDDCOLMOD as 0x00 twice, neither of which means
 * anything. Reading 0xD3 raw settles it, because the answer is known
 * in advance: 0x94 0x88 has to appear somewhere in the bytes, and
 * wherever it lands tells you the offset for everything else.
 */
void ili9488_read_raw(uint8_t cmd, uint8_t *buf, uint32_t nbytes);

/* The panel's memory, raw, for screenshots: RAMRD over a window.
 * Leading dummy or not is left to the reader. See ili9488.c. */
void ili9488_read_gram(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       uint8_t *buf, uint32_t nbytes);

/* Microseconds taken by the last full-screen fill, for the throughput
 * number that decides the UI design. */
uint32_t ili9488_last_fill_us(void);

#endif /* MALAHIT_ILI9488_H */
