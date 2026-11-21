#include "am.h"
#include "stm32h7xx.h"  /* DWT->CYCCNT, for the hook's own budget */
#include "sai1.h"
#include "board.h"      /* board_cycles(), for the hook's own budget */
#include "arm_math.h"
#include <math.h>       /* expf(), once, at init */
#include "agc.h"        /* ../../User/agc.c      - upstream */
#include "audiofil.h"   /* ../../User/audiofil.c - upstream */

/*
 * AM BY ENVELOPE, FROM THE I/Q BLOCK TO THE SPEAKER.
 *
 * WHY THIS IS NOT UPSTREAM'S demod_am.c, WHICH IS THE OBVIOUS QUESTION.
 *
 * This port's rule is that upstream code is compiled in place rather
 * than copied, and it has been followed everywhere it could be - the
 * encoder, the FFT, the I/Q corrector, the whole UI. It stops here, and
 * the number says why: `User/demod_am.c` is 6 987 lines and includes
 * thirty other modules - RDS, SAM, spectral noise reduction, HFDL,
 * RTTY, CW, NAVTEX, PSK31, WEFAX, SSTV, Feld-Hell, FT8, WSPR, AIS, ALE,
 * JTTY, STANAG, APRS, DSC. It is not a demodulator, it is the entire
 * decoder suite with an AM path running through it. Grafting it means
 * grafting most of the firmware, and this board does not yet have the
 * main loop, the settings store or the mode machine that the suite is
 * wired into.
 *
 * WHAT IS STILL UPSTREAM'S IS THE PART WITH JUDGEMENT IN IT. The two
 * pieces here that someone tuned by ear rather than derived - the
 * automatic gain control and the audio filter's coefficients - are
 * `agc.c` and `audiofil.c`, compiled in place, already in this build.
 * What is written here is the arithmetic between them: a channel
 * filter, a magnitude, a DC blocker. Those have one correct answer
 * each.
 *
 * THE CHAIN, which is upstream's own, described in demod_am.h and
 * followed here step for step:
 *
 *   1. channel low-pass, the SAME filter on I and on Q, which is a
 *      complex low-pass centred on the tuned frequency. It has to come
 *      BEFORE the envelope detector: |z| is not linear and would
 *      intermodulate everything it could see - the whole +/-96 kHz
 *      window rather than the station in the middle of it.
 *   2. |I + jQ| by arm_cmplx_mag_f32, which uses the Cortex-M7's
 *      VSQRT.F32. The envelope IS the signal in AM, and it has the
 *      classic property of not caring about a small tuning error: an
 *      offset carrier rotates the phasor and |z| does not move. There
 *      is no need to zero-beat.
 *   3. DC blocker, one pole at about 15 Hz, to take the carrier's DC
 *      term out before the AGC and the converter see it.
 *   4. audio band-pass, upstream's design.
 *   5. upstream's AGC, which also writes the stereo int16 out.
 *
 * IT RUNS IN THE BLOCK INTERRUPT, below SysTick. Nothing here prints,
 * waits, or allocates.
 */

/*
 * THE CHUNK, AND WHY IT IS NOT THE WHOLE BLOCK.
 *
 * A receive half is 8 192 frames. Carrying that through the chain in
 * one go would need an I and a Q buffer, an interleaved complex buffer
 * and an envelope buffer at 8 192 floats each - about 160 kB, on a part
 * that is already at 89 % of its RAM. Upstream works in 512-frame
 * chunks for the same reason and says so.
 *
 * So the block is walked in chunks. The filters keep their state across
 * chunks, which is what makes this identical to processing the whole
 * block at once - a biquad does not care where a buffer boundary falls,
 * only that its history is continuous.
 */
#define AM_CHUNK        512U

static float s_i[AM_CHUNK];
static float s_q[AM_CHUNK];
static float s_cplx[AM_CHUNK * 2U];
static float s_env[AM_CHUNK];

/*
 * Three cascades: one for I, one for Q, one for the audio. The two
 * channel filters share coefficients and MUST NOT share state - they
 * are filtering different signals, and one state for both would mix
 * them into each other, which is the kind of fault that sounds like a
 * bad antenna.
 */
static arm_biquad_casd_df1_inst_f32 s_chf_i, s_chf_q, s_af;
static float s_chf_state_i[4U * AUDIOFIL_ETAPAS];
static float s_chf_state_q[4U * AUDIOFIL_ETAPAS];
static float s_af_state[4U * AUDIOFIL_ETAPAS];
static float s_chf_coef[AUDIOFIL_COEFS];
static float s_af_coef[AUDIOFIL_COEFS];

