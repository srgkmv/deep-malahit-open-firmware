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
#include "ui_kbd.h"        /* ../../User - upstream: the frequency keypad */
#include "idioma.h"        /* ../../User - upstream: tr(), T(), idioma() */
#include "banco.h"         /* ../../User - upstream: the memory channels */
#include "bip.h"           /* ../../User - upstream: the pips */
#include "board.h"         /* src/ - board_millis(), for the long press */
#include "backlight.h"     /* ../../User - upstream's API, src/backlight.c */
#include "estereo.h"      /* src/ - FM stereo: Auto / Mono */

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
 * Upstream's mode screen, its families in the column (k_modo_familias[]
 * in its main.c). Analogue: WFM since 21/11/2026 - upstream's WFM path
 * demodulates the station at 0 Hz, so in WFM the tuner goes ON the dial
 * instead of 12 kHz under it (main.c, radio_mode_set()) - the owner's
 * choice, as upstream does it. The descriptions are upstream's, except
 * AM's: "Amplitud modulada" is 91 px in a 92 px cell, measured by the
 * host render (deepsdr-ui `make malahit-menus`).
 *
 * DIGITAL since 22/11/2026, one decoder at a time as each is grafted
 * (PORTING.md step 42 on): an entry is a demodulator plus a decoder,
 * numbered after upstream's demodulators (menus.h, RADIO_MODE_CW). The
 * families upstream has and this port does not yet are left out, not
 * shown empty: Images since 22/11/2026 (WEFAX, step 47), Aeronautical
 * still to come.
 */
#define FAM_ANA 0U
#define FAM_DIG 1U
#define FAM_IMG 2U
#define N_FAM   3U

static const texto_t k_modo_fam[N_FAM] = {
    T("Analógico", "Analogue"), T("Digital", "Digital"), T("Imágenes", "Images")
};

static const struct { int mode; uint8_t fam; const char *n; texto_t d; } k_modos[] = {
    { DEMOD_MODE_AM,  FAM_ANA, "AM",  T("Envolvente", "Envelope") },
    { DEMOD_MODE_SAM, FAM_ANA, "SAM", T("AM síncrona", "Synchronous AM") },
    { DEMOD_MODE_USB, FAM_ANA, "USB", T("Banda lateral alta", "Upper sideband") },
    { DEMOD_MODE_LSB, FAM_ANA, "LSB", T("Banda lateral baja", "Lower sideband") },
    { DEMOD_MODE_NFM, FAM_ANA, "NFM", T("FM estrecha", "Narrow FM") },
    { DEMOD_MODE_WFM, FAM_ANA, "WFM", T("FM ancha", "Wide FM") },
    { RADIO_MODE_CW,  FAM_DIG, "CW",  T("Telegrafía Morse", "Morse telegraphy") },
    { RADIO_MODE_RTTY_L, FAM_DIG, "RTTY-L", T("Teletipo en LSB", "Teletype on LSB") },
    { RADIO_MODE_RTTY_U, FAM_DIG, "RTTY-U", T("Teletipo en USB", "Teletype on USB") },
    { RADIO_MODE_PSK31,  FAM_DIG, "PSK31",  T("Charla en fase", "Phase-shift chat") },
    { RADIO_MODE_NAVTEX, FAM_DIG, "NAVTEX", T("Avisos marítimos", "Marine warnings") },
    { RADIO_MODE_DSC,    FAM_DIG, "DSC",    T("Llamada marítima", "Maritime DSC") },
    { RADIO_MODE_WEFAX,  FAM_IMG, "WEFAX",  T("Mapas del tiempo", "Weather charts") },
    { RADIO_MODE_SSTV,   FAM_IMG, "SSTV",   T("Fotos por radio", "Pictures over radio") },
    /* upstream's "Fotos por radio (FM)" / "Pictures, narrow FM" are 96 and
     * 92 px in a 90 px cell (make malahit-menus) */
    { RADIO_MODE_SSTV_FM, FAM_IMG, "SSTV-FM", T("Fotos en FM", "Pictures on FM") },
    { RADIO_MODE_HELL,   FAM_IMG, "HELL",   T("Texto dibujado", "Drawn text") },
};
#define N_MODOS ((uint8_t)(sizeof k_modos / sizeof k_modos[0]))

static uint8_t s_modo_fam;                  /* the family shown */
static uint8_t s_modo_of_cel[UIC_CELLS];    /* cell -> index in k_modos[] */

/* The family the mode in force belongs to: where the screen opens. */
static uint8_t modo_fam_actual(void)
{
    uint8_t i;
    int     m = radio_mode_get();

    for (i = 0U; i < N_MODOS; i++) {
        if (k_modos[i].mode == m) { return k_modos[i].fam; }
    }
    return FAM_ANA;
}

static void modos_rellena(void)
{
    uint8_t i, n = 0U, f;
    int     m = radio_mode_get();

    memset(&s_cfg, 0, sizeof s_cfg);
    for (f = 0U; f < N_FAM; f++) { s_cfg.cat[f] = k_modo_fam[f][idioma()]; }
    s_cfg.cats    = N_FAM;
    s_cfg.cat_sel = s_modo_fam;
    s_cfg.denso   = 1U;
    s_cfg.marcada = 0xFFU;
    s_cfg.cursor  = -1;
    s_cfg.pressed = -1;
    for (i = 0U; i < N_MODOS && n < (uint8_t)UIC_CELLS; i++) {
        if (k_modos[i].fam != s_modo_fam) { continue; }
        s_modo_of_cel[n] = i;
        s_cfg.cel[n].nombre = k_modos[i].n;
        s_cfg.cel[n].valor  = k_modos[i].d[idioma()];
        s_cfg.cel[n].extra  = "";
        if (k_modos[i].mode == m) { s_cfg.marcada = n; }
        n++;
    }
    s_cfg.n = n;
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
#define BAND_LO_HZ  RADIO_TUNE_MIN_HZ
#define BAND_HI_HZ  RADIO_TUNE_MAX_HZ

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
    if (k < 0 || k >= N_BANDAS) { return (const char *)0; }
    return idioma() == IDIOMA_EN ? banda[k].nombre_en : banda[k].nombre;
}

