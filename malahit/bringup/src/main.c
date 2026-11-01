/*
 * Step 4 of the Malahit DSP-1 port: the codec, and the question the whole
 * port turns on.
 *
 * PORTING.md section 3.2 is the reason this image exists. Upstream runs
 * 48 / 96 / 192 kHz and decimates the SSB branch by four to 12 kHz, which
 * is the rate HFDL, STANAG, FT8 and every other digital mode expects at
 * its input. The stock Malahit firmware runs 160 / 80 / 40 kHz, and 160/4
 * is 20 kHz - so if this radio cannot hold 192 kHz, every filter corner,
 * decimation factor and time constant downstream has to be retuned, or a
 * resampler inserted in front of all of it.
 *
 * The datasheet says the answer is no. IMCLK is always 256 x Fs and
 * "should not exceed 12.288 MHz under any condition", which caps the part
 * at 48 kHz. The factory firmware already exceeds that by more than three
 * times and works, so the margin is real - but it is undocumented margin,
 * and the difference between 160 and 192 kHz is the difference between a
 * driver swap and a retune of the entire DSP tree.
 *
 * So this image asks the hardware. It:
 *
 *   1. proves the write-only control bus by an effect, since there is no
 *      way to read a register back;
 *   2. measures the frame rate SAI1 actually produces, against the CPU
 *      clock, rather than trusting the dividers;
 *   3. sweeps the sample rate and reports, for each one, whether the
 *      codec is still converting;
 *   4. plays a tone, so the last word belongs to the speaker.
 *
 * Runs from RAM. Flash is untouched:
 *
 *     ../tools/swd.sh ram build/bringup.elf
 */

#include <stddef.h>

#include "stm32h7xx.h"

#include "board.h"
#include "clock.h"
#include "console.h"
#include "fault.h"
#include "nau8822.h"
#include "sai1.h"
#include "msi001.h"
#include "iqmeas.h"
#include "fft.h"
#include "spectrum.h"
#include "spi3.h"
#include "ili9488.h"

/*
 * HOW FAR DID IT GET. A single number at a fixed address, bumped at each
 * stage, readable over SWD with one command:
 *
 *     swd.sh -c init -c halt -c "mdw <addr> 1"
 *
 * The console has produced zero bytes while the firmware was demonstrably
 * executing more than once in this project, and when the evidence and the
 * machine disagree the machine wins. Deliberately not in .bss: zero has
 * to mean "stopped before the first marker".
 */
uint32_t g_phase = 0xFFFFFFFFU;
#define PHASE(n)  do { g_phase = (n); __DSB(); } while (0)

/* ------------------------------------------------------------------ */
/* A tone, without libm                                               */
/* ------------------------------------------------------------------ */

/*
 * One cycle of a sine in 64 steps, amplitude 12000 of 32767.
 *
 * Written out rather than computed because this image links -nostdlib:
 * there is no sinf() and there should not be one. 12000 leaves 8 dB of
 * digital headroom, which keeps the DAC away from its own clipping while
 * the question on the table is the codec's clocking and not its
 * distortion.
 */
static const int16_t sine64[64] = {
         0,   1176,   2341,   3483,   4592,   5657,   6667,   7613,
      8485,   9276,   9978,  10583,  11087,  11483,  11769,  11942,
     12000,  11942,  11769,  11483,  11087,  10583,   9978,   9276,
      8485,   7613,   6667,   5657,   4592,   3483,   2341,   1176,
         0,  -1176,  -2341,  -3483,  -4592,  -5657,  -6667,  -7613,
     -8485,  -9276,  -9978, -10583, -11087, -11483, -11769, -11942,
    -12000, -11942, -11769, -11483, -11087, -10583,  -9978,  -9276,
     -8485,  -7613,  -6667,  -5657,  -4592,  -3483,  -2341,  -1176
};

/*
 * Build a whole number of cycles of a tone into a buffer, by phase
 * accumulation, so the buffer loops seamlessly at any sample rate.
 *
 * The step is computed in 32-bit fixed point: a full turn is 2^32, so
 * one sample advances freq * 2^32 / fs and the top six bits of the
 * accumulator index the table. No division per sample and no rounding
 * drift that accumulates into an audible chirp over a long play.
 */
static void tone_fill(int16_t *buf, uint32_t frames,
                      uint32_t freq_hz, uint32_t fs_hz)
{
    /*
     * THE BUFFER MUST HOLD A WHOLE NUMBER OF CYCLES, AND A PHASE
     * ACCUMULATOR DOES NOT GIVE YOU THAT BY ITSELF.
     *
     * This function used to take the frequency literally, and the
     * comment above it claimed the result "loops seamlessly at any
     * sample rate". It does not. 1000 Hz at 48 kHz is 48 samples a
     * cycle, and 1024 frames is 21.33 of them - so playing the buffer
     * round and round put a phase jump at every wrap, 47 times a
     * second. That is not a tone with a fault in it, it is a tone with
     * a 47 Hz buzz added, and it was audible immediately as crackling.
     *
     * So round the frequency to the nearest one that fits a whole
     * number of cycles into the buffer. The granularity is fs/frames -
     * 46.875 Hz here - which is far finer than anything that matters
     * when the question is "does a tone come out".
     */
    uint32_t cycles = (freq_hz * frames + fs_hz / 2U) / fs_hz;

    if (cycles == 0U) { cycles = 1U; }

    /*
     * step is then exactly cycles/frames of a turn per sample, in 32-bit
     * fixed point. The accumulator wraps to exactly zero after `frames`
     * samples, so the join is seamless by construction rather than by
     * hope.
     */
    uint32_t step = (uint32_t)(((uint64_t)cycles << 32) / frames);

    uint32_t phase = 0;

    for (uint32_t i = 0; i < frames; i++) {
        /*
         * Interpolate between table entries. Sixty-four points a cycle
         * sounds like plenty and is not: taking the nearest entry
         * quantises the PHASE to 1/64 of a turn, which is about 30 dB
         * of harmonic distortion - a tone that is recognisably a tone
         * and recognisably rough. Since the point of this tone is to be
         * judged by ear, the ear should not be judging the table size.
         *
         * Top 6 bits of the accumulator choose the entry, the next 16
         * are the fraction between it and the one after.
         */
        uint32_t idx  = phase >> 26;
        uint32_t frac = (phase >> 10) & 0xFFFFU;
        int32_t  a    = sine64[idx];
        int32_t  b    = sine64[(idx + 1U) & 63U];
        int16_t  s    = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));

        buf[i * 2U]      = s;
        buf[i * 2U + 1U] = s;    /* both channels: the speaker is bridged */
        phase += step;
    }
}

/* ------------------------------------------------------------------ */
/* Buffers                                                           */
/* ------------------------------------------------------------------ */

/*
 * 8192 stereo frames is 32 kB, and at 192 kHz it is 43 ms - long enough
 * for the statistics below to settle and short enough that a sweep of
 * nine rates does not outlast anyone's patience. AXI SRAM has 512 kB and
 * this whole image is under 20 kB, so the space is free.
 */
