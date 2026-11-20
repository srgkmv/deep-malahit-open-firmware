#ifndef MALAHIT_BOOT_H
#define MALAHIT_BOOT_H

/*
 * THE WAY BACK: ROM DFU BY THE ENCODER BUTTONS, AT POWER-ON.
 *
 * Called from Reset_Handler as the first C code after a reset - before
 * .data is copied, before .bss is zeroed, before the rail or a single
 * clock is touched - so that nothing this firmware does later can stand
 * between a held button and the ROM bootloader. It uses no global
 * variable and no library call, which is what makes running that early
 * legal.
 *
 * Hold BOTH encoder buttons and switch on: the radio enumerates as
 * 0483:df11 and dfu-util can rewrite the flash, this firmware included.
 * The small knob's button alone probably does it too - the same
 * electrical fact the factory firmware lives with, see boot.c.
 *
 * Returns if the buttons are not held.
 */
void boot_dfu_check(void);

/*
 * THE POWER BUTTON - SW3 on PC13, which is also wake-up pin WKUP4.
 *
 * The MCU is never unpowered (BOARD.md section 3): "off" is standby with
 * the peripheral rail down, and SW3 wakes it, which is a reset. So two
 * decisions belong to this firmware once it is in flash:
 *
 *   boot_power_check()  at every reset, FIRST - before boot_dfu_check(),
 *                       which waits ~20 ms for the rail:
 *                       if SW3 is not held, this was not somebody
 *                       switching the radio on (a battery going in,
 *                       DFU's "leave", a fault) - go straight back to
 *                       standby, before the rail or a clock is touched.
 *                       Only for an image running FROM FLASH: a RAM
 *                       image loaded over SWD returns at once, so the
 *                       bench workflow is unchanged.
 *
 *   boot_power_tick()   from SysTick: SW3 held for more than
 *                       BOOT_POWER_OFF_MS switches off - the backlight
 *                       and the speaker go at once, so the hand knows,
 *                       and standby follows on RELEASE.
 *
 * boot_power_init() configures the pin and must run before the tick does
 * anything; boot_power_key() is the debounced-nothing raw state, for the
 * console.
 */
#define BOOT_POWER_OFF_MS   3000U

void boot_power_check(void);
void boot_power_init(void);
void boot_power_tick(void);
int  boot_power_key(void);

/* Standby, the factory firmware's recipe (boot.c says where each step
 * comes from). Never returns: waking is a reset. */
void boot_standby(void) __attribute__((noreturn));

/*
 * The power button for a firmware that has stopped: polled, with no
 * SysTick and interrupts masked. fault_park() ends here. Held for
 * BOOT_POWER_OFF_MS it switches off exactly as boot_power_tick() does;
 * until then it only waits. Never returns.
 */
void boot_power_park(void) __attribute__((noreturn));

#endif /* MALAHIT_BOOT_H */
