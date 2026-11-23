/*
 * IDENT COMO LO LLEVA LA RADIO, CONTRA CUALQUIER FICHERO - 23/11/2026, port
 * del Malahit.
 *
 * El modo de verdad -ident.c con el motor de stanag_det.c- alimentado como
 * lo alimenta demod_am.c, bloques de 32 muestras a 12 kHz, con
 * ident_modo_frec_pon() e ident_modo_poll() cada 16 ms de audio, una vuelta
 * del bucle. Al final, los renglones del panel. Los ficheros salen de
 * malahit/tools/hackrf_gen.py <modo> --audio.
 *
 *   ./out/identradio f.f32 [kHz]       (IDENT_TRAZA=1: los renglones cada 5 s)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ident.h"

volatile uint32_t g_msticks = 0;

static uint8_t g_prestada[57344];         /* src/prestamo.c: 56 kB */
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
    uint32_t khz = (argc > 2) ? (uint32_t)strtoul(argv[2], 0, 10) : 14109U;
    uint8_t i, n;
    int traza = (getenv("IDENT_TRAZA") != 0);

    if (argc < 2) { printf("uso: identradio f.f32 [kHz]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    if (!ident_modo_start()) { printf("ident_modo_start() no arranca\n"); return 1; }
    while (fread(buf32, sizeof buf32[0], 32U, f) == 32U) {
        ident_modo_mete(buf32, 32U);
        bloques++;
        g_msticks = (uint32_t)(bloques * 32UL * 1000UL / 12000UL);
        if ((bloques % 6UL) == 0UL) { ident_modo_frec_pon(khz); ident_modo_poll(); }
        if (traza && (bloques % 1875UL) == 0UL) {
            printf("%5.1f s:", (double)bloques * 32.0 / 12000.0);
            n = ident_modo_lineas();
            for (i = 0; i < n && i < 2U; i++) { printf(" | %s", ident_modo_linea(i)); }
            printf("\n");
        }
    }
    fclose(f);
    n = ident_modo_lineas();
    for (i = 0; i < n; i++) { printf("  %s\n", ident_modo_linea(i)); }
    ident_modo_stop();
    return 0;
}
