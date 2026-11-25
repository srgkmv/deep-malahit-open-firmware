/*
 * THE CLOCK - step 56, 24/11/2026.
 *
 * Upstream's contract (../../User/rtc_hw.h), on this board's STM32H743:
 * the RTC on the 32.768 kHz crystal X2 (PC14/PC15, C71/C72 = 10 pF,
 * BOARD.md), in the backup domain, which CPU_VBAT keeps alive from the
 * battery while the radio is off (the owner, 24/11/2026). Upstream's
 * rtc_hw.c does the same on the GD32 through GigaDevice's driver; this
 * one does it on the registers, and keeps every decision of upstream's:
 *
 *   - a calendar that is already running is NOT touched at start: the
 *     INITS flag (the GD32's YCM) says "the year was programmed", and only
 *     a loss of the backup domain clears it. A reset, a new firmware, an
 *     SWD write keep the time;
 *   - a cold calendar is seeded with 2026-01-01 00:00:00, a date that
 *     looks wrong on purpose;
 *   - "ever set" is a magic in backup register 0 and the epoch of the
 *     last setting in register 1 - upstream's same two, with its magic;
 *   - an impossible date is refused and nothing is written
 *     (rtc_cal_valida(), upstream's rtc_cal.c, compiled as it is);
 *   - prescalers 128 x 256: whole seconds, and 1/256 s for the fine shift.
 *
 * ONE DIFFERENCE, AND WHY: the crystal is not waited for. Upstream spins
 * about a second in rtc_hw_init() for its LXTAL to settle; a 32 kHz
 * crystal can take up to two, and one that never starts would add that
 * to every boot of this radio (2.3 s now). So rtc_hw_init() switches it
 * on and returns, and rtc_hw_poll(), from the main loop, finishes when
 * LSERDY comes - or, five seconds on, gives up and says so. Until then
 * rtc_hw_lxtal_failed() is true, which is what upstream's callers already
 * handle: "this clock is not to be believed". After the first start the
 * crystal keeps running on VBAT and the RTC is ready at once.
 *
 * The time it keeps is upstream's: LOCAL (the keypad sets local hh:mm;
 * UTC is that minus the zone, upstream's huso_utc_ahora()).
 *
 * AND THIS PORT'S: THE CLOCK LEARNS ITS CRYSTAL (rtc_hw_pon_anclado()).
 * Measured on this radio, 24-25/11/2026: set on the minute at 20:16, the
 * next evening 22.9 hours later it was 3.96 s fast - +50 ppm, 4.3 s a
 * day; the crystal's loads, C71/C72 = 10 pF, are not its own. FT8 wants
 * the second to half a second, so that is a resync every few hours. The
 * H743's RTC has a smooth calibration (CALR: -487..+488 ppm in 0.954
 * ppm steps), which lives in the backup domain with the calendar. So
 * every setting that knows the second - a station, RDS, the keypad's "On
 * the minute" - first reads how far the RTC has walked since the last
 * such setting, and if that was long enough ago for the two settings'
 * own errors to be under ~5 ppm, moves CALR by what it walked. Kept in
 * backup registers 2 (the last anchor's epoch, in the RTC's own count)
 * and 3 (how good that anchor was, ms). A walk over 2 minutes is not a
 * crystal - it is a zone changed, or a wrong time - and teaches nothing.
 */
#include <string.h>
#include "stm32h7xx.h"
#include "rtc_hw.h"     /* ../../User - the contract */
#include "rtc_cal.h"    /* ../../User - the calendar arithmetic */
#include "board.h"      /* board_millis() */
#include "console.h"
#include "rtc_hw_port.h"

#define RTC_ASYN_PRE   127U      /* 32768 / 128 = 256 Hz */
#define RTC_SYN_PRE    255U      /* 256 / 256 = 1 Hz; a shift step 1/256 s */
#define PASO_US        3906U     /* microseconds per shift step */

#define SEMILLA_YEAR   2026U
#define SEMILLA_MONTH  1U
#define SEMILLA_DAY    1U

#define BKP_MAGIA      0x52544331UL     /* "RTC1", upstream's */

#define LSE_ESPERA_MS  5000U     /* the crystal's chance, from rtc_hw_init() */

#define CAL_PPM_PASO   0.95367f  /* CALM's step: 1 / 2^20 */
#define CAL_S_POR_MS   200U      /* elapsed s needed per ms of anchor error: 5 ppm */
#define CAL_ERR_MAX_S  120.0f    /* a walk over this is not the crystal */
#define CAL_PPM_MAX    300.0f

