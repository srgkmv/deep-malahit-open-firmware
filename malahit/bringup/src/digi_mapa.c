#include <string.h>

#include "board.h"
#include "console.h"
#include "digi_mapa.h"

/* ../../User - upstream's, all of them */
#include "gfx2.h"
#include "mapa.h"
#include "mapa_eti.h"
#include "mapa_bin.h"
#include "mapa_flash.h"
#include "mando_mapa.h"
#include "rtc_hw.h"
#include "idioma.h"
#include "spi_flash.h"
#include "hfdl_ram.h"
#include "ft8_modo.h"
#include "ft8_lista.h"
#include "ft8_decoder.h"
#include "wspr_modo.h"
#include "hfdl_modo.h"
#include "ais_modo.h"
#include "jtty_modo.h"

/*
 * Upstream's main.c: 27220-27850 (the box, the painter, the labels, the
 * draw, the zoom), 35890-35950 and 36580-36640 (the map's touches), 3365
 * (the loop's dirty flag), 17165-17220 (mapa_al_arrancar()). The comments
 * there say why each piece is as it is; here, where something differs.
 */

/* The hole: the panel's area under its header - digi.c's rows. */
#define HUECO_X   0
#define HUECO_Y   ((int16_t)(DIGI_SCOPE_Y + UDG_HDR_H))                     /* 75  */
#define HUECO_W   ((int16_t)GFX2_W)                                         /* 480 */
#define HUECO_H   ((int16_t)(DIGI_PANEL_Y + DIGI_PANEL_H - DIGI_SCOPE_Y - UDG_HDR_H)) /* 211 */

static mapa_caja_t s_caja;
static uint8_t     s_caja_puesta;
static uint8_t     s_sucio;
static uint16_t    s_ahora;             /* the frame's minute, local */

static mapa_gesto_t s_gesto;
static uint8_t      s_zoom_tap;         /* the press was on a zoom key */
static uint8_t      s_mapa_tap;         /* ... or on the map: drag or pick */

uint8_t digi_mapa_modo(void)
{
    return (uint8_t)(ft8_modo_activo() || hfdl_modo_activo()
                     || ais_modo_activo() || jtty_modo_activo()
                     || wspr_modo_activo());
}

/* Placed the first time and kept after: zoom and drag survive leaving
 * the map and coming back, as upstream's. */
static mapa_caja_t *caja(void)
{
    if (!s_caja_puesta) {
        s_caja_puesta = 1U;
        mapa_pon(&s_caja, HUECO_X, HUECO_Y, HUECO_W, HUECO_H);
    }
    return &s_caja;
}

uint8_t digi_mapa_en_zona(uint16_t x, uint16_t y)
{
    return (uint8_t)(x < (uint16_t)HUECO_W && y >= (uint16_t)HUECO_Y
                     && y < (uint16_t)(HUECO_Y + HUECO_H));
}

static gfx2_rgba_t marca_color(uint8_t viva)
{
    return gfx2_rgb(viva ? mapa_bin_color(MAPA_COL_MARCA,     0x46E0A0UL)
                         : mapa_bin_color(MAPA_COL_MARCA_OFF, 0x2E8C68UL));
}

static uint8_t ft8_se_ve(const char *l)
{
    return (uint8_t)(!digi_ft8_solo_cq() || ft8_lista_es_cq(l));
}

/* Upstream's ft8_mapa_pinta(): the sea, the coast, the marks (the three
 * newest bright), home, the labels, the zoom keys, the card. */
