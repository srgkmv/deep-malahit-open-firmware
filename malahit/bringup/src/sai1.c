#include <stddef.h>

#include "stm32h7xx.h"
#include "sai1.h"
#include "clock.h"
#include "board.h"

/*
 * Frame geometry. 2 slots of 16 bits = 32 bits a frame.
 *
 * FRL+1 MUST BE A POWER OF TWO when NODIV = 0, because the hardware
 * divides the 256 x Fs master clock down to the bit clock internally and
 * only exact powers of two come out right. 32 qualifies; a 24-bit frame
 * would not, which is one more reason 16-bit slots are the easy choice
 * here.
 */
#define FRAME_BITS      32U
#define SLOT_BITS       16U
#define NSLOT           2U

static sai1_clock_t s_clock;
static int      s_ready;
static uint32_t s_tx_under;
static uint32_t s_rx_over;
static uint32_t s_measured_fs;

/*
 * Receive timing, remembered across sample-rate changes.
 *
 * sai1_set_fs() rewrites block B's CR1 and FRCR from scratch, so
 * anything sai1_set_rx_timing() chose has to be reapplied there or the
 * rate sweep would silently run with different framing from the test
 * that established it.
 *
 * THE DEFAULT IS THE MEASURED ANSWER, CKSTR = 1, not RM0433's 0. It used
 * to be 0, "as RM0433 describes it", and the radio only worked because
 * main()'s test_rx_timing() measured the four combinations and applied
 * the winner - the radio's receive framing was a test's OUTPUT, which is
 * trap 17 in WORKING.md waiting for the day the test was skipped. It is
 * skipped now on every normal boot (SELFTEST in main.c), so the answer
 * lives here, where BOARD.md section 5 already put it ("CKSTR must be
 * SET on the receiving block - measured"): the test picks CKSTR=1
 * FSOFF=1 at a mean |sample| near 90, against ~16 000 for the frames
 * shifted by a bit. It still runs, and still overrides this, with
 * SELFTEST=1.
 */
static int s_rx_ckstr = 1;
static int s_rx_fsoff = 1;

/*
 * Whether the receiver's slot parity is known. Cleared by anything that
 * can have moved it: a rate change, a framing change, or an overrun.
 */
static int s_aligned;

/* Block status at the end of the last stream, for when a capture comes
 * up short and the question is why. */
static uint32_t s_last_sr_a, s_last_sr_b;

/*
 * How often the polled path has had to take DR back from the DMA
 * transport. Declared up here rather than with the rest of the DMA state
 * at the bottom of the file because sai1_stream() and sai1_play() are
 * the ones that count it, and they come first.
 */
static uint32_t s_mode_switch;

/*
 * SAIEN not holding is not a DMA problem - sai1_start() hits it too - so
 * the counter and the two helpers that deal with it are declared up here
 * and defined with the rest of the DMA section below.
 */
static uint32_t s_en_relatch;

static int  block_enable(SAI_Block_TypeDef *b);
static void spin_us(uint32_t us);

#define WAIT_LIMIT      2000000U

/* ------------------------------------------------------------------ */
/* The clock solver                                                   */
/* ------------------------------------------------------------------ */

void sai1_solve(uint32_t fs_hz, sai1_clock_t *out)
{
    uint64_t best_err = (uint64_t)-1;

    out->fs_target = fs_hz;
    out->ok        = 0;
    out->vco_hz    = 0;     /* read by the tie-break below */

    if (fs_hz == 0U) { return; }

    const uint64_t mclk_target = (uint64_t)fs_hz * 256U;

    /*
     * Brute force, 63 x 128 candidates. It runs once per rate change on
     * a 480 MHz core, so there is no reason to be clever - and a search
     * that simply tries everything cannot be wrong about which corner it
     * forgot.
     *
     * MCKDIV multiplies the kernel clock, P divides the VCO, so the two
     * together decide where in the legal VCO window the fractional
     * divider gets to work. Preferring the smallest frequency error and,
     * on a tie, the highest VCO is what squeezes the residual down to
     * about a part per million.
     */
    for (uint32_t mckdiv = 1U; mckdiv <= 63U; mckdiv++) {
        uint64_t ker = mclk_target * mckdiv;

        if (ker > SAI1_KER_MAX_HZ) { break; }

        for (uint32_t p = 1U; p <= 128U; p++) {
            uint64_t vco = ker * p;

            if (vco < SAI1_VCO_MIN_HZ) { continue; }
            if (vco > SAI1_VCO_MAX_HZ) { break; }

            /*
             * N + FRACN/8192 = vco / ref, rounded to the nearest step of
             * the fractional divider.
             */
            uint64_t n_x8192 = (vco * 8192U + SAI1_PLL2_REF_HZ / 2U)
                             / SAI1_PLL2_REF_HZ;
            uint32_t n     = (uint32_t)(n_x8192 / 8192U);
            uint32_t fracn = (uint32_t)(n_x8192 % 8192U);

            if (n < 4U || n > 512U) { continue; }

            /* What that actually produces, same arithmetic the hardware
             * does, so the printed numbers are the real ones. */
            uint64_t vco_got  = ((uint64_t)SAI1_PLL2_REF_HZ * n_x8192) / 8192U;

            if (vco_got < SAI1_VCO_MIN_HZ || vco_got > SAI1_VCO_MAX_HZ) {
                continue;
            }

            uint64_t ker_got  = vco_got / p;
            uint64_t mclk_got = ker_got / mckdiv;
            uint64_t err      = (mclk_got > mclk_target)
                              ? (mclk_got - mclk_target)
                              : (mclk_target - mclk_got);

            if (err < best_err
                || (err == best_err && vco_got > out->vco_hz)) {
                best_err      = err;
                out->divn     = n;
                out->fracn    = fracn;
                out->divp     = p;
                out->mckdiv   = mckdiv;
                out->vco_hz   = (uint32_t)vco_got;
                out->ker_hz   = (uint32_t)ker_got;
                out->mclk_hz  = (uint32_t)mclk_got;
                out->fs_hz    = (uint32_t)((mclk_got + 128U) / 256U);
                out->ok       = 1;
            }
        }
    }

    if (out->ok) {
        /*
         * Parts per BILLION, signed. In ppm every rate this solver finds
         * rounds to zero - 192 kHz is out by three hertz in forty-nine
         * million - and a column of zeroes cannot be told apart from a
         * column that was never computed.
         *
         * The scaling is done before the division and in 64 bits: d is a
         * handful of hertz and mclk_target is tens of millions, so doing
         * it the other way round gives zero every time.
         */
        int64_t d = (int64_t)out->mclk_hz - (int64_t)mclk_target;
        out->err_ppb = (int32_t)((d * 1000000000) / (int64_t)mclk_target);
    } else {
        out->err_ppb = 0;
    }
}

/* ------------------------------------------------------------------ */
/* PLL2                                                               */
/* ------------------------------------------------------------------ */