static uint8_t  s_lxtal_mal = 1U;    /* until shown otherwise */
static uint8_t  s_arrancado;
static uint8_t  s_pendiente;         /* the crystal was started, not yet ready */
static uint32_t s_lse_desde;
static uint8_t  s_sucio;
static uint32_t s_seq;
static int32_t  s_shift_us;
static uint8_t  s_frio;              /* this start seeded the calendar */

/* --------------------------------------------------------------- */

static void respaldo_abre(void)
{
    PWR->CR1 |= PWR_CR1_DBP;
    while ((PWR->CR1 & PWR_CR1_DBP) == 0U) { }
}

static void wpr_abre(void)   { RTC->WPR = 0xCAU; RTC->WPR = 0x53U; }
static void wpr_cierra(void) { RTC->WPR = 0xFFU; }

/* The shadow registers: after a reset, a wake or leaving init mode, RSF
 * has to be seen again before TR/DR mean anything. Two RTCCLK periods
 * (61 us); the bound is for a dead crystal, not for a slow one. */
static void sincroniza(void)
{
    uint32_t t0;

    wpr_abre();
    RTC->ISR &= ~RTC_ISR_RSF;
    wpr_cierra();
    t0 = board_millis();
    while ((RTC->ISR & RTC_ISR_RSF) == 0U) {
        if ((board_millis() - t0) > 10U) { break; }
    }
}

static uint32_t epoca_ahora(void)
{
    rtc_hw_datetime_t d;

    rtc_hw_get(&d);
    return rtc_cal_epoca(d.year, d.month, d.day, d.hour, d.minute, d.second);
}

static void marca_sincronizado(void)
{
    uint32_t e = epoca_ahora();

    if (e == 0UL) { return; }
    respaldo_abre();
    RTC->BKP0R = BKP_MAGIA;
    RTC->BKP1R = e;
}

/* The only place that programs the calendar, as upstream's. */
static uint8_t calendario_pon(uint16_t year, uint8_t month, uint8_t day,
                              uint8_t hour, uint8_t minute, uint8_t second)
{
    uint32_t t0, tr, dr;

    if (!rtc_cal_valida(year, month, day, hour, minute, second)) { return 0U; }
    if (second > 59U) { second = 59U; }     /* the leap second: upstream's */

    tr = ((uint32_t)rtc_cal_dec_a_bcd(hour)   << RTC_TR_HU_Pos)
       | ((uint32_t)rtc_cal_dec_a_bcd(minute) << RTC_TR_MNU_Pos)
       | ((uint32_t)rtc_cal_dec_a_bcd(second) << RTC_TR_SU_Pos);
    dr = ((uint32_t)rtc_cal_dec_a_bcd((uint8_t)(year - 2000U)) << RTC_DR_YU_Pos)
       | ((uint32_t)rtc_cal_dia_semana(year, month, day)       << RTC_DR_WDU_Pos)
       | ((uint32_t)rtc_cal_dec_a_bcd(month)                   << RTC_DR_MU_Pos)
       | ((uint32_t)rtc_cal_dec_a_bcd(day)                     << RTC_DR_DU_Pos);

    respaldo_abre();
    wpr_abre();
    RTC->ISR |= RTC_ISR_INIT;
    t0 = board_millis();
    while ((RTC->ISR & RTC_ISR_INITF) == 0U) {
        if ((board_millis() - t0) > 10U) {
            RTC->ISR &= ~RTC_ISR_INIT;
            wpr_cierra();
            return 0U;
        }
    }
    /* RM0433: the two prescalers in two writes, synchronous first. */
    RTC->PRER = RTC_SYN_PRE;
    RTC->PRER = RTC_SYN_PRE | (RTC_ASYN_PRE << RTC_PRER_PREDIV_A_Pos);
    RTC->TR = tr;
    RTC->DR = dr;
    RTC->CR &= ~RTC_CR_FMT;                  /* 24 hours */
    RTC->ISR &= ~RTC_ISR_INIT;
    wpr_cierra();
    sincroniza();
    return 1U;
}

