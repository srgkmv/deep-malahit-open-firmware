#include "touch.h"
#include "i2c3.h"
#include "ili9488.h"
#include "board.h"

#define FT_ADDR        0x38U

#define FT_TD_STATUS   0x02U
#define FT_G_MODE      0xA4U
#define FT_CIPHER      0xA3U
#define FT_VENDOR      0xA8U

static uint8_t s_chip_id;
static uint8_t s_vendor_id;

uint8_t touch_chip_id(void)   { return s_chip_id; }
uint8_t touch_vendor_id(void) { return s_vendor_id; }

static int read_reg(uint8_t reg, uint8_t *buf, uint32_t n)
{
    return i2c3_write_read(FT_ADDR, &reg, 1U, buf, n);
}

/*
 * WAIT FOR THE IDENTITY, DO NOT ASSUME THE DELAY WAS ENOUGH.
 *
 * i2c3_touch_reset() releases the reset line and waits 120 ms, which
 * is not enough: measured on this radio, the controller ACKs its
 * address almost at once but answers 0x00 to CIPHER and VENDOR for
 * about another 200 ms. An address probe therefore finds it and every
 * register read is a zero - a device that is present and says nothing
 * about itself.
 *
 * THIS WAS HIDDEN FOR A WHOLE STEP BY A CONSOLE LINE. The image that
 * first brought the touch panel up printed four lines and scanned all
 * 112 addresses between the reset and the ID read, which at 115 200
 * baud is a few hundred milliseconds of accidental delay. The driver
 * was written against that and looked solid. The first caller to ask
 * for the ID straight after the reset got zeros, and the symptom -
 * "no answer at 0x38" from a device that the scan on the next line
 * finds - points at the bus, which was never the problem.
 *
 * So the wait is a poll with a deadline, not a longer constant. A
 * constant long enough for this unit is a constant that is wrong for
 * the next one, in whichever direction costs more.
 */
#define FT_ID_TRIES    60U      /* x 10 ms = 600 ms, well past the 200 */

/*
 * BOTH IDs, AND BOTH NON-ZERO.
 *
 * It accepted on VENDOR alone and reported `cipher 0x00 vendor 0x11`
 * on a run that otherwise worked: CIPHER is read about a millisecond
 * before VENDOR, and during the wake-up window the part can answer
 * one and not yet the other. Reporting a part number of zero as a
 * successful identification is a small lie that would be read as a
 * big one the first time a different controller is fitted.
 */
/*
 * WHAT THE IDENTIFY LOOP SAW, for when it does not succeed.
 *
 * The failure is intermittent and the two obvious stories - the bus
 * arrived hung, or our own transfer hung it - predict different
 * things here, so the loop records enough to tell them apart: the
 * lines immediately after the reset pulse, the first iteration at
 * which SDA was found low, and whether the read at that iteration
 * reported success. A read that SUCCEEDS and leaves SDA low is a very
 * different fault from one that fails.
 */
static uint8_t s_dbg_after_reset;
static int16_t s_dbg_sda_low_at = -1;
static uint8_t s_dbg_rc_at;
static uint16_t s_dbg_tries;

uint8_t  touch_dbg_after_reset(void) { return s_dbg_after_reset; }
int16_t  touch_dbg_sda_low_at(void)  { return s_dbg_sda_low_at; }
uint8_t  touch_dbg_rc_at(void)       { return s_dbg_rc_at; }
uint16_t touch_dbg_tries(void)       { return s_dbg_tries; }

static int ft_identify(void)
{
    uint32_t t;

    for (t = 0U; t < FT_ID_TRIES; t++) {
        int rc = read_reg(FT_CIPHER, &s_chip_id, 1U);

        s_dbg_tries++;

        if ((i2c3_line_levels() & 1U) == 0U && s_dbg_sda_low_at < 0) {
            s_dbg_sda_low_at = (int16_t)s_dbg_tries;
            s_dbg_rc_at      = (uint8_t)rc;
        }

        if (rc
            && read_reg(FT_VENDOR, &s_vendor_id, 1U)
            && s_vendor_id == 0x11U
            && s_chip_id != 0x00U) {
            return 1;
        }
        board_delay_ms(10);
    }
    return 0;
}

