#include <string.h>
#include "menus.h"
#include "ui_cfg.h"        /* ../../User - upstream */
#include "ui_grid.h"       /* ../../User - upstream */
#include "gfx2.h"
#include "gfx2_font.h"
#include "console.h"
#include "demod_am.h"      /* DEMOD_MODE_* */
#include "bandas_tabla.h"  /* GENERATED from ../../User/main.c, see reglas.mk */
#include "temas_tabla.h"   /* GENERATED from ../../User/main.c: the themes */
#include "spectrum.h"      /* ../../User - palette, style, bridge, trace */
#include "anotch.h"        /* ../../User - the automatic notch */
#include "nb.h"            /* ../../User - the noise blanker */
#include "nr_ss.h"         /* ../../User - the noise reduction */
#include "palette.h"       /* g_pal */
#include "settings.h"      /* src/ - the FRAM record, not upstream's */
#include "ui_det.h"        /* ../../User - upstream: one continuous setting */

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
/* SETTINGS                                                            */
/* ------------------------------------------------------------------ */

/*
 * Upstream's settings screen is a table, k_ajustes[] in its main.c: a
 * name, a page and an id, and two functions that give each id its value
 * and its action. This is the same shape with the settings this port
 * can actually honour - the ones whose module is grafted - on the same
 * pages under the same names, and the values in upstream's words. A tap
 * (or the knob's press on the cursor) steps an enumerated value, as
 * upstream's does; the two the small knob owns - ATT and volume - hand
 * it over instead. Not kept in the FRAM yet.
 */
typedef enum {
    AJ_AGC, AJ_BW, AJ_NOTCH, AJ_SQL, AJ_NR, AJ_NB, AJ_PRE, AJ_ATT,
    AJ_TEMA, AJ_PALETA, AJ_ESTILO, AJ_VOL,
    AJ_PUENTE, AJ_TRAZA
} aj_id_t;

enum { AJ_PAG_RADIO = 0, AJ_PAG_PANTALLA, AJ_PAG_ESPECTRO, AJ_PAGS };

static const char *const k_aj_pags[AJ_PAGS] = { "Radio", "Pantalla", "Espectro" };

static const struct { const char *nombre; uint8_t pag; uint8_t id; } k_aj[] = {
    { "AGC",         AJ_PAG_RADIO,    AJ_AGC    },
    { "Ancho (BW)",  AJ_PAG_RADIO,    AJ_BW     },
    { "Notch",       AJ_PAG_RADIO,    AJ_NOTCH  },
    { "Silenciador", AJ_PAG_RADIO,    AJ_SQL    },
    { "Reducción",   AJ_PAG_RADIO,    AJ_NR     },
    { "Blanker",     AJ_PAG_RADIO,    AJ_NB     },
    { "Preamplif.",  AJ_PAG_RADIO,    AJ_PRE    },
    { "Atenuador",   AJ_PAG_RADIO,    AJ_ATT    },
    { "Tema",        AJ_PAG_PANTALLA, AJ_TEMA   },
    { "Paleta",      AJ_PAG_PANTALLA, AJ_PALETA },
    { "Estilo",      AJ_PAG_PANTALLA, AJ_ESTILO },
    { "Volumen",     AJ_PAG_PANTALLA, AJ_VOL    },
    { "Puente",      AJ_PAG_ESPECTRO, AJ_PUENTE },
    { "Traza",       AJ_PAG_ESPECTRO, AJ_TRAZA  },
};
#define N_AJ ((uint8_t)(sizeof k_aj / sizeof k_aj[0]))

/* Upstream's k_notch_nombres[] and k_nb_nombres[] (User/main.c). */
static const char *const k_notch_nom[4] = { "Suave", "Normal", "Fuerte", "A saco" };
static const char *const k_nb_nom[4]    = { "Apagado", "Suave", "Medio", "Fuerte" };

/*
 * THE DETAIL PAGE - upstream's ui_det.c, for the four continuous ones:
 * "-" and "+" either side of the value, the knob turning it, the knob's
 * press or "Volver" back to the grid. Opened from a cell, and part of the
 * settings screen: the Ajustes button stays lit, and pressing it from
 * here goes back to the grid, as upstream's does.
 *
 * Squelch: upstream's -10..60 dB in 2 dB steps (SQUELCH_DB_* in its
 * main.c). NR strength: 0..4080 in 16s - upstream's 0..4095 in 10s,
 * moved onto the grid the FRAM stores it on (settings.h).
 */
