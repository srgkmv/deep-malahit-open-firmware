#ifndef MALAHIT_MENUS_H
#define MALAHIT_MENUS_H

#include <stdint.h>
#include "settings.h"      /* src/ - settings_t */

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
    MENU_PASO,
    MENU_AJUSTES,
    MENU_TECLADO,       /* the frequency keypad: no button of its own */
    MENU_MEM,           /* the memory channels: the MEM chip in the header */
    MENU_HORA,          /* the radio clock, Settings > Digital (src/hora.c, step 56) */
    MENU_QTH            /* the locator, Settings > Device (src/qth.c, step 57) */
} menu_t;

/* Which screen is up; MENU_NONE when the spectrum is showing. The main
 * loop draws the spectrum and the waterfall only then. */
menu_t menu_open_id(void);

/* The action button for a screen: opens it, or closes it if it is the
 * one already up - upstream's accion_pantalla(). */
void menu_toggle(menu_t m);
void menu_close(void);

/* A tap on the frequency in the header: upstream's keypad (ui_kbd.c),
 * the entry empty. Over another screen it takes its place. */
void menu_teclado_open(void);
void menu_hora_open(void);      /* a tap on the clock: upstream's KBD_HORA (step 56) */
void menu_hora_tic(void);       /* once a second: the keypad's ticking second */

/* The memory channels (upstream's banco.c, MEM.CSV on the disk - in the
 * FRAM with no disk; src/memfram.c): load them at boot, and again when
 * the PC gives the disk back (it may have edited the file). The screen is
 * menu_toggle(MENU_MEM). */
void menu_mem_init(void);

/* src/memfram.c: where MEM.CSV is this boot, and why the last write was
 * refused ("" when it was not). */
#define MEMF_DONDE_NINGUNO 0U
#define MEMF_DONDE_DISCO   1U
#define MEMF_DONDE_FRAM    2U
uint8_t     memfram_donde(void);
const char *memfram_donde_txt(void);
const char *memfram_porque(void);

/* A finger on the glass while a screen is up. `down` on every poll with
 * a contact, `up` once when it lifts. Returns 1 if the point was the
 * menu's (inside its area), so the caller does not also act on it. */
int  menu_touch_down(uint16_t x, uint16_t y, int nuevo);
void menu_touch_up(void);

/* The big knob while a screen is up: turning moves the cursor, pressing
 * chooses. The main loop sends it here instead of to the dial. */
void menu_knob(int32_t detents);
void menu_knob_press(void);

/* The volume or the RF gain changed from the small knob: if the settings
 * screen's detail page for it is up, its number follows. */
void menu_refresh_det(void);

/* The band the dial is in, by upstream's rule (the narrowest whose
 * edges hold it), as an index into the generated table, or -1; and its
 * name. For the header's band pill. */
int         menu_band_at(uint32_t hz);
const char *menu_band_name(int k);

/* The settings screen in the FRAM record (settings.h, the extension):
 * read what is in force, and apply a loaded record in two halves - the
 * display before the first paint, the demodulator after its init. */
void        menu_ajustes_get(settings_t *s);
void        menu_ajustes_apply_pantalla(const settings_t *s);
void        menu_ajustes_apply_radio(const settings_t *s);
const char *menu_tema_nombre(void);
uint32_t    menu_nr_strength(void);

/* ---- provided by main.c ------------------------------------------ */

/*
 * What the dial can reach: 100 kHz to upstream's own top, 180 MHz - the
 * MSi001's AM path below 60 MHz, its VHF path to 120 behind the VHF1
 * filter, and B3 above behind VHF2 (msi001.c). One place, because the
 * band screen leaves out what the dial cannot tune.
 */
#define RADIO_TUNE_MIN_HZ   100000UL
#define RADIO_TUNE_MAX_HZ   180000000UL

/* Upstream's demod_mode_t as an int. Sets the demodulator, the header's
 * mode chip and the action bar's value.
 *
 * AND ABOVE IT, THE DECODERS - 22/11/2026, step 42. Upstream's mode
 * screen is a table of ENTRIES, not of demodulators: "CW" is USB with
 * the Morse decoder switched on (k_demod_modes[] in its main.c). This
 * port numbers those entries on from upstream's last demod_mode_t, so
 * one int still says everything about the mode - the FRAM byte and the
 * memory channel's `modo` carry it as they are. A firmware that does not
 * know a number refuses it and boots on AM (main.c), which the owner's
 * rule for the FRAM allows. */
