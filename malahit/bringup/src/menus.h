#ifndef MALAHIT_MENUS_H
#define MALAHIT_MENUS_H

#include <stdint.h>

/*
 * THE MENU SCREENS - step 9, 21/11/2026.
 *
 * Upstream's two menu engines, compiled in place at this panel's size
 * (compat/malahit_layout.h): ui_cfg.c - a column of categories and a grid
 * of cells - for the mode and band screens, and ui_grid.c - a paged grid
 * with a title - for the step screen. The same three screens upstream
 * opens from the same three buttons of its action bar, over the same
 * area: the spectrum and the waterfall, between the status band and the
 * buttons. deepsdr-ui `make malahit-menus` renders them on a PC.
 *
 * What is NOT upstream's is this file: upstream's controller for these
 * screens is in its 39 000-line main.c. This is the part of it a radio
 * with five modes, one band table and no disk needs. The band table IS
 * upstream's - generated from its main.c at build time by the same
 * script the host render uses (reglas.mk), so the two cannot disagree.
 *
 * The radio side - what a mode, a band or a step DOES - is in main.c,
 * behind the radio_*() calls below, because that is where the dial, the
 * header and the step already live.
 */

typedef enum {
    MENU_NONE = 0,
    MENU_MODO,
    MENU_BANDAS,
    MENU_PASO
} menu_t;

/* Which screen is up; MENU_NONE when the spectrum is showing. The main
 * loop draws the spectrum and the waterfall only then. */
menu_t menu_open_id(void);

/* The action button for a screen: opens it, or closes it if it is the
 * one already up - upstream's accion_pantalla(). */
void menu_toggle(menu_t m);
void menu_close(void);

/* A finger on the glass while a screen is up. `down` on every poll with
 * a contact, `up` once when it lifts. Returns 1 if the point was the
 * menu's (inside its area), so the caller does not also act on it. */
int  menu_touch_down(uint16_t x, uint16_t y, int nuevo);
void menu_touch_up(void);

/* The big knob while a screen is up: turning moves the cursor, pressing
 * chooses. The main loop sends it here instead of to the dial. */
void menu_knob(int32_t detents);
void menu_knob_press(void);

/* ---- provided by main.c ------------------------------------------ */

/* Upstream's demod_mode_t as an int. Sets the demodulator, the header's
 * mode chip and the action bar's value. */
void        radio_mode_set(int mode);
int         radio_mode_get(void);
const char *radio_mode_name(int mode);

/* Tune to a band: the dial, then the mode (-1 = leave it). Returns 0 if
 * the dial could not go there. `name` goes in the header's band pill. */
int         radio_band_go(uint32_t hz, int mode, const char *name);
uint32_t    radio_dial_hz(void);

/* The tuning step, by index into main.c's table. */
uint32_t    radio_step_count(void);
uint32_t    radio_step_hz(uint32_t i);
uint32_t    radio_step_get(void);
void        radio_step_set(uint32_t i);

/* After a screen closes: the spectrum area belongs to the radio again,
 * and has to be painted whole. */
void        radio_screen_restore(void);

/* The action bar's state changed (which screen is open). */
void        radio_act_refresh(void);

#endif /* MALAHIT_MENUS_H */