static int pll2_apply(const sai1_clock_t *c)
{
    uint32_t n;

    /*
     * Off first, and wait for it. PLL2 may well already be running -
     * the factory firmware drives this same codec - and its dividers
     * cannot be written while it is locked. Nothing of ours uses PLL2;
     * SPI3 and the panel are on pll1_q_ck.
     */
    RCC->CR &= ~RCC_CR_PLL2ON;
    n = WAIT_LIMIT;
    while (RCC->CR & RCC_CR_PLL2RDY) {
        if (--n == 0U) { return SAI1_ERR_PLL2; }
    }

    /* M2 only. DIVM1 belongs to the system PLL and must survive. */
    RCC->PLLCKSELR = (RCC->PLLCKSELR & ~RCC_PLLCKSELR_DIVM2_Msk)
                   | (SAI1_PLL2_DIVM << RCC_PLLCKSELR_DIVM2_Pos);

    /*
     * PLL2RGE = 11 says the reference is 8..16 MHz, which is where
     * 22/2 = 11 MHz sits. PLL2VCOSEL = 0 is the wide VCO range,
     * 192..960 MHz; the medium range tops out at 420 MHz and the solver
     * routinely picks higher than that.
     *
     * Only P is enabled. Q and R would be free running outputs nothing
     * reads, and leaving them off keeps the frequency the SAI gets from
     * depending on a field this code never sets.
     */
    uint32_t cfgr = RCC->PLLCFGR;
    cfgr &= ~(RCC_PLLCFGR_PLL2RGE_Msk | RCC_PLLCFGR_PLL2VCOSEL_Msk
            | RCC_PLLCFGR_PLL2FRACEN_Msk
            | RCC_PLLCFGR_DIVQ2EN_Msk | RCC_PLLCFGR_DIVR2EN_Msk);
    cfgr |= (3UL << RCC_PLLCFGR_PLL2RGE_Pos) | RCC_PLLCFGR_DIVP2EN;
    RCC->PLLCFGR = cfgr;

    /*
     * Every field in PLL2DIVR is "value minus one". P is 7 bits, so the
     * solver's 1..128 fits exactly.
     *
     * ODD VALUES OF P ARE LEGAL HERE and are not on PLL1. The reference
     * manual forbids odd DIVP1 and says nothing of the kind about DIVP2
     * or DIVP3, and the solver uses that freedom - 19 is a common
     * answer. If it turns out to be wrong on this silicon the symptom
     * will not be subtle and it will not be silent: sai1_measured_fs()
     * compares the frame rate against the CPU clock and would report
     * roughly double the intended rate.
     */
    RCC->PLL2DIVR = ((c->divn - 1U) << RCC_PLL2DIVR_N2_Pos)
                  | ((c->divp - 1U) << RCC_PLL2DIVR_P2_Pos);

    RCC->PLL2FRACR = (c->fracn << RCC_PLL2FRACR_FRACN2_Pos);

    /*
     * FRACEN goes on AFTER FRACN is written. The reference manual's
     * procedure for changing the fraction is clear FRACEN, write FRACN,
     * set FRACEN - the latch happens on the rising edge of the enable,
     * so arming it first would load whatever was in the register before.
     *
     * When the fraction came out exactly zero, leave it off: an integer
     * PLL has less jitter than a fractional one sitting at zero, and on
     * this radio MCLK jitter is the receiver's noise floor.
     */
    if (c->fracn != 0U) {
        RCC->PLLCFGR |= RCC_PLLCFGR_PLL2FRACEN;
    }

    RCC->CR |= RCC_CR_PLL2ON;
    n = WAIT_LIMIT;
    while (!(RCC->CR & RCC_CR_PLL2RDY)) {
        if (--n == 0U) { return SAI1_ERR_PLL2; }
    }

    /*
     * SAY WHICH KERNEL CLOCK, DO NOT INHERIT ONE. SPI3 taught this the
     * expensive way: the factory firmware had left SPI123SEL on per_ck
     * and the panel bus ran at 64/240 of its expected speed, scaling
     * perfectly with every divider and uniformly 3.75x wrong.
     *
     * 001 = pll2_p_ck.
     */
    RCC->D2CCIP1R = (RCC->D2CCIP1R & ~RCC_D2CCIP1R_SAI1SEL_Msk)
                  | (1UL << RCC_D2CCIP1R_SAI1SEL_Pos);

    return SAI1_OK;
}

/* ------------------------------------------------------------------ */
/* Pins and blocks                                                    */
/* ------------------------------------------------------------------ */

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

static void blocks_configure(uint32_t mckdiv)
{
    /*
     * Both blocks off before anything else is written. Most of CR1 and
     * all of FRCR and SLOTR are "configure while disabled" fields, and
     * an image loaded over SWD arrives with whatever the factory
     * firmware left running.
     */
    SAI1_Block_A->CR1 &= ~SAI_xCR1_SAIEN;
    SAI1_Block_B->CR1 &= ~SAI_xCR1_SAIEN;
    for (uint32_t n = WAIT_LIMIT;
         n != 0U && ((SAI1_Block_A->CR1 | SAI1_Block_B->CR1) & SAI_xCR1_SAIEN);
         n--) { }

    /*
     * BLOCK A - MASTER TRANSMITTER, and the owner of the clocks.
     *
     *   MODE   = 00  master transmitter
     *   PRTCFG = 00  free protocol; the I2S "protocol" presets are for
     *                SPDIF and AC97, not for I2S
     *   DS     = 100 16-bit
     *   CKSTR  = 0   data changes on the falling edge of SCK, which is
     *                what I2S means by its clock
     *   SYNCEN = 00  asynchronous - it IS the reference
     *   MCKEN  = 1   drive MCLK on PE2. New on the H7 and easy to miss;
     *                without it PE2 stays quiet and the codec has no
     *                master clock at all, which looks exactly like a
     *                dead codec.
     *   NODIV  = 0   MCLK = 256 x Fs, the whole point
     *   OSR    = 0   256, not 512
     */
    SAI1_Block_A->CR1 =
          (0UL << SAI_xCR1_MODE_Pos)
        | (0UL << SAI_xCR1_PRTCFG_Pos)
        | (4UL << SAI_xCR1_DS_Pos)
        | (0UL << SAI_xCR1_SYNCEN_Pos)
        | SAI_xCR1_MCKEN
        | (mckdiv << SAI_xCR1_MCKDIV_Pos);

    /*
     * I2S framing: FS is a 50 % duty "which channel" signal that falls
     * one bit clock BEFORE the first data bit.
     *
     *   FRL   = 31  32 bits a frame
     *   FSALL = 15  FS active for half the frame - channel identification
     *   FSDEF = 1   FS marks start of frame AND channel side
     *   FSPOL = 0   active low: left channel is the low half
     *   FSOFF = 1   FS asserted one bit before slot 0
     *
     * FSOFF is the bit that separates I2S from left-justified, and
     * getting it wrong costs one bit of shift - which sounds like a
     * quiet, slightly distorted version of the right thing rather than
     * like a failure.
     */
    SAI1_Block_A->FRCR =
          ((FRAME_BITS - 1U) << SAI_xFRCR_FRL_Pos)
        | (((FRAME_BITS / 2U) - 1U) << SAI_xFRCR_FSALL_Pos)
        | SAI_xFRCR_FSDEF
        | SAI_xFRCR_FSOFF;

    /*
     * Two slots, slot size tied to the data size, both slots active.
     * FBOFF = 0: the first data bit sits at the start of the slot.
     */
    SAI1_Block_A->SLOTR =
          (0UL << SAI_xSLOTR_FBOFF_Pos)
        | (0UL << SAI_xSLOTR_SLOTSZ_Pos)
        | ((NSLOT - 1U) << SAI_xSLOTR_NBSLOT_Pos)
        | (0x3UL << SAI_xSLOTR_SLOTEN_Pos);

    /* FTH = 001, a quarter full. The polled loop looks at FLVL rather
     * than at FREQ, so this only sets where the flag would move. */
    SAI1_Block_A->CR2 = (1UL << SAI_xCR2_FTH_Pos);

    /*
     * BLOCK B - SLAVE RECEIVER, SYNCHRONOUS WITH A.
     *
     *   MODE   = 11  slave receiver
     *   SYNCEN = 01  synchronous with the other block of this same SAI,
     *                so it borrows A's SCK and FS and PE3 carries data
     *                only. No SAI_GCR involved; that register is for
     *                synchronising one SAI to a different one.
     *
     * Its framing and slots still have to be written: a synchronous
     * slave takes the clocks from A but decodes the frame with its own
     * registers, and a mismatch there reads the right bits at the wrong
     * time.
     *
     * MCKDIV, MCKEN and NODIV are meaningless in slave mode and are left
     * out rather than copied, so nothing suggests block B has a say in
     * the clocks.
     */
    SAI1_Block_B->CR1 =
          (3UL << SAI_xCR1_MODE_Pos)
        | (0UL << SAI_xCR1_PRTCFG_Pos)
        | (4UL << SAI_xCR1_DS_Pos)
        | (1UL << SAI_xCR1_SYNCEN_Pos)
        | (s_rx_ckstr ? SAI_xCR1_CKSTR : 0U);

    /* A's framing, with the receive-side offset that was measured to be
     * right rather than the one that was assumed - see
     * sai1_set_rx_timing(). */
    SAI1_Block_B->FRCR = (SAI1_Block_A->FRCR & ~SAI_xFRCR_FSOFF)
                       | (s_rx_fsoff ? SAI_xFRCR_FSOFF : 0U);
    SAI1_Block_B->SLOTR = SAI1_Block_A->SLOTR;
    SAI1_Block_B->CR2 = (1UL << SAI_xCR2_FTH_Pos);

    SAI1_Block_A->CLRFR = 0xFFFFFFFFU;
    SAI1_Block_B->CLRFR = 0xFFFFFFFFU;
}

int sai1_init(void)
{
    RCC->AHB4ENR  |= RCC_AHB4ENR_GPIOEEN;
    RCC->APB2ENR  |= RCC_APB2ENR_SAI1EN;
    (void)RCC->APB2ENR;

    gpio_af(GPIOE, 2U, 6U);    /* SAI1_CK1   - MCLK out */
    gpio_af(GPIOE, 3U, 6U);    /* SAI1_SD_B  - ADCOUT in */
    gpio_af(GPIOE, 4U, 6U);    /* SAI1_FS_A  - FS out */
    gpio_af(GPIOE, 5U, 6U);    /* SAI1_SCK_A - BCLK out */
    gpio_af(GPIOE, 6U, 6U);    /* SAI1_SD_A  - DACIN out */

    s_ready = 1;
    return SAI1_OK;
}

int sai1_set_fs(uint32_t fs_hz)
{
    sai1_clock_t c;
    int rc;

    if (!s_ready) { return SAI1_ERR_NOT_READY; }

    sai1_solve(fs_hz, &c);
    if (!c.ok) { return SAI1_ERR_NO_CLOCK; }

    sai1_stop();

    rc = pll2_apply(&c);
    if (rc != SAI1_OK) { return rc; }

    blocks_configure(c.mckdiv);

    s_clock   = c;
    s_aligned = 0;
    return SAI1_OK;
}

