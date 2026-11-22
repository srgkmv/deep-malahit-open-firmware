#include <string.h>
#include "demod.h"
#include "am.h"           /* AM_IF_HZ, and am.c for the A/B */
#include "sai1.h"
#include "console.h"
#include "clock.h"        /* clock_sysclk_hz, for cycles -> us */
#include "stm32h7xx.h"    /* DWT->CYCCNT */
#include "demod_am.h"     /* ../../User - upstream */
#include "sdr_rx.h"       /* ../../User - SDR_RX_BLOCK_SAMPLES_WFM */
#include "gd32_i2s.h"     /* ../../User - the sink's prototype */
#include "rds.h"          /* ../../User - rds_feed(), wrapped below */
#include "estereo.h"      /* src/ - FM stereo, this port's */
#include "cw.h"           /* ../../User - the Morse decoder, demod_cw() */
#include "rtty.h"         /* ../../User - the RTTY decoder, demod_rtty() */
#include "rtty_auto.h"    /* ../../User */
#include "config.h"       /* ../../User - CONFIG_RTTY_MARK_HZ/SPACE_HZ */
#include "psk31.h"        /* ../../User - demod_psk31() */

/*
 * Upstream's call: 512 frames at 192 kHz in (with rf_doble), 512 stereo
 * frames out. The port's half is a whole number of them - 8192 = 16 x
 * 512 - and demod_block() still copes with a tail, by silencing it and
 * counting, rather than trusting that the arithmetic never changes.
 */
#define CHUNK   ((uint32_t)SDR_RX_BLOCK_SAMPLES_WFM)

/* Where the call in progress must put its audio, and how much of it. */
static int16_t  *s_sink;
static uint32_t  s_sink_n;

static uint8_t   s_path;                        /* 0 am.c, 1 upstream */
static int32_t   s_mix_hz = (int32_t)AM_IF_HZ;

/*
 * WFM is upstream's OTHER block hook - demod_wfm_process_raw(), its own
 * state, 512 frames at 192 kHz in and out, exactly this port's chunk -
 * so the loop below calls one or the other. Upstream swaps the hook its
 * DMA calls (sdr_rx_set_block_hook()); here the hook stays demod_block()
 * and picks by s_wfm.
 *
 * s_cambiando: a mode change in progress, from the display loop. The
 * block interrupt preempts the display loop, and upstream's mode switch
 * is not something to run a block through half-way - upstream takes its
 * hook off for the duration (apply_demod_mode(): sdr_rx_set_block_hook(0)
 * first). A block that lands then plays silence and is counted.
 */
static volatile uint8_t  s_wfm;
static volatile uint8_t  s_cambiando;
static volatile uint32_t s_mudos;      /* blocks silenced by a mode change */

static volatile uint32_t s_hooks;      /* demod_block() calls               */
static volatile uint32_t s_sink_bad;   /* sink asked for a length != CHUNK  */
static volatile uint32_t s_sink_none;  /* a call returned without audio     */
static volatile uint32_t s_tail;       /* frames silenced past the last CHUNK */
static volatile uint32_t s_last;       /* cycles, the whole hook            */
static volatile uint32_t s_worst;
static volatile uint32_t s_up_worst;   /* upstream's own count, per call    */

void demod_init(void)
{
    /*
     * The rates this port runs at, in upstream's words: the codec at
     * 192 kHz (sai1.c), so rf_doble - which halves to 96 kHz behind the
     * noise blanker and doubles back on the way out - and the 96 kHz
     * coefficient set, not the 48 kHz one. Same calls, same order as
     * upstream's apply_demod_mode().
     */
    demod_am_init();
    demod_am_set_active_rate(0U);
    demod_am_set_rf_doble(1U);
    demod_am_set_mode(DEMOD_MODE_AM);
    demod_am_set_mix_hz((float)s_mix_hz);

    __asm__ volatile ("" :: "r"(demod_path), "r"(demod_mode),
                            "r"(demod_mix), "r"(demod_report),
                            "r"(estereo_pon), "r"(demod_cw),
                            "r"(demod_rtty), "r"(demod_psk31),
                            "r"(demod_graba));
}

