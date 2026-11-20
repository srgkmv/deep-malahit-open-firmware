#include "boot.h"
#include "stm32h7xx.h"
#include "clock.h"          /* clock_sysclk_hz, for boot_power_park() */

/*
 * ROM DFU ON THE ENCODER BUTTONS - THE FACTORY FIRMWARE'S OWN RECIPE.
 *
 * Read out of the factory image (FW 1.10d), because the owners of this
 * radio already know the gesture and because the factory code has been
 * getting it right on every unit sold:
 *
 *   0x08007a34  GPIOA clock on, PA1 an input, IDR bit 1 read; 0 = DFU
 *   0x0800e2ac  SysTick off, RCC de-initialised, every NVIC interrupt
 *               disabled and un-pended, interrupts re-enabled, then MSP
 *               and PC from the system bootloader at 0x1FF09800
 *
 * WHY A DIGITAL READ OF AN ANALOG LADDER WORKS. PA1 is ENC_BUT, the
 * ladder adc.c classifies (BOARD.md section 6). Idle is 3.3 V, the big
 * knob 1.96 V, the small knob 1.05 V and both together 0.86 V. VIL on
 * this part is 0.3 VDD = 0.99 V, so "both" is a guaranteed 0, the big
 * knob alone is in the undefined band and in practice reads 1, and the
 * small knob alone - 1.05 V, sixty millivolts above VIL - probably reads
 * 0 as well. The factory firmware accepts that, and so does this.
 *
 * THE ADDRESS IS 0x1FF09800. 0x1FF00000 is what the BOOT_ADD1 option
 * byte holds, and this file's ancestors wrote it as the jump target; the
 * H743's bootloader code and vector table start 0x9800 higher, and the
 * factory firmware's literal says so.
 */
#define SYSMEM_BOOTLOADER   0x1FF09800UL

/* PA1 must read 0 on every one of these samples, spaced by SAMPLE_GAP
 * loop passes. At 64 MHz HSI, which is what a cold boot runs on here,
 * that is about 8 ms - long against contact bounce, nothing against a
 * thumb that is holding two buttons down. Loaded over SWD onto a part
 * already at 480 MHz it is under 1.5 ms, which is still plenty, because
 * then the buttons were held before the load. A count, not a time - see
 * WORKING.md on bounds counted in loop passes - but here a short wait is
 * harmless in both directions, and no clock is configured yet to time
 * anything with. */
#define SAMPLES             64U
#define SAMPLE_GAP          2000U

static void spin(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++) { }
}

/*
 * Leave the part the way the bootloader expects to find it: as close to
 * reset as this can get without a reset - because, measured, a reset
 * here returns to the flash image instead (WORKING.md, "No way back").
 *
 * After a real reset almost all of this is already true and costs
 * nothing. It matters when this image was loaded over SWD onto a running
 * one, which is how it is tested: then a SAI DMA stream may still be
 * writing into RAM the bootloader is about to use, a PLL may be the
 * system clock, and the factory firmware's USB core may still be set up
 * the way IT wanted.
 *
 * TWO THINGS DELIBERATELY LEFT ALONE:
 *
 *   HSE. Once this firmware's clock_init() has armed the clock security
 *   system it cannot be disarmed without a reset, and switching HSE off
 *   then looks to it exactly like a dead oscillator: an NMI, into
 *   whatever vector table is live - the bootloader's, by then. So HSE
 *   and PC6 (which powers it, BOARD.md section 3) stay as they are, and
 *   only the system clock is moved off the PLL. After a cold boot HSE
 *   was never started and none of this arises.
 *
 *   The independent watchdog. It cannot be stopped, and it does not need
 *   to be: after a cold boot of this firmware it was never started, and
 *   loaded over SWD onto the factory firmware - where it may be running
 *   - the DFU session measured 20/11/2026 was still enumerated two
 *   minutes later. This comment first predicted a reset at 32 s; it was
 *   wrong, and whether the bootloader feeds the watchdog or nothing had
 *   started it is not settled.
 */