const sai1_clock_t *sai1_clock(void)
{
    return &s_clock;
}

void sai1_set_rx_timing(int ckstr, int fsoff)
{
    sai1_stop();

    SAI1_Block_B->CR1 = (SAI1_Block_B->CR1 & ~SAI_xCR1_CKSTR)
                      | (ckstr ? SAI_xCR1_CKSTR : 0U);

    SAI1_Block_B->FRCR = (SAI1_Block_B->FRCR & ~SAI_xFRCR_FSOFF)
                       | (fsoff ? SAI_xFRCR_FSOFF : 0U);

    s_rx_ckstr = ckstr;
    s_rx_fsoff = fsoff;
    s_aligned  = 0;
}

/*
 * THE POLLED START, AND IT NEEDS THE SAME VERIFICATION THE DMA ARM DOES.
 *
 * SAIEN DOES NOT ALWAYS HOLD, and this is not a DMA problem - it was in
 * this function from the beginning and nobody had looked. Measured on
 * the radio after a cold power cycle: three polled captures in one run
 * returned ZERO frames, and the first of them was in test 0, long before
 * anything in this file had armed a DMA stream. What it looks like from
 * outside:
 *
 *     *** capture got 0 of 8192 frames; SR_A 0x00040000 SR_B 0x00000000
 *
 * SR_A's FLVL says the transmit FIFO is three-quarters full and SR_B's
 * says the receive FIFO is empty, which is the signature of blocks that
 * are not running: the polled loop will not write while the FIFO is that
 * full and cannot read from one that never fills, so it spins out its
 * guard and returns nothing.
 *
 * The cause is the one blocks_enable() documents at length - a disable
 * that has not finished taking SAIEN down with the new one - and the
 * pattern that triggers it is "stop, then start again immediately",
 * which is exactly what sai1_set_rx_timing() and sai1_stream()'s
 * re-alignment do.
 *
 * So verify here too. The reason it went unnoticed for so long is worth
 * keeping: capture() ALREADY printed the warning above every time it
 * happened, and the only thing that was missing was somebody checking
 * the return value of the thing that fills the buffer.
 */
void sai1_start(void)
{
    for (uint32_t attempt = 0; attempt < 8U; attempt++) {
        SAI1_Block_A->CLRFR = 0xFFFFFFFFU;
        SAI1_Block_B->CLRFR = 0xFFFFFFFFU;

        /*
         * The transmit FIFO is primed with silence before block A
         * starts, because a master transmitter whose FIFO is empty
         * underruns on its very first frame. That costs nothing but it
         * does set a flag, and a flag that is always set is a flag that
         * cannot be used as evidence later.
         */
        for (uint32_t i = 0; i < 4U; i++) { SAI1_Block_A->DR = 0U; }

        /*
         * THE SLAVE GOES FIRST. A synchronous slave has to be enabled
         * before the block that generates its clocks, or the first
         * frames arrive at a receiver that is not listening yet and the
         * two ends disagree about which half of the frame is the left
         * channel from then on.
         */
        if (block_enable(SAI1_Block_B) != 0) { return; }
        if (block_enable(SAI1_Block_A) != 0) { return; }

        spin_us(200U);

        if ((SAI1_Block_A->CR1 & SAI_xCR1_SAIEN)
            && (SAI1_Block_B->CR1 & SAI_xCR1_SAIEN)) {
            return;
        }

        s_en_relatch++;

        /*
         * Unwind and try again - and FLUSH, because the primed words and
         * whatever the receiver managed to take are now of unknown
         * count, and an odd number left in the receive FIFO exchanges I
         * and Q for the whole capture that follows.
         */
        sai1_stop();
        SAI1_Block_A->CR2 |= SAI_xCR2_FFLUSH;
        SAI1_Block_B->CR2 |= SAI_xCR2_FFLUSH;
        spin_us(400U);
    }
}

void sai1_stop(void)
{
    SAI1_Block_A->CR1 &= ~SAI_xCR1_SAIEN;
    SAI1_Block_B->CR1 &= ~SAI_xCR1_SAIEN;

    for (uint32_t n = WAIT_LIMIT;
         n != 0U && ((SAI1_Block_A->CR1 | SAI1_Block_B->CR1) & SAI_xCR1_SAIEN);
         n--) { }
}

/*
 * FLVL IS A BAND, NOT A COUNT, AND THAT MATTERS HERE.
 *
 * The FIFO is eight words deep and SR.FLVL reports which quarter it is
 * in: 000 empty, 001 "not empty but no more than a quarter" (so 1 OR 2
 * words), 010 up to a half (3 or 4), 011 up to three quarters (5 or 6),
 * 100 above three quarters but not full (7), 101 full.
 *
 * This code moves a whole stereo FRAME at a time - two words - so the
 * obvious tests are both wrong. "FLVL != empty" can mean a single word
 * is waiting, and the second read then returns nothing real. "FLVL !=
 * full" can mean room for exactly one, and the second write is dropped.
 * Either way left and right swap for the rest of the capture, and since
 * the two channels are I and Q a swap mirrors the spectrum instead of
 * breaking it - a bug that survives a long time.
 *
 * So: read only at 010 or above, which guarantees three words present,
 * and write only at 011 or below, which guarantees room for two.
 */
#define RX_HAS_FRAME    (2UL << SAI_xSR_FLVL_Pos)
#define TX_HAS_ROOM     (3UL << SAI_xSR_FLVL_Pos)

uint32_t sai1_stream(const int16_t *tone, uint32_t tone_frames,
                     int16_t *rx, uint32_t rx_frames)
{
    uint32_t tx_pos = 0;
    uint32_t got    = 0;
    uint64_t t0, t1;

    /*
     * ONE TRANSPORT AT A TIME. See sai1.h: a CPU write into a FIFO that
     * a DMA stream is also filling shifts the slots by one and they stay
     * shifted, so the polled path takes DR back rather than sharing it.
     * Counted, because a run that keeps swapping transports is a caller
     * bug and silence would hide it.
     */
    if (sai1_dma_running()) { s_mode_switch++; sai1_dma_stop(); }

    s_tx_under    = 0;
    s_rx_over     = 0;
    s_measured_fs = 0;

    /*
     * ALIGN ONCE, NOT ONCE PER CALL.
     *
     * Frame alignment has to be established, not assumed: draining a
     * running receive FIFO leaves the next arriving word as whichever
     * slot the stream had reached, so a drain that finished mid-frame
     * hands back the right channel as the left one, for the whole
     * capture, with nothing to show that it did. Stopping both blocks,
     * flushing the FIFOs and starting again fixes the phase in
     * hardware - a synchronous slave receiver begins at the frame A
     * generates, so the first word out is slot 0 by construction.
     *
     * Doing that on EVERY call was a mistake, and an expensive one. It
     * is slow, and it turned out to be unreliable: captures began
     * intermittently returning nothing at all, which produced two
     * register settings with byte-identical statistics and no hint why.
     * Once alignment exists it survives, because the SAI keeps clocking
     * and the slots keep alternating; the only thing that can break the
     * parity is losing an odd number of words, which means an overrun.
     *
     * So: align when there is no alignment, and give it up only when an
     * overrun says the count may have slipped.
     *
     * MCLK survives all of it. MCKEN makes the H7 generate the master
     * clock independently of SAIEN, and it is set in CR1 and never
     * cleared, so the codec's own clock never stops.
     */
    if (!s_aligned) {
        sai1_stop();
        SAI1_Block_A->CR2 |= SAI_xCR2_FFLUSH;
        SAI1_Block_B->CR2 |= SAI_xCR2_FFLUSH;
        sai1_start();
        s_aligned = 1;
    }

    /*
     * Bounded, like every other wait in this project. The budget is
     * generous - a frame takes thousands of CPU cycles and this allows
     * tens of thousands of loop passes per frame - but it is finite, so
     * a stopped frame clock returns a short count instead of hanging a
     * bring-up image with no way to ask it what happened.
     */
    uint32_t warm  = 128U;
    uint32_t guard = (rx_frames + warm + 64U) * 20000U;

    /*
     * Throw the first frames away. The converters have just seen their
     * frame clock restart, and at the start of a sweep MCLK has only
     * just changed rate; the first samples out of a sigma-delta
     * modulator in that state are not a measurement of anything. They
     * are also outside the timing window, so they cannot skew the rate.
     */
    while (warm > 0U) {
        /* Same reason as in sai1_play(): the guard is counted in loop
         * passes and spent in APB reads, so it is not a time limit and
         * cannot protect the watchdog on its own. */
        if ((guard & 0xFFFFU) == 0U) { board_watchdog_feed(); }

        if (--guard == 0U) {
            s_aligned   = 0;
            s_last_sr_a = SAI1_Block_A->SR;
            s_last_sr_b = SAI1_Block_B->SR;
            return 0U;
        }

        if ((SAI1_Block_A->SR & SAI_xSR_FLVL_Msk) <= TX_HAS_ROOM) {
            SAI1_Block_A->DR = 0U;
            SAI1_Block_A->DR = 0U;
        }
        if ((SAI1_Block_B->SR & SAI_xSR_FLVL_Msk) >= RX_HAS_FRAME) {
            (void)SAI1_Block_B->DR;
            (void)SAI1_Block_B->DR;
            warm--;
        }
    }

    SAI1_Block_A->CLRFR = 0xFFFFFFFFU;
    SAI1_Block_B->CLRFR = 0xFFFFFFFFU;

    t0 = board_cycles();

    while (got < rx_frames) {
        if ((guard & 0xFFFFU) == 0U) { board_watchdog_feed(); }

        if (--guard == 0U) { break; }

        if ((SAI1_Block_A->SR & SAI_xSR_FLVL_Msk) <= TX_HAS_ROOM) {
            int16_t l = 0, r = 0;
            if (tone != NULL && tone_frames != 0U) {
                l = tone[tx_pos * 2U];
                r = tone[tx_pos * 2U + 1U];
                if (++tx_pos >= tone_frames) { tx_pos = 0; }
            }
            SAI1_Block_A->DR = (uint32_t)(uint16_t)l;
            SAI1_Block_A->DR = (uint32_t)(uint16_t)r;
        }

        if ((SAI1_Block_B->SR & SAI_xSR_FLVL_Msk) >= RX_HAS_FRAME) {
            int16_t l = (int16_t)(uint16_t)SAI1_Block_B->DR;
            int16_t r = (int16_t)(uint16_t)SAI1_Block_B->DR;
            if (rx != NULL) {
                rx[got * 2U]      = l;
                rx[got * 2U + 1U] = r;
            }
            got++;
        }

        if (SAI1_Block_A->SR & SAI_xSR_OVRUDR) {
            s_tx_under++;
            SAI1_Block_A->CLRFR = SAI_xCLRFR_COVRUDR;
        }
        if (SAI1_Block_B->SR & SAI_xSR_OVRUDR) {
            s_rx_over++;
            SAI1_Block_B->CLRFR = SAI_xCLRFR_COVRUDR;
        }
    }

    t1 = board_cycles();

    s_last_sr_a = SAI1_Block_A->SR;
    s_last_sr_b = SAI1_Block_B->SR;

    /*
     * An overrun means an unknown number of words went missing, so the
     * slot parity is no longer trustworthy and the next call has to
     * re-establish it. A short capture means the clocks stopped, which
     * invalidates the same thing.
     */
    if (s_rx_over != 0U || got < rx_frames) { s_aligned = 0; }

    /*
     * The frame rate the hardware actually produced, measured against
     * the CPU clock. This is the number that matters: everything else in
     * this file is arithmetic about what should happen.
     */
    if (t1 > t0 && got > 0U) {
        s_measured_fs = (uint32_t)(((uint64_t)got * clock_sysclk_hz)
                                   / (t1 - t0));
    }

    return got;
}

