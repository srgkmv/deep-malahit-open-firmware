/*
 * THE RADIO CLOCK - step 56, 24/11/2026. See hora.h.
 *
 * Upstream's hora_show(), hora_sintoniza(), hora_poll(), hora_poll_rds(),
 * hora_touch(), the sweep (barrido_*()), the zone grid (GRID_HUSO) and
 * rds_poll(), from its main.c, onto this port's calls: radio_mode_set()
 * and radio_dial_go() where upstream has apply_demod_mode() and
 * apply_lo_tune(), menus.c's MENU_HORA where upstream has its own menu
 * layer, the RTC through upstream's rtc_hw contract (src/rtc_hw.c). The
 * screen is upstream's ui_hora.c; the decoders upstream's dcf77.c,
 * hora_rbu.c, hora_wwv.c, hora_comun.c; the table of cities husos.c.
 *
 * WHAT IS THIS PORT'S, AND WHY:
 *
 *   THE SECOND, PROPERLY. Upstream anchors its software clock to the
 *   minute's edge and then writes the RTC with whole seconds, so the
 *   fraction of the second at the moment of writing is lost - up to a
 *   second, which is FT8's whole margin. Here rtc_pon_anclado() writes
 *   the whole seconds and then the fraction with the RTC's own shift
 *   (rtc_hw_apply_shift(), 1/256 s steps).
 *
 *   THE EDGE IS WATCHED EVERY PASS. Upstream notes when DCF_LISTO turned
 *   up from a poll throttled to 100-200 ms; here hora_poll() looks at the
 *   decoder on every pass of the loop (~45 ms) and only the drawing is
 *   throttled, so the "atraso" is off by a pass, not by a throttle.
 *
 *   RDS ONLY FIXES A CLOCK THAT IS PLAINLY WRONG, AND THE DATE. Upstream's
 *   rds_poll() anchors the clock to every confirmed group, believing the
 *   standard's +-0.1 s; Riga's stations measured 2 to 6 s late (see
 *   RDS_UMBRAL_S). Here RDS sets the clock when it was never set or is
 *   more than 15 s off, sets the date when only the date is wrong - a
 *   radio that only ever hears FM would otherwise keep the seed's
 *   2026-01-01 - and never counts as knowing the second.
 *
 *   THE ZONE LIST IS ORDERED BY OFFSET, NOT BY DISTANCE. Upstream orders
 *   the cities by the distance to the FT8 locator; this port has no
 *   locator yet, so the cities whose offset today is the radio's come
 *   first, then the nearer offsets.
 *
 *   EACH DECODER'S LATENCY IS TAKEN OFF. Upstream counts the "atraso" from
 *   the moment its poll saw DCF_LISTO, as if that were the minute's edge.
 *   It is not: each decoder knows the minute at its own place in the
 *   frame. Measured on upstream's own benches (sim/ *test.c, their
 *   generators, at the rates the radio feeds them), from the true edge to
 *   the decoder's new minute:
 *       DCF77 +0.029 s   MSF +0.421 s   RBU +0.190 s
 *       WWV   +1.107 s   BPM +1.086 s   - the rise of second 01's
 *                                         subcarrier closes the minute
 *       ALS162 -0.898 s  - it knows the next minute in second 59, before
 *                          the minute begins
 *   Uncorrected, WWV set this radio's RTC 1.41 s late against the HackRF
 *   (24/11/2026; the rest is the HackRF's start). k_latencia_ms[], below.
 *
 *   THE KNOB, WITH RDS, TUNES IN 100 kHz - FM's channels; upstream's also
 *   offers its PGA gain on a press, which this port does not have.
 */
#include <string.h>
#include "hora.h"
#include "menus.h"
#include "console.h"
#include "board.h"
#include "ui_hora.h"       /* ../../User - upstream's screen */
#include "ui_grid.h"       /* ../../User - upstream's grid, for the zone */
#include "dcf77.h"         /* ../../User - the six stations */
#include "hora_comun.h"    /* ../../User - the zone, days and minutes */
#include "husos.h"         /* ../../User - the cities */
#include "rds.h"           /* ../../User - rds_info(), rds_hora_atraso_ms() */
#include "rds_pi.h"
#include "rtc_hw.h"        /* ../../User - the contract, src/rtc_hw.c */
#include "rtc_hw_port.h"   /* rtc_hw_pon_anclado(): the second, and the crystal */
#include "demod_am.h"
#include "sdr_rx.h"        /* SDR_RX_BLOCK_SAMPLES */
#include "idioma.h"

#define HORA_FRESCA_MS    60000UL   /* upstream's: a minute's edge is good for this long */
#define BARRIDO_VISITA_S  25U       /* upstream's: seconds a station in the sweep */
#define RDS_CT_RETARDO_MS 1U        /* upstream's, measured in its sim/rdstest.c */
#define FM_PASO_HZ        100000UL
/* How good each source's minute edge is, for the crystal's learning
 * (src/rtc_hw.c): RDS's is the standard's +-0.1 s; a station's, the
 * decoder's filters and a pass of the loop - not measured, so generous. */
/*
 * RDS IS GOOD TO THE MINUTE, NOT TO THE SECOND - measured in Riga,
 * 25/11/2026, against the PC's NTP time: LR-1 (90.7) 5.75 s late, SWH
 * ROCK (89.2) 2.1 s late; the standard asks +-0.1 s. So RDS corrects the
 * clock only when it is plainly wrong - never set, or more than this off
 * (a lost VBAT, another minute, another zone) - and otherwise leaves a
 * station's or a hand's second alone, fixing at most the date. It never
 * teaches the crystal (RTC_CAL_NINGUNA). Upstream trusts its +-0.1 s.
 */
#define RDS_UMBRAL_S      15.0f
#define PASADA_MEDIA_MS   22        /* half a pass of the loop: when the edge was seen */

