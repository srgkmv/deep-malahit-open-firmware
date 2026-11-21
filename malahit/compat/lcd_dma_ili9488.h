/*
 * The lcd_dma contract, for a panel that is on SPI and not on a
 * parallel bus with a DMA channel behind it.
 *
 * Substituted the same way as the panel header - by declaring the
 * guard of the file it replaces:
 *
 *     -DLCD_DMA_H -include lcd_dma_ili9488.h
 *
 * See compat/rm68120_ili9488.h for why an -I cannot do this.
 *
 * gfx.c needs three of these. Upstream's are asynchronous: lcd_dma_manda()
 * hands a block of RGB565 to a DMA channel and returns, lcd_dma_hay()
 * says whether the DMA is there and switched on (this header used to
 * say "whether one is still in flight", which is not upstream's
 * meaning: "1 si el empuje por DMA esta disponible y encendido"), and
 * lcd_dma_espera() blocks until the block finishes. That is worth real time on the DEEPSDR, where the parallel
 * bus runs while the CPU composes the next band.
 *
 * HERE THEY WERE SYNCHRONOUS UNTIL 21/11/2026, deliberately: the SPI
 * driver was polled, and DMA belonged in a step about DMA. That step is
 * done (spi3.c, "TRANSMIT BY DMA"), and the contract is now kept like
 * this:
 *
 *   hay()     1 when the DMA passed its self-test at boot, has not failed
 *             since, and is switched on. While it says 0, upstream takes
 *             its synchronous paths, exactly as before.
 *   pon()     the switch, ili9488_dma_enable() underneath - which is
 *             the one to call over SWD for an A/B of the same picture in
 *             the same run (`swd-call.sh build/bringup.elf
 *             ili9488_dma_enable 0`): nothing compiled here calls pon(),
 *             so the linker drops it.
 *   manda()   converts the pixels into the driver's staging buffer
 *             (RGB565 to three bytes a pixel has to happen somewhere,
 *             and the panel cannot take RGB565 over SPI) and starts the
 *             DMA from there. It returns with the caller's buffer
 *             ALREADY FREE.
 *   espera()  so it has nothing of the caller's to wait for, and returns
 *             1 at once. The transfer still in flight is the driver's
 *             own, and the driver collects it before anything else
 *             touches the bus - upstream's rule that nobody may touch
 *             the panel while a block is going out is enforced inside
 *             ili9488.c instead of being left to every caller.
 *   fallos()  transfers that did not finish. Any one of them switches
 *             the DMA off for the rest of the run.
 *
 * WHERE THE TIME COMES FROM is not the overlap upstream wrote this for.
 * On the DEEPSDR the parallel bus is fast and the CPU's time is the
 * prize; here the bus is 30 MHz SPI either way, and the polled loop lost
 * bus time every time an interrupt took the core - the FIFO holds 4 us
 * and the AM block interrupt runs for milliseconds. The DMA keeps the
 * bus going through those.
 */
#ifndef MALAHIT_LCD_DMA_ILI9488_H
#define MALAHIT_LCD_DMA_ILI9488_H

#include <stdint.h>
/* The header this replaces pulled in NULL for its users; gfx.c relies
 * on that. A substitute has to provide what it stood in for. */
#include <stddef.h>

void     lcd_dma_init(void);
uint8_t  lcd_dma_hay(void);
void     lcd_dma_pon(uint8_t on);
uint8_t  lcd_dma_manda(const uint16_t *px, uint32_t n);
uint8_t  lcd_dma_espera(void);
uint16_t lcd_dma_fallos(void);

#endif