void sai1_play(const int16_t *tone, uint32_t tone_frames, uint32_t frames)
{
    static uint32_t pos;

    if (tone == NULL || tone_frames == 0U) { return; }
    if (pos >= tone_frames) { pos = 0; }

    /* Same exclusion as sai1_stream(), same reason. */
    if (sai1_dma_running()) { s_mode_switch++; sai1_dma_stop(); }

    /*
     * AND THEN THE BLOCKS HAVE TO BE RUNNING AGAIN, WHICH AFTER A DMA
     * STOP THEY ARE NOT. THIS SWITCHED THE RADIO OFF.
     *
     * sai1_dma_stop() clears SAIEN on both blocks - it has to, the
     * streams and the FIFOs are being taken apart - so a polled play
     * straight after a DMA phase polls a peripheral that is standing
     * still: FLVL never changes, the loop below never moves a frame, and
     * it spins out its whole guard. Each pass reads two APB registers at
     * 120 MHz from a 480 MHz core, so the guard is not the microseconds
     * it looks like on paper: ONE call took about 46 seconds, the
     * watchdog can only be stretched to 32, and the radio **switched
     * itself off** - PC6 low, 3.3V_ON gone, debug port dead, exactly the
     * failure WORKING.md describes under "if the radio switches itself
     * off".
     *
     * Nothing caught it earlier because the A/B test plays polled FIRST,
     * while the blocks are still running from main()'s own sai1_start().
     * Polled-after-DMA was the untested order, and the listening test
     * was the first thing to ask for it.
     *
     * sai1_stream() never had the bug: it re-aligns when s_aligned is
     * clear, and sai1_dma_stop() clears it, so it stops and starts the
     * blocks itself. This does the same, and reads SAIEN rather than
     * assuming - the enable does not always hold, see sai1_start().
     */
    if (!(SAI1_Block_A->CR1 & SAI_xCR1_SAIEN)
        || !(SAI1_Block_B->CR1 & SAI_xCR1_SAIEN)) {
        sai1_stop();
        SAI1_Block_A->CR2 |= SAI_xCR2_FFLUSH;
        SAI1_Block_B->CR2 |= SAI_xCR2_FFLUSH;
        sai1_start();
    }

    s_tx_under = 0;

    uint32_t guard = (frames + 64U) * 20000U;

    while (frames > 0U) {
        /*
         * FEED THE WATCHDOG FROM INSIDE THE LOOP. A stall here must cost
         * a silence of the wrong length, never a dark radio - which is
         * the same conclusion board_delay_ms() reached for the same
         * reason, and the guard above is no protection on its own
         * because it is counted in loop passes and spent in APB reads.
         */
        if ((guard & 0xFFFFU) == 0U) { board_watchdog_feed(); }

        if (--guard == 0U) { break; }

        if ((SAI1_Block_A->SR & SAI_xSR_FLVL_Msk) <= TX_HAS_ROOM) {
            SAI1_Block_A->DR = (uint32_t)(uint16_t)tone[pos * 2U];
            SAI1_Block_A->DR = (uint32_t)(uint16_t)tone[pos * 2U + 1U];
            if (++pos >= tone_frames) { pos = 0; }
            frames--;
        }

        /*
         * COUNT THE UNDERRUNS WHILE PLAYING, or there is no way to tell
         * a tone that is generated badly from a tone that is fed too
         * slowly. They sound the same.
         *
         * The transmit FIFO is eight words - four stereo frames, which
         * at 48 kHz is 83 microseconds. Anything that blocks this loop
         * for longer than that is an audible click, and a console line
         * at 115200 baud takes thirty times longer. That is not a fault
         * to be fixed by polling harder; it is the reason the real
         * driver will use DMA.
         */
        if (SAI1_Block_A->SR & SAI_xSR_OVRUDR) {
            s_tx_under++;
            SAI1_Block_A->CLRFR = SAI_xCLRFR_COVRUDR;
        }

        /*
         * The receiver is running whether anybody wants its data or
         * not - it is synchronous with the transmitter and cannot be
         * stopped separately without stopping the clocks. Drain it, or
         * its FIFO fills and OVRUDR latches, and the overrun counter
         * stops meaning anything the next time it is read.
         */
        if ((SAI1_Block_B->SR & SAI_xSR_FLVL_Msk) >= RX_HAS_FRAME) {
            (void)SAI1_Block_B->DR;
            (void)SAI1_Block_B->DR;
        }
    }
}

uint32_t sai1_tx_underruns(void) { return s_tx_under; }
uint32_t sai1_rx_overruns(void)  { return s_rx_over; }
uint32_t sai1_measured_fs(void)  { return s_measured_fs; }
uint32_t sai1_sr_a(void)         { return s_last_sr_a; }
uint32_t sai1_sr_b(void)         { return s_last_sr_b; }

/* ================================================================== */
/* THE DMA TRANSPORT                                                  */
/* ================================================================== */

/*
 * DMA1 stream 0 carries the receiver and stream 1 the transmitter, which
 * puts both in the LOW half of the status registers - one LISR read sees
 * every flag of both. Streams 4..7 live in HISR, and a driver that
 * straddles the two ends up with two of every line.
 *
 * The request numbers are the DMAMUX1 inputs, not a channel selector:
 * the H7 replaced the F4's per-stream CHSEL field with a 1-of-128
 * multiplexer, and a stream left at request 0 is a memory-to-memory
 * stream that will never see a peripheral. 87 and 88 are SAI1_A and
 * SAI1_B, taken from ST's own stm32h7xx_hal_dma.h rather than counted
 * off the reference manual's table by hand.
 *
 * DMAMUX1 channel N is DMA1 stream N; DMA2's streams are channels 8..15.
 */
#define RX_STREAM       DMA1_Stream0
#define TX_STREAM       DMA1_Stream1
#define RX_MUX          DMAMUX1_Channel0
#define TX_MUX          DMAMUX1_Channel1
#define RX_FLAG_SHIFT   0U
#define TX_FLAG_SHIFT   6U

