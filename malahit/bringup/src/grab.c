#include "grab.h"
#include "ili9488.h"

uint8_t g_grab[GRAB_BYTES] __attribute__((section(".tcmram"), aligned(4)));
volatile uint32_t g_grab_seq;

__attribute__((noinline)) void panel_grab(int y0, int rows)
{
    if (y0 < 0 || y0 >= 320 || rows <= 0) { g_grab_seq++; return; }
    if (rows > (int)GRAB_ROWS)  { rows = (int)GRAB_ROWS; }
    if (y0 + rows > 320)        { rows = 320 - y0; }

    ili9488_read_gram(0U, (uint16_t)y0, (uint16_t)GRAB_W, (uint16_t)rows,
                      g_grab, (uint32_t)rows * GRAB_W * 3U + 4U);
    g_grab_seq++;
}

void grab_init(void)
{
    g_grab_seq = 0U;
    /* panel_grab() has no caller in the image: swd-call is its caller.
     * Named here so --gc-sections keeps it (see src/demod.c). */
    __asm__ volatile ("" :: "r"(panel_grab));
}
