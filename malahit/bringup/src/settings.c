#include <string.h>

#include "settings.h"
#include "fram.h"

/*
 * One slot: 20 bytes of record, padded to 32 so the slot boundaries are
 * round numbers in a hex dump. Little-endian, as the core stores it -
 * nothing but this firmware reads it.
 */
#define SLOT_BYTES      32U
#define REC_MAGIC       0x31484C4DU     /* "MLH1" */
#define REC_VERSION     1U

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t dial_hz;
    int8_t   vol_db;
    uint8_t  step_idx;
    uint8_t  version;
    uint8_t  rsv;
    uint32_t crc;           /* CRC-32 of everything above */
} rec_t;

_Static_assert(sizeof(rec_t) == 20U, "the record is 20 bytes on the FRAM");

static int      s_slot = -1;            /* where the newest record is */
static uint32_t s_seq;
static uint32_t s_save_errors;

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
    return r->magic == REC_MAGIC
        && r->version == REC_VERSION
        && r->crc == crc32_of((const uint8_t *)r, sizeof *r - 4U);
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
        s_slot = -1;
        s_seq  = 0U;
        return 0;
    }

    /* Newer by sequence, compared the way that survives a wrap. */
    int pick = (!ok0) ? 1
             : (!ok1) ? 0
             : ((int32_t)(r[1].seq - r[0].seq) > 0) ? 1 : 0;

    s_slot = pick;
    s_seq  = r[pick].seq;

    out->dial_hz  = r[pick].dial_hz;
    out->vol_db   = r[pick].vol_db;
    out->step_idx = r[pick].step_idx;
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
    w.crc      = crc32_of((const uint8_t *)&w, sizeof w - 4U);

    fram_write(slot_addr(slot), (const uint8_t *)&w, sizeof w);
    fram_read(slot_addr(slot), (uint8_t *)&back, sizeof back);

    if (memcmp(&w, &back, sizeof w) != 0) {
        /* The old slot still holds the last good record, and s_slot
         * still points at it: the next save tries this slot again. */
        s_save_errors++;
        return 0;
    }

    s_slot = slot;
    s_seq  = w.seq;
    return 1;
}