/* From the minute's true edge to the decoder's new minute, ms - see the
 * top. In hora_emisora_t's order; RDS keeps its own (rds_hora_atraso_ms()). */
static const int16_t k_latencia_ms[HORA_N] = {
    29, 421, -898, 190, 1107, 1107, 1086, 0
};

#define CAL_EMISORA_MS    200U

/* ------------------------------------------------------------------ */

static uint8_t         s_abierta;          /* the screen is up */
static uint8_t         s_rejilla;          /* ...showing the zone grid */
static hora_emisora_t  s_emisora = HORA_DCF77;
static int             s_modo_antes;
static uint32_t        s_dial_antes;
static audio_bw_t      s_bw_antes;
static ui_hora_state_t s_hora;
static uint32_t        s_ult_ms;
static uint32_t        s_listo_ms;         /* when the decoder's minute began */
static uint8_t         s_listo_ant, s_hh_ant = 0xFFU, s_mm_ant = 0xFFU;
static uint8_t         s_anclada;

static char s_hora_txt[12];
static char s_hora_fecha[16];
static char s_hora_det[96];
static char s_huso_btn[24];

/* the sweep */
static uint8_t  s_barrido;
static uint8_t  s_barrido_i;
static uint32_t s_barrido_ms0;
static uint8_t  s_barrido_mejor[HORA_N];
static char     s_barrido_txt[12U * HORA_N + 48U];

/* RDS's clock: upstream's rds_poll() state */
static uint8_t  s_rds_hora_ok;
static uint32_t s_rds_seq_ant;
static uint16_t s_rds_cand_mins;
static uint32_t s_rds_cand_ms;
static uint8_t  s_rds_cand_vale;
static uint16_t s_rds_ct_n, s_rds_ct_mal;

/* the zone grid */
static ui_grid_state_t s_grid;
static uint8_t  s_huso_orden[64];
static char     s_huso_titulo[48];
static char     s_huso_cel[UIG_CELLS][3][18];
static uint8_t  s_huso_medida_hay;
static int16_t  s_huso_medida;
static int8_t   s_grid_press = -1;
_Static_assert(sizeof s_huso_orden >= 50U, "s_huso_orden[] must hold husos_n()");

/* How long since the minute's true edge, ms; negative = it has not begun
 * (ALS162 just after its frame). */
static int32_t atraso_emisora(void)
{
    if (s_listo_ms == 0U) { return 0; }
    return (int32_t)(board_millis() - s_listo_ms) + (int32_t)k_latencia_ms[s_emisora]
           + PASADA_MEDIA_MS;
}

/* ------------------------------------------------------------------ */

uint8_t hora_anclada(void)         { return s_anclada; }
void    hora_anclada_pon(uint8_t v) { s_anclada = (uint8_t)(v != 0U); }

uint8_t hora_huso_byte(void)
{
    return (uint8_t)((hora_huso() / 15) + 64);
}

uint8_t hora_medido_byte(void)
{
    return (uint8_t)(hora_huso_medido() ? 2U : 1U);
}

void hora_huso_carga(uint8_t huso, uint8_t medido)
{
    int16_t min;

    if (huso == 0U) { return; }                     /* not stored: upstream's +1:00 */
    min = (int16_t)(((int16_t)huso - 64) * 15);
    if (medido == 2U) { hora_huso_mide(min); } else { hora_huso_pon(min); }
}

/* ------------------------------------------------------------------ */

static char *u2(char *b, uint32_t v)
{
    b[0] = (char)('0' + ((v / 10U) % 10U));
    b[1] = (char)('0' + (v % 10U));
    return b + 2;
}
static char *u3(char *b, uint32_t v) { b[0] = (char)('0' + ((v / 100U) % 10U)); return u2(b + 1, v); }
static char *u4(char *b, uint32_t v) { b[0] = (char)('0' + ((v / 1000U) % 10U)); return u3(b + 1, v); }
static char *pega(char *p, const char *t) { while (*t != '\0') { *p++ = *t++; } return p; }

/* "+5:30", "-3:00", "UTC" - upstream's huso_txt(). */
static void huso_txt(int16_t min, char *out)
{
    int16_t a = (int16_t)((min < 0) ? -min : min);
    char *p = out;

    if (min == 0) { (void)pega(out, "UTC"); out[3] = '\0'; return; }
    *p++ = (min < 0) ? '-' : '+';
    if (a / 60 >= 10) { *p++ = (char)('0' + (a / 60) / 10); }
    *p++ = (char)('0' + (a / 60) % 10);
    *p++ = ':';
    p = u2(p, (uint32_t)(a % 60));
    *p = '\0';
}

static void huso_boton_txt(void)
{
    char t[12];
    char *p = s_huso_btn;

    huso_txt(hora_huso(), t);
    p = pega(p, tr("Huso", "Zone"));
    *p++ = ' ';
    p = pega(p, t);
    *p = '\0';
    s_hora.huso = s_huso_btn;
}

/* The RTC's local time minus the zone: what the summer rules want.
 * Upstream's huso_utc_ahora(). */
static void huso_utc_ahora(uint8_t *dia, uint8_t *mes, uint8_t *anno2, uint8_t *hora)
{
    rtc_hw_datetime_t dt;
    uint8_t h, mi, d, m, a, ds;

    rtc_hw_get(&dt);
    if (dt.year < 2000U || dt.year > 2099U) { dt.year = 2026U; }
    h = dt.hour; mi = dt.minute;
    d = dt.day; m = dt.month; a = (uint8_t)(dt.year - 2000U);
    ds = hora_dia_semana(d, m, a);
    hora_suma_minutos((int16_t)-hora_huso(), &h, &mi, &d, &m, &a, &ds);
    *dia = d; *mes = m; *anno2 = a; *hora = h;
}

/* Upstream's huso_mide(): your clock minus a station's UTC, to the
 * quarter hour. 0 if the station sends no UTC or has not confirmed. */