static void pinta(gfx2_surf_t *s, void *ctx)
{
    mapa_caja_t c = *caja();
    int16_t px, py;

    (void)ctx;
    gfx2_fill(s, HUECO_X, HUECO_Y, HUECO_W, HUECO_H, gfx2_rgb(0x06101AUL));
    mapa_dibuja(s, &c);

    if (ft8_modo_activo()) {
        uint8_t n = 0U, r;

        while (n < (uint8_t)FT8_MODO_LINEAS && ft8_modo_linea(n)[0] != '\0') { n++; }
        for (r = n; r > 0U; r--) {
            char loc[8];
            const char *l = ft8_modo_linea((uint8_t)(r - 1U));

            if (!ft8_se_ve(l)) { continue; }
            if (!mapa_loc_de_linea(l, loc)) { continue; }
            if (!mapa_loc_a_px(&c, loc, &px, &py)) { continue; }
            mapa_marca(s, &c, px, py, (uint8_t)(r <= 3U), marca_color((uint8_t)(r <= 3U)));
        }
    } else if (hfdl_modo_activo()) {
        uint32_t n = hfdl_modo_aviones(), i;

        for (i = n; i > 0UL; i--) {
            int32_t la, lo;

            if (!hfdl_modo_pos(i - 1UL, &la, &lo)) { continue; }
            if (!mapa_e4_a_px(&c, lo, la, &px, &py)) { continue; }
            mapa_marca(s, &c, px, py, (uint8_t)(i <= 3UL), marca_color((uint8_t)(i <= 3UL)));
        }
    } else if (ais_modo_activo()) {
        uint32_t n = (uint32_t)ais_modo_barcos(), i;

        for (i = n; i > 0UL; i--) {
            int32_t la, lo;

            if (!ais_modo_pos(i - 1UL, &la, &lo)) { continue; }
            if (!mapa_e4_a_px(&c, lo, la, &px, &py)) { continue; }
            mapa_marca(s, &c, px, py, (uint8_t)(i <= 3UL), marca_color((uint8_t)(i <= 3UL)));
        }
    } else if (wspr_modo_activo()) {
        uint8_t n = (uint8_t)wspr_modo_lineas(), r;

        if (n > (uint8_t)WSPR_MODO_LINEAS) { n = (uint8_t)WSPR_MODO_LINEAS; }
        for (r = n; r > 0U; r--) {
            char loc[8];
            const char *l = wspr_modo_linea((uint32_t)(r - 1U));

            if (!mapa_loc_de_campo(l, 3U, loc)) { continue; }
            if (!mapa_loc_a_px(&c, loc, &px, &py)) { continue; }
            mapa_marca(s, &c, px, py, (uint8_t)(r <= 3U), marca_color((uint8_t)(r <= 3U)));
        }
    } else if (jtty_modo_activo()) {
        uint8_t n = jtty_modo_lineas(), r;

        if (n > (uint8_t)JTTY_MODO_LINEAS) { n = (uint8_t)JTTY_MODO_LINEAS; }
        for (r = n; r > 0U; r--) {
            char loc[8];
            const char *l = jtty_modo_linea((uint8_t)(n - r));

            if (!mapa_loc_de_campo(l, 4U, loc)) { continue; }
            if (!mapa_loc_a_px(&c, loc, &px, &py)) { continue; }
            mapa_marca(s, &c, px, py, (uint8_t)(r <= 3U), marca_color((uint8_t)(r <= 3U)));
        }
    }

    {
        const char *mio = ft8_decoder_get_own_grid();

        if (mio != 0 && mio[0] != '\0' && mapa_loc_a_px(&c, mio, &px, &py)) {
            mapa_casa(s, &c, px, py);
        }
    }
    mapa_eti_dibuja(s);
    mapa_zoom_botones(s, &c);           /* last: over everything */
    mapa_eti_ficha(s, s_ahora);
}

/* Upstream's campo_n(), campo_i16(), campo_hhmm(): a row's fields, tab
 * separated, the first is 1. */
static const char *campo_n(const char *l, uint8_t n)
{
    uint8_t tabs = 1U;

    if (l == 0) { return ""; }
    while (*l != '\0' && tabs < n) {
        if (*l == '\t') { tabs++; }
        l++;
    }
    return (tabs == n) ? l : "";
}

static int16_t campo_i16(const char *l, uint8_t n)
{
    const char *p = campo_n(l, n);
    int16_t v = 0, sg = 1;

    while (*p == ' ') { p++; }
    if (*p == '-') { sg = -1; p++; }
    else if (*p == '+') { p++; }
    while (*p >= '0' && *p <= '9') {
        v = (int16_t)(v * 10 + (int16_t)(*p - '0'));
        if (v > 30000) { v = 30000; }
        p++;
    }
    return (int16_t)(sg * v);
}

static uint16_t campo_hhmm(const char *l, uint8_t n)
{
    const char *p = campo_n(l, n);
    uint16_t h, m;

    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') { return 0xFFFFU; }
    if (p[2] != ':') { return 0xFFFFU; }
    if (p[3] < '0' || p[3] > '9' || p[4] < '0' || p[4] > '9') { return 0xFFFFU; }
    h = (uint16_t)((p[0] - '0') * 10 + (p[1] - '0'));
    m = (uint16_t)((p[3] - '0') * 10 + (p[4] - '0'));
    if (h > 23U || m > 59U) { return 0xFFFFU; }
    return (uint16_t)(h * 60U + m);
}

/* Upstream's mapa_eti_llena(): the labels - FT8's and WSPR's calls, HFDL's
 * aircraft with their card's extra line. */