int touch_init(void)
{
    i2c3_touch_reset();
    s_dbg_after_reset = (uint8_t)i2c3_line_levels();

    if (!ft_identify()) {
        /*
         * ONE MORE TRY, WITH THE BUS CLOCKED FREE IN BETWEEN.
         *
         * i2c3_init() already recovered the bus, but it did so BEFORE
         * the controller was reset, and a part that is reset in the
         * middle of shifting a byte out can take the line down again
         * on the next start. The observed failure is exactly that: a
         * recovery that truthfully reports zero clocks needed,
         * followed by SDA low and a scan that finds nothing.
         *
         * So the second attempt recovers the bus AFTER the reset
         * rather than before it. If that does not do it, the caller
         * gets a plain failure and the diagnostics in main.c say what
         * the lines were doing.
         */
        (void)i2c3_bus_recover();
        i2c3_touch_reset();
        if (!ft_identify()) { return 0; }
    }

    /*
     * G_MODE = 0x00, interrupt POLLING mode.
     *
     * It powers up at 0x01, interrupt TRIGGER mode, where INT gives a
     * pulse per report rather than sitting low while a finger is down.
     * Nothing here samples the pin - see the note in touch.h about PC8
     * - but leaving the controller in a mode whose signal we cannot
     * use would only make the eventual interrupt driver harder to
     * write.
     */
    uint8_t gmode[2] = { FT_G_MODE, 0x00U };
    (void)i2c3_write(FT_ADDR, gmode, 2U);

    /* FocalTech parts all report vendor 0x11; CIPHER distinguishes the
     * family member - 0x36 is the FT6236 fitted here. */
    return (s_vendor_id == 0x11U) ? 1 : 0;
}

static uint16_t map_x(uint32_t raw_ty)
{
    int32_t v = ((((int32_t)raw_ty - TOUCH_TY_REF) * TOUCH_SCALE_X) >> 10)
              + TOUCH_REF_X;
    if (v < 0) { v = 0; }
    if (v > (int32_t)ILI9488_W - 1) { v = (int32_t)ILI9488_W - 1; }
    return (uint16_t)v;
}

static uint16_t map_y(uint32_t raw_tx)
{
    int32_t v = ((((int32_t)raw_tx - TOUCH_TX_REF) * TOUCH_SCALE_Y) >> 10)
              + TOUCH_REF_Y;
    if (v < 0) { v = 0; }
    if (v > (int32_t)ILI9488_H - 1) { v = (int32_t)ILI9488_H - 1; }
    return (uint16_t)v;
}

uint32_t touch_poll(touch_point_t *pts)
{
    uint8_t b[15];

    /*
     * One transfer for the status byte and both contact blocks. Two
     * transfers - status first, then the points only if the status
     * says so - would save bus time when nothing is touching, and cost
     * a second bus turnaround when something is. At 100 kHz and a
     * handful of polls a second, neither matters; one transfer is
     * simpler and cannot see a half-updated state.
     */
    if (!read_reg(0x00U, b, 15U)) { return 0; }

    uint32_t n = b[FT_TD_STATUS] & 0x0FU;
    if (n > TOUCH_MAX_POINTS) { n = TOUCH_MAX_POINTS; }

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = &b[3U + i * 6U];

        uint32_t raw_x = (((uint32_t)p[0] & 0x0FU) << 8) | p[1];
        uint32_t raw_y = (((uint32_t)p[2] & 0x0FU) << 8) | p[3];

        pts[i].raw_x = (uint16_t)raw_x;
        pts[i].raw_y = (uint16_t)raw_y;
        pts[i].event = (uint8_t)(p[0] >> 6);
        pts[i].id    = (uint8_t)(p[2] >> 4);

        /* The swap lives here: the controller's Y becomes the display's
         * X, and its X becomes the display's Y, mirrored. */
        pts[i].x = map_x(raw_y);
        pts[i].y = map_y(raw_x);
    }

    return n;
}
