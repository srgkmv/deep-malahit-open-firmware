/*
 * THE DECODERS' SCREEN - step 42, 22/11/2026. See digi.h.
 *
 * Upstream's pieces, each named where it comes from in its main.c:
 *
 *   rtty_text_push()/putc()/newline()  the rows: a character at a time,
 *                       a new row at \r or \n or at the right margin,
 *                       the rows scrolling up when the last one is used
 *   digi_sync()         the panel's state: rows oldest first, the
 *                       Borrar button, the chip ("20 ppm 14 dB") and
 *                       the lock dot
 *   rtty_scope_draw()   the tone scope: rtty_scope.c's frame in dB,
 *                       spectrum_draw() across the full width, 0 to
 *                       Nyquist (6 kHz), and the tone's two lines
 *
 * What this port adds is only the cut to the decoders grafted so far,
 * CW (step 42) and RTTY (step 43), each with upstream's header:
 *
 *   CW     Borrar | Aprende                     | "20 ppm 14 dB" + dot
 *   RTTY   Borrar |      | 450 Hz | 50 Bd | Auto
 *   PSK31  Borrar                               | "+1,2 Hz" + dot
 *   NAVTEX Borrar                               | "FA01" / "SITOR" /
 *                                                 "bits" / "buscando" + dot
 *   DSC    Borrar                               | "buscando" / "sync" /
 *                                                 "llamada" + dot
 *   WEFAX  Sincroniza | 120 lpm                 | "Recibiendo" + dot,
 *          and the chart below the header, in place of scope and rows
 *   SSTV   Empezar | Auto / Martin 1 ...        | "Martin 1 120/256" + dot,
 *          and the picture below, the same way
 *   HELL   Borrar                               | "Recibiendo" + dot, and
 *          the strips of columns below
 *   JTTY   Borrar                               | atoms count + dot, a
 *          table over the whole area: Time Offset Qual. Message Country
 *   ALE    Borrar | quality bar / "last one N min" | words count + dot,
 *          a table over the whole area: Callsign Type kHz Qual. N
 *   APRS,  Borrar                               | frames with their
 *   PKT300 check right + dot "flags seen"; the scope's two lines the
 *          decoder's mark and space (1200/2200 or 1600/1800)
 *   AIS    Borrar | "N ships" bar (signal) | 161.975 | frames right
 *          / closed, a table over the whole area: MMSI Name Position
 *          Speed Course N, the channel in the title
 *   HFDL   Borrar | phase / "A1>A2>M1" | 8.942 | "35% 3/9",
 *          a table: ICAO Model Flight Position Latest N
 *   WSPR   Borrar | "starts in 1:47" / "listening 63 %" | 14.095,6 |
 *          "N heard", a table: Time Callsign Loc. dBm Offset Qual.
 *   FT8    Borrar | CQ | 14.074 |  | the slot's bar "Nc . DT" + "8/15"
 *          and the dot, a table over the whole area: Time Hz dB DT
 *          Message km Country, CQs in their colour, a finger scrolls it
 *   IDENT  Borrar                               | "measuring" /
 *          "listening" + dot, the names that fit, their notes and the
 *          five numbers measured, over the whole area
 *   STANAG Borrar | Auto/S4285/... | speed | interleave | format | state,
 *          over the whole area: the identifier's table (Period Signal
 *          Quality), or the 4285's or the 4415's text
 *
 * and upstream's tap on each (rtty_shift_siguiente(), rtty_baud_
 * siguiente(), the Auto toggle, cw_set_aprende()) - on the press here,
 * with the button drawn down for that frame, as Borrar.
 */
#include <string.h>
#include <math.h>
#include "digi.h"
#include "console.h"
#include "gfx.h"           /* ../../User - gfx_vline(), gfx_fill_rect() */
#include "gfx2.h"          /* ../../User */
#include "palette.h"       /* ../../User - PAL_* */
#include "spectrum.h"      /* ../../User - spectrum_draw() */
#include "ui_digi.h"       /* ../../User - the panel */
#include "rtty_scope.h"    /* ../../User - the scope's audio spectrum */
#include "cw.h"            /* ../../User - the decoders */
#include "rtty.h"
#include "rtty_auto.h"
#include "psk31.h"
#include "navtex.h"
#include "dsc.h"
#include "wefax.h"
#include "sstv.h"
#include "hell.h"
#include "jtty_modo.h"
#include "ale_modo.h"
#include "stanag_modo.h"
#include "ident.h"
#include "ax25.h"
#include "ais_modo.h"
#include "ft8_modo.h"
#include "ft8_lista.h"     /* ../../User - the CQ filter and the finger's scroll */
#include "wspr_modo.h"
#include "hfdl_modo.h"
#include "menus.h"         /* radio_dial_go(), radio_dial_hz() - AIS's channel button */
#include "board.h"         /* board_millis() - ALE's poll and its clock */
#include "demod.h"         /* demod_quieto(): a decoder reconfigured, no block through it */
#include "idioma.h"        /* ../../User - tr() */
#include "digi_mapa.h"     /* src/ - the map in the rows' place, step 64 */
#include "ais_modo.h"      /* ais_modo_tramas(): the map redrawn when the ships change */

/*
 * THE MAP (step 64): one switch for the five modes that have one -
 * the fifth button of their header - and off at every mode change, as
 * upstream's s_ft8_mapa ("what is painted is the last mode's stations").
 */
static uint8_t  s_mapa;
static uint32_t s_ais_mapa_visto = 0xFFFFFFFFU;
static void     mapa_boton(void);
static uint8_t  filas_o_mapa(void);

/*
 * The face ui_digi.c draws the rows in: its font_ui_14b, which this
 * build renames to font_ui_10b (reglas.mk, MALAHIT_FONTS_MENU). A row
 * is cut against the SAME face it is drawn in, or the wrap and the
 * drawing disagree by a character at the margin.
 */
extern const gfx2_font_t font_ui_10b;
#define DIGI_FONT        (&font_ui_10b)
#define DIGI_TEXT_W      ((int16_t)(GFX2_W - 2 * UDG_PAD_X))
#define DIGI_COLS        95U       /* a row of 'i's: 478 px / 5 px, and the NUL */
#define DIGI_SCOPE_FLOOR (-60.0f)  /* upstream's RTTY_SCOPE_DB_FLOOR */

static uint8_t         s_on;
static ui_digi_state_t s_st;
static char            s_fila[UDG_ROWS][DIGI_COLS + 1U];
static uint8_t         s_cur;          /* the row being written */
static uint8_t         s_col;
static uint8_t         s_eol;          /* the last character was a line end */
static uint8_t         s_cambio;       /* rows to repaint */
static uint8_t         s_todo;         /* the whole area to repaint */
static char            s_chip[48];         /* WEFAX's state is 29 + its slant */
static char            s_chip_visto[48];
static uint8_t         s_eng_visto = 0xFFU;
static float           s_db[RTTY_SCOPE_BINS];
static char            s_shift_txt[12];
static char            s_baud_txt[12];

/* upstream's k_rtty_shift_values[] and k_rtty_baud_values[]/_labels[] */
static const float      k_shift[4] = { 170.0f, 425.0f, 450.0f, 850.0f };
static const float      k_baud[4]  = { 45.45f, 50.0f, 75.0f, 100.0f };
static const char *const k_baud_txt[4] = { "45.45", "50", "75", "100" };

uint8_t digi_rtty_baud_idx(void)
{
    uint8_t i, best = 1U;
    float   d, dbest = 1.0e9f, b = rtty_get_baud();

    for (i = 0U; i < 4U; i++) {
        d = k_baud[i] - b;
        if (d < 0.0f) { d = -d; }
        if (d < dbest) { dbest = d; best = i; }
    }
    return best;
}

void digi_rtty_baud_pon(uint8_t idx)
{
    if (idx >= 4U) { return; }
    demod_quieto(1);
    rtty_set_baud(k_baud[idx]);
    demod_quieto(0);
}

/* upstream's color_tema(): a theme colour as gfx.c's RGB565 */
static uint16_t color_tema(uint32_t hex)
{
    return gfx2_to565((uint8_t)((hex >> 16) & 0xFFU),
                      (uint8_t)((hex >> 8) & 0xFFU), (uint8_t)(hex & 0xFFU));
}

static void borra_texto(void)
{
    uint8_t r;

    for (r = 0U; r < (uint8_t)UDG_ROWS; r++) { s_fila[r][0] = '\0'; }
    s_cur = 0U;
    s_col = 0U;
    s_eol = 1U;                         /* a leading \n makes no empty row */
    s_cambio = 1U;
}

/*
 * WEFAX - step 47, 22/11/2026. Upstream's fax screen (fax_panel_linea(),
 * fax_hdr_draw() in its main.c): the chart is NOT kept anywhere - each line
 * goes to the panel's own memory as it comes, in a ring the height of the
 * area with a red line on the row that comes next, and a menu over it wipes
 * it (upstream keeps it on a USB stick; this port has none, and the owner
 * chose to stay with upstream's way). The header is ui_digi's: Sync - the
 * line starts HERE, paint now - and, where upstream puts its save button,
 * the line rate, which upstream sets in Ajustes.
 *
 * THE SCALE IS THIS PORT'S, the owner's choice: PROPORTIONAL. An IOC 576
 * chart has square pixels, 1809 a line; the panel has 480 across, so a
 * screen row is 1809/480 = 3.77 chart lines, averaged - the chart looks
 * like a chart, ~310 rows for 1200 lines. Upstream draws a line a row (756
 * of its 800 px across), 2.4 times taller than wide.
 */
#define FAX_Y        ((uint16_t)(DIGI_SCOPE_Y + UDG_HDR_H))
#define FAX_ALTO     ((uint16_t)(DIGI_PANEL_Y + DIGI_PANEL_H - FAX_Y))
#define FAX_W        ((uint16_t)GFX2_W)
#define FAX_PX_LINEA 1809.0f    /* IOC 576: pi x 576 */

static uint16_t s_fax_fila;
static uint32_t s_fax_acc[GFX2_W];
static uint8_t  s_fax_cnt[GFX2_W];       /* source pixels per column */
static uint8_t  s_fax_n;                 /* lines in the accumulator */
static float    s_fax_pos;               /* how much of a row they make */
static uint16_t s_fax_px[GFX2_W];
static uint8_t  s_fax_linea[WEFAX_ANCHO];
static char     s_fax_lpm_txt[12];
static char     s_fax_chip[40];

static void fax_reinicia(void)
{
    uint16_t i;

    memset(s_fax_acc, 0, sizeof s_fax_acc);
    memset(s_fax_cnt, 0, sizeof s_fax_cnt);
    for (i = 0U; i < WEFAX_ANCHO; i++) {
        s_fax_cnt[(uint32_t)i * FAX_W / WEFAX_ANCHO]++;
    }
    s_fax_n = 0U;
    s_fax_pos = 0.0f;
    s_fax_fila = 0U;
}

/* The area under the header, cleared, and the rows from the top. */
static void fax_limpia(void)
{
    gfx_fill_rect(0, DIGI_SCOPE_Y, GFX2_W, (uint16_t)(FAX_Y + FAX_ALTO - DIGI_SCOPE_Y),
                  color_tema(PAL_SURF_0));
    fax_reinicia();
}

