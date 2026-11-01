#include <stddef.h>

#include "stm32h7xx.h"
#include "spi3.h"

/*
 * The H7's SPI is not the F4's, and transcribing an F4 driver here does
 * not work. The differences that matter:
 *
 *   - The bit rate divider lives in CFG1.MBR, not CR1.BR.
 *   - Transfers are counted. CR2.TSIZE says how many frames are coming;
 *     the peripheral raises EOT when that many have gone, and refuses to
 *     start another run until it is told again.
 *   - Enabling the peripheral is not enough to make it transmit. SPE
 *     turns it on, CSTART starts the transfer. Two separate writes, in
 *     that order.
 *   - Writing a byte means an 8-bit write to TXDR. A 32-bit write pushes
 *     four frames' worth into the FIFO and the panel receives rubbish.
 *     The cast below is load-bearing.
 */

#define PLL1Q_HZ    240000000U

static spi3_div_t s_div = SPI3_DIV16;

uint32_t spi3_hz(void)
{
    return PLL1Q_HZ >> (s_div + 1U);
}

static void gpio_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (2UL << (pin * 2U));
    port->OSPEEDR |= (3UL << (pin * 2U));            /* very high speed */
    port->OTYPER &= ~(1UL << pin);
    port->PUPDR &= ~(3UL << (pin * 2U));
    port->AFR[pin >> 3U] =
          (port->AFR[pin >> 3U] & ~(0xFUL << ((pin & 7U) * 4U)))
        | (af << ((pin & 7U) * 4U));
}

void spi3_set_div(spi3_div_t div)
{
    s_div = div;

    SPI3->CR1 &= ~SPI_CR1_SPE;
    SPI3->CFG1 = (SPI3->CFG1 & ~SPI_CFG1_MBR_Msk)
               | ((uint32_t)div << SPI_CFG1_MBR_Pos);
}

void spi3_init(spi3_div_t div)
{
    RCC->AHB4ENR  |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOCEN
                   | RCC_AHB4ENR_GPIODEN;
    RCC->APB1LENR |= RCC_APB1LENR_SPI3EN;
    (void)RCC->APB1LENR;

    /*
     * SAY WHICH KERNEL CLOCK. DO NOT INHERIT ONE.
     *
     * SPI123SEL defaults to 000 = pll1_q_ck, and this code assumed the
     * default. It should not have: an image loaded over SWD inherits
     * every RCC setting the previous firmware left behind, and the
     * factory firmware here sets SPI123SEL = 100 = per_ck, which is HSI
     * at 64 MHz.
     *
     * The symptom was a bus that measured exactly 3.75 times slower than
     * it should - 240/64 - at every divider, with the times scaling
     * perfectly as the divider changed. Perfect scaling says the bus is
     * the limit; the constant factor says the bus is not the speed you
     * think. Both were true at once, which is why it took a readback of
     * D2CCIP1R to see.
     */
    RCC->D2CCIP1R &= ~RCC_D2CCIP1R_SPI123SEL_Msk;   /* 000 = pll1_q_ck */

    gpio_af(GPIOC, 10U, 6U);   /* SCK  */
    gpio_af(GPIOC, 11U, 6U);   /* MISO - configured, not yet used */
    gpio_af(GPIOC, 12U, 6U);   /* MOSI */

    s_div = div;

    SPI3->CR1  = 0U;
    SPI3->IFCR = 0xFFFFFFFFU;

    /*
     * SSI BEFORE CFG2, AND THIS ORDER IS NOT COSMETIC.
     *
     * With SSM set, the master's view of its own NSS input comes from
     * CR1.SSI. Enable master mode while SSI is still 0 and the
     * peripheral sees NSS pulled low - another master on the bus, as far
     * as it knows - raises a mode fault, and a mode fault CLEARS THE
     * MASTER BIT. In hardware, silently.
     *
     * What that looks like from the outside: CFG2 is written with
     * 0x84420000 and reads back 0x84020000, the transfer starts, TXTF
     * says the FIFO took the byte, and EOT never arrives because a slave
     * does not generate a clock. The panel stays dark and the firmware
     * hangs in a wait loop with every clock register looking perfect.
     */
    SPI3->CR1 = SPI_CR1_SSI;

    /*
     * 8-bit frames. FTHLV = 0 means the FIFO raises TXP with room for one
     * frame, which is what the byte-at-a-time loop below wants.
     */
    SPI3->CFG1 = (7UL << SPI_CFG1_DSIZE_Pos)
               | ((uint32_t)div << SPI_CFG1_MBR_Pos);

    /*
     * Master, simplex transmitter, software slave management.
     *
     * COMM = 01 is transmit-only: it stops the peripheral from clocking
     * data in and filling the receive FIFO with noise that nobody drains
     * - which, once the FIFO is full, stalls the transmitter and hangs
     * the loop below. A full-duplex configuration here would appear to
     * work for short writes and die on the first screen fill.
     *
     * AFCNTR keeps the pins driven while the peripheral is between
     * transfers, so SCK does not float around and clock the panel by
     * accident.
     *
     * Mode 0 (CPOL = CPHA = 0), which is what the ILI9488 expects.
     */
    SPI3->CFG2 = SPI_CFG2_MASTER
               | (1UL << SPI_CFG2_COMM_Pos)
               | SPI_CFG2_SSM
               | SPI_CFG2_AFCNTR;
}

