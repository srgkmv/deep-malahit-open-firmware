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
 *
 * VERSION 3 (21/11/2026, step 9) is version 2 plus the demodulator's
 * mode in the byte that was `rsv`, and a different MEANING for step_idx:
 * an index into upstream's eleven steps instead of this port's first
 * five. The bytes are the same either way; main.c reads
 * settings_version() and maps an older index through its hertz. Older
 * firmware rejects a version-3 record and boots on defaults - it does
 * not misread it.
 *
 * THE EXTENSION (21/11/2026, the settings screen) is the slot's last 8
 * bytes, past the 24 of the record, and the version byte STAYS 3. That is
 * the point: firmware that knows version 3 and nothing more reads and
 * checks its 24 bytes as always and never looks further, so loading an
 * older image - which this project does over SWD every hour - keeps the
 * dial, the mode and the rest. A version 4 would have sent it to
 * defaults, which is exactly what version 3 did to the image before it.
 *
 *   24  ext_ver  1
 *   25  AGC profile (bits 0-1) | spectrum style (bits 2-3) | EXT_SQLNR
 *       (bit 7: bytes 18-19, squelch and NR strength, are values)
 *   26  SET_AJ_* flags: blanker, notch, NR, bridge, white trace
 *   27  theme index      28  waterfall palette
 *   29  filter presets, 2 bits a family
 *   30  CRC-32 of bytes 0-29, low 16 bits
 *
 * And two values that did not fit there live in the base record's two
 * spare bytes (18, 19: squelch and NR strength). Version-3 firmware
 * writes them as zeros and never reads them; they count only when the
 * extension checks, and the extension's check covers them - so a base
 * rewritten by an older image cannot hand over stale ones.
 *
 * The check covers the base record too, so an extension left behind by
 * a newer image under a base an OLDER image has since rewritten (it
 * writes 24 bytes, not 32) fails, and is ignored - its settings were
 * about another record.
 */
#define SLOT_BYTES      32U
#define REC_MAGIC       0x31484C4DU     /* "MLH1" */
#define REC_VERSION     3U

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t dial_hz;
    int8_t   vol_db;
    uint8_t  step_idx;
    uint8_t  version;
    uint8_t  mode;          /* v3: upstream's demod_mode_t; v1/v2: 0 */
    uint8_t  rf_gr;         /* v2; v1's CRC started here */
    uint8_t  preamp;        /* v2 */
    uint8_t  sql;           /* the base's two spare bytes, written as 0 by */
    uint8_t  nr;            /* v3 firmware: squelch and NR strength, read
                               ONLY when the extension checks - see below */
    uint32_t crc;           /* v2: CRC-32 of the 20 bytes above */
} rec_t;

_Static_assert(sizeof(rec_t) == 24U, "the record is 24 bytes on the FRAM");

typedef struct {
    rec_t    base;
    uint8_t  ext_ver;
    uint8_t  agc_estilo;
    uint8_t  flags;
    uint8_t  tema;
    uint8_t  paleta;
    uint8_t  bw;
    uint16_t check;
} slot_t;

_Static_assert(sizeof(slot_t) == SLOT_BYTES, "the extension fills the slot");

/* 21/11/2026, the settings screen. It STAYS 1 when squelch and NR were
 * added: 705749d reads only an extension of exactly version 1, and a 2
 * would have cost it the whole settings screen. EXT_SQLNR says instead. */
#define EXT_VERSION  1U
/*
 * Squelch and NR strength live in the base's two spare bytes, which an
 * extension written before them also covers - as ZEROS. This bit, in
 * the AGC/style byte's unused top half (705749d masks it away), says
 * they were written as values.
 */
#define EXT_SQLNR    0x80U

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

    if (r->version == 2U || r->version == 3U) {
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

static uint16_t ext_check(const slot_t *sl)
{
    return (uint16_t)crc32_of((const uint8_t *)sl, 30U);
}

int settings_load(settings_t *out)
{
    slot_t sl[2];
    rec_t  r[2];

    /* 15 MHz: inside the part's 20, and the probe in main.c
     * (test_fram()) showed nothing in this footprint but FRAM. */
    fram_init(3U);

    fram_read(slot_addr(0), (uint8_t *)&sl[0], sizeof sl[0]);
    fram_read(slot_addr(1), (uint8_t *)&sl[1], sizeof sl[1]);
    r[0] = sl[0].base;
    r[1] = sl[1].base;

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
    out->has_mode = (r[pick].version >= 3U);
    out->mode     = out->has_mode ? r[pick].mode : 0U;

    {
        const slot_t *e = &sl[pick];

        out->has_aj = (uint8_t)(r[pick].version >= 3U
                                && e->ext_ver == EXT_VERSION
                                && e->check == ext_check(e));
        out->has_sqlnr = (uint8_t)(out->has_aj && (e->agc_estilo & EXT_SQLNR));
        out->aj_agc    = (uint8_t)(e->agc_estilo & 3U);
        out->aj_estilo = (uint8_t)((e->agc_estilo >> 2) & 3U);
        out->aj_flags  = e->flags;
        out->aj_tema   = e->tema;
        out->aj_paleta = e->paleta;
        out->aj_bw     = e->bw;
        out->aj_sql    = r[pick].sql;
        out->aj_nr     = r[pick].nr;
    }
    return 1;
}

int settings_save(const settings_t *s)
{
    slot_t ws, back;
    rec_t  w;
    int   slot = (s_slot == 0) ? 1 : 0;     /* the other one */

    memset(&w, 0, sizeof w);
    w.magic    = REC_MAGIC;
    w.seq      = s_seq + 1U;
    w.dial_hz  = s->dial_hz;
    w.vol_db   = s->vol_db;
    w.step_idx = s->step_idx;
    w.version  = REC_VERSION;
    w.mode     = s->mode;
    w.rf_gr    = s->rf_gr;
    w.preamp   = s->preamp;
    w.sql      = s->aj_sql;
    w.nr       = s->aj_nr;
    w.crc      = crc32_of((const uint8_t *)&w, 20U);

    memset(&ws, 0, sizeof ws);
    ws.base       = w;
    ws.ext_ver    = EXT_VERSION;
    ws.agc_estilo = (uint8_t)((s->aj_agc & 3U) | ((s->aj_estilo & 3U) << 2)
                              | EXT_SQLNR);
    ws.flags      = s->aj_flags;
    ws.tema       = s->aj_tema;
    ws.paleta     = s->aj_paleta;
    ws.bw         = s->aj_bw;
    ws.check      = ext_check(&ws);

    fram_write(slot_addr(slot), (const uint8_t *)&ws, sizeof ws);
    fram_read(slot_addr(slot), (uint8_t *)&back, sizeof back);

    if (memcmp(&ws, &back, sizeof ws) != 0) {
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