/* LSE ready: the RTC on it, and the calendar kept or seeded. */
static void termina(void)
{
    uint32_t bdcr = RCC->BDCR;

    s_pendiente = 0U;
    s_lxtal_mal = 0U;
    respaldo_abre();
    /* RTCSEL can only be written once after a backup-domain reset. If
     * something else (LSI, HSE) holds it, only BDRST frees it - and that
     * also clears the calendar and the registers, which were not ours. */
    if ((bdcr & RCC_BDCR_RTCSEL) != (1UL << RCC_BDCR_RTCSEL_Pos)) {
        if ((bdcr & RCC_BDCR_RTCSEL) != 0U) {
            uint32_t lse = bdcr & (RCC_BDCR_LSEON | RCC_BDCR_LSEDRV);
            uint32_t t0;

            RCC->BDCR |= RCC_BDCR_BDRST;
            RCC->BDCR &= ~RCC_BDCR_BDRST;
            RCC->BDCR = lse;
            t0 = board_millis();
            while ((RCC->BDCR & RCC_BDCR_LSERDY) == 0U) {
                if ((board_millis() - t0) > LSE_ESPERA_MS) { s_lxtal_mal = 1U; return; }
            }
        }
        RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) | (1UL << RCC_BDCR_RTCSEL_Pos);
    }
    RCC->BDCR |= RCC_BDCR_RTCEN;
    RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;
    (void)RCC->APB4ENR;

    if ((RTC->ISR & RTC_ISR_INITS) != 0U) {
        sincroniza();
        s_arrancado = 1U;
        return;
    }
    s_frio = 1U;
    s_arrancado = calendario_pon(SEMILLA_YEAR, SEMILLA_MONTH, SEMILLA_DAY, 0U, 0U, 0U);
}

void rtc_hw_init(void)
{
    respaldo_abre();
    if ((RCC->BDCR & RCC_BDCR_LSERDY) != 0U) {   /* kept by VBAT: at once */
        termina();
        return;
    }
    /*
     * A cold crystal. Medium-high drive (LSEDRV = 01) - it can only be
     * set with the oscillator off, which is now. The lowest drive is the
     * datasheet's default for the smallest current; with 10 pF loads and
     * an unknown crystal, starting surely is worth the half microamp.
     */
    RCC->BDCR = (RCC->BDCR & ~(RCC_BDCR_LSEDRV | RCC_BDCR_LSEBYP))
              | (1UL << RCC_BDCR_LSEDRV_Pos);
    RCC->BDCR |= RCC_BDCR_LSEON;
    s_pendiente = 1U;
    s_lse_desde = board_millis();
}

void rtc_hw_poll(void)
{
    if (!s_pendiente) { return; }
    if ((RCC->BDCR & RCC_BDCR_LSERDY) != 0U) {
        termina();
        console_printf("  clock: the 32.768 kHz crystal started after %u ms%s\n",
                       board_millis() - s_lse_desde,
                       s_frio ? ", calendar seeded 2026-01-01 00:00" : "");
        return;
    }
    if ((board_millis() - s_lse_desde) > LSE_ESPERA_MS) {
        s_pendiente = 0U;
        console_printf("  *** clock: the 32.768 kHz crystal did not start in %u ms -"
                       " the clock counts uptime ***\n", (uint32_t)LSE_ESPERA_MS);
    }
}

bool rtc_hw_lxtal_failed(void)
{
    return (s_lxtal_mal != 0U);
}

uint8_t rtc_hw_frio(void)
{
    return s_frio;
}

void rtc_hw_get(rtc_hw_datetime_t *out)
{
    uint32_t tr, dr;

    if (out == 0) { return; }
    if (!s_arrancado) {
        out->year = SEMILLA_YEAR; out->month = SEMILLA_MONTH;
        out->day  = SEMILLA_DAY;  out->hour  = 0U;
        out->minute = 0U;         out->second = 0U;
        return;
    }
    /* TR first: reading it freezes DR's shadow until DR is read. */
    tr = RTC->TR;
    dr = RTC->DR;
    out->year   = (uint16_t)(2000U + rtc_cal_bcd_a_dec((uint8_t)((dr >> RTC_DR_YU_Pos) & 0xFFU)));
    out->month  = rtc_cal_bcd_a_dec((uint8_t)((dr >> RTC_DR_MU_Pos) & 0x1FU));
    out->day    = rtc_cal_bcd_a_dec((uint8_t)((dr >> RTC_DR_DU_Pos) & 0x3FU));
    out->hour   = rtc_cal_bcd_a_dec((uint8_t)((tr >> RTC_TR_HU_Pos) & 0x3FU));
    out->minute = rtc_cal_bcd_a_dec((uint8_t)((tr >> RTC_TR_MNU_Pos) & 0x7FU));
    out->second = rtc_cal_bcd_a_dec((uint8_t)((tr >> RTC_TR_SU_Pos) & 0x7FU));

    /* An impossible calendar is not served as a good one: upstream's. */
    if (!rtc_cal_valida(out->year, out->month, out->day,
                        out->hour, out->minute, out->second)) {
        out->year = SEMILLA_YEAR; out->month = SEMILLA_MONTH;
        out->day  = SEMILLA_DAY;  out->hour  = 0U;
        out->minute = 0U;         out->second = 0U;
    }
}

