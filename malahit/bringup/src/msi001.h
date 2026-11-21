#ifndef MALAHIT_MSI001_H
#define MALAHIT_MSI001_H

#include <stdint.h>

/*
 * Mirics MSi001 tuner: LNA, quadrature mixer, LO synthesiser and
 * programmable baseband filter, with analog differential I/Q out.
 *
 *   PA4  SPI2_MSI_CS    -> EN   (pin 33)  chip select, GPIO
 *   PA5  SPI2_MSI_SCK   -> CLK  (pin 34)  SPI1_SCK, AF5
 *   PA7  SPI2_MSI_MOSI  -> DATA (pin 35)  SPI1_MOSI, AF5
 *
 * The schematic's `SPI2_*` net names are wrong for this MCU - PA4/PA5/PA7
 * are SPI1 or SPI6 pins on the STM32H743, never SPI2 - and SPI6 is taken
 * by the codec. So SPI1 it is, at 24 bits a frame, with EN driven by hand
 * because PA4 as a hardware NSS would not frame the word the way this
 * part needs.
 *
 * I/Q goes to the codec's microphone inputs, differentially:
 *   OPQ_P/OPQ_N -> LMICP/LMICN  (Q, the left channel)
 *   OPI_P/OPI_N -> RMICP/RMICN  (I, the right channel)
 * Note the ordering: Q is left, I is right. Getting that backwards
 * conjugates the spectrum, which mirrors it about the tuned frequency
 * rather than breaking anything visibly.
 *
 * Reference clock is the same 22 MHz module that feeds the MCU - X1, on
 * the switched rail. One oscillator for both means no beat between them,
 * and it means anything done to that clock is done to the tuner too.
 */

/*
 * THE CONTROL INTERFACE IS WRITE-ONLY, like the codec's, and for the
 * same practical consequence: no register can be confirmed by asking.
 * Unlike the codec there is not even a convenient side effect to measure
 * - so the tuner is verified the only way left, by tuning it to a known
 * carrier and seeing where the carrier lands.
 *
 * Format, from the datasheet section 5: up to 24 bits a word, MSB first,
 * clocked in on the RISING edge of CLK, and latched on the RISING EDGE
 * OF EN. So EN is low for the word and goes high to commit it, which is
 * an ordinary active-low chip select. The ADDRESS IS THE LAST FOUR BITS
 * SENT, i.e. bits 3..0 of the word, with data from bit 4 upwards.
 *
 * Minimum 300 ns between words (tIWD) and up to 50 MHz on CLK, so the
 * bus is in no way the limiting factor; this driver runs it at 3.75 MHz.
 */
#define MSI_REG_MODE        0U      /* IC mode / power control       */
#define MSI_REG_GAIN        1U      /* receiver gain control         */
#define MSI_REG_SYNTH       2U      /* INT and FRAC                  */
#define MSI_REG_LOTRIM      3U      /* AFC                           */
#define MSI_REG_AUX         4U      /* sine generator                */
#define MSI_REG_SYNTHCFG    5U      /* THRESH, plus two must-be-one  */
#define MSI_REG_DCCAL       6U      /* DC offset calibration timers  */

/*
 * THE REFERENCE IS 22 MHz, WHICH IS THE AWKWARD ONE AGAIN.
 *
 * XTAL_SEL = 001 selects it, and that choice also fixes the first IF
 * used by the AM up-convert path at 132 MHz (datasheet table 23 - it is
 * 6 x FREF1, and each supported crystal has its own value). Every
 * STM32H7 example assumes 8 or 25 MHz and every MSi001 example assumes
 * 24.576 or 26; this board agrees with neither.
 */
#define MSI_XTAL_SEL        1U
#define MSI_FREF1_HZ        22000000U
#define MSI_FIF1_HZ         132000000U    /* AM first IF, 22 MHz part */

/*
 * The DC calibration timers are counted in N reference periods, and N is
 * fixed by the same XTALSEL choice - datasheet table 21 pairs the two.
 * For 22 MHz, N = 55, so one tick is 55/22 = 2.5 us exactly. That the
 * two tables are indexed by the same three bits is easy to miss, and
 * getting N from the wrong row makes every DC timing figure wrong by a
 * factor the console would print without complaint.
 */
#define MSI_DCCAL_N         55U

