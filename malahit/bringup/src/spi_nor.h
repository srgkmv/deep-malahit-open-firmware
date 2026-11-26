#ifndef MALAHIT_SPI_NOR_H
#define MALAHIT_SPI_NOR_H

#include <stdint.h>

/*
 * The SPI NOR flash's transport: the bus under upstream's User/spi_flash.c.
 *
 * The chip is a Winbond W25Q64CV, 8 MB, that the owner soldered on
 * 26/11/2026 stacked on the FRAM D11: every pin in parallel with the
 * FRAM's but /CS, which is bent up and wired to PB0 (LQFP pin 34, the
 * schematic's RESERVED_PB0), with its own 10k pull-up to 3.3V - measured
 * over SWD on 26/11: PB0 reads 1 against the H7's internal pull-down.
 *
 *   PB0   flash /CS      plain GPIO, 10k pull-up
 *   PB12  FRAM /CS       plain GPIO, R37 10k pull-up
 *   PB13  SCK, PB14 MISO, PB15 MOSI - SPI2, AF5, both chips
 *
 * So SPI2 has two chips and fram.c is the bus's owner: fram_init() sets
 * the peripheral up and drives BOTH chip selects high. This file only
 * runs transfers on it, at fram_init()'s speed (15 MHz; the FRAM's limit
 * is 20, the flash's 50 on READ 0x03).
 *
 * Upstream's file talks to the chip a byte at a time between f_cs(0) and
 * f_cs(1). Here f_cs(0) starts an ENDLESS run of SPI2 (TSIZE = 0) and
 * f_cs(1) suspends it (CSUSP) and switches the peripheral off, so a byte
 * costs one TXDR write and one RXDR read, not a peripheral restart -
 * fram.c's fixed-size runs find it off with its flags clear, as they
 * leave it. The sequence was checked over SWD on the running radio
 * before this was written (26/11/2026): JEDEC EF 40 17 and the same
 * bytes as fixed runs read, SUSP at once after CSUSP, the FRAM fine
 * after.
 *
 * Both drivers run from the main loop, with one exception: the USB disk
 * (usbdisco.h) reads and writes the flash from its interrupt, the lowest
 * priority in the system. While it is on, every transaction started from
 * thread mode masks that interrupt from select to deselect
 * (fram_bus_coge()/fram_bus_suelta(), fram.h) - so the bus is never
 * shared mid-transaction, and needs no lock beyond that.
 */

/* Bus up (fram_init(3U) - both CS high) and the counters cleared. */
void    spi_nor_init(void);

/* 0 = select and start a run, 1 = end the run and deselect. */
void    spi_nor_cs(uint8_t level);

/* One byte each way inside a run. */
uint8_t spi_nor_byte(uint8_t out);

/* A real microsecond wait, from SysTick (board_cycles()). Upstream's
 * delay_us_approx() is a NOP loop timed on the GD32. */
void    spi_nor_delay_us(uint32_t us);

/* SPI waits that ran out - each one a byte that did not move. 0. */
uint32_t spi_nor_timeouts(void);

#endif /* MALAHIT_SPI_NOR_H */