void rtc_hw_set(const rtc_hw_datetime_t *dt)
{
    if (dt == 0) { return; }
    if (s_lxtal_mal) { return; }
    if (!calendario_pon(dt->year, dt->month, dt->day,
                        dt->hour, dt->minute, dt->second)) {
        return;
    }
    s_arrancado = 1U;
    s_sucio     = 1U;
    s_seq++;
    marca_sincronizado();
    RTC->BKP2R = 0U;     /* set without its second: the anchor chain breaks
                          * (rtc_hw_pon_anclado() puts it back) */
}

bool rtc_hw_apply_shift(bool add_one_second, uint16_t subsecond_fraction)
{
    int32_t us;

    if (!s_arrancado) { return false; }
    if (subsecond_fraction > RTC_SYN_PRE) { return false; }
    if ((RTC->ISR & RTC_ISR_SHPF) != 0U) { return false; }   /* one still running */

    respaldo_abre();
    wpr_abre();
    RTC->SHIFTR = (add_one_second ? RTC_SHIFTR_ADD1S : 0U)
                | ((uint32_t)subsecond_fraction & RTC_SHIFTR_SUBFS);
    wpr_cierra();

    us  = add_one_second ? 1000000L : 0L;
    us -= (int32_t)subsecond_fraction * (int32_t)PASO_US;
    s_shift_us += us;
    marca_sincronizado();
    return true;
}

bool rtc_hw_consume_dirty(void)
{
    if (s_sucio) { s_sucio = 0U; return true; }
    return false;
}

uint32_t rtc_hw_get_set_seq(void)
{
    return s_seq;
}

int32_t rtc_hw_get_cumulative_shift_ms(void)
{
    return s_shift_us / 1000L;
}

bool rtc_hw_has_ever_synced(void)
{
    if (s_lxtal_mal) { return false; }
    return (RTC->BKP0R == BKP_MAGIA);
}

/* ------------------------------------------------------------------ */
/* the smooth calibration */

float rtc_hw_cal_ppm(void)
{
    uint32_t c;

    if (s_lxtal_mal) { return 0.0f; }
    c = RTC->CALR;
    return (float)((int32_t)(((c & RTC_CALR_CALP) != 0U) ? 512 : 0)
                   - (int32_t)(c & RTC_CALR_CALM)) * CAL_PPM_PASO;
}

static void cal_pon(float ppm)
{
    uint32_t calp = 0U, calm, t0;
    int32_t  k;

    if (ppm >  CAL_PPM_MAX) { ppm =  CAL_PPM_MAX; }
    if (ppm < -CAL_PPM_MAX) { ppm = -CAL_PPM_MAX; }
    k = (int32_t)((ppm >= 0.0f) ? (ppm / CAL_PPM_PASO + 0.5f) : (ppm / CAL_PPM_PASO - 0.5f));
    if (k > 0) { calp = 1U; calm = (uint32_t)(512 - k); }
    else       { calm = (uint32_t)(-k); }
    respaldo_abre();
    t0 = board_millis();
    while ((RTC->ISR & RTC_ISR_RECALPF) != 0U) {
        if ((board_millis() - t0) > 1100U) { return; }
    }
    wpr_abre();
    RTC->CALR = (calp ? RTC_CALR_CALP : 0U) | (calm & RTC_CALR_CALM);
    wpr_cierra();
}

/* The RTC now, as seconds of the day with the fraction (SSR counts down
 * from 255 within each second). */
