/*
 * STANAG COMO LO LLEVA LA RADIO, CONTRA CUALQUIER FICHERO - 23/11/2026,
 * port del Malahit.
 *
 * El modo de verdad -stanag_modo.c con stanag_det.c, stanag_rx.c y s4415.c-
 * alimentado como lo alimenta demod_am.c, bloques de 32 muestras a 12 kHz,
 * y stanag_modo_poll() cada 16 ms de audio, una vuelta del bucle. La forma
 * de onda, la velocidad y el entrelazado como los pondrian los botones. Al
 * final, los renglones del panel y la chapa.
 *
 *   ./out/stanagradio f.f32 [Auto|S4285|S4529|S4481|S4538|S4415] [vel] [S|L]
 *
 * vel: 75..2400, o AUTO (el boton de velocidad en su ultima posicion).
 * STANAG_TRAZA=1: el estado y los renglones de diagnostico cada segundo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "stanag_modo.h"
#include "stanag_rx.h"

volatile uint32_t g_msticks = 0;

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
uint32_t ui_bucle_peor_us(void)  { return 0U; }
uint32_t ui_polls_peor_us(void)  { return 0U; }
uint32_t ui_dibujo_peor_us(void) { return 0U; }

static void tabs(const char *l)
{
    char t[160];
    unsigned k;

    for (k = 0; l[k] != '\0' && k < sizeof t - 1U; k++) { t[k] = (l[k] == '\t') ? '|' : l[k]; }
    t[k] = '\0';
    printf("  %s\n", t);
}

int main(int argc, char **argv)
{
    static const char *const k_forma[STANAG_FORMAS] = { "Auto", "S4285", "S4529", "S4481", "S4538", "S4415" };
    static const int k_vel[6] = { 75, 150, 300, 600, 1200, 2400 };
    FILE *f;
    float buf32[32];
    unsigned long bloques = 0UL;
    uint8_t forma = STANAG_AUTO, i, n;
    int traza = (getenv("STANAG_TRAZA") != 0);   /* STANAG_TRAZA=1: el estado cada segundo */

    if (argc < 2) { printf("uso: stanagradio f.f32 [Auto|S4285|...] [vel|AUTO] [S|L]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("no puedo abrir %s\n", argv[1]); return 2; }
    if (argc > 2) {
        for (i = 0; i < STANAG_FORMAS; i++) { if (strcmp(argv[2], k_forma[i]) == 0) { forma = i; } }
    }
    stanag_modo_forma_pon(forma);
    if (!stanag_modo_start()) { printf("stanag_modo_start() no arranca\n"); return 1; }
    if (argc > 3) {
        if (strcmp(argv[3], "AUTO") == 0) { stanag_modo_vel_pon((uint8_t)(stanag_modo_vel_n() - 1U)); }
        else { for (i = 0; i < 6; i++) { if (atoi(argv[3]) == k_vel[i]) { stanag_modo_vel_pon(i); } } }
    }
    if (argc > 4) { stanag_modo_entre_pon((uint8_t)(argv[4][0] == 'L' || argv[4][0] == 'l')); }
    printf("forma %s, velocidad %s, entrelazado %s\n", stanag_modo_forma_txt(stanag_modo_forma()),
           stanag_modo_demod() ? stanag_modo_vel_txt() : "-", stanag_modo_demod() ? stanag_modo_entre_txt() : "-");
    while (fread(buf32, sizeof buf32[0], 32U, f) == 32U) {
        stanag_modo_mete(buf32, 32U);
        bloques++;
        g_msticks = (uint32_t)(bloques * 32UL * 1000UL / 12000UL);
        if ((bloques % 6UL) == 0UL) { stanag_modo_poll(); }
        if (traza && (bloques % 375UL) == 0UL) {          /* cada segundo */
            uint8_t k, nl = stanag_modo_lineas();

            printf("%5.1f s  \"%s\"", (double)bloques * 32.0 / 12000.0, stanag_modo_estado_txt());
            for (k = 0; k < nl; k++) {
                const char *l = stanag_modo_linea(k);
                if (strstr(l, "tramas") || strstr(l, "desvio") || strstr(l, "ENGANCH") || strstr(l, "mejor a")) {
                    printf(" | %s", l);
                }
            }
            printf("\n");
        }
    }
    for (i = 0; i < 200; i++) { stanag_modo_poll(); }
    fclose(f);
    printf("chapa \"%s\", %u medidas\n", stanag_modo_estado_txt(), (unsigned)stanag_modo_medidas());
    n = stanag_modo_lineas();
    for (i = 0; i < n; i++) { tabs(stanag_modo_linea(i)); }
    stanag_modo_stop();
    return 0;
}
