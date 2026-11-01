#include "stm32h7xx.h"
#include "msi001.h"
#include "board.h"

/*
 * SPI1 at 240/64 = 3.75 MHz, 24-bit frames.
 *
 * 24-bit frames are the reason this uses hardware SPI at all rather
 * than bit-banging: the H7's SPI takes any DSIZE from 4 to 32 bits, so
 * the MSi001's awkward 20-data-plus-4-address word is a single native
 * transfer. A byte-oriented controller could not send it without
 * padding, and padding changes which bits land in the address field.
 *
 * SPI1 shares the SPI123SEL kernel clock selector with SPI3, which the
 * panel uses; both drivers set it to pll1_q_ck, so they agree.
 */
#define SPI1_MBR_DIV64      5UL

/* PA4 = EN, driven by hand. */
#define EN_PORT             GPIOA
#define EN_PIN              4U

/* PE10..PE15 = BPF1..BPF6, PE0 = PREAMP. */
#define BPF_SHIFT           10U
#define BPF_MASK            0x3FU

static uint32_t  s_shadow[8];
static msi_plan_t s_plan;
static uint32_t  s_bpf;

/* ------------------------------------------------------------------ */
/* Band table                                                         */
/* ------------------------------------------------------------------ */

/*
 * LO division ratio per mode, datasheet table 22, and the mode bits
 * that go with it. The mode field is register 0 bits 4..11, which the
 * datasheet quotes as D[11:4] - its D numbering is the data field, four
 * places below the register bit numbering, and mixing the two up is an
 * easy way to enable the wrong band.
 */
typedef struct {
    uint32_t lodiv;
    uint32_t mode_bits;     /* register 0, bits 4..11 already in place */
    int      up_convert;    /* AM path through FIF1 */
} band_info_t;

#define BIT_AM_MODE     (1UL << 4)
#define BIT_VHF_MODE    (1UL << 5)
#define BIT_B3_MODE     (1UL << 6)
#define BIT_B45_MODE    (1UL << 7)
#define BIT_BL_MODE     (1UL << 8)
#define BIT_AM_MODE2    (1UL << 9)      /* up-convert mixer enable */
#define BIT_RF_SYNTH    (1UL << 10)
#define BIT_AM_PORT2    (1UL << 11)

static const band_info_t s_bands[] = {
    /* AM  */ { 16U, BIT_AM_MODE  | BIT_AM_MODE2 | BIT_RF_SYNTH, 1 },
    /* VHF */ { 32U, BIT_VHF_MODE | BIT_RF_SYNTH,                0 },
    /* B3  */ { 16U, BIT_B3_MODE  | BIT_RF_SYNTH,                0 },
    /* B45 */ {  4U, BIT_B45_MODE | BIT_RF_SYNTH,                0 },
    /* L   */ {  2U, BIT_BL_MODE  | BIT_RF_SYNTH,                0 }
};

msi_band_t msi001_band_for(uint32_t freq_hz)
{
    /*
     * Chosen by the filters actually fitted to THIS board, not by the
     * chip's capability. The input bank covers 0-12, 12-30 and
     * 30-60 MHz plus a VHF path, so everything up to 60 MHz goes
     * through the AM up-convert mixer and above that through the VHF
     * mixer.
     *
     * 60 MHz in "AM mode" reads oddly for a part whose AM description
     * says long, medium and short wave, but the mode is just the
     * up-convert path and the synthesiser reaches it comfortably:
     * (132 + 60) x 16 = 3072 MHz.
     */
    if (freq_hz <  60000000U) { return MSI_BAND_AM; }
    if (freq_hz < 150000000U) { return MSI_BAND_VHF; }
    if (freq_hz < 260000000U) { return MSI_BAND_B3; }
    if (freq_hz < 1000000000U) { return MSI_BAND_B45; }
    return MSI_BAND_L;
}

/* ------------------------------------------------------------------ */
/* Tuning arithmetic                                                  */
/* ------------------------------------------------------------------ */