#define DMAREQ_SAI1_A   87U
#define DMAREQ_SAI1_B   88U

/* FEIF, DMEIF, TEIF, HTIF, TCIF of one stream: the six bits of its
 * group, cleared together. */
#define DMA_ALL_FLAGS   0x3DU
/* The three that are faults rather than progress. */
#define DMA_ERR_FLAGS   0x0DU

/*
 * THE RINGS.
 *
 * Two halves each, interleaved L/R, in .bss - which this image links
 * into AXI SRAM, where DMA1 can reach them. 64 kB for the receiver and
 * 8 kB for the transmitter, out of 512 kB.
 *
 * Not static-const-sized by the caller's request: a DMA buffer whose
 * size is decided at run time has to be allocated somewhere, and this
 * image deliberately has no heap. The maxima are the geometry; the
 * armed size is how much of it is used.
 */
static sai1_block_hook_t s_block_hook;
static volatile uint8_t  s_in_hook;

volatile uint32_t sai1_blocks;
volatile uint32_t sai1_block_overruns;

void sai1_set_block_hook(sai1_block_hook_t fn)
{
    /*
     * Written with the interrupt masked, because this is the one field
     * the handler reads on every block and a half-written function
     * pointer is a branch to nowhere. Cheap: it is two instructions
     * under a mask that nothing else is waiting on.
     */
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    s_block_hook = fn;
    __set_PRIMASK(primask);
}

/*
 * IN D2 SRAM SINCE 21/11/2026, not in .bss: 128 kB that AXI SRAM could
 * no longer spare once upstream's menus and demodulator were in the RAM
 * image (484 kB of 512). DMA1 lives in D2, so this is the shorter path
 * for it anyway. The linker scripts place .d2sram; rings_clock_on()
 * below is why the first touch is legal.
 */
#define RING __attribute__((section(".d2sram"), aligned(32)))
static int16_t  s_rx_ring[2U * SAI1_RX_HALF_MAX * 2U] RING;
static int16_t  s_tx_ring[2U * SAI1_TX_HALF_MAX * 2U] RING;

/*
 * SRAM1-3 come out of reset with their clocks OFF, and boot.c's power-off
 * turns them off again (D2 cannot enter standby with an enable bit set).
 * An access to an unclocked SRAM is a bus fault for the core and a
 * transfer error for the DMA, so this goes before the first one. Read
 * back for the same reason as every enable in this port: the posted
 * write must land before the next access.
 */
static void rings_clock_on(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_SRAM1EN | RCC_AHB2ENR_SRAM2EN
                  | RCC_AHB2ENR_SRAM3EN;
    (void)RCC->AHB2ENR;
}

static uint32_t s_rx_half;          /* frames per half, as armed */
static uint32_t s_tx_half;
static int      s_dma_on;
static uint32_t s_armed_cyc;    /* DWT->CYCCNT when the streams started */
static int      s_rx_primed;    /* a whole half has filled since then   */

static uint32_t s_dma_err;
static uint32_t s_dma_err_bits;
static sai1_arm_t s_arm;
static uint32_t s_rx_torn;
static uint32_t s_len_bad;

static uint32_t s_tx_repeat;
static uint32_t s_tx_min = 0xFFFFFFFFU;
static uint32_t s_tx_max;
static uint32_t s_tx_last = 2U;     /* 2 = no half written yet */

/*
 * WE ARE NOT THE ONLY ONES WIRED TO THIS PERIPHERAL.
 *
 * THIS COST THE FIRST DMA RUN AND IT IS THE SAME LESSON AS EVERY OTHER
 * EXPENSIVE ONE IN THIS PROJECT, ONE LEVEL FURTHER OUT: loading over SWD
 * resets nothing - and a DMA controller is not even stopped by halting
 * the core. It is a separate master. It keeps going.
 *
 * Read out of this radio while our own image was running:
 *
 *   DMAMUX1 ch9  = 87 (SAI1_A)  ->  DMA2 stream 1, EN=1, mem->periph,
 *                                   double buffered, M0AR 0x30023de0
 *   DMAMUX1 ch13 = 88 (SAI1_B)  ->  DMA2 stream 5, EN=1, periph->mem,
 *                                   double buffered, M0AR 0x30027de0
 *
 * That is the FACTORY FIRMWARE's audio transport, still armed, still
 * enabled, pointed at its own buffers in D2 SRAM. It has been running
 * through every run of this bring-up. The only reason it has never
 * corrupted anything is that this image lives entirely in AXI SRAM at
 * 0x24000000 and those buffers are at 0x30000000 - which is luck, not
 * design.
 *
 * It did no harm while the SAI was polled, because DMAEN was clear in
 * both blocks and a SAI that makes no requests feeds no stream. The
 * moment this file set DMAEN the request lines came alive and TWO
 * channels were listening to each of them. The flag is a level and a
 * transfer by either stream clears it, so the two streams took alternate
 * words: our receiver got EXACTLY HALF the stream, consecutive samples
 * of one channel ended up looking like an I/Q pair, and the spectrum
 * came out at twice the right offset with an image almost as strong as
 * the carrier. No overrun, no error flag, nothing to point at.
 *
 * So before arming anything, take the request lines back: walk all
 * sixteen DMAMUX1 channels, and any that names one of our two requests
 * and is not one of ours gets its stream disabled and its channel
 * unwired. The rule is deliberately about the REQUEST and not about
 * "DMA2 streams 1 and 5", which is this unit's accident rather than a
 * property of the part.
 *
 * The same scan was owed to SPI3_TX (request 62) the day the panel moved
 * to DMA - the factory firmware has that one configured too, on DMA1
 * stream 2 - and spi3.c's dma_release_requests() is it, 21/11/2026.
 */
static uint32_t s_mux_released;     /* bitmask of channels taken back */

static DMA_Stream_TypeDef *stream_of_mux(uint32_t ch)
{
    uint32_t base = (ch < 8U) ? (uint32_t)DMA1_Stream0 : (uint32_t)DMA2_Stream0;
    uint32_t n    = (ch < 8U) ? ch : (ch - 8U);

    return (DMA_Stream_TypeDef *)(base + n * sizeof(DMA_Stream_TypeDef));
}

static void stream_disable(DMA_Stream_TypeDef *st);

static void dma_release_sai_requests(void)
{
    for (uint32_t ch = 0; ch < 16U; ch++) {
        /* Ours. Stopped and re-armed by the caller, not here. */
        if (ch == 0U || ch == 1U) { continue; }

        /*
         * Only look at a controller that has a clock. An unclocked
         * DMA2 cannot be running anything, and reading its registers to
         * find that out is a bus fault.
         */
        if (ch >= 8U && !(RCC->AHB1ENR & RCC_AHB1ENR_DMA2EN)) { break; }

        DMAMUX_Channel_TypeDef *mux =
            (DMAMUX_Channel_TypeDef *)((uint32_t)DMAMUX1_Channel0
                                       + ch * sizeof(DMAMUX_Channel_TypeDef));
        uint32_t req = mux->CCR & DMAMUX_CxCR_DMAREQ_ID;

        if (req != DMAREQ_SAI1_A && req != DMAREQ_SAI1_B) { continue; }

        stream_disable(stream_of_mux(ch));
        mux->CCR = 0U;

        s_mux_released |= (1UL << ch);
    }
}

/*
 * Set SAIEN and make sure it is really set. See the long comment at the
 * call site: on this part, written back to back with the registers
 * before it, it sometimes is not.
 */
static uint32_t s_en_retries;

static int block_enable(SAI_Block_TypeDef *b)
{
    for (uint32_t n = 0; n < 1000U; n++) {
        b->CR1 |= SAI_xCR1_SAIEN;

        if (b->CR1 & SAI_xCR1_SAIEN) { return 0; }

        s_en_retries++;
    }

    return -1;
}

static void spin_us(uint32_t us)
{
    uint64_t until = board_cycles()
                   + (((uint64_t)clock_sysclk_hz * us) / 1000000U);

    while (board_cycles() < until) { }
}

