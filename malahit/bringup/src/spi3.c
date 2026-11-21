#include <stddef.h>

#include "stm32h7xx.h"
#include "spi3.h"
#include "clock.h"

static void dma_backstop(void);

/*
 * The H7's SPI is not the F4's, and transcribing an F4 driver here does
 * not work. The differences that matter:
 *
 *   - The bit rate divider lives in CFG1.MBR, not CR1.BR.
 *   - Transfers are counted. CR2.TSIZE says how many frames are coming;
 *     the peripheral raises EOT when that many have gone, and refuses to
 *     start another run until it is told again.
 *   - Enabling the peripheral is not enough to make it transmit. SPE
 *     turns it on, CSTART starts the transfer. Two separate writes, in
 *     that order.
 *   - Writing a byte means an 8-bit write to TXDR. A 32-bit write pushes
 *     four frames' worth into the FIFO and the panel receives rubbish.
 *     The cast below is load-bearing.
 */

#define PLL1Q_HZ    240000000U

static spi3_div_t s_div = SPI3_DIV16;

uint32_t spi3_hz(void)
{
    return PLL1Q_HZ >> (s_div + 1U);
}

static void gpio_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (2UL << (pin * 2U));
    port->OSPEEDR |= (3UL << (pin * 2U));            /* very high speed */
    port->OTYPER &= ~(1UL << pin);
    port->PUPDR &= ~(3UL << (pin * 2U));
    port->AFR[pin >> 3U] =
          (port->AFR[pin >> 3U] & ~(0xFUL << ((pin & 7U) * 4U)))
        | (af << ((pin & 7U) * 4U));
}

void spi3_set_div(spi3_div_t div)
{
    dma_backstop();

    s_div = div;

    SPI3->CR1 &= ~SPI_CR1_SPE;
    SPI3->CFG1 = (SPI3->CFG1 & ~SPI_CFG1_MBR_Msk)
               | ((uint32_t)div << SPI_CFG1_MBR_Pos);
}

void spi3_init(spi3_div_t div)
{
    dma_backstop();

    RCC->AHB4ENR  |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOCEN
                   | RCC_AHB4ENR_GPIODEN;
    RCC->APB1LENR |= RCC_APB1LENR_SPI3EN;
    (void)RCC->APB1LENR;

    /*
     * SAY WHICH KERNEL CLOCK. DO NOT INHERIT ONE.
     *
     * SPI123SEL defaults to 000 = pll1_q_ck, and this code assumed the
     * default. It should not have: an image loaded over SWD inherits
     * every RCC setting the previous firmware left behind, and the
     * factory firmware here sets SPI123SEL = 100 = per_ck, which is HSI
     * at 64 MHz.
     *
     * The symptom was a bus that measured exactly 3.75 times slower than
     * it should - 240/64 - at every divider, with the times scaling
     * perfectly as the divider changed. Perfect scaling says the bus is
     * the limit; the constant factor says the bus is not the speed you
     * think. Both were true at once, which is why it took a readback of
     * D2CCIP1R to see.
     */
    RCC->D2CCIP1R &= ~RCC_D2CCIP1R_SPI123SEL_Msk;   /* 000 = pll1_q_ck */

    gpio_af(GPIOC, 10U, 6U);   /* SCK  */
    gpio_af(GPIOC, 11U, 6U);   /* MISO - configured, not yet used */
    gpio_af(GPIOC, 12U, 6U);   /* MOSI */

    s_div = div;

    SPI3->CR1  = 0U;
    SPI3->IFCR = 0xFFFFFFFFU;

    /*
     * SSI BEFORE CFG2, AND THIS ORDER IS NOT COSMETIC.
     *
     * With SSM set, the master's view of its own NSS input comes from
     * CR1.SSI. Enable master mode while SSI is still 0 and the
     * peripheral sees NSS pulled low - another master on the bus, as far
     * as it knows - raises a mode fault, and a mode fault CLEARS THE
     * MASTER BIT. In hardware, silently.
     *
     * What that looks like from the outside: CFG2 is written with
     * 0x84420000 and reads back 0x84020000, the transfer starts, TXTF
     * says the FIFO took the byte, and EOT never arrives because a slave
     * does not generate a clock. The panel stays dark and the firmware
     * hangs in a wait loop with every clock register looking perfect.
     */
    SPI3->CR1 = SPI_CR1_SSI;

    /*
     * 8-bit frames. FTHLV = 0 means the FIFO raises TXP with room for one
     * frame, which is what the byte-at-a-time loop below wants.
     */
    SPI3->CFG1 = (7UL << SPI_CFG1_DSIZE_Pos)
               | ((uint32_t)div << SPI_CFG1_MBR_Pos);

    /*
     * Master, simplex transmitter, software slave management.
     *
     * COMM = 01 is transmit-only: it stops the peripheral from clocking
     * data in and filling the receive FIFO with noise that nobody drains
     * - which, once the FIFO is full, stalls the transmitter and hangs
     * the loop below. A full-duplex configuration here would appear to
     * work for short writes and die on the first screen fill.
     *
     * AFCNTR keeps the pins driven while the peripheral is between
     * transfers, so SCK does not float around and clock the panel by
     * accident.
     *
     * Mode 0 (CPOL = CPHA = 0), which is what the ILI9488 expects.
     */
    SPI3->CFG2 = SPI_CFG2_MASTER
               | (1UL << SPI_CFG2_COMM_Pos)
               | SPI_CFG2_SSM
               | SPI_CFG2_AFCNTR;
}

