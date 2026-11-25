#ifndef RTC_HW_PORT_H
#define RTC_HW_PORT_H
/*
 * What this port's src/rtc_hw.c adds to upstream's contract
 * (../../User/rtc_hw.h). Step 56.
 */
#include <stdint.h>

/* From the main loop: finishes the RTC's start when the 32.768 kHz
 * crystal is ready (rtc_hw_init() does not wait for it), or gives up. */
void    rtc_hw_poll(void);

/* 1 if this start found the calendar dead and seeded it (2026-01-01). */
uint8_t rtc_hw_frio(void);

/* Set it "now" to dt plus frac_ms, from a source good to calidad_ms (a
 * station, RDS, the keypad's On the minute); learns the crystal's error
 * since the last such setting into the smooth calibration. src/rtc_hw.c. */
#include "rtc_hw.h"
void  rtc_hw_pon_anclado(const rtc_hw_datetime_t *dt, uint16_t frac_ms, uint16_t calidad_ms);
#define RTC_CAL_NINGUNA 0xFFFFU /* calidad_ms: sets, never teaches nor anchors */
float rtc_hw_desvio_s(const rtc_hw_datetime_t *dt, uint16_t frac_ms);  /* RTC - dt, s */
float rtc_hw_cal_ppm(void);     /* the calibration in force, ppm (+ = faster) */

#endif
