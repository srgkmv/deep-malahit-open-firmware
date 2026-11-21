#include <string.h>

#include "settings.h"
#include "fram.h"

/*
 * One slot: a 24-byte record, padded to 32 so the slot boundaries are
 * round numbers in a hex dump. Little-endian, as the core stores it -
 * nothing but this firmware reads it.
 *
 * VERSION 1 (21/11/2026, b2534c3) was 20 bytes: the same first 16, and
 * its CRC at offset 16 - where version 2 keeps rf_gr and preamp. Both
 * are read: a radio that has only a version-1 record comes back on its
 * dial, volume and step, with the RF front end measured as before, and
 * its first save writes version 2.
 */
#define SLOT_BYTES      32U
#define REC_MAGIC       0x31484C4DU     /* "MLH1" */
#define REC_VERSION     2U

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t dial_hz;
    int8_t   vol_db;
    uint8_t  step_idx;
    uint8_t  version;
    uint8_t  rsv;
    uint8_t  rf_gr;         /* v2; v1's CRC started here */
    uint8_t  preamp;        /* v2 */
    uint8_t  rsv2[2];
    uint32_t crc;           /* v2: CRC-32 of the 20 bytes above */
} rec_t;

_Static_assert(sizeof(rec_t) == 24U, "the record is 24 bytes on the FRAM");

static int      s_slot = -1;            /* where the newest record is */
static uint32_t s_seq;
static uint32_t s_version;
static uint32_t s_save_errors;

uint32_t settings_version(void)     { return s_version; }

int      settings_slot(void)        { return s_slot; }
uint32_t settings_seq(void)         { return s_seq; }
uint32_t settings_save_errors(void)  { return s_save_errors; }

static uint32_t crc32_of(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFU;

    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (uint32_t k = 0; k < 8U; k++) {
            c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
        }
    }
    return ~c;
}

static int rec_ok(const rec_t *r)
{
    if (r->magic != REC_MAGIC) { return 0; }

    if (r->version == 2U) {
        return r->crc == crc32_of((const uint8_t *)r, 20U);
    }
    if (r->version == 1U) {
        const uint8_t *b = (const uint8_t *)r;
        uint32_t crc1 = (uint32_t)b[16] | ((uint32_t)b[17] << 8)
                      | ((uint32_t)b[18] << 16) | ((uint32_t)b[19] << 24);

        return crc1 == crc32_of(b, 16U);
    }
    return 0;
}

static uint32_t slot_addr(int slot)
{
    return SETTINGS_ADDR + (uint32_t)slot * SLOT_BYTES;
}

int settings_load(settings_t *out)
{
    rec_t r[2];

    /* 15 MHz: inside the part's 20, and the probe in main.c
     * (test_fram()) showed nothing in this footprint but FRAM. */
    fram_init(3U);

    fram_read(slot_addr(0), (uint8_t *)&r[0], sizeof r[0]);
    fram_read(slot_addr(1), (uint8_t *)&r[1], sizeof r[1]);

    int ok0 = rec_ok(&r[0]);
    int ok1 = rec_ok(&r[1]);

    if (!ok0 && !ok1) {
        s_slot    = -1;
        s_seq     = 0U;
        s_version = 0U;
        return 0;
    }

    /* Newer by sequence, compared the way that survives a wrap. */
    int pick = (!ok0) ? 1
             : (!ok1) ? 0
             : ((int32_t)(r[1].seq - r[0].seq) > 0) ? 1 : 0;

    s_slot    = pick;
    s_seq     = r[pick].seq;
    s_version = r[pick].version;

    out->dial_hz  = r[pick].dial_hz;
    out->vol_db   = r[pick].vol_db;
    out->step_idx = r[pick].step_idx;
    out->has_rf   = (r[pick].version >= 2U);
    out->rf_gr    = out->has_rf ? r[pick].rf_gr  : 0U;
    out->preamp   = out->has_rf ? r[pick].preamp : 0U;
    return 1;
}

int settings_save(const settings_t *s)
{
    rec_t w, back;
    int   slot = (s_slot == 0) ? 1 : 0;     /* the other one */

    memset(&w, 0, sizeof w);
    w.magic    = REC_MAGIC;
    w.seq      = s_seq + 1U;
    w.dial_hz  = s->dial_hz;
    w.vol_db   = s->vol_db;
    w.step_idx = s->step_idx;
    w.version  = REC_VERSION;
    w.rf_gr    = s->rf_gr;
    w.preamp   = s->preamp;
    w.crc      = crc32_of((const uint8_t *)&w, 20U);

    fram_write(slot_addr(slot), (const uint8_t *)&w, sizeof w);
    fram_read(slot_addr(slot), (uint8_t *)&back, sizeof back);

    if (memcmp(&w, &back, sizeof w) != 0) {
        /* The old slot still holds the last good record, and s_slot
         * still points at it: the next save tries this slot again. */
        s_save_errors++;
        return 0;
    }

    s_slot    = slot;
    s_seq     = w.seq;
    s_version = REC_VERSION;
    return 1;
}