/* upstream's fax_gris565(): a grey of 8 bits, 5-6-5 */
static uint16_t fax_gris565(uint32_t g)
{
    if (g > 255U) { g = 255U; }
    return (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
}

static void fax_fila_pinta(void)
{
    uint16_t x;
    uint32_t den;

    for (x = 0U; x < FAX_W; x++) {
        den = (uint32_t)s_fax_cnt[x] * (uint32_t)s_fax_n;
        s_fax_px[x] = fax_gris565(den ? (s_fax_acc[x] + den / 2U) / den : 0U);
    }
    gfx_blit(0, (uint16_t)(FAX_Y + s_fax_fila), FAX_W, 1, s_fax_px);
    s_fax_fila++;
    if (s_fax_fila >= FAX_ALTO) { s_fax_fila = 0U; }
    /* upstream's red line on the next row: where it is, after the wrap */
    gfx_hline(0, (uint16_t)(FAX_Y + s_fax_fila), FAX_W, 0xF800U);
}

/*
 * From the main loop, whenever WEFAX is on - lines have to be taken out of
 * wefax.c's eight-line ring even under a menu. `pinta`: the screen is the
 * fax's (no menu). A row is drawn once its 3.77 lines are in.
 */
void digi_fax_poll(uint8_t pinta)
{
    static uint8_t tapado;
    uint16_t i;

    if (!s_on || !wefax_activo()) { return; }
    if (!pinta) {
        tapado = 1U;
    } else if (tapado) {
        tapado = 0U;
        fax_reinicia();                 /* digi_repinta() has cleared it */
    }
    while (wefax_linea_en(s_fax_linea)) {
        for (i = 0U; i < WEFAX_ANCHO; i++) {
            s_fax_acc[(uint32_t)i * FAX_W / WEFAX_ANCHO] += s_fax_linea[i];
        }
        s_fax_n++;
        s_fax_pos += (float)FAX_W / FAX_PX_LINEA;
        if (s_fax_pos >= 1.0f) {
            s_fax_pos -= 1.0f;
            if (pinta) { fax_fila_pinta(); }
            memset(s_fax_acc, 0, sizeof s_fax_acc);
            s_fax_n = 0U;
        }
    }
}

/* upstream's fax_hdr_draw(): the phase it is in, and the slant correction
 * once it has one; the line rate on the second button. */
static void fax_cabecera(void)
{
    wefax_info_t in;
    const char  *e;
    uint8_t      i = 0U;
    int32_t      ppm;
    uint32_t     lpm = wefax_get_lpm();

    wefax_info(&in);
    switch (in.estado) {
    case WEFAX_BUSCA:  e = tr("Esperando el tono de arranque", "Waiting for the start tone"); break;
    case WEFAX_FASE:   e = tr("Señal de fase", "Phasing signal"); break;
    case WEFAX_IMAGEN: e = tr("Recibiendo", "Receiving"); break;
    default:           e = tr("Parado", "Stopped"); break;
    }
    while (*e != '\0' && i < (uint8_t)(sizeof s_fax_chip - 12U)) { s_fax_chip[i++] = *e++; }
    ppm = (int32_t)in.ppm;
    if (ppm != 0) {
        uint32_t v = (uint32_t)(ppm < 0 ? -ppm : ppm);

        if (v > 9999U) { v = 9999U; }
        s_fax_chip[i++] = ' ';
        s_fax_chip[i++] = (ppm < 0) ? '-' : '+';
        if (v >= 1000U) { s_fax_chip[i++] = (char)('0' + v / 1000U); }
        if (v >= 100U)  { s_fax_chip[i++] = (char)('0' + (v / 100U) % 10U); }
        if (v >= 10U)   { s_fax_chip[i++] = (char)('0' + (v / 10U) % 10U); }
        s_fax_chip[i++] = (char)('0' + v % 10U);
    }
    s_fax_chip[i] = '\0';

    i = 0U;
    if (lpm >= 100U) { s_fax_lpm_txt[i++] = (char)('0' + lpm / 100U); }
    s_fax_lpm_txt[i++] = (char)('0' + (lpm / 10U) % 10U);
    s_fax_lpm_txt[i++] = (char)('0' + lpm % 10U);
    s_fax_lpm_txt[i++] = ' '; s_fax_lpm_txt[i++] = 'l';
    s_fax_lpm_txt[i++] = 'p'; s_fax_lpm_txt[i++] = 'm';
    s_fax_lpm_txt[i] = '\0';

    memset(&s_st, 0, sizeof s_st);
    s_st.y     = (int16_t)DIGI_SCOPE_Y;
    s_st.h     = (int16_t)UDG_HDR_H;
    s_st.filas = 0U;
    s_st.btn   = tr("Sincroniza", "Sync");
    s_st.btn2  = s_fax_lpm_txt;
    s_st.chip  = s_fax_chip;
    s_st.enganchado = (uint8_t)(in.estado == WEFAX_IMAGEN);
    strcpy(s_chip, s_fax_chip);
}

/*
 * SSTV - step 48, 22/11/2026. Upstream's sstv_panel_linea()/sstv_hdr_draw():
 * each line goes to the panel as it comes, at its own row, and nothing
 * keeps the picture (a menu wipes it, as WEFAX's chart). 320 x 256 does not
 * fit the 211 rows under the header, so - as WEFAX, proportional - the
 * picture is 263 x 211, centred: a line to row y * 211/256 (two now and
 * then on the same row, the later wins), its 320 points averaged to 263.
 * Upstream draws it 1:1 on its 800 x 480.
 *
 * The header: Empezar / Start (upstream's: search again, or start now in
 * the mode chosen) and, where upstream has its save button, the mode
 * choice - upstream picks it in a grid of its own; here a button that
 * steps Auto, then each mode, and applies at once, as upstream's grid does.
 */
#define SST_W  ((uint16_t)((SSTV_ANCHO * FAX_ALTO) / SSTV_ALTO))   /* 263 */
#define SST_X  ((uint16_t)((GFX2_W - SST_W) / 2U))

static uint8_t  s_sst_idx;               /* 0 Auto, k = mode k - 1 */
static uint16_t s_sst_px[GFX2_W];
static uint16_t s_sst_r[GFX2_W], s_sst_g[GFX2_W], s_sst_b[GFX2_W];
static char     s_sst_chip[40];

/* upstream's: Auto searches (the VIS header, or the lines' rhythm), a mode
 * starts in it now */
static void sstv_aplica(void)
{
    demod_quieto(1);
    if (s_sst_idx == 0U) {
        sstv_auto(1U);
        sstv_busca();
    } else {
        sstv_auto(0U);
        sstv_fuerza((uint8_t)(s_sst_idx - 1U));
    }
    demod_quieto(0);
}

static void sstv_linea_pinta(const uint8_t *l, uint16_t y)
{
    uint16_t x, xo, row;

    if (y >= SSTV_ALTO) { return; }
    row = (uint16_t)(((uint32_t)y * FAX_ALTO) / SSTV_ALTO);
    memset(s_sst_r, 0, sizeof s_sst_r);
    memset(s_sst_g, 0, sizeof s_sst_g);
    memset(s_sst_b, 0, sizeof s_sst_b);
    memset(s_fax_cnt, 0, sizeof s_fax_cnt);
    for (x = 0U; x < SSTV_ANCHO; x++) {
        xo = (uint16_t)(((uint32_t)x * SST_W) / SSTV_ANCHO);
        s_sst_r[xo] = (uint16_t)(s_sst_r[xo] + l[x * 3U]);
        s_sst_g[xo] = (uint16_t)(s_sst_g[xo] + l[x * 3U + 1U]);
        s_sst_b[xo] = (uint16_t)(s_sst_b[xo] + l[x * 3U + 2U]);
        s_fax_cnt[xo]++;
    }
    for (xo = 0U; xo < SST_W; xo++) {
        uint16_t n = s_fax_cnt[xo] ? s_fax_cnt[xo] : 1U;
        uint16_t r = (uint16_t)(s_sst_r[xo] / n), g = (uint16_t)(s_sst_g[xo] / n);
        uint16_t b = (uint16_t)(s_sst_b[xo] / n);

        s_sst_px[xo] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    gfx_blit(SST_X, (uint16_t)(FAX_Y + row), SST_W, 1, s_sst_px);
}

/* From the main loop whenever SSTV is on: the lines out of sstv.c's queue
 * even under a menu; drawn when `pinta`. */
void digi_sstv_poll(uint8_t pinta)
{
    const uint8_t *l;
    uint16_t y;

    if (!s_on || !sstv_activo()) { return; }
    while ((l = sstv_linea(&y)) != 0) {
        if (pinta) { sstv_linea_pinta(l, y); }
    }
}

/* upstream's sstv_hdr_draw() */
static void sstv_cabecera(void)
{
    sstv_info_t in;
    const char *e;
    uint8_t     i = 0U;

    sstv_info(&in);
    if (in.estado == SSTV_IMAGEN && in.modo) {
        uint32_t a = in.alto ? in.alto : SSTV_ALTO, v;

        e = in.modo;
        while (*e != '\0' && i < 20U) { s_sst_chip[i++] = *e++; }
        if (in.como == SSTV_COMO_RITMO) { s_sst_chip[i++] = '*'; }
        s_sst_chip[i++] = ' ';
        v = in.linea;
        if (v >= 100U) { s_sst_chip[i++] = (char)('0' + (v / 100U) % 10U); }
        if (v >= 10U)  { s_sst_chip[i++] = (char)('0' + (v / 10U) % 10U); }
        s_sst_chip[i++] = (char)('0' + v % 10U);
        s_sst_chip[i++] = '/';
        if (a >= 100U) { s_sst_chip[i++] = (char)('0' + (a / 100U) % 10U); }
        s_sst_chip[i++] = (char)('0' + (a / 10U) % 10U);
        s_sst_chip[i++] = (char)('0' + a % 10U);
    } else {
        e = in.vis ? tr("Modo no soportado, VIS ", "Unsupported mode, VIS ")
          : (sstv_auto_get() ? tr("Buscando el modo", "Finding the mode")
                             : tr("Esperando una imagen", "Waiting for a picture"));
        while (*e != '\0' && i < 30U) { s_sst_chip[i++] = *e++; }
        if (in.vis) {
            s_sst_chip[i++] = (char)('0' + (in.vis / 100U) % 10U);
            s_sst_chip[i++] = (char)('0' + (in.vis / 10U) % 10U);
            s_sst_chip[i++] = (char)('0' + in.vis % 10U);
        }
    }
    s_sst_chip[i] = '\0';

    memset(&s_st, 0, sizeof s_st);
    s_st.y     = (int16_t)DIGI_SCOPE_Y;
    s_st.h     = (int16_t)UDG_HDR_H;
    s_st.btn   = tr("Empezar", "Start");
    s_st.btn2  = (s_sst_idx == 0U) ? "Auto" : sstv_modo_nombre((uint8_t)(s_sst_idx - 1U));
    s_st.chip  = s_sst_chip;
    s_st.enganchado = (uint8_t)(in.estado == SSTV_IMAGEN);
    strcpy(s_chip, s_sst_chip);
}

/*
 * FELD-HELL - step 49, 22/11/2026. Upstream's hell_panel_columna() and
 * hell_hdr_draw(): each column of 14 points drawn TWICE, one copy over the
 * other - Hell has no vertical sync, the text drifts up or down, and with
 * two copies a whole letter is always there - 2 px wide, in strips across
 * the width, the next strip cleared as the writing reaches it. Nothing is
 * kept: a menu wipes it, as the fax and SSTV. Here 6 strips of 240 columns
 * fit the 211 rows (upstream: as many as its area holds). Borrar clears.
 */
#define HELL_COL_W   2U
#define HELL_TIRA_PX ((uint16_t)(2U * HELL_ALTO))
#define HELL_TIRA_H  ((uint16_t)(HELL_TIRA_PX + 4U))
#define HELL_TIRAS   ((uint16_t)(FAX_ALTO / HELL_TIRA_H))

static uint16_t s_hell_x;
static uint8_t  s_hell_tira;
static uint16_t s_hell_px[HELL_COL_W * 2U * HELL_ALTO];

static void hell_reinicia(void)
{
    s_hell_x = 0U;
    s_hell_tira = 0U;
}

static void hell_columna_pinta(const uint8_t *c)
{
    uint16_t y0 = (uint16_t)(FAX_Y + s_hell_tira * HELL_TIRA_H);
    uint16_t f, w;

    for (f = 0U; f < HELL_TIRA_PX; f++) {
        uint16_t g = c[f % (uint16_t)HELL_ALTO];
        uint16_t px = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));

        for (w = 0U; w < HELL_COL_W; w++) { s_hell_px[f * HELL_COL_W + w] = px; }
    }
    gfx_blit(s_hell_x, y0, HELL_COL_W, HELL_TIRA_PX, s_hell_px);
    s_hell_x = (uint16_t)(s_hell_x + HELL_COL_W);
    if ((uint16_t)(s_hell_x + HELL_COL_W) > GFX2_W) {
        s_hell_x = 0U;
        s_hell_tira = (uint8_t)((s_hell_tira + 1U) % HELL_TIRAS);
        gfx_fill_rect(0, (uint16_t)(FAX_Y + s_hell_tira * HELL_TIRA_H), GFX2_W, HELL_TIRA_H,
                      color_tema(PAL_SURF_0));
    }
}

/* From the main loop whenever Hell is on: columns out even under a menu,
 * drawn when `pinta`. */
void digi_hell_poll(uint8_t pinta)
{
    const uint8_t *c;

    if (!s_on || !hell_activo()) { return; }
    while ((c = hell_columna()) != 0) {
        if (pinta) { hell_columna_pinta(c); }
    }
}

/* upstream's hell_hdr_draw() */
static void hell_cabecera(void)
{
    hell_info_t in;
    const char *e;
    uint8_t     i = 0U;

    hell_info(&in);
    e = (in.columnas > 0UL) ? tr("Recibiendo", "Receiving")
                            : tr("Esperando señal", "Waiting for signal");
    while (*e != '\0' && i < (uint8_t)(sizeof s_chip - 1U)) { s_chip[i] = *e; i++; e++; }
    s_chip[i] = '\0';
    memset(&s_st, 0, sizeof s_st);
    s_st.y     = (int16_t)DIGI_SCOPE_Y;
    s_st.h     = (int16_t)UDG_HDR_H;
    s_st.btn   = tr("Borrar", "Clear");
    s_st.chip  = s_chip;
    s_st.enganchado = (uint8_t)(in.nivel > 0.01f);
}

/*
 * JTTY - step 50, 22/11/2026. Upstream's digi_sync() JTTY branch: the
 * FT8-style table over the whole area (no scope), the newest line last,
 * zebra rows, upstream's column titles, the chip its jtty_modo_estado_txt()
 * and the dot "an atom has come". Upstream's frequency button and its Map
 * are left out: this port has neither a JTTY frequency list nor maps.
 * Borrar clears. The columns are this port's (UDG_COLS_JTTY, 480 px).
 */
#define JTTY_FILAS ((uint8_t)((FAX_Y + FAX_ALTO - DIGI_SCOPE_Y - UDG_HDR_H) / UDG_LINE_H - 1U))

static uint32_t s_jtty_visto = 0xFFFFFFFFUL;

static void jtty_sync(void)
{
    uint8_t n = JTTY_FILAS, k, hay = 0U;

    if (n > (uint8_t)JTTY_MODO_LINEAS) { n = (uint8_t)JTTY_MODO_LINEAS; }
    memset(&s_st, 0, sizeof s_st);
    s_st.y       = (int16_t)DIGI_SCOPE_Y;
    s_st.h       = (int16_t)(FAX_Y + FAX_ALTO - DIGI_SCOPE_Y);
    s_st.filas   = n;
    s_st.btn     = tr("Borrar", "Clear");
    s_st.cebra   = 1U;
    s_st.cols    = ui_digi_cols_jtty;
    s_st.ncols   = UI_DIGI_COLS_JTTY_N;
    s_st.titulos = tr("Hora\tDesvío\tCal.\tMensaje\tPaís",
                      "Time\tOffset\tQual.\tMessage\tCountry");
    while ((hay < n) && (hay < jtty_modo_lineas())) { hay++; }
    for (k = 0U; k < hay; k++) { s_st.fila[k] = jtty_modo_linea(k); }
    s_st.chip = jtty_modo_estado_txt();
    s_st.enganchado = (uint8_t)(jtty_modo_atomos() != 0UL);
    strncpy(s_chip, s_st.chip ? s_st.chip : "", sizeof s_chip - 1U);
    s_chip[sizeof s_chip - 1U] = '\0';
    mapa_boton();
}

/*
 * ALE - step 51, 22/11/2026. Upstream's digi_sync() ALE branch and its
 * ale_chapa(): the table of stations over the whole area, as JTTY's (no
 * scope), in the order heard, no zebra (five columns, few rows),
 * upstream's column titles; Borrar; no channel button (ALE has no fixed
 * channels) and no map, as upstream. The bar is the quality of the last
 * word - the unanimous votes of the 2-of-3, out of 48 - while locked; and
 * without a lock, "searching for a word" until something has been heard,
 * then "last one N min": upstream's answer to "is the net quiet, or is the
 * decoder broken?", which an empty table cannot give. The chip counts the
 * words, the dot is the lock.
 *
 * The clock is kept here, as upstream keeps it in its main.c: ale_modo.c
 * has none. After the bar's text, what ale_rx.c's AFC has moved the tones
 * (23/11/2026, "  AFC +62"), when it is not zero. One word in upstream's bar was left Spanish in English
 * ("calidad"); here it is "quality".
 */
#define ALE_POLL_MS 16U                 /* upstream's main loop: digi_ale_poll() */

static uint32_t s_ale_poll_t;
static uint32_t s_ale_ult_ms;           /* when the last word came */
static uint32_t s_ale_pal_vista;
static uint8_t  s_ale_hubo;
static uint32_t s_ale_hash[ALE_MODO_LINEAS];
static uint32_t s_ale_gen;              /* moves when a row of the table does */
static uint32_t s_ale_gen_visto = 0xFFFFFFFFUL;
static char     s_ale_barra[40];
static char     s_ale_barra_visto[40];
static char     s_ale_chip[16];

static uint8_t pega(char *d, uint8_t i, uint8_t cap, const char *t)
{
    while (*t != '\0' && (uint8_t)(i + 1U) < cap) { d[i++] = *t++; }
    d[i] = '\0';
    return i;
}

static uint8_t pega_u(char *d, uint8_t i, uint8_t cap, uint32_t v)
{
    char t[11];
    uint8_t n = 0U;

    do { t[n++] = (char)('0' + (v % 10U)); v /= 10U; } while (v != 0U);
    while (n > 0U && (uint8_t)(i + 1U) < cap) { d[i++] = t[--n]; }
    d[i] = '\0';
    return i;
}

static uint16_t ale_desde_min(void)
{
    if (!s_ale_hubo) { return 0xFFFFU; }
    return (uint16_t)(((board_millis() - s_ale_ult_ms) / 1000U) / 60U);
}

void digi_ale_borra(void)
{
    s_ale_ult_ms = 0U;
    s_ale_pal_vista = 0U;
    s_ale_hubo = 0U;
    memset(s_ale_hash, 0, sizeof s_ale_hash);
    s_ale_gen++;
    s_ale_poll_t = board_millis();
}

/*
 * From the main loop, ALE on. UPSTREAM'S POLL AT UPSTREAM'S PACE: ale_modo.c
 * closes the address it is joining after 200 polls with no word - 3.2 s in
 * upstream's 16 ms loop, eight words' time. This port's loop runs as fast
 * as its drawing lets it, and at under 2 ms a pass 200 polls would be less
 * than one word: "TIS EA4" would be noted alone before its "DATA XYZ" came,
 * 392 ms later, and the DATA dropped as an extension with no anchor. So the
 * polls are counted in 16 ms of the clock, not in passes - any number per
 * pass, each one harmless (a poll takes what words there are, up to four).
 *
 * Then the clock of the last word, and each row of the table that changed
 * to the console - its hash moves when a station is added, heard again,
 * or its quality or count change.
 */
void digi_ale_poll(void)
{
    uint32_t now = board_millis(), n, w, k;

    n = (now - s_ale_poll_t) / ALE_POLL_MS;
    if (n > 0U) {
        s_ale_poll_t += n * ALE_POLL_MS;
        if (n > 64U) { n = 64U; }
        while (n-- > 0U) { ale_modo_poll(); }
    }
    w = ale_modo_palabras();
    if (w != s_ale_pal_vista) {
        s_ale_pal_vista = w;
        if (w != 0U) { s_ale_ult_ms = now; s_ale_hubo = 1U; }
    }
    for (k = 0U; k < ale_modo_lineas() && k < ALE_MODO_LINEAS; k++) {
        const char *l = ale_modo_linea(k), *p;
        uint32_t h = 2166136261UL;

        for (p = l; *p != '\0'; p++) { h = (h ^ (uint8_t)*p) * 16777619UL; }
        if (h != s_ale_hash[k]) {
            char t[ALE_MODO_LARGO];
            uint8_t i;

            s_ale_hash[k] = h;
            s_ale_gen++;
            for (i = 0U; l[i] != '\0' && i < sizeof t - 1U; i++) { t[i] = (l[i] == '\t') ? '|' : l[i]; }
            t[i] = '\0';
            console_printf("  ale: %u %s  (%u words, quality %u/48, afc %d Hz)\n", (uint32_t)k, t,
                           w, (uint32_t)ale_modo_calidad(), (int)ale_modo_afc_hz());
        }
    }
}

