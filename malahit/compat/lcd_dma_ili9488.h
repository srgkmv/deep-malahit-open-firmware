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
 * says whether one is still in flight, lcd_dma_espera() blocks until it
 * finishes. That is worth real time on the DEEPSDR, where the parallel
 * bus runs while the CPU composes the next band.
 *
 * HERE THEY ARE SYNCHRONOUS, AND THAT IS A DELIBERATE SIMPLIFICATION
 * RATHER THAN AN OVERSIGHT. The SPI driver in this port is polled - the
 * D-cache against DMA is the one hazard PORTING.md section 3.4 singles
 * out, and it belongs in a step that is about DMA, with the MPU regions
 * that make it safe. So manda() transmits before it returns, hay()
 * always answers "nothing pending" and espera() always answers "done".
 *
 * The semantics stay honest: every caller that asks whether the blit
 * has finished is told the truth. What is lost is the overlap, which
 * costs frame rate and nothing else, and the measurement is already
 * written down - 138 ms of the 160 ms a full screen takes is SPI.
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
