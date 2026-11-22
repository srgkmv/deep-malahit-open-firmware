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

OFF_U16(ais_modo_activo,    ais_modo_mete)
OFF_U16(ale_modo_activo,    ale_modo_mete)
OFF_U32(analiz_activo,      analiz_feed)
OFF_U32(ax25_activo,        ax25_process)
OFF_U32(dsc_activo,         dsc_process)
OFF_U16(ft8_modo_activo,    ft8_modo_come)
OFF_U32(hell_activo,        hell_process)
OFF_U16(ident_modo_activo,  ident_modo_mete)
OFF_U16(jtty_modo_activo,   jtty_modo_mete)
OFF_U32(navtex_activo,      navtex_process)
/* psk31_activo, psk31_process: the real ones since 22/11/2026 (psk31.c, step 44) */
/* rds_activo, rds_feed: the real ones since 22/11/2026 (rds.c, step 40) */
OFF_U32(sstv_activo,        sstv_process)
OFF_U16(stanag_modo_activo, stanag_modo_mete)
OFF_U32(wefax_activo,       wefax_process)
OFF_U16(wspr_modo_activo,   wspr_modo_mete)

/* The ones that do not follow the pattern. */

/* cw_*: the real ones since 22/11/2026 (cw.c, step 42) */

/* rtty_*: the real ones since 22/11/2026 (rtty.c, step 43) */
/* rtty_scope_*: the real ones since 22/11/2026 (rtty_scope.c, step 42) */

uint8_t hfdl_modo_activo(void) { return 0U; }
void    hfdl_modo_mete(const float *i_in, const float *q_in, uint32_t n)
{ (void)i_in; (void)q_in; (void)n; }

/* The time-signal decoder. Off means demod_am.c never asks the other
 * four, but they have to exist for it to link. */
uint8_t        dcf77_activo(void) { return 0U; }
void           dcf77_feed(float m) { (void)m; }
float          dcf77_fase_iq(float i, float q) { (void)i; (void)q; return 0.0f; }
uint8_t        dcf77_usa_fase(hora_emisora_t e) { (void)e; return 0U; }
uint8_t        dcf77_submuestras(hora_emisora_t e) { (void)e; return 1U; }
hora_emisora_t dcf77_emisora(void) { return (hora_emisora_t)0; }

/*
 * Upstream asks this at the top of every block: sdr_rx.c on the DEEPSDR
 * inspects the DMA for a torn half and the demodulator mutes one if so.
 * This port's SAI driver has no such check yet - its halves are handed
 * over whole by the interrupt that completes them (sai1.c) - so the
 * honest answer is "no", and the place to wire a real one is here.
 */
uint8_t sdr_rx_last_block_corrupted(void) { return 0U; }