static void ale_sync(void)
{
    uint8_t  n = JTTY_FILAS, k, hay = 0U, i;
    uint8_t  cal = ale_modo_calidad(), eng = ale_modo_enganchado();
    uint32_t w = ale_modo_palabras();

    if (n > (uint8_t)ALE_MODO_LINEAS) { n = (uint8_t)ALE_MODO_LINEAS; }
    memset(&s_st, 0, sizeof s_st);
    s_st.y       = (int16_t)DIGI_SCOPE_Y;
    s_st.h       = (int16_t)(FAX_Y + FAX_ALTO - DIGI_SCOPE_Y);
    s_st.filas   = n;
    s_st.btn     = tr("Borrar", "Clear");
    s_st.cebra   = 0U;
    s_st.cols    = ui_digi_cols_ale;
    s_st.ncols   = UI_DIGI_COLS_ALE_N;
    s_st.titulos = tr("Indicativo\tTipo\tkHz\tCal.\tN",
                      "Callsign\tType\tkHz\tQual.\tN");
    while ((hay < n) && (hay < ale_modo_lineas())) { hay++; }
    for (k = 0U; k < hay; k++) { s_st.fila[k] = ale_modo_linea(k); }

    s_st.barra_on  = 1U;
    s_st.barra_max = 48U;
    s_st.barra_v   = eng ? cal : 0U;
    if (eng) {
        i = pega(s_ale_barra, 0U, (uint8_t)sizeof s_ale_barra, tr("calidad ", "quality "));
        i = pega_u(s_ale_barra, i, (uint8_t)sizeof s_ale_barra, cal);
        (void)pega(s_ale_barra, i, (uint8_t)sizeof s_ale_barra, "/48");
    } else {
        uint16_t min = ale_desde_min();

        if (min == 0xFFFFU) {
            (void)pega(s_ale_barra, 0U, (uint8_t)sizeof s_ale_barra,
                       tr("buscando palabra", "searching for a word"));
        } else {
            if (min > 999U) { min = 999U; }
            i = pega(s_ale_barra, 0U, (uint8_t)sizeof s_ale_barra,
                     tr("última hace ", "last one "));
            i = pega_u(s_ale_barra, i, (uint8_t)sizeof s_ale_barra, min);
            (void)pega(s_ale_barra, i, (uint8_t)sizeof s_ale_barra, " min");
        }
    }
    {
        /* Y lo que el CAF de ale_rx.c ha movido los tonos (23/11/2026): es,
         * sobre todo, el error de esta radio en esta frecuencia. */
        int16_t caf = ale_modo_afc_hz();

        if (caf != 0) {
            i = (uint8_t)strlen(s_ale_barra);
            i = pega(s_ale_barra, i, (uint8_t)sizeof s_ale_barra, tr("  CAF ", "  AFC "));
            i = pega(s_ale_barra, i, (uint8_t)sizeof s_ale_barra, (caf > 0) ? "+" : "-");
            (void)pega_u(s_ale_barra, i, (uint8_t)sizeof s_ale_barra,
                         (uint32_t)((caf > 0) ? caf : -caf));
        }
    }
    s_st.barra_txt = s_ale_barra;

    if (w > 9999U) { w = 9999U; }
    i = pega_u(s_ale_chip, 0U, (uint8_t)sizeof s_ale_chip, w);
    (void)pega(s_ale_chip, i, (uint8_t)sizeof s_ale_chip, tr(" pal", " words"));
    s_st.chip = s_ale_chip;
    s_st.enganchado = eng;
    strncpy(s_chip, s_ale_chip, sizeof s_chip - 1U);
    s_chip[sizeof s_chip - 1U] = '\0';
}

/*
 * STANAG - step 52, 23/11/2026. Upstream's digi_sync() STANAG branch, over
 * the whole area as JTTY's and ALE's, in its three jobs behind the waveform
 * button (btn2: Auto -> S4285 -> S4529 -> S4481 -> S4538 -> S4415):
 *
 *   Auto, S4529, S4538  the identifier: a zebra table, Period / Signal /
 *                       Quality, its first row upstream's loop instrument
 *   S4285, S4481        the 4285's text, one column; speed (btn3, with
 *                       AUTO), interleave (btn4), output format (btn5);
 *                       upstream's bar of the interleaver's filling is
 *                       left out - see below
 *   S4415               the 4415's text; only the format (btn5)
 *
 * The chip is stanag_modo_estado_txt(), the dot "a period is measured".
 *
 * NOT REPAINTED WHOLE: upstream repaints the panel at each beat of
 * stanag_modo_latido(), four times a second, which on the DEEPSDR's
 * parallel bus is cheap and on this SPI panel is ~90 ms each. Here each
 * beat goes through ui_digi_draw_cambios() - the rows, chip and buttons
 * that changed - with the bar left out of it (its every move would repaint
 * everything: firma_caja() holds it) and drawn on its own, and buttons 4
 * and 5, which cambios() does not watch, compared by hand.
 */
static uint32_t    s_sg_latido = 0xFFFFFFFFUL;
static uint16_t    s_sg_barra_v;
static const char *s_sg_b4;
static const char *s_sg_b5;
static uint32_t    s_sg_hash[STANAG_MODO_LINEAS];
static uint32_t    s_sg_log_t;

static void stanag_sync(void)
{
    uint8_t n = JTTY_FILAS, k, hay = 0U;

    if (n > (uint8_t)STANAG_MODO_LINEAS) { n = (uint8_t)STANAG_MODO_LINEAS; }
    memset(&s_st, 0, sizeof s_st);
    s_st.y     = (int16_t)DIGI_SCOPE_Y;
    s_st.h     = (int16_t)(FAX_Y + FAX_ALTO - DIGI_SCOPE_Y);
    s_st.filas = n;
    s_st.btn   = tr("Borrar", "Clear");
    s_st.btn2  = stanag_modo_forma_txt(stanag_modo_forma());
    s_st.cebra = 1U;
    if (stanag_modo_4415()) {
        s_st.btn5 = stanag_modo_fmt_txt();
    } else if (stanag_modo_demod()) {
        uint16_t hechos = 0U, hacen = 0U;

        stanag_modo_llenado(&hechos, &hacen);
        s_st.btn3      = stanag_modo_vel_txt();
        s_st.btn4      = stanag_modo_entre_txt();
        s_st.btn5      = stanag_modo_fmt_txt();
        /* The bar's width kept, so buttons 3-5 stand where they should,
         * but the bar NOT drawn: upstream shows the interleaver filling in
         * it, but it shares its slot with the waveform button (UDG_BAR_X is
         * UDG_BTN2_X), which on 800 px leaves some of it and on 480 hides
         * it; the filling is in the chip and in the last row anyway. */
        (void)hechos;
        s_st.barra_w   = (int16_t)DIGI_RTTY_BARRA_W;
        s_st.barra_on  = 0U;
    } else {
        s_st.cols    = ui_digi_cols_ident;
        s_st.ncols   = UI_DIGI_COLS_IDENT_N;
        s_st.titulos = tr("Periodo\tSeñal\tCalidad", "Period\tSignal\tQuality");
    }
    while ((hay < n) && (hay < stanag_modo_lineas())) { hay++; }
    for (k = 0U; k < hay; k++) { s_st.fila[k] = stanag_modo_linea(k); }
    s_st.chip = stanag_modo_estado_txt();
    s_st.enganchado = (uint8_t)(stanag_modo_periodo() != 0U);
    strncpy(s_chip, s_st.chip ? s_st.chip : "", sizeof s_chip - 1U);
    s_chip[sizeof s_chip - 1U] = '\0';
}

/* The panel through cambios(), the bar as it was last drawn; then the bar
 * and buttons 4 and 5 if they moved. Returns 0 if nothing could be drawn. */
static uint8_t stanag_pinta(void)
{
    ui_digi_state_t c = s_st;

    c.barra_v = s_sg_barra_v;
    if (!ui_digi_draw_cambios(&c)) { return 0U; }
    if (s_st.barra_on && s_st.barra_v != s_sg_barra_v) {
        ui_digi_draw_barra(&s_st);
        s_sg_barra_v = s_st.barra_v;
    }
    if (s_st.btn4 != s_sg_b4) { ui_digi_draw_boton4(&s_st); s_sg_b4 = s_st.btn4; }
    if (s_st.btn5 != s_sg_b5) { ui_digi_draw_boton5(&s_st); s_sg_b5 = s_st.btn5; }
    return 1U;
}

/* From the main loop, STANAG on: each row that changed to the console, at
 * most once a second - the 4285's text grows a character at a time. */
void digi_stanag_poll(void)
{
    uint32_t now = board_millis();
    uint8_t  k, n;

    if ((now - s_sg_log_t) < 1000U) { return; }
    s_sg_log_t = now;
    n = stanag_modo_lineas();
    if (n > (uint8_t)STANAG_MODO_LINEAS) { n = (uint8_t)STANAG_MODO_LINEAS; }
    for (k = 0U; k < n; k++) {
        const char *l = stanag_modo_linea(k), *p;
        uint32_t h = 2166136261UL;

        for (p = l; *p != '\0'; p++) { h = (h ^ (uint8_t)*p) * 16777619UL; }
        if (h != s_sg_hash[k]) {
            char t[STANAG_MODO_LARGO + 8U];
            uint8_t i;

            s_sg_hash[k] = h;
            for (i = 0U; l[i] != '\0' && i < sizeof t - 1U; i++) { t[i] = (l[i] == '\t') ? '|' : l[i]; }
            t[i] = '\0';
            console_printf("  stanag: %u %s\n", (uint32_t)k, t);
        }
    }
}

/*
 * AIS - step 55, 24/11/2026. Upstream's digi_sync() AIS branch and its
 * ais_chapa(): the table of ships over the whole area, zebra rows (six wide
 * columns), the channel in the titles ("N  (channel A)"), a ship's row
 * keeping its place; the bar the count of ships over the signal level
 * (0..99), the chip the frames with the CRC right over those closed. The
 * fourth button is upstream's frequency: 161.975 / 162.025, a tap goes to
 * the other channel (from any dial, to A first). No map: this port has
 * none. Repainted through cambios(), the bar on its own, as STANAG's.
 */
typedef struct { uint32_t khz; } ais_canal_t;
static const ais_canal_t k_ais_canales[2] = { { 161975UL }, { 162025UL } };

static char     s_ais_barra[28];
static char     s_ais_chip[16];
static char     s_ais_frec[12];
static uint8_t  s_ais_barra_v;
static uint32_t s_ais_hash[AIS_MODO_LINEAS];
static uint32_t s_ais_t;

static void ais_sync(void)
{
    uint8_t  n = JTTY_FILAS, k, hay = 0U, i;
    uint32_t b = ais_modo_tramas(), v = ais_modo_vistas(), khz = radio_dial_hz() / 1000U;
    uint8_t  barcos = ais_modo_barcos();

    if (n > (uint8_t)AIS_MODO_LINEAS) { n = (uint8_t)AIS_MODO_LINEAS; }
    memset(&s_st, 0, sizeof s_st);
    s_st.y      = (int16_t)DIGI_SCOPE_Y;
    s_st.h      = (int16_t)(FAX_Y + FAX_ALTO - DIGI_SCOPE_Y);
    s_st.filas  = n;
    s_st.btn    = tr("Borrar", "Clear");
    s_st.cebra  = 1U;
    s_st.cols   = ui_digi_cols_ais;
    s_st.ncols  = UI_DIGI_COLS_AIS_N;
    s_st.titulos = (ais_modo_canal() == 0U)
                   ? tr("MMSI\tNombre\tPosición\tVel\tRumbo\tN  (canal A)",
                        "MMSI\tName\tPosition\tSpd\tCourse\tN  (channel A)")
                   : tr("MMSI\tNombre\tPosición\tVel\tRumbo\tN  (canal B)",
                        "MMSI\tName\tPosition\tSpd\tCourse\tN  (channel B)");
    while ((hay < n) && (hay < ais_modo_lineas())) { hay++; }
    for (k = 0U; k < hay; k++) { s_st.fila[k] = ais_modo_linea((uint32_t)k); }

    /* upstream's ais_frec_rotulo(): kHz with a thousands point */
    i = pega_u(s_ais_frec, 0U, (uint8_t)sizeof s_ais_frec, khz / 1000U);
    i = pega(s_ais_frec, i, (uint8_t)sizeof s_ais_frec, ".");
    s_ais_frec[i++] = (char)('0' + ((khz / 100U) % 10U));
    s_ais_frec[i++] = (char)('0' + ((khz / 10U) % 10U));
    s_ais_frec[i++] = (char)('0' + (khz % 10U));
    s_ais_frec[i] = '\0';
    s_st.btn4 = s_ais_frec;

    s_st.barra_on  = 1U;
    s_st.barra_max = 99U;
    s_st.barra_v   = ais_modo_senal();
    i = pega_u(s_ais_barra, 0U, (uint8_t)sizeof s_ais_barra, barcos);
    (void)pega(s_ais_barra, i, (uint8_t)sizeof s_ais_barra,
               (barcos == 1U) ? tr(" barco", " ship") : tr(" barcos", " ships"));
    s_st.barra_txt = s_ais_barra;

    if (b > 999U) { b = 999U; }
    if (v > 999U) { v = 999U; }
    i = pega_u(s_ais_chip, 0U, (uint8_t)sizeof s_ais_chip, b);
    i = pega(s_ais_chip, i, (uint8_t)sizeof s_ais_chip, "/");
    (void)pega_u(s_ais_chip, i, (uint8_t)sizeof s_ais_chip, v);
    s_st.chip = s_ais_chip;
    s_st.enganchado = (uint8_t)(b > 0U);
    strncpy(s_chip, s_ais_chip, sizeof s_chip - 1U);
    s_chip[sizeof s_chip - 1U] = '\0';
    mapa_boton();
}

/* The panel through cambios(), the bar as last drawn; then the bar. */
static uint8_t ais_pinta(void)
{
    ui_digi_state_t c = s_st;

    if (s_mapa && digi_mapa_modo()) {
        /* the map when a frame came, the bar when the level moved */
        uint32_t t = ais_modo_tramas();

        if (t != s_ais_mapa_visto) {
            ui_digi_draw_cabecera(&s_st);
            digi_mapa_dibuja();
            s_ais_mapa_visto = t;
            s_ais_barra_v = s_st.barra_v;
        } else if (s_st.barra_v != s_ais_barra_v) {
            ui_digi_draw_barra(&s_st);
            s_ais_barra_v = s_st.barra_v;
        }
        return 1U;
    }

    c.barra_v = s_ais_barra_v;
    if (!ui_digi_draw_cambios(&c)) { return 0U; }
    if (s_st.barra_v != s_ais_barra_v) {
        ui_digi_draw_barra(&s_st);
        s_ais_barra_v = s_st.barra_v;
    }
    return 1U;
}

void digi_ais_borra(void)
{
    memset(s_ais_hash, 0, sizeof s_ais_hash);
}

/* From the main loop, AIS on: each row that changed to the console. */
void digi_ais_poll(void)
{
    uint32_t k, n = ais_modo_lineas();

    if (n > AIS_MODO_LINEAS) { n = AIS_MODO_LINEAS; }
    for (k = 0U; k < n; k++) {
        const char *l = ais_modo_linea(k), *p;
        uint32_t h = 2166136261UL;

        for (p = l; *p != '\0'; p++) { h = (h ^ (uint8_t)*p) * 16777619UL; }
        if (h != s_ais_hash[k]) {
            char t[AIS_MODO_LARGO + 4U];
            uint32_t i;

            s_ais_hash[k] = h;
            for (i = 0U; l[i] != '\0' && i < sizeof t - 1U; i++) { t[i] = (l[i] == '\t') ? '|' : l[i]; }
            t[i] = '\0';
            console_printf("  ais: %u %s  (%u/%u frames)\n", k, t, ais_modo_tramas(), ais_modo_vistas());
        }
    }
}

/*
 * HFDL - step 59, 26/11/2026. Upstream's digi_sync() HFDL branch, its
 * hfdl_chapa() and its channel table: the aircraft over the whole area,
 * newest last, zebra rows (ICAO Model Flight Position, the ground station
 * last heard in the title, N - UDG_COLS_HFDL). The bar the
 * receiver's phase (searching / half a signal / almost / LOCKED) or, once a
 * preamble has been seen, its funnel "A1>A2>M1" and "U ud/pos"; the chip
 * the signal % and the frames right / seen, with upstream's P/Z/X count of
 * frames that did not fit when there are any. The fourth button the
 * channel: upstream's 29 HFDL frequencies (Atlantic and Gulf ground
 * stations), the next on a tap, lit when the dial is on one. Upstream's
 * third (a grid of the channels with the frames each gave) is not here yet;
 * its fifth - Map, or "Bytes", the rows' raw frames - is left out: with a
 * fifth button the chip has 86 px here, and "99% 999/999 P999" needs 129.
 * The Model column needs upstream's ICAO24.BIN (external flash); this
 * radio has none, so it stays empty.
 */
typedef struct { uint32_t khz; const char *estacion; } hfdl_canal_t;
/* upstream's k_hfdl_canales[] (night group under 10 MHz, day group over) */
static const hfdl_canal_t k_hfdl_canales[] = {
    {  2986U, "Bahrein" }, {  2998U, "Shannon" }, {  3455U, "Shannon" },
    {  3900U, "Reykjavik" }, {  5544U, "Bahrein" }, {  5547U, "Shannon" },
    {  5720U, "Reykjavik" }, {  6529U, "Canarias" }, {  6532U, "Shannon" },
    {  6646U, "Bahrein" }, {  6712U, "Reykjavik" }, {  8843U, "Shannon" },
    {  8885U, "Bahrein" }, {  8942U, "Shannon" }, {  8948U, "Canarias" },
    {  8977U, "Reykjavik" }, { 10030U, "Bahrein" }, { 10081U, "Shannon" },
    { 11184U, "Reykjavik" }, { 11348U, "Canarias" }, { 11384U, "Shannon" },
    { 13303U, "Canarias" }, { 13312U, "Bahrein" }, { 15025U, "Reykjavik" },
    { 17928U, "Canarias" }, { 17967U, "Bahrein" }, { 17985U, "Reykjavik" },
    { 21955U, "Canarias" }, { 21982U, "Bahrein" },
};
#define HFDL_CANALES ((int16_t)(sizeof k_hfdl_canales / sizeof k_hfdl_canales[0]))

