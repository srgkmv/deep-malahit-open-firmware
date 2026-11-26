#include <string.h>

#include "rotulos.h"
#include "emisoras.h"       /* ../../User - upstream's, all of these */
#include "spec_rotulos.h"
#include "spectrum.h"
#include "gfx2.h"
#include "palette.h"
#include "spec_chrome.h"
#include "rtc_hw.h"
#include "font_ui_11.h"     /* spec_rotulos_draw()'s face: its line height */

/*
 * Upstream's main.c, 8648-9035: emis_cuando(), bd_rotulos_refresca(),
 * bd_rotulos_textos(), bd_rotulos_banda(), and their call before
 * spectrum_draw() at 38613. Their comments there say why each step is
 * where it is (the 25/10 invalidate, the 15/10 lesson about the brake,
 * the 24/10 raw-list cache); what changes here is said where it does.
 */

static uint8_t        s_puesto;         /* upstream's s_bd_espectro: born off */
static spec_rotulos_t s_rot;            /* s_bd_rot */

static uint32_t s_visto_c, s_visto_s, s_visto_tune;
static uint16_t s_visto_min = 0xFFFFU;

static spec_rot_txt_t s_txt;            /* the names, read once a list */
static uint8_t        s_txt_vale;
static uint32_t       s_txt_span;

/* The window being drawn, for the hook (upstream: s_chrome's). */
static uint32_t s_dib_c, s_dib_s;
static uint8_t  s_enganchado;

uint8_t rotulos_puesto(void) { return s_puesto; }

void rotulos_pon(uint8_t on)
{
    /* A forced re-read, as upstream's AJ_BD_ESPECTRO. Its "only with a
     * database" is the menu's (menus.c): from the FRAM at boot the
     * setting comes before the database is opened, and is kept. */
    s_puesto     = on ? 1U : 0U;
    s_visto_min  = 0xFFFFU;
}

/*
 * Upstream's emis_cuando(): the UTC minute and day. Upstream reads the
 * clock through reloj_ahora_ms(); here rtc_hw_get(), the same RTC under
 * upstream's contract (src/rtc_hw.c), local time - emisoras_utc() takes
 * the radio's own zone off, hora_huso(). One addition: a clock never set
 * (rtc_hw_has_ever_synced() false - after the battery went, as now) does
 * not filter by DAY either, as upstream does for a failed crystal: a
 * station too many beats hiding the one being heard.
 */
static uint8_t emis_cuando(uint16_t *min_utc, uint8_t *dia)
{
    rtc_hw_datetime_t dt;
    uint16_t min_local;
    uint8_t  d = EMISORAS_DIA_CUALQUIERA;
    uint8_t  fiable = (uint8_t)(!rtc_hw_lxtal_failed() && rtc_hw_has_ever_synced());

    rtc_hw_get(&dt);
    min_local = (uint16_t)(dt.hour * 60U + dt.minute);
    *min_utc  = min_local;

    if (fiable) {
        d = emisoras_dia_semana(dt.year, dt.month, dt.day);
    }
    emisoras_utc(min_local, (d == EMISORAS_DIA_CUALQUIERA) ? 0U : d, min_utc, &d);
    if (!fiable) { d = EMISORAS_DIA_CUALQUIERA; }
    *dia = d;
    return 1U;
}

