/*
 * RAFAGAS DE HFDL PARA EL HACKRF - 26/11/2026, port del Malahit (paso 59).
 *
 * Lo que hace sim/hfdl_extremo.c en su simbolos_monta() y audio_emite(),
 * sin el decodificador detras: la rafaga entera -los 448 simbolos de
 * portadora, A, A, M1 del giro pedido, M2, las nueve ventanas de
 * entrenamiento y los segmentos de datos de la autoprueba de
 * hfdl_payload_decode.c (un mensaje con su CRC) con su entrenamiento
 * intercalado-, conformada con la raiz de coseno alzado (beta 0,35) a 12 kHz
 * y subida a la subportadora de 1.440 Hz. Sale I/Q en f32 intercalado, que
 * malahit/tools/hackrf_gen.py hfdl --iq12 lleva al HackRF.
 *
 * NO ES UN TRANSMISOR INDEPENDIENTE: es el de la autoprueba, el mismo autor
 * que el receptor. Lo que prueba es la CADENA DE LA RADIO -la I/Q a 12 kHz
 * del gancho de demod_am.c, el nivel, la carga de la interrupcion, la
 * memoria prestada, el bucle-; que el protocolo este bien entendido lo
 * dice el aire, no esto.
 *
 *   ./out/hfdlgen salida.c32 [giro 0..7] [rafagas] [hueco_s]
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "hfdl_preamble_seq.h"
#include "hfdl_payload_decode.h"
#include "hfdl_data_segment.h"
#include "hfdl_iq_mixer.h"

/* hfdl_modo.c viene con el comodin y pide la base de modelos; aqui no hay. */
bool icao24_db_lookup(uint32_t icao24, char *out, uint32_t out_len)
{
    (void)icao24;
    if (out && out_len) { out[0] = '\0'; }
    return false;
}

#define MAX_SIM  5200u
#define MAX_SIMB 8704u
static float g_di[MAX_SIM], g_dq[MAX_SIM];
static float g_si[MAX_SIMB], g_sq[MAX_SIMB];
static uint32_t g_ns;

static void pon(float i_, float q_)
{
    if (g_ns < MAX_SIMB) { g_si[g_ns] = i_; g_sq[g_ns] = q_; g_ns++; }
}

/* sim/hfdl_extremo.c, simbolos_monta() */
static uint32_t monta(uint32_t giro)
{
    float a[HFDL_A_LEN], m1[HFDL_M1_LEN];
    uint32_t i, v, nd = 0u, idx = 0u;

    hfdl_preamble_seq_build_a(a);
    hfdl_preamble_seq_build_m1_variant(giro, m1);
    if (!hfdl_payload_decode_tx_simbolos(giro, g_di, g_dq, MAX_SIM, &nd)) { return 0u; }
    g_ns = 0u;
    for (i = 0; i < 60u; i++)         { pon(0.0f, 0.0f); }
    for (i = 0; i < 448u; i++)        { pon(1.0f, 0.0f); }
    for (i = 0; i < HFDL_A_LEN; i++)  { pon(a[i], 0.0f); }
    for (i = 0; i < HFDL_A_LEN; i++)  { pon(a[i], 0.0f); }
    for (i = 0; i < HFDL_M1_LEN; i++) { pon(m1[i], 0.0f); }
    for (i = 0; i < HFDL_M2_LEN; i++) { pon(1.0f, 0.0f); }
    for (v = 0; v < 9u; v++) {
        for (i = 0; i < HFDL_T_LEN; i++) { pon(HFDL_T_SEQ[0][i], 0.0f); }
    }
    for (v = 0; v < (uint32_t)HFDL_FRAME_PARAMS[giro].data_segment_cnt
                && g_ns < MAX_SIMB - 50u; v++) {
        for (i = 0; i < 30u; i++) {
            pon((idx < nd) ? g_di[idx] : 1.0f, (idx < nd) ? g_dq[idx] : 0.0f);
            idx++;
        }
        for (i = 0; i < HFDL_T_LEN; i++) { pon(HFDL_T_SEQ[0][i], 0.0f); }
    }
    for (i = 0; i < 40u; i++)         { pon(0.0f, 0.0f); }
    return 1u;
}