static uint8_t huso_mide(int16_t *fuera)
{
    dcf_info_t inf;
    rtc_hw_datetime_t dt;
    int32_t utc, local, d;

    dcf77_info(&inf);
    if (!inf.utc_hay || inf.estado != DCF_LISTO) { return 0U; }
    if (rtc_hw_lxtal_failed()) { return 0U; }
    rtc_hw_get(&dt);
    utc   = (int32_t)inf.utc_hora * 60 + (int32_t)inf.utc_minuto;
    local = (int32_t)dt.hour * 60 + (int32_t)dt.minute;
    d = local - utc;
    while (d >  HORA_HUSO_MAX) { d -= 1440; }
    while (d <  HORA_HUSO_MIN) { d += 1440; }
    d = ((d >= 0) ? ((d + 7) / 15) : ((d - 7) / 15)) * 15;
    if (d > HORA_HUSO_MAX || d < HORA_HUSO_MIN) { return 0U; }
    *fuera = (int16_t)d;
    return 1U;
}

/*
 * THE RTC, ANCHORED: "atraso_ms ago it was exactly mins (local) of that
 * day". The whole seconds by rtc_hw_set(), the fraction by the RTC's
 * shift, which can advance a whole second less up to 1/256: ADD1S with
 * SUBFS = 256 - fraction*256 (src/rtc_hw.c, rtc_hw_pon_anclado(), which
 * also learns the crystal from it). con_fecha = 0 keeps the RTC's date;
 * calidad_ms is how good the source's edge is.
 */
static uint8_t rtc_pon_anclado(uint8_t con_fecha, uint8_t dia, uint8_t mes, uint8_t anno2,
                               int16_t mins, uint32_t atraso_ms, uint16_t calidad_ms,
                               float umbral_s)
{
    rtc_hw_datetime_t dt;
    uint8_t hh, mi, dd, mo, aa, ds, ss;
    uint32_t seg = atraso_ms / 1000U, frac = atraso_ms % 1000U, seq;

    if (rtc_hw_lxtal_failed()) { return 0U; }
    while (mins < 0)     { mins = (int16_t)(mins + 1440); }
    while (mins >= 1440) { mins = (int16_t)(mins - 1440); }
    rtc_hw_get(&dt);
    if (con_fecha && mes >= 1U && mes <= 12U && dia >= 1U && dia <= 31U) {
        dd = dia; mo = mes; aa = anno2;
    } else {
        dd = dt.day; mo = dt.month; aa = (uint8_t)(dt.year - 2000U);
    }
    hh = (uint8_t)(mins / 60); mi = (uint8_t)(mins % 60);
    ds = hora_dia_semana(dd, mo, aa);
    hora_suma_minutos((int16_t)(seg / 60U), &hh, &mi, &dd, &mo, &aa, &ds);
    ss = (uint8_t)(seg % 60U);

    dt.year = (uint16_t)(2000U + aa); dt.month = mo; dt.day = dd;
    dt.hour = hh; dt.minute = mi; dt.second = ss;
    if (umbral_s > 0.0f && rtc_hw_has_ever_synced()) {
        float e = rtc_hw_desvio_s(&dt, (uint16_t)frac);
        rtc_hw_datetime_t ya;

        rtc_hw_get(&ya);
        if (e < umbral_s && e > -umbral_s) {
            if (ya.year == dt.year && ya.month == dt.month && ya.day == dt.day) {
                console_printf("  hora: %d ms de la del reloj - se deja (menos de %u s)\n",
                               (int32_t)(-e * 1000.0f), (uint32_t)umbral_s);
                return 0U;
            }
            /* Only the date is wrong: the date, and the RTC's own second. */
            ya.year = dt.year; ya.month = dt.month; ya.day = dt.day;
            rtc_hw_set(&ya);
            console_printf("  hora: fecha corregida a %02u/%02u/%04u, la hora se deja\n",
                           (uint32_t)ya.day, (uint32_t)ya.month, (uint32_t)ya.year);
            radio_reloj_repinta();
            return 1U;
        }
    }
    seq = rtc_hw_get_set_seq();
    rtc_hw_pon_anclado(&dt, (uint16_t)frac, calidad_ms);
    if (rtc_hw_get_set_seq() == seq) { return 0U; } /* refused: an impossible date */
    s_anclada = (uint8_t)(calidad_ms != RTC_CAL_NINGUNA);
    radio_reloj_repinta();
    return 1U;
}

/* ------------------------------------------------------------------ */
/* the station */

static void sintoniza(void)
{
    s_listo_ms = 0U;
    s_listo_ant = 0U;
    if (s_emisora == HORA_RDS) {
        dcf77_stop();
        if (radio_mode_get() != (int)DEMOD_MODE_WFM) { radio_mode_set((int)DEMOD_MODE_WFM); }
        if (radio_dial_hz() < 87500000UL || radio_dial_hz() > 108000000UL) {
            (void)radio_dial_go(100000000UL);
        }
        s_hora.emisora = dcf77_nombre(HORA_RDS);
        s_hora.hora = 0; s_hora.fecha = 0; s_hora.puede_aplicar = 0U;
        console_printf("  hora: RDS, %u Hz\n", radio_dial_hz());
        return;
    }
    /* The six: AM and the narrowest filter there is - upstream's. */
    if (radio_mode_get() != (int)DEMOD_MODE_AM) { radio_mode_set((int)DEMOD_MODE_AM); }
    demod_am_set_audio_bw(AUDIO_BW_1K8);
    (void)radio_dial_go(dcf77_freq_hz(s_emisora));
    dcf77_start(s_emisora, demod_am_get_active_fs_hz() / (float)SDR_RX_BLOCK_SAMPLES
                           * (float)dcf77_submuestras(s_emisora));
    s_hora.emisora = dcf77_nombre(s_emisora);
    s_hora.hora = 0; s_hora.fecha = 0; s_hora.puede_aplicar = 0U;
    console_printf("  hora: %s, %u Hz\n", dcf77_nombre(s_emisora), radio_dial_hz());
}

