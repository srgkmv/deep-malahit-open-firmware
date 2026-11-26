#ifndef MALAHIT_USB_MALAHIT_H
#define MALAHIT_USB_MALAHIT_H

/*
 * FORCE-INCLUDED (-include) INTO GIGADEVICE'S USB LIBRARY AND UPSTREAM'S
 * boot/disco_msc.c, and only there (reglas.mk, step 60, 26/11/2026).
 *
 * The library drives a Synopsys DWC2 core, and the STM32H743's OTG_FS is
 * the same core: the registers, their offsets and the FIFO windows match.
 * What differs is where the core sits, where the chip keeps its serial
 * number, and what three bits of GCCFG mean - and upstream's tree now has
 * an #ifndef at each of those (drv_usb_regs.h, usbd_enum.h,
 * drv_usb_core.c; PROCEDENCIA.md), defaulting to the GD32's. This file is
 * where the port says the H7's.
 *
 * Like compat/gd32f4xx.h it declares only what the files it is forced
 * into need. Anything else the library reaches for fails to compile.
 */

#include <stddef.h>         /* NULL: the GD32's headers bring it, ST's not */
#include <stdint.h>
#include "stm32h7xx.h"

/* USB2_OTG_FS, the one on PA11/PA12 - the DFU connector, J3. */
#define USBFS_REG_BASE  USB2_OTG_FS_PERIPH_BASE        /* 0x40080000 */

/* The serial string (usbd_enum.c, serial_string_get()) from the H7's
 * 96-bit unique ID - the "unique id" line of the boot log. */
#define DEVICE_ID1      (UID_BASE)
#define DEVICE_ID2      (UID_BASE + 4UL)
#define DEVICE_ID3      (UID_BASE + 8UL)
#define DEVICE_ID       (UID_BASE)

/*
 * THE PHY ON, H7 STYLE (usb_core_init()). In the H7's GCCFG:
 *
 *   bit 16  PWRDWN  1 = transceiver on - the GD32's PWRON, same bit;
 *   bit 18  PDEN, bit 19 SDEN - battery-charger detection, which takes
 *           D+ and D- over: the GD32's VBUS comparators there, so its
 *           value would switch the line off;
 *   bit 21  VBDEN   1 = SENSE VBUS - the GD32's "ignore VBUS" there.
 *
 * VBUS reaches PA9 only through JP2 (BOARD.md), not known to be fitted,
 * so it is not sensed: the B-session is forced valid in GOTGCTL
 * (BVALOEN bit 6, BVALOVAL bit 7, the library's GOTGCS) - what ST's own
 * driver does without VBUS sensing. Written after the core's soft reset,
 * which the library has just done; the reset leaves GOTGCTL's override
 * alone from here on.
 */
#define USB_PHY_ENCIENDE(regs)                                  \
    do {                                                        \
        (regs)->gr->GCCFG   = (1UL << 16);                      \
        (regs)->gr->GOTGCS |= (1UL << 6) | (1UL << 7);          \
    } while (0)

/*
 * AND WAITING FOR THE MODE (usbd_init(), after usb_curmode_set()).
 * Forcing device mode takes the DWC2 core up to 25 ms, and device
 * registers written before it has switched are dropped, with "mode
 * mismatch" (GINTSTS bit 1) raised. The H7's core comes out of reset as
 * a HOST - PA10, the ID pin, is not an input of the core here - so the
 * library's speed in DCFG and its connect in DCTL were both lost and the
 * PC saw nothing (26/11/2026: 0x08100000 and 0x00000002 read back; set by
 * hand over SWD, the disk enumerated at once). The library's GINTF is
 * GINTSTS; bit 0, CMOD, is 0 in device mode. src/usbdisco.c waits for it,
 * as ST's driver does, up to 50 ms.
 */
void usbdisco_espera_modo(volatile uint32_t *gintsts);
#define USB_MODO_ESPERA(regs)   usbdisco_espera_modo(&(regs)->gr->GINTF)

/* The GD32 header's own definitions (CMSIS/GD/GD32F4xx/Include/
 * gd32f4xx.h:299-301), which the library's register header leans on
 * throughout. */
#ifndef BIT
#define BIT(x)  ((uint32_t)((uint32_t)0x01U << (x)))
#endif
#ifndef BITS
#define BITS(start, end) \
    ((0xFFFFFFFFUL << (start)) & (0xFFFFFFFFUL >> (31U - (uint32_t)(end))))
#endif

/*
 * Deep sleep on USB suspend: called only when the core's low_power is
 * set, and usb_conf.h sets USBFS_LOW_POWER 0. Declared so the two call
 * sites compile; src/usbdisco.c's definition reports and returns.
 */
#define PMU_LDO_LOWPOWER        0U
#define PMU_LOWDRIVER_DISABLE   0U
#define WFI_CMD                 ((uint8_t)0x00U)
void pmu_to_deepsleepmode(uint32_t ldo, uint32_t lowdrive, uint8_t deepsleepmodecmd);

#endif /* MALAHIT_USB_MALAHIT_H */