/*
 * ENABLE BOTH BLOCKS AND MAKE SURE THEY STAYED ENABLED.
 *
 * THIS IS THE BUG THAT TOOK FOUR LOADS TO FIND, and it is worth the
 * space because every instrument pointed somewhere else.
 *
 * Written as the obvious `CR1 |= SAIEN` for each block, the transport
 * came up on two arms in four. On the other two, block B's CR1 read
 * 0x00020683 afterwards - DMAEN, MODE, DS, CKSTR, SYNCEN all exactly as
 * written and SAIEN simply gone. What the measurements then said, in the
 * order they said it:
 *
 *   - the write was NOT dropped: read back immediately, SAIEN was 1, and
 *     the retry counter never moved;
 *   - 100 us later - nineteen frames - it was 0;
 *   - block B's SR was 0x00000000. No overrun, no AFSDET, no LFSDET, an
 *     empty FIFO. Nothing was objecting;
 *   - block A, the master, was enabled and transmitting throughout.
 *
 * An enable that takes and then quietly un-takes, with no error of any
 * kind, is a block whose previous DISABLE had not finished. SAIEN is not
 * a plain register bit: clearing it asks the hardware to complete the
 * current frame, and the bit reads 0 when that is done - which is what
 * sai1_stop() waits for - but re-enabling immediately after that catches
 * something in the peripheral still winding down, and it takes the new
 * SAIEN down with it.
 *
 * The polled path never showed this only because sai1_start() writes
 * four words into the transmit FIFO between the stop and the enable, and
 * because it does not also write DMAEN. Neither is a delay anybody put
 * there on purpose. This one is: a frame at 192 kHz is 5.2 us, so 200 us
 * is forty of them.
 *
 * And then it is VERIFIED rather than assumed: enable, wait, check, and
 * do the WHOLE ARM again if it did not hold. Measured on this radio: the
 * 400 us settle is not enough on its own - three arms in four still
 * needed a second attempt - and the second attempt has always held. So
 * the counter below is the honest record of which half of the fix is
 * doing the work, and it is the retry, not the wait.
 *
 * WHY THE RETRY HAS TO REDO THE STREAMS TOO, which cost one more load to
 * learn: the first version retried only SAIEN, and the receive stream
 * was already running while it did. A failed attempt therefore moved a
 * FEW WORDS - an unknown number, and the parity is what matters - so
 * when the enable finally held, the ring was offset by one half-word and
 * I and Q came out exchanged. The symptom was two slot means swapped
 * against the polled reference, which is the only reason it was noticed
 * at all; on a spectrum it is a mirrored picture and on a board with
 * matched channels it would be invisible.
 *
 * The invariant that makes the parity right is the same one the polled
 * path relies on: the FIRST half-word a synchronous slave receiver
 * delivers after SAIEN is slot 0. It only holds if the FIFO was flushed
 * and the stream started from the top of the ring IN THE SAME ATTEMPT
 * that succeeded. So the unit of retry is the arm.
 */
static uint32_t s_en_relatch;

static void stream_disable(DMA_Stream_TypeDef *st)
{
    st->CR &= ~DMA_SxCR_EN;

    /*
     * EN does not fall at once: the stream finishes the transfer it is
     * in and then clears the bit itself. Writing the configuration while
     * it is still set is ignored, silently, which is the worst available
     * outcome - so wait for it, bounded, like every other wait here.
     */
    for (uint32_t n = WAIT_LIMIT; n != 0U && (st->CR & DMA_SxCR_EN); n--) { }
}

static void stream_arm(DMA_Stream_TypeDef *st, uint32_t shift,
                       volatile uint32_t *dr, void *mem,
                       uint32_t words, int to_peripheral)
{
    stream_disable(st);

    DMA1->LIFCR = DMA_ALL_FLAGS << shift;

    st->PAR  = (uint32_t)dr;
    st->M0AR = (uint32_t)mem;
    st->NDTR = words;

    /*
     * FCR = 0: direct mode, no FIFO. DMDIS clear means every request
     * moves one item straight through, which is what a 16-bit audio slot
     * wants - the burst the FIFO exists to assemble would only add
     * latency to a stream that is already one word per 5 us. Direct mode
     * also requires PSIZE == MSIZE, which is the next line.
     *
     * The reset value is 0x21, not 0, so this has to be WRITTEN. It is
     * one of the registers WORKING.md's rule is about: an image loaded
     * over SWD inherits whatever the factory firmware left in it.
     */
    st->FCR = 0U;

    /*
     * Half-words at both ends, because the slots are 16 bits. HAL's own
     * polled SAI transmitter writes DR through a uint16_t pointer for
     * every DataSize up to 16, and the DMA has to agree with it: a
     * 32-bit access to DR moves one slot and advances the memory pointer
     * by two, so the stream would read every other sample and play the
     * left channel twice at half speed.
     */
    uint32_t cr = (1UL << DMA_SxCR_PSIZE_Pos)     /* 16-bit peripheral */
                | (1UL << DMA_SxCR_MSIZE_Pos)     /* 16-bit memory     */
                | DMA_SxCR_MINC                   /* memory walks      */
                | DMA_SxCR_CIRC                   /* and wraps forever */
                | (3UL << DMA_SxCR_PL_Pos);       /* very high priority */

    /* PINC stays clear: DR is one register, not an array. */
    if (to_peripheral) { cr |= (1UL << DMA_SxCR_DIR_Pos); }

    /*
     * THE RECEIVE STREAM INTERRUPTS, AND ONLY IT.
     *
     * Half-transfer and transfer-complete together give one interrupt
     * per receive half, which is the block rate a demodulator needs.
     * The transmit stream stays silent on purpose: its halves are now
     * the same size as the receive ones and are filled from the receive
     * interrupt, so a second interrupt saying "transmit is also halfway"
     * would carry no information this code does not already have, and
     * every interrupt that carries no information is one that can
     * arrive at a bad moment.
     */
    if (!to_peripheral) { cr |= DMA_SxCR_HTIE | DMA_SxCR_TCIE; }

    st->CR = cr;
    st->CR |= DMA_SxCR_EN;
}

