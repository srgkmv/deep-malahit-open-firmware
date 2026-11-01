#include "stm32h7xx.h"
#include "nau8822.h"
#include "board.h"

/*
 * SPI6 at about 3.75 MHz.
 *
 * The datasheet's control timing gives TSCK >= 80 ns, so 12.5 MHz is the
 * ceiling; 3.75 MHz leaves a factor of three in hand on a bus that only
 * ever carries a few dozen configuration words. There is nothing to gain
 * from going faster and a 33 ohm series network on every line to lose it
 * in.
 *
 * SPI6 lives in the D3 domain on APB4, so its kernel clock is pclk4 at
 * 120 MHz and MBR = 101 divides by 64, giving 1.875 MHz. Slower still,
 * and still irrelevant: the whole codec configuration is under a
 * millisecond.
 */
#define SPI6_MBR_DIV64      5UL

/* PB6 = SPI6_CODEC_CS, plain GPIO. */
#define CS_PORT             GPIOB
#define CS_PIN              6U

static uint16_t s_shadow[128];

/*
 * The datasheet's reset values, so that a shadow read before the first
 * write returns what the hardware actually holds rather than zero. Only
 * the registers whose reset value is non-zero are listed.
 */
static void shadow_reset_values(void)
{
    for (uint32_t i = 0; i < 128U; i++) { s_shadow[i] = 0U; }

    s_shadow[NAU_IFACE]       = 0x050U;
    s_shadow[NAU_CLK1]        = 0x140U;
    s_shadow[NAU_DACVOL_L]    = 0x0FFU;
    s_shadow[NAU_DACVOL_R]    = 0x0FFU;
    s_shadow[NAU_ADC_CTRL]    = 0x100U;
    s_shadow[NAU_ADCVOL_L]    = 0x0FFU;
    s_shadow[NAU_ADCVOL_R]    = 0x0FFU;
    s_shadow[18]              = 0x12CU;   /* EQ1 */
    s_shadow[19]              = 0x02CU;   /* EQ2 */
    s_shadow[20]              = 0x02CU;   /* EQ3 */
    s_shadow[21]              = 0x02CU;   /* EQ4 */
    s_shadow[22]              = 0x02CU;   /* EQ5 */
    s_shadow[24]              = 0x032U;   /* DAC limiter 1 */
    s_shadow[32]              = 0x038U;   /* ALC 1 */
    s_shadow[33]              = 0x00BU;   /* ALC 2 */
    s_shadow[34]              = 0x032U;   /* ALC 3 */
    s_shadow[35]              = 0x010U;   /* noise gate */
    s_shadow[36]              = 0x008U;   /* PLL N */
    s_shadow[37]              = 0x00CU;   /* PLL K1 */
    s_shadow[38]              = 0x093U;   /* PLL K2 */
    s_shadow[39]              = 0x0E9U;   /* PLL K3 */
    s_shadow[NAU_INPUT_CTRL]  = 0x033U;
    s_shadow[NAU_PGA_L]       = 0x010U;
    s_shadow[NAU_PGA_R]       = 0x010U;
    s_shadow[NAU_BOOST_L]     = 0x100U;
    s_shadow[NAU_BOOST_R]     = 0x100U;
    s_shadow[NAU_OUTPUT_CTRL] = 0x002U;
    s_shadow[NAU_MIXER_L]     = 0x001U;
    s_shadow[NAU_MIXER_R]     = 0x001U;
    s_shadow[NAU_HPVOL_L]     = 0x039U;
    s_shadow[NAU_HPVOL_R]     = 0x039U;
    s_shadow[NAU_SPKVOL_L]    = 0x039U;
    s_shadow[NAU_SPKVOL_R]    = 0x039U;
    s_shadow[56]              = 0x001U;   /* AUX2 mixer */
    s_shadow[57]              = 0x001U;   /* AUX1 mixer */
    s_shadow[60]              = 0x020U;   /* misc */
}

