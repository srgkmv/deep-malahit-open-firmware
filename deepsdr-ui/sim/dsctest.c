/*
 * DSC: el entramado y el parseo, con trama sintetica.
 *
 * QUE PRUEBA ESTO Y QUE NO.
 *
 * El CAÑON -los dos tonos, el reloj de bit- se comprueba con señal de verdad:
 * out/dscaire mete las grabaciones de referencia por el mismo dsc.c y saca cuatro
 * llamadas con su comprobacion correcta. Eso aqui no se repite.
 *
 * Lo que se prueba aqui es lo de DETRAS del detector, metiendo los simbolos
 * ya decodificados por dsc_mete_simbolo(): el entramado DX/RX, la diversidad
 * temporal, el ECC y el parseo de cada formato.
 *
 * Y sobre todo el formato de SOCORRO, que es el unico que NO TIENE GRABACION
 * -el usuario no la tiene y ojala siga sin tenerla-. Esa rama esta construida
 * desde el estandar y comprobada SOLO aqui. Va dicho para que nadie le de mas
 * credito del que tiene.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "dsc.h"
#include "rtty_scope.h"   /* la seccion 6: como en la radio */

static unsigned g_cog, g_sue;
static unsigned char g_pozo[0xD4A0];
void      hfdl_ram_coge(void) { g_cog++; }
unsigned char *hfdl_ram_todo(unsigned *cap) { if (cap) { *cap = (unsigned)sizeof g_pozo; } return g_pozo; }
void      hfdl_ram_suelta(void) { g_sue++; }

static int fallos = 0;
static void comprueba(int ok, const char *que)
{
    printf("  %-56s %s\n", que, ok ? "ok" : "MAL");
    if (!ok) { fallos++; }
}

/* §10: el especificador de formato se emite dos veces pero cuenta UNA.
 * Medido sobre las grabaciones reales, ver docs/DSC_FORMATO.md. */
static uint8_t ecc_de(const uint8_t *m, uint8_t n)
{
    uint8_t i = (n >= 2U && m[1] == m[0]) ? 1U : 0U, r = 0U;
    for (; i < n; i++) { r = (uint8_t)(r ^ (m[i] & 0x7FU)); }
    return r;
}

/*
 * Emite un mensaje como lo emite una estacion: en la ranura DX el caracter
 * n, y en la RX el caracter n-2 (cuatro caracteres de separacion, §1.2.1).
 * `roto` es el indice de un caracter al que se le estropea la copia DX, para
 * ver si la RX lo rescata.
 */
static void emite(const uint8_t *m, uint8_t n, int roto)
{
    uint8_t i;
    for (i = 0U; i < (uint8_t)(n + 3U); i++) {
        if (i < n) { dsc_mete_simbolo(m[i], (uint8_t)(i != (uint8_t)roto), 0U); }
        else       { dsc_mete_simbolo(0U, 0U, 0U); }
        /* La ranura RX va dos caracteres por detras, asi que en las tres
         * ultimas vueltas i-2 ya se sale del mensaje: ahi va relleno, como
         * al principio. Antes solo se miraba i >= 2 y se leia m[n]. */
        if (i >= 2U && (uint8_t)(i - 2U) < n) {
            dsc_mete_simbolo(m[i - 2U], 1U, 1U);
        } else {
            dsc_mete_simbolo(104U, 1U, 1U);
        }
    }
}
static void arranca(void) { dsc_stop(); dsc_start(12000.0f); }
static int tiene(const char *t)
{
    uint8_t i;
    for (i = 0U; i < dsc_lineas(); i++) { if (strstr(dsc_linea(i), t)) { return 1; } }
    return 0;
}
static void enseña(void)
{
    uint8_t i;
    for (i = 0U; i < dsc_lineas(); i++) { printf("      %s\n", dsc_linea(i)); }
}

/* ------------------------------------------------------------------------
 * 6. EN AUDIO, COMO EN LA RADIO - 22/11/2026, port del Malahit.
 *
 * Todo lo de arriba entra por dsc_mete_simbolo(). Aqui entra AUDIO, con lo
 * que la radio pone alrededor: el osciloscopio comiendo el mismo audio y
 * dsc_poll() cada quince bloques (una vuelta del bucle, ~40 ms), que es
 * quien mueve el centro. Ruido de verdad delante y entre llamadas, que es
 * donde fallaba: el centro saltando en cada cuadro, las cuentas de la
 * busqueda de fase llenandose de azar, el reloj de bit yendose al tope.
 * Las llamadas con la señal de puntos CORTA (20 bits), la que menos tiempo
 * da. Antes del 22/11 salian la mitad.
 * ------------------------------------------------------------------------ */
static unsigned long g_sem = 12345UL;
static double        g_fase;
static unsigned long g_bloques, g_movidas;
static float         g_centro_visto;

