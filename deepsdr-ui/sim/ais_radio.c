/*
 * AIS COMO LO LLEVA LA RADIO, CONTRA CUALQUIER FICHERO - 24/11/2026, port del
 * Malahit.
 *
 * El modo de verdad -ais_modo.c con ais_rx.c y ais_msg.c- alimentado con la
 * salida del discriminador de FM (f32, radianes por muestra) a la tasa que
 * se le diga, a bloques, con ais_modo_poll() cada 16 ms. Al final, la tabla
 * de barcos y las cuentas.
 *
 *   ./out/aisradio disc.f32 [48000|96000]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ais_modo.h"

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
    float buf[64];
    float fs = (argc > 2) ? (float)atof(argv[2]) : 96000.0f;
    unsigned long n = 0UL, cada;
    uint32_t i;

    if (argc < 2) { printf("uso: aisradio disc.f32 [48000|96000]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    ais_modo_tasa_pon(fs);
    if (!ais_modo_start()) { printf("ais_modo_start() no arranca\n"); return 1; }
    cada = (unsigned long)(fs * 0.016f / 64.0f);
    if (cada == 0UL) { cada = 1UL; }
    while (fread(buf, sizeof buf[0], 64U, f) == 64U) {
        ais_modo_mete(buf, 64U);
        if ((++n % cada) == 0UL) { ais_modo_poll(); }
    }
    for (i = 0U; i < 50U; i++) { ais_modo_poll(); }
    fclose(f);
    printf("%u tramas buenas de %u, %u barcos\n", ais_modo_tramas(), ais_modo_vistas(),
           (unsigned)ais_modo_barcos());
    for (i = 0U; i < ais_modo_lineas(); i++) {
        const char *l = ais_modo_linea(i);
        char t[AIS_MODO_LARGO + 4];
        uint32_t k;

        for (k = 0U; l[k] != '\0' && k < sizeof t - 1U; k++) { t[k] = (l[k] == '\t') ? '|' : l[k]; }
        t[k] = '\0';
        printf("  %s\n", t);
    }
    ais_modo_stop();
    return 0;
}