static void barrido_arranca(void)
{
    memset(s_barrido_mejor, 0, sizeof s_barrido_mejor);
    s_barrido = 1U;
    s_barrido_i = 0U;
    s_barrido_ms0 = board_millis();
    s_emisora = HORA_DCF77;
    sintoniza();
    s_hora.emisora = tr("Buscando en todas", "Scanning all of them");
}

static void barrido_texto(void)
{
    static const char *const k_corto[HORA_N] = { "DCF", "MSF", "ALS",
                                                 "RBU", "W10", "W15", "BPM", "RDS" };
    char *p = s_barrido_txt;
    uint8_t k;

    for (k = 0U; k < (uint8_t)HORA_BARRIDO_N; k++) {
        p = pega(p, k_corto[k]);
        p = u3(p, s_barrido_mejor[k]);
        if (dcf77_usa_fase((hora_emisora_t)k)) { *p++ = '*'; }
        if (k == s_barrido_i && s_barrido) { *p++ = '<'; }
        *p++ = ' ';
    }
    *p = '\0';
    s_hora.detalle = s_barrido_txt;
}

static void barrido_paso(const dcf_info_t *inf)
{
    uint32_t pct = (uint32_t)(inf->contraste * 100.0f);

    if (pct > 100U) { pct = 100U; }
    if ((uint8_t)pct > s_barrido_mejor[s_barrido_i]) { s_barrido_mejor[s_barrido_i] = (uint8_t)pct; }
    if ((uint32_t)(board_millis() - s_barrido_ms0) < BARRIDO_VISITA_S * 1000U) { return; }
    if ((uint8_t)(s_barrido_i + 1U) < (uint8_t)HORA_BARRIDO_N) {
        s_barrido_i++;
        s_barrido_ms0 = board_millis();
        s_emisora = (hora_emisora_t)s_barrido_i;
        sintoniza();
        s_hora.emisora = tr("Buscando en todas", "Scanning all of them");
        return;
    }
    {   /* the end: the one with the most contrast */
        uint8_t k, mejor = 0U;

        for (k = 1U; k < (uint8_t)HORA_BARRIDO_N; k++) {
            if (s_barrido_mejor[k] > s_barrido_mejor[mejor]) { mejor = k; }
        }
        s_barrido = 0U;
        s_emisora = (hora_emisora_t)mejor;
        sintoniza();
        console_printf("  hora: barrido hecho, %s\n", dcf77_nombre(s_emisora));
    }
}

/* ------------------------------------------------------------------ */
/* the screen */

void hora_abre(void)
{
    s_modo_antes = radio_mode_get();
    s_dial_antes = radio_dial_hz();
    s_bw_antes   = demod_am_get_audio_bw();
    s_abierta = 1U;
    s_rejilla = 0U;
    s_barrido = 0U;
    memset(&s_hora, 0, sizeof s_hora);
    s_hora.titulo  = tr("Hora por radio", "Radio clock");
    s_hora.estado  = tr("Buscando la señal", "Searching for the signal");
    s_hora.detalle = tr("puede tardar 3 o 4 minutos", "it can take 3 or 4 minutes");
    s_hora.pressed = UIH_HIT_NONE;
    huso_boton_txt();
    sintoniza();
    s_ult_ms = 0U;
    ui_hora_draw(&s_hora);
    console_printf("  menu -> Hora por radio\n");
}

void hora_suelta(void)
{
    if (!s_abierta) { return; }
    s_abierta = 0U;
    s_rejilla = 0U;
    s_barrido = 0U;
    dcf77_stop();
    if (radio_mode_get() != s_modo_antes) { radio_mode_set(s_modo_antes); }
    demod_am_set_audio_bw(s_bw_antes);
    (void)radio_dial_go(s_dial_antes);
    console_printf("  hora: fuera, de vuelta a %u Hz\n", s_dial_antes);
}

static void rejilla_rellena(void);

void hora_dibuja(void)
{
    if (!s_abierta) { return; }
    if (s_rejilla) { rejilla_rellena(); ui_grid_draw(&s_grid); }
    else           { ui_hora_draw(&s_hora); }
}

