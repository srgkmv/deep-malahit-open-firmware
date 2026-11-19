#include "stm32h7xx.h"
#include "board.h"
#include "clock.h"
#include "encoder.h"        /* ../../User/encoder.c, compiled in place */
#include "enc2.h"           /* the same file again, for the second knob */
#include "adc.h"

static volatile uint32_t s_millis;

/*
 * WHO ELSE RUNS AT 1 kHz.
 *
 * Upstream's encoder driver asks to be sampled at 1 kHz from
 * SysTick_Handler, in those words, and this board already had exactly
 * that - so the graft needed no new interrupt, no priority decision and,
 * most to the point, no named vector: startup_stm32h743.S has a name for
 * none of the 150 external interrupts, and adding one is a change to the
 * file that every fault handler lives in. Sampling here instead costs
 * three GPIO reads and a table lookup a millisecond.
 *
 * THE FLAG IS NOT TIMIDITY. SysTick starts in board_power_on(), which is
 * the second thing main() does; encoder_init() cannot run until the pins
 * are configured, which is later. Ticking before that samples a pin
 * still in its reset state - analog, reading zero - and every transition
 * it then invents lands in the quadrature accumulator, so the first turn
 * of the knob would be reported as a turn plus whatever the first
 * milliseconds of the program made up. Three lines to make the start
 * explicit instead.
 */
static volatile uint8_t s_encoder_live;

void SysTick_Handler(void)
{
    s_millis++;

    /*
     * THE ADC GOES FIRST, and only because of what reads it second:
     * encoder_tick() asks this board whether the button is down, and on
     * this board that answer comes from the converter rather than from a
     * pin. One millisecond of staleness would not matter against a 20 ms
     * debounce - this is about keeping the order of the two obvious to
     * the next reader, not about the delay.
     *
     * Neither call is a wait. adc_tick() starts a conversion and
     * collects the previous one, so the cost here is a handful of
     * register accesses; see its own comment for why it can never spin.
     * Its gate lives inside it, which is why there is no flag for it
     * here.
     */
    /*
     * THE WATCHDOG IS FED FROM HERE, AND THAT IS A JUDGEMENT ABOUT THIS
     * BOARD RATHER THAN A SHORTCUT.
     *
     * A watchdog exists to recover from a hang. On this radio it does
     * not recover: the reset returns PC6 to an input, R38 pulls the
     * peripheral rail down, and the radio SWITCHES ITSELF OFF - it has
     * to be turned back on by hand, and from the bench it looks like a
     * hardware fault. That is not recovery, it is a second failure on
     * top of the first.
     *
     * So the tick feeds it. A stall in the main loop no longer kills
     * the radio; instead s_loop_ms below says how long the loop has
     * been away, which is a number somebody can read. The watchdog
     * still protects against what it can actually protect against here:
     * if SysTick itself stops, nothing feeds it and the part resets.
     */
    board_watchdog_feed();

    adc_tick();

    if (s_encoder_live) {
        encoder_tick();
        encoder2_tick();
    }
}

void board_encoder_sampling(int on)
{
    s_encoder_live = (uint8_t)(on ? 1 : 0);
}

uint32_t board_millis(void)
{
    return s_millis;
}

/*
 * HOW LONG SINCE THE MAIN LOOP LAST SAID IT WAS ALIVE.
 *
 * Now that the tick feeds the watchdog, a main loop that stops no
 * longer announces itself by switching the radio off. It has to
 * announce itself some other way, and this is it: the loop stamps its
 * own time, and anyone can ask how stale that stamp is.
 */
static volatile uint32_t s_loop_ms;

void board_loop_alive(void)
{
    s_loop_ms = s_millis;
}

uint32_t board_loop_stale_ms(void)
{
    return s_millis - s_loop_ms;
}

