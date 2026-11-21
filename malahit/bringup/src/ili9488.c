#include <stddef.h>

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

/*
 * THE BUS HAS ONE OWNER AT A TIME, AND THE BLOCK QUEUE IS ONE.
 *
 * Since 21/11/2026 pixel blocks go to the panel through a queue (below,
 * "THE BLOCK QUEUE"): the display loop converts a block into a slot and
 * queues it, and SPI3's end-of-transfer interrupt sends one block after
 * another by DMA. While anything is queued the bus is the interrupt's,
 * CS may be low in the middle of a RAMWR, and anything else sent - a
 * window command, a fill, a register read - would land among some
 * block's pixels. So every function below that lowers CS from the
 * display loop calls bus_idle() first, which waits for the queue to
 * empty. That is the only rule: upstream's lcd_dma contract asks every
 * CALLER to remember ("mientras esto esta en marcha NO se puede tocar el
 * panel por ningun otro sitio"); here the driver remembers for them.
 *
 * The interrupt itself uses the raw_ functions, which do not wait - it
 * is the thing being waited for.
 *
 * s_dma_ok is the self-test's verdict, and it goes back to 0 the first
 * time the queue stalls - upstream's policy, and the right one: a DMA
 * that did not finish once is not trusted to finish next time, and the
 * polled path is always there. s_dma_want is the switch.
 */
static int s_dma_ok;
static int s_dma_want = 1;

static void bus_idle(void);

void ili9488_sync(void)
{
    bus_idle();
}

static void gpio_out(GPIO_TypeDef *port, uint32_t pin)
{
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
    port->OTYPER &= ~(1UL << pin);
    port->OSPEEDR |= (3UL << (pin * 2U));
    port->PUPDR &= ~(3UL << (pin * 2U));
}

static void cmd(uint8_t c)
{
    bus_idle();
    DC_CMD();
    CS_LOW();
    spi3_write(&c, 1U);
    CS_HIGH();
}

static void raw_cmd_data(uint8_t c, const uint8_t *d, uint32_t n)
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

static void cmd_data(uint8_t c, const uint8_t *d, uint32_t n)
{
    bus_idle();
    raw_cmd_data(c, d, n);
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
    bus_idle();

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

static void raw_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];

    buf[0] = (uint8_t)(x0 >> 8); buf[1] = (uint8_t)x0;
    buf[2] = (uint8_t)(x1 >> 8); buf[3] = (uint8_t)x1;
    raw_cmd_data(0x2A, buf, 4U);                 /* column address set   */

    buf[0] = (uint8_t)(y0 >> 8); buf[1] = (uint8_t)y0;
    buf[2] = (uint8_t)(y1 >> 8); buf[3] = (uint8_t)y1;
    raw_cmd_data(0x2B, buf, 4U);                 /* page address set     */
}

void ili9488_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    bus_idle();
    raw_window(x0, y0, x1, y1);
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

    bus_idle();

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

/* ================================================================== */
/* THE BLOCK QUEUE                                                    */
/* ================================================================== */

/*
 * WHY A QUEUE, AND WHY IT IS RUN FROM AN INTERRUPT.
 *
 * The first DMA version (21/11/2026, same day) started each block from
 * the display loop, double-buffered: convert block N+1 while N goes out,
 * then wait for N and start N+1. Measured on the radio against the
 * polled path, same run, same picture: waterfall 48.3 -> 39.5 ms, frame
 * 72-80 -> 59.6/68.3 ms. Short of the 32 ms the waterfall's bytes need,
 * and by one number: the AM block interrupt runs 8.4 ms at a time, and a
 * block is 2.7 ms on the wire. Whatever block was going out when the
 * interrupt arrived finished, and then the bus stood idle until the
 * display loop got the core back to start the next one.
 *
 * So the next block has to be started by something that outranks the
 * demodulator, and that has blocks to hand. SPI3's end-of-transfer
 * interrupt, at priority 0 (with SysTick; the block interrupt is 1),
 * raises CS, sends the next block's window and RAMWR polled - eleven
 * bytes, a few microseconds - and starts its DMA. The display loop only
 * converts and queues, and waits only when every slot is taken.
 *
 * EIGHT SLOTS OF FOUR FULL ROWS, 46 kB in all - the same 46 kB the
 * staging buffer always was, moved here and cut differently. Full,
 * that is 12.3 ms on the wire, against the 8.4 ms interrupt it has to
 * outlast. Four rows because the blocks that cost the most are full
 * width: gfx2's 8-row bands and the waterfall's 8-row chunks each go as
 * two blocks, and the spectrum's half-bands (4 rows) as one.
 *
 * The counters only ever increase: head counts blocks queued, tail blocks
 * finished, and head - tail is how many are queued or on the wire. The
 * display loop is the only writer of head and the interrupt of tail, so
 * neither needs a lock. The slot being filled is head % Q_SLOTS, which
 * is free whenever head - tail < Q_SLOTS.
 */
#define Q_SLOTS         8U
#define Q_SLOT_BYTES    (ILI9488_W * 4U * 3U)