enum { DET_NINGUNO = 0, DET_SQL, DET_NR, DET_VOL, DET_ATT };

#define SQL_FLOOR_DB   (-10)
#define SQL_CEIL_DB      60
#define SQL_STEP_DB       2
#define NR_STEP          16U
#define NR_MAX         4080U

static uint8_t        s_det;             /* DET_*: which one is up, 0 = grid */
static ui_det_state_t s_dst;
static char           s_det_val[16];
/*
 * Upstream boots with 50 (s_nr_strength in its main.c); nr_ss_init()
 * leaves 0, which nr_ss.c documents as a transparent bypass. This port
 * never set it - so until now the Ruido button switched on an NR that
 * did nothing. 48 is 50 on the 16 grid.
 */
static uint16_t       s_nr_strength = 48U;

static uint8_t s_aj_pag;
static uint8_t s_aj_of_cel[UIC_CELLS];
static uint8_t s_tema;                   /* index into k_sim_temas[]; 0 at boot */

static char s_aj_buf[16];

/* "-10 dB", "48": a signed number and an optional unit. */
static const char *num_txt(char *o, int32_t v, const char *unit)
{
    char     d[10];
    uint32_t n = (uint32_t)((v < 0) ? -v : v), k = 0U, i = 0U;

    if (v < 0) { o[i++] = '-'; }
    do { d[k++] = (char)('0' + n % 10U); n /= 10U; } while (n);
    while (k) { o[i++] = d[--k]; }
    while (unit && *unit) { o[i++] = *unit++; }
    o[i] = '\0';
    return o;
}

static const char *aj_valor(uint8_t id)
{
    switch (id) {
    case AJ_SQL:    return num_txt(s_aj_buf, (int32_t)demod_am_get_squelch_db(), " dB");
    case AJ_AGC:    return radio_agc_text();
    case AJ_BW:     return radio_bw_text();
    case AJ_NOTCH:  return anotch_activo() ? k_notch_nom[anotch_fuerza() & 3U]
                                           : "Apagado";
    case AJ_NR:     return num_txt(s_aj_buf + 8, (int32_t)s_nr_strength, 0);
    case AJ_NB:     return k_nb_nom[nb_get_nivel() & 3U];
    case AJ_PRE:    return radio_preamp() ? "Activo" : "Apagado";
    case AJ_ATT:    return radio_att_text();
    case AJ_TEMA:   return k_sim_temas[s_tema].nombre;
    case AJ_PALETA: return spectrum_palette_nombre((uint8_t)spectrum_get_palette());
    case AJ_ESTILO:
        switch (spectrum_get_style()) {
        case SPECTRUM_STYLE_HEATMAP: return "Relleno";
        case SPECTRUM_STYLE_LINE:    return "Línea";
        default:                     return "Contorno";
        }
    case AJ_VOL:    return radio_vol_text();
    case AJ_PUENTE: return spectrum_get_bridge() ? "Unido" : "Suelto";
    case AJ_TRAZA:  return spectrum_get_heatmap_trace_white() ? "Blanca" : "Del color";
    default:        return "";
    }
}

static void ajustes_rellena(void)
{
    uint8_t i, n = 0U;

    memset(&s_cfg, 0, sizeof s_cfg);
    for (i = 0U; i < AJ_PAGS; i++) { s_cfg.cat[i] = k_aj_pags[i]; }
    s_cfg.cats    = AJ_PAGS;
    s_cfg.cat_sel = s_aj_pag;
    s_cfg.denso   = 0U;                 /* 3 x 3: name small, value big */
    s_cfg.marcada = 0xFFU;
    s_cfg.cursor  = -1;
    s_cfg.pressed = -1;
    for (i = 0U; i < N_AJ && n < (uint8_t)(UIC_COLS * UIC_ROWS); i++) {
        if (k_aj[i].pag != s_aj_pag) { continue; }
        s_cfg.cel[n].nombre = k_aj[i].nombre;
        s_cfg.cel[n].valor  = aj_valor(k_aj[i].id);
        s_cfg.cel[n].extra  = 0;
        s_aj_of_cel[n]      = i;
        n++;
    }
    s_cfg.n = n;
}

static void menu_dibuja(void);

