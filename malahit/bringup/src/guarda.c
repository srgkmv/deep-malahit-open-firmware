#include <string.h>

#include "board.h"
#include "console.h"
#include "guarda.h"
#include "grab.h"           /* g_grab: the screenshot's strip buffer */
#include "ili9488.h"        /* ili9488_read_gram() */
#include "menus.h"          /* radio_dial_hz(), radio_repaint_all() */
#include "usbdisco.h"

/* ../../User - upstream's */
#include "spi_flash.h"
#include "bmp.h"
#include "sstv.h"
#include "wefax.h"
#include "wfx_guarda.h"
#include "rtc_hw.h"
#include "idioma.h"
#include "bip.h"
#include "gfx2.h"
#include "palette.h"

extern const gfx2_font_t font_ui_14b;

/*
 * Upstream's main.c: GSV_BLOQUE 553, the gsv_* family 29700-30610 and
 * 31340-31520, its SSTV and WEFAX callers 32600-33010, the screenshot's
 * frame 30608-31340. What upstream's comments explain is not repeated;
 * what differs here is said where it does.
 */
#define GSV_BLOQUE        4096U
#define GSV_TROZO         32U
#define GSV_ESPERA_MS     10000UL       /* an SSTV picture closed on silence */
#define WFX_ALTO_MAX      1536U
#define WFX_TRAMO_BLOQUES 16U           /* a chart grows 64 kB at a time */
#define WFX_CERROJO_MS    25000UL       /* no new chart for 25 s after one */

#define CAPT_ANCHO        480U
#define CAPT_ALTO         320U
#define CAPT_FILAS        4U            /* rows read a pass: ~one block */
#define AVISO_MS          3000U

enum { SSTV_GUARDA_NO = 0, SSTV_GUARDA_24, SSTV_GUARDA_16, SSTV_GUARDA_N };

static uint8_t  s_on;               /* a file is open */
static uint8_t  s_bits;
static uint8_t  s_buf[GSV_BLOQUE];
static uint16_t s_n;                /* bytes in s_buf */
static uint16_t s_fila;             /* the next row to be written */
static char     s_n8[8];            /* "SSTV001 " */
static uint8_t  s_bloques;          /* blocks written this pass */
static uint32_t s_addr;             /* where the file starts on the chip */
static uint16_t s_blk, s_blk_n;     /* the block to write; how many reserved */
static uint32_t s_ms;               /* when the last row came */
static uint8_t  s_limpio;           /* the reserved space already erased */
static uint8_t  s_trozos;           /* the file grows (a chart) */
static uint32_t s_ext_addr;
static uint16_t s_ext_n, s_ext_i;
static uint16_t s_ancho, s_alto;
static char     s_pre[4];
static spi_anade_r_t s_porque;
static uint8_t  s_fallo;

static uint8_t  s_sstv_guarda;      /* SSTV_GUARDA_* */
static uint8_t  s_wfx_guarda;       /* 0 / 1 */
static uint8_t  s_wfx_hubo;
static uint8_t  s_wfx_estaba;       /* WEFAX was on last pass (upstream's s_estaba) */
static uint32_t s_wfx_ult;
static uint16_t s_gy;               /* the chart's next row */

static uint8_t  s_capt;             /* a screenshot is being taken */
static uint8_t  s_capt_fase;        /* 0 erasing ahead, 1 rows */
static uint32_t s_capt_borra, s_capt_borra_fin;
static uint16_t s_capt_fila;
static uint32_t s_aviso_hasta;      /* the notice is up until then; 0 = none */
static char     s_aviso[48];

static void cierra(void);

static uint8_t es_pre(const char p[4])
{
    return (uint8_t)(memcmp(s_pre, p, 4U) == 0);
}

/* ------------------------------------------------------------------ */
/* upstream's gsv_*                                                     */
/* ------------------------------------------------------------------ */