static uint8_t s_slot[Q_SLOTS][Q_SLOT_BYTES] __attribute__((aligned(32)));

typedef struct {
    uint16_t x, y, w, h;
    uint32_t len;
} q_blk_t;

static q_blk_t           s_q[Q_SLOTS];
static volatile uint32_t s_q_head;      /* written by the display loop  */
static volatile uint32_t s_q_tail;      /* written by SPI3_IRQHandler   */
static volatile int      s_q_busy;      /* a block is on the wire       */
static uint32_t          s_q_stalls;    /* queues that stopped moving   */
static uint32_t          s_q_depth_max; /* deepest since last asked     */

/*
 * Start the block at the tail. Interrupt context, or the display loop
 * with the interrupt unable to run (ili9488_q_push() pends it instead of
 * calling this, so in practice: interrupt context only).
 */
static void q_kick(void)
{
    uint32_t       i = s_q_tail % Q_SLOTS;
    const q_blk_t *b = &s_q[i];

    raw_window(b->x, b->y, (uint16_t)(b->x + b->w - 1U),
               (uint16_t)(b->y + b->h - 1U));

    DC_CMD();
    CS_LOW();
    uint8_t ramwr = 0x2C;
    spi3_write(&ramwr, 1U);
    DC_DATA();

    if (spi3_dma_start(s_slot[i], b->len, 1) == 0) {
        s_q_busy = 1;
        return;
    }

    /* Cannot happen - the slots are in AXI SRAM and never longer than a
     * transfer - but if it did, polled is still right, just slow. The
     * caller sees the tail move and calls again for the next one. */
    spi3_write(s_slot[i], b->len);
    CS_HIGH();
    s_q_tail = s_q_tail + 1U;
}

/*
 * END OF A BLOCK, OR A NUDGE FROM THE DISPLAY LOOP.
 *
 * EOT set: the block on the wire has gone - collect it (spi3_dma_wait()
 * returns at once, and its teardown clears EOT and the interrupt
 * enable), raise CS, and start the next if there is one. EOT clear while
 * busy: the loop pended this to say "there is more", and the block in
 * flight will bring the handler back by itself. Not busy: start one.
 */
void SPI3_IRQHandler(void)
{
    if (s_q_busy) {
        if (!(SPI3->SR & SPI_SR_EOT)) { return; }

        (void)spi3_dma_wait();
        CS_HIGH();
        s_q_busy = 0;
        s_q_tail = s_q_tail + 1U;
    }

    while (!s_q_busy && s_q_head != s_q_tail) { q_kick(); }
}

/*
 * THE QUEUE STOPPED MOVING. Display loop only, with the interrupt off
 * for the duration. Whatever was on the wire is torn down (spi3_dma_wait()
 * times out and aborts), what was queued is dropped - those blocks will
 * be missing from one frame, and the next frame repaints them - and the
 * DMA stays off for the rest of the run.
 */
static void q_abort(void)
{
    NVIC_DisableIRQ(SPI3_IRQn);

    (void)spi3_dma_wait();
    CS_HIGH();

    s_q_busy = 0;
    s_q_tail = s_q_head;
    s_dma_ok = 0;
    s_q_stalls++;

    NVIC_ClearPendingIRQ(SPI3_IRQn);
    NVIC_EnableIRQ(SPI3_IRQn);
}

/*
 * Wait until at most `n` blocks are queued. The limit is on PROGRESS,
 * not on the whole wait: as long as the tail keeps moving the queue is
 * working, however long it is. 20 ms without a block finishing is ten
 * times a full slot's time on the wire at 30 MHz; the spin count is the
 * same second bound spi3_dma_wait() has, for a cycle counter a debugger
 * switched off.
 */
static void q_wait_until(uint32_t n)
{
    uint32_t last  = s_q_tail;
    uint32_t t0    = DWT->CYCCNT;
    uint32_t limit = clock_sysclk_hz / 50U;
    uint32_t spins = 0U;

    while ((s_q_head - s_q_tail) > n) {
        uint32_t t = s_q_tail;

        if (t != last) {
            last  = t;
            t0    = DWT->CYCCNT;
            spins = 0U;
            continue;
        }
        if ((uint32_t)(DWT->CYCCNT - t0) > limit || ++spins > 100000000U) {
            if (s_q_tail != last) { continue; }     /* moved meanwhile */
            q_abort();
            return;
        }
    }
}

static void bus_idle(void)
{
    if (s_q_head != s_q_tail) { q_wait_until(0U); }
}

uint8_t *ili9488_q_slot(uint32_t *cap)
{
    q_wait_until(Q_SLOTS - 1U);

    if (cap != NULL) { *cap = Q_SLOT_BYTES; }
    return s_slot[s_q_head % Q_SLOTS];
}