void demod_block(const int16_t *rx, int16_t *tx, uint32_t frames)
{
    uint32_t t0  = DWT->CYCCNT;
    uint32_t off = 0U;

    if (s_cambiando) {
        memset(tx, 0, frames * 2U * sizeof(int16_t));
        s_mudos++;
        return;
    }

    for (; off + CHUNK <= frames; off += CHUNK) {
        s_sink   = &tx[off * 2U];
        s_sink_n = CHUNK;

        if (s_wfm) { demod_wfm_process_raw(&rx[off * 2U]); }
        else       { demod_am_process_raw(&rx[off * 2U]); }

        /*
         * A call that delivered nothing leaves this stretch of the
         * transmit half holding what it played two halves ago - an echo,
         * 85 ms late. Silence instead, and count it: upstream does have
         * paths that skip the sink (demod_am.c, "no se le llama para
         * este bloque").
         */
        if (s_sink != 0) {
            memset(s_sink, 0, CHUNK * 2U * sizeof(int16_t));
            s_sink = 0;
            s_sink_none++;
        }

        uint32_t up = demod_am_get_last_cycles();

        if (up > s_up_worst) { s_up_worst = up; }
    }

    if (off < frames) {
        memset(&tx[off * 2U], 0, (frames - off) * 2U * sizeof(int16_t));
        s_tail += frames - off;
    }

    s_hooks++;
    s_last = DWT->CYCCNT - t0;
    if (s_last > s_worst) { s_worst = s_last; }
}

/*
 * UPSTREAM'S AUDIO SINK. On the DEEPSDR this copies into the I2S
 * transmit half; here, into the 512 frames of the port's transmit half
 * that demod_block() pointed it at. One delivery per call: a second one
 * would be audio for a stretch that has no slot, so it is dropped and
 * counted, not written past the end.
 */
void gd32_i2s_stream_write_half(const int16_t *stereo_frames, uint32_t n_frames)
{
    if (s_sink == 0) { s_sink_bad++; return; }

    if (n_frames != s_sink_n) {
        s_sink_bad++;
        if (n_frames > s_sink_n) { n_frames = s_sink_n; }
        memset(s_sink, 0, s_sink_n * 2U * sizeof(int16_t));
    }
    /* In WFM, upstream's frames are the mono sum: the stereo decoder
     * makes L and R of them, with the difference it found in this
     * block's multiplex (src/estereo.c), straight into the ring. */
    if (!s_wfm || !estereo_aplica(stereo_frames, s_sink, n_frames)) {
        memcpy(s_sink, stereo_frames, n_frames * 2U * sizeof(int16_t));
    }
    s_sink = 0;
}

/*
 * THE MULTIPLEX, WHERE UPSTREAM HANDS IT TO RDS - linked with
 * --wrap=rds_feed (reglas.mk): demod_am.c's call lands here, the stereo
 * decoder sees the block first, and rds.c gets it as before. Upstream
 * calls rds_feed() only while rds_activo(), and this port keeps RDS on in
 * every WFM session (main.c, rds_modo()) - so in WFM this always runs.
 */
void __real_rds_feed(const float *mpx, uint32_t n);
void __wrap_rds_feed(const float *mpx, uint32_t n)
{
    if (s_wfm) { estereo_mpx(mpx, n); }
    __real_rds_feed(mpx, n);
}

/*
 * WHAT A TEXT DECODER IS FED, RECORDED - for the bench, 22/11/2026. PSK31
 * came out of the radio as clean phase and wrong letters while the same
 * signal decoded on the host: the question is what reaches psk31_process()
 * here. `swd-call ... demod_graba 1` arms it; the next GRABA_N samples of
 * the 12 kHz sideband audio upstream hands PSK31 land in g_graba (D2 SRAM,
 * float), and `swd.sh -c init -c "dump_image f.f32 <g_graba> 120000"`
 * takes them out for the host's tools. psk31_process() is wrapped for it,
 * as rds_feed() is for the stereo.
 */
/*
 * 120 kB of D2 SRAM, so it is built only on request - the decoders still
 * to come want that memory:
 *     make BUILD_DIR=/some/scratch OPT="-O2 -g3 -DDEMOD_GRABA=1"
 * It found that PSK31's "clean phase, wrong letters" was the signal 13 Hz
 * outside the decoder's +-7.8 Hz, not the radio's chain (PORTING.md 44).
 */
#ifndef DEMOD_GRABA
#define DEMOD_GRABA 0
#endif
#if DEMOD_GRABA
#define GRABA_N  30000U                     /* 2.5 s at 12 kHz */
__attribute__((section(".d2sram"), aligned(32))) float g_graba[GRABA_N];
volatile uint32_t g_graba_n = GRABA_N;      /* GRABA_N = not recording */
#endif

