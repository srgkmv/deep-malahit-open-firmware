/*
 * WSPR COMO LO LLEVA LA RADIO, CONTRA CUALQUIER FICHERO - 25/11/2026, port del
 * Malahit (paso 58).
 *
 * El modo de verdad -wspr_modo.c con wspr_rx.c, wspr_fano.c y wspr_msg.c, la
 * memoria prestada de upstream (hfdl_ram.c sobre la union)- alimentado con
 * audio de banda lateral a 12 kHz (f32), a bloques de 32 muestras como los da
 * demod_am.c a 96 kHz, con wspr_modo_poll() cada TICK ms. El reloj de mentira
 * empieza `lead` segundos antes de un minuto par, que es como escribe sus
 * ficheros malahit/tools/hackrf_gen.py wspr --audio.
 *
 *   ./out/wsprradio audio.f32 [lead_s] [escala] [tick_ms]
 *
 * Saca cada baliza, y al cerrar cada ventana la corroboracion (q, 88 para
 * darla por buena) y lo que tardo wspr_modo_poll() en el reloj del PC.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "wspr_modo.h"
#include "rtc_hw.h"

volatile uint32_t g_msticks;
static uint32_t g_reloj_ms;

void rtc_hw_get(rtc_hw_datetime_t *o)
{
    uint32_t s = g_reloj_ms / 1000UL;

    if (!o) { return; }
    memset(o, 0, sizeof *o);
    o->second = (uint8_t)(s % 60UL);
    o->minute = (uint8_t)((s / 60UL) % 60UL);
    o->hour   = (uint8_t)((s / 3600UL) % 24UL);
    o->year = 2026; o->month = 11; o->day = 25;
}
bool rtc_hw_has_ever_synced(void) { return true; }

#define BLOQUE 32U

int main(int argc, char **argv)
{
    FILE *f;
    float buf[BLOQUE];
    float lead = (argc > 2) ? (float)atof(argv[2]) : 5.0f;
    float esc  = (argc > 3) ? (float)atof(argv[3]) : 1.0f;
    uint32_t tick = (argc > 4) ? (uint32_t)atoi(argv[4]) : 40U;
    uint32_t n = 0U, ult = 0U, k, t0_ms;
    uint8_t capt = 0U;
    double peor = 0.0;

    if (argc < 2) { printf("uso: wsprradio audio.f32 [lead_s] [escala] [tick_ms]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    g_msticks = 1000U;
    t0_ms = 120000U - (uint32_t)(lead * 1000.0f + 0.5f);   /* el minuto 2 es par */
    g_reloj_ms = t0_ms;
    if (!wspr_modo_start()) { printf("wspr_modo_start() no arranca\n"); return 1; }
    for (;;) {
        size_t got = fread(buf, sizeof buf[0], BLOQUE, f);

        if (got < BLOQUE) { memset(&buf[got], 0, (BLOQUE - got) * sizeof buf[0]); }
        for (k = 0U; k < BLOQUE; k++) { buf[k] *= esc; }
        wspr_modo_mete(buf, (uint16_t)BLOQUE);
        n += BLOQUE;
        {
            uint32_t ms = (uint32_t)(((uint64_t)n * 1000ULL) / 12000ULL);

            g_msticks = 1000U + ms;
            g_reloj_ms = t0_ms + ms;
        }
        if ((g_msticks - ult) >= tick) {
            struct timespec a, b;
            double d;

            ult = g_msticks;
            clock_gettime(CLOCK_MONOTONIC, &a);
            wspr_modo_poll();
            clock_gettime(CLOCK_MONOTONIC, &b);
            d = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
            if (d > peor) { peor = d; }
        }
        if (wspr_modo_capturando() && !capt) {
            printf("ventana abierta en %u:%02u\n", (unsigned)((g_reloj_ms / 60000U) % 60U),
                   (unsigned)((g_reloj_ms / 1000U) % 60U));
        }
        if (!wspr_modo_capturando() && capt) {
            uint32_t i;

            printf("ventana cerrada: q %u, %u oidas\n", (unsigned)wspr_modo_q(),
                   (unsigned)wspr_modo_buenas());
            for (i = 0U; i < wspr_modo_lineas() && i < 1U; i++) {
                const char *l = wspr_modo_linea(i);
                char t[WSPR_MODO_LARGO + 4];
                uint32_t j;

                for (j = 0U; l[j] != '\0' && j < sizeof t - 1U; j++) { t[j] = (l[j] == '\t') ? '|' : l[j]; }
                t[j] = '\0';
                printf("  %s\n", t);
            }
        }
        capt = wspr_modo_capturando();
        if (got < BLOQUE && !capt && (g_reloj_ms % 120000U) > 115000U) { break; }
    }
    fclose(f);
    printf("%u balizas; el peor wspr_modo_poll() %.1f ms en este PC\n",
           (unsigned)wspr_modo_buenas(), peor);
    wspr_modo_stop();
    return 0;
}