/*
 * The spectrum offset. The generator is put 20 kHz above the tuning so
 * its peak lands well to the right of centre on screen - about 107 bins
 * at 187.5 Hz a bin, which is 50 pixels from the middle. Far enough that
 * it cannot be confused with the LO leakage that sits at the centre, and
 * close enough to be plainly on the same display.
 */
#define SPEC_OFFSET_HZ  20000

#define CAP_FRAMES   8192U
static int16_t s_rx[CAP_FRAMES * 2U];

/*
 * FFT working arrays. 1024 floats each is 4 kB apiece - cheap, and
 * static rather than on the stack because 12 kB of locals in main()
 * would be most of a default stack frame budget and the fault it caused
 * would look like anything but an array.
 */
static float   s_fft_re[FFT_N];
static float   s_fft_im[FFT_N];
static int16_t s_db10[FFT_N];
static int16_t s_tone[1024U * 2U];

/* ------------------------------------------------------------------ */
/* Statistics                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t  mean;          /* DC level - with the ADC high-pass off,
                             * this is a real measurement and not an
                             * artefact to be filtered away */
    uint32_t rms;           /* about the mean, so it is the AC part */
    int16_t  min, max;
    uint32_t stuck;         /* 1 if every sample was identical */
} chan_stats_t;

static void stats(const int16_t *buf, uint32_t frames, uint32_t slot,
                  chan_stats_t *out)
{
    int64_t  sum  = 0;
    uint64_t sumq = 0;
    int16_t  mn = 32767, mx = -32768;

    for (uint32_t i = 0; i < frames; i++) {
        int16_t v = buf[i * 2U + slot];
        sum += v;
        if (v < mn) { mn = v; }
        if (v > mx) { mx = v; }
    }

    int32_t mean = (int32_t)(sum / (int64_t)frames);

    for (uint32_t i = 0; i < frames; i++) {
        int32_t d = (int32_t)buf[i * 2U + slot] - mean;
        sumq += (uint64_t)((int64_t)d * d);
    }

    /*
     * Integer square root by bisection on the bit pattern. Cheaper than
     * explaining why a floating point sqrt in a bring-up image is fine,
     * and exact.
     */
    uint64_t var = sumq / frames;
    uint32_t r   = 0;
    for (uint32_t bit = 1UL << 15; bit != 0U; bit >>= 1) {
        uint32_t t = r | bit;
        if ((uint64_t)t * t <= var) { r = t; }
    }

    out->mean  = mean;
    out->rms   = r;
    out->min   = mn;
    out->max   = mx;
    out->stuck = (mn == mx) ? 1U : 0U;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                          */
/* ------------------------------------------------------------------ */

static const char *clock_error_text(int status)
{
    switch (status) {
    case CLOCK_OK:         return "ok";
    case CLOCK_ERR_VOSRDY: return "core voltage never became ready (VOSRDY)";
    case CLOCK_ERR_HSERDY: return "HSE never started - X1 unpowered? it is on the SWITCHED 3.3V rail (PC6)";
    case CLOCK_ERR_PLLRDY: return "PLL1 never locked";
    case CLOCK_ERR_SWITCH: return "SYSCLK never switched to PLL1";
    default:               return "unknown";
    }
}

static void banner(void)
{
    console_printf("\n\n");
    console_printf("Malahit DSP-1 - bring-up step 4: NAU8822 and SAI1\n");
    console_printf("=================================================\n");

    console_printf("unique id : %08x %08x %08x\n",
                   *(volatile uint32_t *)0x1FF1E800,
                   *(volatile uint32_t *)0x1FF1E804,
                   *(volatile uint32_t *)0x1FF1E808);
    console_printf("last reset: %s\n", board_reset_cause());
    console_printf("clock     : %s, SYSCLK %u Hz\n",
                   clock_error_text(clock_status), clock_sysclk_hz);

    if (clock_status != CLOCK_OK) {
        console_printf("\n"
            "  *** Running on HSI. Every sample rate below will be\n"
            "  *** derived from a 22 MHz reference that is not there,\n"
            "  *** so the measurements are meaningless. Fix the clock\n"
            "  *** first.\n");
    }
    console_printf("\n");
}

/*
 * Print what the solver would do for a rate, without doing it. Useful
 * for rates that are never applied, and it makes the arithmetic in
 * sai1.h checkable against the console rather than against trust.
 */
static void print_plan(uint32_t fs)
{
    sai1_clock_t c;

    sai1_solve(fs, &c);

    if (!c.ok) {
        console_printf("  %7u | no PLL2 configuration exists\n", fs);
        return;
    }

    /*
     * Right-aligned widths only. console_printf() is a few dozen lines
     * in console.c and has no left-justify flag; a stray "%-3u" there
     * prints the literal characters, which is a tidy way to make a
     * table unreadable.
     */
    console_printf("  %7u | %3u %4u/8192 /%3u /%2u | %9u | %9u | %5d\n",
                   fs, c.divn, c.fracn, c.divp, c.mckdiv,
                   c.vco_hz, c.mclk_hz, c.err_ppb);
}

/* ------------------------------------------------------------------ */
/* The two tests that answer the two questions                        */
/* ------------------------------------------------------------------ */


/*
 * CAPTURE, OR SAY SO. NEVER AVERAGE A BUFFER NOBODY REFILLED.
 *
 * This exists because of a bug in this very program that produced the
 * most convincing wrong answer of the whole step. test_control_bus()
 * called sai1_stream() and then computed statistics over CAP_FRAMES
 * regardless of how many frames had actually arrived - so when a
 * capture returned nothing, stats() happily re-averaged the PREVIOUS
 * capture and printed it as the result of the new register setting.
 *
 * The output was two register settings with byte-identical means, down
 * to the last digit, which reads as "this bit does nothing" when it
 * really meant "this measurement never happened". Identical numbers
 * from an analogue measurement should have been unbelievable on sight;
 * instead they were nearly believed.
 *
 * Returns the frame count, zero if the capture failed, and prints the
 * SAI status when it does - FLVL tells an empty FIFO apart from a
 * stopped one.
 */
static uint32_t capture(void)
{
    uint32_t got = sai1_stream(NULL, 0U, s_rx, CAP_FRAMES);

    if (got < CAP_FRAMES) {
        console_printf("    *** capture got %u of %u frames; "
                       "SR_A 0x%08x SR_B 0x%08x ***\n",
                       got, CAP_FRAMES, sai1_sr_a(), sai1_sr_b());
    }
    return got;
}

/*
 * TEST 0: WHICH RECEIVE TIMING IS THE RIGHT ONE?
 *
 * This has to run before anything else looks at a sample, because with
 * the wrong one every later measurement is a measurement of the wrong
 * bits - and convincingly so. The first version of this program ran
 * with the RM0433 reading of CKSTR and reported a railed converter at
 * every sample rate from 8 to 384 kHz. The raw words said otherwise:
 *
 *     7f65 ff62 ff68 ff64 ff68 7f65 7f69 7f65
 *
 * Only bit 15 moves. Everything below it is near-constant. That is not
 * a loud signal and not an overload, it is one 16-bit window sitting
 * one bit to the left of the data: the low 15 bits are the top 15 bits
 * of a true sample of 0xFECA - which is -310, a perfectly ordinary
 * quiet input - and the sign bit is the last bit of the previous slot,
 * toggling because the previous sample's LSB is noise.
 *
 * So rather than pick a reading of the documentation and hope, try all
 * four combinations and let the numbers choose. The right one is
 * obvious once seen: a quiet input gives small numbers near zero, and
 * the three wrong ones give something near half scale.
 *
 * The score is the mean absolute sample value. The correct timing
 * minimises it, and by a factor of a hundred rather than a few percent,
 * which is the kind of margin that does not need a judgement call.
 */
static void test_rx_timing(void)
{
    chan_stats_t sl, sr;
    int best_ckstr = 0, best_fsoff = 1;
    uint32_t best_score = 0xFFFFFFFFU;

    console_printf("Test 0 - receive timing, settled by measurement\n");
    console_printf("----------------------------------------------\n");
    console_printf("  RM0433 and ST's own HAL disagree about CKSTR for a\n");
    console_printf("  receiver, so ask the hardware. A quiet input should\n");
    console_printf("  read near zero; a one-bit shift reads near half\n");
    console_printf("  scale with only the sign bit moving.\n\n");
    console_printf("  CKSTR FSOFF |  L mean  L min  L max |  R mean | "
                   "first words\n");
    console_printf("  ------------+-----------------------+---------+"
                   "------------\n");

    for (int ck = 0; ck <= 1; ck++) {
        for (int fo = 1; fo >= 0; fo--) {
            sai1_set_rx_timing(ck, fo);
            if (capture() == 0U) { continue; }
            stats(s_rx, CAP_FRAMES, 0U, &sl);
            stats(s_rx, CAP_FRAMES, 1U, &sr);

            /*
             * Mean magnitude, not the mean itself: a shifted frame can
             * average anywhere depending on how the sign bit lands, but
             * its individual samples are always large.
             */
            uint64_t acc = 0;
            for (uint32_t i = 0; i < CAP_FRAMES; i++) {
                int32_t v = s_rx[i * 2U];
                acc += (uint64_t)((v < 0) ? -v : v);
            }
            uint32_t score = (uint32_t)(acc / CAP_FRAMES);

            console_printf("    %d     %d   | %7d %6d %6d | %7d | "
                           "%04x %04x %04x\n",
                           ck, fo, sl.mean, sl.min, sl.max, sr.mean,
                           (uint16_t)s_rx[0], (uint16_t)s_rx[2],
                           (uint16_t)s_rx[4]);

            if (score < best_score) {
                best_score  = score;
                best_ckstr  = ck;
                best_fsoff  = fo;
            }
        }
    }

    sai1_set_rx_timing(best_ckstr, best_fsoff);
    console_printf("\n  chosen: CKSTR=%d FSOFF=%d, mean |sample| = %u\n\n",
                   best_ckstr, best_fsoff, best_score);
}

/*
 * TEST 1: DOES THE WRITE-ONLY CONTROL BUS WORK?
 *
 * There is no read path - the MODE pin is strapped for SPI 3-wire write
 * mode, which has no fourth wire - so a register can only be shown to
 * have taken a value by its effect. Three effects are measured here,
 * each one a single bit, each one independent of the others:
 *
 *   LADCPL  inverts the sign of the left ADC output. With the high-pass
 *           filter off the channel carries a standing DC offset, so this
 *           flips the sign of a large number. Unambiguous.
 *   ADCPHS  swaps which half of the I2S frame each ADC occupies. The two
 *           channels have different offsets, so this exchanges the two
 *           measurements - and it also catches a frame alignment error
 *           in the SAI, because if left and right were already swapped
 *           this test un-swaps them.
 *   PGA     gain moves the whole chain, offset included.
 *
 * If all three move, the bus works, the ADC is converting, and the I2S
 * link is carrying real samples. If none of them move, nothing below
 * this line means anything.
 */
static void test_control_bus(void)
{
    chan_stats_t a_l, a_r, b_l, b_r;

    console_printf("Test 1 - the write-only control bus\n");
    console_printf("-----------------------------------\n");
    console_printf("  There is no read-back pin on this part, so every\n");
    console_printf("  register is verified by its effect on the samples.\n\n");

    nau8822_set_adc_invert(0);
    nau8822_set_adc_swap(0);
    board_delay_ms(20);
    if (capture() == 0U) {
        console_printf("  no frames at all - nothing below can be read.\n\n");
        return;
    }
    stats(s_rx, CAP_FRAMES, 0U, &a_l);
    stats(s_rx, CAP_FRAMES, 1U, &a_r);

    console_printf("  as configured   : L mean %6d rms %5u "
                   "min %6d max %6d\n",
                   a_l.mean, a_l.rms, a_l.min, a_l.max);
    console_printf("                    R mean %6d rms %5u "
                   "min %6d max %6d\n",
                   a_r.mean, a_r.rms, a_r.min, a_r.max);

    if (a_l.stuck && a_r.stuck) {
        console_printf("\n  *** BOTH CHANNELS ARE A CONSTANT. Either the\n"
                       "  *** ADCs are not running, or SD_B carries\n"
                       "  *** nothing, or MCLK never reached PE2.\n\n");
    }

    /*
     * THE RAW WORDS, BEFORE ANY INTERPRETATION.
     *
     * A mean near 16384 with an RMS near 16384 is arithmetically what you
     * get from samples that alternate between zero and full scale, and
     * that is a very different fault from a loud signal - it is the
     * signature of either an overloaded converter or a frame read at the
     * wrong bit offset. Summary statistics cannot tell those apart and
     * the raw words can, so print them. Every previous surprise on this
     * board was settled by dumping bytes rather than by reasoning about
     * them.
     */
    console_printf("\n  first 16 frames as received, hex (slot0 slot1):\n   ");
    for (uint32_t i = 0; i < 16U; i++) {
        console_printf(" %04x:%04x", (uint16_t)s_rx[i * 2U],
                       (uint16_t)s_rx[i * 2U + 1U]);
        if ((i & 3U) == 3U) { console_printf("\n   "); }
    }
    console_printf("\n");

    /*
     * WHICH SIDE OF THE CONVERTER IS THE PROBLEM ON.
     *
     * (Kept after the receive-timing search below found its answer,
     * because these two mutes are what proved the search was looking in
     * the right place at all: the digital mute returning exact zeros
     * said the MCU really was reading the converter, which ruled out
     * every "the codec is not running" explanation in one line.)
     *
     * Two mutes, in the order that narrows fastest. The digital one
     * first, because if it does nothing then nothing else measured here
     * is about the codec at all.
     */
    nau8822_set_adc_digital_mute(1);
    board_delay_ms(30);
    (void)capture();
    stats(s_rx, CAP_FRAMES, 0U, &b_l);
    stats(s_rx, CAP_FRAMES, 1U, &b_r);
    nau8822_set_adc_digital_mute(0);

    console_printf("\n  ADC digital mute (R15/R16 = 0):\n");
    console_printf("    L mean %6d rms %5u min %6d max %6d\n",
                   b_l.mean, b_l.rms, b_l.min, b_l.max);
    console_printf("    R mean %6d rms %5u min %6d max %6d\n",
                   b_r.mean, b_r.rms, b_r.min, b_r.max);
    console_printf("    -> silence here means the MCU really is reading\n");
    console_printf("       the ADC, and the framing is right. No change\n");
    console_printf("       means it is reading something else entirely.\n");

    nau8822_set_pga_mute(1);
    board_delay_ms(30);
    (void)capture();
    stats(s_rx, CAP_FRAMES, 0U, &b_l);
    stats(s_rx, CAP_FRAMES, 1U, &b_r);
    nau8822_set_pga_mute(0);

    console_printf("\n  input PGA muted (R45/R46 bit 6):\n");
    console_printf("    L mean %6d rms %5u min %6d max %6d\n",
                   b_l.mean, b_l.rms, b_l.min, b_l.max);
    console_printf("    R mean %6d rms %5u min %6d max %6d\n",
                   b_r.mean, b_r.rms, b_r.min, b_r.max);
    console_printf("    -> what the converter reads with nothing\n");
    console_printf("       connected to it: its own noise floor plus\n");
    console_printf("       its own offset. The difference from the\n");
    console_printf("       first measurement is the tuner's DC.\n");

    /* LADCPL: invert the left channel only. */
    nau8822_set_adc_invert(1);
    board_delay_ms(20);
    (void)capture();
    stats(s_rx, CAP_FRAMES, 0U, &b_l);
    stats(s_rx, CAP_FRAMES, 1U, &b_r);
    nau8822_set_adc_invert(0);

    console_printf("  R14[0] LADCPL=1 : L mean %6d            "
                   "R mean %6d          %s\n",
                   b_l.mean, b_r.mean,
                   ((a_l.mean > 8 && b_l.mean < -8)
                    || (a_l.mean < -8 && b_l.mean > 8))
                       ? "<- L sign flipped, bus works"
                       : "<- no sign flip");

    /* ADCPHS: swap the two channels in the frame. */
    nau8822_set_adc_swap(1);
    board_delay_ms(20);
    (void)capture();
    stats(s_rx, CAP_FRAMES, 0U, &b_l);
    stats(s_rx, CAP_FRAMES, 1U, &b_r);
    nau8822_set_adc_swap(0);

    console_printf("  R4[1]  ADCPHS=1 : L mean %6d            "
                   "R mean %6d          <- should be the pair above,\n"
                   "                                                 "
                   "               exchanged\n",
                   b_l.mean, b_r.mean);

    /* PGA gain: 0 dB, then +24 dB. */
    console_printf("\n  PGA gain sweep (R45/R46, 0.75 dB a step):\n");
    for (uint32_t g = 0; g <= 48U; g += 8U) {
        nau8822_set_pga_gain((uint8_t)g);
        board_delay_ms(20);
        sai1_stream(NULL, 0U, s_rx, CAP_FRAMES);
        stats(s_rx, CAP_FRAMES, 0U, &b_l);
        stats(s_rx, CAP_FRAMES, 1U, &b_r);

        /* gain in dB x 4, to avoid floating point on the console */
        int32_t db4 = (int32_t)g * 3 - 48;
        console_printf("    gain %2u (%4d.%02u dB) : "
                       "L mean %6d rms %5u   R mean %6d rms %5u\n",
                       g, db4 / 4, (uint32_t)((db4 < 0 ? -db4 : db4) % 4) * 25,
                       b_l.mean, b_l.rms, b_r.mean, b_r.rms);
    }

    nau8822_set_pga_gain(16U);          /* back to 0 dB */
    console_printf("\n");
}

/*
 * TEST 2: HOW FAST WILL IT RUN?
 *
 * For each rate: retune PLL2, measure the frame rate against the CPU
 * clock, count over/underruns, and look at what the ADC produced.
 *
 * The stability criterion is deliberately three separate things, because
 * each one fails on its own:
 *
 *   measured Fs    catches a divider the hardware refused, or a PLL that
 *                  locked somewhere else. This is the measurement step 1
 *                  established as the only trustworthy kind - SysTick
 *                  against the real world, not arithmetic against hope.
 *   over/underrun  catches a data path that cannot keep up.
 *   ADC statistics catch the case this test exists for: the clocks are
 *                  perfect, the bus is perfect, and the codec's
 *                  modulator has quietly given up. That shows as a
 *                  stuck value, a railed output, or a noise floor that
 *                  jumps by an order of magnitude.
 */
static void test_rate_sweep(void)
{
    static const uint32_t rates[] = {
          8000U,  32000U,  48000U,  96000U, 160000U,
        192000U, 240000U, 256000U, 320000U, 384000U
    };

    console_printf("Test 2 - how fast will this codec actually run\n");
    console_printf("---------------------------------------------\n");
    console_printf("  Datasheet ceiling is 48 kHz (IMCLK 12.288 MHz).\n");
    console_printf("  The factory firmware runs 160 kHz. 192 kHz is what\n");
    console_printf("  the upstream DSP tree wants. Everything above that\n");
    console_printf("  is here to find the edge rather than to be used.\n\n");

    console_printf("   target |   measured | err  |  L rms |  R rms |"
                   "  L min |  L max | u/o | verdict\n");
    console_printf("  --------+------------+------+--------+--------+"
                   "--------+--------+-----+--------\n");

    uint32_t prev_rms = 0;

    for (uint32_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        uint32_t fs = rates[i];
        chan_stats_t sl, sr;

        board_watchdog_feed();

        int rc = sai1_set_fs(fs);
        if (rc != SAI1_OK) {
            console_printf("  %7u |  sai1_set_fs failed, rc %d\n", fs, rc);
            continue;
        }

        const sai1_clock_t *c = sai1_clock();

        /* MCLK has just changed. Let the codec's converters resettle
         * before anything they produce is treated as data. */
        board_delay_ms(30);

        uint32_t frames = CAP_FRAMES;
        uint32_t got    = sai1_stream(NULL, 0U, s_rx, frames);

        if (got < 64U) {
            console_printf("  %7u | %7u |   NO FRAMES - the frame clock "
                           "is not running\n", fs, c->fs_hz);
            continue;
        }

        stats(s_rx, got, 0U, &sl);
        stats(s_rx, got, 1U, &sr);

        uint32_t meas = sai1_measured_fs();

        /*
         * The measured rate against the one asked for, in tenths of a
         * percent. A few tenths is the SysTick window and the
         * oscillator's own tolerance; anything in whole percent is a
         * divider that did not do what it was told.
         */
        int32_t dev = (int32_t)(((int64_t)meas - (int64_t)fs) * 1000
                                / (int64_t)fs);

        /*
         * THE NOISE FLOOR HAS TO FOLLOW sqrt(Fs), AND THAT IS THE REAL
         * TEST OF WHETHER THE CONVERTER IS STILL CONVERTING.
         *
         * A sigma-delta spreads its quantisation noise over the whole
         * sampled band, so doubling the rate should raise the in-band
         * RMS by about sqrt(2) and no more. Measured on this part the
         * trend holds beautifully from 8 kHz to 320 kHz - rms 2, 3, 6,
         * 7, 10, 11, 15 - which is positive evidence that the modulator
         * is working at every one of those rates, not merely that
         * plausible numbers arrived.
         *
         * At 384 kHz it breaks: rms 182 against 15 one step earlier,
         * with the extremes blowing out to -4142 and +9995. That is a
         * modulator losing stability, and it is the answer this sweep
         * was built to find.
         *
         * Checking the trend rather than looking for a railed sample is
         * what catches it. The first version tested min and max against
         * full scale, and at 384 kHz the breakup is violent but not yet
         * full scale - so it passed, and the one row that mattered was
         * the one row reported wrongly. A fixed threshold answers "is it
         * saturated"; the question is "is it still behaving".
         */
        const char *verdict;
        uint32_t worst = (sl.rms > sr.rms) ? sl.rms : sr.rms;

        if (dev > 20 || dev < -20) {
            verdict = "CLOCK WRONG";
        } else if (sai1_tx_underruns() || sai1_rx_overruns()) {
            verdict = "fifo";
        } else if (sl.stuck || sr.stuck) {
            verdict = "ADC STUCK";
        } else if (sl.min <= -32700 || sl.max >= 32700
                   || sr.min <= -32700 || sr.max >= 32700) {
            verdict = "RAILED";
        } else if (prev_rms != 0U && worst > prev_rms * 3U) {
            verdict = "BREAKING UP";
        } else {
            verdict = "ok";
        }

        prev_rms = worst;

        console_printf("  %7u | %10u | %4d | %6u | %6u | %6d | %6d | "
                       "%u/%u | %s\n",
                       fs, meas, dev,
                       sl.rms, sr.rms, sl.min, sl.max,
                       sai1_tx_underruns(), sai1_rx_overruns(),
                       verdict);
    }

    console_printf("\n  err is (measured - target), in tenths of a percent.\n");
    console_printf("  u/o is transmit underruns / receive overruns.\n\n");
}

/* ------------------------------------------------------------------ */


/* ================================================================== */
/* STEP 5: THE TUNER                                                  */
/* ================================================================== */

/*
 * The test signal, and why it is placed 3 kHz off.
 *
 * A zero-IF receiver puts the tuned frequency at DC, and DC is the one
 * place in the spectrum where nothing can be measured cleanly: it is
 * where the mixer's own offset sits, where the tuner's DC calibration
 * digs its hole, and where any amplifier's drift appears. A carrier
 * parked there would be measured together with all of that.
 *
 * So the receiver is tuned deliberately BESIDE the carrier, by exactly
 * Fs/64 = 3000 Hz at 192 kHz, and the carrier is measured in that bin.
 * Fs/64 is not an arbitrary choice either - it is the one offset whose
 * reference phasor is exactly the 64-entry sine table, so the
 * measurement needs no window, leaks nothing from DC, and is coherent
 * to the last bit. See iqmeas.h.
 */
#define GEN_HZ          10000000U       /* the generator, as set by hand */
#define OFFSET_HZ       (192000U / IQ_BIN_DIVISOR)      /* 3000 Hz */
#define TUNE_HZ         (GEN_HZ - OFFSET_HZ)

/* Capture and measure, reporting a failed capture rather than averaging
 * a stale buffer - the lesson from step 4. */
static int measure(iq_result_t *r)
{
    if (capture() == 0U) { return 0; }
    iq_measure(s_rx, CAP_FRAMES, r);
    return 1;
}


/*
 * THE CARRIER IS NOT ON THE BIN, AND IT CANNOT BE.
 *
 * iq_measure() evaluates one DFT bin at exactly Fs/64, which is
 * perfect arithmetic and the wrong instrument for an absolute level.
 * The generator and this radio do not share a reference: the carrier
 * arrives at 3000 Hz plus whatever the two oscillators disagree by. The
 * single bin is 192000/8192 = 23.4 Hz wide, so an offset of a hundred
 * hertz puts the carrier several bins away and the measurement reads
 * mostly window skirt.
 *
 * That is exactly what happened. Step 5's front-end table reported the
 * carrier at -20.6 dBFS; the FFT, which looks for the peak wherever it
 * actually is, finds the same signal at -4 dBFS. Sixteen decibels, and
 * the single-bin figure was the wrong one.
 *
 * The RELATIVE numbers in that table survive - port 1 against port 2,
 * preamp on against off, each gain-reduction step against the last -
 * because the frequency offset is the same for all of them and cancels
 * in a ratio. The absolute dBFS figures do not survive, and the image
 * rejection does, being a ratio of two bins equally far off-tune.
 *
 * So: peak_dbfs10() for anything absolute. It costs an FFT, which is
 * 1.7 ms against a 43 ms capture.
 */
static int32_t peak_dbfs10(int32_t *offset_hz)
{
    spectrum_peak_t pk;

    if (capture() == 0U) { return -1800; }

    fft_load(s_rx, s_fft_re, s_fft_im);
    fft_run(s_fft_re, s_fft_im);
    fft_spectrum_db10(s_fft_re, s_fft_im, s_db10);
    spectrum_peak(s_db10, 192000U, &pk);

    if (offset_hz != NULL) { *offset_hz = pk.offset_hz; }
    return pk.db10;
}

static void print_tune_plan(uint32_t f, msi_band_t band);

static void tuner_report_plans(void)
{
    console_printf("MSi001 tuning plan, before anything is written\n");
    console_printf("----------------------------------------------\n");
    console_printf("  FREF1 22 MHz, so XTAL_SEL=001 and the AM first IF\n");
    console_printf("  is 132 MHz. Step is 4xFREF1/(LODIV x THRESH x 4096).\n\n");
    console_printf("        want |  band | LODIV |     FSYNTH | INT  FRAC  AFC"
                   " |    actual | err\n");
    console_printf("  -----------+-------+-------+------------+---------------"
                   "-+-----------+-----\n");

    print_tune_plan(TUNE_HZ,    MSI_BAND_AM);
    print_tune_plan(1000000U, MSI_BAND_AM);
    print_tune_plan(7100000U, MSI_BAND_AM);
    print_tune_plan(14200000U, MSI_BAND_AM);
    print_tune_plan(28500000U, MSI_BAND_AM);
    print_tune_plan(100000000U, MSI_BAND_VHF);
    console_printf("\n");
}

static void print_tune_plan(uint32_t f, msi_band_t band)
{
    msi_plan_t p;

    msi001_plan(f, band, &p);

    if (!p.ok) {
        console_printf("  %10u | INT out of range - wrong band for this "
                       "frequency\n", f);
        return;
    }

    console_printf("  %10u |   %u   |  %3u  | %10u | %3u %4u %4u |"
                   " %9u | %3d\n",
                   p.f_target, (uint32_t)p.band, p.lodiv, p.fsynth_hz,
                   p.divint, p.frac, p.afc, p.f_actual, p.err_hz);
}

/*
 * TEST 5A: FIND THE BAND FILTER BY MEASUREMENT.
 *
 * The mask is predicted from the schematic - see msi001.h - but one
 * step of that prediction was inferred from label positions in a PDF
 * rather than read off a wire, so it gets checked. All 64 combinations,
 * the carrier amplitude in each, printed.
 *
 * This is cheap and it answers more than the one question. The pattern
 * across 64 rows says whether the bank behaves like two independent
 * 4-way switches in series - exactly four combinations should pass
 * strongly, one per filter, and the rest should be deeply down - which
 * is a structural check on the whole reading of the schematic rather
 * than a lucky hit on one value.
 */
static void test_bpf_sweep(void)
{
    uint32_t best_mask = 0;
    uint64_t best_mag  = 0;
    uint32_t predicted = msi001_bpf_for(GEN_HZ);

    console_printf("Test 5a - which band filter passes %u Hz\n", GEN_HZ);
    console_printf("----------------------------------------------\n");
    console_printf("  Schematic predicts mask 0x%02x for this frequency.\n",
                   predicted);
    console_printf("  All 64 combinations, carrier level in the +%u Hz bin.\n",
                   OFFSET_HZ);
    console_printf("  Expect four strong rows, one per filter path.\n\n");

    for (uint32_t m = 0; m < 64U; m++) {
        iq_result_t r;

        board_watchdog_feed();
        msi001_bpf(m);
        board_delay_ms(3);

        if (!measure(&r)) { continue; }

        if (r.mag_pos > best_mag) { best_mag = r.mag_pos; best_mask = m; }

        /* Only print rows worth reading: everything, but flagged, would
         * be 64 lines of noise. Print the strong ones and a summary. */
        if (r.mag_pos > 0U) {
            int32_t rel = iq_dbfs10(r.mag_pos, CAP_FRAMES);
            console_printf("    mask 0x%02x  BPF 654321 = %u%u%u%u%u%u  "
                           "level %4d.%u dBFS%s\n",
                           m,
                           (m >> 5) & 1U, (m >> 4) & 1U, (m >> 3) & 1U,
                           (m >> 2) & 1U, (m >> 1) & 1U, m & 1U,
                           IQ_DB_INT(rel), IQ_DB_FRAC(rel),
                           (m == predicted) ? "   <- predicted" : "");
        }
    }

    int32_t margin = iq_dbfs10(best_mag, CAP_FRAMES);

    /*
     * COMPARE ONLY THE BITS THAT DO ANYTHING.
     *
     * BPF1 and BPF2 drive the VHF sub-band switch, which is not in the
     * HF signal path at all, so on a short-wave frequency they are
     * don't-care - and the sweep duly shows each strong path appearing
     * four times over, once for each value of those two bits.
     *
     * The first version of this compared the whole six-bit mask and
     * announced that the schematic was wrong because the strongest row
     * was 0x15 rather than the predicted 0x14. The difference is BPF1,
     * a bit with nothing connected to it on this band, and 0.9 dB of
     * run-to-run noise deciding which of four identical paths won.
     * A test that reports a real agreement as a disagreement is worse
     * than no test.
     */
    #define BPF_PATH_BITS   0x3CU   /* BPF3..BPF6: the two HF switches */

    console_printf("\n  strongest: mask 0x%02x at %d.%u dBFS\n",
                   best_mask, IQ_DB_INT(margin), IQ_DB_FRAC(margin));
    console_printf("  path bits 0x%02x against predicted 0x%02x: %s\n",
                   best_mask & BPF_PATH_BITS, predicted & BPF_PATH_BITS,
                   ((best_mask & BPF_PATH_BITS) == (predicted & BPF_PATH_BITS))
                       ? "the schematic was right"
                       : "*** DIFFERENT PATH - re-read the schematic ***");
    console_printf("  (BPF1/BPF2 are the VHF sub-band switch and are\n");
    console_printf("   don't-care here, so each real path appears 4x.)\n");

    /* Carry on with the PREDICTED mask, not the strongest row: they
     * are the same path, and the predicted one has the don't-care bits
     * clear, which is the configuration a real tuning routine would
     * produce. */
    msi001_bpf(predicted);
    console_printf("\n");
}

/*
 * TEST 5B: DOES THE LO ACTUALLY GO WHERE IT IS TOLD?
 *
 * This is the one test that cannot be faked by a receiver that merely
 * works. Sweep the tuned frequency past the carrier in 500 Hz steps and
 * watch the fixed +3000 Hz measurement bin: the response has to peak
 * when, and only when, the tuned frequency is exactly 3000 Hz below the
 * generator. A synthesiser programmed with the wrong reference, the
 * wrong LO divider or a misplaced FRAC field would still produce a
 * signal somewhere - but the peak would not land on the one frequency
 * the arithmetic predicts.
 *
 * The width of the peak is information too: the measurement bin is
 * 192000/8192 = 23.4 Hz wide, so a carrier 500 Hz away should be far
 * down. A broad peak means the LO is not where the dividers say.
 */
static void test_lo_sweep(void)
{
    console_printf("Test 5b - the LO sweep, which settles the synthesiser\n");
    console_printf("----------------------------------------------------\n");
    console_printf("  Tuning past the carrier in 500 Hz steps, watching the\n");
    console_printf("  fixed +%u Hz bin. It must peak at %u Hz and nowhere\n",
                   OFFSET_HZ, TUNE_HZ);
    console_printf("  else - that is what proves INT, FRAC and AFC.\n\n");

    int32_t   best_off = 0;
    uint64_t  best_mag = 0;

    for (int32_t off = -5000; off <= 5000; off += 500) {
        iq_result_t r;
        uint32_t f = (uint32_t)((int32_t)TUNE_HZ + off);

        board_watchdog_feed();
        if (msi001_retune(f) != MSI_OK) {
            console_printf("    %+6d Hz : retune refused\n", off);
            continue;
        }
        if (!measure(&r)) { continue; }

        if (r.mag_pos > best_mag) { best_mag = r.mag_pos; best_off = off; }

        int32_t lvl = iq_dbfs10(r.mag_pos, CAP_FRAMES);
        console_printf("    tuned %u (%+5d) : %4d.%u dBFS%s\n",
                       f, off, IQ_DB_INT(lvl), IQ_DB_FRAC(lvl),
                       (off == 0) ? "   <- the carrier should be here" : "");
    }

    console_printf("\n  peak at %+d Hz from the predicted tuning.\n",
                   best_off);
    console_printf("  %s\n\n", (best_off == 0)
        ? "  The synthesiser is where the arithmetic says it is."
        : "  *** PEAK IN THE WRONG PLACE - the LO maths is wrong ***");

    (void)msi001_retune(TUNE_HZ);
}

/*
 * TEST 5C: THE FRONT END, ONE CONTROL AT A TIME.
 *
 * Each row changes exactly one thing and reports what it did to the
 * carrier. That is the only way to attribute a change to a cause on a
 * signal path with this many switches in it - and it produces the gain
 * figures the AGC will eventually need.
 *
 * The AM port question is settled here rather than guessed: port 1 is
 * 1 kohm and meant for a ferrite rod, port 2 is 75 ohm and meant for an
 * antenna, and this board feeds the tuner from the filter bank, so port
 * 2 ought to win. Ought to is not a measurement.
 */
static void test_front_end(void)
{
    iq_result_t r;
    int32_t     lvl;

    console_printf("Test 5c - the front end, one control at a time\n");
    console_printf("  Levels from the FFT peak, not a fixed bin - see\n");
    console_printf("  peak_dbfs10() for why that distinction is 16 dB.\n");
    console_printf("---------------------------------------------\n");

    struct { const char *what; int port2; int preamp; uint8_t bb_gr; } cases[] = {
        { "AM port 2 (75 ohm), preamp off, 0 dB GR ", 1, 0,  0 },
        { "AM port 1 (1 kohm), preamp off, 0 dB GR ", 0, 0,  0 },
        { "AM port 2, PREAMP ON,            0 dB GR", 1, 1,  0 },
        { "AM port 2, preamp off,          20 dB GR", 1, 0, 20 },
        { "AM port 2, preamp off,          40 dB GR", 1, 0, 40 },
        { "AM port 2, preamp off,          59 dB GR", 1, 0, 59 }
    };

    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        board_watchdog_feed();
        msi001_set_am_port(cases[i].port2);
        msi001_preamp(cases[i].preamp);
        msi001_set_gain(cases[i].bb_gr, 0, 0, 0);
        board_delay_ms(10);

        int32_t off = 0;

        lvl = peak_dbfs10(&off);
        if (!measure(&r)) { continue; }

        console_printf("  %s : %4d.%u dBFS at %+6d Hz\n",
                       cases[i].what,
                       IQ_DB_INT(lvl), IQ_DB_FRAC(lvl), off);
    }

    /* Back to the sensitive configuration. */
    msi001_set_am_port(1);
    msi001_preamp(0);
    msi001_set_gain(0, 0, 0, 0);
    board_delay_ms(10);

    /*
     * IMAGE REJECTION, which is the number iqbal.c exists to improve.
     *
     * A single carrier at +3000 Hz should appear ONLY at +3000 Hz. Any
     * amplitude at -3000 Hz is the mirror image, and it is there
     * because the tuner's I and Q are not exactly equal in amplitude
     * nor exactly 90 degrees apart. The ratio of the two is the raw
     * image rejection of the hardware, before any correction - and it
     * is the figure that decides how much work the software has to do.
     */
    /*
     * MEASURE THE IMAGE WITH THE PREAMP ON, or measure noise instead.
     *
     * Raw image rejection is around 20 dB, so with the carrier at
     * -35 dBFS the image sits near -55 dBFS - which is exactly where
     * the noise floor was in the LO sweep above. The first run of this
     * test duly reported the image as anything between 8 and 31 dB
     * down on consecutive captures, because most of what it was
     * measuring was noise.
     *
     * The preamp adds about 15 dB and lifts the image clear of the
     * floor. The number then stops jittering, which is itself the
     * evidence that it is now an image measurement.
     */
    msi001_preamp(1);
    board_delay_ms(20);

    if (measure(&r)) {
        /*
         * dBFS for all three, not raw magnitudes.
         *
         * These are uint64_t, and console_printf's %u takes a 32-bit
         * argument - so printing them directly showed the low word of
         * each and nothing else, which is how the carrier came out as
         * 1898496640 while the very next line called the same quantity
         * -35 dBFS. Two numbers for one measurement that disagree by
         * 29 dB is the kind of thing that gets explained away.
         */
        int32_t c = iq_dbfs10(r.mag_pos, CAP_FRAMES);
        int32_t i = iq_dbfs10(r.mag_neg, CAP_FRAMES);
        int32_t d = iq_dbfs10(r.mag_dc,  CAP_FRAMES);

        console_printf("\n  raw I/Q balance, uncorrected, preamp on:\n");
        console_printf("    carrier  +%u Hz : %4d.%u dBFS\n",
                       OFFSET_HZ, IQ_DB_INT(c), IQ_DB_FRAC(c));
        console_printf("    image    -%u Hz : %4d.%u dBFS\n",
                       OFFSET_HZ, IQ_DB_INT(i), IQ_DB_FRAC(i));
        console_printf("    DC, LO leakage     : %4d.%u dBFS\n",
                       IQ_DB_INT(d), IQ_DB_FRAC(d));
        console_printf("    image rejection    : %+3d.%u dB   "
                       "<- what iqbal.c has to improve on\n",
                       IQ_DB_INT(r.image_db10), IQ_DB_FRAC(r.image_db10));
        console_printf("    carrier over DC    : %+3d.%u dB\n",
                       IQ_DB_INT(r.dc_db10), IQ_DB_FRAC(r.dc_db10));
    }

    msi001_preamp(0);
    console_printf("\n");
}

