#include <string.h>
#include "menus.h"
#include "ui_cfg.h"        /* ../../User - upstream */
#include "ui_grid.h"       /* ../../User - upstream */
#include "gfx2.h"
#include "gfx2_font.h"
#include "console.h"
#include "demod_am.h"      /* DEMOD_MODE_* */
#include "bandas_tabla.h"  /* GENERATED from ../../User/main.c, see reglas.mk */

/* What ui_cfg.c draws the range line in, defined once in fonts_menu.c. */
extern const gfx2_font_t font_ui_10;

static menu_t          s_open;
static ui_cfg_state_t  s_cfg;
static ui_grid_state_t s_grid;
static int8_t          s_pressed = -1;   /* the hit index under the finger */

/* ------------------------------------------------------------------ */
/* MODES                                                               */
/* ------------------------------------------------------------------ */

/*
 * The analogue family of upstream's mode screen, without WFM: upstream's
 * WFM path demodulates the station at 0 Hz and this port keeps it 12 kHz
 * off the LO (am.h), so it is not offered until that is reconciled. The
 * digital families need their decoders, which are switched off
 * (decoders_off.c). The descriptions are upstream's, except AM's:
 * "Amplitud modulada" is 91 px in a 92 px cell, measured by the host
 * render (deepsdr-ui `make malahit-menus`).
 */
static const struct { int mode; const char *n; const char *d; } k_modos[] = {
    { DEMOD_MODE_AM,  "AM",  "Envolvente" },
    { DEMOD_MODE_SAM, "SAM", "AM síncrona" },
    { DEMOD_MODE_USB, "USB", "Banda lateral alta" },
    { DEMOD_MODE_LSB, "LSB", "Banda lateral baja" },
    { DEMOD_MODE_NFM, "NFM", "FM estrecha" },
};
#define N_MODOS ((uint8_t)(sizeof k_modos / sizeof k_modos[0]))

static void modos_rellena(void)
{
    uint8_t i;
    int     m = radio_mode_get();

    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.cat[0]  = "Analógico";
    s_cfg.cats    = 1U;
    s_cfg.cat_sel = 0U;
    s_cfg.denso   = 1U;
    s_cfg.n       = N_MODOS;
    s_cfg.marcada = 0xFFU;
    s_cfg.cursor  = -1;
    s_cfg.pressed = -1;
    for (i = 0U; i < N_MODOS; i++) {
        s_cfg.cel[i].nombre = k_modos[i].n;
        s_cfg.cel[i].valor  = k_modos[i].d;
        s_cfg.cel[i].extra  = "";
        if (k_modos[i].mode == m) { s_cfg.marcada = i; }
    }
}

/* ------------------------------------------------------------------ */
/* BANDS                                                               */
/* ------------------------------------------------------------------ */

/*
 * Upstream's whole table is generated in; what this radio can tune is
 * main.c's TUNE_MIN_HZ..TUNE_MAX_HZ, so a band outside it is left out, and
 * so is a family left with none. radio_band_go() would clamp the dial
 * instead, which is a band button that tunes somewhere else.
 */
#define BAND_LO_HZ  100000UL
#define BAND_HI_HZ  30000000UL

static uint8_t s_fam_of_cat[UIC_CATS];   /* visible category -> family */
static uint8_t s_band_of_cel[UIC_CELLS]; /* cell -> index in banda[] */
static uint8_t s_fam_sel;                /* the family last shown */
static int     s_band_last = -1;         /* the band last chosen */
static char    s_rango[UIC_CELLS][24];

static int banda_ok(int k)
{
    return banda[k].hz >= BAND_LO_HZ && banda[k].hz <= BAND_HI_HZ;
}

/*
 * WHICH BAND THE DIAL IS IN - upstream's banda_indice(), the same rule:
 * the NARROWEST band whose edges hold the frequency, because the ranges
 * overlap on purpose (40 m ends where 41 m begins, and at the edge the
 * small one says more). A preset with no edges - "Horaria", three
 * frequencies - is not a range and is never the answer.
 */
