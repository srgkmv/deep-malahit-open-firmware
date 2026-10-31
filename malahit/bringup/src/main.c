/*
 * Step 1 of the Malahit DSP-1 port: prove the board is ours.
 *
 * This image answers four questions, and nothing else:
 *
 *   1. Does HSE start in bypass mode off the 22 MHz module, and does PLL1
 *      lock on dividers nobody has ever used on this chip before?
 *   2. Does PC6 really switch the rail that feeds everything outside the
 *      MCU? (Confirmed by hand over SWD before this code existed - the
 *      screen lit. This is the same thing done properly, in software.)
 *   3. Is UART7 on PE7/PE8 a usable console?
 *   4. Is the timebase right?
 *
 * Question 4 is the one worth explaining. There is no user LED on this
 * board - HL1 belongs to the TP4056 charger, not to the processor - so the
 * only thing the MCU can blink is the panel backlight. That turns out to
 * be better than an LED: the blink is driven by SysTick, SysTick is loaded
 * from the frequency clock_init() actually achieved, so a backlight that
 * blinks at a visibly correct one second per cycle is direct evidence that
 * the whole clock tree is what it claims to be. If the PLL silently came
 * up at the wrong multiple, the blink is visibly wrong and you know it
 * from across the room.
 *
 * Runs from RAM. It does not erase, write or otherwise touch flash, so the
 * factory firmware is still there and a power cycle brings it back:
 *
 *     ../tools/swd.sh ram build/bringup.elf
 */

#include "stm32h7xx.h"

#include "board.h"
#include "clock.h"
#include "console.h"
#include "fault.h"

static const char *clock_error_text(int status)
{
    switch (status) {
    case CLOCK_OK:         return "ok";
    case CLOCK_ERR_VOSRDY: return "core voltage never became ready (VOSRDY)";
    case CLOCK_ERR_HSERDY: return "HSE never started - is X1 (22 MHz) running?";
    case CLOCK_ERR_PLLRDY: return "PLL1 never locked";
    case CLOCK_ERR_SWITCH: return "SYSCLK never switched to PLL1";
    default:               return "unknown";
    }
}

static void report(void)
{
    uint32_t idcode = DBGMCU->IDCODE;
    uint32_t rev    = idcode >> 16;

    console_printf("\n\n");
    console_printf("Malahit DSP-1 - bring-up step 1\n");
    console_printf("================================\n");

    console_printf("chip      : dev 0x%x  rev 0x%x (%s)\n",
                   idcode & 0xFFFUL, rev,
                   (rev == 0x2003U) ? "V, 480 MHz capable"
                 : (rev == 0x1003U) ? "Y, 400 MHz max"
                                    : "unrecognised");
    console_printf("flash     : %u kB\n",
                   (uint32_t)(*(volatile uint16_t *)0x1FF1E880));
    console_printf("unique id : %08x %08x %08x\n",
                   *(volatile uint32_t *)0x1FF1E800,
                   *(volatile uint32_t *)0x1FF1E804,
                   *(volatile uint32_t *)0x1FF1E808);

    console_printf("clock     : %s\n", clock_error_text(clock_status));
    console_printf("  HSE     : %u Hz, bypass (X1 module on PH0)\n", HSE_HZ);
    console_printf("  SYSCLK  : %u Hz\n", clock_sysclk_hz);
    console_printf("  AHB/AXI : %u Hz\n", clock_ahb_hz);
    console_printf("  APB1    : %u Hz  (UART7 kernel clock)\n", clock_apb1_hz);

    if (clock_status != CLOCK_OK) {
        console_printf("\n"
            "  Running on HSI at 64 MHz. Everything below still works,\n"
            "  just slower - and the backlight blink will be at the right\n"
            "  rate anyway, because SysTick was loaded from the real\n"
            "  frequency and not from the hoped-for one.\n");
    }

    console_printf("\nrails     : PC6 high -> 3.3V on (display, touch, "
                   "MSi001, NAU8822)\n");
    console_printf("backlight : PB8, blinking at 1 Hz\n");
    console_printf("\nIf the backlight is blinking once a second, step 1 "
                   "is done.\n\n");
}

int main(void)
{
    /*
     * Power before anything else. Until PC6 is high, the panel, the touch
     * controller, the tuner and the codec are all unpowered and none of
     * them will answer - which from the debugger looks exactly like a
     * driver bug. See board.h.
     */
    board_power_on();

    console_init();

    /*
     * Before anything else this run has to say: did the last run die?
     *
     * The record lives at a fixed address in SRAM that survives a warm
     * reset and a reload, so a crash reports itself here, in readable
     * text, instead of having to be reconstructed from register dumps
     * after the fact. That is not a luxury - an evening went into a crash
     * that turned out to be an artefact of the loader rather than of the
     * firmware, and this is the thing that would have said so in one line.
     */
    fault_report_previous();

    report();

    uint32_t next = board_millis();
    int      on   = 0;
    uint32_t beat = 0;

    for (;;) {
        board_backlight(on);
        int shown = on;        /* before the flip, or the line always lies */
        on = !on;

        /*
         * A heartbeat line every second, forever.
         *
         * The banner above prints once, at boot, which is useless for the
         * job this console actually has to do first: finding out which
         * physical pin PE8 comes out on. J6 is a bare 2x7 header with no
         * silkscreen numbering and no ground pin, so locating UART7 means
         * moving a probe around while something talks. A one-shot banner
         * means re-running the whole load for every guess.
         *
         * Printing twice a second - this loop runs at 2 Hz - also makes
         * the far more common failure obvious. Wrong baud rate gives
         * steady garbage at a steady rate rather than silence, and the
         * two look nothing alike.
         */
        if ((beat++ & 1U) == 0U) {
            console_printf("[%u s] alive, sysclk %u Hz, backlight %s\n",
                           board_millis() / 1000U, clock_sysclk_hz,
                           shown ? "on" : "off");
        }

        /*
         * Absolute deadlines rather than delay-from-now, so the period
         * stays 1000 ms whatever else creeps into the loop later. At this
         * size it makes no difference; it is a habit worth keeping before
         * the loop grows a panel driver.
         */
        next += 500U;
        while ((int32_t)(board_millis() - next) < 0) {
            __WFI();
        }
    }
}
