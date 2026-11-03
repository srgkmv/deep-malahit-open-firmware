#ifndef MALAHIT_TOUCH_H
#define MALAHIT_TOUCH_H

#include <stdint.h>

/*
 * FT6236 capacitive touch, in display coordinates.
 *
 * The controller is at 0x38 on I2C3 (CIPHER 0xA3 = 0x36,
 * FOCALTECH_ID 0xA8 = 0x11, both read back from this board). It
 * reports in its own portrait frame - 320 across by 480 down - while
 * the display runs landscape, so every reading needs a swap, a mirror
 * and the calibration below.
 */

#define TOUCH_MAX_POINTS   2U

typedef struct {
    uint16_t x, y;      /* display coordinates, 0..479 by 0..319 */
    uint16_t raw_x;     /* as the controller reported it */
    uint16_t raw_y;
    uint8_t  id;        /* contact id, for following a finger */
    uint8_t  event;     /* 0 press, 1 lift, 2 contact, 3 none */
} touch_point_t;

/*
 * CALIBRATION, measured on this radio.
 *
 * Four targets inset 40 px from the edges, touched by hand, giving:
 *
 *     screen (40,40)   -> raw (265, 46)
 *     screen (440,40)  -> raw (265, 442)
 *     screen (440,280) -> raw (37, 440)
 *     screen (40,280)  -> raw (38, 45)
 *
 * The four readings form a clean rectangle in raw space - opposite
 * corners agree on tx to within one count and on ty to within two -
 * which says the panel is linear and the numbers are worth trusting.
 *
 * Averaging each edge gives ty = 45 at x = 40 and ty = 441 at x = 440,
 * tx = 265 at y = 40 and tx = 37 at y = 280. Hence:
 *
 *     scale x = 400 / 396 = 1.0101   ->  1034/1024
 *     scale y = 240 / -228 = -1.0526 -> -1077/1024
 *
 * Both scales are within a few percent of unity, which is the sanity
 * check: the controller is reporting over essentially the same span as
 * the display, as it should for a panel bonded to it. The small
 * residual - a cursor sitting a little right of the finger before
 * calibration - was the glass offset, and these constants absorb it.
 *
 * Fixed point, shifted by 10, because this eventually runs from a
 * timer callback and floating point in an interrupt is a trap.
 */
#define TOUCH_TY_REF       45
#define TOUCH_TX_REF       265
#define TOUCH_SCALE_X      1034
#define TOUCH_SCALE_Y     (-1077)
#define TOUCH_REF_X        40
#define TOUCH_REF_Y        40

/* Reset the controller and put it in level-interrupt mode. Call after
 * i2c3_init(). Returns 1 if an FT6x36 answered and identified itself. */
int touch_init(void);

/*
 * How many fingers are down, filling in up to TOUCH_MAX_POINTS.
 * Returns 0 when nothing is touching, which is the common case and
 * costs one short I2C transfer.
 *
 * Polled, not interrupt driven, and deliberately so for now: PC8 reads
 * LOW continuously on this board whether or not anything is touching,
 * so the line cannot yet be trusted to mean what it should. The
 * register always tells the truth.
 */
uint32_t touch_poll(touch_point_t *pts);

/* CIPHER and FOCALTECH_ID as read at init, for reporting. */
uint8_t touch_chip_id(void);
uint8_t touch_vendor_id(void);

/*
 * What a failed touch_init() saw while it was failing. See the block
 * above ft_identify() in touch.c: the point is to tell "the bus
 * arrived hung" apart from "our own transfer hung it", which look
 * identical afterwards and need opposite fixes.
 *
 *   after_reset   SCL/SDA right after the reset pulse, as
 *                 i2c3_line_levels() encodes them
 *   sda_low_at    the attempt at which SDA was first found low,
 *                 or -1 if it never was
 *   rc_at         whether the read on that attempt reported success
 *   tries         how many attempts were made in total
 */
uint8_t  touch_dbg_after_reset(void);
int16_t  touch_dbg_sda_low_at(void);
uint8_t  touch_dbg_rc_at(void);
uint16_t touch_dbg_tries(void);

#endif /* MALAHIT_TOUCH_H */
