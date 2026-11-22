/*
 * EL FELD-HELL CONTRA CUALQUIER FICHERO - 22/11/2026, port del Malahit.
 *
 * hell.c de verdad, en bloques de 32 muestras como lo alimenta demod_am.c,
 * y cada columna que entrega a un PGM como la pinta la pantalla: las dos
 * copias una encima de otra, en tiras de 240 columnas. Los ficheros salen
 * de malahit/tools/hackrf_gen.py hell --audio.
 *
 *   ./out/hellaire f.f32 salida.pgm
 */
#include "hell.h"
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

#define COLS   240U
#define TIRAS  8U
#define TIRA_H (2U * HELL_ALTO + 4U)

int main(int argc, char **argv)
{
    static uint8_t img[TIRAS * TIRA_H][COLS];
    float buf[32];
    FILE *f, *o;
    unsigned n = 0U;
    hell_info_t in;

    if (argc < 3) { printf("uso: hellaire f.f32 salida.pgm\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    hell_start(12000.0f);
    if (!hell_activo()) { printf("hell_start() no arranca: el pozo es pequeño\n"); return 1; }
    while (fread(buf, sizeof buf[0], 32U, f) == 32U) {
        const uint8_t *c;

        hell_process(buf, 32U);
        while ((c = hell_columna()) != 0) {
            unsigned t = (n / COLS) % TIRAS, x = n % COLS, k;
            for (k = 0U; k < 2U * HELL_ALTO; k++) {
                img[t * TIRA_H + k][x] = c[k % HELL_ALTO];
            }
            n++;
        }
    }
    fclose(f);
    hell_info(&in);
    printf("%u columnas (%u caracteres de 7)\n", n, n / 7U);
    o = fopen(argv[2], "wb");
    if (!o) { return 2; }
    fprintf(o, "P5\n%u %u\n255\n", COLS, TIRAS * TIRA_H);
    fwrite(img, COLS, TIRAS * TIRA_H, o);
    fclose(o);
    hell_stop();
    return 0;
}
