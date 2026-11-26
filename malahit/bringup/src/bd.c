#include <stdint.h>

#include "board.h"
#include "clock.h"
#include "console.h"
#include "bd.h"
#include "spi_flash.h"      /* ../../User - upstream's */
#include "zona_alta.h"
#include "emisoras.h"
#include "idioma.h"

/*
 * Upstream's main.c, 16912-17045 (datos_arranca()) and 17243-17310
 * (datos_al_arrancar()), and 2609-2615 (the opening). What is left out is
 * its info screen's bookkeeping (s_vol_estado and the rest) and its
 * full-screen progress card, which here is ui_prompt() through `avisa`.
 */

#define BD_N8  "BD      "
#define BD_E3  "BIN"

static spi_flash_alta_t s_alta;
static uint8_t          s_alta_hecho;
static const char      *s_porque;

static uint32_t ms_desde(uint64_t t0)
{
    return (uint32_t)(((board_cycles() - t0) * 1000U) / clock_sysclk_hz);
}

/* Upstream's alta_mira(): spi_flash_mira_alta() once - about a tenth of a
 * second a megabyte - and remembered. */
static const spi_flash_alta_t *alta_mira(void)
{
    if (!s_alta_hecho) {
        spi_flash_mira_alta(&s_alta);
        s_alta_hecho = 1U;
    }
    return &s_alta;
}

/* Upstream's datos_arranca(): the checks, then spi_flash_restaura_abre(). */
static uint8_t datos_arranca(void)
{
    const spi_flash_alta_t *a;
    uint32_t tam = 0UL, bloques;

    s_porque = 0;
    if (!spi_flash_fichero_tam(BD_N8, BD_E3, &tam)) {
        s_porque = spi_flash_volcado_porque_txt();   /* upstream: s_vol_porque */
        return 0U;
    }
    if (tam == 0UL) {
        s_porque = tr("BD.BIN mide cero", "BD.BIN is empty");
        return 0U;
    }
    bloques = (tam + 4095UL) & ~4095UL;
    if (bloques > (ZA_TOPE_DATOS - ZA_BASE)) {
        s_porque = tr("BD.BIN no cabe arriba", "BD.BIN does not fit");
        return 0U;
    }

    a = alta_mira();
    if (a->fs_bytes == 0UL) {
        s_porque = tr("no mido el disco", "cannot measure disk");
        return 0U;
    }
    if (ZA_TOPE_DATOS > spi_flash_capacidad() || ZA_BASE < a->fs_bytes) {
        s_porque = (a->fs_bytes > za_disco_de(spi_flash_capacidad()))
                 ? tr("disco mayor: formatea", "disk too big: format")
                 : tr("sin zona alta en el chip", "no upper area here");
        return 0U;
    }
    if (spi_flash_restaura_abre(BD_N8, BD_E3, ZA_BASE, bloques,
                                a->fs_bytes, ZA_TOPE_DATOS)) {
        return 1U;
    }
    s_porque = spi_flash_volcado_porque_txt();
    return 0U;
}

static void (*s_avisa)(const char *, const char *);
static char  s_linea[48];
static uint32_t s_pct;

static void avisa_pct(const char *que, uint32_t pct)
{
    /* "Copying 45 %" in the panel's words, both languages. */
    uint32_t n = 0U;

    for (const char *p = que; *p != '\0' && n < sizeof s_linea - 8U; p++) {
        s_linea[n++] = *p;
    }
    s_linea[n++] = ' ';
    if (pct >= 100U) { s_linea[n++] = '1'; s_linea[n++] = '0'; s_linea[n++] = '0'; }
    else {
        if (pct >= 10U) { s_linea[n++] = (char)('0' + pct / 10U); }
        s_linea[n++] = (char)('0' + pct % 10U);
    }
    s_linea[n++] = ' ';
    s_linea[n++] = '%';
    s_linea[n]   = '\0';
    if (s_avisa != 0) {
        s_avisa(tr("Bases de datos", "Databases"), s_linea);
    }
}

/* Upstream's datos_comprobando(): the verify's progress, a step a 10 %. */
static void comprobando(uint32_t hechos, uint32_t total)
{
    uint32_t pct;

    if (total == 0UL) { return; }
    pct = ((hechos * 100UL) / total) / 10U * 10U;
    if (pct == s_pct) { return; }
    s_pct = pct;
    avisa_pct(tr("Comprobando", "Checking"), pct);
}

