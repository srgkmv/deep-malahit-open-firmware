#include "stm32h7xx.h"
#include "board.h"
#include "clock.h"
#include "console.h"
#include "nor.h"
#include "spi_nor.h"
#include "spi_flash.h"
#include "idioma.h"
#include "disco.h"
#include "formato.h"

/* Status register 2 (0x35): upstream reads only the first. */
static uint8_t read_sr(uint8_t op)
{
    uint8_t v;

    spi_nor_cs(0U);
    (void)spi_nor_byte(op);
    v = spi_nor_byte(0x00U);
    spi_nor_cs(1U);
    return v;
}

static uint32_t crc32_add(uint32_t c, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (uint32_t k = 0; k < 8U; k++) {
            c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
        }
    }
    return c;
}

__attribute__((noinline)) void nor_bench(uint32_t addr, uint32_t len)
{
    uint8_t  buf[512];
    uint32_t cap = spi_flash_capacidad();
    uint32_t c   = 0xFFFFFFFFU;
    uint64_t t_read = 0U;       /* 8 MB is ~12 s: 5.8e9 cycles, past 32 bits */
    uint32_t to0 = spi_nor_timeouts();

    if (len == 0U || addr >= cap || len > cap - addr) {
        addr = (len == 0U) ? 0U : addr;
        len  = cap - addr;
    }

    for (uint32_t done = 0U; done < len; ) {
        uint32_t n = len - done;

        if (n > sizeof buf) { n = sizeof buf; }

        uint64_t t0 = board_cycles();
        spi_flash_read(addr + done, buf, n);
        t_read += board_cycles() - t0;

        c = crc32_add(c, buf, n);
        done += n;
    }

    uint32_t us = (uint32_t)((t_read * 1000000U) / clock_sysclk_hz);

    console_printf("nor: read 0x%06x..0x%06x (%u bytes): crc32 %08x, "
                   "%u us in spi_flash_read = %u kB/s, timeouts %u\n",
                   addr, addr + len - 1U, len, ~c, us,
                   (us != 0U) ? (uint32_t)(((uint64_t)len * 1000000U)
                                           / 1024U / us) : 0U,
                   spi_nor_timeouts() - to0);
}

/* The test pattern: a 32-bit hash of each word's address and the seed
 * (murmur3's finaliser), so a page written to the wrong address, a bit
 * stuck at either level or an address line short all show. */
static uint32_t patron(uint32_t addr, uint32_t semilla)
{
    uint32_t h = addr ^ semilla;

    h ^= h >> 16; h *= 0x85EBCA6BU;
    h ^= h >> 13; h *= 0xC2B2AE35U;
    h ^= h >> 16;
    return h;
}

static uint32_t us_desde(uint64_t t0)
{
    return (uint32_t)(((board_cycles() - t0) * 1000000U) / clock_sysclk_hz);
}

__attribute__((noinline)) void nor_prueba(uint32_t fase, uint32_t semilla)
{
    static const char *const nombre[] = {
        "erase all", "blank check", "program pattern", "verify pattern"
    };
    uint32_t cap = spi_flash_capacidad();
    uint32_t malos = 0U, primero = 0xFFFFFFFFU, pasos = 0U;
    uint32_t to0 = spi_nor_timeouts();
    uint64_t t0  = board_cycles();
    union { uint8_t b[256]; uint32_t w[64]; } p;

    if (fase > 3U) { return; }

    if (fase == 0U) {
        uint32_t cursor = 0U;

        while (spi_flash_borrado_paso(&cursor, cap)) { pasos++; }
    }

    for (uint32_t a = 0U; fase != 0U && a < cap; a += sizeof p) {
        if (fase == 2U) {
            for (uint32_t i = 0U; i < 64U; i++) {
                p.w[i] = patron(a + 4U * i, semilla);
            }
            spi_flash_page_program(a, p.b, sizeof p);
            pasos++;
            continue;
        }

        spi_flash_read(a, p.b, sizeof p);
        for (uint32_t i = 0U; i < 64U; i++) {
            uint32_t want = (fase == 1U) ? 0xFFFFFFFFU
                                         : patron(a + 4U * i, semilla);
            if (p.w[i] == want) { continue; }
            for (uint32_t k = 0U; k < 4U; k++) {
                if (p.b[4U * i + k] != (uint8_t)(want >> (8U * k))) {
                    if (malos < 8U) {
                        console_printf("nor:   0x%06x wrote %02x read %02x\n",
                                       a + 4U * i + k,
                                       (uint32_t)(uint8_t)(want >> (8U * k)),
                                       (uint32_t)p.b[4U * i + k]);
                    }
                    if (primero == 0xFFFFFFFFU) { primero = a + 4U * i + k; }
                    malos++;
                }
            }
        }
    }

    console_printf("nor: %s, 0x000000..0x%06x, seed %08x: %u bad bytes",
                   nombre[fase], cap - 1U, semilla, malos);
    if (malos != 0U) { console_printf(" (first 0x%06x)", primero); }
    console_printf(", %u steps, %u ms, timeouts %u\n", pasos,
                   us_desde(t0) / 1000U, spi_nor_timeouts() - to0);
}

