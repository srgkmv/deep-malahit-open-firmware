#include "stm32h7xx.h"
#include "backlight.h"     /* ../../User - upstream's API, this board's timer */

/*
 * THE BACKLIGHT'S BRIGHTNESS - 22/11/2026.
 *
 * Upstream's API (User/backlight.h: 0..100 %, a floor under it, 20 kHz)
 * on this board's pin: PB8 -> R27/R28 -> Q1 (IRLML2502, N-channel) -> the
 * panel's LED anode, so ACTIVE HIGH - the opposite of the DEEPSDR's, whose
 * driver lights on a low pin (upstream's polarity note). TIM16_CH1 on AF1
 * (BOARD.md, the pin table: "PWM capable: TIM4_CH3 (AF2) or TIM16_CH1
 * (AF1)"); nothing else in this image uses TIM16.
 *
 * 20 kHz for upstream's reasons: far above the ~200 Hz where an LED
 * flickers, and not in the audio. TIM16 runs on APB2's timer clock,
 * 240 MHz here (clock.c: APB2 120 MHz, prescaled, so timers x2), so the
 * period is 12 000 counts and a percent is 120 of them.
 *
 * AT 100 % THE PIN NEVER SWITCHES - CCR above ARR holds PWM mode 1 high -
 * which is what this board had before brightness existed: whatever a
 * square wave on the panel's LED current does to the receiver, the
 * default does none of it.
 *
 * Every register this depends on is written, and TIM16 is reset first:
 * an image loaded over SWD inherits whatever the last one left
 * (WORKING.md, "LOADING OVER SWD RESETS NOTHING").
 */
#define BL_PORT        GPIOB
#define BL_PIN         8U
#define BL_ARR         11999U          /* 240 MHz / 20 kHz - 1 */
#define BL_MIN_PCT     10U             /* upstream's BACKLIGHT_MIN_PERCENT */

static uint8_t s_pct = 100U;
static uint8_t s_pwm;                  /* the timer drives the pin */
static uint8_t s_dark;                 /* backlight_sleep(): pin low */

static uint32_t ccr_de(uint8_t pct)
{
    return ((BL_ARR + 1U) * (uint32_t)pct) / 100U;    /* 100 % = ARR + 1: never low */
}

static void pin_timer(void)
{
    BL_PORT->AFR[1] = (BL_PORT->AFR[1] & ~(0xFUL << ((BL_PIN - 8U) * 4U)))
                    | (1UL << ((BL_PIN - 8U) * 4U));          /* AF1: TIM16_CH1 */
    BL_PORT->MODER  = (BL_PORT->MODER & ~(3UL << (BL_PIN * 2U)))
                    | (2UL << (BL_PIN * 2U));
}

static void pin_low(void)
{
    BL_PORT->BSRR  = 1UL << (BL_PIN + 16U);
    BL_PORT->MODER = (BL_PORT->MODER & ~(3UL << (BL_PIN * 2U)))
                   | (1UL << (BL_PIN * 2U));
}

void backlight_init(void)
{
    RCC->APB2RSTR |= RCC_APB2RSTR_TIM16RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_TIM16RST;
    RCC->APB2ENR  |= RCC_APB2ENR_TIM16EN;
    (void)RCC->APB2ENR;

    TIM16->CR1   = 0U;
    TIM16->PSC   = 0U;
    TIM16->ARR   = BL_ARR;
    TIM16->CCR1  = ccr_de(s_pct);
    TIM16->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1      /* PWM mode 1 */
                 | TIM_CCMR1_OC1PE;                         /* CCR1 at the update */
    TIM16->CCER  = TIM_CCER_CC1E;                           /* active high */
    TIM16->BDTR  = TIM_BDTR_MOE;       /* TIM16 has a break stage: no MOE, no output */
    TIM16->EGR   = TIM_EGR_UG;
    TIM16->CR1   = TIM_CR1_ARPE | TIM_CR1_CEN;

    s_pwm  = 1U;
    s_dark = 0U;
    pin_timer();
}

void backlight_set_percent(uint8_t percent)
{
    if (percent > 100U)       { percent = 100U; }
    if (percent < BL_MIN_PCT) { percent = BL_MIN_PCT; }
    s_pct = percent;
    if (s_pwm) { TIM16->CCR1 = ccr_de(percent); }
}

uint8_t backlight_get_percent(void) { return s_pct; }

void backlight_sleep(void)
{
    s_dark = 1U;
    pin_low();
}

void backlight_wake(void)
{
    s_dark = 0U;
    if (s_pwm) { pin_timer(); }
    else       { BL_PORT->BSRR = 1UL << BL_PIN; }    /* before init: plain on */
}
