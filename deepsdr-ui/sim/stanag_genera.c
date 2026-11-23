/*
 * UNA EMISION DE STANAG 4285 A UN FICHERO - 23/11/2026, port del Malahit.
 *
 * El transmisor de sim/stanag_tx.h -el inverso exacto de User/stanag_rx.c,
 * validado ida y vuelta contra la grabacion del aire (ver su cabecera)-
 * escribiendo audio f32 a 12 kHz, la portadora en 1800 Hz, para que
 * malahit/tools/hackrf_gen.py audio lo mande a la radio como banda lateral.
 * El mensaje se repite, como hace una estacion; delante, un segundo de
 * silencio.
 *
 *   ./out/stanaggenera f.f32 <75|150|300|600|1200|2400> <S|L> <segundos> ["texto"]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stanag_tx.h"

static stx_t   g_t;
static uint8_t g_bits[1U << 16];
static float   g_aud[12000U * 600U];

int main(int argc, char **argv)
{
    static const int k_vel[6] = { 75, 150, 300, 600, 1200, 2400 };
    const char *txt = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 DE R1TEST\r\n";
    int vel = -1, largo, i;
    uint32_t nbits, n, lead = 12000U, max;
    double seg;
    FILE *f;

    if (argc < 5) {
        printf("uso: stanaggenera f.f32 <75|150|300|600|1200|2400> <S|L> <segundos> [\"texto\"]\n");
        return 2;
    }
    for (i = 0; i < 6; i++) { if (atoi(argv[2]) == k_vel[i]) { vel = i; } }
    if (vel < 0) { printf("velocidad %s: no es de la norma\n", argv[2]); return 2; }
    largo = (argv[3][0] == 'L' || argv[3][0] == 'l');
    seg = atof(argv[4]);
    if (argc > 5) { txt = argv[5]; }
    max = (uint32_t)(seg * 12000.0);
    if (max + lead > sizeof g_aud / sizeof g_aud[0]) { printf("demasiado largo\n"); return 2; }

    stx_init(&g_t, vel, largo);
    nbits = stx_mensaje(&g_t, txt, g_bits, (uint32_t)sizeof g_bits);
    memset(g_aud, 0, lead * sizeof g_aud[0]);
    n = stx_audio(&g_t, g_bits, nbits, &g_aud[lead], max);
    f = fopen(argv[1], "wb");
    if (!f) { printf("no puedo escribir %s\n", argv[1]); return 2; }
    fwrite(g_aud, sizeof g_aud[0], lead + n, f);
    fclose(f);
    printf("%s: STANAG 4285 %d bps %s, %.1f s, el mensaje de %u bits de FEC\n",
           argv[1], k_vel[vel], largo ? "largo" : "corto",
           (double)(lead + n) / 12000.0, (unsigned)nbits);
    return 0;
}
