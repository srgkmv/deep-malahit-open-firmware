#include "stm32h7xx.h"
#include "clock.h"
#include "board.h"

/*
 * CMSIS declares this in system_stm32h7xx.h and defines it in
 * system_stm32h7xx.c - a file this project does not use, because its
 * SystemInit() knows nothing about a 22 MHz bypass oscillator and would
 * have to be rewritten line by line anyway. Defining the one variable here
 * is cheaper than carrying a vendor file that is wrong for this board.
 */
uint32_t SystemCoreClock = 64000000U;

int      clock_status    = CLOCK_ERR_HSERDY;
uint32_t clock_sysclk_hz = 64000000U;   /* HSI, until proven otherwise */
uint32_t clock_ahb_hz    = 64000000U;
uint32_t clock_apb1_hz   = 64000000U;

/*
 * Bounded waits. A clock bring-up that spins forever on a flag tells you
 * nothing at all: the board is dark and the debugger shows an address
 * inside a while loop that could be any of six. Every wait here gives up
 * after a while and reports which one it was, so the failure arrives as a
 * line of text instead of a guess.
 *
 * The count is in loop iterations, not microseconds, because at this point
 * in the boot there is no timebase to measure against - that is the whole
 * problem. At 64 MHz on HSI, 2 million iterations is comfortably more than
 * a millisecond, and every flag here is specified to come up in far less.
 */
#define WAIT_LIMIT  2000000U

#define WAIT_FOR(cond, errcode)                 \
    do {                                        \
        uint32_t _n = WAIT_LIMIT;               \
        while (!(cond)) {                       \
            if (--_n == 0U) { return (errcode); } \
        }                                       \
    } while (0)

#define WAIT_WHILE(cond, errcode)               \
    do {                                        \
        uint32_t _n = WAIT_LIMIT;               \
        while (cond) {                          \
            if (--_n == 0U) { return (errcode); } \
        }                                       \
    } while (0)

