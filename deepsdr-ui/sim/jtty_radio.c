/*
 * JTTY COMO LO LLEVA LA RADIO, CONTRA CUALQUIER FICHERO - 22/11/2026, port
 * del Malahit.
 *
 * El modo de verdad -jtty_modo.c, que junta los atomos en mensajes, con su
 * jtty_rx.c, jtty_fec.c, jtty_msg.c y dxcc.c- alimentado como lo alimenta
 * demod_am.c, bloques de 32 muestras a 12 kHz, y jtty_modo_poll() cada
 * quince bloques, una vuelta del bucle de la radio. Al final, los renglones
 * del panel. Los ficheros salen de malahit/tools/hackrf_gen.py jtty --audio,
 * con los simbolos de los vectores normativos de WSJT-X.
 *
 *   ./out/jttyradio f.f32
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "jtty_modo.h"
#include "rtc_hw.h"

volatile uint32_t g_msticks = 0;

static uint8_t g_prestada[54432];
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
void rtc_hw_get(rtc_hw_datetime_t *out)
{
    if (out) { memset(out, 0, sizeof *out); out->hour = 0U; out->minute = (uint8_t)(g_msticks / 60000U); }
}

int main(int argc, char **argv)
{
    FILE *f;
    float buf32[32];
    unsigned long bloques = 0UL;
    uint8_t i;

    if (argc < 2) { printf("uso: jttyradio f.f32\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    if (!jtty_modo_start()) { printf("jtty_modo_start() no arranca\n"); return 1; }
    while (fread(buf32, sizeof buf32[0], 32U, f) == 32U) {
        jtty_modo_mete(buf32, 32U);
        bloques++;
        g_msticks = (uint32_t)(bloques * 32UL * 1000UL / 12000UL);
        if ((bloques % 15UL) == 0UL) { jtty_modo_poll(); }
    }
    for (i = 0U; i < 200U; i++) { jtty_modo_poll(); }   /* lo que quede pendiente */
    fclose(f);
    printf("%lu atomos, estado \"%s\"\n", (unsigned long)jtty_modo_atomos(),
           jtty_modo_estado_txt());
    for (i = 0U; i < jtty_modo_lineas(); i++) {
        const char *l = jtty_modo_linea(i);
        char t[160];
        uint8_t k;

        for (k = 0U; l[k] != '\0' && k < sizeof t - 1U; k++) { t[k] = (l[k] == '\t') ? '|' : l[k]; }
        t[k] = '\0';
        printf("  %s\n", t);
    }
    jtty_modo_stop();
    return 0;
}
