#ifndef MALAHIT_ADDON_H
#define MALAHIT_ADDON_H

#include <stdint.h>

/*
 * THE OPTIONAL ADD-ON BOARD - High-Z input, PE4312 step attenuator and a
 * second four-filter bank, all behind one 74HC595. BOARD.md section 7
 * has the whole map; this header carries only what the code needs.
 *
 * IT IS IN SERIES AHEAD OF THE RECEIVER AND CANNOT BE BYPASSED. The
 * vendor's fitting note says the standard SMA-UFL pigtail is dismantled
 * and the add-on board's output goes to the receiver's input, so every
 * signal this radio has ever measured came through it. Until this file
 * existed, it came through it in whatever state the factory firmware
 * last left - which is why BOARD.md section 6 now says the front-end
 * table has to be taken again.
 *
 * All six control lines are on port D, brought out on the main board's
 * J7 header. The assignment was NOT read off a drawing: the vendor's
 * main-board connector is 8-pin and this radio's J7 is 10-pin, so a
 * pin-for-pin harness is wrong and the link is hand-soldered wire. It
 * came from the factory firmware that drives it - see BOARD.md.
 */
#define ADDON_PIN_REL_50R   8U      /* latching relay -> 50 ohm input  */
#define ADDON_PIN_VEXT      9U      /* LDO enable: no power, no board  */
#define ADDON_PIN_LE       10U      /* 74HC595 RCLK                    */
#define ADDON_PIN_REL_HIZ  11U      /* latching relay -> High-Z input  */
#define ADDON_PIN_DIN      12U      /* 74HC595 SER                     */
#define ADDON_PIN_CLK      13U      /* 74HC595 SRCLK                   */

/*
 * Take the six lines over from the factory firmware without disturbing
 * what matters.
 *
 * Vext_ON is left HIGH and the latch line is left HIGH, because that is
 * where the factory firmware leaves them and both are load-bearing: low
 * Vext_ON unpowers the board outright, and the shift routine's last act
 * is to raise the latch. The two coil lines are forced LOW first and
 * only then configured, because a latching relay coil is built to be
 * pulsed and holding one on is how it gets cooked.
 */
void addon_init(void);

/*
 * How long addon_init() waits after powering the board up, before the
 * first write - measured, see addon.c. A variable only so that
 * test_addon_cold() can sweep it; nothing else should touch it.
 */
#define ADDON_RAIL_MS   200U
extern uint32_t addon_rail_ms;

/*
 * ONE COIL, ONE PULSE, TEN MILLISECONDS - the factory firmware's own
 * timing, read out of its code: set one line, clear the other, delay
 * 10, clear the first.
 *
 * THESE WERE CALLED A AND B UNTIL THE MEASUREMENT NAMED THEM, which is
 * worth keeping in the history rather than quietly tidying away. The
 * drawing cannot say which coil is which - the sign of the difference
 * depends on which way round the TC4-1+ is wired - so for a while the
 * only honest names were the pin numbers.
 *
 * What named them: the stock firmware can select 50 ohm by label, so
 * it was set that way, this image was loaded, and the level was read
 * BEFORE either coil was touched. It matched PD8's position and sat
 * 3-4 dB from PD11's. See test_addon_relay() in main.c.
 *
 * The relay LATCHES, so calling neither of these leaves the input
 * wherever it was last put - by us, by the factory firmware, or by
 * whoever had the radio before.
 */
void addon_input_50r(void);
void addon_input_highz(void);

/*
 * THE 74HC595 BYTE. The first bit sent lands in QH, which is the
 * factory firmware's own convention and not an assumption - its
 * shift-out loop tests bit 7 and shifts left, and BOARD.md section 7
 * quotes it.
 *
 * Bit 0 (QA) is not connected to anything.
 */
#define ADDON_B_ATT_DATA    0x80U   /* QH -> PE4312 DATA, via 10 kohm */
#define ADDON_B_ATT_CLK     0x40U   /* QG -> PE4312 CLK               */
#define ADDON_B_ATT_LE      0x20U   /* QF -> PE4312 LE                */
#define ADDON_B_BPFE12      0x10U   /* QE -> U3 (input switch)  V2    */
#define ADDON_B_BPFE11      0x08U   /* QD -> U3                 V1    */
#define ADDON_B_BPFE22      0x04U   /* QC -> U4 (output switch) V2    */
#define ADDON_B_BPFE21      0x02U   /* QB -> U4                 V1    */

