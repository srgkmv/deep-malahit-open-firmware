#ifndef MALAHIT_ADC_H
#define MALAHIT_ADC_H

#include <stdint.h>

/*
 * ADC1, FOR THE TWO ANALOG LINES THIS BOARD HAS.
 *
 * Both are dual-pad pins whose analog switch the factory firmware leaves
 * OPEN, which means the pad reaches the ADC and NOT the digital port:
 *
 *   PA1_C  ENC_BUT   both encoder push-buttons, on a resistor ladder
 *   PA0_C  ADC_IN    battery, through a 10k/10k divider off BAT
 *
 * The buttons are why this exists. They cannot be read as GPIOs at all -
 * one of the two presses puts the line at 1.95 V, which on a 3.3 V input
 * is neither a low nor a high but the undefined band in between. See
 * BOARD.md section 6 for the ladder and PORTING.md step 12.
 */

/*
 * WHICH CHANNEL IS WHICH NET, AND WHY NEITHER IS A GUESS.
 *
 * No STM32 datasheet is in docs/, and this project's rule is that a pin
 * map is quoted and not remembered. So both were settled on the radio by
 * test_adc_pins(), which reads the four candidate channels with PCSEL
 * narrowed to one at a time and prints them (05/11/2026):
 *
 *   ch  0    1568, wandering 927..1984   a pad connected to nothing
 *   ch  1   65534, steady               ENC_BUT idle - and the finger
 *                                       moves it, which is the proof
 *   ch 16   41460, steady 41439..41500  ADC_IN
 *   ch 17    5528, wandering 4511..7311  a pad connected to nothing
 *
 * CHANNEL 16 IS THE BATTERY, confirmed against a multimeter rather than
 * against a pin table: the schematic makes ADC_IN the midpoint of two
 * 10k resistors across the cell, the meter read 4.15 V at the cell, and
 * 41460 counts is 4.17 V doubled - six parts in a thousand.
 *
 * AND THAT CONCLUSION WAS HALF WRONG, which is worth the space because
 * of HOW it was wrong - 06/11/2026.
 *
 * "Channel 1 is ENC_BUT" was a correct observation and a false fact. The
 * H743 gives PA1 a second, analog-only pad PA1_C with its own channel,
 * and an analog switch between them. That switch is left by the FACTORY
 * firmware, and on the day of the measurement it happened to be CLOSED -
 * so channel 1 was showing a mirror of channel 17, and the finger
 * confirmed the mirror.
 *
 * The next day the radio was power-cycled, the factory firmware ran from
 * cold, and the switch came up OPEN: PA1_C went isolated, channel 1 read
 * a floating pad, and every button in the radio reported itself held
 * down for ever. The counts were right, the classifier was right, the
 * debounce was right; they were all describing a disconnected pin.
 *
 * SO THE SWITCH IS NO LONGER INHERITED. adc_init() writes PMCR itself -
 * see there - and both nets are read on the pads this board actually
 * bonds, which are the PLAIN ones: ENC_BUT on 17, ADC_IN on 16. The
 * asymmetry that was noted and shrugged at above was the clue.
 *
 * The general shape of it: a measurement taken through somebody else's
 * configuration measures their configuration too, and theirs can change
 * when yours did not.
 */
#define ADC_CH_ENC_BUT      17U
#define ADC_CH_BAT          16U

/*
 * VREF+ IS NO LONGER THIS CONSTANT. It is measured, every millisecond,
 * against the part's own bandgap - see adc_vdda_mv() and adc3_init().
 *
 * What is left here is the FALLBACK, used only if ADC3 fails to start,
 * and it is kept because of where it came from: one calibration point
 * against a multimeter, 4.15 V on the cell against 41460 counts at the
 * divider's midpoint, so VREF+ = 2075 mV * 65535 / 41460 = 3280 mV.
 *
 * It is also the OUTSIDE CHECK on the measured value, and that is worth
 * more than its accuracy. Measured against the chip's factory
 * calibration the rail came out at 3298 mV; against the bandgap's
 * nominal 1216 mV, 3274 mV; against this meter reading, 3280 mV. Three
 * routes within 0.7 % of each other, and only one of the three did not
 * come out of this chip - so only that one could have caught the other
 * two agreeing on something wrong.
 */