/*
 * LOW IF AT A FIXED Fs/16, AND WHY ZERO IF DID NOT SURVIVE A MODEL.
 *
 * The tuner is parked AM_IF_HZ = 12 kHz below the dial (main.c), so the
 * station arrives 12 kHz from the oscillator's leakage. Below, the
 * leakage is subtracted as the DC term it is, and then the whole
 * baseband is rotated by 12 kHz, which puts the station on zero for the
 * detector and moves what is left of the leakage out to 12 kHz, where
 * the 4 kHz channel filter is a second line of defence.
 *
 * There was a zero-IF version of this file for one session, tuner on
 * the dial and the DC subtraction alone, and it never ran on the radio.
 * It could not have worked on a real station: at zero IF the AM CARRIER
 * is the DC term too, and a DC estimate cannot tell the two apart. A
 * model of this chain (70 % AM, leakage 2.4 times the carrier, as
 * am_probe() measured) put the second harmonic at +7 dB ABOVE the tone
 * with the carrier 0.5 Hz from the oscillator, and the tone gone
 * entirely at 0 Hz - |z| of the sidebands alone is the rectified
 * audio. The bench could not have shown it: the generator sits a
 * repeatable 2 250 Hz off the dial, where the same model is clean.
 *
 * The reverse-engineering report was read as "the stock firmware tunes
 * to the dial", and it does not say that. FIL_MODE = 11 is the MSi001's
 * own output mode - baseband I/Q out of the chip rather than its
 * 450 kHz or 2 MHz IF - and the stock DSP chain has a shift stage AFTER
 * its DC blocker (report 2.4 item 7, 4.3). Its waterfall shows the
 * leakage a few kHz off the tuned station, which is this arrangement.
 *
 * WHY 12 kHz AND NOT THE OLD 48. Fs/4 made the rotation free and cost
 * half the panadapter; the station has to be centred on the display, and
 * a display centred on the dial can only show 96 kHz minus the offset
 * each side. At 12 kHz that is 168 of the 192 kHz. Any closer and the
 * leakage residue would sit inside the channel filter's skirt.
 *
 * WHY A FIXED OFFSET AND NOT A WINDOW. The earlier low-IF version let
 * the dial wander inside a window and moved the hardware only at its
 * edges, which was a re-park - and a click - every few detents. Here
 * the tuner follows the dial on every detent, exactly as it did at zero
 * IF, and the offset never changes.
 */

/*
 * The rotation, e^(+j*pi*n/8), one entry per sample of its 16-sample
 * period. Literal values rather than cosf() at init so that am_probe()
 * can use them before am_init() has ever run.
 *
 * +j AND NOT -j, AND THAT WAS MEASURED, NOT DERIVED. With the tuner
 * below the dial the display puts the station on its + side, but
 * am_probe() found it on the NEGATIVE side of z = I + jQ as this file
 * builds it - the old Fs/4 mixer had to turn by (+j)^n, not (-j)^n:
 *
 *     DC 1712     (-j)^n 1     (+j)^n 715
 *
 * Whether the slot assignment or the transform conjugates is not
 * settled and does not need to be. The station is at -AM_IF_HZ in z,
 * and multiplying by e^(+j*2*pi*AM_IF_HZ*n/Fs) brings it to zero.
 * am_probe() re-checks it every second on the radio.
 */
static const float k_rot_c[AM_IF_STEPS] = {
     1.0000000f,  0.9238795f,  0.7071068f,  0.3826834f,
     0.0000000f, -0.3826834f, -0.7071068f, -0.9238795f,
    -1.0000000f, -0.9238795f, -0.7071068f, -0.3826834f,
     0.0000000f,  0.3826834f,  0.7071068f,  0.9238795f,
};
static const float k_rot_s[AM_IF_STEPS] = {
     0.0000000f,  0.3826834f,  0.7071068f,  0.9238795f,
     1.0000000f,  0.9238795f,  0.7071068f,  0.3826834f,
     0.0000000f, -0.3826834f, -0.7071068f, -0.9238795f,
    -1.0000000f, -0.9238795f, -0.7071068f, -0.3826834f,
};

_Static_assert(AM_IF_STEPS * AM_IF_HZ == AM_FS_HZ,
               "the IF must divide the sample rate, or the table drifts");
_Static_assert(AM_IF_STEPS == 16U,
               "k_rot_c/k_rot_s are written out for exactly 16 steps");

/*
 * The rotation's phase, kept across chunks and blocks so it is
 * continuous. Both sizes are multiples of 16 today, so it would survive
 * a reset, but a phase that depends on a block size is a trap for
 * whoever changes the block size.
 */
static uint32_t s_rot;

/*
 * The baseband DC estimate - the oscillator's leakage - and its rate.
 * 5.2e-5 per sample at 192 kHz is a corner near 1.6 Hz.
 */
#define DC_K            0.000052f

static float s_dc_i, s_dc_q;

static float s_dc_x1, s_dc_y1;
static float s_peak;
static float s_release;

static volatile uint8_t  s_on;
static volatile uint32_t s_blocks;
static volatile uint32_t s_last, s_worst;
static volatile float    s_level;

/*
 * The fixed gain behind the AGC. Upstream calls it DEMOD_AM_GAIN and
 * sets it to 1.0; the constant is repeated rather than included because
 * demod_am.h drags the whole suite with it, and it is written here as a
 * quotation with its source named rather than as a number someone liked.
 */
#define AM_GAIN         1.0f

/* Upstream's floor for the AM/SSB/NFM path - its wideband-FM path uses a
 * different one because the discriminator's output has another scale. */
#define AM_PEAK_MIN     40.0f

/*
 * THE CHANNEL FILTER'S WIDTH, and the one number here that is a choice
 * rather than a derivation. 4 kHz is upstream's: wide enough for an AM
 * broadcast channel's audio and narrow enough that the detector sees the
 * station in the middle of the window instead of the loudest one in it.
 */
#define AM_CHANNEL_HZ   4000.0f

/* The audio cuts. 200 Hz removes rumble and the DC blocker's tail;
 * 4 kHz matches the channel filter, because audio above the channel
 * cannot be anything but noise. */
#define AM_AUDIO_LO_HZ   200.0f
#define AM_AUDIO_HI_HZ  4000.0f