void ili9488_q_push(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (w == 0U || h == 0U) { return; }

    uint32_t len = (uint32_t)w * (uint32_t)h
                 * ((s_fmt == ILI9488_FMT_16BIT) ? 2U : 3U);
    uint32_t i   = s_q_head % Q_SLOTS;

    if (len > Q_SLOT_BYTES) { return; }     /* not from ili9488_q_slot() */

    if (!s_dma_ok || !s_dma_want) {
        /* Polled, from the slot, before returning - so the slot is free
         * again at once and head never moves. */
        ili9488_blit(x, y, w, h, s_slot[i]);
        return;
    }

    s_q[i].x   = x;
    s_q[i].y   = y;
    s_q[i].w   = w;
    s_q[i].h   = h;
    s_q[i].len = len;

    /* The descriptor and the slot's bytes before the count that hands
     * them over. Same core, so program order already holds for the
     * handler; the barrier is for the compiler, and costs nothing. */
    __DMB();
    s_q_head = s_q_head + 1U;

    uint32_t depth = s_q_head - s_q_tail;

    if (depth > s_q_depth_max) { s_q_depth_max = depth; }

    /* "There is more." The handler starts it if the wire is idle and
     * ignores it otherwise. */
    NVIC_SetPendingIRQ(SPI3_IRQn);
}

uint32_t ili9488_q_depth_max(void)
{
    uint32_t d = s_q_depth_max;

    s_q_depth_max = 0U;
    return d;
}

uint32_t ili9488_q_stalls(void)
{
    return s_q_stalls;
}

int ili9488_dma_init(uint32_t *test_us)
{
    static uint8_t probe[256];

    bus_idle();
    s_dma_ok = 0;

    if (spi3_dma_init() != 0) { return -1; }

    /*
     * CS HIGH: the panel ignores the clock entirely, so this proves the
     * stream, the multiplexer, TXDMAEN and the teardown - everything but
     * the pixels - without a mark on the screen. The bytes are a ramp
     * only so that a logic analyser would see something recognisable.
     */
    CS_HIGH();
    for (uint32_t i = 0; i < sizeof probe; i++) { probe[i] = (uint8_t)i; }

    uint32_t t0 = DWT->CYCCNT;

    if (spi3_dma_start(probe, sizeof probe, 0) != 0) { return -2; }

    int      rc = spi3_dma_wait();
    uint32_t us = (DWT->CYCCNT - t0) / (clock_sysclk_hz / 1000000U);

    if (test_us != NULL) { *test_us = us; }
    if (rc != 0) { return -3; }

    /* 256 bytes are 2 048 bit times; at 30 MHz, 68 us. An EOT that turns
     * up in less than half of that was not this transfer's. */
    uint32_t wire_us = (uint32_t)((sizeof probe * 8U * 1000000ULL)
                                  / spi3_hz());

    if (us < wire_us / 2U) { return -4; }

    /*
     * The queue's interrupt. Priority 0, with SysTick and the console:
     * it has to outrank the block interrupt (1), whose 8.4 ms are exactly
     * the time the queue exists to keep the bus busy through. What it
     * costs them is a few microseconds per block - the window commands
     * go out polled from inside it.
     */
    SPI3->IER = 0U;
    NVIC_ClearPendingIRQ(SPI3_IRQn);
    NVIC_SetPriority(SPI3_IRQn, 0);
    NVIC_EnableIRQ(SPI3_IRQn);

    s_dma_ok = 1;
    return 0;
}

void ili9488_dma_enable(int on)
{
    bus_idle();
    s_dma_want = (on != 0);
}

int ili9488_dma_on(void)
{
    return s_dma_ok && s_dma_want;
}

void ili9488_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       uint8_t r, uint8_t g, uint8_t b)
{
    if (w == 0U || h == 0U) { return; }

    bus_idle();

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
    bus_idle();
    DC_CMD();
    CS_LOW();
    spi3_write(&cmd, 1U);
    DC_DATA();
    spi3_read(buf, nbytes);
    CS_HIGH();
}

/*
 * READ THE PANEL'S OWN MEMORY BACK - 21/11/2026, for screenshots.
 *
 * Window, then RAMRD (0x2E), then the bytes as they come: three a pixel
 * in 18-bit mode, data in the top six bits of each. Whether a dummy
 * byte leads them is NOT decided here - register reads on this panel
 * turned out to have none, against most drivers' habit, so this hands
 * back everything raw and the reader finds the phase (tools/grab.py
 * reads one byte more than it needs for exactly that). Through
 * spi3_read(), so at 3.75 MHz: 46 kB in about 100 ms.
 */
void ili9488_read_gram(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       uint8_t *buf, uint32_t nbytes)
{
    if (w == 0U || h == 0U) { return; }
    bus_idle();
    raw_window(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U));
    ili9488_read_raw(0x2E, buf, nbytes);
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
    bus_idle();
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
    /*
     * Switched on, NOT zeroed. It used to be zeroed here, and the
     * counter is shared: am.c, sai1.c and spi3.c's DMA timeout all time
     * intervals on it, and a reset in the middle of one of those makes
     * it look four billion cycles long. The fill below only ever uses a
     * difference, which does not care where the counter started.
     */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
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