#define ADC_VREF_MV         3280U

/* The divider is R34/R36, 10k and 10k, from ground to BAT with ADC_IN
 * between them - schematic page 3. No diode anywhere in that path, which
 * the DEEPSDR's battery sense does have. */
#define ADC_BAT_DIVIDER     2U

/*
 * THE DIVIDER'S OWN TOLERANCE, IN PARTS PER THOUSAND, SIGNED.
 *
 * With VREF+ measured against the chip's bandgap there is one error left
 * in the battery reading and the reference cannot touch it: the two 10k
 * resistors. At 1 % each they allow about 40 mV at a full cell, and the
 * radio read 4166 mV where a meter said 4150 - 0.4 %, comfortably inside
 * that. Nothing in the chip can see this; it takes a second measurement
 * from outside.
 *
 * WHY MULTIPLICATIVE AND NOT AN OFFSET. A divider sets a RATIO, so its
 * error scales with the voltage: 16 mV at a full cell is 13 mV at an
 * empty one. Upstream's battery_calib_pon() adds millivolts instead,
 * which is right for ITS board - there the error being trimmed is a
 * diode's forward drop, and a diode drop really is roughly constant.
 * Same idea, different shape, because the hardware underneath is
 * different.
 *
 * ZERO, AND THAT IS A RESULT RATHER THAN A DEFAULT - 05/11/2026.
 *
 * Measured: the radio said 4163 mV and the meter said 4.16 V. Three
 * millivolts apart, which looks like a trim of -1 part per thousand
 * waiting to be written down.
 *
 * It must not be written down. The meter reads to 10 mV, so "4.16"
 * means somewhere between 4155 and 4165 - and 4163 is inside that. The
 * disagreement is SMALLER THAN THE RESOLUTION OF THE INSTRUMENT
 * MEASURING IT, so fitting a constant to it would be fitting the
 * meter's last digit. It would look like a refinement and would be an
 * arbitrary offset.
 *
 * So the honest reading of this check is not "they agree" but "they do
 * not disagree down to 10 mV, which is as far as this meter can see".
 * Closing the remaining 1 % that the resistors are allowed needs a
 * meter with millivolt resolution; test_bat_calib() prints the table to
 * read the answer off, on the day there is one.
 *
 * It is a COMPILE-TIME constant only because this port has no settings
 * storage yet - the FRAM is on the board and has no driver.
 */
#define ADC_BAT_TRIM_PPT    0

enum {
    ADC_OK          =  0,
    ADC_ERR_LDO     = -1,   /* voltage regulator never came up */
    ADC_ERR_CAL     = -2,   /* calibration never finished */
    ADC_ERR_RDY     = -3,   /* ADRDY never appeared after ADEN */
    ADC_ERR_BUSY    = -4    /* could not stop what was already running */
};

int  adc_init(void);

/*
 * One conversion, polled, bounded. Returns the 16-bit count, or a
 * NEGATIVE number if the conversion did not happen - which is the whole
 * reason it is int32_t and not uint16_t. A failed read that returns a
 * plausible zero is how an ADC tells you the battery is flat.
 */
int32_t adc_read(uint32_t ch);

/*
 * Counts -> millivolts at the pin, against the NOMINAL 3.3 V rail.
 *
 * Nominal, and the name says so. VREF+ on this board is the 3.3 V rail
 * itself, so this inherits the LD3985's tolerance - a couple of per cent.
 * Upstream's battery.c has the whole story of what that costs and how it
 * gets fixed (measure VREFINT and divide), and the Malahit's VREFINT is
 * on ADC3, which is a separate job.
 *
 * The BUTTONS do not use this and must not: see adc_buttons().
 */
