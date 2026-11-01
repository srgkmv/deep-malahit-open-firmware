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
 * that established it. Defaults are I2S as RM0433 describes it; main()
 * measures which is actually right.
 */
static int s_rx_ckstr = 0;
static int s_rx_fsoff = 1;

/*
 * Whether the receiver's slot parity is known. Cleared by anything that
 * can have moved it: a rate change, a framing change, or an overrun.
 */
static int s_aligned;

/* Block status at the end of the last stream, for when a capture comes
 * up short and the question is why. */
static uint32_t s_last_sr_a, s_last_sr_b;

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

void sai1_start(void)
{
    /*
     * THE SLAVE GOES FIRST. A synchronous slave has to be enabled before
     * the block that generates its clocks, or the first frames arrive at
     * a receiver that is not listening yet and the two ends disagree
     * about which half of the frame is the left channel from then on.
     *
     * The transmit FIFO is primed with silence before block A starts,
     * because a master transmitter whose FIFO is empty underruns on its
     * very first frame. That costs nothing but it does set a flag, and a
     * flag that is always set is a flag that cannot be used as evidence
     * later.
     */
    SAI1_Block_A->CLRFR = 0xFFFFFFFFU;
    SAI1_Block_B->CLRFR = 0xFFFFFFFFU;

    for (uint32_t i = 0; i < 4U; i++) { SAI1_Block_A->DR = 0U; }

    SAI1_Block_B->CR1 |= SAI_xCR1_SAIEN;
    SAI1_Block_A->CR1 |= SAI_xCR1_SAIEN;
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

    s_tx_under = 0;

    uint32_t guard = (frames + 64U) * 20000U;

    while (frames > 0U) {
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
