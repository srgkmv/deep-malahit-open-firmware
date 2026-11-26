#ifndef MALAHIT_NOR_H
#define MALAHIT_NOR_H

#include <stdint.h>

/*
 * The SPI NOR flash seen from the port: what is there at boot, and an
 * SWD-callable read bench. The driver is upstream's User/spi_flash.c over
 * src/spi_nor.c (step 60).
 */

/*
 * At boot, after the settings (the FRAM): upstream's spi_flash_init(),
 * then one console block - JEDEC ID, capacity, the chip's serial, status
 * registers 1 and 2 (block protection said loudly if set: the chip then
 * ignores every erase and program), whether there is a volume upstream
 * can write, and upstream's repair of a write a power cut interrupted
 * (spi_flash_respaldo_repara(), a no-op without a volume).
 */
void nor_boot_report(void);

/*
 * Over SWD (tools/swd-call.sh): read `len` bytes from `addr` with
 * upstream's spi_flash_read(), 512 at a time, and print their CRC-32 and
 * the rate. Read only. len 0 = the whole chip.
 */
void nor_bench(uint32_t addr, uint32_t len);

/*
 * Over SWD, the whole chip, one phase a call (each blocks the main loop
 * for 12-25 s; audio carries on): 0 erase everything (upstream's
 * spi_flash_borrado_paso(), 64 kB blocks), 1 check it all reads 0xFF,
 * 2 program a pattern hashed from each word's address and `semilla`,
 * 3 read it back. Mismatches - the first eight, and a count - to the
 * console. DESTROYS THE CHIP'S CONTENTS (phases 0 and 2).
 */
void nor_prueba(uint32_t fase, uint32_t semilla);

/* Over SWD: upstream's formato_haz() - the FAT12 volume on the disk part
 * of the chip, 7 MB on this one - then formato_comprueba(), which writes
 * PRUEBA.TXT through the FAT and reads it back. */
void nor_formatea(void);

#endif /* MALAHIT_NOR_H */