__attribute__((noinline)) void demod_graba(int on)
{
#if DEMOD_GRABA
    g_graba_n = on ? 0U : GRABA_N;
    console_printf("demod     : recording the decoder's audio %s\n", on ? "armed" : "off");
#else
    (void)on;
    console_printf("demod     : no recorder in this image (DEMOD_GRABA=1)\n");
#endif
}

void __real_psk31_process(const float *audio, uint32_t n);
void __wrap_psk31_process(const float *audio, uint32_t n)
{
#if DEMOD_GRABA
    uint32_t i;

    for (i = 0U; i < n && g_graba_n < GRABA_N; i++) { g_graba[g_graba_n++] = audio[i]; }
#endif
    __real_psk31_process(audio, n);
}

/*
 * The swd-call entry points. Nothing in the image calls them, so
 * --gc-sections would take them out and swd-call.sh would then say "no
 * function" about a function that is in the source. `retain` is the
 * attribute for that and this target ignores it (no SHF_GNU_RETAIN on
 * arm-none-eabi), so demod_init() names them in an empty asm instead:
 * a reference the linker sees and the CPU never executes.
 */
#define SWD_ENTRY __attribute__((noinline))

SWD_ENTRY void demod_path(int upstream)
{
    /*
     * From the display loop, which the block interrupt preempts, so
     * neither hook is ever half-way through a block when this runs.
     * am.c is re-initialised on the way back (am_enable(1) does that),
     * so it starts from clean filters rather than from 2 s-old ones.
     * Upstream's state is NOT reset going the other way, on purpose:
     * that is what its own mode switch does, and it is the thing being
     * tried.
     */
    if (upstream) {
        sai1_set_block_hook(demod_block);
        am_enable(0);
        s_path = 1U;
    } else {
        am_enable(1);
        sai1_set_block_hook(am_block);
        s_path = 0U;
    }
    console_printf("Audio     : %s\n", s_path ? "upstream demod_am.c"
                                              : "this port's am.c");
}

int demod_path_get(void) { return s_path; }

/*
 * A MODE, upstream's apply_demod_mode() minus what this board does not
 * have: no codec rate to change - the NAU8822 runs at 192 kHz for every
 * mode here, which is the rate upstream's WFM wants - and no hook to
 * swap. What is left is its order: the mode; rf_doble off in WFM, as
 * upstream's "!will_be_wfm && s_rf_doble"; the decimators forgotten; a
 * fresh WFM entry or a short WFM mute, or the AM chain's reset; and the
 * mix at 0 in WFM, as demod_am.h asks of whoever tunes ("demod_am_set_
 * mix_hz(0)"). The TUNER's half - on the dial in WFM - is main.c's
 * radio_mode_set().
 */
SWD_ENTRY int demod_mode(int mode)
{
    uint8_t wfm, era;

    if (mode < (int)DEMOD_MODE_AM || mode > (int)DEMOD_MODE_SAM) {
        console_printf("demod     : mode %d refused (0 AM 1 USB 2 LSB "
                       "3 NFM 4 WFM 5 SAM)\n", mode);
        return (int)demod_am_get_mode();
    }
    wfm = (uint8_t)(mode == (int)DEMOD_MODE_WFM);
    era = s_wfm;

    s_cambiando = 1U;
    __DSB();

    demod_am_set_mode((demod_mode_t)mode);
    demod_am_set_rf_doble((uint8_t)!wfm);
    demod_am_decim_olvida();
    if (wfm) {
        if (era) { demod_wfm_mute_corto(); }
        else     { demod_wfm_reset_diag(); }
    } else {
        demod_am_reset_diag();
    }
    demod_am_set_mix_hz(wfm ? 0.0f : (float)s_mix_hz);
    if (wfm) { estereo_reset(); }       /* under s_cambiando: no block runs */
    s_wfm = wfm;

    __DSB();
    s_cambiando = 0U;
    return (int)demod_am_get_mode();
}

/*
 * THE MORSE DECODER'S SWITCH, under the same flag as a mode change.
 * Upstream throws it from its display loop with the hook still running
 * (menu_mode_preset_callback(): cw_set_enabled()), and turning it ON
 * clears every timer cw_process() is in the middle of using - a block
 * landing half-way through that would run on a half-cleared state. Two
 * stores and a memset are microseconds; a block that lands then plays
 * silence and is counted, as for a mode.
 */