/*
 * The retune trace - see am.h. ARMED BY REQUEST, NOT BY WRITING THE
 * COUNTER: main() runs below this interrupt, and resetting s_tr_n from
 * there could land between the hook reading it and writing it back,
 * losing the arm. So main() raises s_tr_req and the hook, which owns
 * the counter, does the reset at its next chunk. And `done` is false
 * while a request is pending, or a trace that has not started yet would
 * read as finished.
 */
static am_trace_t        s_tr[AM_TRACE_N];
static volatile uint32_t s_tr_n    = AM_TRACE_N;    /* full = idle */
static volatile uint32_t s_tr_mark = UINT32_MAX;
static volatile uint8_t  s_tr_req;

static void trace_put(float car, float aud, float agc, float out)
{
    if (s_tr_req) {
        s_tr_req  = 0U;
        s_tr_n    = 0U;
        s_tr_mark = UINT32_MAX;
    }
    uint32_t n = s_tr_n;

    if (n < AM_TRACE_N) {
        s_tr[n].car = car;
        s_tr[n].aud = aud;
        s_tr[n].agc = agc;
        s_tr[n].out = out;
        s_tr_n = n + 1U;
    }
}

void am_trace_arm(void)  { s_tr_req = 1U; }
void am_trace_mark(void) { s_tr_mark = s_tr_n; }
int  am_trace_done(void) { return !s_tr_req && s_tr_n >= AM_TRACE_N; }

const am_trace_t *am_trace_get(uint32_t *mark)
{
    *mark = s_tr_mark;
    return s_tr;
}

/*
 * The fine trace - see am.h. A ring written continuously once armed, so
 * the block BEFORE the retune is already there when the retune happens;
 * frozen two blocks after the block the retune landed in.
 *
 * WHICH BLOCK THE RETUNE LANDED IN: sai1_blocks counts receive halves
 * as they finish and is bumped before the hook runs, so at the moment
 * main() retunes, the half being filled is the one that will arrive as
 * block (sai1_blocks + 1). am_retune_note() reads the count and the DMA's
 * position in that half together - the count twice, so a half finishing
 * between the two reads is caught and the pair is taken again.
 */
static am_fine_t         s_ft[AM_FINE_N];
static volatile uint8_t  s_ft_on, s_ft_done;
static volatile uint32_t s_ft_mark_blk = UINT32_MAX;
static volatile uint32_t s_ft_mark_pos;
static uint32_t          s_ft_e;            /* entries written, running */
static uint32_t          s_ft_at = UINT32_MAX;
static uint32_t          s_ft_end;

void am_fine_arm(void)
{
    s_ft_done     = 0U;
    s_ft_mark_blk = UINT32_MAX;
    s_ft_at       = UINT32_MAX;
    s_ft_on       = 1U;
}

/*
 * THE CLICK REPAIR'S STATE - see am.h, "THE RETUNE CLICK".
 *
 * main() notes the write as (receive half, frames into it); the hook
 * turns that into an absolute sample number when the half arrives, and
 * from then on everything is "samples from the write", which carries
 * across block boundaries by itself. s_samp counts every frame the
 * chain has processed; it wraps after six hours and every comparison is
 * a signed difference, so the wrap is harmless.
 */
#define HOLD_LO     (AM_HOLD_FROM - AM_HOLD_RAMP)       /* -192 */
#define HOLD_HI     (AM_HOLD_TO + AM_HOLD_RAMP)         /*  512 */

static float             k_hold_ramp[AM_HOLD_RAMP];     /* 1 -> 0 */
static uint32_t          s_samp;
static volatile uint32_t s_ev_blk = UINT32_MAX;         /* noted, pending */
static volatile uint32_t s_ev_pos;
static uint32_t          s_ev_abs;
static uint8_t           s_ev_on, s_ev_frozen;
static float             s_ev_c;                        /* carrier level */

void am_retune_note(void)
{
    uint32_t b0, b1, pos;

    /* The half count twice around the position, so a half finishing
     * between the reads is caught and the pair taken again. */
    do {
        b0  = sai1_blocks;
        pos = sai1_rx_frame_pos();
        b1  = sai1_blocks;
    } while (b0 != b1);

    s_ev_pos = pos;
    s_ev_blk = b0;                      /* last: the hook keys on it */

    if (s_ft_on) {                      /* the fine trace, same instant */
        s_ft_mark_pos = pos;
        s_ft_mark_blk = b0;
    }
}

int am_retune_ok(void)
{
    return sai1_rx_frame_pos() >= (uint32_t)(-HOLD_LO) + 64U;
}

/*
 * The modulation's gain at `d` samples from the write: 1 outside the
 * window, a raised cosine down, 0 across the glitch, a raised cosine up.
 */
static float hold_gain(int32_t d)
{
    if (d < HOLD_LO || d >= HOLD_HI)  { return 1.0f; }
    if (d < AM_HOLD_FROM)             { return k_hold_ramp[d - HOLD_LO]; }
    if (d < AM_HOLD_TO)               { return 0.0f; }
    return 1.0f - k_hold_ramp[d - AM_HOLD_TO];
}