int sai1_dma_start(uint32_t rx_frames, uint32_t tx_frames)
{
    if (!s_ready) { return SAI1_ERR_NOT_READY; }
    if (rx_frames == 0U || rx_frames > SAI1_RX_HALF_MAX) {
        return SAI1_ERR_DMA_SIZE;
    }
    if (tx_frames == 0U || tx_frames > SAI1_TX_HALF_MAX) {
        return SAI1_ERR_DMA_SIZE;
    }

    /*
     * The clock before anything else looks at DMA1 - including
     * sai1_dma_stop() below, which disables the two streams and
     * therefore needs them to be addressable.
     */
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    (void)RCC->AHB1ENR;

    sai1_dma_stop();

    /*
     * AND THEN LEAVE IT ALONE FOR A MOMENT. sai1_stop() waits for SAIEN
     * to read 0, which is the hardware saying the current frame is
     * finished - but not, apparently, that it has finished winding down:
     * re-enabling immediately afterwards is what blocks_enable() exists
     * to survive. 400 us is 77 frames at 192 kHz, and this runs once per
     * arm, not per block.
     */
    spin_us(400U);

    s_rx_half = rx_frames;
    s_tx_half = tx_frames;

    /*
     * SILENCE FIRST, AND BOTH RINGS.
     *
     * The transmit ring is cleared because it is about to be played: on
     * this radio it would otherwise come up playing whatever the
     * PREVIOUS IMAGE left at those addresses, which over SWD is not even
     * audio. The receive ring is cleared so that a consumer which reads
     * a half before the stream has filled one gets zeros rather than a
     * convincing-looking measurement of the last run.
     */
    rings_clock_on();
    for (uint32_t i = 0; i < (2U * tx_frames * 2U); i++) { s_tx_ring[i] = 0; }
    for (uint32_t i = 0; i < (2U * rx_frames * 2U); i++) { s_rx_ring[i] = 0; }

    s_tx_last = 2U;
    s_tx_min  = 0xFFFFFFFFU;
    s_tx_max  = 0U;

    /*
     * NOBODY ELSE ON OUR REQUEST LINES. See the long comment on
     * dma_release_sai_requests(): the factory firmware's own SAI
     * transport is still enabled on DMA2, and sharing a request line
     * costs exactly half the stream.
     */
    dma_release_sai_requests();

    /*
     * THE MUX BEFORE THE STREAM. A stream enabled while its multiplexer
     * channel still says request 0 is armed for memory-to-memory, and
     * the first thing it would do is run off at full speed through the
     * ring - once, immediately, with no peripheral involved at all.
     */
    RX_MUX->CCR = DMAREQ_SAI1_B;
    TX_MUX->CCR = DMAREQ_SAI1_A;

    /*
     * THE BLOCK INTERRUPT, BELOW SysTick AND DELIBERATELY SO.
     *
     * SysTick is this radio's 1 kHz heartbeat: it samples the ADC - the
     * buttons and the battery - and both encoders. The block interrupt
     * carries a demodulator and can run for a millisecond or two every
     * 42.7 ms. Leaving them at the same priority would let a long block
     * delay a tick, and a delayed tick is a dropped encoder detent or a
     * button that needs pressing twice.
     *
     * A higher NUMBER is lower urgency on this core, so SysTick at its
     * default 0 preempts this at 1. Explicit, because the default would
     * have been 0 for both and the failure would be intermittent.
     */
    NVIC_SetPriority(DMA1_Stream0_IRQn, 1);
    NVIC_EnableIRQ(DMA1_Stream0_IRQn);

    /*
     * THE ARM, AND IT IS A RETRY LOOP AROUND THE WHOLE THING RATHER THAN
     * AROUND SAIEN. The long comment above s_en_relatch says why: an
     * attempt that fails has already moved an unknown number of words,
     * and an odd one exchanges I and Q for the rest of the run. Flush,
     * stream from the top of the ring, DMAEN, SAIEN - all of it, in the
     * attempt that ends up being the one that holds.
     */
    int armed = 0;

    for (uint32_t attempt = 0; attempt < 8U && !armed; attempt++) {
        SAI1_Block_A->CLRFR = 0xFFFFFFFFU;
        SAI1_Block_B->CLRFR = 0xFFFFFFFFU;

        /*
         * FLUSH BOTH FIFOS, AND THIS IS NOT HOUSEKEEPING.
         *
         * A polled capture leaves whatever it did not read sitting in
         * block B's receive FIFO. The DMA would hand those stale words
         * over first, and since each frame is two of them, an ODD number
         * left behind shifts the parity for the entire run: I becomes Q,
         * permanently, with nothing on screen but a mirrored spectrum to
         * say so. The polled path flushes for exactly this reason before
         * it re-aligns.
         *
         * FFLUSH is only writable with SAIEN clear, which the stop
         * before this loop - and at the end of each failed attempt -
         * guarantees.
         */
        SAI1_Block_A->CR2 |= SAI_xCR2_FFLUSH;
        SAI1_Block_B->CR2 |= SAI_xCR2_FFLUSH;

        /* Half-words, not frames: two slots a frame, two halves a ring.
         * Re-armed every attempt so that the ring always starts at index
         * 0 in the attempt that succeeds, which is what makes slot 0
         * land on an even index. */
        stream_arm(RX_STREAM, RX_FLAG_SHIFT, &SAI1_Block_B->DR,
                   s_rx_ring, 4U * rx_frames, 0);
        stream_arm(TX_STREAM, TX_FLAG_SHIFT, &SAI1_Block_A->DR,
                   s_tx_ring, 4U * tx_frames, 1);

        /*
         * DMAEN BEFORE SAIEN, which is ST's documented order and is also
         * what fills the transmit FIFO: with the stream already enabled,
         * the SAI asks for data as soon as DMAEN is set and the FIFO is
         * full before the first frame clock exists. That is why there is
         * no hand-written priming here - and why there MUST not be. The
         * polled sai1_start() writes four zeros into DR itself, and a
         * CPU write into a FIFO a DMA stream is also filling is one word
         * too many: the slots shift by one and stay shifted.
         */
        SAI1_Block_B->CR1 |= SAI_xCR1_DMAEN;
        SAI1_Block_A->CR1 |= SAI_xCR1_DMAEN;

        /*
         * THE SLAVE FIRST. Block B takes its clocks from A, so it has to
         * be listening before A generates a frame, or the two ends
         * disagree about which half of the frame is the left channel.
         */
        if (block_enable(SAI1_Block_B) != 0) { break; }
        if (block_enable(SAI1_Block_A) != 0) { break; }

        spin_us(200U);          /* 38 frames at 192 kHz */

        if ((SAI1_Block_A->CR1 & SAI_xCR1_SAIEN)
            && (SAI1_Block_B->CR1 & SAI_xCR1_SAIEN)) {
            armed = 1;
            break;
        }

        /* It did not hold. Unwind completely - blocks, DMAEN, streams -
         * and go round again. */
        s_en_relatch++;

        sai1_stop();
        SAI1_Block_A->CR1 &= ~SAI_xCR1_DMAEN;
        SAI1_Block_B->CR1 &= ~SAI_xCR1_DMAEN;
        stream_disable(TX_STREAM);
        stream_disable(RX_STREAM);
        spin_us(400U);
    }

    if (!armed) {
        s_dma_on = 0;
        return SAI1_ERR_SAIEN;
    }

    s_dma_on  = 1;
    s_aligned = 0;      /* the polled path's parity is no longer known */
    /* See sai1_rx_half(): nothing in the ring is real yet. DWT is
     * switched on here rather than assumed - it is am_init()'s, and the
     * tests arm before am_init() has run. Never reset: only differences
     * are taken. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
    s_armed_cyc = DWT->CYCCNT;
    s_rx_primed = 0;

    /*
     * A SNAPSHOT, A HUNDRED MICROSECONDS IN.
     *
     * SAIEN reads back as set and is then clear by the time anything
     * else has looked - so the interesting instant is neither "just
     * written" nor "a frame later", it is the twenty frames in between.
     * 100 us is 19 frames at 192 kHz: long enough for the first frame
     * synchronisation to have happened and for anything the hardware
     * objects to to have shown up in SR, short enough that nothing else
     * in the program has run.
     */
    uint64_t until = board_cycles() + ((uint64_t)clock_sysclk_hz / 10000U);

    while (board_cycles() < until) { }

    s_arm.cr1_a   = SAI1_Block_A->CR1;
    s_arm.cr1_b   = SAI1_Block_B->CR1;
    s_arm.sr_a    = SAI1_Block_A->SR;
    s_arm.sr_b    = SAI1_Block_B->SR;
    s_arm.rx_cr   = RX_STREAM->CR;
    s_arm.tx_cr   = TX_STREAM->CR;
    s_arm.rx_ndtr = RX_STREAM->NDTR;
    s_arm.tx_ndtr = TX_STREAM->NDTR;

    return SAI1_OK;
}

const sai1_arm_t *sai1_arm_snapshot(void) { return &s_arm; }

void sai1_dma_stop(void)
{
    /*
     * THE INTERRUPT GOES FIRST, before the streams it reports on. The
     * other order leaves a block interrupt able to arrive after the
     * ring it points into has been torn down, and the hook would read
     * a buffer nobody is filling any more - which is not a crash, just
     * one block of whatever was there, and that is worse.
     */
    NVIC_DisableIRQ(DMA1_Stream0_IRQn);

    sai1_stop();

    SAI1_Block_A->CR1 &= ~SAI_xCR1_DMAEN;
    SAI1_Block_B->CR1 &= ~SAI_xCR1_DMAEN;

    /*
     * ONLY IF DMA1 HAS A CLOCK. sai1_dma_start() calls this before it
     * has enabled one, and on this part a register access to a
     * clock-gated peripheral is not a harmless no-op - it is a bus
     * fault, from a line that looks like tidying up.
     *
     * The flip side is worth knowing too: if the bit IS set on arrival,
     * it was the factory firmware that set it, and streams 0 and 1 may
     * still be running its transfers into its own buffers. Disabling
     * them before arming is not politeness. It is the same rule as every
     * other register here - loading over SWD resets nothing, and a DMA
     * controller does not stop just because the core was halted.
     */
    if (RCC->AHB1ENR & RCC_AHB1ENR_DMA1EN) {
        stream_disable(TX_STREAM);
        stream_disable(RX_STREAM);
    }

    s_dma_on  = 0;
    s_aligned = 0;
}

int sai1_dma_running(void) { return s_dma_on; }

/*
 * Where the receive DMA is right now, in frames into the half it is
 * filling. NDTR counts the ring's half-words DOWN from 4 x s_rx_half
 * (two halves, two slots a frame), so what has been written is the
 * difference, and a frame is two of those.
 *
 * Read with sai1_blocks either side of it by the caller if it needs the
 * pair to be consistent; one read of a free-running counter is all this
 * is.
 */
uint32_t sai1_rx_frame_pos(void)
{
    if (!s_dma_on || s_rx_half == 0U) { return 0U; }

    uint32_t written = 4U * s_rx_half - RX_STREAM->NDTR;

    return (written / 2U) % s_rx_half;
}

/*
 * The flags both directions can raise, read where the caller already is
 * rather than in an interrupt. OVRUDR is sticky, so one read per call
 * catches anything that happened since the last one; what it cannot do
 * is count two of them inside one call, which sai1.h says out loud.
 */
static void dma_flags_poll(void)
{
    if (SAI1_Block_A->SR & SAI_xSR_OVRUDR) {
        s_tx_under++;
        SAI1_Block_A->CLRFR = SAI_xCLRFR_COVRUDR;
    }
    if (SAI1_Block_B->SR & SAI_xSR_OVRUDR) {
        s_rx_over++;
        SAI1_Block_B->CLRFR = SAI_xCLRFR_COVRUDR;
    }

    /*
     * WHICH FLAG, AND ON WHICH STREAM. A count on its own says "there
     * was an error" and sends you looking at all three possible ones;
     * the three mean completely different things. TEIF is an address the
     * DMA cannot reach - a bug in where a buffer was linked. DMEIF is a
     * direct-mode error. FEIF is the FIFO, and in direct mode it is
     * mostly raised by tearing a stream down with a preloaded word still
     * in it, which is a teardown artefact and not a running fault.
     *
     * Kept as sticky bits rather than a per-flag count: the question
     * asked of this is "which", and the count already answers "how
     * often".
     */
    uint32_t lisr = DMA1->LISR;
    uint32_t rx   = (lisr >> RX_FLAG_SHIFT) & DMA_ERR_FLAGS;
    uint32_t tx   = (lisr >> TX_FLAG_SHIFT) & DMA_ERR_FLAGS;

    if (rx != 0U) {
        s_dma_err++;
        s_dma_err_bits |= rx;
        DMA1->LIFCR = rx << RX_FLAG_SHIFT;
    }
    if (tx != 0U) {
        s_dma_err++;
        s_dma_err_bits |= tx << 8;
        DMA1->LIFCR = tx << TX_FLAG_SHIFT;
    }
}