void msi001_plan(uint32_t freq_hz, msi_band_t band, msi_plan_t *out)
{
    out->f_target = freq_hz;
    out->band     = band;
    out->ok       = 0;

    if ((uint32_t)band > MSI_BAND_L) { return; }

    const band_info_t *b = &s_bands[band];

    out->lodiv = b->lodiv;

    /*
     * The mixer LO. In the AM modes the signal is first up-converted to
     * FIF1, so the LO has to clear that as well; everywhere else it is
     * direct conversion and the LO sits on the carrier.
     *
     * Zero IF throughout, so there is no second IF term to add. A tone
     * at the tuned frequency therefore lands at DC, and the way to put
     * it somewhere measurable is to tune deliberately beside it - which
     * is what main.c does.
     */
    uint64_t flo = (uint64_t)freq_hz;
    if (b->up_convert) { flo += MSI_FIF1_HZ; }

    out->flo_hz    = (uint32_t)flo;
    uint64_t fsynth = flo * b->lodiv;
    out->fsynth_hz = (uint32_t)fsynth;

    /*
     * X = FSYNTH / (4 x FREF1), split into INT and a fraction of
     * THRESH x 4096. Everything in 64-bit integers: the numbers run to
     * 4e9 x 4095 x 4096 and a 32-bit intermediate would wrap silently
     * somewhere in the middle of the band.
     */
    uint32_t divint = (uint32_t)(fsynth / MSI_FCOMP_HZ);
    uint64_t rem    = fsynth - (uint64_t)divint * MSI_FCOMP_HZ;

    /* fraction x THRESH x 4096, rounded */
    uint64_t scale = (uint64_t)MSI_THRESH * 4096U;
    uint64_t total = (rem * scale + MSI_FCOMP_HZ / 2U) / MSI_FCOMP_HZ;

    /* Rounding can carry all the way into the integer part. */
    if (total >= scale) { total -= scale; divint++; }

    /* INT is six bits. Outside that the band or the reference is wrong,
     * and quietly truncating would tune somewhere arbitrary. */
    if (divint < 4U || divint > 63U) { return; }

    out->divint = divint;
    out->frac   = (uint32_t)(total / 4096U);
    out->afc    = (uint32_t)(total % 4096U);

    /*
     * Now work the arithmetic back the other way, through exactly the
     * expression the hardware evaluates, and report what it really
     * gives. This is the same discipline as the SAI's measured frame
     * rate: a divider chain should be asked what it did, not told what
     * it was meant to do.
     */
    uint64_t num = (uint64_t)divint * scale + total;
    uint64_t flo_act = ((uint64_t)MSI_FCOMP_HZ * num) / (scale * b->lodiv);

    uint64_t f_act = flo_act;
    if (b->up_convert) { f_act -= MSI_FIF1_HZ; }

    out->f_actual = (uint32_t)f_act;
    out->err_hz   = (int32_t)((int64_t)f_act - (int64_t)freq_hz);

    /* Tuning step in micro-Hz, which is the only way to show it without
     * floating point: it is well under a hertz. */
    out->step_mhz_e6 = (uint32_t)(((uint64_t)MSI_FCOMP_HZ * 1000000U)
                                  / (b->lodiv * scale));
    out->ok = 1;
}

/* ------------------------------------------------------------------ */
/* Bus                                                                */
/* ------------------------------------------------------------------ */

static void gpio_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (2UL << (pin * 2U));
    port->OSPEEDR |= (2UL << (pin * 2U));
    port->OTYPER &= ~(1UL << pin);
    port->PUPDR &= ~(3UL << (pin * 2U));
    port->AFR[pin >> 3U] =
          (port->AFR[pin >> 3U] & ~(0xFUL << ((pin & 7U) * 4U)))
        | (af << ((pin & 7U) * 4U));
}

