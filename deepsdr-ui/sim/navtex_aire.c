/*
 * EL NAVTEX CONTRA CUALQUIER FICHERO - 22/11/2026.
 *
 * navtex.c de verdad, alimentado en bloques de 32 muestras como lo
 * alimenta demod_am.c, con el fichero y el punto de partida en la linea de
 * ordenes. Lo que sim/navtextest.c no prueba: una señal que NO sale de la
 * misma tabla que la lee - la fabrica malahit/tools/hackrf_gen.py navtex
 * --audio con su propia tabla del CCIR 476 (f32, 12 kHz), que es la misma
 * señal que el HackRF le mete a la radio en el banco del port.
 *
 *   ./out/navtexaire f.f32 [centro_hz [invertido 0/1 [espectro 0/1]]]
 *
 * Saca el texto y, cada segundo que cambia algo, en que fase va el
 * decodificador -lo que la chapa de la radio enseña- y donde escucha.
 *
 * Con espectro 1 (lo de siempre) hace lo que el bucle de la radio: el
 * osciloscopio de sintonia (rtty_scope.c) comiendo el mismo audio y cada
 * cuadro suyo a navtex_espectro(), que es quien mide el centro. Con 0, el
 * navtex.c de antes del 22/11/2026: el centro por cruces por cero.
 */
#include "navtex.h"
#include "rtty_scope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE *f;
    float buf[NAVTEX_BLOCK_SAMPLES];
    static char t[16384];
    size_t nt = 0U;
    unsigned long bloques = 0UL;
    char c, fase_vista[16] = "";
    int  desvio_visto = 0;
    uint8_t espectro = 1U;
    float nota_max = 0.0f, ph_visto = -1.0f;
    navtex_info_t in;

    if (argc < 2) {
        printf("uso: navtexaire f.f32 [centro_hz [invertido 0/1 [espectro 0/1]]]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }

    if (argc > 2) { navtex_set_centro_hz((float)atof(argv[2])); }   /* antes: no fija */
    if (argc > 3) { navtex_set_invertido((uint8_t)(atoi(argv[3]) != 0)); }
    if (argc > 4) { espectro = (uint8_t)(atoi(argv[4]) != 0); }
    rtty_scope_init();
    navtex_start(12000.0f);

    while (fread(buf, sizeof buf[0], NAVTEX_BLOCK_SAMPLES, f) == NAVTEX_BLOCK_SAMPLES) {
        navtex_process(buf, NAVTEX_BLOCK_SAMPLES);
        if (espectro) {
            rtty_scope_feed(buf, NAVTEX_BLOCK_SAMPLES);
            rtty_scope_poll();
            navtex_espectro(rtty_scope_frame_peek(), (uint16_t)RTTY_SCOPE_BINS,
                            rtty_scope_hz_per_bin(), rtty_scope_frame_n());
        }
        while (navtex_get_char(&c)) {
            if (nt < sizeof t - 1U) { t[nt++] = c; }
        }
        {   /* la nota mas alta que ha sacado la busqueda en el espectro */
            float ph = 0.0f, nota = 0.0f;
            navtex_espectro_dbg(&ph, &nota);
            if (nota > nota_max) { nota_max = nota; }
            if (getenv("NAVTEX_TRAZA") && ph != ph_visto) {
                printf("    %6.2f s: pareja %.0f Hz, nota %.2f\n",
                       (double)bloques * 32.0 / 12000.0, (double)ph, (double)nota);
                ph_visto = ph;
            }
        }
        if ((++bloques % 375UL) == 0UL) {       /* un segundo */
            const char *fase;

            navtex_info(&in);
            fase = in.tiene_cab ? in.cab : in.car_sync ? "SITOR"
                 : in.bit_sync ? "bits" : "buscando";
            if (strcmp(fase, fase_vista) != 0 || in.desvio_hz != desvio_visto) {
                printf("  %4lu s: %-8s calidad %.2f, centro %+d Hz, "
                       "buenos %u malos %u rescatados %u\n",
                       bloques / 375UL, fase, (double)in.calidad, (int)in.desvio_hz,
                       (unsigned)in.car_ok, (unsigned)in.car_mal,
                       (unsigned)in.rescatados);
                strncpy(fase_vista, fase, sizeof fase_vista - 1U);
                desvio_visto = in.desvio_hz;
            }
        }
    }
    fclose(f);
    t[nt] = '\0';
    navtex_info(&in);
    printf("texto (%u caracteres):\n%s\n", (unsigned)nt, t);
    printf("fin: cabecera %s, mensajes cerrados %u, calidad %.2f, buenos %u "
           "malos %u rescatados %u, centro %+d Hz\n",
           in.tiene_cab ? in.cab : "-", (unsigned)in.mensajes, (double)in.calidad,
           (unsigned)in.car_ok, (unsigned)in.car_mal, (unsigned)in.rescatados,
           (int)in.desvio_hz);
    printf("la pareja mas clara que vio el espectro: nota %.2f (pasa con 2,00)\n",
           (double)nota_max);
    printf("veces que paso a otro desplazamiento de bit: %u\n", (unsigned)navtex_recolocados());
    return 0;
}