/*
 * AND WHAT FILLS THE WINDOW: THE MODULATION'S OWN CONTINUATION -
 * 21/11/2026. See am.h, "PREDICTED, NOT FADED".
 *
 * s_hist keeps the envelope as it went INTO the DC blocker - after any
 * repair, so a second retune soon after the first finds clean history -
 * one float a sample, PRED_HIST of them (21.3 ms). At the first sample
 * of a window pred_period() looks for the envelope's period in that
 * history, and the window is filled by repeating the last period; the
 * gain that used to fade the modulation now crossfades the live
 * envelope with the repetition. No period worth trusting (r below
 * PRED_R_MIN: noise, or programme without one) and the window is the
 * old hold instead, which on noise nobody hears.
 *
 * The search, as tools/clickpredict.py models it ("FIRMWARE"): a coarse
 * pass on every PRED_DEC-th sample - the envelope is band-limited far
 * below 24 kHz, so sub-sampling finds a period well enough - over lags
 * of 2 kHz down to 80 Hz; then a full-rate pass within +-PRED_DEC of the
 * coarse answer. Only autocorrelation PEAKS count: near its top a
 * sinusoid's autocorrelation falls off as a cosine, and "the first lag
 * within 90 % of the best" lands on the leading slope of the right peak
 * - 179 samples for a 192-sample period, which the model first did.
 *
 * ~90 000 multiply-adds, once a retune, inside the hook.
 */
#define PRED_HIST   4096U       /* power of two */
#define PRED_DEC    8U
#define PRED_CWIN   96U         /* coarse window, decimated: 4 ms */
#define PRED_CLO    12U         /* coarse lags 12..300 = 2 kHz..80 Hz */
#define PRED_CHI    300U
#define PRED_FWIN   512U        /* fine window, full rate: 2.7 ms */
#define PRED_R_MIN  0.5f

_Static_assert(PRED_CHI * PRED_DEC + PRED_DEC + (HOLD_HI - HOLD_LO)
               + PRED_FWIN < PRED_HIST,
               "the repeated period must outlive the window in the ring");

static float    s_hist[PRED_HIST];
static float    s_pr[PRED_CHI + 1U];
static uint8_t  s_pred_on = 1U;
static uint32_t s_pred_p;               /* this window's period, 0 = hold */
static uint32_t s_pred_t0;              /* the window's first sample */
static uint32_t s_pred_n, s_hold_n;     /* windows of each kind */
static uint32_t s_pred_last_p;
static float    s_pred_last_r;

static inline float hist_at(uint32_t t)
{
    return s_hist[t & (PRED_HIST - 1U)];
}

static float ncorr(float xy, float xx, float yy)
{
    float d = xx * yy;

    return (d > 0.0f) ? xy / sqrtf(d) : 0.0f;
}

/* The period of the history that ends just before sample t0, in
 * samples, or 0 if there is none worth repeating. */
static uint32_t pred_period(uint32_t t0, float *r_out)
{
    float m = 0.0f;

    for (uint32_t i = 1U; i <= PRED_FWIN; i++) { m += hist_at(t0 - i); }
    m /= (float)PRED_FWIN;

    /* Coarse: hd(j) = hist(t0 - 1 - j*DEC) - m, j = 0 the newest. */
    float xx = 0.0f, best = -2.0f;

    for (uint32_t j = 0; j < PRED_CWIN; j++) {
        float x = hist_at(t0 - 1U - j * PRED_DEC) - m;
        xx += x * x;
    }
    for (uint32_t L = PRED_CLO; L <= PRED_CHI; L++) {
        float xy = 0.0f, yy = 0.0f;

        for (uint32_t j = 0; j < PRED_CWIN; j++) {
            float x = hist_at(t0 - 1U - j * PRED_DEC) - m;
            float y = hist_at(t0 - 1U - (j + L) * PRED_DEC) - m;
            xy += x * y;
            yy += y * y;
        }
        s_pr[L] = ncorr(xy, xx, yy);
        if (s_pr[L] > best) { best = s_pr[L]; }
    }

    uint32_t L = PRED_CLO;
    float    top = -2.0f;

    for (uint32_t k = PRED_CLO; k <= PRED_CHI; k++) {      /* argmax */
        if (s_pr[k] > top) { top = s_pr[k]; L = k; }
    }
    for (uint32_t k = PRED_CLO + 1U; k < PRED_CHI; k++) {  /* first peak */
        if (s_pr[k] >= s_pr[k - 1U] && s_pr[k] >= s_pr[k + 1U]
            && s_pr[k] >= 0.9f * best) {
            L = k;
            break;
        }
    }

    /* Fine, at full rate, around L * DEC. */
    float    fxx = 0.0f, br = -2.0f;
    uint32_t bp  = 0U;

    for (uint32_t i = 0; i < PRED_FWIN; i++) {
        float x = hist_at(t0 - 1U - i) - m;
        fxx += x * x;
    }
    for (uint32_t p = L * PRED_DEC - PRED_DEC; p <= L * PRED_DEC + PRED_DEC;
         p++) {
        float xy = 0.0f, yy = 0.0f;

        for (uint32_t i = 0; i < PRED_FWIN; i++) {
            float x = hist_at(t0 - 1U - i) - m;
            float y = hist_at(t0 - 1U - i - p) - m;
            xy += x * y;
            yy += y * y;
        }
        float r = ncorr(xy, fxx, yy);
        if (r > br) { br = r; bp = p; }
    }

    *r_out = br;
    return (br >= PRED_R_MIN) ? bp : 0U;
}

void am_pred_set(int on) { s_pred_on = (uint8_t)(on ? 1U : 0U); }
int  am_pred_get(void)   { return s_pred_on; }

void am_pred_stats(uint32_t *pred, uint32_t *held, uint32_t *period,
                   float *r)
{
    *pred   = s_pred_n;
    *held   = s_hold_n;
    *period = s_pred_last_p;
    *r      = s_pred_last_r;
}

int am_fine_done(void) { return s_ft_done; }

