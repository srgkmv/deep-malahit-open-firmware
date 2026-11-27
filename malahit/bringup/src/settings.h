#ifndef MALAHIT_SETTINGS_H
#define MALAHIT_SETTINGS_H

#include <stdint.h>

/*
 * WHAT THE RADIO REMEMBERS ACROSS A POWER-OFF, kept in the FRAM (fram.h).
 *
 * The dial, the volume, the tuning step, since record version 2 the
 * RF front end as the person left it - the gain reduction the ATT pill
 * shows (the small knob in RF mode) and the preamp - and since version
 * 3 the demodulator's mode. Not the small knob's mode, which a person
 * expects back on volume; not the band, which the dial already says.
 *
 * The RF pair was left out of version 1 on purpose - rf_autorange()
 * measures a gain at every boot - and put in on the owner's word, the
 * same evening: a setting the hand chose comes back as the hand left
 * it. rf_autorange() now runs only when there is nothing to restore.
 *
 * WHERE: 0x4000, in the middle of the 25 kB the factory firmware leaves
 * blank. Read out of this radio on 21/11/2026 (backups/fram_a.bin,
 * outside the repo), the factory firmware's own settings sit in
 * 0x0000-0x0CFF and 0x7000-0x7FFF; nothing here touches either, so a
 * return to the factory image finds its settings as it left them.
 *
 * HOW: two 32-byte slots, each one record with a sequence number and a
 * CRC-32. A save goes to the slot NOT holding the newest record, and a
 * load takes the newest slot that checks. A write cut short by the power
 * going - it takes ~25 us, but the battery can always choose that moment
 * - leaves the other slot whole, and the radio comes back one change
 * behind instead of on defaults.
 *
 * The values are not judged here: a record that checks is handed over
 * as it was stored, and main.c decides whether a dial or a volume is one
 * it can use. This file only promises that the bytes are the ones that
 * were written.
 */