static void quiesce(void)
{
    __disable_irq();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    for (uint32_t i = 0; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /* Every bus master that could still be writing memory, and both USB
     * cores, back to their reset state. */
    RCC->AHB1RSTR |=  (RCC_AHB1RSTR_DMA1RST | RCC_AHB1RSTR_DMA2RST
                     | RCC_AHB1RSTR_USB1OTGHSRST | RCC_AHB1RSTR_USB2OTGFSRST);
    RCC->AHB3RSTR |=  RCC_AHB3RSTR_MDMARST;
    RCC->AHB4RSTR |=  RCC_AHB4RSTR_BDMARST;
    RCC->APB2RSTR |=  RCC_APB2RSTR_SAI1RST;
    (void)RCC->APB2RSTR;
    RCC->AHB1RSTR &= ~(RCC_AHB1RSTR_DMA1RST | RCC_AHB1RSTR_DMA2RST
                     | RCC_AHB1RSTR_USB1OTGHSRST | RCC_AHB1RSTR_USB2OTGFSRST);
    RCC->AHB3RSTR &= ~RCC_AHB3RSTR_MDMARST;
    RCC->AHB4RSTR &= ~RCC_AHB4RSTR_BDMARST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_SAI1RST;
    (void)RCC->APB2RSTR;

    /* The system clock onto HSI, the PLLs off, every prescaler back to
     * divide-by-one: the reset clock tree, minus HSE (see above). Bounded
     * waits: HSI is on silicon and does not fail to start, but a wait
     * that cannot end is the one thing this function must not have. */
    RCC->CR |= RCC_CR_HSION;
    for (uint32_t i = 0; i < 1000000U && !(RCC->CR & RCC_CR_HSIRDY); i++) { }

    RCC->CFGR = 0U;                                 /* SW = HSI */
    for (uint32_t i = 0; i < 1000000U && (RCC->CFGR & RCC_CFGR_SWS) != 0U; i++) { }

    RCC->CR   &= ~(RCC_CR_PLL1ON | RCC_CR_PLL2ON | RCC_CR_PLL3ON);
    RCC->D1CFGR = 0U;
    RCC->D2CFGR = 0U;
    RCC->D3CFGR = 0U;

    /* The kernel-clock selections to their reset values, so the
     * bootloader's USB setup starts from what it was written against. */
    RCC->D1CCIPR  = 0U;
    RCC->D2CCIP1R = 0U;
    RCC->D2CCIP2R = 0U;
    RCC->D3CCIPR  = 0U;

    /* Both caches off and the I-cache invalidated. After a reset they
     * are off already; Reset_Handler cleared them on the way here too. */
    __DSB();
    __ISB();
    SCB->CCR &= ~(SCB_CCR_IC_Msk | SCB_CCR_DC_Msk);
    SCB->ICIALLU = 0U;
    __DSB();
    __ISB();
}

void boot_dfu_check(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN;
    (void)RCC->AHB4ENR;

    /* PA1 as a plain input, no pull. Its reset mode is ANALOG, which
     * disconnects the Schmitt trigger and makes IDR read 0 whatever the
     * pin is doing - a "button held" on every boot. Saved and put back,
     * because adc_init() wants it analog and should find it so. */
    uint32_t moder = GPIOA->MODER;
    uint32_t pupdr = GPIOA->PUPDR;

    GPIOA->PUPDR &= ~GPIO_PUPDR_PUPD1;
    GPIOA->MODER &= ~GPIO_MODER_MODE1;

    /*
     * THE PERIPHERAL RAIL FIRST - PC6 high, and time for it to come up -
     * BECAUSE THE LADDER HANGS FROM IT.
     *
     * ENC_BUT's top resistor is fed from the switched 3.3 V rail that PC6
     * enables, not from the MCU's own supply. Read with PC6 still low, as
     * it is after every real reset, PA1 has no pull-up at all and reads
     * 0: "both buttons held". The first image written to flash did
     * exactly that, 20/11/2026 - a short press of the power button, no
     * encoder touched, and the radio came up in DFU. Every RAM test had
     * passed because PC6 was still high from the image before it: the
     * "loading over SWD resets nothing" lesson at the top of WORKING.md,
     * one more time. The factory firmware raises PC6 before its PA1 read;
     * that was in its code, and it was the one step not copied.
     *
     * AND IT HOLDS THE MCU'S OWN POWER ON. The owner's unit is modified:
     * U3, the MCU's regulator, is enabled through diodes by 3.3V_ON (PC6)
     * and by the power button, which is fed from BAT - not tied always-on
     * as the schematic has it (BOARD.md section 3). On that board the MCU
     * stays powered only while the button is held or PC6 is high. So
     * raising PC6 here also latches the supply, and DFU no longer lasts
     * only as long as the thumbs on the buttons - the owner pointed that
     * out before the first flash write. On an unmodified board it just
     * brings the peripheral rail up a few milliseconds early.
     *
     * Level first, then output, so the pin never drives low on the way.
     * Left high whichever way this goes: SystemInit() raises it anyway,
     * and the DFU branch needs it. The wait is the same counted ~19 ms at
     * 64 MHz that board_rail_on_early() gives the rail and the 22 MHz
     * module, for the same capacitance.
     */
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;
    (void)RCC->AHB4ENR;
    GPIOC->BSRR    = (1UL << 6U);
    GPIOC->OTYPER &= ~(1UL << 6U);
    GPIOC->MODER   = (GPIOC->MODER & ~GPIO_MODER_MODE6) | GPIO_MODER_MODE6_0;

    spin(400000U);                      /* the rail, then the ladder, settle */

    uint32_t low = 0U;

    for (uint32_t i = 0; i < SAMPLES; i++) {
        if ((GPIOA->IDR & GPIO_IDR_ID1) == 0U) { low++; }
        spin(SAMPLE_GAP);
    }

    if (low != SAMPLES) {
        GPIOA->MODER = moder;
        GPIOA->PUPDR = pupdr;
        return;
    }

    /* PC6 is already high from above, and nothing in quiesce() or in the
     * bootloader touches GPIOC: the supply stays latched for as long as
     * DFU lasts. */
    quiesce();

    /* Re-enabled, as the factory code does: the bootloader is written to
     * be entered from reset, where PRIMASK is clear. Nothing can fire -
     * every source in the NVIC is disabled and SysTick is stopped. */
    __enable_irq();

    uint32_t sp = *(volatile uint32_t *)(SYSMEM_BOOTLOADER + 0U);
    uint32_t pc = *(volatile uint32_t *)(SYSMEM_BOOTLOADER + 4U);

    /*
     * The stack pointer and the jump in ONE asm statement. From C, the
     * compiler is free to touch the stack between __set_MSP() and the
     * call - a spill, a saved register - and after the first statement
     * "the stack" is the bootloader's. Both values are already in
     * registers here; nothing can come between them.
     */
    __asm volatile ("msr msp, %0\n\t"
                    "bx  %1"
                    : : "r" (sp), "r" (pc) : "memory");

    for (;;) { }
}

/* ------------------------------------------------------------------ */
/* The power button                                                   */
/* ------------------------------------------------------------------ */

/*
 * SW3 PRESSED READS 1. BOARD.md section 3 has it on PC13 "via
 * R39/R44/R45 divider off 3.3V_CPU", and the factory firmware arms
 * WKUP4 on a RISING edge (0x08009aa8, WKUPP4 left at 0) - the press is
 * what has to wake the part. Inferred from those two, and checked on the
 * radio by the console's `power` column before anything relies on it.
 */
#define PWRKEY_PORT     GPIOC
#define PWRKEY_PIN      13U

static int key_down_raw(void)
{
    return (PWRKEY_PORT->IDR & GPIO_IDR_ID13) != 0U;
}

static void key_input(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;
    (void)RCC->AHB4ENR;

    /* Reset mode is ANALOG, which reads 0 whatever the pin does. No
     * pull: the divider is the bias, and a pull would shift it. */
    PWRKEY_PORT->PUPDR &= ~GPIO_PUPDR_PUPD13;
    PWRKEY_PORT->MODER &= ~GPIO_MODER_MODE13;
}

int boot_power_key(void) { return key_down_raw(); }

void boot_standby(void)
{
    __disable_irq();

    SysTick->CTRL = 0U;
    for (uint32_t i = 0; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /*
     * AND NO SYSTEM EXCEPTION LEFT PENDING, or the WFI below never
     * sleeps: WFI wakes on anything pending even with PRIMASK set, and
     * stopping SysTick does not un-pend a tick. From SysTick's own
     * handler - the button in normal use - the tick is active, not
     * pending, so it never showed. From boot_power_park(), after a
     * crash with interrupts masked, a tick had been pending all along:
     * found 20/11/2026 with the backlight dark and the speaker off and
     * the MCU still powered, spinning through WFI - a radio that looked
     * off and was draining the battery. Read on the radio: ICSR
     * 0x0440F000, SysTick pending; cleared over SWD, it went into
     * standby at once.
     */
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    /*
     * What the hand notices goes first: backlight (PB8) and speaker
     * amplifier (PE1) off. These are plain logic outputs into MOSFET
     * gates and switching them changes nothing the clock tree depends
     * on.
     *
     * PC6 IS DELIBERATELY NOT TOUCHED. It powers the 22 MHz module that
     * HSE runs from, and once this firmware's clock_init() has armed the
     * clock security system, cutting it here would look exactly like a
     * dead oscillator: an NMI, which wakes the core out of the WFI below.
     * The factory firmware does not touch it either. Standby leaves every
     * pin floating, and R38 pulls the rail's enable down by itself.
     */
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOEEN;
    (void)RCC->AHB4ENR;
    GPIOB->BSRR = (1UL << (8U + 16U));
    GPIOE->BSRR = (1UL << (1U + 16U));

    /* Wake-up on SW3: WKUP4 = PC13, rising edge - the factory's values
     * exactly, including the mask it keeps the other pins' fields with.
     * Disable, clear the flag, then enable, so a stale flag cannot wake
     * the part the instant it sleeps. */
    PWR->WKUPEPR &= ~PWR_WKUPEPR_WKUPEN4;
    PWR->WKUPCR   =  PWR_WKUPCR_WKUPC4;
    PWR->WKUPEPR  = (PWR->WKUPEPR & 0xF000C0C0UL) | PWR_WKUPEPR_WKUPEN4;

    /*
     * Every peripheral clock off. Not tidiness: on this part a domain
     * enters DStandby only when the CPU has no peripheral allocated in
     * it, so a single enable bit left set in D2 keeps the whole system
     * out of standby. The factory code clears the same nine registers.
     */
    RCC->AHB3ENR  = 0U;
    RCC->AHB1ENR  = 0U;
    RCC->AHB2ENR  = 0U;
    RCC->AHB4ENR  = 0U;
    RCC->APB3ENR  = 0U;
    RCC->APB1LENR = 0U;
    RCC->APB1HENR = 0U;
    RCC->APB2ENR  = 0U;
    RCC->APB4ENR  = 0U;

    /* Standby for all three domains, the stop/standby flags cleared. */
    PWR->CPUCR |= PWR_CPUCR_PDDS_D1 | PWR_CPUCR_PDDS_D2 | PWR_CPUCR_PDDS_D3
                | PWR_CPUCR_CSSF;

    /*
     * DEBUG OFF IN STANDBY, as the factory code does - and this was
     * tried the other way first. With DBG_STANDBYD1, DBG_STOPD1,
     * DBG_SLEEPD1 and both debug clocks kept on, the radio went to
     * standby exactly as designed and SWD was STILL "cannot read IDR",
     * twice, at 1 MHz and at 200 kHz (20/11/2026): with all three domains
     * in standby the debug link goes with them, and those bits do not
     * hold it. A setting that keeps nothing reachable would only cost
     * battery, so it is the factory's value. The way back from a flashed
     * image is boot_dfu_check() first, and SWD while the image is
     * running - WORKING.md, "No way back".
     */
    DBGMCU->CR = 0U;

    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    __DSB();
    __ISB();

    /* Waking is a reset; if WFI ever returns, something was pending -
     * go round again rather than run on with every clock off. */
    for (;;) { __WFI(); }
}

/*
 * AT RESET: is somebody switching the radio on?
 *
 * Like boot_dfu_check(), this runs before .data and .bss and uses no
 * global. 32 samples over a few milliseconds, and "held" means at least
 * three quarters of them: a power-on press is still down when the part
 * gets here - the wake-up is the press itself, and this is microseconds
 * after it - but a contact can bounce, and a bounce must not send the
 * radio back to sleep under the finger switching it on.
 */
void boot_power_check(void)
{
    if (SCB->VTOR >= 0x20000000UL) {
        return;     /* a RAM image over SWD: somebody is at the bench */
    }

    key_input();
    spin(SAMPLE_GAP);

    uint32_t high = 0U;

    for (uint32_t i = 0; i < 32U; i++) {
        if (key_down_raw()) { high++; }
        spin(SAMPLE_GAP);
    }

    if (high >= 24U) {
        return;     /* SW3 held: switching on */
    }

    boot_standby();
}

static volatile uint8_t  s_key_live;
static volatile uint8_t  s_key_armed;
static volatile uint32_t s_key_ms;

void boot_power_init(void)
{
    key_input();
    s_key_ms    = 0U;
    s_key_armed = 0U;
    s_key_live  = 1U;
}

/*
 * From SysTick, 1 kHz, so that switching off works even with the main
 * loop stuck - which is when somebody most wants to.
 *
 * Held for BOOT_POWER_OFF_MS: the backlight and the speaker go at once,
 * which tells the hand it worked. Standby waits for the RELEASE, plus
 * 50 ms of it: sleeping with SW3 still down would be sleeping with the
 * wake-up pin already high.
 */
void boot_power_tick(void)
{
    if (!s_key_live) { return; }

    int down = key_down_raw();

    if (!s_key_armed) {
        if (!down) { s_key_ms = 0U; return; }
        if (++s_key_ms < BOOT_POWER_OFF_MS) { return; }

        s_key_armed = 1U;
        s_key_ms    = 0U;
        GPIOB->BSRR = (1UL << (8U + 16U));      /* backlight */
        GPIOE->BSRR = (1UL << (1U + 16U));      /* speaker amplifier */
        return;
    }

    if (down) { s_key_ms = 0U; return; }
    if (++s_key_ms >= 50U) { boot_standby(); }
}

/*
 * THE POWER BUTTON AFTER A CRASH - found 20/11/2026.
 *
 * fault_park() runs with interrupts masked, so SysTick never runs and
 * boot_power_tick() with it. There is no watchdog either - nothing in
 * this firmware starts the IWDG, and the option bytes leave it to
 * software - so a crashed radio stayed on, frozen, with a power button
 * that did nothing: the only way out was the battery.
 *
 * So the same gesture, polled: held BOOT_POWER_OFF_MS, the backlight
 * and the speaker go, and standby follows 50 ms after the release -
 * the rules boot_power_tick() keeps, for the same reasons. Timed on the
 * DWT cycle counter, switched on here, because nothing else can be
 * trusted after a fault. clock_sysclk_hz is clamped to what the clock
 * tree can be: at the wrong end the hold is 0.4 s or 22 s, never
 * for ever.
 */
void boot_power_park(void)
{
    key_input();

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

    uint32_t hz = clock_sysclk_hz;

    if (hz < 64000000U || hz > 480000000U) { hz = 480000000U; }

    uint32_t per_ms = hz / 1000U;
    uint32_t t0     = 0U;
    int      held   = 0;

    for (;;) {
        IWDG1->KR = 0xAAAAU;            /* harmless if it is not running */

        if (!key_down_raw()) { held = 0; continue; }
        if (!held) { held = 1; t0 = DWT->CYCCNT; continue; }
        if ((uint32_t)(DWT->CYCCNT - t0) >= per_ms * BOOT_POWER_OFF_MS) {
            break;
        }
    }

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOEEN;
    (void)RCC->AHB4ENR;
    GPIOB->BSRR = (1UL << (8U + 16U));      /* backlight */
    GPIOE->BSRR = (1UL << (1U + 16U));      /* speaker amplifier */

    t0 = DWT->CYCCNT;   /* the release is timed from now, not the press */
    for (;;) {
        IWDG1->KR = 0xAAAAU;
        if (key_down_raw()) { t0 = DWT->CYCCNT; continue; }
        if ((uint32_t)(DWT->CYCCNT - t0) >= per_ms * 50U) { break; }
    }

    boot_standby();
}
