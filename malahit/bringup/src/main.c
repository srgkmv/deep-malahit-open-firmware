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
#include "spi3.h"
#include "ili9488.h"

static const char *clock_error_text(int status)
{
    switch (status) {
    case CLOCK_OK:         return "ok";
    case CLOCK_ERR_VOSRDY: return "core voltage never became ready (VOSRDY)";
    case CLOCK_ERR_HSERDY: return "HSE never started - X1 unpowered? it is on the SWITCHED 3.3V rail (PC6)";
    case CLOCK_ERR_PLLRDY: return "PLL1 never locked";
    case CLOCK_ERR_SWITCH: return "SYSCLK never switched to PLL1";
    default:               return "unknown";
    }
}

/*
 * NOTHING HERE READS DBGMCU, AND THAT IS DELIBERATE.
 *
 * This used to print the device and revision ID out of DBGMCU->IDCODE at
 * 0x5C001000, which worked perfectly - right up until the debug adapter
 * changed. DBGMCU lives in the debug power domain, and that domain is
 * only guaranteed to be powered while a debugger is holding
 * CDBGPWRUPREQ. One adapter left it asserted after OpenOCD exited and
 * the other did not, so the same firmware that had printed its chip ID
 * a hundred times took a precise bus error (CFSR 0x00008200) on the very
 * first line of its banner, before the console had said anything at all.
 *
 * Firmware has no business depending on whether someone has a debugger
 * plugged in. The chip ID is a debugger's question; "swd.sh id" asks it.
 * What stays here is what the CPU can always read: flash size and the
 * unique ID, both in system memory, and the clock tree, which is ours.
 */
static void report(void)
{

    console_printf("\n\n");
    console_printf("Malahit DSP-1 - bring-up step 1\n");
    console_printf("================================\n");

    console_printf("flash     : %u kB\n",
                   (uint32_t)(*(volatile uint16_t *)0x1FF1E880));
    console_printf("unique id : %08x %08x %08x\n",
                   *(volatile uint32_t *)0x1FF1E800,
                   *(volatile uint32_t *)0x1FF1E804,
                   *(volatile uint32_t *)0x1FF1E808);

    console_printf("last reset: %s\n", board_reset_cause());
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

    /*
     * STEP 2: THE PANEL, AND THE QUESTION IT HAS TO ANSWER.
     *
     * The ILI9488 is widely said to accept only 18-bit pixels over SPI -
     * three bytes each - even though it does RGB565 on its parallel bus.
     * If that holds here, a full frame is 460 800 bytes instead of
     * 307 200, and that single fact sets the budget for the whole user
     * interface: how fast a waterfall can scroll, whether the spectrum
     * can be redrawn every frame, what a framebuffer costs in AXI SRAM.
     *
     * Rather than believe the internet, ask the panel. The loop below
     * draws the same eight colour bars in both formats, four seconds
     * each, and prints how long the full-screen fill took in each case.
     * Whichever picture looks right is the format this panel supports,
     * and the measured microseconds are the real number to design
     * against - not a calculation from the clock rate, which ignores the
     * per-byte overhead that turns out to dominate.
     */
    spi3_init(SPI3_DIV16);
    uint32_t cfg2 = spi3_check();
    console_printf("panel     : SPI3 at %u Hz, CFG2 0x%08x%s\n",
                   spi3_hz(), cfg2,
                   (cfg2 & (1UL << 22)) ? "" :
                   "  *** MASTER BIT CLEARED - mode fault, SPI will not clock ***");
    ili9488_init(ILI9488_FMT_18BIT);
    board_backlight(1);
    console_printf("panel     : initialised, backlight on\n\n");

    console_printf("Watch the screen. Eight bars, left to right:\n");
    console_printf("  black red green blue yellow cyan magenta white,\n");
    console_printf("  inside a one-pixel white frame that must close.\n\n");

    /*
     * THE SIMPLEST QUESTION THAT SETTLES IT.
     *
     * Colour bars turned out to be a poor discriminator: they take a
     * second to paint at this bus speed, and watching eight rectangles
     * appear one by one tells you nothing about which of two formats
     * just drew them. The eye has nothing to compare against.
     *
     * So: solid screens, one colour each, alternating. The format is
     * wrong if EITHER goes wrong, and both failure modes are visible
     * from across the room:
     *
     *   - wrong colour. Send RGB565 to a panel expecting three bytes
     *     and the bytes regroup into different pixels entirely; red
     *     comes out as something else, usually a muddy pattern.
     *   - short fill. Two bytes per pixel into a panel that wants three
     *     covers only two thirds of the window, so the bottom third of
     *     the screen keeps whatever was there before.
     *
     * A clean full-screen red followed by a clean full-screen green,
     * both reaching all four edges, means this panel takes both
     * formats - and then we use 16-bit, because it is a third less
     * data for the same picture.
     */
    static const struct {
        ili9488_fmt_t fmt;
        const char   *fmt_name;
        uint8_t       r, g, b;
        const char   *colour;
    } phases[] = {
        { ILI9488_FMT_18BIT, "18-bit (3 bytes/px)", 255,   0,   0, "RED"   },
        { ILI9488_FMT_16BIT, "16-bit (2 bytes/px)",   0, 255,   0, "GREEN" },
    };

    console_printf("\nTwo solid screens, six seconds each, round and round:\n");
    console_printf("  18-bit -> RED      16-bit -> GREEN\n");
    console_printf("Both clean and edge to edge = this panel takes both,\n");
    console_printf("and 16-bit wins on size. Wrong colour or a band at the\n");
    console_printf("bottom that never fills = that format is not supported.\n\n");

    uint32_t round = 0;

    for (;;) {
        uint32_t i = round % 2U;

        ili9488_set_format(phases[i].fmt);
        ili9488_fill_screen(phases[i].r, phases[i].g, phases[i].b);

        console_printf("NOW SHOWING: %s  in %s   (%u ms to paint)\n",
                       phases[i].colour, phases[i].fmt_name,
                       ili9488_last_fill_us() / 1000U);

        uint32_t until = board_millis() + 6000U;
        while ((int32_t)(board_millis() - until) < 0) {
            board_watchdog_feed();
            __WFI();
        }
        round++;
    }
}
