/*
 * ======================================================================
 * EL DECODIFICADOR ESTEREO DEL MALAHIT, DETRAS DEL WFM DE VERDAD - 22/11/2026
 * ======================================================================
 *
 * malahit/bringup/src/estereo.c no es de upstream: upstream es mono a
 * proposito. Aqui se le pone detras de demod_wfm_process_raw() tal cual,
 * enganchado como en la radio - rds_feed() envuelto con --wrap para darle
 * el multiplex crudo, y su estereo_aplica() sobre lo que upstream manda a
 * la salida -, y se mide con numeros:
 *
 *   1. SEPARACION. Izquierda un tono de 1 kHz, derecha uno de 3 kHz, el
 *      multiplex como lo emite una emisora (BS.450): la suma, el piloto en
 *      sin(wt), la diferencia en sin(2wt). En la salida izquierda el 1 kHz
 *      tiene que estar y el 3 kHz no, y al reves en la derecha.
 *   2. EQUILIBRIO. Los dos tonos con el mismo nivel en su canal.
 *   3. MONO SIN PILOTO. Una emisora mono no tiene que encender nada.
 *   4. CON RUIDO. Que la mezcla baja hacia mono cuando el piloto se hunde.
 *
 * Se repite con el AGC manual y con el automatico: el automatico cambia la
 * ganancia dentro del bloque y es lo que la regresion tiene que seguir.
 *
 *   make wfmestereo
 */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "demod_am.h"
#include "sdr_rx.h"
#include "agc.h"
#include "demod_stubs.h"
#include "estereo.h"

#define FS     192000.0
#define NBLK   SDR_RX_BLOCK_SAMPLES_WFM
#define NCAP   (NBLK * 128)            /* 341 ms */
static double g_desv = 75000.0;        /* desviacion de pico */
static double g_fl = 1000.0, g_fr = 3000.0;   /* el tono de cada canal */

/* El multiplex lo ve rds_feed() y, envuelto, tambien el estereo. */
void __real_rds_feed(const float *mpx, uint32_t n);
void __wrap_rds_feed(const float *mpx, uint32_t n)
{
    estereo_mpx(mpx, n);
    __real_rds_feed(mpx, n);
}

static double   g_fi, g_t;
static unsigned g_rnd = 7u;
static float    g_l[NCAP], g_r[NCAP];
static int      s_fallos;

