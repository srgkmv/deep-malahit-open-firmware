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
 * Eight colour bars with a one-pixel white border, drawn left to right:
 * black, red, green, blue, yellow, cyan, magenta, white.
 *
 * Chosen so a format mismatch is unmistakable rather than subtle. If the
 * panel is reading three bytes where two were sent, the bars come out in
 * the wrong colours AND at the wrong widths, and the picture walks
 * sideways down the screen instead of standing in neat columns.
 */
void ili9488_test_pattern(void);

/* Microseconds taken by the last full-screen fill, for the throughput
 * number that decides the UI design. */
uint32_t ili9488_last_fill_us(void);

#endif /* MALAHIT_ILI9488_H */
