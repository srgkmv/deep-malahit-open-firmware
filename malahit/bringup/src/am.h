#ifndef MALAHIT_AM_H
#define MALAHIT_AM_H

#include <stdint.h>

/*
 * AM by envelope, from the I/Q block to the speaker.
 *
 * Registered as the SAI's receive block hook, so it runs in the DMA
 * interrupt at block rate - 23.4 times a second here - rather than in
 * the display loop, which runs at eleven frames a second and cannot
 * feed an audio stream at all.
 *
 * `am.c` explains why this is not upstream's `demod_am.c` and which
 * parts of it still are upstream's. The short version: that file is
 * 6 987 lines and pulls in the whole decoder suite, while the two
 * pieces of it with any judgement in them - the AGC and the audio
 * filter's design - are compiled in place here already.
 */

/*
 * THE IF OFFSET. The tuner is parked AM_IF_HZ BELOW the dial, so the
 * station arrives 12 kHz from the oscillator's leakage instead of on
 * top of it, and am.c rotates it back to zero. Shared here because
 * three things must agree on it - where main.c tunes the MSi001, where
 * the panadapter puts its centre, and what am.c rotates by - and a
 * number written three times is three numbers.
 *
 * Fs/16 because at an exact submultiple of the rate the rotation is a
 * 16-entry table that repeats exactly: no phase accumulator, no drift,
 * no spurs from one. am.c explains why there has to be an offset at
 * all; the short version is that at zero IF the AM carrier IS the DC
 * term, and the leakage cannot be removed without removing it.
 */
#define AM_FS_HZ        192000U
#define AM_IF_HZ        12000U
#define AM_IF_STEPS     (AM_FS_HZ / AM_IF_HZ)       /* 16 */

void     am_init(float fs_hz);

/* There was an am_resettle() here: a two-block mute and a fade after
 * every retune. Measured, it made the transient worse than no mute at
 * all, and by ear the radio is better without it (19/11/2026, WORKING.md
 * "The retune transient, measured"). */

/*
 * THE RETUNE CLICK, AND WHAT IS DONE ABOUT IT.
 *
 * Writing the MSi001's synthesiser makes it relock, and for 40-60
 * samples starting 13-50 samples after the write (0.2-0.3 ms) the
 * station's I/Q collapses into noise and comes back with another phase
 * - measured at sample level by the retune test's raw capture. Through
 * the channel filter that is the envelope falling to nearly nothing,
 * through the DC blocker and the band-pass it is a spike as big as the
 * programme, and upstream's instant-attack AGC then latches the spike
 * and ducks the audio 2-4 dB for 150 ms. That is the click.
 *
 * Call am_retune_note() IMMEDIATELY before writing the synthesiser. It
 * pins the write to a sample (which receive half, how far in), and the
 * hook then holds the envelope at the CARRIER's level around it: the
 * modulation fades out over 1 ms before the write, stays out for
 * 1.7 ms, and fades back over 1 ms - while the carrier level, frozen
 * from the DC blocker's own running mean, carries straight through. No
 * spike is ever made, so the DC blocker is not disturbed and the AGC
 * has nothing to latch.
 *
 * The fade starts BEFORE the write, which a hook that processes whole
 * blocks after the fact can do - provided the write is at least 192
 * samples into its half, or the start of the fade is in a block that
 * has already been played. dial_tune() waits for that (am_retune_ok).
 *
 * Designed on the four raw captures with tools/clickmodel.py: the
 * click's energy above 2 kHz went from -5..-17 dB re the tone to -20 dB
 * on all four, where the rest is the fade itself on a pure 1 kHz tone.
 */
#define AM_HOLD_RAMP    192         /* samples down, and again up: 1 ms  */
#define AM_HOLD_FROM    0           /* the modulation is out from here   */
#define AM_HOLD_TO      320         /* ... to here, samples from the write */

void     am_retune_note(void);
/* True when the DMA is far enough into its half for the fade to start
 * in the same block as the write. */
int      am_retune_ok(void);

void     am_enable(int on);
int      am_enabled(void);

/* The block hook itself: sai1_set_block_hook(am_block). */
void     am_block(const int16_t *rx, int16_t *tx, uint32_t frames);

/* Diagnostics. The level is the AGC's own peak follower, which is the
 * number that says whether anything is arriving at all. */
/* Prints what the designed filters actually do, through a callback so
 * this file needs no console. Call it from main(), never from the hook. */
void     am_report(void (*out)(const char *name, float hz, float db));

/*
 * Where the energy sits in the raw baseband: at DC (the leakage), where
 * the rotation expects the station, and at its mirror image. Measured
 * rather than deduced - with a carrier on the dial, `station` must
 * clearly beat `mirror`, and if `mirror` wins, the rotation's sign is
 * wrong and the audio is the image's. Not compared with `dc`: the
 * leakage is normally bigger than the carrier.
 */
void     am_probe(const int16_t *rx, uint32_t frames,
                  float *dc, float *station, float *mirror);

/* What the hook cost, in CPU cycles: the last block and the worst seen.
 * The budget is one block - 20.5 million cycles at 480 MHz. */