static void gpio_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (2UL << (pin * 2U));
    port->OSPEEDR |= (2UL << (pin * 2U));            /* high speed */
    port->OTYPER &= ~(1UL << pin);
    port->PUPDR &= ~(3UL << (pin * 2U));
    port->AFR[pin >> 3U] =
          (port->AFR[pin >> 3U] & ~(0xFUL << ((pin & 7U) * 4U)))
        | (af << ((pin & 7U) * 4U));
}

void nau8822_bus_init(void)
{
    shadow_reset_values();

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN;
    RCC->APB4ENR |= RCC_APB4ENR_SPI6EN;
    (void)RCC->APB4ENR;

    /*
     * Say which kernel clock. 000 = rcc_pclk4, which is what we want and
     * also the reset value - but an image loaded over SWD inherits
     * whatever the factory firmware chose, and this project has already
     * paid for that assumption once on SPI3.
     */
    RCC->D3CCIPR &= ~RCC_D3CCIPR_SPI6SEL_Msk;

    /* CS idle high BEFORE the pin becomes an output, so the codec never
     * sees a low-going edge it could read as the start of a word. */
    CS_PORT->BSRR = (1UL << CS_PIN);
    CS_PORT->MODER = (CS_PORT->MODER & ~(3UL << (CS_PIN * 2U)))
                   | (1UL << (CS_PIN * 2U));
    CS_PORT->OTYPER &= ~(1UL << CS_PIN);
    CS_PORT->OSPEEDR |= (1UL << (CS_PIN * 2U));
    CS_PORT->PUPDR &= ~(3UL << (CS_PIN * 2U));

    /*
     * PB3 IS JTDO. Handing it to SPI6 gives up the SWO trace pin for
     * good - which costs nothing here, because the debug path is 2-wire
     * SWD and `swd.sh` has never used trace, but it is worth knowing
     * before someone wonders why printf-over-SWO will not come up.
     */
    gpio_af(GPIOB, 3U, 8U);    /* SPI6_SCK  */
    gpio_af(GPIOB, 5U, 8U);    /* SPI6_MOSI */

    SPI6->CR1  = 0U;
    SPI6->IFCR = 0xFFFFFFFFU;

    /*
     * SSI BEFORE MASTER, for the reason spi3.c explains at length: with
     * software slave management the master reads its own NSS from
     * CR1.SSI, and enabling master mode with SSI still clear raises a
     * mode fault - which CLEARS THE MASTER BIT in hardware, silently.
     * The symptom is a transfer that accepts its data and never
     * completes.
     */
    SPI6->CR1 = SPI_CR1_SSI;

    /*
     * 16-BIT FRAMES, which is the whole transaction. Address and data
     * are not two bytes in this protocol, they are one 16-bit word:
     * 7 bits of register then 9 bits of value, and the ninth data bit is
     * exactly why a byte-oriented driver cannot talk to this part.
     */
    SPI6->CFG1 = (15UL << SPI_CFG1_DSIZE_Pos)
               | (SPI6_MBR_DIV64 << SPI_CFG1_MBR_Pos);

    /*
     * Master, transmit only, software NSS, mode 0 (CPOL = CPHA = 0) so
     * data is sampled on the rising edge of SCLK as the datasheet's
     * figure 13 shows. COMM = 01 keeps the receive FIFO out of it -
     * there is nothing on the other end to receive from.
     *
     * AFCNTR holds the pins driven between transfers, so SCK does not
     * float and clock stray bits into a part that is counting them.
     */
    SPI6->CFG2 = SPI_CFG2_MASTER
               | (1UL << SPI_CFG2_COMM_Pos)
               | SPI_CFG2_SSM
               | SPI_CFG2_AFCNTR;
}