__attribute__((noinline)) void nor_formatea(void)
{
    uint64_t    t0 = board_cycles();
    formato_r_t r  = formato_haz();
    uint32_t    t_haz = us_desde(t0);
    formato_r_t c  = (r == FORMATO_OK) ? formato_comprueba() : r;
    uint8_t     lengua = idioma();
    uint32_t    libre_kb = 0U, seguido_kb = 0U;
    uint16_t    huecos = 0U, ranuras = 0U;

    (void)spi_flash_sitio(&libre_kb, &seguido_kb, &huecos, &ranuras);
    idioma_pon(IDIOMA_EN);
    console_printf("nor: format %s (%u ms), check %s (%u ms); %u blocks of "
                   "512, volume: %s\n",
                   formato_porque_txt(r), t_haz / 1000U,
                   formato_porque_txt(c), (us_desde(t0) - t_haz) / 1000U,
                   disco_bloques(), spi_flash_geo_txt());
    console_printf("nor: free %u kB, largest run %u kB, root entries %u "
                   "free of %u\n", libre_kb, seguido_kb,
                   (uint32_t)huecos, (uint32_t)ranuras);
    idioma_pon(lengua);
}

void nor_boot_report(void)
{
    spi_flash_jedec_id_t id;
    uint8_t  uid[8];
    uint64_t t0 = board_cycles();

    spi_flash_init();
    spi_flash_read_jedec_id(&id);

    if ((id.manufacturer_id == 0xFFU && id.memory_type == 0xFFU)
        || (id.manufacturer_id == 0x00U && id.memory_type == 0x00U)) {
        /* MISO is pulled up (fram.c): nobody answering reads 0xFF. */
        console_printf("SPI flash : none - JEDEC %02x %02x %02x\n",
                       (uint32_t)id.manufacturer_id,
                       (uint32_t)id.memory_type,
                       (uint32_t)id.capacity_code);
        return;
    }

    uint8_t sr1 = read_sr(0x05U);
    uint8_t sr2 = read_sr(0x35U);

    console_printf("SPI flash : JEDEC %02x %02x %02x%s, %u kB, SR1 %02x "
                   "SR2 %02x\n",
                   (uint32_t)id.manufacturer_id, (uint32_t)id.memory_type,
                   (uint32_t)id.capacity_code,
                   (id.manufacturer_id == 0xEFU && id.memory_type == 0x40U
                    && id.capacity_code == 0x17U) ? " (W25Q64)" : "",
                   spi_flash_capacidad() / 1024U,
                   (uint32_t)sr1, (uint32_t)sr2);

    if (spi_flash_unique_id(uid)) {
        console_printf("            serial %02x%02x%02x%02x%02x%02x%02x%02x\n",
                       (uint32_t)uid[0], (uint32_t)uid[1], (uint32_t)uid[2],
                       (uint32_t)uid[3], (uint32_t)uid[4], (uint32_t)uid[5],
                       (uint32_t)uid[6], (uint32_t)uid[7]);
    }

    /* BP2..0, TB, SEC in SR1 and CMP in SR2: any of them set write-protects
     * part of the array, and an erase or program there is ignored with no
     * error - the FAT would look written and read back old. SRP0/SRP1
     * only lock the register itself (with /WP, which is tied high). */
    if ((sr1 & 0x7CU) != 0U || (sr2 & 0x40U) != 0U) {
        console_printf("            *** BLOCK PROTECTION SET (SR1 BP/TB/SEC "
                       "%02x, SR2 CMP %u): writes there are ignored ***\n",
                       (uint32_t)(sr1 & 0x7CU), (uint32_t)((sr2 >> 6) & 1U));
    }

    uint8_t reparado = spi_flash_respaldo_repara();

    /* Upstream's reason comes through tr(), and the language from the
     * settings is applied later in the boot: the console is English. */
    uint8_t lengua = idioma();

    idioma_pon(IDIOMA_EN);
    console_printf("            volume: %s%s - %u us\n",
                   spi_flash_geo_txt(),
                   reparado ? "; an interrupted write REPAIRED" : "",
                   (uint32_t)(((board_cycles() - t0) * 1000000U)
                              / clock_sysclk_hz));
    idioma_pon(lengua);

    /* The SWD entry point kept past --gc-sections (demod.c's way). */
    __asm__ volatile ("" :: "r"(nor_bench), "r"(nor_prueba),
                            "r"(nor_formatea));
}