static void gpio_out(GPIO_TypeDef *port, uint32_t pin)
{
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
    port->OTYPER &= ~(1UL << pin);
    port->OSPEEDR &= ~(3UL << (pin * 2U));
    port->PUPDR &= ~(3UL << (pin * 2U));
}

void msi001_init(void)
{
    for (uint32_t i = 0; i < 8U; i++) { s_shadow[i] = 0U; }

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOEEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;

    /* Say which kernel clock rather than inherit one: 000 = pll1_q_ck.
     * The factory firmware leaves SPI123SEL on per_ck, which cost the
     * panel bring-up an evening of measurements that were all a uniform
     * 3.75x out. */
    RCC->D2CCIP1R &= ~RCC_D2CCIP1R_SPI123SEL_Msk;

    /* EN idle high before the pin becomes an output, so the tuner never
     * sees a rising edge it would read as "latch what you have". */
    EN_PORT->BSRR = (1UL << EN_PIN);
    gpio_out(EN_PORT, EN_PIN);

    gpio_af(GPIOA, 5U, 5U);     /* SPI1_SCK  */
    gpio_af(GPIOA, 7U, 5U);     /* SPI1_MOSI */

    /* Band filter selects and the preamp enable, all plain outputs. */
    for (uint32_t p = 10U; p <= 15U; p++) { gpio_out(GPIOE, p); }
    gpio_out(GPIOE, 0U);
    GPIOE->BSRR = (BPF_MASK << (BPF_SHIFT + 16U));      /* all low */
    GPIOE->BSRR = (1UL << (0U + 16U));                  /* preamp off */
    s_bpf = 0;

    SPI1->CR1  = 0U;
    SPI1->IFCR = 0xFFFFFFFFU;

    /* SSI before MASTER. With software slave management the master reads
     * its own NSS from CR1.SSI, and enabling master mode with SSI clear
     * raises a mode fault - which clears the master bit in hardware,
     * silently. spi3.c has the full story. */
    SPI1->CR1 = SPI_CR1_SSI;

    /* 24-bit frames: 20 data bits then the 4 address bits. */
    SPI1->CFG1 = (23UL << SPI_CFG1_DSIZE_Pos)
               | (SPI1_MBR_DIV64 << SPI_CFG1_MBR_Pos);

    /* Master, transmit only, software NSS, mode 0 so data is sampled on
     * the rising edge of CLK as the datasheet requires. */
    SPI1->CFG2 = SPI_CFG2_MASTER
               | (1UL << SPI_CFG2_COMM_Pos)
               | SPI_CFG2_SSM
               | SPI_CFG2_AFCNTR;
}

void msi001_write(uint32_t reg, uint32_t data)
{
    uint32_t word = ((data & 0xFFFFFU) << 4) | (reg & 0xFU);

    s_shadow[reg & 7U] = data & 0xFFFFFU;

    SPI1->CR1 &= ~SPI_CR1_SPE;
    SPI1->IFCR = 0xFFFFFFFFU;
    SPI1->CR2  = 1U;

    EN_PORT->BSRR = (1UL << (EN_PIN + 16U));    /* EN low */

    SPI1->CR1 |= SPI_CR1_SPE;
    SPI1->CR1 |= SPI_CR1_CSTART;

    SPI1->TXDR = word;                          /* one 24-bit frame */

    /* EOT, not TXC: EOT means the shift register has emptied and the
     * last bit is on the wire. Here that is load-bearing in the
     * strongest sense - THE RISING EDGE OF EN IS THE WRITE, so an EN
     * that goes up early latches a short word, and a short word puts
     * the data bits into the address field. The part would then obey a
     * register nobody addressed. */
    for (uint32_t n = 200000U; n != 0U && !(SPI1->SR & SPI_SR_EOT); n--) { }

    /* Hold EN low briefly after the last clock edge, then commit. */
    for (volatile uint32_t d = 0; d < 20U; d++) { }
    EN_PORT->BSRR = (1UL << EN_PIN);            /* EN high - THE WRITE */

    SPI1->IFCR = 0xFFFFFFFFU;
    SPI1->CR1 &= ~SPI_CR1_SPE;

    /* tIWD, the interword delay, is 300 ns minimum. At 480 MHz that is
     * 144 cycles, which the few instructions above do not cover. */
    for (volatile uint32_t d = 0; d < 80U; d++) { }
}