static void eti_llena(const mapa_caja_t *c)
{
    const char *mio = ft8_decoder_get_own_grid();
    mapa_eti_dx_t dx;
    uint8_t i, n = 0U;

    memset(&dx, 0, sizeof dx);
    mapa_eti_empieza(c, mio);

    if (ft8_modo_activo()) {
        while (n < (uint8_t)FT8_MODO_LINEAS && ft8_modo_linea(n)[0] != '\0') { n++; }
        for (i = 0U; i < n; i++) {
            const char *l = ft8_modo_linea(i);

            if (!ft8_se_ve(l)) { continue; }
            if (!mapa_loc_de_linea(l, dx.loc)) { continue; }
            if (!mapa_ind_de_linea(l, dx.call)) { continue; }
            dx.snr = campo_i16(l, 3U);
            dx.minuto = campo_hhmm(l, 1U);
            (void)mapa_eti_pon(&dx);
        }
    } else if (wspr_modo_activo()) {
        n = (uint8_t)wspr_modo_lineas();
        if (n > (uint8_t)WSPR_MODO_LINEAS) { n = (uint8_t)WSPR_MODO_LINEAS; }
        for (i = 0U; i < n; i++) {
            const char *l = wspr_modo_linea((uint32_t)i);
            const char *cl = campo_n(l, 2U);
            uint8_t k = 0U;

            if (!mapa_loc_de_campo(l, 3U, dx.loc)) { continue; }
            while (cl[k] != '\0' && cl[k] != '\t' && cl[k] != ' '
                   && k < (uint8_t)(MAPA_ETI_CALL - 1U)) {
                dx.call[k] = cl[k];
                k++;
            }
            dx.call[k] = '\0';
            if (k < 3U) { continue; }
            dx.snr = campo_i16(l, 4U);          /* WSPR: dBm of POWER */
            dx.minuto = campo_hhmm(l, 1U);
            (void)mapa_eti_pon(&dx);
        }
    } else if (hfdl_modo_activo()) {
        uint32_t a, na = hfdl_modo_aviones();
        char ficha[32];

        for (a = 0UL; a < na; a++) {
            int32_t la, lo;

            if (!hfdl_modo_pos(a, &la, &lo)) { continue; }
            if (!hfdl_modo_ident(a, dx.call, (uint8_t)MAPA_ETI_CALL)) { continue; }
            if (!hfdl_modo_ficha(a, ficha, (uint8_t)sizeof ficha)) { ficha[0] = '\0'; }
            dx.extra   = ficha;
            dx.loc[0]  = '\0';                  /* an aircraft has no square */
            dx.hay_pos = 1U;
            dx.avion   = 1U;
            dx.lon_c   = lo / 100L;
            dx.lat_c   = la / 100L;
            dx.snr     = 0;
            dx.minuto  = 0U;
            (void)mapa_eti_pon(&dx);
        }
    }
    mapa_eti_cierra();
}

void digi_mapa_dibuja(void)
{
    rtc_hw_datetime_t dt;

    rtc_hw_get(&dt);
    s_ahora = (uint16_t)((uint16_t)dt.hour * 60U + (uint16_t)dt.minute);
    (void)mapa_ventana(caja());         /* once a frame, never per band */
    eti_llena(caja());
    gfx2_render(HUECO_X, HUECO_Y, HUECO_W, HUECO_H, pinta, 0);
    s_sucio = 0U;
}

void digi_mapa_sucio(void) { s_sucio = 1U; }

void digi_mapa_poll(void)
{
    if (s_sucio) { digi_mapa_dibuja(); }
}

void digi_mapa_zoom(int32_t pasos)
{
    mapa_caja_t *c = caja();
    uint8_t antes = c->esc;

    (void)mapa_zoom_paso(c, pasos, (int16_t)(c->x + c->w / 2), (int16_t)(c->y + c->h / 2));
    if (c->esc != antes) { digi_mapa_dibuja(); }
}

uint8_t digi_mapa_toque(uint16_t x, uint16_t y, uint8_t nuevo)
{
    if (nuevo) {
        s_zoom_tap = 0U;
        s_mapa_tap = 0U;
        if (!digi_mapa_en_zona(x, y)) { return 0U; }
        s_zoom_tap = mapa_zoom_hit(caja(), (int16_t)x, (int16_t)y);
        if (!s_zoom_tap) {
            s_mapa_tap = 1U;                    /* drag or pick */
            mapa_gesto_empieza(&s_gesto, (int16_t)x, (int16_t)y);
        }
        return 1U;
    }
    if (s_mapa_tap) {
        int16_t dx = 0, dy = 0;

        if (mapa_gesto_mueve(&s_gesto, (int16_t)x, (int16_t)y, &dx, &dy)) {
            mapa_arrastra(caja(), dx, dy);
            s_sucio = 1U;                       /* painted by the loop, once */
        }
        return 1U;
    }
    return (uint8_t)(s_zoom_tap != 0U);
}

