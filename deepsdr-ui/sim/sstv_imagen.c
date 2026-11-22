/*
 * LA FOTO QUE SACA EL SSTV, A UN FICHERO - 22/11/2026, port del Malahit.
 *
 * sim/sstvaire.c cuenta lineas y pasos del VIS; esto guarda lo que se ve:
 * sstv.c de verdad, en bloques de 32 muestras como lo alimenta demod_am.c,
 * con la busqueda automatica puesta como la deja la radio, y cada linea que
 * entrega a un PPM de SSTV_ANCHO x SSTV_ALTO. Los ficheros salen de
 * malahit/tools/hackrf_gen.py sstv --audio.
 *
 *   ./out/sstvimagen f.f32 salida.ppm
 */
#include "sstv.h"
#include <stdio.h>
#include <string.h>

static unsigned char g_pozo[0x4000];
static int g_cogida;
void hfdl_ram_coge(void) { g_cogida = 1; }
unsigned char *hfdl_ram_todo(unsigned *cap)
{
    if (!g_cogida) { if (cap) { *cap = 0U; } return 0; }
    if (cap) { *cap = (unsigned)sizeof g_pozo; }
    return g_pozo;
}
void hfdl_ram_suelta(void) { g_cogida = 0; }

int main(int argc, char **argv)
{
    static uint8_t img[SSTV_ALTO][SSTV_ANCHO * 3U];
    float buf[32];
    FILE *f, *o;
    unsigned lineas = 0U;
    sstv_info_t in;

    if (argc < 3) { printf("uso: sstvimagen f.f32 salida.ppm\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    sstv_start(12000.0f);
    if (!sstv_activo()) { printf("sstv_start() no arranca: el pozo es pequeño\n"); return 1; }
    sstv_auto(1U);
    sstv_busca();
    while (fread(buf, sizeof buf[0], 32U, f) == 32U) {
        const uint8_t *l;
        uint16_t y;

        sstv_process(buf, 32U);
        while ((l = sstv_linea(&y)) != 0) {
            if (y < SSTV_ALTO) { memcpy(img[y], l, SSTV_ANCHO * 3U); lineas++; }
        }
    }
    fclose(f);
    sstv_info(&in);
    printf("modo %s (VIS %u, %s), %u lineas, imagenes %u\n",
           in.modo ? in.modo : "-", (unsigned)in.vis,
           in.como == SSTV_COMO_VIS ? "por la cabecera" : in.como == SSTV_COMO_RITMO
           ? "por el ritmo" : in.como == SSTV_COMO_MANO ? "a mano" : "-",
           lineas, (unsigned)in.imagenes);
    o = fopen(argv[2], "wb");
    if (!o) { return 2; }
    fprintf(o, "P6\n%u %u\n255\n", (unsigned)SSTV_ANCHO, (unsigned)SSTV_ALTO);
    fwrite(img, SSTV_ANCHO * 3U, SSTV_ALTO, o);
    fclose(o);
    sstv_stop();
    return 0;
}
