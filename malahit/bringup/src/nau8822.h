#ifndef MALAHIT_NAU8822_H
#define MALAHIT_NAU8822_H

#include <stdint.h>

/*
 * NAU8822 codec: the receiver's I/Q ADC and its audio output at once.
 *
 *   MSi001 I/Q  ->  RMICP/RMICN (I) and LMICP/LMICN (Q), differential
 *   SAI1 block B <- ADCOUT   the I/Q pair, 16-bit I2S
 *   SAI1 block A -> DACIN    audio out
 *   LSPKOUT/RSPKOUT -> LS1, the internal speaker, BRIDGE TIED
 *   LHP/RHP     ->  J5, the 3.5 mm jack
 *
 * Control is on SPI6, PB3 (SCK) and PB5 (MOSI), with PB6 as the chip
 * select in plain GPIO - PB6 is not an SPI6 NSS pin. 33 ohm series
 * networks (RN5/RN6) sit on all three.
 *
 * THE CONTROL INTERFACE IS WRITE-ONLY. The MODE pin is strapped high, so
 * the part is in SPI 3-wire mode: 16 bits a transaction, 7 bits of
 * register address then 9 bits of data, MSB first, sampled on the rising
 * edge of SCLK, and LATCHED INTO THE REGISTER ON THE RISING EDGE OF CSB.
 * There is no fourth wire and no read path.
 *
 * Two consequences run through this whole file.
 *
 * First, every register this code depends on has to be written in full,
 * because read-modify-write is impossible. nau8822_write() keeps a
 * shadow copy so that the helpers below can change one field without
 * inventing the rest.
 *
 * Second, nothing can be verified by asking. A register either had the
 * effect it should or the bus is broken, and telling those apart needs
 * an effect that is visible from the MCU side - which, on this board,
 * means something that changes the numbers coming out of the ADC. See
 * nau8822_set_adc_invert().
 */

void nau8822_bus_init(void);

/* One 16-bit transaction. reg is 0..127, val is 9 bits. */
void nau8822_write(uint8_t reg, uint16_t val);

/* What was last written there, or the datasheet reset value if nothing
 * has been. The only way to read this part. */
uint16_t nau8822_shadow(uint8_t reg);

/*
 * Software reset, then the datasheet's own power-up sequence including
 * its 250 ms wait for the reference to charge through its tie-off
 * resistors. Skipping that wait is what makes these codecs click.
 */
void nau8822_reset(void);

/*
 * Everything else: 16-bit I2S slave, MCLK straight through with the
 * internal PLL bypassed, differential I/Q into both PGAs, DAC to the
 * bridge-tied speaker and to the headphone jack. Outputs start muted.
 */
void nau8822_configure(void);

/*
 * Input PGA gain, 0..63, -12 dB to +35.25 dB in 0.75 dB steps.
 * 16 is 0 dB. Both channels together, committed with the update bit so
 * I and Q never sit at different gains even for one sample.
 */
void nau8822_set_pga_gain(uint8_t gain);

/*
 * The two mutes that localise a fault, and why there are two.
 *
 * When the samples arriving over I2S look wrong, the fault is in exactly
 * one of two places: the analogue front end is saturating the ADC, or the
 * digital path is not carrying what we think it is. These two registers
 * tell them apart, because each silences a different half of the chain.
 *
 *   nau8822_set_adc_digital_mute()  attenuates INSIDE the codec, after
 *       the converter and before the I2S serialiser. If the received
 *       samples go to zero, then what the MCU is reading really is the
 *       ADC output, the framing is right, and any remaining problem is
 *       in front of the converter. If they do NOT change, the MCU is not
 *       reading the ADC at all - wrong slot, wrong offset, wrong frame.
 *
 *   nau8822_set_pga_mute()  disconnects the input PGA, so the converter
 *       sees its own noise floor and nothing else. If the samples fall
 *       to a small number, the converter and everything behind it are
 *       healthy and the input was overdriving them.
 */
