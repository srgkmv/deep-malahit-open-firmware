#ifndef MALAHIT_CONSOLE_H
#define MALAHIT_CONSOLE_H

#include <stdint.h>

/*
 * UART7 on PE7 (RX) / PE8 (TX), AF7.
 *
 * These two pins are labelled RESERVED_PE7_UART7_RX and
 * RESERVED_PE8_UART7_TX on the schematic and come out on header J6 - pin 3
 * and pin 2 respectively, per the netlist. The board designers left a
 * console on the board and never used it.
 *
 * J6 has no ground pin. Take GND from J2 pin 3 (the SWD connector) or any
 * ground pad; a USB-serial adapter that shares no ground with the radio
 * reads noise.
 *
 * 115200 8N1. Output only for now - nothing reads from the console yet,
 * but RX is wired up so it is there when a command interpreter is.
 */
void console_init(void);

void console_putc(char c);
void console_puts(const char *s);

/*
 * A deliberately small printf. Supports %s %c %% and, for integers,
 * %d %u %x %08x (and %lu/%lx, which are the same thing on this target).
 *
 * Not newlib's. Pulling in the real printf costs tens of kilobytes, drags
 * in malloc, and - the part that actually matters - its %f would quietly
 * add floating point work inside whatever interrupt someone eventually
 * calls it from. A bring-up console needs numbers and names, and nothing
 * here should ever be the reason a deadline is missed.
 */
void console_printf(const char *fmt, ...);

#endif /* MALAHIT_CONSOLE_H */
