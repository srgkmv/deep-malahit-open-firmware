#ifndef MALAHIT_USBDISCO_H
#define MALAHIT_USBDISCO_H

#include <stdint.h>

/*
 * THE USB DISK (step 60): the SPI flash's FAT12 volume - 7 MB of the
 * W25Q64 - as a USB mass-storage device on J3, the micro-B connector the
 * ROM's DFU uses (OTG_FS, PA11/PA12).
 *
 * All of it is upstream's but the hardware: GigaDevice's USB library as
 * upstream carries it (usb/GD32F4xx_usb_library - the DWC2 driver, the
 * device core, the MSC class with its SCSI), and upstream's bootloader's
 * glue to the disk, boot/disco_msc.c, which hands every request to
 * User/disco.c. On the DEEPSDR the disk lives in the bootloader; this
 * radio's bootloader is ST's ROM, so here it is a mode of the
 * application: switched on and off, and the radio keeps playing.
 *
 * The class reads and writes the flash FROM THE USB INTERRUPT, as it does
 * in upstream's bootloader. That interrupt is the lowest priority in the
 * system (audio, SysTick, the panel and the console preempt it), and it
 * shares SPI2 with the FRAM, which the main loop uses: fram_bus_irq()
 * tells fram.c to mask it around every FRAM transaction.
 *
 * While it is on, the volume belongs to the PC: nothing in the radio
 * writes to it. Switching off detaches from the bus - eject on the PC
 * first, or its unflushed writes are lost - and makes upstream's driver
 * forget what it knew of the volume (spi_flash_geo_olvida()).
 */

void    usbdisco_abre(void);
void    usbdisco_cierra(void);
uint8_t usbdisco_activo(void);

/* On, and a host has configured it (the class's SET_CONFIGURATION). */
uint8_t usbdisco_con_pc(void);

/* Over SWD: interrupts served, the device's state, the class's
 * reads/writes/failures and the last block. */
void    usbdisco_report(void);

#endif /* MALAHIT_USBDISCO_H */
