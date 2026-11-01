#ifndef MALAHIT_I2C3_H
#define MALAHIT_I2C3_H

#include <stdint.h>

/*
 * I2C3, master, for the capacitive touch controller.
 *
 * Pins, from the schematic:
 *   PA8  I2C3_SCL  AF4
 *   PC9  I2C3_SDA  AF4
 *   PC8  touch IRQ, active low while a finger is down
 *   PC7  touch RESET, active low
 *
 * Both bus pins need external pull-ups; the panel flex carries them.
 * They are configured open-drain here regardless, because an I2C master
 * that drives high is a short circuit waiting for the other end to
 * answer.
 *
 * KERNEL CLOCK. I2C123SEL in RCC_D2CCIP2R picks it, and this driver
 * SETS it rather than trusting the reset value - the fifth time that
 * lesson has been paid for on this board. 00 is rcc_pclk1, 120 MHz
 * here, which is what the timing constant below is computed from.
 *
 * All waits are bounded. A bus with nothing on it, or with a device
 * holding SDA low, must produce an error and a line of text, not a
 * firmware that hangs with the screen still showing the last thing it
 * drew.
 */

void i2c3_init(void);

/* The two bus lines read as plain inputs: bit 1 is SCL, bit 0 is SDA.
 * Both should be 1 when the bus is idle. Anything else is a wiring or
 * pull-up problem wearing the costume of a protocol problem. */
uint32_t i2c3_line_levels(void);

/* 1 if a device acknowledges its address, 0 if not. The whole of a bus
 * scan is this function in a loop. */
int i2c3_probe(uint8_t addr7);

/* Each returns 1 on success, 0 on NACK, timeout or bus error. */
int i2c3_write(uint8_t addr7, const uint8_t *data, uint32_t len);
int i2c3_read(uint8_t addr7, uint8_t *data, uint32_t len);

/* Write then read without releasing the bus - a repeated start. This is
 * how nearly every touch controller is read: say which register, then
 * take the answer. */
int i2c3_write_read(uint8_t addr7,
                    const uint8_t *wdata, uint32_t wlen,
                    uint8_t *rdata, uint32_t rlen);

/* Drive the controller's reset line low, then release it. */
void i2c3_touch_reset(void);

/* 1 while the IRQ line says a finger is present (it is active low). */
int i2c3_touch_irq_asserted(void);

#endif /* MALAHIT_I2C3_H */
