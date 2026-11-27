#ifndef MALAHIT_DIGI_MAPA_H
#define MALAHIT_DIGI_MAPA_H

#include <stdint.h>

/*
 * THE MAP (step 64): upstream's world map under the decoders that place
 * what they hear - FT8 and WSPR by the locator in the message, HFDL's
 * aircraft and AIS's ships by position, JTTY by its locator field. The
 * fifth button of their panel ("Map", in HFDL "List"/"Map") shows it in
 * the place of the rows: 480 x 211 here (the owner's choice, 27/11/2026,
 * of the whole area below the panel's header), the whole world at zoom 1.
 *
 * The drawing, the projection, the zoom and drag, the labels and the card
 * are upstream's mapa.c, mapa_eti.c and mapa_bin.c, unmodified; this is
 * their glue from upstream's main.c (mapa_caja(), ft8_mapa_pinta(),
 * mapa_eti_llena(), ft8_mapa_draw(), mapa_zoom_pasos(), the map's part of
 * the touch handler), rewritten with the same calls.
 */

/* The five modes that have a map (upstream's modo_tiene_mapa()). */
uint8_t digi_mapa_modo(void);

/* The whole map, drawn now (upstream's ft8_mapa_draw()). */
void    digi_mapa_dibuja(void);

/* Draw it on the next digi_mapa_poll(): a drag moves the map many times
 * a frame and is painted once (upstream's s_mapa_sucio). */
void    digi_mapa_sucio(void);
void    digi_mapa_poll(void);

/* A finger in the map's area: the press (nuevo 1) and the moves; the lift.
 * The zoom keys on the release, a tap on a call opens its card, a drag
 * moves the map (upstream's s_zoom_tap / s_mapa_tap). */
uint8_t digi_mapa_toque(uint16_t x, uint16_t y, uint8_t nuevo);
void    digi_mapa_suelta(void);

/* The big knob while the map is shown: its zoom (upstream's
 * mapa_zoom_pasos() through mando_mapa_gira()). */
void    digi_mapa_zoom(int32_t pasos);

/* The map's area, for whoever routes the touches. */
uint8_t digi_mapa_en_zona(uint16_t x, uint16_t y);

/* From digi.c: FT8's CQ filter, which the marks and the labels follow
 * as the rows do (upstream's ft8_se_ve()). */
uint8_t digi_ft8_solo_cq(void);

/*
 * AT BOOT (upstream's mapa_al_arrancar()): a .map on the USB disk is
 * copied into the high zone's map hole, checked and deleted from the disk
 * - BD.BIN's ritual - and then whatever the hole holds is loaded (the
 * built-in coast otherwise). `avisa` shows a title and a line on the
 * panel, as for BD.BIN. 1 if a catalogue map is in force.
 */
uint8_t digi_mapa_al_arrancar(void (*avisa)(const char *titulo, const char *linea));

#endif /* MALAHIT_DIGI_MAPA_H */
