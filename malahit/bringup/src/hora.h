#ifndef MALAHIT_HORA_H
#define MALAHIT_HORA_H
/*
 * THE RADIO CLOCK - step 56. Upstream's "Hora por radio" (its main.c,
 * hora_*()): a screen in the menus' place, Settings > Digital, that tunes
 * a time station - DCF77, MSF, ALS162, RBU, WWV 10/15, BPM - or FM with
 * RDS, shows what the decoder sees and puts the RTC in time; the zone,
 * chosen from upstream's table of cities or measured against a station
 * that sends UTC. And, as upstream's rds_poll(), FM with RDS sets the
 * clock on its own.
 */
#include <stdint.h>

/* The screen: menus.c's MENU_HORA calls these. */
void    hora_abre(void);          /* takes the radio: station, mode, dial */
void    hora_suelta(void);        /* gives it back, as it was */
void    hora_dibuja(void);
void    hora_toque(uint16_t x, uint16_t y, uint8_t pulsado);
void    hora_knob(int32_t d);
void    hora_knob_press(void);

/* From the main loop, every pass. */
void    hora_poll(void);          /* the screen, if it is up */
void    hora_rds_poll(void);      /* FM with RDS: the clock on its own */
void    hora_rds_reinicia(void);  /* another station: what RDS said, forgotten */

/* "The second is known": set to a minute's edge (the keypad's On the
 * minute, a station, RDS), not just to hh:mm. Upstream's s_hora_anclada. */
uint8_t hora_anclada(void);
void    hora_anclada_pon(uint8_t v);

/* The zone, for the FRAM: quarter hours + 64 (1..120), and whether a
 * station measured it (1 no, 2 yes); 0 = not stored. */
uint8_t hora_huso_byte(void);
uint8_t hora_medido_byte(void);
void    hora_huso_carga(uint8_t huso, uint8_t medido);

#endif
