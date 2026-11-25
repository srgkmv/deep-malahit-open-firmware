/*
 * THE BORROWED RAM, THIS PORT'S OWN - step 46, 22/11/2026.
 *
 * Upstream's decoders that need a big state - DSC, Feld-Hell, WEFAX, WSPR,
 * HFDL, AIS, ALE, JTTY - do not own it: they borrow it through
 * User/hfdl_ram.h, and upstream's hfdl_ram.c lends them the DEEPSDR's
 * waterfall pixels (waterfall_presta()), which is free while a text or an
 * image mode has the screen. The modes are exclusive, so one region serves
 * them all, and a decoder's stop() gives it back.
 *
 * This port's waterfall cannot be lent like that: main.c keeps pushing rows
 * into it in every mode. So the region is a buffer of its own (in D2 SRAM
 * until step 57, AXI since - below).
 * Same three calls, same contract: coge() before todo(), suelta() after,
 * and a decoder that finds it too small refuses to start rather than
 * running on someone else's memory.
 *
 * IT IS UPSTREAM'S UNION ITSELF SINCE STEP 57 (FT8, 25/11/2026). FT8 does
 * not borrow through hfdl_ram_todo(): its files name the members of
 * g_ft8_shared_ram (User/ft8_shared_ram.h) directly - the FFT's arrays, the
 * spectrogram of the slot, the LDPC's messages, the lines - and take the
 * loan with waterfall_presta(1). So the pool is now that union, defined
 * here, and hfdl_ram_todo() hands out the whole of it, as upstream's
 * hfdl_scope_get_hfdl_ram() does. Its size is the largest member's: FT8's,
 * 159 kB with this port's flags (FREQ_OSR=2 and a byte a cell, reglas.mk;
 * ~68 kB as upstream builds it), against HFDL's 66 kB and the waterfall's
 * 54 kB. It was 56 kB (JTTY's 52.4 kB - 48 was refused on the radio), and
 * the assert below keeps it from ever being less.
 *
 * IN AXI SRAM SINCE STEP 57, NOT D2. It was in D2 because a RAM image had
 * to fit AXI whole; this port loads flash now, and AXI has the room. And it
 * is faster with the D-cache off: FT8's candidate search (ftx_find_candidates(),
 * one call at the slot edge, over the whole spectrogram) took long enough
 * from D2 with FREQ_OSR=2 that the decimator's 320 ms ran out and two
 * sub-blocks were lost each slot - right where the transmissions begin.
 *
 * Upstream's union is .bss and is wiped each time it is given back
 * (hfdl_ram_suelta(), waterfall_init()). Here it is wiped when it is TAKEN:
 * FT8's lines live in it and its start() does not clear them, so a JTTY
 * table left behind would come up as FT8 decodes.
 */
#include <stdint.h>
#include <string.h>
#include "hfdl_ram.h"          /* ../../User - the contract */
#include "ft8_shared_ram.h"    /* ../../User - the union, FT8's way in */
#include "waterfall.h"         /* ../../User - waterfall_presta() and friends */
#include "console.h"

/* What the borrowers before FT8 were sized against: JTTY's 52.4 kB. */
#define PRESTAMO_MIN  57344U

__attribute__((aligned(32)))
ft8_shared_ram_t g_ft8_shared_ram;
_Static_assert(sizeof(ft8_shared_ram_t) >= PRESTAMO_MIN,
               "the borrowed RAM would shrink below what JTTY needs");

static uint8_t s_cogida;

/* upstream's waterfall.c: 1 lends the pixels, 0 takes them back. Here, the
 * pool's lock - and wiped on the way out, see the top of the file. */
void waterfall_presta(uint8_t si)
{
    if (si) {
        if (s_cogida) {
            /* Upstream's modes give it back in their stop() before the next
             * start(); two at once would be a bug in the mode change. */
            console_printf("  *** borrowed RAM taken twice ***\n");
        }
        memset(&g_ft8_shared_ram, 0, sizeof g_ft8_shared_ram);
        s_cogida = 1U;
    } else {
        s_cogida = 0U;
    }
}

/* upstream's: the memset that makes the buffer pixels again, called by
 * ft8_modo_stop() after it gives the loan back. There are no pixels here;
 * the wipe is done when the pool is taken. */
void waterfall_init(void)
{
}

void hfdl_ram_coge(void)
{
    waterfall_presta(1U);
}

void hfdl_ram_suelta(void)
{
    waterfall_presta(0U);
}

/* upstream's waterfall.c answer to "is someone else holding it?", asked by
 * the modes that take the loan from a poll (JTTY, ALE, AIS...) rather than
 * from the mode change. Here, the same lock. */
uint8_t waterfall_prestada(void)
{
    return s_cogida;
}

uint8_t *hfdl_ram_todo(uint32_t *capacity_out)
{
    if (!s_cogida) {
        if (capacity_out != (uint32_t *)0) { *capacity_out = 0U; }
        return (uint8_t *)0;
    }
    if (capacity_out != (uint32_t *)0) { *capacity_out = (uint32_t)sizeof g_ft8_shared_ram; }
    return (uint8_t *)&g_ft8_shared_ram;
}
