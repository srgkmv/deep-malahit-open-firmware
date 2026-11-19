#ifndef MALAHIT_ENC2_H
#define MALAHIT_ENC2_H

#include <stdint.h>

/*
 * THE SECOND KNOB - the small one on top, RENC2 on PC2/PC3, with its
 * button on the analog ladder.
 *
 * WHERE THESE FUNCTIONS COME FROM, because grepping for them finds
 * nothing: they are `User/encoder.c`, compiled a SECOND time with its
 * public names renamed on the command line (see `ENC2_RENAME` in
 * reglas.mk). Upstream's module keeps its state in file-scope statics,
 * which makes it a one-knob module; a second translation unit of the
 * same source gets its own copy of those statics, which is exactly what
 * a second knob needs.
 *
 * So this knob runs the identical quadrature table, the identical
 * debounce integrator and the identical short/long classification as
 * the tuning knob - already tested on the radio - and there is no
 * second decoder anywhere to drift out of step with the first.
 *
 * Its button arrives through the same ENC_BTN_READ hook as the other
 * one, pointed at adc_enc2_down(): both buttons are voltages on one
 * analog line and neither is a pin. See adc.h.
 */

void     encoder2_init(void);
void     encoder2_tick(void);           /* 1 kHz, from SysTick */

int32_t  encoder2_take_delta(void);     /* whole detents, remainder carried */
uint8_t  encoder2_take_press(void);
uint8_t  encoder2_take_long_press(void);
uint8_t  encoder2_button_down(void);
void     encoder2_consume_press(void);
void     encoder2_invertido_pon(uint8_t v);
uint8_t  encoder2_invertido(void);

#endif /* MALAHIT_ENC2_H */
