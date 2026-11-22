/*
 * EL FAX CONTRA CUALQUIER FICHERO - 22/11/2026, port del Malahit.
 *
 * wefax.c de verdad, alimentado en bloques de 32 muestras como lo alimenta
 * demod_am.c, con el fichero en la linea de ordenes. Los ficheros salen de
 * malahit/tools/hackrf_gen.py wefax --audio (la misma señal que el HackRF le
 * mete a la radio), con su propia carta de prueba.
 *
 *   ./out/wefaxaire f.f32 salida.pgm [lpm]
 *
 * Saca cada cambio de estado con su segundo, y las lineas recibidas en un
 * PGM de WEFAX_ANCHO puntos de ancho: lo que el decodificador entrega, antes
 * de que la pantalla de nadie lo reduzca.
 */
#include "wefax.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char g_pozo[0xD4A0];
static int g_cogida;
void hfdl_ram_coge(void) { g_cogida = 1; }
unsigned char *hfdl_ram_todo(unsigned *cap)
{
    if (!g_cogida) { if (cap) { *cap = 0U; } return 0; }
    if (cap) { *cap = (unsigned)sizeof g_pozo; }
    return g_pozo;
}
void hfdl_ram_suelta(void) { g_cogida = 0; }

#define MAX_LINEAS 4000U

int main(int argc, char **argv)
{
    static uint8_t img[MAX_LINEAS][WEFAX_ANCHO];
    float buf[32];
    FILE *f, *o;
    uint32_t n = 0U;
    unsigned long bloques = 0UL;
    uint8_t est_visto = 0xFFU;
    wefax_info_t in;

    if (argc < 3) { printf("uso: wefaxaire f.f32 salida.pgm [lpm]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    wefax_start(12000.0f);
    if (argc > 3) { wefax_set_lpm((uint16_t)atoi(argv[3])); }

    while (fread(buf, sizeof buf[0], 32U, f) == 32U) {
        wefax_process(buf, 32U);
        bloques++;
        while (n < MAX_LINEAS && wefax_linea_en(img[n])) { n++; }
        if ((bloques % 375UL) == 0UL) {
            wefax_info(&in);
            if (in.estado != est_visto) {
                static const char *const k[] = { "parado", "busca", "fase", "imagen" };
                printf("  %5.1f s: %s (lpm %u, ppm %+.0f, lineas %u)\n",
                       (double)bloques * 32.0 / 12000.0,
                       in.estado < 4U ? k[in.estado] : "?", (unsigned)in.lpm,
                       (double)in.ppm, (unsigned)in.lineas);
                est_visto = in.estado;
            }
        }
    }
    fclose(f);
    wefax_info(&in);
    printf("fin: %u lineas entregadas, ppm %+.0f, fase vista %u lineas\n",
           (unsigned)n, (double)in.ppm, (unsigned)in.fase_n);
    o = fopen(argv[2], "wb");
    if (!o) { printf("no puedo escribir %s\n", argv[2]); return 2; }
    fprintf(o, "P5\n%u %u\n255\n", (unsigned)WEFAX_ANCHO, (unsigned)n);
    fwrite(img, WEFAX_ANCHO, n, o);
    fclose(o);
    wefax_stop();
    return 0;
}