static void bloque(void)
{
    s_bloques++;
    if (s_blk >= s_blk_n) {             /* a chart at WFX_ALTO_MAX too */
        s_fallo = 1U;
        cierra();
        return;
    }
    if (s_trozos) {
        if (s_ext_i >= s_ext_n) {
            uint32_t dados = 0UL;

            if (!spi_flash_amplia(s_n8, "BMP", WFX_TRAMO_BLOQUES,
                                  &s_ext_addr, &dados, &s_porque)) {
                s_fallo = 1U;
                cierra();
                console_printf("  guarda: the chart cannot grow - %s\n",
                               spi_flash_anade_porque_txt(s_porque));
                return;
            }
            s_ext_n = (uint16_t)dados;
            s_ext_i = 0U;
        }
        if (s_blk == 0U) { s_addr = s_ext_addr; }
        spi_flash_write_block_4k(s_ext_addr + (uint32_t)s_ext_i * GSV_BLOQUE, s_buf);
        s_ext_i++;
        s_blk++;
        s_n = 0U;
        return;
    }
    {
        uint32_t a = s_addr + (uint32_t)s_blk * GSV_BLOQUE;

        if (s_limpio) { spi_flash_write_block_4k_borrado(a, s_buf); }
        else          { spi_flash_write_block_4k(a, s_buf); }
    }
    s_blk++;
    s_n = 0U;
}

static void mete(const uint8_t *d, uint32_t n)
{
    while (n > 0UL && s_on) {
        uint32_t hueco = GSV_BLOQUE - s_n;
        uint32_t c = (n < hueco) ? n : hueco;

        memcpy(&s_buf[s_n], d, c);
        s_n = (uint16_t)(s_n + c);
        d += c;
        n -= c;
        if (s_n == GSV_BLOQUE) { bloque(); }
    }
}

/* upstream's gsv_sello(): the FAT date and time from the RTC, or none.
 * memfram.c's too, before MEM.CSV is written (step 67). */
void guarda_sello(void)
{
    rtc_hw_datetime_t dt;

    if (!rtc_hw_has_ever_synced()) { spi_flash_sello_pon(0U, 0U); return; }
    rtc_hw_get(&dt);
    if (dt.year < 1980U || dt.year > 2107U || dt.month < 1U || dt.month > 12U
        || dt.day < 1U || dt.day > 31U) {
        spi_flash_sello_pon(0U, 0U);
        return;
    }
    spi_flash_sello_pon((uint16_t)(((uint16_t)(dt.year - 1980U) << 9)
                                   | ((uint16_t)dt.month << 5) | (uint16_t)dt.day),
                        (uint16_t)(((uint16_t)dt.hour << 11)
                                   | ((uint16_t)dt.minute << 5)
                                   | (uint16_t)(dt.second / 2U)));
}

static void abre(uint16_t ancho, uint16_t alto, uint8_t bits, const char pre[4],
                 uint8_t crece)
{
    uint32_t idx, total;
    uint8_t  k;

    s_trozos = crece;
    guarda_sello();
    memcpy(s_pre, pre, 4U);
    s_ancho = ancho;
    s_alto  = alto;
    s_porque = SPI_ANADE_OK;
    idx = spi_flash_dir_max_indice(s_pre, "BMP") + 1UL;
    if (idx > 999UL) {
        s_porque = SPI_ANADE_DIR_LLENO;
        s_fallo  = 1U;
        return;
    }
    memcpy(s_n8, pre, 4U);
    s_n8[4] = (char)('0' + (idx / 100UL) % 10UL);
    s_n8[5] = (char)('0' + (idx / 10UL) % 10UL);
    s_n8[6] = (char)('0' + idx % 10UL);
    s_n8[7] = ' ';

    /* upstream's: a chart is named by its frequency, W + five figures of
     * kHz + two of a counter - W0819801.BMP */
    if (es_pre("WFAX")) {
        uint32_t khz = (radio_dial_hz() + 500UL) / 1000UL;

        if (khz <= 99999UL) {
            char nf[8];
            uint32_t j;

            nf[0] = 'W';
            nf[1] = (char)('0' + (khz / 10000UL) % 10UL);
            nf[2] = (char)('0' + (khz / 1000UL) % 10UL);
            nf[3] = (char)('0' + (khz / 100UL) % 10UL);
            nf[4] = (char)('0' + (khz / 10UL) % 10UL);
            nf[5] = (char)('0' + khz % 10UL);
            j = spi_flash_dir_max_indice_n(nf, 6U, "BMP") + 1UL;
            if (j <= 99UL) {
                nf[6] = (char)('0' + (j / 10UL) % 10UL);
                nf[7] = (char)('0' + j % 10UL);
                for (k = 0U; k < 8U; k++) { s_n8[k] = nf[k]; }
            }
        }
    }

    s_bits = bits;
    total = bmp_cabecera(s_buf, ancho, alto, bits);
    total = ((total + GSV_BLOQUE - 1UL) / GSV_BLOQUE) * GSV_BLOQUE;
    if (s_trozos) {
        s_addr  = 0UL;
        s_ext_n = 0U;
        s_ext_i = 0U;
    } else if (!spi_flash_reserva(s_n8, "BMP", total, &s_addr, &s_porque)) {
        s_fallo = 1U;
        return;
    }
    s_blk    = 0U;
    s_limpio = 0U;
    s_blk_n  = (uint16_t)(total / GSV_BLOQUE);
    s_ms     = board_millis();
    s_n      = GSV_BLOQUE;              /* the header IS a whole block */
    s_fila   = 0U;
    s_fallo  = 0U;
    s_on     = 1U;
    bloque();                           /* written now: it is the first */
    {
        char nom[9];

        memcpy(nom, s_n8, 8U);
        for (k = 8U; k > 0U && nom[k - 1U] == ' '; k--) { }
        nom[k] = '\0';
        console_printf("  guarda: %s.BMP open, %u x %u, %u bit\n", nom,
                       (uint32_t)ancho, (uint32_t)alto, (uint32_t)bits);
    }
}