void digi_mapa_suelta(void)
{
    if (s_zoom_tap) {
        /* upstream's: +1 or -1 at the box's centre, on the release */
        mapa_caja_t *c = caja();
        uint8_t antes = c->esc;

        (void)mapa_zoom_paso(c, (s_zoom_tap == 1U) ? 1 : -1,
                             (int16_t)(c->x + c->w / 2), (int16_t)(c->y + c->h / 2));
        if (c->esc != antes) { digi_mapa_dibuja(); }
        console_printf("  map: zoom x%u\n", (uint32_t)c->esc);
    } else if (s_mapa_tap) {
        int16_t tx = 0, ty = 0;

        if (mapa_gesto_suelta(&s_gesto, &tx, &ty) && mapa_eti_toca(tx, ty)) {
            digi_mapa_dibuja();
            console_printf("  map: card %s\n", mapa_eti_sel() ? mapa_eti_sel() : "-");
        }
    }
    s_zoom_tap = 0U;
    s_mapa_tap = 0U;
}

/* Upstream's mapa_al_arrancar() and mapa_graba(), with BD.BIN's prompt. */
uint8_t digi_mapa_al_arrancar(void (*avisa)(const char *titulo, const char *linea))
{
    char     n8[8], linea[40];
    uint8_t *pozo;
    uint32_t cap = 0UL;
    uint8_t  hay_fich;
    mapa_bin_r_t r;
    const char *tit = tr("Mapa del mundo", "World map");

    hay_fich = spi_flash_dir_primero_ext("MAP", n8);
    if (!hay_fich && !mapa_flash_hay(0)) {
        console_printf("Map       : the built-in coast (no .map in the high zone)\n");
        return 0U;
    }

    hfdl_ram_coge();
    pozo = hfdl_ram_todo(&cap);
    if (pozo == 0 || cap < (uint32_t)(MAPA_BIN_RAM + 512U)) {
        hfdl_ram_suelta();
        console_printf("Map       : no work buffer (%u bytes)\n", cap);
        return 0U;
    }

    if (hay_fich) {
        mapa_flash_r_t g;
        uint32_t total, ultimo = 0xFFFFFFFFUL;
        int8_t   paso;

        {
            char nom[9];
            uint8_t k;

            for (k = 0U; k < 8U && n8[k] != ' '; k++) { nom[k] = n8[k]; }
            nom[k] = '\0';
            console_printf("Map       : %s.MAP on the disk - into the high zone\n", nom);
        }
        if (avisa) { avisa(tit, tr("Del disco a la flash", "From the disk to the flash")); }
        g = mapa_flash_graba_abre(n8);
        if (g == MAPA_FLASH_OK) {
            total = mapa_flash_graba_total();
            do {
                paso = mapa_flash_graba_paso();
                if (total != 0UL) {
                    uint32_t pct = ((mapa_flash_graba_hechos() * 100UL) / total) / 5U * 5U;

                    if (pct != ultimo && avisa) {
                        uint32_t k = 0U;
                        const char *q = tr("Copiando ", "Copying ");

                        ultimo = pct;
                        while (*q != '\0' && k < sizeof linea - 6U) { linea[k++] = *q++; }
                        if (pct >= 100U) { linea[k++] = '1'; linea[k++] = '0'; linea[k++] = '0'; }
                        else {
                            if (pct >= 10U) { linea[k++] = (char)('0' + pct / 10U); }
                            linea[k++] = (char)('0' + pct % 10U);
                        }
                        linea[k++] = ' '; linea[k++] = '%'; linea[k] = '\0';
                        avisa(tit, linea);
                    }
                }
            } while (paso > 0);
            if (paso != 0) {
                g = MAPA_FLASH_COPIA_MAL;
            } else {
                if (avisa) { avisa(tit, tr("Comprobando", "Checking")); }
                g = mapa_flash_graba_cierra(n8, pozo, cap);
            }
        }
        console_printf("Map       : %s\n", mapa_flash_porque_txt(g));
        if (avisa) { avisa(tit, mapa_flash_porque_txt(g)); }
        board_delay_ms((g == MAPA_FLASH_OK) ? 1500U : 3000U);
    }

    r = mapa_flash_carga(pozo, cap);
    hfdl_ram_suelta();
    console_printf("Map       : %s (verdict %u)\n",
                   (r == MAPA_BIN_OK) ? "a catalogue map in force" : "the built-in coast",
                   (uint32_t)r);
    return (uint8_t)(r == MAPA_BIN_OK);
}
