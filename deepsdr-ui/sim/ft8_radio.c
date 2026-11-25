/*
 * FT8 COMO LO LLEVA LA RADIO, CONTRA CUALQUIER FICHERO - 25/11/2026, port del
 * Malahit (paso 57).
 *
 * El modo de verdad -ft8_modo.c con el decimador, la FFT, el adaptador y el
 * decodificador de Ft8Lib- alimentado con audio de banda lateral a 12 kHz
 * (f32, la escala que se le de: es la de s_ssb_dec en la radio), a bloques
 * de 32 muestras como los da demod_am.c a 96 kHz, con ft8_modo_tick() cada
 * TICK ms como el bucle principal. El reloj de mentira empieza `lead`
 * segundos antes de un borde de ranura, que es como escribe sus ficheros
 * malahit/tools/hackrf_gen.py ft8 --audio.
 *
 *   ./out/ft8radio audio.f32 [lead_s] [escala] [tick_ms] [ms_por_candidato]
 *
 * ms_por_candidato: lo que tarda en la radio cada paso del decodificador.
 * Sin el, la decodificacion entera cabe en un tick (el reloj de mentira no
 * corre dentro de ft8_modo_tick()); con el, cada paso adelanta g_msticks y
 * la ranura se decodifica a lo largo de varios ticks, como en la radio
 * (0,5-0,7 s). Necesita -Wl,--wrap=ft8_decoder_slot_paso (out/ft8radio lo
 * lleva).
 *
 * Saca cada mensaje y, por ranura, los numeros que la radio saca por la
 * consola: candidatos, mensajes, el DT medio y el margen de dB.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ft8_modo.h"
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

static uint32_t g_ms_cand;
uint8_t __real_ft8_decoder_slot_paso(void);
uint8_t __wrap_ft8_decoder_slot_paso(void)
{
    g_msticks += g_ms_cand;     /* lo que el tick ve pasar; se devuelve despues */
    return __real_ft8_decoder_slot_paso();
}

int main(int argc, char **argv)
{
    FILE *f;
    float buf[BLOQUE];
    float lead = (argc > 2) ? (float)atof(argv[2]) : 5.0f;
    float esc  = (argc > 3) ? (float)atof(argv[3]) : 1.0f;
    uint32_t tick = (argc > 4) ? (uint32_t)atoi(argv[4]) : 40U;
    uint32_t t_antes;
    uint32_t n = 0U, ult_tick = 0U, total_visto = 0U, ranura = 0U;
    uint8_t seg_ant = 0xFFU;
    uint32_t k;

    if (argc < 2) { printf("uso: ft8radio audio.f32 [lead_s] [escala] [tick_ms] [ms_por_candidato]\n"); return 2; }
    g_ms_cand = (argc > 5) ? (uint32_t)atoi(argv[5]) : 0U;
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    g_msticks = 1000U;
    g_reloj_ms = 60000U - (uint32_t)(lead * 1000.0f + 0.5f);
    ft8_modo_start(12000.0f);
    for (;;) {
        size_t got = fread(buf, sizeof buf[0], BLOQUE, f);

        if (got < BLOQUE) {
            /* y despues del fichero, silencio hasta que se ha contado la
             * ultima ranura (su segundo 5, abajo) */
            memset(&buf[got], 0, (BLOQUE - got) * sizeof buf[0]);
        }
        for (k = 0U; k < BLOQUE; k++) { buf[k] *= esc; }
        ft8_modo_come(buf, (uint16_t)BLOQUE);
        n += BLOQUE;
        /* 32 muestras a 12 kHz son 2,667 ms: el reloj en microsegundos */
        {
            uint32_t ms = (uint32_t)(((uint64_t)n * 1000ULL) / 12000ULL);
            uint32_t d = ms - (g_msticks - 1000U);

            g_msticks += d;
            g_reloj_ms += d;
        }
        if ((g_msticks - ult_tick) >= tick) {
            ult_tick = g_msticks;
            t_antes = g_msticks;
            ft8_modo_tick();
            g_msticks = t_antes;        /* el tiempo de verdad lo lleva el audio */
        }
        if (ft8_modo_total() != total_visto) {
            uint32_t nuevos = ft8_modo_total() - total_visto, i;

            if (nuevos > FT8_MODO_LINEAS) { nuevos = FT8_MODO_LINEAS; }
            for (i = nuevos; i > 0U; i--) {
                const char *l = ft8_modo_linea((uint8_t)(i - 1U));
                char t[FT8_MODO_LARGO + 4];
                uint32_t j;

                for (j = 0U; l[j] != '\0' && j < sizeof t - 1U; j++) { t[j] = (l[j] == '\t') ? '|' : l[j]; }
                t[j] = '\0';
                printf("  %s\n", t);
            }
            total_visto = ft8_modo_total();
        }
        {
            uint8_t seg = ft8_modo_segundo();

            if (seg >= 5U && seg_ant < 5U) {
                int16_t lo = 0, hi = 0;

                ranura++;
                ft8_modo_db(&lo, &hi);
                printf("ranura %u: %u candidatos, %u mensajes, %u perdidos, %u cortas, DT %d ms, "
                       "ajuste %u ms + %u s, dB %d..%d\n",
                       ranura, (unsigned)ft8_modo_candidatos(), (unsigned)ft8_modo_ultima(),
                       (unsigned)ft8_modo_perdidos(), (unsigned)ft8_modo_cortas(),
                       (int)ft8_modo_dt_ms(), (unsigned)ft8_modo_ajuste_ms(),
                       (unsigned)ft8_modo_desfase(), (int)lo, (int)hi);
            }
            seg_ant = seg;
        }
        if (got < BLOQUE && ft8_modo_segundo() == 6U) { break; }
    }
    fclose(f);
    printf("%u mensajes en total\n", (unsigned)ft8_modo_total());
    ft8_modo_stop();
    return 0;
}