void board_delay_ms(uint32_t ms)
{
    uint32_t start = s_millis;

    /*
     * THE ONLY UNBOUNDED WAIT IN THIS PROJECT USED TO BE HERE, AND IT
     * WAS THE WORST PLACE FOR ONE.
     *
     * This loop spins until SysTick says enough milliseconds have gone
     * by. If SysTick is not ticking it never ends - and the first call
     * is inside board_power_on(), BEFORE console_init(), so the failure
     * produces not one byte of explanation. Worse, the radio does not
     * sit there looking broken: thirty-two seconds later the watchdog
     * that board_watchdog_tame() could only slow down resets the part,
     * the reset returns PC6 to an input, R38 pulls the peripheral rail
     * low, and the radio SWITCHES ITSELF OFF. From the bench that looks
     * like a hardware fault, and it takes the debug port with it,
     * because X1 is on that same rail.
     *
     * Two changes. The watchdog is fed from inside the loop, so a long
     * delay can never be the thing that trips it. And a cycle-count
     * backstop ends the wait even if no tick ever arrives: four times
     * longer than the delay should take, so it never fires in normal
     * use, and when it does fire the result is a delay of the wrong
     * length rather than a radio that has gone dark.
     *
     * __WFI() had to go with it. Sleeping until an interrupt arrives is
     * exactly wrong when the question is whether interrupts arrive at
     * all - the backstop below would never get to count.
     */
    uint64_t guard = (uint64_t)(ms + 1U) * (clock_sysclk_hz / 1000U);

    while ((s_millis - start) < ms) {
        board_watchdog_feed();
        if (guard-- == 0U) { break; }
    }
}

uint64_t board_cycles(void)
{
    uint32_t load = SysTick->LOAD + 1U;     /* cycles in one tick */
    uint32_t ms, ms2, val;

    /*
     * SysTick counts DOWN from LOAD to zero, so the elapsed part of the
     * current tick is (load - 1 - val). Read the millisecond count on
     * both sides of the register and retry if it moved: a tick between
     * the two reads means VAL has already wrapped and belongs to the
     * next millisecond.
     */
    do {
        ms  = s_millis;
        val = SysTick->VAL;
        ms2 = s_millis;
    } while (ms != ms2);

    return (uint64_t)ms * load + (uint64_t)(load - 1U - val);
}

/* PC6 = 3.3V_ON, the enable for everything outside the MCU. */
#define RAIL_PORT   GPIOC
#define RAIL_PIN    6U

/* PB8 = SPI_LCD_BL, despite the name an ordinary GPIO. */
#define BL_PORT     GPIOB
#define BL_PIN      8U

static void gpio_output(GPIO_TypeDef *port, uint32_t pin)
{
    /* MODER: 01 = general purpose output. */
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
    /* OTYPER: 0 = push-pull. Both nets drive a gate, nothing open-drain. */
    port->OTYPER &= ~(1UL << pin);
    /* OSPEEDR: 00 = low speed. These are DC enables; a fast edge here
     * would only put a spike on a power rail. */
    port->OSPEEDR &= ~(3UL << (pin * 2U));
    port->PUPDR   &= ~(3UL << (pin * 2U));
}

static char s_reset_cause[96];

const char *board_reset_cause(void)
{
    return s_reset_cause;
}

static void append(const char *t)
{
    uint32_t n = 0;
    while (s_reset_cause[n] != 0 && n < sizeof s_reset_cause - 1U) { n++; }
    if (n > 0U && n < sizeof s_reset_cause - 3U) {
        s_reset_cause[n++] = ',';
        s_reset_cause[n++] = ' ';
    }
    while (*t != 0 && n < sizeof s_reset_cause - 1U) { s_reset_cause[n++] = *t++; }
    s_reset_cause[n] = 0;
}

static void reset_cause_latch(void)
{
    uint32_t rsr = RCC->RSR;

    s_reset_cause[0] = 0;

    if (rsr & RCC_RSR_PORRSTF)   { append("power-on"); }
    if (rsr & RCC_RSR_BORRSTF)   { append("brown-out"); }
    if (rsr & RCC_RSR_PINRSTF)   { append("NRST pin"); }
    if (rsr & RCC_RSR_SFTRSTF)   { append("software"); }
    if (rsr & RCC_RSR_IWDG1RSTF) { append("INDEPENDENT WATCHDOG"); }
    if (rsr & RCC_RSR_WWDG1RSTF) { append("window watchdog"); }
    if (rsr & RCC_RSR_LPWRRSTF)  { append("illegal low-power"); }
    if (s_reset_cause[0] == 0)   { append("none recorded"); }

    /* Clear, so the next boot reports its own cause rather than every
     * cause since the battery was last connected. */
    RCC->RSR |= RCC_RSR_RMVF;
}