/* Upstream's bd_rotulos_refresca(). */
static void refresca(uint32_t center_hz, uint32_t span_hz, uint32_t tune_hz)
{
    uint32_t mitad = span_hz / 2UL, lo, hi;
    uint16_t min_utc;
    uint8_t  dia;
    spec_rot_cand_t c[SPEC_ROT_CAND];
    uint16_t antes_cl[SPEC_ROT_MAX];
    uint16_t antes_d10[SPEC_ROT_MAX];
    uint32_t antes_base;
    uint8_t  antes_n, n, i;

    if (!s_puesto || !emisoras_hay() || span_hz == 0UL) {
        if (s_rot.n != 0U) { spectrum_invalida(); }
        s_rot.n = 0U;
        return;
    }
    if (!emis_cuando(&min_utc, &dia)) {
        if (s_rot.n != 0U) { spectrum_invalida(); }
        s_rot.n = 0U;
        return;
    }
    if (center_hz == s_visto_c && span_hz == s_visto_s
        && min_utc == s_visto_min && tune_hz == s_visto_tune) {
        return;
    }

    lo = (center_hz > mitad) ? (center_hz - mitad) : 0UL;
    hi = center_hz + mitad;

    antes_n    = s_rot.n;
    antes_base = s_rot.base_hz;
    memcpy(antes_cl,  s_rot.clave, sizeof antes_cl);
    memcpy(antes_d10, s_rot.d10,   sizeof antes_d10);

    {
        /* The raw list kept: the dial alone does not change what is in
         * the window, only who wins each slot (upstream, 24/10/2026). */
        static emisoras_rango_r_t s_crudas[SPEC_ROT_CAND];
        static uint8_t  s_crudas_n;
        static uint32_t s_crudas_lo, s_crudas_hi;
        static uint16_t s_crudas_min;
        static uint8_t  s_crudas_vale;

        if (!s_crudas_vale || lo != s_crudas_lo || hi != s_crudas_hi
            || min_utc != s_crudas_min) {
            uint16_t hubo;

            s_crudas_n = emisoras_rango_repartido(lo, hi, min_utc, dia,
                                                  s_crudas,
                                                  (uint8_t)SPEC_ROT_CAND, &hubo);
            s_crudas_lo   = lo;
            s_crudas_hi   = hi;
            s_crudas_min  = min_utc;
            s_crudas_vale = 1U;
        }
        n = s_crudas_n;
        for (i = 0U; i < n; i++) {
            c[i].hz    = s_crudas[i].hz;
            c[i].clave = s_crudas[i].nombre;   /* two alike: one station */
        }
    }
    n = spec_rotulos_filtra(c, n, center_hz, span_hz, tune_hz, SPEC_ROT_MAX);

    s_rot.base_hz = lo;
    for (i = 0U; i < n; i++) {
        spec_rotulos_pon(&s_rot, i, c[i].hz, c[i].clave);
    }
    s_rot.n = n;

    {
        uint8_t cambio = (uint8_t)(
            center_hz != s_visto_c || span_hz != s_visto_s
            || n != antes_n
            || s_rot.base_hz != antes_base
            || memcmp(antes_cl,  s_rot.clave, sizeof antes_cl)  != 0
            || memcmp(antes_d10, s_rot.d10,   sizeof antes_d10) != 0);

        if (cambio) { spectrum_invalida(); }
    }
    if (n != antes_n || memcmp(antes_cl, s_rot.clave, sizeof antes_cl) != 0) {
        s_txt_vale = 0U;
    }

    s_visto_c    = center_hz;
    s_visto_s    = span_hz;
    s_visto_min  = min_utc;
    s_visto_tune = tune_hz;
}

/* Upstream's bd_rotulos_textos(): the names out of the database once a
 * list, cut to the label's width, then cleaned of repeats. */
static void textos(uint32_t span_hz)
{
    uint8_t i;

    if (s_txt_vale && s_txt_span == span_hz) { return; }

    memset(s_txt, 0, sizeof s_txt);
    for (i = 0U; i < s_rot.n && i < SPEC_ROT_MAX; i++) {
        char largo[33];

        emisoras_texto(s_rot.clave[i], largo, (uint8_t)sizeof largo);
        spec_rotulos_recorta(s_txt[i], largo);
    }
    spec_rotulos_limpia(&s_rot, s_txt, span_hz);
    s_txt_span = span_hz;
    s_txt_vale = 1U;
}

/* Upstream's bd_rotulos_banda(): spectrum_draw() calls it for each band
 * it puts out, and the labels go over the trace's top rows. */
