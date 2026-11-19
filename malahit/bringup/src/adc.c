#include "stm32h7xx.h"
#include "adc.h"
#include "clock.h"
#include "board.h"

uint32_t adc_conversions;
/* Entered, and entered with sampling live: three states that all look
 * like a line sitting at zero, and only these tell them apart. */
uint32_t adc_ticks;
uint32_t adc_ticks_live;
uint32_t adc_timeouts;
uint32_t adc_cal_factor;
uint32_t adc_found_cr;
uint32_t adc_found_pmcr;
uint32_t adc_cal_ms;
uint32_t adc_cr_after_vreg;
uint32_t adc_cr_after_cal;
uint32_t adc_isr_after_aden;
uint32_t adc_ccr;
uint8_t  adc_attempt_used;
int      adc_attempt_rc[ADC_ATTEMPTS];

static uint32_t s_kernel_hz;            /* after the prescaler */
static volatile uint32_t s_raw[2];      /* 0 = ENC_BUT, 1 = BAT */
static volatile uint8_t  s_btn;
static volatile uint8_t  s_live;
static uint8_t  s_slot;                 /* which of the two is converting */
static uint8_t  s_pending;

/* Bounded, because none of these waits may become the thing that stops
 * the radio - see board_delay_ms() for what that costs. Every one of
 * them is a handful of microseconds in normal operation. */
#define WAIT_LIMIT      200000U

/*
 * SAMPLING TIME IS SET BY THE LADDER, NOT BY THE SIGNAL.
 *
 * The source impedance at this pin is 47 kohm in the idle state - the
 * ladder's top resistor, with both switches open - which is enormous for
 * an ADC input. The sampling capacitor has to charge through it, and if
 * the sample ends too early the reading is low by however much is
 * missing, which looks exactly like a lower voltage and would land
 * somewhere in the middle of a button band.
 *
 * 110 = 387.5 ADC clock cycles, about 24 us at the clock this driver
 * sets up. Against a 47 kohm source and a few picofarads of sampling
 * capacitance the time constant is a couple of hundred nanoseconds, so
 * this is two orders of magnitude of margin. It costs nothing: the
 * conversion is started from one SysTick and collected by the next.
 */
#define SMP_LONG        6U

static void spin_us(uint32_t us)
{
    /*
     * A COUNTED LOOP, NOT board_cycles().
     *
     * board_cycles() is built on the SysTick interrupt's millisecond
     * counter, so a wait written against it never ends if that interrupt
     * is not arriving - and "is the timebase alive" is precisely the
     * question one cannot assume the answer to during bring-up. This is
     * also why board_delay_ms() grew a cycle-count backstop.
     *
     * A volatile loop iteration cannot be fewer than about three cycles,
     * so dividing by three turns a cycle budget into an iteration count
     * that is guaranteed to be LONG enough. Being too slow is free here;
     * being too fast is a settling time that did not happen.
     */
    uint32_t iters = ((clock_sysclk_hz / 1000000U) * us) / 3U + 8U;

    for (volatile uint32_t i = 0; i < iters; i++) { }
}

/* ------------------------------------------------------------------ */
/* The kernel clock                                                   */
/* ------------------------------------------------------------------ */

/*
 * WHY per_ck AND NOT A PLL.
 *
 * The ADC's kernel clock has four sources and three of them are a
 * problem here. pll2_p_ck is the SAI's: PLL2 is tuned to put MCLK at
 * 256 x Fs and touching it would retune the audio clock. pll3_r_ck means
 * bringing up a third PLL for a peripheral that needs 16 MHz. The
 * synchronous option (CKMODE != 0) divides the AHB clock, and the
 * smallest division available is /4, which from 240 MHz is 60 MHz -
 * above the part's ADC clock ceiling.
 *
 * per_ck is the one nothing else in this firmware uses, and it has a
 * prescaler of its own that reaches /256. HSI is never switched off here
 * (clock.c parks on it before building the tree and leaves it running),
 * so per_ck's default source is alive by the time this runs.
 */
static const struct { uint8_t code; uint16_t div; } k_presc[] = {
    { 0U, 1U }, { 1U, 2U }, { 2U, 4U }, { 3U, 6U }, { 4U, 8U }, { 5U, 10U },
    { 6U, 12U }, { 7U, 16U }, { 8U, 32U }, { 9U, 64U }, { 10U, 128U },
    { 11U, 256U }
};

/* Read, do not assume: whatever CKPERSEL and HSIDIV actually say. */
static uint32_t per_ck_hz(void)
{
    uint32_t sel = (RCC->D1CCIPR & RCC_D1CCIPR_CKPERSEL_Msk)
                 >> RCC_D1CCIPR_CKPERSEL_Pos;

    if (sel == 0U) {
        /* hsi_ker_ck: 64 MHz divided by HSIDIV, which is a field and not
         * a constant - a part left at /8 by someone else would otherwise
         * have this driver believing in 64 MHz and setting BOOST eight
         * times too high. */
        uint32_t hsidiv = (RCC->CR & RCC_CR_HSIDIV_Msk) >> RCC_CR_HSIDIV_Pos;
        return 64000000U >> hsidiv;
    }
    if (sel == 1U) { return 4000000U; }     /* csi_ker_ck */
    if (sel == 2U) { return HSE_HZ; }       /* hse_ck, 22 MHz on this board */

    return 0U;                              /* 11 = per_ck disabled */
}