static char     s_hf_tit[64];
static char     s_hf_barra[36];
static char     s_hf_chip[24];
static char     s_hf_frec[12];
static uint32_t s_hf_visto = 0xFFFFFFFFUL;
static uint8_t  s_hf_barra_v = 0xFFU;
static char     s_hf_barra_visto[36];
static char     s_hf_chip_visto[24];
static uint32_t s_hf_log;               /* frames right, logged */
static uint32_t s_hf_lin_log;           /* rows, logged */
static uint32_t s_hf_t;

static int16_t hfdl_canal_actual(void)
{
    int16_t i;
    uint32_t khz = radio_dial_hz() / 1000U;

    if ((radio_dial_hz() % 1000U) != 0U) { return -1; }
    for (i = 0; i < HFDL_CANALES; i++) { if (k_hfdl_canales[i].khz == khz) { return i; } }
    return -1;
}

static uint8_t pega_lim(char *d, uint8_t i, uint8_t cap, uint32_t v)
{
    return pega_u(d, i, cap, (v > 999U) ? 999U : v);
}

static void hfdl_sync(void)
{
    static const texto_t k_fase[4] = {
        T("buscando", "searching"), T("media señal", "half a signal"),
        T("casi", "almost"), T("ENGANCHADO", "LOCKED")
    };
    uint8_t  n = JTTY_FILAS, k, hay = 0U, i;
    uint32_t khz = radio_dial_hz() / 1000U;
    uint8_t  e = hfdl_modo_estado();
    const char *gs = hfdl_modo_estacion();

    memset(&s_st, 0, sizeof s_st);
    s_st.y      = (int16_t)DIGI_SCOPE_Y;
    s_st.h      = (int16_t)(FAX_Y + FAX_ALTO - DIGI_SCOPE_Y);
    s_st.filas  = n;
    s_st.btn    = tr("Borrar", "Clear");
    s_st.cebra  = 1U;
    s_st.cols   = ui_digi_cols_hfdl;
    s_st.ncols  = UI_DIGI_COLS_HFDL_N;
    i = pega(s_hf_tit, 0U, (uint8_t)sizeof s_hf_tit,
             tr("OACI\tModelo\tVuelo\tPosición\t", "ICAO\tModel\tFlight\tPosition\t"));
    if (gs[0] != '\0') {
        i = pega(s_hf_tit, i, (uint8_t)sizeof s_hf_tit, "> ");
        i = pega(s_hf_tit, i, (uint8_t)(sizeof s_hf_tit - 4U), gs);
    } else {
        i = pega(s_hf_tit, i, (uint8_t)sizeof s_hf_tit, tr("Último", "Latest"));
    }
    (void)pega(s_hf_tit, i, (uint8_t)sizeof s_hf_tit, "\tN");
    s_st.titulos = s_hf_tit;

    while (hay < n && hay < hfdl_modo_lineas()) { hay++; }
    for (k = 0U; k < hay; k++) { s_st.fila[k] = hfdl_modo_linea((uint32_t)(hay - 1U - k)); }

    /* upstream's hfdl_frec_rotulo(): kHz with a thousands point */
    if (khz >= 1000U) {
        i = pega_u(s_hf_frec, 0U, (uint8_t)sizeof s_hf_frec, khz / 1000U);
        i = pega(s_hf_frec, i, (uint8_t)sizeof s_hf_frec, ".");
        s_hf_frec[i++] = (char)('0' + ((khz / 100U) % 10U));
        s_hf_frec[i++] = (char)('0' + ((khz / 10U) % 10U));
        s_hf_frec[i++] = (char)('0' + (khz % 10U));
        s_hf_frec[i] = '\0';
    } else {
        (void)pega_u(s_hf_frec, 0U, (uint8_t)sizeof s_hf_frec, khz);
    }
    s_st.btn4 = s_hf_frec;
    s_st.btn4_press = (uint8_t)(hfdl_canal_actual() >= 0);

    /* upstream's hfdl_chapa(): the bar */
    if (e > 3U) { e = 3U; }
    s_st.barra_on  = 1U;
    s_st.barra_max = 3U;
    s_st.barra_v   = e;
    s_st.enganchado = (uint8_t)(e == 3U);
    {
        uint32_t a1 = 0U, a2 = 0U, m1 = 0U, ud = 0U, pos = 0U;

        hfdl_modo_embudo(&a1, &a2, &m1);
        hfdl_modo_datos(&ud, &pos);
        if (a1 == 0U) {
            s_st.barra_txt = k_fase[e][idioma()];
        } else {
            i = pega_lim(s_hf_barra, 0U, (uint8_t)sizeof s_hf_barra, a1);
            i = pega(s_hf_barra, i, (uint8_t)sizeof s_hf_barra, ">");
            i = pega_lim(s_hf_barra, i, (uint8_t)sizeof s_hf_barra, a2);
            i = pega(s_hf_barra, i, (uint8_t)sizeof s_hf_barra, ">");
            i = pega_lim(s_hf_barra, i, (uint8_t)sizeof s_hf_barra, m1);
            if (ud != 0U || pos != 0U) {
                i = pega(s_hf_barra, i, (uint8_t)sizeof s_hf_barra, " U");
                i = pega_u(s_hf_barra, i, (uint8_t)sizeof s_hf_barra, (ud > 99U) ? 99U : ud);
                i = pega(s_hf_barra, i, (uint8_t)sizeof s_hf_barra, "/");
                (void)pega_u(s_hf_barra, i, (uint8_t)sizeof s_hf_barra, (pos > 99U) ? 99U : pos);
            }
            s_st.barra_txt = s_hf_barra;
        }
    }
    /* and the chip: the signal, frames right / seen, the ones that did not
     * fit (P overwritten, Z rate 7, X the rest) */
    {
        uint32_t nc = hfdl_modo_no_cupo(), n7 = hfdl_modo_no_cupo_7(), np = hfdl_modo_pisada();
        uint32_t n6 = (nc > (n7 + np)) ? (nc - n7 - np) : 0U;

        i = pega_u(s_hf_chip, 0U, (uint8_t)sizeof s_hf_chip, hfdl_modo_senal());
        i = pega(s_hf_chip, i, (uint8_t)sizeof s_hf_chip, "% ");
        i = pega_lim(s_hf_chip, i, (uint8_t)sizeof s_hf_chip, hfdl_modo_buenas());
        i = pega(s_hf_chip, i, (uint8_t)sizeof s_hf_chip, "/");
        i = pega_lim(s_hf_chip, i, (uint8_t)sizeof s_hf_chip, hfdl_modo_total());
        /* The P/Z/X count of frames that did not fit went for the Map
         * button's place (step 64, the owner's choice): with a fifth
         * button the chip has 86 px, "99% 999/999" fits, "P999" did not.
         * Kept on the console (digi_hfdl_poll()). */
        (void)nc; (void)n7; (void)np; (void)n6; (void)i;
    }
    s_st.chip = s_hf_chip;
    strncpy(s_chip, s_hf_chip, sizeof s_chip - 1U);
    s_chip[sizeof s_chip - 1U] = '\0';
    mapa_boton();
}

void digi_hfdl_borra(void)
{
    s_hf_log = hfdl_modo_buenas();
    s_hf_lin_log = hfdl_modo_lineas();
}

/* From the main loop, HFDL on: each frame right and each new or changed
 * row to the console, at most four times a second. */
void digi_hfdl_poll(void)
{
    uint32_t b, nl;

    if ((board_millis() - s_hf_t) < 250U) { return; }
    s_hf_t = board_millis();
    b = hfdl_modo_buenas();
    nl = hfdl_modo_lineas();
    if (b < s_hf_log) { s_hf_log = 0U; }
    if (b != s_hf_log || nl != s_hf_lin_log) {
        const char *l = (nl > 0U) ? hfdl_modo_linea(0U) : "";
        char t[80];
        uint32_t j;

        for (j = 0U; l[j] != '\0' && j < sizeof t - 1U; j++) { t[j] = (l[j] == '\t') ? '|' : l[j]; }
        t[j] = '\0';
        console_printf("  hfdl: %u/%u frames, %u rows, %s; newest: %s\n", b, hfdl_modo_total(), nl,
                       hfdl_modo_estacion()[0] ? hfdl_modo_estacion() : "-", t);
        s_hf_log = b;
        s_hf_lin_log = nl;
    }
}

/*
 * WSPR - step 58, 25/11/2026. Upstream's digi_sync() WSPR branch and its
 * wspr_chapa(): the beacons heard over the whole area, the newest last,
 * zebra rows (Time Callsign Loc. dBm Offset Qual., UDG_COLS_WSPR in
 * malahit_layout.h); the bar the 2-minute cycle - "starts in 1:47" waiting
 * for the even minute, "listening 63 %" through the 110.6 s window - with
 * the last attempt's corroboration ("q 92", 88 needed) once there has been
 * one; the chip the beacons heard, or "then 20 m" when the band button was
 * pressed during a window. The fourth button is upstream's dial: its ten
 * WSPR frequencies, the next on a tap - kept until the window closes if one
 * is open (upstream's wspr_cambio_pendiente()). No map.
 * Upstream's bar texts are Spanish in both languages; here tr() gives them
 * English too.
 */
typedef struct { uint32_t hz; const char *banda; } wspr_canal_t;
/* upstream's k_wspr_canales[] */
static const wspr_canal_t k_wspr_canales[] = {
    {  1836600UL, "160 m" }, {  3568600UL,  "80 m" }, {  5287200UL,  "60 m" },
    {  7038600UL,  "40 m" }, { 10138700UL,  "30 m" }, { 14095600UL,  "20 m" },
    { 18104600UL,  "17 m" }, { 21094600UL,  "15 m" }, { 24924600UL,  "12 m" },
    { 28124600UL,  "10 m" },
};
#define WSPR_CANALES ((int16_t)(sizeof k_wspr_canales / sizeof k_wspr_canales[0]))

static char     s_ws_barra[40];
static char     s_ws_chip[16];
static char     s_ws_frec[12];
static int16_t  s_ws_pide = -1;         /* a band asked for during a window */
static uint32_t s_ws_visto = 0xFFFFFFFFUL;
static uint8_t  s_ws_barra_v = 0xFFU;
static char     s_ws_barra_visto[40];
static char     s_ws_chip_visto[16];
static uint32_t s_ws_log;               /* beacons logged */
static uint8_t  s_ws_capt;              /* capturing, last poll */

static int16_t wspr_canal_actual(void)
{
    int16_t i;
    uint32_t hz = radio_dial_hz();

    for (i = 0; i < WSPR_CANALES; i++) { if (k_wspr_canales[i].hz == hz) { return i; } }
    return -1;
}

static void wspr_sync(void)
{
    uint8_t  n = JTTY_FILAS, k, hay = 0U, i;
    int16_t  c = (s_ws_pide >= 0) ? s_ws_pide : wspr_canal_actual();
    uint32_t hz = (c >= 0) ? k_wspr_canales[c].hz : radio_dial_hz();
    uint32_t khz = hz / 1000U;
    uint8_t  q = wspr_modo_q();

    if (n > (uint8_t)WSPR_MODO_LINEAS) { n = (uint8_t)WSPR_MODO_LINEAS; }
    memset(&s_st, 0, sizeof s_st);
    s_st.y      = (int16_t)DIGI_SCOPE_Y;
    s_st.h      = (int16_t)(FAX_Y + FAX_ALTO - DIGI_SCOPE_Y);
    s_st.filas  = n;
    s_st.btn    = tr("Borrar", "Clear");
    s_st.cebra  = 1U;
    s_st.cols   = ui_digi_cols_wspr;
    s_st.ncols  = UI_DIGI_COLS_WSPR_N;
    s_st.titulos = tr("Hora\tIndicativo\tLoc.\tdBm\tDesvío\tCal.",
                      "Time\tCallsign\tLoc.\tdBm\tOffset\tQual.");
    while (hay < n && hay < wspr_modo_lineas()) { hay++; }
    for (k = 0U; k < hay; k++) { s_st.fila[k] = wspr_modo_linea((uint32_t)(hay - 1U - k)); }

    /* upstream's wspr_frec_rotulo(): kHz with a thousands point and the
     * hundreds of hertz - "14.095,6" */
    i = 0U;
    if (khz >= 1000U) {
        i = pega_u(s_ws_frec, 0U, (uint8_t)sizeof s_ws_frec, khz / 1000U);
        i = pega(s_ws_frec, i, (uint8_t)sizeof s_ws_frec, ".");
        s_ws_frec[i++] = (char)('0' + ((khz / 100U) % 10U));
        s_ws_frec[i++] = (char)('0' + ((khz / 10U) % 10U));
        s_ws_frec[i++] = (char)('0' + (khz % 10U));
    } else {
        i = pega_u(s_ws_frec, 0U, (uint8_t)sizeof s_ws_frec, khz);
    }
    s_ws_frec[i++] = ',';
    s_ws_frec[i++] = (char)('0' + ((hz % 1000U) / 100U));
    s_ws_frec[i] = '\0';
    s_st.btn4 = s_ws_frec;

    s_st.barra_on  = 1U;
    s_st.barra_max = 100U;
    if (!wspr_modo_hay_hora()) {
        s_st.barra_v    = 0U;
        s_st.barra_txt  = tr("pon el reloj en hora", "set the clock first");
        s_st.chip       = tr("sin hora", "no clock");
        s_st.enganchado = 0U;
        strncpy(s_chip, s_st.chip, sizeof s_chip - 1U);
        return;
    }
    if (wspr_modo_capturando()) {
        uint8_t pc = wspr_modo_por_ciento();

        s_st.barra_v    = pc;
        s_st.enganchado = 1U;
        i = pega(s_ws_barra, 0U, (uint8_t)sizeof s_ws_barra, tr("escuchando ", "listening "));
        i = pega_u(s_ws_barra, i, (uint8_t)sizeof s_ws_barra, pc);
        i = pega(s_ws_barra, i, (uint8_t)sizeof s_ws_barra, " %");
    } else {
        uint16_t f = wspr_modo_espera_s();

        s_st.barra_v    = (uint8_t)((f > 119U) ? 100U : ((f * 100U) / 119U));
        s_st.enganchado = 0U;
        i = pega(s_ws_barra, 0U, (uint8_t)sizeof s_ws_barra, tr("empieza en ", "starts in "));
        i = pega_u(s_ws_barra, i, (uint8_t)sizeof s_ws_barra, (uint32_t)(f / 60U));
        i = pega(s_ws_barra, i, (uint8_t)sizeof s_ws_barra, ":");
        s_ws_barra[i++] = (char)('0' + ((f % 60U) / 10U));
        s_ws_barra[i++] = (char)('0' + (f % 10U));
        s_ws_barra[i] = '\0';
    }
    if (q != 0U) {                      /* upstream's wspr_pon_q() */
        i = pega(s_ws_barra, i, (uint8_t)sizeof s_ws_barra, "  q ");
        (void)pega_u(s_ws_barra, i, (uint8_t)sizeof s_ws_barra, q);
    }
    s_st.barra_txt = s_ws_barra;
    if (s_ws_pide >= 0) {
        i = pega(s_ws_chip, 0U, (uint8_t)sizeof s_ws_chip, tr("luego ", "then "));
        (void)pega(s_ws_chip, i, (uint8_t)sizeof s_ws_chip, k_wspr_canales[s_ws_pide].banda);
    } else {
        uint32_t b = wspr_modo_buenas();

        if (b > 999U) { b = 999U; }
        i = pega_u(s_ws_chip, 0U, (uint8_t)sizeof s_ws_chip, b);
        (void)pega(s_ws_chip, i, (uint8_t)sizeof s_ws_chip,
                   (b == 1U) ? tr(" oída", " heard") : tr(" oídas", " heard"));
    }
    s_st.chip = s_ws_chip;
    strncpy(s_chip, s_ws_chip, sizeof s_chip - 1U);
    s_chip[sizeof s_chip - 1U] = '\0';
    mapa_boton();
}

void digi_wspr_borra(void)
{
    s_ws_pide = -1;
    s_ws_log = wspr_modo_buenas();
}

/*
 * From the main loop, WSPR on: the band asked for during a window, once it
 * has closed (upstream's wspr_cambio_pendiente()); each beacon to the
 * console; and each window's end with its corroboration, decoded or not.
 */