uint32_t spi3_check(void)
{
    /*
     * Read CFG2 back and say whether master mode survived. Cheap, and it
     * turns the failure above from a silent hang into one printed line.
     * Anything that can be cleared by hardware behind your back deserves
     * a readback.
     */
    return SPI3->CFG2;
}

/* One run of up to 65535 frames. The peripheral counts them and will not
 * start the next run until TSIZE is written again. */
static void run_block(const uint8_t *data, uint32_t len,
                      const uint8_t *pattern, uint32_t pattern_len)
{
    SPI3->CR1 &= ~SPI_CR1_SPE;
    SPI3->IFCR = 0xFFFFFFFFU;
    SPI3->CR2  = len;
    SPI3->CR1 |= SPI_CR1_SPE;
    SPI3->CR1 |= SPI_CR1_CSTART;

    volatile uint8_t *txdr = (volatile uint8_t *)&SPI3->TXDR;

    if (data != NULL) {
        for (uint32_t i = 0; i < len; i++) {
            while (!(SPI3->SR & SPI_SR_TXP)) { }
            *txdr = data[i];
        }
    } else {
        uint32_t p = 0;
        for (uint32_t i = 0; i < len; i++) {
            while (!(SPI3->SR & SPI_SR_TXP)) { }
            *txdr = pattern[p];
            if (++p == pattern_len) { p = 0; }
        }
    }

    /* EOT, not TXC: EOT is the one that means the shift register is empty
     * and the last bit is on the wire. Dropping CS on TXC clips it. */
    while (!(SPI3->SR & SPI_SR_EOT)) { }

    SPI3->IFCR = 0xFFFFFFFFU;
    SPI3->CR1 &= ~SPI_CR1_SPE;
}

void spi3_write(const uint8_t *data, uint32_t len)
{
    dma_backstop();

    while (len > 0U) {
        uint32_t chunk = (len > 65535U) ? 65535U : len;
        run_block(data, chunk, NULL, 0U);
        data += chunk;
        len  -= chunk;
    }
}

void spi3_write_repeat(const uint8_t *pattern, uint32_t pattern_len,
                       uint32_t repeats)
{
    uint32_t total = pattern_len * repeats;

    dma_backstop();

    /*
     * Chunks have to land on a pattern boundary, or a three-byte pixel
     * gets split across two runs and every colour after it is shifted by
     * a byte. 65535 is not a multiple of 3, so round down to one.
     */
    uint32_t max_chunk = (65535U / pattern_len) * pattern_len;

    while (total > 0U) {
        uint32_t chunk = (total > max_chunk) ? max_chunk : total;
        run_block(NULL, chunk, pattern, pattern_len);
        total -= chunk;
    }
}