#define RADIO_MODE_CW       6           /* DEMOD_MODE_USB + cw.c */
#define RADIO_MODE_RTTY_L   7           /* DEMOD_MODE_LSB + rtty.c, config.h's tones */
#define RADIO_MODE_RTTY_U   8           /* DEMOD_MODE_USB + rtty.c, the tones swapped */
#define RADIO_MODE_PSK31    9           /* DEMOD_MODE_USB + psk31.c */
#define RADIO_MODE_NAVTEX   10          /* DEMOD_MODE_USB + navtex.c */
#define RADIO_MODE_DSC      11          /* DEMOD_MODE_USB + dsc.c */
#define RADIO_MODE_WEFAX    12          /* DEMOD_MODE_USB + wefax.c */
#define RADIO_MODE_SSTV     13          /* DEMOD_MODE_USB + sstv.c */
#define RADIO_MODE_SSTV_FM  14          /* DEMOD_MODE_NFM + sstv.c */
#define RADIO_MODE_HELL     15          /* DEMOD_MODE_USB + hell.c */
#define RADIO_MODE_JTTY     16          /* DEMOD_MODE_USB + jtty_modo.c */
#define RADIO_MODE_ALE      17          /* DEMOD_MODE_USB + ale_modo.c */
#define RADIO_MODE_STANAG   18          /* DEMOD_MODE_USB + stanag_modo.c */
#define RADIO_MODE_IDENT    19          /* DEMOD_MODE_USB + ident.c */
#define RADIO_MODE_APRS     20          /* DEMOD_MODE_NFM + ax25.c, 1200 Bd */
#define RADIO_MODE_PKT300   21          /* DEMOD_MODE_LSB + ax25.c, 300 Bd */
#define RADIO_MODE_AIS      22          /* DEMOD_MODE_NFM + ais_modo.c */
#define RADIO_MODE_FT8      23          /* DEMOD_MODE_USB + ft8_modo.c */
#define RADIO_MODE_WSPR     24          /* DEMOD_MODE_USB + wspr_modo.c */
#define RADIO_MODE_HFDL     25          /* DEMOD_MODE_USB + hfdl_modo.c, on the I/Q */
#define RADIO_MODE_LAST     RADIO_MODE_HFDL
void        radio_mode_set(int mode);
int         radio_mode_get(void);
const char *radio_mode_name(int mode);

/* Tune to a band: the dial, then the mode (-1 = leave it). Returns 0 if
 * the dial could not go there. `name` goes in the header's band pill. */
int         radio_band_go(uint32_t hz, int mode, const char *name);
uint32_t    radio_dial_hz(void);

/* Tune to a typed frequency: clamped to the dial's range, as upstream's
 * keypad clamps to its own. Returns the dial reached, 0 if the tuner
 * refused. */
uint32_t    radio_dial_go(uint32_t hz);
void        radio_fmt_khz(uint32_t hz, char *out, uint32_t n);  /* "9.600,00" */

/* The filter's two cuts, of the family the mode in force uses. */
void        radio_filtro_get(uint16_t *lo, uint16_t *hi);
void        radio_filtro_set(uint16_t lo, uint16_t hi);

/* The tuning step, by index into upstream's table (paso_tabla[] in the
 * generated bandas_tabla.h). */
uint32_t    radio_step_count(void);
uint32_t    radio_step_get(void);
void        radio_step_set(uint32_t i);

/* For the settings screen: values to show and what a tap does, for the
 * settings whose state is main.c's. */
const char *radio_agc_text(void);
void        radio_agc_cycle(void);
const char *radio_bw_text(void);
void        radio_bw_cycle(void);
int         radio_preamp(void);
void        radio_preamp_toggle(void);
const char *radio_att_text(void);
const char *radio_vol_text(void);
void        radio_knob2_rf(int rf);
int         radio_vol_db(void);
int         radio_vol_set(int db);      /* clamps; returns what is in force */
int         radio_att_db(void);         /* dB of gain reduction */
int         radio_att_set(int db);
int         radio_suav_pct(void);       /* the trace's smoothing, 0-95 % */
int         radio_suav_set(int pct);    /* clamps, 5 % steps; returns it */
void        radio_repaint_all(void);
void        radio_idioma(uint8_t v);    /* IDIOMA_ES / IDIOMA_EN, idioma.h */

/* The filter of each family as upstream's quick preset it equals, 2 bits
 * a family (3 = none of them, leave it); and back. */
uint8_t     radio_bw_pack(void);
void        radio_bw_unpack(uint8_t packed);

/* After a screen closes: the spectrum area belongs to the radio again,
 * and has to be painted whole. */
void        radio_screen_restore(void);

/* The action bar's state changed (which screen is open). */
void        radio_act_refresh(void);

/* The clock was set: the header's corner now, not at the next second. */
void        radio_reloj_repinta(void);

#endif /* MALAHIT_MENUS_H */
