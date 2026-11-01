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

int touch_init(void)
{
    i2c3_touch_reset();

    if (!read_reg(FT_CIPHER, &s_chip_id, 1U))  { return 0; }
    if (!read_reg(FT_VENDOR, &s_vendor_id, 1U)) { return 0; }

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