/* upstream's gsv_cierra_interno(): the last block filled up, the header
 * rewritten with the real height, the file cut to it. */
static void cierra(void)
{
    if (!s_on) { return; }
    s_on = 0U;
    if (es_pre("WFAX")) {
        s_wfx_hubo = 1U;
        s_wfx_ult  = board_millis();
    }
    if (!s_fallo && s_n > 0U && s_n < GSV_BLOQUE) {
        memset(&s_buf[s_n], 0xFF, GSV_BLOQUE - s_n);
        s_n = GSV_BLOQUE;
        bloque();
    }
    s_limpio = 0U;

    if (s_trozos) {
        if (s_fila != 0U && s_addr != 0UL) {
            uint32_t nf = bmp_fila_bytes(s_ancho, s_bits);
            uint32_t hay = (s_blk > 1U) ? ((uint32_t)(s_blk - 1U) * GSV_BLOQUE) : 0UL;
            uint16_t filas = (nf != 0UL) ? (uint16_t)(hay / nf) : 0U;

            if (filas > s_fila) { filas = s_fila; }
            if (filas == 0U) {
                s_fila = 0U;
                (void)spi_flash_recorta_cadena(s_n8, "BMP", GSV_BLOQUE);
            } else {
                s_fila = filas;
                (void)bmp_cabecera(s_buf, s_ancho, s_fila, s_bits);
                spi_flash_write_block_4k(s_addr, s_buf);
                (void)spi_flash_recorta_cadena(s_n8, "BMP", GSV_BLOQUE + nf * filas);
            }
        } else {
            (void)spi_flash_recorta_cadena(s_n8, "BMP", GSV_BLOQUE);
        }
        s_ext_n = 0U;
        s_ext_i = 0U;
    } else if (s_fila == 0U) {
        (void)spi_flash_recorta(s_n8, "BMP", GSV_BLOQUE);
    } else if (s_fila < s_alto) {
        (void)spi_flash_recorta(s_n8, "BMP",
                                GSV_BLOQUE + bmp_fila_bytes(s_ancho, s_bits) * (uint32_t)s_fila);
        (void)bmp_cabecera(s_buf, s_ancho, s_fila, s_bits);
        spi_flash_write_block_4k(s_addr, s_buf);
    }
    console_printf("  guarda: closed, %u rows%s\n", (uint32_t)s_fila,
                   s_fallo ? " - FAILED on the way" : "");
}

static void cierra_si_es(const char pre[4])
{
    if (!s_on || s_capt) { return; }        /* the capture closes itself */
    if (!es_pre(pre)) { return; }
    cierra();
}

/* upstream's gsv_linea(): rows that never came are written black, so the
 * picture keeps its geometry; a row that comes late is dropped. */