#define ADDON_B_FILTER_MASK (ADDON_B_BPFE12 | ADDON_B_BPFE11 \
                           | ADDON_B_BPFE22 | ADDON_B_BPFE21)

/*
 * Shift one byte in and latch it.
 *
 * WRITING THIS DOES NOT DISTURB THE ATTENUATOR, and the reason is the
 * 595's two-stage design rather than care on our part: the outputs are
 * frozen by the storage register while the shift register is being
 * clocked, so PE4312's DATA, CLK and LE do not see the bits shuffling
 * past. They change once, all together, on the latch edge. As long as
 * ATT_LE is low in both the old byte and the new one, the PE4312 is
 * given no latch edge and keeps whatever attenuation it was holding -
 * which on arrival is the 30 dB the factory firmware leaves.
 */
void addon_595(uint8_t byte);

/* What was last written there. The 595 cannot be read back. */
uint8_t addon_595_shadow(void);

/*
 * The four filters, and THE SAME TRAP AS THE MAIN BOARD'S BANK.
 *
 * The two PE42641s put the same filter on DIFFERENT port numbers, so
 * BPFE1x and BPFE2x need different values - exactly as on the main
 * board (BOARD.md section 6), where writing the same code to both ends
 * connects the input of one filter to the output of another and the
 * result is not a weak signal but no signal.
 *
 * This header said the opposite for a day, on the authority of the
 * KiCad rebuild of the board, whose output switch has its four ports
 * in a different order from the vendor's own drawing. Fourteen of the
 * sixteen codes produce silence, and the first driver picked four of
 * those fourteen. See the measured table in addon.c.
 */
typedef enum {
    ADDON_F_1M5_4M5 = 0,    /* RF1, V2:0 V1:0 - 1.5..4.5 MHz bandpass */
    ADDON_F_0M5_1M5,        /* RF2, V2:1 V1:0 - 0.5..1.5 MHz bandpass */
    ADDON_F_HPF_4M5,        /* RF3, V2:0 V1:1 - highpass above 4.5    */
    ADDON_F_LPF_500K        /* RF4, V2:1 V1:1 - lowpass below 500 kHz */
} addon_filter_t;

/*
 * The four filter bits raw, for sweeping every combination. The three
 * attenuator lines keep their resting value; only the filter bits
 * change. Used to establish the truth table by measurement rather than
 * by reading a drawing - see test_addon_filter_sweep() in main.c.
 */
void           addon_595_filter_bits(uint8_t bits);

void           addon_set_filter(addon_filter_t f);
addon_filter_t addon_filter(void);
const char    *addon_filter_name(addon_filter_t f);

/* Which filter this board needs for a frequency. Same shape as
 * msi001_bpf_for() on the main board, and for the same reason: the
 * caller knows the frequency, not the port numbering. */
addon_filter_t addon_filter_for(uint32_t freq_hz);

/*
 * THE PE4312 STEP ATTENUATOR, 0 to 31.5 dB in half-decibel steps, so
 * the argument is 0..63 and reads in HALF decibels.
 *
 * It is programmed through the 595, because its DATA, CLK and LE are
 * three of the 595's outputs - about twenty-two 595 writes per change.
 *
 * AND IT LATCHES, which is the part that bites. The attenuation
 * survives a firmware reload over SWD: this board keeps its own power,
 * and both the 595 and the PE4312 hold their state. **A bad value
 * written once stays written until something writes a good one** - and
 * one bad write cost an afternoon of measurements that all looked like
 * a broken filter bank, because every test after it ran through 31 dB
 * of attenuation that nothing in the firmware knew about.
 *
 * So call addon_known_state() early, rather than measuring through
 * whatever was left behind.
 */
void    addon_set_att_halfdb(uint8_t halfdb);
uint8_t addon_att_halfdb(void);

/*
 * Put the whole board somewhere known, in one call: power, latch line,
 * attenuator and filter. Everything this port measures should be
 * measured after this has run.
 */
void addon_known_state(uint32_t freq_hz, uint8_t att_halfdb);

/* The six lines as they stand, for printing. Bit n is PDn. */
uint32_t addon_odr(void);

/* Whether the board is powered at all. Everything else is meaningless
 * if this is false: the filter switches lose their supply with it. */
int addon_powered(void);

#endif /* MALAHIT_ADDON_H */