void spi3_read(uint8_t *data, uint32_t len)
{
    spi3_div_t saved = s_div;

    dma_backstop();

    SPI3->CR1 &= ~SPI_CR1_SPE;

    /* Full duplex, and slow: reads are the fragile direction. */
    SPI3->CFG2 = (SPI3->CFG2 & ~SPI_CFG2_COMM_Msk);
    SPI3->CFG1 = (SPI3->CFG1 & ~SPI_CFG1_MBR_Msk)
               | ((uint32_t)SPI3_DIV64 << SPI_CFG1_MBR_Pos);

    SPI3->IFCR = 0xFFFFFFFFU;
    SPI3->CR2  = len;
    SPI3->CR1 |= SPI_CR1_SPE;
    SPI3->CR1 |= SPI_CR1_CSTART;

    volatile uint8_t *txdr = (volatile uint8_t *)&SPI3->TXDR;
    volatile uint8_t *rxdr = (volatile uint8_t *)&SPI3->RXDR;

    for (uint32_t i = 0; i < len; i++) {
        while (!(SPI3->SR & SPI_SR_TXP)) { }
        *txdr = 0x00U;                      /* clock out anything */
        while (!(SPI3->SR & SPI_SR_RXP)) { }
        data[i] = *rxdr;
    }

    while (!(SPI3->SR & SPI_SR_EOT)) { }
    SPI3->IFCR = 0xFFFFFFFFU;
    SPI3->CR1 &= ~SPI_CR1_SPE;

    /* Back to transmit-only at the previous speed. */
    SPI3->CFG2 = (SPI3->CFG2 & ~SPI_CFG2_COMM_Msk)
               | (1UL << SPI_CFG2_COMM_Pos);
    SPI3->CFG1 = (SPI3->CFG1 & ~SPI_CFG1_MBR_Msk)
               | ((uint32_t)saved << SPI_CFG1_MBR_Pos);
    s_div = saved;
}

/* ================================================================== */
/* TRANSMIT BY DMA                                                    */
/* ================================================================== */

/*
 * DMA1 STREAM 2, AND WHY THAT ONE.
 *
 * sai1.c has streams 0 and 1. Stream 2 is the next one in the LOW half
 * of the status registers, so its six flags sit in LISR bits 16..21 -
 * beside the SAI's, but in a group of their own: every write to LIFCR in
 * either file clears only its own stream's bits, and neither reads the
 * other's. It is also the stream the factory firmware used for exactly
 * this - SPI3_TX on DMA1 stream 2, read out of this radio while ours was
 * running (sai1.c, the comment above dma_release_sai_requests()).
 *
 * DMAMUX1 channel N is DMA1 stream N, and 62 is SPI3_TX in ST's own
 * stm32h7xx_hal_dma.h (DMA_REQUEST_SPI3_TX).
 *
 * PRIORITY LOW, on purpose: the SAI's streams are "very high" and share
 * this controller's arbiter. The audio cannot wait; the panel can, and
 * the SPI FIFO gives it 4 us of slack per refill besides.
 */
#define DMA_STREAM      DMA1_Stream2
#define DMA_MUX         DMAMUX1_Channel2
#define DMA_MUX_CH      2U
#define DMA_SHIFT       16U         /* stream 2's group in LISR/LIFCR */
#define DMAREQ_SPI3_TX  62U

#define DMA_ALL_FLAGS   0x3DU       /* FEIF, DMEIF, TEIF, HTIF, TCIF */
#define DMA_FEIF        0x01U
#define DMA_DMEIF       0x04U
#define DMA_TEIF        0x08U

static int      s_dma_ready;
static int      s_dma_busy;
static uint32_t s_dma_len;
static uint32_t s_dma_t0;           /* DWT->CYCCNT at the start */
static uint32_t s_dma_failures;
static uint32_t s_dma_flags_seen;
static uint32_t s_dma_done;         /* transfers collected */
static uint32_t s_dma_fe;           /* ... of which with FEIF set */
static uint32_t s_dma_mux_released;

static void dma_stream_disable(DMA_Stream_TypeDef *st)
{
    st->CR &= ~DMA_SxCR_EN;

    /* EN falls only when the stream has finished the item it is on, and
     * a configuration written while it is still set is ignored without
     * a word. Bounded, like every wait in this port. */
    for (uint32_t n = 2000000U; n != 0U && (st->CR & DMA_SxCR_EN); n--) { }
}

