#include "stm32h7xx.h"
#include "addon.h"
#include "board.h"
#include "clock.h"      /* clock_sysclk_hz, for the bit-bang timing */

#define PORT    GPIOD

/*
 * THE COIL PULSE, 10 ms, which is the factory firmware's own figure and
 * not a guess. Its two routines read, in Thumb:
 *
 *     orr  #0x800   PD11 = 1        set one coil
 *     bic  #0x100   PD8  = 0        make sure the other is off
 *     movs r0, #10  delay           ten
 *     bic  #0x800   PD11 = 0        and let go
 *
 * and the mirror image of it for PD8. A two-coil latching relay needs
 * exactly this: enough current to throw the armature and then nothing,
 * because the magnet holds it. The coil is rated for the pulse and not
 * for the hold, so the "clear the first" line is not tidiness.
 */
#define RELAY_PULSE_MS  10U

static void pin_output(uint32_t pin)
{
    PORT->MODER = (PORT->MODER & ~(3UL << (pin * 2U)))
                | (1UL << (pin * 2U));
}

void addon_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIODEN;
    (void)RCC->AHB4ENR;         /* the usual RCC read-back delay */

    /* Was the board already powered? Asked before anything is written:
     * see the settle at the end. */
    int was_on = addon_powered()
              && ((PORT->MODER >> (ADDON_PIN_VEXT * 2U)) & 3U) == 1U;

    /*
     * LEVELS BEFORE MODES. Setting MODER first would make a pin an
     * output carrying whatever ODR happened to hold, which for the two
     * coils is the one thing that must not happen. BSRR is atomic and
     * works whatever the pin's current mode is, so the safe order
     * costs nothing.
     *
     * Vext_ON and the latch line are driven HIGH rather than left
     * alone: if this image is ever started on a radio whose factory
     * firmware has not run, "leave it as it is" means "leave it an
     * analogue input" and the board silently has no power.
     */
    PORT->BSRR = (1UL << ADDON_PIN_VEXT)
               | (1UL << ADDON_PIN_LE)
               | (1UL << (ADDON_PIN_REL_50R + 16U))
               | (1UL << (ADDON_PIN_REL_HIZ + 16U));

    pin_output(ADDON_PIN_VEXT);
    pin_output(ADDON_PIN_LE);
    pin_output(ADDON_PIN_REL_50R);
    pin_output(ADDON_PIN_REL_HIZ);
    pin_output(ADDON_PIN_DIN);
    pin_output(ADDON_PIN_CLK);

    /*
     * AND IF IT WAS OFF, LET IT COME UP before anybody writes to it - for
     * addon_rail_ms, which is MEASURED, see below. A byte clocked into the
     * 595 too early is simply gone, with nothing to say so: neither part
     * on this board can be read back.
     *
     * Only on a power-up: a RAM load over a running image finds the board
     * already on and skips it.
     */
    if (!was_on) {
        board_delay_ms(addon_rail_ms);
    }
}

/*
 * HOW LONG THE BOARD NEEDS AFTER Vext_ON - and getting it wrong made the
 * radio deaf on cold starts, 20-21/11/2026.
 *
 * This was 10 ms, chosen from "the LDO needs well under a millisecond".
 * The board needs more than the LDO: test_addon_cold() in main.c powers
 * it down and up with the MCU warm and sweeps this wait, boot order,
 * 21/11/2026, room temperature:
 *
 *   wait     10   15   20   25   30   50  100  200 ms
 *   deaf    6/6  6/6  6/6  0/6  0/6  0/6  0/6  0/6
 *
 * - a threshold between 20 and 25 ms, sharp and repeatable, and not the
 * relay pulse (the same test without it was deaf 12 times in 12). It
 * looks like a power-on reset held by an RC on the board; that is a
 * guess, not checked against the add-on board's schematic. A lost code is no
 * signal at all, and nothing writes it again until the dial changes
 * band. The boot got away with it whenever its console lines happened
 * to push the writes past the threshold - intermittent, and a cold
 * morning found it.
 *
 * 200 ms: eight times the threshold, for a colder morning than the one
 * measured, and it costs a fifth of a second at power-on only.
 *
 * A variable, not a constant, only so that the test can sweep it.
 */
uint32_t addon_rail_ms = ADDON_RAIL_MS;