/*
 * THE RECEIVE BLOCK INTERRUPT.
 *
 * One per receive half: half-transfer says the first half is complete,
 * transfer-complete says the second is. Which half just FILLED is the
 * opposite of the one the stream is now writing into, and that is the
 * one handed to the hook - reading the half still being written is the
 * tearing this interrupt exists to avoid.
 *
 * SHORT, BOUNDED, AND NOT RE-ENTRANT. The flags are cleared first so a
 * long hook cannot lose the next block's edge, and s_in_hook refuses a
 * second entry rather than letting two blocks interleave in a
 * demodulator's filter state. A refusal is counted, because a
 * demodulator that cannot keep up with the block rate has to be a
 * number rather than a mystery crackle.
 */
void DMA1_Stream0_IRQHandler(void)
{
    uint32_t lisr = DMA1->LISR;
    uint32_t done = lisr & ((DMA_LISR_HTIF0 | DMA_LISR_TCIF0) << RX_FLAG_SHIFT);

    if (done == 0U) { return; }

    DMA1->LIFCR = done;

    sai1_blocks++;

    if (s_in_hook) { sai1_block_overruns++; return; }

    sai1_block_hook_t fn = s_block_hook;

    if (fn == 0) { return; }

    /*
     * HTIF means the first half just filled, TCIF the second. Taking
     * them from the flags rather than from NDTR is deliberate: NDTR has
     * already moved on by the time this runs, and at a block boundary it
     * is exactly the ambiguous value.
     */
    uint32_t half = (done & (DMA_LISR_TCIF0 << RX_FLAG_SHIFT)) ? 1U : 0U;

    /*
     * THE SAME INDEX ON BOTH SIDES. The two streams were armed in the
     * same attempt, carry the same number of frames and are clocked by
     * the same frame clock, so they advance together: the transmit half
     * that was playing while this receive half filled is the one with
     * the same index, and it is now free for a whole block. Any residual
     * skew from the arming order is a handful of frames against 42.7 ms
     * of margin.
     */
    s_in_hook = 1U;
    fn(&s_rx_ring[half * s_rx_half * 2U],
       &s_tx_ring[half * s_tx_half * 2U],
       s_rx_half);
    s_in_hook = 0U;
}

/*
 * WHICH HALF THE STREAM IS WRITING, out of NDTR.
 *
 * A circular stream counts NDTR down to zero and reloads it, so the
 * position within the ring is (total - NDTR) and the half is which side
 * of the middle that falls on. No interrupt, no flag, no state of our
 * own to get out of step with the hardware - and it is upstream's own
 * mechanism on the DEEPSDR, where the same question is asked of the
 * GD32's DMA.
 */
static uint32_t stream_half(DMA_Stream_TypeDef *st, uint32_t total)
{
    uint32_t left = st->NDTR;
    uint32_t pos  = (left <= total) ? (total - left) : 0U;

    return (pos >= (total / 2U)) ? 1U : 0U;
}

static uint32_t stream_pos(DMA_Stream_TypeDef *st, uint32_t total)
{
    uint32_t left = st->NDTR;

    return (left <= total) ? (total - left) : 0U;
}

uint32_t sai1_rx_half(int16_t *dst, uint32_t frames)
{
    if (!s_dma_on || dst == NULL) { return 0U; }
    if (frames != s_rx_half) { s_len_bad++; return 0U; }

    dma_flags_poll();

    uint32_t total = 4U * frames;
    uint32_t words = 2U * frames;

    /*
     * NOT BEFORE A HALF HAS FILLED. The half the stream is NOT in is
     * the finished one - except for the first 42.7 ms after an arm,
     * when it is the half sai1_dma_start() has just zeroed. A caller
     * that comes straight back gets a block of digital silence that
     * looks like a perfectly good capture: one black waterfall row and
     * a probe reading "leak DC 0 station 0 mirror 0".
     *
     * It was always there and always hidden: the display loop printed
     * a ~600-character arm report first, ~50 ms on the polled console,
     * which is longer than a half. Buffering the console (20/11/2026)
     * took the delay away and the zeros showed.
     *
     * Waited out here on the CPU's cycle counter - not on the block
     * interrupt, and not on board_cycles(), which stands still when
     * SysTick is not being serviced - so a capture depends on nothing
     * that might not be running. Once per arm, at most one half.
     */
    if (!s_rx_primed) {
        uint32_t fs   = (s_clock.fs_hz != 0U) ? s_clock.fs_hz : 192000U;
        uint32_t need = (uint32_t)(((uint64_t)frames * clock_sysclk_hz) / fs)
                      + clock_sysclk_hz / 2000U;            /* +0.5 ms */

        while ((uint32_t)(DWT->CYCCNT - s_armed_cyc) < need) { }
        s_rx_primed = 1;
    }

    /*
     * Copy the half the stream is NOT in, then check it is still not in
     * it. The margin is enormous - 42.7 ms of half against about 30 us
     * of copy - so this can only fire if the stream is not where NDTR
     * says, and that is worth knowing rather than averaging. Two torn
     * copies in a row is a failure, not a retry.
     */
    for (uint32_t attempt = 0; attempt < 2U; attempt++) {
        uint32_t w = stream_half(RX_STREAM, total);
        const int16_t *src = &s_rx_ring[(w ^ 1U) * words];

        for (uint32_t i = 0; i < words; i++) { dst[i] = src[i]; }

        if (stream_half(RX_STREAM, total) == w) { return frames; }
        s_rx_torn++;
    }

    return 0U;
}

int sai1_tx_write_half(const int16_t *stereo_frames, uint32_t frames)
{
    if (!s_dma_on || stereo_frames == NULL) { return -1; }
    if (frames != s_tx_half) { s_len_bad++; return -1; }

    dma_flags_poll();

    uint32_t total = 4U * frames;
    uint32_t words = 2U * frames;
    uint32_t pos   = stream_pos(TX_STREAM, total);
    uint32_t play  = (pos >= words) ? 1U : 0U;
    uint32_t into  = play ^ 1U;

    /* Where inside the playing half this landed - the producer's phase
     * margin. Upstream watches the same two numbers. */
    uint32_t off = pos - (play * words);
    if (off < s_tx_min) { s_tx_min = off; }
    if (off > s_tx_max) { s_tx_max = off; }

    if (into == s_tx_last) { s_tx_repeat++; }
    s_tx_last = into;

    int16_t *d = &s_tx_ring[into * words];
    for (uint32_t i = 0; i < words; i++) { d[i] = stereo_frames[i]; }

    return 0;
}

uint32_t sai1_rx_pos(void)
{
    if (!s_dma_on) { return 0U; }

    return stream_pos(RX_STREAM, 4U * s_rx_half);
}

uint32_t sai1_rx_ring_words(void)
{
    return 4U * s_rx_half;
}

int sai1_tx_due(void)
{
    if (!s_dma_on) { return 0; }

    uint32_t total = 4U * s_tx_half;
    uint32_t words = 2U * s_tx_half;
    uint32_t play  = (stream_pos(TX_STREAM, total) >= words) ? 1U : 0U;

    return ((play ^ 1U) != s_tx_last) ? 1 : 0;
}

uint32_t sai1_tx_margin_min(void) { return s_tx_min; }
uint32_t sai1_tx_margin_max(void) { return s_tx_max; }
uint32_t sai1_tx_repeats(void)    { return s_tx_repeat; }
uint32_t sai1_tx_half_words(void) { return 2U * s_tx_half; }

uint32_t sai1_rx_cr(void)           { return RX_STREAM->CR; }
uint32_t sai1_tx_cr(void)           { return TX_STREAM->CR; }
uint32_t sai1_rx_ndtr(void)         { return RX_STREAM->NDTR; }
uint32_t sai1_tx_ndtr(void)         { return TX_STREAM->NDTR; }
uint32_t sai1_dma_lisr(void)        { return DMA1->LISR; }

uint32_t sai1_en_retries(void)      { return s_en_retries; }
uint32_t sai1_en_relatch(void)      { return s_en_relatch; }
uint32_t sai1_mux_released(void)    { return s_mux_released; }
uint32_t sai1_dma_errors(void)      { return s_dma_err; }
uint32_t sai1_dma_err_bits(void)    { return s_dma_err_bits; }
uint32_t sai1_mode_switches(void)   { return s_mode_switch; }
uint32_t sai1_rx_torn(void)         { return s_rx_torn; }
uint32_t sai1_len_mismatches(void)  { return s_len_bad; }

int sai1_dma_streams_live(void)
{
    if (!s_dma_on) { return 0; }

    return ((RX_STREAM->CR & DMA_SxCR_EN) && (TX_STREAM->CR & DMA_SxCR_EN))
           ? 1 : 0;
}