#define RRC_SPS   (12000.0f / 1800.0f)
#define RRC_SPAN  8
#define RRC_LEN   ((int)(RRC_SPAN * RRC_SPS) | 1)
static float g_rrc[RRC_LEN];

/* sim/hfdl_extremo.c, rrc_init() */
static void rrc_init(void)
{
    const float beta = 0.35f;
    int n;
    float suma = 0.0f;

    for (n = 0; n < RRC_LEN; n++) {
        float t = ((float)n - (float)(RRC_LEN - 1) * 0.5f) / RRC_SPS;
        float v;

        if (fabsf(t) < 1e-6f) {
            v = 1.0f - beta + 4.0f * beta / 3.14159265f;
        } else if (fabsf(fabsf(4.0f * beta * t) - 1.0f) < 1e-4f) {
            float a = beta / 1.41421356f;
            float b = (1.0f + 2.0f / 3.14159265f) * sinf(0.78539816f / beta);
            float c = (1.0f - 2.0f / 3.14159265f) * cosf(0.78539816f / beta);
            v = a * (b + c);
        } else {
            float pt = 3.14159265f * t;
            float num = sinf(pt * (1.0f - beta)) + 4.0f * beta * t * cosf(pt * (1.0f + beta));
            float den = pt * (1.0f - 16.0f * beta * beta * t * t);
            v = num / den;
        }
        g_rrc[n] = v;
        suma += v;
    }
    for (n = 0; n < RRC_LEN; n++) { g_rrc[n] /= suma; }
}

/* sim/hfdl_extremo.c, audio_emite() sin canal: a 12 kHz, en la subportadora */
static void emite(FILE *f)
{
    double t = 0.0;
    const double PASO = 1800.0 / 12000.0;
    float sub_ph = 0.0f;

    while (t < (double)(g_ns - 1u)) {
        float ii = 0.0f, qq = 0.0f, z[2];
        int kk, centro = (int)(t + 0.5);

        for (kk = centro - RRC_SPAN; kk <= centro + RRC_SPAN; kk++) {
            int idx2;
            float dt;

            if (kk < 0 || (uint32_t)kk >= g_ns) { continue; }
            dt = (float)(t - (double)kk);
            idx2 = (int)((dt * RRC_SPS) + (float)(RRC_LEN - 1) * 0.5f + 0.5f);
            if (idx2 < 0 || idx2 >= RRC_LEN) { continue; }
            ii += g_si[kk] * g_rrc[idx2];
            qq += g_sq[kk] * g_rrc[idx2];
        }
        ii *= RRC_SPS; qq *= RRC_SPS;
        z[0] = ii * cosf(sub_ph) - qq * sinf(sub_ph);
        z[1] = ii * sinf(sub_ph) + qq * cosf(sub_ph);
        sub_ph += 6.28318530718f * HFDL_MIXER_SUBCARRIER_HZ / 12000.0f;
        if (sub_ph > 6.28318530718f) { sub_ph -= 6.28318530718f; }
        fwrite(z, sizeof z[0], 2, f);
        t += PASO;
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    uint32_t giro = (argc > 2) ? (uint32_t)atoi(argv[2]) : 3u;
    uint32_t n = (argc > 3) ? (uint32_t)atoi(argv[3]) : 4u, k, j;
    float hueco = (argc > 4) ? (float)atof(argv[4]) : 2.0f;
    static const float cero[2] = { 0.0f, 0.0f };

    if (argc < 2) { printf("uso: hfdlgen salida.c32 [giro 0..7] [rafagas] [hueco_s]\n"); return 2; }
    f = fopen(argv[1], "wb");
    if (!f) { printf("no puedo escribir %s\n", argv[1]); return 2; }
    rrc_init();
    if (!monta(giro)) { printf("el transmisor no monta el giro %u\n", giro); return 1; }
    for (k = 0u; k < n; k++) {
        for (j = 0u; j < (uint32_t)(hueco * 12000.0f); j++) { fwrite(cero, sizeof cero[0], 2, f); }
        emite(f);
    }
    for (j = 0u; j < (uint32_t)(hueco * 12000.0f); j++) { fwrite(cero, sizeof cero[0], 2, f); }
    fclose(f);
    printf("%u rafagas del giro %u (%u simbolos cada una), %.1f s de hueco, I/Q f32 a 12 kHz\n",
           n, giro, g_ns, hueco);
    return 0;
}
