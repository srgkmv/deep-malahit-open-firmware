#include "stm32h7xx.h"
#include "board.h"
#include "clock.h"
#include "fram.h"
#include "spi_nor.h"
#include "spi_flash.h"

/*
 * SPI2 in endless runs for the flash on PB0 - see spi_nor.h. The H7
 * rules fram.c lists hold here too (SSI before MASTER, SPE off after a
 * run, TXDR/RXDR through byte pointers); what is new is TSIZE = 0, which
 * keeps the master clocking a byte for every byte written to TXDR until
 * CSUSP, and SUSP as the end of the run in place of EOT.
 */

#define CS_PORT     GPIOB
#define CS_PIN      0U

#define WAIT_LIMIT  2000000U

static uint32_t s_timeouts;
static uint8_t  s_running;

uint32_t spi_nor_timeouts(void) { return s_timeouts; }

static int wait_sr(uint32_t bit)
{
    for (uint32_t n = WAIT_LIMIT; n != 0U; n--) {
        if (SPI2->SR & bit) { return 1; }
    }
    s_timeouts++;
    return 0;
}

void spi_nor_init(void)
{
    s_timeouts = 0U;
    s_running  = 0U;
    fram_init(3U);                  /* the bus, and both chip selects high */
}

void spi_nor_cs(uint8_t level)
{
    if (level == 0U) {
        fram_bus_coge();            /* the USB disk's interrupt, if on */
        SPI2->CR1 &= ~SPI_CR1_SPE;
        SPI2->IFCR = 0xFFFFFFFFU;
        SPI2->CR2  = 0U;            /* TSIZE 0: endless, until CSUSP */
        SPI2->CR1 |= SPI_CR1_SPE;
        CS_PORT->BSRR = (1UL << (CS_PIN + 16U));
        /* Nothing in TXFIFO yet, so CSTART clocks nothing: the first byte
         * goes out when spi_nor_byte() writes it, with CS already low. */
        SPI2->CR1 |= SPI_CR1_CSTART;
        s_running = 1U;
        return;
    }

    if (s_running) {
        /* Every byte's answer has been read, so the wire is idle and the
         * suspend lands at once (SUSP with no polls, on the SWD check). */
        SPI2->CR1 |= SPI_CR1_CSUSP;
        (void)wait_sr(SPI_SR_SUSP);
        SPI2->IFCR = 0xFFFFFFFFU;
        SPI2->CR1 &= ~SPI_CR1_SPE;
        s_running = 0U;
    }
    CS_PORT->BSRR = (1UL << CS_PIN);
    /* tSHSL: 50 ns after a read, 100 ns after anything else - the same
     * eight reads across to the port fram.c's cs_high() uses. */
    for (uint32_t i = 0; i < 8U; i++) { (void)CS_PORT->IDR; }
    fram_bus_suelta();
}

uint8_t spi_nor_byte(uint8_t out)
{
    volatile uint8_t *txdr = (volatile uint8_t *)&SPI2->TXDR;
    volatile uint8_t *rxdr = (volatile uint8_t *)&SPI2->RXDR;

    if (!wait_sr(SPI_SR_TXP)) { return 0xFFU; }
    *txdr = out;
    if (!wait_sr(SPI_SR_RXP)) { return 0xFFU; }
    return *rxdr;
}

void spi_nor_delay_us(uint32_t us)
{
    uint64_t t0  = board_cycles();
    uint64_t len = (uint64_t)us * (clock_sysclk_hz / 1000000U);

    /* board_cycles() needs the SysTick interrupt to move past a
     * millisecond; the bound keeps a call with interrupts off from
     * hanging (it then waits ~us microseconds of loop, roughly). */
    for (uint32_t n = us * 64U + 64U; n != 0U; n--) {
        if (board_cycles() - t0 >= len) { break; }
    }
}

/* Upstream asks whether its bit-banged bus made it to full speed (the
 * info window shows it). This bus has one speed and no slow mode. */
uint8_t spi_flash_bus_rapido(void)
{
    return 1U;
}
