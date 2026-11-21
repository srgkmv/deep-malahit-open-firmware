#include <stddef.h>

#include "stm32h7xx.h"
#include "fram.h"

/*
 * SPI2, polled, full duplex, mode 0, 8-bit frames - the same peripheral
 * as spi3.c's and the same H7 rules, written out again rather than
 * shared because spi3.c is now the panel's DMA driver and this is ten
 * lines of it. The rules, in the order that bit:
 *
 *   - SSI before MASTER, or a mode fault clears MASTER silently
 *     (spi3.c, spi3_init());
 *   - TSIZE per run, SPE then CSTART, and SPE off after EOT - which is
 *     also the workaround for the H7 erratum about CSTART set within an
 *     SCK of the previous EOT;
 *   - TXDR and RXDR through BYTE pointers: a word access moves four
 *     frames.
 */

#define CS_PORT     GPIOB
#define CS_PIN      12U

#define WAIT_LIMIT  2000000U

static uint32_t s_timeouts;

uint32_t fram_timeouts(void) { return s_timeouts; }

static void cs_low(void)
{
    CS_PORT->BSRR = (1UL << (CS_PIN + 16U));
}

/*
 * CS high, and kept high a while: the part wants 60 ns between
 * transactions (an SPI EEPROM in the same footprint up to 100 ns), and
 * at 480 MHz the next call's CS_LOW can follow in less. Eight reads
 * across to the GPIO port are well over that.
 */
static void cs_high(void)
{
    CS_PORT->BSRR = (1UL << CS_PIN);
    for (uint32_t i = 0; i < 8U; i++) { (void)CS_PORT->IDR; }
}

/*
 * AFR BEFORE MODER - the lesson i2c3.c paid for the same day: on a cold
 * chip AFR is 0, and a pin switched to "alternate function" before its
 * function is chosen drives whatever AF0 is for those few cycles.
 */
static void gpio_af(uint32_t pin, uint32_t pupd)
{
    GPIOB->AFR[pin >> 3U] = (GPIOB->AFR[pin >> 3U]
                             & ~(0xFUL << ((pin & 7U) * 4U)))
                          | (5UL << ((pin & 7U) * 4U));
    GPIOB->OTYPER  &= ~(1UL << pin);
    GPIOB->OSPEEDR  = (GPIOB->OSPEEDR & ~(3UL << (pin * 2U)))
                    | (2UL << (pin * 2U));          /* high, not very high */
    GPIOB->PUPDR    = (GPIOB->PUPDR & ~(3UL << (pin * 2U)))
                    | (pupd << (pin * 2U));
    GPIOB->MODER    = (GPIOB->MODER & ~(3UL << (pin * 2U)))
                    | (2UL << (pin * 2U));
}

void fram_init(uint32_t mbr)
{
    RCC->AHB4ENR  |= RCC_AHB4ENR_GPIOBEN;
    RCC->APB1LENR |= RCC_APB1LENR_SPI2EN;
    (void)RCC->APB1LENR;

    /* The same mux spi3_init() writes, written here too: what this file
     * depends on, it says. 000 = pll1_q_ck, 240 MHz. */
    RCC->D2CCIP1R &= ~RCC_D2CCIP1R_SPI123SEL_Msk;

    /* CS: the level first, then the mode, so it never glitches low. R37
     * holds it high until here. */
    CS_PORT->BSRR   = (1UL << CS_PIN);
    CS_PORT->OTYPER &= ~(1UL << CS_PIN);
    CS_PORT->MODER  = (CS_PORT->MODER & ~(3UL << (CS_PIN * 2U)))
                    | (1UL << (CS_PIN * 2U));

    gpio_af(13U, 0U);       /* SCK  */
    gpio_af(14U, 1U);       /* MISO - pulled up: nobody driving reads 0xFF */
    gpio_af(15U, 0U);       /* MOSI */

    SPI2->CR1  = 0U;
    SPI2->IFCR = 0xFFFFFFFFU;
    SPI2->CR1  = SPI_CR1_SSI;
    SPI2->CFG1 = (7UL << SPI_CFG1_DSIZE_Pos)
               | ((mbr & 7UL) << SPI_CFG1_MBR_Pos);
    /* Master, full duplex (COMM = 00), software NSS, pins kept driven
     * between runs, CPOL = CPHA = 0. */
    SPI2->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_AFCNTR;
}

static int wait_sr(uint32_t bit)
{
    for (uint32_t n = WAIT_LIMIT; n != 0U; n--) {
        if (SPI2->SR & bit) { return 1; }
    }
    s_timeouts++;
    return 0;
}

/* One run of n frames, n <= 65535. tx NULL sends zeros, rx NULL
 * discards. */
static void run(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
    volatile uint8_t *txdr = (volatile uint8_t *)&SPI2->TXDR;
    volatile uint8_t *rxdr = (volatile uint8_t *)&SPI2->RXDR;

    SPI2->CR1 &= ~SPI_CR1_SPE;
    SPI2->IFCR = 0xFFFFFFFFU;
    SPI2->CR2  = n;
    SPI2->CR1 |= SPI_CR1_SPE;
    SPI2->CR1 |= SPI_CR1_CSTART;

    for (uint32_t i = 0; i < n; i++) {
        if (!wait_sr(SPI_SR_TXP)) { break; }
        *txdr = (tx != NULL) ? tx[i] : 0x00U;
        if (!wait_sr(SPI_SR_RXP)) { break; }
        uint8_t b = *rxdr;
        if (rx != NULL) { rx[i] = b; }
    }

    (void)wait_sr(SPI_SR_EOT);
    SPI2->IFCR = 0xFFFFFFFFU;
    SPI2->CR1 &= ~SPI_CR1_SPE;
}

uint8_t fram_rdsr(void)
{
    const uint8_t tx[2] = { FRAM_RDSR, 0x00U };
    uint8_t       rx[2] = { 0U, 0U };

    cs_low();
    run(tx, rx, 2U);
    cs_high();
    return rx[1];
}

void fram_wren(void)
{
    const uint8_t op = FRAM_WREN;

    cs_low();
    run(&op, NULL, 1U);
    cs_high();
}

void fram_wrsr(uint8_t sr)
{
    const uint8_t tx[2] = { FRAM_WRSR, sr };

    fram_wren();
    cs_low();
    run(tx, NULL, 2U);
    cs_high();
}

void fram_cmd_read(uint8_t op, uint8_t *buf, uint32_t n)
{
    cs_low();
    run(&op, NULL, 1U);
    if (n > 0U) { run(NULL, buf, n); }
    cs_high();
}

static void header(uint8_t op, uint32_t addr, uint8_t h[3])
{
    h[0] = op;
    h[1] = (uint8_t)((addr >> 8) & 0x7FU);
    h[2] = (uint8_t)addr;
}

void fram_read(uint32_t addr, uint8_t *buf, uint32_t n)
{
    uint8_t h[3];

    header(FRAM_READ, addr, h);
    cs_low();
    run(h, NULL, 3U);
    if (n > 0U) { run(NULL, buf, n); }
    cs_high();
}

void fram_write(uint32_t addr, const uint8_t *buf, uint32_t n)
{
    uint8_t h[3];

    header(FRAM_WRITE, addr, h);
    fram_wren();
    cs_low();
    run(h, NULL, 3U);
    if (n > 0U) { run(buf, NULL, n); }
    cs_high();
}
