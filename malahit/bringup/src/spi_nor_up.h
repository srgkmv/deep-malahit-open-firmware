#ifndef MALAHIT_SPI_NOR_UP_H
#define MALAHIT_SPI_NOR_UP_H

/*
 * Included INSIDE User/spi_flash.c, in place of its bit-banged bus, by
 * -DSPI_FLASH_TRANSPORTE='"spi_nor_up.h"' (reglas.mk). Upstream's #ifndef
 * there (26/11/2026) names what it needs: f_cs(), spi_xfer_byte(),
 * delay_us_approx(), spi_flash_bus_rapido() (src/spi_nor.c) and
 * spi_flash_transporte_init(). Everything else in that file - commands,
 * erase/program, FAT12, the async save - is upstream's, unchanged.
 */

#include "spi_nor.h"

static inline void f_cs(uint8_t level)
{
    spi_nor_cs(level);
}

static inline uint8_t spi_xfer_byte(uint8_t out)
{
    return spi_nor_byte(out);
}

/*
 * Upstream waits 1 us after every f_cs(0) - the select's setup for its
 * bit-banged clock - and 100 us between status polls while the chip is
 * busy. The first is 5 ns on the W25Q64 (tSLCH), already covered by the
 * CS write and CSTART, so it is skipped; the second is a real wait, and
 * upstream's timeouts (0.8 s, 10 s) are counted in it.
 */
static inline void delay_us_approx(uint32_t us)
{
    if (us > 1U) { spi_nor_delay_us(us); }
}

static inline void spi_flash_transporte_init(void)
{
    spi_nor_init();
}

#endif /* MALAHIT_SPI_NOR_UP_H */
