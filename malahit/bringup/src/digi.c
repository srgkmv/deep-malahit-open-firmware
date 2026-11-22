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
#include "demod.h"         /* demod_quieto(): a decoder reconfigured, no block through it */
#include "idioma.h"        /* ../../User - tr() */

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

void digi_pon(uint8_t on)
{
    s_on = (uint8_t)(on != 0U);
    borra_texto();
    s_chip_visto[0] = '\0';
    s_eng_visto = 0xFFU;
    s_todo = 1U;
    if (s_on) { rtty_scope_avg_reset(); }
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

void digi_repinta(void)
{
    if (!s_on) { return; }
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
