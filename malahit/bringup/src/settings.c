#include <string.h>

#include "settings.h"
#include "fram.h"

/*
 * VERSIONS 1-3, at 0x4000 - READ ONLY since version 4 (below), which is
 * what this firmware writes. Kept so a radio comes back on its last
 * record the first time it boots a version-4 image.
 *
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

/*
 * VERSION 4 (21/11/2026, late night) - and the end of squeezing.
 *
 * Everything above was shaped by one rule: the firmware BEFORE must still
 * read what this one writes, because this project loads older images over
 * SWD every hour. That is why the settings screen went into an extension
 * the older image does not see, why squelch and NR went into two spare
 * bytes under a flag bit, and why the smoothing would have had three
 * bits left. The owner dropped the rule the same night: a record an
 * older image cannot read is fine - it boots on defaults.
 *
 * So version 4 is a record of its own, laid out plainly - a byte a
 * value, as the modules hold them - in 64-byte slots at a NEW address,
 * 0x4100, in the same blank stretch of the FRAM. The two old slots at
 * 0x4000 are only READ now: a radio whose newest record is version 1-3
 * boots on it exactly as before (the code above), and its first save
 * writes version 4, continuing the sequence number. Nothing ever writes
 * the old slots again, so going back to an older image finds its last
 * record there, stale but whole.
 *
 *   0  magic "MLH4"   4 seq   8 dial_hz
 *  12  version 4      13 vol_db   14 step_idx   15 mode
 *  16  rf_gr  17 preamp  18 AGC profile  19 spectrum style
 *  20  SET_AJ_* flags  21 theme  22 waterfall palette  23 filter presets
 *  24  squelch (dB + 10) / 2   25 NR strength / 16   26 smoothing, %
 *  27  WFM audio low-pass: upstream's audio_bw_t + 1, 0 = not stored
 *  28  language: IDIOMA_ES / IDIOMA_EN + 1, 0 = not stored (Spanish)
 *  29  backlight, % (20-100), 0 = not stored (100 %)
 *  30  FM stereo: 1 mono, 2 auto; 0 = not stored (auto)
 *  31..59  zero - room. A field added there reads 0 from a record
 *          written before it, so it must mean "not stored" at 0, or
 *          the version goes to 5 (the owner's word allows it).
 *  60  CRC-32 of bytes 0-59
 *
 * Two slots and the newest valid one, as before; 64 bytes are 35 us of
 * SPI at 15 MHz, so a power cut mid-write is still the rare case the
 * second slot is there for.
 */
#define V4_ADDR     0x4100U
#define V4_BYTES    64U
#define V4_MAGIC    0x34484C4DU         /* "MLH4" */
#define V4_VERSION  4U

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t dial_hz;
    uint8_t  version;
    int8_t   vol_db;
    uint8_t  step_idx;
    uint8_t  mode;
    uint8_t  rf_gr;
    uint8_t  preamp;
    uint8_t  agc;
    uint8_t  estilo;
    uint8_t  flags;
    uint8_t  tema;
    uint8_t  paleta;
    uint8_t  bw;
    uint8_t  sql;
    uint8_t  nr;
    uint8_t  suav;
    uint8_t  wfm_bw;        /* audio_bw_t + 1; 0 = written before it */
    uint8_t  idioma;        /* IDIOMA_* + 1; 0 = written before it */
    uint8_t  brillo;        /* %; 0 = written before it */
    uint8_t  estereo;       /* 1 mono, 2 auto; 0 = written before it */
    uint8_t  rsv[29];
    uint32_t crc;
} rec4_t;

_Static_assert(sizeof(rec4_t) == V4_BYTES, "a version-4 slot is 64 bytes");

static int      s_slot = -1;            /* where the newest record is */
static int      s_v4_slot = -1;         /* ...if it is a version-4 one */
static uint32_t s_seq;
static uint32_t s_version;
static uint32_t s_addr;
static uint32_t s_save_errors;

uint32_t settings_version(void)     { return s_version; }
uint32_t settings_addr(void)        { return s_addr; }

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

/* A version 1-3 record at 0x4000, as the firmware before version 4 wrote
 * it. Read only. */
