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
 * into it in every mode. So the region is a buffer of its own, in D2 SRAM -
 * 160 kB of it are free, and AXI SRAM, where a RAM image has to fit whole,
 * is not (454 of 512 kB). Same three calls, same contract: coge() before
 * todo(), suelta() after, and a decoder that finds it too small refuses to
 * start rather than running on someone else's memory.
 *
 * SIZED FOR WHAT IS GRAFTED: DSC's state is ~3.5 kB, WEFAX's ring 6 kB,
 * SSTV's output queue and Robot lines ~13 kB (step 48), JTTY's spectrum
 * ring, lines and message buckets 52.4 kB (step 50: jtty_rx_ram_t alone is
 * 49 504 bytes on this target - 48 kB was refused on the radio). The decoders still to come ask for
 * more (WSPR 47 kB, HFDL HFDL_RAM_TOTAL, 66 kB): grow it when one of them
 * arrives - its start() will refuse until then, and demod.c says so on
 * the console.
 *
 * .d2sram is NOLOAD, so not zeroed at boot - every borrower clears what it
 * takes (dsc_start() does). The SRAM1-3 clocks are on from sai1.c's
 * rings_clock_on(), long before any mode starts.
 */
#include <stdint.h>
#include "hfdl_ram.h"          /* ../../User - the contract */
#include "console.h"

#define PRESTAMO_BYTES  57344U

__attribute__((section(".d2sram"), aligned(32)))
static uint8_t s_pozo[PRESTAMO_BYTES];
static uint8_t s_cogida;

void hfdl_ram_coge(void)
{
    if (s_cogida) {
        /* Upstream's modes give it back in their stop() before the next
         * start(); two at once would be a bug in the mode change. */
        console_printf("  *** borrowed RAM taken twice ***\n");
    }
    s_cogida = 1U;
}

void hfdl_ram_suelta(void)
{
    s_cogida = 0U;
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
    if (capacity_out != (uint32_t *)0) { *capacity_out = PRESTAMO_BYTES; }
    return s_pozo;
}