void digi_wspr_poll(void)
{
    uint8_t capt = wspr_modo_capturando();

    if (s_ws_pide >= 0 && !capt) {
        (void)radio_dial_go(k_wspr_canales[s_ws_pide].hz);
        console_printf("  digi: WSPR dial %u Hz (asked for during the window)\n",
                       k_wspr_canales[s_ws_pide].hz);
        s_ws_pide = -1;
        s_todo = 1U;
    }
    if (capt && !s_ws_capt) {
        console_printf("  wspr: window open\n");
    }
    if (!capt && s_ws_capt) {
        uint32_t b = wspr_modo_buenas();

        if (b != s_ws_log && wspr_modo_lineas() > 0U) {
            const char *l = wspr_modo_linea(0U);
            char t[WSPR_MODO_LARGO + 4U];
            uint32_t j;

            for (j = 0U; l[j] != '\0' && j < sizeof t - 1U; j++) { t[j] = (l[j] == '\t') ? '|' : l[j]; }
            t[j] = '\0';
            console_printf("  wspr: %s\n", t);
        }
        console_printf("  wspr: window closed, %s, q %u (88 to accept), %u heard\n",
                       (b != s_ws_log) ? "DECODED" : "nothing", (uint32_t)wspr_modo_q(), b);
        s_ws_log = b;
    }
    s_ws_capt = capt;
}

/*
 * FT8 - step 57, 25/11/2026. Upstream's digi_sync() FT8 branch and its
 * ft8_chapa(): the table over the whole area, oldest first and stuck to the
 * top, CQs in their colour (UDG_T_LLAMA), no zebra; the second layout of the
 * header (barra_dcha): Borrar, the CQ filter, the frequency - upstream's nine
 * FT8 dials, a tap goes to the next - and the bar of the slot on the right,
 * its fill the second, "8/15" by the dot, and in the middle the candidates,
 * then whichever diagnostic says something (sub-blocks lost, a decode over
 * 7.5 s, the slot's dB range when nothing looked like FT8) and the mean DT.
 * No clock: the bar says so. Upstream's fifth button, the Map, is left out
 * (this port has no maps); its slot stays empty, as the bar's place is fixed.
 * A tap on the bar squares the slot by hand (ft8_modo_cuadra()); a finger
 * dragged on the rows scrolls the 48 kept (ft8_lista.c), as upstream's.
 * Repainted whole when a decode comes or the view moves, the bar each
 * second.
 */
typedef struct { uint32_t khz; } ft8_canal_t;
/* upstream's k_ft8_canales[], checked there against sigidwiki (27/09/2026) */
static const ft8_canal_t k_ft8_canales[] = {
    {  1840UL }, {  3573UL }, {  7074UL }, { 10136UL }, { 14074UL },
    { 18100UL }, { 21074UL }, { 24915UL }, { 28074UL },
};
#define FT8_CANALES ((uint8_t)(sizeof k_ft8_canales / sizeof k_ft8_canales[0]))
#define FT8_BARRA_MS_AVISA 7500U        /* upstream's: half a slot */

static char     s_ft8_barra[40];
static char     s_ft8_ranura[8];
static char     s_ft8_frec[12];
static uint8_t  s_ft8_cq;               /* the CQ filter */
static uint8_t  s_ft8_subidos;          /* rows the finger moved up */
static uint32_t s_ft8_total_ant;        /* ft8_modo_metidas() last seen */
static uint32_t s_ft8_visto = 0xFFFFFFFFUL;  /* ... when the panel was drawn */
static uint8_t  s_ft8_vista_ant;        /* the view (filter, scroll) drawn */
static uint8_t  s_ft8_seg_visto = 0xFFU;
static char     s_ft8_barra_visto[40];
static int16_t  s_ft8_acum, s_ft8_y;    /* the finger, in rows */
static uint32_t s_ft8_log;              /* ft8_modo_total() logged */
static uint32_t s_ft8_ranuras;          /* slots logged */
static uint8_t  s_ft8_seg_log = 0xFFU;  /* the slot's second, last poll */

/* upstream's ft8_punto(): " · " (U+00B7, in font_ui_10b) */
static uint8_t ft8_punto(char *b, uint8_t i, uint8_t cap)
{
    return pega(b, i, cap, " \xC2\xB7 ");
}

static uint8_t ft8_num(char *b, uint8_t i, uint8_t cap, int32_t v)
{
    if (v < 0) { i = pega(b, i, cap, "-"); v = -v; }
    return pega_u(b, i, cap, (uint32_t)v);
}

/* upstream's ft8_chapa(): the bar */
static void ft8_barra(void)
{
    uint8_t i = 0U;
    uint16_t c, ms, pd;
    int16_t dt;

    s_st.barra_on  = 1U;
    s_st.barra_max = 15U;
    s_st.chip = 0;                      /* the bar carries what it said */
    if (!ft8_modo_hay_hora()) {
        s_st.barra_v = 0U;
        s_st.barra_txt = tr("pon el reloj en hora", "set the clock first");
        s_st.barra_txt_dcha = 0;
        s_st.enganchado = 0U;
        return;
    }
    s_st.barra_v = ft8_modo_segundo();
    i = ft8_num(s_ft8_ranura, 0U, (uint8_t)sizeof s_ft8_ranura, (int32_t)ft8_modo_segundo());
    (void)pega(s_ft8_ranura, i, (uint8_t)sizeof s_ft8_ranura, "/15");
    s_st.barra_txt_dcha = s_ft8_ranura;

    c = ft8_modo_candidatos();
    ms = ft8_modo_ms();
    pd = ft8_modo_perdidos();
    dt = ft8_modo_dt_ms();
    i = ft8_num(s_ft8_barra, 0U, (uint8_t)sizeof s_ft8_barra, (int32_t)((c > 999U) ? 999U : c));
    i = pega(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, "c");
    if (pd > 0U) {
        i = ft8_punto(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra);
        i = ft8_num(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, (int32_t)((pd > 99U) ? 99U : pd));
        i = pega(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, "p");
    } else if (ms >= FT8_BARRA_MS_AVISA) {
        i = ft8_punto(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra);
        i = ft8_num(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, (int32_t)((ms > 9999U) ? 9999U : ms));
        i = pega(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, " ms");
    } else if (c == 0U) {
        int16_t lo = 0, hi = 0;

        ft8_modo_db(&lo, &hi);
        if (lo < -99) { lo = -99; }
        if (lo >  99) { lo =  99; }
        if (hi < -99) { hi = -99; }
        if (hi >  99) { hi =  99; }
        i = ft8_punto(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra);
        i = ft8_num(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, (int32_t)lo);
        i = pega(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, (hi >= 0) ? "/+" : "/");
        i = ft8_num(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, (int32_t)hi);
        i = pega(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, " dB");
    }
    /* the mean DT, in seconds with one figure, as WSJT-X */
    i = ft8_punto(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra);
    if (dt > 9999) { dt = 9999; }
    if (dt < -9999) { dt = -9999; }
    i = pega(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, (dt < 0) ? "-" : "+");
    if (dt < 0) { dt = (int16_t)(-dt); }
    i = pega_u(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, (uint32_t)(dt / 1000));
    i = pega(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, ",");
    i = pega_u(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, (uint32_t)((dt / 100) % 10));
    (void)pega(s_ft8_barra, i, (uint8_t)sizeof s_ft8_barra, " s");
    s_st.barra_txt = s_ft8_barra;
    s_st.enganchado = 1U;
}

static void ft8_sync(void)
{
    uint8_t n = JTTY_FILAS, k, hay, guardados = 0U, i;
    uint8_t idx[FT8_MODO_LINEAS];
    uint32_t khz = radio_dial_hz() / 1000U, total = ft8_modo_metidas();

    if (n > (uint8_t)FT8_MODO_LINEAS) { n = (uint8_t)FT8_MODO_LINEAS; }
    memset(&s_st, 0, sizeof s_st);
    s_st.y       = (int16_t)DIGI_SCOPE_Y;
    s_st.h       = (int16_t)(FAX_Y + FAX_ALTO - DIGI_SCOPE_Y);
    s_st.filas   = n;
    s_st.btn     = tr("Borrar", "Clear");
    s_st.btn3    = "CQ";
    s_st.btn3_press = s_ft8_cq;
    s_st.barra_dcha = 1U;
    s_st.cols    = ui_digi_cols_ft8;
    s_st.ncols   = UI_DIGI_COLS_FT8_N;
    s_st.titulos = tr("Hora\tHz\tdB\tDT\tMensaje\tkm\tPaís",
                      "Time\tHz\tdB\tDT\tMessage\tkm\tCountry");

    /* upstream's ft8_frec_rotulo(): kHz with a thousands point */
    i = pega_u(s_ft8_frec, 0U, (uint8_t)sizeof s_ft8_frec, khz / 1000U);
    i = pega(s_ft8_frec, i, (uint8_t)sizeof s_ft8_frec, ".");
    s_ft8_frec[i++] = (char)('0' + ((khz / 100U) % 10U));
    s_ft8_frec[i++] = (char)('0' + ((khz / 10U) % 10U));
    s_ft8_frec[i++] = (char)('0' + (khz % 10U));
    s_ft8_frec[i] = '\0';
    s_st.btn4 = s_ft8_frec;

    /* the rows: all those kept are looked at (the CQ filter would otherwise
     * search half a list), the window kept where the finger left it while
     * new ones come in at the top - upstream's, through ft8_lista.c */
    while (guardados < (uint8_t)FT8_MODO_LINEAS && ft8_modo_linea(guardados)[0] != '\0') {
        guardados++;
    }
    if (total < s_ft8_total_ant) { s_ft8_total_ant = total; }      /* mode re-entered */
    if (s_ft8_subidos != 0U && total > s_ft8_total_ant) {
        uint32_t ins = total - s_ft8_total_ant;
        uint16_t sube;

        if (ins > (uint32_t)FT8_MODO_LINEAS) { ins = (uint32_t)FT8_MODO_LINEAS; }
        sube = (uint16_t)((uint16_t)s_ft8_subidos
                          + (uint16_t)ft8_lista_nuevos(ft8_modo_linea, (uint8_t)ins, s_ft8_cq));
        s_ft8_subidos = (sube > 255U) ? 255U : (uint8_t)sube;
    }
    s_ft8_total_ant = total;
    hay = ft8_lista_escoge(ft8_modo_linea, guardados, s_ft8_cq, n, &s_ft8_subidos, idx);
    for (k = 0U; k < hay; k++) {
        s_st.fila[k] = ft8_modo_linea(idx[k]);
        s_st.fila_tinta[k] = ft8_lista_es_cq(s_st.fila[k]) ? UDG_T_LLAMA : UDG_T_NORMAL;
    }
    ft8_barra();
    mapa_boton();
}

void digi_ft8_borra(void)
{
    s_ft8_subidos = 0U;
    s_ft8_acum = 0;
    s_ft8_total_ant = ft8_modo_metidas();
    s_ft8_log = ft8_modo_total();
    s_ft8_ranuras = 0U;
}

/*
 * From the main loop, FT8 on: each decode to the console as it comes - its
 * fields with '|' for the tabs - and once a slot, its numbers: candidates,
 * decodes, ms the decode took, sub-blocks lost, slots decoded short, the
 * mean DT and the slot's dB range. The last line is the level
 * check this port needs: upstream's 4-bit spectrogram is a fixed ladder
 * (-35 dB + 3.4 dB a step after FT8_ADAPTER_DB_OFFSET), so the radio's
 * audio level decides where its noise sits on it.
 */
void digi_ft8_poll(void)
{
    uint32_t total = ft8_modo_total();

    if (total < s_ft8_log) { s_ft8_log = 0UL; }
    if (total != s_ft8_log) {
        uint32_t nuevos = total - s_ft8_log, k;

        if (nuevos > (uint32_t)FT8_MODO_LINEAS) { nuevos = (uint32_t)FT8_MODO_LINEAS; }
        s_ft8_log = total;
        for (k = nuevos; k > 0U; k--) {
            const char *l = ft8_modo_linea((uint8_t)(k - 1U));
            char t[FT8_MODO_LARGO + 4U];
            uint32_t j;

            if (l[0] == '\0') { continue; }
            for (j = 0U; l[j] != '\0' && j < sizeof t - 1U; j++) { t[j] = (l[j] == '\t') ? '|' : l[j]; }
            t[j] = '\0';
            console_printf("  ft8: %s\n", t);
        }
    }
    /* and the slot's numbers once a slot, at its second 5: the decode of
     * the slot before has started at its edge and, at the 8 ms a pass it
     * is given, ended (it takes well under a second). Keyed on the second
     * and not on the decode's end, which can begin and finish inside one
     * ft8_modo_tick() and leave nothing to see from here. */
    if (ft8_modo_hay_hora()) {
        uint8_t seg = ft8_modo_segundo();

        if (seg >= 5U && s_ft8_seg_log < 5U) {
            int16_t lo = 0, hi = 0;
            float sg = 0.0f, g = 0.0f;

            s_ft8_ranuras++;
            ft8_modo_db(&lo, &hi);
            demod_ft8_nivel(&sg, &g);
            console_printf("  ft8 slot %u: %u candidates, %u decoded, %u ms, lost %u, short %u, "
                           "DT %d ms, dB %d..%d, in %u.%u LSB x %u\n",
                           s_ft8_ranuras, (uint32_t)ft8_modo_candidatos(),
                           (uint32_t)ft8_modo_ultima(), (uint32_t)ft8_modo_ms(),
                           (uint32_t)ft8_modo_perdidos(), (uint32_t)ft8_modo_cortas(),
                           (int)ft8_modo_dt_ms(), (int)lo, (int)hi,
                           (uint32_t)sg, (uint32_t)(sg * 10.0f) % 10U, (uint32_t)(g + 0.5f));
        }
        s_ft8_seg_log = seg;
    }
}

/*
 * IDENT - step 53, 23/11/2026. Upstream's digi_sync() IDENT branch: rows
 * over the whole area, no columns (a name, its note, the five numbers -
 * phrases, not fields), Borrar, no waveform button (here nothing is
 * chosen, it is asked); the chip "measuring" or "listening", the dot when
 * a width has been measured. Repainted through cambios(), as STANAG's.
 */
static uint32_t s_id_t;
static uint32_t s_id_hash[IDENT_LINEAS];
static uint32_t s_id_log_t;

static void ident_sync(void)
{
    uint8_t n = JTTY_FILAS, k, hay = 0U;
    stanag_det_t m;

    if (n > (uint8_t)IDENT_LINEAS) { n = (uint8_t)IDENT_LINEAS; }
    memset(&s_st, 0, sizeof s_st);
    s_st.y     = (int16_t)DIGI_SCOPE_Y;
    s_st.h     = (int16_t)(FAX_Y + FAX_ALTO - DIGI_SCOPE_Y);
    s_st.filas = n;
    s_st.btn   = tr("Borrar", "Clear");
    while ((hay < n) && (hay < ident_modo_lineas())) { hay++; }
    for (k = 0U; k < hay; k++) { s_st.fila[k] = ident_modo_linea(k); }
    ident_medidas(&m);
    s_st.chip = (m.hay_esp || m.vale) ? tr("midiendo", "measuring")
                                      : tr("escuchando", "listening");
    s_st.enganchado = (uint8_t)(m.hay_esp && (m.ancho10_hz != 0U));
    strncpy(s_chip, s_st.chip, sizeof s_chip - 1U);
    s_chip[sizeof s_chip - 1U] = '\0';
}

/* From the main loop, IDENT on: each row that changed to the console, at
 * most once a second. */
void digi_ident_poll(void)
{
    uint32_t now = board_millis();
    uint8_t  k, n;

    if ((now - s_id_log_t) < 1000U) { return; }
    s_id_log_t = now;
    n = ident_modo_lineas();
    if (n > (uint8_t)IDENT_LINEAS) { n = (uint8_t)IDENT_LINEAS; }
    for (k = 0U; k < n; k++) {
        const char *l = ident_modo_linea(k), *p;
        uint32_t h = 2166136261UL;

        for (p = l; *p != '\0'; p++) { h = (h ^ (uint8_t)*p) * 16777619UL; }
        if (h != s_id_hash[k]) {
            s_id_hash[k] = h;
            console_printf("  ident: %u %s\n", (uint32_t)k, l);
        }
    }
}

void digi_pon(uint8_t on)
{
    s_on = (uint8_t)(on != 0U);
    s_mapa = 0U;                        /* upstream's: the last mode's stations */
    borra_texto();
    s_chip_visto[0] = '\0';
    s_eng_visto = 0xFFU;
    s_todo = 1U;
    if (s_on) { rtty_scope_avg_reset(); }
    /* upstream, on entering SSTV: "the chip rules" - Auto searches, a mode
     * chosen before waits in that mode */
    if (s_on && sstv_activo()) {
        sstv_auto((uint8_t)(s_sst_idx == 0U));
    }
}

uint8_t digi_en_pantalla(void) { return s_on; }

