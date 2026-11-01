#include <stddef.h>

#include "stm32h7xx.h"
#include "i2c3.h"
#include "board.h"

/*
 * TIMINGR, computed rather than copied from a reference manual table,
 * because no table covers a 120 MHz kernel clock.
 *
 *   PRESC  = 11  -> the timing unit is 12 / 120 MHz = 100 ns
 *   SCLL   = 49  -> low  for 50 units  = 5.0 us
 *   SCLH   = 49  -> high for 50 units  = 5.0 us   total 10 us = 100 kHz
 *   SDADEL = 2   -> 200 ns of data hold after the falling edge
 *   SCLDEL = 4   -> 500 ns of data setup before the rising edge
 *
 * 100 kHz on purpose. This is a bring-up driver whose first job is to
 * find out what is on the bus, and standard mode is what an unknown
 * device is least likely to argue with. Raising it is a later
 * decision, made once the part is known.
 */
#define I2C3_TIMINGR    0xB0423131UL

#define WAIT_LIMIT      500000U

#define TOUCH_RST_PORT  GPIOC
#define TOUCH_RST_PIN   7U
#define TOUCH_IRQ_PORT  GPIOC
#define TOUCH_IRQ_PIN   8U

static void gpio_af_od(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (2UL << (pin * 2U));
    port->OTYPER |= (1UL << pin);                    /* open drain */
    port->OSPEEDR |= (3UL << (pin * 2U));
    /* Weak internal pull-ups as a safety net. The flex should carry
     * real ones; these are too weak to make a marginal bus work, but
     * they stop a disconnected line from floating and reading as a
     * stuck-low bus, which looks like a very different fault. */
    port->PUPDR = (port->PUPDR & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
    port->AFR[pin >> 3U] =
          (port->AFR[pin >> 3U] & ~(0xFUL << ((pin & 7U) * 4U)))
        | (af << ((pin & 7U) * 4U));
}

void i2c3_init(void)
{
    RCC->AHB4ENR  |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOCEN;
    RCC->APB1LENR |= RCC_APB1LENR_I2C3EN;
    (void)RCC->APB1LENR;

    /* Say which kernel clock. 00 = rcc_pclk1. */
    RCC->D2CCIP2R &= ~RCC_D2CCIP2R_I2C123SEL_Msk;

    gpio_af_od(GPIOA, 8U, 4U);    /* SCL */
    gpio_af_od(GPIOC, 9U, 4U);    /* SDA */

    /* Reset line: push-pull output, released (high). */
    TOUCH_RST_PORT->MODER = (TOUCH_RST_PORT->MODER
                             & ~(3UL << (TOUCH_RST_PIN * 2U)))
                          | (1UL << (TOUCH_RST_PIN * 2U));
    TOUCH_RST_PORT->BSRR = (1UL << TOUCH_RST_PIN);

    /* IRQ line: input with a pull-up, since it is active low and the
     * controller drives it open drain on most parts. */
    TOUCH_IRQ_PORT->MODER &= ~(3UL << (TOUCH_IRQ_PIN * 2U));
    TOUCH_IRQ_PORT->PUPDR = (TOUCH_IRQ_PORT->PUPDR
                             & ~(3UL << (TOUCH_IRQ_PIN * 2U)))
                          | (1UL << (TOUCH_IRQ_PIN * 2U));

    I2C3->CR1     = 0U;                 /* PE off while configuring */
    I2C3->TIMINGR = I2C3_TIMINGR;
    I2C3->CR1     = I2C_CR1_PE;
}

void i2c3_touch_reset(void)
{
    TOUCH_RST_PORT->BSRR = (1UL << (TOUCH_RST_PIN + 16U));
    board_delay_ms(10);
    TOUCH_RST_PORT->BSRR = (1UL << TOUCH_RST_PIN);
    /* Controllers in this class want tens of milliseconds before they
     * will answer. Being generous costs nothing once at startup. */
    board_delay_ms(120);
}

int i2c3_touch_irq_asserted(void)
{
    return ((TOUCH_IRQ_PORT->IDR & (1UL << TOUCH_IRQ_PIN)) == 0U) ? 1 : 0;
}

/*
 * Clear the sticky flags AND MAKE SURE THE CLEAR HAS LANDED.
 *
 * Writing ICR and immediately reading ISR does not work: the write
 * crosses to a 120 MHz peripheral bus and the read can overtake it, so
 * the next transfer starts by seeing the PREVIOUS transfer's STOPF and
 * concludes instantly that it has finished.
 *
 * What that looked like: a bus scan that reported a device at every odd
 * address and nothing at any even one. Every second probe was reading
 * the leftovers of the one before it, which is a very convincing
 * imitation of hardware.
 *
 * So read the flags back until they really are clear, and wait for the
 * bus to be free before starting anything. Bounded, because a bus held
 * low by a confused device must produce an error rather than a hang.
 */
static int bus_idle(void)
{
    I2C3->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF
              | I2C_ICR_ARLOCF | I2C_ICR_OVRCF;

    for (uint32_t n = WAIT_LIMIT; n != 0U; n--) {
        uint32_t isr = I2C3->ISR;
        if (((isr & (I2C_ISR_STOPF | I2C_ISR_NACKF | I2C_ISR_BERR
                     | I2C_ISR_ARLO)) == 0U)
            && ((isr & I2C_ISR_BUSY) == 0U)) {
            return 1;
        }
    }
    return 0;
}

static void bus_clear_flags(void)
{
    I2C3->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF
              | I2C_ICR_ARLOCF | I2C_ICR_OVRCF;
    (void)I2C3->ISR;
}

/*
 * What the two bus lines are actually doing, read as plain inputs.
 *
 * Worth asking before blaming software: both should idle HIGH, pulled
 * up by resistors on the panel flex. A line stuck low means no
 * pull-ups, a device holding the bus, or the wrong pins entirely - and
 * every one of those produces scan results that look like devices.
 */
uint32_t i2c3_line_levels(void)
{
    uint32_t scl = (GPIOA->IDR >> 8U) & 1U;    /* PA8 */
    uint32_t sda = (GPIOC->IDR >> 9U) & 1U;    /* PC9 */
    return (scl << 1U) | sda;
}

static int wait_flag(uint32_t mask, uint32_t *isr_out)
{
    for (uint32_t n = WAIT_LIMIT; n != 0U; n--) {
        uint32_t isr = I2C3->ISR;
        if (isr & (mask | I2C_ISR_NACKF | I2C_ISR_BERR | I2C_ISR_ARLO)) {
            if (isr_out != NULL) { *isr_out = isr; }
            return (isr & mask) ? 1 : 0;
        }
    }
    if (isr_out != NULL) { *isr_out = I2C3->ISR; }
    return 0;
}

static void start(uint8_t addr7, uint32_t nbytes, int read, int autoend)
{
    uint32_t cr2 = ((uint32_t)addr7 << 1U) & I2C_CR2_SADD_Msk;
    cr2 |= (nbytes << I2C_CR2_NBYTES_Pos) & I2C_CR2_NBYTES_Msk;
    if (read)    { cr2 |= I2C_CR2_RD_WRN; }
    if (autoend) { cr2 |= I2C_CR2_AUTOEND; }
    cr2 |= I2C_CR2_START;
    I2C3->CR2 = cr2;
}

int i2c3_probe(uint8_t addr7)
{
    if (!bus_idle()) { return 0; }

    start(addr7, 0U, 0, 1);

    uint32_t isr = 0;
    for (uint32_t n = WAIT_LIMIT; n != 0U; n--) {
        isr = I2C3->ISR;
        if (isr & (I2C_ISR_STOPF | I2C_ISR_BERR | I2C_ISR_ARLO)) { break; }
    }

    int acked = ((isr & I2C_ISR_STOPF) != 0U)
             && ((isr & I2C_ISR_NACKF) == 0U);

    bus_clear_flags();
    return acked;
}

int i2c3_write(uint8_t addr7, const uint8_t *data, uint32_t len)
{
    if (!bus_idle()) { return 0; }
    start(addr7, len, 0, 1);

    for (uint32_t i = 0; i < len; i++) {
        if (!wait_flag(I2C_ISR_TXIS, NULL)) { bus_clear_flags(); return 0; }
        I2C3->TXDR = data[i];
    }

    int ok = wait_flag(I2C_ISR_STOPF, NULL);
    bus_clear_flags();
    return ok;
}

int i2c3_read(uint8_t addr7, uint8_t *data, uint32_t len)
{
    if (!bus_idle()) { return 0; }
    start(addr7, len, 1, 1);

    for (uint32_t i = 0; i < len; i++) {
        if (!wait_flag(I2C_ISR_RXNE, NULL)) { bus_clear_flags(); return 0; }
        data[i] = (uint8_t)I2C3->RXDR;
    }

    int ok = wait_flag(I2C_ISR_STOPF, NULL);
    bus_clear_flags();
    return ok;
}

int i2c3_write_read(uint8_t addr7,
                    const uint8_t *wdata, uint32_t wlen,
                    uint8_t *rdata, uint32_t rlen)
{
    if (!bus_idle()) { return 0; }

    /* Write phase WITHOUT autoend, so the bus is held for the repeated
     * start that follows. TC - transfer complete - is the flag that
     * says the write finished and the peripheral is waiting to be told
     * what comes next. */
    start(addr7, wlen, 0, 0);

    for (uint32_t i = 0; i < wlen; i++) {
        if (!wait_flag(I2C_ISR_TXIS, NULL)) { bus_clear_flags(); return 0; }
        I2C3->TXDR = wdata[i];
    }
    if (!wait_flag(I2C_ISR_TC, NULL)) { bus_clear_flags(); return 0; }

    start(addr7, rlen, 1, 1);

    for (uint32_t i = 0; i < rlen; i++) {
        if (!wait_flag(I2C_ISR_RXNE, NULL)) { bus_clear_flags(); return 0; }
        rdata[i] = (uint8_t)I2C3->RXDR;
    }

    int ok = wait_flag(I2C_ISR_STOPF, NULL);
    bus_clear_flags();
    return ok;
}