static int clock_bring_up(void)
{
    /*
     * 0. GET BACK TO A KNOWN STATE FIRST.
     *
     * This function must work when it runs a SECOND time, over a clock
     * tree it already configured - which is the normal case for an image
     * loaded by the debugger, because nothing resets RCC between loads.
     *
     * Without this block the failure is beautifully misleading. Step 2
     * below clears HSEON, and the hardware simply refuses: RM0433 says
     * HSEON cannot be cleared while HSE feeds the system clock, directly
     * or through a PLL. HSERDY therefore never drops, the wait burns all
     * two million iterations and returns CLOCK_ERR_HSERDY - reporting a
     * dead oscillator while the part is, in fact, humming along at
     * 480 MHz on that very oscillator.
     *
     * The damage is downstream: the error path writes the 64 MHz fallback
     * into clock_apb1_hz, console_init() computes its divisor from that,
     * and the console comes out at 216 kbaud while claiming 115200. We
     * spent a while reading that garbage.
     *
     * So: park on HSI, stop PLL1, put the bus dividers back to /1, and
     * only then start building. HSI is always available - it is what the
     * part boots on - and at 64 MHz with /1 dividers every bus is well
     * inside its limit.
     */
    RCC->CR |= RCC_CR_HSION;
    WAIT_FOR(RCC->CR & RCC_CR_HSIRDY, CLOCK_ERR_SWITCH);

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
    WAIT_FOR((RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_HSI,
             CLOCK_ERR_SWITCH);

    RCC->D1CFGR = 0U;
    RCC->D2CFGR = 0U;
    RCC->D3CFGR = 0U;

    RCC->CR &= ~RCC_CR_PLL1ON;
    WAIT_WHILE(RCC->CR & RCC_CR_PLL1RDY, CLOCK_ERR_PLLRDY);

    /*
     * 1. CORE VOLTAGE.
     *
     * PWR_CR3 is deliberately not touched. Its reset value already selects
     * the internal LDO, which is what this board uses: VCAP1 and VCAP2
     * carry 2.2 uF each and there is no SMPS inductor anywhere on the
     * schematic. CR3 also latches - several of its bits can only be
     * written once after reset - so writing it "just to be sure" is a way
     * to lose, not to win.
     *
     * Scale 1 first (VOS = 0b11), then the overdrive step to Scale 0.
     */
    PWR->D3CR = PWR_D3CR_VOS;                      /* VOS = 0b11, Scale 1 */
    WAIT_FOR(PWR->D3CR & PWR_D3CR_VOSRDY, CLOCK_ERR_VOSRDY);

    /*
     * Scale 0 - "overdrive" - is what allows anything above 400 MHz, and
     * on the H743 it is reached through a SYSCFG bit rather than through
     * PWR, which is easy to miss. SYSCFG needs its clock on first.
     */
    RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
    (void)RCC->APB4ENR;                            /* let the write land */
    SYSCFG->PWRCR |= SYSCFG_PWRCR_ODEN;
    (void)SYSCFG->PWRCR;
    WAIT_FOR(PWR->D3CR & PWR_D3CR_VOSRDY, CLOCK_ERR_VOSRDY);

    /*
     * 2. HSE, IN BYPASS.
     *
     * X1 is an oscillator module driving PH0 with a square wave; PH1 is
     * not connected. Feeding that into a crystal oscillator circuit would
     * simply never start. HSEBYP may only be changed while HSEON is clear,
     * so the order below is not negotiable.
     */
    RCC->CR &= ~RCC_CR_HSEON;
    WAIT_WHILE(RCC->CR & RCC_CR_HSERDY, CLOCK_ERR_HSERDY);
    RCC->CR |= RCC_CR_HSEBYP;
    RCC->CR |= RCC_CR_HSEON;
    WAIT_FOR(RCC->CR & RCC_CR_HSERDY, CLOCK_ERR_HSERDY);

    /*
     * 3. PLL1. See clock.h for where 11 / 480 / 2 comes from.
     */
    RCC->CR &= ~RCC_CR_PLL1ON;
    WAIT_WHILE(RCC->CR & RCC_CR_PLL1RDY, CLOCK_ERR_PLLRDY);

    RCC->PLLCKSELR = RCC_PLLCKSELR_PLLSRC_HSE
                   | (PLL1_DIVM << RCC_PLLCKSELR_DIVM1_Pos);

    /*
     * PLL1RGE = 00 says the reference is between 1 and 2 MHz, which at
     * exactly 2 MHz is the right bucket - ST's own range check puts 2 MHz
     * in range 0 and only moves to range 1 above it.
     *
     * PLL1VCOSEL = 0 selects the wide VCO range, 192 to 960 MHz. The
     * medium range tops out at 420 MHz and could not hold 960.
     *
     * The fractional divider stays off: 480 is an integer multiple and a
     * fractional PLL would only add noise for nothing.
     */
    RCC->PLLCFGR = (0U << RCC_PLLCFGR_PLL1RGE_Pos)
                 | (0U << RCC_PLLCFGR_PLL1VCOSEL_Pos)
                 | RCC_PLLCFGR_DIVP1EN
                 | RCC_PLLCFGR_DIVQ1EN
                 | RCC_PLLCFGR_DIVR1EN;

    /* Every field in PLL1DIVR is "value minus one". */
    RCC->PLL1DIVR = ((PLL1_DIVN - 1U) << RCC_PLL1DIVR_N1_Pos)
                  | ((PLL1_DIVP - 1U) << RCC_PLL1DIVR_P1_Pos)
                  | ((PLL1_DIVQ - 1U) << RCC_PLL1DIVR_Q1_Pos)
                  | ((PLL1_DIVR - 1U) << RCC_PLL1DIVR_R1_Pos);

    RCC->CR |= RCC_CR_PLL1ON;
    WAIT_FOR(RCC->CR & RCC_CR_PLL1RDY, CLOCK_ERR_PLLRDY);

    /*
     * 4. BUS DIVIDERS, BEFORE THE SWITCH.
     *
     * This ordering is the one that bites people. SYSCLK becomes 480 MHz
     * the instant step 5 runs; if the AHB divider were still /1 at that
     * moment, every bus in the chip would be asked to run at 480 MHz for
     * however many cycles it takes to write the next register. The
     * ceilings are 240 MHz for AXI/AHB and 120 MHz for all four APBs, so
     * the dividers go in first and the switch happens into a tree that is
     * already legal.
     */
    RCC->D1CFGR = RCC_D1CFGR_D1CPRE_DIV1     /* CPU   480 MHz */
                | RCC_D1CFGR_HPRE_DIV2       /* AHB   240 MHz */
                | RCC_D1CFGR_D1PPRE_DIV2;    /* APB3  120 MHz */
    RCC->D2CFGR = RCC_D2CFGR_D2PPRE1_DIV2    /* APB1  120 MHz */
                | RCC_D2CFGR_D2PPRE2_DIV2;   /* APB2  120 MHz */
    RCC->D3CFGR = RCC_D3CFGR_D3PPRE_DIV2;    /* APB4  120 MHz */

    /*
     * Flash wait states. This image runs from RAM and never fetches an
     * instruction out of flash, so it is tempting to skip - but the option
     * bytes and the device ID still live behind the flash controller, and
     * anything that reads them at 240 MHz with the reset latency would
     * fault. Four wait states with WRHIGHFREQ = 2 is ST's own setting for
     * VOS0 at 240 MHz AXI.
     */
    FLASH->ACR = FLASH_ACR_LATENCY_4WS | (2U << FLASH_ACR_WRHIGHFREQ_Pos);
    WAIT_FOR((FLASH->ACR & FLASH_ACR_LATENCY) == FLASH_ACR_LATENCY_4WS,
             CLOCK_ERR_SWITCH);

    /* 5. Go. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL1;
    WAIT_FOR((RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL1,
             CLOCK_ERR_SWITCH);

    return CLOCK_OK;
}

int clock_init(void)
{
    clock_status = clock_bring_up();

    if (clock_status == CLOCK_OK) {
        clock_sysclk_hz = SYSCLK_HZ;
        clock_ahb_hz    = AHB_HZ;
        clock_apb1_hz   = APB_HZ;
    } else {
        /*
         * Whatever failed, we are still on HSI at 64 MHz with every
         * divider at reset, so say so honestly rather than leaving the
         * optimistic values in place. The UART divisor is computed from
         * these, so getting them right here is what makes the error
         * message readable instead of garbage.
         */
        clock_sysclk_hz = 64000000U;
        clock_ahb_hz    = 64000000U;
        clock_apb1_hz   = 64000000U;
    }

    SystemCoreClock = clock_sysclk_hz;
    return clock_status;
}