static void poll_rds_pantalla(void)
{
    rds_info_t in;
    char *p;
    uint32_t pct;

    rds_info(&in);
    {
        float n = demod_am_get_signal_peak();
        uint32_t nv = 0U;

        while (n >= 2.0f && nv < 100U) { n *= 0.5f; nv += 10U; }
        s_hora.nivel = (uint8_t)nv;
    }
    pct = (uint32_t)(in.calidad * 100.0f + 0.5f);
    if (pct > 100U) { pct = 100U; }
    s_hora.marca  = (uint8_t)pct;
    s_hora.latido = in.enganchado;
    s_hora.emisora = dcf77_nombre(HORA_RDS);
    s_hora.hora = 0; s_hora.fecha = 0; s_hora.puede_aplicar = 0U;
    s_hora.detalle = tr("gira el mando para sintonizar", "turn the knob to tune");

    if (!in.enganchado) {
        s_hora.estado = tr("Buscando el RDS", "Searching for RDS");
    } else if (!in.tiene_hora) {
        s_hora.estado = in.tiene_ps ? in.ps : tr("Leyendo el RDS", "Reading RDS");
        s_hora.detalle = tr("la hora se manda una vez por minuto",
                            "the time is sent once a minute");
    } else {
        int16_t loc = (int16_t)((int16_t)in.hora * 60 + (int16_t)in.minuto
                                + (int16_t)in.desp_medias * 30);

        while (loc < 0)     { loc = (int16_t)(loc + 1440); }
        while (loc >= 1440) { loc = (int16_t)(loc - 1440); }
        if (!s_rds_hora_ok) {
            s_hora.estado = tr("Hora recibida - sin confirmar", "Time received - unconfirmed");
            p = s_hora_det;
            p = u3(p, (uint32_t)s_rds_ct_n);
            p = pega(p, (s_rds_ct_n == 1U)
                        ? tr(" recibida - hace falta otra que cuadre",
                             " received - another matching one is needed")
                        : tr(" recibidas - hace falta que dos cuadren",
                             " received - two of them must match"));
            if (s_rds_ct_mal > 0U) {
                p = pega(p, tr(", descartadas ", ", discarded "));
                p = u3(p, (uint32_t)s_rds_ct_mal);
            }
            *p = '\0';
            s_hora.detalle = s_hora_det;
        } else {
            s_hora.estado = in.tiene_ps ? in.ps : tr("Hora confirmada", "Time confirmed");
            s_hora.detalle = tr("RDS da el minuto; el segundo, a unos segundos",
                                "RDS gives the minute; the second, within seconds");
            s_hora.puede_aplicar = 1U;
        }
        p = s_hora_txt;
        if (s_rds_hora_ok && s_anclada) {
            rtc_hw_datetime_t dt;

            rtc_hw_get(&dt);
            p = u2(p, dt.hour); *p++ = ':'; p = u2(p, dt.minute); *p++ = ':'; p = u2(p, dt.second);
        } else {
            p = u2(p, (uint32_t)(loc / 60)); *p++ = ':'; p = u2(p, (uint32_t)(loc % 60));
        }
        *p = '\0';
        s_hora.hora = s_hora_txt;
        if (in.dia != 0U && in.mes != 0U) {
            p = s_hora_fecha;
            p = u2(p, in.dia); *p++ = '/'; p = u2(p, in.mes); *p++ = '/'; p = u2(p, in.anno2);
            *p = '\0';
            s_hora.fecha = s_hora_fecha;
        }
    }
    ui_hora_draw(&s_hora);
}

void hora_poll(void)
{
    dcf_info_t inf;
    char *p;
    uint32_t marca_pct;

    if (!s_abierta) { return; }
    if (s_emisora != HORA_RDS) {
        /* The minute's edge, every pass: see the top of this file. */
        uint8_t listo;

        dcf77_info(&inf);
        listo = (uint8_t)(inf.estado == DCF_LISTO);
        if (listo) {
            if (!s_listo_ant || inf.hora != s_hh_ant || inf.minuto != s_mm_ant) {
                s_listo_ms = board_millis();
                console_printf("  hora: %s dice %02u:%02u %02u/%02u/%02u\n",
                               dcf77_nombre(s_emisora), (uint32_t)inf.hora,
                               (uint32_t)inf.minuto, (uint32_t)inf.dia,
                               (uint32_t)inf.mes, (uint32_t)inf.anno);
            }
            s_hh_ant = inf.hora; s_mm_ant = inf.minuto;
        } else {
            s_listo_ms = 0U;
        }
        s_listo_ant = listo;
    }
    if (s_rejilla) { return; }
    {
        uint8_t con_seg = (s_emisora == HORA_RDS) ? s_anclada : (uint8_t)(s_listo_ms != 0U);

        if ((uint32_t)(board_millis() - s_ult_ms) < (con_seg ? 100U : 200U)) { return; }
    }
    s_ult_ms = board_millis();
    if (s_emisora == HORA_RDS) { poll_rds_pantalla(); return; }

    {
        float n = dcf77_nivel_propio(s_emisora) ? inf.nivel : demod_am_get_signal_peak();
        uint32_t pct = 0U;

        while (n >= 2.0f && pct < 100U) { n *= 0.5f; pct += 10U; }
        s_hora.nivel = (uint8_t)pct;
    }
    marca_pct = (uint32_t)(inf.contraste * 100.0f);
    if (marca_pct > 100U) { marca_pct = 100U; }
    s_hora.marca  = (uint8_t)marca_pct;
    s_hora.latido = (uint8_t)(marca_pct >= 25U);

    if (s_barrido) {
        s_hora.estado = dcf77_nombre(s_emisora);
        s_hora.hora = 0; s_hora.fecha = 0; s_hora.puede_aplicar = 0U;
        barrido_paso(&inf);
        barrido_texto();
        if (!s_barrido) { s_hora.emisora = dcf77_nombre(s_emisora); }
        ui_hora_draw(&s_hora);
        return;
    }

    p = s_hora_det;
    switch (inf.estado) {
    case DCF_BUSCANDO: {
        uint32_t c100 = (uint32_t)(inf.contraste * 100.0f + 0.5f);

        s_hora.estado = tr("Buscando la señal", "Searching for the signal");
        p = u3(p, c100 / 100U); *p++ = ','; p = u2(p, c100 % 100U);
        *p++ = ' ';
        p = pega(p, tr("de marca", "of mark"));
        *p = '\0';
        s_hora.detalle = s_hora_det;
        break;
    }
    case DCF_LEYENDO:
    case DCF_PERDIDO:
        s_hora.estado = (inf.estado == DCF_PERDIDO)
                        ? tr("Se ha perdido - reintentando", "Lost it - retrying")
                        : tr("Leyendo la trama", "Reading the frame");
        p = u2(p, inf.bits); *p++ = '/'; p = u2(p, 59U);
        p = pega(p, tr("  marca ", "  mark "));  p = u3(p, inf.ms_marca);
        p = pega(p, tr("  ciclo ", "  cycle ")); p = u4(p, inf.ms_ciclo);
        if (inf.tramas_malas == 0U) {
            p = pega(p, tr("  raras ", "  odd "));
            p = u3(p, (inf.marcas_raras > 999U) ? 999U : inf.marcas_raras);
        } else {
            static const char *const k_mot_es[] = {
                "", "corta", "b0", "b20", "z1z2", "par-min", "par-hor",
                "par-fec", "rango", "patron", "par-anno"
            };
            static const char *const k_mot_en[] = {
                "", "short", "b0", "b20", "z1z2", "par-min", "par-hour",
                "par-date", "range", "pattern", "par-year"
            };
            _Static_assert((int)(sizeof k_mot_es / sizeof k_mot_es[0]) == (int)DCF_MOT_MSF_PAR_ANNO + 1,
                           "k_mot_es[] and DCF_MOT_* out of step");
            const char *const *k_mot = (idioma() == IDIOMA_EN) ? k_mot_en : k_mot_es;
            uint8_t mt = dcf77_motivo();

            if (mt >= (uint8_t)(sizeof k_mot_es / sizeof k_mot_es[0])) { mt = 0U; }
            p = pega(p, tr("  mal ", "  bad "));
            p = u3(p, (inf.tramas_malas > 999U) ? 999U : inf.tramas_malas);
            if (k_mot[mt][0] != '\0') { *p++ = ' '; p = pega(p, k_mot[mt]); }
        }
        *p = '\0';
        s_hora.detalle = s_hora_det;
        break;
    case DCF_CONFIRMANDO:
        s_hora.estado = tr("Una trama buena - confirmando con la siguiente",
                           "One good frame - confirming with the next one");
        s_hora.detalle = tr("un minuto mas", "one more minute");
        break;
    case DCF_LISTO:
    default:
        s_hora.estado = tr("Hora recibida y confirmada", "Time received and confirmed");
        s_hora.detalle = tr("pulsa Aplicar para ponerla", "press Apply to set it");
        break;
    }
    s_hora.hora = 0; s_hora.fecha = 0; s_hora.puede_aplicar = 0U;
    if (inf.estado == DCF_LISTO) {
        int32_t atr = atraso_emisora();

        p = s_hora_txt;
        p = u2(p, inf.hora); *p++ = ':'; p = u2(p, inf.minuto);
        if (atr >= 0 && atr < (int32_t)HORA_FRESCA_MS && s_listo_ms != 0U) {
            *p++ = ':'; p = u2(p, (uint32_t)atr / 1000U);
        }
        *p = '\0';
        s_hora.hora = s_hora_txt;
        p = s_hora_fecha;
        p = u2(p, inf.dia); *p++ = '/'; p = u2(p, inf.mes); *p++ = '/'; p = u2(p, inf.anno);
        *p = '\0';
        s_hora.fecha = s_hora_fecha;
        s_hora.puede_aplicar = 1U;
    }
    ui_hora_draw(&s_hora);
}

