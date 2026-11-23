/*
 * ALE COMO LO LLEVA LA RADIO, CONTRA CUALQUIER FICHERO - 22/11/2026, port
 * del Malahit.
 *
 * El modo de verdad -ale_modo.c, que junta las palabras en direcciones, con
 * su ale_rx.c y ale_golay.c- alimentado como lo alimenta demod_am.c, bloques
 * de 32 muestras a 12 kHz, y ale_modo_poll() cada 16 ms de audio, que es el
 * paso al que lo llama la radio (src/digi.c, digi_ale_poll(): el cierre por
 * silencio de ale_modo.c cuenta llamadas, y upstream las da cada 16 ms). Al
 * final, la tabla de estaciones. Los ficheros salen de malahit/tools/
 * hackrf_gen.py ale --audio, escrito desde el estandar y no desde ale_*.c.
 *
 *   ./out/aleradio f.f32 [kHz]        (ALE_TRAZA=1: cada movimiento del CAF)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ale_modo.h"

static uint8_t g_prestada[8192];
static uint8_t g_cogida = 0;
uint8_t *hfdl_ram_todo(uint32_t *cap)
{
    if (!g_cogida) { if (cap) { *cap = 0U; } return 0; }
    if (cap) { *cap = (uint32_t)sizeof g_prestada; }
    return g_prestada;
}
void hfdl_ram_coge(void)   { g_cogida = 1; }
void hfdl_ram_suelta(void) { g_cogida = 0; }
uint8_t waterfall_prestada(void) { return 0U; }

int main(int argc, char **argv)
{
    FILE *f;
    float buf32[32];
    unsigned long bloques = 0UL;
    uint32_t i, n, vistas = 0U;
    int traza = (getenv("ALE_TRAZA") != 0);   /* ALE_TRAZA=1: cada movimiento del CAF */

    if (argc < 2) { printf("uso: aleradio f.f32 [kHz]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    if (!ale_modo_start()) { printf("ale_modo_start() no arranca\n"); return 1; }
    ale_modo_frec_pon(argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 14109U);
    while (fread(buf32, sizeof buf32[0], 32U, f) == 32U) {
        ale_modo_mete(buf32, 32U);
        bloques++;
        if ((bloques % 6UL) == 0UL) {            /* 6 x 32 muestras = 16 ms */
            static int16_t caf_visto = 0;

            if (traza && ale_modo_afc_hz() != caf_visto) {
                caf_visto = ale_modo_afc_hz();
                printf("  %6.2f s  CAF %+d Hz\n", (double)bloques * 32.0 / 12000.0, (int)caf_visto);
            }
            ale_modo_poll();
            if (ale_modo_palabras() != vistas) {
                vistas = ale_modo_palabras();
                printf("  %6.2f s  %u palabras, calidad %u/48, %s, CAF %+d Hz\n",
                       (double)bloques * 32.0 / 12000.0, vistas,
                       ale_modo_calidad(), ale_modo_enganchado() ? "enganchado" : "suelto",
                       (int)ale_modo_afc_hz());
            }
        }
    }
    for (i = 0U; i < 400U; i++) { ale_modo_poll(); }   /* el cierre por silencio */
    fclose(f);
    n = ale_modo_lineas();
    printf("%u palabras, %u estaciones, CAF %+d Hz\n", ale_modo_palabras(),
           (uint32_t)ale_modo_estaciones(), (int)ale_modo_afc_hz());
    for (i = 0U; i < n; i++) {
        const char *l = ale_modo_linea(i);
        char t[ALE_MODO_LARGO];
        uint32_t k;

        for (k = 0U; l[k] != '\0' && k < sizeof t - 1U; k++) { t[k] = (l[k] == '\t') ? '|' : l[k]; }
        t[k] = '\0';
        printf("  %s\n", t);
    }
    ale_modo_stop();
    return 0;
}