/* upstream's rtty_text_newline(): the next row, or all of them up one */
static void nueva_fila(void)
{
    if (s_cur + 1U < (uint8_t)UDG_ROWS) {
        s_cur++;
    } else {
        uint8_t r;

        for (r = 0U; r + 1U < (uint8_t)UDG_ROWS; r++) {
            memcpy(s_fila[r], s_fila[r + 1U], sizeof s_fila[r]);
        }
    }
    s_fila[s_cur][0] = '\0';
    s_col = 0U;
    s_cambio = 1U;
}

void digi_texto(char c)
{
    if (c == '\r' || c == '\n') {
        if (!s_eol) { nueva_fila(); }
        s_eol = 1U;
        return;
    }
    s_eol = 0U;
    if (s_col >= DIGI_COLS) {
        nueva_fila();
    } else if (s_col > 0U) {
        char uno[2] = { c, '\0' };

        if (gfx2_text_w(s_fila[s_cur], DIGI_FONT) + gfx2_text_w(uno, DIGI_FONT)
            > DIGI_TEXT_W) {
            nueva_fila();
        }
    }
    if (s_col == 0U && c == ' ') { return; }    /* no row starts with a space */
    s_fila[s_cur][s_col++] = c;
    s_fila[s_cur][s_col] = '\0';
    s_cambio = 1U;
}

/* upstream's digi_sync(), the CW branch */
static void sincroniza(void)
{
    uint8_t  r, i = 0U;
    uint32_t w = (uint32_t)(cw_get_wpm() + 0.5f);

    const char *u = tr("ppm", "wpm");
    float    lo = cw_get_floor(), hi = cw_get_peak();
    uint32_t db = 0U;

    memset(&s_st, 0, sizeof s_st);
    s_st.y     = (int16_t)DIGI_PANEL_Y;
    s_st.h     = (int16_t)DIGI_PANEL_H;
    s_st.filas = (uint8_t)UDG_ROWS;
    s_st.btn   = tr("Borrar", "Clear");
    for (r = 0U; r < (uint8_t)UDG_ROWS; r++) {
        s_st.fila[r] = (s_fila[r][0] != '\0') ? s_fila[r] : 0;
    }

    if (rtty_get_enabled()) {
        /* upstream's RTTY header: the bar shortened to make room for the
         * shift, then the speed, then Auto - and no chip. */
        uint32_t sh = (uint32_t)(rtty_get_shift_hz() + 0.5f);
        const char *b = k_baud_txt[digi_rtty_baud_idx()];

        i = 0U;
        if (sh >= 1000U) { s_shift_txt[i++] = (char)('0' + sh / 1000U); }
        if (sh >= 100U)  { s_shift_txt[i++] = (char)('0' + (sh / 100U) % 10U); }
        s_shift_txt[i++] = (char)('0' + (sh / 10U) % 10U);
        s_shift_txt[i++] = (char)('0' + sh % 10U);
        s_shift_txt[i++] = ' '; s_shift_txt[i++] = 'H'; s_shift_txt[i++] = 'z';
        s_shift_txt[i] = '\0';
        i = 0U;
        while (*b != '\0') { s_baud_txt[i++] = *b++; }
        s_baud_txt[i++] = ' '; s_baud_txt[i++] = 'B'; s_baud_txt[i++] = 'd';
        s_baud_txt[i] = '\0';
        s_st.barra_w    = (int16_t)DIGI_RTTY_BARRA_W;
        s_st.btn3       = s_shift_txt;
        s_st.btn4       = s_baud_txt;
        s_st.btn5       = tr("Auto", "Auto");
        s_st.btn5_press = rtty_auto_get();
        s_chip[0] = '\0';
        s_st.chip = 0;
        s_st.enganchado = 0U;
        return;
    }

    if (ax25_activo()) {
        /* AX.25 - step 54. Upstream shows no chip in APRS or PKT300; here
         * the frames that passed their check ("12 ok"), and the dot ax25.c's
         * own "flags seen lately" - the two questions a quiet packet
         * channel leaves: is anything there, and does it decode. */
        ax25_info_t in;
        uint32_t ok;

        ax25_info(&in);
        ok = (in.tramas_ok > 9999U) ? 9999U : in.tramas_ok;
        i = 0U;
        if (ok >= 1000U) { s_chip[i++] = (char)('0' + ok / 1000U); }
        if (ok >= 100U)  { s_chip[i++] = (char)('0' + (ok / 100U) % 10U); }
        if (ok >= 10U)   { s_chip[i++] = (char)('0' + (ok / 10U) % 10U); }
        s_chip[i++] = (char)('0' + ok % 10U);
        s_chip[i++] = ' '; s_chip[i++] = 'o'; s_chip[i++] = 'k';
        {
            /* and in PKT300, what its AFC has moved the tones - the scope's
             * two lines move with it (ax25_marca_hz()) */
            int32_t caf = (int32_t)ax25_caf_hz();

            if (caf >= 5 || caf <= -5) {
                uint32_t v = (uint32_t)((caf < 0) ? -caf : caf);

                s_chip[i++] = ' ';
                s_chip[i++] = (caf < 0) ? '-' : '+';
                if (v >= 100U) { s_chip[i++] = (char)('0' + v / 100U); }
                if (v >= 10U)  { s_chip[i++] = (char)('0' + (v / 10U) % 10U); }
                s_chip[i++] = (char)('0' + v % 10U);
                s_chip[i++] = ' '; s_chip[i++] = 'H'; s_chip[i++] = 'z';
            }
        }
        s_chip[i] = '\0';
        s_st.chip = s_chip;
        s_st.enganchado = in.enganchado;
        return;
    }

    if (psk31_activo()) {
        /* upstream's: the chip is the FREQUENCY ERROR, "-3,4 Hz" - the
         * number to tune by - and the dot is the quality (PSK31_CAL_MIN). */
        uint8_t cal = 0U;
        int16_t e = 0;
        int32_t v;

        psk31_diag(&cal, &e);
        v = (int32_t)e;
        i = 0U;
        if (v < 0) { s_chip[i++] = '-'; v = -v; } else { s_chip[i++] = '+'; }
        if (v > 9999) { v = 9999; }
        if (v / 1000 != 0) { s_chip[i++] = (char)('0' + v / 1000); }
        s_chip[i++] = (char)('0' + (v / 100) % 10);
        s_chip[i++] = ',';
        s_chip[i++] = (char)('0' + (v / 10) % 10);
        s_chip[i++] = ' '; s_chip[i++] = 'H'; s_chip[i++] = 'z';
        s_chip[i] = '\0';
        s_st.chip = s_chip;
        s_st.enganchado = (uint8_t)(cal >= PSK31_CAL_MIN);
        return;
    }

    if (navtex_activo()) {
        /* upstream's digi_sync(), its NAVTEX branch: the chip says which
         * phase the decoder is in - it takes seconds to lock, and then a
         * station can be silent for minutes - and once a header has come,
         * its four letters (who sends, what about, the number). Copied:
         * navtex_info() fills a local. The dot is the character sync. */
        navtex_info_t in;
        const char   *t;

        navtex_info(&in);
        if (in.tiene_cab) {
            for (i = 0U; i < 4U; i++) { s_chip[i] = in.cab[i]; }
            s_chip[4] = '\0';
        } else {
            t = in.car_sync ? "SITOR" : in.bit_sync ? "bits" : tr("buscando", "searching");
            for (i = 0U; t[i] != '\0' && i < (uint8_t)(sizeof s_chip - 1U); i++) {
                s_chip[i] = t[i];
            }
            s_chip[i] = '\0';
        }
        s_st.chip = s_chip;
        s_st.enganchado = in.car_sync;
        return;
    }

    if (dsc_activo()) {
        /* This port's: upstream's digi_sync() has no DSC branch. What the
         * decoder is doing - a call is a few seconds now and then, and
         * between them "searching" is the honest word - with the lock as
         * the dot. The calls themselves go to the rows. */
        dsc_info_t in;
        const char *t;

        dsc_info(&in);
        t = in.en_mensaje ? tr("llamada", "call") : in.enganchado ? "sync"
          : tr("buscando", "searching");
        for (i = 0U; t[i] != '\0' && i < (uint8_t)(sizeof s_chip - 1U); i++) {
            s_chip[i] = t[i];
        }
        s_chip[i] = '\0';
        s_st.chip = s_chip;
        s_st.enganchado = in.enganchado;
        return;
    }

    s_st.btn2       = tr("Aprende", "Learn");
    s_st.btn2_press = cw_get_aprende();

    /* "20 ppm 14 dB": the speed, and the margin between cw.c's two
     * means in dB - not the signal's SNR, what the threshold has to work
     * with (upstream's comment there). */
    if (w > 99U) { w = 99U; }
    if (w >= 10U) { s_chip[i++] = (char)('0' + w / 10U); }
    s_chip[i++] = (char)('0' + w % 10U);
    s_chip[i++] = ' ';
    while (*u != '\0') { s_chip[i++] = *u++; }
    if (lo > 0.0f && hi > lo) {
        float d = 20.0f * log10f(hi / lo);

        if (d > 99.0f) { d = 99.0f; }
        if (d > 0.0f)  { db = (uint32_t)(d + 0.5f); }
    }
    s_chip[i++] = ' ';
    if (db >= 10U) { s_chip[i++] = (char)('0' + db / 10U); }
    s_chip[i++] = (char)('0' + db % 10U);
    s_chip[i++] = ' '; s_chip[i++] = 'd'; s_chip[i++] = 'B';
    s_chip[i] = '\0';
    s_st.chip = s_chip;
    s_st.enganchado = cw_get_decode_ok();
}

/* upstream's rtty_scope_draw(), its CW lines (scope_rayas()) */
static void osciloscopio(void)
{
    const float *mag;
    float    nyq = rtty_scope_hz_per_bin() * (float)RTTY_SCOPE_BINS;
    uint32_t k;

    rtty_scope_poll();
    if (!rtty_scope_frame_ready()) { return; }
    mag = rtty_scope_get_frame();
    for (k = 0U; k < RTTY_SCOPE_BINS; k++) {
        float p = mag[k];

        if (p < 1.0e-6f) { p = 1.0e-6f; }
        s_db[k] = 10.0f * log10f(p);
        if (s_db[k] < DIGI_SCOPE_FLOOR) { s_db[k] = DIGI_SCOPE_FLOOR; }
    }
    spectrum_draw(s_db, RTTY_SCOPE_BINS, 0, DIGI_SCOPE_Y, GFX2_W, DIGI_SCOPE_H,
                  DIGI_SCOPE_FLOOR, 0.0f, 0, 0U, 0, 0);
    if (nyq > 0.0f && navtex_activo()) {
        /* NAVTEX: B (the higher tone, unless reversed) in the accent, Y in
         * the warning colour, 85 Hz either side of where the decoder
         * listens NOW - its setting plus what it measured by itself.
         * Upstream's scope_rayas() has no NAVTEX branch and drew RTTY's. */
        navtex_info_t in;
        float c, m, e;
        uint16_t a, b;

        navtex_info(&in);
        c = navtex_get_centro_hz() + (float)in.desvio_hz;
        m = navtex_get_invertido() ? c - 0.5f * NAVTEX_SHIFT_HZ : c + 0.5f * NAVTEX_SHIFT_HZ;
        e = navtex_get_invertido() ? c + 0.5f * NAVTEX_SHIFT_HZ : c - 0.5f * NAVTEX_SHIFT_HZ;
        a = (uint16_t)(m / nyq * (float)GFX2_W);
        b = (uint16_t)(e / nyq * (float)GFX2_W);
        if (a < GFX2_W) { gfx_vline(a, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_ACCENT)); }
        if (b < GFX2_W) { gfx_vline(b, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_WARN)); }
    } else if (nyq > 0.0f && dsc_activo()) {
        /* DSC: B, the higher tone (ITU-R M.493 section 1), in the accent,
         * Y in the warning colour, 85 Hz either side of where it listens. */
        float    c = dsc_get_centro_hz();
        uint16_t a = (uint16_t)((c + 0.5f * DSC_SHIFT_HZ) / nyq * (float)GFX2_W);
        uint16_t b = (uint16_t)((c - 0.5f * DSC_SHIFT_HZ) / nyq * (float)GFX2_W);

        if (a < GFX2_W) { gfx_vline(a, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_ACCENT)); }
        if (b < GFX2_W) { gfx_vline(b, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_WARN)); }
    } else if (nyq > 0.0f && psk31_activo()) {
        /* PSK31: where it listens. Upstream's scope_rayas() has no branch
         * for it and drew RTTY's two lines; one line at the centre is
         * what there is to tune to. */
        uint16_t a = (uint16_t)(psk31_get_centro_hz() / nyq * (float)GFX2_W);

        if (a < GFX2_W) { gfx_vline(a, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_ACCENT)); }
    } else if (nyq > 0.0f && ax25_activo()) {
        /* AX.25: upstream's scope_rayas() branch - the tones from ax25.c
         * itself, 1200/2200 in APRS and 1600/1800 in PKT300 - mark in the
         * accent, space in the warning colour */
        uint16_t a = (uint16_t)(ax25_marca_hz()   / nyq * (float)GFX2_W);
        uint16_t b = (uint16_t)(ax25_espacio_hz() / nyq * (float)GFX2_W);

        if (a < GFX2_W) { gfx_vline(a, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_ACCENT)); }
        if (b < GFX2_W) { gfx_vline(b, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_WARN)); }
    } else if (nyq > 0.0f && rtty_get_enabled()) {
        /* RTTY: mark in the accent, space in the warning colour */
        uint16_t a = (uint16_t)(rtty_get_mark_hz()  / nyq * (float)GFX2_W);
        uint16_t b = (uint16_t)(rtty_get_space_hz() / nyq * (float)GFX2_W);

        if (a < GFX2_W) { gfx_vline(a, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_ACCENT)); }
        if (b < GFX2_W) { gfx_vline(b, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_WARN)); }
    } else if (nyq > 0.0f) {
        /* CW: the pitch set, muted, and the tone locked, in the accent */
        uint16_t a = (uint16_t)(cw_get_pitch_hz()  / nyq * (float)GFX2_W);
        uint16_t b = (uint16_t)(cw_get_detect_hz() / nyq * (float)GFX2_W);

        if (a < GFX2_W && a != b) {
            gfx_vline(a, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_INK_MUTE));
        }
        if (b < GFX2_W) {
            gfx_vline(b, DIGI_SCOPE_Y, DIGI_SCOPE_H, color_tema(PAL_ACCENT));
        }
    }
}

/* The fifth button of the five modes with a map: "Map" lit when shown;
 * HFDL's says which view it is in, as upstream's ("List"/"Map" - its
 * "Bytes" view is not ported). */
static void mapa_boton(void)
{
    if (!digi_mapa_modo()) { return; }
    s_st.btn5 = hfdl_modo_activo() ? (s_mapa ? tr("Mapa", "Map") : tr("Lista", "List"))
                                   : tr("Mapa", "Map");
    s_st.btn5_press = s_mapa;
}

/* The rows, or the header and the map in their place. */
static uint8_t filas_o_mapa(void)
{
    if (s_mapa && digi_mapa_modo()) {
        ui_digi_draw_cabecera(&s_st);
        digi_mapa_dibuja();
        return 1U;
    }
    return ui_digi_draw_texto(&s_st);
}

uint8_t digi_mapa_visible(void)
{
    return (uint8_t)(s_on && s_mapa && digi_mapa_modo());
}

uint8_t digi_ft8_solo_cq(void) { return s_ft8_cq; }

void digi_suelta(void)
{
    if (digi_mapa_visible()) { digi_mapa_suelta(); }
}