int menu_band_at(uint32_t hz)
{
    int      k, best = -1;
    uint32_t w_best = UINT32_MAX;

    for (k = 0; k < N_BANDAS; k++) {
        uint32_t w;

        if (banda[k].lo == 0UL && banda[k].hi == 0UL) { continue; }
        if (hz < banda[k].lo || hz > banda[k].hi)    { continue; }
        w = (uint32_t)(banda[k].hi - banda[k].lo);
        if (w < w_best) { w_best = w; best = k; }
    }
    return best;
}

const char *menu_band_name(int k)
{
    return (k >= 0 && k < N_BANDAS) ? banda[k].nombre : (const char *)0;
}

static int modo_de(const char *s)
{
    if (!strcmp(s, "AM"))  { return DEMOD_MODE_AM; }
    if (!strcmp(s, "SAM")) { return DEMOD_MODE_SAM; }
    if (!strcmp(s, "USB")) { return DEMOD_MODE_USB; }
    if (!strcmp(s, "LSB")) { return DEMOD_MODE_LSB; }
    if (!strcmp(s, "NFM")) { return DEMOD_MODE_NFM; }
    return -1;                                  /* WFM: not here yet */
}

/*
 * The range line is upstream's text, and a few are 1-2 px wider than this
 * cell ("26,965-27,405 MHz", 92 px in 92). ui_cfg.c would clip them in
 * silence, so the unit goes instead - the band's own name says the rest.
 */
static const char *rango_cabe(uint8_t cel, const char *r)
{
    size_t n = strlen(r);
    char  *o = s_rango[cel];

    if (gfx2_text_w(r, &font_ui_10) <= (int16_t)(UIC_DCELL_W - 2)) {
        return r;
    }
    if (n >= sizeof s_rango[0]) { n = sizeof s_rango[0] - 1U; }
    memcpy(o, r, n);
    o[n] = '\0';
    if (n > 4U && (!strcmp(&o[n - 4U], " kHz") || !strcmp(&o[n - 4U], " MHz"))) {
        o[n - 4U] = '\0';
    }
    return o;
}

static void bandas_rellena(void)
{
    uint8_t c = 0U, n = 0U, f;
    int     k;

    memset(&s_cfg, 0, sizeof s_cfg);

    for (f = 0U; f < (uint8_t)(sizeof familia_nombre / sizeof familia_nombre[0])
                 && c < UIC_CATS; f++) {
        for (k = 0; k < N_BANDAS; k++) {
            if (banda[k].familia == f && banda_ok(k)) { break; }
        }
        if (k == N_BANDAS) { continue; }        /* nothing tunable in it */
        s_fam_of_cat[c] = f;
        s_cfg.cat[c]    = familia_nombre[f];
        if (f == s_fam_sel) { s_cfg.cat_sel = c; }
        c++;
    }
    s_cfg.cats    = c;
    s_cfg.denso   = 1U;
    s_cfg.marcada = 0xFFU;
    s_cfg.cursor  = -1;
    s_cfg.pressed = -1;
    s_fam_sel     = s_fam_of_cat[s_cfg.cat_sel];

    for (k = 0; k < N_BANDAS && n < UIC_CELLS; k++) {
        if (banda[k].familia != s_fam_sel || !banda_ok(k)) { continue; }
        s_cfg.cel[n].nombre = banda[k].nombre;
        s_cfg.cel[n].valor  = rango_cabe(n, banda[k].rango);
        s_cfg.cel[n].extra  = banda[k].modo;
        s_band_of_cel[n]    = (uint8_t)k;
        if (k == menu_band_at(radio_dial_hz())) { s_cfg.marcada = n; }
        n++;
    }
    s_cfg.n = n;
}

/* The family to open on: the band last chosen, or the one whose centre is
 * nearest the dial. */
static void bandas_familia_inicial(void)
{
    uint32_t dial = radio_dial_hz(), best = UINT32_MAX;
    int      k;

    {
        int here = menu_band_at(dial);

        if (here >= 0) { s_fam_sel = (uint8_t)banda[here].familia; return; }
    }
    if (s_band_last >= 0) { s_fam_sel = (uint8_t)banda[s_band_last].familia; return; }
    for (k = 0; k < N_BANDAS; k++) {
        uint32_t d;

        if (!banda_ok(k)) { continue; }
        d = (banda[k].hz > dial) ? (uint32_t)(banda[k].hz - dial)
                                 : (uint32_t)(dial - banda[k].hz);
        if (d < best) { best = d; s_fam_sel = (uint8_t)banda[k].familia; }
    }
}

