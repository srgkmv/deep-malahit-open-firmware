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

void board_rail_on_early(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;
    (void)RCC->AHB4ENR;

    gpio_output(RAIL_PORT, RAIL_PIN);
    RAIL_PORT->BSRR = (1UL << RAIL_PIN);

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