static double gauss(void)
{
    double u1, u2;
    g_rnd = g_rnd * 1103515245u + 12345u;
    u1 = ((g_rnd >> 8) & 0xFFFFFFu) / (double)0x1000000u + 1e-12;
    g_rnd = g_rnd * 1103515245u + 12345u;
    u2 = ((g_rnd >> 8) & 0xFFFFFFu) / (double)0x1000000u;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* Potencia de un tono por Goertzel, en dB relativos. */
static double tono_db(const float *x, int n, double f)
{
    double w = 2.0 * M_PI * f / FS, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0, p;
    int k;
    for (k = 0; k < n; k++) { double s0 = x[k] + c * s1 - s2; s2 = s1; s1 = s0; }
    p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 10.0 * log10(p + 1e-9);
}

/* Una pasada: piloto 1/0, ruido en el I/Q, perfil de AGC. */
static void corre(int piloto, double ruido, agc_profile_t perfil, double amp)
{
    int16_t raw[2 * NBLK];
    int16_t sal[2 * NBLK];
    int b, k, tomadas = 0;
    const int calienta = 300;

    demod_am_init();
    demod_am_set_mode(DEMOD_MODE_WFM);
    demod_am_set_audio_bw(AUDIO_BW_4K0);
    demod_am_set_wfm_ifbw(WFM_IFBW_WIDE);
    demod_am_set_agc_profile(perfil);
    cons_todos_apagados();
    g_cons[CONS_RDS].activo = 1U;      /* en WFM el port tiene el RDS encendido */
    cons_captura_tramas((uint32_t)NBLK);
    estereo_reset();
    estereo_pon(1U);
    g_fi = 0.0; g_t = 0.0; g_rnd = 7u;

    for (b = 0; b < calienta + NCAP / NBLK + 2; b++) {
        uint32_t n;
        for (n = 0; n < (uint32_t)NBLK; n++) {
            double t = g_t / FS, w = 2.0 * M_PI * 19000.0 * t;
            double L = sin(2.0 * M_PI * g_fl * t), R = sin(2.0 * M_PI * g_fr * t);
            double m = 0.9 * (0.5 * (L + R) + (piloto ? 0.5 * (L - R) * sin(2.0 * w) : 0.0))
                     + (piloto ? 0.1 * sin(w) : 0.0);
            double i, q;

            g_fi += 2.0 * M_PI * g_desv * m / FS;
            if (g_fi >  M_PI) g_fi -= 2.0 * M_PI;
            if (g_fi < -M_PI) g_fi += 2.0 * M_PI;
            g_t += 1.0;
            i = amp * cos(g_fi); q = amp * sin(g_fi);
            if (ruido > 0.0) { i += ruido * gauss(); q += ruido * gauss(); }
            if (i >  32767.0) { i =  32767.0; }
            if (i < -32768.0) { i = -32768.0; }
            if (q >  32767.0) { q =  32767.0; }
            if (q < -32768.0) { q = -32768.0; }
            raw[2U * n] = (int16_t)lrint(i); raw[2U * n + 1U] = (int16_t)lrint(q);
        }
        demod_wfm_process_raw(raw);
        if (!estereo_aplica(cons_audio(), sal, (uint32_t)NBLK)) {
            memcpy(sal, cons_audio(), sizeof sal);
        }
        if (b < calienta || tomadas >= NCAP) { continue; }
        for (k = 0; k < NBLK && tomadas < NCAP; k++) {
            g_l[tomadas] = (float)sal[2 * k];
            g_r[tomadas] = (float)sal[2 * k + 1];
            tomadas++;
        }
    }
}

enum { ESTEREO, MONO, RUIDO, HUNDIDO };

static void informe(const char *que, int piloto, double ruido, agc_profile_t perfil,
                    int tipo, double sep_min)
{
    estereo_estado_t e;
    double l1, l3, r1, r3;

    corre(piloto, ruido, perfil, 12000.0);
    estereo_estado(&e);
    l1 = tono_db(g_l, NCAP, 1000.0); l3 = tono_db(g_l, NCAP, 3000.0);
    r1 = tono_db(g_r, NCAP, 1000.0); r3 = tono_db(g_r, NCAP, 3000.0);
    printf("  %-28s piloto %5.1f dB %6.0f Hz dev  mezcla %.2f  g %5.2f  %8.2f Hz  resto %6.1f dB\n",
           que, e.snr_db, e.piloto_dev_hz, e.mezcla, e.ganancia, e.piloto_hz, e.resto_db);
    printf("  %-28s izq: 1k %6.1f 3k %6.1f   der: 1k %6.1f 3k %6.1f   "
           "separacion %5.1f / %5.1f dB\n", "", l1, l3, r1, r3, l1 - l3, r3 - r1);
    switch (tipo) {
    case ESTEREO:
        if (l1 - l3 < sep_min || r3 - r1 < sep_min) {
            printf("    *** separacion por debajo de %.0f dB\n", sep_min); s_fallos++;
        }
        break;
    case MONO:
    case HUNDIDO:
        /* mono: los dos canales iguales, y la mezcla a cero */
        if (fabs(l1 - r1) > 0.5 || fabs(l3 - r3) > 0.5) {
            printf("    *** en mono los canales no son iguales\n"); s_fallos++;
        }
        if (e.mezcla > 0.0f) { printf("    *** encendio estereo donde no debia\n"); s_fallos++; }
        break;
    default:            /* RUIDO: a medias, y se dice cuanto */
        if (e.mezcla <= 0.0f || e.mezcla >= 1.0f) {
            printf("    *** con el piloto a medias la mezcla deberia estar entre 0 y 1\n");
            s_fallos++;
        }
        break;
    }
}

int main(void)
{
    printf("estereo de FM detras del WFM de upstream\n");
    informe("estereo, AGC manual", 1, 0.0, AGC_PROFILE_MANUAL, ESTEREO, 55.0);
    informe("estereo, AGC media", 1, 0.0, AGC_PROFILE_MEDIUM, ESTEREO, 55.0);
    /* La misma con la mitad de desviacion: si la separacion sube, lo que
     * la limita es la ventana de +/-96 kHz y no el decodificador. */
    g_desv = 37500.0;
    informe("estereo, media desviacion", 1, 0.0, AGC_PROFILE_MEDIUM, ESTEREO, 55.0);
    g_desv = 75000.0;
    g_fl = 3000.0; g_fr = 1000.0;      /* los tonos al reves: canal o frecuencia */
    corre(1, 0.0, AGC_PROFILE_MANUAL, 12000.0);
    printf("  %-28s izq: 1k %6.1f 3k %6.1f   der: 1k %6.1f 3k %6.1f\n", "al reves (3k izq, 1k der)",
           tono_db(g_l, NCAP, 1000.0), tono_db(g_l, NCAP, 3000.0),
           tono_db(g_r, NCAP, 1000.0), tono_db(g_r, NCAP, 3000.0));
    g_fl = 1000.0; g_fr = 3000.0;
    informe("mono, sin piloto", 0, 0.0, AGC_PROFILE_MEDIUM, MONO, 0.0);
    informe("estereo con ruido (debil)", 1, 3000.0, AGC_PROFILE_MEDIUM, RUIDO, 0.0);
    informe("estereo con mucho ruido", 1, 9000.0, AGC_PROFILE_MEDIUM, HUNDIDO, 0.0);
    printf(s_fallos ? "\n%d FALLOS\n" : "\nTODO BIEN\n", s_fallos);
    return s_fallos ? 1 : 0;
}