uint32_t msi001_shadow(uint32_t reg)
{
    return s_shadow[reg & 7U];
}

/* ------------------------------------------------------------------ */
/* Registers                                                          */
/* ------------------------------------------------------------------ */

/* Register 0 data field, bits numbered as the register (address apart). */
static uint32_t reg0_build(msi_band_t band, int port2)
{
    uint32_t v = s_bands[band].mode_bits;

    if (band == MSI_BAND_AM && port2) { v |= BIT_AM_PORT2; }

    v |= (MSI_FILMODE_ZEROIF << 12);
    v |= (MSI_FILBW_200K     << 14);
    v |= (MSI_XTAL_SEL       << 17);
    /* IF_LPMODE and VCO_LPMODE left at zero: both channels on, full
     * power. The low-power modes drop one of I or Q, which for a
     * quadrature receiver is not a power saving, it is a broken
     * receiver. */

    return v >> 4;      /* the builder works in register bits; the bus
                         * wants the data field, four places down */
}

static uint32_t reg1_build(uint8_t bb_gr, int lna_gr, int mix_gr,
                           uint8_t mixbu, uint32_t dccal)
{
    uint32_t v = 0;

    if (bb_gr > 59U) { bb_gr = 59U; }

    v |= ((uint32_t)bb_gr & 0x3FU) << 4;
    v |= ((uint32_t)mixbu & 0x3U)  << 10;
    if (mix_gr) { v |= (1UL << 12); }
    if (lna_gr) { v |= (1UL << 13); }
    v |= (dccal & 0x7U) << 14;

    return v >> 4;
}

/*
 * DC offset calibration mode.
 *
 * 000 is "static": calibrate once, on power-up and on every
 * synthesiser programming, then hold. That is the right default for a
 * receiver and not the obvious one.
 *
 * The alternatives all keep correcting while the receiver is listening,
 * and each correction digs a spectral hole at DC - which on a
 * zero-IF receiver is the middle of the passband, the tuned frequency
 * itself. Continuous mode keeps that hole open permanently. Static mode
 * puts the correction exactly where it belongs: at the moment of
 * retuning, when the receiver is not being listened to anyway, and
 * never again until the next retune.
 *
 * The cost is drift, since the hold circuit leaks. If that turns out to
 * matter, the periodic modes and register 6's timers are the answer -
 * but they should be a measured response to drift, not a precaution.
 */
#define MSI_DCCAL_STATIC    0U

static uint8_t s_bb_gr   = 0;
static int     s_lna_gr  = 0;
static int     s_mix_gr  = 0;
static uint8_t s_mixbu   = 0;
static int     s_port2   = 1;

static void write_synth(const msi_plan_t *p)
{
    /*
     * Register 5 first: THRESH has to be in place before register 2 is
     * written, because writing register 2 is what starts the
     * synthesiser - and the datasheet's power-up flow puts it in that
     * order for the same reason.
     *
     * BITS 19 AND 21 MUST BE SET. The datasheet says so in one line of
     * body text, outside the register table, with no explanation: "bits
     * 19 and 21 need to be set high for correct operation of the part".
     * The table marks them reserved-must-be-0 and reserved-must-be-1
     * respectively, contradicting itself on bit 19. The worked example
     * in table 16 settles it - its register 5 word has both set - and
     * that example is also what confirmed this whole bit layout.
     */
    uint32_t r5 = ((MSI_THRESH & 0xFFFU) << 4)
                | (1UL << 19)
                | (1UL << 21);
    msi001_write(MSI_REG_SYNTHCFG, r5 >> 4);

    uint32_t r2 = ((p->frac   & 0xFFFU) << 4)
                | ((p->divint & 0x3FU)  << 16);
    msi001_write(MSI_REG_SYNTH, r2 >> 4);

    /* AFC last, as the flow diagram has it: it is a trim on top of a
     * synthesiser that is already running. */
    uint32_t r3 = (p->afc & 0xFFFU) << 4;
    msi001_write(MSI_REG_LOTRIM, r3 >> 4);
}

