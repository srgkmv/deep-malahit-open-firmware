#include "stm32h7xx.h"
#include "ili9488.h"
#include "spi3.h"
#include "board.h"
#include "clock.h"

/* Control pins. None of these are SPI signals despite the net names. */
#define CS_PORT   GPIOA
#define CS_PIN    15U
#define DC_PORT   GPIOD
#define DC_PIN    0U
#define RST_PORT  GPIOD
#define RST_PIN   1U

#define CS_LOW()   (CS_PORT->BSRR = (1UL << (CS_PIN + 16U)))
#define CS_HIGH()  (CS_PORT->BSRR = (1UL << CS_PIN))
#define DC_CMD()   (DC_PORT->BSRR = (1UL << (DC_PIN + 16U)))
#define DC_DATA()  (DC_PORT->BSRR = (1UL << DC_PIN))

static ili9488_fmt_t s_fmt = ILI9488_FMT_18BIT;
static uint32_t      s_last_fill_us;

static void gpio_out(GPIO_TypeDef *port, uint32_t pin)
{
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
    port->OTYPER &= ~(1UL << pin);
    port->OSPEEDR |= (3UL << (pin * 2U));
    port->PUPDR &= ~(3UL << (pin * 2U));
}

static void cmd(uint8_t c)
{
    DC_CMD();
    CS_LOW();
    spi3_write(&c, 1U);
    CS_HIGH();
}

static void cmd_data(uint8_t c, const uint8_t *d, uint32_t n)
{
    DC_CMD();
    CS_LOW();
    spi3_write(&c, 1U);
    if (n > 0U) {
        DC_DATA();
        spi3_write(d, n);
    }
    CS_HIGH();
}

/*
 * The init sequence. Gamma and power values are the ones every ILI9488
 * driver carries, because they come from Ilitek's own application notes;
 * they are not tuned for this panel and probably could be, later, by
 * looking at a grey ramp.
 */
static void init_sequence(void)
{
    static const uint8_t pgamma[] = {
        0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78,
        0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F
    };
    static const uint8_t ngamma[] = {
        0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45,
        0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F
    };
    static const uint8_t pwr1[]   = { 0x17, 0x15 };
    static const uint8_t pwr2[]   = { 0x41 };
    static const uint8_t vcom[]   = { 0x00, 0x12, 0x80 };
    static const uint8_t ifmode[] = { 0x00 };
    static const uint8_t frmctr[] = { 0xA0 };
    static const uint8_t invctr[] = { 0x02 };
    static const uint8_t dfunc[]  = { 0x02, 0x02 };
    static const uint8_t entry[]  = { 0xC6 };
    static const uint8_t adj3[]   = { 0xA9, 0x51, 0x2C, 0x82 };

    /*
     * MADCTL 0x28: MV set swaps the axes and BGR matches this panel's
     * filter order, giving 480 wide by 320 tall with the ribbon at the
     * usual edge. If the picture comes out mirrored or rotated, this is
     * the single byte to change - 0xE8 is the other landscape option.
     */
    static const uint8_t madctl[] = { 0x28 };

    cmd(0x01);                                   /* software reset       */
    board_delay_ms(120);

    cmd_data(0xE0, pgamma, sizeof pgamma);       /* positive gamma       */
    cmd_data(0xE1, ngamma, sizeof ngamma);       /* negative gamma       */
    cmd_data(0xC0, pwr1,   sizeof pwr1);         /* power control 1      */
    cmd_data(0xC1, pwr2,   sizeof pwr2);         /* power control 2      */
    cmd_data(0xC5, vcom,   sizeof vcom);         /* VCOM control         */
    cmd_data(0x36, madctl, sizeof madctl);       /* memory access        */
    cmd_data(0xB0, ifmode, sizeof ifmode);       /* interface mode       */
    cmd_data(0xB1, frmctr, sizeof frmctr);       /* frame rate           */
    cmd_data(0xB4, invctr, sizeof invctr);       /* inversion control    */
    cmd_data(0xB6, dfunc,  sizeof dfunc);        /* display function     */
    cmd_data(0xB7, entry,  sizeof entry);        /* entry mode           */
    cmd_data(0xF7, adj3,   sizeof adj3);         /* adjust control 3     */

    ili9488_set_format(s_fmt);

    cmd(0x11);                                   /* sleep out            */
    board_delay_ms(120);
    cmd(0x29);                                   /* display on           */
    board_delay_ms(20);
}

