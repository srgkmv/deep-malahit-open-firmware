/*
 * AX.25 COMO LO LLEVA LA RADIO, CONTRA CUALQUIER FICHERO - 23/11/2026, port
 * del Malahit.
 *
 * User/ax25.c alimentado a bloques de 32 muestras, ax25_get_char() como lo
 * vacia el bucle de la radio. APRS se arranca con la tasa del fichero (en la
 * radio es la del discriminador de FM; ax25_start() acepta cualquiera),
 * PKT300 con lo de ax25.h. Los ficheros, de malahit/tools/hackrf_gen.py
 * ax25 --audio (12 kHz). En PKT300, como en la radio, el osciloscopio de
 * sintonia (rtty_scope.c) come el mismo audio y cada cuadro va a
 * ax25_espectro(), que le dice al CAF si hay señal.
 *
 *   ./out/ax25radio f.f32 [aprs|pkt300]
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ax25.h"
#include "rtty_scope.h"

int main(int argc, char **argv)
{
    FILE *f;
    float buf32[32];
    char c;
    ax25_info_t in;

    if (argc < 2) { printf("uso: ax25radio f.f32 [aprs|pkt300]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    int hf = (argc > 2 && strcmp(argv[2], "pkt300") == 0);

    if (hf) {
        ax25_start_fsk(PKT_HF_FS_HZ, PKT_HF_BAUD, PKT_HF_MARCA_HZ, PKT_HF_ESPACIO_HZ);
    } else {
        ax25_start(12000.0f);
    }
    rtty_scope_init();
    while (fread(buf32, sizeof buf32[0], 32U, f) == 32U) {
        ax25_process(buf32, 32U);
        if (hf) {
            rtty_scope_feed(buf32, 32U);
            rtty_scope_poll();
            ax25_espectro(rtty_scope_frame_peek(), (uint16_t)RTTY_SCOPE_BINS,
                          rtty_scope_hz_per_bin(), rtty_scope_frame_n());
        }
        while (ax25_get_char(&c)) { putchar((c == '\r') ? '\n' : c); }
    }
    fclose(f);
    ax25_info(&in);
    printf("\n%u tramas buenas, %u malas, %u banderas, CAF %+.0f Hz\n",
           (unsigned)in.tramas_ok, (unsigned)in.tramas_mal, (unsigned)in.banderas,
           (double)ax25_caf_hz());
    return 0;
}
