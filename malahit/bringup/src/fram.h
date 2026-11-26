#ifndef MALAHIT_FRAM_H
#define MALAHIT_FRAM_H

#include <stdint.h>

/*
 * FM25W256 - 32 kB of ferroelectric RAM - on SPI2, D11 on the schematic.
 *
 *   PB12  SPI2_FRAM_CS    plain GPIO, R37 10k pull-up to 3.3V
 *   PB13  SPI2_FRAM_SCK   AF5
 *   PB14  SPI2_FRAM_MISO  AF5
 *   PB15  SPI2_FRAM_MOSI  AF5
 *
 * Since 26/11/2026 the bus has a second chip, the owner's W25Q64 SPI NOR
 * flash stacked on this one with its /CS on PB0 (spi_nor.h). fram_init()
 * drives that select high too: this file owns the bus.
 *
 * /WP and /HOLD are tied to 3.3V on the board, so neither is ours to
 * drive: hardware write protection is off, and nothing can pause a
 * transfer.
 *
 * The part's whole command set is six opcodes - WREN, WRDI, RDSR, WRSR,
 * READ, WRITE - with a two-byte address of which the top bit is ignored.
 * Writes take effect as the bytes arrive: no page, no write cycle, no
 * busy bit. That difference from an SPI EEPROM in the same footprint is
 * what fram_probe-style tests lean on (main.c, test_fram()), because the
 * FM25W256 has no ID command to ask.
 *
 * Kernel clock: SPI1/2/3 share SPI123SEL, which spi3_init() sets to
 * pll1_q_ck = 240 MHz, so the dividers are the same as spi3.h's.
 */

#define FRAM_SIZE   32768U

#define FRAM_WREN   0x06U
#define FRAM_WRDI   0x04U
#define FRAM_RDSR   0x05U
#define FRAM_WRSR   0x01U
#define FRAM_READ   0x03U
#define FRAM_WRITE  0x02U

/* Status register bits. Bit 0 is WIP on an SPI EEPROM, and fixed at 0
 * on this part. */
#define FRAM_SR_WIP   0x01U
#define FRAM_SR_WEL   0x02U
#define FRAM_SR_BP    0x0CU
#define FRAM_SR_WPEN  0x80U

/*
 * Pins, peripheral, divider. `mbr` is CFG1.MBR: 2 = 30 MHz is over the
 * part's 20 MHz, 3 = 15 MHz is the working speed, 5 = 3.75 MHz is slow
 * enough for any SPI memory that could be sitting in this footprint.
 * Safe to call again to change speed.
 */
void fram_init(uint32_t mbr);

uint8_t fram_rdsr(void);
void    fram_wren(void);
void    fram_wrsr(uint8_t sr);       /* WREN first, as the part requires */

/*
 * Any opcode, then `n` bytes clocked in. For commands this part does not
 * have - RDID, 0x9F, is the one worth asking: other FRAMs answer it with
 * a JEDEC ID, and this one should leave MISO alone.
 */
void fram_cmd_read(uint8_t op, uint8_t *buf, uint32_t n);

/* addr is masked to the part's 15 bits; a run past the end wraps to 0,
 * which is the part's own behaviour. n up to 65535 per call. */
void fram_read(uint32_t addr, uint8_t *buf, uint32_t n);

/* WREN, then the write. Returns at once: on FRAM it is done when CS
 * rises. */
void fram_write(uint32_t addr, const uint8_t *buf, uint32_t n);

/* SPI waits that ran out - each one a byte that did not move. 0. */
uint32_t fram_timeouts(void);

/*
 * THE BUS AGAINST AN INTERRUPT (step 60). The USB disk's interrupt reads
 * and writes the SPI flash on this bus (usbdisco.h); fram_bus_irq() names
 * it while the disk is on (-1 when off), and every transaction started
 * from thread mode - this file's, and spi_nor.c's - masks it from select
 * to deselect with fram_bus_coge()/fram_bus_suelta().
 */
void fram_bus_irq(int32_t irqn);
void fram_bus_coge(void);
void fram_bus_suelta(void);

#endif /* MALAHIT_FRAM_H */
