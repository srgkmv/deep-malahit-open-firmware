#include <string.h>

#include "stm32h7xx.h"
#include "board.h"
#include "clock.h"
#include "console.h"
#include "fram.h"
#include "usbdisco.h"
#include "guarda.h"
#include "spi_flash.h"
#include "disco.h"

/* GigaDevice's library, as upstream carries it (usb/), compiled with
 * compat/usb_malahit.h forced in - this file too, for its types. */
#include "drv_usb_hw.h"
#include "drv_usb_dev.h"
#include "drv_usbd_int.h"
#include "usbd_core.h"
#include "usbd_msc_core.h"

/* boot/disco_msc.c's counters (it says what they are for). */
extern volatile uint32_t g_msc_lee, g_msc_escribe, g_msc_bloque, g_msc_fallo;

static usb_core_driver   s_usb;
static uint8_t           s_activo;
static volatile uint32_t s_isr;
static uint32_t          s_errores;     /* clock steps that timed out */

#define WAIT_LIMIT  2000000U

static void espera(volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
    for (uint32_t n = WAIT_LIMIT; n != 0U; n--) {
        if ((*reg & mask) == want) { return; }
    }
    s_errores++;
}

/* ------------------------------------------------------------------ */
/* drv_usb_hw.h - the four calls upstream's boot/usb_hw.c makes on the  */
/* GD32, and the two delays the library calls.                         */
/* ------------------------------------------------------------------ */

/*
 * 48 MHz from PLL3, the one PLL nothing else uses (PLL1 is the core and
 * the SPIs, PLL2 the codec's MCLK, sai1.c). Its reference is HSE like
 * PLL1's - PLLSRC is shared - so the same division: 22 MHz / 11 = 2 MHz,
 * the 1-2 MHz input range (clock.c says why exactly 2 belongs there),
 * x 240 = 480 MHz on the wide VCO, / 10 = 48 MHz on Q. The crystal's
 * -11 ppm (SIGUIENTE.md) is far inside USB full speed's 2500.
 *
 * Then the USB's own supply detector (VDD33USB, PWR_CR3.USB33DEN) - the
 * PHY stays off until it says ready - and the core's clock. Its ULPI
 * clock's sleep enable is cleared: there is no ULPI PHY, and with that
 * bit set an FS core stops when the CPU sleeps (ST's examples clear it).
 */
void usb_rcu_config(void)
{
    RCC->CR &= ~RCC_CR_PLL3ON;
    espera(&RCC->CR, RCC_CR_PLL3RDY, 0U);

    RCC->PLLCKSELR = (RCC->PLLCKSELR & ~RCC_PLLCKSELR_DIVM3_Msk)
                   | (11U << RCC_PLLCKSELR_DIVM3_Pos);
    RCC->PLLCFGR = (RCC->PLLCFGR & ~(RCC_PLLCFGR_PLL3RGE_Msk
                                     | RCC_PLLCFGR_PLL3VCOSEL
                                     | RCC_PLLCFGR_PLL3FRACEN
                                     | RCC_PLLCFGR_DIVP3EN
                                     | RCC_PLLCFGR_DIVQ3EN
                                     | RCC_PLLCFGR_DIVR3EN))
                 | RCC_PLLCFGR_DIVQ3EN;
    /* Every field "value minus one"; P and R are off, set legal anyway. */
    RCC->PLL3DIVR = ((240U - 1U) << RCC_PLL3DIVR_N3_Pos)
                  | ((2U - 1U)   << RCC_PLL3DIVR_P3_Pos)
                  | ((10U - 1U)  << RCC_PLL3DIVR_Q3_Pos)
                  | ((2U - 1U)   << RCC_PLL3DIVR_R3_Pos);
    RCC->CR |= RCC_CR_PLL3ON;
    espera(&RCC->CR, RCC_CR_PLL3RDY, RCC_CR_PLL3RDY);

    /* USBSEL = 10: pll3_q_ck. */
    RCC->D2CCIP2R = (RCC->D2CCIP2R & ~RCC_D2CCIP2R_USBSEL_Msk)
                  | (2U << RCC_D2CCIP2R_USBSEL_Pos);

    PWR->CR3 |= PWR_CR3_USB33DEN;
    espera(&PWR->CR3, PWR_CR3_USB33RDY, PWR_CR3_USB33RDY);

    RCC->AHB1LPENR &= ~RCC_AHB1LPENR_USB2OTGFSULPILPEN;
    RCC->AHB1ENR   |= RCC_AHB1ENR_USB2OTGFSEN;
    (void)RCC->AHB1ENR;
}

