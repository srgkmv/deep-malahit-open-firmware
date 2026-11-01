#ifndef MALAHIT_SPI3_H
#define MALAHIT_SPI3_H

#include <stdint.h>

/*
 * SPI3, transmit only, for the ILI9488 panel.
 *
 * Pins, from the schematic:
 *   PC10  SPI3_SCK   AF6
 *   PC11  SPI3_MISO  AF6   (wired, unused for now - see below)
 *   PC12  SPI3_MOSI  AF6
 *   PA15  LCD_CS           plain GPIO, not SPI3_NSS
 *   PD0   LCD_DC           plain GPIO
 *   PD1   LCD_RST          plain GPIO
 *
 * CS is a GPIO rather than hardware NSS on purpose: the panel wants CS to
 * stay low across a command and all of its data bytes, and hardware NSS
 * on this part likes to lift between transfers unless coaxed. A GPIO is
 * one line of code and does exactly what the datasheet's timing diagrams
 * show.
 *
 * MISO is wired on this board and left configured but unused. Reading the
 * panel back is the honest way to prove the bus works - ILI9488 can
 * report its own ID - but reads need a different frame shape and a dummy
 * clock, so that waits until writes are known good.
 *
 * KERNEL CLOCK. SPI1/2/3 take their clock from RCC_D2CCIP1R.SPI123SEL,
 * which this project leaves at its reset value of 000 = pll1_q_ck. PLL1's
 * Q output is set to 240 MHz in clock.h, so every divider below is a
 * division of 240 MHz, NOT of the APB clock. Getting that wrong gives a
 * panel that works at a quarter of the speed you think, or not at all.
 */

typedef enum {
    SPI3_DIV2   = 0,   /* 120 MHz - far past the panel, here for completeness */
    SPI3_DIV4   = 1,   /*  60 MHz */
    SPI3_DIV8   = 2,   /*  30 MHz */
    SPI3_DIV16  = 3,   /*  15 MHz */
    SPI3_DIV32  = 4,   /* 7.5 MHz */
    SPI3_DIV64  = 5,   /* 3.75 MHz */
    SPI3_DIV128 = 6,
    SPI3_DIV256 = 7
} spi3_div_t;

/* Set up the GPIOs and the peripheral. Safe to call again to change speed. */
void spi3_init(spi3_div_t div);

/* Change the bit rate without disturbing anything else. */
void spi3_set_div(spi3_div_t div);

/* Actual SCK frequency for the current divider, for reporting. */
uint32_t spi3_hz(void);

/*
 * Read bytes back from the panel.
 *
 * The driver is configured simplex-transmit, which is right for pushing
 * pixels and useless for asking the panel a question, so this switches
 * to full duplex for the duration and back afterwards. It also drops
 * the clock: ILI9488 read cycles are specified far slower than writes,
 * and a read that works at 15 MHz on one board will not on the next.
 *
 * Asking the panel is worth the trouble. Whether it accepted a pixel
 * format is otherwise decided by a human looking at a screen and saying
 * what colour it seems to be, which is exactly the kind of evidence
 * that has already sent this project down two wrong paths today.
 */
void spi3_read(uint8_t *data, uint32_t len);

/* CFG2 as the peripheral actually holds it. Master mode can be cleared
 * by hardware on a mode fault, so this is worth checking after init
 * rather than assuming the write landed - see spi3.c. */
uint32_t spi3_check(void);

/* Blocking write. Returns when the last bit has left the shift register,
 * so the caller may drop CS immediately afterwards. */
void spi3_write(const uint8_t *data, uint32_t len);

/* Same byte, len times, without needing a buffer for it. This is what
 * fills the screen: 460 800 bytes of one colour would otherwise want a
 * 460 kB source buffer. */
void spi3_write_repeat(const uint8_t *pattern, uint32_t pattern_len,
                       uint32_t repeats);

#endif /* MALAHIT_SPI3_H */