void digi_repinta(void)
{
    if (!s_on) { return; }
    if (ais_modo_activo()) {
        ais_sync();
        ui_digi_olvida();
        s_ais_barra_v = s_st.barra_v;
        s_ais_mapa_visto = 0xFFFFFFFFU;     /* the map too, whole */
        if (ais_pinta()) { s_ais_t = board_millis(); s_todo = 0U; }
        return;
    }
    if (ident_modo_activo()) {
        ident_sync();
        ui_digi_olvida();
        if (ui_digi_draw_cambios(&s_st)) {
            s_id_t = board_millis();
            s_todo = 0U;
        }
        return;
    }
    if (stanag_modo_activo()) {
        stanag_sync();
        ui_digi_olvida();
        s_sg_barra_v = s_st.barra_v;
        if (stanag_pinta()) {
            s_sg_b4 = s_st.btn4;
            s_sg_b5 = s_st.btn5;
            s_sg_latido = stanag_modo_latido();
            s_todo = 0U;
        }
        return;
    }
    if (ale_modo_activo()) {
        ale_sync();
        ui_digi_olvida();
        if (ui_digi_draw_texto(&s_st)) {
            s_ale_gen_visto = s_ale_gen;
            s_todo = 0U;
        }
        strcpy(s_chip_visto, s_chip);
        strcpy(s_ale_barra_visto, s_ale_barra);
        s_eng_visto = s_st.enganchado;
        return;
    }
    if (jtty_modo_activo()) {
        jtty_sync();
        ui_digi_olvida();
        if (filas_o_mapa()) {
            s_jtty_visto = jtty_modo_atomos();
            s_todo = 0U;
        }
        strcpy(s_chip_visto, s_chip);
        s_eng_visto = s_st.enganchado;
        return;
    }
    if (hfdl_modo_activo()) {
        hfdl_sync();
        ui_digi_olvida();
        if (filas_o_mapa()) {
            s_hf_visto = hfdl_modo_lineas() ^ (hfdl_modo_buenas() << 8);
            s_hf_barra_v = s_st.barra_v;
            strcpy(s_hf_barra_visto, s_st.barra_txt ? s_st.barra_txt : "");
            strcpy(s_hf_chip_visto, s_st.chip ? s_st.chip : "");
            s_todo = 0U;
        }
        return;
    }
    if (wspr_modo_activo()) {
        wspr_sync();
        ui_digi_olvida();
        if (filas_o_mapa()) {
            s_ws_visto = wspr_modo_lineas() | ((uint32_t)wspr_modo_buenas() << 8);
            s_ws_barra_v = s_st.barra_v;
            strcpy(s_ws_barra_visto, s_st.barra_txt ? s_st.barra_txt : "");
            strcpy(s_ws_chip_visto, s_st.chip ? s_st.chip : "");
            s_todo = 0U;
        }
        return;
    }
    if (ft8_modo_activo()) {
        ft8_sync();
        ui_digi_olvida();
        if (filas_o_mapa()) {
            s_ft8_visto = ft8_modo_metidas();
            s_ft8_vista_ant = (uint8_t)(s_ft8_cq | (uint8_t)(s_ft8_subidos << 1));
            s_ft8_seg_visto = s_st.barra_v;
            strcpy(s_ft8_barra_visto, s_st.barra_txt ? s_st.barra_txt : "");
            s_todo = 0U;
        }
        return;
    }
    if (hell_activo()) {
        fax_limpia();
        hell_reinicia();
        hell_cabecera();
        ui_digi_draw_cabecera(&s_st);
        strcpy(s_chip_visto, s_chip);
        s_eng_visto = s_st.enganchado;
        s_todo = 0U;
        return;
    }
    if (sstv_activo()) {
        fax_limpia();                   /* the same area, cleared */
        sstv_cabecera();
        ui_digi_draw_cabecera(&s_st);
        strcpy(s_chip_visto, s_chip);
        s_eng_visto = s_st.enganchado;
        s_todo = 0U;
        return;
    }
    if (wefax_activo()) {
        fax_limpia();
        fax_cabecera();
        ui_digi_draw_cabecera(&s_st);
        strcpy(s_chip_visto, s_chip);
        s_eng_visto = s_st.enganchado;
        s_todo = 0U;
        return;
    }
    gfx_fill_rect(0, DIGI_SCOPE_Y, GFX2_W, DIGI_SCOPE_H + DIGI_SCOPE_GAP,
                  color_tema(PAL_SURF_0));
    spectrum_invalida();
    s_todo = 1U;
    digi_dibuja();
}

void digi_dibuja(void)
{
    if (!s_on) { return; }
    if (!s_todo && s_mapa && digi_mapa_modo()) { digi_mapa_poll(); }
    if (ais_modo_activo()) {
        /* four times a second, only what changed - upstream repaints the
         * panel when the frame count moves, the bar when the level does */
        if (s_todo) { digi_repinta(); return; }
        if ((board_millis() - s_ais_t) < 250U) { return; }
        ais_sync();
        if (ais_pinta()) { s_ais_t = board_millis(); }
        return;
    }
    if (ident_modo_activo()) {
        /* four times a second, only what changed - upstream repaints the
         * panel when the rows or the first row's name change, and its
         * counters are in the rows */
        if (s_todo) { digi_repinta(); return; }
        if ((board_millis() - s_id_t) < 250U) { return; }
        ident_sync();
        if (ui_digi_draw_cambios(&s_st)) { s_id_t = board_millis(); }
        return;
    }
    if (stanag_modo_activo()) {
        /* upstream's: at each beat of the latido (four a second, and at
         * every new row); here only what changed (above) */
        uint32_t t;

        if (s_todo) { digi_repinta(); return; }
        t = stanag_modo_latido();
        if (t == s_sg_latido) { return; }
        stanag_sync();
        if (stanag_pinta()) { s_sg_latido = t; }
        return;
    }
    if (ale_modo_activo()) {
        /* the whole panel when a row of the table moves; the bar and the
         * chip when their text or the dot do - upstream repaints the panel
         * at every word, and its bar only when the quality moves (so an
         * unlock left "calidad 48/48" up until the next minute) */
        if (s_todo || s_ale_gen != s_ale_gen_visto) { digi_repinta(); return; }
        ale_sync();
        if (strcmp(s_chip_visto, s_chip) != 0 || s_eng_visto != s_st.enganchado
            || strcmp(s_ale_barra_visto, s_ale_barra) != 0) {
            strcpy(s_chip_visto, s_chip);
            strcpy(s_ale_barra_visto, s_ale_barra);
            s_eng_visto = s_st.enganchado;
            ui_digi_draw_barra(&s_st);
            ui_digi_draw_chip(&s_st);
        }
        return;
    }
    if (hfdl_modo_activo()) {
        /* the table when a frame comes or a row changes; the bar and the
         * chip four times a second, when their text moves */
        if (s_todo || (hfdl_modo_lineas() ^ (hfdl_modo_buenas() << 8)) != s_hf_visto) {
            digi_repinta();
            return;
        }
        if ((board_millis() - s_hf_t) < 250U) { return; }
        hfdl_sync();
        if (s_st.barra_v != s_hf_barra_v
            || strcmp(s_hf_barra_visto, s_st.barra_txt ? s_st.barra_txt : "") != 0) {
            s_hf_barra_v = s_st.barra_v;
            strcpy(s_hf_barra_visto, s_st.barra_txt ? s_st.barra_txt : "");
            ui_digi_draw_barra(&s_st);
        }
        if (strcmp(s_hf_chip_visto, s_st.chip ? s_st.chip : "") != 0) {
            strcpy(s_hf_chip_visto, s_st.chip ? s_st.chip : "");
            ui_digi_draw_chip(&s_st);
        }
        return;
    }
    if (wspr_modo_activo()) {
        /* the list when a beacon comes (once in two minutes at most), the
         * bar and the chip when their text or the bar's fill move */
        if (s_todo || (wspr_modo_lineas() | ((uint32_t)wspr_modo_buenas() << 8)) != s_ws_visto) {
            digi_repinta();
            return;
        }
        wspr_sync();
        if (s_st.barra_v != s_ws_barra_v
            || strcmp(s_ws_barra_visto, s_st.barra_txt ? s_st.barra_txt : "") != 0) {
            s_ws_barra_v = s_st.barra_v;
            strcpy(s_ws_barra_visto, s_st.barra_txt ? s_st.barra_txt : "");
            ui_digi_draw_barra(&s_st);
        }
        if (strcmp(s_ws_chip_visto, s_st.chip ? s_st.chip : "") != 0) {
            strcpy(s_ws_chip_visto, s_st.chip ? s_st.chip : "");
            ui_digi_draw_cabecera(&s_st);   /* the chip and the button beside it */
        }
        return;
    }
    if (ft8_modo_activo()) {
        /* upstream's: the list when a message comes (once a slot at most)
         * or the view moves, the bar when the second or its text does */
        if (s_todo || ft8_modo_metidas() != s_ft8_visto
            || (uint8_t)(s_ft8_cq | (uint8_t)(s_ft8_subidos << 1)) != s_ft8_vista_ant) {
            digi_repinta();
            return;
        }
        ft8_sync();
        if (s_st.barra_v != s_ft8_seg_visto
            || strcmp(s_ft8_barra_visto, s_st.barra_txt ? s_st.barra_txt : "") != 0) {
            s_ft8_seg_visto = s_st.barra_v;
            strcpy(s_ft8_barra_visto, s_st.barra_txt ? s_st.barra_txt : "");
            ui_digi_draw_barra(&s_st);
        }
        return;
    }
    if (jtty_modo_activo()) {
        /* upstream's: the whole panel when the count of atoms moves */
        if (s_todo || jtty_modo_atomos() != s_jtty_visto) { digi_repinta(); return; }
        jtty_sync();
        if (strcmp(s_chip_visto, s_chip) != 0 || s_eng_visto != s_st.enganchado) {
            strcpy(s_chip_visto, s_chip);
            s_eng_visto = s_st.enganchado;
            ui_digi_draw_chip(&s_st);
        }
        return;
    }
    if (hell_activo()) {
        if (s_todo) { digi_repinta(); return; }
        hell_cabecera();
        if (strcmp(s_chip_visto, s_chip) != 0 || s_eng_visto != s_st.enganchado) {
            strcpy(s_chip_visto, s_chip);
            s_eng_visto = s_st.enganchado;
            ui_digi_draw_cabecera(&s_st);
        }
        return;
    }
    if (sstv_activo()) {
        if (s_todo) { digi_repinta(); return; }
        sstv_cabecera();
        if (strcmp(s_chip_visto, s_chip) != 0 || s_eng_visto != s_st.enganchado) {
            strcpy(s_chip_visto, s_chip);
            s_eng_visto = s_st.enganchado;
            ui_digi_draw_cabecera(&s_st);
        }
        return;
    }
    if (wefax_activo()) {
        /* the rows come from digi_fax_poll(); here the header, when its
         * text or the lock dot change - upstream repaints it every 500 ms */
        if (s_todo) { digi_repinta(); return; }
        fax_cabecera();
        if (strcmp(s_chip_visto, s_chip) != 0 || s_eng_visto != s_st.enganchado) {
            strcpy(s_chip_visto, s_chip);
            s_eng_visto = s_st.enganchado;
            ui_digi_draw_cabecera(&s_st);
        }
        return;
    }
    osciloscopio();
    sincroniza();
    if (s_todo) {
        ui_digi_olvida();
        if (ui_digi_draw_texto(&s_st)) { s_todo = 0U; s_cambio = 0U; }
        strcpy(s_chip_visto, s_chip);
        s_eng_visto = s_st.enganchado;
        return;
    }
    if (s_cambio) {
        if (ui_digi_draw_cambios(&s_st)) { s_cambio = 0U; }
    }
    if (strcmp(s_chip_visto, s_chip) != 0 || s_eng_visto != s_st.enganchado) {
        strcpy(s_chip_visto, s_chip);
        s_eng_visto = s_st.enganchado;
        ui_digi_draw_chip(&s_st);
    }
}

