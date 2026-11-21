#ifndef MALAHIT_GRAB_H
#define MALAHIT_GRAB_H

#include <stdint.h>

/*
 * SCREENSHOTS FROM THE PANEL ITSELF - 21/11/2026.
 *
 * The host render (deepsdr-ui) shows what the UI code MEANS to draw; this
 * shows what the glass holds, read back over SPI3's MISO with RAMRD. The
 * two differ exactly where the bugs are that a PNG cannot predict - an
 * uncleared gap, a clipped pill, a repaint that never came - and step 9
 * is a screen of menus that has to be checked without someone standing
 * at the bench.
 *
 * tools/grab.py drives it over SWD: panel_grab() for each strip of
 * GRAB_ROWS rows, called from the display loop the way swd-call.sh calls
 * anything, then the strip read out of g_grab and the next one asked
 * for. The buffer is in the DTCM, which the debugger reads like any
 * other RAM and which AXI SRAM cannot spare 46 kB of.
 */
#define GRAB_W      480U
#define GRAB_ROWS   32U
/* +4: RAMRD may or may not lead with a dummy byte, and the reader finds
 * the phase from the data, so every strip carries a byte to spare. */
#define GRAB_BYTES  (GRAB_W * GRAB_ROWS * 3U + 4U)

extern uint8_t           g_grab[GRAB_BYTES];
extern volatile uint32_t g_grab_seq;    /* +1 after each strip is in g_grab */

/* Called once from main(), so the linker keeps what nothing calls. */
void grab_init(void);

/* Rows y0 .. y0+rows-1 (rows <= GRAB_ROWS) into g_grab, then g_grab_seq++. */
void panel_grab(int y0, int rows);

#endif /* MALAHIT_GRAB_H */