int main(void)
{
    /*
     * CONSOLE FIRST, BEFORE THE RAIL AND BEFORE SYSTICK.
     *
     * It used to be the other way round, and that cost a power cycle to
     * diagnose: a run that hung inside board_power_on()'s settling
     * delay produced no output at all, and a radio that reports nothing
     * is indistinguishable from a radio that never started.
     *
     * Nothing stops the console going first. UART7 is on PE7/PE8, which
     * are MCU pins on the always-on CPU rail, and the clock tree is
     * already up - SystemInit() ran before main(), and it raised PC6
     * itself because HSE depends on it (board_rail_on_early). So by the
     * time main() begins there is a 480 MHz clock and a powered
     * peripheral rail, and the only thing board_power_on() still adds
     * is SysTick and the backlight.
     *
     * Which means the first line of output can come before anything
     * that can hang.
     */
    PHASE(1);
    console_init();
    console_printf("\n\n[alive] clock up, console up, rail already on\n");
    PHASE(2);

    board_power_on();
    PHASE(3);

    fault_report_previous();
    PHASE(4);

    banner();
    PHASE(5);

    /*
     * MCLK BEFORE THE CODEC.
     *
     * The control interface is clocked by SCLK and would answer without
     * a master clock, but nothing else in the part would: the converters
     * and all the internal filters run off IMCLK, which is MCLK. Bring
     * SAI1 up first, at the rate the upstream tree wants, so that by the
     * time the codec is configured its clock is already there and
     * stable. The other order works too and makes the first measurement
     * after configuration meaningless, which is worse than failing.
     */
    console_printf("PLL2 plan, before anything is written\n");
    console_printf("-------------------------------------\n");
    console_printf("  Neither 160 nor 192 kHz is exactly synthesisable\n");
    console_printf("  from 22 MHz - see sai1.h. The error below is the\n");
    console_printf("  residual, in parts per BILLION, against X1's own\n");
    console_printf("  1500 ppm measured in step 1.\n\n");
    console_printf("       fs | N  FRACN   /P /MCK |       VCO |      "
                   "MCLK |   ppb\n");
    console_printf("  --------+---------------------+-----------+------"
                   "-----+------\n");
    print_plan(48000U);
    print_plan(96000U);
    print_plan(160000U);
    print_plan(192000U);
    console_printf("\n");

    PHASE(6);
    if (sai1_init() != SAI1_OK) {
        console_printf("sai1_init failed\n");
        for (;;) { board_watchdog_feed(); }
    }

    PHASE(7);
    int rc = sai1_set_fs(192000U);
    if (rc != SAI1_OK) {
        console_printf("sai1_set_fs(192000) failed, rc %d\n", rc);
        for (;;) { board_watchdog_feed(); }
    }

    const sai1_clock_t *c = sai1_clock();
    console_printf("SAI1      : master, MCLK on PE2 = %u Hz, "
                   "BCLK = %u Hz\n", c->mclk_hz, c->fs_hz * 32U);
    console_printf("            PLL2 %u MHz VCO, ker %u Hz, MCKDIV %u\n",
                   c->vco_hz / 1000000U, c->ker_hz, c->mckdiv);

    PHASE(8);
    sai1_start();

    /*
     * Prove MCLK is actually coming out of the pin before blaming the
     * codec for anything. A master transmitter with an empty FIFO still
     * generates its clocks, so this reads back the one piece of state
     * the hardware can clear on its own: SAIEN.
     */
    console_printf("            block A CR1 0x%08x%s\n",
                   SAI1_Block_A->CR1,
                   (SAI1_Block_A->CR1 & (1UL << 16)) ? ""
                       : "  *** SAIEN CLEARED BY HARDWARE ***");

    PHASE(9);
    console_printf("\nNAU8822   : SPI6 on PB3/PB5, CS on PB6, write-only\n");
    nau8822_bus_init();
    PHASE(10);
    nau8822_reset();        /* includes the datasheet's 250 ms wait */
    PHASE(11);
    nau8822_configure();
    PHASE(12);
    console_printf("            configured: I2S 16-bit slave, CLKM=0 "
                   "(PLL bypassed),\n");
    console_printf("            HPF off (DC is the passband centre), "
                   "no +20 dB boost,\n");
    console_printf("            speaker bridge-tied via R43 RSUBBYP\n\n");

    PHASE(13);
    test_rx_timing();
    test_control_bus();

    PHASE(14);
    /*
     * The rate sweep and the tone belong to step 4 and are not repeated
     * every run: they are slow, they are answered, and their results
     * are written down in PORTING.md section 3.2. test_rate_sweep() is
     * still here and still builds, for when a change to the clocking
     * needs re-checking.
     */
    (void)test_rate_sweep;
    (void)tone_fill;
    (void)s_tone;

    /* ============================================================== */
    /* STEP 5                                                         */
    /* ============================================================== */

    console_printf("\n");
    tuner_report_plans();

    PHASE(15);
    msi001_init();
    console_printf("MSi001    : SPI1 on PA5/PA7, EN on PA4, 24-bit words,\n");
    console_printf("            write-only. BPF1..6 on PE10..PE15.\n");

    PHASE(16);
    int trc = msi001_tune(TUNE_HZ, MSI_BAND_AM);
    if (trc != MSI_OK) {
        console_printf("  msi001_tune failed, rc %d\n", trc);
        for (;;) { board_watchdog_feed(); }
    }

    const msi_plan_t *p = msi001_last_plan();
    console_printf("            tuned %u Hz (%d Hz off), LO %u Hz,\n",
                   p->f_actual, p->err_hz, p->flo_hz);
    console_printf("            FSYNTH %u Hz, step %u uHz\n\n",
                   p->fsynth_hz, p->step_mhz_e6);

    console_printf("  Generator expected at %u Hz, which should land in\n",
                   GEN_HZ);
    console_printf("  the +%u Hz bin. Everything below measures that bin.\n\n",
                   OFFSET_HZ);

    PHASE(17);
    /*
     * The band filter sweep, the LO sweep and the front-end table are
     * step 5's work and are answered: BOARD.md section 6 has the
     * measured tables. They are slow - the filter sweep alone is 64
     * captures - and running them before every spectrum would make the
     * interesting part of this image arrive a minute late. All three
     * still build and can be called back when something in the RF path
     * changes.
     */
    (void)test_bpf_sweep;
    (void)test_lo_sweep;

    msi001_bpf(msi001_bpf_for(GEN_HZ));
    fft_init();                 /* test_front_end() now needs the FFT */
    test_front_end();

    /*
     * Settings for the display. 12 dB of gain reduction with the preamp
     * on: the peak measured -4 dBFS at 0 dB reduction, which is four
     * decibels from clipping and no place to leave a receiver whose
     * input is about to be poked at by hand.
     */
    msi001_set_am_port(1);              /* port 2, the 75 ohm one */
    msi001_preamp(1);
    msi001_set_gain(12, 0, 0, 0);
    board_delay_ms(20);

    /* ============================================================== */
    /* STEP 6: THE SPECTRUM                                           */
    /* ============================================================== */

    /*
     * THE PANEL, AGAIN. SPI3 at 30 MHz this time rather than the 15 MHz
     * step 2 measured at: the panel was shown to be clean at twice the
     * clock, and a full frame is 460 800 bytes, so the difference is
     * 245 ms a frame against 123 ms. The bus is the whole cost of this
     * display, and halving it is the only optimisation available.
     */
    PHASE(18);
    spi3_init(SPI3_DIV8);
    ili9488_init(ILI9488_FMT_18BIT);
    board_backlight(1);

    console_printf("Panel     : SPI3 at %u Hz, 18 bpp, 480x320\n", spi3_hz());

    PHASE(19);
    spectrum_init();

    /*
     * Retune so the carrier sits off centre. Everything up to here put
     * it at +3000 Hz, which is the right place for a single-bin
     * measurement and the wrong place for a picture: three kilohertz is
     * sixteen bins, which on a 480-column display is seven pixels from
     * the middle and indistinguishable from the LO leakage.
     */
    (void)msi001_retune(GEN_HZ - SPEC_OFFSET_HZ);

    const msi_plan_t *sp = msi001_last_plan();
    console_printf("Tuned     : %u Hz, so the generator at %u Hz should\n",
                   sp->f_actual, GEN_HZ);
    console_printf("            appear %+d Hz from centre.\n\n",
                   SPEC_OFFSET_HZ);

    console_printf("Spectrum\n");
    console_printf("--------\n");
    console_printf("  %u-point complex FFT of the I/Q pair, Hann windowed,\n",
                   FFT_N);
    console_printf("  %u Hz a bin over a %u Hz span. Centre column is the\n",
                   192000U / FFT_N, 192000U);
    console_printf("  tuned frequency; left is below it, right is above.\n");
    console_printf("  Grid: 20 dB horizontally, 19.2 kHz vertically.\n\n");
    console_printf("  The console reports the peak so the picture can be\n");
    console_printf("  checked without looking at it - the offset below\n");
    console_printf("  must match the %+d Hz asked for above.\n\n",
                   SPEC_OFFSET_HZ);

    PHASE(20);
    for (;;) {
        spectrum_peak_t pk;

        board_watchdog_feed();

        if (capture() == 0U) {
            console_printf("  capture failed\n");
            continue;
        }

        uint64_t t0 = board_cycles();
        fft_load(s_rx, s_fft_re, s_fft_im);
        fft_run(s_fft_re, s_fft_im);
        fft_spectrum_db10(s_fft_re, s_fft_im, s_db10);
        uint32_t fft_us = (uint32_t)(((board_cycles() - t0) * 1000000U)
                                     / clock_sysclk_hz);

        spectrum_peak(s_db10, 192000U, &pk);
        spectrum_draw(s_db10);

        console_printf("  peak %+7d Hz  %4d.%u dBFS   centre %4d.%u dBFS"
                       "   fft %u us  draw %u us\n",
                       pk.offset_hz,
                       pk.db10 / 10, (uint32_t)((pk.db10 < 0 ? -pk.db10
                                                             : pk.db10) % 10),
                       s_db10[FFT_N / 2U] / 10,
                       (uint32_t)((s_db10[FFT_N / 2U] < 0
                                   ? -s_db10[FFT_N / 2U]
                                   : s_db10[FFT_N / 2U]) % 10),
                       fft_us, spectrum_last_us());
    }
}