static void relay_pulse(uint32_t coil, uint32_t other)
{
    /* The other coil off FIRST and as its own write: energising both
     * at once is a short across the latching mechanism's intent and
     * leaves the armature wherever it feels like. */
    PORT->BSRR = (1UL << (other + 16U));

    PORT->BSRR = (1UL << coil);
    board_delay_ms(RELAY_PULSE_MS);
    PORT->BSRR = (1UL << (coil + 16U));

    /* And a moment for the contacts to stop bouncing before anything
     * measures what they did. */
    board_delay_ms(5U);
}

void addon_input_50r(void)
{
    relay_pulse(ADDON_PIN_REL_50R, ADDON_PIN_REL_HIZ);
}

void addon_input_highz(void)
{
    relay_pulse(ADDON_PIN_REL_HIZ, ADDON_PIN_REL_50R);
}

/* ------------------------------------------------------------------ */
/* The 74HC595                                                        */
/* ------------------------------------------------------------------ */

/*
 * A MICROSECOND BETWEEN EDGES, which is about fifty times longer than
 * a 74HC595 at 3.3 V needs.
 *
 * The part is specified for tens of megahertz and the factory firmware
 * calls a delay of 1 between its edges. The generosity here is not
 * about the chip: the link between the two boards is, in the vendor's
 * own words, "pieces of any wire" soldered by hand, with no ground
 * plane and no termination. An eight-bit write costs sixteen
 * microseconds at this rate and happens when a band changes, so there
 * is nothing to buy by going faster and a ringing clock edge to lose.
 */
#define ADDON_EDGE_US   1U

/*
 * THE RESTING VALUE OF THE THREE ATTENUATOR LINES, and it is not all
 * zeroes - which cost a run to find out.
 *
 * The factory firmware's PE4312 routine ends like this:
 *
 *     and  r3, #0x7f      ATT_DATA = 0
 *     orr  r3, #0x20      ATT_LE   = 1      <- the latch, rising edge
 *     bic  r3, #0x40      ATT_CLK  = 0
 *
 * so it leaves **ATT_LE HIGH** and the other two low. The first
 * version of addon_set_filter() forced ATT_LE LOW instead, on the
 * reasoning that a low latch line cannot produce a latch edge - true,
 * and beside the point, because it took the attenuator OUT of the
 * state the part is left sitting in. Every filter the test selected
 * then read the same 31 dB down, which looked exactly like "the
 * filters are not switching".
 *
 * The rule this replaces the reasoning with: **idle the lines where
 * the firmware that works idles them**, and change only the bits being
 * asked for.
 */
#define ADDON_595_IDLE  ADDON_B_ATT_LE

/* Shadow: the 595 cannot be read back. Initialised to the resting
 * value above rather than to zero, so that the first write this driver
 * makes is already the right shape. */
static uint8_t s_595 = ADDON_595_IDLE;

static void addon_nap(void)
{
    uint64_t t0 = board_cycles();
    uint64_t n  = ((uint64_t)clock_sysclk_hz * ADDON_EDGE_US) / 1000000U;

    while ((board_cycles() - t0) < n) { }
}

void addon_595(uint8_t byte)
{
    uint32_t i;

    /*
     * LATCH LOW FOR THE WHOLE SHIFT. The outputs are held by the
     * storage register meanwhile, so nothing downstream sees the bits
     * moving - which is what lets a filter change leave the
     * attenuator's three control lines alone.
     */
    PORT->BSRR = (1UL << (ADDON_PIN_LE + 16U));
    addon_nap();

    for (i = 0; i < 8U; i++) {
        /* MSB first: the first bit sent ends up in QH. */
        if (byte & 0x80U) { PORT->BSRR = (1UL << ADDON_PIN_DIN); }
        else { PORT->BSRR = (1UL << (ADDON_PIN_DIN + 16U)); }
        byte = (uint8_t)(byte << 1);
        addon_nap();

        PORT->BSRR = (1UL << ADDON_PIN_CLK);        /* rising: shifts */
        addon_nap();
        PORT->BSRR = (1UL << (ADDON_PIN_CLK + 16U));
        addon_nap();
    }

    /* And commit. Everything changes here, at once. */
    PORT->BSRR = (1UL << ADDON_PIN_LE);
    addon_nap();
}

uint8_t addon_595_shadow(void)
{
    return s_595;
}

/* ------------------------------------------------------------------ */
/* The filter bank                                                    */
/* ------------------------------------------------------------------ */

