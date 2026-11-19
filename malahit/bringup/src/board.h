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
 * THE RAIL HAS TO COME UP BEFORE THE CLOCK TREE, AND HERE IS WHY.
 *
 * X1, the 22 MHz oscillator that HSE runs on, takes its supply from the
 * SWITCHED 3.3V rail - pin 4 of the module, through ferrite bead L27,
 * from U4's output. Not from 3.3V_CPU. So until PC6 is high there is no
 * 22 MHz, HSE cannot start, PLL1 cannot lock, and clock_init() reports a
 * dead oscillator on a board whose oscillator is merely unpowered.
 *
 * This is called from SystemInit() BEFORE clock_init(), which is earlier
 * than board_power_on() can run - there is no SysTick yet, so the settle
 * time is a counted loop rather than a delay.
 *
 * It hid for a long time. Every run until a real reset existed inherited
 * a PC6 that some earlier run had already set, because GPIO state
 * survives reloading an image. The day the FT2232D gave us a working
 * reset, PC6 went back to zero, the oscillator lost power, and the
 * firmware started failing in the clock bring-up with symptoms that
 * looked like everything except a missing clock.
 */
void board_rail_on_early(void);

/*
 * Panel backlight: PB8 -> R27/R28 -> Q1 (IRLML2502) -> the panel's LED
 * anode. Plain on/off here. PB8 can do PWM later through TIM4_CH3 (AF2) or
 * TIM16_CH1 (AF1); brightness control is not a bring-up problem.
 */
void board_backlight(int on);

/*
 * WHY A FIRMWARE THAT USES NO WATCHDOG HAS TO DEAL WITH ONE.
 *
 * Loading over SWD does not reset the chip, so whatever the factory
 * firmware had running is still running when our image starts - and the
 * independent watchdog, once started, CANNOT BE STOPPED by anything
 * short of a reset. Our image never feeds it, so some seconds later it
 * resets the part. PC6 goes low with the reset, the switched rail dies,
 * and from the outside the radio simply switches itself off in the
 * middle of whatever it was doing.
 *
 * That is exactly what was seen: colour bars appeared on the panel and
 * the radio went dark moments later.
 *
 * board_watchdog_tame() stretches the timeout to its maximum - about 32
 * seconds - and feeds it once. board_watchdog_feed() keeps it fed from
 * the main loop. Both are harmless if no watchdog is running.
 */
void board_watchdog_tame(void);
void board_watchdog_feed(void);

/*
 * Why the last reset happened, decoded from RCC_RSR, as a string. Reads
 * the flags once at startup and clears them, so each boot reports its
 * own cause and not an accumulation.
 */
const char *board_reset_cause(void);

/* Milliseconds since board_power_on(). Driven by SysTick. */
uint32_t board_millis(void);

/*
 * The main loop stamps itself alive; anyone can ask how long ago.
 * SysTick feeds the watchdog on this board - see board.c for why that
 * is the right trade here - so a stalled loop shows up as this number
 * growing rather than as a radio that has switched itself off.
 */
void     board_loop_alive(void);
uint32_t board_loop_stale_ms(void);

/*
 * Start or stop sampling the rotary encoder from SysTick, which runs at
 * 1 kHz and is where upstream's encoder_tick() asks to be called.
 *
 * Off until encoder_init() has configured the pins: before that, the
 * tick samples a pin in its reset state and feeds whatever it reads into
 * the quadrature accumulator, so the first real turn of the knob arrives
 * with company. See the comment in board.c.
 */
void board_encoder_sampling(int on);
void     board_delay_ms(uint32_t ms);

/*
 * CPU cycles since board_power_on(), from the same SysTick, read as the
 * millisecond count plus the position inside the current tick.
 *
 * This exists because measuring a sample rate needs far better than
 * millisecond resolution: 50 ms of audio at 192 kHz is 9600 frames, and
 * a one-millisecond uncertainty would be a 2 % error on the answer -
 * enough to hide a wrong divider. Against the CPU clock the same
 * measurement resolves to a few parts per million.
 *
 * DWT->CYCCNT would be the obvious instrument and is deliberately not
 * used. It lives behind DEMCR in the core debug block, and this project
 * has already lost a day to firmware that depended on the debug power
 * domain being alive: one adapter left CDBGPWRUPREQ asserted after
 * OpenOCD exited and the other did not, so the same image faulted on a
 * register it had read a hundred times. SysTick is ours and is always
 * there.
 *
 * The retry loop closes the race where the tick fires between reading
 * the counter and reading the millisecond count: the two would then
 * belong to different ticks and the result would jump a millisecond
 * backwards.
 */
uint64_t board_cycles(void);

#endif /* MALAHIT_BOARD_H */
