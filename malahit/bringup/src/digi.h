#ifndef MALAHIT_DIGI_H
#define MALAHIT_DIGI_H

#include <stdint.h>

/*
 * THE DECODERS' SCREEN - step 42, 22/11/2026.
 *
 * Upstream's arrangement for a text mode, the owner's choice: the
 * spectrum and the waterfall give their place to the tone scope on top
 * (upstream's rtty_scope_draw(): rtty_scope.c's audio spectrum, 0-6 kHz,
 * through spectrum.c, with the tone's lines) and ui_digi.c's text panel
 * below, the panel's bottom where the waterfall's was. The geometry is
 * compat/malahit_layout.h's DIGI_* and UDG_*; the host render
 * (deepsdr-ui `make malahit-menus`, out/malahit_cw.png) draws the same.
 *
 * What upstream's main.c does for it - rtty_text_push(), digi_sync(),
 * rtty_scope_draw(), the Borrar button - is here, cut down to what the
 * decoders grafted so far need: CW.
 */

/* On (1) when a text decoder is chosen, off (0) when it is left. Clears
 * the text. Draws nothing: main.c repaints the screen after a mode. */
void    digi_pon(uint8_t on);
uint8_t digi_en_pantalla(void);

/* A character the decoder has finished, as upstream's rtty_text_push():
 * \r and \n start a new row, a row that would pass the right margin
 * wraps. */
void    digi_texto(char c);

/* Once a frame, in place of the spectrum and the waterfall, and only
 * with no menu open: the scope when rtty_scope.c has a frame, the rows
 * that changed, and the chip when it changed. */
void    digi_dibuja(void);

/* The whole area again - after a menu, a theme, entering the mode. */
void    digi_repinta(void);

/* A finger in the area: returns 1 if it is the panel's (anything from
 * the scope's top to the panel's bottom while it is shown). A press on
 * the Borrar button clears the text. */
uint8_t digi_toque(uint16_t x, uint16_t y, uint8_t nuevo);

/* RTTY's speed as an index into upstream's four (45.45, 50, 75, 100),
 * the nearest to what rtty.c runs at - for the FRAM - and setting one. */
uint8_t digi_rtty_baud_idx(void);
void    digi_rtty_baud_pon(uint8_t idx);

/* The bench's: RTTY's settings now and the scope's two peaks, on the
 * console (swd-call ... digi_report). */
void    digi_report(void);

/* WEFAX (step 47): from the main loop whenever it is on - takes the lines
 * out of wefax.c and, if `pinta` (no menu over the screen), draws them. */
void digi_fax_poll(uint8_t pinta);

/* SSTV (step 48): the same, for its picture. */
void digi_sstv_poll(uint8_t pinta);

/* Feld-Hell (step 49): the same, for its columns. */
void digi_hell_poll(uint8_t pinta);

/* ALE (step 51): upstream's ale_modo_poll() at its 16 ms pace, the clock of
 * the last word and the table's changes to the console - from the main
 * loop, ALE on; and the clock and that record cleared (entry, Borrar). */
void digi_ale_poll(void);
void digi_ale_borra(void);

/* STANAG (step 52): its rows to the console when they change, once a
 * second at most - from the main loop, STANAG on. */
void digi_stanag_poll(void);

/* IDENT (step 53): its rows to the console when they change, once a
 * second at most - from the main loop, IDENT on. */
void digi_ident_poll(void);

/* AIS (step 55): its rows to the console when they change, and that record
 * cleared (entry, Borrar) - from the main loop, AIS on. */
void digi_ais_poll(void);
void digi_ais_borra(void);
/* FT8 (step 57): each decode and each slot's numbers to the console; the
 * panel's view (scroll, counters) back to the newest on entering. */
void digi_ft8_poll(void);
void digi_ft8_borra(void);

#endif /* MALAHIT_DIGI_H */