/*
 * PE42641 truth table, from the datasheet and already proven on the
 * main board's identical switches: RF1 = V2:0 V1:0, RF2 = 1,0,
 * RF3 = 0,1, RF4 = 1,1.
 *
 * Both switches here agree about which filter sits on which port, so
 * the same two bits go to both ends. See the warning in addon.h.
 */
/*
 * THE TWO SWITCHES NEED DIFFERENT CODES FOR THE SAME FILTER, and this
 * file said the opposite until the hardware was asked.
 *
 * Measured, by sweeping all sixteen codes against a 10 MHz carrier -
 * test_addon_filter_sweep() in main.c. Exactly one code passed it:
 *
 *     E12=0 E11=1 E22=0 E21=0      136 dB, snr 56   <- the highpass
 *     E12=0 E11=0 E22=0 E21=1       99 dB, snr 23   <- 1.5-4.5 leaking
 *     the other fourteen            84-87 dB        <- noise floor
 *
 * The second row is the confirmation worth having: a 10 MHz carrier
 * 37 dB down through a 1.5-4.5 MHz bandpass is a real filter's
 * stopband, and it lands on the one code the same truth table predicts
 * for that filter. Two independent rows, one table.
 *
 * Which gives the port order, and it is the VENDOR's drawing that is
 * right - `docs/Receiver20_Optional_PCB_EN.pdf`, not the KiCad rebuild:
 *
 *     input  U3   RF1 1.5-4.5   RF2 0.5-1.5   RF3 HPF     RF4 LPF
 *     output U4   RF1 HPF       RF2 LPF       RF3 1.5-4.5 RF4 0.5-1.5
 *
 * PE42641 truth table: RF1 = V2:0 V1:0, RF2 = 1,0, RF3 = 0,1, RF4 = 1,1.
 *
 * SO THIS BANK HAS THE SAME TRAP AS THE MAIN BOARD'S, which this file
 * previously stated it did not. Writing the same code to both ends
 * joins the input of one filter to the output of another, and the
 * result is not a weak signal but NO signal - fourteen rows of it.
 */
static const uint8_t k_filter_bits[4] = {
    /* 1M5_4M5   U3 RF1 (0,0)   U4 RF3 (0,1) */
    ADDON_B_BPFE21,
    /* 0M5_1M5   U3 RF2 (1,0)   U4 RF4 (1,1) */
    ADDON_B_BPFE12 | ADDON_B_BPFE22 | ADDON_B_BPFE21,
    /* HPF_4M5   U3 RF3 (0,1)   U4 RF1 (0,0) */
    ADDON_B_BPFE11,
    /* LPF_500K  U3 RF4 (1,1)   U4 RF2 (1,0) */
    ADDON_B_BPFE12 | ADDON_B_BPFE11 | ADDON_B_BPFE22
};

static addon_filter_t s_filter = ADDON_F_1M5_4M5;

void addon_set_filter(addon_filter_t f)
{
    uint8_t b;

    if ((uint32_t)f > (uint32_t)ADDON_F_LPF_500K) { return; }

    /*
     * Change the filter bits and NOTHING else: the three attenuator
     * lines keep whatever they are holding, which after any call to
     * this driver is ADDON_595_IDLE - ATT_LE high, the other two low,
     * exactly where the factory firmware leaves them.
     *
     * The 595 makes this free. Its outputs are frozen by the storage
     * register while the shift register is clocked, so the PE4312
     * never sees the bits shuffling past; all eight change once,
     * together, on the latch edge. With ATT_LE high before and after,
     * it is given no edge at all and holds its attenuation.
     */
    b = (uint8_t)((s_595 & ~ADDON_B_FILTER_MASK) | k_filter_bits[f]);

    addon_595(b);
    s_595    = b;
    s_filter = f;
}

void addon_595_filter_bits(uint8_t bits)
{
    uint8_t b = (uint8_t)((s_595 & ~ADDON_B_FILTER_MASK)
                          | (bits & ADDON_B_FILTER_MASK));

    addon_595(b);
    s_595 = b;
}

addon_filter_t addon_filter(void)
{
    return s_filter;
}