/*
 * The synthesiser comparison frequency is 4 x FREF1, and FLO is that
 * divided by the band's LO divider:
 *
 *     FLO = (4 x FREF1 / LODIV) x (INT + (FRAC x 4096 + AFC)
 *                                        / (THRESH x 4096))
 *
 * Checked against both worked examples in the datasheet before any of
 * this ran: THRESH = 312 decodes from its register word, and INT = 32
 * with FRAC = 51 gives exactly 1672.5 MHz for a 26 MHz part. Two
 * independent confirmations of the bit layout, which matters for a
 * register map where the address is at the BOTTOM of the word and the
 * datasheet numbers its bits with "bit 0 last sent".
 */
#define MSI_FCOMP_HZ        (4U * MSI_FREF1_HZ)     /* 88 MHz */

/*
 * THRESH is pinned at its maximum. It sets the FRAC step size, and AFC
 * subdivides one FRAC step by a further 4096, so the tuning resolution
 * is 4 x FREF1 / (LODIV x THRESH x 4096) - about 0.33 Hz in AM mode.
 * There is no reason to use anything coarser and a 12-bit field to pay
 * for it with, so this never changes.
 */
#define MSI_THRESH          4095U

typedef enum {
    MSI_BAND_AM = 0,    /* LW/MW/SW, up-converted through FIF1, LODIV 16 */
    MSI_BAND_VHF,       /* 64..108 MHz broadcast FM, LODIV 32; used
                           60..120 here, as the factory firmware does   */
    MSI_BAND_B3,        /* 174..240 MHz, LODIV 16; used 120..250         */
    MSI_BAND_B45,       /* UHF, LODIV 4                                  */
    MSI_BAND_L          /* L band, LODIV 2                               */
} msi_band_t;

/*
 * Baseband filter. For a receiver sampling I/Q at 192 kHz the only
 * sensible setting is zero IF with the narrowest low pass available -
 * and the narrowest available is 200 kHz single-sided, which is WIDER
 * than the 96 kHz the sample rate can represent.
 *
 * So there is unavoidable aliasing from 96 to 200 kHz folding back into
 * the passband. It is not a mistake in this driver and it cannot be
 * configured away: the chip was built for DVB channels, and 200 kHz is
 * the bottom of its range. The stock firmware runs the same part at
 * 160 kHz with the same filter and the same fold, so whatever the radio
 * does about it, it has always done.
 */
#define MSI_FILMODE_ZEROIF  3U      /* low pass, zero IF */
#define MSI_FILBW_200K      0U
#define MSI_FILBW_300K      1U
#define MSI_FILBW_600K      2U
#define MSI_FILBW_1M536     3U

/* What the tuning arithmetic decided, so it can be printed before it is
 * believed. Same reasoning as sai1_clock_t: a synthesiser that is going
 * to land in the wrong place should say so while the console still
 * works. */
typedef struct {
    uint32_t    f_target;       /* asked for, Hz                      */
    uint32_t    f_actual;       /* what the dividers really give, Hz,
                                   rounded to the nearest (not
                                   truncated - see msi001_plan()) */
    int32_t     err_hz;         /* actual - target                    */
    msi_band_t  band;
    uint32_t    lodiv;
    uint32_t    fsynth_hz;      /* FLO x LODIV, the VCO frequency     */
    uint32_t    flo_hz;         /* mixer LO: f + FIF1 in AM, f else   */
    uint32_t    divint;         /* INT, 6 bits                        */
    uint32_t    frac;           /* FRAC, 12 bits                      */
    uint32_t    afc;            /* AFC, 12 bits                       */
    uint32_t    step_mhz_e6;    /* tuning step, micro-Hz              */
    int         ok;
} msi_plan_t;

/* Pure arithmetic, touches no register. */
void msi001_plan(uint32_t freq_hz, msi_band_t band, msi_plan_t *out);

/* Which band this driver would choose for a frequency, from the filters
 * actually fitted to this board. */
msi_band_t msi001_band_for(uint32_t freq_hz);

void msi001_init(void);

/* One 24-bit word: 20 bits of data, then the 4-bit address. */
void msi001_write(uint32_t reg, uint32_t data);

/* What was last written there. The only way to read this part. */
uint32_t msi001_shadow(uint32_t reg);

/*
 * Program every register from reset, then tune. The datasheet's own
 * power-up order (figure 5) is register 0, 5, 2, 1, 6, 4, 3, and it is
 * followed exactly - the synthesiser's DC offset calibration is
 * triggered by programming register 2, so the gain and DC settings have
 * to be in place around it rather than after it.
 *
 * Returns 0, or MSI_ERR_* below.
 */
int msi001_tune(uint32_t freq_hz, msi_band_t band);