void nau8822_write(uint8_t reg, uint16_t val)
{
    uint16_t word = (uint16_t)(((uint16_t)(reg & 0x7FU) << 9)
                               | (val & 0x1FFU));

    s_shadow[reg & 0x7FU] = (uint16_t)(val & 0x1FFU);

    SPI6->CR1 &= ~SPI_CR1_SPE;
    SPI6->IFCR = 0xFFFFFFFFU;
    SPI6->CR2  = 1U;                       /* one frame */

    CS_PORT->BSRR = (1UL << (CS_PIN + 16U));   /* CS low */

    SPI6->CR1 |= SPI_CR1_SPE;
    SPI6->CR1 |= SPI_CR1_CSTART;

    *(volatile uint16_t *)&SPI6->TXDR = word;

    /*
     * EOT, not TXC. EOT is the flag that means the shift register has
     * emptied and the last bit is on the wire; raising CS on TXC clips
     * it. Here that would be worse than a clipped bit on a display,
     * because in this protocol THE RISING EDGE OF CS IS THE WRITE. A CS
     * that goes up one bit early does not corrupt the value, it latches
     * a 15-bit word - which the codec reads as a different register
     * entirely.
     *
     * Bounded, so a dead SPI6 costs a wrong register rather than a hung
     * image with no console.
     */
    for (uint32_t n = 200000U; n != 0U && !(SPI6->SR & SPI_SR_EOT); n--) { }

    /*
     * TSCCSH, the last SCLK rising edge to CS rising hold time, is 30 ns.
     * At 480 MHz that is fifteen cycles, and the few instructions above
     * do not reliably cover it. A short counted loop does.
     */
    for (volatile uint32_t d = 0; d < 20U; d++) { }

    CS_PORT->BSRR = (1UL << CS_PIN);           /* CS high - THE WRITE */

    SPI6->IFCR = 0xFFFFFFFFU;
    SPI6->CR1 &= ~SPI_CR1_SPE;

    /* TCSBH, CS high between words, 30 ns. Same reasoning. */
    for (volatile uint32_t d = 0; d < 20U; d++) { }
}

uint16_t nau8822_shadow(uint8_t reg)
{
    return s_shadow[reg & 0x7FU];
}

void nau8822_reset(void)
{
    /*
     * Any write to R0 resets the part. Do it first so none of what
     * follows depends on what the factory firmware left configured -
     * and on this board it always has left something, because the
     * factory firmware drives this same codec and loading an image over
     * SWD resets no peripheral outside the MCU at all.
     */
    nau8822_write(NAU_RESET, 0x000U);
    shadow_reset_values();
    board_delay_ms(10);

    /*
     * The datasheet's power-up sequence, section 11.2.1, in its own
     * order.
     *
     * First the DC tie-off buffers, before any reference current flows:
     * DCBUFEN and IOBUFEN, which is R1 = 0x104.
     *
     * DCBUFEN is the one that needs a decision. The datasheet asks for
     * it when operating above 3.60 V, and VDDSPK here is BAT itself -
     * the speaker amplifier is fed straight from the battery through
     * Q4, so it is 4.2 V on a charger and 3.0 V on a flat cell. It
     * spans the threshold, so it cannot be answered by a constant. On
     * with the boost left off (see below) is the setting that is valid
     * across the whole range.
     */
    nau8822_write(NAU_PWR1, 0x104U);

    /*
     * Then the analogue bias, with the reference deliberately charging
     * through its 80 kohm tie-off rather than being driven: REFIMP = 01
     * and ABIASEN, R1 = 0x10D.
     *
     * AND THEN WAIT. 250 ms, which the datasheet asks for and which is
     * the slowest thing in the whole codec bring-up by two orders of
     * magnitude. It is the time for the external coupling capacitors -
     * 100 uF on each output here - to charge through that resistor. Cut
     * it short and the charge happens anyway, through the speaker, as a
     * thump.
     */
    nau8822_write(NAU_PWR1, 0x10DU);
    board_delay_ms(250);
}