static void falla(const char *motivo)
{
    console_printf("BD.BIN    : FAILED - %s\n", motivo != 0 ? motivo : "?");
    if (s_avisa != 0) {
        s_avisa(tr("Bases de datos", "Databases"), motivo != 0 ? motivo : "?");
    }
    board_delay_ms(3000U);
}

uint8_t bd_al_arrancar(void (*avisa)(const char *titulo, const char *linea))
{
    uint32_t total, ultimo = 0xFFFFFFFFUL;
    int8_t   paso;
    uint64_t t0 = board_cycles();

    if (!spi_flash_fichero_hay(BD_N8, BD_E3)) { return 0U; }

    s_avisa = avisa;
    if (s_avisa != 0) {
        s_avisa(tr("Bases de datos", "Databases"),
                tr("BD.BIN, del disco a la zona alta de la flash",
                   "BD.BIN, from disk to the upper flash area"));
    }
    if (!datos_arranca()) {
        falla(s_porque);
        return 1U;
    }

    total = spi_flash_volcado_total();
    console_printf("BD.BIN    : on the disk - %u bytes into the high zone at "
                   "0x%06x\n", total, ZA_BASE);
    do {
        paso = spi_flash_volcado_paso();
        if (total != 0UL) {
            uint32_t pct = ((spi_flash_volcado_hechos() * 100UL) / total)
                           / 5U * 5U;

            if (pct != ultimo) {
                ultimo = pct;
                avisa_pct(tr("Copiando", "Copying"), pct);
            }
        }
    } while (paso > 0);

    if (paso != 0) {
        falla(spi_flash_volcado_porque_txt());
        return 1U;
    }
    uint32_t t_copia = ms_desde(t0);

    s_pct = 0xFFFFFFFFUL;
    avisa_pct(tr("Comprobando", "Checking"), 0U);

    /* Upstream's order: the directory, then every section's sum, and
     * only then is BD.BIN deleted from the disk. */
    if (!zona_alta_abre(spi_flash_read)) {
        falla(tr("Copiado, pero el directorio no cuadra",
                 "Copied, but the directory does not add up"));
    } else if (!zona_alta_verifica_p(comprobando)) {
        falla(tr("Copiado, pero las sumas no cuadran",
                 "Copied, but the checksums do not match"));
    } else if (spi_flash_fichero_borra(BD_N8, BD_E3)) {
        console_printf("BD.BIN    : copied in %u ms, checked, deleted from "
                       "the disk - %u ms in all\n", t_copia, ms_desde(t0));
        avisa_pct(tr("Cargado", "Loaded"), 100U);
        board_delay_ms(1500U);
    } else {
        falla(tr("Cargado, pero no se pudo borrar del disco",
                 "Loaded, but it could not be deleted from the disk"));
    }
    return 1U;
}

void bd_abre(void)
{
    uint32_t addr = 0UL, tam = 0UL;

    if (!zona_alta_abre(spi_flash_read)) {
        console_printf("Databases : none in the high zone (0x%06x) - copy "
                       "BD.BIN to the USB disk and restart\n", ZA_BASE);
        return;
    }

    console_printf("Databases : high zone at 0x%06x, %u sections\n",
                   ZA_BASE, (uint32_t)zona_alta_secciones());

    if (zona_alta_donde(ZA_TIPO_AVIONES, &addr, &tam)) {
        console_printf("            aircraft models: %u bytes at 0x%06x "
                       "(icao24_db.c, HFDL's Model column)\n", tam, addr);
    }
    if (zona_alta_donde(ZA_TIPO_EMISORAS, &addr, &tam)) {
        uint32_t n_frec = 0UL, n_ent = 0UL, hasta = 0UL;

        if (emisoras_abre(addr, spi_flash_read)) {
            emisoras_info(&n_frec, &n_ent, &hasta);
            console_printf("            stations: %u frequencies, %u entries, "
                           "valid until %u, UTC offset %d h in the file\n", n_frec,
                           n_ent, hasta, (int32_t)emisoras_horas_utc());
        } else {
            console_printf("            stations: %u bytes at 0x%06x, header "
                           "does not add up\n", tam, addr);
        }
    }
}
