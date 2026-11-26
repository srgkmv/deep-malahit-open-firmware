/*
 * HFDL COMO LO LLEVA LA RADIO, CONTRA CUALQUIER FICHERO - 26/11/2026, port del
 * Malahit (paso 59).
 *
 * El modo de verdad -hfdl_modo.c y su cadena, la memoria prestada de upstream
 * (hfdl_ram.c sobre la union)- alimentado con I/Q a 12 kHz (f32 intercalado,
 * la subportadora en +1.440 Hz, como el gancho de demod_am.c), a bloques de
 * 32 muestras, con hfdl_modo_poll() tras cada bloque. Ruido complejo
 * opcional: sigma por componente. Al final, tramas buenas / vistas y la
 * tabla.
 *
 *   ./out/hfdlradio iq.c32 [sigma] [escala]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "hfdl_modo.h"

bool icao24_db_lookup(uint32_t icao24, char *out, uint32_t out_len)
{
    (void)icao24;
    if (out && out_len) { out[0] = '\0'; }
    return false;
}

static uint32_t s_sem = 12345u;
static float gauss(void)
{
    float u1, u2;
    s_sem = s_sem * 1103515245u + 12345u; u1 = ((float)((s_sem >> 8) & 0xFFFFFFu) + 1.0f) / 16777217.0f;
    s_sem = s_sem * 1103515245u + 12345u; u2 = ((float)((s_sem >> 8) & 0xFFFFFFu) + 1.0f) / 16777217.0f;
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

int main(int argc, char **argv)
{
    FILE *f;
    float buf[64], bi[32], bq[32];
    float sigma = (argc > 2) ? (float)atof(argv[2]) : 0.0f;
    float esc = (argc > 3) ? (float)atof(argv[3]) : 1.0f;
    uint32_t i;

    if (argc < 2) { printf("uso: hfdlradio iq.c32 [sigma] [escala]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    if (!hfdl_modo_start()) { printf("hfdl_modo_start() no arranca\n"); return 1; }
    while (fread(buf, sizeof buf[0], 64U, f) == 64U) {
        for (i = 0U; i < 32U; i++) {
            bi[i] = (buf[2U * i] + gauss() * sigma) * esc;
            bq[i] = (buf[2U * i + 1U] + gauss() * sigma) * esc;
        }
        hfdl_modo_mete(bi, bq, 32U);
        hfdl_modo_poll();
    }
    fclose(f);
    for (i = 0U; i < 200U; i++) { hfdl_modo_poll(); }
    printf("%u tramas buenas de %u, %u renglones\n", (unsigned)hfdl_modo_buenas(),
           (unsigned)hfdl_modo_total(), (unsigned)hfdl_modo_lineas());
    for (i = 0U; i < hfdl_modo_lineas() && i < 4U; i++) {
        const char *l = hfdl_modo_linea(i);
        char t[96];
        uint32_t k;

        for (k = 0U; l[k] != '\0' && k < sizeof t - 1U; k++) { t[k] = (l[k] == '\t') ? '|' : l[k]; }
        t[k] = '\0';
        printf("  %s\n", t);
    }
    hfdl_modo_stop();
    return 0;
}
