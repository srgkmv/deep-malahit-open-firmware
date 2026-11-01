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
#include "i2c3.h"

/*
 * HOW FAR DID IT GET.
 *
 * A single number, bumped at each stage of startup, living at a fixed
 * place in memory so it can be read over SWD with one command:
 *
 *     swd.sh -c init -c halt -c "mdw <addr> 1"
 *
 * This exists because the console is not trustworthy enough to be the
 * only witness. Several runs have produced zero bytes on the UART
 * while the firmware was demonstrably executing, and when the evidence
 * and the machine disagree the machine wins. A progress marker cannot
 * be lost to a baud rate, a termios setting, a capture that started
 * late, or a radio that switched off between the output and the eye.
 *
 * Deliberately NOT in .bss: a value of zero has to mean "stopped
 * before the first marker", and .bss is full of zeros already. It
 * starts at 0xFFFFFFFF, which no stage ever writes.
 */
uint32_t g_phase = 0xFFFFFFFFU;
#define PHASE(n)  do { g_phase = (n); __DSB(); } while (0)

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
    PHASE(1);
    board_power_on();
    PHASE(2);

    console_init();
    PHASE(3);

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
    PHASE(4);

    report();
    PHASE(5);

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
    PHASE(6);
    spi3_init(SPI3_DIV16);
    PHASE(7);
    uint32_t cfg2 = spi3_check();
    console_printf("panel     : SPI3 at %u Hz, CFG2 0x%08x%s\n",
                   spi3_hz(), cfg2,
                   (cfg2 & (1UL << 22)) ? "" :
                   "  *** MASTER BIT CLEARED - mode fault, SPI will not clock ***");
    ili9488_init(ILI9488_FMT_18BIT);
    PHASE(8);
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
    /*
     * THREE PHASES, BECAUSE TWO WERE AMBIGUOUS.
     *
     * Red in 18-bit and green in 16-bit gave a screen that stayed red
     * throughout. That is informative but not yet conclusive: a screen
     * that never changes could mean the 16-bit write did nothing, or it
     * could mean the panel stopped accepting anything at all after the
     * first COLMOD change.
     *
     * Adding a third phase - blue, back in 18-bit - separates the two.
     *
     *   red, blue, red, blue ... and never green
     *       -> 18-bit works, 16-bit is ignored. The panel discards
     *          pixel data in a mode it does not support rather than
     *          drawing something wrong, which is why nothing appeared.
     *
     *   red, then nothing ever again
     *       -> the first COLMOD change wedged the panel, and the
     *          question is still open.
     *
     * The bus is at 30 MHz for this run as well, double the previous
     * one. If the colours are still clean at twice the clock, the frame
     * time halves to about 122 ms and that is the number the interface
     * gets designed around.
     */
    spi3_set_div(SPI3_DIV16);

    /*
     * ASK THE PANEL, DO NOT ASK A PERSON LOOKING AT IT.
     *
     * The datasheet settles what SHOULD be possible: with IM[2:0]
     * strapped to 111 - and R23/R24/R25 pull all three to 3.3V on this
     * board - the 4-line serial interface supports 3, 16 and 18 bits
     * per pixel, DBI[2:0] = 001, 101 and 110. 16-bit is legal here, the
     * folklore notwithstanding.
     *
     * But a green screen never appeared when 16-bit was selected, and
     * "I only saw red" cannot distinguish between the panel ignoring
     * the data, the panel ignoring the format change, or the two of us
     * disagreeing about which six-second window was which.
     *
     * RDDCOLMOD (0x0C) returns the format the panel believes it is in.
     * Write a format, read it back, and the panel answers for itself:
     * 0x06 is 18 bits per pixel, 0x05 is 16. RDDID (0x04) goes first,
     * because a read path that cannot fetch a known-constant device ID
     * has no business being trusted about anything else.
     */
    spi3_set_div(SPI3_DIV16);

    console_printf("\nAsking the panel directly\n");
    console_printf("-------------------------\n");
    console_printf("  raw bytes as they arrive, dummy included.\n");
    console_printf("  0xD3 must contain 94 88 somewhere - that is the\n");
    console_printf("  known answer, and where it lands gives the offset.\n\n");

    static const struct { uint8_t cmd; const char *name; } regs[] = {
        { 0xD3U, "RDDID4   (expect .. 00 94 88)" },
        { 0x04U, "RDDID                        " },
        { 0x09U, "RDDST    display status      " },
        { 0x0CU, "RDDCOLMOD                    " },
    };

    for (uint32_t r = 0; r < 4U; r++) {
        uint8_t raw[6];
        ili9488_read_raw(regs[r].cmd, raw, 6U);
        console_printf("  %s : %02x %02x %02x %02x %02x %02x\n",
                       regs[r].name,
                       raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    }

    console_printf("\n  now the same register either side of a format change:\n");
    for (uint32_t k = 0; k < 2U; k++) {
        uint8_t raw[6];
        ili9488_set_format((k == 0U) ? ILI9488_FMT_18BIT : ILI9488_FMT_16BIT);
        ili9488_read_raw(0x0CU, raw, 6U);
        console_printf("  COLMOD written 0x%02x -> 0x0C : %02x %02x %02x %02x %02x %02x\n",
                       (k == 0U) ? 0x66U : 0x55U,
                       raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    }
    console_printf("\n");

    /*
     * STEP 3: FIND OUT WHAT THE TOUCH PANEL IS.
     *
     * The schematic names four nets - I2C3_TOUCH_SCL, _SDA, _IRQ,
     * _RST - and never says which controller is behind them. So ask
     * the bus. A scan of the 7-bit address space takes a moment and
     * narrows it to one or two candidates immediately, because the
     * usual suspects live at known addresses:
     *
     *   0x38          FT6206 / FT6236 / FT6336, by far the most common
     *   0x5D or 0x14  GT911
     *   0x15          CST816 and relatives
     *   0x48          NS2009, a resistive controller with an I2C face
     *
     * Then read whatever identifies it, and dump the first registers
     * raw either way - a name is nice but the register contents are
     * what the driver gets written against.
     */
    PHASE(9);
    i2c3_init();
    PHASE(10);
    console_printf("\nTouch panel on I2C3 (PA8 SCL, PC9 SDA)\n");
    console_printf("-------------------------------------\n");

    i2c3_touch_reset();
    PHASE(11);
    console_printf("  reset pulsed on PC7; IRQ (PC8) reads %s\n",
                   i2c3_touch_irq_asserted() ? "LOW - asserted"
                                             : "high - idle");

    PHASE(12);
    uint32_t lines = i2c3_line_levels();
    PHASE(13);
    console_printf("  bus idle levels: SCL=%u SDA=%u  %s\n",
                   (lines >> 1) & 1U, lines & 1U,
                   (lines == 3U) ? "(both high, as they should be)"
                                 : "*** A LINE IS LOW - no pull-ups, a "
                                   "stuck device, or the wrong pins ***");

    console_printf("  scanning 0x08..0x77 ...\n");

    uint8_t found[8];
    uint32_t nfound = 0;

    PHASE(14);
    for (uint8_t a = 0x08U; a <= 0x77U; a++) {
        g_phase = 0x1400U | a;
        board_watchdog_feed();
        if (i2c3_probe(a)) {
            console_printf("    device at 0x%02x\n", a);
            if (nfound < 8U) { found[nfound++] = a; }
        }
    }

    PHASE(15);
    if (nfound == 0U) {
        console_printf("    NOTHING ANSWERED.\n"
                       "    Either the bus is not pulled up, or the\n"
                       "    controller is still held in reset, or SCL and\n"
                       "    SDA are not where the schematic says.\n");
    }

    /* Dump the low registers of whatever answered. Done with a repeated
     * start - write the register index, read back - which is how all of
     * these parts are addressed. */
    PHASE(16);
    for (uint32_t d = 0; d < nfound; d++) {
        uint8_t reg0 = 0x00U;
        uint8_t buf[16];

        board_watchdog_feed();
        if (i2c3_write_read(found[d], &reg0, 1U, buf, 16U)) {
            console_printf("  0x%02x regs 0x00..0x0f:", found[d]);
            for (uint32_t i = 0; i < 16U; i++) {
                console_printf(" %02x", buf[i]);
            }
            console_printf("\n");
        } else {
            console_printf("  0x%02x did not answer a register read\n",
                           found[d]);
        }

        /* FT6x36 keeps its identity high up: 0xA3 is the chip code
         * (0x36, 0x64, 0x11 and friends) and 0xA8 the vendor, 0x11 for
         * FocalTech. Harmless to ask of anything else. */
        uint8_t rega = 0xA3U;
        uint8_t id[2];
        if (i2c3_write_read(found[d], &rega, 1U, id, 1U)) {
            uint8_t regv = 0xA8U;
            uint8_t vend = 0;
            (void)i2c3_write_read(found[d], &regv, 1U, &vend, 1U);
            console_printf("  0x%02x chip id 0x%02x, vendor 0x%02x%s\n",
                           found[d], id[0], vend,
                           (vend == 0x11U) ? "  <- FocalTech FT6x36" : "");
        }

        /* GT911 answers at 0x8140 with four ASCII bytes naming itself. */
        uint8_t gt[2] = { 0x81U, 0x40U };
        uint8_t pid[4];
        if (i2c3_write_read(found[d], gt, 2U, pid, 4U)) {
            console_printf("  0x%02x id-at-0x8140 = '%c%c%c%c'\n",
                           found[d],
                           (pid[0] >= 32 && pid[0] < 127) ? pid[0] : '.',
                           (pid[1] >= 32 && pid[1] < 127) ? pid[1] : '.',
                           (pid[2] >= 32 && pid[2] < 127) ? pid[2] : '.',
                           (pid[3] >= 32 && pid[3] < 127) ? pid[3] : '.');
        }
    }

    /*
     * FT6236 / FT6236U, per the user. Register map, which is shared by
     * the whole FT6x06/FT6x36 family:
     *
     *   0x00 DEV_MODE      0 = normal reporting
     *   0x01 GEST_ID       gesture, 0 when the firmware has none
     *   0x02 TD_STATUS     [3:0] = number of points currently down
     *   0x03 P1_XH         [7:6] event, [3:0] X high nibble
     *   0x04 P1_XL                        X low byte
     *   0x05 P1_YH         [7:4] touch id, [3:0] Y high nibble
     *   0x06 P1_YL                        Y low byte
     *   0x07 P1_WEIGHT     pressure, often unimplemented
     *   0x08 P1_MISC       [7:4] contact area
     *   0x09..0x0E         the same six bytes for the second finger
     *
     * The event field is 00 press down, 01 lift up, 10 contact, 11 none.
     *
     * COORDINATES ARE IN THE PANEL'S OWN ORIENTATION - 320 across by 480
     * down, portrait - while the display is driven landscape through
     * MADCTL 0x28. So they need swapping, and probably mirroring in one
     * axis. Rather than reason about which, the code draws a white
     * square where it thinks the finger is: if the square lands under
     * the finger the mapping is right, and if it lands somewhere else
     * its position says exactly which transform is missing.
     */
    #define FT_ADDR  0x38U

    console_printf("\n  Touch the screen. A white square should appear\n");
    console_printf("  under your finger. Where it actually appears tells\n");
    console_printf("  us the orientation mapping.\n\n");

    /*
     * G_MODE = 0x00, interrupt POLLING mode.
     *
     * The datasheet's default is 0x01, interrupt TRIGGER mode, where
     * the INT pin gives a pulse per report rather than sitting low for
     * as long as a finger is down. A loop that samples the pin level
     * misses pulses between samples and concludes the panel is dead.
     * In polling mode the line stays low while there is a contact,
     * which is what level-sampling wants.
     *
     * The loop below does not depend on it either way: it reads
     * TD_STATUS on a timer and believes the register, not the wire.
     * The IRQ state is printed alongside so the two can be compared,
     * because an interrupt-driven driver is what this eventually
     * becomes and it is worth knowing now whether the line behaves.
     */
    {
        PHASE(17);
        uint8_t gmode[2] = { 0xA4U, 0x00U };
        console_printf("  G_MODE <- 0x00 (level, not pulse): %s\n",
                       i2c3_write(FT_ADDR, gmode, 2U) ? "ok" : "FAILED");
    }

    ili9488_set_format(ILI9488_FMT_18BIT);
    ili9488_fill_screen(0U, 0U, 0U);

    PHASE(18);
    uint32_t last_beat = 0;
    uint32_t touches = 0;

    for (;;) {
        PHASE(19);
        board_watchdog_feed();

        {
            uint8_t reg0 = 0x00U;
            uint8_t b[9];

            if (i2c3_write_read(FT_ADDR, &reg0, 1U, b, 9U)
                && (b[0x02] & 0x0FU) != 0U) {
                uint32_t npoints = b[0x02] & 0x0FU;
                uint32_t event   = (uint32_t)(b[0x03] >> 6);
                uint32_t tx      = (((uint32_t)b[0x03] & 0x0FU) << 8) | b[0x04];
                uint32_t ty      = (((uint32_t)b[0x05] & 0x0FU) << 8) | b[0x06];
                uint32_t weight  = b[0x07];
                uint32_t area    = (uint32_t)(b[0x08] >> 4);

                /*
                 * First guess at the transform: swap the axes, because
                 * the panel is portrait and the display is landscape,
                 * and mirror Y so that the origin ends up in the same
                 * corner. Both halves are guesses and the square on
                 * screen is the test.
                 */
                uint32_t sx = ty;
                uint32_t sy = (tx < ILI9488_H) ? (ILI9488_H - 1U - tx) : 0U;

                if (sx > ILI9488_W - 1U) { sx = ILI9488_W - 1U; }
                if (sy > ILI9488_H - 1U) { sy = ILI9488_H - 1U; }

                console_printf("  n=%u ev=%u raw(%u,%u) -> screen(%u,%u) "
                               "w=%u a=%u\n",
                               npoints, event, tx, ty, sx, sy, weight, area);

                if (npoints > 0U) {
                    uint16_t x = (uint16_t)((sx > 10U) ? (sx - 10U) : 0U);
                    uint16_t y = (uint16_t)((sy > 10U) ? (sy - 10U) : 0U);
                    if (x > ILI9488_W - 20U) { x = ILI9488_W - 20U; }
                    if (y > ILI9488_H - 20U) { y = ILI9488_H - 20U; }
                    ili9488_fill_rect(x, y, 20U, 20U, 255U, 255U, 255U);
                    touches++;
                }
            }
            board_delay_ms(30);
        }

        if ((int32_t)(board_millis() - last_beat) > 3000) {
            last_beat = board_millis();
            console_printf("  [%u s] %u touches so far, IRQ %s\n",
                           board_millis() / 1000U, touches,
                           i2c3_touch_irq_asserted() ? "LOW" : "high");
        }
    }
}
