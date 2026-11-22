/*
 * EL RTTY AJUSTANDOSE SOLO, CONTRA CUALQUIER FICHERO - 22/11/2026.
 *
 * Lo mismo que sim/rtty_solo.c -rtty.c, rtty_scope.c y rtty_auto.c de
 * verdad, el oscilador alimentado con el mismo audio, rtty_auto_poll()
 * cada doce bloques como lo llama el bucle de la radio- pero con el
 * fichero Y el punto de partida en la linea de ordenes, para probar lo que
 * la grabacion de rtty_solo no trae: otra velocidad, otro desplazamiento,
 * la polaridad contraria, solo ruido.
 *
 * Los ficheros salen de malahit/tools/hackrf_gen.py con --audio (el audio
 * que la rama de banda lateral le da al decodificador: f32, 12 kHz), que es
 * la misma señal que el HackRF le mete a la radio en el banco del port.
 *
 *   ./out/rttyautoaire f.f32 marca espacio baudios [auto 0/1]
 *
 * Saca el texto, y una linea por ventana de salud con lo que el ajuste
 * automatico ha dejado puesto.
 */
#include "rtty.h"
#include "rtty_auto.h"
#include "rtty_scope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE *f;
    float buf[RTTY_BLOCK_SAMPLES];
    static char t[16384];
    size_t nt = 0U;
    unsigned bloques = 0U;
    uint16_t vista = 0U;
    char c;
    float mark, space, baud;
    unsigned i;

    if (argc < 5) {
        printf("uso: rttyautoaire f.f32 marca espacio baudios [auto 0/1]\n");
        return 2;
    }
    mark  = (float)atof(argv[2]);
    space = (float)atof(argv[3]);
    baud  = (float)atof(argv[4]);
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }

    rtty_init();
    rtty_scope_init();
    rtty_auto_reset();
    rtty_auto_set((uint8_t)(argc > 5 ? atoi(argv[5]) != 0 : 1));
    rtty_set_mark_space_hz(mark, space);
    rtty_set_baud(baud);
    rtty_set_enabled(1U);

    while (fread(buf, sizeof buf[0], RTTY_BLOCK_SAMPLES, f) == RTTY_BLOCK_SAMPLES) {
        rtty_process(buf, RTTY_BLOCK_SAMPLES);
        rtty_scope_feed(buf, RTTY_BLOCK_SAMPLES);
        if ((++bloques % 12U) == 0U) {
            rtty_scope_poll();
            rtty_auto_poll();
            if (vista != rtty_salud_n()) {
                vista = rtty_salud_n();
                printf("  %6.1f s  salud %3u%%  marca %5.0f  espacio %5.0f  %6.2f Bd\n",
                       (double)bloques * RTTY_BLOCK_SAMPLES / 12000.0,
                       (unsigned)rtty_salud_pc(), (double)rtty_get_mark_hz(),
                       (double)rtty_get_space_hz(), (double)rtty_get_baud());
            }
        }
        while (rtty_get_char(&c)) { if (nt < sizeof t - 1U) { t[nt++] = c; } }
    }
    fclose(f);
    t[nt] = '\0';
    printf("  acaba en marca %.0f espacio %.0f %.2f Bd (tonos movidos %u, "
           "velocidad %u, vueltas %u)\n  ", (double)rtty_get_mark_hz(),
           (double)rtty_get_space_hz(), (double)rtty_get_baud(),
           (unsigned)rtty_auto_tonos_veces(), (unsigned)rtty_auto_baud_veces(),
           (unsigned)rtty_auto_inv_veces());
    {
        uint16_t p[4];
        rtty_auto_puntos(p);
        printf("velocidad medida con cada una puesta: 45,45 -> %.1f  50 -> %.1f  "
               "75 -> %.1f  100 -> %.1f\n  ", p[0] / 10.0, p[1] / 10.0, p[2] / 10.0,
               p[3] / 10.0);
    }
    for (i = 0U; i < nt; i++) {
        char q = t[i];
        putchar((q == '\r' || q == '\n') ? ' ' : ((q >= 32 && q < 127) ? q : '.'));
    }
    printf("\n");
    return 0;
}
