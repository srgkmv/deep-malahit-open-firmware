#ifndef MALAHIT_GUARDA_H
#define MALAHIT_GUARDA_H

#include <stdint.h>

/*
 * PICTURES TO THE DISK (step 66): screenshots, SSTV pictures and WEFAX
 * charts as BMP files on the SPI flash's FAT volume - the USB disk.
 *
 * SSTV and WEFAX are upstream's own machinery from its main.c (gsv_abre(),
 * gsv_bloque(), gsv_mete(), gsv_linea(), gsv_cierra() and their callers in
 * sstv_poll() and fax_poll()), rewritten here with the same calls into
 * spi_flash.c and bmp.c: a picture is written AS IT ARRIVES, a line at a
 * time, into space reserved for it (SSTV, fixed size) or grown 64 kB at a
 * time (a chart, whose length nobody knows), a 4 kB block at most per pass
 * of the loop; closed with its real height in the header.
 *
 * THE SCREENSHOT IS THIS PORT'S OWN. Upstream redraws the screen band by
 * band into the file, because its panel cannot be read back; the ILI9488
 * here can (RAMRD, src/grab.c), so the file is what the glass holds. While
 * it is taken - a few seconds - the display loop draws nothing (upstream's
 * rule too), so the picture is of one moment; the radio and the decoders
 * carry on. Upstream's gesture: hold the frequency four seconds (main.c).
 */

/* Every pass of the main loop, before the decoders' polls: a capture's
 * step, a notice's end, SSTV/WEFAX opening and closing. */
void    guarda_poll(void);

/* Before the disk goes to the PC (usbdisco_abre()): an open file closed
 * with what it has. */
void    guarda_suelta(void);

/* The FAT date and time the next file written takes: the radio's clock
 * (local time, as upstream), or upstream's filler with no clock set. */
void    guarda_sello(void);

/* 1 while the display loop must not draw: a capture, or its notice. */
uint8_t guarda_pantalla_quieta(void);

/* A screenshot: 1 started (a beep), 0 refused (a beep and a notice). */
uint8_t guarda_captura_pide(void);

/* SSTV and WEFAX lines, from digi.c, every one that comes - each file
 * decides whether it wants it. guarda_cupo() is upstream's "one block a
 * pass": a caller stops taking lines from the decoder when it is 0. */
uint8_t guarda_cupo(void);
void    guarda_sstv_linea(const uint8_t *l, uint16_t y);
void    guarda_fax_linea(const uint8_t *l);

/* The two settings (FRAM v4 bytes 47, 48), upstream's: SSTV 0 do not save,
 * 1 BMP 24 bit, 2 BMP 16 bit; WEFAX 0 do not save, 1 grey BMP. The
 * buttons' labels say the setting, or "REC 042" while a file is open. */
uint8_t     guarda_sstv_formato(void);
void        guarda_sstv_formato_pon(uint8_t f);
void        guarda_sstv_cicla(void);
const char *guarda_sstv_rotulo(void);
uint8_t     guarda_fax(void);
void        guarda_fax_pon(uint8_t on);
void        guarda_fax_cicla(void);
const char *guarda_fax_rotulo(void);
uint8_t     guarda_grabando(void);

#endif /* MALAHIT_GUARDA_H */
