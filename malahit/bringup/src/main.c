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
#include "addon.h"
#include "iqmeas.h"
#include "adc.h"
#include "encoder.h"      /* ../../User/encoder.c - upstream */
#include "enc2.h"         /* the same file again, for the small knob */
#include "am.h"
#include "demod.h"        /* upstream's demod_am.c, A/B against am.c */
#include "grab.h"         /* screenshots off the panel, over SWD */
#include "menus.h"        /* step 9: the mode, band and step screens */
#include "idioma.h"       /* ../../User - upstream: tr(), T(), idioma() */
#include "spec_snap.h"    /* ../../User - upstream: a tap lands on the signal */
#include "backlight.h"    /* ../../User - upstream's API, src/backlight.c */
#include "rds.h"          /* ../../User - upstream: the RDS decoder */
#include "rds_pi.h"       /* ../../User - upstream: networks by their PI */
#include "estereo.h"      /* src/ - FM stereo, this port's own */
#include "cw.h"           /* ../../User - upstream: the Morse decoder */
#include "rtty.h"         /* ../../User - upstream: the RTTY decoder */
#include "rtty_auto.h"    /* ../../User */
#include "psk31.h"        /* ../../User - upstream: the PSK31 decoder */
#include "navtex.h"       /* ../../User - upstream: the NAVTEX decoder */
#include "rtty_scope.h"   /* ../../User - its spectrum, for navtex_espectro() */
#include "config.h"       /* ../../User - CONFIG_RTTY_SHIFT_MIN/MAX_HZ */
#include "nb.h"           /* ../../User - upstream: the blanker, cw_report() */
#include "digi.h"         /* src/ - the decoders' screen: scope and text */
#include "bandas_tabla.h" /* GENERATED from ../../User/main.c: the steps */
#include "demod_am.h"     /* ../../User - upstream: DEMOD_MODE_*, for the menus */
#include "nr_ss.h"        /* ../../User - upstream: the Ruido button */
#include "boot.h"         /* the power button, for the console */
#include "agc.h"          /* ../../User/agc.c - upstream; the retune test
                           * computes the gain with ITS constants */
#include <math.h>         /* log10f, for the retune test's decibels */
#include <string.h>       /* strcmp, strcpy: the RDS strip */
#include "fft.h"          /* ../../User/fft.c - upstream */
#include "iqbal.h"        /* ../../User/iqbal.c - upstream */
#include "gfx2.h"         /* ../../User/gfx2.c  - upstream */
#include "gfx2_font.h"
#include "spectrum.h"    /* ../../User/spectrum.c - upstream */
#include "rm68120_ili9488.h"   /* g_lcd_ciclos, and our panel backend */

extern const gfx2_font_t font_ui_14b, font_ui_11, font_num_20;

/*
 * THE UI THEME, WHICH UPSTREAM DEFINES IN ITS main.c.
 *
 * palette.h declares `extern const palette_t *g_pal` and the whole
 * drawing tree reads the chrome colours through it. The definition
 * lives in User/main.c - 39 094 lines of hand-fitted 800x480 layout
 * that cannot come along yet - so it has to be supplied here.
 *
 * k_pal_oscura is the same default upstream picks on line 7484. The
 * theme switcher reassigns this pointer at run time; there is nothing
 * to switch to yet, so it is simply the default and nothing else.
 *
 * This is one of the few places where grafting a file means supplying
 * a definition rather than a driver, and it is worth naming as such:
 * it is a seam left by main.c, not by the hardware.
 */
#include "palette.h"
const palette_t *g_pal = &k_pal_oscura;
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

/*
 * SELFTEST: WHETHER THE BOOT MEASURES THE BOARD OR JUST RUNS THE RADIO.
 *
 * 0, the default: screen and audio 2.5 s after SysTick starts. The
 * radio boots from flash and is used as a radio now, and 37 seconds of
 * bring-up tests on every power-on - most of them wanting a generator
 * on the bench - was the bring-up image's habit, not the radio's need.
 * (Both figures off the "Boot      : screen and audio up" line, on the
 * radio, 20/11/2026.)
 *
 * 1: every test that ran before, in the same order (`make SELFTEST=1`,
 * or `make flash SELFTEST=1`). The makefile rebuilds when the value
 * changes - see FLAGS_STAMP in reglas.mk, and trap 20 for why it has to.
 *
 * The tests are skipped with if (SELFTEST), not #if, so both builds
 * compile every one of them and neither can rot.
 *
 * WHAT THIS COST TO MAKE SAFE is trap 17 again, three times: three
 * settings the radio ran on were OUTPUTS of tests. The receive clock
 * edge came from test_rx_timing() (now sai1.c's default), the add-on
 * relay from test_addon_relay()'s measurement, and the add-on filter
 * from whichever test touched it last. main() now sets all of them
 * itself, after the tests, so what the radio runs on cannot depend on
 * which tests ran.
 */
#ifndef SELFTEST
#define SELFTEST    0
#endif

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
 * UPSTREAM'S TRANSFORM WANTS I AND Q IN SEPARATE ARRAYS.
 *
 * fft_compute_db_iq() takes two int16 pointers, not one interleaved
 * block - which is what sdr_rx.c hands it upstream, where the DMA
 * de-interleaves on the way in. The SAI here delivers them interleaved
 * (slot 0 Q, slot 1 I), so they are split on the way to the transform.
 *
 * That split is not waste: it is the shape the whole upstream DSP tree
 * expects, so doing it here once is what lets demod_am.c and the rest
 * be used unmodified later.
 */
#define PAN_BINS    512U        /* must be a power of two, <= FFT_SIZE */

static int16_t s_i[PAN_BINS];
static int16_t s_q[PAN_BINS];
static float   s_db[PAN_BINS];
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
/*
 * HOW MUCH OF THE CONVERTER THE LAST CAPTURE USED, and whether it hit
 * the end of it.
 *
 * This belongs to capture() and not to its callers, for the same reason
 * the frame count does: every caller would otherwise have to remember,
 * and the one that forgot would draw a picture of a clipped signal and
 * call it a spectrum. Which is exactly what happened - the display loop
 * ran for an afternoon at 100 % of the ADC, turning a clean carrier
 * into a forest of intermodulation products, and the only symptom
 * available was that the trace "looked like porridge".
 *
 * A clipped capture is not a loud capture. It is a DIFFERENT SIGNAL:
 * the products are at frequencies nothing on the air is transmitting,
 * they move when the input moves, and no amount of staring at the
 * spectrum distinguishes them from reality. One percentage in the
 * per-frame line is the whole fix.
 */
static uint32_t s_cap_used_pct;     /* peak |sample| as % of full scale */
static uint32_t s_cap_clipped;      /* samples within 0.02 dB of a rail */
static uint32_t s_cap_us;           /* what the last capture cost */

static uint32_t capture_stats(uint32_t got);

static uint32_t capture(void)
{
    uint64_t t0  = board_cycles();
    uint32_t got = sai1_stream(NULL, 0U, s_rx, CAP_FRAMES);

    s_cap_us = (uint32_t)(((board_cycles() - t0) * 1000000U) / clock_sysclk_hz);

    if (got < CAP_FRAMES) {
        console_printf("    *** capture got %u of %u frames; "
                       "SR_A 0x%08x SR_B 0x%08x ***\n",
                       got, CAP_FRAMES, sai1_sr_a(), sai1_sr_b());
    }

    return capture_stats(got);
}

/*
 * THE SAME CAPTURE OUT OF THE DMA RING, AND IT IS NOT THE SAME
 * MEASUREMENT.
 *
 * capture() above stops the blocks, restarts them and polls a fresh
 * 8192 frames, so every sample is from after the call. That is what a
 * test wants - "set this register, now measure" - and it costs 42.7 ms
 * of waiting plus a transport that cannot be interrupted.
 *
 * This one takes the half the stream has just finished. It costs the
 * memcpy and nothing else, the samples are between 0 and 42.7 ms old,
 * and the receiver never stops - so there is no gap between one frame's
 * block and the next, which is what a demodulator will need.
 *
 * Both exist because both are right, for different questions. The tests
 * keep the strict one; the display loop uses this.
 */
static uint32_t capture_live(void)
{
    uint64_t t0  = board_cycles();
    uint32_t got = sai1_rx_half(s_rx, CAP_FRAMES);

    s_cap_us = (uint32_t)(((board_cycles() - t0) * 1000000U) / clock_sysclk_hz);

    if (got == 0U) {
        console_printf("    *** no receive half; dma %s, streams %s, "
                       "err %u, torn %u, SR_B 0x%08x ***\n",
                       sai1_dma_running() ? "on" : "OFF",
                       sai1_dma_streams_live() ? "live" : "STOPPED",
                       sai1_dma_errors(), sai1_rx_torn(),
                       SAI1_Block_B->SR);
    }

    return capture_stats(got);
}

static uint32_t capture_stats(uint32_t got)
{
    /*
     * Both slots together: I and Q share a converter range and either
     * one clipping ruins the pair. 32700 rather than 32767 because a
     * real analogue signal does not land exactly on the last code, and
     * a count that only triggers on 32767 reports a clean capture for a
     * waveform that has been flat-topped for a millisecond.
     */
    int32_t  pk   = 0;
    uint32_t clip = 0;

    for (uint32_t k = 0; k < got * 2U; k++) {
        int32_t v = s_rx[k];

        if (v < 0) { v = -v; }
        if (v > pk) { pk = v; }
        if (v >= 32700) { clip++; }
    }

    s_cap_used_pct = (uint32_t)((pk * 100) / 32768);
    s_cap_clipped  = clip;

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
/*
 * Declared here because the tests come long before the drawing code and
 * several of them ask for a hand. Defined next to ui_paint_all(), which
 * is where everything else that touches the panel lives; see the comment
 * there for why a bench test that needs a person belongs on the screen
 * that person is holding.
 */
static void ui_prompt(const char *title, const char *a, const char *b,
                      const char *c);

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
    nau8822_set_adc_swap(NAU_ADCPHS_DEFAULT);
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
    nau8822_set_adc_swap(NAU_ADCPHS_DEFAULT);

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
/* STEP 7/C: UPSTREAM'S GRAPHICS LAYER ON THIS PANEL                  */
/* ================================================================== */

/*
 * Everything drawn below is drawn by ../../User/gfx2.c - the DEEPSDR's
 * own band compositor, 969 lines, compiled for this board without an
 * edit. The only thing this port supplies is three functions over SPI;
 * see compat/rm68120_ili9488.h.
 *
 * gfx2 works by bands: it reserves GFX2_BAND_H rows, calls this
 * callback once per band with a surface covering that strip, and
 * pushes each band to the panel before composing the next. The draw
 * code therefore runs several times per frame and must be a pure
 * function of the surface it is handed - which is exactly how the
 * upstream UI is written, and the reason its screens can be rendered
 * on a PC by deepsdr-ui.
 */
static uint32_t s_gfx_frame;

static void gfx_demo(gfx2_surf_t *s, void *ctx)
{
    (void)ctx;
    char nbuf[16];

    gfx2_fill(s, 0, 0, GFX2_W, GFX2_H, gfx2_rgb(0x0A1020));

    /* A header strip with a gradient, the way the real UI draws one. */
    gfx2_vgrad(s, 0, 0, GFX2_W, 34, gfx2_rgb(0x1E3A5F), gfx2_rgb(0x0C1A2E));
    gfx2_hline(s, 0, 34, GFX2_W, gfx2_rgb(0x3C6FA0));

    gfx2_text(s, 10, 8, "Malahit DSP-1", &font_ui_14b, gfx2_rgb(0xE8F0FF));
    gfx2_text(s, 330, 11, "gfx2 upstream", &font_ui_11, gfx2_rgb(0x8FB6E0));

    /* Rounded panels, to exercise the span/coverage code rather than
     * just flat fills - those are where a band-boundary bug would show. */
    gfx2_rrect(s, 10, 48, 220, 96, 10, gfx2_rgb(0x16233A));
    gfx2_rrect_outline(s, 10, 48, 220, 96, 10, 1, gfx2_rgb(0x3C6FA0));
    gfx2_text(s, 24, 58, "10.000 000", &font_num_20, gfx2_rgb(0x7CFF9E));
    gfx2_text(s, 24, 92, "MHz   AM   BPF 0-12", &font_ui_11,
              gfx2_rgb(0x9FC4E8));
    gfx2_text(s, 24, 112, "iqbal on, mirror 34 dB", &font_ui_11,
              gfx2_rgb(0x9FC4E8));

    gfx2_rrect(s, 246, 48, 224, 96, 10, gfx2_rgb(0x16233A));
    gfx2_rrect_outline(s, 246, 48, 224, 96, 10, 1, gfx2_rgb(0x3C6FA0));
    gfx2_text(s, 260, 58, "upstream, unmodified:", &font_ui_11,
              gfx2_rgb(0xE8F0FF));
    gfx2_text(s, 260, 78, "gfx2 fft iqbal iqancho", &font_ui_11,
              gfx2_rgb(0x9FC4E8));
    gfx2_text(s, 260, 96, "agc audiofil + CMSIS-DSP", &font_ui_11,
              gfx2_rgb(0x9FC4E8));
    gfx2_text(s, 260, 118, "3 functions are ours", &font_ui_11,
              gfx2_rgb(0xFFC87C));

    /* Frame counter, so a frozen screen is obvious. */
    {
        uint32_t v = s_gfx_frame;
        int      k = 0;
        nbuf[k++] = 'f'; nbuf[k++] = ' ';
        if (v >= 10U) { nbuf[k++] = (char)('0' + (v / 10U) % 10U); }
        nbuf[k++] = (char)('0' + v % 10U);
        nbuf[k]   = 0;
        gfx2_text(s, 420, 11, nbuf, &font_ui_11, gfx2_rgb(0xFFC87C));
    }

    /* A ruler along the bottom, so a band seam would be obvious as a
     * step in a line that should be straight. */
    for (int16_t x = 0; x < GFX2_W; x += 20) {
        int16_t t = ((x % 100) == 0) ? 12 : 6;
        gfx2_vline(s, x, (int16_t)(GFX2_H - t), t, gfx2_rgb(0x4E7FB0));
    }
    gfx2_hline(s, 0, (int16_t)(GFX2_H - 13), GFX2_W, gfx2_rgb(0x2A4668));
    gfx2_line(s, 0, 160, GFX2_W - 1, GFX2_H - 20, gfx2_rgb(0xFF6060));
}

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
/* Split the interleaved capture into the two arrays upstream wants. */
static void deinterleave(uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        s_i[k] = s_rx[k * 2U];          /* slot 0 is I - ADCPHS is set */
        s_q[k] = s_rx[k * 2U + 1U];     /* slot 1 is Q */
    }
}

static int32_t peak_db10(int32_t *offset_hz)
{
    iq_peak_t pk;

    if (capture() == 0U) { return -1800; }

    deinterleave(PAN_BINS);
    fft_compute_db_iq(s_i, s_q, s_db);
    iq_peak(s_db, PAN_BINS, 192000U, &pk);

    if (offset_hz != NULL) { *offset_hz = pk.offset_hz; }

    /* Upstream's dB is uncalibrated - see panbringup.h - so this is a
     * relative figure in tenths, good for comparing rows of a table
     * against each other and for nothing else. */
    return (int32_t)(pk.db * 10.0f);
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
#define PEAK_IS_CARRIER(off_hz) \
    (((off_hz) > ((int32_t)OFFSET_HZ - 1200)) && \
     ((off_hz) < ((int32_t)OFFSET_HZ + 1200)))

static int32_t s_last_off_hz;       /* where the last reported peak was */

/* Defined with the add-on tests further down: one capture, one line,
 * and the peak in tenths of a dB handed back. */
static int32_t addon_report_tail(void);

/* Defined with the constant it replaced, further down. */
static uint8_t rf_autorange(void);

static void test_front_end(void)
{
    iq_result_t r;

    /* peak_db10() is retired rather than deleted: this table now reports
     * through addon_report_tail(), which gives tenths of a dB and the
     * ADC column with it. The old one stays because the comment above
     * explaining why a fixed bin under-reads by 16 dB points at it. */
    (void)peak_db10;

    console_printf("Test 5c - the front end, one control at a time\n");
    console_printf("=============================================\n");
    console_printf("  RETAKEN on a KNOWN path. Every earlier version of\n");
    console_printf("  this table went through the add-on board in an\n");
    console_printf("  inherited state - 30 dB of attenuation the firmware\n");
    console_printf("  did not know about, and an unrecorded filter. Those\n");
    console_printf("  numbers are void; these replace them.\n\n");
    console_printf("  Generator %u Hz, AM 70%% at 1000 Hz, 100 mV through\n",
                   GEN_HZ);
    console_printf("  a 40 dB pad: -47 dBm at the antenna. WRITE THE\n");
    console_printf("  LEVEL DOWN - the old table said \"<= -40 dBm\",\n");
    console_printf("  which is not a level, and that is the single\n");
    console_printf("  reason it cannot be compared with anything.\n\n");
    console_printf("  Add-on board: 50 ohm input, %s\n",
                   addon_filter_name(addon_filter()));
    console_printf("  Levels from the FFT peak, not a fixed bin - see\n");
    console_printf("  peak_db10() for why that distinction is 16 dB.\n");
    console_printf("  Relative dB (upstream fft.c is uncalibrated); the\n");
    console_printf("  ADC column is the absolute one.\n\n");

    /*
     * SET THE LEVEL WITH THE ADD-ON ATTENUATOR, NOT WITH THE THING
     * BEING MEASURED.
     *
     * The hottest row of this table is the preamp at zero gain
     * reduction, and at -47 dBm that clips - which would compress the
     * one row everything else is compared against and quietly shrink
     * every figure derived from it. The old table had exactly that
     * problem and no way to see it.
     *
     * The add-on board's attenuator is ahead of everything the table
     * measures, so using it to find a working point changes all rows
     * by the same amount and none of the comparisons. Found by
     * looking, like rf_autorange(), and reported so the table says
     * what it was taken at.
     */
    /*
     * ZERO GAIN REDUCTION CANNOT BE THE REFERENCE ROW, at any input
     * level, and the first retake of this table learned it the
     * expensive way.
     *
     * Step 8b already established why: at 0 dB GR the baseband has
     * 77 dB of gain and fills the converter with its own amplified
     * noise whatever the signal does. Attenuating the input cannot
     * reach that operating point - the first retake wound the add-on
     * attenuator to its 31.5 dB limit and the row still clipped, and
     * the two rows taken there reported the loudest NOISE bin to a
     * tenth of a decibel. Every figure derived from them was wrong,
     * and none of it looked wrong.
     *
     * (The old table had the same flaw and it did not show, because
     * 30 dB of add-on attenuation nobody knew about was holding it in
     * the linear region by accident.)
     *
     * So the reference is chosen by rf_autorange() on the HOTTEST
     * configuration - preamp on - and every row is referred to that.
     */
    addon_set_att_halfdb(0U);
    msi001_set_am_port(1);
    msi001_preamp(1);
    board_delay_ms(20);

    uint8_t gr0 = rf_autorange();

    console_printf("  Add-on attenuator 0 dB; reference gain reduction\n"
                   "  %u dB, chosen on the preamp-on row so that the\n"
                   "  hottest case is still linear. ADC %u%%.\n\n",
                   (uint32_t)gr0, s_cap_used_pct);

    struct { const char *what; int port2; int preamp; } cases[] = {
        { "AM port 2 (75 ohm), PREAMP ON       ", 1, 1 },
        { "AM port 1 (1 kohm), PREAMP ON       ", 0, 1 },
        { "AM port 2, preamp OFF               ", 1, 0 }
    };

    int32_t lvl10[3];
    int     ok[3];

    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        board_watchdog_feed();
        msi001_set_am_port(cases[i].port2);
        msi001_preamp(cases[i].preamp);
        msi001_set_gain(gr0, 0, 0, 0);
        board_delay_ms(20);

        console_printf("  %s | ", cases[i].what);
        lvl10[i] = addon_report_tail();
        ok[i]    = PEAK_IS_CARRIER(s_last_off_hz) && (s_cap_clipped == 0U);
    }

    /*
     * THE DERIVED FIGURES, computed here rather than left to whoever
     * reads the table. Each is a difference between two rows, so the
     * uncalibrated scale and the pad above both cancel.
     */
    /*
     * A DERIVED FIGURE IS ONLY PRINTED IF BOTH ITS ROWS WERE REAL.
     * Subtracting two numbers of which one is the loudest noise bin
     * produces a figure that is wrong without looking wrong, and that
     * is what made the first retake of this table worthless.
     */
    console_printf("\n");
    if (ok[0] && ok[1]) {
        int32_t d = lvl10[0] - lvl10[1];
        console_printf("  port 2 over port 1      : %+3d.%u dB\n",
                       d / 10, (uint32_t)((d < 0 ? -d : d) % 10));
    } else {
        console_printf("  port 2 over port 1      : not measurable - one\n"
                       "                            of the two rows was "
                       "noise\n");
    }

    if (ok[0] && ok[2]) {
        int32_t d = lvl10[0] - lvl10[2];
        console_printf("  the preamp is worth     : %+3d.%u dB\n",
                       d / 10, (uint32_t)((d < 0 ? -d : d) % 10));
    } else {
        console_printf("  the preamp is worth     : not measurable\n");
    }

    /*
     * GAIN-REDUCTION TRACKING NEEDS ITS OWN WORKING POINT, because the
     * one above has no room above it.
     *
     * The first attempt swept upward from the autoranged reference,
     * which at this input level is 48 dB - eleven decibels short of
     * the register's 59 dB limit. Two of the three rows clamped to 59,
     * read identically, and were reported as 9 and 19 dB of tracking
     * error. The receiver was fine; the sweep had run off the end of
     * the control and said so in the wrong words.
     *
     * So: drop the signal with the ADD-ON attenuator and turn the
     * preamp off, which buys about 46 dB, and start the sweep low
     * enough that forty decibels of it fit. Setting the level with one
     * control in order to measure another is the whole reason the
     * add-on attenuator is worth having.
     */
    {
        static const uint8_t k_gr[5] = { 12U, 22U, 32U, 42U, 52U };
        int32_t  g10[5];
        int      gok[5];
        uint32_t k;

        console_printf("\n  Gain reduction over its full range, at its "
                       "own working\n  point: preamp OFF and the add-on "
                       "attenuator at 31.5 dB,\n  so that fifty decibels "
                       "of reduction fit under the limit.\n\n");

        msi001_set_am_port(1);
        msi001_preamp(0);
        addon_set_att_halfdb(63U);

        for (k = 0; k < 5U; k++) {
            board_watchdog_feed();
            msi001_set_gain(k_gr[k], 0, 0, 0);
            board_delay_ms(20);

            console_printf("    %2u dB GR | ", (uint32_t)k_gr[k]);
            g10[k] = addon_report_tail();
            gok[k] = PEAK_IS_CARRIER(s_last_off_hz) && (s_cap_clipped == 0U);
        }

        /*
         * STEP BY STEP, NOT CUMULATIVELY AGAINST THE FIRST ROW.
         *
         * Measured against a single reference, a compressed bottom row
         * poisons every figure above it: the first version of this
         * reported a flat "-9 dB error" at 20, 30 and 40 dB and looked
         * like a gain control that was broken everywhere. It is not -
         * a constant offset across every row is the signature of ONE
         * bad row, which here is the reference itself.
         *
         * Consecutive steps show it for what it is. Each should be
         * exactly ten decibels; where it is not, that is where the
         * front end stops being linear, and THAT is the number worth
         * having.
         */
        console_printf("\n    step by step - each should be 10.0 dB:\n");
        for (k = 1; k < 5U; k++) {
            int32_t got = g10[k - 1U] - g10[k];
            int32_t err = got - 100;

            if (!gok[k - 1U] || !gok[k]) {
                console_printf("      %2u -> %2u dB : not the carrier\n",
                               (uint32_t)k_gr[k - 1U], (uint32_t)k_gr[k]);
                continue;
            }
            console_printf("      %2u -> %2u dB GR : %2d.%u dB, "
                           "error %+3d.%u dB%s\n",
                           (uint32_t)k_gr[k - 1U], (uint32_t)k_gr[k],
                           got / 10,
                           (uint32_t)((got < 0 ? -got : got) % 10),
                           err / 10,
                           (uint32_t)((err < 0 ? -err : err) % 10),
                           (err < -15 || err > 15)
                               ? "   <- compressing" : "");
        }

        addon_set_att_halfdb(0U);
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

/* ================================================================== */
/* STEP 8: THE MIXER DC OFFSET                                        */
/* ================================================================== */

/*
 * THE WORST NUMBER ON THIS BOARD, and the one measurement that has to
 * be fixed before any other is worth taking again.
 *
 * Step 5 measured the standing DC offset at -7.7 dBFS with the preamp
 * on: 15 dB ABOVE the wanted carrier and 8 dB from clipping the
 * converter. Most of the ADC's range is therefore spent digitising the
 * receiver's own error. That is a sensitivity loss, not a cosmetic one,
 * and every level measured afterwards would have to be retaken.
 *
 * WHY IT IS WORTH ATTACKING RATHER THAN FILTERING.
 *
 * A software DC block would clean up the picture and recover nothing:
 * the offset is already inside the converter's range by the time any
 * firmware sees it, so the headroom it ate is gone. The part has its
 * own analogue trim for exactly this, and the datasheet says what it
 * should achieve - 50 mV of residual offset in continuous mode, 5 mV
 * with the speed-up bit (table 13, register 1 D[17:14] = 0101 and
 * 1101). Against the 583 mV measured below, that is a factor of twelve
 * at worst and a hundred at best, and it says the trim is not working
 * rather than that the part is poor.
 *
 * WHAT THIS TEST HAS TO SEPARATE, in order, because each answer makes
 * the next question meaningful:
 *
 *   A. Is it even the tuner? The codec's ADC high-pass filter is off on
 *      purpose - DC is the tuned frequency on a zero-IF receiver - so a
 *      codec-side offset would land in exactly the same place and look
 *      exactly the same. Two mutes tell them apart.
 *
 *   B. Does the calibration do anything? Static mode is "no DC
 *      tracking": it holds whatever the last calibration left. If
 *      nothing ever calibrated, the held value is whatever the part
 *      powered up with.
 *
 *   C. Does it depend on the gain? The datasheet says it does, and the
 *      driver used to change the gain without recalibrating.
 *
 *   D. Does the hold circuit leak? That is the only cost of holding,
 *      and the only thing that would justify a periodic refresh.
 *
 * THE UNITS ARE MILLIVOLTS, not just dBFS, and that is the point of
 * this test rather than a nicety. -7.7 dBFS cannot be compared with
 * anything in a datasheet; 583 mV can be compared directly with the
 * 50 mV Mirics specifies, and that comparison is what turns "this
 * number looks bad" into "this number is twelve times what the part
 * promises, so something is switched off".
 *
 * The conversion is exact and comes from one line of the NAU8822
 * datasheet: full scale is 1.0 Vrms at 0 dB PGA gain with VDDA = 3.3 V,
 * so 32768 counts is 1.4142 V peak and one count is 43.16 uV. R47/R48
 * leave the +20 dB boost off and R45/R46 sit at 0 dB, so there is
 * nothing else in the chain to account for - see nau8822_configure().
 */

/* Tenths of a millivolt from an ADC count. 1.4142 V / 32768 counts is
 * 43.157 uV a count, so tenths of a mV is count x 0.43157 - carried as
 * x4316/10000 to stay in integers, which is exact to four figures and
 * costs nothing on a part with a 32-bit multiplier. */
static int32_t dc_mv10(int32_t counts)
{
    return (int32_t)(((int64_t)counts * 4316) / 10000);
}

/* One row: both channels in counts and millivolts, the combined offset
 * as the receiver sees it, and how much of the converter is left. */
static void dc_row(const char *what)
{
    chan_stats_t si, sq;
    iq_result_t  r;

    if (!measure(&r)) { return; }

    stats(s_rx, CAP_FRAMES, 0U, &si);   /* slot 0 is I - ADCPHS is set */
    stats(s_rx, CAP_FRAMES, 1U, &sq);

    int32_t dc_dbfs = iq_dbfs10(r.mag_dc, CAP_FRAMES);

    /*
     * THE HEADROOM IS THE WHOLE POINT, so it is a column and not a
     * remark. max(|min|, |max|) against 32768 is what is actually
     * being spent, offset and signal together; the offset's own share
     * of it is the mV columns to the left.
     */
    int32_t pi = (si.max > -si.min) ? si.max : -(int32_t)si.min;
    int32_t pq = (sq.max > -sq.min) ? sq.max : -(int32_t)sq.min;
    int32_t pk = (pi > pq) ? pi : pq;

    int32_t mvi = dc_mv10(si.mean);
    int32_t mvq = dc_mv10(sq.mean);

    console_printf("  %s | I %+6d %+5d.%u | Q %+6d %+5d.%u | "
                   "%4d.%u dBFS | ac %5u | used %2u%%\n",
                   what,
                   si.mean, mvi / 10, (uint32_t)((mvi < 0 ? -mvi : mvi) % 10),
                   sq.mean, mvq / 10, (uint32_t)((mvq < 0 ? -mvq : mvq) % 10),
                   IQ_DB_INT(dc_dbfs), IQ_DB_FRAC(dc_dbfs),
                   (si.rms > sq.rms) ? si.rms : sq.rms,
                   (uint32_t)((pk * 100) / 32768));
}

static void dc_header(void)
{
    console_printf("                                  | mean mV       "
                   "| mean mV       |   combined |  noise | ADC\n");
}

static void test_dc_offset(void)
{
    static const struct { const char *name; uint32_t mode; int speedup; }
    k_modes[] = {
        { "static, no tracking at all    ", MSI_DCCAL_STATIC,     0 },
        { "1 shot, short track then hold ", MSI_DCCAL_ONESHOT,    0 },
        { "periodic1                     ", MSI_DCCAL_PERIODIC1,  0 },
        { "continuous                    ", MSI_DCCAL_CONTINUOUS, 0 },
        { "continuous + SPEEDUP          ", MSI_DCCAL_CONTINUOUS, 1 }
    };
    static const uint8_t k_grs[] = { 0U, 12U, 20U, 40U };

    /* Written out rather than assembled, because console_printf() has no
     * width modifier for a string and a column that drifts is a column
     * nobody reads. Two rows per gain: held, then recalibrated. */
    static const char *const k_gr_labels[] = {
        " 0 dB GR, held                ", " 0 dB GR, recalibrated        ",
        "12 dB GR, held                ", "12 dB GR, recalibrated        ",
        "20 dB GR, held                ", "20 dB GR, recalibrated        ",
        "40 dB GR, held                ", "40 dB GR, recalibrated        "
    };

    uint32_t i;

    console_printf("\n");
    console_printf("Test 8 - the mixer DC offset, in millivolts\n");
    console_printf("==========================================\n");
    console_printf("  Datasheet table 13, O/P residual DC offset:\n");
    console_printf("      50 mV   continuous calibration\n");
    console_printf("       5 mV   continuous + the speed-up bit\n");
    console_printf("  One ADC count is 43.16 uV (NAU8822 full scale is\n");
    console_printf("  1.0 Vrms at 0 dB PGA, VDDA 3.3 V), so anything past\n");
    console_printf("  about 1160 counts is already outside the loosest\n");
    console_printf("  figure Mirics gives.\n\n");

    /* Nothing below depends on the generator: a standing offset is the
     * receiver's own error and is cleanest measured with the input
     * quiet. The carrier only matters for the ratio in step 5. */
    msi001_set_am_port(1);
    msi001_preamp(0);
    msi001_set_dc_auto(0);          /* measure the uncalibrated case first */
    msi001_set_gain(0, 0, 0, 0);
    /* DCCAL = static, explicitly: this driver's default is periodic1
     * now, and the first row of this test has to be the one that made
     * the change necessary, not the one that followed from it. */
    msi001_set_dccal(MSI_DCCAL_STATIC, 0);
    board_delay_ms(20);

    /* -------------------------------------------------------------- */
    console_printf("  A. WHOSE OFFSET IS IT? The codec's ADC high-pass is\n");
    console_printf("     off on purpose, so a codec-side offset would land\n");
    console_printf("     in the same bin and look identical. The two mutes\n");
    console_printf("     silence different halves of the chain.\n\n");
    dc_header();
    dc_row("DCCAL static, the old default ");

    nau8822_set_adc_digital_mute(1);
    board_delay_ms(20);
    dc_row("ADC digital mute (expect ~0)  ");
    nau8822_set_adc_digital_mute(0);
    board_delay_ms(20);

    nau8822_set_pga_mute(1);
    board_delay_ms(20);
    dc_row("input PGA muted (codec alone) ");
    nau8822_set_pga_mute(0);
    board_delay_ms(20);

    msi001_preamp(1);
    board_delay_ms(20);
    dc_row("preamp ON (step 5's -7.7 dBFS)");
    msi001_preamp(0);
    board_delay_ms(20);

    console_printf("\n     Read it this way: the digital mute near zero\n");
    console_printf("     says the MCU really is reading the converter, so\n");
    console_printf("     the framing is right. A SMALL figure with the PGA\n");
    console_printf("     muted puts the offset in front of the codec, i.e.\n");
    console_printf("     in the tuner, which is the only case where any of\n");
    console_printf("     the calibration below can help.\n\n");
    console_printf("     AND THE PGA-MUTED ROW IS THE BENCH'S PICKUP\n");
    console_printf("     DETECTOR, which is not what it was written for.\n");
    console_printf("     With the PGA muted the tuner is disconnected from\n");
    console_printf("     the converter, so ANY signal left is arriving\n");
    console_printf("     after the input stage - radiated or conducted\n");
    console_printf("     straight into an unshielded board, bypassing the\n");
    console_printf("     antenna, the attenuators, the filters and the\n");
    console_printf("     mixer. It reads 'ac 11, used 0%%' on a clean\n");
    console_printf("     bench. It read 'ac 18438, used 99%%' with the\n");
    console_printf("     generator at 500 mV, and no amount of gain\n");
    console_printf("     reduction touched it - because gain reduction is\n");
    console_printf("     upstream of where it was getting in.\n\n");

    /* -------------------------------------------------------------- */
    console_printf("  B. DOES THE CALIBRATION DO ANYTHING? Static mode is\n");
    console_printf("     'no DC tracking' - it holds what the last\n");
    console_printf("     calibration left, and if nothing calibrated, that\n");
    console_printf("     is whatever the part powered up with.\n\n");
    dc_header();

    for (i = 0; i < sizeof k_modes / sizeof k_modes[0]; i++) {
        board_watchdog_feed();
        msi001_set_dccal(k_modes[i].mode, k_modes[i].speedup);
        /* Long enough for any of the modes to have acted: the tracking
         * times in table 20 are microseconds and the slowest refresh
         * period with the reset timers is 15 ms. */
        board_delay_ms(50);
        dc_row(k_modes[i].name);
    }

    /* The sequence this driver actually uses, which is none of the five
     * above: converge hard with the speed-up bit, then hold. It should
     * read like the speed-up row and be quiet like the static one. */
    msi001_set_dccal(MSI_DCCAL_STATIC, 0);
    msi001_dc_recal(2U);
    dc_row("SPEEDUP burst, then hold      ");

    console_printf("\n     The noise column matters here. Continuous and\n");
    console_printf("     speed-up both remove energy around DC - that is\n");
    console_printf("     what section 5.4 warns about - so a mode that\n");
    console_printf("     wins on offset and loses on noise has taken the\n");
    console_printf("     signal with it. The burst is the one that should\n");
    console_printf("     win on both.\n\n");

    /* -------------------------------------------------------------- */
    console_printf("  C. DOES IT FOLLOW THE GAIN? Section 5.4 says it\n");
    console_printf("     does, and the datasheet's own power-up flow\n");
    console_printf("     writes register 2 - the calibration trigger -\n");
    console_printf("     BEFORE register 1, which carries the gain. So\n");
    console_printf("     following it literally calibrates for a gain the\n");
    console_printf("     part no longer has.\n\n");
    console_printf("     Each pair: the same gain without a recalibration,\n");
    console_printf("     then with one.\n\n");
    dc_header();

    for (i = 0; i < sizeof k_grs / sizeof k_grs[0]; i++) {
        board_watchdog_feed();

        msi001_set_dc_auto(0);      /* the gain moves, the trim does not */
        msi001_set_gain(k_grs[i], 0, 0, 0);
        board_delay_ms(20);
        dc_row(k_gr_labels[i * 2U]);

        msi001_dc_recal(2U);
        dc_row(k_gr_labels[i * 2U + 1U]);
    }

    /* -------------------------------------------------------------- */
    console_printf("\n  D. DOES THE HOLD LEAK? Drift is the only cost of\n");
    console_printf("     holding, and measured drift is the only thing\n");
    console_printf("     that would justify a periodic refresh and the\n");
    console_printf("     hole it opens at the tuned frequency.\n\n");
    console_printf("     AT 20 dB OF GAIN REDUCTION, not at full gain:\n");
    console_printf("     section C shows the hold railing the converter\n");
    console_printf("     at 0 dB GR, and a question about leakage cannot\n");
    console_printf("     be asked of a reading that is already pinned.\n\n");

    msi001_set_dc_auto(0);
    msi001_set_gain(20, 0, 0, 0);
    msi001_set_dccal(MSI_DCCAL_STATIC, 0);
    msi001_dc_recal(2U);
    dc_header();
    dc_row("t = 0 s, just calibrated      ");

    {
        static const uint32_t k_waits_ms[] = { 1000U, 4000U, 15000U };
        static const char *const k_at[] = {
            "t = 1 s                       ",
            "t = 5 s                       ",
            "t = 20 s                      "
        };

        for (i = 0; i < sizeof k_waits_ms / sizeof k_waits_ms[0]; i++) {
            uint32_t until = board_millis() + k_waits_ms[i];

            while ((int32_t)(board_millis() - until) < 0) {
                board_watchdog_feed();
            }
            dc_row(k_at[i]);
        }
    }

    /* -------------------------------------------------------------- */
    /* And finally the mode the driver now defaults to, at the gain the
     * screen is about to run at, so the last row of this test is the
     * one that describes the rest of the run. */
    console_printf("\n  E. WHAT THE DRIVER NOW DOES, at the gain the\n");
    console_printf("     screen runs at.\n\n");

    msi001_set_dc_auto(1);
    msi001_set_dccal(MSI_DCCAL_PERIODIC1, 0);
    msi001_set_gain(12, 0, 0, 0);
    msi001_preamp(1);
    board_delay_ms(20);

    {
        uint32_t period_ns = 0, track_ns = 0;

        msi001_dc_timing_ns(&period_ns, &track_ns);
        console_printf("     periodic1, register 6 at its reset timers:\n");
        console_printf("     tracking %u.%u us every %u us - one part in\n",
                       track_ns / 1000U, (track_ns % 1000U) / 100U,
                       period_ns / 1000U);
        console_printf("     %u, repeating at %u Hz.\n\n",
                       track_ns ? period_ns / track_ns : 0U,
                       period_ns ? 1000000000U / period_ns : 0U);
    }

    dc_header();
    dc_row("periodic1, 12 dB GR, preamp on");

    msi001_preamp(0);
    msi001_set_gain(0, 0, 0, 0);
    console_printf("\n");
}

/* ================================================================== */
/* IS THE MESS OURS OR THE GENERATOR'S?                               */
/* ================================================================== */

/*
 * A spectrum that "looks like porridge" has two completely different
 * causes and they need opposite fixes, so guessing is expensive.
 *
 *   OURS. The converter is clipping. A clipped carrier is not a loud
 *   carrier, it is a different signal: flat-topping generates
 *   intermodulation products across the whole span, at frequencies
 *   nothing is transmitting. The fix is less level.
 *
 *   THEIRS. The generator's own DAC is working in a handful of least
 *   significant bits, so its quantisation noise is broadband and large.
 *   The fix is MORE level at the generator and an external attenuator -
 *   the opposite change at the generator, the same change at the
 *   receiver.
 *
 * THE MEASUREMENT THAT SEPARATES THEM COSTS NOTHING AND NEEDS NO
 * HARDWARE. Walk the baseband gain reduction and watch the
 * signal-to-floor ratio:
 *
 *   - if it is clipping, backing the gain off takes the receiver out of
 *     compression and the ratio IMPROVES, steeply, and then flattens;
 *
 *   - if it is the generator's noise floor, the carrier and the noise
 *     are attenuated together and the ratio DOES NOT MOVE, because a
 *     gain change cannot improve a ratio that arrived at the antenna.
 *
 * That is the whole test. A number that changes with our gain is ours;
 * a number that does not is in the signal, and no setting of this
 * receiver will fix it.
 */
/* Defined with the display range it shares, down in the step 7 block:
 * the floor and the gutter labels have to describe the same decibels. */
static float spec_floor_db(const float *db, uint32_t bins);

/* Defined below, with the constant it replaced. */
static uint8_t rf_autorange(void);

static void test_signal_quality(void)
{
    static const uint8_t k_gr[] = { 0U, 6U, 12U, 20U, 30U, 40U };
    static const char *const k_gr_txt[] = {
        " 0", " 6", "12", "20", "30", "40"
    };

    int      preamp;
    uint32_t i;

    console_printf("Test 8b - is the mess ours or the generator's?\n");
    console_printf("=============================================\n");
    console_printf("  snr is the peak over the spectrum's MEDIAN, which\n");
    console_printf("  is the floor whatever the carrier does - see\n");
    console_printf("  spec_floor_db(). Walk our own gain and watch it:\n");
    console_printf("    snr IMPROVES as gain comes off -> WE are clipping\n");
    console_printf("    snr does not move              -> it is in the\n");
    console_printf("                                      signal, and no\n");
    console_printf("                                      gain fixes it\n\n");

    fft_tam_iq(PAN_BINS);

    console_printf("  preamp | GR | adc  | clipped | peak | floor | snr\n");
    console_printf("  -------+----+------+---------+------+-------+-----\n");

    for (preamp = 1; preamp >= 0; preamp--) {
        msi001_preamp(preamp);

        for (i = 0; i < sizeof k_gr / sizeof k_gr[0]; i++) {
            iq_peak_t pk;

            board_watchdog_feed();
            msi001_set_gain(k_gr[i], 0, 0, 0);
            board_delay_ms(20);

            if (capture() == 0U) { continue; }

            deinterleave(PAN_BINS);
            fft_compute_db_iq(s_i, s_q, s_db);
            iq_peak(s_db, PAN_BINS, 192000U, &pk);

            float fl = spec_floor_db(s_db, PAN_BINS);

            console_printf("    %s  | %s | %3u%% | %7u | %4d | %5d | %3d%s\n",
                           preamp ? "ON " : "off",
                           k_gr_txt[i],
                           s_cap_used_pct, s_cap_clipped,
                           (int32_t)pk.db, (int32_t)fl,
                           (int32_t)(pk.db - fl),
                           s_cap_clipped ? "  *CLIP*" : "");
        }
    }

    msi001_preamp(0);
    msi001_set_gain(0, 0, 0, 0);
    console_printf("\n");
}

/* ================================================================== */
/* THE AM ENVELOPE, AND WHOSE DISTORTION IT IS                        */
/* ================================================================== */

/*
 * MEASURE THE ENVELOPE, NOT THE SPECTRUM.
 *
 * The question is whether the AM envelope is distorted, and the
 * sidebands that would show it sit at +/-1000, +/-2000 and +/-3000 Hz
 * around a carrier whose ABSOLUTE frequency nobody knows to better than
 * about 125 Hz - the generator and this radio do not share a reference,
 * which is the same fact that cost 16 dB in step 5. Measuring those
 * sidebands in the spectrum therefore means finding the carrier first
 * and then doing seven non-coherent DFTs with a window, and the whole
 * answer depends on how well the carrier was found.
 *
 * The envelope |I + jQ| throws the carrier away. What is left is DC
 * plus a 1000 Hz tone plus harmonics - at frequencies the GENERATOR
 * sets exactly, so they are known to the hertz without measuring
 * anything. And 1000 Hz at 192 kHz is one cycle in exactly 192
 * samples, so a 192-entry table measures the fundamental and both
 * harmonics coherently with nothing but a stride: no window, no
 * leakage, no scalloping, and no dependence on where the carrier
 * landed.
 *
 *     depth        m = 2 |X(1000)| / |X(0)|
 *     distortion   |X(2000)| / |X(1000)|, which is ZERO for real AM
 *
 * WHAT MAKES IT WORTH THE CODE: the two things we need to tell apart
 * have opposite signatures, and that was checked on synthetic signals
 * on a host before any of it ran on the radio - tools/envdft.py, which
 * also generates the table:
 *
 *     clean AM at 70 %            depth 70.0 %   2nd at -325 dB
 *     envelope clipped 10 %       depth 66.9 %   2nd at - 23.9 dB
 *     envelope clipped 25 %       depth 57.8 %   2nd at - 12.9 dB
 *     envelope clipped hard       depth 47.6 %   2nd at -  8.3 dB
 *     DAC working in ~10 LSBs     depth 69.7 %   2nd at - 42.9 dB
 *
 * So CLIPPING drags the depth DOWN and the harmonic UP, while
 * QUANTISATION leaves the depth right. One line of output separates
 * "the generator is clipping its own modulation" from "the generator is
 * out of bits" from "our converter is clipping" - and the third is
 * already covered, because capture() reports the ADC usage next to it.
 *
 * The table is generated on a host and embedded rather than computed
 * from a hand-rolled sine: libm's sinf() does not link in this image
 * (newlib wants __aeabi_l2d, which -nostdlib does not provide), and
 * trap 10 in WORKING.md is about a polynomial written from memory that
 * was exact at every power of two and 1.5 bits wrong in between.
 */
#include "env_tbl.h"        /* ../../compat/env_tbl.h, generated */

/*
 * A SQUARE ROOT, IN ONE INSTRUCTION, because getting one here is
 * harder than it looks and the three obvious routes all fail:
 *
 *   sqrtf()            does not link. newlib's wants __aeabi_l2d, and
 *                      -nostdlib does not provide it. Same reason
 *                      sinf() is unavailable and upstream's fft.c
 *                      carries its own approximations.
 *   __builtin_sqrtf()  compiles to this very instruction, but ONLY
 *                      with -fno-math-errno. Without it gcc emits a
 *                      call to sqrtf() for the negative-argument errno
 *                      path, and that is the previous line again.
 *   arm_sqrt_f32()     CMSIS guards the sign and then calls sqrtf()
 *                      anyway on GCC, so it inherits the same problem.
 *
 * Adding -fno-math-errno to CFLAGS would fix all three at once and is
 * the usual answer, but CFLAGS here also compiles eleven grafted
 * upstream files, and changing how THEY are built to get a square root
 * in one of ours is the wrong trade. The FPU has the instruction; this
 * asks for it.
 *
 * VSQRT.F32 returns NaN for a negative input rather than trapping, and
 * every caller below feeds it a sum of squares, so the sign check the
 * library versions spend their time on has nothing to do here.
 */
__attribute__((always_inline))
static inline float fsqrt(float x)
{
    float r;

    __asm__ ("vsqrt.f32 %0, %1" : "=t" (r) : "t" (x));
    return r;
}

static float fhypot(float a, float b)
{
    return fsqrt(a * a + b * b);
}

/* Whole cycles only. A partial cycle is exactly what makes a coherent
 * measurement leak, and the capture is far longer than it needs to be,
 * so rounding down costs nothing. */
#define ENV_FRAMES   ((CAP_FRAMES / ENV_TBL_N) * ENV_TBL_N)

static float s_env[ENV_FRAMES];

/* Coherent magnitude of the envelope at harm x 1000 Hz. harm 0 is the
 * mean, which is the carrier level the depth is measured against. */
static float env_dft(uint32_t harm)
{
    float re = 0.0f, im = 0.0f;
    uint32_t k;

    if (harm == 0U) {
        for (k = 0; k < ENV_FRAMES; k++) { re += s_env[k]; }
        return re / (float)ENV_FRAMES;
    }

    for (k = 0; k < ENV_FRAMES; k++) {
        uint32_t idx = (k * harm) % (uint32_t)ENV_TBL_N;

        re += s_env[k] * env_cos[idx];
        im -= s_env[k] * env_sin[idx];
    }

    /* x2 because a real tone splits its energy between +f and -f, and
     * only one of the two is being evaluated here. */
    return 2.0f * fhypot(re, im) / (float)ENV_FRAMES;
}

/*
 * THE MEASUREMENT. Reports the depth the generator is actually
 * delivering and the two harmonics that say whether its envelope
 * survived the trip - next to the ADC usage, which says whether OUR
 * converter is the one doing the damage.
 */
static void test_am_envelope(void)
{
    uint32_t k;

    console_printf("Test 8c - the AM envelope: whose distortion is it?\n");
    console_printf("=================================================\n");
    console_printf("  |I+jQ| throws the carrier away, so none of this\n");
    console_printf("  depends on where the carrier landed - and 1000 Hz\n");
    console_printf("  is one cycle in exactly 192 samples at 192 kHz, so\n");
    console_printf("  the fundamental and both harmonics are coherent.\n");
    console_printf("  Verified on synthetic signals first - see\n");
    console_printf("  ../tools/envdft.py:\n\n");
    console_printf("    clean AM at 70 %%        depth 70.0 %%  2nd -325 dB\n");
    console_printf("    envelope clipped 10 %%   depth 66.9 %%  2nd - 24 dB\n");
    console_printf("    envelope clipped 25 %%   depth 57.8 %%  2nd - 13 dB\n");
    console_printf("    DAC in about 10 LSBs    depth 69.7 %%  2nd - 43 dB\n\n");
    console_printf("  So: depth LOW and 2nd HIGH is clipping; depth RIGHT\n");
    console_printf("  with the 2nd up is quantisation. And if adc says\n");
    console_printf("  CLIP then it is ours and the rest is moot.\n\n");

    if (capture() == 0U) {
        console_printf("  capture failed\n\n");
        return;
    }

    /*
     * SUBTRACT THE DC BEFORE TAKING THE ENVELOPE, or measure the DC
     * instead of the modulation.
     *
     * |I + jQ| is not linear, so a standing offset does not simply add
     * to the envelope - it beats against the rotating carrier and both
     * flattens the modulation and manufactures harmonics of it. The
     * first version of this test skipped the subtraction and reported
     * 62.5 % depth with a 2nd harmonic at -39.7 dB for a generator set
     * to 70 %, which reads exactly like a distorting generator.
     *
     * It was this instrument. With the offset this board actually has
     * (+1705 and -332 counts against an envelope of 6569, so 26 %),
     * tools/envdft.py predicts 66.2 % and -37.7 dB FROM A PERFECT 70 %
     * SIGNAL - which is most of what was measured, produced entirely by
     * the measurement.
     *
     * Note what this is: the same correction upstream applies in
     * demod_am.c as s_rf_dc_i / s_rf_dc_q, subtracted before the NCO
     * mix, and for the same reason. Arriving at it independently from
     * the other end is some evidence it is the right place for it.
     *
     * The mean is a sound DC estimate here: the carrier sits at
     * 19875 Hz, which is 834.75 cycles over this capture, so what it
     * leaves in the mean is under a part in four thousand.
     */
    float dc_i = 0.0f, dc_q = 0.0f;

    for (k = 0; k < ENV_FRAMES; k++) {
        dc_i += (float)s_rx[k * 2U];
        dc_q += (float)s_rx[k * 2U + 1U];
    }
    dc_i /= (float)ENV_FRAMES;
    dc_q /= (float)ENV_FRAMES;

    for (k = 0; k < ENV_FRAMES; k++) {
        float i = (float)s_rx[k * 2U]      - dc_i;
        float q = (float)s_rx[k * 2U + 1U] - dc_q;

        s_env[k] = fhypot(i, q);
    }

    float dc  = env_dft(0U);
    float h1  = env_dft(1U);
    float h2  = env_dft(2U);
    float h3  = env_dft(3U);

    /* Depth as a percentage, in tenths, and the harmonics in dB
     * relative to the fundamental - which is the only ratio that means
     * anything here, since the absolute level depends on the gain. */
    int32_t depth10 = (dc > 0.0f) ? (int32_t)((h1 / dc) * 1000.0f) : 0;
    int32_t d2      = iq_db10((uint64_t)(h2 * 1000.0f),
                              (uint64_t)(h1 * 1000.0f));
    int32_t d3      = iq_db10((uint64_t)(h3 * 1000.0f),
                              (uint64_t)(h1 * 1000.0f));

    console_printf("  adc %u%%%s, %u frames (%u whole cycles of 1000 Hz)\n",
                   s_cap_used_pct, s_cap_clipped ? " *CLIP*" : "",
                   (uint32_t)ENV_FRAMES,
                   (uint32_t)(ENV_FRAMES / ENV_TBL_N));
    console_printf("  DC removed first        : I %+d  Q %+d counts\n",
                   (int32_t)dc_i, (int32_t)dc_q);
    console_printf("  carrier (envelope mean) : %u counts\n",
                   (uint32_t)dc);
    console_printf("  modulation depth        : %d.%u %%   "
                   "<- the generator is set to 70\n",
                   depth10 / 10, (uint32_t)(depth10 % 10));
    console_printf("  2nd harmonic            : %4d.%u dB\n",
                   IQ_DB_INT(d2), IQ_DB_FRAC(d2));
    console_printf("  3rd harmonic            : %4d.%u dB\n\n",
                   IQ_DB_INT(d3), IQ_DB_FRAC(d3));
}

/* ================================================================== */
/* STEP 9a: WHICH RELAY COIL IS WHICH                                 */
/* ================================================================== */

/*
 * The add-on board's input relay is a two-coil LATCHING type, and the
 * factory firmware pulses PD8 and PD11 to throw it. Which of the two
 * selects the High-Z input and which the straight 50 ohm path is the
 * one thing the schematics cannot settle: the vendor's main-board
 * connector is 8-pin, this radio's J7 is 10-pin, and the link between
 * them is hand-soldered wire.
 *
 * SO ASK THE SIGNAL - but do not expect it to shout.
 *
 * THE FIRST VERSION OF THIS TEST EXPECTED TENS OF DECIBELS and called
 * the measured three a failure. The expectation was wrong twice over:
 *
 *   - R19 and R20, the 470 kohm pair, are a static drain to ground on
 *     the antenna, not 940 kohm in series with the signal. Nothing in
 *     this board puts that in the path.
 *
 *   - A High-Z input barely LOADS a 50 ohm generator, so it sees the
 *     full open-circuit voltage - six decibels more than the matched
 *     case - and the source follower and the TC4-1+ then give some of
 *     that back. The two paths landing within a few decibels of each
 *     other is what the physics predicts, not a fault.
 *
 * What the measurement does establish, and firmly: the relay is ours,
 * it latches, and the two positions differ repeatably. What it does
 * NOT establish is which is which, because the sign of a few decibels
 * depends on which way round the 4:1 transformer is wired and that is
 * not readable from the drawing.
 *
 * AND HERE IS WHAT SETTLES IT, WITHOUT A WHIP: let the factory
 * firmware choose the position and then ask which of our two coils
 * reproduces it.
 *
 * The stock UI can select the 50 ohm input by name. So: power-cycle
 * into it, set 50 ohm, load this image, and measure BEFORE touching
 * either coil. Then pulse A, measure; pulse B, measure. The as-found
 * reading matches exactly one of them, and that one is the 50 ohm
 * position - established by the firmware that has the label, not by
 * guessing the sign of three decibels.
 *
 * THE GAIN MUST BE FIXED FOR THIS, and that is the trap worth writing
 * down: rf_autorange() exists to hold the converter near half scale,
 * so running it between these readings would quietly cancel the very
 * difference being measured. All three are taken at one gain, chosen
 * once, and the comparison is inside a single run so that nothing
 * about the bench can drift between the numbers being compared.
 *
 * WHY THIS IS SAFE TO RUN. Each coil gets one 10 ms pulse - the factory
 * firmware's own timing - and is then released. That is the correct and
 * only correct way to drive a latching relay, so the worst case is the
 * relay ending up in the other position, which the next pulse undoes.
 * Nothing here can hold a coil on.
 *
 * AND IT LEAVES THE RELAY WHERE THE BENCH WANTS IT, which is the
 * position that passed more signal. A test that settles a question and
 * leaves the hardware in the worse of the two states it just compared
 * is a trap for whoever runs the next one.
 */
/*
 * One capture, one line, and the peak handed back so that a caller
 * comparing two states does not have to capture a second time for the
 * number it is going to compare. Returns -200 if the capture failed,
 * which is far enough below any real reading to be obvious.
 */
/*
 * WAS THE PEAK THE CARRIER, OR JUST THE LOUDEST NOISE?
 *
 * iq_peak() always returns something. When the carrier is below the
 * floor it returns whichever noise bin happened to be highest, and the
 * level that comes back is a perfectly plausible number that means
 * nothing - the front-end table's first two rows were exactly that,
 * reported to a tenth of a decibel, and every figure derived from them
 * was wrong.
 *
 * The carrier's position is known: the receiver is tuned OFFSET_HZ
 * below the generator. A peak more than a few bins away from there is
 * not the carrier, whatever its amplitude. One bin is 375 Hz, so three
 * is generous and still unambiguous.
 */
static int32_t addon_report_tail(void)
{
    iq_peak_t pk;

    if (capture() == 0U) {
        console_printf("capture failed\n");
        s_last_off_hz = 0;
        return -2000;
    }

    deinterleave(PAN_BINS);
    fft_compute_db_iq(s_i, s_q, s_db);
    iq_peak(s_db, PAN_BINS, 192000U, &pk);

    float fl = spec_floor_db(s_db, PAN_BINS);

    s_last_off_hz = pk.offset_hz;

    console_printf("adc %3u%%%s | peak %+7d Hz %5d.%u dB "
                   "| floor %3d | snr %3d dB%s\n",
                   s_cap_used_pct,
                   s_cap_clipped ? " CLIP" : "    ",
                   pk.offset_hz,
                   (int32_t)pk.db,
                   (uint32_t)(((int32_t)(pk.db * 10.0f)) % 10),
                   (int32_t)fl, (int32_t)(pk.db - fl),
                   PEAK_IS_CARRIER(pk.offset_hz) ? "" : "  <- NOT the carrier");

    return (int32_t)(pk.db * 10.0f);
}

static void addon_report(const char *what)
{
    console_printf("  %s | ", what);
    (void)addon_report_tail();
}

/*
 * The peak level alone, in TENTHS of a decibel.
 *
 * Whole decibels were not enough, and the way that showed is worth
 * keeping: the attenuator sweep read a tidy, systematic 1 dB short at
 * every setting above 2 dB. A systematic error that size looks like a
 * real property of the part - and it was `(int32_t)pk.db` truncating
 * both the reference and the reading, losing up to a decibel from a
 * difference of two numbers that are each up to a decibel low.
 *
 * An instrument that quantises to 1 dB cannot check a part specified
 * in half-decibel steps. That should have been obvious before the
 * measurement rather than after it.
 */
static int32_t addon_peak_db10(void)
{
    iq_peak_t pk;

    if (capture() == 0U) { return -2000; }
    deinterleave(PAN_BINS);
    fft_compute_db_iq(s_i, s_q, s_db);
    iq_peak(s_db, PAN_BINS, 192000U, &pk);
    return (int32_t)(pk.db * 10.0f);
}

/* Whole decibels, where a decibel is enough. */
static int32_t addon_peak_db(void)
{
    return addon_peak_db10() / 10;
}

/*
 * One gain for all three readings, and not rf_autorange()'s.
 *
 * 16 dB is what autorange chose on this bench with the generator at
 * 100 mV through 40 dB and the add-on board's own attenuator at the
 * 30 dB the factory firmware leaves it on. It is written down here
 * rather than measured because the whole point is that it must not
 * move between the readings - and if the bench changes, this test
 * says so loudly by clipping or by sinking into the floor, which is
 * better than silently normalising the answer away.
 */
#define RELAY_TEST_GR   16U

static void test_addon_relay(void)
{
    int32_t found_db, a_db, b_db;

    console_printf("\n");
    console_printf("Test 9a - the add-on board's input relay\n");
    console_printf("========================================\n");
    console_printf("  Two coils, PD8 and PD11, one 10 ms pulse each -\n");
    console_printf("  the factory firmware's own timing. One selects the\n");
    console_printf("  High-Z input, the other the straight 50 ohm path.\n\n");
    console_printf("  EXPECT ONLY A FEW dB, not tens. A High-Z input\n");
    console_printf("  hardly loads a 50 ohm generator, so it sees the\n");
    console_printf("  full open-circuit voltage - 6 dB more than the\n");
    console_printf("  matched case - and the follower and the 4:1\n");
    console_printf("  transformer give some of that back. What matters\n");
    console_printf("  here is that the difference REPEATS.\n\n");

    addon_init();

    console_printf("  GPIOD ODR after addon_init(): 0x%08x\n", addon_odr());
    console_printf("  Vext_ON (PD9) = %u, so the board is %s\n\n",
                   (addon_odr() >> ADDON_PIN_VEXT) & 1U,
                   addon_powered() ? "POWERED" : "*** NOT POWERED ***");

    /*
     * ONE GAIN, FIXED, FOR ALL THREE. Set before the first reading and
     * not touched again - see RELAY_TEST_GR.
     */
    msi001_set_gain(RELAY_TEST_GR, 0, 0, 0);
    board_delay_ms(20);
    console_printf("  all readings at a FIXED %u dB of gain reduction, "
                   "so that\n  autoranging cannot normalise the "
                   "difference away\n\n", (uint32_t)RELAY_TEST_GR);

    /* FIRST, AND BEFORE EITHER COIL IS TOUCHED. Whatever the factory
     * firmware selected is still latched in; this is the only chance
     * to see it. */
    addon_report("as found, relay UNTOUCHED ");
    found_db = addon_peak_db();

    addon_input_50r();
    addon_report("after pulsing PD8  (coil A)");
    a_db = addon_peak_db();

    addon_input_highz();
    addon_report("after pulsing PD11 (coil B)");
    b_db = addon_peak_db();

    /* Round two. A latching relay that gives the same two readings the
     * second time is a latching relay; one that does not is a contact
     * that is not closing, and the difference matters more than either
     * reading. */
    addon_input_50r();
    addon_report("PD8  again, to show it is ");
    addon_input_highz();
    addon_report("PD11 again, repeatable    ");

    /*
     * WHICH COIL REPRODUCES WHAT WE FOUND. The as-found reading was
     * taken with the factory firmware's own choice still latched in,
     * so whichever coil lands on it is the coil that selects whatever
     * the stock UI was set to.
     */
    {
        int32_t da = found_db - a_db; if (da < 0) { da = -da; }
        int32_t db_ = found_db - b_db; if (db_ < 0) { db_ = -db_; }

        console_printf("\n  as found %d dB; PD8 is %d dB away, "
                       "PD11 is %d dB away\n", found_db, da, db_);

        /*
         * NEARER WINS, measured against the gap between the two - not
         * against a fixed margin.
         *
         * The first version demanded that the nearer be two whole
         * decibels nearer, and on the run that answered the question
         * it got 1 dB against 3 dB and printed "cannot tell". Asking
         * for a fixed margin between two distances is the wrong shape
         * of test when the quantity being resolved is itself only
         * three decibels: the right scale is the A-to-B gap, and half
         * of it is the natural dividing line.
         */
        int32_t gap = a_db - b_db; if (gap < 0) { gap = -gap; }

        if (gap < 2) {
            console_printf("  -> *** THE TWO POSITIONS READ THE SAME ***"
                           " nothing to match against\n");
        } else if (da * 2 < gap && db_ * 2 > gap) {
            console_printf("  -> the factory firmware had selected "
                           "PD8's position\n");
        } else if (db_ * 2 < gap && da * 2 > gap) {
            console_printf("  -> the factory firmware had selected "
                           "PD11's position\n");
        } else {
            console_printf("  -> *** CANNOT TELL *** the as-found "
                           "reading sits between the two\n");
        }
        console_printf("     If the stock UI was set to 50 ohm, that "
                       "position IS the\n     50 ohm one, and the "
                       "other is High-Z.\n");
    }

    console_printf("\n  coil A (PD8) %d dB, coil B (PD11) %d dB, "
                   "difference %d dB\n",
                   a_db, b_db, a_db - b_db);

    /*
     * Two decibels is the threshold, and it is a question about
     * REPEATABILITY rather than about size. The frame-to-frame scatter
     * on this measurement is well under a decibel, so a difference that
     * survives two rounds at two decibels is real even though it is
     * small. Asking for tens of decibels here was the first version's
     * mistake, and it reported a working relay as a broken one.
     */
    if (a_db > b_db + 2) {
        console_printf("  -> PD8's position passes more. The relay works\n");
        console_printf("     and latches; WHICH of the two is High-Z is\n");
        console_printf("     NOT settled by this - see the comment above.\n");
        addon_input_50r();
    } else if (b_db > a_db + 2) {
        console_printf("  -> PD11's position passes more. The relay works\n");
        console_printf("     and latches; WHICH of the two is High-Z is\n");
        console_printf("     NOT settled by this - see the comment above.\n");
        addon_input_highz();
    } else {
        /*
         * No difference at all is the result that means something is
         * wrong - the contacts are not moving, or the signal is not
         * arriving through them. Say so rather than picking the larger
         * of two numbers that are the same.
         */
        console_printf("  -> *** THE TWO POSITIONS READ THE SAME ***\n");
        console_printf("     the contacts are not moving, or the signal\n");
        console_printf("     is not arriving through them. If the relay\n");
        console_printf("     can be HEARD clicking, it is the second.\n");
        addon_input_50r();
    }

    console_printf("  left in the position that passed more signal\n\n");
}

/* ================================================================== */
/* STEP 9b: THE ADD-ON BOARD'S FILTER BANK                            */
/* ================================================================== */

/*
 * THE FIRST WRITE TO THE 74HC595, and the first time this board has
 * been in a state we chose rather than inherited.
 *
 * The check is as hard as checks get, because the right answer is
 * mostly ZERO. A 10 MHz carrier may pass the 4.5 MHz highpass and must
 * be rejected by the 1.5-4.5 MHz bandpass, by the 0.5-1.5 MHz bandpass
 * and by the 500 kHz lowpass. Three of the four readings should fall
 * into the noise, and a test whose expected result is "the signal
 * disappears three times out of four" cannot be passed by accident:
 * wiring that does nothing leaves all four readings identical, and
 * that is a distinctive failure rather than a plausible one.
 *
 * THE GAIN IS FIXED, for the reason step 9a learned the hard way:
 * rf_autorange() holds the converter near half scale, so letting it
 * run between these readings would wind the gain up as each filter
 * rejected the carrier and report four comfortable, meaningless
 * numbers.
 *
 * WHAT THIS DOES NOT TOUCH IS THE ATTENUATOR. The 595's outputs are
 * frozen by its storage register while the shift register is clocked,
 * so the PE4312's DATA, CLK and LE lines change once, together, on the
 * latch edge - and addon_set_filter() forces ATT_LE low in every byte
 * it writes, so the attenuator is never handed a latch edge and keeps
 * the 30 dB the factory firmware left it on.
 */
/*
 * ALL SIXTEEN COMBINATIONS, BECAUSE THE DRAWINGS DISAGREE.
 *
 * The filter table this driver started with came from the KiCad
 * rebuild of the board, which lists the output switch's four ports in
 * a different order from the vendor's own schematic. Both cannot be
 * right, and BOARD.md's own warning - check the rebuild against the
 * vendor PDF before trusting a pin number from it - was written by the
 * same hand that then did not.
 *
 * The cost of being wrong is not a weak signal, it is NO signal: the
 * two switches sit at opposite ends of four filters, so a code that
 * means "filter 3" at one end and "filter 1" at the other connects the
 * input of one to the output of another. That is exactly what happened
 * - all four selections read the noise floor, which looked like a bank
 * that was not switching at all.
 *
 * Four bits is sixteen combinations, which is nothing. At 10 MHz
 * exactly one filter can pass, so the sweep should show a small number
 * of combinations well above the rest - and those identify the true
 * truth table without reading anything. The main board's bank was
 * settled the same way, by sweeping all 64, and BOARD.md section 6
 * still carries the result.
 */
static void test_addon_filter_sweep(void)
{
    uint32_t m;
    int32_t  best = -200;
    uint32_t best_m = 0;

    console_printf("\n");
    console_printf("Test 9b - the add-on filter bank, all 16 codes\n");
    console_printf("=============================================\n");
    console_printf("  The two drawings of this board disagree about which\n");
    console_printf("  filter sits on which port of the OUTPUT switch, so\n");
    console_printf("  neither is used: all sixteen are tried instead.\n\n");
    console_printf("  A wrong code is not a weak signal, it is NO signal -\n");
    console_printf("  it joins the input of one filter to the output of\n");
    console_printf("  another. At %u Hz only the highpass can pass, so\n",
                   GEN_HZ);
    console_printf("  only the codes that select it at BOTH ends should\n");
    console_printf("  show a carrier.\n\n");

    msi001_set_gain(RELAY_TEST_GR, 0, 0, 0);
    board_delay_ms(20);

    console_printf("  E12 E11 E22 E21  byte |\n");
    console_printf("  ---------------------+\n");

    for (m = 0; m < 16U; m++) {
        uint8_t bits = (uint8_t)(m << 1);   /* E21=bit1 .. E12=bit4 */
        int32_t lvl;

        board_watchdog_feed();
        addon_595_filter_bits(bits);
        board_delay_ms(20);

        console_printf("   %u   %u   %u   %u   0x%02x | ",
                       (m >> 3) & 1U, (m >> 2) & 1U,
                       (m >> 1) & 1U, m & 1U,
                       (uint32_t)addon_595_shadow());
        lvl = addon_report_tail();

        if (lvl > best) { best = lvl; best_m = m; }
    }

    console_printf("\n  strongest: E12=%u E11=%u E22=%u E21=%u, %d dB\n",
                   (best_m >> 3) & 1U, (best_m >> 2) & 1U,
                   (best_m >> 1) & 1U, best_m & 1U, best);
    console_printf("  That code selects the highpass at both ends. The\n");
    console_printf("  other three filters follow from the same two\n");
    console_printf("  truth tables once this one is pinned.\n\n");

    /* Leave it on whatever actually passed, so the rest of the run has
     * a signal to work with. */
    addon_595_filter_bits((uint8_t)(best_m << 1));
}

/* ================================================================== */
/* STEP 9c: THE PE4312 STEP ATTENUATOR                                */
/* ================================================================== */

/*
 * DOES IT ATTENUATE BY WHAT IT WAS TOLD TO?
 *
 * The protocol came out of the factory firmware's disassembly, and
 * every part of it is a claim that can be wrong independently: the bit
 * order, the step size, which edge latches, and whether the register
 * counts decibels or half-decibels. A sweep of the SIX INDIVIDUAL BITS
 * tests all four at once, because each bit has a different weight and
 * the weights double:
 *
 *     code  1 -> 0.5 dB      code  8 ->  4 dB
 *     code  2 -> 1 dB        code 16 ->  8 dB
 *     code  4 -> 2 dB        code 32 -> 16 dB
 *
 * If the bit order were reversed, that column would run backwards and
 * the error would be unmissable - code 1 would measure 16 dB. If the
 * register were in whole decibels, every measured value would be
 * double its label. Neither failure can hide in a monotonic sweep of
 * round numbers, which is why this walks the bits rather than the
 * decibels.
 *
 * THE GAIN IS FIXED, set once at zero attenuation, for the reason step
 * 9a learned: rf_autorange() would wind the gain up as the attenuator
 * wound the signal down and report a flat line.
 */
static void test_addon_atten(void)
{
    /* The six bits, then both ends of the range. */
    static const uint8_t k_codes[9] = { 0U, 1U, 2U, 4U, 8U, 16U, 32U,
                                        63U, 0U };
    int32_t  ref = 0;
    uint32_t i;

    console_printf("\n");
    console_printf("Test 9c - the PE4312 step attenuator\n");
    console_printf("====================================\n");
    console_printf("  Walking the six BITS, not round decibels: each has\n");
    console_printf("  twice the weight of the last, so a reversed bit\n");
    console_printf("  order or a wrong step size cannot hide. The\n");
    console_printf("  protocol came out of the factory firmware and\n");
    console_printf("  every part of it is a separate claim.\n\n");

    addon_set_filter(addon_filter_for(GEN_HZ));
    addon_set_att_halfdb(0U);
    board_delay_ms(20);

    {
        uint8_t gr = rf_autorange();
        console_printf("  gain fixed at %u dB, set at 0 dB attenuation "
                       "so nothing clips\n\n", (uint32_t)gr);
    }

    console_printf("  code | asked  | peak   | measured | error\n");
    console_printf("  -----+--------+--------+----------+------\n");

    for (i = 0; i < 9U; i++) {
        uint8_t  c = k_codes[i];
        int32_t  lvl, want, got, err;

        board_watchdog_feed();
        addon_set_att_halfdb(c);
        board_delay_ms(20);

        lvl = addon_peak_db10();        /* tenths, not whole decibels */
        if (i == 0U) { ref = lvl; }

        want = (int32_t)c * 5;          /* tenths of a dB */
        got  = ref - lvl;
        err  = got - want;

        console_printf("   %2u  | %2u.%u dB | %4d.%u dB | %3d.%u dB  | "
                       "%+3d.%u dB%s\n",
                       (uint32_t)c,
                       (uint32_t)(c / 2U), (uint32_t)((c & 1U) ? 5U : 0U),
                       lvl / 10, (uint32_t)((lvl < 0 ? -lvl : lvl) % 10),
                       got / 10, (uint32_t)((got < 0 ? -got : got) % 10),
                       err / 10, (uint32_t)((err < 0 ? -err : err) % 10),
                       (i == 8U) ? "   <- back to zero" : "");
    }

    console_printf("\n  The middle six rows must DOUBLE down the "
                   "'measured'\n  column. If they run the other way the "
                   "bit order is\n  reversed; if they are all twice "
                   "their label the\n  register counts whole decibels, "
                   "not halves.\n");
    console_printf("  The last row returning to zero is the proof that "
                   "the\n  part is being programmed and not drifting.\n\n");

    addon_set_att_halfdb(0U);
}

/* ================================================================== */
/* THE ADD-ON BOARD FROM COLD, OVER AND OVER                          */
/* ================================================================== */

/*
 * WHY THIS EXISTS - 21/11/2026. Some cold starts came up deaf: the
 * oscillator's leakage normal, the audio running, and no carrier at all.
 * Caught live one morning and cured on the spot by ONE call over SWD,
 * addon_set_filter(HPF) (tools/swd-call.sh): the add-on board's 74HC595
 * had lost the filter code the boot wrote, or never got it - and a wrong
 * code joins one filter's input to another's output, which is no signal.
 * Nothing writes it again until the dial changes band.
 *
 * Why it was lost, MEASURED with this test, MCU warm, 12 cycles each:
 *
 *   boot order: 10 ms, attenuator, filter, relay pulse   12 deaf
 *   the same without the relay pulse                    12 deaf
 *   10 ms, relay pulse (15 ms), attenuator, filter        0 deaf
 *   200 ms, attenuator, filter, relay pulse               0 deaf
 *
 * So not the relay: TIME. The board is not ready 10 ms after its
 * Vext_ON, and the boot's real writes, pushed later by console lines,
 * sat near the threshold - hence a fault that came and went.
 *
 * What it does now is sweep that time - addon_rail_ms - in the boot's
 * own order, n cycles per value, interleaved so that a drift on the
 * bench lands on every value alike, and check after each cycle whether
 * the carrier is back. Off is Vext_ON low with every other control
 * line in analogue, as after a reset: a line driven high into an
 * unpowered 74HC595 feeds it through its input protection, which is
 * not a cold start at all.
 *
 * ADDON_COLD_TEST is the number of cycles per value; 0 leaves it out.
 * It needs the generator on the dial and takes ~1.2 s a cycle.
 */
#ifndef ADDON_COLD_TEST
#define ADDON_COLD_TEST 0
#endif

static void addon_power_down(void)
{
    static const uint32_t k_lines[] = {
        ADDON_PIN_LE, ADDON_PIN_DIN, ADDON_PIN_CLK,
        ADDON_PIN_REL_50R, ADDON_PIN_REL_HIZ
    };

    GPIOD->BSRR = (1UL << (ADDON_PIN_VEXT + 16U));
    for (uint32_t k = 0; k < sizeof k_lines / sizeof k_lines[0]; k++) {
        GPIOD->MODER |= (3UL << (k_lines[k] * 2U));     /* analogue */
    }
}

static void test_addon_cold(uint32_t n)
{
    static const uint32_t k_ms[] = { 10U, 15U, 20U, 25U, 30U, 50U, 100U, 200U };
    enum { NV = sizeof k_ms / sizeof k_ms[0] };
    uint32_t deaf[NV] = { 0U };
    uint32_t keep = addon_rail_ms;
    int32_t  want = (int32_t)GEN_HZ - (int32_t)msi001_last_plan()->f_actual;

    console_printf("\nTest - the add-on board from cold: %u cycles per "
                   "rail time, carrier expected at %+d Hz\n", n, want);
    console_printf("=========================================="
                   "===========================\n");

    for (uint32_t i = 0; i < n * NV; i++) {
        uint32_t v = i % NV;
        iq_peak_t pk;

        board_watchdog_feed();
        addon_power_down();
        board_delay_ms(1000);

        addon_rail_ms = k_ms[v];
        addon_init();                       /* waits addon_rail_ms */
        addon_set_att_halfdb(0U);
        addon_set_filter(ADDON_F_HPF_4M5);
        addon_input_50r();
        board_delay_ms(30);

        if (capture() == 0U) { console_printf("  capture failed\n"); continue; }
        deinterleave(PAN_BINS);
        fft_compute_db_iq(s_i, s_q, s_db);
        iq_peak(s_db, PAN_BINS, 192000U, &pk);
        float fl = spec_floor_db(s_db, PAN_BINS);
        int32_t off = pk.offset_hz - want;
        int ok = (off > -400 && off < 400) && (pk.db - fl) > 30.0f;

        if (!ok) { deaf[v]++; }
        console_printf("  %2u  rail %3u ms | peak %+7d Hz %4d dB | snr %3d dB"
                       "  %s\n", i / NV, k_ms[v], pk.offset_hz,
                       (int32_t)pk.db, (int32_t)(pk.db - fl),
                       ok ? "ok" : "*** DEAF ***");
    }

    console_printf("\n  deaf, of %u each:", n);
    for (uint32_t v = 0; v < NV; v++) {
        console_printf("  %u ms: %u", k_ms[v], deaf[v]);
    }
    console_printf("\n\n");

    /* Back as the boot leaves it, whatever the last cycle did. */
    addon_rail_ms = keep;
    addon_set_filter(addon_filter_for(GEN_HZ));
    addon_input_50r();
}

static void test_addon_filters(void)
{
    static const addon_filter_t k_order[4] = {
        ADDON_F_HPF_4M5,        /* the one that should pass, first */
        ADDON_F_1M5_4M5,
        ADDON_F_0M5_1M5,        /* and last, so the bank ends up    */
        ADDON_F_LPF_500K        /* somewhere obviously wrong        */
    };

    int32_t lvl[4];
    uint32_t i;

    console_printf("\n");
    console_printf("Test 9b - the add-on board's filter bank\n");
    console_printf("========================================\n");
    console_printf("  First write to the 74HC595, so this is also the\n");
    console_printf("  first time the board is in a state we CHOSE.\n\n");
    console_printf("  The generator is at %u Hz, so only the 4.5 MHz\n",
                   GEN_HZ);
    console_printf("  highpass should pass it. The other three must put\n");
    console_printf("  the carrier into the noise - three rejections out\n");
    console_printf("  of four is a result that cannot happen by accident,\n");
    console_printf("  because wiring that does nothing leaves all four\n");
    console_printf("  readings the same.\n\n");

    /*
     * AUTORANGE ONCE, ON THE FILTER THAT PASSES, THEN HOLD.
     *
     * A rejection figure measured against a CLIPPED reference is not a
     * rejection figure: flat-topping compresses the loud row and makes
     * the bank look worse than it is. So the gain is set from the
     * passing filter - the loudest case - and then frozen, which is
     * the only arrangement where all four rows share a scale and none
     * of them clips.
     */
    addon_set_filter(addon_filter_for(GEN_HZ));
    board_delay_ms(20);
    {
        uint8_t gr = rf_autorange();

        console_printf("  gain fixed at %u dB, chosen on the filter that\n"
                       "  passes so that the loudest row does not clip; "
                       "595 0x%02x\n\n",
                       (uint32_t)gr, (uint32_t)addon_595_shadow());
    }

    for (i = 0; i < 4U; i++) {
        board_watchdog_feed();
        addon_set_filter(k_order[i]);
        board_delay_ms(20);

        console_printf("  0x%02x %s | ",
                       (uint32_t)addon_595_shadow(),
                       addon_filter_name(k_order[i]));
        lvl[i] = addon_report_tail();
    }

    /*
     * The verdict, and it is a comparison and not a threshold: the
     * highpass against the WORST of the other three. An absolute level
     * would depend on the generator, the attenuator and the gain; the
     * ratio between two filters measured a second apart depends on
     * neither.
     */
    {
        int32_t worst = lvl[1];
        uint32_t k;

        for (k = 2U; k < 4U; k++) {
            if (lvl[k] > worst) { worst = lvl[k]; }
        }

        console_printf("\n  highpass %d dB, best of the other three "
                       "%d dB, rejection %d dB\n",
                       lvl[0], worst, lvl[0] - worst);

        if (lvl[0] - worst >= 20) {
            console_printf("  -> the bank switches, and the 595 is ours\n");
        } else if (lvl[0] - worst >= 6) {
            console_printf("  -> it switches, but by less than expected "
                           "of a filter bank\n");
        } else {
            console_printf("  -> *** THE FILTERS ARE NOT SWITCHING ***\n");
            console_printf("     all four alike means the 595, its wiring "
                           "or Vext_ON.\n");
        }
    }

    /* Leave it where the bench needs it, chosen by frequency rather
     * than written out, so this line stays right if the generator
     * moves. */
    addon_set_filter(addon_filter_for(GEN_HZ));
    console_printf("  left on %s for %u Hz\n\n",
                   addon_filter_name(addon_filter()), GEN_HZ);
}

/* ------------------------------------------------------------------ */
/* Step 10: the SAI on DMA                                            */
/* ------------------------------------------------------------------ */

/*
 * The transmit half, in stereo frames. 1024 at 192 kHz is 5.3 ms, which
 * is the output latency this buys and is also the deadline a producer
 * has to meet. s_tone is exactly this long, which is not a coincidence:
 * the tone buffer is the test's half.
 */
#define TX_HALF_FRAMES  1024U

/*
 * THE SIZE THE RADIO ITSELF RUNS AT, which is not the one above.
 *
 * The tone tests keep 1024 because s_tone is exactly that long. The
 * display loop arms with EQUAL halves instead, receive and transmit,
 * because that is what lets the block interrupt hand a demodulator one
 * receive half and the transmit half to put the result in - no staging
 * buffer, no resynchronisation, and the arithmetic of "one block in,
 * one block out". See sai1.h's block hook.
 *
 * It costs output latency: 8192 frames is 42.7 ms rather than 5.3. On a
 * receiver that is the delay of a metre of air.
 */
#define AUDIO_HALF_FRAMES  CAP_FRAMES

/*
 * WHAT AN ARM ACTUALLY DID, printed after every single one of them.
 *
 * An arm can fail silently and this one did: the transport came up on
 * three arms and delivered digital silence on the fourth, with rc 0, no
 * error flag, a plausible frame rate and a ring full of the zeros
 * sai1_dma_start() had just written into it. Everything derived from the
 * samples - ADC usage, floor, peak - was consistent with a muted codec,
 * which is where the search would have gone.
 *
 * So print the hardware instead of a verdict, at every arm, and let the
 * one that differs identify itself. Four registers and a position that
 * moves: EN in each stream's CR, NDTR counting down, SAIEN in each
 * block's CR1, and the stream's event flags - which are the ones that
 * stop EN from taking.
 */
static void dma_arm_report(const char *where, int rc)
{
    uint32_t p0 = sai1_rx_pos();

    board_delay_ms(20);        /* 7680 half-words at 192 kHz */

    uint32_t p1 = sai1_rx_pos();

    console_printf("  arm [%s] rc %d | rx CR 0x%08x NDTR %5u | "
                   "tx CR 0x%08x NDTR %5u\n",
                   where, rc, sai1_rx_cr(), sai1_rx_ndtr(),
                   sai1_tx_cr(), sai1_tx_ndtr());
    console_printf("            A CR1 0x%08x B CR1 0x%08x LISR 0x%08x | "
                   "rx pos %5u -> %5u %s\n",
                   SAI1_Block_A->CR1, SAI1_Block_B->CR1, sai1_dma_lisr(),
                   p0, p1,
                   (p1 != p0) ? "moving" : "*** NOT MOVING ***");

    /*
     * SAIEN bit 16 in each CR1 above, and how many extra writes it took
     * to get there. A zero here with the transport working would mean
     * the readback loop is decoration; it is not.
     */
    /*
     * SAIEN now, and the two counters. They say different things and
     * both are needed: retries counts writes the hardware would not
     * accept at all - always zero so far - and relatches counts writes
     * it accepted and then threw away, which is the real failure.
     *
     * The relatch count is for the WHOLE PERIPHERAL, not just the arms:
     * sai1_start() needs the same verification and now does it, so this
     * number includes every polled start as well. It is routinely in the
     * tens by the end of a run, which is the measure of how far from
     * decoration the check is.
     */
    console_printf("            SAIEN A %u B %u | retries %u relatches %u\n",
                   (SAI1_Block_A->CR1 >> 16) & 1U,
                   (SAI1_Block_B->CR1 >> 16) & 1U,
                   sai1_en_retries(), sai1_en_relatch());

    /*
     * AND THE SAME REGISTERS 100 us AFTER THE ARM, taken inside the
     * driver before anything else ran. The difference between this and
     * the line above is the whole diagnosis: if SAIEN was already clear
     * here, the hardware dropped it within nineteen frames, and SR says
     * what it objected to.
     */
    const sai1_arm_t *a = sai1_arm_snapshot();

    console_printf("            at +100us: A CR1 0x%08x SR 0x%08x | "
                   "B CR1 0x%08x SR 0x%08x\n",
                   a->cr1_a, a->sr_a, a->cr1_b, a->sr_b);
    console_printf("                       SAIEN A %u B %u | rx EN %u "
                   "NDTR %5u | tx EN %u NDTR %5u\n",
                   (a->cr1_a >> 16) & 1U, (a->cr1_b >> 16) & 1U,
                   a->rx_cr & 1U, a->rx_ndtr, a->tx_cr & 1U, a->tx_ndtr);
}

/*
 * The DMA's three error flags by name, because a count of "errors" sends
 * you looking in three places and only one of them is ever the problem.
 * Printed for both streams; a quiet transport prints nothing but zeros.
 */
static void dma_err_report(void)
{
    uint32_t b = sai1_dma_err_bits();

    console_printf("  dma errors %u, flags:", sai1_dma_errors());

    if (b == 0U) {
        console_printf(" none\n");
        return;
    }

    for (uint32_t s = 0; s < 2U; s++) {
        uint32_t g = (b >> (s * 8U)) & 0x3FU;

        if (g == 0U) { continue; }

        console_printf("  %s:", (s == 0U) ? " rx" : " tx");
        if (g & (1UL << 0)) { console_printf(" FEIF"); }
        if (g & (1UL << 2)) { console_printf(" DMEIF"); }
        if (g & (1UL << 3)) { console_printf(" TEIF"); }
    }
    console_printf("\n");
}

/*
 * THE LISTENING TEST, AND IT IS A SEPARATE FUNCTION FOR A HUMAN REASON.
 *
 * test_audio_gap() is the record: 24 console lines, 24 underruns polled
 * and 0 on DMA, every run. Its two phases are 0.67 s each and they
 * happen about 34 seconds into a run, which is a fine place for a
 * measurement and a useless one for an ear. **It was missed the first
 * time it was offered** - the window opened at 33.9 s, closed at 45.4 s,
 * and whoever was meant to hear it had stepped away. A check that needs
 * a person present needs to arrive when the person is.
 *
 * So this runs AS SOON AS THE CODEC CAN MAKE A SOUND - right after
 * nau8822_configure(), a few seconds after the load - and it is long
 * enough that being distracted for ten seconds does not cost the run.
 *
 * AND IT OPENS WITH A MARKER, which is the part that makes the answer
 * usable. "I heard no clicks" has two completely different causes - the
 * clicks are gone, or nothing was audible at all - and one steady tone
 * before the comparison separates them. Without it, the most important
 * report this test can produce is ambiguous, and an ambiguous report
 * from a human is worse than no report, because it looks like data.
 *
 * It leaves the SAI polled and running, which is what every test after
 * it expects.
 */
static void test_audio_listen(void)
{
    /* Six cycles while the question was open and the answer had to be
     * judged by ear; two now that it has been, which is enough to hear
     * that the click has not come back. The counters below do the rest
     * every run. */
    const uint32_t cycles = 2U;
    const uint32_t lines  = 12U;

    console_printf("\nListening test - IS THE CLICK GONE? (needs an ear)\n");
    console_printf("================================================\n");
    console_printf("  Starting at %u ms after reset, about %u s long.\n",
                   board_millis(), (cycles * 2U * lines * 105U) / 1000U + 2U);
    console_printf("  The transmit FIFO is 21 us at 192 kHz and a console\n");
    console_printf("  line is 9000 us, so on the polled transport every\n");
    console_printf("  printed line is a gap in the audio. On DMA it is not.\n\n");

    ui_prompt("LISTEN",
              "A tone, about half a minute.",
              "It should switch between clean",
              "and clicking, and back.");

    tone_fill(s_tone, TX_HALF_FRAMES, 1000U, 192000U);

    /*
     * UNMUTE, AND THIS LINE IS THE WHOLE REASON THE FIRST TWO LISTENING
     * SESSIONS HEARD NOTHING AT ALL.
     *
     * nau8822_configure() ends with nau8822_mute(1) on purpose - drivers
     * enabled, outputs quiet, so that a bring-up image cannot come up
     * shouting a full-scale tone into a small speaker. It is a good
     * default and it is still there. What was missing is that NOTHING IN
     * THIS IMAGE EVER UNMUTED: step 4's tone test did it, that test was
     * retired when the rate question was answered, and the unmute left
     * with it. The codec has been muted since 02/11/2026.
     *
     * So the speaker was silent for the two sessions above, and the
     * report "no sound at all" was exactly right. Worth keeping in mind
     * how this looked from the other side: 48 transmit underruns in the
     * same run PROVE the digital path was alive - a transmitter only
     * starves if it is clocking frames - so the counters and the ear
     * were both telling the truth about different halves of the chain,
     * and the contradiction was ours for not having looked at the mute
     * bit.
     *
     * The test owns the unmute, and re-mutes at the end. A listening
     * test is the one caller that is allowed to make a noise.
     */
    nau8822_set_speaker_db(-18);
    nau8822_set_headphone_db(-18);
    nau8822_mute(0);
    console_printf("  speaker and headphones unmuted at -18 dB for this\n");
    console_printf("  test only - nau8822_configure() leaves them MUTED,\n");
    console_printf("  and nothing else in this image unmutes them.\n\n");

    /*
     * THE MARKER FIRST: two seconds of steady tone with nothing else
     * going on, so that "I heard nothing" and "I heard no clicks" cannot
     * be confused for each other.
     */
    console_printf("  (1) a steady 1 kHz tone for 2 s - if THIS is not\n");
    console_printf("      audible, the rest of this test says nothing\n");

    for (uint32_t i = 0; i < 21U; i++) {
        board_watchdog_feed();
        sai1_play(s_tone, TX_HALF_FRAMES, TX_HALF_FRAMES * 18U);
    }

    console_printf("  (2) now %u x (1.2 s polled, 1.2 s DMA): the polled\n",
                   cycles);
    console_printf("      half clicks once per line below, the DMA half\n");
    console_printf("      should not. Clicking, clean, clicking, clean.\n\n");

    /*
     * COUNTED SEPARATELY, AND IT HAS TO BE: sai1_play() resets
     * sai1_tx_underruns() at the top of every call - it is the polled
     * path's per-call figure - while on DMA the same counter accumulates
     * across calls. So the polled total is summed call by call and the
     * DMA total is a difference taken around each DMA phase. One reading
     * at the end would have mixed the two conventions and reported
     * something that is neither.
     */
    uint32_t pol_under = 0;
    uint32_t dma_under = 0;

    for (uint32_t cycle = 0; cycle < cycles; cycle++) {
        for (uint32_t i = 0; i < lines; i++) {
            board_watchdog_feed();
            sai1_play(s_tone, TX_HALF_FRAMES, TX_HALF_FRAMES * 18U);
            pol_under += sai1_tx_underruns();
            console_printf("    cycle %u POLLED %2u/%u - a click on this "
                           "line\n", cycle + 1U, i + 1U, lines);
        }

        if (sai1_dma_start(CAP_FRAMES, TX_HALF_FRAMES) != SAI1_OK) {
            console_printf("    *** could not arm the DMA, stopping ***\n");
            break;
        }

        uint32_t before = sai1_tx_underruns();

        for (uint32_t i = 0; i < lines; i++) {
            board_watchdog_feed();

            uint64_t until = board_cycles()
                           + ((uint64_t)clock_sysclk_hz * 100U) / 1000U;

            while (board_cycles() < until) {
                if (sai1_tx_due()) {
                    (void)sai1_tx_write_half(s_tone, TX_HALF_FRAMES);
                }
            }

            console_printf("    cycle %u DMA    %2u/%u - this line costs "
                           "nothing\n", cycle + 1U, i + 1U, lines);
        }

        dma_under += sai1_tx_underruns() - before;
    }

    console_printf("\n  over at %u ms. underruns: polled %u, DMA %u - and\n",
                   board_millis(), pol_under, dma_under);
    console_printf("  %u is exactly one per printed line.\n", pol_under);

    /*
     * BACK TO MUTED, which is the state the rest of the image is written
     * for and the only safe default while the audio path has no volume
     * control and no user in charge of it.
     */
    nau8822_mute(1);

    /*
     * Hand the SAI back to the polled path, running, which is the state
     * every test after this one is written for. Stopping the transport
     * also clears the alignment flag, so the next capture re-establishes
     * the slot parity itself.
     */
    sai1_dma_stop();
    sai1_start();
    console_printf("  SAI handed back to the polled path: SAIEN A %u B %u\n\n",
                   (SAI1_Block_A->CR1 >> 16) & 1U,
                   (SAI1_Block_B->CR1 >> 16) & 1U);
}

/* ------------------------------------------------------------------ */
/* Step 11: the rotary encoders                                       */
/* ------------------------------------------------------------------ */

/*
 * TEST 12b: THE A/B LINES, WITH PULLS THIS FIRMWARE WROTE ITSELF.
 *
 * The one thing not yet checked, and it is the oldest rule in
 * WORKING.md: every reading so far has been taken through pull-ups that
 * the FACTORY FIRMWARE configured, inherited and trusted. "Whatever the
 * previous firmware left is fine" is exactly the sentence that rule
 * exists to forbid, and this is the first measurement in the encoder
 * hunt that does not depend on it.
 *
 * The schematic (page 3 of Malahit_schematics_15012020.pdf) settles what
 * the answer should be: each encoder's common terminal C goes to GROUND,
 * A and B carry nothing but a 1 nF cap to ground, and there is no
 * external pull anywhere. So an open contact must float - and a floating
 * line follows whatever pull the MCU applies.
 *
 *   both pulls read the same  -> the line is tied, and to which rail the
 *                                value says
 *   follows the pull          -> floating, i.e. the contact is open and
 *                                the wiring is fine
 *
 * Nothing here drives a pin. A line that might be shorted to ground is
 * the one thing a push-pull output must not be pointed at, and the
 * question can be answered without it.
 */
/*
 * ======================================================================
 * Test 13 - the ADC, and the two buttons on one analog line
 * ======================================================================
 *
 * THIS TEST EXISTS TO IDENTIFY A PIN, which is not something firmware
 * usually has to do. No STM32 datasheet is in docs/, and the rule in this
 * project is that a register map or a pin map gets quoted rather than
 * remembered - several of the expensive mistakes here came from
 * recalling one. The H743 gives its four dual pads channels of their own
 * and the ordinary pads other ones, so PA1_C is channel 1 while a plain
 * PA1 would be channel 17, and a confident wrong answer reads as a flat
 * number that looks like a working ADC.
 *
 * So every channel is read and printed, and then a hand settles it:
 * ENC_BUT is the channel that sits at full scale and DROPS when a button
 * is held, and the battery is the one that sits near half the rail and
 * stays there. Two readings, no recollection.
 *
 * It also prints what the classifier makes of each plateau, so one run
 * answers all three questions: which channel, what voltages, and whether
 * the thresholds taken from the schematic's resistor values are where
 * the real ladder actually lands.
 */
/*
 * One line of a filter's measured response. A callback so am.c needs no
 * console of its own, and dB printed as tenths because the interesting
 * part of a filter's skirt happens inside a decibel.
 */
static void am_filter_line(const char *name, float hz, float db)
{
    int32_t d10 = (int32_t)(db * 10.0f);

    console_printf("    %-8s %6u Hz  %+d.%u dB\n", name, (uint32_t)hz,
                   d10 / 10, (uint32_t)((d10 < 0 ? -d10 : d10) % 10));
}

/* What adc_init() returned, so main() can own the startup and the test
 * can only report it - see inside test_adc(). */
static int s_adc_rc  = ADC_ERR_RDY;
static int s_adc3_rc = ADC_ERR_RDY;

/* An attempt that never ran must not print as one that succeeded, which
 * is what a column of zeroes did on the first run. */
static const char *adc_rc_txt(int rc)
{
    if (rc == ADC_NOT_TRIED) { return "-"; }
    if (rc == ADC_OK)        { return "OK"; }
    if (rc == ADC_ERR_LDO)   { return "LDO"; }
    if (rc == ADC_ERR_CAL)   { return "CAL"; }
    if (rc == ADC_ERR_RDY)   { return "NOCLK"; }
    return "BUSY";
}

static void test_adc(int interactive)
{
    static const char *const k_class[4] = {
        "idle", "ENC1", "ENC2", "ENC1+ENC2"
    };

    console_printf("\nTest 13 - the ADC and the button ladder (needs a "
                   "hand)\n");
    console_printf("=======================================================\n");

    /*
     * adc_init() IS NOT CALLED HERE, and that is trap 17 applied before
     * it bites a third time. The tone test owned the codec's unmute and
     * took it away when it was retired, which cost three days of silence;
     * the encoder's setup was moved out of its test for the same reason.
     * So main() owns the ADC's startup and this test only reports it.
     * Retiring this function must not be able to switch the buttons off.
     */
    int rc = s_adc_rc;

    console_printf("  adc_init         : %d (%s)\n", rc,
                   (rc == ADC_OK) ? "ok" : "FAILED");
    console_printf("  ADC1->CR found at: 0x%08x  (ADEN %u, ADSTART %u, "
                   "DEEPPWD %u)\n",
                   adc_found_cr,
                   (adc_found_cr & ADC_CR_ADEN) ? 1U : 0U,
                   (adc_found_cr & ADC_CR_ADSTART) ? 1U : 0U,
                   (adc_found_cr & ADC_CR_DEEPPWD) ? 1U : 0U);
    console_printf("  SYSCFG->PMCR     : 0x%08x  (PA0SO %u, PA1SO %u - 1 = "
                   "switch OPEN,\n", adc_found_pmcr,
                   (adc_found_pmcr & (1UL << 24)) ? 1U : 0U,
                   (adc_found_pmcr & (1UL << 25)) ? 1U : 0U);
    console_printf("                     which is what an ADC input "
                   "wants)\n");
    console_printf("  kernel clock     : %u Hz   (CCR 0x%08x)\n",
                   adc_kernel_hz(), adc_ccr);
    console_printf("  calibration      : 0x%08x in %u ms\n",
                   adc_cal_factor, adc_cal_ms);
    console_printf("  CR after ADVREGEN: 0x%08x\n", adc_cr_after_vreg);
    console_printf("  CR after ADCAL   : 0x%08x\n", adc_cr_after_cal);
    console_printf("  ISR after ADEN   : 0x%08x  (ADRDY %u - and ADRDY is "
                   "also the\n", adc_isr_after_aden,
                   (adc_isr_after_aden & ADC_ISR_ADRDY) ? 1U : 0U);
    console_printf("                     proof that the kernel clock is "
                   "reaching it)\n");
    console_printf("  attempts         : per_ck+cal %s, per_ck %s, HSE %s"
                   "  -> used #%u\n",
                   adc_rc_txt(adc_attempt_rc[0]),
                   adc_rc_txt(adc_attempt_rc[1]),
                   adc_rc_txt(adc_attempt_rc[2]),
                   adc_attempt_used);

    if (rc != ADC_OK) {
        console_printf("\n  No conversions, so nothing below would mean "
                       "anything. The\n");
        console_printf("  return codes are in adc.h: -1 the internal "
                       "regulator, -2 a\n");
        console_printf("  calibration that never finished, -3 ADRDY "
                       "missing, which is\n");
        console_printf("  the clock. The three attempt codes above say "
                       "which of the\n");
        console_printf("  three it is without needing another run.\n\n");
        return;
    }

    /*
     * THE SCAN. Nothing here configures a GPIO: selecting a channel does
     * not change the pin's mode, and the converter's input is high
     * impedance, so reading a channel whose pad something else is
     * driving costs that pin nothing and tells us what is on it.
     */
    console_printf("\n  Every channel, once. Full scale is 65535 = VREF+ = "
                   "the 3.3 V\n");
    console_printf("  rail, so the mV column is NOMINAL - it inherits the "
                   "LD3985's\n");
    console_printf("  tolerance. The buttons do not use it; see adc.c on "
                   "why\n");
    console_printf("  thresholds are fractions and not volts.\n\n");
    console_printf("    ch | counts |   mV | expected\n");
    console_printf("    ---+--------+------+---------------------------\n");

    /* The polled read and the SysTick sampler must not share the
     * converter - whoever reads DR second gets the other one's channel -
     * so the sampler stands down for the scan and is put back below. */
    adc_sampling(0);

    for (uint32_t ch = 0; ch <= 19U; ch++) {
        int32_t v = adc_read(ch);
        const char *note = "";

        /*
         * ONLY TWO LABELS, AND THEY ARE THE TWO THE HAND CONFIRMS.
         *
         * The first version of this table also named channels 16 and 17
         * as the plain PA0/PA1 pads and called them unbonded, which was
         * a recollection of the H743 channel map dressed up as a
         * measurement - and the readings argued with it immediately:
         * PA1SO reads 0, which should CLOSE the switch and make channel
         * 17 the same electrical node as channel 1, and the two came
         * back 65535 and 8623. Either the switch is open regardless or
         * channel 17 is not that pad, and no datasheet here can settle
         * it. So the column says what the schematic says about the two
         * nets that matter, and everything else is printed bare.
         */
        if (ch == 0U)  { note = "ADC_IN  = battery through 10k/10k"; }
        if (ch == 1U)  { note = "ENC_BUT = the ladder (full scale = idle)"; }

        if (v < 0) {
            console_printf("    %2u |   FAIL | rc %d | %s\n",
                           ch, (int)v, note);
        } else {
            console_printf("    %2u | %6u | %4u | %s\n",
                           ch, (uint32_t)v, adc_mv((uint32_t)v), note);
        }
    }

    /*
     * PUT PCSEL BACK. The scan preselected all twenty channels and four
     * of them are pads this firmware is using for something else -
     * PC0/PC1 carry the encoder the driver is reading right now. The
     * preselection should not disturb a digital function and the
     * encoder's own test, which runs after this one, would say so if it
     * did; leaving twenty analog input paths connected on the strength
     * of "should not" is still the wrong way round, and narrowing it
     * back costs one register write.
     */
    ADC1->PCSEL = (1UL << ADC_CH_ENC_BUT) | (1UL << ADC_CH_BAT);

    /*
     * THE LIVE PART RUNS THROUGH THE REAL PATH - adc_tick() from
     * SysTick, the same code the radio will use - rather than through a
     * polled read written for the test. Trap 17: a test that proves
     * something with its own private setup proves it about the setup.
     *
     * And it waits for the hand. A minute of patience, and twenty-five
     * seconds once something actually moves, because three times in this
     * port an absence was recorded in a window nobody was standing in
     * and twice it was written down as a fact (trap 18).
     */
    adc_sampling(1);
    board_delay_ms(10);         /* two ticks: one per channel */

    if (!interactive) {
        /*
         * THE HALF THAT NEEDS A FINGER HAS ANSWERED. The four plateaus
         * are measured, written into adc.c's thresholds and into
         * BOARD.md section 6, and the button has been used in anger
         * since. Asking for four presses on every load would be a
         * minute of somebody's attention for a result already known -
         * and an unanswered prompt would then sit on the screen looking
         * like a hang.
         *
         * The scan above still runs every time, because it costs
         * nothing and it is what would notice the day a channel stops
         * reading what it used to. test_adc(1) brings the rest back.
         */
        console_printf("\n  The press measurement is retired - see "
                       "test_adc(). Call it\n");
        console_printf("  with 1 to take the ladder's four plateaus "
                       "again.\n\n");
        return;
    }

    ui_prompt("PRESS THE KNOB BUTTONS",
              "big knob first, then the small one,",
              "then both together.",
              "Hold each for about a second.");

    console_printf("\n  Now press the buttons ON TOP OF THE KNOBS - the "
                   "BIG knob's\n");
    console_printf("  first, then the SMALL one's, then both together. "
                   "Hold each\n");
    console_printf("  for about a second. Every change is printed with "
                   "what the\n");
    console_printf("  classifier makes of it.\n\n");

    /*
     * HELD FOR HOW LONG, in milliseconds, and not "how many times this
     * loop went round". The first version counted loop passes and
     * printed them under "samples", which is two lies in one column: the
     * loop is free-running so a pass is not a unit of anything, and
     * adc_conversions sits four lines below with the real count. A press
     * that flickered for two milliseconds and one held for two seconds
     * have to look different here, and in milliseconds they do.
     */
    uint32_t seen[4]  = { 0, 0, 0, 0 };     /* ms spent in each class */
    uint32_t last_ms  = board_millis();
    uint32_t vmin[4]  = { 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU };
    uint32_t vmax[4]  = { 0, 0, 0, 0 };
    uint32_t prev     = adc_raw_enc_but();
    uint32_t beat     = board_millis();
    int      started  = 0;
    uint32_t t_end    = board_millis() + 60000U;

    while ((int32_t)(board_millis() - t_end) < 0) {
        board_watchdog_feed();

        /*
         * BOTH HALVES OF ONE SAMPLE, OR NEITHER.
         *
         * The counts and the class live in two variables that SysTick
         * writes a millisecond apart from this loop, so reading them one
         * after the other can catch a tick in between and pair a fresh
         * voltage with a stale verdict. It shows up in the table as a
         * press whose counts run all the way up to the idle value -
         * which is what the first version printed, and it reads as
         * noise in the hardware rather than a torn read in the test.
         *
         * Re-reading the class and discarding the pair if it moved is
         * enough: the only pairing that can mislead is one where the
         * class changed, and that is exactly what this catches.
         */
        uint8_t  cls = adc_buttons();
        uint32_t raw = adc_raw_enc_but();

        if (cls != adc_buttons()) { continue; }

        uint32_t now_ms = board_millis();

        seen[cls & 3U] += now_ms - last_ms;
        last_ms = now_ms;

        if (raw < vmin[cls & 3U]) { vmin[cls & 3U] = raw; }
        if (raw > vmax[cls & 3U]) { vmax[cls & 3U] = raw; }

        uint32_t d = (raw > prev) ? (raw - prev) : (prev - raw);

        if (d > 1500U) {
            prev = raw;

            if (!started) {
                started = 1;
                t_end   = board_millis() + 25000U;
                console_printf("    (something moved - twenty-five seconds "
                               "from here)\n");
            }

            console_printf("    %6u counts  %4u mV  -> %s\n",
                           raw, adc_mv(raw), k_class[cls & 3U]);
        }

        if ((int32_t)(board_millis() - beat) >= 0) {
            beat = board_millis() + 2000U;
            /*
             * THE COUNTERS GO HERE, NOT ONLY IN THE SUMMARY. A sampler
             * that is not storing anything looks exactly like a line
             * sitting at zero, and the summary that would have
             * distinguished them prints after a minute of waiting for a
             * hand - which is a minute spent before learning the test
             * could not have worked.
             */
            console_printf("    .. ENC_BUT %6u (%4u mV, %s)   battery pin "
                           "%6u   conv %u  timeouts %u\n",
                           raw, adc_mv(raw), k_class[cls & 3U],
                           adc_raw_bat(), adc_conversions, adc_timeouts);
            console_printf("       ticks %u live %u  ADC1->CR 0x%08x\n",
                           adc_ticks, adc_ticks_live, ADC1->CR);
        }
    }

    console_printf("\n  What the ladder actually measured:\n\n");
    console_printf("    class      | held ms | counts seen     | mV      | "
                   "measured before\n");
    console_printf("    -----------+---------+-----------------+---------+"
                   "----------------\n");

    /* The column to compare against is the previous MEASUREMENT - the
     * 20/11/2026 one, with PMCR written, which also agrees with the
     * schematic's 47k/68k/22k to half a per cent. The 05/11 numbers
     * that used to be here were taken through a closed analog switch
     * (WORKING.md trap 23). See adc.c. */
    static const uint32_t k_nom[4] = { 65535U, 38924U, 20855U, 17091U };

    for (uint32_t i = 0; i < 4U; i++) {
        if (seen[i] == 0U) {
            console_printf("    %-10s |       0 | NEVER SEEN      |         "
                           "| %5u\n", k_class[i], k_nom[i]);
        } else {
            console_printf("    %-10s | %7u | %6u..%-6u | %4u-%-4u | %5u\n",
                           k_class[i], seen[i], vmin[i], vmax[i],
                           adc_mv(vmin[i]), adc_mv(vmax[i]), k_nom[i]);
        }
    }

    console_printf("\n  A class NEVER SEEN is either a button that was not "
                   "pressed or a\n");
    console_printf("  threshold in the wrong place, and the counts column "
                   "says which.\n");
    console_printf("  'ENC1' is the BIG knob here, which the hand settled: "
                   "big first,\n");
    console_printf("  small second, both third, and the plateaus arrived "
                   "in that\n");
    console_printf("  order. With PMCR written they match the schematic's "
                   "47k/68k/22k\n");
    console_printf("  to half a per cent - see adc.c, including the check "
                   "that needs\n");
    console_printf("  no resistor value at all.\n");
    console_printf("  adc_conversions %u, adc_timeouts %u - the second one "
                   "must be 0.\n\n",
                   adc_conversions, adc_timeouts);
}

/*
 * ======================================================================
 * Test 13b - which pad carries which net, against a multimeter
 * ======================================================================
 *
 * THE BATTERY DID NOT ADD UP, and this is the test that settles why.
 *
 * The schematic is unambiguous - page 3, R34 and R36 both 10k, ADC_IN
 * their midpoint, BAT below and ground above - so the pin must read
 * exactly half the cell. With a meter on the cell saying 4.15 V the pin
 * owes 2.075 V, and the channel this driver was reading said 1.53 V.
 * That cannot be fixed by doubting the reference: it would take a VREF+
 * of 4.47 V to explain, and VREF+ cannot exceed the 3.3 V rail.
 *
 * So the suspect is WHICH PAD. The H743 gives PA0 and PA1 each a second
 * analog-only pad, PA0_C and PA1_C, with their own ADC channels and an
 * analog switch between the two; which one a package bonds, and which
 * channel number goes with it, is a datasheet question and there is no
 * datasheet in docs/. The scan's channel 16 read 2.086 V - half a per
 * cent off what the meter demands - while channel 0 did not.
 *
 * Four candidates, read one at a time with PCSEL set to that channel
 * ALONE, then watched while a finger goes on the button. Whichever
 * channel sits at half the cell is ADC_IN; whichever follows the button
 * is ENC_BUT. Neither answer comes from remembering a pin table.
 */
static void test_adc_pins(void)
{
    static const uint32_t k_ch[4] = { 0U, 1U, 16U, 17U };

    console_printf("\nTest 13b - which pad is which, against the meter\n");
    console_printf("===============================================\n");

    if (s_adc_rc != ADC_OK) {
        console_printf("  The ADC did not start, so there is nothing to "
                       "read.\n\n");
        return;
    }

    adc_sampling(0);

    console_printf("  ADC_IN is the midpoint of two 10k resistors across "
                   "the cell\n");
    console_printf("  (R34/R36, schematic page 3), so it owes exactly half "
                   "of it.\n");
    console_printf("  At 4.15 V measured on the cell that is 2075 mV, or "
                   "41220\n");
    console_printf("  counts against a 3.3 V reference.\n\n");
    console_printf("    ch | counts (8 reads)              | mV   | x2 = "
                   "cell\n");
    console_printf("    ---+-------------------------------+------+--------"
                   "---\n");

    for (uint32_t i = 0; i < 4U; i++) {
        uint32_t ch   = k_ch[i];
        uint32_t sum  = 0U;
        uint32_t n    = 0U;
        uint32_t lo   = 0xFFFFFFFFU;
        uint32_t hi   = 0U;

        for (uint32_t k = 0; k < 8U; k++) {
            int32_t v = adc_read(ch);

            if (v < 0) { continue; }
            sum += (uint32_t)v;
            n++;
            if ((uint32_t)v < lo) { lo = (uint32_t)v; }
            if ((uint32_t)v > hi) { hi = (uint32_t)v; }
        }

        if (n == 0U) {
            console_printf("    %2u | no conversion completed\n", ch);
            continue;
        }

        uint32_t mean = sum / n;

        console_printf("    %2u | mean %5u  spread %5u..%-5u | %4u | %4u "
                       "mV\n",
                       ch, mean, lo, hi, adc_mv(mean), adc_mv(mean) * 2U);
    }

    /*
     * And now the hand, because the other half of the question cannot be
     * answered by a steady voltage. Both pads see the ladder if the
     * analog switch between them is closed, so the only way to know
     * which channels are connected to it is to watch all four while a
     * button goes down.
     */
    ui_prompt("HOLD THE BIG KNOB'S BUTTON",
              "down for a few seconds, so the",
              "channels wired to it can be told",
              "from the ones that are not.");

    console_printf("\n  HOLD THE BIG KNOB'S BUTTON DOWN for a few seconds. "
                   "All four\n");
    console_printf("  channels are printed twice a second; the ones that "
                   "move are\n");
    console_printf("  wired to ENC_BUT, the one that does not move and "
                   "sits at half\n");
    console_printf("  the cell is ADC_IN.\n\n");

    uint32_t t_end   = board_millis() + 45000U;
    uint32_t beat    = board_millis();
    uint32_t base[4] = { 0, 0, 0, 0 };
    uint32_t moved[4]= { 0, 0, 0, 0 };
    int      started = 0;

    for (uint32_t i = 0; i < 4U; i++) {
        int32_t v = adc_read(k_ch[i]);
        base[i] = (v < 0) ? 0U : (uint32_t)v;
    }

    while ((int32_t)(board_millis() - t_end) < 0) {
        board_watchdog_feed();

        if ((int32_t)(board_millis() - beat) < 0) { continue; }
        beat = board_millis() + 500U;

        uint32_t now[4];

        for (uint32_t i = 0; i < 4U; i++) {
            int32_t v = adc_read(k_ch[i]);
            now[i] = (v < 0) ? 0U : (uint32_t)v;

            uint32_t d = (now[i] > base[i]) ? (now[i] - base[i])
                                            : (base[i] - now[i]);
            if (d > 3000U) { moved[i]++; }
        }

        if (!started) {
            started = 1;
            /* Forty-five seconds of patience, fifteen once a finger has
             * arrived - the same shape as every other test here, and for
             * the same reason: a window nobody was standing in proves
             * nothing. */
        }

        console_printf("    ch0 %5u  ch1 %5u  ch16 %5u  ch17 %5u\n",
                       now[0], now[1], now[2], now[3]);

        if (moved[0] + moved[1] + moved[2] + moved[3] >= 6U) {
            t_end = board_millis() + 4000U;
        }
    }

    console_printf("\n    channel | readings that moved by >3000 counts\n");
    for (uint32_t i = 0; i < 4U; i++) {
        console_printf("    ch %2u   | %u\n", k_ch[i], moved[i]);
    }
    console_printf("\n  A channel that moved is on ENC_BUT. A channel that "
                   "did not and\n");
    console_printf("  reads near 41220 is ADC_IN. A channel that did "
                   "neither is a pad\n");
    console_printf("  this package does not bond, reading whatever leaks "
                   "into it.\n\n");

    adc_sampling(1);
}

/*
 * ======================================================================
 * Test 13c - the internal reference, and which channel it is on
 * ======================================================================
 *
 * The rail was a constant taken from one multimeter reading. This is
 * what replaces it: a bandgap inside the part that does NOT follow the
 * supply, so converting it against the supply measures the supply.
 *
 * Two things have to be established and neither is in any document in
 * this tree. WHICH CHANNEL the bandgap is on - found by switching it off
 * and on and seeing which channel moves. And WHERE ST'S FACTORY
 * CALIBRATION LIVES - not trusted on sight, but range-checked, and the
 * range is narrow enough to be an identification in itself: a 16-bit
 * conversion of a 1.18-1.24 V reference against 3.3 V can only land
 * between about 23 400 and 24 700 counts.
 *
 * And there is an outside number to check the answer against, which is
 * the thing that caught the fictional battery reading: the meter said
 * the cell was at 4.15 V, which puts VREF+ at 3 280 mV. Two independent
 * routes to the same rail, printed side by side.
 */
static void test_vrefint(void)
{
    console_printf("\nTest 13c - VREFINT on ADC3, and the rail it "
                   "measures\n");
    console_printf("=========================================="
                   "============\n");

    console_printf("  adc3_init        : %d (%s)\n", s_adc3_rc,
                   (s_adc3_rc == ADC_OK) ? "ok" : "FAILED");

    if (s_adc3_rc != ADC_OK) {
        console_printf("  ADC3 did not start, so the rail stays at the "
                       "constant\n");
        console_printf("  measured against the meter. adc.h has the "
                       "codes.\n\n");
        return;
    }

    /*
     * ST's calibration word, and the two neighbours it is usually
     * printed with - the temperature sensor's two points. They are shown
     * together because seeing all three at once is what makes the
     * plausibility test convincing: a wrong base address does not
     * produce three sensible-looking numbers in a row.
     */
    uint32_t cal = adc_vrefin_cal();

    console_printf("\n  System memory, where the factory calibration "
                   "should be:\n");
    console_printf("    0x1FF1E820 (TS_CAL1?)  %5u\n",
                   (uint32_t)*(volatile const uint16_t *)0x1FF1E820UL);
    console_printf("    0x1FF1E840 (TS_CAL2?)  %5u\n",
                   (uint32_t)*(volatile const uint16_t *)0x1FF1E840UL);
    console_printf("    0x1FF1E860 (VREFIN_CAL)%5u  <- %s\n", cal,
                   (cal >= 23400U && cal <= 24700U)
                       ? "in range for a 1.2 V bandgap at 3.3 V"
                       : "NOT a plausible calibration - nominal used");

    /*
     * THE CHANNEL, by difference. Everything is read with the bandgap
     * off, then everything again with it on. A channel that is wired to
     * nothing reads roughly the same both times; the one the reference
     * is on jumps to a quarter of full scale.
     */
    adc_sampling(0);

    /*
     * THREE SWEEPS, NOT TWO, AND EACH ENABLE TOGGLED SEPARATELY.
     *
     * The first version switched only VREFEN and then called whatever
     * moved second "the thermometer". That is not a measurement: TSEN
     * was on throughout, so the sensor's channel read the same in both
     * sweeps, and the runner-up was simply the channel that wandered
     * most - an unconnected pad, which drifts by hundreds of counts
     * between two readings. It named channel 16 and printed a confident
     * warning that the build was using the wrong one.
     *
     * A difference test identifies only what it actually switched. So:
     * everything off, then one bit on, then the other.
     */
    uint32_t off[20], on[20], ton[20];

    ADC3_COMMON->CCR &= ~(ADC_CCR_VREFEN | ADC_CCR_TSEN);
    board_delay_ms(5);
    for (uint32_t ch = 0; ch < 20U; ch++) {
        int32_t v = adc3_read(ch);
        off[ch] = (v < 0) ? 0U : (uint32_t)v;
    }

    ADC3_COMMON->CCR |= ADC_CCR_VREFEN;
    board_delay_ms(5);
    for (uint32_t ch = 0; ch < 20U; ch++) {
        int32_t v = adc3_read(ch);
        on[ch] = (v < 0) ? 0U : (uint32_t)v;
    }

    ADC3_COMMON->CCR &= ~ADC_CCR_VREFEN;
    ADC3_COMMON->CCR |= ADC_CCR_TSEN;
    board_delay_ms(5);
    for (uint32_t ch = 0; ch < 20U; ch++) {
        int32_t v = adc3_read(ch);
        ton[ch] = (v < 0) ? 0U : (uint32_t)v;
    }

    /* Both back on, which is how the driver runs. */
    ADC3_COMMON->CCR |= ADC_CCR_VREFEN | ADC_CCR_TSEN;
    board_delay_ms(5);

    console_printf("\n    ch | both off | +bandgap | +sensor | moved by\n");
    console_printf("    ---+----------+----------+---------+----------\n");

    uint32_t best = 0U, best_d = 0U;         /* the reference */
    uint32_t t_best = 0U, t_best_d = 0U;     /* the thermometer */

    for (uint32_t ch = 0; ch < 20U; ch++) {
        uint32_t dv = (on[ch]  > off[ch]) ? (on[ch]  - off[ch])
                                          : (off[ch] - on[ch]);
        uint32_t dt = (ton[ch] > off[ch]) ? (ton[ch] - off[ch])
                                          : (off[ch] - ton[ch]);

        if (dv > best_d)   { best_d   = dv; best   = ch; }
        if (dt > t_best_d) { t_best_d = dt; t_best = ch; }

        console_printf("    %2u | %8u | %8u | %7u | %u / %u\n",
                       ch, off[ch], on[ch], ton[ch], dv, dt);
    }

    console_printf("\n  Bandgap: channel %u moved %u counts when VREFEN "
                   "went on.\n", best, best_d);
    console_printf("  Sensor : channel %u moved %u counts when TSEN went "
                   "on.\n", t_best, t_best_d);

    if (best != ADC3_CH_VREFINT) {
        console_printf("  ADC3_CH_VREFINT in adc.h is %u and wants to be "
                       "%u.\n", (uint32_t)ADC3_CH_VREFINT, best);
    }
    if (t_best != ADC3_CH_TEMP) {
        console_printf("  ADC3_CH_TEMP in adc.h is %u and wants to be "
                       "%u.\n", (uint32_t)ADC3_CH_TEMP, t_best);
    }

    /*
     * THE TWO CALIBRATION WORDS, CHECKED AGAINST PHYSICS rather than
     * against a document. ST measures the sensor at 30 C and at 110 C,
     * both at 3.3 V. What makes these two numbers identifiable is that
     * they produce an offset AND a slope, independently, and both come
     * out where a silicon thermometer has to be: about 0.62 V at 30 C
     * and about 2.5 mV per degree.
     */
    uint32_t c1 = adc_ts_cal1();
    uint32_t c2 = adc_ts_cal2();

    console_printf("\n  TS_CAL1 %5u = %u mV at %d C\n",
                   c1, (c1 * 3300U) / 65535U, (int32_t)TS_CAL1_C);
    console_printf("  TS_CAL2 %5u = %u mV at %d C\n",
                   c2, (c2 * 3300U) / 65535U, (int32_t)TS_CAL2_C);

    if (c2 > c1) {
        uint32_t span   = c2 - c1;
        /* mV across the whole 80 C span FIRST, then per degree. The
         * first version multiplied the span by 3 300 000 and overflowed
         * 32 bits, printing 0.087 mV/C for a sensor that climbs at
         * 2.5 - a wrong number in the middle of an argument that the
         * numbers are right. */
        uint32_t mv_span = (span * 3300U) / 65535U;
        uint32_t uv_c    = (mv_span * 1000U)
                         / (uint32_t)(TS_CAL2_C - TS_CAL1_C);

        console_printf("  slope   %u.%u counts/C = %u.%03u mV/C - a "
                       "silicon sensor of\n",
                       span / 80U, (span % 80U) * 10U / 80U,
                       uv_c / 1000U, uv_c % 1000U);
        console_printf("  this kind sits near 620 mV at 30 C and climbs "
                       "about 2.5 mV/C,\n");
        console_printf("  so BOTH numbers land where they have to, from "
                       "two words that\n");
        console_printf("  were only read. That is what makes the address "
                       "believable.\n");
    }

    /*
     * And the rail, three ways. The meter's number is the outside check
     * - it is the only one of the three that did not come out of this
     * chip.
     */
    uint32_t raw = (uint32_t)adc3_read(ADC3_CH_VREFINT);
    uint32_t v_cal = (cal >= 23400U && cal <= 24700U && raw > 0U)
                   ? (3300U * cal) / raw : 0U;
    uint32_t v_nom = (raw > 0U) ? (1216U * 65535U) / raw : 0U;

    console_printf("\n  VREFINT raw %u counts ->\n", raw);
    console_printf("    VDDA from the factory value : %u mV%s\n",
                   v_cal, (v_cal == 0U) ? "  (unusable)" : "");
    console_printf("    VDDA from the nominal 1216  : %u mV\n", v_nom);
    console_printf("    VDDA from the multimeter    : %u mV  "
                   "(4.15 V at the cell)\n", (uint32_t)ADC_VREF_MV);
    console_printf("  Three routes, one rail. The third is the only one "
                   "that did\n");
    console_printf("  not come out of this chip, so it is the one that "
                   "can catch\n");
    console_printf("  the other two agreeing on something wrong.\n");

    adc_sampling(1);
    board_delay_ms(200);        /* let the smoothing fill */

    int32_t t10 = adc_temp_c10();

    if (t10 == ADC_TEMP_NONE) {
        console_printf("\n  Die temperature: UNAVAILABLE - the "
                       "calibration words did not\n");
        console_printf("  pass the plausibility test, and a temperature "
                       "from a guessed\n");
        console_printf("  slope would be worse than none.\n\n");
    } else {
        console_printf("\n  Die temperature: %d.%u C, from %u counts "
                       "referred back to\n",
                       t10 / 10, (uint32_t)((t10 < 0 ? -t10 : t10) % 10),
                       adc_temp_raw());
        console_printf("  3.3 V - the calibration was taken there, so a "
                       "reading against\n");
        console_printf("  a different rail has to be scaled or the "
                       "thermometer moves\n");
        console_printf("  when the battery sags.\n");
        console_printf("  THIS IS THE DIE, NOT THE ROOM: at 480 MHz it "
                       "runs well above\n");
        console_printf("  ambient, so something near 50 C in a cool room "
                       "is the sensor\n");
        console_printf("  working rather than lying.\n\n");
    }
}

/*
 * ======================================================================
 * Test 13d - the divider's tolerance, against a meter
 * ======================================================================
 *
 * The last error in the battery reading, and the only one the chip
 * cannot see by itself. VREF+ is measured against the bandgap now, so
 * what remains is the ratio of two resistors - 1 % each, which is about
 * 40 mV at a full cell.
 *
 * It takes a number from outside, and that is the whole point: every
 * other check in this file compares the chip against itself. So this
 * prints the cell voltage once a second, steadily, and asks for the
 * meter's reading of the same thing at the same moment. One number
 * comes back and the trim follows from it.
 *
 * WHAT TO DO WITH THE ANSWER is printed at the end rather than left as
 * arithmetic for whoever reads the log: the test computes the parts per
 * thousand for a range of plausible meter readings and prints the table,
 * so the line to change in adc.h can be read off directly.
 */
static void test_bat_calib(void)
{
    console_printf("\nTest 13d - the divider, against a meter\n");
    console_printf("======================================\n");

    if (s_adc_rc != ADC_OK) {
        console_printf("  The ADC did not start; nothing to calibrate.\n\n");
        return;
    }

    ui_prompt("READ YOUR METER",
              "Put it on the battery terminals and",
              "read the cell voltage now.",
              "Twenty seconds.");

    console_printf("  VREF+ is measured against the chip's bandgap, so "
                   "the only\n");
    console_printf("  error left here is the ratio of R34 and R36 - 1 %% "
                   "each,\n");
    console_printf("  about 40 mV at a full cell. The chip cannot see "
                   "that. A\n");
    console_printf("  meter can.\n\n");
    console_printf("  PUT A METER ON THE BATTERY TERMINALS and read it "
                   "while these\n");
    console_printf("  lines go by. Twenty seconds.\n\n");

    uint32_t sum = 0U, n = 0U;
    uint32_t beat = board_millis();
    uint32_t t_end = board_millis() + 20000U;

    while ((int32_t)(board_millis() - t_end) < 0) {
        board_watchdog_feed();

        if ((int32_t)(board_millis() - beat) < 0) { continue; }
        beat = board_millis() + 1000U;

        uint32_t mv = adc_bat_mv();

        sum += mv;
        n++;

        console_printf("    t=%2u s   cell %u mV   rail %u mV   raw %u\n",
                       n, mv, adc_vdda_mv(), adc_raw_bat());
    }

    if (n == 0U) { return; }

    uint32_t mean = sum / n;

    console_printf("\n  This radio says %u mV, averaged over %u "
                   "readings.\n", mean, n);
    console_printf("  Find your meter's reading below and put that "
                   "number into\n");
    console_printf("  ADC_BAT_TRIM_PPT in adc.h:\n\n");
    console_printf("    meter says | ADC_BAT_TRIM_PPT\n");
    console_printf("    -----------+-----------------\n");

    /*
     * A table rather than a formula, because the person reading this has
     * a meter in one hand and no reason to do arithmetic. Twenty
     * millivolts either side of what we measured, in fives, which covers
     * a 1 % divider comfortably.
     */
    for (int32_t d = -20; d <= 20; d += 5) {
        int32_t meter = (int32_t)mean + d;
        int32_t ppt   = ((meter - (int32_t)mean) * 1000) / (int32_t)mean;

        console_printf("    %4d mV   | %+3d%s\n", meter, ppt,
                       (d == 0) ? "   (no trim: they already agree)" : "");
    }

    console_printf("\n  Current setting: %+d. The trim is "
                   "MULTIPLICATIVE - a\n", (int32_t)ADC_BAT_TRIM_PPT);
    console_printf("  divider sets a ratio, so its error scales with the "
                   "voltage.\n");
    console_printf("  Upstream's battery_calib_pon() adds millivolts "
                   "instead, which\n");
    console_printf("  is right for ITS board: there the error being "
                   "trimmed is a\n");
    console_printf("  diode drop, and that really is roughly "
                   "constant.\n\n");
}

static void test_encoder_lines(void)
{
    static const char *const k_name[4] = {
        "PC0 RENC1_A", "PC1 RENC1_B", "PC2 RENC2_A", "PC3 RENC2_B"
    };
    uint32_t saved_moder = GPIOC->MODER;
    uint32_t saved_pupdr = GPIOC->PUPDR;

    console_printf("\nTest 12b - the A/B lines, pulled by US\n");
    console_printf("====================================\n");
    console_printf("  Every reading before this one went through the "
                   "factory\n");
    console_printf("  firmware's pull-ups. This writes its own.\n\n");

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;
    (void)RCC->AHB4ENR;

    /* Inputs, explicitly - they already are, and that is the point. */
    for (uint32_t pin = 0; pin < 4U; pin++) {
        GPIOC->MODER &= ~(3UL << (pin * 2U));
    }

    uint32_t lvl[3];

    for (uint32_t which = 0; which < 3U; which++) {
        uint32_t pupd = (which == 0U) ? 1UL : (which == 1U) ? 2UL : 0UL;

        for (uint32_t pin = 0; pin < 4U; pin++) {
            GPIOC->PUPDR = (GPIOC->PUPDR & ~(3UL << (pin * 2U)))
                         | (pupd << (pin * 2U));
        }

        board_delay_ms(5);      /* 1 nF against a 40 kohm pull is 40 us */
        lvl[which] = GPIOC->IDR & 0x0FU;
    }

    console_printf("  line        | pull-up | pull-down | no pull | verdict\n");
    console_printf("  ------------+---------+-----------+---------+--------\n");

    for (uint32_t pin = 0; pin < 4U; pin++) {
        uint32_t up = (lvl[0] >> pin) & 1U;
        uint32_t dn = (lvl[1] >> pin) & 1U;
        uint32_t no = (lvl[2] >> pin) & 1U;
        const char *verdict;

        if (up == 1U && dn == 0U) {
            verdict = "floats - contact OPEN, wiring fine";
        } else if (up == 0U && dn == 0U) {
            verdict = "TIED LOW - contact closed, or shorted";
        } else {
            verdict = "TIED HIGH";
        }

        console_printf("  %s |    %u    |     %u     |    %u    | %s\n",
                       k_name[pin], up, dn, no, verdict);
    }

    console_printf("\n  At a detent an EC11 rests with both contacts "
                   "CLOSED, so all\n");
    console_printf("  four reading tied low is a legitimate resting "
                   "state - what it\n");
    console_printf("  cannot be is a resting state that survives a "
                   "quarter turn.\n");
    console_printf("  TURN BOTH KNOBS SLOWLY - 20 seconds, pull-ups on, "
                   "and every\n");
    console_printf("  change is printed.\n\n");

    for (uint32_t pin = 0; pin < 4U; pin++) {
        GPIOC->PUPDR = (GPIOC->PUPDR & ~(3UL << (pin * 2U)))
                     | (1UL << (pin * 2U));
    }

    uint32_t prev  = GPIOC->IDR & 0x0FU;
    uint32_t moves = 0;
    uint32_t t_end = board_millis() + 20000U;

    while ((int32_t)(board_millis() - t_end) < 0) {
        uint32_t now = GPIOC->IDR & 0x0FU;

        board_watchdog_feed();

        if (now != prev) {
            moves++;
            if (moves <= 40U) {
                console_printf("    %u -> %u   (RENC1 AB %u, RENC2 AB %u)\n",
                               prev, now, now & 3U, (now >> 2) & 3U);
            }
            prev = now;
        }
    }

    console_printf("\n  %u changes in 20 s.\n", moves);

    GPIOC->MODER = saved_moder;
    GPIOC->PUPDR = saved_pupdr;
    console_printf("  port C put back as it was (MODER 0x%08x PUPDR "
                   "0x%08x)\n\n", GPIOC->MODER, GPIOC->PUPDR);
}

/*
 * TEST 12a: WHICH PINS ARE THE ENCODERS? ASK THE HAND, NOT THE DRAWING.
 *
 * Written because three measurements in a row failed to find them where
 * the pin map says they are. BOARD.md section 5 puts RENC1 on PC0/PC1
 * and RENC2 on PC2/PC3, read off the schematic - and that section
 * carries no [measured] tag, which is exactly the distinction it exists
 * to make. On the radio, with the factory firmware's own pull-ups still
 * in place and both knobs being turned for a full minute, those four
 * lines did not change once.
 *
 * So this watches EVERY pin of ports A to E and reports which ones move
 * while somebody turns the knobs. It assumes nothing about the wiring,
 * which is the only honest position left: it is the add-on board's
 * method - "when a peripheral's wiring is undocumented, the firmware
 * that already drives it is a schematic" - one step further, because
 * here even the firmware is not saying.
 *
 * TWO THINGS MAKE IT READABLE.
 *
 * It prints NOTHING while it watches. The console's own TX pin is PE8,
 * so a scan that reports as it goes is a scan that sees itself; and
 * SAI1 is clocking PE2..PE6 continuously whatever anybody does. Port E
 * is therefore expected to be noisy and is reported with those pins
 * named, rather than quietly masked - a mask would hide the one thing
 * that would prove the scan is working at all.
 *
 * And it records, for every pin, the level it STARTED at as well as the
 * fact that it changed. That is what settles a button's polarity in one
 * go: a pin that starts high and goes low is a switch to ground, and no
 * second experiment with the pull the other way is needed. The instant
 * probe this replaces could not tell "held high at rest" from "held
 * high because pressed", and said the wrong one out loud.
 */
static void test_encoder_scan(void)
{
    GPIO_TypeDef *const port[5] = { GPIOA, GPIOB, GPIOC, GPIOD, GPIOE };
    static const char   name[5] = { 'A', 'B', 'C', 'D', 'E' };
    uint32_t first[5], changed[5], last[5];

    console_printf("\nTest 12a - which pins move when a knob turns?\n");
    console_printf("===========================================\n");
    console_printf("  The pin map says PC0/PC1 and PC2/PC3 and it has "
                   "never been\n");
    console_printf("  checked on hardware. This checks it the other way "
                   "round: it\n");
    console_printf("  watches every pin of ports A..E and says which ones "
                   "moved.\n\n");

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN
                  | RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIODEN
                  | RCC_AHB4ENR_GPIOEEN;
    (void)RCC->AHB4ENR;

    for (uint32_t p = 0; p < 5U; p++) {
        first[p]   = port[p]->IDR;
        last[p]    = first[p];
        changed[p] = 0U;
    }

    console_printf("  TURN BOTH KNOBS AND PRESS BOTH BUTTONS - 25 seconds,\n");
    console_printf("  starting now. Nothing will be printed until it is "
                   "over.\n\n");

    uint32_t t_end = board_millis() + 25000U;

    while ((int32_t)(board_millis() - t_end) < 0) {
        board_watchdog_feed();

        for (uint32_t p = 0; p < 5U; p++) {
            uint32_t now = port[p]->IDR;

            changed[p] |= (now ^ last[p]);
            last[p]     = now;
        }
    }

    console_printf("  port | pins that moved (start level -> last level)\n");
    console_printf("  -----+------------------------------------------\n");

    for (uint32_t p = 0; p < 5U; p++) {
        if (changed[p] == 0U) {
            console_printf("    P%c | none\n", name[p]);
            continue;
        }

        console_printf("    P%c |", name[p]);
        for (uint32_t pin = 0; pin < 16U; pin++) {
            if ((changed[p] & (1UL << pin)) == 0U) { continue; }

            console_printf(" P%c%u(%u->%u)", name[p], pin,
                           (first[p] >> pin) & 1U, (last[p] >> pin) & 1U);
        }
        console_printf("\n");
    }

    console_printf("\n  Expect PE2..PE6 (SAI1, always clocking) and PE8 "
                   "(this console's\n");
    console_printf("  own TX). Those two are the proof the scan works. "
                   "Anything else\n");
    console_printf("  that moved is a hand on the hardware - and a pin "
                   "that starts\n");
    console_printf("  high and ends low is a switch to GROUND.\n\n");
}

/*
 * TEST 12: THE ENCODER, AND THE TWO THINGS ABOUT IT THAT ARE NOT IN ANY
 * DOCUMENT.
 *
 * Upstream's encoder.c is grafted whole and does the hard part - a full
 * quadrature table at 1 kHz, inherently debounced, with quarter-step
 * carry and short/long press classification. What it cannot know is this
 * board's wiring, and two details of it are not in the schematic notes:
 *
 *   1. WHICH LEVEL IS A PRESS. The DEEPSDR's button drives Vcc and rests
 *      at 0 V; this board's encoder A/B lines are pulled UP by the
 *      factory firmware, which says its contacts go to ground, so the
 *      button probably does too. "Probably" is what this test is for.
 *      The probe is to read the pin with an internal pull-up and then
 *      with a pull-down: a pin nothing is driving follows the pull both
 *      times, and a pin the button is holding does not. That needs a
 *      finger on the button, which is why it asks for one.
 *
 *   2. WHICH WAY IS CLOCKWISE. Upstream found that this is not even
 *      constant across DEEPSDR units - encoder.c's comment of 30/09/2026
 *      - and carries a user-settable inversion for exactly that reason.
 *      So this test does not decide it; it reports the sign and lets the
 *      hand that turned the knob say whether it agreed.
 *
 * AND PA1 HAS AN ANALOG SWITCH IN FRONT OF IT. On the H743, PA0/PA1 and
 * PC2/PC3 are dual pads joined by a switch that SYSCFG_PMCR opens and
 * closes. Read on this radio: PMCR = 0x03000000, so the factory firmware
 * left PA0's and PA1's switches OPEN and PC2/PC3's closed. An open
 * switch is right for an analog input - PA0 is the battery sense - and
 * it is the wrong state for reading a button, because the digital input
 * is then looking at the wrong pad. So this closes PA1's, explicitly,
 * and leaves PA0's alone: that one is an ADC pin and opening it was
 * somebody's deliberate choice.
 */
static uint32_t enc_ab(GPIO_TypeDef *port, uint32_t shift)
{
    return (port->IDR >> shift) & 3U;
}

static void test_encoder(void)
{
    console_printf("\nTest 12 - the encoders (needs a hand)\n");
    console_printf("====================================\n");

    /*
     * THE BUTTON PROBE THAT USED TO BE HERE IS GONE, and what replaced
     * it is the schematic. It read PA1 with a pull-up and then a
     * pull-down and concluded from the levels - and it exited after five
     * agreeing samples, which on this board took twenty milliseconds,
     * long before a finger could arrive. So the row it printed as "while
     * held" was the idle level, and it announced the wrong polarity with
     * a straight face.
     *
     * Page 3 of Malahit_schematics_15012020.pdf settles it and no probe
     * can: ENC_BUT is a RESISTOR LADDER. 47k from 3.3 V, then 68k to
     * ground through ENC1's switch and 22k through ENC2's, which puts
     * the line at 1.95 V for one button, 1.05 V for the other and 0.86 V
     * for both. On a 3.3 V input VIL is 0.99 V and VIH is 2.31 V, so one
     * of those three voltages is not a digital level at all. Two buttons
     * on one pin can only be told apart by measuring, which is why the
     * factory firmware keeps PA1 in analog mode with its analog switch
     * open.
     *
     * So the buttons want the ADC. They have it now - test_adc() and
     * src/adc.c - and the presses this test reports arrive through it,
     * but the test itself is still about the rotation.
     */

    /*
     * Now hand the pins to upstream's driver and start sampling. Calling
     * this twice is harmless: it reconfigures the same pins and re-reads
     * the same starting state. It stays because this test has to work
     * when it is called on its own, from a build where main() has not
     * done it.
     */
    encoder_init();
    board_encoder_sampling(1);

    ui_prompt("TURN BOTH KNOBS",
              "The twenty seconds start at the",
              "first detent, so take as long as",
              "you like to get there.");

    console_printf("\n  upstream's encoder.c is now sampling at 1 kHz from "
                   "SysTick.\n");
    console_printf("  TURN BOTH KNOBS. The twenty seconds start at the "
                   "first\n");
    console_printf("  detent, so take as long as you like to get there. "
                   "Every detent\n");
    console_printf("  prints a line, and the heartbeat also reports "
                   "RENC2's raw\n");
    console_printf("  lines, which the driver is not reading.\n\n");

    int32_t  total   = 0;
    uint32_t presses = 0, longs = 0;
    uint32_t beat    = board_millis();
    /*
     * The driver has RENC1 now - the big knob - so this watches RENC2,
     * the one nothing is bound to. It was the other way round when it
     * was written, and that is the reason "RENC1 is dead" lasted exactly
     * one run: three windows had recorded no movement on it and that had
     * already been written down as a hardware fault; this watcher then
     * counted 151 changes in twenty seconds. An absence is only evidence
     * if somebody was turning the knob while it was being recorded, and
     * that is not a thing firmware can check.
     */
    uint32_t enc2_prev  = enc_ab(GPIOC, 2U);
    uint32_t enc2_moves = 0;

    /*
     * A minute of patience, and twenty seconds once something happens.
     * Same reason as the probe above.
     */
    int      started = 0;
    uint32_t t_end   = board_millis() + 60000U;

    while ((int32_t)(board_millis() - t_end) < 0) {
        board_watchdog_feed();

        int32_t d = encoder_take_delta();

        if (!started && (d != 0 || encoder_button_down())) {
            started = 1;
            t_end   = board_millis() + 20000U;
            console_printf("    (something moved - twenty seconds from "
                           "here)\n");
        }

        if (d != 0) {
            total += d;
            console_printf("    %+3d detent%s -> running total %+4d  "
                           "(AB %u, button %s)\n",
                           (int32_t)d, (d == 1 || d == -1) ? " " : "s",
                           total, enc_ab(GPIOC, 0U),
                           encoder_button_down() ? "DOWN" : "up");
        }

        if (encoder_take_press())      { presses++;
            console_printf("    short press #%u\n", presses); }
        if (encoder_take_long_press()) { longs++;
            console_printf("    LONG press #%u\n", longs); }

        /*
         * The second encoder has no driver yet - upstream has one knob
         * and this board has two - so watch its lines directly. It costs
         * two reads and it turns "there is a second encoder" from a line
         * in the schematic into something that has been seen to move.
         */
        uint32_t e2 = enc_ab(GPIOC, 2U);

        if (e2 != enc2_prev) { enc2_prev = e2; enc2_moves++; }

        if ((int32_t)(board_millis() - beat) >= 0) {
            beat = board_millis() + 2000U;
            console_printf("    .. RENC1 AB %u (the driver's)  RENC2 AB "
                           "%u (%u changes)\n",
                           enc_ab(GPIOC, 0U), e2, enc2_moves);
        }
    }

    console_printf("\n  %+d detents from RENC1, %u short presses, %u long "
                   "presses,\n", total, presses, longs);
    console_printf("  and RENC2's lines changed %u times.\n", enc2_moves);
    console_printf("  WHICH WAY: positive should mean clockwise. If it did "
                   "not, this\n");
    console_printf("  board wants encoder_invertido_pon(1) - upstream's own "
                   "setting,\n");
    console_printf("  which exists because it is not the same on every "
                   "unit.\n\n");
}

/*
 * TEST 11: IS THE RECEIVE STREAM MOVING TWO WORDS A FRAME, OR ONE?
 *
 * Written because the first DMA run drew a spectrum with its peak at
 * TWICE the right offset and an image almost as strong as the carrier,
 * and that pair of symptoms has exactly one cause: consecutive samples
 * of ONE channel being read as an I/Q pair. Two samples of a real signal
 * taken a sample apart look like a complex pair at half the sample rate,
 * so the apparent frequency doubles and the mirror comes up to meet it.
 * Nothing else in a receiver does that.
 *
 * So the question is not "what is wrong with the spectrum". It is
 * arithmetic about the transport, and three measurements settle it
 * without an opinion:
 *
 *   1. THE ITEM RATE out of NDTR against the CPU clock. The stream
 *      should move 384 000 half-words a second at 192 kHz - two slots a
 *      frame. Half of that is the whole diagnosis.
 *   2. THE TWO SLOTS, separately. On this board I and Q have offsets
 *      that differ by a factor of five (BOARD.md section 6), so two
 *      slots reporting the SAME mean are not two channels.
 *   3. THE SAME BLOCK, BOTH WAYS. The polled transport is known good -
 *      every number in BOARD.md came through it - so printing the two
 *      side by side turns "the DMA looks wrong" into a diff.
 */
static void test_dma_transport(void)
{
    console_printf("\nTest 11 - what the receive stream actually moves\n");
    console_printf("===============================================\n");

    if (!sai1_dma_running()) {
        console_printf("  the DMA transport is not running - nothing to "
                       "measure\n\n");
        return;
    }

    /* ---- 1. the item rate, straight out of NDTR ---- */

    uint32_t ring = sai1_rx_ring_words();
    uint32_t p0   = sai1_rx_pos();
    uint64_t c0   = board_cycles();

    board_delay_ms(50);         /* 19 200 half-words at 192 kHz: no wrap */

    uint32_t p1 = sai1_rx_pos();
    uint64_t c1 = board_cycles();

    uint32_t moved = (p1 >= p0) ? (p1 - p0) : (ring - p0 + p1);
    uint32_t us    = (uint32_t)(((c1 - c0) * 1000000U) / clock_sysclk_hz);
    uint32_t rate  = (us != 0U) ? (uint32_t)(((uint64_t)moved * 1000000U) / us)
                                : 0U;

    console_printf("  ring %u half-words, moved %u in %u us\n",
                   ring, moved, us);
    console_printf("  -> %u half-words a second, so %u frames a second\n",
                   rate, rate / 2U);
    console_printf("     192 kHz stereo wants 384000 and 192000.%s\n",
                   (rate > 360000U && rate < 408000U)
                       ? ""
                       : "  *** NOT WHAT IT WANTS ***");
    if (rate > 170000U && rate < 215000U) {
        console_printf("     192000 half-words a second is ONE WORD A "
                       "FRAME: half the\n");
        console_printf("     stream is being dropped and the pairs are "
                       "consecutive samples\n");
        console_printf("     of one channel, which is why the peak "
                       "doubles.\n");
    }

    /* ---- 2. and 3. the same block, both transports ---- */

    chan_stats_t d0, d1, q0, q1;

    if (sai1_rx_half(s_rx, CAP_FRAMES) == 0U) {
        console_printf("  *** no half out of the ring ***\n\n");
        return;
    }

    stats(s_rx, CAP_FRAMES, 0U, &d0);
    stats(s_rx, CAP_FRAMES, 1U, &d1);

    uint16_t dhex[8];
    for (uint32_t i = 0; i < 8U; i++) { dhex[i] = (uint16_t)s_rx[i]; }

    /*
     * The polled block is taken SECOND and from the same signal, which
     * is standing still on this bench. It stops the DMA - sai1_stream()
     * says so and counts it - and the transport is re-armed below.
     *
     * AND ITS RETURN VALUE IS CHECKED, because this test printed a
     * comparison it had not measured. The polled capture came back with
     * ZERO frames - SAIEN had not held, see sai1_start() - and s_rx
     * still held the DMA block, so the "polled" rows below were the DMA
     * rows averaged a second time. They matched to the last digit, which
     * is how it was noticed: four identical columns out of an analogue
     * measurement are impossible, and this file's own capture() had
     * printed "got 0 of 8192 frames" three lines earlier.
     *
     * WORKING.md's last trap, in this very file, for the second time.
     */
    uint32_t polled_got = capture();

    if (polled_got == 0U) {
        console_printf("\n  *** the polled reference capture returned "
                       "nothing, so there is\n");
        console_printf("      nothing to compare against - the DMA rows "
                       "alone are below.\n");
        console_printf("      (SAIEN did not hold: see sai1_start().)\n");
    }

    stats(s_rx, CAP_FRAMES, 0U, &q0);
    stats(s_rx, CAP_FRAMES, 1U, &q1);

    console_printf("\n  transport | slot |     mean |   rms |    min |"
                   "    max\n");
    console_printf("  ----------+------+----------+-------+--------+"
                   "--------\n");
    console_printf("  DMA       |   0  | %8d | %5u | %6d | %6d\n",
                   d0.mean, d0.rms, (int32_t)d0.min, (int32_t)d0.max);
    console_printf("  DMA       |   1  | %8d | %5u | %6d | %6d\n",
                   d1.mean, d1.rms, (int32_t)d1.min, (int32_t)d1.max);
    if (polled_got != 0U) {
        console_printf("  polled    |   0  | %8d | %5u | %6d | %6d\n",
                       q0.mean, q0.rms, (int32_t)q0.min, (int32_t)q0.max);
        console_printf("  polled    |   1  | %8d | %5u | %6d | %6d\n",
                       q1.mean, q1.rms, (int32_t)q1.min, (int32_t)q1.max);
    } else {
        console_printf("  polled    |  --  |      --- |   --- |    --- |"
                       "    ---   (no capture)\n");
    }

    console_printf("\n  first 4 frames, slot0:slot1 -\n");
    console_printf("    DMA    %04x:%04x %04x:%04x %04x:%04x %04x:%04x\n",
                   (uint32_t)dhex[0], (uint32_t)dhex[1],
                   (uint32_t)dhex[2], (uint32_t)dhex[3],
                   (uint32_t)dhex[4], (uint32_t)dhex[5],
                   (uint32_t)dhex[6], (uint32_t)dhex[7]);
    if (polled_got != 0U) {
        console_printf("    polled %04x:%04x %04x:%04x %04x:%04x "
                       "%04x:%04x\n",
                       (uint32_t)(uint16_t)s_rx[0],
                       (uint32_t)(uint16_t)s_rx[1],
                       (uint32_t)(uint16_t)s_rx[2],
                       (uint32_t)(uint16_t)s_rx[3],
                       (uint32_t)(uint16_t)s_rx[4],
                       (uint32_t)(uint16_t)s_rx[5],
                       (uint32_t)(uint16_t)s_rx[6],
                       (uint32_t)(uint16_t)s_rx[7]);
    }

    console_printf("\n  The two means are the test: this board's I and Q\n");
    console_printf("  offsets differ by about five times, so two slots\n");
    console_printf("  with the same mean are one channel read twice.\n");

    int rc = sai1_dma_start(CAP_FRAMES, TX_HALF_FRAMES);

    console_printf("\n  re-armed, mode switches %u, rx overruns %u\n",
                   sai1_mode_switches(), sai1_rx_overruns());
    dma_arm_report("test 11 re-arm", rc);
    dma_err_report();
    console_printf("  The count above is one per arm and does not grow "
                   "while the\n");
    console_printf("  radio runs - the per-frame line reports it as err, "
                   "and a\n");
    console_printf("  number that stops moving is a teardown artefact "
                   "rather than\n");
    console_printf("  a fault in the transport. TEIF would not be: that "
                   "is an\n");
    console_printf("  address the DMA cannot reach.\n\n");
}

/*
 * TEST 10: DOES THE TRANSMITTER RUN DRY WHEN THE CPU IS BUSY?
 *
 * The question the whole step exists for, and it is not a question about
 * CPU load. The transmit FIFO is eight words - four frames, 21 us at
 * 192 kHz - and the console writes a line in about 9000 us. Thirty times
 * the FIFO. So on the polled transport every console line is a gap in
 * the audio, and a gap is a click.
 *
 * MEASURED BOTH WAYS WITH THE SAME TONE AND THE SAME CONSOLE TRAFFIC,
 * because "DMA is better" is an assertion and a pair of numbers from one
 * afternoon on one bench is not. The load is deliberately the console
 * and not a dummy delay loop: it is the real thing that was breaking the
 * audio, it blocks for a real 9 ms at a time, and the lines it prints
 * are the evidence that it ran.
 *
 * AND IT IS AUDIBLE, which matters more than the counters. The polled
 * phase clicks once per printed line; the DMA phase is a clean tone
 * through the same traffic. Anyone at the bench can judge that without
 * reading a number, which is the only kind of result that cannot be an
 * arithmetic mistake.
 */
static void test_audio_gap(void)
{
    const uint32_t lines = 24U;

    console_printf("\nTest 10 - the transmit FIFO against the console\n");
    console_printf("==============================================\n");
    console_printf("  The FIFO is 8 words = 4 frames = 21 us at 192 kHz.\n");
    console_printf("  A console line at 115200 baud is about 9000 us. The\n");
    console_printf("  same tone is played both ways through the same %u\n",
                   lines);
    console_printf("  console writes, and the underrun count is the\n");
    console_printf("  measurement. It is audible too: one click per line\n");
    console_printf("  on the polled transport.\n\n");

    tone_fill(s_tone, TX_HALF_FRAMES, 1000U, 192000U);

    /* ---------------- polled ---------------- */

    uint32_t polled_under = 0;
    uint64_t t0 = board_cycles();

    for (uint32_t i = 0; i < lines; i++) {
        board_watchdog_feed();

        /*
         * Four halves' worth of tone per line, so the CPU is inside the
         * transport for about 21 ms and inside the console for about 9,
         * which is the duty cycle a real program would have. The
         * underruns all happen in the 9.
         */
        sai1_play(s_tone, TX_HALF_FRAMES, TX_HALF_FRAMES * 4U);
        polled_under += sai1_tx_underruns();

        console_printf("    polled %2u/%2u - tone, then a console line, "
                       "and the FIFO empties in the line\n", i + 1U, lines);
    }

    uint32_t polled_ms = (uint32_t)(((board_cycles() - t0) * 1000U)
                                    / clock_sysclk_hz);

    /* ---------------- DMA ---------------- */

    int rc = sai1_dma_start(CAP_FRAMES, TX_HALF_FRAMES);

    if (rc != SAI1_OK) {
        console_printf("\n  *** sai1_dma_start failed, rc %d - staying "
                       "polled ***\n\n", rc);
        return;
    }

    console_printf("\n  DMA armed: rx half %u frames (%u ms), tx half %u "
                   "frames (%u.%u ms),\n",
                   CAP_FRAMES, (CAP_FRAMES * 1000U) / 192000U,
                   TX_HALF_FRAMES, (TX_HALF_FRAMES * 1000U) / 192000U,
                   ((TX_HALF_FRAMES * 10000U) / 192000U) % 10U);
    console_printf("  DMA1 stream 0 = SAI1_B in, stream 1 = SAI1_A out, "
                   "both circular.\n");

    /*
     * WHO ELSE WAS ON THE REQUEST LINE. This line exists because the
     * answer was "the factory firmware" and it cost the first DMA run:
     * two channels on one SAI request line take alternate words, so the
     * receiver gets exactly half the stream and the spectrum comes out
     * at twice the offset. sai1.c has the whole story.
     */
    uint32_t mux = sai1_mux_released();

    console_printf("  DMAMUX channels taken back: 0x%04x", mux);
    if (mux == 0U) {
        /*
         * Expected on a reload and NOT expected on the first load after
         * a power cycle. Measured both ways on this radio: 0x2200 on the
         * first load after switching on, 0x0000 on every reload after
         * it, because by then this firmware has already unwired them and
         * the factory image has not run since.
         */
        console_printf("  (none - right for a reload, wrong for the\n");
        console_printf("              first load after a power cycle)\n\n");
    } else {
        console_printf("\n");
        for (uint32_t ch = 0; ch < 16U; ch++) {
            if (mux & (1UL << ch)) {
                console_printf("    ch %2u = DMA%u stream %u, left enabled "
                               "by the factory firmware\n",
                               ch, (ch < 8U) ? 1U : 2U,
                               (ch < 8U) ? ch : (ch - 8U));
            }
        }
        console_printf("\n");
    }

    dma_arm_report("test 10", rc);

    uint32_t base_under = sai1_tx_underruns();
    uint32_t base_rep   = sai1_tx_repeats();

    t0 = board_cycles();

    for (uint32_t i = 0; i < lines; i++) {
        board_watchdog_feed();

        /*
         * Feed a half whenever one is free, which is the polled stand-in
         * for the half-transfer interrupt step 8 will use. Between the
         * writes the CPU is in the console, and the transmitter does not
         * need it there.
         */
        uint64_t until = board_cycles()
                       + ((uint64_t)clock_sysclk_hz * 21U) / 1000U;

        while (board_cycles() < until) {
            if (sai1_tx_due()) { (void)sai1_tx_write_half(s_tone,
                                                          TX_HALF_FRAMES); }
        }

        console_printf("    DMA    %2u/%2u - tone, then a console line, "
                       "and the stream does not care\n", i + 1U, lines);
    }

    uint32_t dma_ms = (uint32_t)(((board_cycles() - t0) * 1000U)
                                 / clock_sysclk_hz);

    console_printf("\n  transport |      elapsed | tx underruns | half "
                   "repeats | dma errors\n");
    console_printf("  ----------+--------------+--------------+-------"
                   "-------+-----------\n");
    console_printf("  polled    |      %4u ms | %12u |          n/a "
                   "| n/a\n", polled_ms, polled_under);
    console_printf("  DMA       |      %4u ms | %12u | %12u | %10u\n",
                   dma_ms, sai1_tx_underruns() - base_under,
                   sai1_tx_repeats() - base_rep, sai1_dma_errors());

    console_printf("\n  producer phase margin: the writes landed between "
                   "%u and %u\n",
                   sai1_tx_margin_min() == 0xFFFFFFFFU
                       ? 0U : sai1_tx_margin_min(),
                   sai1_tx_margin_max());
    console_printf("  half-words into a half of %u - both low means the\n",
                   sai1_tx_half_words());
    console_printf("  producer is comfortable, both high means it is "
                   "arriving at\n");
    console_printf("  the end of its budget. (Upstream's own diagnostic; "
                   "see sai1.h.)\n");

    /*
     * BACK TO SILENCE BEFORE THE SCREEN. Re-arming is the way to do it:
     * sai1_dma_start() clears both rings, so this cannot leave a tone
     * playing under the display loop - and it exercises the start path a
     * second time, which is free.
     */
    rc = sai1_dma_start(CAP_FRAMES, AUDIO_HALF_FRAMES);
    console_printf("\n  re-armed silent for the display loop, streams %s\n",
                   sai1_dma_streams_live() ? "live" : "*** STOPPED ***");
    dma_arm_report("test 10 re-arm", rc);
    console_printf("\n");
}

/*
 * ENOUGH GAIN REDUCTION TO STOP CLIPPING, FOUND BY LOOKING.
 *
 * This replaces a hard-coded 12 dB, and the 12 was not a bad guess - it
 * was measured, once, against a generator at one level on one
 * afternoon. Then the generator was unplugged, reconnected by cable at
 * a different level, and the same 12 dB put the converter at 100 % for
 * a whole session. The spectrum it drew was a forest of intermodulation
 * products and the only visible symptom was that the trace looked
 * wrong.
 *
 * A constant that was right when it was written is the most expensive
 * kind: nobody suspects it, because it was measured. The fix is not a
 * better constant, it is to stop having one - the hardware can tell us
 * what it needs in a few tens of milliseconds, and then it is right at
 * every input level instead of at one.
 *
 * This is NOT an AGC. It runs once, it only ever reduces gain, and it
 * has no time constants, no hysteresis and no attack. It is the floor
 * that an AGC will eventually sit on, and it exists so that the screen
 * is never drawing a clipped capture while nobody is looking at the
 * console.
 *
 * 60 % of full scale as the target: far enough from the rail that the
 * modulation peaks of an AM signal - 1.7x the carrier at 70 % depth -
 * still fit, and close enough that the converter is being used.
 */
#define RF_TARGET_PCT   60U
#define RF_GR_STEP      4U

static uint8_t rf_autorange(void)
{
    uint8_t gr;

    for (gr = 0U; gr < 59U; gr = (uint8_t)(gr + RF_GR_STEP)) {
        msi001_set_gain(gr, 0, 0, 0);
        board_delay_ms(10);
        board_watchdog_feed();

        if (capture() == 0U) { continue; }

        if (s_cap_used_pct < RF_TARGET_PCT && s_cap_clipped == 0U) {
            return gr;
        }
    }

    /* Ran out of range: the input is stronger than this receiver can
     * accommodate even at maximum reduction, which is worth saying
     * rather than silently returning the last value tried. */
    console_printf("  *** rf_autorange: still %u%% at %u dB GR - the "
                   "input is too strong ***\n", s_cap_used_pct, gr);
    return gr;
}

/* ================================================================== */
/* STEP 7: THE SCREEN                                                 */
/* ================================================================== */

/*
 * The whole 480 x 320 main screen, drawn by upstream's own UI modules:
 * ui_top.c for the header and the status band, spec_chrome.c for the
 * gutter and the ruler, spectrum.c for the trace and the waterfall
 * colours, ui_act.c for the six buttons. Every coordinate comes from
 * compat/malahit_layout.h, which is the same file deepsdr-ui's host
 * render uses - so what follows should put on the panel exactly what
 * `make malahit` puts in the PNG.
 *
 * WHAT IS REAL ON THIS SCREEN AND WHAT IS NOT.
 *
 * This is the first thing to say about it, because a receiver's screen
 * is an instrument and an instrument that invents a reading is worse
 * than one that shows nothing. Of the fields ui_top_state_t carries:
 *
 *   REAL   the frequency (straight off msi001_last_plan()), the
 *          spectrum, the waterfall, the ruler, the dB gutter, and the
 *          S-meter's MOVEMENT - it follows the measured peak.
 *
 *   STUB   the S-meter's SCALE. There is no calibration yet, so S9 is
 *          simply the top of the display range; see ui_s_units().
 *          dbm_valid is 0 for the same reason, so no dBm number is
 *          claimed.
 *
 *   STUB   the clock, which counts UPTIME because this port has no RTC
 *          driver, and the battery, which reads 0 because nothing has
 *          been written for the ADC on PA0 yet. Both are drawn rather
 *          than hidden so that the layout gets tested at full width,
 *          and both are listed on the console at startup.
 *
 *   HONEST ZEROS   mode is AM because the RF path is set up for AM and
 *          there is no demodulator to contradict it; AGC is OFF and
 *          ATT is OFF because they are; bw, band, RDS, NCO and JMP are
 *          left empty because nothing drives them.
 *
 * ui_top.c treats a null pointer as "do not draw this", which is what
 * makes an honest empty field cheap.
 */
#include "spec_chrome.h"  /* ../../User/spec_chrome.c - upstream */
#include "ui_top.h"       /* ../../User/ui_top.c      - upstream */
#include "ui_act.h"       /* ../../User/ui_act.c      - upstream */
#include "i2c3.h"
#include "touch.h"

#include "fram.h"
#include "settings.h"

/*
 * WHAT IS SOLDERED AT D11 - 21/11/2026.
 *
 * The schematic says FM25W256, and the FM25W256 has no ID command: its
 * whole vocabulary is WREN, WRDI, RDSR, WRSR, READ and WRITE. So the
 * part is identified by what it DOES, against the thing most likely to
 * be in the same SO-8 footprint instead - an SPI EEPROM (25LC256,
 * AT25256, M95256: same pins, same six opcodes):
 *
 *   - an EEPROM is busy for ~5 ms after a write, and says so in bit 0
 *     of the status register (WIP); on this FRAM bit 0 is fixed at 0;
 *   - an EEPROM writes in 64-byte pages, and a write that runs past a
 *     page's end wraps to that page's start; FRAM has no pages;
 *   - other FRAMs (MB85RS256, FM25V02) answer RDID, 0x9F, with a JEDEC
 *     ID; this one should leave MISO alone - read as 0xFF through the
 *     pull-up.
 *
 * FRAM_TEST 1 only reads: the status register, RDID, all 32 kB into
 * g_fram_image - for a backup over SWD before anything is written - and
 * a sequential read across the end, which wraps at 32 kB on a 32 kB
 * part. FRAM_TEST 2 adds the write: 96 bytes across the page line at
 * 0x3FC0 - in the middle of the 25 kB the factory firmware leaves
 * blank, 0x0D00-0x6FFF, so none of its data is touched even for a
 * moment - then the 128 bytes around it put back from g_fram_image, in
 * page-aligned 32-byte pieces with the busy bit waited out - correct on
 * an EEPROM too, which is the case it is for.
 *
 * Slow on purpose: 3.75 MHz, inside every candidate's rating.
 */
#ifndef FRAM_TEST
#define FRAM_TEST 0
#endif

#if FRAM_TEST
uint8_t g_fram_image[FRAM_SIZE];

static uint32_t crc32_of(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFU;

    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (uint32_t k = 0; k < 8U; k++) {
            c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
        }
    }
    return ~c;
}

/* Microseconds until bit 0 of the status register reads 0, polled;
 * 20 000 if it never does. */
static uint32_t fram_wip_us(void)
{
    uint64_t t0 = board_cycles();

    for (;;) {
        uint32_t us = (uint32_t)(((board_cycles() - t0) * 1000000U)
                                 / clock_sysclk_hz);

        if ((fram_rdsr() & FRAM_SR_WIP) == 0U) { return us; }
        if (us >= 20000U) { return 20000U; }
    }
}

static void test_fram(uint32_t mode)
{
    static uint8_t buf[4096];
    uint8_t  id[9];
    uint32_t n00 = 0U, nff = 0U, seen[8] = { 0U }, distinct = 0U;

    fram_init(5U);                                  /* 3.75 MHz */

    console_printf("\nTest - what is soldered at D11 (FRAM_TEST %u), "
                   "SPI2 at 3750000 Hz\n", mode);
    console_printf("=================================================="
                   "==========\n");

    uint8_t sr = fram_rdsr();

    console_printf("  RDSR        0x%02x   WPEN %u  BP %u  WEL %u  bit0 "
                   "(WIP on an EEPROM) %u\n", (uint32_t)sr,
                   (uint32_t)(sr >> 7), (uint32_t)((sr >> 2) & 3U),
                   (uint32_t)((sr >> 1) & 1U), (uint32_t)(sr & 1U));

    fram_cmd_read(0x9FU, id, sizeof id);
    console_printf("  RDID (0x9F) ");
    for (uint32_t i = 0; i < sizeof id; i++) {
        console_printf(" %02x", (uint32_t)id[i]);
    }
    console_printf("   (all ff: no such command)\n");

    uint64_t t0 = board_cycles();
    fram_read(0U, g_fram_image, FRAM_SIZE);
    uint32_t us = (uint32_t)(((board_cycles() - t0) * 1000000U)
                             / clock_sysclk_hz);

    for (uint32_t i = 0; i < FRAM_SIZE; i++) {
        uint8_t b = g_fram_image[i];

        if (b == 0x00U) { n00++; }
        if (b == 0xFFU) { nff++; }
        if (!(seen[b >> 5] & (1UL << (b & 31U)))) {
            seen[b >> 5] |= (1UL << (b & 31U));
            distinct++;
        }
    }
    console_printf("  read 32768 bytes in %u us: crc32 0x%08x, 0x00 x%u, "
                   "0xff x%u, %u distinct values, at 0x%08x\n", us,
                   crc32_of(g_fram_image, FRAM_SIZE), n00, nff, distinct,
                   (uint32_t)(uintptr_t)g_fram_image);

    /* The same bytes twice: a read that is noise does not repeat. */
    fram_read(0U, buf, sizeof buf);
    uint32_t same = 0U;
    for (uint32_t i = 0; i < sizeof buf; i++) {
        if (buf[i] == g_fram_image[i]) { same++; }
    }
    console_printf("  first 4096 read again: %u identical\n", same);

    /* Across the end: 2 kB before it and 2 kB after. A 32 kB part wraps
     * to 0; a bigger one would go on into bytes of its own. */
    fram_read(0x7800U, buf, sizeof buf);
    uint32_t tail = 0U, wrap = 0U;
    for (uint32_t i = 0; i < 2048U; i++) {
        if (buf[i] == g_fram_image[0x7800U + i]) { tail++; }
        if (buf[2048U + i] == g_fram_image[i]) { wrap++; }
    }
    console_printf("  read across the end: 0x7800.. %u/2048 match, and "
                   "the next 2048 match 0x0000.. %u/2048\n", tail, wrap);

    console_printf("  first 256 bytes:\n");
    for (uint32_t r = 0; r < 256U; r += 16U) {
        console_printf("    %04x:", r);
        for (uint32_t i = 0; i < 16U; i++) {
            console_printf(" %02x", (uint32_t)g_fram_image[r + i]);
        }
        console_printf("\n");
    }

    if (mode < 2U) {
        console_printf("  (FRAM_TEST 1: nothing written)\n\n");
        return;
    }

    /*
     * THE WRITE. 96 bytes from 0x3FA0, so 32 before the 64-byte page
     * line at 0x3FC0 and 64 after it. Every byte position-unique and
     * different from what is there, so a wrap shows wherever it lands.
     */
    enum { A = 0x3FA0, N = 96, LO = 0x3F80, HI = 0x4000 };
    uint8_t pat[N], back[N];
    uint32_t imm = 0U, later = 0U, outside = 0U;

    for (uint32_t i = 0; i < (uint32_t)N; i++) {
        pat[i] = (uint8_t)(0x5AU + i * 13U);
        if (pat[i] == g_fram_image[A + i]) { pat[i] ^= 0x01U; }
    }

    fram_write((uint32_t)A, pat, (uint32_t)N);
    uint8_t sr_after = fram_rdsr();
    fram_read((uint32_t)A, back, (uint32_t)N);       /* at once */
    for (uint32_t i = 0; i < (uint32_t)N; i++) {
        if (back[i] == pat[i]) { imm++; }
    }
    uint32_t wip = fram_wip_us();
    board_delay_ms(20);
    fram_read((uint32_t)LO, buf, (uint32_t)(HI - LO));
    for (uint32_t i = 0; i < (uint32_t)N; i++) {
        if (buf[(A - LO) + i] == pat[i]) { later++; }
    }
    for (uint32_t i = 0; i < (uint32_t)(A - LO); i++) {
        if (buf[i] != g_fram_image[LO + i]) { outside++; }
    }

    console_printf("  write 96 B at 0x%04x, across the page line at 0x%04x:\n",
                   (uint32_t)A, (uint32_t)(A + 32));
    console_printf("    status right after   0x%02x (bit0 %u, WEL %u)\n",
                   (uint32_t)sr_after, (uint32_t)(sr_after & 1U),
                   (uint32_t)((sr_after >> 1) & 1U));
    console_printf("    read back at once    %u/96 match\n", imm);
    console_printf("    busy (bit0) for      %u us\n", wip);
    console_printf("    read back after 20ms %u/96 match, and %u of the 32 "
                   "bytes BEFORE it changed\n", later, outside);

    /* Back as it was, in pieces no write can wrap inside. */
    for (uint32_t a = LO; a < (uint32_t)HI; a += 32U) {
        fram_write(a, &g_fram_image[a], 32U);
        (void)fram_wip_us();
    }
    fram_read((uint32_t)LO, buf, (uint32_t)(HI - LO));
    uint32_t restored = 0U;
    for (uint32_t i = 0; i < (uint32_t)(HI - LO); i++) {
        if (buf[i] == g_fram_image[LO + i]) { restored++; }
    }
    console_printf("    restored 0x%04x..0x%04x: %u/128 as before; "
                   "status 0x%02x, SPI timeouts %u\n",
                   (uint32_t)LO, (uint32_t)(HI - 1), restored,
                   (uint32_t)fram_rdsr(), fram_timeouts());

    int fram = (imm == (uint32_t)N) && (later == (uint32_t)N)
            && (wip == 0U) && (outside == 0U) && ((sr_after & 1U) == 0U);
    console_printf("  VERDICT: %s\n\n", fram
                   ? "behaves as FRAM - written at once, no busy bit, "
                     "no page wrap"
                   : "*** NOT FRAM-LIKE - see the lines above ***");
}
#endif

/*
 * THE TOUCH CONTROLLER'S WAKE-UP, MEASURED - 21/11/2026.
 *
 * Four cold boots of the first panel-DMA image: two found the FT6236 at
 * once, one only on touch_init()'s second attempt (+1.8 s) and one not
 * at all ("no answer at 0x38", touch dead until the next power-on). The
 * two bad ones had SDA stuck low from the 5th or 6th identify read on -
 * read over SWD, see SIGUIENTE.md - and the good ones answered on the
 * 6th: the poll, starting 120 ms after the reset, was reading straight
 * through the part's wake-up, and the read that met the moment its I2C
 * came alive could leave it holding SDA. Nothing then recovered the bus
 * until the second attempt.
 *
 * Two changes, and this says which one does the work: n cycles each of
 *
 *   120 ms, no recovery in the poll   what the boot did until today
 *   120 ms, recovery in the poll      the safety net alone
 *   300 ms, recovery in the poll      the datasheet's Trsi, and the net
 *
 * interleaved so that a drift lands on all three alike. A cycle is the
 * whole of touch_init(): the reset pulse, the wait, the poll, and the
 * second attempt if the first fails. The MCU is warm, which is not a
 * cold boot - but the reset pulse is what the poll races, and it is
 * the same pulse either way.
 *
 * TOUCH_WAKE_TEST is the number of cycles per variant; 0 leaves it out.
 * A good cycle is ~0.2-0.4 s, a bad one up to ~3.6 s.
 */
#ifndef TOUCH_WAKE_TEST
#define TOUCH_WAKE_TEST 0
#endif

static void test_touch_wake(uint32_t n)
{
    static const struct {
        uint32_t    wake_ms;
        int         recover;
        const char *name;
    } k[] = {
        { 120U, 0, "120 ms, no recovery" },
        { 120U, 1, "120 ms + recovery  " },
        { 300U, 1, "300 ms + recovery  " },
    };
    enum { NV = sizeof k / sizeof k[0] };
    uint32_t failed[NV] = { 0U }, stuck[NV] = { 0U }, second[NV] = { 0U };
    uint32_t ms_sum[NV] = { 0U }, ms_max[NV] = { 0U };

    console_printf("\nTest - the touch controller after its reset: %u "
                   "cycles per variant\n", n);
    console_printf("================================================"
                   "==============\n");

    for (uint32_t i = 0; i < n * NV; i++) {
        uint32_t v  = i % NV;

        board_watchdog_feed();

        uint32_t t0 = board_millis();
        int      ok = touch_init_cfg(k[v].wake_ms, k[v].recover);
        uint32_t ms = board_millis() - t0;
        int16_t  lo = touch_dbg_sda_low_at();

        if (!ok)                       { failed[v]++; }
        if (lo >= 0)                   { stuck[v]++; }
        if (touch_dbg_attempts() > (uint8_t)1U) { second[v]++; }
        ms_sum[v] += ms;
        if (ms > ms_max[v]) { ms_max[v] = ms; }

        console_printf("  %2u  %s | %s  reads %3u  SDA low at %3d  "
                       "recoveries %2u  attempts %u  %4u ms\n",
                       i / NV, k[v].name, ok ? "found  " : "*** NO ***",
                       (uint32_t)touch_dbg_tries(), (int32_t)lo,
                       (uint32_t)touch_dbg_recovers(),
                       (uint32_t)touch_dbg_attempts(), ms);
    }

    console_printf("\n  variant              | not found | SDA stuck | "
                   "2nd attempt | mean ms | max ms\n");
    for (uint32_t v = 0; v < NV; v++) {
        console_printf("  %s  |  %2u / %2u  |  %2u / %2u  |   %2u / %2u   "
                       "|  %5u  | %5u\n", k[v].name, failed[v], n,
                       stuck[v], n, second[v], n, ms_sum[v] / n, ms_max[v]);
    }
    console_printf("\n");

    /* And leave it as the boot would have: identified, polling mode. */
    (void)touch_init();
}


/*
 * THE DISPLAY RANGE, in upstream's uncalibrated dB - the same 40..130
 * the spectrum has been drawn with since step 6, named now that three
 * different pieces of the screen have to agree about it. The gutter
 * labels, the colour legend, the trace and the S-meter all read these
 * two numbers, and a spectrum whose axis disagrees with its trace is
 * the worst failure available to something built to measure with.
 */
/*
 * Integer twins first, so the histogram in spec_floor_db() and the
 * drawing code cannot end up describing different ranges. Deriving the
 * floats from the integers rather than writing both out is the point:
 * this file already has one pair of numbers that three pieces of the
 * screen have to agree about, and that is one pair too many to maintain
 * by hand.
 */
#define UI_DB_MIN_I  40
#define UI_DB_MAX_I 130

#define UI_DB_MIN   ((float)UI_DB_MIN_I)
#define UI_DB_MAX   ((float)UI_DB_MAX_I)

/*
 * THE SPECTRUM'S OWN NOISE FLOOR, BY MEDIAN - which turns "the trace
 * looks like porridge" into a number that can be compared between two
 * runs.
 *
 * The median and not the mean, because a spectrum with a carrier in it
 * is exactly the case a mean gets wrong: one bin 50 dB above the rest
 * drags the average up by an amount that depends on the carrier, so the
 * "floor" would move when the signal moved. Half the bins are below the
 * median whatever the other half are doing, and with one carrier and
 * two sidebands out of 512 bins the median is the floor by definition.
 *
 * A histogram rather than a sort: 1 dB buckets over the display range,
 * one pass, 184 bytes of stack, and the answer to the nearest half
 * decibel. A floor measurement does not need better, and a sort of 512
 * floats per frame would cost more than the FFT that produced them.
 */
static float spec_floor_db(const float *db, uint32_t bins)
{
    enum { NB = (UI_DB_MAX_I - UI_DB_MIN_I) + 2 };

    uint16_t hist[NB];
    uint32_t k, acc = 0U;

    for (k = 0; k < (uint32_t)NB; k++) { hist[k] = 0U; }

    for (k = 0; k < bins; k++) {
        float   v = db[k];
        uint32_t b;

        /* Bucket 0 is everything under the range and bucket NB-1
         * everything over it, so nothing is silently dropped and a
         * floor below the display range reports as the bottom rather
         * than as the first bucket that happens to be occupied. */
        if (v < UI_DB_MIN)        { b = 0U; }
        else if (v >= UI_DB_MAX)  { b = (uint32_t)NB - 1U; }
        else { b = 1U + (uint32_t)(v - UI_DB_MIN); }

        hist[b]++;
    }

    for (k = 0; k < (uint32_t)NB; k++) {
        acc += hist[k];
        if (acc * 2U >= bins) { break; }
    }

    if (k == 0U)                     { return UI_DB_MIN; }
    if (k >= (uint32_t)NB - 1U)      { return UI_DB_MAX; }

    return UI_DB_MIN + (float)k - 0.5f;     /* the bucket's centre */
}

/*
 * THE WATERFALL HISTORY, 8 bits a pixel.
 *
 * Indices into the colormap, not colour: 92 x 440 is 40 kB this way and
 * 79 kB as RGB565, and gfx2_wf_blit() converts through the LUT as it
 * pushes. That indirection is not only about memory - it means a change
 * of palette repaints the whole history and not just the new rows.
 *
 * It is a ring. `head` is the OLDEST row, which is the one that goes at
 * the top; a new row is written there and the head then advances, which
 * leaves the newest at the bottom and costs no copying.
 */
static uint8_t s_wf[(uint32_t)SPC_WF_ROWS * (uint32_t)SPC_TRACE_W];
static int16_t s_wf_head;

static ui_top_state_t s_top;
static ui_act_state_t s_act;
static spec_chrome_t  s_chrome;

static char s_freq_txt[16];

/*
 * WHAT THE KNOB MOVES.
 *
 * The tuned frequency has lived in msi001's own plan until now, because
 * nothing in this image ever changed it after startup. A knob changes
 * it, so it needs somewhere to live that the UI and the tuner agree on.
 *
 * THE STEP CYCLES ON A SHORT PRESS, which is upstream's own gesture and
 * is what the ADC bought: ENC1's press arrives through upstream's
 * debounce, and a release shorter than 600 ms moves to the next step.
 *
 * DECADES, AND NOT UPSTREAM'S LIST, deliberately. Upstream has eleven
 * curated steps - 500 Hz for searching in SSB, 9 kHz for mediumwave,
 * 12,5 kHz for marine, 25 kHz for airband - in an X-macro inside the
 * 39 094-line main.c that is not coming yet, together with its labels
 * and its descriptions in two languages. Copying those values here would
 * be the same data written twice, which is the failure this project has
 * been bitten by most; so this scaffold generates what it needs instead.
 * Powers of ten, with the label FORMATTED from the number rather than
 * stored beside it, so there is no second list to keep in step. When
 * upstream's main loop arrives it brings its own table and this goes.
 *
 * 0.33 Hz is the synthesiser's own resolution, so every step here is the
 * hand's limit and not the hardware's.
 */
static uint32_t s_tuned_hz;

/*
 * UPSTREAM'S ELEVEN, FROM 21/11/2026 - and the paragraph above was right
 * about how: not copied. paso_tabla[] is TUNE_STEPS_LISTA itself,
 * generated out of upstream's main.c into bandas_tabla.h by the script
 * that also makes the band table (reglas.mk), labels and descriptions
 * included. The band presets name their step as an index into it, so a
 * band chosen from the menu brings its own step.
 *
 * The FRAM's step_idx changed meaning with it - record version 3; an
 * older record indexes k_old_steps[] and is mapped through its hertz
 * at boot.
 */
#define TUNE_STEP_N     ((uint32_t)N_PASOS)
static const uint32_t k_old_steps[] = { 10U, 100U, 1000U, 10000U, 100000U };

static uint8_t s_step_idx = 3U;         /* 1 kHz; checked at boot */

/* The index of the step nearest `hz` - for the default, and for an old
 * record's index. */
static uint8_t step_index_of(uint32_t hz)
{
    uint32_t i, best = 0U, d_best = UINT32_MAX;

    for (i = 0U; i < TUNE_STEP_N; i++) {
        uint32_t d = (paso_tabla[i].hz > hz) ? (uint32_t)(paso_tabla[i].hz - hz)
                                             : (uint32_t)(hz - paso_tabla[i].hz);
        if (d < d_best) { d_best = d; best = i; }
    }
    return (uint8_t)best;
}
static char    s_step_txt[12];

/*
 * WHAT THE SECOND KNOB DOES, AND WHY IT IS THIS.
 *
 * The small knob was free - upstream has one encoder and this board has
 * two - and the honest answer to "what should it adjust" is that most of
 * the obvious candidates have nothing behind them yet. Volume, mode and
 * bandwidth all sit downstream of a demodulator, and this image has
 * none: the only sound it has ever made is a test tone. A control
 * labelled for something that does not exist is worse than a control
 * that does nothing, because it looks like it works.
 *
 * RF gain does exist, is measured, and is the one setting that matters
 * on a receiver whose AGC is still a stub: too much and the converter
 * clips, too little and the signal is in the noise. It is also VISIBLE
 * WITHOUT AUDIO - the ADC fill percentage, the noise floor and the SNR
 * in the status line all move with it, and the trace lifts and drops.
 *
 * So: turning adjusts baseband gain reduction, 0 to 59 dB, which is the
 * range msi001_set_gain() takes. Pressing toggles the preamp. Both are
 * real controls with measured effects, neither is invented, and when a
 * demodulator arrives and the knob wants to be volume instead, this is
 * the one place that changes.
 */
/* RF_GR_STEP is already taken, by rf_autorange()'s search stride - a
 * different quantity that happens to share the obvious name. This one
 * is dB per detent of the hand. */
#define RF_GR_MAX       59U
#define RF_KNOB_STEP    1

static uint8_t s_rf_gr;             /* dB of gain reduction, from autorange */
static int     s_rf_preamp = 1;
static char    s_att_txt[10];

/*
 * WHAT THE SMALL KNOB ADJUSTS, now that there is something to listen to.
 *
 * It was RF gain, which was the right answer when the radio made no
 * sound: gain was the only setting that did anything, and it was
 * visible without audio. That changed the moment the demodulator
 * worked - the first thing a listener reaches for is the volume, and
 * the first thing this radio needed was one, because -18 dB into a
 * small speaker is loud and there was no way down.
 *
 * So the knob is volume and its BUTTON swaps it to RF gain. Both still
 * reachable, the one you want far more often under the hand, and the
 * pill says which it is - a knob whose meaning changes has to say so.
 */
#define VOL_MIN_DB      (-60)
#define VOL_MAX_DB      (0)

static int  s_vol_db = -24;
static int  s_knob2_rf;             /* 0 = volume, 1 = RF gain */
static char s_knob2_txt[12];

#define TUNE_STEP_HZ    ((uint32_t)paso_tabla[s_step_idx].hz)
#define TUNE_MIN_HZ     ((uint32_t)RADIO_TUNE_MIN_HZ)  /* src/menus.h */
#define TUNE_MAX_HZ     ((uint32_t)RADIO_TUNE_MAX_HZ)

/*
 * THE DIAL, THE TUNER, AND A FIXED 12 kHz BETWEEN THEM.
 *
 * s_tuned_hz is the DIAL - the frequency being listened to. The MSi001
 * is always programmed AM_IF_HZ below it (am.h), on every detent, so the
 * station arrives 12 kHz from the oscillator's leakage and am.c rotates
 * it back onto zero after subtracting the leakage. The offset never
 * changes: there is no window and no re-park, which is what made the
 * first low-IF version click every few detents.
 *
 * ZERO IF WAS TRIED FOR ONE SESSION AND NEVER RAN. At zero IF the AM
 * carrier and the leakage are the same DC term, and subtracting one
 * subtracts the other - a model of am.c put the second harmonic above
 * the tone with the carrier half a hertz from the oscillator. am.c has
 * the numbers and what the RE report actually says.
 *
 * THE PANADAPTER STAYS CENTRED ON THE DIAL, which costs width. The
 * captured baseband is centred on the TUNER, so a display centred on
 * the dial can only show 96 kHz minus the offset each way: bins
 * [PAN_LO, PAN_BINS) of the transform, 168 of the 192 kHz. The offset
 * is a whole number of bins, so the dial falls on a bin and not between
 * two. The leakage appears 12 kHz left of centre - which is where the
 * stock firmware's waterfall shows its own.
 */
#define PAN_BIN_HZ      (AM_FS_HZ / PAN_BINS)               /* 375 Hz   */
#define PAN_IF_BINS     (AM_IF_HZ / PAN_BIN_HZ)             /* 32       */
#define PAN_LO          (2U * PAN_IF_BINS)                  /* first: 64 */
#define PAN_SHOWN       (PAN_BINS - 2U * PAN_IF_BINS)       /* 448 bins */
#define PAN_SHOWN_HZ    (PAN_SHOWN * PAN_BIN_HZ)            /* 168 kHz  */

_Static_assert(PAN_BIN_HZ * PAN_BINS == AM_FS_HZ,
               "the bin width must divide the sample rate");
_Static_assert(PAN_IF_BINS * PAN_BIN_HZ == AM_IF_HZ,
               "the IF must be a whole number of bins, or the dial is "
               "drawn between two");
/* The window [PAN_LO, PAN_BINS) is centred on bin PAN_BINS/2 +
 * PAN_IF_BINS - DC is db[PAN_BINS/2] (fft.c), the station is
 * PAN_IF_BINS above it - and that is what puts the dial mid-trace. */
_Static_assert(PAN_LO + PAN_SHOWN / 2U == PAN_BINS / 2U + PAN_IF_BINS,
               "the shown window must be centred on the dial");

/*
 * THE OFFSET IS THE MODE'S SINCE WFM - 21/11/2026, the owner's choice.
 *
 * Upstream's WFM path takes the station at 0 Hz: no down-mix, no channel
 * filter, the discriminator straight on the raw +/-96 kHz (demod_am.h,
 * its WFM note: "the local oscillator exactly on frequency"). A 200 kHz
 * broadcast channel 12 kHz off the middle of a 192 kHz window loses its
 * top edge to the codec's anti-alias filter, so in WFM the TUNER goes on
 * the dial, as upstream's does, and the leakage sits under the carrier -
 * which a discriminator does not mind the way an envelope detector does.
 * Every other mode keeps the 12 kHz (am.h).
 *
 * So the dial-to-tuner offset is a variable, and with it the window of
 * bins the trace and the waterfall show: still the 448 centred on the
 * dial, which at 0 Hz is centred on DC.
 */
#define PAN_LO_WFM      (PAN_BINS / 2U - PAN_SHOWN / 2U)    /* 32       */

static uint32_t s_lo_off = AM_IF_HZ;    /* dial minus tuner */
static uint32_t s_pan_lo = PAN_LO;      /* first bin shown */

static uint32_t lo_off_for(int mode)
{
    return (mode == (int)DEMOD_MODE_WFM) ? 0U : AM_IF_HZ;
}

/* am_probe()'s MIRROR side, with nothing there, read at most 118 over
 * two runs on 19/11/2026 (no generator); a verdict needs one side over
 * three times that. The STATION side is not a noise figure: in the same
 * runs it carried a weak, fading signal within a few hertz of
 * 10.000 MHz, up to 625. MEASURED ONCE, at whatever gain rf_autorange()
 * chose - trap 13 on constants that were measured once. */
#define PROBE_FLOOR     400.0f

static char s_clock_txt[8];

/*
 * The ATT chip shows the gain reduction, which is what it is: dB taken
 * out of the signal before the converter. "OFF" at zero.
 *
 * IN UPSTREAM'S OWN NOTATION, three characters - its values are "OFF",
 * "-06", "-12" - and that is not cosmetic. ui_top.c computes the whole
 * status band's layout from these strings, and the knob pill gets what
 * is left over (see kw_max there), so a wider chip is a narrower pill.
 * "48 dB" fitted but took five characters where the layout was designed
 * around three.
 *
 * WHAT THIS CHIP DOES NOT SAY is the preamp. The BGA614 on PE0 is worth
 * a measured +15.1 dB (BOARD.md section 4) and its button is the small
 * knob's, so the radio can be 15 dB less sensitive with the chip
 * reading the same number. Folding the two together was considered and
 * rejected: they are not interchangeable - one is removed ahead of the
 * mixer and one behind it, which is a different noise figure for the
 * same total - and a chip that showed 59 for both "59 at baseband" and
 * "44 plus no preamp" would disagree with the knob in the hand.
 *
 * So the preamp goes to the console and is a KNOWN MISSING INDICATOR,
 * written down rather than papered over with a symbol this UI has no
 * convention for.
 */
static void rf_att_fmt(void)
{
    uint32_t n = s_rf_gr;

    if (n == 0U) {
        s_att_txt[0] = 'O'; s_att_txt[1] = 'F';
        s_att_txt[2] = 'F'; s_att_txt[3] = '\0';
        return;
    }

    if (n > 99U) { n = 99U; }

    s_att_txt[0] = '-';
    s_att_txt[1] = (char)('0' + (n / 10U));
    s_att_txt[2] = (char)('0' + (n % 10U));
    s_att_txt[3] = '\0';
}

/* The small knob's pill: what it adjusts and where that is now. A knob
 * whose meaning changes with a press has to say which meaning is in
 * force, or the next turn is a guess. */
static void knob2_fmt(void)
{
    int      v = s_knob2_rf ? (int)s_rf_gr : s_vol_db;
    uint32_t n = (uint32_t)((v < 0) ? -v : v);
    char     dig[4];
    uint32_t nd = 0U, w = 0U;

    s_knob2_txt[w++] = s_knob2_rf ? 'R' : 'V';
    s_knob2_txt[w++] = ' ';
    if (v < 0) { s_knob2_txt[w++] = '-'; }

    do { dig[nd++] = (char)('0' + (n % 10U)); n /= 10U; } while (n);
    while (nd > 0U) { s_knob2_txt[w++] = dig[--nd]; }

    s_knob2_txt[w] = '\0';
}

/* "10 Hz", "1 kHz", "100 kHz" - from the number, so the label cannot
 * disagree with the step it names. */
static void tune_step_fmt(void)
{
    const char *l = paso_tabla[s_step_idx].rotulo;
    uint32_t    w = 0U;

    /*
     * "S" FOR STEP, and the letter is not decoration. One pill shows
     * whichever knob moved last, so without a letter the same box reads
     * "1 kHz" for the tuning step and "-24" for the volume and the
     * user cannot tell which knob they are looking at - which is
     * exactly what happened. The rest is upstream's own label now
     * ("12,5 kHz", "1 MHz"), not formatted here.
     */
    s_step_txt[w++] = 'S';
    s_step_txt[w++] = ' ';
    while (*l != '\0' && w + 1U < sizeof s_step_txt) { s_step_txt[w++] = *l++; }
    s_step_txt[w] = '\0';
}

/*
 * The frequency, formatted the way upstream's tune_freq_format_khz()
 * does it: kilohertz, two decimals, a dot every three digits and a
 * comma before the decimals. 10 000 000 Hz becomes "10.000,00", and
 * ui_top.c draws "kHz" after it.
 *
 * Written here rather than borrowed because the upstream function is
 * inside the 39 094-line main.c that is not coming. It is eleven lines
 * and it is a seam left by main.c, not by the hardware - the same kind
 * of seam as g_pal.
 */
static void ui_fmt_khz(uint32_t hz, char *out, uint32_t n)
{
    uint32_t khz  = hz / 1000U;
    uint32_t cent = (hz % 1000U) / 10U;
    char     dig[12];
    uint32_t nd = 0U;
    uint32_t w  = 0U;

    do { dig[nd++] = (char)('0' + (khz % 10U)); khz /= 10U; } while (khz);

    while (nd > 0U && w + 4U < n) {
        nd--;
        out[w++] = dig[nd];
        if (nd > 0U && (nd % 3U) == 0U) { out[w++] = '.'; }
    }
    out[w++] = ',';
    out[w++] = (char)('0' + (cent / 10U));
    out[w++] = (char)('0' + (cent % 10U));
    out[w]   = '\0';
}

/*
 * S-UNITS FROM THE PEAK, AND WHAT THAT IS WORTH.
 *
 * The bar moves with a real measurement - the strongest bin of the
 * finished spectrum - but the SCALE is arbitrary: S1 is the bottom of
 * the display range and S9 the top, linearly, in upstream's
 * uncalibrated dB. It is not a calibrated S-meter and must not be read
 * as one.
 *
 * It is here anyway because a bar that tracks the signal proves the
 * status band is being repainted from live data, which is one of the
 * things this step exists to find out. The calibration waits on the
 * mixer DC offset, which is the worst number on the board and is step
 * 8's first job.
 */
static void ui_s_units(float db, uint8_t *units, uint8_t *over)
{
    float f = (db - UI_DB_MIN) * 9.0f / (UI_DB_MAX - UI_DB_MIN);

    *over = 0U;
    if (f < 0.0f) { *units = 0U; return; }
    if (f > 9.0f) {
        /* Above the top of the range: S9 + however many dB are left,
         * in the same uncalibrated dB as everything else here. */
        *units = 10U;
        f = db - UI_DB_MAX;
        *over = (uint8_t)(f > 60.0f ? 60.0f : f);
        return;
    }
    *units = (uint8_t)f;
}

/* Uptime as mm:ss, in the slot where a clock will go. See the note at
 * the top of this section: it counts up, and it is labelled as such on
 * the console, because a wrong time of day is a lie and an uptime is
 * not. */
static void ui_fmt_uptime(uint32_t ms, char *out)
{
    uint32_t s = (ms / 1000U) % 3600U;

    out[0] = (char)('0' + ((s / 60U) / 10U));
    out[1] = (char)('0' + ((s / 60U) % 10U));
    out[2] = ':';
    out[3] = (char)('0' + ((s % 60U) / 10U));
    out[4] = (char)('0' + ((s % 60U) % 10U));
    out[5] = '\0';
}

/* The six buttons, in upstream's own order - the same k_act_slots[]
 * that sim/principal.c lists. Nothing behind them yet: they are drawn
 * and they are hit-tested, which is what tells us the geometry at 480
 * px is reachable with a finger. */
/* Upstream's k_act_slots[] names, T(es, en) - except the first: upstream
 * has Func there, this bar has Bandas. Resolved in act_values(), on
 * every draw of the bar, so a change of language needs no list of its
 * own to refresh. */
static const texto_t k_act_names[UI_ACT_N] = {
    T("Bandas", "Bands"), T("Modo", "Mode"), T("Paso", "Step"),
    T("Volumen", "Volume"), T("Ruido", "Noise"), T("Ajustes", "Settings")
};

static void ui_init_state(uint32_t tuned_hz)
{
    uint32_t i;

    s_tuned_hz = tuned_hz;
    ui_fmt_khz(tuned_hz, s_freq_txt, sizeof s_freq_txt);
    ui_fmt_uptime(board_millis(), s_clock_txt);

    s_top.freq       = s_freq_txt;
    s_top.freq_digit = -1;          /* nothing is tuning: no cursor */
    s_top.mode       = "AM";
    s_top.band       = tr("Bandas", "Bands");   /* band_pill_sync() sets it */
    s_top.en_banda   = 0U;
    s_top.clock      = s_clock_txt;
    /*
     * THE BATTERY READS FOR REAL NOW, and the pill has never had a
     * number in it before - on this layout it was drawn off-screen and
     * clipped away entirely, see PROCEDENCIA.md.
     *
     * The voltage is measured: ADC1 channel 16, the midpoint of two 10k
     * resistors across the cell, against a VREF+ that was itself
     * measured against a meter. The PERCENTAGE is a straight line
     * between 3.3 and 4.2 V and is the weakest thing on this screen -
     * see adc_bat_pct(), which says why, and note that upstream's own
     * battery.c does exactly the same.
     */
    s_top.batt_pct   = adc_bat_pct();
    s_top.agc        = "OFF";
    rf_att_fmt();
    s_top.att        = s_att_txt;   /* the gain reduction, which is real */
    s_top.dbm_valid  = 0U;          /* the dB figure is not calibrated */
    s_top.knob       = UI_KNOB_TUNE;
    /*
     * The pill named no adjustment while there was no encoder driver.
     * There is one now - upstream's own, on RENC1, the big knob - so it
     * names the one the knob actually makes and the step it makes it in,
     * and a short press of the knob's button changes that step.
     */
    s_top.knob_name  = 0;           /* 0 = use the name for `knob` */
    tune_step_fmt();
    knob2_fmt();
    /* The pill shows the SMALL knob, because that is the one whose
     * meaning changes under a press. The big knob's step is a number
     * nobody has to look up while turning it. */
    s_top.knob_value = s_knob2_txt;

    for (i = 0U; i < (uint32_t)UI_ACT_N; i++) {
        s_act.name[i]   = k_act_names[i][idioma()];
        s_act.value[i]  = 0;
        s_act.active[i] = 0U;
    }
    s_act.pressed = -1;

    s_chrome.db_min    = UI_DB_MIN;
    s_chrome.db_max    = UI_DB_MAX;
    /* CENTRED ON THE DIAL, and only as wide as the baseband reaches
     * either side of it - PAN_SHOWN of the transform's bins, 168 kHz.
     * The loop draws exactly that window, so ruler and trace agree. */
    s_chrome.center_hz = tuned_hz;
    s_chrome.span_hz   = PAN_SHOWN_HZ;
    /*
     * ZERO: the demodulation point is the dial, and the dial is the
     * middle of the window. The 12 kHz offset lives entirely between
     * the tuner and am.c; the display never sees it except as the
     * leakage line left of centre.
     */
    s_chrome.demod_px  = 0;
    s_chrome.band      = SPC_BAND_NONE;   /* no audio filter yet */
    s_chrome.band_hz   = 0U;
    s_chrome.en_banda  = 0U;
    s_chrome.cmap      = spectrum_colormap_lut();
}

/*
 * THE BACKGROUND, AND WHY IT IS NOT OPTIONAL.
 *
 * ili9488_init() resets the panel and sends the init sequence; it
 * never clears GRAM, so after a reset every pixel holds whatever the
 * RAM powered up with. Nothing in the UI covers the whole screen -
 * each module paints its own band and leaves the gaps alone - so
 * without this the gaps show uninitialised panel memory.
 *
 * THERE ARE FOUR GAPS AND THEY ARE ALL VISIBLE, which is how this was
 * found: a photograph of the radio next to the host render.
 *
 *     rows  50..53    between SPC_Y and the top of the trace
 *     rows 174..175   between the bottom of the trace and the ruler
 *     rows 192..193   between the ruler and the waterfall
 *     cols 474..479   the right margin, the full height of both
 *
 * They came out as dotted grey-black lines along the top and bottom
 * of the spectrum and a bright stripe down the right of the spectrum
 * AND the waterfall - which is the giveaway, because one stripe
 * crossing two modules that share no drawing code is not a bug in
 * either of them.
 *
 * The host render could not show this: sim/principal.c opens with
 * lcd_sim_reset() and its own fondo(), so on the PC the gaps are
 * always the surface colour. This is the first thing in the port that
 * the PNG was structurally unable to predict.
 */
static void ui_fondo(gfx2_surf_t *s, void *ctx)
{
    (void)ctx;
    gfx2_fill(s, 0, 0, GFX2_W, GFX2_H, gfx2_rgb(PAL_SURF_0));
}

/*
 * ======================================================================
 * ASKING FOR A HAND, ON THE SCREEN THE HAND IS LOOKING AT
 * ======================================================================
 *
 * Six tests in this image need somebody to turn a knob, press a button
 * or listen, and every one of them asked over the UART - which the
 * person holding the radio cannot see. They were following instructions
 * relayed from the other end of a debugger while the panel in their
 * hands stayed dark, and they said so: "I don't see any requests on the
 * receiver's screen, or do they go over UART? I'm confused."
 *
 * Entirely fair. A bench test that needs a human is a user interface,
 * and it was on the wrong device. The panel now comes up BEFORE the
 * tests that need a hand rather than after them, and these two functions
 * put the question where it belongs. The console still gets everything,
 * because that is where the measurements are read afterwards.
 */
static const char *s_prompt[4];

static void ui_prompt_draw(gfx2_surf_t *s, void *ctx)
{
    (void)ctx;

    gfx2_fill(s, 0, 0, GFX2_W, GFX2_H, gfx2_rgb(PAL_SURF_0));

    int16_t y = (int16_t)(GFX2_H / 2 - 46);

    for (uint32_t i = 0; i < 4U; i++) {
        if (s_prompt[i] == 0) { continue; }

        gfx2_text_in(s, 0, y, GFX2_W, s_prompt[i], &font_ui_14b,
                     gfx2_rgb(i == 0U ? PAL_ACCENT : PAL_INK),
                     GFX2_ALIGN_C);
        y = (int16_t)(y + (i == 0U ? 34 : 24));
    }
}

/* Non-zero once the panel can be drawn on. Before that these are no-ops
 * rather than a fault: the early tests run on a dark screen and say so
 * on the console, which is better than a hang in the drawing code. */
static int s_panel_up;

static void ui_prompt(const char *title, const char *a, const char *b,
                      const char *c)
{
    if (!s_panel_up) { return; }

    s_prompt[0] = title;
    s_prompt[1] = a;
    s_prompt[2] = b;
    s_prompt[3] = c;

    gfx2_render_screen(ui_prompt_draw, 0);
}

/*
 * REMEMBERING THE DIAL, THE VOLUME, THE STEP - 21/11/2026 - AND, SINCE
 * THE SAME EVENING, THE ATT AND THE PREAMP.
 *
 * settings.c keeps them in the FRAM; this decides when. Once per frame
 * settings_tick() compares the three with the last values it saw: a
 * change restarts a one-second clock, and when the clock runs out with
 * the values differing from what the FRAM holds, they are saved. So a
 * knob being turned is saved once, when the hand stops, and the power
 * button - held 3 s to switch off - gives any last change time to land.
 * The FRAM would take a write every frame without noticing; the second
 * is for the console, which prints each save.
 */
#define SETTINGS_SAVE_MS    1000U

/*
 * NOT IN A RETUNE TEST BUILD. The test needs the generator on the dial
 * and a measured gain, and it moves the dial by itself for a minute -
 * restoring the person's dial and ATT would aim it at the wrong thing,
 * and saving its moves would overwrite what the person left in the
 * FRAM. So such an image neither reads nor writes the settings.
 */
#define SETTINGS_LIVE       (RETUNE_TEST == 0)

static settings_t s_set_saved;      /* what the FRAM holds, or the boot's */
static settings_t s_set_seen;       /* the values a frame ago */
static uint32_t   s_set_changed_at;

static settings_t s_boot_set;       /* read once at boot, before the RF */
static int        s_boot_have;

static int settings_same(const settings_t *a, const settings_t *b)
{
    return a->dial_hz == b->dial_hz && a->vol_db == b->vol_db
        && a->step_idx == b->step_idx && a->rf_gr == b->rf_gr
        && a->preamp == b->preamp && a->mode == b->mode
        && a->aj_agc == b->aj_agc && a->aj_estilo == b->aj_estilo
        && a->aj_flags == b->aj_flags && a->aj_tema == b->aj_tema
        && a->aj_paleta == b->aj_paleta && a->aj_bw == b->aj_bw
        && a->aj_sql == b->aj_sql && a->aj_nr == b->aj_nr
        && a->aj_suav == b->aj_suav && a->aj_wfm_bw == b->aj_wfm_bw
        && a->aj_idioma == b->aj_idioma && a->aj_brillo == b->aj_brillo
        && a->aj_estereo == b->aj_estereo && a->cw_bw == b->cw_bw
        && a->rtty_shift == b->rtty_shift && a->rtty_baud == b->rtty_baud
        && a->rtty_auto == b->rtty_auto && a->cw_aprende == b->cw_aprende;
}

static void settings_now(settings_t *out)
{
    out->dial_hz  = s_tuned_hz;
    out->vol_db   = (int8_t)s_vol_db;
    out->step_idx = s_step_idx;
    out->rf_gr    = s_rf_gr;
    out->preamp   = (uint8_t)(s_rf_preamp ? 1U : 0U);
    out->has_rf   = 1U;
    out->mode     = (uint8_t)radio_mode_get();     /* CW is 6: menus.h */
    out->cw_bw    = (uint8_t)((demod_am_get_cw_bw_hz() + 5.0f) / 10.0f);
    out->rtty_shift = (uint8_t)((rtty_get_shift_hz() + 2.5f) / 5.0f);
    out->rtty_baud  = (uint8_t)(digi_rtty_baud_idx() + 1U);
    out->rtty_auto  = (uint8_t)(rtty_auto_get() ? 2U : 1U);
    out->cw_aprende = (uint8_t)(cw_get_aprende() ? 2U : 1U);
    out->has_mode = 1U;
    menu_ajustes_get(out);              /* the settings screen's values */
}

static void settings_tick(void)
{
    settings_t cur;

    if (!SETTINGS_LIVE) { return; }

    settings_now(&cur);
    if (!settings_same(&cur, &s_set_seen)) {
        s_set_seen       = cur;
        s_set_changed_at = board_millis();
    }
    if (settings_same(&s_set_seen, &s_set_saved)
        || (board_millis() - s_set_changed_at) < SETTINGS_SAVE_MS) {
        return;
    }

    if (settings_save(&s_set_seen)) {
        s_set_saved = s_set_seen;
        console_printf("  saved: dial %u Hz, %s, volume %d dB, step %u Hz, "
                       "ATT %u dB, preamp %s (FRAM slot %d, seq %u)\n",
                       s_set_saved.dial_hz,
                       radio_mode_name((int)s_set_saved.mode),
                       (int32_t)s_set_saved.vol_db,
                       (uint32_t)paso_tabla[s_set_saved.step_idx].hz,
                       (uint32_t)s_set_saved.rf_gr,
                       s_set_saved.preamp ? "ON" : "OFF",
                       settings_slot(), settings_seq());
    } else {
        /* The other slot still holds the last good record. Try again
         * in a second rather than every frame. */
        s_set_changed_at = board_millis();
        console_printf("  *** settings save did not read back (%u so "
                       "far) ***\n", settings_save_errors());
    }
}

/*
 * THE DIAL LOST UP TO 1 Hz A RETUNE - 21/11/2026.
 *
 * Seen in the FRAM saves: 10 000 000 Hz walked in 1 kHz steps came to
 * 9 990 997, then in 100 Hz steps to 9 990 093. dial_tune() takes the
 * next dial from s_tuned_hz, which is msi001_plan()'s f_actual plus the
 * IF, and f_actual was the synthesiser's true frequency TRUNCATED: the
 * synthesiser lands within half its ~0.33 Hz step of the request, and
 * every time it landed below, the dial lost a hertz.
 *
 * MSI_ROUND_TEST runs the planner - arithmetic only, the synthesiser is
 * not touched - over ~30 000 dials across 100 kHz..30 MHz, and walks
 * the dial the way the knob does: 1 000 detents each of 1 kHz, 100 Hz
 * and 10 Hz from 10 MHz, up and back down. Every walk should end where
 * it started; every f_actual should equal its request.
 */
#ifndef MSI_ROUND_TEST
#define MSI_ROUND_TEST 0
#endif

#if MSI_ROUND_TEST
static uint32_t msi_walk(msi_band_t band, uint32_t dial, int32_t step,
                         uint32_t n)
{
    msi_plan_t p;

    for (uint32_t i = 0; i < n; i++) {
        msi001_plan((uint32_t)((int32_t)dial + step) - AM_IF_HZ, band, &p);
        if (!p.ok) { return 0U; }
        dial = p.f_actual + AM_IF_HZ;
    }
    return dial;
}

static void test_msi_round(void)
{
    msi_band_t band = msi001_last_plan()->band;
    msi_plan_t p;
    uint32_t   n = 0U, off = 0U, refused = 0U;
    int32_t    emin = 0, emax = 0;

    console_printf("\nTest - the dial's arithmetic (MSI_ROUND_TEST), band "
                   "%u\n", (uint32_t)band);
    console_printf("============================================\n");

    for (uint32_t f = 100000U; f <= 30000000U; f += 997U) {
        msi001_plan(f, band, &p);
        if (!p.ok) { refused++; continue; }
        n++;
        int32_t e = (int32_t)(p.f_actual - f);
        if (e != 0) { off++; }
        if (e < emin) { emin = e; }
        if (e > emax) { emax = e; }
    }
    console_printf("  %u dials: f_actual != request on %u, error %d..%d Hz"
                   "; refused %u; synth step %u uHz\n", n, off, emin, emax,
                   refused, p.step_mhz_e6);

    static const int32_t k_step[] = { 1000, 100, 10 };
    for (uint32_t k = 0; k < 3U; k++) {
        uint32_t up   = msi_walk(band, 10000000U, k_step[k], 1000U);
        uint32_t back = msi_walk(band, up, -k_step[k], 1000U);
        console_printf("  1000 x %+5d Hz from 10000000: %u (want %u), "
                       "and back: %u\n", k_step[k], up,
                       (uint32_t)(10000000 + 1000 * k_step[k]), back);
    }
    console_printf("\n");
}
#endif

/*
 * THE ONE WAY TO MOVE THE DIAL - the knob and the retune test both come
 * through here, so the test measures the path the hand uses rather than
 * a copy of it.
 *
 * The tuner goes AM_IF_HZ below the dial (am.h). No mute: there was an
 * am_resettle() call here and it measured worse than nothing (am.h).
 *
 * THE DIAL MOVES ONLY IF THE TUNER DID. The knob's old code wrote the
 * new frequency into s_tuned_hz before asking the synthesiser, so a
 * refused retune left the number on the screen somewhere the hardware
 * never went. Returns 1 if the tuner moved.
 */
static void band_pill_sync(void);

/*
 * RDS - 22/11/2026. Upstream's decoder (rds.c, rds_pi.c, compiled in
 * place) is fed by its own WFM hook (demod_am.c: rds_feed() on the
 * discriminator's output whenever rds_activo()), and this is upstream's
 * rds_marq_sync() / rds_marq_poll() / rds_reinicia() from its main.c, on
 * this port's s_top: the header's strip shows the station's name - the
 * network from its PI code if rds_pi.c knows it, else the PS, else the
 * PI in hex - over a line that scrolls when it does not fit: traffic, the
 * programme type, the radiotext.
 *
 * Started on entering WFM and stopped on leaving it (upstream has a
 * setting for it; here WFM is where RDS is, so it is simply on), and
 * restarted on every retune: "a station name left over from the last
 * station is a screen that lies" (upstream).
 *
 * Not taken: setting the clock from RDS (group 4A) - this port's clock is
 * an uptime counter with nothing to set yet.
 */
static char     s_rds_txt[176], s_rds_txt_ant[176];
static char     s_rds_nom[20], s_rds_nom_ant[20];
static int16_t  s_rds_off, s_rds_txt_w;
static uint32_t s_rds_freq;             /* the dial the decoder was started on */

#define RDS_MARQ_PX  2                  /* upstream's: 2 px a step */

static uint8_t rds_txt_pon(char *b, uint8_t i, const char *t)
{
    while (*t != '\0' && i < (uint8_t)(sizeof s_rds_txt - 1U)) { b[i++] = *t++; }
    return i;
}

/* Upstream's rds_pty_txt(), the table as it is there. 0 = undefined. */
static const char *rds_pty_txt(uint8_t pty)
{
    static const texto_t k_pty[32] = {
        T(0, 0),
        T("Noticias", "News"),                  T("Actualidad", "Current affairs"),
        T("Informacion", "Information"),        T("Deportes", "Sport"),
        T("Educacion", "Education"),            T("Teatro", "Drama"),
        T("Cultura", "Culture"),                T("Ciencia", "Science"),
        T("Variedades", "Varied"),              T("Musica pop", "Pop music"),
        T("Musica rock", "Rock music"),         T("Musica ligera", "Easy listening"),
        T("Clasica ligera", "Light classical"), T("Clasica", "Serious classical"),
        T("Otra musica", "Other music"),        T("El tiempo", "Weather"),
        T("Economia", "Finance"),               T("Infantil", "Children's"),
        T("Temas sociales", "Social affairs"),  T("Religion", "Religion"),
        T("Participacion", "Phone-in"),         T("Viajes", "Travel"),
        T("Ocio", "Leisure"),                   T("Musica jazz", "Jazz music"),
        T("Musica country", "Country music"),   T("Musica nacional", "National music"),
        T("Melodias de siempre", "Oldies music"), T("Musica folk", "Folk music"),
        T("Documental", "Documentary"),         T("Prueba de alarma", "Alarm test"),
        T("ALARMA", "Alarm")
    };

    if (pty > 31U || k_pty[pty][0] == 0) { return (const char *)0; }
    return k_pty[pty][idioma()];
}

/* What the strip says, from what the decoder knows: upstream's
 * rds_marq_sync() without the clock. */
static void rds_marq_sync(void)
{
    rds_info_t in;
    uint8_t    i = 0U, j;

    rds_info(&in);
    if (in.tp && in.ta) {
        i = rds_txt_pon(s_rds_txt, i, tr("TRAFICO", "TRAFFIC"));
    }
    if (in.tiene_pty) {
        const char *t = rds_pty_txt(in.pty);

        if (t != 0) {
            if (i > 0U) { i = rds_txt_pon(s_rds_txt, i, "   "); }
            i = rds_txt_pon(s_rds_txt, i, t);
        }
    }
    if (in.tiene_rt) {
        uint8_t fin;

        if (i > 0U) { i = rds_txt_pon(s_rds_txt, i, "   "); }
        fin = i;
        for (j = 0U; j < 64U && i < (uint8_t)(sizeof s_rds_txt - 8U); j++) {
            s_rds_txt[i++] = in.rt[j];
            if (in.rt[j] != ' ') { fin = i; }
        }
        i = fin;                        /* the radiotext's trailing blanks out */
    }
    if (i > 0U) { i = rds_txt_pon(s_rds_txt, i, "     "); }   /* the gap before it comes round */
    s_rds_txt[i] = '\0';

    {
        const char *n = in.tiene_pi ? rds_pi_nombre(in.pi) : 0;
        uint8_t     k = 0U;

        if (n) {
            while (n[k] != '\0' && k < (uint8_t)(sizeof s_rds_nom - 1U)) { s_rds_nom[k] = n[k]; k++; }
        } else if (in.tiene_ps) {
            for (k = 0U; k < 8U; k++) { s_rds_nom[k] = in.ps[k]; }
            while (k > 0U && s_rds_nom[k - 1U] == ' ') { k--; }
        } else if (in.tiene_pi) {
            static const char hx[] = "0123456789ABCDEF";

            s_rds_nom[k++] = hx[(in.pi >> 12) & 0xFU];
            s_rds_nom[k++] = hx[(in.pi >> 8) & 0xFU];
            s_rds_nom[k++] = hx[(in.pi >> 4) & 0xFU];
            s_rds_nom[k++] = hx[in.pi & 0xFU];
        }
        s_rds_nom[k] = '\0';
    }
}

/* Once a pass of the loop - a frame, ~45 ms, where upstream's is 40 ms -
 * in WFM: the strip is repainted only when its text changed or it has to
 * scroll, and only the strip (ui_top_draw_rds(), its own band). */
static void rds_marq_poll(void)
{
    if (!rds_activo() || demod_am_get_mode() != DEMOD_MODE_WFM) { return; }

    rds_marq_sync();
    if (strcmp(s_rds_txt, s_rds_txt_ant) == 0 && strcmp(s_rds_nom, s_rds_nom_ant) == 0
        && s_rds_txt_w + 8 <= ui_top_rds_ancho()) {
        return;
    }
    if (strcmp(s_rds_nom, s_rds_nom_ant) != 0) {
        strcpy(s_rds_nom_ant, s_rds_nom);
        console_printf("  RDS: \"%s\"\n", s_rds_nom);
    }
    if (strcmp(s_rds_txt, s_rds_txt_ant) != 0) {
        strcpy(s_rds_txt_ant, s_rds_txt);
        s_rds_txt_w = ui_top_rds_texto_w(s_rds_txt);
        if (s_rds_off > (int16_t)(s_rds_txt_w + 8)) { s_rds_off = 0; }
    }
    s_top.rds        = s_rds_txt;
    s_top.rds_off    = s_rds_off;
    s_top.rds_nombre = s_rds_nom;
    ui_top_draw_rds(&s_top);

    if (s_rds_txt_w + 8 > ui_top_rds_ancho()) {
        s_rds_off = (int16_t)(s_rds_off + RDS_MARQ_PX);
        if (s_rds_off > (int16_t)(s_rds_txt_w + 8)) {
            s_rds_off = (int16_t)(-ui_top_rds_ancho());       /* back in from the right */
        }
    } else {
        s_rds_off = 0;
    }
}

/*
 * CW - step 42, 22/11/2026. Upstream's Morse decoder, cw.c, fed by
 * upstream's demod_am.c from its USB branch (the 12 kHz audio before the
 * NR, cw_process_chunks()) once cw_get_enabled() says so; demod_am_init()
 * already set its tone and its band-pass to CONFIG_CW_PITCH_HZ, 700 Hz.
 *
 * THE TEXT goes to the screen's panel (src/digi.c) and to the console,
 * the way upstream's cw_poll()/rtty_poll() also log it ("cw: ...").
 * Upstream logs a line when it fills or at a line break, and Morse has
 * no line breaks - so a pause of CW_LINEA_MS flushes it too, or a slow
 * station would sit unseen in the buffer.
 *
 * RTTY since step 43, the same way: upstream's rtty.c on the SAME branch
 * of demod_am.c, RTTY-L on LSB and RTTY-U on USB with the tones swapped
 * (demod.c, demod_rtty()), its text through the same panel.
 */
#define CW_LINEA_MS   2000U

static char     s_cw_lin[64];
static uint8_t  s_cw_n;
static uint32_t s_cw_t;                 /* when the last character came */

static void cw_linea_suelta(void)
{
    if (s_cw_n == 0U) { return; }
    s_cw_lin[s_cw_n] = '\0';
    console_printf("  %s: %s\n", rtty_get_enabled() ? "rtty"
                                 : psk31_activo() ? "psk31"
                                 : navtex_activo() ? "navtex" : "cw", s_cw_lin);
    s_cw_n = 0U;
}

/* A decoded character: the panel, and the console's line. */
static void texto_char(char c)
{
    s_cw_t = board_millis();
    digi_texto(c);
    if (c == '\r' || c == '\n') { cw_linea_suelta(); return; }
    if (c < ' ' || c > '~') { c = '.'; }
    s_cw_lin[s_cw_n++] = c;
    if (s_cw_n >= (uint8_t)(sizeof s_cw_lin - 1U)) { cw_linea_suelta(); }
}

static uint8_t s_rtty_var;              /* 0 off, 1 RTTY-L, 2 RTTY-U */

/* The demodulator under a mode entry (menus.h). */
static int modo_demod(int mode)
{
    if (mode == RADIO_MODE_CW || mode == RADIO_MODE_RTTY_U
        || mode == RADIO_MODE_PSK31 || mode == RADIO_MODE_NAVTEX) { return (int)DEMOD_MODE_USB; }
    if (mode == RADIO_MODE_RTTY_L)                          { return (int)DEMOD_MODE_LSB; }
    return mode;
}

/*
 * The text decoders an entry wants, after demod_mode() has set its
 * demodulator - upstream's rule: choosing any other entry switches them
 * off without asking (menu_mode_preset_callback()). And the screen: in a
 * text mode the spectrum gives its place to the scope and the panel
 * (src/digi.c); from one text mode to another the panel starts empty.
 * Under a menu, its close repaints.
 */
static void texto_modo(int mode)
{
    uint8_t cw = (uint8_t)(mode == RADIO_MODE_CW);
    uint8_t rv = (uint8_t)((mode == RADIO_MODE_RTTY_L) ? 1U
                         : (mode == RADIO_MODE_RTTY_U) ? 2U : 0U);
    uint8_t pk = (uint8_t)(mode == RADIO_MODE_PSK31);
    uint8_t nx = (uint8_t)(mode == RADIO_MODE_NAVTEX);
    uint8_t era_cw = cw_get_enabled(), era_rv = s_rtty_var, era_pk = psk31_activo();
    uint8_t era_nx = navtex_activo();

    if (cw == era_cw && rv == era_rv && pk == era_pk && nx == era_nx) { return; }
    cw_linea_suelta();
    demod_cw(cw);
    demod_rtty(rv);
    demod_psk31(pk);
    demod_navtex(nx);
    s_rtty_var = rv;
    digi_pon((uint8_t)(cw || rv || pk || nx));
    if (pk != era_pk) {
        console_printf("  PSK31 %s - centre %u Hz\n", pk ? "on" : "off",
                       (uint32_t)(psk31_get_centro_hz() + 0.5f));
    }
    if (nx != era_nx) {
        console_printf("  NAVTEX %s - centre %u Hz, %s\n", nx ? "on" : "off",
                       (uint32_t)(navtex_get_centro_hz() + 0.5f),
                       navtex_get_invertido() ? "reversed" : "B the higher tone");
    }
    if (menu_open_id() == MENU_NONE) { radio_screen_restore(); }
    if (cw != era_cw) {
        console_printf("  CW %s - tone %u Hz, band-pass %u Hz\n", cw ? "on" : "off",
                       (uint32_t)(cw_get_pitch_hz() + 0.5f),
                       (uint32_t)(demod_am_get_cw_bw_hz() + 0.5f));
    }
    if (rv != era_rv) {
        if (rv) {
            console_printf("  RTTY on, %s - mark %u Hz, space %u Hz, %u.%02u Bd, auto %s\n",
                           rv == 1U ? "LSB" : "USB",
                           (uint32_t)(rtty_get_mark_hz() + 0.5f),
                           (uint32_t)(rtty_get_space_hz() + 0.5f),
                           (uint32_t)rtty_get_baud(),
                           (uint32_t)(rtty_get_baud() * 100.0f + 0.5f) % 100U,
                           rtty_auto_get() ? "on" : "off");
        } else {
            console_printf("  RTTY off\n");
        }
    }
}

/*
 * What the decoder sees, for the bench: swd-call ... cw_report. Its own
 * envelope statistics (x 10000, the console has no %f) and what in the
 * chain ahead of it could be shaping the audio - the blanker works on
 * the I/Q before the channel filter, and nothing else here is behind
 * the CW tap except the AGC (demod_am.c, s_ssb_dec).
 */
__attribute__((noinline, used)) void cw_report(void)
{
    console_printf("  cw report: %s, gate %s, tone %u Hz (set %u), %u wpm, "
                   "fit error %u/100\n",
                   cw_get_enabled() ? "on" : "OFF",
                   cw_get_decode_ok() ? "OPEN" : "closed",
                   (uint32_t)(cw_get_detect_hz() + 0.5f),
                   (uint32_t)(cw_get_pitch_hz() + 0.5f),
                   (uint32_t)(cw_get_wpm() + 0.5f),
                   (uint32_t)(100.0f * cw_get_quality() + 0.5f));
    console_printf("             level %u floor %u peak %u (x10000), key %u\n",
                   (uint32_t)(10000.0f * cw_get_level()),
                   (uint32_t)(10000.0f * cw_get_floor()),
                   (uint32_t)(10000.0f * cw_get_peak()),
                   (uint32_t)cw_get_key());
    console_printf("             blanker %u, squelch %d dB (%s), AGC profile %u\n",
                   (uint32_t)nb_get_nivel(), (int32_t)demod_am_get_squelch_db(),
                   demod_am_get_squelch_open() ? "open" : "SHUT",
                   (uint32_t)demod_am_get_agc_profile());
}

/* What rtty_auto.c and rtty.c's inversion detector decided, on the
 * console as it happens - upstream's own lines go to a debug UART this
 * port does not build. */
static void rtty_auto_log(void)
{
    static uint32_t m0, s0, b0;
    static uint8_t  v0 = 0xFFU;
    uint32_t m = (uint32_t)(rtty_get_mark_hz() + 0.5f);
    uint32_t sp = (uint32_t)(rtty_get_space_hz() + 0.5f);
    uint32_t b = (uint32_t)(rtty_get_baud() * 100.0f + 0.5f);
    uint8_t  v = rtty_get_vuelta_auto();

    if (m == m0 && sp == s0 && b == b0 && v == v0) { return; }
    console_printf("  rtty now: mark %u Hz, space %u Hz, %u.%02u Bd%s (health %u %%)\n",
                   m, sp, b / 100U, b % 100U, v ? ", flipped by the detector" : "",
                   (uint32_t)rtty_salud_pc());
    m0 = m; s0 = sp; b0 = b; v0 = v;
}

/*
 * NAVTEX's phase on the console, as it changes - upstream's chip says it
 * on the panel (searching / bits / SITOR / the header) and its own
 * navtex_poll() logs only the text. Where the decoder listens matters as
 * much: navtex.c measures its centre by itself after a search window
 * without a lock (zero crossings, cruces_mete()), and this says where it
 * went.
 */
static void navtex_linea_estado(const char *que)
{
    navtex_info_t in;
    int32_t  c;

    navtex_info(&in);
    c = (int32_t)(navtex_get_centro_hz() + 0.5f) + (int32_t)in.desvio_hz;
    console_printf("  navtex: %s %s (header %s), centre %d Hz (%+d measured), quality %u %%, "
                   "chars %u good %u bad %u saved by the repeat, %u messages\n",
                   que, in.car_sync ? "SITOR" : in.bit_sync ? "bits" : "searching",
                   in.tiene_cab ? in.cab : "-",
                   (int)c, (int)in.desvio_hz, (uint32_t)(100.0f * in.calidad + 0.5f),
                   (uint32_t)in.car_ok, (uint32_t)in.car_mal, (uint32_t)in.rescatados,
                   (uint32_t)in.mensajes);
}

static void navtex_log(void)
{
    static uint8_t  f0 = 0xFFU;
    static int16_t  d0;
    static char     cab0[5];
    navtex_info_t in;
    uint8_t f;

    navtex_info(&in);
    f = (uint8_t)((in.tiene_cab << 2) | (in.car_sync << 1) | in.bit_sync);
    if (f == f0 && in.desvio_hz == d0 && (!in.tiene_cab || memcmp(cab0, in.cab, 4U) == 0)) {
        return;
    }
    f0 = f;
    d0 = in.desvio_hz;
    memcpy(cab0, in.cab, 4U);
    navtex_linea_estado("now");
}

static void cw_poll(void)
{
    char c;
    uint8_t hay = 0U;

    if (cw_get_enabled()) {
        while (cw_get_char(&c)) { hay = 1U; texto_char(c); }
    }
    if (rtty_get_enabled()) {
        while (rtty_get_char(&c)) { hay = 1U; texto_char(c); }
        rtty_auto_poll();               /* upstream's: shift, baud and polarity by itself */
        rtty_auto_log();
    }
    if (psk31_activo()) {
        while (psk31_get_char(&c)) { hay = 1U; texto_char(c); }
    }
    if (navtex_activo()) {
        /* navtex.c's centre, from the scope's spectrum (navtex_espectro(),
         * 22/11/2026): the frame src/digi.c draws, made here too so a menu
         * over the screen does not stop the search. Each frame once. */
        rtty_scope_poll();
        navtex_espectro(rtty_scope_frame_peek(), (uint16_t)RTTY_SCOPE_BINS,
                        rtty_scope_hz_per_bin(), rtty_scope_frame_n());
        while (navtex_get_char(&c)) { hay = 1U; texto_char(c); }
        navtex_log();
    }
    (void)hay;
    if (s_cw_n > 0U && (board_millis() - s_cw_t) >= CW_LINEA_MS) {
        cw_linea_suelta();
        if (cw_get_enabled()) {
            console_printf("  cw: -- %u wpm, tone at %u Hz, fit error %u/100\n",
                           (uint32_t)(cw_get_wpm() + 0.5f),
                           (uint32_t)(cw_get_detect_hz() + 0.5f),
                           (uint32_t)(100.0f * cw_get_quality() + 0.5f));
        } else if (navtex_activo()) {
            navtex_linea_estado("--");
        } else if (psk31_activo()) {
            uint8_t cal = 0U;
            int16_t e = 0;

            psk31_diag(&cal, &e);
            console_printf("  psk31: -- centre %u Hz, error %c%u.%02u Hz, quality %u\n",
                           (uint32_t)(psk31_get_centro_hz() + 0.5f), e < 0 ? '-' : '+',
                           (uint32_t)(e < 0 ? -e : e) / 100U,
                           (uint32_t)(e < 0 ? -e : e) % 100U, (uint32_t)cal);
        } else {
            console_printf("  rtty: -- %u Hz shift, %u Bd, %u %% of frames whole\n",
                           (uint32_t)(rtty_get_shift_hz() + 0.5f),
                           (uint32_t)(rtty_get_baud() + 0.5f),
                           (uint32_t)rtty_salud_pc());
        }
    }
}

/* A new station, or none: the decoder from zero and the strip empty. */
static void rds_limpia(void)
{
    s_rds_txt[0] = '\0';
    s_rds_nom[0] = '\0';
    s_rds_txt_ant[0] = '\1';           /* force a re-measure and a repaint */
    s_rds_nom_ant[0] = '\0';
    s_rds_off = 0;
    s_top.rds = s_rds_txt;
    s_top.rds_nombre = s_rds_nom;
    s_top.rds_off = 0;
}

/*
 * "WFM ST" IN THE MODE CHIP while the stereo decoder is blending in
 * (src/estereo.c) - the owner's choice of indicator, 22/11/2026. On at a
 * blend of 50 %, off below 40, so a station on the edge does not make it
 * blink. The chip widens by " ST" and the RDS strip gives the room back
 * while it is lit.
 */
static uint8_t s_st_on;

static const char *mode_chip(void)
{
    int m = radio_mode_get();

    if (m == (int)DEMOD_MODE_WFM && s_st_on) { return "WFM ST"; }
    return radio_mode_name(m);
}

static void estereo_ui_poll(void)
{
    estereo_estado_t e;
    uint8_t          on = s_st_on;

    if (demod_am_get_mode() != DEMOD_MODE_WFM) {
        on = 0U;
    } else {
        estereo_estado(&e);
        if (!on && e.mezcla >= 0.5f) { on = 1U; }
        if (on && e.mezcla < 0.4f)   { on = 0U; }
    }
    if (on == s_st_on) { return; }
    s_st_on = on;
    s_top.mode = mode_chip();
    ui_top_draw(&s_top);                /* the chip, and the strip after it */
    console_printf("  stereo %s\n", on ? "ON" : "off");
}

static void rds_modo(void)
{
    s_st_on = 0U;                       /* the poll lights it when there is stereo */
    if (demod_am_get_mode() == DEMOD_MODE_WFM) {
        rds_start(192000.0f);           /* the WFM path's rate */
        s_rds_freq = s_tuned_hz;
        console_printf("  RDS on\n");
    } else if (rds_activo()) {
        rds_stop();
        console_printf("  RDS off\n");
    }
    rds_limpia();
    s_top.rds = 0;                      /* nothing in the strip until there is */
    s_top.rds_nombre = 0;
}

static int dial_tune(int64_t dial_hz)
{
    if (dial_hz < (int64_t)TUNE_MIN_HZ) { dial_hz = (int64_t)TUNE_MIN_HZ; }
    if (dial_hz > (int64_t)TUNE_MAX_HZ) { dial_hz = (int64_t)TUNE_MAX_HZ; }

    /*
     * THE CLICK REPAIR (am.h, "THE RETUNE CLICK") needs the write noted
     * to the sample, and noted far enough into a receive half that the
     * fade before it is still in the same block. So: wait for that -
     * at most 256 frames, 1.3 ms, and bounded at 3 ms whatever happens
     * - note, and write at once. Nothing may come between the note and
     * the write; that interval is what the window was measured from.
     */
    if (sai1_dma_running() && am_enabled()) {
        uint32_t t0 = board_millis();

        while (!am_retune_ok() && (board_millis() - t0) < 3U) { }
        am_retune_note();
    }

    if (msi001_retune((uint32_t)dial_hz - s_lo_off) != MSI_OK) {
        return 0;
    }

    const msi_plan_t *kp = msi001_last_plan();

    /*
     * The READOUT shows what the synthesiser actually reached, not what
     * it was asked for. They agree to 0 Hz on this board (step 5) and
     * printing the request would hide the day they stop agreeing. The
     * synthesiser reached the TUNER's frequency; the dial is the same
     * fixed offset above it.
     */
    s_tuned_hz = kp->f_actual + s_lo_off;
    ui_fmt_khz(s_tuned_hz, s_freq_txt, sizeof s_freq_txt);
    ui_top_draw_freq(&s_top);

    /* The ruler is centred on the dial, and so is the window of bins
     * the loop draws (PAN_LO). */
    s_chrome.center_hz = s_tuned_hz;
    if (menu_open_id() == MENU_NONE && !digi_en_pantalla()) {
        spec_chrome_draw(&s_chrome);
    }

    /*
     * The band filter follows the frequency, or the signal goes away at
     * the first band edge and the knob gets blamed for it.
     *
     * ONLY WHEN IT CHANGES, which is what the stock firmware does with
     * every tuner write (RE report 5.3). Rewriting the same value is
     * harmless to the filters themselves, but the add-on board's 74HC595
     * is bit-banged on PD12/PD13, on a board that sits in the RF path in
     * front of the input: a burst of logic edges next to the antenna at
     * the exact instant of every retune, for nothing. Whether that is
     * part of the click is for the fine trace to say; it costs nothing
     * not to do it.
     *
     * The caches start invalid, so the first move writes both whatever
     * main() set up before the loop.
     */
    static uint32_t s_bpf_was   = UINT32_MAX;
    static uint32_t s_addon_was = UINT32_MAX;
    /* The filter by the TUNER's frequency, as msi001_retune() picks the
     * MSi001's path: at 60 or 120 MHz the dial and the tuner can be on
     * two sides of an edge, and an AM path behind a VHF filter is
     * silence. */
    uint32_t        bpf   = msi001_bpf_for(kp->f_actual);
    addon_filter_t  addon = addon_filter_for(s_tuned_hz);

    if (bpf != s_bpf_was) {
        msi001_bpf(bpf);
        s_bpf_was = bpf;
    }
    if ((uint32_t)addon != s_addon_was) {
        addon_set_filter(addon);
        s_addon_was = (uint32_t)addon;
    }
    band_pill_sync();

    /* Another frequency is another station: RDS from zero (upstream's
     * apply_lo_tune()), but not on a call that lands where it was. */
    if (rds_activo() && s_tuned_hz != s_rds_freq) {
        s_rds_freq = s_tuned_hz;
        estereo_pide_reset();           /* another pilot: from zero, by the IRQ */
        rds_start(192000.0f);
        rds_limpia();
        ui_top_draw_rds(&s_top);
    }
    return 1;
}

/*
 * THE RETUNE TEST - what a retune does to the AGC, measured instead of
 * reasoned about, because the AGC is about to be changed and "the
 * transient" has so far been a story nobody had looked at.
 *
 * Scripted, so it needs no hand and repeats exactly: the dial is moved
 * by itself, a few seconds apart, with am.c's trace recording two
 * seconds around each move (am.h, am_trace_*).
 *
 *   +/-10 Hz   the smallest step the knob has, and the owner hears
 *              the click on it too (21/11/2026)
 *   +/-1 kHz   the carrier stays inside the 4 kHz channel: whatever
 *              the AGC does here is the retune itself - the PLL, the
 *              tuner's DC trim, the leakage estimate re-settling
 *   +/-20 kHz  the carrier leaves the channel and comes back: tuning
 *              OFF a station and ONTO one
 *
 * Each pair twice, because one run of anything here is an anecdote.
 * The ±1 kHz moves also dump am.c's FINE trace (am.h) - one point per
 * 167 us across the retune - because what is left after the AGC was
 * settled is a click, and a click is shorter than the coarse trace's
 * 2.67 ms. It needs RETUNE_TEST 2; 1 runs the coarse table only.
 *
 * (There was a third group here with am_resettle(2), the knob's old
 * mute. It showed the mute was worse than no mute, and the mute is
 * gone.)
 *
 * It runs once per boot when RETUNE_TEST is not 0, starting a few
 * seconds into the display loop, and moves the dial while it does:
 * leave the knob alone for the minute it takes. It ends where it
 * started.
 *
 * OFF BY DEFAULT: a radio that takes its own dial away for a minute on
 * every boot is not a radio. The switch is in am.h, next to the buffers
 * it sizes; WORKING.md has the tables to compare against.
 */
static const int32_t k_rt[] = {
    +10, -10, +10, -10,
    +1000, -1000, +1000, -1000, +20000, -20000, +20000, -20000,
};
#define RT_N            (sizeof k_rt / sizeof k_rt[0])
#define RT_START_MS     6000U   /* into the loop: DMA up, AGC settled  */
#define RT_GAP_MS       2500U   /* after each trace: release tau 0.5 s */
#define RT_BASE_MS      100U    /* recorded before the move: 2+ blocks */
#define RT_CHUNK_US     2667    /* AM_CHUNK / 192 kHz, for the ms axis */

static uint32_t s_rt_i;
static uint32_t s_rt_at;        /* 0 = not scheduled yet */
static uint8_t  s_rt_busy;
static uint8_t  s_rt_fine;      /* this move dumps the fine trace */

/* The gain upstream's agc_bloque() applies for a given peak: target over
 * peak, capped. Its own constants, from its own header. */
static float rt_gain_db(float agc)
{
    float g = (agc > 0.0f) ? (AGC_TARGET / agc) : AGC_GAIN_MAX;

    if (g > AGC_GAIN_MAX) { g = AGC_GAIN_MAX; }
    return 20.0f * log10f(g);
}

/* Tenths of a dB as text: console_printf() has no %f, and a format it
 * does not know shifts every later argument (trap 8). */
static const char *rt_db(char *buf, float db)
{
    int32_t t = (int32_t)((db < 0.0f) ? (db * 10.0f - 0.5f)
                                      : (db * 10.0f + 0.5f));
    uint32_t a = (uint32_t)((t < 0) ? -t : t);
    uint32_t w = 0U;
    char     d[12];
    uint32_t nd = 0U;

    buf[w++] = (t < 0) ? '-' : '+';
    uint32_t ip = a / 10U;

    do { d[nd++] = (char)('0' + (ip % 10U)); ip /= 10U; } while (ip);
    while (nd) { buf[w++] = d[--nd]; }
    buf[w++] = '.';
    buf[w++] = (char)('0' + (a % 10U));
    buf[w]   = '\0';
    return buf;
}

static int32_t rt_ms(uint32_t k, uint32_t mark)
{
    return (((int32_t)k - (int32_t)mark) * RT_CHUNK_US) / 1000;
}

static void retune_test_print(uint32_t idx)
{
    uint32_t          mark;
    const am_trace_t *t = am_trace_get(&mark);
    char              b1[16], b2[16], b3[16];

    if (mark >= AM_TRACE_N) {
        console_printf("  retune %u: never marked - no trace\n", idx + 1U);
        return;
    }

    /* Reference: the block before the move, and the last 32 chunks -
     * where the AGC was, and where it ends up. */
    uint32_t b0 = (mark >= 16U) ? (mark - 16U) : 0U;
    float    base = 0.0f, endg = 0.0f;
    uint32_t nb = 0U;

    for (uint32_t k = b0; k < mark; k++) {
        base += rt_gain_db(t[k].agc);
        nb++;
    }
    base = (nb != 0U) ? (base / (float)nb) : rt_gain_db(t[mark].agc);

    for (uint32_t k = AM_TRACE_N - 32U; k < AM_TRACE_N; k++) {
        endg += rt_gain_db(t[k].agc);
    }
    endg /= 32.0f;

    /*
     * After the move, measured against WHERE THE AGC ENDS UP, not where
     * it started: tuning off a station or onto one legitimately ends at
     * a different gain, and "under the starting gain" then measures the
     * station, not the transient. The dip is the gain's excursion below
     * its own final value - the part a listener hears as a dropout.
     */
    float    worst = 1000.0f;
    uint32_t worst_k = mark, below_k = mark, muted = 0U, clipped = 0U;
    float    out_max = 0.0f;

    for (uint32_t k = mark; k < AM_TRACE_N; k++) {
        float g = rt_gain_db(t[k].agc);

        if (g < worst)        { worst = g; worst_k = k; }
        if (g < endg - 1.0f)  { below_k = k + 1U; }
        if (t[k].car < 0.0f)  { muted++; }
        if (t[k].out > out_max) { out_max = t[k].out; }
        if (t[k].out >= 32767.0f) { clipped++; }
    }

    console_printf("\n  RETUNE %u of %u: %+d Hz - dial now %u Hz\n",
                   idx + 1U, (uint32_t)RT_N, k_rt[idx], s_tuned_hz);
    console_printf("    gain: before %s dB, at the end %s dB, deepest %s dB "
                   "at %d ms\n",
                   rt_db(b1, base), rt_db(b2, endg), rt_db(b3, worst),
                   rt_ms(worst_k, mark));
    console_printf("    DIP %s dB below the end value, more than 1 dB "
                   "under it until %d ms\n",
                   rt_db(b1, worst - endg), rt_ms(below_k, mark));
    console_printf("    out max %u   clipped chunks %u   muted chunks %u\n",
                   (uint32_t)out_max, clipped, muted);
    console_printf("    (the move is at 0 ms to within one 42.7 ms block: "
                   "the hook runs per block)\n");
    console_printf("        ms     car     aud     agc   gain dB    out\n");

    /* Every chunk from one block before to 48 after, then one per block. */
    for (uint32_t k = b0; k < AM_TRACE_N; k++) {
        if (k > mark + 48U && ((k - mark) % 16U) != 0U) { continue; }

        if (t[k].car < 0.0f) {
            console_printf("    %6d   (muted)          %6u   %s   %5u\n",
                           rt_ms(k, mark), (uint32_t)t[k].agc,
                           rt_db(b1, rt_gain_db(t[k].agc)),
                           (uint32_t)t[k].out);
        } else {
            console_printf("    %6d  %6u  %6u  %6u   %s   %5u\n",
                           rt_ms(k, mark), (uint32_t)t[k].car,
                           (uint32_t)t[k].aud, (uint32_t)t[k].agc,
                           rt_db(b1, rt_gain_db(t[k].agc)),
                           (uint32_t)t[k].out);
        }
    }
}

/*
 * The fine trace, raw, one line per 167 us entry - for the host to read,
 * not a person: 768 lines a move. `F rel lr li cr ci a0 a1`, rel in
 * entries from the one the synthesiser was written in (rel 0), the rest
 * as am.h describes, rounded to integers.
 */
static int32_t rt_round(float x)
{
    return (int32_t)((x < 0.0f) ? (x - 0.5f) : (x + 0.5f));
}

static void retune_test_print_fine(uint32_t idx)
{
    uint32_t         first, at;
    const am_fine_t *f = am_fine_get(&first, &at);

    if (at == UINT32_MAX) {
        console_printf("  FINE %u: the retune's block never arrived\n",
                       idx + 1U);
        return;
    }

    int32_t at_rel = (int32_t)((at + AM_FINE_N - first) % AM_FINE_N);

    console_printf("  FINE %u begin: %u entries of %u samples, retune at "
                   "entry %d\n", idx + 1U, AM_FINE_N, AM_FINE_SUB, at_rel);
    for (uint32_t j = 0; j < AM_FINE_N; j++) {
        const am_fine_t *e = &f[(first + j) % AM_FINE_N];

        console_printf("F %d %d %d %d %d %d %d\n",
                       (int32_t)j - at_rel,
                       rt_round(e->lr), rt_round(e->li),
                       rt_round(e->cr), rt_round(e->ci),
                       (int32_t)e->a0, (int32_t)e->a1);
    }
    console_printf("  FINE %u end\n", idx + 1U);

    /* The raw frames around the write: `R rel q i`, rel in samples from
     * the write (the DMA position read immediately before it). */
    uint32_t       rn;
    int32_t        rel0;
    const int16_t *r = am_raw_get(&rn, &rel0);

    console_printf("  RAW %u begin: %u frames from %d\n", idx + 1U, rn, rel0);
    for (uint32_t k = 0; k < rn; k++) {
        console_printf("R %d %d %d\n", rel0 + (int32_t)k,
                       (int32_t)r[k * 2U], (int32_t)r[k * 2U + 1U]);
    }
    console_printf("  RAW %u end\n", idx + 1U);

    /* What the codec got over the same frames, and which repair made it. */
    uint32_t       an;
    const int16_t *au = am_aud_get(&an);
    uint32_t       pn, hn, pp;
    float          pr;

    am_pred_stats(&pn, &hn, &pp, &pr);
    console_printf("  AUD %u begin: %u frames from %d, repair %s, period %u, "
                   "r %d/1000\n", idx + 1U, an, rel0,
                   am_pred_get() ? "PREDICT" : "HOLD", pp,
                   (int32_t)(pr * 1000.0f));
    for (uint32_t k = 0; k < an; k++) {
        console_printf("A %d %d\n", rel0 + (int32_t)k, (int32_t)au[k]);
    }
    console_printf("  AUD %u end\n", idx + 1U);
}

static void retune_test_step(void)
{
    if (!RETUNE_TEST) { return; }
    if (s_rt_i >= RT_N) { return; }
    if (!sai1_dma_running() || !am_enabled()) { return; }

    uint32_t now = board_millis();

    if (s_rt_at == 0U) {
        s_rt_at = now + RT_START_MS;
        console_printf("\n  RETUNE TEST: %u moves of the dial, by itself, "
                       "starting in %u s - leave the knob alone\n",
                       (uint32_t)RT_N, RT_START_MS / 1000U);
        return;
    }

    if (!s_rt_busy) {
        if ((int32_t)(now - s_rt_at) < 0) { return; }

        /* The fine trace and the raw capture for every move that stays
         * inside the channel: +-1 kHz, and since 21/11/2026 +-10 Hz -
         * the owner hears the click on a 10 Hz step too. */
        int fine = (RETUNE_TEST >= 2) && (k_rt[s_rt_i] >= -1000
                                          && k_rt[s_rt_i] <= 1000);

        am_trace_arm();
        if (fine) { am_fine_arm(); }
        board_delay_ms(RT_BASE_MS);         /* a baseline before the move */

        /*
         * MARK FIRST, THEN MOVE. The first version marked after
         * dial_tune() returned, and the hook - which runs a whole block
         * per interrupt - had sometimes already recorded a block of the
         * new frequency, or of the old mute's silence, by then. Those
         * chunks landed BEFORE the mark and were averaged into "gain
         * before": +49.5 dB, from a muted AGC at its floor. Marked
         * first, everything before the mark is the old frequency, and
         * the move shows up within one block after it.
         *
         * The fine trace's mark is not here: dial_tune() notes the
         * write with am_retune_note() immediately before making it, and
         * that one call pins both the click repair and the fine trace.
         */
        am_trace_mark();
        s_rt_fine = (uint8_t)fine;

        /*
         * THE A/B, INTERLEAVED: predicted fill or the plain hold, in the
         * pattern P H H P over each group of four, so +10/-10 and
         * +1k/-1k each get both and a direction never stands in for a
         * repair (am.h, "PREDICTED, NOT FADED").
         */
        if (fine) {
            uint32_t q = s_rt_i % 4U;

            am_pred_set(q == 0U || q == 3U);
        }

        /*
         * A RAW CAPTURE IS ONLY AS GOOD AS ITS PLACE IN THE BLOCK: it is
         * cut at the edges of the receive half the write lands in. So a
         * move that captures waits - 50 ms at most, a block is 42.7 -
         * for a DMA position with AM_RAW_PRE frames behind it and the
         * rest of AM_RAW_N ahead.
         */
        if (fine) {
            uint32_t lo = AM_RAW_PRE + 64U;
            uint32_t hi = CAP_FRAMES - (AM_RAW_N - AM_RAW_PRE) - 64U;
            uint32_t t0 = board_millis();

            while ((board_millis() - t0) < 50U) {
                uint32_t pos = sai1_rx_frame_pos();

                if (pos >= lo && pos <= hi) { break; }
            }
        }

        if (!dial_tune((int64_t)s_tuned_hz + k_rt[s_rt_i])) {
            console_printf("  retune %u: the tuner refused %d Hz\n",
                           s_rt_i + 1U, k_rt[s_rt_i]);
            s_rt_i++;
            s_rt_busy = 0U;
            s_rt_at = board_millis() + RT_GAP_MS;
            return;
        }
        s_rt_busy = 1U;
        return;
    }

    if (!am_trace_done()) { return; }
    if (s_rt_fine && !am_fine_done()) { return; }

    retune_test_print(s_rt_i);
    if (s_rt_fine) { retune_test_print_fine(s_rt_i); }
    s_rt_busy = 0U;
    s_rt_i++;
    s_rt_at = board_millis() + RT_GAP_MS;

    if (s_rt_i >= RT_N) {
        am_pred_set(1);
        console_printf("\n  RETUNE TEST done - the knob is yours again\n\n");
    }
}

/* The static half of the screen: everything that does not change from
 * one capture to the next. */
static void ui_paint_all(void)
{
    gfx2_render_screen(ui_fondo, 0);
    /* In a text mode the gutter and the ruler are not there: the scope
     * and the panel are (src/digi.c). */
    if (!digi_en_pantalla()) { spec_chrome_draw(&s_chrome); }
    ui_top_draw(&s_top);
    ui_act_draw(&s_act);
    digi_repinta();                     /* nothing unless a decoder is on */
}

/*
 * One new waterfall row out of a finished spectrum.
 *
 * The trace is 440 px and the window shown is PAN_SHOWN = 448 bins, so
 * a column takes the bin it lands on. The caller passes the window, not
 * the whole transform, or the waterfall and the trace above it would be
 * centred 12 kHz apart. Upstream's own waterfall does the same thing;
 * picking the maximum of the bins a column covers would be better and
 * is a change to make once there is something to compare against.
 */
static void ui_wf_push(const float *db, uint32_t bins)
{
    uint8_t *row = &s_wf[(uint32_t)s_wf_head * (uint32_t)SPC_TRACE_W];
    int16_t  i;

    for (i = 0; i < SPC_TRACE_W; i++) {
        uint32_t src = (uint32_t)((int32_t)i * (int32_t)bins) / SPC_TRACE_W;
        row[i] = spectrum_colormap_index(db[src], UI_DB_MIN, UI_DB_MAX);
    }

    s_wf_head = (int16_t)((s_wf_head + 1) % SPC_WF_ROWS);
}

/*
 * THE RADIO SIDE OF THE MENUS - step 9, 21/11/2026.
 *
 * src/menus.c draws upstream's screens and decides what was chosen; what
 * a choice DOES is here, because the dial, the header, the step and the
 * action bar all live in this file. See src/menus.h.
 */
static const char *const k_mode_names[] = { "AM", "USB", "LSB", "NFM",
                                            "WFM", "SAM", "CW", "RTTY-L",
                                            "RTTY-U", "PSK31", "NAVTEX" };
_Static_assert(sizeof k_mode_names / sizeof k_mode_names[0]
               == (size_t)RADIO_MODE_LAST + 1U,
               "a name for every mode, upstream's and the decoders' (menus.h)");
static const char *s_band_name;          /* the band the dial is in, or 0 */
static char        s_act_step[12];
static char        s_act_vol[12];

/*
 * THE AGC AND BW CHIPS - upstream's top_sync() for these two, and its
 * agc_profile_cycle() / audio_bw_cycle(): the AGC chip steps OFF -> SLW
 * -> MED -> FST, the BW chip through upstream's three quick filters
 * (k_fil_rapidos[] in User/main.c), widest to narrowest and round.
 */
static const char *const k_agc_names[4] = { "OFF", "SLW", "MED", "FST" };
static const struct { uint16_t lo, hi; } k_fil_quick[3] = {
    { 0U, 1800U }, { 0U, 2300U }, { 0U, 4000U }
};
static char s_top_bw[12];

/*
 * IN WFM THE BW CHIP IS THE AUDIO LOW-PASS - upstream's wfm_alpf_khz():
 * WFM has no channel filter family (demod_am_filtro_familia() would
 * answer SSB's), and what there is to choose is the audio low-pass
 * behind the discriminator, upstream's three AUDIO_BW_* reused as 15, 8
 * and 4 kHz. "15 k", as upstream's wfm_alpf_format() writes it.
 */
static uint16_t wfm_alpf_khz(void)
{
    switch (demod_am_get_audio_bw()) {
    case AUDIO_BW_2K3: return 8U;
    case AUDIO_BW_1K8: return 4U;
    case AUDIO_BW_4K0:
    default:           return 15U;
    }
}

static void top_sync_chips(void)
{
    uint16_t lo = 0U, hi = 0U;
    uint8_t  i  = 0U;
    agc_profile_t p = demod_am_get_agc_profile();

    s_top.agc = k_agc_names[(p <= AGC_PROFILE_FAST) ? (uint32_t)p : 0U];

    if (demod_am_get_mode() == DEMOD_MODE_WFM) {
        uint16_t k = wfm_alpf_khz();

        if (k >= 10U) { s_top_bw[i++] = (char)('0' + k / 10U); }
        s_top_bw[i++] = (char)('0' + k % 10U);
        s_top_bw[i++] = ' ';
        s_top_bw[i++] = 'k';
        s_top_bw[i]   = '\0';
        s_top.bw = s_top_bw;
        return;
    }

    /* IN CW, THE CW BAND-PASS - upstream's top_sync(): its real width,
     * asked of the demodulator, written by bw_format() as "0,5 k". */
    if (cw_get_enabled()) {
        uint32_t hz = (uint32_t)(demod_am_get_cw_bw_hz() + 0.5f);

        s_top_bw[i++] = (char)('0' + (hz / 1000U) % 10U);
        s_top_bw[i++] = ',';
        s_top_bw[i++] = (char)('0' + (hz % 1000U) / 100U);
        s_top_bw[i++] = ' ';
        s_top_bw[i++] = 'k';
        s_top_bw[i]   = '\0';
        s_top.bw = s_top_bw;
        return;
    }

    demod_am_get_filtro(demod_am_filtro_familia(), &lo, &hi);
    /* upstream's fil_format(): "0-4,0k", "0,3-2,7k" */
    if (lo == 0U) { s_top_bw[i++] = '0'; }
    else {
        s_top_bw[i++] = (char)('0' + lo / 1000U);
        s_top_bw[i++] = ',';
        s_top_bw[i++] = (char)('0' + (lo / 100U) % 10U);
    }
    s_top_bw[i++] = '-';
    s_top_bw[i++] = (char)('0' + (hi / 1000U) % 10U);
    s_top_bw[i++] = ',';
    s_top_bw[i++] = (char)('0' + (hi / 100U) % 10U);
    s_top_bw[i++] = 'k';
    s_top_bw[i]   = '\0';
    s_top.bw = s_top_bw;
}

static void agc_cycle(void)
{
    agc_profile_t p = demod_am_get_agc_profile();

    p = (p >= AGC_PROFILE_FAST) ? AGC_PROFILE_MANUAL : (agc_profile_t)(p + 1);
    demod_am_set_agc_profile(p);
    top_sync_chips();
    ui_top_draw_status(&s_top);
    console_printf("  AGC -> %s\n", s_top.agc);
}

static void bw_cycle(void)
{
    /* WFM: upstream's audio_bw_cycle(), 15 -> 8 -> 4 -> 15 kHz. */
    if (demod_am_get_mode() == DEMOD_MODE_WFM) {
        audio_bw_t b = demod_am_get_audio_bw();

        b = (b == AUDIO_BW_4K0) ? AUDIO_BW_2K3
          : (b == AUDIO_BW_2K3) ? AUDIO_BW_1K8 : AUDIO_BW_4K0;
        demod_am_set_audio_bw(b);
        top_sync_chips();
        ui_top_draw_status(&s_top);
        console_printf("  BW (WFM audio) -> %s\n", s_top.bw);
        return;
    }

    /* CW: upstream's three for its band-pass (k_cw_rapidos[]), widest
     * to narrowest and round, as the filters below. */
    if (cw_get_enabled()) {
        static const uint16_t k_cw_bw[3] = { 1000U, 500U, 250U };
        uint32_t hz = (uint32_t)(demod_am_get_cw_bw_hz() + 0.5f);
        uint32_t j  = 0U;

        while (j < 3U && k_cw_bw[j] != hz) { j++; }
        j = (j >= 2U) ? 0U : j + 1U;           /* not one of them: 1,0 k */
        demod_am_set_cw_bw_hz((float)k_cw_bw[j]);
        top_sync_chips();
        ui_top_draw_status(&s_top);
        console_printf("  BW (CW band-pass) -> %u Hz\n", (uint32_t)k_cw_bw[j]);
        return;
    }

    uint8_t  fam = demod_am_filtro_familia();
    uint16_t lo = 0U, hi = 0U;
    int      k, now = -1;

    demod_am_get_filtro(fam, &lo, &hi);
    for (k = 0; k < 3; k++) {
        if (k_fil_quick[k].lo == lo && k_fil_quick[k].hi == hi) { now = k; }
    }
    k = (now <= 0) ? 2 : now - 1;
    demod_am_set_filtro(fam, k_fil_quick[k].lo, k_fil_quick[k].hi);
    top_sync_chips();
    ui_top_draw_status(&s_top);
    console_printf("  BW -> %s\n", s_top.bw);
}

/* Each family's filter as the quick preset it equals, 2 bits a family,
 * 3 = none of them - for the FRAM (menus.c, menu_ajustes_get()). */
uint8_t radio_bw_pack(void)
{
    uint8_t out = 0U, fam;

    for (fam = 0U; fam < (uint8_t)FIL_FAM_N && fam < 4U; fam++) {
        uint16_t lo = 0U, hi = 0U;
        uint8_t  k, q = 3U;

        demod_am_get_filtro(fam, &lo, &hi);
        for (k = 0U; k < 3U; k++) {
            if (k_fil_quick[k].lo == lo && k_fil_quick[k].hi == hi) { q = k; }
        }
        out |= (uint8_t)(q << (fam * 2U));
    }
    return out;
}

void radio_bw_unpack(uint8_t packed)
{
    uint8_t fam;

    for (fam = 0U; fam < (uint8_t)FIL_FAM_N && fam < 4U; fam++) {
        uint8_t q = (uint8_t)((packed >> (fam * 2U)) & 3U);

        if (q < 3U) {
            demod_am_set_filtro(fam, k_fil_quick[q].lo, k_fil_quick[q].hi);
        }
    }
}

const char *radio_mode_name(int mode)
{
    return (mode >= 0 && mode <= RADIO_MODE_LAST) ? k_mode_names[mode] : "?";
}

/* Upstream's demod_mode_entry_active(), for the one entry above the
 * demodulators so far: USB with the decoder on is CW. */
int radio_mode_get(void)
{
    int m = (int)demod_am_get_mode();

    if (m == (int)DEMOD_MODE_USB && cw_get_enabled()) { return RADIO_MODE_CW; }
    if (m == (int)DEMOD_MODE_LSB && s_rtty_var == 1U) { return RADIO_MODE_RTTY_L; }
    if (m == (int)DEMOD_MODE_USB && s_rtty_var == 2U) { return RADIO_MODE_RTTY_U; }
    if (m == (int)DEMOD_MODE_USB && psk31_activo())   { return RADIO_MODE_PSK31; }
    if (m == (int)DEMOD_MODE_USB && navtex_activo())  { return RADIO_MODE_NAVTEX; }
    return m;
}

void radio_mode_set(int mode)
{
    int dm = modo_demod(mode);

    if (mode < 0 || mode > RADIO_MODE_LAST) {
        console_printf("  mode %d refused\n", mode);
        return;
    }
    /*
     * A mode is upstream's demodulator by definition: am.c only knows AM.
     * So choosing one - AM included - puts upstream's on the hook, which
     * is also where the boot leaves it now (main(), after demod_init()).
     */
    if (!demod_path_get()) { demod_path(1); }
    (void)demod_mode(dm);
    texto_modo(mode);                   /* the text decoders, and the screen */

    /* Into or out of WFM: the same dial, the tuner moved under it. */
    {
        uint32_t off = lo_off_for(radio_mode_get());

        if (off != s_lo_off) {
            s_lo_off = off;
            s_pan_lo = off ? PAN_LO : PAN_LO_WFM;
            if (!dial_tune((int64_t)s_tuned_hz)) {
                console_printf("  *** the tuner refused %u Hz for %s ***\n",
                               s_tuned_hz - off, radio_mode_name(radio_mode_get()));
            }
            console_printf("  tuner %s the dial\n", off ? "12 kHz below" : "on");
        }
    }
    rds_modo();                         /* on in WFM, off elsewhere */
    s_top.mode = mode_chip();
    top_sync_chips();                   /* the filter family follows the mode */
    ui_top_draw(&s_top);
    radio_act_refresh();
}

uint32_t radio_dial_hz(void) { return s_tuned_hz; }

/*
 * THE BAND PILL FOLLOWS THE DIAL, as upstream's top_sync() makes it:
 * the band the frequency is in by upstream's own rule (menu_band_at(),
 * the narrowest that holds it), or none - and then the pill says
 * "Bandas", which is what it opens. Redrawn only when it changes: the
 * header is 49 kB of SPI, and the knob calls this every detent.
 */
static void band_pill_sync(void)
{
    const char *n = menu_band_name(menu_band_at(s_tuned_hz));

    if (n == s_band_name) { return; }
    s_band_name    = n;
    s_top.band     = n ? n : tr("Bandas", "Bands");
    s_top.en_banda = (uint8_t)(n != (const char *)0);
    if (menu_open_id() == MENU_NONE) {
        ui_top_draw(&s_top);
        radio_act_refresh();
    }
}

int radio_band_go(uint32_t hz, int mode, const char *name)
{
    if (hz < TUNE_MIN_HZ || hz > TUNE_MAX_HZ) { return 0; }
    (void)name;                         /* the pill follows the dial instead */
    if (!dial_tune((int64_t)hz)) { return 0; }
    if (mode >= 0) {
        radio_mode_set(mode);           /* redraws the header and the bar */
    } else {
        ui_top_draw(&s_top);
        radio_act_refresh();
    }
    return 1;
}

/*
 * SMOOTHING - upstream's "Suavizado", s_spectrum_smooth_alpha in its
 * main.c: an exponential average of the TRACE between frames, per bin,
 * with this much weight on what was drawn before. 0 draws each frame
 * raw; 95 % is upstream's ceiling (100 would freeze it). Upstream's
 * default is 25 since 30/10/2026, its owner's word: at 75 a short burst
 * left a trail and the noise floor came out rounded.
 *
 * The WATERFALL stays raw, as upstream's does since it split the two
 * arrays (its s_db_frame / s_db_traza): a row is one moment, and an
 * average smears it into the rows after it. So does the S-meter's peak.
 *
 * Kept up to date under a menu too - it is 448 multiplies - so the trace
 * is current the moment the menu closes.
 */
#define SUAV_MAX_PCT    95
#define SUAV_STEP_PCT    5

static float   s_db_traza[PAN_SHOWN];
static uint8_t s_db_traza_lista;
static uint8_t s_suav_pct = 25U;

static void traza_suaviza(const float *db)
{
    const float a = (float)s_suav_pct * 0.01f, b = 1.0f - a;
    uint32_t    i;

    if (!s_db_traza_lista) {
        /* The first frame: nothing to blend with, so no fade-in from
         * zero at every boot - upstream's s_db_traza_lista. */
        for (i = 0U; i < PAN_SHOWN; i++) { s_db_traza[i] = db[i]; }
        s_db_traza_lista = 1U;
        return;
    }
    for (i = 0U; i < PAN_SHOWN; i++) { s_db_traza[i] = a * s_db_traza[i] + b * db[i]; }
}

int radio_suav_pct(void) { return (int)s_suav_pct; }

int radio_suav_set(int pct)
{
    if (pct < 0)            { pct = 0; }
    if (pct > SUAV_MAX_PCT) { pct = SUAV_MAX_PCT; }
    pct -= pct % SUAV_STEP_PCT;
    s_suav_pct = (uint8_t)pct;
    return pct;
}

void radio_fmt_khz(uint32_t hz, char *out, uint32_t n) { ui_fmt_khz(hz, out, n); }

void radio_filtro_get(uint16_t *lo, uint16_t *hi)
{
    demod_am_get_filtro(demod_am_filtro_familia(), lo, hi);
}

/* Upstream's fil_pon(), and the BW chip after it. */
void radio_filtro_set(uint16_t lo, uint16_t hi)
{
    demod_am_set_filtro(demod_am_filtro_familia(), lo, hi);
    top_sync_chips();
    if (menu_open_id() == MENU_NONE) { ui_top_draw_status(&s_top); }
}

uint32_t radio_dial_go(uint32_t hz)
{
    if (hz < TUNE_MIN_HZ) { hz = TUNE_MIN_HZ; }
    if (hz > TUNE_MAX_HZ) { hz = TUNE_MAX_HZ; }
    if (!dial_tune((int64_t)hz)) { return 0U; }
    ui_top_draw(&s_top);
    radio_act_refresh();
    return s_tuned_hz;
}

uint32_t radio_step_count(void)      { return (uint32_t)TUNE_STEP_N; }
uint32_t radio_step_get(void)        { return s_step_idx; }

void radio_step_set(uint32_t i)
{
    if (i >= TUNE_STEP_N) { return; }
    s_step_idx = (uint8_t)i;
    tune_step_fmt();
    s_top.knob_value = s_step_txt;
    ui_top_draw_status(&s_top);
    radio_act_refresh();
}

/*
 * FOR THE SETTINGS SCREEN (src/menus.c, MENU_AJUSTES): what it shows and
 * what a tap does, for the settings whose state lives in this file. The
 * ones that are upstream modules' own (notch, blanker, NR, the spectrum's
 * palette and style) menus.c sets directly.
 */
static void act_values(void);

static const texto_t k_agc_long[4] = {         /* upstream's k_agc_nombres[] */
    T("Sin AGC", "No AGC"), T("Lenta", "Slow"), T("Media", "Medium"), T("Rápida", "Fast")
};

const char *radio_agc_text(void)
{
    agc_profile_t p = demod_am_get_agc_profile();
    return k_agc_long[(p <= AGC_PROFILE_FAST) ? (uint32_t)p : 0U][idioma()];
}
void        radio_agc_cycle(void)  { agc_cycle(); }
const char *radio_bw_text(void)    { top_sync_chips(); return s_top_bw; }
void        radio_bw_cycle(void)   { bw_cycle(); }
int         radio_preamp(void)     { return s_rf_preamp ? 1 : 0; }

void radio_preamp_toggle(void)
{
    s_rf_preamp = !s_rf_preamp;
    msi001_preamp(s_rf_preamp);
    console_printf("  preamp -> %s\n", s_rf_preamp ? "ON" : "OFF");
}

/* The pill's "-59" with its unit, or upstream's word for none. */
const char *radio_att_text(void)
{
    static char t[12];
    uint32_t    i = 0U, k = 0U;

    rf_att_fmt();
    if (s_att_txt[0] != '-') { return "Apagado"; }
    while (s_att_txt[k] != '\0' && i < 8U) { t[i++] = s_att_txt[k++]; }
    t[i++] = ' '; t[i++] = 'd'; t[i++] = 'B'; t[i] = '\0';
    return t;
}
const char *radio_vol_text(void)   { act_values(); return s_act_vol; }

/* Which control the small knob turns: 1 the RF gain, 0 the volume. */
void radio_knob2_rf(int rf)
{
    s_knob2_rf = rf ? 1 : 0;
    knob2_fmt();
    s_top.knob_value = s_knob2_txt;
    ui_top_draw_status(&s_top);
    console_printf("  small knob -> %s\n", s_knob2_rf ? "RF gain" : "volume");
}

/*
 * THE VOLUME AND THE RF GAIN REDUCTION, ONE WAY IN EACH - the small knob
 * and the settings screen's detail page (menus.c) both change them here,
 * so the codec, the header pill, the action bar and the console can never
 * disagree about which of the two did it. Clamped here, not by callers.
 * Return the value now in force.
 */
static void act_volume_refresh(void);

int radio_vol_db(void) { return s_vol_db; }
int radio_vol_min(void) { return VOL_MIN_DB; }
int radio_vol_max(void) { return VOL_MAX_DB; }

int radio_vol_set(int db)
{
    if (db < VOL_MIN_DB) { db = VOL_MIN_DB; }
    if (db > VOL_MAX_DB) { db = VOL_MAX_DB; }
    if (db == s_vol_db) { return s_vol_db; }

    s_vol_db = db;
    nau8822_set_speaker_db((int8_t)s_vol_db);
    nau8822_set_headphone_db((int8_t)s_vol_db);
    knob2_fmt();
    if (!s_knob2_rf) { s_top.knob_value = s_knob2_txt; }
    ui_top_draw_status(&s_top);
    act_volume_refresh();
    console_printf("  volume -> %d dB\n", s_vol_db);
    return s_vol_db;
}

int radio_att_db(void)  { return (int)s_rf_gr; }
int radio_att_max(void) { return (int)RF_GR_MAX; }

int radio_att_set(int db)
{
    if (db < 0)                { db = 0; }
    if (db > (int)RF_GR_MAX)   { db = (int)RF_GR_MAX; }
    if ((uint8_t)db == s_rf_gr) { return (int)s_rf_gr; }

    s_rf_gr = (uint8_t)db;
    /* msi001_set_gain() recalibrates the DC offset behind itself by
     * default - see msi001_set_dc_auto() - which is what keeps the centre
     * of the spectrum from walking every time the gain moves. */
    msi001_set_gain(s_rf_gr, 0, 0, 0);
    rf_att_fmt();
    s_top.att = s_att_txt;
    ui_top_draw_status(&s_top);
    console_printf("  RF gain reduction -> %u dB\n", s_rf_gr);
    return (int)s_rf_gr;
}

/* A new theme: everything is repainted in it. The caller redraws its
 * menu on top. */
void radio_repaint_all(void)
{
    ui_paint_all();
    spectrum_invalida();
}

/*
 * A NEW LANGUAGE - the settings screen's Idioma, and the FRAM at boot.
 * What is resolved at draw time follows by itself (ui_top.c's and
 * spectrum.c's own tr(), the bar's names in act_values()); the band pill
 * holds a pointer to a name, so it is looked up again; then everything
 * is painted, as for a theme. The caller redraws its menu on top.
 */
void radio_idioma(uint8_t v)
{
    idioma_pon(v);
    s_band_name = (const char *)1;      /* force band_pill_sync() to set it */
    band_pill_sync();
    top_sync_chips();
    s_top.mode = mode_chip();
    act_values();       /* the bar's names: ui_paint_all() draws s_act as it is */
}

void radio_screen_restore(void)
{
    /* The menu painted over the trace, the waterfall, the ruler and the
     * gutters; the cheapest whole answer is the full static repaint the
     * boot does, and a trace that forgets what it drew last. */
    ui_paint_all();
    spectrum_invalida();
}

static void act_values(void);

void radio_act_refresh(void)
{
    act_values();
    ui_act_draw(&s_act);
}

/* Only the Volumen button: the small knob turns it a detent at a time,
 * and the whole bar is 43 kB of SPI where one button is 7. */
static void act_volume_refresh(void)
{
    act_values();
    ui_act_draw_one(&s_act, 3U);
}

static void act_values(void)
{
    menu_t m = menu_open_id();
    uint32_t i;

    /* The step without its pill's "S " - the button already says Paso. */
    for (i = 0U; s_step_txt[i] != '\0' && s_step_txt[i] != ' '; i++) { }
    if (s_step_txt[i] == ' ') { i++; }
    {
        uint32_t k = 0U;
        while (s_step_txt[i] != '\0' && k + 1U < sizeof s_act_step) {
            s_act_step[k++] = s_step_txt[i++];
        }
        s_act_step[k] = '\0';
    }
    {
        int v = s_vol_db, k = 0;
        uint32_t n = (uint32_t)((v < 0) ? -v : v);
        char d[4];
        int nd = 0;

        if (v < 0) { s_act_vol[k++] = '-'; }
        do { d[nd++] = (char)('0' + n % 10U); n /= 10U; } while (n);
        while (nd > 0) { s_act_vol[k++] = d[--nd]; }
        s_act_vol[k++] = ' '; s_act_vol[k++] = 'd'; s_act_vol[k++] = 'B';
        s_act_vol[k] = '\0';
    }

    s_act.value[0]  = s_band_name;
    s_act.value[1]  = radio_mode_name(radio_mode_get());
    s_act.value[2]  = s_act_step;
    s_act.value[3]  = s_act_vol;
    s_act.value[4]  = nr_ss_get_enabled() ? tr("activo", "on") : tr("apagado", "off");
    for (i = 0U; i < (uint32_t)UI_ACT_N; i++) { s_act.name[i] = k_act_names[i][idioma()]; }
    s_act.value[5]  = 0;
    s_act.active[0] = (uint8_t)(m == MENU_BANDAS);
    s_act.active[1] = (uint8_t)(m == MENU_MODO);
    s_act.active[2] = (uint8_t)(m == MENU_PASO);
    s_act.active[3] = 0U;
    s_act.active[4] = nr_ss_get_enabled() ? 1U : 0U;
    s_act.active[5] = (uint8_t)(m == MENU_AJUSTES);
}

/*
 * A BUTTON OF THE ACTION BAR, ON RELEASE - upstream's own rule
 * (demo_button_callback() acts on UI_EVENT_RELEASE), so a finger that
 * slides off a button cancels it.
 */
static void act_release(int8_t b)
{
    switch (b) {
    case 0: menu_toggle(MENU_BANDAS); break;
    case 1: menu_toggle(MENU_MODO);   break;
    case 2: menu_toggle(MENU_PASO);   break;
    case 3:
        /* Upstream points the big knob at the volume; here the small knob
         * is the volume knob already, so the button gives it back to the
         * volume when it was on the RF gain - the same toggle as its own
         * press. */
        s_knob2_rf = !s_knob2_rf;
        knob2_fmt();
        s_top.knob_value = s_knob2_txt;
        ui_top_draw_status(&s_top);
        console_printf("  small knob -> %s\n", s_knob2_rf ? "RF gain" : "volume");
        break;
    case 4:
        /* Upstream's spectral-subtraction noise reduction, in its own
         * demodulator - so it does nothing on am.c's path. */
        nr_ss_set_enabled(nr_ss_get_enabled() ? 0U : 1U);
        console_printf("  NR -> %s%s\n", nr_ss_get_enabled() ? "on" : "off",
                       demod_path_get() ? "" : " (am.c is playing: no effect)");
        radio_act_refresh();
        break;
    default:
        menu_toggle(MENU_AJUSTES);
        break;
    }
}

/*
 * A TAP FROM THE BENCH, OVER SWD - for checking the screens with nobody
 * at the glass. swd-call.sh build/bringup.elf ui_tap <x> <y>: the next
 * poll sees a finger at (x, y), the one after sees it lift, and both go
 * through ui_touch() exactly as a real press does.
 */
static volatile uint16_t s_tap_x, s_tap_y;
static volatile uint8_t  s_tap_n;

__attribute__((noinline)) void ui_tap(int x, int y)
{
    s_tap_x = (uint16_t)x;
    s_tap_y = (uint16_t)y;
    s_tap_n = 1U;
}

/*
 * AND A FINGER THAT MOVES - swd-call ... ui_swipe <x0> <x1> <y>: eight
 * polls from x0 to x1 at height y, then the lift, through ui_touch() like
 * a real drag. For the spectrum's drag-to-tune, which a single tap cannot
 * test.
 */
#define SWIPE_POLLS 8U
static volatile uint16_t s_sw_x0, s_sw_x1, s_sw_y;
static volatile uint8_t  s_sw_n;

__attribute__((noinline)) void ui_swipe(int x0, int x1, int y)
{
    s_sw_x0 = (uint16_t)x0;
    s_sw_x1 = (uint16_t)x1;
    s_sw_y  = (uint16_t)y;
    s_sw_n  = SWIPE_POLLS;
}

/*
 * AND A FINGER THAT STAYS - swd-call ... ui_hold <x> <y> <ms>: down at
 * (x, y) for that long, then the lift. For the gestures that are a
 * duration, the memory screen's two-second save first.
 */
static volatile uint16_t s_hold_x, s_hold_y;
static volatile uint32_t s_hold_until;
static volatile uint8_t  s_hold_on;

__attribute__((noinline)) void ui_hold(int x, int y, int ms)
{
    s_hold_x     = (uint16_t)x;
    s_hold_y     = (uint16_t)y;
    s_hold_until = board_millis() + (uint32_t)ms;
    s_hold_on    = 1U;
}

/*
 * TOUCHING THE SPECTRUM TUNES THE RADIO - upstream's spec_tap_tune_to_x()
 * and spec_drag_tune_apply() (User/main.c), on this port's window.
 *
 *   a TAP     tunes to where the finger was, and through upstream's
 *             spec_snap.c it lands ON the signal near it - the carrier in
 *             AM, the edge of the voice in USB/LSB - rather than on the
 *             step grid. Nothing worth snapping to: the step grid, as the
 *             knob would put it.
 *   a DRAG    pans: the content follows the finger, so a drag right
 *             tunes DOWN, in whole steps, the remainder carried.
 *
 * A press is a drag once it has travelled more than 6 px from where it
 * started (upstream's SPEC_TAP_MOVE_THRESHOLD_PX): a finger is never
 * still, and a tap that wobbled must not move the dial.
 *
 * The zone is the trace AND the waterfall - upstream's is the trace only,
 * on an 800 x 480 panel where the waterfall is 72 rows; here it is 92 rows
 * of the 233 the two share, too much glass to leave dead. Same x mapping
 * in both: the waterfall's columns are the trace's bins.
 *
 * The window is PAN_SHOWN bins, 375 Hz each, centred on the dial whatever
 * the mode's offset (s_pan_lo) - so a pixel is a frequency without asking
 * where the tuner is. The snap reads the SMOOTHED trace, as upstream's
 * reads s_db_traza: what is drawn is what the finger aimed at.
 */
#define SPEC_TAP_MOVE_PX    6

static uint8_t  s_sp_active, s_sp_moved;
static uint16_t s_sp_x0, s_sp_prev;
static float    s_sp_accum;

/* Upstream's tune_mueve_pasos(): `pasos` steps from f, the first one only
 * as far as the grid - so a dragged dial lands on the step grid, as the
 * knob's does upstream. */
static int64_t tune_mueve_pasos(int64_t f, int32_t pasos, int64_t paso_hz)
{
    int64_t resto;

    if (paso_hz <= 0 || pasos == 0) { return f; }
    resto = f % paso_hz;
    if (resto < 0) { resto += paso_hz; }
    if (resto != 0) {
        f -= resto;
        if (pasos > 0) { f += paso_hz; pasos--; }
        else           { pasos++; }
    }
    return f + (int64_t)pasos * paso_hz;
}

static int spec_en_zona(uint16_t x, uint16_t y)
{
    return x >= SPC_TRACE_X && x < SPC_TRACE_X + SPC_TRACE_W
        && y >= SPC_Y && y < SPC_WF_Y + SPC_WF_ROWS;
}

static void spec_tap_tune(uint16_t x)
{
    int32_t  px = (int32_t)x - (int32_t)SPC_TRACE_X;
    int      m  = radio_mode_get();
    uint8_t  eng = 0U;
    float    bin, b;
    int64_t  f, paso = (int64_t)TUNE_STEP_HZ;

    if (px < 0) { px = 0; }
    if (px > (int32_t)SPC_TRACE_W - 1) { px = (int32_t)SPC_TRACE_W - 1; }
    bin = ((float)px + 0.5f) * (float)PAN_SHOWN / (float)SPC_TRACE_W;

    if (s_db_traza_lista) {
        /* In CW the tone is a peak, sought as one although the mode
         * underneath is a sideband - upstream's, word for word. */
        snap_modo_t sm = (m == (int)DEMOD_MODE_USB) ? SNAP_USB
                       : (m == (int)DEMOD_MODE_LSB) ? SNAP_LSB : SNAP_PICO;

        b = spec_snap_bin(s_db_traza, PAN_SHOWN, bin, sm, &eng);
    } else {
        b = bin;
    }
    f = (int64_t)s_tuned_hz
      + (int64_t)(((double)b - (double)(PAN_SHOWN / 2U)) * (double)PAN_BIN_HZ);

    /* And landing on a carrier in CW puts the DIAL a tone below it, so it
     * sounds at the pitch instead of at zero beat, which is silence -
     * upstream's cw_signo_vfo(): USB, so below. */
    if (eng && m == RADIO_MODE_CW) {
        f -= (int64_t)(cw_get_pitch_hz() + 0.5f);
    }

    /* Off the grid only when it landed on a signal: there the exact
     * frequency is the point, and rounding would undo it. */
    if (!eng && paso > 0) {
        int64_t rem = f % paso;

        if (rem < 0) { rem += paso; }
        f -= rem;
        if (rem * 2 >= paso) { f += paso; }
    }
    if (f < (int64_t)TUNE_MIN_HZ) { f = (int64_t)TUNE_MIN_HZ; }
    if (f > (int64_t)TUNE_MAX_HZ) { f = (int64_t)TUNE_MAX_HZ; }
    if ((uint32_t)f != s_tuned_hz && dial_tune(f)) {
        console_printf("  spectrum tap at x %u: %s %u Hz\n", (uint32_t)x,
                       eng ? "on the signal," : "no signal, the grid:",
                       s_tuned_hz);
    }
}

static void spec_drag_tune(uint16_t x, uint16_t prev_x)
{
    int32_t dx   = (int32_t)x - (int32_t)prev_x;
    float   step = (float)TUNE_STEP_HZ;
    int32_t steps;

    if (dx == 0 || step <= 0.0f) { return; }
    /* Right is DOWN: the content moves with the finger. */
    s_sp_accum -= (float)dx * ((float)PAN_SHOWN_HZ / (float)SPC_TRACE_W);
    steps = (int32_t)(s_sp_accum / step);
    if (steps == 0) { return; }
    s_sp_accum -= (float)steps * step;
    (void)dial_tune(tune_mueve_pasos((int64_t)s_tuned_hz, steps, (int64_t)TUNE_STEP_HZ));
}

/* A press, or a finger still down: returns 1 if the spectrum owns it. */
static int spec_gesto(uint16_t x, uint16_t y, uint8_t nuevo)
{
    if (nuevo) {
        s_sp_active = (uint8_t)(menu_open_id() == MENU_NONE && spec_en_zona(x, y));
        s_sp_moved  = 0U;
        s_sp_x0 = s_sp_prev = x;
        s_sp_accum  = 0.0f;
        return s_sp_active;
    }
    if (!s_sp_active) { return 0; }
    {
        int32_t d = (int32_t)x - (int32_t)s_sp_x0;

        if (d < 0) { d = -d; }
        if (d > SPEC_TAP_MOVE_PX) { s_sp_moved = 1U; }
    }
    if (s_sp_moved) { spec_drag_tune(x, s_sp_prev); }
    s_sp_prev = x;
    return 1;
}

/* The lift: a press that never moved is a tap - at where it STARTED,
 * as upstream's, because the lift's coordinates are not to be trusted. */
static void spec_suelta(void)
{
    if (s_sp_active && !s_sp_moved) { spec_tap_tune(s_sp_x0); }
    if (s_sp_active && s_sp_moved) {
        console_printf("  spectrum drag: dial %u Hz\n", s_tuned_hz);
    }
    s_sp_active = 0U;
    s_sp_moved  = 0U;
}

/*
 * WHAT THE FINGER LANDED ON, reported to the console.
 *
 * Nothing is wired to an action - there is no band list, no menu and
 * no mode to change - so the only thing this can usefully do is say
 * what was hit. That is not a placeholder: the hit boxes come out of
 * the drawing itself (ui_top.c records each chip's rectangle as it
 * paints it), so this is the test that the geometry which LOOKS right
 * in the PNG is also reachable with a finger on a 480 px panel, which
 * is a question the host render cannot answer.
 *
 * The pressed button is drawn, and only that button, through
 * ui_act_draw_one() - which is also the cheapest possible check that
 * partial repaint lands where it should.
 */
static void ui_touch(void)
{
    static int8_t  s_down = -1;
    /*
     * THE PRESS IS A TRANSITION HERE, NOT THE CONTROLLER'S EVENT CODE.
     *
     * It was `if (pt[0].event != 0U) return;` - report only on the
     * FT6236's own event 0, "contact down" - and the result was that
     * the action bar answered every tap and nothing above it answered
     * any. The six buttons worked because their branch never looked
     * at the event.
     *
     * The reason is the frame rate. A frame is about 140 ms here, most
     * of it the waterfall going out over SPI, and the controller
     * reports event 0 exactly once per finger: a tap that begins and
     * ends between two polls is only ever seen as event 2, "contact",
     * or missed outright. Sampling a one-shot flag at 7 Hz does not
     * work, and no amount of pressing harder makes it work.
     *
     * So the edge is detected here, from "was anything down last time"
     * - which is true whichever report the poll happens to land on.
     */
    static uint8_t s_was_down;
    touch_point_t  pt[TOUCH_MAX_POINTS];
    uint32_t       n = touch_poll(pt);

    if (s_tap_n != 0U) {                /* ui_tap(), from the bench */
        s_tap_n  = 0U;
        n        = 1U;
        pt[0].x  = s_tap_x;
        pt[0].y  = s_tap_y;
    } else if (s_hold_on) {             /* ui_hold(), from the bench */
        if ((int32_t)(board_millis() - s_hold_until) >= 0) {
            s_hold_on = 0U;             /* this poll is the lift */
        } else {
            n       = 1U;
            pt[0].x = s_hold_x;
            pt[0].y = s_hold_y;
        }
    } else if (s_sw_n != 0U) {          /* ui_swipe(), from the bench */
        uint32_t k = SWIPE_POLLS - s_sw_n;

        s_sw_n--;
        n       = 1U;
        pt[0].x = (uint16_t)((int32_t)s_sw_x0 + ((int32_t)s_sw_x1 - (int32_t)s_sw_x0)
                             * (int32_t)k / (int32_t)(SWIPE_POLLS - 1U));
        pt[0].y = s_sw_y;
    }

    if (n == 0U) {
        s_was_down = 0U;
        menu_touch_up();
        spec_suelta();
        if (s_down >= 0) {
            int8_t was = s_down;
            s_down = -1;
            s_act.pressed = -1;
            ui_act_draw_one(&s_act, (uint8_t)was);
            act_release(was);
        }
        return;
    }

    {
        uint16_t x = pt[0].x, y = pt[0].y;
        int8_t   b = ui_act_hit(x, y);
        uint8_t  nuevo = (uint8_t)!s_was_down;

        s_was_down = 1U;

        if (b >= 0) {
            if (b != s_down) {
                int8_t was = s_down;
                s_down = b;
                s_act.pressed = b;
                if (was >= 0) {
                    /* Redrawing the one it left as well, or the old
                     * highlight stays on a button nobody is touching. */
                    s_act.pressed = -1;
                    ui_act_draw_one(&s_act, (uint8_t)was);
                    s_act.pressed = b;
                }
                ui_act_draw_one(&s_act, (uint8_t)b);
                console_printf("  touch (%3u,%3u) -> boton %d \"%s\"\n",
                               (uint32_t)x, (uint32_t)y, (int32_t)b,
                               k_act_names[b][IDIOMA_ES]);
            }
            return;
        }

        if (s_down >= 0) {
            int8_t was = s_down;
            s_down = -1;
            s_act.pressed = -1;
            ui_act_draw_one(&s_act, (uint8_t)was);
        }

        /* An open menu owns its area. */
        if (menu_touch_down(x, y, nuevo)) { return; }

        /* In a text mode the scope and the panel are there instead. */
        if (digi_toque(x, y, nuevo)) { return; }

        /* With none open, the spectrum and the waterfall tune. */
        if (spec_gesto(x, y, nuevo)) { return; }

        /* Everything above the action bar, once per press. */
        if (!nuevo) { return; }

        {
            ui_top_hit_t h = ui_top_hit(x, y);
            static const char *const k_chip[] = {
                "-", "AGC", "BW", "ATT", "NCO", "JMP", "SPK", "OVR"
            };
            const char *que;

            if      (h != UI_TOP_HIT_NONE) { que = k_chip[h]; }
            else if (ui_top_freq_hit(x, y))  { que = "frecuencia"; }
            else if (ui_top_clock_hit(x, y)) { que = "reloj"; }
            else if (ui_top_band_hit(x, y))  { que = "banda"; }
            else if (ui_top_mem_hit(x, y))   { que = "MEM"; }
            else                             { que = 0; }

            console_printf("  touch (%3u,%3u)%s%s\n",
                           (uint32_t)x, (uint32_t)y,
                           que ? " -> " : "", que ? que : "");

            /* What upstream does with them (its ui_top tap handling). */
            if (h == UI_TOP_HIT_AGC) { agc_cycle(); return; }
            if (h == UI_TOP_HIT_BW)  { bw_cycle();  return; }
            if (h == UI_TOP_HIT_NONE && ui_top_band_hit(x, y)) {
                menu_toggle(MENU_BANDAS);
                return;
            }
            /* MEM: the memory channels, and again to close them - as
             * upstream's chip does when no scan is running. */
            if (h == UI_TOP_HIT_NONE && ui_top_mem_hit(x, y)) {
                menu_toggle(MENU_MEM);
                return;
            }
            /* The frequency: upstream's keypad. Its zone is ui_top.c's,
             * which drew the digits and so knows where they end. */
            if (h == UI_TOP_HIT_NONE && ui_top_freq_hit(x, y)) {
                menu_teclado_open();
                return;
            }

            /*
             * THE KNOB PILL NO LONGER REPORTS TOUCHES - 22/11/2026.
             *
             * From the bring-up, when nothing above the action bar did
             * anything, the pill said what the finger had hit ("Tocado" /
             * "Sin asignar") so the hit boxes could be checked on the
             * glass. Everything there is wired now, and the report had
             * turned into a pill that read "Unassigned" after a tap on
             * the mode chip - which upstream gives no action either - and
             * stayed so. The console line above still says what was hit.
             */
        }
    }
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

    /*
     * IS THE HEARTBEAT ALIVE, asked at the first line that can ask.
     *
     * Everything timed in this radio hangs off SysTick, and
     * board_delay_ms() has a cycle-count backstop that HIDES its
     * absence - the boot continues, every delay about fifteen times too
     * long, every deadline never reached. That backstop was added so a
     * dead tick could not hang the boot before the console existed, and
     * it did its job; what it also did was turn a stopped heartbeat
     * into a radio that merely behaves strangely.
     */
    console_printf("SysTick : CTRL 0x%08x  LOAD %u  VAL %u  millis %u  "
                   "PRIMASK %u\n",
                   SysTick->CTRL, SysTick->LOAD, SysTick->VAL,
                   board_millis(), __get_PRIMASK());

    /*
     * AND IS ANY EXCEPTION STILL MARKED ACTIVE? Only possible on a load
     * over SWD: the startup code returns out of the exception the old
     * image was halted in (startup_stm32h743.S, step 0), and a second
     * one nested underneath - SysTick inside the block interrupt - would
     * be left behind. An active bit cannot be cleared by writing to it,
     * and while it stays set that interrupt never arrives again. So say
     * so, before the symptom - a radio with no audio - can be mistaken
     * for anything else.
     */
    {
        uint32_t act = 0U;

        for (uint32_t i = 0; i < 8U; i++) { act |= NVIC->IABR[i]; }
        act |= SCB->SHCSR & (SCB_SHCSR_SYSTICKACT_Msk
                             | SCB_SHCSR_PENDSVACT_Msk
                             | SCB_SHCSR_MONITORACT_Msk
                             | SCB_SHCSR_SVCALLACT_Msk
                             | SCB_SHCSR_USGFAULTACT_Msk
                             | SCB_SHCSR_BUSFAULTACT_Msk
                             | SCB_SHCSR_MEMFAULTACT_Msk);
        if (act != 0U || __get_IPSR() != 0U) {
            console_printf("*** started with an exception still active: "
                           "IPSR %u, IABR0 0x%08x, SHCSR 0x%08x - its "
                           "interrupt\n    will never arrive. Power-cycle "
                           "the radio. ***\n", __get_IPSR(), NVIC->IABR[0],
                           SCB->SHCSR);
        }
    }
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
    if (SELFTEST) {
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
    } else {
        console_printf("Boot      : SELFTEST 0 - the bring-up tests are "
                       "skipped. Build with\n");
        console_printf("            SELFTEST=1 to run them (main.c, "
                       "\"SELFTEST\").\n\n");
    }

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

    /*
     * ANYTHING THAT NEEDS A PERSON GOES HERE, as early as it can: the
     * codec is configured and MCLK is running, so a sound is possible
     * from this point, and the panel comes up two lines below. Arriving
     * three seconds after the load rather than thirty-four is the
     * difference between a check somebody performs and one somebody
     * misses.
     *
     * All of them are retired now, having answered, and the list of what
     * each settled is further down where they are stood aside. What
     * stays here is the SETUP they used to own.
     */
    PHASE(13);

    /*
     * THE PANEL COMES UP HERE, BEFORE ANY TEST THAT NEEDS A HAND, and
     * that is the whole reason it moved. It used to be brought up in
     * step 6 with the spectrum, forty seconds later, so every test that
     * asked for a finger asked a dark screen and the person holding the
     * radio had to be told what to do from the other end of a debugger.
     *
     * It costs nothing to move: three calls, all self-contained, and the
     * clock it wants - 30 MHz on SPI3 - was settled in step 2 and does
     * not depend on anything between there and here. Step 6 keeps the
     * rest of its work and simply finds the panel already awake.
     */
    spi3_init(SPI3_DIV8);
    ili9488_init(ILI9488_FMT_18BIT);
    backlight_init();                   /* on, at 100 % until the FRAM says */
    s_panel_up = 1;
    console_printf("Panel     : SPI3 at %u Hz, 18 bpp, 480x320 - up early, "
                   "so the\n", spi3_hz());
    console_printf("            tests below can ask for a hand on the "
                   "screen.\n");

    /*
     * THE PANEL ON DMA, from its first picture - 21/11/2026.
     *
     * Every block gfx2 and gfx hand over goes out by DMA1 stream 2 from
     * here on, while the CPU converts the next one (spi3.c, "TRANSMIT BY
     * DMA"; rm68120_ili9488.c for the two halves of the staging buffer).
     * What it buys is the bus time the polled loop lost to interrupts:
     * the waterfall measured 48 ms for the 32 ms its 121 kB need.
     *
     * The self-test sends 256 bytes with CS high - the panel does not
     * listen - and only a clean, plausibly-timed transfer switches the
     * DMA on. Anything else leaves it off and every blit polled, which
     * is how this radio drew until today: a failure here costs frame
     * rate, never the screen.
     */
    {
        uint32_t probe_us = 0U;
        int      dma_rc   = ili9488_dma_init(&probe_us);

        console_printf("Panel DMA : %s - DMA1 stream 2, request 62, "
                       "probe 256 B in %u us",
                       (dma_rc == 0) ? "on" : "OFF", probe_us);
        if (dma_rc != 0) {
            console_printf(" - rc %d, every blit stays polled", dma_rc);
        }
        if (spi3_dma_mux_released() != 0U) {
            console_printf("\n            request 62 taken back from "
                           "mux channels 0x%04x", spi3_dma_mux_released());
        }
        console_printf("\n");

        /*
         * THE SWITCH, said once here so that it is in the image at all:
         * nothing else calls it, and --gc-sections would drop it. It is
         * the A/B of the two paths on one running radio, same picture,
         * same signal - see the `lcd` column of the status line:
         *
         *   swd-call.sh build/bringup.elf ili9488_dma_enable 0   (polled)
         *   swd-call.sh build/bringup.elf ili9488_dma_enable 1   (DMA)
         *
         * On only takes if the self-test above passed.
         */
        ili9488_dma_enable(1);
    }

    /*
     * THE ENCODER IS SET UP HERE, NOT INSIDE ITS TEST, and that
     * placement is the whole of trap 17 applied before it could bite a
     * second time. The audio went silent for three days because step 4's
     * tone test owned the unmute and the test was retired; the two
     * encoder tests below are about to be retired the same way, having
     * answered what they were written for. So the SETUP leaves them
     * first: encoder_init() configures the pins and
     * board_encoder_sampling(1) starts the 1 kHz tick, here, where the
     * radio's own startup can be read top to bottom.
     *
     * What the two of them settled, in order:
     *   test_encoder_lines()  the A/B lines float when the contact is
     *                         open and follow a pull this firmware
     *                         wrote, on both encoders - 297 changes in
     *                         twenty seconds of turning.
     *   test_encoder()        upstream's decoder counts whole detents
     *                         off RENC2, +1 and -1 with the direction
     *                         reversing when the hand does.
     * Both still build and can be called back when something in the
     * wiring changes.
     */
    /*
     * THE ADC COMES BEFORE THE ENCODER, because the encoder's button now
     * depends on it: -DENC_BTN_READ=adc_enc1_down hands upstream's
     * debounce a function instead of a pin, and that function answers
     * from whatever the converter last measured. Start it the other way
     * round and the first 20 ms of debounce integrate a reading that
     * does not exist yet.
     *
     * Ownership is here for the same reason the encoder's is: a test
     * that owns a setup takes it away when the test is retired, which is
     * what trap 17 cost.
     */
    s_adc_rc = adc_init();

    /*
     * ADC3 AFTER ADC1 AND BEFORE SAMPLING STARTS. It brings up the
     * internal reference, which is what turns VREF+ from a constant
     * measured against a meter one afternoon into something the radio
     * measures for itself every millisecond - and every voltage this
     * firmware reports is a fraction of VREF+.
     *
     * Its failure is not fatal and must not be: without it adc_mv()
     * falls back to that constant, which is wrong by under a per cent
     * rather than wrong by the whole reading.
     */
    s_adc3_rc = adc3_init(ADC3_CH_VREFINT);

    adc_sampling(1);

    /*
     * NOT INTERACTIVE, AND THE LADDER IS RE-MEASURED SOMEWHERE ELSE.
     *
     * The thresholds do need taking again - adc_init() now opens the
     * analog switch, which takes the PA1_C pad off the ladder and can
     * move every plateau. The interactive half of this test was turned
     * back on to do it and then HUNG: one heartbeat and no more, the
     * boot never reaching the screen.
     *
     * Chasing that would spend the user's bench time on the instrument
     * rather than on the radio, so the measurement moved to the display
     * loop's own once-a-second buttons line, which has been printing
     * correctly all along. The radio comes up, the buttons get pressed
     * whenever there is a hand free, and the plateaus are read off the
     * log. The hang is written down in WORKING.md rather than fixed.
     */
    if (SELFTEST) {
        test_adc(0);
        test_vrefint();
    }

    /*
     * RETIRED, HAVING ANSWERED: the divider needed no trim, because the
     * disagreement with the meter was smaller than the meter's own
     * resolution. It prints the table to read a trim off and costs
     * twenty seconds of somebody's attention on every load, so it waits
     * for the day there is a millivolt-resolution meter to answer it
     * with. See ADC_BAT_TRIM_PPT.
     */
    (void)test_bat_calib;

    /*
     * RETIRED, HAVING ANSWERED, and both are kept building for the day
     * the wiring question comes back:
     *
     *   test_adc_pins()      which channel carries which net - settled
     *                        against a multimeter: ADC_IN is channel 16,
     *                        ENC_BUT is channel 1.
     *   test_audio_listen()  the DMA transport is audibly clean where
     *                        the polled one clicks.
     *
     * Retiring the listening test is safe in a way step 4's tone test
     * was not, and the difference is worth stating because it is trap
     * 17: that one OWNED the codec's unmute and took it away with it,
     * costing three days of silence. This one restores everything it
     * touches - it re-mutes and hands the SAI back - so nothing depends
     * on it having run.
     */
    (void)test_adc_pins;
    (void)test_audio_listen;

    encoder_init();
    encoder2_init();
    board_encoder_sampling(1);
    console_printf("Encoder   : ../../User/encoder.c on RENC1 (PC0/PC1), "
                   "the big knob,\n");
    console_printf("            sampled at 1 kHz from SysTick.\n");
    console_printf("            Button: ENC1 off the ADC ladder, through "
                   "upstream's own\n");
    console_printf("            debounce. Short press cycles the tuning "
                   "step.\n");
    console_printf("            The SAME FILE again on RENC2 (PC2/PC3), "
                   "the small knob -\n");
    console_printf("            compiled twice with its names renamed, so "
                   "both knobs\n");
    console_printf("            run the one tested decoder. See "
                   "src/enc2.h.\n\n");

    (void)test_encoder_lines;
    (void)test_encoder;

    /*
     * The all-port scan is kept, retired, and it earned that: it is what
     * proved the lines were not moving ANYWHERE rather than only where
     * the pin map says. Its own flaw is worth remembering before it is
     * believed again - a pin in analog mode, which is the H7's reset
     * state for most of them, reads 0 for ever and is invisible to it.
     */
    (void)test_encoder_scan;

    /*
     * test_rx_timing() CHOOSES the receive edge and applies it - which
     * is why sai1.c's default is now the answer it always chose, so that
     * skipping it changes nothing. test_control_bus() restores every
     * register it touches.
     */
    if (SELFTEST) {
        test_rx_timing();
        test_control_bus();
    }

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

    if (SELFTEST) {
        console_printf("\n");
        tuner_report_plans();
    }

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

    /*
     * THE ADD-ON BOARD, INTO A STATE WE CHOSE, BEFORE ANY RF
     * MEASUREMENT - and this line is the whole lesson of step 9.
     *
     * It sits in series ahead of the receiver and it LATCHES: its
     * attenuator and its shift register keep their contents across a
     * firmware reload, because the board has its own power. Measuring
     * before setting it means measuring through whatever the last
     * thing to touch it left behind, and for one afternoon that was
     * 31 dB of attenuation nothing in this firmware knew about. Every
     * level in that session was wrong by the same unknown amount, and
     * the symptom was a filter bank that appeared not to switch.
     *
     * Zero attenuation because this radio has its own gain controls
     * and rf_autorange() to work them; the add-on board's attenuator
     * is a second, coarser one and having two of them fighting is how
     * a level becomes untraceable.
     */
    msi001_bpf(msi001_bpf_for(GEN_HZ));
    fft_init();                 /* test_front_end() now needs the FFT */

    /*
     * THE ADD-ON BOARD, INTO A STATE WE CHOSE, BEFORE ANY RF
     * MEASUREMENT - and this placement is the whole lesson of step 9.
     *
     * It sits in series ahead of the receiver and it LATCHES: its
     * attenuator and its shift register keep their contents across a
     * firmware reload, because the board has its own power. So
     * measuring before setting it means measuring through whatever the
     * last thing to touch it left behind - and once this driver had
     * written one wrong filter code, every run after it measured a
     * dead receiver and blamed the bench.
     *
     * The attenuator goes to zero because this radio has its own gain
     * controls and rf_autorange() to work them. Two attenuators
     * fighting is how a level becomes untraceable.
     *
     * The filter code is SWEPT rather than looked up, because the two
     * drawings of this board disagree about the output switch and the
     * sweep is the only thing that has been right so far.
     */
    PHASE(17);
    addon_init();
    console_printf("Add-on    : Vext_ON %s, 595 idle byte 0x%02x\n",
                   addon_powered() ? "on" : "*** OFF ***",
                   (uint32_t)addon_595_shadow());
    addon_set_att_halfdb(0U);
    console_printf("            attenuator set to %u.%u dB "
                   "(was whatever was latched)\n\n",
                   (uint32_t)(addon_att_halfdb() / 2U),
                   (uint32_t)((addon_att_halfdb() & 1U) ? 5U : 0U));

    if (SELFTEST) {
        test_addon_filter_sweep();
        test_addon_atten();
        test_front_end();

        /*
         * STEP 8'S FIRST QUESTION, and it runs before the screen rather
         * than after it: the DC offset is the one measurement that
         * invalidates every other level on the board, so anything measured
         * before it is fixed has to be measured again. It needs no
         * generator - a standing offset is the receiver's own error.
         */
        test_dc_offset();
        test_signal_quality();

        /* The envelope test wants a gain that is not clipping, or it
         * measures our own converter instead of the generator. */
        msi001_preamp(1);
        (void)rf_autorange();
        test_am_envelope();

        /*
         * STEP 9a. The gain is already autoranged for the 50 ohm path, so
         * the High-Z position will simply read low - which is the whole
         * point and carries no risk of clipping.
         */
        test_addon_relay();
        test_addon_filters();       /* table now measured, not read */
    }

    /*
     * THE ADD-ON BOARD, SET FOR LISTENING BY main() - not left as the
     * last test found it, and on a normal boot no test touches it at all.
     *
     * It latches across power cycles, so without this it would run on
     * whatever the previous boot - or the previous firmware - left.
     *
     *   filter  the one for the dial. dial_tune() keeps it right from
     *           the first detent on; this covers the time before that.
     *   relay   50 ohm, PD8 (BOARD.md section 7, "And PD8 is the 50 ohm
     *           input"). test_addon_relay() used to choose it by
     *           MEASURING a generator, and with an antenna instead of a
     *           generator that is a coin toss between two readings of
     *           noise; the stock firmware leaves it on 50 ohm too.
     *
     * The attenuator was set to 0 dB above, and the tests that move it
     * put it back.
     */
    addon_set_filter(addon_filter_for(GEN_HZ));
    addon_input_50r();
    console_printf("Add-on    : filter %s, 50 ohm input, %u.%u dB "
                   "attenuation\n\n", addon_filter_name(addon_filter()),
                   (uint32_t)(addon_att_halfdb() / 2U),
                   (uint32_t)((addon_att_halfdb() & 1U) ? 5U : 0U));

    /*
     * Settings for the display, with the gain reduction MEASURED rather
     * than remembered - see rf_autorange(). It used to be a hard-coded
     * 12 dB, which was correct against one generator at one level and
     * put the converter at 100 % the day the level changed.
     */
    msi001_set_am_port(1);              /* port 2, the 75 ohm one */

    /*
     * WHAT THE LAST SESSION LEFT is read HERE, not where the dial is
     * set: the RF front end is set up first, and since record version 2
     * the FRAM holds the person's ATT and preamp. The dial, volume and
     * step wait in s_boot_set for PHASE(20).
     */
    s_boot_have = SETTINGS_LIVE ? settings_load(&s_boot_set) : 0;
    if (!SETTINGS_LIVE) {
        console_printf("Settings  : not read or written - a RETUNE_TEST "
                       "build (SETTINGS_LIVE)\n");
    }

    if (s_boot_have && s_boot_set.has_rf
        && s_boot_set.rf_gr <= RF_GR_MAX && s_boot_set.preamp <= 1U) {
        /*
         * AS THE HAND LEFT THEM, and rf_autorange() does not run: a
         * gain the person set is not second-guessed by one measured on
         * whatever happens to be on the dial at power-on. The small
         * knob is one press away if the band has changed since.
         */
        s_rf_preamp = s_boot_set.preamp;
        s_rf_gr     = s_boot_set.rf_gr;
        msi001_preamp(s_rf_preamp);
        msi001_set_gain(s_rf_gr, 0, 0, 0);

        console_printf("RF level   : %u dB of baseband gain reduction, "
                       "preamp %s - from the FRAM,\n", s_rf_gr,
                       s_rf_preamp ? "ON" : "OFF");
        console_printf("             as the knob left them; "
                       "rf_autorange() not run.\n\n");
    } else {
        if (s_boot_have && s_boot_set.has_rf) {
            console_printf("Settings  : stored ATT %u dB / preamp %u not "
                           "usable - measuring instead\n",
                           (uint32_t)s_boot_set.rf_gr,
                           (uint32_t)s_boot_set.preamp);
        }
        s_rf_preamp = 1;
        msi001_preamp(1);

        /* KEPT, not just printed: the small knob adjusts from here, so
         * the starting point has to be the measured one rather than a
         * number this code assumed. */
        s_rf_gr = rf_autorange();

        console_printf("RF level   : %u dB of baseband gain reduction, "
                       "ADC at %u%% - chosen by\n", s_rf_gr,
                       s_cap_used_pct);
        console_printf("             rf_autorange(), not by a constant. "
                       "The small knob\n");
        console_printf("             adjusts it from here; its button "
                       "toggles the preamp.\n\n");
    }
    board_delay_ms(20);

    /* ============================================================== */
    /* STEP 10: THE SAI ON DMA                                        */
    /* ============================================================== */

    /*
     * EVERY TEST ABOVE RAN ON THE POLLED TRANSPORT, AND THAT IS
     * DELIBERATE.
     *
     * capture() stops the blocks and polls a fresh block, so each of
     * those measurements is of samples taken after the register it was
     * testing was written. The DMA ring cannot promise that - it is
     * always running, so a block fetched straight after a gain change
     * can be entirely from before it - and nothing above would have
     * failed, it would just have measured the previous setting. Which is
     * WORKING.md's "did the measurement actually happen?" with a new
     * costume on.
     *
     * So the transport changes here, at the boundary between "measure
     * the hardware" and "run the radio", and every number in BOARD.md
     * section 6 stays comparable with the ones taken before this step.
     *
     * Both are tests, and neither OWNS the transport the radio runs on:
     * they leave the DMA armed at the tone test's sizes, and the display
     * loop's first DMA frame finds that and re-arms at AUDIO_HALF_FRAMES
     * after its polled frames have stopped it. With SELFTEST 0 the loop
     * arms it from nothing, on its first frame - the same arm.
     */
    if (SELFTEST) {
        test_audio_gap();
        test_dma_transport();
    }

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

    /*
     * THE PANEL IS ALREADY UP - it moved to PHASE(13) so the tests that
     * need a hand could ask on the screen. Nothing is repeated here;
     * the comment above about 30 MHz still explains the clock, and the
     * call that chooses it now lives up there.
     */
    console_printf("Panel     : SPI3 at %u Hz, 18 bpp, 480x320\n", spi3_hz());

    console_printf("\ngfx2     : ../../User/gfx2.c at %ux%u, band %u rows\n",
                   (uint32_t)GFX2_W, (uint32_t)GFX2_H,
                   (uint32_t)GFX2_BAND_H);

    /*
     * gfx_demo() WAS WHAT DREW HERE, and it has been retired rather
     * than deleted: it was step 6's proof that upstream's drawing API
     * reaches this panel at all, and the real UI below is a better
     * proof of the same thing in every respect. It still builds, for
     * the day something in the band compositor needs isolating from
     * everything the UI puts on top of it. Same treatment as
     * test_bpf_sweep() and the rate sweep.
     */
    (void)gfx_demo;
    (void)s_gfx_frame;

    /*
     * THE TOUCH PANEL, which this image has not needed until now.
     * FT6236 at 0x38 on I2C3, calibrated in step 3 - see touch.h. It
     * is here so that the UI's own hit boxes can be checked against a
     * finger, which is the one thing the host render cannot do.
     */
    i2c3_init();
    if (i2c3_recover_clocks() != 0) {
        /* Worth a line every time it is not zero: it says the previous
         * image was halted mid-transfer, which is normal after a load
         * and would be alarming at a power-on. */
        console_printf("I2C3      : bus recovered, %d clocks\n",
                       i2c3_recover_clocks());
    }
    if (TOUCH_WAKE_TEST) {
        test_touch_wake(TOUCH_WAKE_TEST);
    }
#if FRAM_TEST
    test_fram(FRAM_TEST);
#endif
#if MSI_ROUND_TEST
    test_msi_round();
#endif
    if (touch_init()) {
        console_printf("Touch     : FT6236 at 0x38, cipher 0x%02x "
                       "vendor 0x%02x\n",
                       (uint32_t)touch_chip_id(),
                       (uint32_t)touch_vendor_id());
    } else {
        /*
         * SAY WHAT THE BUS LOOKS LIKE, not just that it failed.
         *
         * "No answer" has at least three causes that need different
         * work - no pull-ups or the wrong pins (a line stuck low), a
         * controller that has not come out of reset (lines idle, no
         * ACK), and a device that answers at some other address. One
         * console line separates them, and the first run of this image
         * reported the failure without any of it.
         */
        uint32_t lines = i2c3_line_levels();

        console_printf("Touch     : *** no answer at 0x38 ***\n");
        console_printf("            SCL=%u SDA=%u  IRQ(PC8)=%s\n",
                       (lines >> 1) & 1U, lines & 1U,
                       i2c3_touch_irq_asserted() ? "low" : "high");
        {
            static const char *const k_paso[I2C3_LV_N] = {
                "al entrar", "PE apagado", "tras rescate"
            };
            uint32_t k;

            /* %s, not %-13s: console_printf() has no width modifier
             * for strings, and an unsupported conversion prints
             * itself AND DOES NOT CONSUME ITS ARGUMENT, so every
             * value after it on the line shifts by one. The first
             * version of this printed a string pointer labelled
             * "SCL=" and the real SCL labelled "SDA=". It is trap 8
             * in WORKING.md, and it still caught me. */
            console_printf("            lineas durante init "
                           "(rescate %d clocks):\n", i2c3_recover_clocks());
            for (k = 0U; k < I2C3_LV_N; k++) {
                uint8_t v = i2c3_level_at(k);
                console_printf("              SCL=%u SDA=%u  %s\n",
                               (uint32_t)((v >> 1) & 1U),
                               (uint32_t)(v & 1U), k_paso[k]);
            }
        }
        console_printf("            cipher 0x%02x vendor 0x%02x "
                       "(want 0x36 / 0x11)\n",
                       (uint32_t)touch_chip_id(),
                       (uint32_t)touch_vendor_id());
        console_printf("            tras el reset: SCL=%u SDA=%u | "
                       "%u intentos, SDA baja en el %d (lectura %s)\n",
                       (uint32_t)((touch_dbg_after_reset() >> 1) & 1U),
                       (uint32_t)(touch_dbg_after_reset() & 1U),
                       (uint32_t)touch_dbg_tries(),
                       (int32_t)touch_dbg_sda_low_at(),
                       touch_dbg_rc_at() ? "OK" : "fallida");
        console_printf("            scanning: ");
        for (uint8_t a = 0x08U; a <= 0x77U; a++) {
            board_watchdog_feed();
            if (i2c3_probe(a)) { console_printf("0x%02x ", (uint32_t)a); }
        }
        console_printf("\n            the screen will draw but not "
                       "respond\n");
    }

    PHASE(19);
    fft_init();                 /* ../../User/fft.c */
    fft_tam_iq(PAN_BINS);
    /*
     * UPSTREAM'S PANADAPTER. 2078 lines of spectrum.c drawing through
     * gfx.c and gfx2.c, all three compiled in place for this board.
     * panbringup.c - the stand-in that used to draw here - is now only
     * used for its peak search, and goes when something upstream
     * provides that too.
     */
    spectrum_init();
    console_printf("spectrum : ../../User/spectrum.c, %u palettes, "
                   "style/heatmap from upstream\n",
                   (uint32_t)spectrum_palette_count());

    /*
     * THE IMAGE CORRECTOR, UPSTREAM'S OWN - ../../User/iqbal.c.
     *
     * 1694 lines grafted without a single change. It is the right tool
     * for what step 5 measured on this board: 23 to 32 dB of raw image
     * rejection, which is ordinary for an uncorrected quadrature mixer
     * and not good enough to listen through.
     *
     * Two things are hooked up, and they do different jobs:
     *
     *   iqbal_bloque() runs on the raw interleaved block, measuring one
     *   block in eight and correcting all of them. Upstream calls it
     *   from the DMA interrupt, before the block reaches anyone, so the
     *   demodulator and the display see the same numbers. Here it goes
     *   in the same place relative to the data - immediately after the
     *   capture and before the FFT - for the same reason.
     *
     *   fft_set_bins_hook(iqbal_calibra) gives it the COMPLEX spectrum.
     *   fft.c computes it and throws the phase away two lines later, so
     *   the hook exists to let the calibrator see the phase without a
     *   second transform. That is upstream's own design, and it is why
     *   fft.c takes a function pointer instead of calling iqbal by
     *   name: a transform has no business knowing an I/Q corrector
     *   exists.
     */
    iqbal_init();
    fft_set_bins_hook(iqbal_calibra);

    /*
     * Retune so the carrier sits off centre. Everything up to here put
     * it at +3000 Hz, which is the right place for a single-bin
     * measurement and the wrong place for a picture: three kilohertz is
     * eight bins at 375 Hz, which on a 480-column display is seven
     * pixels from the middle and indistinguishable from the LO leakage.
     */
    (void)msi001_retune(GEN_HZ - SPEC_OFFSET_HZ);

    const msi_plan_t *sp = msi001_last_plan();
    console_printf("Tuned     : %u Hz, so the generator at %u Hz should\n",
                   sp->f_actual, GEN_HZ);
    console_printf("            appear %+d Hz from centre.\n\n",
                   SPEC_OFFSET_HZ);

    console_printf("Spectrum and I/Q correction - UPSTREAM's own\n");
    console_printf("------------------------------------------\n");
    console_printf("  ../../User/fft.c and ../../User/iqbal.c, compiled in\n");
    console_printf("  place out of the DEEPSDR tree - no copy, no fork.\n");
    console_printf("  2154 lines of it, unmodified.\n\n");
    console_printf("  fft_compute_db_iq(), %u-point complex, Hann, output\n",
                   fft_tam_iq_actual());
    console_printf("  already in fftshift order with the VFO in the middle,\n");
    console_printf("  %u Hz a bin over %u Hz. It blanks the DC bin and its\n",
                   192000U / PAN_BINS, 192000U);
    console_printf("  neighbours, which is upstream's answer to the LO\n");
    console_printf("  spike this board measures above the carrier.\n\n");
    console_printf("  iqbal measures one block in eight and corrects all,\n");
    console_printf("  so its figures settle over seconds, not frames.\n");
    console_printf("  phase/gain are what it thinks the imbalance is;\n");
    console_printf("  mirror is what it reads off the spectrum at the\n");
    console_printf("  actual peak, which is the honest one.\n\n");
    console_printf("  Peak offset must match the %+d Hz asked for above.\n\n",
                   SPEC_OFFSET_HZ);

    /* ============================================================== */
    /* STEP 7: THE SCREEN                                             */
    /* ============================================================== */

    /*
     * THE WHOLE SCREEN, ONCE, and then only the parts that move.
     *
     * 480 x 320 at three bytes a pixel is 460 800 bytes over SPI, so a
     * full repaint costs about an eighth of a second and there is no
     * version of this that can afford one per frame. The division is
     * upstream's own and it is the reason ui_top.c and spec_chrome.c
     * are split into the pieces they are: the header, the chips, the
     * gutter and the ruler change when the tuning or the scale change,
     * and the trace, the waterfall and the S-meter change every
     * capture.
     */
    PHASE(20);

    /*
     * WHAT THE LAST SESSION LEFT, out of the FRAM - see settings_tick().
     * A stored value this image cannot use - a dial outside TUNE_MIN_HZ..
     * TUNE_MAX_HZ, a volume outside the knob's range, a step past the
     * table - is replaced by its default and said so; the record is not
     * thrown away for one bad field.
     */
    uint32_t boot_dial = GEN_HZ;

    /* A WFM session comes back with the tuner ON the dial (s_lo_off): the
     * offset has to be right before the first listening tune below, as
     * radio_mode_set() makes it right on every later change. */
    if (s_boot_have && s_boot_set.has_mode) {
        s_lo_off = lo_off_for((int)s_boot_set.mode);
        s_pan_lo = s_lo_off ? PAN_LO : PAN_LO_WFM;
    }

    /* The default step is 1 kHz by VALUE: the table is generated, so its
     * index is upstream's to move. */
    if (paso_tabla[s_step_idx].hz != 1000UL) { s_step_idx = step_index_of(1000U); }
    {
        const settings_t st   = s_boot_set;
        const int        have = s_boot_have;

        if (have) {
            if (st.dial_hz >= TUNE_MIN_HZ && st.dial_hz <= TUNE_MAX_HZ) {
                /*
                 * ONTO THE FINEST STEP'S GRID. Until 21/11/2026 the dial
                 * lost up to 1 Hz a retune (msi001_plan()'s f_actual,
                 * truncated), and the FRAM kept dials like 9 000 003 -
                 * which no step of 10 Hz or more can ever bring back to
                 * a round number. Every dial reached since the fix is on
                 * this grid already, so this only touches what the bug
                 * left behind.
                 */
                uint32_t g = (uint32_t)paso_tabla[0].hz;
                uint32_t d = ((st.dial_hz + g / 2U) / g) * g;

                if (d != st.dial_hz) {
                    console_printf("Settings  : stored dial %u Hz is off "
                                   "the %u Hz grid (the old 1 Hz-a-retune "
                                   "loss) - %u\n", st.dial_hz, g, d);
                }
                boot_dial = d;
            } else {
                console_printf("Settings  : stored dial %u Hz is outside "
                               "%u..%u - not used\n", st.dial_hz,
                               TUNE_MIN_HZ, TUNE_MAX_HZ);
            }
            if (st.vol_db >= VOL_MIN_DB && st.vol_db <= VOL_MAX_DB) {
                s_vol_db = st.vol_db;
            } else {
                console_printf("Settings  : stored volume %d dB is outside "
                               "%d..%d - not used\n", (int32_t)st.vol_db,
                               VOL_MIN_DB, VOL_MAX_DB);
            }
            if (!st.has_mode && st.step_idx < (sizeof k_old_steps
                                               / sizeof k_old_steps[0])) {
                /* A version 1-2 record: its index is into the old five. */
                s_step_idx = step_index_of(k_old_steps[st.step_idx]);
            } else if (st.has_mode && st.step_idx < TUNE_STEP_N) {
                s_step_idx = st.step_idx;
            } else {
                console_printf("Settings  : stored step %u is past the "
                               "table - not used\n", (uint32_t)st.step_idx);
            }
        }
        console_printf("Settings  : %s - dial %u Hz, volume %d dB, step "
                       "%u Hz\n", have ? "from the FRAM" : "nothing in "
                       "the FRAM yet, defaults", boot_dial, s_vol_db,
                       (uint32_t)paso_tabla[s_step_idx].hz);
        if (have) {
            console_printf("            (slot %d, seq %u, record version "
                           "%u, at 0x%04x)\n", settings_slot(),
                           settings_seq(), settings_version(),
                           settings_addr());
        }

        /* The band filters for THAT dial: everything above set them for
         * the generator, and dial_tune() only rewrites them when the
         * band changes, which from here it would not know. */
        msi001_bpf(msi001_bpf_for(boot_dial - s_lo_off));
        addon_set_filter(addon_filter_for(boot_dial));
    }

    /*
     * ONTO THE CARRIER, NOT 3 kHz BELOW IT.
     *
     * Every RF step above runs at TUNE_HZ = GEN_HZ - 3000, and that
     * offset is not arbitrary: 3000 Hz is exactly one FFT bin at this
     * rate and block size, so the generator's tone lands on a bin
     * centre and the single-bin measurements - tuning error, image
     * rejection, the DC figures - are clean rather than smeared across
     * two bins. It is a measurement setting.
     *
     * The listening path is different: the dial goes ON the carrier,
     * and the tuner goes AM_IF_HZ below the dial, so the carrier
     * arrives 12 kHz from the oscillator's leakage and am.c rotates it
     * back to zero. The DIAL is the number on the screen; the TUNER is
     * always 12 kHz under it from here on.
     */
    if (msi001_retune(boot_dial - s_lo_off) == MSI_OK) {
        const msi_plan_t *cp = msi001_last_plan();

        if (s_lo_off == 0U) {
            console_printf("Tuning    : dial %u Hz, tuner ON it - WFM: upstream's "
                           "discriminator\n            takes the station at "
                           "0 Hz (radio_mode_set()).\n\n", cp->f_actual);
        } else {
            console_printf("Tuning    : dial %u Hz, tuner %u Hz - %u Hz below, "
                           "so the\n", cp->f_actual + s_lo_off, cp->f_actual,
                           s_lo_off);
            console_printf("            leakage stays off the carrier. am.c "
                           "subtracts it and\n");
            console_printf("            rotates the station back to zero.\n\n");
        }
        ui_init_state(cp->f_actual + s_lo_off);
    } else {
        /* The tuner stayed where the spectrum test left it, so the dial
         * is 12 kHz above THAT - the same relation, on whatever it has. */
        ui_init_state(sp->f_actual + s_lo_off);
    }

    /* What the FRAM holds is now the starting point: nothing is saved
     * until something changes from it. */
    /* The settings screen's display half - theme, palette, style - before
     * the first paint, so a stored theme is what is drawn first. */
    if (s_boot_have && s_boot_set.has_aj) {
        menu_ajustes_apply_pantalla(&s_boot_set);
    }

    settings_now(&s_set_saved);
    s_set_seen = s_set_saved;

    {
        uint64_t p0 = board_cycles();
        ui_paint_all();
        console_printf("Screen    : 480x320 from upstream's own UI - "
                       "ui_top.c, spec_chrome.c, ui_act.c\n");
        console_printf("            geometry from compat/malahit_layout.h,"
                       " the same file the\n");
        console_printf("            host render uses, so the panel and "
                       "deepsdr-ui's PNG agree.\n");
        console_printf("            first full paint %u us\n",
                       (uint32_t)(((board_cycles() - p0) * 1000000U)
                                  / clock_sysclk_hz));
    }

    console_printf("\n  WHAT ON THAT SCREEN IS A REAL MEASUREMENT:\n");
    console_printf("    frequency, spectrum, waterfall, ruler, dB axis.\n");
    console_printf("  WHAT MOVES BUT IS NOT CALIBRATED:\n");
    console_printf("    the S bar - S1..S9 spans %d..%d in upstream's\n",
                   (int32_t)UI_DB_MIN, (int32_t)UI_DB_MAX);
    console_printf("    uncalibrated dB. No dBm figure is shown.\n");
    /*
     * THE DEMODULATOR GOES LIVE HERE, after every test that drives the
     * transport itself has finished with it. Registering it earlier
     * would have the hook writing audio into halves the tone tests are
     * also writing, which is two producers on one ring.
     */
    am_init(192000.0f);
    sai1_set_block_hook(am_block);
    am_enable(1);

    /*
     * AND UPSTREAM'S, READY BUT NOT HOOKED - step 8. demod_init() sets
     * up demod_am.c at this port's rates and touches no hook, so am.c is
     * still what plays; `swd-call.sh ... demod_path 1` switches over in
     * the running radio. See src/demod.h.
     */
    demod_init();
    grab_init();
    __asm__ volatile ("" :: "r"(ui_tap), "r"(ui_swipe), "r"(ui_hold));   /* swd-call's, see ui_tap() */
    __asm__ volatile ("" :: "r"(cw_report), "r"(digi_report));           /* and the decoders' bench */

    /*
     * UPSTREAM'S DEMODULATOR IS WHAT PLAYS, from step 9 on: the mode
     * screen needs it, and the owner heard every mode right on it
     * (PORTING.md step 26). am.c stays in the image for the retune-click
     * A/B - `swd-call ... demod_path 0` - until that is settled.
     */
    demod_path(1);

    /* The mode the last session left, from a version-3 record. WFM too
     * since 21/11/2026: the tuner already went on the dial for it, above
     * (s_lo_off, set from the record before the boot tune). */
    if (s_boot_have && s_boot_set.has_mode
        && s_boot_set.mode <= (uint8_t)RADIO_MODE_LAST) {
        (void)demod_mode(modo_demod((int)s_boot_set.mode));
    }
    /* CW's band-pass, from byte 31 of the record (tens of hertz, 0 = not
     * stored), and the decoder itself if the session was in CW. */
    if (s_boot_have && s_boot_set.cw_bw >= 10U && s_boot_set.cw_bw <= 100U) {
        demod_am_set_cw_bw_hz(10.0f * (float)s_boot_set.cw_bw);
    }
    /* RTTY's shift, speed and automatic, and CW's learning: bytes 32-35,
     * before the decoders come on below (nothing runs them yet). The
     * shift within config.h's own limits, as upstream's ex_rec(). */
    if (s_boot_have && s_boot_set.rtty_shift != 0U) {
        float sh = 5.0f * (float)s_boot_set.rtty_shift;

        if (sh >= CONFIG_RTTY_SHIFT_MIN_HZ && sh <= CONFIG_RTTY_SHIFT_MAX_HZ) {
            rtty_set_shift_hz(sh);
        }
    }
    if (s_boot_have && s_boot_set.rtty_baud >= 1U && s_boot_set.rtty_baud <= 4U) {
        digi_rtty_baud_pon((uint8_t)(s_boot_set.rtty_baud - 1U));
    }
    if (s_boot_have && s_boot_set.rtty_auto != 0U) {
        rtty_auto_set((uint8_t)(s_boot_set.rtty_auto == 2U));
    }
    if (s_boot_have && s_boot_set.cw_aprende != 0U) {
        cw_set_aprende((uint8_t)(s_boot_set.cw_aprende == 2U));
    }
    if (s_boot_have && s_boot_set.has_mode
        && s_boot_set.mode <= (uint8_t)RADIO_MODE_LAST) {
        texto_modo((int)s_boot_set.mode);   /* CW or RTTY: on, and the screen */
    }
    rds_modo();                         /* a WFM session comes back with RDS on */
    console_printf("Mode      : %s%s\n", radio_mode_name(radio_mode_get()),
                   (s_boot_have && s_boot_set.has_mode) ? ", from the FRAM"
                                                        : "");

    /* And its demodulator half, now that demod_am_init() - which resets
     * the notch and the NR - has run. */
    if (s_boot_have && s_boot_set.has_aj) {
        menu_ajustes_apply_radio(&s_boot_set);
    }
    console_printf("Ajustes   : %s - tema %s, AGC %s, BW %s, squelch %d dB, "
                   "NR %u, suavizado %d %%%s\n",
                   (s_boot_have && s_boot_set.has_aj) ? "from the FRAM"
                                                     : "defaults",
                   menu_tema_nombre(), radio_agc_text(), radio_bw_text(),
                   (int32_t)demod_am_get_squelch_db(), menu_nr_strength(),
                   radio_suav_pct(),
                   (s_boot_have && s_boot_set.has_suav) ? ""
                       : " (a record before version 4: suavizado is the default)");

    /* The memory channels: upstream's MEM.CSV, kept in the FRAM. */
    menu_mem_init();

    /* Everything the FRAM held is in force: THAT is the baseline, so the
     * boot does not save back what it has just read. */
    settings_now(&s_set_saved);
    s_set_seen = s_set_saved;
    s_band_name = (const char *)1;      /* force the first sync to draw */
    band_pill_sync();
    s_top.mode = mode_chip();
    top_sync_chips();
    ui_top_draw(&s_top);
    radio_act_refresh();

    /*
     * AND THE CODEC COMES OFF MUTE, for the first time in this image's
     * normal operation.
     *
     * nau8822_configure() leaves the outputs muted on purpose, so that a
     * bring-up image cannot come up shouting into a small speaker, and
     * until now nothing ever lifted it outside a listening test. There
     * is something to listen to now, which is what that default was
     * waiting for.
     *
     * -24 dB rather than the -18 the listening test used: there is a
     * volume control now, so the quiet end of its range is the right
     * place to start. The user's words were "very loud, and there is no
     * volume control" - the first half of that is fixed here and the
     * second half by the small knob.
     */
    nau8822_set_speaker_db((int8_t)s_vol_db);
    nau8822_set_headphone_db((int8_t)s_vol_db);
    nau8822_mute(0);

    am_report(am_filter_line);

    console_printf("Audio     : AM by envelope, in the block interrupt. "
                   "Speaker and\n");
    console_printf("            headphones unmuted at %d dB. Channel "
                   "filter 4 kHz,\n", s_vol_db);
    console_printf("            audio 200 Hz - 4 kHz, upstream's AGC and "
                   "filter design.\n");
    console_printf("            PLAYING NOW: upstream's demod_am.c (step 9);"
                   " am.c kept\n");
    console_printf("            for the A/B: swd-call demod_path 0|1,"
                   " demod_report.\n\n");

    console_printf("  WHAT IS A STUB, AND WHY:\n");
    console_printf("    the clock counts UPTIME (no RTC driver). Mode,\n");
    console_printf("    AGC, ATT and the six buttons are drawn at their\n");
    console_printf("    real values, which are 'nothing yet'.\n");
    console_printf("  THE BATTERY IS REAL: %u mV at the cell, %u %%,\n",
                   adc_bat_mv(), adc_bat_pct());
    console_printf("    measured on ADC1 channel 16 against a rail of\n");
    console_printf("    %u mV, which is itself measured - VREFINT on "
                   "ADC3,\n", adc_vdda_mv());
    console_printf("    %s. The percentage is a straight\n",
                   adc_vref_from_cal() ? "with this die's factory value"
                                       : "with the nominal 1216 mV");
    console_printf("    line between 3.3 and 4.2 V - see adc_bat_pct()\n");
    console_printf("    for why that is the weak part now.\n");
    console_printf("  WHAT THE KNOB DOES: turning tunes, a short press of\n");
    console_printf("    its button cycles the step. Both print here.\n\n");

    /*
     * HOW LONG THE BOOT TOOK, measured rather than estimated from a log:
     * SysTick starts in board_power_on(), a few milliseconds into main(),
     * and this is the last line before the radio is a radio. Measured
     * 20/11/2026: 2.5-2.6 s with SELFTEST 0, 36.6-36.9 s with SELFTEST 1.
     */
    console_printf("Boot      : screen and audio up %u ms after SysTick "
                   "started (SELFTEST %u)\n\n", board_millis(),
                   (uint32_t)SELFTEST);

    /*
     * THE CONSOLE GOES BUFFERED HERE, and not a line earlier: from now
     * on a byte is queued and UART7's interrupt sends it, instead of the
     * CPU waiting 87 us for each. The status line below is ~300 of them
     * a frame - ~26 ms of a ~113 ms frame, measured 20/11/2026.
     *
     * The boot stays polled on purpose: it is where runs hang, and a
     * hang keeps whatever is still queued. console.c has the rest.
     */
    if (ADDON_COLD_TEST) {
        test_addon_cold(ADDON_COLD_TEST);
    }

    console_async(1);

    /*
     * Two things the loop does early on that are MEASUREMENTS, and so
     * belong to SELFTEST: the iqbal A/B at 15 s (the corrector off, then
     * on - a stretch of worse image rejection, on purpose), and the
     * first eight frames on the polled transport, below.
     */
    uint32_t ab_at = board_millis() + 15000U;
    int      ab_done = !SELFTEST;
    uint32_t clock_at = 0U;
    uint32_t s_probe_at = 0U;
    uint32_t s_press_n = 0U, s_press2_n = 0U;
    int      s_err_absurd = 0;      /* the warning below fires once */
    uint32_t polled_left = SELFTEST ? 8U : 0U;  /* transport A/B, below */

    for (;;) {
        iq_peak_t pk;

        board_loop_alive();

        /*
         * THE KNOB, BEFORE THE CAPTURE.
         *
         * Once per frame rather than once per detent, which is what
         * encoder_take_delta() is built for: it accumulates quarter-
         * steps in the 1 kHz tick and hands over whole detents with the
         * remainder carried, so a frame that arrives 90 ms late gets the
         * sum of everything the hand did and loses nothing. Reading it
         * per detent would need an interrupt and buy nothing - the panel
         * cannot repaint faster than the frame anyway.
         *
         * It goes before the capture so that a block measured in this
         * frame belongs to the frequency the frame draws. The other
         * order retunes after capturing and shows one frame of the old
         * spectrum under the new number, which is the kind of thing that
         * is invisible until somebody turns a knob fast and sees the
         * trace lag the readout.
         */
        int32_t detents = encoder_take_delta();

        if (detents != 0 && menu_open_id() != MENU_NONE) {
            menu_knob(detents);             /* the menu's cursor, not the dial */
        } else if (detents != 0) {
            /*
             * THE TUNER FOLLOWS THE DIAL ON EVERY CLICK, a fixed
             * AM_IF_HZ below it - see dial_tune().
             *
             * NO MUTE, AND THAT IS A MEASUREMENT. The knob used to call
             * am_resettle(2): two blocks of silence and a fade, to stop
             * the retune's transient latching upstream's instant-attack
             * AGC. The retune test measured both, 19/11/2026, generator
             * on the dial:
             *
             *   +/-1 kHz, no mute    one chunk +1.3 dB, gain back in
             *                        65 ms - output down 1.2 dB, briefly
             *   +/-1 kHz, mute       85 ms of silence, THEN the audio
             *                        at +6.3 dB and ~400 ms to recover
             *
             * The mute reset the DC blocker along with everything else,
             * so the carrier came back as a step from zero - the mute
             * MADE the transient it existed to hide, and a bigger one
             * than the retune itself.
             */
            (void)dial_tune((int64_t)s_tuned_hz
                            + (int64_t)detents * (int64_t)TUNE_STEP_HZ);
        }

        retune_test_step();
        rds_marq_poll();                /* the header's RDS strip, in WFM */
        estereo_ui_poll();              /* and "WFM ST" in the mode chip */
        cw_poll();                      /* the text decoders' text, CW and RTTY */

        /*
         * THE PRESS, which is upstream's gesture and upstream's
         * classification: encoder.c times the hold and decides on the
         * RELEASE whether it was short or long, so neither the timing nor
         * the 600 ms boundary is re-invented here. All this board
         * contributed is where the press came from - a voltage on a
         * ladder instead of a level on a pin.
         *
         * A press consumed by turning is neither, and that is also
         * upstream's: encoder_consume_press() exists so that the
         * "hold and turn" gesture does not ALSO change the step on
         * release, which is the thing the gesture was adjusting.
         */
        if (menu_open_id() != MENU_NONE && encoder_take_press()) {
            menu_knob_press();              /* choose what the cursor is on */
        } else if (encoder_take_press()) {
            s_press_n++;
            s_step_idx = (uint8_t)((s_step_idx + 1U) % TUNE_STEP_N);
            tune_step_fmt();
            s_top.knob_value = s_step_txt;
            ui_top_draw_status(&s_top);
            radio_act_refresh();            /* the Paso button shows it too */
            console_printf("  step -> %s\n", s_step_txt);
        }

        /*
         * THE SECOND KNOB: gain reduction, and its button the preamp.
         * Same shape as the first - once per frame, whole detents, the
         * remainder carried - and the same reason: the panel cannot
         * repaint faster than a frame, so reading it per detent would
         * buy nothing and cost an interrupt.
         */
        int32_t g = encoder2_take_delta();

        if (g != 0 && !s_knob2_rf) {
            (void)radio_vol_set(s_vol_db + (int)g);     /* clamps, draws, logs */
            menu_refresh_det();                         /* if its page is up */
            g = 0;
        }

        if (g != 0) {
            /* Clockwise reduces the reduction, i.e. turns the gain UP,
             * which is the way a volume-shaped control has to feel. */
            (void)radio_att_set((int)s_rf_gr - (int)g * RF_KNOB_STEP);
            menu_refresh_det();
        }

        if (encoder2_take_press()) {
            s_press2_n++;
            s_knob2_rf = !s_knob2_rf;
            knob2_fmt();
            s_top.knob_value = s_knob2_txt;
            ui_top_draw_status(&s_top);
            console_printf("  small knob -> %s\n",
                           s_knob2_rf ? "RF gain" : "volume");
        }

        /* The preamp moved here when the button took on the knob's
         * mode. It is a rare setting - once per band, not once per
         * station - so a long press is the right home for it. */
        if (encoder2_take_long_press()) {
            s_rf_preamp = !s_rf_preamp;
            msi001_preamp(s_rf_preamp);
            console_printf("  preamp -> %s\n", s_rf_preamp ? "ON" : "OFF");
        }

        /*
         * The long press has no job yet. It is READ anyway, because the
         * counter it comes out of saturates nowhere and a gesture that
         * is never collected would hand the next short press a stale
         * answer; and it prints, because a long press that silently did
         * nothing is indistinguishable from a button that stopped
         * working.
         */
        if (encoder_take_long_press()) {
            console_printf("  long press (nothing bound to it yet)\n");
        }

        settings_tick();

        /*
         * ONE PASS PER RECEIVE BLOCK WHILE A MENU IS UP. The loop is
         * paced by its own drawing - the waterfall alone is 36 ms - and
         * under a menu it draws nothing, so it ran several times a block
         * and the console line it prints every pass filled the UART:
         * 140 000 stalls in a few seconds of one open menu, 21/11/2026.
         * Waiting for the next half keeps the pass rate, the console and
         * the touch polling exactly what they are with the spectrum up.
         */
        if (menu_open_id() != MENU_NONE) {
            static uint32_t s_menu_blk;
            uint32_t t0 = board_millis();

            while (sai1_blocks == s_menu_blk && (board_millis() - t0) < 60U) { }
            s_menu_blk = sai1_blocks;
        }

        /*
         * THE LIVE RING, NOT A FRESH POLL. capture_live() is a memcpy
         * out of the half the DMA has just finished; the old capture()
         * stopped the clocks and waited 42.7 ms for a new block, which
         * was 30 % of the frame time and a gap in the receive stream
         * besides. See capture_live() for which question each answers.
         *
         * THE FIRST FEW FRAMES RUN POLLED ON PURPOSE, so that the frame
         * period of the two transports is measured by the same code, on
         * the same signal, in the same run - rather than by comparing
         * this log with yesterday's. It costs eight frames and it is the
         * difference between a measurement and a recollection. The mode
         * switches it costs are counted and reported below; here they
         * are deliberate.
         */
        int used_dma = 1;

        if (polled_left != 0U) {
            polled_left--;
            used_dma = 0;
            if (capture() == 0U) {
                console_printf("  capture failed\n");
                continue;
            }
        } else {
            if (!sai1_dma_running()) {
                int arc = sai1_dma_start(CAP_FRAMES, AUDIO_HALF_FRAMES);

                console_printf("\n  -- switching to the DMA transport, "
                               "mux 0x%04x --\n", sai1_mux_released());
                dma_arm_report("display loop", arc);
                console_printf("\n");
            }
            if (capture_live() == 0U) {
                console_printf("  capture failed\n");
                continue;
            }
        }

        uint64_t t0 = board_cycles();

        /*
         * CORRECT BEFORE ANYTHING LOOKS AT THE BLOCK. In place, on the
         * interleaved buffer, exactly as upstream does it - so the
         * spectrum below and anything grafted later see one corrected
         * copy and nobody corrects twice.
         */
        iqbal_bloque(s_rx, PAN_BINS);

        deinterleave(PAN_BINS);
        fft_compute_db_iq(s_i, s_q, s_db);
        uint32_t dsp_us = (uint32_t)(((board_cycles() - t0) * 1000000U)
                                     / clock_sysclk_hz);

        /* Let it read its own mirror off the finished spectrum. */
        iqbal_mira_espectro(s_db, (uint16_t)PAN_BINS);

        iq_peak(s_db, PAN_BINS, 192000U, &pk);

        /*
         * THE TRACE, now inside the layout instead of over the whole
         * panel. SPC_TRACE_* comes from compat/malahit_layout.h, so
         * these four numbers are the same four the gutter and the
         * ruler were drawn with and the three cannot drift apart.
         *
         * It used to be (0, 40, 480, 240), full width and no chrome.
         * The height mattered then for a reason worth keeping written
         * down: spectrum_draw() opens with `h > SPEC_MAX_H` and simply
         * RETURNS, and SPEC_MAX_H is 280, so asking for all 320 rows
         * drew nothing and complained about nothing. What caught it
         * was timing the call - a draw that reports 0 us has not
         * drawn. The trace is 120 rows now and nowhere near that
         * limit, and draw_us is still printed for the same reason.
         *
         * ONLY THE WINDOW CENTRED ON THE DIAL is drawn: bins
         * [PAN_LO, PAN_BINS), 168 kHz. The whole transform is centred
         * on the TUNER, 12 kHz below, and drawing it would put the
         * station right of the centre mark and the ruler's labels on
         * the wrong frequencies. The peak search above still sees all
         * 512 bins, because it reports offsets from the tuner.
         *
         * The centre-mark offset is therefore 0 - the mark is the dial.
         * Upstream passes SPC_TRACE_W/4 there because the DEEPSDR draws
         * its whole baseband and demodulates at +Fs/4. The three
         * trailing arguments are the band edges, which belong to a
         * receiver with an audio filter and are still not meaningful.
         */
        uint64_t d0 = board_cycles();
        /* Under an open menu the trace is not drawn - the screen is
         * the menu's (src/menus.h) - but the waterfall keeps its rows,
         * so it is current the moment the menu closes. */
        traza_suaviza(&s_db[s_pan_lo]);
        if (menu_open_id() == MENU_NONE && !digi_en_pantalla()) {
            spectrum_draw(s_db_traza, PAN_SHOWN,
                          SPC_TRACE_X, SPC_TRACE_Y, SPC_TRACE_W, SPC_TRACE_H,
                          UI_DB_MIN, UI_DB_MAX, 0, 0U, 0, 0);
        } else if (menu_open_id() == MENU_NONE) {
            digi_dibuja();              /* the scope and the text, step 42 */
        }
        uint32_t draw_us = (uint32_t)(((board_cycles() - d0) * 1000000U)
                                      / clock_sysclk_hz);

        /*
         * THE WATERFALL. One new row per capture, then the whole
         * history out through the colormap.
         *
         * The blit is the expensive half of the frame - 92 x 440
         * pixels is 121 kB over SPI - and it is done whole rather than
         * scrolled because this panel has no hardware scroll that gfx2
         * can reach and the ring already avoids the memory copy.
         *
         * ON DMA SINCE 21/11/2026, and that changes what `wf` and
         * `trace` in the status line mean. They are still timed from
         * the first pixel to the return, but the return now comes when
         * the last block has been QUEUED, not sent: the display loop
         * waits only for a free slot, so the figure is the wire time of
         * all but the last eight blocks plus the CPU's, and the queue's
         * last ~12 ms drain under whatever comes next. `frame` is the
         * number that cannot flatter. `lcd ... qN` is the deepest the
         * queue got in the frame - 8 means the loop ran ahead of the
         * wire and had to wait, which is the healthy case.
         */
        uint64_t w0 = board_cycles();
        ui_wf_push(&s_db[s_pan_lo], PAN_SHOWN);
        if (menu_open_id() == MENU_NONE && !digi_en_pantalla()) {
            gfx2_wf_blit(SPC_TRACE_X, SPC_WF_Y, SPC_TRACE_W, SPC_WF_ROWS,
                         s_wf, SPC_TRACE_W, s_wf_head,
                         spectrum_colormap_lut());
        }
        uint32_t wf_us = (uint32_t)(((board_cycles() - w0) * 1000000U)
                                    / clock_sysclk_hz);

        /*
         * THE S-METER, and only the S-meter. ui_top_draw_smeter() is
         * upstream's own narrow repaint of the part of the status band
         * that moves with the signal; the whole band is ten times the
         * bus traffic for the same information.
         */
        /*
         * WHERE THE CARRIER IS, once a second, measured on the same
         * block the spectrum was computed from. Three numbers - the
         * leakage at DC, the station where am.c's rotation expects it,
         * and that place's mirror image. With a carrier on the dial a
         * working radio has `station` the largest; `mirror` largest
         * means the rotation's sign is wrong and the audio is the image.
         */
        if (s_probe_at == 0U || (int32_t)(board_millis() - s_probe_at) >= 0) {
            float dc, station, mirror;

            s_probe_at = board_millis() + 1000U;

            /*
             * THE BUTTONS STOPPED WORKING and this is where to look
             * first: the ladder's counts, what the classifier makes of
             * them, what upstream's debounce says is down, and how many
             * presses have come out the far end. Four places a press
             * can be lost, and the first of them that is wrong is the
             * one to fix.
             */
            console_printf("  buttons: ENC_BUT %u -> class %u   down "
                           "%u/%u   presses %u/%u   power %u\n",
                           adc_raw_enc_but(), adc_buttons(),
                           encoder_button_down(), encoder2_button_down(),
                           s_press_n, s_press2_n,
                           (uint32_t)boot_power_key());
            /*
             * THE RETUNE REPAIR, when there has been a retune since the
             * last line: how many windows were filled by prediction and
             * how many fell back to the hold, and the last period found
             * (am.h, "PREDICTED, NOT FADED").
             */
            {
                static uint32_t was = UINT32_MAX;
                uint32_t pn, hn, pp;
                float    pr;

                am_pred_stats(&pn, &hn, &pp, &pr);
                if (pn + hn != was) {
                    was = pn + hn;
                    console_printf("  retune repair: predicted %u, held %u; "
                                   "last period %u samples (%u Hz), r %d/1000"
                                   "\n", pn, hn, pp,
                                   (pp != 0U) ? 192000U / pp : 0U,
                                   (int32_t)(pr * 1000.0f));
                }
            }
            /*
             * THE PANEL'S DMA, once a second: blocks sent since boot, how
             * many of them ended with FEIF up (spi3_dma_counts() says why
             * that is a question and not a fault), and the two things
             * that ARE faults and switch the DMA off.
             */
            {
                uint32_t n, fe;

                spi3_dma_counts(&n, &fe);
                console_printf("  lcd: %s, %u transfers, FEIF on %u, "
                               "failed %u, stalls %u\n",
                               ili9488_dma_on() ? "dma" : "polled",
                               n, fe, spi3_dma_failures(),
                               ili9488_q_stalls());
            }
            /*
             * THE LINE IS LOW AND THESE THREE SAY WHY. A high-impedance
             * source read with too short a sample time reads low, so
             * SMPR is the first suspect; the analog switch is the
             * second, because a closed one hangs the digital pad on the
             * ladder.
             *
             * THE TWO POLLED CHANNEL READS THAT USED TO BE HERE ARE
             * GONE. They answered which pad carries ENC_BUT, and then
             * went on stealing conversions from the sampler printed
             * beside them, so the two disagreed threefold about the
             * same channel and the disagreement looked like the bug.
             * Reading a register costs the measured thing nothing;
             * starting a conversion inside somebody else's does. Trap
             * 21 again, in a function written to run alongside the
             * sampler.
             */
            console_printf("           SMPR1 0x%08x  PMCR 0x%08x\n",
                           ADC1->SMPR1, SYSCFG->PMCR);

            /*
             * A VERDICT ONLY WHEN THERE IS SOMETHING TO JUDGE. On noise
             * the two correlations swap places from line to line, so
             * "which is bigger" means nothing until one side clears a
             * floor by a margin. PROBE_FLOOR is over three times the
             * worst the empty mirror side read; the bench carrier read
             * about 715.
             *
             * NOT COMPARED WITH THE LEAKAGE. The first version flagged
             * the mirror only if it also beat DC, and at the bench's
             * working level the leakage (~1700) is bigger than the
             * carrier - so the warning could never have fired.
             */
            am_probe(s_rx, CAP_FRAMES, &dc, &station, &mirror);
            console_printf("  baseband: leak DC %u   station %u   "
                           "mirror %u%s\n",
                           (uint32_t)dc, (uint32_t)station, (uint32_t)mirror,
                           (station > PROBE_FLOOR && station > 4.0f * mirror)
                               ? "   <- carrier on the dial"
                           : (mirror > PROBE_FLOOR && mirror > 4.0f * station)
                               ? "   *** MIRROR LOUDEST: rotation sign? ***"
                               : "");
        }

        ui_s_units(pk.db, &s_top.s_units, &s_top.over_db);
        s_top.dbm = (int16_t)pk.db;     /* with dbm_valid 0: not shown */
        ui_top_draw_smeter(&s_top);

        /*
         * The clock corner, once a second, which is as often as a
         * display of whole seconds can change - and the battery pill
         * with it, because ui_top_draw_hora() draws both. That is why
         * the battery needs no drawing path of its own and no second
         * window: it shares the one the clock already pays for.
         */
        if ((int32_t)(board_millis() - clock_at) >= 0) {
            clock_at = board_millis() + 1000U;
            ui_fmt_uptime(board_millis(), s_clock_txt);
            s_top.batt_pct = adc_bat_pct();
            ui_top_draw_hora(&s_top);
        }

        ui_touch();

        /*
         * THE A/B TEST, which is upstream's and is the only honest way
         * to report what the corrector is worth: it measures the mirror
         * with the corrector off, then with it on, and hands back both.
         * A single "rejection" figure from a corrector grading its own
         * homework is worth very little.
         *
         * iqbal_ab_paso() has to be called or the run never advances -
         * a bug upstream found and wrote down, see iqbal.h.
         */
        if (!ab_done && (int32_t)(board_millis() - ab_at) >= 0) {
            iqbal_ab_lanza();
            ab_done = 1;
            console_printf("\n  A/B launched: mirror with the corrector"
                           " off, then on.\n\n");
        }
        if (iqbal_ab_corriendo()) { iqbal_ab_paso(); }

        /*
         * ADC USAGE AND THE NOISE FLOOR, BEFORE THE DSP FIGURES.
         *
         * They go first among the diagnostics because they are the two
         * that invalidate everything after them. A line reporting a
         * 36 dB mirror out of a capture marked CLIP is not reporting a
         * 36 dB mirror; it is reporting what the intermodulation
         * products happened to do that frame.
         */
        float floor_db = spec_floor_db(s_db, PAN_BINS);

        /*
         * WHAT IS THE JUNK LEFT OF THE CARRIER? Three candidates sit
         * there and they are different problems, so the line names
         * them instead of leaving it to the eye:
         *
         *   dc    the middle of the screen, which is the LO leakage
         *         and the mixer's residual offset. Note that fft.c
         *         BLANKS the DC bin and its four neighbours and fills
         *         them with the average of the bins four places either
         *         side - so this reads what the display shows, which on
         *         a raised floor is a plausible value and not a small
         *         one. It is the right number for "what is on screen"
         *         and the wrong one for "how big is the leakage".
         *
         *   img   the mirror of the peak about the centre. On a
         *         quadrature receiver an image is the SAME distance
         *         the other side, so for a carrier right of centre the
         *         image is left of it - which is exactly where junk
         *         gets noticed and blamed on something else.
         *
         *   floor the median, i.e. our own noise.
         *
         * An image is a fixed number of decibels below the carrier and
         * moves with it; the leakage sits at the centre whatever the
         * carrier does; the floor is everywhere. One frame of this
         * tells the three apart.
         */
        const uint32_t mid = PAN_BINS / 2U;
        uint32_t       mir = (pk.index > mid) ? (mid - (pk.index - mid))
                                              : (mid + (mid - pk.index));
        float dc_db = s_db[mid];

        for (uint32_t k = mid - 2U; k <= mid + 2U; k++) {
            if (s_db[k] > dc_db) { dc_db = s_db[k]; }
        }

        console_printf("  adc %3u%%%s | floor %3d dc %3d img %3d dB | "
                       "snr %3d dB | ",
                       s_cap_used_pct,
                       s_cap_clipped ? " *CLIP*" : "       ",
                       (int32_t)floor_db,
                       (int32_t)dc_db,
                       (int32_t)(mir < PAN_BINS ? s_db[mir] : 0.0f),
                       (int32_t)(pk.db - floor_db));

        /*
         * THE TRANSPORT'S OWN LINE, and it is here rather than in a
         * summary because the thing being watched is whether the PANEL
         * starves the AUDIO. That can only be seen while both are
         * happening: 57 ms of SPI a frame against a transmit FIFO 21 us
         * deep. `dry` is the count of frames in which the transmitter
         * ran out of data, and the whole claim of this step is that it
         * stays at zero while trace and wf are what they are.
         */
        /*
         * THE FRAME PERIOD, MEASURED, not added up from the parts. The
         * parts are each timed separately and their sum leaves out
         * everything between them - the console write above all, which
         * on this line was ~300 characters at 115200 baud, ~26 ms, until
         * the console went buffered (console_async). A frame rate quoted
         * from the sum of the timed pieces would be wrong by whatever
         * that leaves out, in the flattering direction.
         *
         * `con` is console_stalls(): the writes that found the console's
         * ring full and had to wait for the line. While it stays at 0
         * the console costs the frame only the formatting.
         */
        static uint64_t last_frame;
        uint64_t        now_c  = board_cycles();
        uint32_t        per_us = (last_frame != 0U)
                               ? (uint32_t)(((now_c - last_frame) * 1000000U)
                                            / clock_sysclk_hz)
                               : 0U;
        last_frame = now_c;

        /*
         * A COUNTER THAT CANNOT HAVE COUNTED THAT HIGH - 05/11/2026.
         *
         * Seen once, in one build, and never before or since: the DMA
         * error counter jumped mid-run from 3 to 4 245 803 899 and then
         * went on incrementing normally. It only ever does ++, so no
         * sequence of events reaches that value; a 32-bit word was
         * written into it from outside.
         *
         * What wrote it is NOT KNOWN. .bss is zeroed correctly by the
         * startup code and collected correctly by the linker script;
         * the variable's neighbours are scalars rather than arrays, so
         * there is no array to have run past; and the builds either
         * side are clean over a thousand status lines each.
         *
         * 20/11/2026: ONE THING THAT DOES EXACTLY THIS WAS FOUND, at
         * load time: the previous image's SAI DMA, still running while
         * a new one is loaded over SWD, wrote I/Q pairs into sai1.c's
         * s_mux_released. core_clean in openocd-helpers.cfg now stops
         * it. Whether it explains THIS event is not shown - this value
         * also reads as a sample pair, but it appeared mid-run.
         *
         * So this is not a fix, it is a detector. The number is never
         * clamped - clamping would hide exactly the thing worth seeing -
         * and the first implausible reading says so loudly, once. A wild
         * write that landed on a diagnostic counter this time could land
         * on something that matters next time, and the difference
         * between noticing and not is this line.
         */
        if (sai1_dma_errors() > 1000000U && !s_err_absurd) {
            s_err_absurd = 1;
            console_printf("\n  *** THE DMA ERROR COUNT IS %u, WHICH IT "
                           "CANNOT HAVE REACHED BY\n", sai1_dma_errors());
            console_printf("      COUNTING. Something wrote that word. "
                           "See WORKING.md. ***\n\n");
        }

        console_printf("%s cap %5u us dry %u err %u irq %u/%u am %u/%u "
                       "hook %uk/%uk stale %u con %u | frame %5u us "
                       "(%u fps) | ",
                       used_dma ? "dma" : "pol",
                       s_cap_us, sai1_tx_underruns(), sai1_dma_errors(),
                       sai1_blocks, sai1_block_overruns,
                       am_blocks(), (uint32_t)am_level(),
                       am_cycles() / 1000U, am_cycles_worst() / 1000U,
                       board_loop_stale_ms(), console_stalls(),
                       per_us, (per_us != 0U) ? (1000000U / per_us) : 0U);

        console_printf("peak %+7d Hz %3d dB | phase %+3d.%02u deg "
                       "gain %+2d.%02u dB | mirror %3d dB @%+4d bin | "
                       "blk %u | dsp %u us trace %u us wf %u us | "
                       "lcd %s q%u %u/%u/%x\n",
                       pk.offset_hz, (int32_t)pk.db,
                       (int32_t)iqbal_fase_grados(),
                       (uint32_t)((int32_t)(iqbal_fase_grados() * 100.0f)
                                  % 100 < 0
                                  ? -((int32_t)(iqbal_fase_grados()
                                                * 100.0f) % 100)
                                  : ((int32_t)(iqbal_fase_grados()
                                               * 100.0f) % 100)),
                       (int32_t)iqbal_gan_db(),
                       (uint32_t)(((int32_t)(iqbal_gan_db() * 100.0f) % 100)
                                  < 0
                                  ? -((int32_t)(iqbal_gan_db() * 100.0f) % 100)
                                  : ((int32_t)(iqbal_gan_db() * 100.0f) % 100)),
                       (int32_t)iqbal_espejo_db(),
                       (int32_t)iqbal_espejo_bin(),
                       iqbal_bloques(),
                       dsp_us, draw_us, wf_us,
                       ili9488_dma_on() ? "dma" : "pol",
                       ili9488_q_depth_max(), spi3_dma_failures(),
                       ili9488_q_stalls(), spi3_dma_flags_seen());

        /*
         * ONCE PER RESULT, NOT ONCE PER FRAME. iqbal_ab_hay() is sticky -
         * it says a result EXISTS - so testing it alone reprinted the
         * same verdict under every frame line, 359 times in a two-minute
         * capture, burying everything else. Print when the pair changes.
         */
        if (iqbal_ab_hay()) {
            static int32_t s_ab_said_off = INT32_MIN, s_ab_said_on;
            int32_t off = (int32_t)iqbal_ab_off();
            int32_t on  = (int32_t)iqbal_ab_on();

            if (off != s_ab_said_off || on != s_ab_said_on) {
                s_ab_said_off = off;
                s_ab_said_on  = on;
                console_printf("\n  *** A/B RESULT: mirror %d dB with the"
                               " corrector OFF, %d dB with it ON ***\n\n",
                               off, on);
            }
        }
    }
}