static void banda(gfx2_surf_t *sf)
{
    if (!s_puesto || s_rot.n == 0U) { return; }

    /*
     * THE BAND, DOWN TO A GRID LINE THAT LEAVES ROOM FOR THE THREE ROWS.
     * Upstream's is down to the FIRST grid line, and on the DEEPSDR's
     * 208-row trace that is ~42 rows: exactly SPEC_ROT_FILAS rows of
     * font_ui_11 (14). Here the trace is 117 rows and the first line ~24
     * down - one row - and spec_rotulos_draw() then puts all three
     * zigzag rows at one height while keeping their collisions apart:
     * names over names (26/11/2026, the first screenshot). So the same
     * rule, a grid line, but the first one with three rows above it - the
     * second, here. (The collapse itself is upstream's, latent on the
     * DEEPSDR: its band always holds the three.)
     */
    int16_t fila = spectrum_rejilla_fila(1);

    for (uint8_t k = 0U; k < 8U
         && (int16_t)(fila + 1) < (int16_t)(SPEC_ROT_FILAS * font_ui_11.line_h); k++) {
        int16_t sig = spectrum_rejilla_fila((int16_t)(fila + 1));

        if (sig <= fila) { break; }             /* no line further down */
        fila = sig;
    }
    const int16_t alto_franja = (int16_t)(fila + 1);

    if (!gfx2_band_hits(sf, (int16_t)SPC_TRACE_Y, (int16_t)(alto_franja + 4))) {
        return;
    }
    {
        uint32_t texto = PAL_INK, raya = PAL_INK_DIM;

        spec_rotulos_tinta(spectrum_fondo_rgb(), PAL_INK, PAL_INK_DIM,
                           PAL_TRACE, &texto, &raya);
        (void)spec_rotulos_draw(sf, &s_rot, s_txt, s_dib_c, s_dib_s,
                                SPC_TRACE_X, SPC_TRACE_W,
                                (int16_t)SPC_TRACE_Y, alto_franja, 0,
                                gfx2_rgb(texto), gfx2_rgb(raya));
    }
}

void rotulos_antes(uint32_t center_hz, uint32_t span_hz, uint32_t tune_hz)
{
    if (!s_puesto) {
        if (s_rot.n != 0U) { spectrum_invalida(); s_rot.n = 0U; }
        return;
    }
    refresca(center_hz, span_hz, tune_hz);
    if (s_rot.n != 0U) {
        textos(span_hz);
        s_dib_c = center_hz;
        s_dib_s = span_hz;
        spectrum_encima(banda);
        s_enganchado = 1U;
    }
}

void rotulos_despues(void)
{
    if (s_enganchado) {
        spectrum_encima(0);
        s_enganchado = 0U;
    }
}

/* ------------------------------------------------------------------ */
/* WHO IS ON THE DIAL, AND THE JUMP (step 62)                          */
/* ------------------------------------------------------------------ */

static char *emis_num(char *p, uint32_t v, uint8_t cifras)
{
    char t[8];
    uint8_t n = 0U;

    do { t[n++] = (char)('0' + (v % 10UL)); v /= 10UL; } while (v != 0UL && n < 8U);
    while (n < cifras) { t[n++] = '0'; }
    while (n > 0U) { *p++ = t[--n]; }
    return p;
}

/* Copy `q` on at `p`, never past `fin` (the buffer's last byte). */
static char *emis_pega(char *p, const char *fin, const char *q)
{
    while (*q != '\0' && p < fin) { *p++ = *q++; }
    return p;
}

/*
 * Upstream's emis_pon() (main.c 37256): the station on the air within
 * 600 Hz of `hz`, its name, and a line "country - site - language -
 * hh:mm-hh:mm UTC (+n)". Upstream writes into its 48- and 96-byte
 * buffers without a bound; here every write is bounded by the caller's.
 */