void nau8822_set_pga_mute(int mute);
void nau8822_set_adc_digital_mute(int mute);

/*
 * Invert the sign of the left ADC output (R14 LADCPL).
 *
 * THIS IS THE PROOF THAT THE WRITE-ONLY BUS WORKS. There is no register
 * to read back, so the control interface can only be shown to work by
 * an effect, and this is the cleanest one available: one bit, whose
 * entire job is to flip the sign of numbers the MCU is already
 * receiving. With the ADC high-pass filter switched off the left channel
 * carries a standing DC offset from the tuner's output stage, so
 * flipping this bit flips the sign of a large, obvious, unambiguous
 * number. No gain curve to interpret and no noise floor to argue about.
 */
void nau8822_set_adc_invert(int invert);

/*
 * Swap which half of the I2S frame each ADC channel occupies (R4
 * ADCPHS). Two jobs, and the second one is now the important one.
 *
 * It is a second independent check of the write-only control bus, and
 * the one that catches a frame alignment error in the SAI rather than
 * in the codec.
 *
 * AND IT IS WHAT MAKES THIS BOARD'S I/Q ORDER MATCH UPSTREAM'S.
 *
 * The schematic wires AUDIO_Q to LMICP/LMICN and AUDIO_I to
 * RMICP/RMICN, so the codec's LEFT channel carries Q and the
 * interleaved stream comes out Q first. The DEEPSDR 101 is the other
 * way round, and the whole upstream tree is written for its order -
 * iqbal_bloque() reads `crudo[2k]` as I and `crudo[2k+1]` as Q, and so
 * does everything downstream of it.
 *
 * Two ways to reconcile that: swap at every upstream call site, or make
 * the hardware produce the order upstream expects. The second is one
 * register bit, costs nothing per sample, and leaves the grafted code
 * untouched - which is the entire point of porting rather than
 * rewriting. So ADCPHS is set, permanently, and I arrives in slot 0.
 *
 * Getting this wrong does not break anything visibly. It conjugates the
 * spectrum: signals appear on the wrong side of the dial, mirrored
 * about the tuned frequency, and every image-rejection measurement
 * reads its own reciprocal.
 */
#define NAU_ADCPHS_DEFAULT  1

void nau8822_set_adc_swap(int swap);

/*
 * Output levels in dB, -57 to +6, and mute. The speaker is bridge tied
 * across LSPKOUT and RSPKOUT, so both sides move together and the
 * loudness for a given setting is 6 dB above what a single-ended output
 * would give.
 */
void nau8822_set_speaker_db(int db);
void nau8822_set_headphone_db(int db);
void nau8822_mute(int mute);

/* Register numbers, by name, for the ones this file does not wrap. */
enum {
    NAU_RESET        = 0,
    NAU_PWR1         = 1,
    NAU_PWR2         = 2,
    NAU_PWR3         = 3,
    NAU_IFACE        = 4,
    NAU_COMPAND      = 5,
    NAU_CLK1         = 6,
    NAU_CLK2         = 7,
    NAU_GPIO         = 8,
    NAU_DAC_CTRL     = 10,
    NAU_DACVOL_L     = 11,
    NAU_DACVOL_R     = 12,
    NAU_ADC_CTRL     = 14,
    NAU_ADCVOL_L     = 15,
    NAU_ADCVOL_R     = 16,
    NAU_RSPK_SUBMIX  = 43,
    NAU_INPUT_CTRL   = 44,
    NAU_PGA_L        = 45,
    NAU_PGA_R        = 46,
    NAU_BOOST_L      = 47,
    NAU_BOOST_R      = 48,
    NAU_OUTPUT_CTRL  = 49,
    NAU_MIXER_L      = 50,
    NAU_MIXER_R      = 51,
    NAU_HPVOL_L      = 52,
    NAU_HPVOL_R      = 53,
    NAU_SPKVOL_L     = 54,
    NAU_SPKVOL_R     = 55,
    NAU_PWR4         = 58
};

#endif /* MALAHIT_NAU8822_H */