/*
 * And the audio as it leaves for the codec, over the same frames as the
 * raw capture - after the AGC, what the ear gets. RETUNE_TEST 2 only.
 */
static int16_t  s_aud[AM_RAW_N];
static uint32_t s_aud_n;
static uint32_t s_aud_from = UINT32_MAX;    /* pending: block offset */

const int16_t *am_aud_get(uint32_t *n)
{
    *n = s_aud_n;
    return s_aud;
}

static int16_t  s_raw[AM_RAW_N * 2U];
static uint32_t s_raw_n;
static int32_t  s_raw_rel0;

const int16_t *am_raw_get(uint32_t *n, int32_t *rel0)
{
    *n    = s_raw_n;
    *rel0 = s_raw_rel0;
    return s_raw;
}

/* Called once, from the hook, on the block the write landed in: copy
 * the frames around it before anything touches them. */
static void raw_capture(const int16_t *rx, uint32_t frames, uint32_t pos)
{
    uint32_t from = (pos > AM_RAW_PRE) ? (pos - AM_RAW_PRE) : 0U;
    uint32_t to   = from + AM_RAW_N;

    if (to > frames) { to = frames; }

    for (uint32_t k = from; k < to; k++) {
        s_raw[(k - from) * 2U]      = rx[k * 2U];
        s_raw[(k - from) * 2U + 1U] = rx[k * 2U + 1U];
    }
    s_raw_n    = to - from;
    s_raw_rel0 = (int32_t)from - (int32_t)pos;
    s_aud_from = from;          /* the end of this block copies the audio */
}

const am_fine_t *am_fine_get(uint32_t *first, uint32_t *at)
{
    *first = (s_ft_end >= AM_FINE_N) ? ((s_ft_end - AM_FINE_N) % AM_FINE_N)
                                     : 0U;
    *at    = (s_ft_at != UINT32_MAX) ? (s_ft_at % AM_FINE_N) : UINT32_MAX;
    return s_ft;
}

uint32_t am_blocks(void)  { return s_blocks; }
float    am_level(void)   { return s_level; }
int      am_enabled(void) { return s_on ? 1 : 0; }

void am_init(float fs_hz)
{
    /*
     * The channel filter is designed by the SAME upstream function as
     * the audio one, asked for a low-pass: lo = 0 means "no high-pass"
     * and the designer says so. Reusing it is deliberate - a second
     * biquad design written here would be a second place for a
     * bilinear transform to be wrong, and this one is measured by
     * upstream's own bank.
     */
    audiofil_disena(0.0f, AM_CHANNEL_HZ, fs_hz, s_chf_coef, 0, 0);
    audiofil_disena(AM_AUDIO_LO_HZ, AM_AUDIO_HI_HZ, fs_hz, s_af_coef, 0, 0);

    arm_biquad_cascade_df1_init_f32(&s_chf_i, AUDIOFIL_ETAPAS,
                                    s_chf_coef, s_chf_state_i);
    arm_biquad_cascade_df1_init_f32(&s_chf_q, AUDIOFIL_ETAPAS,
                                    s_chf_coef, s_chf_state_q);
    arm_biquad_cascade_df1_init_f32(&s_af, AUDIOFIL_ETAPAS,
                                    s_af_coef, s_af_state);

    /* The cycle counter has to be switched on before it counts. Free,
     * idempotent, and the trace enable above it is what gates the whole
     * debug block. Not zeroed: it is shared, and spi3.c's DMA timeout
     * is one of the intervals a reset would stretch to four billion. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

    s_dc_x1 = 0.0f;
    s_dc_y1 = 0.0f;
    s_peak  = AM_PEAK_MIN;

    /*
     * THE RELEASE IS A MULTIPLIER PER SAMPLE, AND THE HEADER SAYS
     * OTHERWISE.
     *
     * agc.h describes it as "cuanto baja el pico POR MUESTRA" - how much
     * the peak drops per sample - which reads as a subtraction, and the
     * first version here computed one: (target - floor) / samples, about
     * 0.19. The code does `pico *= relajacion`. As a multiplier, 0.19
     * collapses the peak to nothing in a dozen samples.
     *
     * So this is a one-pole decay, exp(-1 / (fs * tau)), with tau half a
     * second: fast enough to follow a fading carrier, slow enough not to
     * pump on speech. Computed from the rate because the quantity is per
     * SAMPLE - upstream's own constants belong to the rates upstream
     * runs at, and this path runs at the full 192 kHz.
     *
     * The lesson is older than this file: the comment and the code
     * disagreed, and only one of them is what runs.
     */
    s_release = expf(-1.0f / (fs_hz * 0.5f));

    /* The click repair's fade, 1 -> 0 as a raised cosine: a table once
     * here rather than a cosf() per sample in the interrupt. */
    for (uint32_t i = 0; i < AM_HOLD_RAMP; i++) {
        k_hold_ramp[i] = 0.5f * (1.0f + cosf(3.14159265f * (float)i
                                             / (float)AM_HOLD_RAMP));
    }
    s_ev_on = 0U;
}

/*
 * WHAT THE DESIGNED FILTERS ACTUALLY DO, measured rather than assumed.
 *
 * Both cascades come out of upstream's designer, which was written for
 * audio rates. This path asks it for the same shapes at 192 kHz, where
 * the normalised corner frequencies are four times lower than anything
 * it was built for - the audio high-pass at 200 Hz is fc/fs = 0.001,
 * which is where a direct-form-I biquad in single precision starts to
 * lose its footing. Whether that matters is a measurement, and
 * upstream ships the instrument: audiofil_respuesta_db() evaluates the
 * cascade it designed.
 *
 * Printed from main(), not from the hook - this is not interrupt code.
 */