void board_watchdog_tame(void)
{
    /*
     * The IWDG cannot be turned off. It can, however, be slowed right
     * down and fed, and that is enough: 0x5555 unlocks the prescaler and
     * reload registers, /256 with a reload of 0xFFF puts the timeout at
     * roughly 32 seconds on the 32 kHz LSI, and 0xAAAA reloads the
     * counter.
     *
     * If no watchdog is running this writes to a peripheral nobody is
     * watching and nothing happens, which is the right behaviour for
     * something that has to run before we know.
     */
    IWDG1->KR  = 0x5555U;
    IWDG1->PR  = 7U;        /* /256 */
    IWDG1->RLR = 0x0FFFU;   /* maximum reload */

    /*
     * PR and RLR are not accepted immediately: the peripheral runs on
     * the LSI, so each write takes a few LSI cycles to cross over and
     * reports busy in SR until it has. Reloading the counter before
     * that finishes uses the OLD prescaler, which is the short one we
     * are trying to escape. Bounded, because this must not hang if no
     * watchdog is running and the registers never move.
     */
    for (uint32_t i = 0; i < 200000U && IWDG1->SR != 0U; i++) { }

    IWDG1->KR  = 0xAAAAU;
}

void board_watchdog_feed(void)
{
    IWDG1->KR = 0xAAAAU;
}

void board_rail_on_early(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;
    (void)RCC->AHB4ENR;

    gpio_output(RAIL_PORT, RAIL_PIN);
    RAIL_PORT->BSRR = (1UL << RAIL_PIN);

    /* Before anything else slow happens: the factory firmware's
     * watchdog is still ticking and does not care that a different
     * image is running now. */
    reset_cause_latch();
    board_watchdog_tame();

    /*
     * Wait for the LD3985 to bring the rail up and for the oscillator
     * module to start. No timebase exists yet - this runs before the PLL
     * and before SysTick - so it is a counted loop, and the count is
     * deliberately generous: at the 64 MHz the part boots on, 400 000
     * iterations is somewhere above 10 ms, against a few hundred
     * microseconds for the regulator and a millisecond or so for a
     * crystal oscillator module to settle. Nothing is in a hurry here,
     * and an oscillator that is still ramping when the PLL tries to lock
     * is a failure that looks like a hardware fault.
     */
    for (volatile uint32_t i = 0; i < 400000U; i++) { }
}

void board_power_on(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOCEN;
    (void)RCC->AHB4ENR;

    /* PC6 is already high - board_rail_on_early() raised it before the
     * clock tree was touched, because HSE depends on it. Repeating it is
     * harmless and keeps this function honest about what it leaves on. */
    gpio_output(RAIL_PORT, RAIL_PIN);
    gpio_output(BL_PORT, BL_PIN);

    /* Backlight off while the rail settles, so a half-powered panel is
     * never lit - it looks alarming and proves nothing. */
    BL_PORT->BSRR = (1UL << (BL_PIN + 16U));

    RAIL_PORT->BSRR = (1UL << RAIL_PIN);

    /*
     * SysTick last, because board_delay_ms() below needs it. 1 kHz off the
     * CPU clock, using whatever clock_init() actually achieved rather than
     * what it hoped for - if the PLL failed and we are on HSI at 64 MHz,
     * this still ticks at 1 kHz and the blink rate still means something.
     */
    SysTick->LOAD = (clock_sysclk_hz / 1000U) - 1U;
    SysTick->VAL  = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk
                  | SysTick_CTRL_TICKINT_Msk
                  | SysTick_CTRL_ENABLE_Msk;

    /*
     * The LD3985 needs a moment to bring 3.3 V up through the panel's and
     * the codec's bulk capacitance - there is 100 uF on the codec rail
     * alone. Nothing downstream should be addressed before that.
     */
    board_delay_ms(50);
}

void board_backlight(int on)
{
    BL_PORT->BSRR = on ? (1UL << BL_PIN) : (1UL << (BL_PIN + 16U));
}
