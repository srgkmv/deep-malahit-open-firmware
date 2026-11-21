#include <stddef.h>

#include "stm32h7xx.h"
#include "i2c3.h"
#include "board.h"
#include "clock.h"      /* clock_sysclk_hz, for the bit timing below */

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

/*
 * EVERYTHING ELSE FIRST, MODER LAST - 21/11/2026.
 *
 * It was MODER first and AFR last, and on a cold chip AFR is 0: for the
 * few bus cycles in between, PA8 was AF0 = MCO1 and PC9 AF0 = MCO2, and
 * an MCO with no clock routed to it drives low. Two short pulls to
 * ground on SCL and SDA at every power-on - a START and a STOP to
 * anything listening - and i2c3_bus_recover(), reading SDA straight
 * after handing the pins back, caught the line still rising and
 * reported "-1 clocks" on every cold boot, four of four. Warm loads
 * never showed it, because the previous image had left AFR at 4.
 */
static void gpio_af_od(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    port->AFR[pin >> 3U] =
          (port->AFR[pin >> 3U] & ~(0xFUL << ((pin & 7U) * 4U)))
        | (af << ((pin & 7U) * 4U));
    port->OTYPER |= (1UL << pin);                    /* open drain */
    port->OSPEEDR |= (3UL << (pin * 2U));
    /* Weak internal pull-ups as a safety net. The flex should carry
     * real ones; these are too weak to make a marginal bus work, but
     * they stop a disconnected line from floating and reading as a
     * stuck-low bus, which looks like a very different fault. */
    port->PUPDR = (port->PUPDR & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (2UL << (pin * 2U));
}

/*
 * ======================================================================
 * UNSTICK THE BUS BEFORE USING IT - 03/11/2026
 * ======================================================================
 *
 * THE SYMPTOM. `swd.sh ram` loads a new image, the touch panel reports
 * "no answer at 0x38", the bus scan finds NOTHING at all, and SDA
 * reads LOW with SCL high. Power-cycle the radio and the same image
 * works perfectly. It is intermittent in the way that wastes the most
 * time: it depends on exactly where the core was halted.
 *
 * THE CAUSE is WORKING.md's own headline lesson - loading over SWD
 * resets nothing - one level below the MCU. A slave that is shifting
 * out a byte drives SDA between clocks and only lets go when it has
 * been clocked to the end. Halting the core mid-transfer leaves the
 * FT6236 holding the line down and waiting for the clocks that will
 * now never come. OUR RESET PIN DOES NOT HELP: PC7 is pulsed by
 * i2c3_touch_reset() and the part does come back, but by then the
 * peripheral has already tried a transfer on a bus it could not
 * arbitrate, and a device that is mid-byte when reset can latch low
 * again on the next start.
 *
 * The MCU cannot fix this as a master, either: every transfer begins
 * with a START, which needs a falling edge on SDA, and SDA is already
 * low. There is nothing to drive.
 *
 * THE FIX is the standard one and it is mechanical: take the two pins
 * back as plain GPIO and CLOCK THE SLAVE OUT of whatever byte it is
 * in - up to nine pulses, one for each bit plus the ACK - watching SDA
 * after every one. Then make a STOP by hand, which is what tells the
 * device the transfer is over rather than merely finished.
 *
 * Nine is not a guess: the longest a slave can hold the line is the
 * eight bits it has left plus the acknowledge.
 *
 * It runs unconditionally at init rather than only when SDA reads low.
 * On a clean bus it costs 90 microseconds and nine clock pulses that
 * no device will mistake for a transfer, because no START precedes
 * them. Making it conditional would mean the recovery path is the one
 * that almost never runs, which is the path that is broken when it
 * finally does.
 */
/* How many clocks the last recovery needed, or -1 if SDA never came
 * back. Reported by the caller: a bus that needs recovering EVERY time
 * is a different problem from one that needs it after a reload. */
static int s_recover_clocks;

int i2c3_recover_clocks(void) { return s_recover_clocks; }

/*
 * WHERE SDA GOES LOW, SAMPLED AT EVERY STEP OF init.
 *
 * Needed because two honest measurements disagreed: the bus recovery
 * reported a clean bus needing zero clocks, and the diagnostic a
 * moment later found SDA low. Both were right about different
 * instants, and nothing in between was being looked at. Five samples
 * cost five register reads and settle it.
 */
static uint8_t s_lv[I2C3_LV_N];

uint8_t i2c3_level_at(uint32_t i)
{
    return (i < I2C3_LV_N) ? s_lv[i] : 0xFFU;
}

static void sda_scl_gpio(void)
{
    /* Open drain, no alternate function: the pin only ever pulls low
     * or lets the pull-up win, exactly like the I2C cell. */
    GPIOA->MODER   = (GPIOA->MODER & ~(3UL << (8U * 2U))) | (1UL << (8U * 2U));
    GPIOA->OTYPER |= (1UL << 8U);
    GPIOA->BSRR    = (1UL << 8U);               /* SCL released */

    GPIOC->MODER   = (GPIOC->MODER & ~(3UL << (9U * 2U))) | (1UL << (9U * 2U));
    GPIOC->OTYPER |= (1UL << 9U);
    GPIOC->BSRR    = (1UL << 9U);               /* SDA released */
}

/* Half a bit at 100 kHz. Spin on the cycle counter rather than
 * SysTick: this runs at init, where a microsecond delay has no other
 * source, and it must work before any of the rest is trusted. */
static void half_bit(void)
{
    uint64_t t0 = board_cycles();
    uint64_t n  = (uint64_t)(clock_sysclk_hz / 200000U);   /* 5 us */

    while ((board_cycles() - t0) < n) { }
}

#define SDA_HIGH()  ((GPIOC->IDR & (1UL << 9U)) != 0U)

/* PE off, and PROVEN off: the reference manual wants at least three
 * APB cycles low for the software reset to take, and two back-to-back
 * posted writes from a 480 MHz core to a 120 MHz bus are not that. A
 * reset that does not take leaves the peripheral in whatever state the
 * debugger halted it in - which is the whole problem here. */
static void pe_off(void)
{
    I2C3->CR1 &= ~I2C_CR1_PE;
    for (uint32_t n = WAIT_LIMIT; n != 0U; n--) {
        if ((I2C3->CR1 & I2C_CR1_PE) == 0U) { break; }
    }
    half_bit();
}

int i2c3_bus_recover(void)
{
    uint32_t i;

    /*
     * THE PERIPHERAL GOES FIRST, AND THE ORDER IS THE WHOLE POINT.
     *
     * I2C3 can arrive here still enabled and still mid-transfer,
     * holding SDA through its alternate function. Taking the pins as
     * GPIO makes it let go, so SDA reads HIGH and the clocking below
     * finds nothing to do - and then handing the pins back wakes the
     * same unreset peripheral, which pulls SDA down again. That is
     * exactly what was seen: a recovery reporting a clean bus needing
     * zero clocks, and `SCL=1 SDA=0` printed a moment later. Both
     * measurements were right about different instants.
     */
    pe_off();

    sda_scl_gpio();
    half_bit();

    for (i = 0U; i < 9U && !SDA_HIGH(); i++) {
        GPIOA->BSRR = (1UL << (8U + 16U));      /* SCL low  */
        half_bit();
        GPIOA->BSRR = (1UL << 8U);              /* SCL high */
        half_bit();
    }

    /*
     * A STOP by hand: SDA low while SCL is high, then SDA released.
     * The rising edge of SDA with SCL high is the stop condition, and
     * a slave that has been clocked free but never told the transfer
     * ended can still be waiting for it.
     */
    GPIOC->BSRR = (1UL << (9U + 16U));          /* SDA low  */
    half_bit();
    GPIOA->BSRR = (1UL << 8U);                  /* SCL high */
    half_bit();
    GPIOC->BSRR = (1UL << 9U);                  /* SDA high = STOP */
    half_bit();

    /*
     * Hand the pins back and restart the peripheral, so this is a
     * complete operation and not half of one. That is what lets it be
     * called again at run time, after the bus has hung a second time,
     * without the caller having to know how the pins were set up.
     *
     * TIMINGR survives PE = 0 - the software reset clears the state
     * machine, not the configuration - so i2c3_init() can write it
     * before calling this and it is still there afterwards.
     */
    gpio_af_od(GPIOA, 8U, 4U);    /* SCL */
    gpio_af_od(GPIOC, 9U, 4U);    /* SDA */
    I2C3->CR1 = I2C_CR1_PE;

    return SDA_HIGH() ? (int)i : -1;
}

void i2c3_init(void)
{
    RCC->AHB4ENR  |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOCEN;
    RCC->APB1LENR |= RCC_APB1LENR_I2C3EN;
    (void)RCC->APB1LENR;

    /* Say which kernel clock. 00 = rcc_pclk1. */
    RCC->D2CCIP2R &= ~RCC_D2CCIP2R_I2C123SEL_Msk;

    s_lv[0] = (uint8_t)i2c3_line_levels();

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

    /*
     * TIMINGR BEFORE THE RECOVERY, which is the only ordering subtlety
     * left here. It can only be written with PE clear, and the
     * recovery ends by turning PE back on - so it has to be in place
     * before, not after. PE = 0 resets the state machine and leaves
     * the configuration alone, so it survives.
     */
    pe_off();
    I2C3->TIMINGR = I2C3_TIMINGR;
    s_lv[1] = (uint8_t)i2c3_line_levels();

    /*
     * And the whole of the rest of bringing this bus up is the
     * recovery: reset the peripheral, clock any half-finished slave
     * free, issue a STOP, hand the pins to the alternate function and
     * enable. There is no separate "clean start" path, deliberately -
     * see i2c3_bus_recover(). A recovery path that only runs when
     * something is already wrong is the path that is broken when it
     * finally runs.
     */
    s_recover_clocks = i2c3_bus_recover();
    s_lv[2] = (uint8_t)i2c3_line_levels();
}

void i2c3_touch_reset(uint32_t wake_ms)
{
    TOUCH_RST_PORT->BSRR = (1UL << (TOUCH_RST_PIN + 16U));
    board_delay_ms(10);                 /* Trst, 5 ms minimum */
    TOUCH_RST_PORT->BSRR = (1UL << TOUCH_RST_PIN);
    /* How long the controller gets before anybody speaks to it is the
     * caller's - touch.c, FT_WAKE_MS, says why it matters. */
    board_delay_ms(wake_ms);
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

/*
 * ======================================================================
 * ABANDON A TRANSFER PROPERLY - 03/11/2026
 * ======================================================================
 *
 * Every error path here used to be `bus_clear_flags(); return 0;`,
 * which clears the sticky flags and tells the CALLER the transfer
 * failed without telling the BUS anything at all. The peripheral is
 * left mid-transfer, and in a read it is the slave that is driving
 * SDA - so the line stays low, the next transfer cannot even produce
 * a START, and every subsequent access fails. A single failed read
 * hangs the bus until something clocks it free.
 *
 * i2c3_write_read() is where it really bites, because its write phase
 * runs with AUTOEND OFF on purpose - the bus has to be held for the
 * repeated start that follows - so there is no hardware stop coming
 * to save it.
 *
 * WHAT MADE THIS VISIBLE was making touch_init() poll for the
 * controller's ID instead of waiting a fixed 120 ms, which was itself
 * a fix for a real problem. One read that fails during the part's
 * wake-up window went from an occasional annoyance to sixty attempts
 * where the first failure poisons all fifty-nine that follow. The
 * symptom was a touch panel that worked perhaps half the time, with
 * SDA low afterwards and a bus scan that found nothing - and it
 * looked exactly like inherited state from the previous image, which
 * is what sent the search in the wrong direction first.
 *
 * A stop condition is the one thing that ends a transfer for both
 * ends of the bus rather than only for this end.
 */
static void bus_stop(void)
{
    I2C3->CR2 |= I2C_CR2_STOP;

    for (uint32_t n = WAIT_LIMIT; n != 0U; n--) {
        if ((I2C3->ISR & I2C_ISR_STOPF) != 0U) { break; }
    }

    I2C3->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF
              | I2C_ICR_ARLOCF | I2C_ICR_OVRCF;
    (void)I2C3->ISR;

    /*
     * AND IF THE SLAVE IS STILL HOLDING SDA, CLOCK IT FREE.
     *
     * A stop condition ends the transfer for the MASTER. A slave that
     * was in the middle of shifting a byte out is driving SDA between
     * clocks and lets go only when it has been clocked to the end of
     * that byte - so after a failed read the line can stay down, and
     * then no START is possible and every later transfer fails too.
     *
     * MEASURED, not assumed. Bringing the touch panel up, the first
     * four register reads after its reset succeed and the fifth
     * fails, roughly 45 ms into the part's wake-up; from that moment
     * SDA was low for the remaining 115 attempts and for the rest of
     * the run. Adding the stop above did not change it, which is what
     * said the problem was the slave's shift register and not the
     * master's state machine.
     *
     * So the abort path finishes the job: stop, and if that was not
     * enough, nine clocks and another stop. An error return now
     * always leaves a bus the next call can use, which is the only
     * useful meaning of "this transfer failed".
     */
    if ((i2c3_line_levels() & 1U) == 0U) {
        (void)i2c3_bus_recover();
    }
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
        if (!wait_flag(I2C_ISR_TXIS, NULL)) { bus_stop(); return 0; }
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
        if (!wait_flag(I2C_ISR_RXNE, NULL)) { bus_stop(); return 0; }
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
        if (!wait_flag(I2C_ISR_TXIS, NULL)) { bus_stop(); return 0; }
        I2C3->TXDR = wdata[i];
    }
    if (!wait_flag(I2C_ISR_TC, NULL)) { bus_stop(); return 0; }

    start(addr7, rlen, 1, 1);

    for (uint32_t i = 0; i < rlen; i++) {
        if (!wait_flag(I2C_ISR_RXNE, NULL)) { bus_stop(); return 0; }
        rdata[i] = (uint8_t)I2C3->RXDR;
    }

    int ok = wait_flag(I2C_ISR_STOPF, NULL);
    bus_clear_flags();
    return ok;
}