/* PA11 = D-, PA12 = D+, AF10. AFR before MODER (fram.c's lesson). PA9
 * (VBUS through JP2) and PA10 (ID) are left alone. */
void usb_gpio_config(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN;
    (void)RCC->AHB4ENR;

    for (uint32_t pin = 11U; pin <= 12U; pin++) {
        GPIOA->AFR[1]  = (GPIOA->AFR[1] & ~(0xFUL << ((pin - 8U) * 4U)))
                       | (10UL << ((pin - 8U) * 4U));
        GPIOA->OTYPER &= ~(1UL << pin);
        GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3UL << (pin * 2U)))
                       | (3UL << (pin * 2U));
        GPIOA->PUPDR  &= ~(3UL << (pin * 2U));
        GPIOA->MODER   = (GPIOA->MODER & ~(3UL << (pin * 2U)))
                       | (2UL << (pin * 2U));
    }
}

/* Upstream's GD32 runs its delays off TIMER2; here SysTick already
 * counts cycles (board_cycles()). Nothing to set up. */
void usb_timer_init(void)
{
}

/*
 * The lowest priority there is: everything this radio does in interrupts
 * - the audio block, SysTick (the knobs, the watchdog, board_cycles()),
 * the panel's queue, the console - preempts the disk. A 4 kB write in
 * here takes ~70 ms (an erase and sixteen pages), and none of those may
 * wait for it.
 */
void usb_intr_config(void)
{
    NVIC_SetPriority(OTG_FS_IRQn, (1UL << __NVIC_PRIO_BITS) - 1UL);
    NVIC_ClearPendingIRQ(OTG_FS_IRQn);
    NVIC_EnableIRQ(OTG_FS_IRQn);
}

static void espera_us(uint32_t us)
{
    uint64_t t0  = board_cycles();
    uint64_t len = (uint64_t)us * (clock_sysclk_hz / 1000000U);

    /* Bounded as spi_nor_delay_us() is: board_cycles() wants SysTick,
     * which does preempt this interrupt - but a bound costs nothing. */
    for (uint32_t n = us * 64U + 64U; n != 0U; n--) {
        if (board_cycles() - t0 >= len) { break; }
    }
}

void usb_udelay(const uint32_t usec)
{
    espera_us(usec);
}

void usb_mdelay(const uint32_t msec)
{
    for (uint32_t i = 0U; i < msec; i++) { espera_us(1000U); }
}

/* compat/usb_malahit.h, USB_MODO_ESPERA: device mode forced, wait until
 * the core is in it (GINTSTS.CMOD = 0) - up to 50 ms, as ST's driver. */
static uint32_t s_modo_ms = 0xFFFFFFFFU;

void usbdisco_espera_modo(volatile uint32_t *gintsts)
{
    for (uint32_t ms = 0U; ms <= 50U; ms++) {
        if ((*gintsts & 1U) == 0U) { s_modo_ms = ms; return; }
        usb_mdelay(1U);
    }
    s_modo_ms = 0xFFFFFFFFU;
    s_errores++;
}

void OTG_FS_IRQHandler(void)
{
    s_isr++;
    usbd_isr(&s_usb);
}

/* compat/usb_malahit.h: reached only with low_power set, which it is
 * not (usb_conf.h, USBFS_LOW_POWER 0). Said once, and nothing done. */
void pmu_to_deepsleepmode(uint32_t ldo, uint32_t lowdrive, uint8_t cmd)
{
    static uint8_t dicho;

    (void)ldo; (void)lowdrive; (void)cmd;
    if (!dicho) {
        dicho = 1U;
        console_printf("usb: the library asked for deep sleep - ignored\n");
    }
}

/* ------------------------------------------------------------------ */
/* The mode                                                            */
/* ------------------------------------------------------------------ */

uint8_t usbdisco_activo(void) { return s_activo; }

uint8_t usbdisco_con_pc(void)
{
    return (s_activo && s_usb.dev.cur_status == (uint8_t)USBD_CONFIGURED)
           ? 1U : 0U;
}

/*
 * Upstream's bootloader's sequence (boot_main.c, "Y AQUI SE ENCIENDE EL
 * USB"), which is GigaDevice's msc_udisk example's: GPIO, clocks, the
 * delay timer, usbd_init(), the interrupt. usbd_init() ends connected:
 * D+ pulled up, the PC enumerates from there.
 */
