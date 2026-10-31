#ifndef MALAHIT_CLOCK_H
#define MALAHIT_CLOCK_H

#include <stdint.h>

/*
 * THE 22 MHz PROBLEM.
 *
 * Every STM32H7 clock example in existence assumes 8 or 25 MHz. This board
 * has neither: X1 is a 22.000 MHz oscillator MODULE (EN/V+/GND/OUT), its
 * output wired to PH0 with PH1 left unconnected, which means HSE runs in
 * BYPASS mode and every PLL divider has to be worked out from scratch.
 *
 * The same 22 MHz also feeds REF_CLK_MSI001, so the tuner and the CPU share
 * one reference. That is good for spurs - no beat between two oscillators -
 * and it means anything done to this clock is also done to the tuner.
 */
#define HSE_HZ              22000000U

/*
 * Why 480 MHz, and why these numbers.
 *
 * To land on 480 MHz the VCO has to run at 960 MHz with /2 on the output,
 * which is also what ST's own 480 MHz configurations do (25 MHz / 5 = 5 MHz,
 * x192 = 960). From 22 MHz there is exactly ONE clean way to get there:
 *
 *     22 MHz / DIVM1(11) = 2 MHz      PLL reference
 *     2 MHz x DIVN1(480) = 960 MHz    VCO, wide range (192..960)
 *     960 MHz / DIVP1(2) = 480 MHz    SYSCLK
 *
 * Every other divisor pair gives a fractional DIVN1. Checked: M=2 wants
 * N=87.27, M=4 wants 174.5, M=5 wants 218.18, M=10 wants 436.36.
 *
 * The cost is a 2 MHz phase comparison frequency, which is the bottom of
 * the allowed input range and worse for jitter than a higher reference
 * would be. If that ever shows up as phase noise on the receiver, the
 * fallback is 440 MHz: 22/2 = 11 MHz reference, x80 = 880 MHz VCO, /2 =
 * 440 MHz - a much healthier PLL for 8% less CPU. Nothing in this project
 * needs 480 specifically.
 *
 * 480 MHz requires revision V silicon. This radio measured REV_ID 0x2003,
 * which is rev V; a rev Y part tops out at 400 MHz and would need
 * different numbers AND no VOS0 step.
 */
#define PLL1_DIVM           11U
#define PLL1_DIVN           480U
#define PLL1_DIVP           2U
#define PLL1_DIVQ           4U      /* 240 MHz, spare */
#define PLL1_DIVR           2U      /* 480 MHz, spare */

#define SYSCLK_HZ           480000000U
#define AHB_HZ              240000000U  /* SYSCLK / 2  - AXI/AHB ceiling */
#define APB_HZ              120000000U  /* AHB / 2     - APB ceiling     */

/*
 * Bring the clock tree up: VOS0, HSE bypass, PLL1, bus dividers, flash wait
 * states, then switch SYSCLK over. Called from SystemInit(), before main().
 *
 * Returns 0 on success, or a small negative number saying which wait loop
 * gave up - see the enum below. It does NOT hang forever on a dead
 * oscillator: a board whose X1 is not running should still reach main() on
 * HSI and be able to say so out of the UART, which is worth a great deal
 * more than a silent lockup.
 */
int clock_init(void);

enum {
    CLOCK_OK            =  0,
    CLOCK_ERR_VOSRDY    = -1,   /* core voltage never reported ready */
    CLOCK_ERR_HSERDY    = -2,   /* the 22 MHz module is not oscillating */
    CLOCK_ERR_PLLRDY    = -3,   /* PLL1 never locked */
    CLOCK_ERR_SWITCH    = -4    /* SYSCLK never switched to PLL1 */
};

/* What clock_init() returned, kept so main() can report it. */
extern int      clock_status;

/* Actual frequencies after clock_init(), whatever it managed to achieve. */
extern uint32_t clock_sysclk_hz;
extern uint32_t clock_ahb_hz;
extern uint32_t clock_apb1_hz;

#endif /* MALAHIT_CLOCK_H */
