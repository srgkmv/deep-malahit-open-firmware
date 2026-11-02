/*
 * THE PANEL CONTRACT gfx2 EXPECTS, IMPLEMENTED FOR THIS BOARD.
 *
 * It declares the include guard of the file it replaces, so that this
 * header and the upstream one can never both be in a translation unit:
 *
 *     -DRM68120_EXMC_H -include rm68120_ili9488.h
 *
 * The -D makes User/rm68120_exmc.h expand to nothing when gfx2.c
 * includes it, and the -include puts this in its place. Nothing
 * upstream is edited, and nothing upstream is copied.
 *
 * WHY THE OBVIOUS TRICK DOES NOT WORK. The usual way to substitute a
 * header is to put yours earlier on the include path. That cannot work
 * here: for `#include "rm68120_exmc.h"` the compiler searches the
 * directory of the file containing the directive FIRST, and that
 * directive is inside User/gfx2.c, so User/rm68120_exmc.h always wins
 * no matter what -I says. Wrapping gfx2.c in a file of our own does not
 * help either, for the same reason - the directive is still in gfx2.c.
 * Neutralising the guard is what is left, and it is exact.
 *
 * ---------------------------------------------------------------
 *
 * WHAT gfx2 ACTUALLY NEEDS, which is remarkably little: three symbols.
 *
 *     rm68120_set_window(x0,y0,x1,y1)   where the next pixels go
 *     rm68120_dato(uint16_t)            one RGB565 pixel
 *     rm68120_ciclo()                   a cycle counter, diagnostics only
 *
 * PORTING.md budgeted 1401 lines to replace the display layer. Most of
 * that is rm68120_exmc.c and lcd_dma.c, the panel driver itself - which
 * this board already has, in ili9488.c. The part gfx2 is coupled to is
 * three functions, because gfx2 was already decoupled: it is the same
 * file deepsdr-ui compiles against lcd_sim.c to render the UI on a PC.
 *
 * ---------------------------------------------------------------
 *
 * TWO MISMATCHES, BOTH HANDLED HERE.
 *
 * FORMAT. gfx2 composes in RGB565, one 16-bit write per pixel into a
 * memory-mapped parallel bus - on the GD32 `LCD_DAT = v` is a single
 * store and costs nothing. This panel is 18 bits over SPI (settled in
 * step 2, BOARD.md section 6): three bytes a pixel, and no memory
 * mapping at all.
 *
 * RATE. A store per pixel is free; an SPI transaction per pixel would
 * be ruinous - 480x320 is 153 600 of them. So rm68120_dato() does not
 * transmit. It converts into a staging buffer and the buffer goes out
 * in ONE transfer when the window is full. gfx2 always fills exactly
 * the window it just set, so the pixel count is known in advance and
 * the flush needs no cooperation from upstream.
 */
#ifndef MALAHIT_RM68120_ILI9488_H
#define MALAHIT_RM68120_ILI9488_H

#include <stdint.h>
/* The header this replaces pulled in NULL for its users; gfx.c relies
 * on that. A substitute has to provide what it stood in for. */
#include <stddef.h>

/* Set the destination rectangle and arm the staging buffer. Flushes
 * anything still pending from a previous window. */
void rm68120_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* Free-running cycle count, for gfx2's own bus-time accounting. */
uint32_t rm68120_ciclo(void);

/*
 * gfx2 accumulates bus time here: `g_lcd_ciclos += rm68120_ciclo() - t0`
 * around the push. It is a diagnostic the radio can display, so it has
 * to exist and be truthful, and it costs one add per band.
 */
extern volatile uint32_t g_lcd_ciclos;

/*
 * One pixel, RGB565 in, 18-bit in the staging buffer out.
 *
 * Inline and branch-light on purpose: it is called once per pixel and
 * upstream's comment on the original says the same thing - the cheap
 * ones go in the header, "ponerlas por pixel si, y por eso
 * rm68120_dato() SI esta aqui dentro".
 *
 * The ILI9488 takes the top six bits of each byte, so the conversion is
 * a shift and a mask per channel and no rounding: RGB565's five bits of
 * red become the top five of six, and the sixth is left clear. That
 * loses half a least-significant bit of brightness and nothing that can
 * be seen.
 */
extern uint8_t *g_rm_put;           /* next byte in the staging buffer */
extern uint8_t *g_rm_end;           /* one past the last of this window */

void rm68120_flush(void);           /* send what is staged */

/*
 * gfx.c wants two more. Both are small and both go through the same
 * staging path, so there is one way pixels reach the panel and not two.
 *
 * write_data() is only ever called by gfx.c straight after a 1x1
 * set_window, to put a single pixel - so that is what it does, rather
 * than anything to do with command parameters.
 */
void rm68120_write_data(uint16_t rgb565);
void rm68120_fill_screen(uint16_t rgb565);

static inline void rm68120_dato(uint16_t v)
{
    uint8_t *p = g_rm_put;

    if (p < g_rm_end) {
        p[0] = (uint8_t)((v >> 8) & 0xF8U);         /* R: 5 bits */
        p[1] = (uint8_t)((v >> 3) & 0xFCU);         /* G: 6 bits */
        p[2] = (uint8_t)((v << 3) & 0xF8U);         /* B: 5 bits */
        g_rm_put = p + 3;

        if (g_rm_put >= g_rm_end) { rm68120_flush(); }
    }
}

#endif /* MALAHIT_RM68120_ILI9488_H */
