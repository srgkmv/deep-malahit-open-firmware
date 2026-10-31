#ifndef MALAHIT_BOARD_H
#define MALAHIT_BOARD_H

#include <stdint.h>

/*
 * The Malahit DSP-1 has TWO 3.3 V rails and only one of them comes up by
 * itself:
 *
 *   U3 -> 3.3V_CPU   INH tied to its own input. Always on. Feeds the
 *                    STM32H743 and nothing else.
 *   U4 -> 3.3V       INH driven by PC6, held low by R38 (1 kOhm). Feeds the
 *                    DISPLAY, the TOUCH PANEL, the MSi001 and the NAU8822.
 *
 * So after reset the processor is awake and every single thing it wants to
 * talk to is unpowered. No bus answers, no panel lights, and none of it
 * looks like a power problem from the debugger - it looks like a broken
 * driver.
 *
 * board_power_on() is therefore the first call in main(), before any other
 * peripheral is touched.
 *
 * (Jumper JP3 shorts BAT onto the same net and would make the rail
 * permanent. Don't fit it: the radio's "off" state is the MCU in standby
 * with PC6 low, so a fitted JP3 keeps the panel and the tuner powered
 * around the clock and flattens the battery. Bench work is easier on USB.)
 */
void board_power_on(void);

/*
 * Panel backlight: PB8 -> R27/R28 -> Q1 (IRLML2502) -> the panel's LED
 * anode. Plain on/off here. PB8 can do PWM later through TIM4_CH3 (AF2) or
 * TIM16_CH1 (AF1); brightness control is not a bring-up problem.
 */
void board_backlight(int on);

/* Milliseconds since board_power_on(). Driven by SysTick. */
uint32_t board_millis(void);
void     board_delay_ms(uint32_t ms);

#endif /* MALAHIT_BOARD_H */