void demod_quieto(int on)
{
    __DSB();
    s_cambiando = (uint8_t)(on != 0);
    __DSB();
}

SWD_ENTRY void demod_cw(int on)
{
    if ((uint8_t)(on != 0) == cw_get_enabled()) { return; }
    demod_quieto(1);
    cw_set_enabled((uint8_t)(on != 0));
    demod_quieto(0);
}

/* PSK31 on or off, upstream's psk31_start(DEMOD_DEC_FS_HZ) / psk31_stop()
 * - from zero on every start - under the same flag. */
SWD_ENTRY void demod_psk31(int on)
{
    if ((uint8_t)(on != 0) == psk31_activo()) { return; }
    demod_quieto(1);
    if (on) { psk31_start(12000.0f); } else { psk31_stop(); }
    demod_quieto(0);
}

/*
 * RTTY, upstream's menu_mode_preset_callback() for its two entries:
 * RTTY-L (LSB) with config.h's mark and space, RTTY-U (USB) with them
 * swapped, because USB hears the same two radio tones mirrored. Then the
 * station's own inversion again on top (rtty_reapply_station_inversion()),
 * on, and rtty_auto.c from zero. Same flag as a mode: rtty_set_enabled()
 * restarts the bit sync the block interrupt is in the middle of.
 */
SWD_ENTRY void demod_rtty(int variante)
{
    demod_quieto(1);
    if (variante == 1 || variante == 2) {
        /* The shift the person chose survives the entry, as the speed
         * does: upstream sets config.h's pair here and so drops it. */
        float sh = rtty_get_shift_hz();

        if (variante == 1) { rtty_set_mark_space_hz(CONFIG_RTTY_MARK_HZ, CONFIG_RTTY_SPACE_HZ); }
        else               { rtty_set_mark_space_hz(CONFIG_RTTY_SPACE_HZ, CONFIG_RTTY_MARK_HZ); }
        rtty_set_shift_hz(sh);
        rtty_reapply_station_inversion();
        rtty_set_enabled(1U);
        rtty_auto_reset();
    } else {
        rtty_set_enabled(0U);
    }
    demod_quieto(0);
}

SWD_ENTRY void demod_mix(int hz)
{
    s_mix_hz = (int32_t)hz;
    if (!s_wfm) { demod_am_set_mix_hz((float)hz); }
    console_printf("demod     : mix %d Hz\n", hz);
}

static uint32_t us(uint32_t cycles)
{
    return (uint32_t)(((uint64_t)cycles * 1000000U) / clock_sysclk_hz);
}

SWD_ENTRY void demod_report(void)
{
    static const char *const names[] = { "AM", "USB", "LSB", "NFM",
                                         "WFM", "SAM" };
    int m = (int)demod_am_get_mode();

    console_printf("demod     : path %s, mode %s, mix %d Hz\n",
                   s_path ? "upstream" : "am.c",
                   (m >= 0 && m <= 5) ? names[m] : "?",
                   s_wfm ? 0 : s_mix_hz);     /* in force: 0 in WFM */
    console_printf("            hook %u us last, %u us worst of 42667; "
                   "upstream %u us worst a call\n",
                   us(s_last), us(s_worst), us(s_up_worst));
    console_printf("            hooks %u, SAI overruns %u, sink bad %u, "
                   "no audio %u, tail %u, muted by a mode change %u\n",
                   s_hooks, sai1_block_overruns, s_sink_bad, s_sink_none,
                   s_tail, s_mudos);
    if (s_wfm) {
        estereo_estado_t e;

        estereo_estado(&e);
        console_printf("            stereo %s: pilot %d Hz deviation, SNR %d dB, "
                       "at %d.%02d Hz; blend %d %%, g %d.%02d, match %d dB\n",
                       estereo_puesto() ? "on" : "OFF",
                       (int32_t)e.piloto_dev_hz, (int32_t)e.snr_db,
                       (int32_t)e.piloto_hz, (int32_t)(e.piloto_hz * 100.0f) % 100,
                       (int32_t)(e.mezcla * 100.0f),
                       (int32_t)e.ganancia, (int32_t)(e.ganancia * 100.0f) % 100,
                       (int32_t)e.resto_db);
        console_printf("            stereo cost: decode %u us, apply %u us a call "
                       "(x16 a block)\n", us(e.ciclos_mpx), us(e.ciclos_aplica));
    }
}