void nau8822_configure(void)
{
    /*
     * CLOCKING. The single most important register on this board.
     *
     * CLKM = 0 selects the MCLK pin rather than the internal PLL, and
     * MCLKSEL = 000 passes it through undivided, so IMCLK = MCLK = the
     * 256 x Fs that SAI1 is generating on PE2. CLKIOEN = 0 leaves FS
     * and BCLK as inputs, because the MCU drives them.
     *
     * The reset value is 0x140 - CLKM = 1, the internal PLL, divided by
     * two - so this register MUST be written. Leaving it alone gives a
     * codec running off an unprogrammed PLL while the MCU happily
     * generates clocks nobody is using.
     *
     * Bypassing the PLL is also what makes the NAU8822L-versus-NAU8822A
     * question moot: the documented difference between the two parts is
     * a PLL output divider bit, and the PLL is not in the path.
     */
    nau8822_write(NAU_CLK1, 0x000U);

    /*
     * SMPLR exists only to scale internal filter coefficients and
     * "does not affect in any way the actual device sample rate". Its
     * highest setting is 48 kHz, so at the rates this radio wants -
     * 160 or 192 kHz - the nearest available value is as close as the
     * register can get. 000 it is.
     */
    nau8822_write(NAU_CLK2, 0x000U);

    /*
     * AUDIO INTERFACE: standard I2S, 16-bit words.
     *
     * AIFMT = 10 is I2S, WLEN = 00 is 16 bits. The reset value says
     * 24-bit, so this too has to be written. 16 bits matches the SAI
     * configuration and the int16_t that the whole upstream DSP tree
     * carries.
     */
    nau8822_write(NAU_IFACE, 0x010U);

    /* No companding, no internal ADC-to-DAC passthrough. */
    nau8822_write(NAU_COMPAND, 0x000U);

    /*
     * THE ADC HIGH-PASS FILTER MUST BE OFF, AND THIS IS NOT AN AUDIO
     * CODEC DECISION.
     *
     * R14 resets to 0x100, which is HPFEN set: a first-order 3.7 Hz
     * filter to block DC, exactly right for a microphone and exactly
     * wrong here. The MSi001 delivers quadrature baseband, so zero Hz
     * at the ADC is the CENTRE of the receiver's passband - the tuned
     * frequency itself. A DC blocker would carve a notch out of the
     * middle of every signal the radio is pointed at.
     *
     * ADCOS = 0 keeps 64x oversampling. 128x buys a little SNR at low
     * sample rates and has no chance of keeping up at 192 kHz, which is
     * already far outside what this part is specified for.
     */
    nau8822_write(NAU_ADC_CTRL, 0x000U);

    /* DAC: no soft mute, no automute - a silent stream must stay
     * connected, or a quiet band mutes the radio - and 64x. */
    nau8822_write(NAU_DAC_CTRL, 0x000U);

    /*
     * INPUT ROUTING. The I/Q pair arrives differentially:
     * Q on LMICP/LMICN, I on RMICP/RMICN.
     *
     * R44 = 0x033 connects both the P and the N pin of each channel to
     * its PGA, which is the differential configuration, and leaves the
     * unused LLIN/RLIN line inputs out. That happens to be the reset
     * value; it is written anyway, because this driver should not
     * depend on a reset value it did not choose.
     */
    nau8822_write(NAU_INPUT_CTRL, 0x033U);

    /*
     * NO +20 dB PGA BOOST. R47 and R48 reset to 0x100, which is the
     * boost ON - twenty decibels of gain nobody asked for, ahead of the
     * ADC, in a receiver whose whole front end gain plan belongs to the
     * MSi001 and the AGC. Turning it off also disconnects the unused
     * LLIN and LAUXIN paths into the boost stage, which is what the
     * zeroes in the low bits do.
     */
    nau8822_write(NAU_BOOST_L, 0x000U);
    nau8822_write(NAU_BOOST_R, 0x000U);

    nau8822_set_pga_gain(16U);          /* 0 dB */

    /* ADC and DAC digital volumes at 0 dB. */
    nau8822_write(NAU_ADCVOL_L, 0x0FFU);
    nau8822_write(NAU_ADCVOL_R, 0x1FFU);    /* update bit commits both */
    nau8822_write(NAU_DACVOL_L, 0x0FFU);
    nau8822_write(NAU_DACVOL_R, 0x1FFU);

    /*
     * OUTPUT MIXERS: each DAC to its own side's mixer and nothing else.
     * Both reset to exactly that, and both are written anyway.
     */
    nau8822_write(NAU_MIXER_L, 0x001U);     /* LDACLMX */
    nau8822_write(NAU_MIXER_R, 0x001U);     /* RDACRMX */

    /*
     * THE SPEAKER IS BRIDGE TIED, which the schematic says and nothing
     * else does: LS1 sits between LSPKOUT and RSPKOUT with no ground
     * connection, so the two outputs have to drive it in antiphase.
     *
     * R43 RSUBBYP routes the right speaker amplifier through the
     * submixer, whose documented job is to invert RMIX for exactly this
     * arrangement. With the same audio in both mixers the speaker then
     * sees twice the swing of a single-ended output - 6 dB more
     * loudness for free, and the reason a radio this small is as loud
     * as it is.
     *
     * Get this wrong and the speaker still works, quietly: driving both
     * terminals in phase leaves only the mismatch between the two
     * amplifiers across the coil.
     */
    nau8822_write(NAU_RSPK_SUBMIX, 0x010U);

    /*
     * NO OUTPUT BOOST. R49 keeps its reset value: SPKBST, AUX1BST and
     * AUX2BST all clear, thermal shutdown on, 1 kohm tie-off.
     *
     * The 1.5x boost needs VDDSPK above 3.6 V, and VDDSPK is the
     * battery. It would be legal on a charger and out of specification
     * on a flat cell, and a setting that depends on the state of charge
     * is a setting that will be wrong one day. The honest version is to
     * leave the boost off and get the loudness from the bridge tie
     * above, which does not care what the battery is doing.
     */
    nau8822_write(NAU_OUTPUT_CTRL, 0x002U);

    /*
     * Levels before drivers, drivers before unmuting. Start quiet: a
     * full-scale digital tone through a 4.2 V amplifier into a small
     * speaker is loud enough to be unpleasant and loud enough to
     * clip, and neither tells us anything we wanted to learn.
     */
    nau8822_set_speaker_db(-24);
    nau8822_set_headphone_db(-24);
    nau8822_mute(1);

    /*
     * POWER, input chain first.
     *
     * R2: both ADCs, both input PGAs, both boost/mix stages, and the
     * two headphone drivers. R3: both DACs, both output mixers and both
     * speaker drivers.
     *
     * The headphone drivers are enabled whether or not anything is
     * plugged into J5, and deliberately: the datasheet is explicit that
     * leaving outputs enabled and using the mute controls is kinder to
     * the coupling capacitors than switching the drivers, and THERE IS
     * NO JACK DETECT ON THIS BOARD - J5 has only tip, ring and sleeve,
     * and the codec's jack-sense inputs are GPIO2 and GPIO3, both of
     * which the schematic leaves unconnected. Speaker-versus-headphone
     * will have to be a menu, not a sensor.
     */
    nau8822_write(NAU_PWR2, 0x1BFU);
    nau8822_write(NAU_PWR3, 0x06FU);

    /* The converters need their bias to settle before the first sample
     * out of them means anything. */
    board_delay_ms(50);
}