/* ------------------------------------------------------------------ */
/* Apply */

static void aplica(void)
{
    if (s_emisora == HORA_RDS) {
        rds_info_t in;

        rds_info(&in);
        if (!in.tiene_hora || !s_rds_hora_ok) { return; }
        hora_huso_mide((int16_t)((int16_t)in.desp_medias * 30));
        {
            uint8_t hh = in.hora, mi = in.minuto, dd = in.dia, mo = in.mes, aa = in.anno2;
            uint8_t ds = hora_dia_semana(dd, mo, aa);

            hora_suma_minutos((int16_t)((int16_t)in.desp_medias * 30), &hh, &mi, &dd, &mo, &aa, &ds);
            (void)rtc_pon_anclado((uint8_t)(in.dia != 0U && in.mes != 0U), dd, mo, aa,
                                  (int16_t)((int16_t)hh * 60 + (int16_t)mi),
                                  rds_hora_atraso_ms() + RDS_CT_RETARDO_MS,
                                  RTC_CAL_NINGUNA, 0.0f);
        }
        console_printf("  hora: RDS aplicada, huso %d min (medido)\n", (int32_t)hora_huso());
        return;
    }
    {
        dcf_info_t inf;
        int32_t atr;
        int16_t mins;

        dcf77_info(&inf);
        if (inf.estado != DCF_LISTO || s_listo_ms == 0U) { return; }
        atr = atraso_emisora();
        if (atr >= (int32_t)HORA_FRESCA_MS) { return; }
        mins = (int16_t)((int16_t)inf.hora * 60 + (int16_t)inf.minuto);
        if (atr < 0) { mins = (int16_t)(mins - 1); atr += 60000; }   /* not begun yet */
        (void)rtc_pon_anclado(1U, inf.dia, inf.mes, inf.anno, mins, (uint32_t)atr,
                              CAL_EMISORA_MS, 0.0f);
        console_printf("  hora: %s aplicada, %02u:%02u %02u/%02u/%02u + %d ms"
                       " (latencia del decodificador %d ms)\n",
                       dcf77_nombre(s_emisora), (uint32_t)inf.hora, (uint32_t)inf.minuto,
                       (uint32_t)inf.dia, (uint32_t)inf.mes, (uint32_t)inf.anno, atr,
                       (int32_t)k_latencia_ms[s_emisora]);
    }
}

/* ------------------------------------------------------------------ */
/* the zone grid (upstream's GRID_HUSO) */

static uint16_t rejilla_n(void) { return (uint16_t)(1U + husos_n()); }

static void rejilla_ordena(void)
{
    /* By how far each city's offset today is from the radio's: see the
     * top of this file. A stable insertion sort of fifty. */
    uint8_t dd, mm, aa, hh, i, j, n = husos_n();
    int16_t d[64];

    huso_utc_ahora(&dd, &mm, &aa, &hh);
    for (i = 0U; i < n; i++) {
        int16_t x = (int16_t)(husos_ahora(i, dd, mm, aa, hh) - hora_huso());

        d[i] = (int16_t)((x < 0) ? -x : x);
        s_huso_orden[i] = i;
    }
    for (i = 1U; i < n; i++) {
        uint8_t k = s_huso_orden[i];

        for (j = i; j > 0U && d[s_huso_orden[j - 1U]] > d[k]; j--) {
            s_huso_orden[j] = s_huso_orden[j - 1U];
        }
        s_huso_orden[j] = k;
    }
}