uint8_t rotulos_emisora(uint32_t hz, char *nom, uint8_t nl, char *txt, uint8_t tl)
{
    emisoras_r_t r[3];
    uint16_t min_utc;
    uint8_t  dia, n;
    char    *p;
    const char *fin;
    char     tmp[24];

    if (nom == 0 || txt == 0 || nl == 0U || tl < 32U) { return 0U; }
    nom[0] = '\0';
    txt[0] = '\0';
    if (!emisoras_hay()) { return 0U; }
    if (!emis_cuando(&min_utc, &dia)) { return 0U; }

    n = emisoras_busca(hz, 600UL, min_utc, dia, r, 3U);
    if (n == 0U || !r[0].ahora) { return 0U; }   /* there is one, at another hour */

    emisoras_texto(r[0].nombre, nom, nl);

    p   = txt;
    fin = txt + tl - 24U;           /* " - hh:mm-hh:mm UTC (+n)" and its 0 */
    emisoras_pais(r[0].pais, tmp, (uint8_t)sizeof tmp);
    p = emis_pega(p, fin, tmp);
    if (r[0].sitio != EMISORAS_SIN_TEXTO) {
        emisoras_texto(r[0].sitio, tmp, (uint8_t)sizeof tmp);
        if (tmp[0] != '\0') { p = emis_pega(p, fin, " - "); p = emis_pega(p, fin, tmp); }
    }
    if (r[0].idioma != EMISORAS_SIN_TEXTO) {
        emisoras_texto(r[0].idioma, tmp, (uint8_t)sizeof tmp);
        if (tmp[0] != '\0') { p = emis_pega(p, fin, " - "); p = emis_pega(p, fin, tmp); }
    }
    *p++ = ' '; *p++ = '-'; *p++ = ' ';
    p = emis_num(p, r[0].ini / 60U, 2U); *p++ = ':';
    p = emis_num(p, r[0].ini % 60U, 2U); *p++ = '-';
    p = emis_num(p, r[0].fin / 60U, 2U); *p++ = ':';
    p = emis_num(p, r[0].fin % 60U, 2U);
    *p++ = ' '; *p++ = 'U'; *p++ = 'T'; *p++ = 'C';
    if (n > 1U) {
        *p++ = ' '; *p++ = '('; *p++ = '+';
        p = emis_num(p, (uint32_t)(n - 1U), 1U);
        *p++ = ')';
    }
    *p = '\0';
    return 1U;
}

static uint8_t s_jmp;               /* upstream's s_jmp_on: born off */

uint8_t rotulos_jmp(void) { return s_jmp; }

void rotulos_jmp_pon(uint8_t on) { s_jmp = on ? 1U : 0U; }

/* Upstream's jmp_conmutar(): never on without a database - a knob that
 * stops tuning and finds nothing is a radio that looks broken. */
uint8_t rotulos_jmp_conmuta(void)
{
    if (!s_jmp && !emisoras_hay()) { return 0U; }
    s_jmp = s_jmp ? 0U : 1U;
    return 1U;
}

/*
 * Upstream's knob with JMP on (main.c 34824): up to ten jumps a pass,
 * each to the next station on the air now, strictly beyond the last, with
 * ONE budget of flash reads shared by all of them (emisoras_salto()'s
 * `gasto`), so a fast turn costs what a slow one does. Returns the
 * frequency reached, or `desde` if none - and the turn is spent either
 * way, as upstream's: it does not fall through to the normal step.
 */
uint32_t rotulos_salta(uint32_t desde, int32_t detents, uint32_t lo_hz, uint32_t hi_hz)
{
    uint16_t min_utc = 0U, gasto = 0U;
    uint8_t  dia = EMISORAS_DIA_CUALQUIERA;
    int32_t  quedan = detents;
    int8_t   dir = (detents < 0) ? -1 : 1;
    uint32_t hz = desde;

    if (detents == 0 || !emisoras_hay()) { return desde; }
    if (!emis_cuando(&min_utc, &dia)) { return desde; }
    if (quedan < 0) { quedan = -quedan; }
    if (quedan > 10) { quedan = 10; }

    while (quedan-- > 0) {
        uint32_t sig;

        if (!emisoras_salto(hz, dir, min_utc, dia, &sig, 0, &gasto)) { break; }
        if (sig < lo_hz || sig > hi_hz) { break; }
        hz = sig;
    }
    return hz;
}