const char *addon_filter_name(addon_filter_t f)
{
    switch (f) {
    /* All 21 characters: console_printf() has no width modifier for a
     * string, so a column that has to line up is padded at the source. */
    case ADDON_F_1M5_4M5:  return "1.5-4.5 MHz bandpass ";
    case ADDON_F_0M5_1M5:  return "0.5-1.5 MHz bandpass ";
    case ADDON_F_HPF_4M5:  return "highpass above 4.5MHz";
    case ADDON_F_LPF_500K: return "lowpass below 500 kHz";
    default:               return "?                    ";
    }
}

addon_filter_t addon_filter_for(uint32_t freq_hz)
{
    if (freq_hz <  500000U) { return ADDON_F_LPF_500K; }
    if (freq_hz < 1500000U) { return ADDON_F_0M5_1M5; }
    if (freq_hz < 4500000U) { return ADDON_F_1M5_4M5; }
    return ADDON_F_HPF_4M5;
}

/* ------------------------------------------------------------------ */
/* The PE4312 step attenuator, programmed THROUGH the 595             */
/* ------------------------------------------------------------------ */

/*
 * SIX BITS, MSB FIRST, IN HALF-DECIBEL STEPS - and every one of them
 * costs three whole 595 writes, because the attenuator's DATA, CLK and
 * LE are three of the 595's outputs. About twenty-two 595 writes for
 * one attenuation change, which is a fraction of a millisecond and
 * only happens when a band or a level changes.
 *
 * The sequence is the factory firmware's, instruction for instruction:
 *
 *     ATT_LE = 0                      open the latch
 *     six times:
 *         ATT_DATA = next bit, MSB first
 *         ATT_CLK  = 0
 *         ATT_CLK  = 1                rising edge clocks it in
 *     ATT_DATA = 0
 *     ATT_LE   = 1                    rising edge latches, stays HIGH
 *     ATT_CLK  = 0
 *
 * Note where it ends: LE HIGH. That is the resting state of this part
 * on this board, it is what ADDON_595_IDLE encodes, and disagreeing
 * with it is what made the first filter test report a dead receiver.
 *
 * The factory passes whole decibels and doubles them on the way in
 * (`lsls r5, r0, #1`), which is how we know the register is in half
 * decibels and which way up it is.
 *
 * AND IT LATCHES. Once written, the attenuation survives a firmware
 * reload over SWD, because this board keeps its own power and both the
 * 595 and the PE4312 hold their state. A bad value written once stays
 * written until something writes a good one - which cost an afternoon
 * of measurements that looked like a broken filter bank.
 */
#define ADDON_ATT_MAX_HALFDB    63U     /* 31.5 dB */

static uint8_t s_att = ADDON_ATT_MAX_HALFDB;    /* unknown until set */

void addon_set_att_halfdb(uint8_t halfdb)
{
    uint8_t b = s_595;
    uint8_t v;
    uint32_t i;

    if (halfdb > ADDON_ATT_MAX_HALFDB) { halfdb = ADDON_ATT_MAX_HALFDB; }
    v = halfdb;

    b = (uint8_t)(b & ~ADDON_B_ATT_LE);         /* open the latch */
    addon_595(b);

    for (i = 0; i < 6U; i++) {
        if (v & 0x20U) { b = (uint8_t)(b | ADDON_B_ATT_DATA); }
        else { b = (uint8_t)(b & ~ADDON_B_ATT_DATA); }
        v = (uint8_t)(v << 1);
        addon_595(b);

        b = (uint8_t)(b & ~ADDON_B_ATT_CLK);
        addon_595(b);
        b = (uint8_t)(b | ADDON_B_ATT_CLK);     /* rising: shifts */
        addon_595(b);
    }

    b = (uint8_t)(b & ~ADDON_B_ATT_DATA);
    addon_595(b);
    b = (uint8_t)(b | ADDON_B_ATT_LE);          /* rising: latches */
    addon_595(b);
    b = (uint8_t)(b & ~ADDON_B_ATT_CLK);
    addon_595(b);

    s_595 = b;
    s_att = halfdb;

    board_delay_ms(1);
}

uint8_t addon_att_halfdb(void)
{
    return s_att;
}

void addon_known_state(uint32_t freq_hz, uint8_t att_halfdb)
{
    addon_init();
    addon_set_att_halfdb(att_halfdb);
    addon_set_filter(addon_filter_for(freq_hz));
}

uint32_t addon_odr(void)
{
    return PORT->ODR;
}

int addon_powered(void)
{
    return (PORT->ODR & (1UL << ADDON_PIN_VEXT)) ? 1 : 0;
}
