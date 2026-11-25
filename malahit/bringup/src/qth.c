/*
 * THE QTH SCREEN - step 57, 25/11/2026. See qth.h.
 *
 * Upstream's controller, out of its main.c (qth_alfabeto(), qth_coord_txt(),
 * qth_fill(), qth_show(), qth_mando(), qth_touch()), on this port's menus:
 * menus.c opens it from Settings > Device and routes the finger and the knob
 * here, and menu_close() takes it down. Kept as upstream wrote it:
 *
 *   - each box offers only its alphabet (A-R, 0-9, a-x), so nothing invalid
 *     can be typed and nothing has to be refused;
 *   - a COPY is edited; the radio's locator changes only on Apply, so
 *     leaving half-way leaves it where it was;
 *   - with no locator yet the boxes start on "AA00aa";
 *   - Apply is dead until the copy differs from what is in force;
 *   - the coordinates line is ft8_decoder.c's, the one conversion.
 *
 * What this port adds: the knob. Upstream leaves it out of this screen;
 * here turning it moves the chosen box through its alphabet, as "-" and
 * "+" do, and its press goes to the next box.
 *
 * Upstream starts a radio with no locator on IN80dk (its owner's, in
 * Spain); this port starts with none - the km column stays empty until the
 * owner sets it here, which is what upstream's "not set" cell is for.
 */
#include <string.h>
#include "qth.h"
#include "ui_qth.h"        /* ../../User - the screen */
#include "ft8_decoder.h"   /* ../../User - the locator in force, its coordinates */
#include "idioma.h"        /* ../../User - tr() */
#include "menus.h"         /* menu_close() */
#include "console.h"

static ui_qth_state_t s_qth;
static char           s_edit[UIQ_N + 1];
static char           s_coord[28];

/* upstream's qth_alfabeto(): one table for the hint and the cycling */
static const char *alfabeto(uint8_t i, uint8_t *n)
{
    if (i < 2U) { *n = 18U; return "ABCDEFGHIJKLMNOPQR"; }
    if (i < 4U) { *n = 10U; return "0123456789"; }
    *n = 24U;   return "abcdefghijklmnopqrstuvwx";
}

static void rellena(void)
{
    uint8_t i, n;
    const char *ya = ft8_decoder_get_own_grid();

    for (i = 0U; i < UIQ_N; i++) {
        s_qth.loc[i] = s_edit[i];
        (void)alfabeto(i, &n);
        s_qth.pista[i] = (i < 2U) ? "A-R" : ((i < 4U) ? "0-9" : "a-x");
    }
    s_qth.loc[UIQ_N] = '\0';
    if (!ft8_decoder_grid_coord_txt(s_edit, (int)UIQ_N, s_coord, sizeof s_coord)) {
        s_coord[0] = '\0';
    }
    s_qth.coord  = s_coord;
    s_qth.titulo = "QTH";
    s_qth.pie    = tr("tu localizador: de aquí salen el aspa de los mapas y las distancias",
                      "your locator: the map cross and the distances come from this");
    s_qth.rot[UIQ_HIT_MENOS]   = "-";
    s_qth.rot[UIQ_HIT_MAS]     = "+";
    s_qth.rot[UIQ_HIT_APLICAR] = tr("Aplicar", "Apply");
    s_qth.rot[UIQ_HIT_SALIR]   = tr("Salir", "Exit");
    s_qth.cambiado = 0U;
    for (i = 0U; i < UIQ_N; i++) {
        if (ya[i] != s_edit[i]) { s_qth.cambiado = 1U; break; }
    }
}

void qth_abre(void)
{
    const char *ya = ft8_decoder_get_own_grid();
    uint8_t i, fin = 0U;

    for (i = 0U; i < UIQ_N; i++) {
        char c = fin ? '\0' : ya[i];

        if (c == '\0') { fin = 1U; c = (i < 2U) ? 'A' : ((i < 4U) ? '0' : 'a'); }
        s_edit[i] = c;
    }
    s_edit[UIQ_N] = '\0';
    s_qth.sel = 0U;
    s_qth.pressed = UIQ_HIT_NONE;
    rellena();
    ui_qth_draw(&s_qth);
}

void qth_dibuja(void)
{
    rellena();
    ui_qth_draw(&s_qth);
}

/* upstream's qth_mando(): the chosen box through its alphabet */
static void mueve(int32_t pasos)
{
    uint8_t n, i;
    const char *alf = alfabeto(s_qth.sel, &n);
    int32_t k;

    for (i = 0U; i < n; i++) { if (alf[i] == s_edit[s_qth.sel]) { break; } }
    if (i >= n) { i = 0U; }
    k = ((int32_t)i + pasos) % (int32_t)n;
    if (k < 0) { k += (int32_t)n; }
    s_edit[s_qth.sel] = alf[k];
    rellena();
    ui_qth_draw(&s_qth);
}

void qth_knob(int32_t d)
{
    if (d != 0) { mueve(d); }
}

void qth_knob_press(void)
{
    s_qth.sel = (uint8_t)((s_qth.sel + 1U) % UIQ_N);
    rellena();
    ui_qth_draw(&s_qth);
}

/* upstream's qth_touch(): decided on the press, done on the lift */
void qth_toque(uint16_t x, uint16_t y, uint8_t pulsado)
{
    int8_t k;

    if (pulsado) {
        if (s_qth.pressed < 0) {
            int8_t h = ui_qth_hit(x, y);

            if (h == UIQ_HIT_APLICAR && !s_qth.cambiado) { h = UIQ_HIT_NONE; }
            if (h != UIQ_HIT_NONE) { s_qth.pressed = h; ui_qth_draw(&s_qth); }
        }
        return;
    }
    if (s_qth.pressed < 0) { return; }
    k = s_qth.pressed;
    s_qth.pressed = UIQ_HIT_NONE;
    if (k >= UIQ_HIT_CAJA) {
        s_qth.sel = (uint8_t)(k - UIQ_HIT_CAJA);
        rellena();
        ui_qth_draw(&s_qth);
    } else if (k == UIQ_HIT_MENOS) {
        mueve(-1);
    } else if (k == UIQ_HIT_MAS) {
        mueve(1);
    } else if (k == UIQ_HIT_APLICAR) {
        /* the settings record follows by itself (main.c settings_tick()) */
        ft8_decoder_set_own_grid(s_edit, (int)UIQ_N);
        console_printf("  qth: %s (%s)\n", ft8_decoder_get_own_grid(), s_coord);
        menu_close();
    } else {
        menu_close();
    }
}