static DMA_Stream_TypeDef *dma_stream_of_mux(uint32_t ch)
{
    uint32_t base = (ch < 8U) ? (uint32_t)DMA1_Stream0 : (uint32_t)DMA2_Stream0;
    uint32_t n    = (ch < 8U) ? ch : (ch - 8U);

    return (DMA_Stream_TypeDef *)(base + n * sizeof(DMA_Stream_TypeDef));
}

/*
 * NOBODY ELSE ON REQUEST 62.
 *
 * WORKING.md trap 16, owed since the SAI paid for it: a request line two
 * streams listen to is served by whichever gets there first, the flag is
 * a level, and each stream ends up with part of the data and no error.
 * For the panel that would be every other byte of a pixel block going
 * nowhere. On a cold boot every multiplexer channel is at reset and this
 * finds nothing; over SWD it finds whatever the previous image left, and
 * core_clean's DMA reset is the only other thing standing in the way.
 *
 * The same scan as sai1.c's dma_release_sai_requests(), about a
 * different request. Deliberately a copy rather than a shared helper:
 * that one is in the audio path, measured, and not worth touching to
 * save twenty lines.
 */
static void dma_release_requests(void)
{
    for (uint32_t ch = 0; ch < 16U; ch++) {
        if (ch == DMA_MUX_CH) { continue; }

        /* An unclocked DMA2 is running nothing, and reading its
         * registers to find that out is a bus fault. */
        if (ch >= 8U && !(RCC->AHB1ENR & RCC_AHB1ENR_DMA2EN)) { break; }

        DMAMUX_Channel_TypeDef *mux =
            (DMAMUX_Channel_TypeDef *)((uint32_t)DMAMUX1_Channel0
                                       + ch * sizeof(DMAMUX_Channel_TypeDef));

        if ((mux->CCR & DMAMUX_CxCR_DMAREQ_ID) != DMAREQ_SPI3_TX) { continue; }

        dma_stream_disable(dma_stream_of_mux(ch));
        mux->CCR = 0U;
        s_dma_mux_released |= (1UL << ch);
    }
}

int spi3_dma_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    (void)RCC->AHB1ENR;

    /* The timeout below counts cycles; nothing guarantees somebody else
     * switched the counter on first. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

    dma_stream_disable(DMA_STREAM);
    dma_release_requests();

    DMA1->LIFCR = DMA_ALL_FLAGS << DMA_SHIFT;

    /* The multiplexer before the stream is ever enabled: a stream whose
     * channel still says request 0 is a memory-to-memory stream, and
     * would run off through the buffer at once with no SPI involved. */
    DMA_MUX->CCR = DMAREQ_SPI3_TX;

    DMA_STREAM->PAR = (uint32_t)&SPI3->TXDR;

    /*
     * Direct mode (FCR = 0, and the reset value is 0x21, so it has to be
     * written), bytes at both ends - as sai1.c does, and as ST's own H7
     * SPI-with-DMA examples do. Each SPI request moves one byte from
     * memory to TXDR. 3.75 MB/s is nothing to the bus; a FIFO in the
     * stream would only add a second place for bytes to be when a
     * transfer is torn down.
     *
     * Bytes and not words because TXDR takes FRAMES: a 32-bit write
     * pushes four, and the cast in run_block() above exists for the same
     * reason. And a pixel block is three bytes a pixel, so its length is
     * not a multiple of four in general.
     */
    DMA_STREAM->FCR = 0U;
    DMA_STREAM->CR  = (1UL << DMA_SxCR_DIR_Pos)       /* memory -> SPI    */
                    | DMA_SxCR_MINC                   /* memory walks     */
                    | (0UL << DMA_SxCR_PSIZE_Pos)     /* byte             */
                    | (0UL << DMA_SxCR_MSIZE_Pos)     /* byte             */
                    | (0UL << DMA_SxCR_PL_Pos);       /* low priority     */

    s_dma_busy  = 0;
    s_dma_ready = 1;
    return 0;
}

