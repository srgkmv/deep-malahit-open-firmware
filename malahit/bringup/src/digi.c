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
 * What this port adds is only the cut: one decoder so far, so no
 * dispatch on which one is running. The next one (RTTY) brings its own
 * chip and its lines into the same three places.
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
#include "cw.h"            /* ../../User - the decoder */
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
static char            s_chip[24];
static char            s_chip_visto[24];
static uint8_t         s_eng_visto = 0xFFU;
static float           s_db[RTTY_SCOPE_BINS];

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
    if (nyq > 0.0f) {
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
    gfx_fill_rect(0, DIGI_SCOPE_Y, GFX2_W, DIGI_SCOPE_H + DIGI_SCOPE_GAP,
                  color_tema(PAL_SURF_0));
    spectrum_invalida();
    s_todo = 1U;
    digi_dibuja();
}

void digi_dibuja(void)
{
    if (!s_on) { return; }
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
    if (nuevo && ui_digi_boton_hit(&s_st, x, y)) {
        borra_texto();
        s_st.btn_press = 1U;
        ui_digi_draw_boton(&s_st);
        s_st.btn_press = 0U;
        s_todo = 1U;                    /* the rows, and the button back up */
        console_printf("  digi: text cleared\n");
    }
    return 1U;
}
