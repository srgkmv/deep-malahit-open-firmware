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

/*
 * TRANSMIT BY DMA: DMA1 stream 2, DMAMUX1 channel 2, request 62.
 *
 * What it buys is not bus time - 30 MHz is 30 MHz - but a bus that keeps
 * running while the CPU is somewhere else. The polled loop above stops
 * the moment an interrupt takes the core, the 16-byte FIFO empties in
 * 4 us, and the clock waits for the CPU to come back; with the AM
 * demodulator in the block interrupt ~40 % of the time, the 121 kB
 * waterfall measured 48 ms against the 32 ms its bytes need. A DMA
 * stream feeds the FIFO through all of that.
 *
 * CS and DC are NOT handled here: they are the panel driver's, and the
 * bus belongs to whoever started the transfer until spi3_dma_wait()
 * returns. None of the polled functions above may be called in between;
 * they call spi3_dma_wait() first themselves, as a backstop, but the
 * caller still owns CS and has to raise it.
 */
#define SPI3_DMA_MAX    65535U      /* CR2.TSIZE is sixteen bits */

/*
 * Clock DMA1, take request 62 back from any other multiplexer channel
 * (the factory firmware has it wired to DMA1 stream 2 - see spi3.c),
 * and set the stream up. Once, after spi3_init(). Returns 0.
 */
int spi3_dma_init(void);

/* Multiplexer channels that were found wired to request 62 and taken
 * back by spi3_dma_init(), as a bitmask. 0 is the normal answer. */
uint32_t spi3_dma_mux_released(void);

/*
 * Start sending len bytes (1..SPI3_DMA_MAX) and return at once. `data`
 * must stay untouched until spi3_dma_wait() returns. Returns 0, or -1 if
 * nothing was started: not initialised, len out of range, or a buffer
 * the DMA cannot reach (DTCM, ITCM) - the caller then sends it polled.
 *
 * eot_irq: also raise SPI3's interrupt at EOT, for a caller that chains
 * transfers from SPI3_IRQHandler (ili9488.c). The handler collects the
 * transfer with spi3_dma_wait(), which returns at once when EOT is
 * already up, and the teardown switches the interrupt off again.
 */
int spi3_dma_start(const uint8_t *data, uint32_t len, int eot_irq);

/* 1 between spi3_dma_start() and the spi3_dma_wait() that collects it. */
int spi3_dma_busy(void);

/*
 * Wait for the last bit to leave the shift register, then hand the
 * peripheral back to the polled functions. 0 on success. -1 if it did
 * not finish in twice its own time plus 10 ms, -2 if the stream
 * reported a transfer error; either way the transfer has been ABORTED
 * - some bytes never left - and the bus is usable again.
 */
int spi3_dma_wait(void);

/* Counters: failed transfers, and the error flags ever seen on stream 2
 * (bit 0 FEIF, 2 DMEIF, 3 TEIF), sticky. */
uint32_t spi3_dma_failures(void);
uint32_t spi3_dma_flags_seen(void);

/* Transfers collected since boot, and how many of them ended with FEIF
 * set. FEIF in direct mode stops nothing and loses nothing, so it is not
 * a failure; whether it is every transfer or some is what tells a
 * configuration artefact from a bus that was late. */
void spi3_dma_counts(uint32_t *done, uint32_t *fe);

#endif /* MALAHIT_SPI3_H */