static uint32_t boost_for(uint32_t hz)
{
    /*
     * The four-way BOOST of revision V silicon - this part measured
     * REV_ID 0x2003, and the device header agrees about the shape of the
     * field: ADC_CR_BOOST_Msk is two bits wide here, where rev Y has a
     * single bit with a different meaning. Getting this wrong costs
     * accuracy rather than function, which is the kind of error that
     * never announces itself, so it is computed from the clock this
     * driver actually ended up with.
     */
    if (hz <=  6250000U) { return 0U; }
    if (hz <= 12500000U) { return 1U; }
    if (hz <= 25000000U) { return 2U; }
    return 3U;
}

uint32_t adc_kernel_hz(void) { return s_kernel_hz; }

/* ------------------------------------------------------------------ */
/* Bring-up                                                           */
/* ------------------------------------------------------------------ */

static int stop_whatever_was_running(ADC_TypeDef *adc)
{
    uint32_t guard;

    /*
     * TRAP 16: SOMEBODY ELSE WAS HERE FIRST. Loading over SWD resets
     * nothing, and the factory firmware reads these buttons - it is how
     * its own DFU gesture works - so ADC1 can arrive already enabled,
     * already converting, and with a calibration factor belonging to a
     * configuration this driver knows nothing about. The DMA controllers
     * taught this lesson at the cost of an evening; the fix is to leave
     * the peripheral in a known state rather than hope.
     */
    if (adc->CR & ADC_CR_ADSTART) {
        adc->CR |= ADC_CR_ADSTP;
        for (guard = 0; guard < WAIT_LIMIT
                        && (adc->CR & ADC_CR_ADSTART); guard++) { }
        if (adc->CR & ADC_CR_ADSTART) { return ADC_ERR_BUSY; }
    }

    if (adc->CR & ADC_CR_JADSTART) {
        adc->CR |= ADC_CR_JADSTP;
        for (guard = 0; guard < WAIT_LIMIT
                        && (adc->CR & ADC_CR_JADSTART); guard++) { }
    }

    if (adc->CR & ADC_CR_ADEN) {
        adc->CR |= ADC_CR_ADDIS;
        for (guard = 0; guard < WAIT_LIMIT
                        && (adc->CR & ADC_CR_ADEN); guard++) { }
        if (adc->CR & ADC_CR_ADEN) { return ADC_ERR_BUSY; }
    }

    return ADC_OK;
}

/*
 * A PERIPHERAL RESET, WHICH IS THE ONLY WAY OUT OF A STUCK CALIBRATION.
 *
 * ADEN cannot be set while ADCAL is still high, and ADCAL is cleared by
 * the hardware and not by us - so a calibration that never finishes
 * leaves the converter unusable and no bit to clear. The way out is to
 * reset ADC1 and ADC2 through RCC and start again, which is also the
 * cleanest answer to finding the peripheral already enabled by somebody
 * else's firmware. Nothing else in this port uses either ADC.
 */
static void adc12_reset(void)
{
    RCC->AHB1RSTR |= RCC_AHB1RSTR_ADC12RST;
    (void)RCC->AHB1RSTR;
    spin_us(2);
    RCC->AHB1RSTR &= ~RCC_AHB1RSTR_ADC12RST;
    (void)RCC->AHB1RSTR;
    spin_us(2);
}

/* ADC3 sits in a different power domain with its own reset and its own
 * enable - D3 rather than D2 - which is the whole of the difference
 * between the two converters as far as this driver is concerned. */
static void adc3_reset(void)
{
    RCC->AHB4RSTR |= RCC_AHB4RSTR_ADC3RST;
    (void)RCC->AHB4RSTR;
    spin_us(2);
    RCC->AHB4RSTR &= ~RCC_AHB4RSTR_ADC3RST;
    (void)RCC->AHB4RSTR;
    spin_us(2);
}