static void det_fmt(void)
{
    s_dst.pie = 0;
    switch (s_det) {
    case DET_SQL:
        s_dst.titulo = "Silenciador";
        s_dst.valor  = num_txt(s_det_val, (int32_t)demod_am_get_squelch_db(), " dB");
        if ((int32_t)demod_am_get_squelch_db() <= SQL_FLOOR_DB) {
            s_dst.pie = "abajo del todo: abierto siempre";
        }
        break;
    case DET_NR:
        s_dst.titulo = "Reducción de ruido";
        s_dst.valor  = num_txt(s_det_val, (int32_t)s_nr_strength, 0);
        s_dst.pie    = nr_ss_get_enabled() ? "encendida (botón Ruido)"
                                           : "apagada: se enciende con Ruido";
        break;
    case DET_VOL:
        s_dst.titulo = "Volumen";
        s_dst.valor  = num_txt(s_det_val, (int32_t)radio_vol_db(), " dB");
        break;
    case DET_ATT:
        s_dst.titulo = "Atenuador";
        s_dst.valor  = num_txt(s_det_val, -(int32_t)radio_att_db(), " dB");
        s_dst.pie    = "reducción de ganancia del sintonizador";
        break;
    default:
        break;
    }
}

/* One knob detent or one press of "-"/"+": d = -1 or +1, or more. */
static void det_add(int32_t d)
{
    switch (s_det) {
    case DET_SQL: {
        int32_t v = (int32_t)demod_am_get_squelch_db() + d * SQL_STEP_DB;

        if (v < SQL_FLOOR_DB) { v = SQL_FLOOR_DB; }
        if (v > SQL_CEIL_DB)  { v = SQL_CEIL_DB; }
        demod_am_set_squelch_db((float)v);
        break;
    }
    case DET_NR: {
        int32_t v = (int32_t)s_nr_strength + d * (int32_t)NR_STEP;

        if (v < 0)               { v = 0; }
        if (v > (int32_t)NR_MAX) { v = (int32_t)NR_MAX; }
        s_nr_strength = (uint16_t)v;
        nr_ss_set_strength(s_nr_strength);
        break;
    }
    case DET_VOL: (void)radio_vol_set(radio_vol_db() + (int)d); break;
    /* "+" is more gain, so the shown "-59 dB" goes UP: less reduction. */
    case DET_ATT: (void)radio_att_set(radio_att_db() - (int)d); break;
    default: break;
    }
    det_fmt();
    ui_det_draw_valor(&s_dst);
}

static void det_open(uint8_t which)
{
    memset(&s_dst, 0, sizeof s_dst);
    s_dst.pressed   = -1;
    s_dst.preset_on = -1;
    s_det = which;
    det_fmt();
    ui_det_draw(&s_dst);
}

static void det_close(void)
{
    s_det = DET_NINGUNO;
    ajustes_rellena();
    ui_cfg_draw(&s_cfg);
}

void menu_refresh_det(void)
{
    if (s_open != MENU_AJUSTES || s_det == DET_NINGUNO) { return; }
    det_fmt();
    ui_det_draw_valor(&s_dst);
}

static void aj_toca(uint8_t id)
{
    switch (id) {
    case AJ_SQL:   det_open(DET_SQL); return;
    case AJ_AGC:   radio_agc_cycle(); break;
    case AJ_BW:    radio_bw_cycle();  break;
    case AJ_NOTCH:
        /* upstream's five: off, then the four strengths */
        if (!anotch_activo())            { anotch_set_fuerza(0U); anotch_set_activo(1U); }
        else if (anotch_fuerza() < 3U)   { anotch_set_fuerza((uint8_t)(anotch_fuerza() + 1U)); }
        else                             { anotch_set_activo(0U); }
        break;
    case AJ_NR:    det_open(DET_NR); return;
    case AJ_NB:    nb_set_nivel((uint8_t)((nb_get_nivel() + 1U) & 3U)); break;
    case AJ_PRE:   radio_preamp_toggle(); break;
    case AJ_ATT:   det_open(DET_ATT); return;
    case AJ_VOL:   det_open(DET_VOL); return;
    case AJ_TEMA:
        s_tema = (uint8_t)((s_tema + 1U) % (uint8_t)SIM_TEMA_COUNT);
        g_pal  = k_sim_temas[s_tema].pal;
        spectrum_set_palette(k_sim_temas[s_tema].wf);
        console_printf("  tema -> %s\n", k_sim_temas[s_tema].nombre);
        radio_repaint_all();            /* everything, in the new colours */
        ajustes_rellena();
        menu_dibuja();
        return;
    case AJ_PALETA:
        spectrum_set_palette((spectrum_palette_t)
            (((uint8_t)spectrum_get_palette() + 1U) % spectrum_palette_count()));
        break;
    case AJ_ESTILO:
        spectrum_set_style((spectrum_style_t)(((uint8_t)spectrum_get_style() + 1U) % 3U));
        break;
    case AJ_PUENTE: spectrum_set_bridge(spectrum_get_bridge() ? 0U : 1U); break;
    case AJ_TRAZA:
        spectrum_set_heatmap_trace_white(spectrum_get_heatmap_trace_white() ? 0U : 1U);
        break;
    default: break;
    }
}