uint32_t am_cycles(void);
uint32_t am_cycles_worst(void);

uint32_t am_blocks(void);
float    am_level(void);

/*
 * THE RETUNE TEST SWITCH, here and not in main.c, because the three
 * traces below live in am.c and are only ever read by that test.
 *
 *   0  off - the default. The test does not run and the three buffers
 *      shrink to one entry each: about 35 kB of RAM handed back, on a
 *      part where RAM is the scarce thing.
 *   1  the coarse table (WORKING.md, "The retune transient, measured")
 *   2  plus the fine and raw dumps around each +/-1 kHz move, for
 *      tools/clickdump.py and tools/clickmodel.py
 *
 * Run it (1 at least) before and after any change to am.c's chain, the
 * AGC or the click repair. It takes the dial for a minute after boot.
 * The CLICK REPAIR does not depend on this switch.
 */
#ifndef RETUNE_TEST
#define RETUNE_TEST     0
#endif

/*
 * THE RETUNE TRACE. What a retune actually does to the chain, chunk by
 * chunk (AM_CHUNK = 512 samples = 2.67 ms), for about a second - because
 * every word written so far about "the transient" was reasoning, and
 * none of it was a measurement.
 *
 * Recorded in the block interrupt into RAM; nothing prints from there.
 * main() arms it, retunes, marks the chunk where the retune happened,
 * and prints the trace once am_trace_done() says it is full.
 *
 *   car   mean |z| after the channel filter - the CARRIER's level, the
 *         one quantity in AM that does not move with the modulation
 *   aud   peak |audio| into the AGC - what the AGC actually follows
 *   agc   the AGC's peak follower after the chunk; gain = 18000 / agc
 *   out   peak |output| to the codec - what the listener gets
 */
/* 2 s, not 1: a latched peak 20 dB high takes 0.5 s x ln(10) = 1.15 s
 * to come back at this release, and a trace that ends before the
 * recovery cannot say how long the recovery was. 12 kB of RAM. */
#define AM_TRACE_N      (RETUNE_TEST ? 768U : 1U)   /* 768 x 2.67 ms = 2.05 s */

typedef struct {
    float car;
    float aud;
    float agc;
    float out;
} am_trace_t;

void              am_trace_arm(void);
void              am_trace_mark(void);
int               am_trace_done(void);
const am_trace_t *am_trace_get(uint32_t *mark);

/*
 * THE FINE TRACE - what a click is made of. The coarse trace above is
 * one point per 2.67 ms, and a click is shorter than that; this is one
 * point per AM_FINE_SUB = 32 samples (167 us), over the block before
 * the retune and the two after it.
 *
 *   lr, li   mean of the RAW I/Q over 32 samples. 32 samples is exactly
 *            two periods of 12 kHz, so the station cancels and what is
 *            left is the 0 Hz term: the oscillator's leakage plus the
 *            tuner's DC offset - which the MSi001 re-calibrates every
 *            time its synthesiser is written.
 *   cr, ci   mean of the same samples after the leakage subtraction and
 *            the rotation, before the channel filter: the STATION's
 *            complex envelope - its amplitude and its phase, so a PLL
 *            that relocks with a phase jump shows as one.
 *   a0, a1   the output to the codec, every 16th sample (12 kHz - the
 *            audio is band-limited to 4 kHz, so this is the waveform).
 *
 * The retune is pinned to its sample by am_retune_note(), the same call
 * the click repair uses - so the trace and the repair cannot disagree
 * about when the synthesiser was written.
 */
#define AM_FINE_SUB     32U
#define AM_FINE_N       ((RETUNE_TEST >= 2) ? 768U : 1U)  /* 3 blocks of 8 192 */

typedef struct {
    float   lr, li;
    float   cr, ci;
    int16_t a0, a1;
} am_fine_t;

void             am_fine_arm(void);
int              am_fine_done(void);
/* The ring, the entry the dump starts at (oldest), and the entry the
 * retune happened in; all indices are mod AM_FINE_N. */
const am_fine_t *am_fine_get(uint32_t *first, uint32_t *at);

/*
 * AND THE RAW SAMPLES THEMSELVES, because the fine trace showed the
 * click comes from a glitch of one or two of its entries - 0.2-0.3 ms
 * of the carrier collapsing and coming back with another phase when the
 * synthesiser is written - and a 32-sample mean cannot say exactly when
 * it starts, how long it lasts, or what it looks like inside. A repair
 * has to be designed on the real shape.
 *
 * AM_RAW_N interleaved frames (slot 0 = Q, slot 1 = I, as the codec
 * delivers them) from AM_RAW_PRE frames before the synthesiser write,
 * out of the block the write landed in. Clipped at the block's edges;
 * `n` says how many frames were captured and `rel0` where the first
 * one sits relative to the write.
 */
#define AM_RAW_N        ((RETUNE_TEST >= 2) ? 2048U : 1U)
#define AM_RAW_PRE      256U

const int16_t   *am_raw_get(uint32_t *n, int32_t *rel0);

#endif /* MALAHIT_AM_H */
