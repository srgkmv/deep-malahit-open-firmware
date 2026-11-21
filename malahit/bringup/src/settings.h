#ifndef MALAHIT_SETTINGS_H
#define MALAHIT_SETTINGS_H

#include <stdint.h>

/*
 * WHAT THE RADIO REMEMBERS ACROSS A POWER-OFF, kept in the FRAM (fram.h).
 *
 * The dial, the volume, the tuning step, and since record version 2
 * the RF front end as the person left it: the gain reduction the ATT
 * pill shows (the small knob in RF mode) and the preamp. Not the small
 * knob's mode, which a person expects back on volume.
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
} settings_t;

#define SETTINGS_ADDR   0x4000U

/*
 * Bring up the FRAM (SPI2 at 15 MHz) and read both slots. Returns 1 and
 * fills `out` with the newest valid record, or 0 if neither checks -
 * blank FRAM the first time, which is the normal case once.
 */
int settings_load(settings_t *out);

/* The record version the newest slot was written in (1 or 2), 0 if
 * none - for the boot line. */
uint32_t settings_version(void);

/*
 * Write `s` to the other slot and read it back - always as version 2,
 * `has_rf` ignored. Returns 1 if the read-back is what was written.
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