static float ruido1(void)
{
    double a, b;
    g_sem = g_sem * 1103515245UL + 12345UL;
    a = (double)((g_sem >> 16) & 0x7FFF) / 32767.0 - 0.5;
    g_sem = g_sem * 1103515245UL + 12345UL;
    b = (double)((g_sem >> 16) & 0x7FFF) / 32767.0 - 0.5;
    return (float)(a + b);
}

/* Una muestra a la radio: un bloque lleno va a dsc_process() y al
 * osciloscopio, y cada quince, la vuelta del bucle. */
static void muestra(float x)
{
    static float buf[32];
    static uint32_t j;

    buf[j++] = x;
    if (j < 32U) { return; }
    j = 0U;
    dsc_process(buf, 32U);
    rtty_scope_feed(buf, 32U);
    if ((++g_bloques % 15UL) == 0UL) {
        rtty_scope_poll();
        dsc_poll();
        if (dsc_get_centro_hz() != g_centro_visto) {
            g_centro_visto = dsc_get_centro_hz();
            g_movidas++;
        }
    }
}

static void silencio(double seg, float sigma)
{
    long k, n = (long)(seg * 12000.0);
    for (k = 0; k < n; k++) { muestra(sigma * ruido1()); }
}

/* §2 y §1.2.1, escrito otra vez aqui y no sacado de dsc.c: siete bits de
 * valor, el de menos peso primero; tres con la cuenta de B (ceros), el de
 * mas peso primero. B es el tono ALTO. */
static void llamada(const uint8_t *m, uint8_t n, uint16_t puntos, float centro, float sigma)
{
    uint8_t dx[64], nd = 0U, p, i;
    uint16_t k;

    for (i = 0U; i < 6U; i++) { dx[nd++] = 125U; }
    for (i = 0U; i < n; i++)  { dx[nd++] = m[i]; }
    dx[nd++] = 117U; dx[nd++] = 117U;           /* el fin de secuencia, otra vez */
    for (k = 0U; k < (uint16_t)(puntos + 20U * (uint16_t)(nd + 2U)); k++) {
        uint8_t bit;
        int s2;

        if (k < puntos) {
            bit = (uint8_t)(k & 1U);
        } else {
            uint16_t q = (uint16_t)(k - puntos);
            uint8_t par = (uint8_t)(q / 20U), b10 = (uint8_t)((q % 20U) % 10U);
            uint8_t sym, ceros = 0U;

            if ((q % 20U) < 10U) {
                sym = (par < nd) ? dx[par] : 126U;
            } else {
                sym = (par < 8U) ? (uint8_t)(111U - par)
                    : ((uint8_t)(par - 2U) < nd ? dx[par - 2U] : 126U);
            }
            for (i = 0U; i < 7U; i++) { if (!((sym >> i) & 1U)) { ceros++; } }
            bit = (b10 < 7U) ? (uint8_t)((sym >> b10) & 1U)
                             : (uint8_t)((ceros >> (9U - b10)) & 1U);
        }
        for (s2 = 0; s2 < 120; s2++) {
            g_fase += 2.0 * M_PI * (centro + (bit ? -85.0 : 85.0)) / 12000.0;
            if (g_fase > 2.0 * M_PI) { g_fase -= 2.0 * M_PI; }
            muestra((float)sin(g_fase) + sigma * ruido1());
        }
    }
    (void)p;
}