int msi001_tune(uint32_t freq_hz, msi_band_t band)
{
    msi_plan_t p;

    msi001_plan(freq_hz, band, &p);
    if (!p.ok) { return MSI_ERR_RANGE; }

    /* Datasheet figure 5, in its order: 0, 5, 2, 1, 6, 4, 3. */
    msi001_write(MSI_REG_MODE, reg0_build(band, s_port2));

    write_synth(&p);        /* 5, 2, 3 - see below for why 3 is here */

    msi001_write(MSI_REG_GAIN, reg1_build(s_bb_gr, s_lna_gr, s_mix_gr,
                                          s_mixbu, MSI_DCCAL_STATIC));

    /* Register 6 keeps its reset timers: with static DC calibration
     * they are never used, and writing invented values would only make
     * a later switch to periodic mode harder to reason about. */

    /* Register 4: the 1.84 MHz sine generator stays off. It exists to
     * drive a varicap tuning-voltage pump for an external tracking
     * filter; this board switches fixed filters with PE4259s and
     * PE42641s instead, so the generator would be nothing but a spur
     * inside the tuner. */
    msi001_write(MSI_REG_AUX, 0U);

    s_plan = p;

    /*
     * Let the synthesiser lock and the DC calibration finish. The
     * datasheet gives DC calibration timing in table 20 and the figures
     * are in the tens of microseconds; a millisecond is ample and this
     * is not a path that runs often.
     */
    board_delay_ms(2);

    return MSI_OK;
}

int msi001_retune(uint32_t freq_hz)
{
    msi_plan_t p;

    msi001_plan(freq_hz, s_plan.band, &p);
    if (!p.ok) { return MSI_ERR_RANGE; }

    write_synth(&p);
    s_plan = p;
    board_delay_ms(2);

    return MSI_OK;
}

const msi_plan_t *msi001_last_plan(void)
{
    return &s_plan;
}

void msi001_set_gain(uint8_t bb_gr, int lna_gr, int mix_gr, uint8_t mixbu)
{
    s_bb_gr  = bb_gr;
    s_lna_gr = lna_gr;
    s_mix_gr = mix_gr;
    s_mixbu  = mixbu;

    msi001_write(MSI_REG_GAIN, reg1_build(bb_gr, lna_gr, mix_gr, mixbu,
                                          MSI_DCCAL_STATIC));
}

void msi001_set_am_port(int port2)
{
    s_port2 = port2;

    if (s_plan.ok) {
        msi001_write(MSI_REG_MODE, reg0_build(s_plan.band, port2));
    }
}

uint32_t msi001_bpf_for(uint32_t freq_hz)
{
    if (freq_hz < 12000000U) { return MSI_BPF_0_12; }
    if (freq_hz < 30000000U) { return MSI_BPF_12_30; }
    if (freq_hz < 60000000U) { return MSI_BPF_30_60; }
    return MSI_BPF_VHF1;
}

void msi001_bpf(uint32_t mask)
{
    s_bpf = mask & BPF_MASK;

    /* BSRR takes set bits in the low half and reset bits in the high
     * half, so one write changes all six lines at the same instant.
     * Walking them one at a time would step through filter
     * combinations nobody asked for on the way. */
    GPIOE->BSRR = (s_bpf << BPF_SHIFT)
                | ((~s_bpf & BPF_MASK) << (BPF_SHIFT + 16U));
}

uint32_t msi001_bpf_get(void)
{
    return s_bpf;
}

void msi001_preamp(int on)
{
    GPIOE->BSRR = on ? (1UL << 0U) : (1UL << (0U + 16U));
}
