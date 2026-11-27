#include <string.h>
#include "spi_flash.h"     /* ../../User - upstream's disk */
#include "fram.h"
#include "console.h"
#include "usbdisco.h"
#include "guarda.h"
#include "menus.h"

/*
 * WHERE MEM.CSV LIVES - banco.c's two calls to the disk, answered here.
 *
 * Upstream keeps its memory channels in MEM.CSV on the DEEPSDR's SPI
 * flash disk (banco.c, banco.h: "DONDE VIVE"), and banco.c reaches the
 * disk through exactly two calls, spi_flash_read_file_by_name() and
 * spi_flash_write_or_update_file(). banco.c is compiled in place,
 * unmodified, with those two renamed to memfram_*() (reglas.mk
 * MEMFRAM_RENAME), and this file answers them.
 *
 * UNTIL STEP 67 (22/11 - 27/11/2026) the answer was the FRAM: this board
 * had no disk, and 0x4200 held MEM.CSV in two slots of 3 kB - each a
 * header (magic, sequence, length, CRC-32 of the data) and the data, a
 * write to the slot NOT holding the newest, data first and header last,
 * read back; a read takes the newest slot whose CRC checks.
 *
 * NOW IT IS THE DISK, upstream's place, for upstream's reasons: the file
 * opens in the PC's notepad, is corrected there, copied to another radio.
 * The FRAM's copy is MOVED to the disk once - the first boot that finds
 * the disk without MEM.CSV and the FRAM with one writes it there, reads it
 * back, and only then marks the FRAM slots moved (their magic changed:
 * the bytes stay, for SWD, but nothing reads them). From then on the
 * file on the disk is the truth: deleted on the PC, the bank is empty,
 * as upstream's would be - an old copy coming back from the FRAM would
 * be a surprise.
 *
 * The FRAM stays the answer when there is no disk (no chip, or a volume
 * this driver does not understand), and for the boot whose move failed.
 *
 * A WRITE TO THE DISK IS REFUSED - the memory stays in RAM, banco.c's
 * table, and the caller beeps - while the PC has the disk (it owns the
 * FAT then) and while a picture is being saved (upstream's rule, its
 * main.c before banco_guarda(): "mientras haya una foto en curso ... la
 * memoria se queda en RAM"). memfram_porque() says which.
 */
#define MEMF_ADDR     0x4200U
#define MEMF_SLOT     0x0C00U                   /* 3 kB */
#define MEMF_MAGIC    0x4D454D4DU               /* "MMEM" */
#define MEMF_MOVIDO   0x564D454DU               /* "MEMV": moved to the disk */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t crc;
} memf_hdr_t;

#define MEMF_DATA_MAX (MEMF_SLOT - sizeof(memf_hdr_t))

/* the names banco.c calls, checked against upstream's prototypes */
uint32_t memfram_read_file_by_name(const char name8[8], const char ext3[3],
                                   uint8_t *out_buf, uint32_t out_buf_size);
int      memfram_write_or_update_file(const char name8[8], const char ext3[3],
                                      const uint8_t *data, uint32_t len);
_Static_assert(__builtin_types_compatible_p(__typeof__(&spi_flash_read_file_by_name),
                                            __typeof__(&memfram_read_file_by_name)),
               "banco.c's read is renamed to memfram_read_file_by_name()");
_Static_assert(__builtin_types_compatible_p(__typeof__(&spi_flash_write_or_update_file),
                                            __typeof__(&memfram_write_or_update_file)),
               "banco.c's write is renamed to memfram_write_or_update_file()");

static int         s_slot = -1;
static uint32_t    s_seq;
static uint8_t     s_donde;                     /* MEMF_DONDE_* */
static const char *s_porque;

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

/* ------------------------------------------------------------------ */
/* the FRAM's two slots                                                 */
/* ------------------------------------------------------------------ */

