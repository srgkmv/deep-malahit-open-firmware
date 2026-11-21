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
                            "r"(demod_mix), "r"(demod_report));
}

void demod_block(const int16_t *rx, int16_t *tx, uint32_t frames)
{
    uint32_t t0  = DWT->CYCCNT;
    uint32_t off = 0U;

    for (; off + CHUNK <= frames; off += CHUNK) {
        s_sink   = &tx[off * 2U];
        s_sink_n = CHUNK;

        demod_am_process_raw(&rx[off * 2U]);

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
    memcpy(s_sink, stereo_frames, n_frames * 2U * sizeof(int16_t));
    s_sink = 0;
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

SWD_ENTRY int demod_mode(int mode)
{
    if (mode < (int)DEMOD_MODE_AM || mode > (int)DEMOD_MODE_SAM
        || mode == (int)DEMOD_MODE_WFM) {
        console_printf("demod     : mode %d refused (0 AM 1 USB 2 LSB "
                       "3 NFM 5 SAM)\n", mode);
    } else {
        demod_am_set_mode((demod_mode_t)mode);
    }
    return (int)demod_am_get_mode();
}

SWD_ENTRY void demod_mix(int hz)
{
    s_mix_hz = (int32_t)hz;
    demod_am_set_mix_hz((float)hz);
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
                   (m >= 0 && m <= 5) ? names[m] : "?", s_mix_hz);
    console_printf("            hook %u us last, %u us worst of 42667; "
                   "upstream %u us worst a call\n",
                   us(s_last), us(s_worst), us(s_up_worst));
    console_printf("            hooks %u, SAI overruns %u, sink bad %u, "
                   "no audio %u, tail %u\n",
                   s_hooks, sai1_block_overruns, s_sink_bad, s_sink_none,
                   s_tail);
}