static float segundos_del_dia(uint32_t *epoca)
{
    uint32_t ssr, tr, dr;
    uint8_t  h, m, sg;

    ssr = RTC->SSR;
    tr  = RTC->TR;
    dr  = RTC->DR;
    h  = rtc_cal_bcd_a_dec((uint8_t)((tr >> RTC_TR_HU_Pos) & 0x3FU));
    m  = rtc_cal_bcd_a_dec((uint8_t)((tr >> RTC_TR_MNU_Pos) & 0x7FU));
    sg = rtc_cal_bcd_a_dec((uint8_t)((tr >> RTC_TR_SU_Pos) & 0x7FU));
    if (epoca) {
        *epoca = rtc_cal_epoca((uint16_t)(2000U + rtc_cal_bcd_a_dec((uint8_t)((dr >> RTC_DR_YU_Pos) & 0xFFU))),
                               rtc_cal_bcd_a_dec((uint8_t)((dr >> RTC_DR_MU_Pos) & 0x1FU)),
                               rtc_cal_bcd_a_dec((uint8_t)((dr >> RTC_DR_DU_Pos) & 0x3FU)),
                               h, m, sg);
    }
    return (float)((uint32_t)h * 3600U + (uint32_t)m * 60U + sg)
         + (float)(RTC_SYN_PRE - (ssr & 0xFFFFU)) / (float)(RTC_SYN_PRE + 1U);
}

/*
 * Set the RTC to dt + frac_ms, "now" - from something that knows the
 * second to within calidad_ms. Learns the crystal first (see the top).
 */
/* The RTC minus dt + frac_ms, seconds, the day wrapped to +-12 h. */
float rtc_hw_desvio_s(const rtc_hw_datetime_t *dt, uint16_t frac_ms)
{
    float e;

    if (dt == 0 || s_lxtal_mal || !s_arrancado) { return 0.0f; }
    e = segundos_del_dia(0)
      - ((float)((uint32_t)dt->hour * 3600U + (uint32_t)dt->minute * 60U + dt->second)
         + (float)frac_ms / 1000.0f);
    if (e >  43200.0f) { e -= 86400.0f; }
    if (e < -43200.0f) { e += 86400.0f; }
    return e;
}

void rtc_hw_pon_anclado(const rtc_hw_datetime_t *dt, uint16_t frac_ms, uint16_t calidad_ms)
{
    uint32_t ep_rtc = 0U, seq;
    float    e;
    uint16_t sub;

    if (dt == 0 || s_lxtal_mal || !s_arrancado) { return; }
    (void)segundos_del_dia(&ep_rtc);
    e = rtc_hw_desvio_s(dt, frac_ms);            /* + = the RTC is ahead */

    if (calidad_ms != RTC_CAL_NINGUNA
        && RTC->BKP0R == BKP_MAGIA && RTC->BKP2R != 0U && ep_rtc > RTC->BKP2R) {
        uint32_t dt_s = ep_rtc - RTC->BKP2R;
        uint32_t hace_falta = ((RTC->BKP3R & 0xFFFFU) + calidad_ms) * CAL_S_POR_MS;
        float    ppm = e / (float)dt_s * 1.0e6f;

        if (dt_s >= hace_falta && (e < CAL_ERR_MAX_S && e > -CAL_ERR_MAX_S)
            && ppm < CAL_PPM_MAX && ppm > -CAL_PPM_MAX) {
            float antes = rtc_hw_cal_ppm();

            cal_pon(antes - ppm);
            console_printf("  clock: %d ms off after %u s - %d.%u ppm; calibration %d -> %d ppm\n",
                           (int32_t)(e * 1000.0f), dt_s,
                           (int32_t)ppm, (uint32_t)((ppm < 0.0f ? -ppm : ppm) * 10.0f) % 10U,
                           (int32_t)antes, (int32_t)rtc_hw_cal_ppm());
        } else {
            console_printf("  clock: %d ms off after %u s - not learnt (needs %u s, under %u s off)\n",
                           (int32_t)(e * 1000.0f), dt_s, hace_falta, (uint32_t)CAL_ERR_MAX_S);
        }
    }

    seq = s_seq;
    rtc_hw_set(dt);
    if (s_seq == seq) { return; }                /* refused */
    sub = (uint16_t)(((uint32_t)frac_ms * 256U + 500U) / 1000U);
    if (sub > 0U && sub < 256U) { (void)rtc_hw_apply_shift(true, (uint16_t)(256U - sub)); }
    respaldo_abre();
    /* A source not good to the second sets the time but anchors nothing:
     * the next learning starts from the next good one. */
    RTC->BKP2R = (calidad_ms == RTC_CAL_NINGUNA) ? 0U : epoca_ahora();
    RTC->BKP3R = calidad_ms;
}

uint32_t rtc_hw_get_seconds_since_sync(void)
{
    uint32_t ahora, antes;

    if (!rtc_hw_has_ever_synced()) { return 0UL; }
    ahora = epoca_ahora();
    antes = RTC->BKP1R;
    if (ahora < antes) { return 0UL; }       /* backwards: upstream's */
    return ahora - antes;
}