/*
 * Called from the startup code, after .data and .bss are ready. ST's own
 * template calls SystemInit() BEFORE those are initialised, which quietly
 * forbids it from touching any global - a rule nobody writes down and
 * everybody eventually breaks. Here it runs after, so clock_status and the
 * three frequencies above are real variables that survive into main().
 */
void SystemInit(void)
{
    /*
     * Instruction cache on: free, and it cannot be wrong - the I-cache is
     * never stale because this image does not modify its own code.
     *
     * Data cache deliberately OFF. On this chip the D-cache is the single
     * most common source of "works, except sometimes": any DMA buffer that
     * is not either in a non-cacheable MPU region or explicitly cleaned
     * and invalidated will silently read or write stale bytes. There is no
     * DMA in this bring-up image, so there is nothing to gain yet, and
     * turning it on now would mean the panel and SAI stages inherit a
     * bug that was already there. It goes on together with the MPU
     * regions, in the step that first needs DMA.
     */
    SCB_EnableICache();

    /*
     * Power before clocks. The 22 MHz oscillator that HSE needs sits on
     * the switched 3.3V rail, so PC6 has to be high and the module has
     * to have settled before clock_init() so much as looks at HSEON.
     * See board.h.
     */
    board_rail_on_early();

    clock_init();
}
