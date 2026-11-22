/*
 * EL DSC COMO LO LLEVA LA RADIO - 22/11/2026, port del Malahit.
 *
 * sim/dscaire.c mete el audio por dsc.c y nada mas. En la radio hay dos
 * cosas mas, y las dos mueven el centro de los tonos: el osciloscopio de
 * sintonia (rtty_scope.c) comiendo el mismo audio, y dsc_poll() desde el
 * bucle principal buscando en el la pareja de tonos mientras no hay
 * enganche. Aqui estan las dos, en el orden de la radio: un bloque de 32
 * muestras a dsc_process() y al osciloscopio, y despues el bucle.
 *
 *   ./out/dscradio f.f32 [centro_hz]
 *
 * Saca cada llamada cuando llega (con su segundo) y cada vez que el centro
 * se mueve, que es lo que hay que vigilar con ruido entre llamada y
 * llamada. Los ficheros salen de malahit/tools/hackrf_gen.py dsc --audio.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dsc.h"
#include "rtty_scope.h"

/* La RAM prestada, como en dscaire.c. */
static unsigned g_cog, g_sue;
static unsigned char g_pozo[0xD4A0];
void      hfdl_ram_coge(void) { g_cog++; }
unsigned char *hfdl_ram_todo(unsigned *cap) { if (cap) { *cap = (unsigned)sizeof g_pozo; } return g_pozo; }
void      hfdl_ram_suelta(void) { g_sue++; }

int main(int argc, char **argv)
{
    FILE *f;
    float buf[32];
    unsigned long bloques = 0UL, vistas = 0UL;
    unsigned movidas = 0U;
    float centro_visto = 0.0f;
    uint8_t eng_visto = 0xFFU;
    dsc_info_t in;

    if (argc < 2) { printf("uso: dscradio f.f32 [centro_hz]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }

    rtty_scope_init();
    dsc_start(12000.0f);
    if (argc > 2) { dsc_set_centro_hz((float)atof(argv[2])); }
    centro_visto = dsc_get_centro_hz();

    while (fread(buf, sizeof buf[0], 32U, f) == 32U) {
        double t;
        float c;

        dsc_process(buf, 32U);
        rtty_scope_feed(buf, 32U);
        bloques++;
        t = (double)bloques * 32.0 / 12000.0;
        /* El bucle de la radio da una vuelta cada ~40 ms: cada 15 bloques. */
        if ((bloques % 15UL) != 0UL) { continue; }
        rtty_scope_poll();
        dsc_poll();

        c = dsc_get_centro_hz();
        dsc_info(&in);
        if (c != centro_visto) {
            printf("  %6.2f s: centro %4.0f Hz\n", t, (double)c);
            centro_visto = c;
            movidas++;
        }
        if (in.enganchado != eng_visto) {
            printf("  %6.2f s: %s\n", t, in.enganchado ? "enganchado" : "buscando");
            eng_visto = in.enganchado;
        }
        if ((unsigned long)(in.llamadas + in.malas) != vistas) {
            uint8_t i;
            vistas = (unsigned long)(in.llamadas + in.malas);
            printf("  %6.2f s: LLAMADA\n", t);
            for (i = 0U; i < dsc_lineas(); i++) { printf("           %s\n", dsc_linea(i)); }
        }
    }
    fclose(f);
    dsc_info(&in);
    printf("fin: llamadas ok %lu, con fallo %lu, el centro se movio %u veces\n",
           (unsigned long)in.llamadas, (unsigned long)in.malas, movidas);
    dsc_stop();
    return 0;
}