uint8_t digi_toque(uint16_t x, uint16_t y, uint8_t nuevo)
{
    if (!s_on) { return 0U; }
    if (y < DIGI_SCOPE_Y || y >= DIGI_PANEL_Y + DIGI_PANEL_H) { return 0U; }

    /* THE MAP (step 64): its button, and with it shown, the finger on it -
     * the press AND the moves, which the line below drops for everyone
     * else (a drag moves the map). */
    if (digi_mapa_modo()) {
        if (nuevo && ui_digi_boton5_hit(&s_st, x, y)) {
            s_mapa = (uint8_t)!s_mapa;
            s_todo = 1U;
            console_printf("  digi: %s\n", s_mapa ? "map" : "list");
            return 1U;
        }
        if (s_mapa && digi_mapa_toque(x, y, nuevo)) { return 1U; }
    }
    if (!nuevo) { return 1U; }

    /*
     * PSK31: A TAP ON THE SCOPE TUNES - this port's, upstream tunes it with
     * the dial alone. PSK31 holds +-7.8 Hz and is 31 Hz wide in a 6 kHz
     * scope, so the finger cannot be exact: the centre goes to the
     * strongest bin within +-60 Hz of the tap in the last frame, the trace
     * the finger was aiming at. psk31_set_centro_hz() also drops the
     * decoder's own tracking, as a manual move should.
     */
    if (psk31_activo() && y < DIGI_SCOPE_Y + DIGI_SCOPE_H) {
        const float *f = rtty_scope_frame_peek();
        float hz = rtty_scope_hz_per_bin();
        float tap = (float)x / (float)GFX2_W * hz * (float)RTTY_SCOPE_BINS;

        if (f != 0 && hz > 0.0f) {
            int32_t k0 = (int32_t)(tap / hz), w = (int32_t)(60.0f / hz), k, best = k0;

            for (k = k0 - w; k <= k0 + w; k++) {
                if (k < 1 || k >= (int32_t)RTTY_SCOPE_BINS - 1) { continue; }
                if (best < 1 || best >= (int32_t)RTTY_SCOPE_BINS - 1 || f[k] > f[best]) { best = k; }
            }
            if (best >= 2 && best < (int32_t)RTTY_SCOPE_BINS - 2) {
                /*
                 * NOT THE PEAK: the CENTRE OF THE POWER round it, two bins
                 * a side. Idling, PSK31 is two lines at +-15.6 Hz with no
                 * carrier between - the strongest bin is a sideband, 15 Hz
                 * off, twice the decoder's +-7.8 Hz - and the centroid of
                 * the pair is the carrier. Sending text, the power sits on
                 * the carrier and the centroid is the peak.
                 */
                float num = 0.0f, den = 0.0f;
                int32_t j;

                for (j = best - 2; j <= best + 2; j++) {
                    num += (float)j * f[j];
                    den += f[j];
                }
                if (den > 0.0f) { tap = (num / den) * hz; }
            }
        }
        demod_quieto(1);
        psk31_set_centro_hz(tap);
        demod_quieto(0);
        console_printf("  digi: PSK31 centre %u Hz (tap at x %u)\n",
                       (uint32_t)(psk31_get_centro_hz() + 0.5f), (uint32_t)x);
        return 1U;
    }
    /*
     * NAVTEX: A TAP ON THE SCOPE SETS THE CENTRE, and it holds until the
     * mode is entered again (navtex_set_centro_hz() pins it, 22/11/2026).
     * Where: the pair of tones navtex.c's own spectral search finds within
     * +-150 Hz of the finger (navtex_centro_cerca()), or the finger itself
     * if it finds none - the same search that moves the centre by itself,
     * so a tap only matters where that one cannot decide.
     */
    if (hfdl_modo_activo()) {
        hfdl_sync();
        if (!nuevo) { return 1U; }
        if (ui_digi_boton_hit(&s_st, x, y)) {
            demod_quieto(1);
            hfdl_modo_borra();          /* upstream's: the table and the counts */
            demod_quieto(0);
            digi_hfdl_borra();
            s_todo = 1U;
            console_printf("  digi: HFDL cleared\n");
        } else if (ui_digi_boton4_hit(&s_st, x, y)) {
            /* upstream's hfdl_frec_siguiente(): the next channel up from
             * the dial, round to the lowest */
            uint32_t khz = radio_dial_hz() / 1000U;
            int16_t  c, sig = 0;

            for (c = 0; c < HFDL_CANALES; c++) {
                if (k_hfdl_canales[c].khz > khz) { sig = c; break; }
            }
            (void)radio_dial_go(k_hfdl_canales[sig].khz * 1000U);
            s_todo = 1U;
            console_printf("  digi: HFDL %u kHz, %s\n", k_hfdl_canales[sig].khz,
                           k_hfdl_canales[sig].estacion);
        }
        return 1U;
    }
    if (wspr_modo_activo()) {
        wspr_sync();
        if (!nuevo) { return 1U; }
        if (ui_digi_boton_hit(&s_st, x, y)) {
            /* upstream's: the list and the count; an open window goes on */
            wspr_modo_borra();
            digi_wspr_borra();
            s_todo = 1U;
            console_printf("  digi: WSPR cleared\n");
        } else if (ui_digi_boton4_hit(&s_st, x, y)) {
            /* upstream's wspr_frec_siguiente(): the next of the ten, or the
             * first from anywhere else; during a window only noted, the
             * window ends where it began */
            int16_t ahora = (s_ws_pide >= 0) ? s_ws_pide : wspr_canal_actual();
            int16_t sig = (ahora >= 0) ? (int16_t)((ahora + 1) % WSPR_CANALES) : 0;

            if (wspr_modo_capturando()) {
                s_ws_pide = sig;
                console_printf("  digi: WSPR %s after this window\n", k_wspr_canales[sig].banda);
            } else {
                s_ws_pide = -1;
                (void)radio_dial_go(k_wspr_canales[sig].hz);
                console_printf("  digi: WSPR dial %u Hz\n", k_wspr_canales[sig].hz);
            }
            s_todo = 1U;
        }
        return 1U;
    }
    if (ft8_modo_activo()) {
        ft8_sync();
        if (y >= (uint16_t)(DIGI_SCOPE_Y + UDG_HDR_H)) {
            /* the rows: a finger dragged scrolls them, upstream's
             * ft8_lista_arrastra() - down brings the older ones */
            if (nuevo) {
                s_ft8_acum = 0;
            } else {
                (void)ft8_lista_arrastra(&s_ft8_acum, (int16_t)((int16_t)y - s_ft8_y),
                                         (uint8_t)UDG_LINE_H, &s_ft8_subidos);
            }
            s_ft8_y = (int16_t)y;
            return 1U;
        }
        if (!nuevo) { return 1U; }
        if (ui_digi_boton_hit(&s_st, x, y)) {
            ft8_modo_borra();           /* upstream's: the list, not the capture */
            digi_ft8_borra();
            s_todo = 1U;
            console_printf("  digi: FT8 cleared\n");
        } else if (ui_digi_boton3_hit(&s_st, x, y)) {
            s_ft8_cq = (uint8_t)!s_ft8_cq;
            s_ft8_subidos = 0U;         /* other rows: shown from the newest */
            console_printf("  digi: FT8 %s\n", s_ft8_cq ? "CQ only" : "all messages");
        } else if (ui_digi_boton4_hit(&s_st, x, y)) {
            /* upstream's ft8_frec_siguiente(): the next of the nine, or the
             * first from anywhere else */
            uint32_t khz = radio_dial_hz() / 1000U;
            uint8_t  sig = 0U, c;

            for (c = 0U; c < FT8_CANALES; c++) {
                if (k_ft8_canales[c].khz == khz) { sig = (uint8_t)((c + 1U) % FT8_CANALES); break; }
            }
            (void)radio_dial_go(k_ft8_canales[sig].khz * 1000U);
            s_todo = 1U;
            console_printf("  digi: FT8 dial %u kHz\n", k_ft8_canales[sig].khz);
        } else if (ui_digi_barra_hit(&s_st, x, y)) {
            /* upstream's: the slot's zero is NOW, the clock untouched */
            ft8_modo_cuadra();
            s_ft8_seg_visto = 0xFFU;
            console_printf("  digi: FT8 slot squared by hand\n");
        }
        return 1U;
    }
    if (ais_modo_activo()) {
        ais_sync();
        if (ui_digi_boton_hit(&s_st, x, y)) {
            demod_quieto(1);
            ais_modo_borra();           /* upstream's: the ships and the counts */
            demod_quieto(0);
            digi_ais_borra();
            s_todo = 1U;
            console_printf("  digi: AIS cleared\n");
        } else if (ui_digi_boton4_hit(&s_st, x, y)) {
            /* upstream's ais_frec_siguiente(): the other channel, or A from
             * anywhere else; the dial goes there and the title says which */
            uint32_t khz = radio_dial_hz() / 1000U;
            uint8_t  sig = 0U, c;

            for (c = 0U; c < 2U; c++) {
                if (k_ais_canales[c].khz == khz) { sig = (uint8_t)((c + 1U) % 2U); }
            }
            (void)radio_dial_go(k_ais_canales[sig].khz * 1000U);
            ais_modo_canal_pon(sig);
            s_todo = 1U;
            console_printf("  digi: AIS channel %c, %u kHz\n", (char)('A' + sig),
                           k_ais_canales[sig].khz);
        }
        return 1U;
    }
    if (ident_modo_activo()) {
        ident_sync();
        if (ui_digi_boton_hit(&s_st, x, y)) {
            demod_quieto(1);
            ident_modo_borra();         /* upstream's: when the dial moved */
            demod_quieto(0);
            memset(s_id_hash, 0, sizeof s_id_hash);
            s_todo = 1U;
            console_printf("  digi: IDENT cleared\n");
        }
        return 1U;
    }
    if (stanag_modo_activo()) {
        /* upstream's five: Borrar; the waveform round (which may change the
         * job and the whole layout); and, by job, speed, interleave and
         * format - each turned as upstream does */
        uint8_t hit = 1U;

        stanag_sync();
        demod_quieto(1);
        if (ui_digi_boton_hit(&s_st, x, y)) {
            stanag_modo_borra();
            memset(s_sg_hash, 0, sizeof s_sg_hash);
            console_printf("  digi: STANAG cleared\n");
        } else if (ui_digi_boton2_hit(&s_st, x, y)) {
            stanag_modo_forma_pon((uint8_t)((stanag_modo_forma() + 1U) % STANAG_FORMAS));
            console_printf("  digi: STANAG waveform %s%s\n",
                           stanag_modo_forma_txt(stanag_modo_forma()),
                           stanag_modo_activo() ? "" : " - REFUSED, the mode is off");
        } else if (stanag_modo_demod() && ui_digi_boton3_hit(&s_st, x, y)) {
            /* upstream's round, but past what does not fit: 2400 bps with
             * the long interleave asks more than the 32 kB, and upstream's
             * stanag_modo_vel_pon() then keeps the speed it had - so the
             * round stuck at 1200 for good. Here the next one that takes. */
            uint8_t n = stanag_modo_vel_n(), v0 = stanag_modo_vel(), k;

            for (k = 1U; k < n; k++) {
                stanag_modo_vel_pon((uint8_t)((v0 + k) % n));
                if (stanag_modo_vel() != v0) { break; }
            }
            console_printf("  digi: STANAG speed %s\n", stanag_modo_vel_txt());
        } else if (stanag_modo_demod() && ui_digi_boton4_hit(&s_st, x, y)) {
            stanag_modo_entre_pon((uint8_t)(stanag_modo_entre() ? 0U : 1U));
            console_printf("  digi: STANAG interleave %s\n", stanag_modo_entre_txt());
        } else if ((stanag_modo_demod() || stanag_modo_4415()) && ui_digi_boton5_hit(&s_st, x, y)) {
            stanag_modo_fmt_pon((uint8_t)((stanag_modo_fmt() + 1U) % stanag_modo_fmt_n()));
            console_printf("  digi: STANAG format %s\n", stanag_modo_fmt_txt());
        } else {
            hit = 0U;
        }
        demod_quieto(0);
        if (hit) { s_todo = 1U; }
        return 1U;
    }
    if (ale_modo_activo()) {
        ale_sync();
        if (ui_digi_boton_hit(&s_st, x, y)) {
            demod_quieto(1);
            ale_modo_borra();           /* upstream's Borrar: the table, the counts */
            demod_quieto(0);
            digi_ale_borra();
            s_todo = 1U;
            console_printf("  digi: ALE cleared\n");
        }
        return 1U;
    }
    if (jtty_modo_activo()) {
        jtty_sync();
        if (ui_digi_boton_hit(&s_st, x, y)) {
            jtty_modo_borra();
            s_todo = 1U;
            console_printf("  digi: JTTY cleared\n");
        }
        return 1U;
    }
    if (hell_activo()) {
        hell_cabecera();
        if (ui_digi_boton_hit(&s_st, x, y)) {
            fax_limpia();
            hell_reinicia();
            console_printf("  digi: HELL cleared\n");
        }
        return 1U;
    }
    if (sstv_activo()) {
        sstv_cabecera();
        if (ui_digi_boton_hit(&s_st, x, y)) {
            sstv_aplica();
            fax_limpia();
            console_printf("  digi: SSTV start, %s\n", s_st.btn2);
        } else if (ui_digi_boton2_hit(&s_st, x, y)) {
            s_sst_idx = (uint8_t)((s_sst_idx + 1U) % (uint8_t)(sstv_modos_n() + 1U));
            sstv_aplica();              /* upstream's grid applies at once */
            fax_limpia();
            sstv_cabecera();
            console_printf("  digi: SSTV mode %s\n", s_st.btn2);
        } else {
            return 1U;
        }
        sstv_cabecera();
        ui_digi_draw_cabecera(&s_st);
        return 1U;
    }
    if (wefax_activo()) {
        fax_cabecera();
        if (ui_digi_boton_hit(&s_st, x, y)) {
            /* upstream's: the line starts HERE, and paint without waiting
             * for the start tone - for a chart caught half-way */
            demod_quieto(1);
            wefax_sincroniza();
            wefax_fuerza_imagen();
            demod_quieto(0);
            fax_limpia();
            console_printf("  digi: WEFAX synchronised by hand\n");
        } else if (ui_digi_boton2_hit(&s_st, x, y)) {
            demod_quieto(1);
            wefax_set_lpm_idx((uint8_t)((wefax_get_lpm_idx() + 1U) % WEFAX_LPM_N));
            demod_quieto(0);
            console_printf("  digi: WEFAX %u lpm\n", (uint32_t)wefax_get_lpm());
        } else {
            return 1U;
        }
        fax_cabecera();
        ui_digi_draw_cabecera(&s_st);
        return 1U;
    }
    if (navtex_activo() && y < DIGI_SCOPE_Y + DIGI_SCOPE_H) {
        float tap = (float)x / (float)GFX2_W * rtty_scope_hz_per_bin() * (float)RTTY_SCOPE_BINS;
        float c = navtex_centro_cerca(tap);

        demod_quieto(1);
        navtex_set_centro_hz(c);
        demod_quieto(0);
        console_printf("  digi: NAVTEX centre %u Hz, held (tap at x %u, %u Hz)\n",
                       (uint32_t)(navtex_get_centro_hz() + 0.5f), (uint32_t)x,
                       (uint32_t)(tap + 0.5f));
        return 1U;
    }
    if (ui_digi_boton_hit(&s_st, x, y)) {
        borra_texto();
        s_st.btn_press = 1U;
        ui_digi_draw_boton(&s_st);
        s_st.btn_press = 0U;
        s_todo = 1U;                    /* the rows, and the button back up */
        console_printf("  digi: text cleared\n");
        return 1U;
    }
    if (cw_get_enabled() && ui_digi_boton2_hit(&s_st, x, y)) {
        cw_set_aprende((uint8_t)!cw_get_aprende());   /* upstream's: drops what it learnt */
        console_printf("  digi: CW learning %s\n", cw_get_aprende() ? "on" : "off");
    } else if (rtty_get_enabled() && ui_digi_boton3_hit(&s_st, x, y)) {
        float   hoy = rtty_get_shift_hz(), nuevo_hz = k_shift[0];
        uint8_t k;

        for (k = 0U; k < 4U; k++) {
            if (k_shift[k] > hoy + 1.0f) { nuevo_hz = k_shift[k]; break; }
        }
        demod_quieto(1);
        rtty_set_shift_hz(nuevo_hz);
        demod_quieto(0);
        console_printf("  digi: RTTY shift %u Hz\n", (uint32_t)(nuevo_hz + 0.5f));
    } else if (rtty_get_enabled() && ui_digi_boton4_hit(&s_st, x, y)) {
        digi_rtty_baud_pon((uint8_t)((digi_rtty_baud_idx() + 1U) % 4U));
        console_printf("  digi: RTTY %s Bd\n", k_baud_txt[digi_rtty_baud_idx()]);
    } else if (rtty_get_enabled() && ui_digi_boton5_hit(&s_st, x, y)) {
        rtty_auto_set((uint8_t)!rtty_auto_get());
        console_printf("  digi: RTTY auto %s\n", rtty_auto_get() ? "on" : "off");
    } else {
        return 1U;
    }
    sincroniza();
    ui_digi_draw_cabecera(&s_st);       /* the buttons, with their new labels */
    return 1U;
}

/*
 * For the bench: swd-call ... digi_report. What RTTY is set to now -
 * rtty_auto.c moves the tones, the speed and the polarity by itself -
 * and the two strongest peaks of the scope's last frame, 23.4 Hz a bin,
 * which is where the tones really are.
 */
__attribute__((noinline, used)) void digi_report(void)
{
    const float *f = rtty_scope_frame_peek();
    float    hz = rtty_scope_hz_per_bin();
    uint32_t k, p1 = 0U, p2 = 0U;

    console_printf("  digi report: rtty %s, mark %u Hz, space %u Hz (shift %u), "
                   "%u.%02u Bd, station %s%s, auto %s, frames whole %u %%\n",
                   rtty_get_enabled() ? "on" : "off",
                   (uint32_t)(rtty_get_mark_hz() + 0.5f),
                   (uint32_t)(rtty_get_space_hz() + 0.5f),
                   (uint32_t)(rtty_get_shift_hz() + 0.5f),
                   (uint32_t)rtty_get_baud(),
                   (uint32_t)(rtty_get_baud() * 100.0f + 0.5f) % 100U,
                   rtty_get_station_inverted() ? "INVERTED" : "normal",
                   rtty_get_vuelta_auto() ? " (flipped by the detector)" : "",
                   rtty_auto_get() ? "on" : "off", (uint32_t)rtty_salud_pc());
    if (psk31_activo()) {
        uint8_t cal = 0U;
        int16_t e = 0;

        psk31_diag(&cal, &e);
        console_printf("               psk31: centre %u Hz, error %c%u.%02u Hz, quality %u\n",
                       (uint32_t)(psk31_get_centro_hz() + 0.5f), e < 0 ? '-' : '+',
                       (uint32_t)(e < 0 ? -e : e) / 100U, (uint32_t)(e < 0 ? -e : e) % 100U,
                       (uint32_t)cal);
    }
    if (navtex_activo()) {
        navtex_info_t in;
        float ppm = 0.0f, niv = 0.0f;
        uint8_t desp = 0U;

        float par = 0.0f, nota = 0.0f;

        navtex_info(&in);
        navtex_lazos_dbg(&ppm, &niv, &desp);
        navtex_espectro_dbg(&par, &nota);
        console_printf("               navtex: centre set %u Hz, measured %+d, %s; bit sync %u, "
                       "char sync %u (shift %u), quality %u %%, clock %d ppm\n",
                       (uint32_t)(navtex_get_centro_hz() + 0.5f), (int)in.desvio_hz,
                       navtex_get_invertido() ? "reversed" : "normal",
                       (uint32_t)in.bit_sync, (uint32_t)in.car_sync, (uint32_t)desp,
                       (uint32_t)(100.0f * in.calidad + 0.5f), (int)ppm);
        console_printf("               navtex: good %u bad %u saved %u, messages %u, header %s\n",
                       (uint32_t)in.car_ok, (uint32_t)in.car_mal, (uint32_t)in.rescatados,
                       (uint32_t)in.mensajes, in.tiene_cab ? in.cab : "-");
        console_printf("               navtex: spectrum's pair %u Hz, score %u.%02u (2.00 to move); "
                       "moved to another bit offset %u times\n",
                       (uint32_t)(par + 0.5f), (uint32_t)nota,
                       (uint32_t)(nota * 100.0f + 0.5f) % 100U, (uint32_t)navtex_recolocados());
    }
    if (sstv_activo()) {
        sstv_info_t in;

        sstv_info(&in);
        console_printf("               sstv: state %u, mode %s (%s), line %u of %u, "
                       "pictures %u, bad syncs %u, offset %d Hz, choice %u\n",
                       (uint32_t)in.estado, in.modo ? in.modo : "-",
                       in.como == SSTV_COMO_VIS ? "VIS" : in.como == SSTV_COMO_RITMO
                       ? "rhythm" : in.como == SSTV_COMO_MANO ? "hand" : "-",
                       (uint32_t)in.linea, (uint32_t)in.alto, (uint32_t)in.imagenes,
                       (uint32_t)in.sincs_malos, (int)sstv_desvio_hz(), (uint32_t)s_sst_idx);
    }
    if (wefax_activo()) {
        wefax_info_t in;
        float desliz = 0.0f, pulso = 0.0f;

        wefax_info(&in);
        wefax_fase_dbg(&desliz, &pulso);
        console_printf("               wefax: state %u, %u lpm, slant %d ppm, %u lines, "
                       "phasing %u lines, level %d, row %u\n",
                       (uint32_t)in.estado, (uint32_t)in.lpm, (int)in.ppm,
                       (uint32_t)in.lineas, (uint32_t)in.fase_n, (int)in.nivel,
                       (uint32_t)s_fax_fila);
    }
    if (dsc_activo()) {
        dsc_info_t in;

        dsc_info(&in);
        console_printf("               dsc: centre %u Hz (the search last found %u), %s, "
                       "calls %u good %u with a bad check\n",
                       (uint32_t)(dsc_get_centro_hz() + 0.5f), (uint32_t)(dsc_busca_hz() + 0.5f),
                       in.en_mensaje ? "receiving" : in.enganchado ? "locked" : "searching",
                       (uint32_t)in.llamadas, (uint32_t)in.malas);
    }
    if (f == 0) { return; }
    for (k = 4U; k < RTTY_SCOPE_BINS; k++) { if (f[k] > f[p1]) { p1 = k; } }
    for (k = 4U; k < RTTY_SCOPE_BINS; k++) {
        if ((k + 3U < p1 || k > p1 + 3U) && f[k] > f[p2]) { p2 = k; }
    }
    console_printf("               scope peaks %u Hz and %u Hz\n",
                   (uint32_t)((float)p1 * hz + 0.5f), (uint32_t)((float)p2 * hz + 0.5f));
}