static void linea(const uint8_t *l, uint16_t y)
{
    uint8_t  trozo[GSV_TROZO * 3U];
    uint32_t nf;

    if (!s_on) { return; }
    if (y < s_fila) { return; }
    nf = bmp_fila_bytes(s_ancho, s_bits);

    while (s_fila < y && s_on && s_bloques == 0U) {
        uint32_t puesto = 0UL;

        memset(trozo, 0, sizeof trozo);
        while (puesto < nf) {
            uint32_t c = nf - puesto;

            if (c > sizeof trozo) { c = sizeof trozo; }
            mete(trozo, c);
            puesto += c;
        }
        s_fila++;
    }
    if (!s_on || s_fila < y) { return; }

    if (s_bits == BMP_8) {
        mete(l, nf);
    } else {
        uint16_t x = 0U;

        while (x < s_ancho) {
            uint16_t m = (uint16_t)(s_ancho - x);

            if (m > GSV_TROZO) { m = GSV_TROZO; }
            if (s_bits == BMP_16) {
                bmp_fila565(l + (uint32_t)x * 3UL, trozo, m);
                mete(trozo, (uint32_t)m * 2UL);
            } else {
                bmp_fila24(l + (uint32_t)x * 3UL, trozo, m);
                mete(trozo, (uint32_t)m * 3UL);
            }
            x = (uint16_t)(x + m);
        }
    }
    if (!s_on) { return; }
    s_fila++;
    s_ms = board_millis();
    if (s_fila >= s_alto) { cierra(); }
}

/* ------------------------------------------------------------------ */
/* the notice                                                           */
/* ------------------------------------------------------------------ */

static void aviso_pinta(gfx2_surf_t *s, void *ctx)
{
    (void)ctx;
    gfx2_rrect(s, 60, 130, 360, 60, 8, gfx2_rgb(PAL_SURF_2));
    gfx2_text_in(s, 60, 150, 360, s_aviso, &font_ui_14b, gfx2_rgb(PAL_INK),
                 GFX2_ALIGN_C);
}

/* A line in the middle of the screen for three seconds, the display loop
 * held still under it, everything repainted after (upstream's capt_avisa()). */
static void aviso(const char *t)
{
    uint32_t i;

    for (i = 0U; t[i] != '\0' && i < sizeof s_aviso - 1U; i++) { s_aviso[i] = t[i]; }
    s_aviso[i] = '\0';
    gfx2_render(60, 130, 360, 60, aviso_pinta, 0);
    s_aviso_hasta = board_millis() + AVISO_MS;
    if (s_aviso_hasta == 0U) { s_aviso_hasta = 1U; }
    console_printf("  guarda: \"%s\"\n", s_aviso);
}

static void aviso_porque(const char *cab)
{
    char t[48];
    const char *r = (s_porque == SPI_ANADE_OK && s_fallo)
                  ? tr("ya hay 999", "999 already") : spi_flash_anade_porque_txt(s_porque);
    uint32_t j = 0U;

    while (*cab != '\0' && j < sizeof t - 1U) { t[j++] = *cab++; }
    while (*r != '\0' && j < sizeof t - 1U) { t[j++] = *r++; }
    t[j] = '\0';
    aviso(t);
}

/* ------------------------------------------------------------------ */
/* the screenshot                                                       */
/* ------------------------------------------------------------------ */

uint8_t guarda_pantalla_quieta(void)
{
    return (uint8_t)(s_capt || s_aviso_hasta != 0U);
}

uint8_t guarda_captura_pide(void)
{
    if (s_capt || s_aviso_hasta != 0U) { return 0U; }
    if (wefax_activo() || sstv_activo()) {
        /* upstream's: the picture saves itself in those two */
        bip_error();
        aviso(tr("EL FAX/SSTV YA SE GUARDA SOLO", "FAX/SSTV SAVE THEMSELVES"));
        return 0U;
    }
    if (usbdisco_activo()) {
        bip_error();
        aviso(tr("EL DISCO ESTA EN EL PC", "THE DISK IS ON THE PC"));
        return 0U;
    }
    if (s_on) {
        bip_error();
        aviso(tr("YA SE ESTA GUARDANDO", "ALREADY SAVING"));
        return 0U;
    }
    abre(CAPT_ANCHO, CAPT_ALTO, BMP_16, "PANT", 0U);
    if (!s_on) {
        bip_error();
        aviso_porque(tr("NO CABE: ", "NO ROOM: "));
        return 0U;
    }
    s_capt_borra     = s_addr + GSV_BLOQUE;
    s_capt_borra_fin = s_addr + (uint32_t)s_blk_n * GSV_BLOQUE;
    s_capt_fase = 0U;
    s_capt_fila = 0U;
    s_capt = 1U;
    bip_empieza();
    return 1U;
}

/* One step: an erase ahead (upstream's), or CAPT_FILAS rows off the glass
 * - RAMRD leads with a dummy byte, then R, G, B a pixel (grab.py found
 * it, 21/11/2026) - which is what bmp_fila565() eats. */