static int bringup(ADC_TypeDef *adc, int do_cal)
{
    uint32_t guard;
    int      rc;

    rc = stop_whatever_was_running(adc);
    if (rc != ADC_OK) { return rc; }

    /*
     * Out of deep power-down FIRST, then the internal regulator. The
     * order is not cosmetic: while DEEPPWD is set the regulator cannot
     * be enabled at all, so doing these in one write - or in the other
     * order - leaves ADVREGEN reading back as zero and every conversion
     * afterwards returns nothing.
     *
     * Which is why this is checked rather than assumed. There is no
     * "regulator ready" flag on this part; the datasheet gives a start-up
     * time and ST's own driver waits it out. So: wait twice as long as
     * the 10 us it asks for, then read the two bits back and refuse to
     * continue if they do not say what was written.
     */
    adc->CR &= ~ADC_CR_DEEPPWD;
    adc->CR |= ADC_CR_ADVREGEN;
    spin_us(20);

    adc_cr_after_vreg = adc->CR;

    if ((adc->CR & ADC_CR_DEEPPWD)
        || !(adc->CR & ADC_CR_ADVREGEN)) {
        return ADC_ERR_LDO;
    }

    adc->CR = (adc->CR & ~ADC_CR_BOOST_Msk)
             | (boost_for(s_kernel_hz) << ADC_CR_BOOST_Pos);

    if (do_cal) {
        /*
         * OFFSET CALIBRATION ONLY, AND BOUNDED IN TIME.
         *
         * Two corrections to the first version of this, both of which
         * the radio pointed out. It asked for the LINEARITY pass as well
         * (ADCALLIN), which is a far longer job and which the factory
         * firmware had already left requested in CR; and it bounded the
         * wait by COUNTING LOOP PASSES, which is not a time limit - the
         * exact mistake that put 46 seconds of polling inside a 32-second
         * watchdog window and switched the radio off. A wait that must
         * not become the thing that breaks the radio is measured in
         * milliseconds, by the millisecond counter.
         *
         * Calibration is also not allowed to be fatal any more. It
         * trims offset; without it a conversion is a few counts out,
         * against button bands thousands of counts wide. Refusing to
         * start the ADC over a few counts is the wrong trade, so a
         * failure here returns and the caller tries again without it.
         */
        adc->CR &= ~ADC_CR_ADCALDIF;
        adc->CR &= ~ADC_CR_ADCALLIN;
        adc->CR |= ADC_CR_ADCAL;

        uint32_t t0 = board_millis();

        while ((adc->CR & ADC_CR_ADCAL)
               && (board_millis() - t0) < 50U) {
            board_watchdog_feed();
        }

        adc_cal_ms       = board_millis() - t0;
        adc_cr_after_cal = adc->CR;

        if (adc->CR & ADC_CR_ADCAL) { return ADC_ERR_CAL; }

        adc_cal_factor = adc->CALFACT;

        /* A few ADC clock cycles have to pass between calibration and
         * ADEN, and the ADC clock is the slow one here. */
        spin_us(4);
    } else {
        adc_cal_factor = 0U;
    }

    /*
     * 16-bit, single conversion, software triggered, and OVRMOD set so a
     * result nobody collected is overwritten instead of blocking the
     * next one. A converter that stops because of a sample we did not
     * want is a button that stops answering.
     */
    adc->CFGR = ADC_CFGR_OVRMOD;       /* RES = 000 = 16 bits */

    uint32_t smp = 0;
    for (uint32_t i = 0; i < 10U; i++) { smp |= (SMP_LONG << (i * 3U)); }
    adc->SMPR1 = smp;
    adc->SMPR2 = smp;

    /*
     * PCSEL, which has no equivalent on the F4 and is easy to leave out:
     * on this part each channel's input path has to be preselected
     * before it will convert. A missing bit here reads as a flat,
     * plausible, wrong number.
     */
    adc->PCSEL = (1UL << ADC_CH_ENC_BUT) | (1UL << ADC_CH_BAT);

    adc->ISR = ADC_ISR_ADRDY;
    adc->CR |= ADC_CR_ADEN;

    /*
     * ADRDY IS ALSO THE CLOCK TEST, and that is why it is worth having a
     * separate error code for. The flag is produced by the ADC's own
     * kernel clock, so if it never appears the converter is not being
     * clocked - which points at ADCSEL and per_ck rather than at
     * anything in this function.
     */
    for (guard = 0; guard < WAIT_LIMIT && !(adc->ISR & ADC_ISR_ADRDY);
         guard++) {
        board_watchdog_feed();
    }

    adc_isr_after_aden = adc->ISR;

    if (!(adc->ISR & ADC_ISR_ADRDY)) { return ADC_ERR_RDY; }

    adc->ISR = ADC_ISR_ADRDY;

    return ADC_OK;
}

/*
 * The prescaler, per common block. Both converters run off the same
 * kernel clock mux but each has its own prescaler, so this is called
 * once for ADC12 and once for ADC3 - and it is a function rather than
 * two copies because a second copy of "which divider lands under
 * 25 MHz" is a second place to get it wrong.
 */
static void presc_setup(ADC_Common_TypeDef *cc)
{
    uint32_t src   = per_ck_hz();
    uint32_t presc = k_presc[0].code;

    s_kernel_hz = src;

    for (uint32_t i = 0; i < sizeof k_presc / sizeof k_presc[0]; i++) {
        if (src / k_presc[i].div <= 25000000U) {
            presc       = k_presc[i].code;
            s_kernel_hz = src / k_presc[i].div;
            break;
        }
    }

    cc->CCR = (cc->CCR & ~(ADC_CCR_CKMODE_Msk | ADC_CCR_PRESC_Msk))
            | ((uint32_t)presc << ADC_CCR_PRESC_Pos);
    /* CKMODE left at 00 = the asynchronous clock selected above. */
}