void am_report(void (*out)(const char *name, float hz, float db))
{
    static const float k_hz[] = {
        50.0f, 200.0f, 500.0f, 1000.0f, 3000.0f, 4000.0f, 6000.0f,
        10000.0f, 20000.0f
    };

    for (uint32_t i = 0; i < sizeof k_hz / sizeof k_hz[0]; i++) {
        out("channel", k_hz[i],
            audiofil_respuesta_db(s_chf_coef, k_hz[i], 192000.0f));
    }
    for (uint32_t i = 0; i < sizeof k_hz / sizeof k_hz[0]; i++) {
        out("audio", k_hz[i],
            audiofil_respuesta_db(s_af_coef, k_hz[i], 192000.0f));
    }
}

void am_enable(int on)
{
    if (on && !s_on) {
        /* Start from a known state rather than from whatever the last
         * run left in the filters: a biquad resumed with stale history
         * rings for a few milliseconds, which is an audible thump at
         * the moment somebody switches the audio on. */
        am_init(192000.0f);
    }
    s_on = (uint8_t)(on ? 1 : 0);
}

/*
 * THE BLOCK HOOK. Receive half in, transmit half out, same size, from
 * the DMA interrupt.
 */
void am_block(const int16_t *rx, int16_t *tx, uint32_t frames)
{
    if (!s_on) {
        /*
         * SILENCE, AND WRITTEN RATHER THAN LEFT ALONE. A transmit half
         * that is not filled plays whatever it held last time, forever,
         * at block rate - a 42.7 ms loop of the last audio there was,
         * which is a far stranger fault than silence.
         */
        for (uint32_t i = 0; i < frames * 2U; i++) { tx[i] = 0; }
        return;
    }

    s_blocks++;

    /* The fine trace: which block this is, and whether the retune that
     * was marked landed in it. */
    uint32_t blk = sai1_blocks;
    int      ft  = s_ft_on;

    /* A noted retune: the half it was written in is THIS block when the
     * count has moved on by one. Later than that it was missed (the
     * hook was busy), and a repair in the wrong place is worse than
     * none, so it is dropped. */
    if (s_ev_blk != UINT32_MAX) {
        if (blk == s_ev_blk + 1U) {
            s_ev_abs    = s_samp + s_ev_pos;
            s_ev_on     = 1U;
            s_ev_frozen = 0U;
            s_ev_blk    = UINT32_MAX;
        } else if ((int32_t)(blk - s_ev_blk) > 1) {
            s_ev_blk = UINT32_MAX;
        }
    }

    if (ft && s_ft_mark_blk != UINT32_MAX && blk == s_ft_mark_blk + 1U) {
        s_ft_at = s_ft_e + s_ft_mark_pos / AM_FINE_SUB;
        raw_capture(rx, frames, s_ft_mark_pos);
    }

    uint32_t t0 = DWT->CYCCNT;

    for (uint32_t off = 0; off < frames; off += AM_CHUNK) {
        uint32_t n = frames - off;

        if (n > AM_CHUNK) { n = AM_CHUNK; }

        /*
         * De-interleave and scale. Slot 0 is Q and slot 1 is I on this
         * board - the codec takes I on its right channel and Q on its
         * left, which main.c's capture says in as many words. For an
         * envelope it would not matter, |z| being symmetric in the two,
         * but a path that is right only by accident is one that breaks
         * when the next mode needs the sign of the phase.
         */
        const int16_t *src = &rx[off * 2U];

        /*
         * IN int16 UNITS, NOT NORMALISED TO +/-1, and that is upstream's
         * scale rather than a preference. agc.c works in the numbers it
         * will eventually write out: AGC_TARGET is 18000 and the peak
         * floor is 40, both plainly int16. The first version here
         * divided by 32768 on the way in, which left the envelope around
         * a hundredth, the AGC asking for a gain of a million, the
         * clamp at 300 granting three counts - and a radio that counted
         * blocks, reported no underruns and played silence.
         *
         * Nothing between here and the AGC cares about scale: biquads
         * are linear, |z| is homogeneous, and the DC blocker is a
         * difference. The scale only has to match at the one end that
         * has an opinion.
         */
        for (uint32_t k = 0; k < n; k++) {
            float qi = (float)src[k * 2U];
            float ii = (float)src[k * 2U + 1U];

            /*
             * SUBTRACT THE LEAKAGE FIRST, while it is still at 0 Hz.
             *
             * The oscillator's leakage is a constant vector at 0 Hz of
             * the complex baseband - DC in I and DC in Q - and on this
             * board it is more than twice the station (am_probe: DC
             * 1712 against 715). A one-pole estimate per channel, corner
             * about 1.6 Hz, takes it out. The station is 12 kHz away
             * from that corner and does not notice.
             *
             * It has to come BEFORE the rotation: after it, the leakage
             * would be a 12 kHz tone and a DC estimate would see
             * nothing to estimate. And it only works because the
             * station is not at 0 Hz too - see the top of this file for
             * what it did to a carrier that was.
             */
            s_dc_i += (ii - s_dc_i) * DC_K;
            s_dc_q += (qi - s_dc_q) * DC_K;
            ii     -= s_dc_i;
            qi     -= s_dc_q;

            /*
             * THEN ROTATE BY +12 kHz: the station from -AM_IF_HZ onto
             * zero, the leakage's residue out to +12 kHz where the
             * channel filter below buries it. Four multiplies a sample,
             * a table that repeats exactly every 16 samples, and no
             * phase accumulator to drift.
             */
            float c = k_rot_c[s_rot];
            float s = k_rot_s[s_rot];

            s_rot = (s_rot + 1U) & (AM_IF_STEPS - 1U);

            s_i[k] = ii * c - qi * s;
            s_q[k] = ii * s + qi * c;
        }

        /* The fine trace's two phasors, taken here: after the rotation
         * and BEFORE the channel filter overwrites s_i/s_q in place. */
        am_fine_t fe[AM_CHUNK / AM_FINE_SUB];
        uint32_t  ng = n / AM_FINE_SUB;

        if (ft) {
            for (uint32_t g = 0; g < ng; g++) {
                float lr = 0.0f, li = 0.0f, cr = 0.0f, ci = 0.0f;

                for (uint32_t k = g * AM_FINE_SUB;
                     k < (g + 1U) * AM_FINE_SUB; k++) {
                    li += (float)src[k * 2U];           /* slot 0 = Q */
                    lr += (float)src[k * 2U + 1U];      /* slot 1 = I */
                    cr += s_i[k];
                    ci += s_q[k];
                }
                fe[g].lr = lr / (float)AM_FINE_SUB;
                fe[g].li = li / (float)AM_FINE_SUB;
                fe[g].cr = cr / (float)AM_FINE_SUB;
                fe[g].ci = ci / (float)AM_FINE_SUB;
            }
        }

        arm_biquad_cascade_df1_f32(&s_chf_i, s_i, s_i, n);
        arm_biquad_cascade_df1_f32(&s_chf_q, s_q, s_q, n);

        for (uint32_t k = 0; k < n; k++) {
            s_cplx[k * 2U]      = s_i[k];
            s_cplx[k * 2U + 1U] = s_q[k];
        }

        arm_cmplx_mag_f32(s_cplx, s_env, n);

        /* For the trace: the carrier's level is the envelope's mean,
         * taken before the DC blocker removes exactly that. */
        float tr_car = 0.0f;

        if (s_tr_req || s_tr_n < AM_TRACE_N) {
            /* Plain loops, not arm_mean_f32(): this build compiles
             * CMSIS-DSP's filtering and complex-math directories only,
             * and a whole directory for a trace is the wrong trade. */
            for (uint32_t k = 0; k < n; k++) { tr_car += s_env[k]; }
            tr_car /= (float)n;
        }

        /*
         * DC blocker, one pole at about 15 Hz: y = x - x1 + a*y1. The
         * carrier is a constant in the envelope and the AGC must not
         * see it, or it would set the gain from the carrier's strength
         * and leave the modulation wherever that put it.
         *
         * AND THE CLICK REPAIR, in the same loop, because it needs the
         * blocker's own state: around a noted retune the envelope is
         * held at the carrier's level - x1 - y1, the blocker's running
         * mean, frozen at the first sample of the window - and only the
         * modulation is faded (am.h, "THE RETUNE CLICK"). The blocker
         * therefore sees a carrier that never moved and modulation that
         * dips smoothly, and the glitch never becomes a spike.
         */
        for (uint32_t k = 0; k < n; k++) {
            float    x = s_env[k];
            uint32_t t = s_samp + off + k;

            if (s_ev_on) {
                int32_t d = (int32_t)(t - s_ev_abs);

                if (d >= HOLD_HI) {
                    s_ev_on = 0U;
                } else if (d >= HOLD_LO) {
                    if (!s_ev_frozen) {
                        float r = 0.0f;

                        s_ev_c      = s_dc_x1 - s_dc_y1;
                        s_ev_frozen = 1U;
                        s_pred_t0   = t;
                        s_pred_p    = s_pred_on ? pred_period(t, &r) : 0U;
                        s_pred_last_p = s_pred_p;
                        s_pred_last_r = r;
                        if (s_pred_p != 0U) { s_pred_n++; } else { s_hold_n++; }
                    }

                    float g = hold_gain(d);

                    if (s_pred_p != 0U) {
                        /* The last period before the window, repeated:
                         * always samples from before s_pred_t0, which
                         * the ring still holds (the assertion above). */
                        uint32_t j  = (t - s_pred_t0) % s_pred_p;
                        float    xp = hist_at(s_pred_t0 - s_pred_p + j);

                        x = xp + g * (x - xp);
                    } else {
                        x = s_ev_c + g * (x - s_ev_c);
                    }
                }
            }

            s_hist[t & (PRED_HIST - 1U)] = x;

            s_dc_y1   = x - s_dc_x1 + 0.9995f * s_dc_y1;
            s_dc_x1   = x;
            s_env[k]  = s_dc_y1;
        }

        arm_biquad_cascade_df1_f32(&s_af, s_env, s_env, n);

        float tr_aud = 0.0f;

        if (s_tr_req || s_tr_n < AM_TRACE_N) {
            for (uint32_t k = 0; k < n; k++) {
                float a = fabsf(s_env[k]);

                if (a > tr_aud) { tr_aud = a; }
            }
        }

        /* Upstream's AGC, which also writes 2*n int16 - the same sample
         * to both channels, because the speaker is bridged. */
        s_peak = agc_bloque(s_env, &tx[off * 2U], n,
                            s_peak, s_release, AM_PEAK_MIN, AM_GAIN);

        /* The fine trace's audio, from what the codec will play, and the
         * chunk's entries into the ring. */
        if (ft) {
            for (uint32_t g = 0; g < ng; g++) {
                uint32_t k = off + g * AM_FINE_SUB;

                fe[g].a0 = tx[k * 2U];
                fe[g].a1 = tx[(k + AM_FINE_SUB / 2U) * 2U];
                s_ft[s_ft_e % AM_FINE_N] = fe[g];
                s_ft_e++;
            }
        }

        if (s_tr_req || s_tr_n < AM_TRACE_N) {
            int32_t mo = 0;

            for (uint32_t k = 0; k < n; k++) {
                int32_t v = tx[(off + k) * 2U];

                if (v < 0) { v = -v; }
                if (v > mo) { mo = v; }
            }
            trace_put(tr_car, tr_aud, s_peak, (float)mo);
        }
    }

    /* Freeze the fine trace once the block AFTER the retune's is in:
     * the ring then holds the one before, the retune's, and the one
     * after. */
    if (ft && s_ft_mark_blk != UINT32_MAX && blk >= s_ft_mark_blk + 2U) {
        s_ft_end  = s_ft_e;
        s_ft_on   = 0U;
        s_ft_done = 1U;
    }

    if (AM_RAW_N > 1U && s_aud_from != UINT32_MAX) {
        uint32_t m = frames - s_aud_from;

        if (m > AM_RAW_N) { m = AM_RAW_N; }
        for (uint32_t k = 0; k < m; k++) {
            s_aud[k] = tx[(s_aud_from + k) * 2U];
        }
        s_aud_n    = m;
        s_aud_from = UINT32_MAX;
    }

    s_samp += frames;
    s_level = s_peak;

    /*
     * HOW LONG THIS TOOK, because it runs in an interrupt and the main
     * loop is what feeds the watchdog. A hook that overran its own
     * block rate would starve that loop, the watchdog would bite after
     * thirty-two seconds, and the reset drops PC6 - which switches the
     * radio OFF. That has happened once already in this project, from a
     * polled loop, and it looks exactly like a hardware fault.
     *
     * So the cost is a number rather than a hope. The budget is one
     * block, 42.7 ms at 480 MHz = about 20.5 million cycles.
     *
     * MEASURED WITH THE CORE'S OWN CYCLE COUNTER, not with
     * board_cycles(). That one is assembled from SysTick's millisecond
     * count and its countdown register, and read from inside an
     * interrupt the pair can come back inconsistent - the first version
     * here reported a difference that had gone NEGATIVE and wrapped,
     * 4 294 734 thousand cycles, which is the unsigned face of about
     * half a millisecond backwards. DWT->CYCCNT is a free-running
     * 32-bit counter of core clocks: monotonic, nothing to race
     * against, and it wraps every 8.9 seconds, which is two hundred
     * times this budget.
     */
    uint32_t took = DWT->CYCCNT - t0;

    if (took > s_worst) { s_worst = took; }
    s_last = took;
}