void nau8822_set_pga_gain(uint8_t gain)
{
    uint16_t g = (uint16_t)(gain & 0x3FU);

    /*
     * ZC - zero-cross gain updates - is deliberately NOT set. It waits
     * for the input to cross zero before applying a new gain, which is
     * what you want on a microphone and a trap here: with the tuner
     * unpowered the input can sit at a DC level that never crosses
     * anything, and the gain change would simply never happen. That
     * would look exactly like a broken control bus, which is the one
     * thing this register is being used to rule out.
     *
     * The left write leaves the update bit clear so the value is held;
     * the right write sets it and both channels change together. I and
     * Q at different gains, even for a single sample, is an imbalance
     * that iqbal.c would spend its time chasing.
     */
    nau8822_write(NAU_PGA_L, g);
    nau8822_write(NAU_PGA_R, (uint16_t)(g | 0x100U));
}

void nau8822_set_pga_mute(int mute)
{
    uint16_t g = nau8822_shadow(NAU_PGA_L) & 0x3FU;

    if (mute) { g |= 0x040U; }      /* LPGAMT / RPGAMT */

    nau8822_write(NAU_PGA_L, g);
    nau8822_write(NAU_PGA_R, (uint16_t)(g | 0x100U));
}

void nau8822_set_adc_digital_mute(int mute)
{
    /*
     * ADC digital volume: 0 is mute, 0xFF is 0 dB, 0.5 dB a step. This
     * sits after the converter, so it proves which side of the
     * converter a problem is on - see the header.
     */
    uint16_t v = mute ? 0x000U : 0x0FFU;

    nau8822_write(NAU_ADCVOL_L, v);
    nau8822_write(NAU_ADCVOL_R, (uint16_t)(v | 0x100U));
}