static int modo_de(const char *s)
{
    if (!strcmp(s, "AM"))  { return DEMOD_MODE_AM; }
    if (!strcmp(s, "SAM")) { return DEMOD_MODE_SAM; }
    if (!strcmp(s, "USB")) { return DEMOD_MODE_USB; }
    if (!strcmp(s, "LSB")) { return DEMOD_MODE_LSB; }
    if (!strcmp(s, "NFM")) { return DEMOD_MODE_NFM; }
    if (!strcmp(s, "WFM")) { return DEMOD_MODE_WFM; }
    return -1;
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
        s_cfg.cat[c]    = idioma() == IDIOMA_EN ? familia_nombre_en[f]
                                                : familia_nombre[f];
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
        s_cfg.cel[n].nombre = menu_band_name(k);
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

/*
 * A step's second line, in the language in force. Upstream's English for
 * 10 kHz, "CB and MW in the Americas", is 136 px in a 114 px cell here
 * (deepsdr-ui `make malahit-menus` measures every one), so it is said
 * shorter - the same rule as AM's "Envolvente" on the mode screen.
 */
static const char *paso_l2(uint32_t i)
{
    if (idioma() != IDIOMA_EN) { return paso_tabla[i].desc; }
    if (paso_tabla[i].hz == 10000UL) { return "CB, MW Americas"; }
    return paso_tabla[i].desc_en;
}

static void pasos_rellena(void)
{
    uint32_t i, n = radio_step_count();

    memset(&s_grid, 0, sizeof s_grid);
    if (n > UIG_CELLS) { n = UIG_CELLS; }
    s_grid.titulo  = tr("Paso de sintonía", "Tuning step");
    s_grid.pagina  = 0U;
    s_grid.paginas = 1U;
    s_grid.n       = (uint8_t)n;
    s_grid.pressed = -1;
    s_grid.cursor  = -1;
    s_grid.marcada = 0xFFU;
    s_grid.nav[0]  = tr("Anterior", "Previous");
    s_grid.nav[1]  = tr("Siguiente", "Next");
    for (i = 0U; i < n; i++) {
        s_grid.cel[i].l1 = paso_tabla[i].rotulo;
        s_grid.cel[i].l2 = paso_l2(i);
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
 * it over instead. Kept in the FRAM (settings.c, record version 4).
 */
typedef enum {
    AJ_AGC, AJ_BW, AJ_NOTCH, AJ_SQL, AJ_NR, AJ_NB, AJ_PRE, AJ_ATT,
    AJ_TEMA, AJ_PALETA, AJ_ESTILO, AJ_VOL,
    AJ_SUAV, AJ_PUENTE, AJ_TRAZA,
    AJ_IDIOMA, AJ_BRILLO, AJ_ESTEREO
} aj_id_t;

/*
 * BOTH LANGUAGES, 21/11/2026 - upstream's idioma.c, which was in the
 * image all along (ui_top.c and spectrum.c already ask it), and its T()
 * and tr(): the English beside the Spanish, in the same place. The
 * English is UPSTREAM'S wherever upstream has the string - k_ajustes[],
 * k_modos[], k_temas[], the band table; the few that are this port's
 * own (Preamplif., Envolvente, the detail pages' lines) are written here
 * in the same style. Numbers keep upstream's format in both, as upstream
 * decided (idioma.h: "14.090,00" either way).
 *
 * Idioma goes where upstream has it: its own page, Equipo / Device.
 */
enum { AJ_PAG_RADIO = 0, AJ_PAG_PANTALLA, AJ_PAG_ESPECTRO, AJ_PAG_EQUIPO, AJ_PAGS };

static const texto_t k_aj_pags[AJ_PAGS] = {
    T("Radio", "Radio"), T("Pantalla", "Display"), T("Espectro", "Spectrum"),
    T("Equipo", "Device")
};

static const struct { texto_t nombre; uint8_t pag; uint8_t id; } k_aj[] = {
    { T("AGC", "AGC"),                 AJ_PAG_RADIO,    AJ_AGC    },
    { T("Ancho (BW)", "Bandwidth"),    AJ_PAG_RADIO,    AJ_BW     },
    { T("Notch", "Notch"),             AJ_PAG_RADIO,    AJ_NOTCH  },
    { T("Silenciador", "Squelch"),     AJ_PAG_RADIO,    AJ_SQL    },
    { T("Reducción", "Noise red."),    AJ_PAG_RADIO,    AJ_NR     },
    { T("Blanker", "Blanker"),         AJ_PAG_RADIO,    AJ_NB     },
    { T("Preamplif.", "Preamp"),       AJ_PAG_RADIO,    AJ_PRE    },
    { T("Atenuador", "Attenuator"),    AJ_PAG_RADIO,    AJ_ATT    },
    { T("Estéreo", "Stereo"),          AJ_PAG_RADIO,    AJ_ESTEREO },  /* this port's own */
    { T("Tema", "Theme"),              AJ_PAG_PANTALLA, AJ_TEMA   },
    { T("Paleta", "Palette"),          AJ_PAG_PANTALLA, AJ_PALETA },
    { T("Estilo", "Style"),            AJ_PAG_PANTALLA, AJ_ESTILO },
    { T("Volumen", "Volume"),          AJ_PAG_PANTALLA, AJ_VOL    },
    { T("Brillo", "Brightness"),       AJ_PAG_PANTALLA, AJ_BRILLO },   /* upstream's: after Volumen */
    { T("Suavizado", "Smoothing"),     AJ_PAG_ESPECTRO, AJ_SUAV   },   /* upstream's place: before Puente */
    { T("Puente", "Bridge"),           AJ_PAG_ESPECTRO, AJ_PUENTE },
    { T("Traza", "Trace"),             AJ_PAG_ESPECTRO, AJ_TRAZA  },
    { T("Idioma", "Language"),         AJ_PAG_EQUIPO,   AJ_IDIOMA },
};
#define N_AJ ((uint8_t)(sizeof k_aj / sizeof k_aj[0]))

/* Upstream's k_notch_nombres[] and k_nb_nombres[] (User/main.c). */
static const texto_t k_notch_nom[4] = {
    T("Suave", "Gentle"), T("Normal", "Normal"), T("Fuerte", "Strong"), T("A saco", "Maximum")
};
static const texto_t k_nb_nom[4] = {
    T("Apagado", "Off"), T("Suave", "Gentle"), T("Medio", "Medium"), T("Fuerte", "Strong")
};

/*
 * THE DETAIL PAGE - upstream's ui_det.c, for the five continuous ones:
 * "-" and "+" either side of the value, the knob turning it, the knob's
 * press or "Volver" back to the grid. Opened from a cell, and part of the
 * settings screen: the Ajustes button stays lit, and pressing it from
 * here goes back to the grid, as upstream's does.
 *
 * Squelch: upstream's -10..60 dB in 2 dB steps (SQUELCH_DB_* in its
 * main.c). NR strength: 0..4080 in 16s - upstream's 0..4095 in 10s,
 * moved onto the grid the FRAM stores it on (settings.h). Suavizado:
 * upstream's 0..95 % in 5s (SPECTRUM_SMOOTH_* in its main.c).
 */
enum { DET_NINGUNO = 0, DET_SQL, DET_NR, DET_VOL, DET_ATT, DET_SUAV, DET_BRILLO };

/* Upstream's knob on Brillo: 20..100 % in 5s (BACKLIGHT_STEP and the
 * clamp in its tune_encoder_poll()); backlight.c's own floor is 10. */
#define BRILLO_MIN   20
#define BRILLO_STEP   5

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
    case AJ_NOTCH:  return anotch_activo() ? k_notch_nom[anotch_fuerza() & 3U][idioma()]
                                           : tr("Apagado", "Off");
    case AJ_NR:     return num_txt(s_aj_buf + 8, (int32_t)s_nr_strength, 0);
    case AJ_NB:     return k_nb_nom[nb_get_nivel() & 3U][idioma()];
    case AJ_PRE:    return radio_preamp() ? tr("Activo", "On") : tr("Apagado", "Off");
    case AJ_ESTEREO: return estereo_puesto() ? tr("Auto", "Auto") : tr("Mono", "Mono");
    case AJ_ATT:    return radio_att_text();
    case AJ_TEMA:   return menu_tema_nombre();
    case AJ_PALETA: return spectrum_palette_nombre((uint8_t)spectrum_get_palette());
    case AJ_ESTILO:
        switch (spectrum_get_style()) {
        case SPECTRUM_STYLE_HEATMAP: return tr("Relleno", "Filled");
        case SPECTRUM_STYLE_LINE:    return tr("Línea", "Line");
        default:                     return tr("Contorno", "Outline");
        }
    case AJ_VOL:    return radio_vol_text();
    case AJ_SUAV: {
        static char b[8];               /* its own: a page's values all live at once */
        return num_txt(b, (int32_t)radio_suav_pct(), " %");
    }
    case AJ_BRILLO: {
        static char b[8];
        return num_txt(b, (int32_t)backlight_get_percent(), " %");
    }
    case AJ_PUENTE: return spectrum_get_bridge() ? tr("Unido", "Joined") : tr("Suelto", "Loose");
    case AJ_TRAZA:  return spectrum_get_heatmap_trace_white() ? tr("Blanca", "White")
                                                              : tr("Del color", "Palette");
    case AJ_IDIOMA: return idioma_nombre();       /* each in its own: "English" */
    default:        return "";
    }
}

static void ajustes_rellena(void)
{
    uint8_t i, n = 0U;

    memset(&s_cfg, 0, sizeof s_cfg);
    for (i = 0U; i < AJ_PAGS; i++) { s_cfg.cat[i] = k_aj_pags[i][idioma()]; }
    s_cfg.cats    = AJ_PAGS;
    s_cfg.cat_sel = s_aj_pag;
    s_cfg.denso   = 0U;                 /* 3 x 3: name small, value big */
    s_cfg.marcada = 0xFFU;
    s_cfg.cursor  = -1;
    s_cfg.pressed = -1;
    for (i = 0U; i < N_AJ && n < (uint8_t)(UIC_COLS * UIC_ROWS); i++) {
        if (k_aj[i].pag != s_aj_pag) { continue; }
        s_cfg.cel[n].nombre = k_aj[i].nombre[idioma()];
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
        s_dst.titulo = tr("Silenciador", "Squelch");
        s_dst.valor  = num_txt(s_det_val, (int32_t)demod_am_get_squelch_db(), " dB");
        if ((int32_t)demod_am_get_squelch_db() <= SQL_FLOOR_DB) {
            s_dst.pie = tr("abajo del todo: abierto siempre", "at the bottom: always open");
        }
        break;
    case DET_NR:
        s_dst.titulo = tr("Reducción de ruido", "Noise reduction");
        s_dst.valor  = num_txt(s_det_val, (int32_t)s_nr_strength, 0);
        s_dst.pie    = nr_ss_get_enabled() ? tr("encendida (botón Ruido)", "on (Noise button)")
                                           : tr("apagada: se enciende con Ruido",
                                                "off: the Noise button turns it on");
        break;
    case DET_VOL:
        s_dst.titulo = tr("Volumen", "Volume");
        s_dst.valor  = num_txt(s_det_val, (int32_t)radio_vol_db(), " dB");
        break;
    case DET_ATT:
        s_dst.titulo = tr("Atenuador", "Attenuator");
        s_dst.valor  = num_txt(s_det_val, -(int32_t)radio_att_db(), " dB");
        s_dst.pie    = tr("reducción de ganancia del sintonizador", "the tuner's gain reduction");
        break;
    case DET_BRILLO:
        /* The number alone, as Suavizado's: no '%' in font_num_31. */
        s_dst.titulo = tr("Brillo", "Brightness");
        s_dst.valor  = num_txt(s_det_val, (int32_t)backlight_get_percent(), 0);
        s_dst.pie    = tr("% de la luz de la pantalla", "% of the screen's light");
        break;
    case DET_SUAV:
        /* The number alone: font_num_31 has no '%' (nor has upstream's
         * font_num_44, so upstream's own page draws "25 " here), so the
         * unit goes to the line under it. */
        s_dst.titulo = tr("Suavizado", "Smoothing");
        s_dst.valor  = num_txt(s_det_val, (int32_t)radio_suav_pct(), 0);
        s_dst.pie    = tr("% promediado entre fotogramas", "% averaged between frames");
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
    case DET_SUAV: (void)radio_suav_set(radio_suav_pct() + 5 * (int)d); break;
    case DET_BRILLO: {
        int32_t v = (int32_t)backlight_get_percent() + d * BRILLO_STEP;

        if (v < BRILLO_MIN) { v = BRILLO_MIN; }
        if (v > 100)        { v = 100; }
        backlight_set_percent((uint8_t)v);
        break;
    }
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
    case AJ_ESTEREO: estereo_pon((uint8_t)!estereo_puesto()); break;
    case AJ_ATT:   det_open(DET_ATT); return;
    case AJ_VOL:   det_open(DET_VOL); return;
    case AJ_SUAV:  det_open(DET_SUAV); return;
    case AJ_BRILLO: det_open(DET_BRILLO); return;
    case AJ_IDIOMA:
        /* As upstream's idioma_cambiar(): the other one, and everything
         * painted again in it - this screen included. */
        radio_idioma((uint8_t)(idioma() == IDIOMA_ES ? IDIOMA_EN : IDIOMA_ES));
        console_printf("  idioma -> %s\n", idioma_nombre());
        radio_repaint_all();
        ajustes_rellena();
        menu_dibuja();
        return;
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
    s->aj_suav   = (uint8_t)radio_suav_pct();
    s->has_suav  = 1U;
    s->aj_wfm_bw = (uint8_t)(demod_am_get_audio_bw() + 1U);
    s->aj_idioma = (uint8_t)(idioma() + 1U);
    s->aj_brillo = backlight_get_percent();
    s->aj_estereo = (uint8_t)(estereo_puesto() + 1U);
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
    if (s->has_suav) { (void)radio_suav_set((int)s->aj_suav); }   /* clamps */
    if (s->aj_estereo == 1U || s->aj_estereo == 2U) {
        estereo_pon((uint8_t)(s->aj_estereo - 1U));
    }
    if (s->aj_brillo >= (uint8_t)BRILLO_MIN && s->aj_brillo <= 100U) {
        backlight_set_percent(s->aj_brillo);
    }
    if (s->aj_idioma >= 1U && s->aj_idioma <= IDIOMA_N) {
        radio_idioma((uint8_t)(s->aj_idioma - 1U));
    }
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
    if (s->aj_wfm_bw >= 1U && s->aj_wfm_bw <= (uint8_t)AUDIO_BW_1K8 + 1U) {
        demod_am_set_audio_bw((audio_bw_t)(s->aj_wfm_bw - 1U));
    }
}

const char *menu_tema_nombre(void)
{
    return idioma() == IDIOMA_EN ? k_sim_temas[s_tema].nombre_en : k_sim_temas[s_tema].nombre;
}
uint32_t    menu_nr_strength(void) { return s_nr_strength; }

/* ------------------------------------------------------------------ */
/* THE MEMORY CHANNELS                                                 */
/* ------------------------------------------------------------------ */

/*
 * Upstream's bank of memories (banco.c, compiled in place): four banks of
 * fifteen, each memory the frequency, the mode, the step and the filter's
 * two cuts - what it takes to be back listening exactly as before, not
 * only on the frequency. Upstream's screen, on ui_cfg.c: the banks in the
 * column, the memories in the grid, each cell its frequency, its mode and
 * its number. And upstream's gestures:
 *
 *   a short tap on a memory   recalls it and closes the screen
 *   a short tap on an empty   the "no" pip, and the screen stays
 *   two seconds on any cell   SAVES the radio as it is into it, with the
 *                             "done" pip - "saving over a memory is
 *                             destructive, so it has to cost more than
 *                             brushing the screen" (upstream, 4 s, then 3,
 *                             then 2 at its owner's word)
 *
 * The bank names are upstream's k_banco_nombres[]. Upstream renames them
 * by editing MEM.CSV on a PC; this board has no disk to edit, so they stay
 * upstream's - a name in the FRAM's copy of the file would still be read.
 *
 * Kept in the FRAM as upstream's own MEM.CSV, written at once on every
 * save (src/memfram.c). Upstream's 16th cell, SCAN, is not here yet.
 */
static const texto_t k_banco_nombres[BANCO_N] = {
    T("HF", "HF"), T("Aviones", "Aircraft"), T("Marina", "Marine"),
    T("Repetidores", "Repeaters")
};

#define MEM_LARGO_MS  2000U             /* upstream's CFG_LARGO_MS */

static uint8_t  s_mem_bk;               /* the bank on screen */
static char     s_mem_txt[BANCO_MEM_N][16];     /* "07" and, from [3], "9.600,00" */
static uint32_t s_press_ms;             /* when the press on a cell began */
static uint8_t  s_largo;                /* this press has saved already */
static char     s_mem_csv[3072];        /* upstream's BANCO_CSV_MIN is 2560 */
static uint32_t s_pasos_hz[N_PASOS];    /* banco_pasos_pon(): the file keeps hertz */

static const char *mem_banco_nombre(uint8_t b)
{
    const char *n = banco_nombre(b);

    return (n[0] != '\0') ? n : k_banco_nombres[b][idioma()];
}

static void mem_rellena(void)
{
    uint8_t i;

    memset(&s_cfg, 0, sizeof s_cfg);
    for (i = 0U; i < (uint8_t)BANCO_N; i++) { s_cfg.cat[i] = mem_banco_nombre(i); }
    s_cfg.cats    = (uint8_t)BANCO_N;
    s_cfg.cat_sel = s_mem_bk;
    s_cfg.denso   = 1U;                 /* 4 x 4, as the band screen */
    s_cfg.marcada = 0xFFU;
    s_cfg.cursor  = -1;
    s_cfg.pressed = -1;
    for (i = 0U; i < (uint8_t)BANCO_MEM_N; i++) {
        char       *t = s_mem_txt[i];
        banco_mem_t m;

        /* The number always, full or empty: it is what a memory is
         * called by, and where the finger has to stay two seconds. */
        t[0] = (char)('0' + (i + 1U) / 10U);
        t[1] = (char)('0' + (i + 1U) % 10U);
        t[2] = '\0';
        s_cfg.cel[i].extra = t;
        if (!banco_lee(s_mem_bk, i, &m)) {
            s_cfg.cel[i].nombre = tr("libre", "empty");
            s_cfg.cel[i].valor  = "";
        } else {
            radio_fmt_khz(m.hz, &t[3], (uint32_t)(sizeof s_mem_txt[i] - 3U));
            s_cfg.cel[i].nombre = &t[3];
            s_cfg.cel[i].valor  = radio_mode_name((int)m.modo);
            /* The one playing now: the frequency AND the mode. */
            if (m.hz == radio_dial_hz() && (int)m.modo == radio_mode_get()) {
                s_cfg.marcada = i;
            }
        }
    }
    s_cfg.n = (uint8_t)BANCO_MEM_N;
}

static void mem_graba(uint8_t bk, uint8_t c)
{
    static const char *serie[BANCO_N];
    banco_mem_t m;
    uint8_t     b;

    memset(&m, 0, sizeof m);
    m.hz   = radio_dial_hz();
    m.modo = (uint8_t)radio_mode_get();
    m.paso = (uint8_t)radio_step_get();
    radio_filtro_get(&m.fil_lo, &m.fil_hi);
    if (!banco_graba(bk, c, &m)) { bip_error(); return; }

    /* The default names and the line that explains them, translated, just
     * before writing - upstream's order, so a file written after a change
     * of language has them in it. */
    for (b = 0U; b < (uint8_t)BANCO_N; b++) { serie[b] = k_banco_nombres[b][idioma()]; }
    banco_nombres_pon(serie, (uint8_t)BANCO_N,
                      tr("Para ponerle nombre a un banco, quita la almohadilla"
                         " y escribe el tuyo:",
                         "To name a bank, remove the hash and write your own:"));
    if (banco_guarda(s_mem_csv, (uint32_t)sizeof s_mem_csv)) {
        bip_listo();
        console_printf("  MEM: bank %u memory %u <- %u Hz %s (FRAM slot %d, seq %u)\n",
                       (uint32_t)bk + 1U, (uint32_t)c + 1U, m.hz,
                       radio_mode_name((int)m.modo), memfram_slot(), memfram_seq());
    } else {
        /* Kept in RAM, which is what matters for using it; said. */
        bip_error();
        console_printf("  *** MEM: bank %u memory %u kept, but NOT written to the FRAM ***\n",
                       (uint32_t)bk + 1U, (uint32_t)c + 1U);
    }
}

/* Upstream's mem_recupera(): the mode first, then the dial, the step and
 * the filter - the filter last because it is the mode's family's. */
static int mem_recupera(uint8_t bk, uint8_t c)
{
    banco_mem_t m;

    if (!banco_lee(bk, c, &m)) { return 0; }
    if ((int)m.modo != radio_mode_get()) { radio_mode_set((int)m.modo); }
    if (radio_dial_go(m.hz) == 0U) { return 0; }
    if ((uint32_t)m.paso < radio_step_count()) { radio_step_set((uint32_t)m.paso); }
    if (m.fil_hi > m.fil_lo) { radio_filtro_set(m.fil_lo, m.fil_hi); }
    console_printf("  MEM: bank %u memory %u -> %u Hz %s\n", (uint32_t)bk + 1U,
                   (uint32_t)c + 1U, m.hz, radio_mode_name((int)m.modo));
    return 1;
}

/* The finger is still on a cell: two seconds of it save. Once a press. */
static void mem_largo_poll(void)
{
    if (s_open != MENU_MEM || s_pressed < 0 || s_largo) { return; }
    if (s_pressed >= (int8_t)BANCO_MEM_N) { return; }          /* the column */
    if ((uint32_t)(board_millis() - s_press_ms) < MEM_LARGO_MS) { return; }
    s_largo = 1U;
    mem_graba(s_mem_bk, (uint8_t)s_pressed);
    {
        int8_t h = s_pressed;

        mem_rellena();
        s_cfg.pressed = h;              /* still under the finger */
        ui_cfg_draw_one(&s_cfg, h);
    }
}

void menu_mem_init(void)
{
    uint32_t i;
    int      n = 0;

    for (i = 0U; i < (uint32_t)N_PASOS; i++) { s_pasos_hz[i] = (uint32_t)paso_tabla[i].hz; }
    banco_init();
    banco_pasos_pon(s_pasos_hz, (uint8_t)N_PASOS);
    (void)banco_carga(s_mem_csv, (uint32_t)sizeof s_mem_csv);
    for (i = 0U; i < (uint32_t)BANCO_N; i++) { n += (int)banco_usadas((uint8_t)i); }
    console_printf("Memories  : %d in use, %s\n", n,
                   memfram_slot() >= 0 ? "MEM.CSV from the FRAM (memfram.c)"
                                       : "none in the FRAM yet");
}

/* ------------------------------------------------------------------ */
/* THE FREQUENCY KEYPAD                                                */
/* ------------------------------------------------------------------ */

/*
 * Upstream's ui_kbd.c, opened by a tap on the frequency in the header.
 * What a key DOES is upstream's too, from its main.c, and kept as it is
 * there for the reason upstream gives: the entry functions carry their
 * odd cases already solved - a comma with nothing after it yet, Borrar
 * back past the comma, nine digits times a million done in 64 bits.
 *
 *   menu_freq_keypad_digit/point/del/clr/accept_callback  -> kbd_*()
 *   kbd_lectura_freq()                                    -> kbd_lectura()
 *   k_kbd_freq_rot[], k_kbd_freq_tipo[], k_kbd_digito[]   -> the same
 *   kbd_apply(), kbd_touch()                              -> kbd_apply(),
 *                                         menu_touch_down()/menu_touch_up()
 *
 * The digits are typed, never pre-filled with the dial: "typing a fresh
 * number is the point" (upstream). Out of the dial's range clamps to its
 * edge, as upstream's does to its own (radio_dial_go()). The big knob
 * does nothing here, as upstream's does nothing (its comment, 23/09/2026:
 * a knob that fell through repainted the radio over invisible keys).
 * Upstream's clock keypad is the same module and is not wired: this
 * radio has no clock setting yet.
 */
#define FE_MAX_DIGITS  9U       /* upstream's FREQ_ENTRY_MAX_DIGITS */
#define FE_NO_POINT    0xFFU    /* upstream's FREQ_ENTRY_NO_POINT */

static ui_kbd_state_t s_kbd;
static uint32_t       s_fe_value;
static uint8_t        s_fe_digits;
static uint8_t        s_fe_point = FE_NO_POINT;  /* digits before the comma */
static char           s_kbd_lectura[16];

static const texto_t k_kbd_rot[UIK_KEYS] = {        /* upstream's k_kbd_freq_rot[] */
    T("1","1"), T("2","2"), T("3","3"), T("Borrar","Delete"),
    T("4","4"), T("5","5"), T("6","6"), T("Vaciar","Clear"),
    T("7","7"), T("8","8"), T("9","9"), T("Volver","Back"),
    T(",",","), T("0","0"), T("kHz","kHz"), T("MHz","MHz"),
};
static const uint8_t k_kbd_tipo[UIK_KEYS] = {
    UIK_DIGITO, UIK_DIGITO, UIK_DIGITO, UIK_BORRA,
    UIK_DIGITO, UIK_DIGITO, UIK_DIGITO, UIK_BORRA,
    UIK_DIGITO, UIK_DIGITO, UIK_DIGITO, UIK_VUELVE,
    UIK_DIGITO, UIK_DIGITO, UIK_ACEPTA, UIK_ACEPTA,
};
static const uint8_t k_kbd_digito[UIK_KEYS] = {
    1U, 2U, 3U, 0xFFU,
    4U, 5U, 6U, 0xFFU,
    7U, 8U, 9U, 0xFFU,
    0xFFU, 0U, 0xFFU, 0xFFU,
};

/* The digits as typed, leading zeros and all, the comma where it went. */
static void kbd_lectura(void)
{
    char     tmp[FE_MAX_DIGITS + 1U];
    uint32_t v = s_fe_value;
    uint8_t  n = s_fe_digits, i = n, k = 0U, j;

    s_kbd.lectura = s_kbd_lectura;
    if (n == 0U && s_fe_point == FE_NO_POINT) {
        s_kbd_lectura[0] = '\0';
        return;
    }
    tmp[n] = '\0';
    while (i > 0U) { tmp[--i] = (char)('0' + (v % 10U)); v /= 10U; }
    for (j = 0U; j < n; j++) {
        if (s_fe_point != FE_NO_POINT && j == s_fe_point) { s_kbd_lectura[k++] = ','; }
        s_kbd_lectura[k++] = tmp[j];
    }
    /* A comma with no digits after it yet is shown, or the tap looks lost. */
    if (s_fe_point != FE_NO_POINT && s_fe_point >= n) { s_kbd_lectura[k++] = ','; }
    s_kbd_lectura[k] = '\0';
}

static void kbd_lectura_draw(void)
{
    kbd_lectura();
    ui_kbd_draw_lectura(&s_kbd);
}

static void kbd_digit(uint8_t d)
{
    if (s_fe_digits >= FE_MAX_DIGITS) { return; }
    s_fe_value = s_fe_value * 10U + d;
    s_fe_digits++;
    kbd_lectura_draw();
}

static void kbd_point(void)
{
    if (s_fe_point != FE_NO_POINT) { return; }      /* one per entry */
    s_fe_point = s_fe_digits;
    kbd_lectura_draw();
}

static void kbd_del(void)
{
    if (s_fe_digits > 0U) {
        s_fe_value /= 10U;
        s_fe_digits--;
        /* Back past the comma, or onto it: the comma goes too. */
        if (s_fe_point != FE_NO_POINT && s_fe_point > s_fe_digits) {
            s_fe_point = FE_NO_POINT;
        }
        kbd_lectura_draw();
    } else if (s_fe_point != FE_NO_POINT) {
        s_fe_point = FE_NO_POINT;                    /* a lone comma */
        kbd_lectura_draw();
    }
}

static void kbd_clr(void)
{
    s_fe_value  = 0U;
    s_fe_digits = 0U;
    s_fe_point  = FE_NO_POINT;
    kbd_lectura_draw();
}

/* kHz or MHz. Nothing typed is nothing done: no retune to the bottom of
 * the dial from an empty entry. The multiply and the divide are in 64
 * bits, so nine digits times a million cannot overflow on the way. */
static void kbd_accept(uint32_t mult)
{
    uint64_t hz;
    uint32_t got;

    if (s_fe_digits == 0U && s_fe_point == FE_NO_POINT) { return; }
    hz = (uint64_t)s_fe_value * mult;
    if (s_fe_point != FE_NO_POINT) {
        uint8_t i;

        for (i = s_fe_point; i < s_fe_digits; i++) { hz /= 10U; }
    }
    if (hz > 0xFFFFFFFFULL) { hz = 0xFFFFFFFFULL; }

    got = radio_dial_go((uint32_t)hz);
    if (got == 0U) {
        console_printf("  teclado: %s -> el sintetizador no fue a %u Hz\n",
                       s_kbd_lectura, (uint32_t)hz);
    } else {
        console_printf("  teclado: %s %s -> %u Hz%s\n", s_kbd_lectura,
                       mult == 1000U ? "kHz" : "MHz", got,
                       (uint64_t)got != hz ? " (el borde del dial)" : "");
    }
    menu_close();
}

static void kbd_apply(uint8_t i)
{
    uint8_t d = k_kbd_digito[i];

    if (d != 0xFFU) { kbd_digit(d); return; }
    switch (i) {
    case 3U:  kbd_del();            break;
    case 7U:  kbd_clr();            break;
    case 11U: menu_close();         break;
    case 12U: kbd_point();          break;
    case 14U: kbd_accept(1000U);    break;
    case 15U: kbd_accept(1000000U); break;
    default:                        break;
    }
}

/* ------------------------------------------------------------------ */

menu_t menu_open_id(void) { return s_open; }

static void dibuja(void)
{
    if (s_open == MENU_PASO)         { ui_grid_draw(&s_grid); }
    else if (s_open == MENU_TECLADO) { ui_kbd_draw(&s_kbd); }
    else                             { ui_cfg_draw(&s_cfg); }
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
    case MENU_TECLADO: menu_teclado_open(); return;
    case MENU_MODO:   s_modo_fam = modo_fam_actual(); modos_rellena(); break;
    case MENU_BANDAS: bandas_familia_inicial(); bandas_rellena(); break;
    case MENU_PASO:   pasos_rellena(); break;
    case MENU_AJUSTES: ajustes_rellena(); break;
    case MENU_MEM:    mem_rellena(); break;
    default:          menu_close(); return;
    }
    s_open = m;
    dibuja();
    radio_act_refresh();
    console_printf("  menu -> %s\n", m == MENU_MODO ? "Modo"
                                   : m == MENU_BANDAS ? "Bandas"
                                   : m == MENU_PASO ? "Paso"
                                   : m == MENU_MEM ? "MEM" : "Ajustes");
}

/*
 * A tap on the frequency. Opening it again over itself starts the entry
 * afresh, as upstream's kbd_show() does; over any other screen it takes
 * that screen's place, as upstream allows on purpose.
 */
void menu_teclado_open(void)
{
    uint8_t i;

    s_det       = DET_NINGUNO;
    s_pressed   = -1;
    s_fe_value  = 0U;
    s_fe_digits = 0U;
    s_fe_point  = FE_NO_POINT;
    memset(&s_kbd, 0, sizeof s_kbd);
    s_kbd.titulo  = tr("Frecuencia", "Frequency");
    s_kbd.pista   = tr("teclea y elige la unidad", "type it and pick the unit");
    s_kbd.pressed = -1;
    s_kbd.cursor  = -1;
    for (i = 0U; i < UIK_KEYS; i++) {
        s_kbd.tecla[i] = k_kbd_rot[i][idioma()];
        s_kbd.tipo[i]  = k_kbd_tipo[i];
    }
    kbd_lectura();
    s_open = MENU_TECLADO;
    ui_kbd_draw(&s_kbd);
    radio_act_refresh();        /* no button of the bar is this screen's */
    console_printf("  menu -> Teclado\n");
}

int menu_touch_down(uint16_t x, uint16_t y, int nuevo)
{
    int8_t h;

    if (s_open == MENU_NONE) { return 0; }
    if (y < UIG_Y || y >= UIG_Y + UIG_H) { return 0; }
    if (!nuevo) { mem_largo_poll(); return 1; }

    /* Decided on the press and kept to the lift, as upstream's
     * kbd_touch(): the lift's coordinates are not to be trusted. */
    if (s_open == MENU_TECLADO) {
        h = ui_kbd_hit(x, y);
        if (h < 0 || s_kbd.tipo[h] == (uint8_t)UIK_NADA) { return 1; }
        s_kbd.pressed = h;
        ui_kbd_draw_one(&s_kbd, h);
        s_pressed = h;
        return 1;
    }

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
    s_pressed  = h;
    s_press_ms = board_millis();
    s_largo    = 0U;
    return 1;
}

static void elige(int8_t h);

void menu_touch_up(void)
{
    int8_t h = s_pressed;

    if (h < 0 || s_open == MENU_NONE) { return; }
    s_pressed = -1;

    if (s_open == MENU_TECLADO) {
        s_kbd.pressed = -1;
        ui_kbd_draw_one(&s_kbd, h);
        kbd_apply((uint8_t)h);
        return;
    }

    /* A press that saved is done: the lift must not recall as well. */
    if (s_open == MENU_MEM && s_largo) {
        s_largo = 0U;
        s_cfg.pressed = -1;
        ui_cfg_draw_one(&s_cfg, h);
        return;
    }

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
        } else if (s_open == MENU_MEM) {
            s_mem_bk = (uint8_t)(h - UIC_HIT_CAT0);
            mem_rellena();
        } else if (s_open == MENU_MODO) {
            s_modo_fam = (uint8_t)(h - UIC_HIT_CAT0);
            modos_rellena();
        }
        ui_cfg_draw(&s_cfg);
        return;
    }
    if (h >= (int8_t)s_cfg.n) { return; }

    if (s_open == MENU_MEM) {
        /* A memory closes the screen, as a band or a mode does; an empty
         * one says no and stays, for the finger to come back and save. */
        if (mem_recupera(s_mem_bk, (uint8_t)h)) { menu_close(); }
        else { bip_error(); ui_cfg_draw_one(&s_cfg, h); }
        return;
    }

    if (s_open == MENU_AJUSTES) {
        /* A setting stays open after a change, as upstream's does: the
         * value just changed is the thing to look at. */
        uint8_t id = k_aj[s_aj_of_cel[h]].id;
        int8_t  cur = s_cfg.cursor;

        aj_toca(id);
        if (id == AJ_TEMA || id == AJ_IDIOMA || s_det != DET_NINGUNO) { return; }  /* drawn already */
        s_cfg.cel[h].valor = aj_valor(id);
        s_cfg.cursor = cur;
        ui_cfg_draw_one(&s_cfg, h);
        console_printf("  ajuste: %s -> %s\n", k_aj[s_aj_of_cel[h]].nombre[IDIOMA_ES],
                       s_cfg.cel[h].valor);
        return;
    }

    if (s_open == MENU_MODO) {
        radio_mode_set(k_modos[s_modo_of_cel[h]].mode);
        console_printf("  menu: modo %s\n", k_modos[s_modo_of_cel[h]].n);
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
    if (s_open == MENU_TECLADO) { return; }         /* upstream's: no knob */
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

        if ((s_open == MENU_BANDAS || s_open == MENU_MEM || s_open == MENU_MODO)
            && (t >= (int32_t)s_cfg.n || t < 0)
            && s_cfg.cats > 1U) {
            uint8_t c = s_cfg.cat_sel;

            if (t >= (int32_t)s_cfg.n && c + 1U < s_cfg.cats)   { c++; t = 0; }
            else if (t < 0 && c > 0U)                          { c--; t = 999; }
            if (c != s_cfg.cat_sel) {
                if (s_open == MENU_MEM) { s_mem_bk = c; mem_rellena(); }
                else if (s_open == MENU_MODO) { s_modo_fam = c; modos_rellena(); }
                else { s_fam_sel = s_fam_of_cat[c]; bandas_rellena(); }
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

    if (s_open == MENU_NONE || s_open == MENU_TECLADO) { return; }
    if (s_open == MENU_AJUSTES && s_det != DET_NINGUNO) { det_close(); return; }
    c = (s_open == MENU_PASO) ? s_grid.cursor : s_cfg.cursor;
    if (c < 0) { c = cursor_ini(); }
    elige(c);
}