/*
 * am_resettle() WAS HERE - a two-block mute, a filter reset and a 5 ms
 * fade after every retune, written to hide "the second of silence" that
 * upstream's instant-attack AGC was believed to produce. Measured
 * 19/11/2026 (WORKING.md, "The retune transient, measured"), it was the
 * worst thing on the table: resetting the DC blocker turned the carrier
 * into a step from nothing, 6.5 dB of dip for 400 ms where the retune
 * left alone cost 2 dB for 100 ms. Removed the same day, after the owner
 * listened to the knob without it: the second of silence was gone.
 */

uint32_t am_cycles(void)      { return s_last; }
uint32_t am_cycles_worst(void){ return s_worst; }

/*
 * WHERE THE CARRIER ACTUALLY IS, which two wrong guesses about signs
 * have now earned the right to be measured instead of reasoned about.
 *
 * Correlating the raw block against the three frequencies that matter
 * costs one pass: DC is just the sum, and the other two are the block
 * multiplied by the demodulator's own table and by its conjugate. Using
 * THE SAME TABLE is the point - a probe with its own idea of the sign
 * could agree with itself and disagree with the audio path.
 *
 *   station  sum of z * e^(+j*pi*k/8)  - what am_block() brings to zero
 *   mirror   sum of z * e^(-j*pi*k/8)  - the image of it
 *
 * With a carrier on the dial, `station` must clearly beat `mirror`. If
 * `mirror` wins, the rotation turns the wrong way and the speaker is
 * playing the image. `dc` is the leakage and is normally the biggest
 * of the three - about 1 700 against a bench carrier of about 715 -
 * so it says nothing about the sign.
 */
void am_probe(const int16_t *rx, uint32_t frames,
              float *dc, float *station, float *mirror)
{
    float dr = 0.0f, di = 0.0f;
    float sr = 0.0f, si = 0.0f;
    float mr = 0.0f, mi = 0.0f;

    for (uint32_t k = 0; k < frames; k++) {
        float q = (float)rx[k * 2U];
        float i = (float)rx[k * 2U + 1U];
        float c = k_rot_c[k & (AM_IF_STEPS - 1U)];
        float s = k_rot_s[k & (AM_IF_STEPS - 1U)];

        dr += i;
        di += q;

        sr += i * c - q * s;            /* (i + jq)(c + js) */
        si += i * s + q * c;

        mr += i * c + q * s;            /* (i + jq)(c - js) */
        mi += q * c - i * s;
    }

    float n = (frames != 0U) ? (float)frames : 1.0f;

    *dc      = sqrtf(dr * dr + di * di) / n;
    *station = sqrtf(sr * sr + si * si) / n;
    *mirror  = sqrtf(mr * mr + mi * mi) / n;
}