/*
 * THE SETTINGS SCREEN IN THE FRAM - the extension of the settings record
 * (settings.c). Read from the modules that hold each value, so the record
 * says what is in force and not what this file thinks it set; applied in
 * two halves because the boot needs them at two different moments.
 */
void menu_ajustes_get(settings_t *s)
{
    uint8_t f = (uint8_t)(nb_get_nivel() & SET_AJ_NB_MASK);

    f |= (uint8_t)((anotch_fuerza() & 3U) << SET_AJ_NOTCH_F_SH);
    if (anotch_activo())                    { f |= SET_AJ_NOTCH_ON; }
    if (nr_ss_get_enabled())                { f |= SET_AJ_NR_ON; }
    if (spectrum_get_bridge())              { f |= SET_AJ_PUENTE; }
    if (spectrum_get_heatmap_trace_white()) { f |= SET_AJ_TRAZA_W; }

    s->has_aj    = 1U;
    s->aj_agc    = (uint8_t)demod_am_get_agc_profile();
    s->aj_estilo = (uint8_t)spectrum_get_style();
    s->aj_flags  = f;
    s->aj_tema   = s_tema;
    s->aj_paleta = (uint8_t)spectrum_get_palette();
    s->aj_bw     = radio_bw_pack();
    {
        int32_t q = ((int32_t)demod_am_get_squelch_db() - SQL_FLOOR_DB) / SQL_STEP_DB;

        s->aj_sql = (uint8_t)((q < 0) ? 0 : q);
    }
    s->aj_nr = (uint8_t)(s_nr_strength / NR_STEP);
}

/* What the screen shows - before the first paint, so a stored theme is
 * the first thing drawn rather than a repaint over the default. */
void menu_ajustes_apply_pantalla(const settings_t *s)
{
    if (!s->has_aj) { return; }
    if (s->aj_tema < (uint8_t)SIM_TEMA_COUNT) {
        s_tema = s->aj_tema;
        g_pal  = k_sim_temas[s_tema].pal;
        spectrum_set_palette(k_sim_temas[s_tema].wf);
    }
    /* After the theme: Paleta can be set apart from it, and was saved
     * as it was in force. */
    if (s->aj_paleta < spectrum_palette_count()) {
        spectrum_set_palette((spectrum_palette_t)s->aj_paleta);
    }
    if (s->aj_estilo < 3U) { spectrum_set_style((spectrum_style_t)s->aj_estilo); }
    spectrum_set_bridge((s->aj_flags & SET_AJ_PUENTE) ? 1U : 0U);
    spectrum_set_heatmap_trace_white((s->aj_flags & SET_AJ_TRAZA_W) ? 1U : 0U);
}

/* What the demodulator does - AFTER demod_am_init(), which resets the
 * notch and the NR. */
void menu_ajustes_apply_radio(const settings_t *s)
{
    /* The NR strength goes in even with nothing stored: demod_am_init()
     * has just put nr_ss at 0, the bypass. See s_nr_strength. */
    if (s->has_sqlnr) {
        uint32_t v = (uint32_t)s->aj_nr * NR_STEP;
        s_nr_strength = (uint16_t)((v > NR_MAX) ? NR_MAX : v);
    }
    nr_ss_set_strength(s_nr_strength);

    if (s->has_sqlnr) {
        int32_t db = SQL_FLOOR_DB + (int32_t)s->aj_sql * SQL_STEP_DB;

        if (db > SQL_CEIL_DB) { db = SQL_CEIL_DB; }
        demod_am_set_squelch_db((float)db);
    }

    if (!s->has_aj) { return; }
    if (s->aj_agc <= (uint8_t)AGC_PROFILE_FAST) {
        demod_am_set_agc_profile((agc_profile_t)s->aj_agc);
    }
    nb_set_nivel((uint8_t)(s->aj_flags & SET_AJ_NB_MASK));
    anotch_set_fuerza((uint8_t)((s->aj_flags >> SET_AJ_NOTCH_F_SH) & 3U));
    anotch_set_activo((s->aj_flags & SET_AJ_NOTCH_ON) ? 1U : 0U);
    nr_ss_set_enabled((s->aj_flags & SET_AJ_NR_ON) ? 1U : 0U);
    radio_bw_unpack(s->aj_bw);
}