void ili9488_set_format(ili9488_fmt_t fmt)
{
    uint8_t colmod = (fmt == ILI9488_FMT_16BIT) ? 0x55U : 0x66U;
    s_fmt = fmt;
    cmd_data(0x3A, &colmod, 1U);
}

ili9488_fmt_t ili9488_format(void)
{
    return s_fmt;
}

void ili9488_init(ili9488_fmt_t fmt)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIODEN;
    (void)RCC->AHB4ENR;

    gpio_out(CS_PORT, CS_PIN);
    gpio_out(DC_PORT, DC_PIN);
    gpio_out(RST_PORT, RST_PIN);

    CS_HIGH();
    DC_DATA();

    /*
     * Hardware reset. The datasheet wants 10 us low as a minimum and
     * 120 ms before the first command after release; these are generous
     * because nothing here is in a hurry and a panel that half-starts is
     * a miserable thing to debug.
     */
    RST_PORT->BSRR = (1UL << (RST_PIN + 16U));
    board_delay_ms(20);
    RST_PORT->BSRR = (1UL << RST_PIN);
    board_delay_ms(150);

    s_fmt = fmt;
    init_sequence();
}

void ili9488_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];

    buf[0] = (uint8_t)(x0 >> 8); buf[1] = (uint8_t)x0;
    buf[2] = (uint8_t)(x1 >> 8); buf[3] = (uint8_t)x1;
    cmd_data(0x2A, buf, 4U);                     /* column address set   */

    buf[0] = (uint8_t)(y0 >> 8); buf[1] = (uint8_t)y0;
    buf[2] = (uint8_t)(y1 >> 8); buf[3] = (uint8_t)y1;
    cmd_data(0x2B, buf, 4U);                     /* page address set     */
}

/*
 * Push a prepared block of pixels. ONE window, ONE transfer.
 *
 * This is what a spectrum display needs and what fill_rect() cannot
 * give it. Drawing 480 columns with fill_rect() means 480 window
 * settings and 480 separate SPI runs, and on this bus the per-transfer
 * overhead - CS, the RAMWR command, SPE, CSTART, waiting for EOT -
 * costs more than the pixels do. Building the whole area in RAM and
 * sending it in a single run turns a frame from hundreds of small
 * transactions into one big one, and the bus is then the only limit:
 * 480 x 160 x 3 bytes is 230 kB, about 61 ms at 30 MHz.
 *
 * `px` must already be in the panel's own format - three bytes a pixel
 * at 18 bits, with the data in the TOP six bits of each byte - because
 * converting here would mean touching every pixel twice.
 */
void ili9488_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  const uint8_t *px)
{
    if (w == 0U || h == 0U) { return; }

    ili9488_window(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U));

    DC_CMD();
    CS_LOW();
    uint8_t ramwr = 0x2C;
    spi3_write(&ramwr, 1U);
    DC_DATA();
    spi3_write(px, (uint32_t)w * (uint32_t)h
                   * ((s_fmt == ILI9488_FMT_16BIT) ? 2U : 3U));
    CS_HIGH();
}

void ili9488_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       uint8_t r, uint8_t g, uint8_t b)
{
    if (w == 0U || h == 0U) { return; }

    ili9488_window(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U));

    uint8_t  px[3];
    uint32_t px_len;

    if (s_fmt == ILI9488_FMT_16BIT) {
        uint16_t c = (uint16_t)(((r & 0xF8U) << 8)
                              | ((g & 0xFCU) << 3)
                              |  (b >> 3));
        px[0] = (uint8_t)(c >> 8);
        px[1] = (uint8_t)c;
        px_len = 2U;
    } else {
        /* 18-bit: the panel takes the top six bits of each byte. */
        px[0] = r & 0xFCU;
        px[1] = g & 0xFCU;
        px[2] = b & 0xFCU;
        px_len = 3U;
    }

    DC_CMD();
    CS_LOW();
    uint8_t ramwr = 0x2C;
    spi3_write(&ramwr, 1U);
    DC_DATA();
    spi3_write_repeat(px, px_len, (uint32_t)w * (uint32_t)h);
    CS_HIGH();
}