int main(void)
{
    dsc_info_t in;

    printf("DSC: entramado, diversidad y parseo (trama sintetica)\n\n");

    /* 1. la llamada real de GMDSS_1, simbolo a simbolo */
    printf("1. una llamada individual, igual que la de GMDSS_1\n");
    {
        uint8_t m[] = {120,120,0,46,34,6,0,108,21,53,22,0,0,118,
                       126,126,126,126,126,126,126,117,0};
        uint8_t n = (uint8_t)(sizeof m / sizeof m[0]);
        m[n - 1U] = ecc_de(m, (uint8_t)(n - 1U));
        arranca(); emite(m, n, -1); dsc_info(&in); enseña();
        comprueba(tiene("004634060"), "sale el MMSI de destino");
        comprueba(tiene("215322000"), "y el de origen");
        comprueba(tiene("individual"), "y el formato");
        comprueba(in.llamadas == 1UL && in.malas == 0UL, "y la comprobacion da correcta");
    }

    /* 2. la diversidad temporal sirve para algo */
    printf("\n2. un caracter roto en DX, rescatado por su copia RX\n");
    printf("    Es LA razon de ser de este modo: cada caracter viaja dos\n");
    printf("    veces, separadas 400 ms. Si la copia RX no rellenase, esto\n");
    printf("    saldria con el MMSI mal y la comprobacion tambien.\n\n");
    {
        uint8_t m[] = {120,120,0,46,34,6,0,108,21,53,22,0,0,118,
                       126,126,126,126,126,126,126,117,0};
        uint8_t n = (uint8_t)(sizeof m / sizeof m[0]);
        m[n - 1U] = ecc_de(m, (uint8_t)(n - 1U));
        arranca(); emite(m, n, 3); dsc_info(&in); enseña();
        comprueba(tiene("004634060"), "el MMSI sale entero aunque se perdiera un caracter");
        comprueba(in.llamadas == 1UL, "y la comprobacion sigue dando correcta");
    }

    /* 3. SOCORRO - la unica rama sin grabacion */
    printf("\n3. alerta de SOCORRO\n");
    printf("    *** SIN COMPROBAR EN EL AIRE. *** No hay grabacion de una\n");
    printf("    alerta real y ojala siga sin haberla. Esta trama esta\n");
    printf("    construida desde ITU-R M.493-16 y esto es lo UNICO que la\n");
    printf("    respalda. Si alguna vez aparece una grabacion, hay que\n");
    printf("    volver aqui.\n\n");
    {
        /* §4 socorro: el formato va dos veces, luego la autoidentificacion
         * y la naturaleza. */
        uint8_t m[] = {112,112,21,53,22,0,0,105,  /* 105 = hundimiento */
                       126,126,126,126,126,127,0};
        uint8_t n = (uint8_t)(sizeof m / sizeof m[0]);
        m[n - 1U] = ecc_de(m, (uint8_t)(n - 1U));
        arranca(); emite(m, n, -1); dsc_info(&in); enseña();
        comprueba(tiene("SOCORRO"), "se dice que es SOCORRO y no otra cosa");
        /*
         * Estas tres son las que pillaron el fallo. Con solo la de arriba el
         * banco pasaba mientras el barco en peligro salia como DESTINATARIO y
         * el remitente en blanco: una asercion tan floja que no podia fallar.
         */
        comprueba(tiene("215322000"), "sale el MMSI del barco en peligro");
        comprueba(tiene("hundimiento") || tiene("sinking"),
                  "y la naturaleza del socorro, no un interrogante");
        comprueba(!tiene("---------"), "y no hay ningun campo en blanco");
        comprueba(in.llamadas == 1UL, "y la comprobacion da correcta");
    }

    /* 4. una comprobacion mala NO se da por buena */
    printf("\n4. con el ECC estropeado\n");
    {
        uint8_t m[] = {120,120,0,46,34,6,0,108,21,53,22,0,0,118,
                       126,126,126,126,126,126,126,117,0};
        uint8_t n = (uint8_t)(sizeof m / sizeof m[0]);
        m[n - 1U] = (uint8_t)(ecc_de(m, (uint8_t)(n - 1U)) ^ 1U);
        arranca(); emite(m, n, -1); dsc_info(&in); enseña();
        comprueba(in.malas == 1UL && in.llamadas == 0UL,
                  "se marca como MAL y no se cuenta como buena");
    }

    /* 6. en audio, como en la radio */
    printf("\n6. en audio, como en la radio: ruido, y llamadas de puntos cortos\n");
    {
        uint8_t m[] = {120,120,0,46,34,6,0,108,21,53,22,0,0,118,
                       126,126,126,126,126,126,126,117,0};
        uint8_t n = (uint8_t)(sizeof m / sizeof m[0]);
        /* SNR 15 dB en 500 Hz: el seno da 0,5 de potencia y el ruido, llano
         * en los 6 kHz, sigma^2/6 * 500/6000 de ella en esos 500 Hz. */
        float sigma = (float)sqrt(0.5 / pow(10.0, 1.5) * 6000.0 / 500.0 * 6.0);
        unsigned long mov_ruido;
        int i;

        m[n - 1U] = ecc_de(m, (uint8_t)(n - 1U));
        arranca();
        rtty_scope_init();
        g_centro_visto = dsc_get_centro_hz();
        silencio(40.0, sigma);
        mov_ruido = g_movidas;
        for (i = 0; i < 4; i++) {
            llamada(m, n, 20U, 1300.0f, sigma);
            silencio(10.0, sigma);
        }
        dsc_info(&in);
        printf("    40 s de ruido: el centro se mueve %lu veces\n", mov_ruido);
        printf("    cuatro llamadas en 1300 Hz, puntos cortos, SNR 15: %lu bien, %lu con fallo\n",
               (unsigned long)in.llamadas, (unsigned long)in.malas);
        comprueba(mov_ruido < 20UL, "con ruido solo el centro casi no se mueve");
        comprueba(in.llamadas == 4UL && in.malas == 0UL, "y salen las cuatro llamadas");
        dsc_stop();
    }

    /* 5. la RAM prestada se devuelve */
    printf("\n5. la RAM prestada\n");
    {
        unsigned c0 = g_cog, s0 = g_sue; int i;
        for (i = 0; i < 5; i++) { dsc_start(12000.0f); dsc_stop(); }
        printf("    cinco arranques y paradas: %u peticiones, %u sueltas\n",
               g_cog - c0, g_sue - s0);
        comprueba((g_cog - c0) == (g_sue - s0), "cada peticion tiene su suelta");
    }

    printf("\n  %s\n", fallos ? "HAY FALLOS" : "TODO CORRECTO (0 fallos)");
    return fallos ? 1 : 0;
}
