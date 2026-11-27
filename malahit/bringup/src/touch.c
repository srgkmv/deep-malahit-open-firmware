#include "touch.h"
#include "i2c3.h"
#include "ili9488.h"
#include "board.h"
#include "console.h"

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
 * i2c3_touch_reset() released the reset line and waited 120 ms (until
 * 21/11/2026 - FT_WAKE_MS below), which is not enough: measured on this radio, the controller ACKs its
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
 * AND THE POLL STARTS AFTER THE DATASHEET'S WAKE TIME, NOT BEFORE IT -
 * 21/11/2026.
 *
 * The poll above began 120 ms after the reset, so its first ~5 reads
 * were made while the controller was still waking - the window in
 * which it ACKs and answers zeros. Four cold boots that afternoon:
 * two found it at once (the 6th read, SDA never low), one needed the
 * second attempt (+1.8 s) and one never found it at all (+5.8 s, "no
 * answer at 0x38", touch dead for the session). Both bad ones had SDA
 * stuck low from the 5th or 6th read on, with that read failing: the
 * read that lands on the moment the part's I2C comes alive can leave
 * it mid-byte, holding SDA, and every read after it then fails without
 * a START.
 *
 * FT6236 datasheet, reset timing: Trsi, "time of starting to report
 * point after resetting", 300 ms minimum. So the bus stays silent for
 * that long and the poll follows - a deadline still, for the unit that
 * is slower than its datasheet. test_touch_wake() in main.c measures
 * this against the old 120 ms.
 */
#define FT_WAKE_MS     300U

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
static uint16_t s_dbg_recovers;
static uint8_t s_dbg_attempts;

uint8_t  touch_dbg_after_reset(void) { return s_dbg_after_reset; }
int16_t  touch_dbg_sda_low_at(void)  { return s_dbg_sda_low_at; }
uint8_t  touch_dbg_rc_at(void)       { return s_dbg_rc_at; }
uint16_t touch_dbg_tries(void)       { return s_dbg_tries; }
uint16_t touch_dbg_recovers(void)    { return s_dbg_recovers; }
uint8_t  touch_dbg_attempts(void)    { return s_dbg_attempts; }

static int ft_identify(int recover)
{
    uint32_t t;

    s_dbg_attempts++;

    for (t = 0U; t < FT_ID_TRIES; t++) {
        int rc = read_reg(FT_CIPHER, &s_chip_id, 1U);
        int sda_low = ((i2c3_line_levels() & 1U) == 0U);

        s_dbg_tries++;

        if (sda_low && s_dbg_sda_low_at < 0) {
            s_dbg_sda_low_at = (int16_t)s_dbg_tries;
            s_dbg_rc_at      = (uint8_t)rc;
        }

        if (rc
            && read_reg(FT_VENDOR, &s_vendor_id, 1U)
            && s_vendor_id == 0x11U
            && s_chip_id != 0x00U) {
            return 1;
        }

        /*
         * ONE STUCK READ MUST NOT POISON THE REST. A failed read that
         * leaves SDA low means the part is holding the line mid-byte,
         * and no START can be made until it is clocked free - so every
         * remaining try of this loop used to fail the same way, and
         * the bus was only recovered between the two attempts. Clock
         * it free here, at once.
         */
        if (recover && sda_low) {
            (void)i2c3_bus_recover();
            s_dbg_recovers++;
        }
        board_delay_ms(10);
    }
    return 0;
}

int touch_init(void)
{
    return touch_init_cfg(FT_WAKE_MS, 1);
}

int touch_init_cfg(uint32_t wake_ms, int recover)
{
    /* Per call, so that a test calling this many times reads each
     * call's own story - and the boot's single call reads the same. */
    s_dbg_sda_low_at = -1;
    s_dbg_rc_at      = 0U;
    s_dbg_tries      = 0U;
    s_dbg_recovers   = 0U;
    s_dbg_attempts   = 0U;
    s_chip_id        = 0U;
    s_vendor_id      = 0U;

    i2c3_touch_reset(wake_ms);
    s_dbg_after_reset = (uint8_t)i2c3_line_levels();

    if (!ft_identify(recover)) {
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
        i2c3_touch_reset(wake_ms);
        if (!ft_identify(recover)) { return 0; }
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

/*
 * NOTHING THE CONTROLLER SAYS IS TAKEN ON TRUST - 27/11/2026.
 *
 * Phantom taps at the panel's top right - (479, 0), (470, 0) - were seen
 * since 22/11 with nobody at the glass, and since the clock became a key
 * (step 56) each one opens the time keypad (the owner, 27/11/2026). This
 * poll believed every frame: TD_STATUS & 0x0F as the count (an FT6236
 * has 0, 1 or 2), and any coordinates. A read that comes back all 0xFF -
 * the bus with nobody driving it - makes 15 points, cut to 2, at raw
 * 0xFFF / 0xFFF, which the swap and the clamp put at exactly (479, 0):
 * outside the controller's own frame (0..319 x 0..479) by eight times.
 * And a failed read returned 0 points - a LIFT in the middle of a drag.
 *
 * So, two guards:
 *
 *  1. A FRAME IS CHECKED: the status's top nibble zero and at most two
 *     points; each point's id 0 or 1 and its coordinates inside the
 *     controller's frame; a point whose event is "lift up" (1) or "no
 *     event" (3) is not a contact. A frame that fails - or a read that
 *     fails - changes nothing: the last good answer is given again.
 *
 *  2. A NEW TOUCH IS SEEN TWICE: from nothing to a finger takes two good
 *     frames in a row, one poll later than before (~40 ms). One frame of
 *     a finger and then nothing is a phantom, not a tap.
 *
 * Both say so on the console, at most once a second, with the raw bytes:
 * what was thrown away is the evidence of what the phantoms are.
 */
#define FT_RAW_X_MAX  319U      /* the controller's portrait frame */
#define FT_RAW_Y_MAX  479U

static touch_point_t s_ult[TOUCH_MAX_POINTS];   /* the last answer given */
static uint32_t      s_ult_n;
static uint8_t       s_pendiente;               /* a new touch seen once */
static touch_point_t s_pend_pt;
static uint32_t      s_malos, s_sueltos;
static uint32_t      s_log_ms;

uint32_t touch_malos(void)   { return s_malos; }
uint32_t touch_sueltos(void) { return s_sueltos; }

static uint8_t log_toca(void)
{
    uint32_t t = board_millis();

    if (s_log_ms != 0U && (t - s_log_ms) < 1000U) { return 0U; }
    s_log_ms = (t != 0U) ? t : 1U;
    return 1U;
}

static uint32_t repite(touch_point_t *pts)
{
    for (uint32_t i = 0; i < s_ult_n; i++) { pts[i] = s_ult[i]; }
    return s_ult_n;
}

uint32_t touch_poll(touch_point_t *pts)
{
    uint8_t  b[15];
    uint32_t n, i, validos = 0U;
    uint8_t  malo = 0U;

    /*
     * One transfer for the status byte and both contact blocks. Two
     * transfers - status first, then the points only if the status
     * says so - would save bus time when nothing is touching, and cost
     * a second bus turnaround when something is. At 100 kHz and a
     * handful of polls a second, neither matters; one transfer is
     * simpler and cannot see a half-updated state.
     */
    if (!read_reg(0x00U, b, 15U)) {
        s_malos++;
        return repite(pts);
    }

    n = b[FT_TD_STATUS] & 0x0FU;
    if ((b[FT_TD_STATUS] & 0xF0U) != 0U || n > TOUCH_MAX_POINTS) { malo = 1U; n = 0U; }

    for (i = 0; i < n && !malo; i++) {
        const uint8_t *p = &b[3U + i * 6U];
        touch_point_t  t;
        uint32_t raw_x = (((uint32_t)p[0] & 0x0FU) << 8) | p[1];
        uint32_t raw_y = (((uint32_t)p[2] & 0x0FU) << 8) | p[3];

        t.raw_x = (uint16_t)raw_x;
        t.raw_y = (uint16_t)raw_y;
        t.event = (uint8_t)(p[0] >> 6);
        t.id    = (uint8_t)(p[2] >> 4);
        if (t.id > 1U || raw_x > FT_RAW_X_MAX || raw_y > FT_RAW_Y_MAX) { malo = 1U; break; }
        if (t.event == 1U || t.event == 3U) { continue; }   /* lifted, or nothing */

        /* The swap lives here: the controller's Y becomes the display's
         * X, and its X becomes the display's Y, mirrored. */
        t.x = map_x(raw_y);
        t.y = map_y(raw_x);
        pts[validos++] = t;
    }

    if (malo) {
        s_malos++;
        if (log_toca()) {
            console_printf("  touch: bad frame thrown away (%u so far) - status %02x, "
                           "%02x %02x %02x %02x %02x %02x\n", s_malos,
                           (uint32_t)b[FT_TD_STATUS], (uint32_t)b[3], (uint32_t)b[4],
                           (uint32_t)b[5], (uint32_t)b[6], (uint32_t)b[7], (uint32_t)b[8]);
        }
        return repite(pts);
    }

    if (validos == 0U) {
        if (s_pendiente) {
            s_sueltos++;
            if (log_toca()) {
                console_printf("  touch: seen once at (%u,%u) raw (%u,%u) ev %u - not a tap "
                               "(%u so far)\n", (uint32_t)s_pend_pt.x, (uint32_t)s_pend_pt.y,
                               (uint32_t)s_pend_pt.raw_x, (uint32_t)s_pend_pt.raw_y,
                               (uint32_t)s_pend_pt.event, s_sueltos);
            }
        }
        s_pendiente = 0U;
        s_ult_n = 0U;
        return 0U;
    }
    if (s_ult_n == 0U && !s_pendiente) {
        s_pendiente = 1U;               /* the first sight: wait for the second */
        s_pend_pt = pts[0];
        return 0U;
    }
    s_pendiente = 0U;
    for (i = 0; i < validos; i++) { s_ult[i] = pts[i]; }
    s_ult_n = validos;
    return validos;
}