uint32_t spi3_dma_mux_released(void) { return s_dma_mux_released; }
uint32_t spi3_dma_failures(void)     { return s_dma_failures; }
uint32_t spi3_dma_flags_seen(void)   { return s_dma_flags_seen; }

void spi3_dma_counts(uint32_t *done, uint32_t *fe)
{
    if (done != NULL) { *done = s_dma_done; }
    if (fe   != NULL) { *fe   = s_dma_fe; }
}
int      spi3_dma_busy(void)         { return s_dma_busy; }

/*
 * WHERE DMA1 CAN READ. Not the TCMs - they hang off the core, and a D2
 * master cannot see them. This image links everything into AXI SRAM, so
 * the check is for the day somebody moves a buffer to DTCM for speed;
 * D2 SRAM is allowed because it is the obvious place to move one to.
 */
static int dma_reachable(const uint8_t *p, uint32_t len)
{
    uint32_t a = (uint32_t)p, e = a + len;

    if (a >= 0x24000000U && e <= 0x24080000U) { return 1; }   /* AXI SRAM */
    if (a >= 0x30000000U && e <= 0x30048000U) { return 1; }   /* SRAM1-3  */
    return 0;
}

int spi3_dma_start(const uint8_t *data, uint32_t len, int eot_irq)
{
    if (!s_dma_ready || len == 0U || len > SPI3_DMA_MAX
        || !dma_reachable(data, len)) {
        return -1;
    }

    dma_backstop();

    /*
     * THE D-CACHE IS OFF IN THIS IMAGE (clock.c, startup), so this does
     * nothing today. It is here for the day it goes on: the CPU's last
     * writes to the buffer would otherwise still be in the cache, and
     * the DMA would send what was in RAM before them - the "works,
     * except sometimes" clock.c warns about. Cleaning is safe at any
     * alignment; it only ever writes back.
     */
    if (SCB->CCR & SCB_CCR_DC_Msk) {
        SCB_CleanDCache_by_Addr((void *)(uintptr_t)data, (int32_t)len);
    }

    /*
     * THE ORDER IS RM0433's, "Communication using DMA": the stream
     * first, then TXDMAEN, then SPE, then CSTART. TXDMAEN lives in CFG1,
     * which is locked while SPE is set, so the peripheral has to be off
     * when it is written - and it is, because run_block() and the
     * teardown below both leave it off.
     *
     * With SPE set the FIFO is empty and asks for data at once, so the
     * stream has filled it before CSTART lets the clock run.
     */
    SPI3->CR1 &= ~SPI_CR1_SPE;
    SPI3->IFCR = 0xFFFFFFFFU;
    SPI3->CR2  = len;

    dma_stream_disable(DMA_STREAM);     /* it cleared itself at TC */
    DMA1->LIFCR      = DMA_ALL_FLAGS << DMA_SHIFT;
    DMA_STREAM->M0AR = (uint32_t)data;
    DMA_STREAM->NDTR = len;

    /*
     * THE BLOCK'S BYTES BEFORE THE STREAM THAT READS THEM. The CPU wrote
     * them a moment ago through its write buffer, and the stream is
     * another bus master: nothing in the architecture promises it sees
     * those writes before the Device-memory write below starts it. A
     * DSB does. A few cycles, once per block.
     */
    __DSB();
    DMA_STREAM->CR  |= DMA_SxCR_EN;

    if (eot_irq) { SPI3->IER = SPI_IER_EOTIE; }

    SPI3->CFG1 |= SPI_CFG1_TXDMAEN;
    SPI3->CR1  |= SPI_CR1_SPE;
    SPI3->CR1  |= SPI_CR1_CSTART;

    s_dma_len  = len;
    s_dma_t0   = DWT->CYCCNT;
    s_dma_busy = 1;
    return 0;
}