static int legacy_load(settings_t *out)
{
    slot_t sl[2];
    rec_t  r[2];

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
    s_addr    = slot_addr(pick);

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

static int rec4_ok(const rec4_t *r)
{
    return r->magic == V4_MAGIC && r->version == V4_VERSION
        && r->crc == crc32_of((const uint8_t *)r, V4_BYTES - 4U);
}

static uint32_t v4_addr(int slot)
{
    return V4_ADDR + (uint32_t)slot * V4_BYTES;
}

int settings_load(settings_t *out)
{
    rec4_t r[2];

    memset(out, 0, sizeof *out);

    /* 15 MHz: inside the part's 20, and the probe in main.c
     * (test_fram()) showed nothing in this footprint but FRAM. */
    fram_init(3U);

    fram_read(v4_addr(0), (uint8_t *)&r[0], sizeof r[0]);
    fram_read(v4_addr(1), (uint8_t *)&r[1], sizeof r[1]);

    int ok0 = rec4_ok(&r[0]);
    int ok1 = rec4_ok(&r[1]);

    if (!ok0 && !ok1) {
        s_v4_slot = -1;
        return legacy_load(out);        /* the radio's first version-4 boot */
    }

    int pick = (!ok0) ? 1
             : (!ok1) ? 0
             : ((int32_t)(r[1].seq - r[0].seq) > 0) ? 1 : 0;
    const rec4_t *p = &r[pick];

    s_v4_slot = pick;
    s_slot    = pick;
    s_seq     = p->seq;
    s_version = V4_VERSION;
    s_addr    = v4_addr(pick);

    out->dial_hz   = p->dial_hz;
    out->vol_db    = p->vol_db;
    out->step_idx  = p->step_idx;
    out->mode      = p->mode;
    out->rf_gr     = p->rf_gr;
    out->preamp    = p->preamp;
    out->aj_agc    = p->agc;
    out->aj_estilo = p->estilo;
    out->aj_flags  = p->flags;
    out->aj_tema   = p->tema;
    out->aj_paleta = p->paleta;
    out->aj_bw     = p->bw;
    out->aj_sql    = p->sql;
    out->aj_nr     = p->nr;
    out->aj_suav   = p->suav;
    out->aj_wfm_bw = p->wfm_bw;
    out->aj_idioma = p->idioma;
    out->aj_brillo = p->brillo;
    out->aj_estereo = p->estereo;
    out->has_rf = out->has_mode = out->has_aj = 1U;
    out->has_sqlnr = out->has_suav = 1U;
    return 1;
}

int settings_save(const settings_t *s)
{
    rec4_t w, back;
    int    slot = (s_v4_slot == 0) ? 1 : 0;   /* the other one */

    memset(&w, 0, sizeof w);
    w.magic    = V4_MAGIC;
    w.seq      = s_seq + 1U;                  /* on from a v1-3 record too */
    w.dial_hz  = s->dial_hz;
    w.version  = V4_VERSION;
    w.vol_db   = s->vol_db;
    w.step_idx = s->step_idx;
    w.mode     = s->mode;
    w.rf_gr    = s->rf_gr;
    w.preamp   = s->preamp;
    w.agc      = s->aj_agc;
    w.estilo   = s->aj_estilo;
    w.flags    = s->aj_flags;
    w.tema     = s->aj_tema;
    w.paleta   = s->aj_paleta;
    w.bw       = s->aj_bw;
    w.sql      = s->aj_sql;
    w.nr       = s->aj_nr;
    w.suav     = s->aj_suav;
    w.wfm_bw   = s->aj_wfm_bw;
    w.idioma   = s->aj_idioma;
    w.brillo   = s->aj_brillo;
    w.estereo  = s->aj_estereo;
    w.crc      = crc32_of((const uint8_t *)&w, V4_BYTES - 4U);

    fram_write(v4_addr(slot), (const uint8_t *)&w, sizeof w);
    fram_read(v4_addr(slot), (uint8_t *)&back, sizeof back);

    if (memcmp(&w, &back, sizeof w) != 0) {
        /* The other slot still holds the last good record, and
         * s_v4_slot still points at it: the next save tries this slot
         * again. */
        s_save_errors++;
        return 0;
    }

    s_v4_slot = slot;
    s_slot    = slot;
    s_seq     = w.seq;
    s_version = V4_VERSION;
    s_addr    = v4_addr(slot);
    return 1;
}