uint32_t adc_mv(uint32_t raw);

/*
 * The cell, in millivolts, and the same reading as a percentage.
 *
 * THE PERCENTAGE IS A STRAIGHT LINE between an empty and a full
 * voltage, and that is worth knowing before trusting it: a lithium
 * cell's real curve is flat through the middle of its charge, so a line
 * reads optimistically high for most of a discharge and then falls
 * quickly. Upstream's battery.c does exactly the same thing, and the
 * honest version of "a better curve" needs this radio discharged once
 * with the voltage logged, which is an afternoon and not a constant.
 *
 * It also reads high while the charger is connected, because a cell on
 * charge sits above its resting voltage. That is the chemistry, not the
 * firmware.
 */
uint32_t adc_bat_mv(void);
uint8_t  adc_bat_pct(void);

/*
 * Sampling from SysTick, one conversion per millisecond, NEVER spinning:
 * a tick starts a conversion and the NEXT tick collects it, by which time
 * it finished long ago. The two channels alternate, so each is fresh
 * within 2 ms - against a 20 ms button debounce, that is nothing.
 */
void     adc_sampling(int on);
void     adc_tick(void);

/* Latest raw counts, whatever adc_tick() last collected. */
uint32_t adc_raw_enc_but(void);
uint32_t adc_raw_bat(void);

/*
 * The buttons, as bits: 1 = ENC1 (the big knob's), 2 = ENC2 (the small
 * one's). Both at once is a legitimate reading and has its own ladder
 * voltage, which is also how the factory firmware's DFU gesture is
 * entered.
 */
#define ADC_BTN_ENC1        1U
#define ADC_BTN_ENC2        2U

uint8_t  adc_buttons(void);

/*
 * ENC1 alone, as upstream's encoder.c wants it: a function that answers
 * "is the button down", with no idea where the answer came from. Handed
 * over by -DENC_BTN_READ=adc_enc1_down, which is what lets this board
 * keep upstream's debounce integrator and its short/long classification
 * instead of writing a second copy of both.
 */
uint8_t  adc_enc1_down(void);
uint8_t  adc_enc2_down(void);

/* What the kernel clock actually came out at, for the console. */
uint32_t adc_kernel_hz(void);

/*
 * ADC3 AND THE INTERNAL REFERENCE.
 *
 * VREFINT is a bandgap near 1.2 V that does not move with the supply, so
 * converting it AGAINST the supply measures the supply. That is the only
 * way to stop VREF+ being a guess, and every voltage in this driver is a
 * fraction of VREF+ - see adc.c for what assuming it cost upstream.
 *
 * On this part the internal channels live on ADC3, which is why there is
 * a second converter here. adc3_init() takes the channel number because
 * no datasheet in this tree says which one it is: test_vrefint() finds
 * it by enabling the bandgap and seeing which channel moves.
 */
/*
 * WHICH ADC3 CHANNEL CARRIES THE BANDGAP. 19 is what this board
 * measured: test_vrefint() reads every ADC3 channel with the bandgap
 * switched off and again with it on, and exactly one of them changes
 * and lands where a 1.2 V reference against a 3.3 V rail has to land.
 * That is an identification, not a recollection - the same method the
 * two ADC1 channels were settled by.
 */
#define ADC3_CH_VREFINT     19U

/*
 * The die thermometer, on the same converter and found the same way -
 * switched off and on, and one channel moves. 18 is what this board
 * measured.
 *
 * IT MEASURES THE DIE, NOT THE ROOM. At 480 MHz this part runs well
 * above ambient, so a reading near 50 C with a cool room is the sensor
 * working rather than the sensor lying.
 */
#define ADC3_CH_TEMP        18U