int spi3_dma_wait(void)
{
    if (!s_dma_busy) { return 0; }

    /*
     * THE TIME IT SHOULD TAKE, TWICE, PLUS 10 ms. len bytes at the
     * current divider is len x 8 x (sysclk / sck) core cycles. Generous
     * because a false timeout throws away a good transfer AND switches
     * the DMA off for the rest of the run (ili9488.c); a real one is a
     * stream that stopped, and twice or ten times its time makes no
     * difference to that.
     */
    uint64_t want  = (uint64_t)s_dma_len * 8U
                   * ((uint64_t)clock_sysclk_hz / spi3_hz());
    uint64_t limit = 2U * want + ((uint64_t)clock_sysclk_hz / 100U);

    if (limit > 0xF0000000ULL) { limit = 0xF0000000ULL; }

    int      rc    = 0;
    uint32_t spins = 0U;

    for (;;) {
        /*
         * EOT, and only EOT, means done: the stream finishes (TCIF) with
         * up to sixteen bytes still in the SPI's FIFO, and raising CS
         * then clips the last pixels. Same rule as run_block().
         */
        if (SPI3->SR & SPI_SR_EOT) { break; }

        /*
         * TEIF stops the stream - the hardware clears EN - and the SPI
         * then waits for bytes that will never come, so it is the one
         * flag that ends the wait early. FEIF and DMEIF are recorded but
         * not acted on: neither stops a stream, and a transfer that ends
         * in EOT despite them delivered every byte.
         */
        uint32_t f = (DMA1->LISR >> DMA_SHIFT) & (DMA_TEIF | DMA_DMEIF | DMA_FEIF);

        if (f != 0U) {
            s_dma_flags_seen |= f;
            if (f & DMA_TEIF) { rc = -2; break; }
        }

        /*
         * Two bounds, because the first one stands on the cycle counter
         * and a debugger can switch that off (DEMCR.TRCENA) behind the
         * firmware's back. The spin count is seconds long: it is only
         * there so that a stopped counter AND a stopped stream together
         * still end.
         */
        if ((uint32_t)(DWT->CYCCNT - s_dma_t0) > (uint32_t)limit
            || ++spins > 100000000U) {
            /* An interrupt between the EOT test and this one can make a
             * finished transfer look late. Ask once more before calling
             * it a failure. */
            if (SPI3->SR & SPI_SR_EOT) { break; }
            rc = -1;
            break;
        }
    }

    /*
     * TEARDOWN, RM0433's order for closing: stream off, SPI off, then
     * TXDMAEN off - the same steps whether it finished or not.
     *
     * A transfer that did NOT finish is still running as far as the SPI
     * knows - TSIZE not reached - and clearing SPE in the middle of one
     * is not the documented way out of it. CSUSP is: it asks the master
     * to stop at the next frame boundary and says so in SUSP. Bounded,
     * because the case it handles is the one where something already
     * failed to finish.
     */
    dma_stream_disable(DMA_STREAM);

    /*
     * The flags once more, now that the stream has stopped: the loop
     * leaves at EOT without looking, and a flag raised on the way is
     * still worth knowing. FEIF is counted per transfer, because the
     * question it raises is "always, or now and then" - see
     * spi3_dma_counts().
     */
    {
        uint32_t f = (DMA1->LISR >> DMA_SHIFT) & (DMA_TEIF | DMA_DMEIF | DMA_FEIF);

        s_dma_flags_seen |= f;
        if (f & DMA_FEIF) { s_dma_fe++; }
        s_dma_done++;
    }

    if (rc != 0) {
        SPI3->CR1 |= SPI_CR1_CSUSP;
        for (uint32_t n = 100000U; n != 0U && !(SPI3->SR & SPI_SR_SUSP); n--) { }
        s_dma_failures++;
    }

    /* IER first: with EOT about to be cleared nothing should be able
     * to raise the interrupt again, but a handler that chains transfers
     * is better off not finding out. */
    SPI3->IER   = 0U;
    DMA1->LIFCR = DMA_ALL_FLAGS << DMA_SHIFT;
    SPI3->IFCR  = 0xFFFFFFFFU;
    SPI3->CR1  &= ~SPI_CR1_SPE;
    SPI3->CFG1 &= ~SPI_CFG1_TXDMAEN;

    s_dma_busy = 0;
    return rc;
}

/*
 * Every polled function above starts here. The panel driver collects its
 * own transfers before it touches the bus, because it owns CS and has to
 * raise it; this is for anything that does not, and it costs one load
 * and a branch when nothing is in flight.
 */
static void dma_backstop(void)
{
    if (s_dma_busy) { (void)spi3_dma_wait(); }
}