static void rejilla_rellena(void)
{
    uint16_t primero = (uint16_t)(s_grid.pagina * UIG_CELLS);
    uint16_t total = rejilla_n();
    uint8_t i;
    char t[12];
    char *p;

    s_grid.paginas = (uint8_t)((total + UIG_CELLS - 1U) / UIG_CELLS);
    s_grid.n = (uint8_t)((total - primero > UIG_CELLS) ? UIG_CELLS : (total - primero));
    s_grid.nav[0] = tr("Anterior", "Previous");
    s_grid.nav[1] = tr("Siguiente", "Next");
    s_grid.nav_on[0] = (uint8_t)(s_grid.pagina > 0U);
    s_grid.nav_on[1] = (uint8_t)(s_grid.pagina + 1U < s_grid.paginas);
    s_grid.marcada = 0xFFU;

    huso_txt(hora_huso(), t);
    p = s_huso_titulo;
    p = pega(p, tr("Huso", "Time zone")); *p++ = ' '; p = pega(p, t);
    p = pega(p, " - ");
    p = pega(p, hora_huso_medido() ? tr("lo dijo una emisora", "a station said so")
                                   : tr("elige tu ciudad", "pick your city"));
    *p = '\0';
    s_grid.titulo = s_huso_titulo;

    for (i = 0U; i < s_grid.n; i++) {
        uint16_t id = (uint16_t)(primero + i);

        memset(s_huso_cel[i], 0, sizeof s_huso_cel[i]);
        if (id == 0U) {
            (void)pega(s_huso_cel[i][0], tr("Medir", "Measure"));
            if (s_huso_medida_hay) {
                huso_txt(s_huso_medida, s_huso_cel[i][1]);
                (void)pega(s_huso_cel[i][2], tr("de la emisora", "from the station"));
            } else {
                (void)pega(s_huso_cel[i][1], tr("no hay", "none"));
                (void)pega(s_huso_cel[i][2], tr("usa WWV/BPM/MSF", "use WWV/BPM/MSF"));
            }
        } else {
            uint8_t k = s_huso_orden[id - 1U];
            const huso_ciudad_t *c = husos_fila(k);
            uint8_t dd, mm, aa, hh;

            huso_utc_ahora(&dd, &mm, &aa, &hh);
            (void)pega(s_huso_cel[i][0], c->nombre);
            huso_txt(c->base, s_huso_cel[i][1]);
            huso_txt(husos_ahora(k, dd, mm, aa, hh), t);
            p = pega(s_huso_cel[i][2], tr("ahora ", "now "));
            (void)pega(p, t);
        }
        s_grid.cel[i].l1 = s_huso_cel[i][0];
        s_grid.cel[i].l2 = s_huso_cel[i][1];
        s_grid.cel[i].l3 = s_huso_cel[i][2];
    }
}

static void rejilla_abre(void)
{
    s_huso_medida_hay = (s_emisora != HORA_RDS) ? huso_mide(&s_huso_medida) : 0U;
    rejilla_ordena();
    memset(&s_grid, 0, sizeof s_grid);
    s_grid.pressed = -1;
    s_grid.cursor  = -1;
    s_rejilla = 1U;
    rejilla_rellena();
    ui_grid_draw(&s_grid);
}

static void rejilla_elige(int8_t cel)
{
    uint16_t id = (uint16_t)(s_grid.pagina * UIG_CELLS + (uint16_t)cel);

    if (id == 0U) {
        if (s_huso_medida_hay) { hora_huso_mide(s_huso_medida); }
    } else if (id < rejilla_n()) {
        uint8_t dd, mm, aa, hh, k = s_huso_orden[id - 1U];

        huso_utc_ahora(&dd, &mm, &aa, &hh);
        hora_huso_pon(husos_ahora(k, dd, mm, aa, hh));   /* a measure wins: upstream's */
    }
    console_printf("  hora: huso %d min%s\n", (int32_t)hora_huso(),
                   hora_huso_medido() ? " (medido)" : "");
    s_rejilla = 0U;
    huso_boton_txt();
    ui_hora_draw(&s_hora);
}

/* ------------------------------------------------------------------ */
/* touches and the knob */

void hora_toque(uint16_t x, uint16_t y, uint8_t pulsado)
{
    if (!s_abierta) { return; }

    if (s_rejilla) {
        if (pulsado) {
            int8_t h = ui_grid_hit(x, y);

            if (h < 0) { return; }
            if (h < UIG_CELLS && h >= (int8_t)s_grid.n) { return; }
            if (h == UIG_HIT_PREV && !s_grid.nav_on[0]) { return; }
            if (h == UIG_HIT_NEXT && !s_grid.nav_on[1]) { return; }
            s_grid_press = h;
            if (h < UIG_CELLS) { s_grid.pressed = h; ui_grid_draw_one(&s_grid, h); }
            return;
        }
        if (s_grid_press < 0) { return; }
        {
            int8_t h = s_grid_press;

            s_grid_press = -1;
            s_grid.pressed = -1;
            if (h == UIG_HIT_PREV || h == UIG_HIT_NEXT) {
                s_grid.pagina = (uint8_t)(s_grid.pagina + ((h == UIG_HIT_NEXT) ? 1 : -1));
                s_grid.cursor = -1;
                rejilla_rellena();
                ui_grid_draw(&s_grid);
                return;
            }
            rejilla_elige(h);
        }
        return;
    }

    if (pulsado) {
        if (s_hora.pressed < 0) {
            int8_t h = ui_hora_hit(x, y);

            if (h == UIH_HIT_APLICAR && !s_hora.puede_aplicar) { h = UIH_HIT_NONE; }
            if (h != UIH_HIT_NONE) { s_hora.pressed = h; ui_hora_draw(&s_hora); }
        }
        return;
    }
    if (s_hora.pressed < 0) { return; }
    {
        int8_t k = s_hora.pressed;

        s_hora.pressed = UIH_HIT_NONE;
        ui_hora_draw(&s_hora);
        if (k == UIH_HIT_EMISORA) {
            /* upstream's cycle: every station, then the sweep, then DCF77 */
            if (s_barrido) {
                s_barrido = 0U;
                s_emisora = HORA_DCF77;
            } else if ((unsigned)s_emisora + 1U >= (unsigned)HORA_N) {
                barrido_arranca();
            } else {
                s_emisora = (hora_emisora_t)(s_emisora + 1U);
            }
            if (!s_barrido) { sintoniza(); }
            ui_hora_draw(&s_hora);
        } else if (k == UIH_HIT_HUSO) {
            rejilla_abre();
        } else if (k == UIH_HIT_SALIR) {
            menu_close();
        } else if (k == UIH_HIT_APLICAR) {
            aplica();
            menu_close();
        }
    }
}

