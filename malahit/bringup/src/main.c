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

    /*
     * FOUR-POINT CALIBRATION.
     *
     * The swap-and-mirror transform lands the cursor under the finger
     * but consistently a little to the right, and a uniform error like
     * that is the touch glass sitting slightly off the matrix - a
     * physical offset, not a mistake in the arithmetic. No amount of
     * thinking about axes will remove it; it has to be measured.
     *
     * So: draw a target, wait for a finger, record what the controller
     * said, four times, at the corners of an inset rectangle. Two
     * targets share each edge, which gives two independent readings per
     * axis and lets the fit average them instead of trusting one poke.
     *
     * Inset by 40 px rather than placed in the actual corners. Corners
     * are the worst place to calibrate: capacitive panels are least
     * linear at their edges, a finger cannot be centred on a point it
     * is half off, and any error there is multiplied when extrapolated
     * back across the screen.
     *
     * The result is printed as constants to paste into the driver, and
     * then applied immediately so it can be checked by poking at it.
     */
    #define CAL_INSET_X   40
    #define CAL_INSET_Y   40

    static const struct { uint16_t x, y; const char *where; } targets[] = {
        { CAL_INSET_X,                  CAL_INSET_Y,                  "top left"     },
        { ILI9488_W - CAL_INSET_X,      CAL_INSET_Y,                  "top right"    },
        { ILI9488_W - CAL_INSET_X,      ILI9488_H - CAL_INSET_Y,      "bottom right" },
        { CAL_INSET_X,                  ILI9488_H - CAL_INSET_Y,      "bottom left"  },
    };

    uint32_t raw_x[4], raw_y[4];

    ili9488_set_format(ILI9488_FMT_18BIT);

    console_printf("\n  CALIBRATION - four targets, touch the centre of\n");
    console_printf("  each cross as accurately as you can.\n\n");

    for (uint32_t t = 0; t < 4U; t++) {
        /* Clean slate, then a cross: two bars and a dot in the middle. */
        ili9488_fill_screen(0U, 0U, 0U);
        uint16_t cx = targets[t].x, cy = targets[t].y;
        ili9488_fill_rect((uint16_t)(cx - 20U), (uint16_t)(cy - 1U),
                          41U, 3U, 255U, 255U, 255U);
        ili9488_fill_rect((uint16_t)(cx - 1U), (uint16_t)(cy - 20U),
                          3U, 41U, 255U, 255U, 255U);
        ili9488_fill_rect((uint16_t)(cx - 3U), (uint16_t)(cy - 3U),
                          7U, 7U, 255U, 0U, 0U);

        console_printf("  target %u of 4, %s (%u,%u) - touch it\n",
                       t + 1U, targets[t].where, cx, cy);

        /*
         * Wait for a steady contact. Three consecutive reads that agree
         * within a few counts, which throws away the first moment of a
         * touch when the reported position is still moving as the
         * contact area grows.
         */
        uint32_t px = 0, py = 0, stable = 0;

        while (stable < 3U) {
            board_watchdog_feed();

            uint8_t reg0 = 0x00U, b[7];
            if (i2c3_write_read(FT_ADDR, &reg0, 1U, b, 7U)
                && (b[0x02] & 0x0FU) != 0U) {
                uint32_t tx = (((uint32_t)b[0x03] & 0x0FU) << 8) | b[0x04];
                uint32_t ty = (((uint32_t)b[0x05] & 0x0FU) << 8) | b[0x06];

                uint32_t dx = (tx > px) ? (tx - px) : (px - tx);
                uint32_t dy = (ty > py) ? (ty - py) : (py - ty);

                stable = (stable > 0U && dx <= 4U && dy <= 4U)
                       ? (stable + 1U) : 1U;
                px = tx;
                py = ty;
            } else {
                stable = 0;
            }
            board_delay_ms(30);
        }

        raw_x[t] = px;
        raw_y[t] = py;
        console_printf("    got raw(%u,%u)\n", px, py);

        /* Acknowledge with a green cross, then wait for the finger to
         * lift so the next target is not satisfied by the same touch. */
        ili9488_fill_rect((uint16_t)(cx - 3U), (uint16_t)(cy - 3U),
                          7U, 7U, 0U, 255U, 0U);

        uint32_t lifted = 0;
        while (lifted < 3U) {
            board_watchdog_feed();
            uint8_t reg0 = 0x00U, b[3];
            if (i2c3_write_read(FT_ADDR, &reg0, 1U, b, 3U)
                && (b[0x02] & 0x0FU) == 0U) {
                lifted++;
            } else {
                lifted = 0;
            }
            board_delay_ms(30);
        }
    }

    /*
     * Fit. screen_x comes from ty, screen_y from tx, inverted - the
     * axes established earlier. Two readings per edge, averaged.
     *
     * Integer arithmetic scaled by 1024 so the constants can be pasted
     * into a driver that has no business doing floating point in an
     * interrupt.
     */
    uint32_t ty_at_xlo = (raw_y[0] + raw_y[3]) / 2U;   /* screen x = 40  */
    uint32_t ty_at_xhi = (raw_y[1] + raw_y[2]) / 2U;   /* screen x = 440 */
    uint32_t tx_at_ylo = (raw_x[0] + raw_x[1]) / 2U;   /* screen y = 40  */
    uint32_t tx_at_yhi = (raw_x[2] + raw_x[3]) / 2U;   /* screen y = 280 */

    int32_t span_x = (int32_t)ty_at_xhi - (int32_t)ty_at_xlo;
    int32_t span_y = (int32_t)tx_at_yhi - (int32_t)tx_at_ylo;

    console_printf("\n  measured:\n");
    console_printf("    ty = %u at screen x=%u, ty = %u at screen x=%u\n",
                   ty_at_xlo, CAL_INSET_X, ty_at_xhi, ILI9488_W - CAL_INSET_X);
    console_printf("    tx = %u at screen y=%u, tx = %u at screen y=%u\n",
                   tx_at_ylo, CAL_INSET_Y, tx_at_yhi, ILI9488_H - CAL_INSET_Y);

    if (span_x == 0 || span_y == 0) {
        console_printf("\n  *** degenerate - two targets read the same. ***\n");
        for (;;) { board_watchdog_feed(); }
    }

    int32_t scale_x = ((int32_t)(ILI9488_W - 2 * CAL_INSET_X) * 1024) / span_x;
    int32_t scale_y = ((int32_t)(ILI9488_H - 2 * CAL_INSET_Y) * 1024) / span_y;

    console_printf("\n  CONSTANTS - paste these into the touch driver:\n");
    console_printf("    #define TOUCH_TY_REF   %u\n", ty_at_xlo);
    console_printf("    #define TOUCH_TX_REF   %u\n", tx_at_ylo);
    console_printf("    #define TOUCH_SCALE_X  %d   /* x1024 */\n", scale_x);
    console_printf("    #define TOUCH_SCALE_Y  %d   /* x1024 */\n", scale_y);
    console_printf("    screen_x = ((ty - TY_REF) * SCALE_X >> 10) + %u\n",
                   CAL_INSET_X);
    console_printf("    screen_y = ((tx - TX_REF) * SCALE_Y >> 10) + %u\n",
                   CAL_INSET_Y);

    console_printf("\n  Now calibrated. Touch anywhere - the square should\n");
    console_printf("  sit under your finger. Corners too.\n\n");

    ili9488_fill_screen(0U, 0U, 0U);

    uint32_t last_beat = 0;

    for (;;) {
        board_watchdog_feed();

        uint8_t reg0 = 0x00U, b[7];
        if (i2c3_write_read(FT_ADDR, &reg0, 1U, b, 7U)
            && (b[0x02] & 0x0FU) != 0U) {
            uint32_t tx = (((uint32_t)b[0x03] & 0x0FU) << 8) | b[0x04];
            uint32_t ty = (((uint32_t)b[0x05] & 0x0FU) << 8) | b[0x06];

            int32_t sx = ((((int32_t)ty - (int32_t)ty_at_xlo) * scale_x) >> 10)
                       + CAL_INSET_X;
            int32_t sy = ((((int32_t)tx - (int32_t)tx_at_ylo) * scale_y) >> 10)
                       + CAL_INSET_Y;

            if (sx < 0) { sx = 0; }
            if (sy < 0) { sy = 0; }
            if (sx > (int32_t)ILI9488_W - 1) { sx = ILI9488_W - 1; }
            if (sy > (int32_t)ILI9488_H - 1) { sy = ILI9488_H - 1; }

            console_printf("  raw(%u,%u) -> (%d,%d)\n", tx, ty, sx, sy);

            uint16_t x = (uint16_t)((sx > 8) ? (sx - 8) : 0);
            uint16_t y = (uint16_t)((sy > 8) ? (sy - 8) : 0);
            if (x > ILI9488_W - 17U) { x = ILI9488_W - 17U; }
            if (y > ILI9488_H - 17U) { y = ILI9488_H - 17U; }
            ili9488_fill_rect(x, y, 17U, 17U, 255U, 255U, 255U);

            board_delay_ms(40);
        }

        if ((int32_t)(board_millis() - last_beat) > 5000) {
            last_beat = board_millis();
            console_printf("  [%u s] waiting\n", board_millis() / 1000U);
        }
    }
}