static void captura_paso(void)
{
    uint16_t nf, r;

    if (!s_on) {                        /* cut short inside */
        s_capt = 0U;
        bip_error();
        aviso_porque(tr("CORTADA: ", "CUT SHORT: "));
        return;
    }
    if (s_capt_fase == 0U) {
        if (spi_flash_borrado_paso(&s_capt_borra, s_capt_borra_fin)) { return; }
        s_capt_fase = 1U;
        s_limpio = 1U;
        return;
    }
    if (s_capt_fila >= CAPT_ALTO) {
        char t[40];
        uint32_t j = 0U, i;
        const char *cab = tr("GUARDADA ", "SAVED ");

        s_fila = CAPT_ALTO;
        cierra();
        s_capt = 0U;
        bip_listo();
        while (*cab != '\0') { t[j++] = *cab++; }
        for (i = 0U; i < 7U; i++) { t[j++] = s_n8[i]; }
        t[j++] = '.'; t[j++] = 'B'; t[j++] = 'M'; t[j++] = 'P'; t[j] = '\0';
        aviso(t);
        return;
    }
    nf = (uint16_t)(CAPT_ALTO - s_capt_fila);
    if (nf > CAPT_FILAS) { nf = CAPT_FILAS; }
    ili9488_read_gram(0U, s_capt_fila, CAPT_ANCHO, nf, g_grab,
                      (uint32_t)nf * CAPT_ANCHO * 3U + 1U);
    for (r = 0U; r < nf && s_on; r++) {
        const uint8_t *rgb = &g_grab[1U + (uint32_t)r * CAPT_ANCHO * 3U];
        uint8_t  trozo[GSV_TROZO * 2U];
        uint16_t x;

        for (x = 0U; x < CAPT_ANCHO; x = (uint16_t)(x + GSV_TROZO)) {
            bmp_fila565(rgb + (uint32_t)x * 3U, trozo, GSV_TROZO);
            mete(trozo, GSV_TROZO * 2U);
        }
    }
    s_capt_fila = (uint16_t)(s_capt_fila + nf);
}

/* The disk goes to the PC (usbdisco_abre()): from there the PC owns the
 * FAT, so whatever is being written ends here, closed with the rows it
 * has - a screenshot half taken is closed short. Upstream never needs
 * this: its disk is the bootloader's, with the firmware not running. */
void guarda_suelta(void)
{
    if (!s_on) { return; }
    if (s_capt) {
        s_capt = 0U;
        s_fila = (s_capt_fase == 0U) ? 0U : s_capt_fila;
        bip_error();
    }
    console_printf("  guarda: the disk goes to the PC, the file closes\n");
    cierra();
}

/* ------------------------------------------------------------------ */
/* SSTV and WEFAX                                                       */
/* ------------------------------------------------------------------ */

uint8_t guarda_cupo(void) { return (uint8_t)(s_bloques == 0U); }

void guarda_sstv_linea(const uint8_t *l, uint16_t y)
{
    /* upstream's: a picture is opened at its first row, never halfway */
    if (!s_on && !s_capt && y == 0U && s_sstv_guarda != SSTV_GUARDA_NO
        && !usbdisco_activo()) {
        abre(SSTV_ANCHO, SSTV_ALTO,
             (uint8_t)((s_sstv_guarda == SSTV_GUARDA_16) ? BMP_16 : BMP_24), "SSTV", 0U);
        if (!s_on) { aviso_porque(tr("NO SE GUARDA: ", "NOT SAVING: ")); }
    }
    if (s_on && es_pre("SSTV")) { linea(l, y); }
}

void guarda_fax_linea(const uint8_t *l)
{
    if (!s_on || !es_pre("WFAX")) { return; }
    linea(l, s_gy);
    s_gy++;
}

