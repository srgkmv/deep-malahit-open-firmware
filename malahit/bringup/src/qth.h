#ifndef MALAHIT_QTH_H
#define MALAHIT_QTH_H

#include <stdint.h>

/*
 * THE QTH SCREEN - step 57, 25/11/2026. Upstream's (its main.c, qth_*()),
 * drawn by its ui_qth.c: the Maidenhead locator in six boxes, each only its
 * own alphabet, the coordinates under them, "-", "+", Apply, Exit. What is
 * edited is a copy; Apply gives it to ft8_decoder_set_own_grid(), where the
 * distances of FT8's CQs come from, and the settings record keeps it (FRAM
 * v4 bytes 39-44). Settings > Device > QTH opens it (src/menus.c).
 */
void qth_abre(void);
void qth_dibuja(void);
void qth_toque(uint16_t x, uint16_t y, uint8_t pulsado);   /* upstream's qth_touch() */
void qth_knob(int32_t d);           /* this port's: the box's character */
void qth_knob_press(void);          /* this port's: the next box */

#endif /* MALAHIT_QTH_H */