/* Retune within the band already configured: registers 2 and 3 only,
 * which is all a frequency change needs once the mode is set. */
int msi001_retune(uint32_t freq_hz);

const msi_plan_t *msi001_last_plan(void);

/*
 * Gain. All three are GAIN REDUCTION, not gain: zero means maximum
 * sensitivity, which is the opposite of the convention most of the
 * upstream AGC code expects and worth getting right once here.
 *
 *   bb_gr    0..59 dB of IF/baseband gain reduction
 *   lna_gr   1 to drop the LNA (all bands except AM)
 *   mix_gr   1 to drop the I/Q mixer
 *   mixbu    AM block-up-convert mixer gain, 0..3
 */
void msi001_set_gain(uint8_t bb_gr, int lna_gr, int mix_gr, uint8_t mixbu);

/*
 * DC OFFSET CALIBRATION - register 1 bits 16:14, and the speed-up bit 17.
 *
 * THIS WAS THE WORST NUMBER ON THIS BOARD, so the controls are public
 * and not a constant buried in the .c file. It was also a firmware
 * fault rather than a hardware one - see the long note in msi001.c.
 *
 * The datasheet specifies 50 mV of residual offset with continuous
 * calibration and 5 mV with the speed-up bit (table 13, "O/P residual
 * DC offset", for register 1 D[17:14] = 0101 and 1101). What this
 * driver used to produce was 1122 mV on one run and a railed converter
 * on another, which is not a poor part, it is a trim that was switched
 * off.
 *
 * Four things the datasheet says about it that the driver has to respect:
 *
 *   1. Calibration happens "on chip power up or synthesizer
 *      programming" - every table of baseband specs is footnoted that
 *      way. Writing register 2 is therefore a calibration trigger as
 *      well as a tuning command.
 *
 *   2. "As ... the baseband gain settings changes then the effective DC
 *      offset will also change" (section 5.4). So a calibration is only
 *      valid for the gain it was taken at - and the datasheet's own
 *      power-up order writes register 2 BEFORE register 1, which means
 *      following it literally calibrates at the previous gain. That is
 *      what msi001_dc_recal() is for.
 *
 *   3. Static mode does not calibrate. It is "no DC tracking": it holds
 *      whatever the last calibration left, until the hold circuit leaks.
 *
 *   4. The speed-up bit widens the high-pass corner so the loop settles
 *      fast, and is "not intended to be used during active reception
 *      periods due to the significant loss of energy around DC". Which
 *      is exactly the shape of a calibration burst: turn it on, let it
 *      converge, turn it off, hold.
 */
#define MSI_DCCAL_STATIC      0U    /* no tracking: hold the last value  */
#define MSI_DCCAL_PERIODIC1   1U    /* refresh every  N/fref x DCRATE    */
#define MSI_DCCAL_PERIODIC2   2U    /* every 2N/fref x DCRATE            */
#define MSI_DCCAL_PERIODIC3   3U    /* every 3N/fref x DCRATE            */
#define MSI_DCCAL_ONESHOT     4U    /* short track, then hold            */
#define MSI_DCCAL_CONTINUOUS  5U    /* always tracking: a hole at DC     */

/* Set the mode, and the speed-up bit with it. Writes register 1, which
 * is also what re-arms a one-shot. */
void msi001_set_dccal(uint32_t mode, int speedup);

uint32_t msi001_dccal_mode(void);
int      msi001_dccal_speedup(void);

/*
 * Calibrate NOW, at the gain that is set now, and go back to holding.
 *
 * Continuous tracking with the speed-up bit for `settle_ms`, then back
 * to whatever mode was configured before the call. This is the sequence
 * the datasheet describes in pieces and never writes down: speed-up to
 * converge deeply and quickly, continuous so it converges at all, then
 * the configured hold mode so that nothing keeps digging a hole at the
 * tuned frequency while someone is listening to it.
 *
 * Call it after any change to the gain or the signal path. Two
 * milliseconds is ample - the tracking time in table 20 is measured in
 * microseconds.
 */
void msi001_dc_recal(uint32_t settle_ms);

/*
 * Whether msi001_set_gain() recalibrates behind itself. On by default,
 * because a gain change invalidates the held calibration and no caller
 * should have to know that. Turn it off to measure the offset without
 * the recalibration - the only way to show what the recalibration is
 * worth - or in an AGC, which steps the gain far too often to pay two
 * milliseconds and a DC transient at every step.
 */
void msi001_set_dc_auto(int on);
int  msi001_dc_auto(void);