void guarda_poll(void)
{
    s_bloques = 0U;

    if (s_aviso_hasta != 0U && !s_capt
        && (int32_t)(board_millis() - s_aviso_hasta) >= 0) {
        s_aviso_hasta = 0U;
        radio_repaint_all();            /* what was under the notice */
    }
    if (s_capt) { captura_paso(); return; }

    /* SSTV: closed when the decoder has no picture any more, or on
     * silence (upstream's sstv_poll()). ONE PASS LATE: this runs before
     * digi.c takes the lines, and the decoder leaves SSTV_IMAGEN with its
     * last row still queued - closed at once, every picture lost it
     * (bench, 27/11: "closed, 255 rows"). Upstream looks after its lines. */
    {
        static uint8_t era, pend;
        uint8_t ahora = 0U;

        if (sstv_activo()) {
            sstv_info_t in;

            sstv_info(&in);
            ahora = (uint8_t)(in.estado == SSTV_IMAGEN);
        }
        if (pend) { pend = 0U; cierra_si_es("SSTV"); }
        if (era && !ahora) { pend = 1U; }
        era = ahora;
        if (s_on && es_pre("SSTV") && (board_millis() - s_ms) > GSV_ESPERA_MS) {
            cierra_si_es("SSTV");
        }
    }

    /* WEFAX (upstream's fax_poll()): with saving on, a chart file is open
     * whenever none is - lines come only inside a chart - except for 25 s
     * after one closed; it closes at the stop tone, or full height, after
     * which the next one opens. */
    if (wefax_activo()) {
        wefax_info_t in;
        static uint8_t era_img, pend_img;
        uint8_t ahora;

        /* upstream's (20/10/2026): ENTERING WEFAX DOES NOT SAVE - the
         * finger opens the file. The button starts at "No save" every time
         * the mode comes in, whatever the FRAM kept, or a radio left
         * saving fills the disk with noise on its own. */
        if (!s_wfx_estaba) {
            s_wfx_estaba = 1U;
            s_wfx_guarda = 0U;
            s_fallo      = 0U;
        }
        wefax_info(&in);
        ahora = (uint8_t)(in.estado == WEFAX_IMAGEN);
        if (s_wfx_guarda) {
            if (!s_capt && wfx_abre_toca(s_on, s_fallo, 1U) && !usbdisco_activo()
                && (!s_wfx_hubo || (board_millis() - s_wfx_ult) > WFX_CERROJO_MS)) {
                abre(WEFAX_ANCHO, WFX_ALTO_MAX, BMP_8, "WFAX", 1U);
                if (s_on) { s_gy = 0U; } else { aviso_porque(tr("NO SE GUARDA: ", "NOT SAVING: ")); }
            }
        } else {
            cierra_si_es("WFAX");
        }
        if (pend_img) { pend_img = 0U; cierra_si_es("WFAX"); }
        if (era_img && !ahora) { pend_img = 1U; }   /* one pass late, as SSTV */
        era_img = ahora;
    } else {
        cierra_si_es("WFAX");
        s_wfx_estaba = 0U;
    }
}

/* ------------------------------------------------------------------ */
/* the settings and the buttons                                         */
/* ------------------------------------------------------------------ */

uint8_t guarda_sstv_formato(void) { return s_sstv_guarda; }

void guarda_sstv_formato_pon(uint8_t f)
{
    s_sstv_guarda = (f < SSTV_GUARDA_N) ? f : SSTV_GUARDA_NO;
}

void guarda_sstv_cicla(void)
{
    s_sstv_guarda = (uint8_t)((s_sstv_guarda + 1U) % SSTV_GUARDA_N);
    s_fallo = 0U;
}

uint8_t guarda_fax(void) { return s_wfx_guarda; }

void guarda_fax_pon(uint8_t on) { s_wfx_guarda = on ? 1U : 0U; }

void guarda_fax_cicla(void)
{
    s_wfx_guarda = (uint8_t)!s_wfx_guarda;
    s_fallo = 0U;                       /* upstream's: a new try */
    s_wfx_hubo = 0U;
}

uint8_t guarda_grabando(void) { return s_on; }

/* "REC 042" while its file is open; "Failed" after a failure; else the
 * setting. 68 px of button: upstream's "Guardando 042" does not fit. */
static const char *rotulo(const char pre[4], const char *ajuste)
{
    static char b[12];

    if (s_on && es_pre(pre)) {
        memcpy(b, "REC ", 4U);
        b[4] = s_n8[(pre[0] == 'W') ? 6U : 4U];
        b[5] = s_n8[(pre[0] == 'W') ? 7U : 5U];
        b[6] = (pre[0] == 'W') ? '\0' : s_n8[6];
        b[7] = '\0';
        return b;
    }
    if (s_fallo) { return tr("Fallo", "Failed"); }
    return ajuste;
}

const char *guarda_sstv_rotulo(void)
{
    static const texto_t k[SSTV_GUARDA_N] = {
        T("No guarda", "No save"), T("BMP 24", "BMP 24"), T("BMP 16", "BMP 16")
    };

    return rotulo("SSTV", k[s_sstv_guarda][idioma()]);
}

const char *guarda_fax_rotulo(void)
{
    return rotulo("WFAX", s_wfx_guarda ? tr("BMP gris", "Grey BMP")
                                       : tr("No guarda", "No save"));
}