/*
 * ST'S TWO CALIBRATION POINTS, AND WHY THESE ADDRESSES ARE BELIEVED.
 *
 * The part is measured at 30 C and 110 C during manufacture, both at
 * 3.3 V, and the two readings are left in system memory. They are not
 * trusted because a document says where they are - there is no such
 * document in this tree - but because the pair of numbers read there
 * only makes sense under one interpretation:
 *
 *   TS_CAL1 = 12375  ->  623.1 mV at 30 C
 *   TS_CAL2 = 16418  ->  826.7 mV at 110 C
 *   slope             =  50.54 counts/C = 2.545 mV/C
 *
 * A silicon temperature sensor of this kind sits near 0.62 V at 30 C
 * and climbs at about 2.5 mV/C. BOTH of those come out right, from two
 * numbers that were only read, and they are independent of each other:
 * an offset and a slope. No other reading of these words - 12 bits, a
 * different pair of temperatures, a different reference - produces two
 * textbook figures at once.
 */
#define TS_CAL1_ADDR        0x1FF1E820UL    /* 30 C  */
#define TS_CAL2_ADDR        0x1FF1E840UL    /* 110 C */
#define TS_CAL1_C           30
#define TS_CAL2_C           110

int      adc3_init(uint32_t vrefint_channel);

uint32_t adc_vdda_mv(void);     /* the measured rail, mV */
uint32_t adc_vref_raw(void);    /* the smoothed VREFINT reading, counts */
uint32_t adc_vrefin_cal(void);  /* ST's factory value, raw */
uint8_t  adc_vref_from_cal(void); /* 1 = the factory value was usable */
uint32_t adc3_status(void);
int32_t  adc3_read(uint32_t ch);

/* Like adc_read(), but takes its turn alongside the running sampler
 * instead of refusing - for looking at a live radio. */
int32_t  adc_read_any(uint32_t ch);

/*
 * The die temperature in TENTHS of a degree, signed. Tenths because the
 * sensor's resolution is better than a degree and rounding to whole
 * degrees makes a reading that is drifting look like one that is stuck.
 *
 * Returns ADC_TEMP_NONE if the calibration words are not usable, which
 * is better than a plausible number computed from a guessed slope.
 */
#define ADC_TEMP_NONE       (-32768)

int32_t  adc_temp_c10(void);
uint32_t adc_temp_raw(void);
uint32_t adc_ts_cal1(void);
uint32_t adc_ts_cal2(void);

/*
 * Diagnostics. adc_timeouts is the one that matters: it counts
 * conversions that were started and never finished, and it should stay
 * at zero for ever.
 */
extern uint32_t adc_conversions;
extern uint32_t adc_ticks;        /* SysTick entered adc_tick() */
extern uint32_t adc_ticks_live;   /* ...and sampling was on */
extern uint32_t adc_timeouts;
extern uint32_t adc_cal_factor;
extern uint32_t adc_found_cr;    /* ADC1->CR as the factory firmware left it */
extern uint32_t adc_found_pmcr;  /* SYSCFG->PMCR, likewise */

/*
 * HOW THE START-UP WENT, which is worth this much space because the
 * first attempt at it failed on the radio and the console could not say
 * why. adc_init() now tries three configurations and records all three
 * results, so one bench run distinguishes "the converter is not clocked"
 * from "calibration is the slow part" from "the leftover state was in
 * the way".
 */
#define ADC_ATTEMPTS        3

/* A result slot nobody wrote. Deliberately not 0, which is ADC_OK. */
#define ADC_NOT_TRIED       (-99)

extern uint8_t  adc_attempt_used;            /* which one worked */
extern int      adc_attempt_rc[ADC_ATTEMPTS];
extern uint32_t adc_cal_ms;                  /* how long calibration took */
extern uint32_t adc_cr_after_vreg;
extern uint32_t adc_cr_after_cal;
extern uint32_t adc_isr_after_aden;
extern uint32_t adc_ccr;

#endif /* MALAHIT_ADC_H */