void hora_knob(int32_t d)
{
    if (!s_abierta || d == 0) { return; }
    if (s_rejilla) {
        int8_t was = s_grid.cursor, now;

        now = (int8_t)((was < 0) ? 0 : was + d);
        if (now < 0) { now = 0; }
        if (now >= (int8_t)s_grid.n) { now = (int8_t)(s_grid.n - 1U); }
        s_grid.cursor = now;
        if (was >= 0 && was != now) { ui_grid_draw_one(&s_grid, was); }
        ui_grid_draw_one(&s_grid, now);
        return;
    }
    if (s_emisora == HORA_RDS) {
        uint32_t f = radio_dial_hz();

        f = (uint32_t)((int32_t)((f + FM_PASO_HZ / 2U) / FM_PASO_HZ * FM_PASO_HZ)
                       + d * (int32_t)FM_PASO_HZ);
        if (f < 87500000UL) { f = 87500000UL; }
        if (f > 108000000UL) { f = 108000000UL; }
        (void)radio_dial_go(f);
        hora_rds_reinicia();
    }
}

void hora_knob_press(void)
{
    if (!s_abierta) { return; }
    if (s_rejilla && s_grid.cursor >= 0) { rejilla_elige(s_grid.cursor); }
}

/* ------------------------------------------------------------------ */
/* RDS's clock: upstream's rds_poll(), from the main loop */

void hora_rds_reinicia(void)
{
    s_rds_hora_ok = 0U;
    s_rds_cand_vale = 0U;
    s_rds_seq_ant = 0UL;
    s_rds_ct_n = 0U;
    s_rds_ct_mal = 0U;
}

static uint8_t rds_ancla(const rds_info_t *in, int16_t mins_utc)
{
    uint8_t puesto;

    uint8_t hh = (uint8_t)(mins_utc / 60), mi = (uint8_t)(mins_utc % 60);
    uint8_t dd = in->dia, mo = in->mes, aa = in->anno2, ds;
    uint8_t con_fecha = (uint8_t)(dd != 0U && mo != 0U);

    if (con_fecha) {
        ds = hora_dia_semana(dd, mo, aa);
        hora_suma_minutos((int16_t)((int16_t)in->desp_medias * 30), &hh, &mi, &dd, &mo, &aa, &ds);
    } else {
        int16_t m = (int16_t)(mins_utc + (int16_t)in->desp_medias * 30);

        while (m < 0)     { m = (int16_t)(m + 1440); }
        while (m >= 1440) { m = (int16_t)(m - 1440); }
        hh = (uint8_t)(m / 60); mi = (uint8_t)(m % 60);
    }
    puesto = rtc_pon_anclado(con_fecha, dd, mo, aa, (int16_t)((int16_t)hh * 60 + (int16_t)mi),
                             rds_hora_atraso_ms() + RDS_CT_RETARDO_MS,
                             RTC_CAL_NINGUNA, RDS_UMBRAL_S);
    s_rds_hora_ok = 1U;
    s_rds_cand_vale = 0U;
    return puesto;
}

void hora_rds_poll(void)
{
    rds_info_t in;
    int16_t mins;

    if (!rds_activo() || demod_am_get_mode() != DEMOD_MODE_WFM) { return; }
    rds_info(&in);
    if (!in.tiene_hora || in.hora_seq == s_rds_seq_ant) { return; }
    s_rds_seq_ant = in.hora_seq;
    mins = (int16_t)((int16_t)in.hora * 60 + (int16_t)in.minuto);   /* UTC */
    s_rds_ct_n++;
    if (in.hora_limpia) {
        if (rds_ancla(&in, mins)) {
            console_printf("  rds: reloj puesto en hora (grupo limpio), %02u:%02u UTC, %d half hours\n",
                           (uint32_t)in.hora, (uint32_t)in.minuto, (int32_t)in.desp_medias);
        }
        return;
    }
    if (s_rds_cand_vale) {
        int32_t pasados = (int32_t)((board_millis() - s_rds_cand_ms + 30000UL) / 60000UL);
        int32_t esperado = ((int32_t)s_rds_cand_mins + pasados) % 1440;
        int32_t d = (int32_t)mins - esperado;

        while (d < -720) { d += 1440; }
        while (d >  720) { d -= 1440; }
        if (d >= -1 && d <= 1) {
            if (rds_ancla(&in, mins)) {
                console_printf("  rds: reloj puesto en hora (dos horas coherentes), %02u:%02u UTC\n",
                               (uint32_t)in.hora, (uint32_t)in.minuto);
            }
            return;
        }
        s_rds_ct_mal++;
        console_printf("  rds: la segunda hora no cuadra, se vuelve a empezar\n");
    }
    s_rds_cand_mins = (uint16_t)mins;
    s_rds_cand_ms = board_millis();
    s_rds_cand_vale = 1U;
}