void nau8822_set_adc_invert(int invert)
{
    uint16_t v = nau8822_shadow(NAU_ADC_CTRL) & ~1U;

    if (invert) { v |= 1U; }        /* LADCPL */
    nau8822_write(NAU_ADC_CTRL, v);
}

void nau8822_set_adc_swap(int swap)
{
    uint16_t v = nau8822_shadow(NAU_IFACE) & ~2U;

    if (swap) { v |= 2U; }          /* ADCPHS */
    nau8822_write(NAU_IFACE, v);
}

/* -57..+6 dB in 1 dB steps, 0 dB at 57. */
static uint16_t vol_code(int db)
{
    int v = db + 57;

    if (v < 0)  { v = 0; }
    if (v > 63) { v = 63; }
    return (uint16_t)v;
}

/*
 * ZERO-CROSS ON THE OUTPUT VOLUMES, and the opposite choice from the
 * input PGA a few lines up.
 *
 * Bit 7 of each output volume register holds the change until the
 * signal crosses zero, which is what stops a gain step from being a
 * step in the waveform - and a step in the waveform is a click. On an
 * output carrying audio there is always another zero crossing coming,
 * so the wait is bounded by the signal itself.
 *
 * The input PGA deliberately does NOT use it: with the tuner unpowered
 * its input can sit at a DC level that never crosses anything, so a
 * gain change would simply never take effect - which would look exactly
 * like the broken control bus that register is used to rule out.
 */
#define VOL_ZC      0x080U

void nau8822_set_speaker_db(int db)
{
    uint16_t v = vol_code(db) | VOL_ZC
               | (nau8822_shadow(NAU_SPKVOL_L) & 0x040U);

    nau8822_write(NAU_SPKVOL_L, v);
    nau8822_write(NAU_SPKVOL_R, (uint16_t)(v | 0x100U));
}

void nau8822_set_headphone_db(int db)
{
    uint16_t v = vol_code(db) | VOL_ZC
               | (nau8822_shadow(NAU_HPVOL_L) & 0x040U);

    nau8822_write(NAU_HPVOL_L, v);
    nau8822_write(NAU_HPVOL_R, (uint16_t)(v | 0x100U));
}

void nau8822_mute(int mute)
{
    uint16_t sl = nau8822_shadow(NAU_SPKVOL_L) & ~0x040U;
    uint16_t hl = nau8822_shadow(NAU_HPVOL_L) & ~0x040U;

    if (mute) { sl |= 0x040U; hl |= 0x040U; }

    nau8822_write(NAU_SPKVOL_L, sl);
    nau8822_write(NAU_SPKVOL_R, (uint16_t)(sl | 0x100U));
    nau8822_write(NAU_HPVOL_L, hl);
    nau8822_write(NAU_HPVOL_R, (uint16_t)(hl | 0x100U));
}