typedef struct {
    uint32_t dial_hz;
    int8_t   vol_db;
    uint8_t  step_idx;
    uint8_t  rf_gr;         /* dB of gain reduction, the ATT pill */
    uint8_t  preamp;        /* 0 / 1 */
    uint8_t  has_rf;        /* 0: loaded from a version-1 record, which
                               had no rf_gr/preamp - leave them alone */
    uint8_t  mode;          /* v3: upstream's demod_mode_t */
    uint8_t  has_mode;      /* 0: a v1/v2 record - no mode, and step_idx
                               indexes the OLD five-step table */

    /*
     * THE SETTINGS SCREEN (src/menus.c, Ajustes), in the slot's last
     * 8 bytes - an EXTENSION to a version-3 record, not a version 4: see
     * settings.c. Values as the modules hold them; menus.c packs and
     * applies them, and range-checks on the way in.
     */
    uint8_t  has_aj;        /* 0: no valid extension - keep defaults */
    uint8_t  aj_agc;        /* agc_profile_t */
    uint8_t  aj_estilo;     /* spectrum_style_t */
    uint8_t  aj_flags;      /* SET_AJ_* below */
    uint8_t  aj_tema;       /* index into the generated theme table */
    uint8_t  aj_paleta;     /* spectrum_palette_t */
    uint8_t  aj_bw;         /* 2 bits a filter family: quick preset 0-2,
                               3 = as the demodulator left it */
    uint8_t  aj_sql;        /* squelch: (dB + 10) / 2, upstream's 2 dB steps */
    uint8_t  aj_nr;         /* NR strength / 16 (0-4080 of upstream's 0-4095) */
    uint8_t  has_sqlnr;     /* the two above are real (settings.c, EXT_SQLNR) */
    uint8_t  aj_suav;       /* v4: the trace's smoothing, % (0-95) */
    uint8_t  has_suav;      /* 0: a v1-3 record - keep the default */
    uint8_t  aj_wfm_bw;     /* v4: WFM's audio low-pass, audio_bw_t + 1;
                               0 = not stored - keep the default */
    uint8_t  aj_idioma;     /* v4: IDIOMA_ES/EN + 1; 0 = not stored (ES) */
    uint8_t  aj_brillo;     /* v4: backlight %, 20-100; 0 = not stored (100) */
    uint8_t  aj_estereo;    /* v4: FM stereo 1 Mono / 2 Auto; 0 = not stored (Auto) */
    uint8_t  aj_bd;         /* v4 byte 45: stations on the spectrum 1 off / 2 on;
                               0 = not stored (off, upstream's default) */
    uint8_t  aj_jmp;        /* v4 byte 46: JMP, the knob station to station
                               1 off / 2 on; 0 = not stored (off) */
    uint8_t  aj_sstv_guarda; /* v4 byte 47: SSTV to the disk, upstream's
                               SSTV_GUARDA_* + 1; 0 = not stored (no) */
    uint8_t  aj_wfx_guarda; /* v4 byte 48: WEFAX to the disk, 1 no / 2 grey
                               BMP; 0 = not stored (no) */
    uint8_t  cw_bw;         /* v4: CW band-pass / 10 Hz; 0 = not stored (500) */
    uint8_t  rtty_shift;    /* v4: RTTY shift / 5 Hz; 0 = not stored (450) */
    uint8_t  rtty_baud;     /* v4: index in upstream's four + 1; 0 = not stored (50) */
    uint8_t  rtty_auto;     /* v4: 1 off, 2 on; 0 = not stored (on) */
    uint8_t  cw_aprende;    /* v4: CW learning, 1 off, 2 on; 0 = not stored (off) */
    uint8_t  wfx_lpm;       /* v4: WEFAX line rate, upstream's index + 1;
                               0 = not stored (its default, 120) */
    uint8_t  huso;          /* v4: the time zone, quarter hours + 64 (1..120);
                               0 = not stored (upstream's +1:00) - step 56 */
    uint8_t  huso_medido;   /* v4: 2 = a station measured it, 1 = a city;
                               0 = not stored */
    char     qth[6];        /* v4: the Maidenhead locator, ASCII, zeros past
                               its end; all zero = not stored (none) - step 57 */
} settings_t;

#define SET_AJ_NB_MASK      0x03U   /* blanker level 0-3 */
#define SET_AJ_NOTCH_F_SH   2U      /* notch strength 0-3, bits 2-3 */
#define SET_AJ_NOTCH_ON     0x10U
#define SET_AJ_NR_ON        0x20U
#define SET_AJ_PUENTE       0x40U
#define SET_AJ_TRAZA_W      0x80U

#define SETTINGS_ADDR   0x4000U     /* versions 1-3; version 4 at 0x4100 */

/*
 * Bring up the FRAM (SPI2 at 15 MHz) and read both slots. Returns 1 and
 * fills `out` with the newest valid record, or 0 if neither checks -
 * blank FRAM the first time, which is the normal case once.
 */
int settings_load(settings_t *out);

/* The record version the newest slot was written in (1-4), 0 if none -
 * for the boot line, and for main.c to know which step table a stored
 * step_idx indexes (3-4: upstream's eleven; 1-2: the old five); and the
 * FRAM address it was read from. */
uint32_t settings_version(void);
uint32_t settings_addr(void);

/*
 * Write `s` to the other slot and read it back - always as version 4,
 * at 0x4100 (settings.c), the `has_*` flags ignored. Returns 1 if the
 * read-back is what was written. Firmware older than version 4 cannot
 * read it and boots on defaults - allowed by the owner, 21/11/2026.
 * Cheap enough to call on every change; main.c calls it a second after
 * the last one.
 */
int settings_save(const settings_t *s);

/* For the console: which slot the newest record is in (0/1, -1 none),
 * its sequence number, and saves that failed to read back. */
int      settings_slot(void);
uint32_t settings_seq(void);
uint32_t settings_save_errors(void);

#endif /* MALAHIT_SETTINGS_H */
