#ifndef MALAHIT_BD_H
#define MALAHIT_BD_H

#include <stdint.h>

/*
 * BD.BIN - upstream's databases (the aircraft models HFDL names, the
 * broadcast stations) - from the USB disk to the SPI flash's high zone,
 * and opened (step 60).
 *
 * Upstream does this in its main.c, which this port does not compile:
 * datos_al_arrancar() at boot copies BD.BIN when the disk has one,
 * checks it and deletes it from the disk, then the high zone's directory
 * and the stations are opened. This file is that, rewritten with the same
 * calls into upstream's spi_flash.c and zona_alta.c, the same checks in
 * the same order - and the same moment: AT BOOT, before HFDL can ask
 * icao24_db.c, which remembers a failed open until the next boot.
 *
 * So the way to load a new BD.BIN is upstream's: copy it to the USB disk
 * (Settings > Device > USB disk), switch the disk off, restart the radio.
 */

/* At boot, the panel up and the language set: if the disk holds BD.BIN,
 * copy it, verify it, delete it. `avisa` shows a title and a line on the
 * panel (main.c's ui_prompt()). 1 if there was a BD.BIN to deal with. */
uint8_t bd_al_arrancar(void (*avisa)(const char *titulo, const char *linea));

/* Open what the high zone holds - its directory, the stations - and say
 * it on the console. Cheap: the directory and two headers. */
void bd_abre(void);

#endif /* MALAHIT_BD_H */