__attribute__((noinline)) void usbdisco_abre(void)
{
    if (s_activo) { return; }

    guarda_suelta();        /* step 66: no file of ours open under the PC */

    memset(&s_usb, 0, sizeof s_usb);
    s_isr = 0U;
    g_msc_lee = g_msc_escribe = g_msc_bloque = g_msc_fallo = 0U;

    usb_gpio_config();

    /*
     * The library's waits for the core's soft reset and its FIFO flushes
     * have no way out, and none of them ends without the 48 MHz clock and
     * the PHY's supply. So those two are checked here first: a radio with
     * no USB disk is better than one stuck in usbd_init().
     */
    uint32_t err0 = s_errores;

    usb_rcu_config();
    if (s_errores != err0) {
        console_printf("usb: not started - PLL3 %s, USB supply %s\n",
                       (RCC->CR & RCC_CR_PLL3RDY) ? "locked" : "NOT LOCKED",
                       (PWR->CR3 & PWR_CR3_USB33RDY) ? "ready" : "NOT READY");
        RCC->AHB1ENR &= ~RCC_AHB1ENR_USB2OTGFSEN;
        RCC->CR      &= ~RCC_CR_PLL3ON;
        GPIOA->MODER |= (3UL << (11U * 2U)) | (3UL << (12U * 2U));
        return;
    }

    /* Before the interrupt can first run: from here the FRAM masks it. */
    fram_bus_irq((int32_t)OTG_FS_IRQn);

    usb_timer_init();
    s_modo_ms = 0xFFFFFFFFU;
    usbd_init(&s_usb, USB_CORE_ENUM_FS, &msc_desc, &msc_class);
    usb_intr_config();
    s_activo = 1U;

    console_printf("usb: disk on - %u blocks of 512 (%u kB), device mode "
                   "after %d ms, waits timed out %u\n", disco_bloques(),
                   disco_bloques() / 2U, (int32_t)s_modo_ms, s_errores);

    /* What the 26/11 failure looked like, said if it ever comes back:
     * the speed (DCFG 1:0 = 11, the internal FS PHY) or the connect
     * (DCTL.SDIS clear) not taken. */
    if ((s_usb.regs.dr->DCFG & 3U) != 3U || (s_usb.regs.dr->DCTL & 2U) != 0U) {
        console_printf("usb: *** DCFG %08x DCTL %08x - the core dropped the "
                       "library's setup; the PC will not see the disk ***\n",
                       s_usb.regs.dr->DCFG, s_usb.regs.dr->DCTL);
    }
}

__attribute__((noinline)) void usbdisco_cierra(void)
{
    if (!s_activo) { return; }

    NVIC_DisableIRQ(OTG_FS_IRQn);
    usbd_disconnect(&s_usb);            /* D+ released: the PC sees it go */
    usb_dev_stop(&s_usb);
    usb_globalint_disable(&s_usb.regs);

    RCC->AHB1RSTR |= RCC_AHB1RSTR_USB2OTGFSRST;
    RCC->AHB1RSTR &= ~RCC_AHB1RSTR_USB2OTGFSRST;
    RCC->AHB1ENR  &= ~RCC_AHB1ENR_USB2OTGFSEN;
    RCC->CR       &= ~RCC_CR_PLL3ON;
    GPIOA->MODER  |= (3UL << (11U * 2U)) | (3UL << (12U * 2U));   /* analog */
    NVIC_ClearPendingIRQ(OTG_FS_IRQn);

    fram_bus_irq(-1);
    s_activo = 0U;

    /* The PC may have changed anything on the volume. */
    spi_flash_geo_olvida();

    console_printf("usb: disk off - %u interrupts, %u reads, %u writes, "
                   "%u failed\n", s_isr, g_msc_lee, g_msc_escribe,
                   g_msc_fallo);
    if (spi_flash_fichero_hay("BD      ", "BIN")) {
        console_printf("usb: BD.BIN is on the disk - restart the radio to "
                       "load it (src/bd.c)\n");
    }
}

__attribute__((noinline)) void usbdisco_report(void)
{
    if (!s_activo) {
        console_printf("usb: disk off\n");
        return;
    }
    console_printf("usb: %u interrupts, state %u, %u reads %u writes %u "
                   "failed, last block %u; GOTGCTL %08x GCCFG %08x DSTS "
                   "%08x\n", s_isr, (uint32_t)s_usb.dev.cur_status,
                   g_msc_lee, g_msc_escribe, g_msc_fallo, g_msc_bloque,
                   s_usb.regs.gr->GOTGCS, s_usb.regs.gr->GCCFG,
                   s_usb.regs.dr->DSTAT);
}