void ili9488_read_raw(uint8_t cmd, uint8_t *buf, uint32_t nbytes)
{
    DC_CMD();
    CS_LOW();
    spi3_write(&cmd, 1U);
    DC_DATA();
    spi3_read(buf, nbytes);
    CS_HIGH();
}

uint32_t ili9488_read_reg(uint8_t cmd, uint32_t nbytes)
{
    uint8_t buf[5] = { 0, 0, 0, 0, 0 };

    if (nbytes > 4U) { nbytes = 4U; }

    /*
     * NO DUMMY BYTE. Measured, not assumed: reading 0x0C raw returns
     * 06 00 00 00 ... with the answer in the FIRST byte, and 0x06 is
     * exactly the documented power-on value for COLMOD. An earlier
     * version here skipped a leading dummy byte, as many ILI9488
     * drivers do, and so returned the byte after the answer - a
     * confident, plausible zero.
     */
    DC_CMD();
    CS_LOW();
    spi3_write(&cmd, 1U);
    DC_DATA();
    spi3_read(buf, nbytes);
    CS_HIGH();

    uint32_t v = 0;
    for (uint32_t i = 0; i < nbytes; i++) {
        v = (v << 8) | buf[i];
    }
    return v;
}

/* DWT cycle counter: the only timebase here fine enough to measure a
 * screen fill, SysTick being a 1 kHz tick. */
static void cycles_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void ili9488_fill_screen(uint8_t r, uint8_t g, uint8_t b)
{
    cycles_init();
    uint32_t t0 = DWT->CYCCNT;

    ili9488_fill_rect(0U, 0U, ILI9488_W, ILI9488_H, r, g, b);

    uint32_t cycles = DWT->CYCCNT - t0;
    s_last_fill_us = cycles / (clock_sysclk_hz / 1000000U);
}

uint32_t ili9488_last_fill_us(void)
{
    return s_last_fill_us;
}

void ili9488_test_pattern(void)
{
    static const uint8_t bars[8][3] = {
        {   0,   0,   0 },   /* black   */
        { 255,   0,   0 },   /* red     */
        {   0, 255,   0 },   /* green   */
        {   0,   0, 255 },   /* blue    */
        { 255, 255,   0 },   /* yellow  */
        {   0, 255, 255 },   /* cyan    */
        { 255,   0, 255 },   /* magenta */
        { 255, 255, 255 }    /* white   */
    };

    uint16_t bw = ILI9488_W / 8U;    /* 60 px per bar */

    for (uint32_t i = 0; i < 8U; i++) {
        ili9488_fill_rect((uint16_t)(i * bw), 0U, bw, ILI9488_H,
                          bars[i][0], bars[i][1], bars[i][2]);
    }

    /*
     * A one-pixel white frame. Its job is geometry, not decoration: if
     * the panel is consuming a different number of bytes per pixel than
     * we are sending, the frame will not close - the bottom edge lands
     * somewhere in the middle, or the sides wander.
     */
    ili9488_fill_rect(0U, 0U, ILI9488_W, 1U, 255U, 255U, 255U);
    ili9488_fill_rect(0U, (uint16_t)(ILI9488_H - 1U), ILI9488_W, 1U,
                      255U, 255U, 255U);
    ili9488_fill_rect(0U, 0U, 1U, ILI9488_H, 255U, 255U, 255U);
    ili9488_fill_rect((uint16_t)(ILI9488_W - 1U), 0U, 1U, ILI9488_H,
                      255U, 255U, 255U);
}
