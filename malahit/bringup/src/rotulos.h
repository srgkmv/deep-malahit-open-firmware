#ifndef MALAHIT_ROTULOS_H
#define MALAHIT_ROTULOS_H

#include <stdint.h>

/*
 * THE STATIONS ON THE SPECTRUM (step 61): upstream's "BD en espectro" -
 * the names of the broadcast stations on the air now, from BD.BIN's
 * station database (emisoras.c), over the trace, at their frequencies.
 *
 * The choosing and the drawing are upstream's spec_rotulos.c, compiled
 * here since step 8; the glue - which window, which minute, when to ask
 * the database again, the hook into spectrum_draw() - lives in upstream's
 * main.c (bd_rotulos_refresca(), bd_rotulos_textos(), bd_rotulos_banda(),
 * emis_cuando()), which this port does not compile, and is rewritten in
 * rotulos.c with the same calls in the same order.
 */

/* Upstream's setting, Settings > Display > "DB on spectrum": born off,
 * kept in the FRAM (record v4 byte 45). The menu switches it on only
 * with a database open, as upstream's; with none, nothing is drawn. */
uint8_t rotulos_puesto(void);
void    rotulos_pon(uint8_t on);

/* Around the display loop's spectrum_draw(): before it, the list for the
 * window about to be drawn (centre and span in Hz, the dial for the
 * tie-breaks) and the hook put in; after it, the hook out. */
void    rotulos_antes(uint32_t center_hz, uint32_t span_hz, uint32_t tune_hz);
void    rotulos_despues(void);

#endif /* MALAHIT_ROTULOS_H */