/* ------------------------------------------------------------------ */
/* STEPS                                                               */
/* ------------------------------------------------------------------ */

/* The steps are upstream's TUNE_STEPS_LISTA, generated into
 * bandas_tabla.h with the bands: value, label and what each is for. */

static void pasos_rellena(void)
{
    uint32_t i, n = radio_step_count();

    memset(&s_grid, 0, sizeof s_grid);
    if (n > UIG_CELLS) { n = UIG_CELLS; }
    s_grid.titulo  = "Paso de sintonía";
    s_grid.pagina  = 0U;
    s_grid.paginas = 1U;
    s_grid.n       = (uint8_t)n;
    s_grid.pressed = -1;
    s_grid.cursor  = -1;
    s_grid.marcada = 0xFFU;
    s_grid.nav[0]  = "Anterior";
    s_grid.nav[1]  = "Siguiente";
    for (i = 0U; i < n; i++) {
        s_grid.cel[i].l1 = paso_tabla[i].rotulo;
        s_grid.cel[i].l2 = paso_tabla[i].desc;
        if (i == radio_step_get()) { s_grid.marcada = (uint8_t)i; }
    }
}

/* ------------------------------------------------------------------ */

menu_t menu_open_id(void) { return s_open; }

static void dibuja(void)
{
    if (s_open == MENU_PASO) { ui_grid_draw(&s_grid); }
    else                     { ui_cfg_draw(&s_cfg); }
}

void menu_close(void)
{
    if (s_open == MENU_NONE) { return; }
    s_open    = MENU_NONE;
    s_pressed = -1;
    radio_screen_restore();
    radio_act_refresh();
}

void menu_toggle(menu_t m)
{
    if (s_open == m) { menu_close(); return; }

    s_pressed = -1;
    switch (m) {
    case MENU_MODO:   modos_rellena(); break;
    case MENU_BANDAS: bandas_familia_inicial(); bandas_rellena(); break;
    case MENU_PASO:   pasos_rellena(); break;
    default:          menu_close(); return;
    }
    s_open = m;
    dibuja();
    radio_act_refresh();
    console_printf("  menu -> %s\n", m == MENU_MODO ? "Modo"
                                   : m == MENU_BANDAS ? "Bandas" : "Paso");
}

int menu_touch_down(uint16_t x, uint16_t y, int nuevo)
{
    int8_t h;

    if (s_open == MENU_NONE) { return 0; }
    if (y < UIG_Y || y >= UIG_Y + UIG_H) { return 0; }
    if (!nuevo) { return 1; }

    h = (s_open == MENU_PASO) ? ui_grid_hit(x, y) : ui_cfg_hit(x, y);
    if (h < 0) { return 1; }

    /* A cell beyond the ones in use is background, not a target. */
    if (s_open == MENU_PASO) {
        if (h < UIG_CELLS && h >= (int8_t)s_grid.n) { return 1; }
        if (h >= UIG_CELLS) { return 1; }        /* one page: nav is off */
        s_grid.pressed = h;
        ui_grid_draw_one(&s_grid, h);
    } else {
        if (h < UIC_HIT_CAT0 && h >= (int8_t)s_cfg.n) { return 1; }
        if (h >= UIC_HIT_CAT0 && h - UIC_HIT_CAT0 >= (int8_t)s_cfg.cats) { return 1; }
        s_cfg.pressed = h;
        ui_cfg_draw_one(&s_cfg, h);
    }
    s_pressed = h;
    return 1;
}

static void elige(int8_t h);

void menu_touch_up(void)
{
    int8_t h = s_pressed;

    if (h < 0 || s_open == MENU_NONE) { return; }
    s_pressed = -1;
    elige(h);
}

/*
 * What a cell, or a category, does - from a finger or from the knob.
 */
