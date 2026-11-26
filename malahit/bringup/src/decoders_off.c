/*
 * THE DECODERS demod_am.c HANDS AUDIO TO, SWITCHED OFF - step 8.
 *
 * demod_am.c does not only demodulate: on the way through it offers
 * copies of the signal to twenty-odd decoders - CW, RTTY, NAVTEX, PSK31,
 * SSTV, WEFAX, Hell, FT8, WSPR, HFDL, STANAG, ALE, AIS, APRS, DSC, RDS,
 * DCF77 and the rest - each behind its own "are you on?" call. Linking
 * the real ones is the 106-file closure; none of them is needed to hear
 * AM, SSB or NFM.
 *
 * So each one is here, answering "off", and taking nothing. This is
 * the same set upstream's host bench stubs (deepsdr-ui/sim/
 * demod_stubs.c), found independently on this port by the census of
 * 21/11/2026: the linker asked for exactly these 52 names plus the
 * four the port supplies for real (src/demod.c, bip.c, compat/).
 *
 * The upstream headers are included ON PURPOSE, unlike the host bench:
 * a stub whose signature drifts from its header is a stub the compiler
 * should refuse, and here it will.
 *
 * GRAFTING A DECODER is: delete its lines here, add its .c to
 * reglas.mk, and let the linker say what it needs next. A name defined
 * twice fails the link, loudly, which is the right way for that to fail.
 */
#include <stdint.h>
#include "ais_modo.h"
#include "ale_modo.h"
#include "analizador.h"
#include "ax25.h"
#include "cw.h"
#include "dcf77.h"
#include "dsc.h"
#include "ft8_modo.h"
#include "hell.h"
#include "hfdl_modo.h"
#include "ident.h"
#include "jtty_modo.h"
#include "navtex.h"
#include "psk31.h"
#include "rds.h"
#include "rtty.h"
#include "rtty_scope.h"
#include "sstv.h"
#include "stanag_modo.h"
#include "wefax.h"
#include "wspr_modo.h"
#include "sdr_rx.h"

#define OFF_U16(ACT, MET)                                                \
    uint8_t ACT(void) { return 0U; }                                     \
    void MET(const float *a, uint16_t n) { (void)a; (void)n; }
#define OFF_U32(ACT, MET)                                                \
    uint8_t ACT(void) { return 0U; }                                     \
    void MET(const float *a, uint32_t n) { (void)a; (void)n; }

/* ais_modo_activo, ais_modo_mete: the real ones since 24/11/2026 (ais_*.c, step 55) */
/* ale_modo_activo, ale_modo_mete: the real ones since 22/11/2026 (ale_*.c, step 51) */
OFF_U32(analiz_activo,      analiz_feed)
/* ax25_activo, ax25_process: the real ones since 23/11/2026 (ax25.c, step 54) */
/* dsc_activo, dsc_process: the real ones since 22/11/2026 (dsc.c, step 46) */
/* ft8_modo_activo, ft8_modo_come: the real ones since 25/11/2026 (ft8_*.c, Ft8Lib, step 57) */
/* hell_activo, hell_process: the real ones since 22/11/2026 (hell.c, step 49) */
/* ident_modo_activo, ident_modo_mete: the real ones since 23/11/2026 (ident.c, step 53) */
/* jtty_modo_activo, jtty_modo_mete: the real ones since 22/11/2026 (jtty_*.c, step 50) */
/* navtex_activo, navtex_process: the real ones since 22/11/2026 (navtex.c, step 45) */
/* psk31_activo, psk31_process: the real ones since 22/11/2026 (psk31.c, step 44) */
/* rds_activo, rds_feed: the real ones since 22/11/2026 (rds.c, step 40) */
/* sstv_activo, sstv_process: the real ones since 22/11/2026 (sstv.c, step 48) */
/* stanag_modo_activo, stanag_modo_mete: the real ones since 23/11/2026 (stanag_*.c, s4415.c, step 52) */
/* wefax_activo, wefax_process: the real ones since 22/11/2026 (wefax.c, step 47) */
/* wspr_modo_activo, wspr_modo_mete: the real ones since 25/11/2026 (wspr_*.c, step 58) */

/* The ones that do not follow the pattern. */

/* cw_*: the real ones since 22/11/2026 (cw.c, step 42) */

/* rtty_*: the real ones since 22/11/2026 (rtty.c, step 43) */
/* rtty_scope_*: the real ones since 22/11/2026 (rtty_scope.c, step 42) */

/* hfdl_modo_activo, hfdl_modo_mete: the real ones since 26/11/2026 (hfdl_*.c, step 59) */

/* dcf77_*: the real ones since 24/11/2026 (dcf77.c, hora_*.c, step 56) */

/*
 * HFDL's aircraft models (step 59): upstream reads them out of ICAO24.BIN on
 * the external flash volume (User/icao24_db.c, over fat32.c and spi_flash.c).
 * This radio has no external flash, so the honest answer is "not found",
 * and HFDL's Model column stays empty - which is what upstream shows when
 * the file is missing. The database could live in the H743's own bank 2.
 */
#include <stdbool.h>
#include "icao24_db.h"
bool icao24_db_lookup(uint32_t icao24, char *out, uint32_t out_len)
{
    (void)icao24;
    if (out != (char *)0 && out_len > 0U) { out[0] = '\0'; }
    return false;
}

/*
 * Upstream asks this at the top of every block: sdr_rx.c on the DEEPSDR
 * inspects the DMA for a torn half and the demodulator mutes one if so.
 * This port's SAI driver has no such check yet - its halves are handed
 * over whole by the interrupt that completes them (sai1.c) - so the
 * honest answer is "no", and the place to wire a real one is here.
 */
uint8_t sdr_rx_last_block_corrupted(void) { return 0U; }