uint32_t spi3_check(void)
{
    /*
     * Read CFG2 back and say whether master mode survived. Cheap, and it
     * turns the failure above from a silent hang into one printed line.
     * Anything that can be cleared by hardware behind your back deserves
     * a readback.
     */
    return SPI3->CFG2;
}

/* One run of up to 65535 frames. The peripheral counts them and will not
 * start the next run until TSIZE is written again. */
static void run_block(const uint8_t *data, uint32_t len,
                      const uint8_t *pattern, uint32_t pattern_len)
{
    SPI3->CR1 &= ~SPI_CR1_SPE;
    SPI3->IFCR = 0xFFFFFFFFU;
    SPI3->CR2  = len;
    SPI3->CR1 |= SPI_CR1_SPE;
    SPI3->CR1 |= SPI_CR1_CSTART;

    volatile uint8_t *txdr = (volatile uint8_t *)&SPI3->TXDR;

    if (data != NULL) {
        for (uint32_t i = 0; i < len; i++) {
            while (!(SPI3->SR & SPI_SR_TXP)) { }
            *txdr = data[i];
        }
    } else {
        uint32_t p = 0;
        for (uint32_t i = 0; i < len; i++) {
            while (!(SPI3->SR & SPI_SR_TXP)) { }
            *txdr = pattern[p];
            if (++p == pattern_len) { p = 0; }
        }
    }

    /* EOT, not TXC: EOT is the one that means the shift register is empty
     * and the last bit is on the wire. Dropping CS on TXC clips it. */
    while (!(SPI3->SR & SPI_SR_EOT)) { }

    SPI3->IFCR = 0xFFFFFFFFU;
    SPI3->CR1 &= ~SPI_CR1_SPE;
}

void spi3_write(const uint8_t *data, uint32_t len)
{
    while (len > 0U) {
        uint32_t chunk = (len > 65535U) ? 65535U : len;
        run_block(data, chunk, NULL, 0U);
        data += chunk;
        len  -= chunk;
    }
}

void spi3_write_repeat(const uint8_t *pattern, uint32_t pattern_len,
                       uint32_t repeats)
{
    uint32_t total = pattern_len * repeats;

    /*
     * Chunks have to land on a pattern boundary, or a three-byte pixel
     * gets split across two runs and every colour after it is shifted by
     * a byte. 65535 is not a multiple of 3, so round down to one.
     */
    uint32_t max_chunk = (65535U / pattern_len) * pattern_len;

    while (total > 0U) {
        uint32_t chunk = (total > max_chunk) ? max_chunk : total;
        run_block(NULL, chunk, pattern, pattern_len);
        total -= chunk;
    }
}