const char *menu_tema_nombre(void) { return k_sim_temas[s_tema].nombre; }
uint32_t    menu_nr_strength(void) { return s_nr_strength; }

/* ------------------------------------------------------------------ */

menu_t menu_open_id(void) { return s_open; }

static void dibuja(void)
{
    if (s_open == MENU_PASO) { ui_grid_draw(&s_grid); }
    else                     { ui_cfg_draw(&s_cfg); }
}

static void menu_dibuja(void) { dibuja(); }

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
    /* From a detail page, its own button goes back to the grid. */
    if (s_open == MENU_AJUSTES && m == MENU_AJUSTES && s_det != DET_NINGUNO) {
        det_close();
        return;
    }
    s_det = DET_NINGUNO;
    if (s_open == m) { menu_close(); return; }

    s_pressed = -1;
    switch (m) {
    case MENU_MODO:   modos_rellena(); break;
    case MENU_BANDAS: bandas_familia_inicial(); bandas_rellena(); break;
    case MENU_PASO:   pasos_rellena(); break;
    case MENU_AJUSTES: ajustes_rellena(); break;
    default:          menu_close(); return;
    }
    s_open = m;
    dibuja();
    radio_act_refresh();
    console_printf("  menu -> %s\n", m == MENU_MODO ? "Modo"
                                   : m == MENU_BANDAS ? "Bandas"
                                   : m == MENU_PASO ? "Paso" : "Ajustes");
}

int menu_touch_down(uint16_t x, uint16_t y, int nuevo)
{
    int8_t h;

    if (s_open == MENU_NONE) { return 0; }
    if (y < UIG_Y || y >= UIG_Y + UIG_H) { return 0; }
    if (!nuevo) { return 1; }

    if (s_open == MENU_AJUSTES && s_det != DET_NINGUNO) {
        h = ui_det_hit(x, y);
        /* Only what is drawn: no presets and no LO/HI on these pages,
         * and ui_det_hit() maps coordinates, not buttons (its comment). */
        if (h != UID_HIT_MENOS && h != UID_HIT_MAS && h != UID_HIT_VOLVER) { return 1; }
        s_dst.pressed = h;
        ui_det_draw_one(&s_dst, h);
        s_pressed = h;
        return 1;
    }

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

    if (s_open == MENU_AJUSTES && s_det != DET_NINGUNO) {
        s_dst.pressed = -1;
        if (h == UID_HIT_VOLVER) { det_close(); return; }
        ui_det_draw_one(&s_dst, h);
        det_add((h == UID_HIT_MAS) ? 1 : -1);
        return;
    }
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
        /* Another family / page: the same screen with other cells. */
        if (s_open == MENU_BANDAS) {
            s_fam_sel = s_fam_of_cat[h - UIC_HIT_CAT0];
            bandas_rellena();
        } else if (s_open == MENU_AJUSTES) {
            s_aj_pag = (uint8_t)(h - UIC_HIT_CAT0);
            ajustes_rellena();
        }
        ui_cfg_draw(&s_cfg);
        return;
    }
    if (h >= (int8_t)s_cfg.n) { return; }

    if (s_open == MENU_AJUSTES) {
        /* A setting stays open after a change, as upstream's does: the
         * value just changed is the thing to look at. */
        uint8_t id = k_aj[s_aj_of_cel[h]].id;
        int8_t  cur = s_cfg.cursor;

        aj_toca(id);
        if (id == AJ_TEMA || s_det != DET_NINGUNO) { return; }  /* drawn already */
        s_cfg.cel[h].valor = aj_valor(id);
        s_cfg.cursor = cur;
        ui_cfg_draw_one(&s_cfg, h);
        console_printf("  ajuste: %s -> %s\n", k_aj[s_aj_of_cel[h]].nombre,
                       s_cfg.cel[h].valor);
        return;
    }

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
    if (s_open == MENU_AJUSTES && s_det != DET_NINGUNO) { det_add(d); return; }

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
    if (s_open == MENU_AJUSTES && s_det != DET_NINGUNO) { det_close(); return; }
    c = (s_open == MENU_PASO) ? s_grid.cursor : s_cfg.cursor;
    if (c < 0) { c = cursor_ini(); }
    elige(c);
}
