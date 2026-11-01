#include "stm32h7xx.h"
#include "board.h"
#include "clock.h"

static volatile uint32_t s_millis;

void SysTick_Handler(void)
{
    s_millis++;
}

uint32_t board_millis(void)
{
    return s_millis;
}

void board_delay_ms(uint32_t ms)
{
    uint32_t start = s_millis;
    while ((s_millis - start) < ms) {
        __WFI();
    }
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