/*
 * Register 6, the periodic-mode timers. Reset values are DCTRK_TIM = 1
 * and DCRATE_TIM = 2048, which with this board's 22 MHz reference
 * (N = 55, so N/fref = 2.5 us) is a 2.5 us tracking window every
 * 5.12 ms - a spectral hole 0.05 % of the time, repeating at 195 Hz.
 *
 *   track  1..63     DCTRK_TIM, tracking window
 *   rate   2..4095   DCRATE_TIM, refresh period, must exceed track
 */
void msi001_set_dc_timers(uint32_t track, uint32_t rate);

/*
 * What those timers mean in time, for the mode now selected. Both may be
 * NULL; a zero period means the mode is not a periodic one.
 *
 * NANOSECONDS, because one tick is 2.5 us on this board and a function
 * returning microseconds reported the reset tracking window as "2 us".
 * A figure that is 20 % short still reads like a measurement.
 */
void msi001_dc_timing_ns(uint32_t *period_ns, uint32_t *track_ns);

/*
 * Which AM input port. Port 1 is 1 kohm, meant for a ferrite rod; port 2
 * is 75 ohm, meant for a real antenna. The datasheet calls the two
 * resulting modes AM_MODE1 and AM_MODE2 - confusingly, since AM_MODE2 is
 * also the name of the bit that enables the up-convert mixer, and that
 * bit is set in both.
 *
 * This board feeds the tuner from a 50-ohm-ish antenna through the
 * filter bank, so port 2 ought to be the right one. "Ought to" is not a
 * measurement, so main.c tries both and compares.
 */
void msi001_set_am_port(int port2);

/*
 * THE BAND FILTER BANK, read off the schematic.
 *
 * Three PE42641 SP4T switches, six control lines on PE10..PE15, named
 * BPF1..BPF6 and carried here as a mask with BPF1 in bit 0.
 *
 *   input switch   RF_IN -> one of four filters   (BPF3 = V2, BPF4 = V1)
 *   output switch  one of four filters -> tuner   (BPF5 = V2, BPF6 = V1)
 *   VHF switch     sub-band select               (BPF1 = V2, BPF2 = V1)
 *
 * PE42641 truth table: RF1 = V2:0 V1:0, RF2 = 1,0, RF3 = 0,1, RF4 = 1,1.
 *
 * THE TWO SWITCHES DO NOT USE THE SAME CODE FOR THE SAME BAND, which is
 * the trap in this bank and the reason the masks below look arbitrary.
 * The filters are wired to different ports at each end:
 *
 *   band        input port        output port
 *   0-12 MHz    RF2  (1,0)        RF2  (1,0)
 *   12-30 MHz   RF4  (1,1)        RF1  (0,0)
 *   30-60 MHz   RF1  (0,0)        RF4  (1,1)
 *   VHF         RF3  (0,1)        RF3  (0,1)
 *
 * so 30-60 MHz, for instance, needs both input control lines LOW and
 * both output lines HIGH. Selecting a band by writing "the same two
 * bits" to both switches connects the input of one filter to the output
 * of another, and the result is not a weak signal, it is no signal.
 *
 * One step of this was inferred rather than read: which of each label
 * pair lands on V2 and which on V1, since the PDF gives text positions
 * and not wires. All three switches are drawn in the same style and the
 * non-crossing routing is unambiguous in each, but main.c sweeps all 64
 * combinations anyway - if the inference is wrong the measurement says
 * so immediately, and the error can only be a swap within a pair.
 */
#define MSI_BPF_0_12        0x14U   /* BPF3 + BPF5 */
#define MSI_BPF_12_30       0x0CU   /* BPF3 + BPF4 */
#define MSI_BPF_30_60       0x30U   /* BPF5 + BPF6 */
#define MSI_BPF_VHF1        0x28U   /* BPF4 + BPF6, VHF sub-band 1 */
#define MSI_BPF_VHF2        0x2BU   /* the same, sub-band switch on RF4
                                       (BPF1 + BPF2): VHF2, 120-250 MHz */

/* The mask this board needs for a frequency, from the table above. */
uint32_t msi001_bpf_for(uint32_t freq_hz);

void msi001_bpf(uint32_t mask);
uint32_t msi001_bpf_get(void);

/* PE0, the 5V preamp supply for the BGA614 ahead of the filters. */
void msi001_preamp(int on);

enum {
    MSI_OK            =  0,
    MSI_ERR_RANGE     = -1,   /* INT out of its 6 bits for that band */
    MSI_ERR_BAND      = -2
};

#endif /* MALAHIT_MSI001_H */