static void elige(int8_t h)
{
    if (s_open == MENU_PASO) {
        s_grid.pressed = -1;
        radio_step_set((uint32_t)h);
        console_printf("  menu: paso %s\n", paso_tabla[h].rotulo);
        menu_close();
        return;
    }

    s_cfg.pressed = -1;

    if (h >= UIC_HIT_CAT0) {
        /* Another family: the same screen with other cells. */
        s_fam_sel = s_fam_of_cat[h - UIC_HIT_CAT0];
        if (s_open == MENU_BANDAS) { bandas_rellena(); }
        ui_cfg_draw(&s_cfg);
        return;
    }
    if (h >= (int8_t)s_cfg.n) { return; }

    if (s_open == MENU_MODO) {
        radio_mode_set(k_modos[h].mode);
        console_printf("  menu: modo %s\n", k_modos[h].n);
        menu_close();
        return;
    }

    if (s_open == MENU_BANDAS) {
        int k = s_band_of_cel[h];
        int m = modo_de(banda[k].modo);

        s_band_last = k;
        /* The band's own step too, as upstream's band button does. */
        if (banda[k].paso >= 0 && banda[k].paso < N_PASOS) {
            radio_step_set((uint32_t)banda[k].paso);
        }
        if (!radio_band_go((uint32_t)banda[k].hz, m, banda[k].nombre)) {
            console_printf("  menu: banda %s - el dial no llega a %u Hz\n",
                           banda[k].nombre, (uint32_t)banda[k].hz);
        } else {
            console_printf("  menu: banda %s, %u Hz, %s%s\n", banda[k].nombre,
                           (uint32_t)banda[k].hz, banda[k].modo,
                           m < 0 ? " (no disponible: el modo no cambia)" : "");
        }
        menu_close();
    }
}

/*
 * THE KNOB IN A MENU - upstream's cursor: the thick outline both engines
 * already draw for `cursor`, distinct from the tinted "this is the one in
 * force" cell. Turning moves it; on the band screen it runs on into the
 * next family and back, so every band is reachable without a finger.
 * Pressing the knob chooses what it points at.
 */
static int8_t cursor_ini(void)
{
    if (s_open == MENU_PASO) {
        return (s_grid.marcada != 0xFFU) ? (int8_t)s_grid.marcada : 0;
    }
    return (s_cfg.marcada != 0xFFU) ? (int8_t)s_cfg.marcada : 0;
}

void menu_knob(int32_t d)
{
    int8_t was, now;

    if (s_open == MENU_NONE || d == 0) { return; }

    if (s_open == MENU_PASO) {
        was = s_grid.cursor;
        now = (int8_t)((was < 0 ? cursor_ini() : was + d));
        if (now < 0) { now = 0; }
        if (now >= (int8_t)s_grid.n) { now = (int8_t)(s_grid.n - 1U); }
        s_grid.cursor = now;
        if (was >= 0 && was != now) { ui_grid_draw_one(&s_grid, was); }
        ui_grid_draw_one(&s_grid, now);
        return;
    }

    was = s_cfg.cursor;
    {
        int32_t t = (was < 0) ? cursor_ini() : (int32_t)was + d;

        if (s_open == MENU_BANDAS && (t >= (int32_t)s_cfg.n || t < 0)
            && s_cfg.cats > 1U) {
            uint8_t c = s_cfg.cat_sel;

            if (t >= (int32_t)s_cfg.n && c + 1U < s_cfg.cats)   { c++; t = 0; }
            else if (t < 0 && c > 0U)                          { c--; t = 999; }
            if (c != s_cfg.cat_sel) {
                s_fam_sel = s_fam_of_cat[c];
                bandas_rellena();
                if (t > (int32_t)s_cfg.n - 1) { t = (int32_t)s_cfg.n - 1; }
                s_cfg.cursor = (int8_t)t;
                ui_cfg_draw(&s_cfg);
                return;
            }
        }
        if (t < 0) { t = 0; }
        if (t >= (int32_t)s_cfg.n) { t = (int32_t)s_cfg.n - 1; }
        now = (int8_t)t;
    }
    s_cfg.cursor = now;
    if (was >= 0 && was != now) { ui_cfg_draw_one(&s_cfg, was); }
    ui_cfg_draw_one(&s_cfg, now);
}

void menu_knob_press(void)
{
    int8_t c;

    if (s_open == MENU_NONE) { return; }
    c = (s_open == MENU_PASO) ? s_grid.cursor : s_cfg.cursor;
    if (c < 0) { c = cursor_ini(); }
    elige(c);
}