/*
 * WHICH KERNEL CLOCK, AND WHETHER TO CALIBRATE: three attempts, in order,
 * each one a full reset followed by the whole sequence.
 *
 * This is not belt-and-braces, it is a bench instrument. The first run
 * of this driver failed at calibration and the console could say only
 * that - it could not say whether the converter was unclocked, whether
 * the factory firmware's leftover state was in the way, or whether the
 * linearity pass it had asked for was simply slower than the wait. Three
 * attempts whose results are all recorded answer that in one run on the
 * radio instead of three, and a bench run costs somebody's attention.
 *
 *   0  per_ck, calibrated     what it should be
 *   1  per_ck, no calibration says calibration specifically is the problem
 *   2  HSE, no calibration    says per_ck was not reaching the converter
 *
 * ADRDY is the clock test: the flag comes out of the ADC's own kernel
 * clock, so attempt 2 succeeding where 1 failed points at the clock mux
 * and nowhere else.
 */
int adc_init(void)
{
    int rc = ADC_ERR_RDY;

    RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
    (void)RCC->APB4ENR;
    adc_found_pmcr = SYSCFG->PMCR;

    /*
     * PMCR IS WRITTEN NOW, AND IT USED TO BE ONLY READ. That change
     * cost a day and is the most useful thing in this file.
     *
     * PA0 and PA1 each have a second, analog-only pad - PA0_C, PA1_C -
     * with its own ADC channel and an analog switch between the two.
     * This register holds those switches, and the FACTORY firmware sets
     * it before we ever run: loading over SWD resets nothing.
     *
     * It was read and respected, on the grounds that an ADC driver has
     * no business deciding whether an analog line reaches a GPIO. The
     * grounds were sound and the conclusion was wrong, because the value
     * is not stable: 0x01000000 one day, 0x03000000 the next, after a
     * power cycle let the factory firmware start from cold. The bit that
     * moved was PA1SO. With the switch closed, ENC_BUT's pad is mirrored
     * onto channel 1; with it open, channel 1 is a floating pad - and a
     * driver reading channel 1 reported every button held down for ever,
     * correctly, about a pin connected to nothing.
     *
     * So both switches are OPENED here, deliberately and every time.
     * That isolates the _C pads, which this board does not use, and
     * leaves each net on the plain pad it is actually bonded to:
     * ENC_BUT on channel 17, ADC_IN on channel 16. Nothing downstream
     * then depends on what somebody else's firmware was doing.
     *
     * Opening a switch is also the safe direction: it DISCONNECTS, where
     * closing one would hang an analog line on a GPIO somebody else may
     * drive. The old comment's caution applies to the opposite move.
     */
    SYSCFG->PMCR |= SYSCFG_PMCR_PA0SO | SYSCFG_PMCR_PA1SO;
    (void)SYSCFG->PMCR;

    RCC->AHB1ENR |= RCC_AHB1ENR_ADC12EN;
    (void)RCC->AHB1ENR;

    adc_found_cr = ADC1->CR;

    /*
     * NOT TRIED is not the same as SUCCEEDED, and zero cannot say both.
     * The first version left this array at its initial zero, so the
     * console reported "per_ck+cal 0, per_ck 0, HSE 0" when only the
     * first had run - three successes where there had been one. A result
     * slot starts out holding a value that no attempt can return.
     */
    for (uint8_t i = 0; i < (uint8_t)ADC_ATTEMPTS; i++) {
        adc_attempt_rc[i] = ADC_NOT_TRIED;
    }

    for (uint8_t att = 0; att < (uint8_t)ADC_ATTEMPTS; att++) {
        /*
         * A reset before each attempt, so attempt N+1 never inherits
         * attempt N's half-configured converter - nor the factory
         * firmware's, which arrives enabled with the linearity
         * calibration already requested.
         */
        adc12_reset();

        if (att == 2U || per_ck_hz() == 0U) {
            /* per_ck pointed at HSE: the 22 MHz module the whole radio
             * already runs on, so nothing new has to be started. */
            RCC->D1CCIPR = (RCC->D1CCIPR & ~RCC_D1CCIPR_CKPERSEL_Msk)
                         | (2UL << RCC_D1CCIPR_CKPERSEL_Pos);
        }

        RCC->D3CCIPR = (RCC->D3CCIPR & ~RCC_D3CCIPR_ADCSEL_Msk)
                     | (2UL << RCC_D3CCIPR_ADCSEL_Pos);   /* 10 = per_ck */

        presc_setup(ADC12_COMMON);
        adc_ccr = ADC12_COMMON->CCR;

        rc = bringup(ADC1, att == 0U);

        adc_attempt_rc[att] = rc;
        adc_attempt_used    = att;

        if (rc == ADC_OK) { break; }
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* ADC3 and the internal reference                                    */
/* ------------------------------------------------------------------ */

/*
 * WHY THE RAIL HAS TO BE MEASURED RATHER THAN NAMED.
 *
 * Every voltage this driver reports is a fraction of VREF+, so VREF+ is
 * a multiplier on all of them. It was a constant - 3280 mV, from one
 * multimeter reading on one afternoon - and a constant cannot follow the
 * rail as it sags under transmit current, warms up, or differs between
 * two boards. Upstream's battery.c is a long account of what a named
 * rail cost it: 3300 mV written over a 3.0 V regulator, a systematic
 * ten per cent on every unit, hidden for months behind a diode constant
 * that had been fudged to cancel half of it.
 *
 * The part carries its own answer. VREFINT is a bandgap near 1.2 V that
 * does NOT move with the rail, so converting it against the rail gives
 * the rail:
 *
 *     VDDA = VREFINT_mV * 65535 / raw
 *
 * On this chip the internal channels are wired to ADC3, which is why
 * there is a second converter in this file at all.
 */
static uint32_t          s_vrefint_ch = 19U; /* settled by measurement */
static uint32_t          s_temp_ch    = 18U; /* likewise */
static volatile uint32_t s_vref_raw;        /* last VREFINT conversion */
static volatile uint32_t s_temp_raw;        /* last thermometer conversion */
static uint8_t           s_adc3_slot;       /* which of the two is converting */
static volatile uint32_t s_vdda_mv;         /* 0 until it has been measured */
static uint8_t           s_adc3_pending;
static int               s_adc3_rc = ADC_ERR_RDY;

uint32_t adc3_status(void)   { return (uint32_t)(-s_adc3_rc); }
uint32_t adc_vref_raw(void)  { return s_vref_raw >> 5; }

/*
 * THE FACTORY CALIBRATION, AND HOW IT IS RECOGNISED RATHER THAN TRUSTED.
 *
 * ST measures VREFINT on every part at 3.3 V and 30 C and leaves the
 * reading in system memory. Using it removes the bandgap's own spread -
 * a couple of per cent, part to part - and turns this from "about 1.2 V"
 * into a number measured on THIS die:
 *
 *     VDDA = 3300 * VREFIN_CAL / raw
 *
 * The address is a datasheet fact and there is no datasheet in docs/, so
 * it is not trusted on sight. It is RANGE-CHECKED instead, and the range
 * is narrow enough to be an identification: a 16-bit conversion of a
 * 1.18-1.24 V bandgap against a 3.3 V reference can only land between
 * about 23 400 and 24 700 counts. A wrong address gives 0xFFFF, zero, or
 * a pointer-shaped number, and all of those fall outside.
 *
 * If it does not look like a calibration value, the nominal 1.216 V is
 * used instead and adc_vref_from_cal() says which happened - so the
 * console can show both answers side by side, and they can be compared
 * against the meter.
 */
#define VREFIN_CAL_ADDR     0x1FF1E860UL
#define VREFINT_NOMINAL_MV  1216U
#define VREFIN_CAL_LO       23400U
#define VREFIN_CAL_HI       24700U

static uint8_t s_cal_used;

uint32_t adc_vrefin_cal(void)
{
    return *(volatile const uint16_t *)VREFIN_CAL_ADDR;
}

uint8_t adc_vref_from_cal(void) { return s_cal_used; }

static uint32_t vdda_from(uint32_t raw)
{
    uint32_t cal;

    if (raw == 0U) { return 0U; }

    cal = adc_vrefin_cal();

    if (cal >= VREFIN_CAL_LO && cal <= VREFIN_CAL_HI) {
        s_cal_used = 1U;
        return (3300U * cal) / raw;
    }

    s_cal_used = 0U;
    return ((uint32_t)VREFINT_NOMINAL_MV * 65535U) / raw;
}

uint32_t adc_vdda_mv(void)
{
    /*
     * Falls back to the measured-by-meter constant until the first
     * conversion lands, which is one millisecond after sampling starts.
     * Returning zero here would put a zero into every voltage on the
     * screen for that millisecond; returning the old constant is wrong
     * by less than a per cent and only for an instant.
     */
    uint32_t v = s_vdda_mv;

    return (v != 0U) ? v : (uint32_t)ADC_VREF_MV;
}

uint32_t adc_ts_cal1(void) { return *(volatile const uint16_t *)TS_CAL1_ADDR; }
uint32_t adc_ts_cal2(void) { return *(volatile const uint16_t *)TS_CAL2_ADDR; }
uint32_t adc_temp_raw(void) { return s_temp_raw >> 5; }

int32_t adc_temp_c10(void)
{
    uint32_t c1 = adc_ts_cal1();
    uint32_t c2 = adc_ts_cal2();
    uint32_t raw = s_temp_raw >> 5;

    /*
     * THE CALIBRATION HAS TO LOOK LIKE A CALIBRATION, and this is the
     * same plausibility test the reference's value gets - see
     * TS_CAL1_ADDR for the arithmetic that makes these two numbers
     * identifiable. A slope outside 30..80 counts per degree is not a
     * silicon thermometer, whatever it is.
     */
    if (raw == 0U || c2 <= c1) { return ADC_TEMP_NONE; }

    uint32_t span = c2 - c1;

    /* 30 to 80 counts per degree. The real figure is 50.5, so this is
     * wide enough not to reject a different die and narrow enough to
     * reject anything that is not a thermometer's calibration. The
     * first version of this line divided the lower bound by ten and
     * would have accepted 3 counts per degree, which is a bound that
     * accepts nearly everything - a check that cannot fail is not a
     * check. */
    if (span < 30U * (TS_CAL2_C - TS_CAL1_C)
        || span > 80U * (TS_CAL2_C - TS_CAL1_C)) {
        return ADC_TEMP_NONE;
    }

    /*
     * BOTH CALIBRATION POINTS WERE TAKEN AT 3.3 V, so a reading taken
     * against a different rail has to be referred back to 3.3 V before
     * it can be compared with them. Skipping this is the classic way to
     * get a thermometer that moves when the battery sags: at 3.28 V the
     * same die temperature reads about half a degree high.
     */
    uint32_t at_3v3 = (raw * adc_vdda_mv()) / 3300U;

    int32_t num = ((int32_t)at_3v3 - (int32_t)c1)
                * ((TS_CAL2_C - TS_CAL1_C) * 10);

    return (TS_CAL1_C * 10) + num / (int32_t)span;
}

int adc3_init(uint32_t vrefint_channel)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_ADC3EN;
    (void)RCC->AHB4ENR;

    adc3_reset();

    presc_setup(ADC3_COMMON);

    /*
     * VREFEN connects the bandgap to its channel. It is in the COMMON
     * block, not in the converter, and it has a start-up time of its own
     * - the reference has to settle before the first conversion means
     * anything, which is what the delay below is for and why it is not
     * the microsecond that the register write takes.
     */
    ADC3_COMMON->CCR |= ADC_CCR_VREFEN | ADC_CCR_TSEN;
    spin_us(100);

    s_vrefint_ch = vrefint_channel;
    s_adc3_rc    = bringup(ADC3, 1);

    return s_adc3_rc;
}

/* ------------------------------------------------------------------ */
/* Conversions                                                        */
/* ------------------------------------------------------------------ */

static void start_one(ADC_TypeDef *adc, uint32_t ch)
{
    /*
     * PCSEL IS SET TO EXACTLY THIS CHANNEL, not OR-ed into whatever was
     * there before.
     *
     * The first version accumulated, and the channel scan - which reads
     * all twenty - therefore ended with every input path preselected at
     * once. The battery channel read 912 mV that way against 1530 mV
     * with two channels selected, a 40 % difference in a number printed
     * with units, so the scan was measuring its own configuration. One
     * conversion wants one channel preselected; anything else is a
     * question about the ADC rather than about the board.
     */
    adc->PCSEL  = (1UL << ch);
    adc->SQR1   = (ch << ADC_SQR1_SQ1_Pos);     /* L = 0: one conversion */
    adc->ISR    = ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
    adc->CR    |= ADC_CR_ADSTART;
}

/* One polled conversion on any of them, bounded. Negative means it did
 * not happen, which is the reason this is not a uint16_t. */
static int32_t read_one(ADC_TypeDef *adc, uint32_t ch)
{
    uint32_t guard;

    if (adc->CR & ADC_CR_ADSTART) { return -3; }

    start_one(adc, ch);

    for (guard = 0; guard < WAIT_LIMIT && !(adc->ISR & ADC_ISR_EOC);
         guard++) { }

    if (!(adc->ISR & ADC_ISR_EOC)) {
        adc_timeouts++;
        return -4;
    }

    adc_conversions++;

    return (int32_t)(adc->DR & 0xFFFFU);        /* reading DR clears EOC */
}

int32_t adc3_read(uint32_t ch)
{
    if (ch > 19U) { return -1; }
    if (s_adc3_rc != ADC_OK) { return -5; }

    /*
     * Unlike adc_read(), this does NOT refuse while sampling is live -
     * it cannot clash with the tick's own ADC3 conversion, because the
     * tick only starts one when the converter is idle and only collects
     * one it started. A conversion this function takes in between is
     * simply one the tick skips.
     */
    return read_one(ADC3, ch);
}

/*
 * A polled read that does NOT refuse while the sampler is live.
 *
 * adc_read() stands aside when the tick owns the converter, which is
 * right for a test but useless for a diagnostic that has to look at a
 * running radio. This one takes its turn between the tick's
 * conversions: the worst it can do is make the tick skip a sample,
 * which costs one millisecond of staleness on a button that is
 * debounced over twenty.
 */
int32_t adc_read_any(uint32_t ch)
{
    if (ch > 19U) { return -1; }

    return read_one(ADC1, ch);
}

int32_t adc_read(uint32_t ch)
{
    if (ch > 19U) { return -1; }

    /*
     * The tick sampler and this function must not share the converter -
     * whoever reads DR second gets the other one's channel. Rather than
     * lock anything, this refuses while sampling is live: the only
     * caller is a bring-up test, and tests run before the sampler
     * starts.
     */
    if (s_live) { return -2; }

    return read_one(ADC1, ch);
}

uint32_t adc_mv(uint32_t raw)
{
    /*
     * 65535 counts = VREF+, and VREF+ is now MEASURED - against the
     * part's own bandgap, a millisecond at a time, so it follows the
     * rail instead of describing the afternoon it was calibrated on.
     * adc_vdda_mv() falls back to the old constant only until the first
     * conversion lands.
     */
    return (raw * adc_vdda_mv()) / 65535U;
}

/* ------------------------------------------------------------------ */
/* The battery                                                        */
/* ------------------------------------------------------------------ */

/*
 * A LITHIUM CELL'S USEFUL RANGE, and both numbers are conventions rather
 * than measurements of THIS cell.
 *
 * 4200 mV is a full lithium-polymer cell by its chemistry. 3300 mV is
 * where this radio should be considered empty - the cell itself goes
 * lower, but the switching regulators and the 3.3 V rail they feed do
 * not, so running it down further buys nothing and costs cycle life.
 */
#define BAT_EMPTY_MV        3300U
#define BAT_FULL_MV         4200U

uint32_t adc_bat_mv(void)
{
    uint32_t mv = adc_mv(s_raw[1]) * ADC_BAT_DIVIDER;

    /* The divider's own tolerance, which the reference cannot see - see
     * ADC_BAT_TRIM_PPT. Signed, so it has to go through int32_t, and the
     * multiply happens before the divide or a 0.4 % trim rounds to
     * nothing. */
    return (uint32_t)(((int32_t)mv * (1000 + ADC_BAT_TRIM_PPT)) / 1000);
}

uint8_t adc_bat_pct(void)
{
    uint32_t mv = adc_bat_mv();

    /*
     * A zero reading means the sampler has not run yet, not a flat
     * battery, and those must not look the same. Below the empty line
     * the answer is 0 either way, so this only matters for how the
     * caller may choose to read it.
     */
    if (mv <= BAT_EMPTY_MV) { return 0U; }
    if (mv >= BAT_FULL_MV)  { return 100U; }

    return (uint8_t)(((mv - BAT_EMPTY_MV) * 100U)
                     / (BAT_FULL_MV - BAT_EMPTY_MV));
}

/* ------------------------------------------------------------------ */
/* The buttons                                                        */
/* ------------------------------------------------------------------ */

/*
 * THE THRESHOLDS ARE FRACTIONS OF FULL SCALE, AND THAT IS THE WHOLE
 * POINT.
 *
 * The ladder's top resistor goes to the same 3.3 V rail that feeds
 * VREF+. So when the rail sags - a fresh battery against a flat one, a
 * transmit burst, a cold regulator - the ladder's voltages and the ADC's
 * full scale move TOGETHER, and their ratio does not move at all. Doing
 * this in millivolts would import the regulator's tolerance into a
 * decision that is immune to it, and upstream's battery.c is a long
 * account of what happens when a rail's real voltage is assumed.
 *
 * MEASURED ON THE RADIO, 05/11/2026, and they are NOT what the
 * schematic's resistor values predict:
 *
 *                  measured              predicted from 47k/68k/22k
 *   idle        65535  (3300 mV)          65535
 *   big knob    49623  (2498 mV)          38750
 *   small knob  32863  (1655 mV)          20894
 *   both        28271  (1423 mV)          17128
 *
 * Every press reads HIGHER than the arithmetic says, so the 68k and 22k
 * read off page 3 are not what is fitted here. Working backwards from
 * the measurement, the two branches are about 3.11 and 0.99 times the
 * top resistor - 146k and 47k if the top one really is 47k.
 *
 * WHAT MAKES THIS A MEASUREMENT AND NOT THREE NUMBERS: the third
 * plateau predicts itself. If "both" is the two branches in parallel,
 * its voltage follows from the other two with no resistor value needed
 * anywhere - the scale cancels - and that arithmetic gives 1418 mV
 * against 1423 measured, a fifth of a per cent. So these three plateaus
 * really are one button, the other button, and both, and no part of that
 * conclusion rests on the schematic or on VREF+ being 3.3 V.
 *
 * WHICH KNOB IS WHICH was settled the only way it can be: the hand
 * pressed the big knob first, the small one second and both third, and
 * the plateaus arrived in that order. "ENC1" below means the big knob,
 * the one that tunes.
 *
 * Boundaries sit halfway between neighbours, with a dead band around
 * each one: a sample that lands inside a dead band keeps the previous
 * answer. That is hysteresis against ADC noise and has nothing to do
 * with contact bounce, which is upstream's 20 ms debounce integrator's
 * job and is left entirely to it. The narrowest gap here is 4592
 * counts, so 1000 of dead band costs nothing.
 */
#define BAND_NONE_ENC1      57579U       /* 65535 .. 49623 */
#define BAND_ENC1_ENC2      41243U       /* 49623 .. 32863 */
#define BAND_ENC2_BOTH      30567U       /* 32863 .. 28271 */
#define BAND_DEAD            1000U       /* ~1.5 % of full scale */

static uint8_t classify(uint32_t raw, uint8_t prev)
{
    uint8_t now;

    if      (raw > BAND_NONE_ENC1) { now = 0U; }
    else if (raw > BAND_ENC1_ENC2) { now = ADC_BTN_ENC1; }
    else if (raw > BAND_ENC2_BOTH) { now = ADC_BTN_ENC2; }
    else                           { now = ADC_BTN_ENC1 | ADC_BTN_ENC2; }

    if (now == prev) { return now; }

    /* Changed class: only believe it outside the dead band. */
    if ((raw > BAND_NONE_ENC1 - BAND_DEAD && raw < BAND_NONE_ENC1 + BAND_DEAD)
     || (raw > BAND_ENC1_ENC2 - BAND_DEAD && raw < BAND_ENC1_ENC2 + BAND_DEAD)
     || (raw > BAND_ENC2_BOTH - BAND_DEAD && raw < BAND_ENC2_BOTH + BAND_DEAD))
    {
        return prev;
    }

    return now;
}

void adc_sampling(int on)
{
    if (on) {
        s_pending = 0U;
        s_slot    = 0U;
    }
    s_live = (uint8_t)(on ? 1 : 0);
}

void adc_tick(void)
{
    adc_ticks++;

    if (!s_live) { return; }

    adc_ticks_live++;

    /*
     * NOTHING HERE WAITS. A conversion takes about 25 us and a tick is a
     * millisecond, so by the time this runs again the result is long
     * since sitting in DR. If it somehow is not, the tick gives up its
     * turn and tries again next millisecond - which is what the ADSTART
     * test below is, and it is also why adc_timeouts exists: a converter
     * that has stopped answering must show up as a number, not as a
     * button that is merely a bit sticky.
     */
    if (ADC1->CR & ADC_CR_ADSTART) { return; }

    if (s_pending) {
        if (ADC1->ISR & ADC_ISR_EOC) {
            uint32_t v = ADC1->DR & 0xFFFFU;

            s_raw[s_slot] = v;
            adc_conversions++;

            if (s_slot == 0U) { s_btn = classify(v, s_btn); }
        } else {
            adc_timeouts++;
        }

        s_pending = 0U;
        s_slot    = (uint8_t)(s_slot ^ 1U);
    }

    start_one(ADC1, (s_slot == 0U) ? ADC_CH_ENC_BUT : ADC_CH_BAT);
    s_pending = 1U;

    /*
     * ADC3 RUNS ALONGSIDE, NOT AFTER. It is a separate converter in a
     * separate power domain, so its conversion overlaps ADC1's and
     * costs this handler nothing but the register writes - the same
     * start-now-collect-next-tick shape, for the same reason: nothing
     * in a SysTick handler is allowed to wait.
     *
     * VREFINT every millisecond is far more often than a power rail can
     * move, and that is deliberate. The bandgap is a small signal at
     * 16 bits and a single conversion carries a few counts of noise, so
     * the reading is smoothed below rather than averaged in a burst.
     */
    if (s_adc3_rc == ADC_OK && !(ADC3->CR & ADC_CR_ADSTART)) {
        if (s_adc3_pending && (ADC3->ISR & ADC_ISR_EOC)) {
            uint32_t v = ADC3->DR & 0xFFFFU;

            /*
             * A first-order filter with a shift of 5: about sixty
             * milliseconds of memory now that two channels share the
             * converter. Started from the first sample rather than from
             * zero, or the rail would appear to climb from nothing over
             * the first tenth of a second and every voltage on the
             * screen with it.
             *
             * The thermometer wants the smoothing more than the
             * reference does: a die temperature cannot change in a
             * millisecond, so every bit of movement at that rate is
             * noise.
             */
            volatile uint32_t *acc = (s_adc3_slot == 0U) ? &s_vref_raw
                                                         : &s_temp_raw;

            if (*acc == 0U) { *acc = v << 5; }
            else { *acc += v - (*acc >> 5); }

            if (s_adc3_slot == 0U) {
                s_vdda_mv = vdda_from(s_vref_raw >> 5);
            }

            s_adc3_slot ^= 1U;
        }

        start_one(ADC3, (s_adc3_slot == 0U) ? s_vrefint_ch : s_temp_ch);
        s_adc3_pending = 1U;
    }
}

uint32_t adc_raw_enc_but(void) { return s_raw[0]; }
uint32_t adc_raw_bat(void)     { return s_raw[1]; }

uint8_t  adc_buttons(void)     { return s_btn; }
uint8_t  adc_enc1_down(void)   { return (uint8_t)((s_btn & ADC_BTN_ENC1) ? 1U : 0U); }
uint8_t  adc_enc2_down(void)   { return (uint8_t)((s_btn & ADC_BTN_ENC2) ? 1U : 0U); }
