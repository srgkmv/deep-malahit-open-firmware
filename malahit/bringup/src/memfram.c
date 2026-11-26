#include <string.h>
#include "spi_flash.h"     /* ../../User - upstream's prototypes, nothing else */
#include "fram.h"

/*
 * THE DISK THIS BOARD DOES NOT HAVE, FOR ONE FILE - 22/11/2026.
 *
 * Upstream keeps its memory channels in MEM.CSV on the DEEPSDR's SPI
 * flash disk (banco.c, banco.h: "DONDE VIVE"), and banco.c reaches the
 * disk through exactly two calls. This board has no disk - PORTING.md
 * 3.3, decided - but it has 25 kB of FRAM nobody uses, so those two calls
 * are answered here, for MEM.CSV and nothing else, and banco.c is
 * compiled in place unmodified: the same table, the same CSV, the same
 * parser, the same validation.
 *
 * WHERE: 0x4200, two slots of 3 kB, after the settings record (0x4000
 * for versions 1-3, 0x4100 for 4) and inside the stretch the factory
 * firmware leaves blank (settings.h). Upstream's own buffer floor is
 * BANCO_CSV_MIN, 2560 bytes; a full bank of sixty memories with the four
 * names is under 2.7 kB.
 *
 * HOW: as the settings record - each slot a header (magic, sequence,
 * length, CRC-32 of the data) and the data; a write goes to the slot NOT
 * holding the newest file, data first and header last, and is read back;
 * a read takes the newest slot whose CRC checks. A power cut in the middle
 * of a write leaves the other slot whole, so the bank comes back one save
 * behind rather than empty.
 *
 * SINCE THE SPI FLASH CAME (step 60, 26/11/2026) upstream's real
 * spi_flash.c is linked too, with these two names. This file and banco.c
 * are compiled with -Dspi_flash_read_file_by_name=memfram_read_file_by_name
 * (and the write's likewise, reglas.mk MEMFRAM_RENAME): the functions
 * below are memfram_*() in the object, and banco.c still reaches them,
 * not the flash. MEM.CSV stays in the FRAM.
 */
#define MEMF_ADDR     0x4200U
#define MEMF_SLOT     0x0C00U                   /* 3 kB */
#define MEMF_MAGIC    0x4D454D4DU               /* "MMEM" */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t crc;
} memf_hdr_t;

#define MEMF_DATA_MAX (MEMF_SLOT - sizeof(memf_hdr_t))

static int      s_slot = -1;
static uint32_t s_seq;

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

static uint32_t slot_addr(int slot) { return MEMF_ADDR + (uint32_t)slot * MEMF_SLOT; }

static int es_mem_csv(const char name8[8], const char ext3[3])
{
    return memcmp(name8, "MEM     ", 8) == 0 && memcmp(ext3, "CSV", 3) == 0;
}

/* The data's CRC, read off the FRAM in pieces - the whole 3 kB need not
 * sit in RAM to be checked. */
static uint32_t crc_en_fram(uint32_t addr, uint32_t len)
{
    uint8_t  t[128];
    uint32_t c = 0xFFFFFFFFU;

    while (len > 0U) {
        uint32_t n = (len < sizeof t) ? len : (uint32_t)sizeof t;

        fram_read(addr, t, n);
        c = crc32_add(c, t, n);
        addr += n;
        len  -= n;
    }
    return ~c;
}

uint32_t spi_flash_read_file_by_name(const char name8[8], const char ext3[3],
                                     uint8_t *out_buf, uint32_t out_buf_size)
{
    memf_hdr_t h[2];
    int        ok[2], pick, i;

    if (!es_mem_csv(name8, ext3)) { return 0U; }

    for (i = 0; i < 2; i++) {
        fram_read(slot_addr(i), (uint8_t *)&h[i], sizeof h[i]);
        ok[i] = h[i].magic == MEMF_MAGIC && h[i].len > 0U
             && h[i].len <= MEMF_DATA_MAX
             && h[i].crc == crc_en_fram(slot_addr(i) + sizeof h[i], h[i].len);
    }
    if (!ok[0] && !ok[1]) { s_slot = -1; s_seq = 0U; return 0U; }

    pick = (!ok[0]) ? 1
         : (!ok[1]) ? 0
         : ((int32_t)(h[1].seq - h[0].seq) > 0) ? 1 : 0;
    s_slot = pick;
    s_seq  = h[pick].seq;

    if (h[pick].len > out_buf_size) { return 0U; }
    fram_read(slot_addr(pick) + sizeof h[pick], out_buf, h[pick].len);
    return h[pick].len;
}

int spi_flash_write_or_update_file(const char name8[8], const char ext3[3],
                                   const uint8_t *data, uint32_t len)
{
    memf_hdr_t h, back;
    int        slot = (s_slot == 0) ? 1 : 0;
    uint32_t   a;

    if (!es_mem_csv(name8, ext3) || len == 0U || len > MEMF_DATA_MAX) { return 0; }

    a = slot_addr(slot);
    h.magic = MEMF_MAGIC;
    h.seq   = s_seq + 1U;
    h.len   = len;
    h.crc   = ~crc32_add(0xFFFFFFFFU, data, len);

    fram_write(a + sizeof h, data, len);
    fram_write(a, (const uint8_t *)&h, sizeof h);

    fram_read(a, (uint8_t *)&back, sizeof back);
    if (memcmp(&h, &back, sizeof h) != 0
        || crc_en_fram(a + sizeof h, len) != h.crc) {
        return 0;               /* the other slot still holds the last good one */
    }
    s_slot = slot;
    s_seq  = h.seq;
    return 1;
}

/* For the console. */
int      memfram_slot(void) { return s_slot; }
uint32_t memfram_seq(void)  { return s_seq; }