static uint32_t fram_lee(uint8_t *out_buf, uint32_t out_buf_size)
{
    memf_hdr_t h[2];
    int        ok[2], pick, i;

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

static int fram_escribe(const uint8_t *data, uint32_t len)
{
    memf_hdr_t h, back;
    int        slot = (s_slot == 0) ? 1 : 0;
    uint32_t   a;

    if (len == 0U || len > MEMF_DATA_MAX) { return 0; }

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

/* Both slots marked moved: the magic only, the rest left as it was. */
static void fram_suelta(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        uint32_t m;

        fram_read(slot_addr(i), (uint8_t *)&m, sizeof m);
        if (m == MEMF_MAGIC) {
            m = MEMF_MOVIDO;
            fram_write(slot_addr(i), (const uint8_t *)&m, sizeof m);
        }
    }
    s_slot = -1;
    s_seq  = 0U;
}

/* ------------------------------------------------------------------ */
/* banco.c's two calls                                                  */
/* ------------------------------------------------------------------ */

uint32_t memfram_read_file_by_name(const char name8[8], const char ext3[3],
                                   uint8_t *out_buf, uint32_t out_buf_size)
{
    uint32_t n, c;

    if (!es_mem_csv(name8, ext3)) { return 0U; }

    if (!spi_flash_puede_escribir()) {
        s_donde = MEMF_DONDE_FRAM;      /* no disk: the FRAM, as before */
        return fram_lee(out_buf, out_buf_size);
    }
    s_donde = MEMF_DONDE_DISCO;
    if (spi_flash_fichero_hay(name8, ext3)) {
        return spi_flash_read_file_by_name(name8, ext3, out_buf, out_buf_size);
    }

    /* The disk without MEM.CSV: the FRAM's, if it has one, goes there. */
    n = fram_lee(out_buf, out_buf_size);
    if (n == 0U) { return 0U; }         /* none anywhere: an empty bank */
    c = crc32_add(0xFFFFFFFFU, out_buf, n);
    guarda_sello();
    if (spi_flash_write_or_update_file(name8, ext3, out_buf, n)
        && spi_flash_read_file_by_name(name8, ext3, out_buf, out_buf_size) == n
        && crc32_add(0xFFFFFFFFU, out_buf, n) == c) {
        fram_suelta();
        console_printf("MEM.CSV   : %u bytes moved from the FRAM to the disk "
                       "(the FRAM's copy is not read any more)\n", n);
        return n;
    }

    /* Not there whole: no half file left behind, and the FRAM's for now. */
    (void)spi_flash_fichero_borra(name8, ext3);
    console_printf("*** MEM.CSV: could not be moved to the disk - from the "
                   "FRAM this time ***\n");
    s_donde = MEMF_DONDE_FRAM;
    return fram_lee(out_buf, out_buf_size);
}

int memfram_write_or_update_file(const char name8[8], const char ext3[3],
                                 const uint8_t *data, uint32_t len)
{
    s_porque = 0;
    if (!es_mem_csv(name8, ext3)) { return 0; }
    if (s_donde == MEMF_DONDE_NINGUNO) {
        s_donde = spi_flash_puede_escribir() ? MEMF_DONDE_DISCO : MEMF_DONDE_FRAM;
    }
    if (s_donde == MEMF_DONDE_FRAM) {
        if (fram_escribe(data, len)) { return 1; }
        s_porque = "the FRAM did not read it back";
        return 0;
    }

    if (usbdisco_activo()) { s_porque = "the disk is on the PC"; return 0; }
    if (guarda_grabando()) { s_porque = "a picture is being saved"; return 0; }
    guarda_sello();                     /* the file's time: when it was written */
    if (!spi_flash_write_or_update_file(name8, ext3, data, len)) {
        s_porque = "the disk refused it";
        return 0;
    }
    return 1;
}

/* For the console. */
uint8_t     memfram_donde(void)   { return s_donde; }
const char *memfram_porque(void)  { return s_porque ? s_porque : ""; }
const char *memfram_donde_txt(void)
{
    return (s_donde == MEMF_DONDE_DISCO) ? "the disk"
         : (s_donde == MEMF_DONDE_FRAM)  ? "the FRAM" : "nowhere yet";
}
