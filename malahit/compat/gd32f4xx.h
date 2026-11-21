#ifndef GD32F4XX_H
#define GD32F4XX_H

/*
 * A STAND-IN FOR THE GD32'S OWN HEADER, FOR THE FEW UPSTREAM FILES WHOSE
 * ONLY HARDWARE DEPENDENCE IS A HANDFUL OF GPIO READS.
 *
 * `User/encoder.c` is the first of them and the reason this file exists:
 * 241 lines of quadrature decoding, button debouncing and press
 * classification, of which exactly four lines touch a register. Copying
 * it to get at the other 237 would have been the worst available trade -
 * this port's whole premise is that upstream code is compiled in place,
 * so that a fix upstream is a fix here and a divergence is a diff rather
 * than a duplicate.
 *
 * WHY AN -I WORKS HERE AND NOT FOR THE PANEL HEADER. A quoted include
 * searches the directory of the file holding the directive first, which
 * is why `rm68120_exmc.h` needed the `-D` guard plus `-include` trick
 * (see compat/rm68120_ili9488.h): that header lives in `User/`.
 * `gd32f4xx.h` does not - it lives in `CMSIS/GD/GD32F4xx/Include/`,
 * which is not on this build's include path at all - so `-I../compat`,
 * which is already first, is enough to put this file in its place.
 *
 * THE TRANSLATION IS AN IDENTITY, WHICH IS WORTH STATING. The GD32's
 * peripheral model is a copy of the STM32F4's, so its mode and pull
 * encodings are numerically the same as the ones MODER and PUPDR want:
 * input/output/AF/analog = 0/1/2/3 and none/up/down = 0/1/2. Nothing
 * here invents a mapping; if it had to, this header would be the wrong
 * place for it.
 *
 * AND IT DECLARES NOTHING ELSE. That is deliberate and it is how a
 * stand-in should fail - the same rule `deepsdr-ui/hostinc/gd32f4xx.h`
 * states for the host bank. An upstream file grafted later that needs a
 * timer, a DMA channel or an EXTI line will not compile, loudly, at the
 * missing name. What it must never do is compile against a plausible
 * guess and run.
 */

#include "stm32h7xx.h"

/*
 * GPIOA..GPIOK are already the STM32's own port pointers, from the CMSIS
 * device header above, and they are what upstream's ENC_x_PORT names
 * resolve to. So `GPIOC` means this chip's port C - which is what the
 * encoder is actually wired to on this board.
 *
 * FlagStatus, SET and RESET come from there too: stm32h7xx.h defines
 * them exactly as the GD32 header does, so upstream's `== SET` works
 * unchanged and must not be redefined here.
 */

/* The GD32 names a pin by its bit mask, same as this does. */
#define GPIO_PIN_0      (1UL << 0)
#define GPIO_PIN_1      (1UL << 1)
#define GPIO_PIN_2      (1UL << 2)
#define GPIO_PIN_3      (1UL << 3)
#define GPIO_PIN_4      (1UL << 4)
#define GPIO_PIN_5      (1UL << 5)
#define GPIO_PIN_6      (1UL << 6)
#define GPIO_PIN_7      (1UL << 7)
#define GPIO_PIN_8      (1UL << 8)
#define GPIO_PIN_9      (1UL << 9)
#define GPIO_PIN_10     (1UL << 10)
#define GPIO_PIN_11     (1UL << 11)
#define GPIO_PIN_12     (1UL << 12)
#define GPIO_PIN_13     (1UL << 13)
#define GPIO_PIN_14     (1UL << 14)
#define GPIO_PIN_15     (1UL << 15)

/* MODER encodings, which are also the GD32's. */
#define GPIO_MODE_INPUT     0UL
#define GPIO_MODE_OUTPUT    1UL
#define GPIO_MODE_AF        2UL
#define GPIO_MODE_ANALOG    3UL

/* PUPDR encodings, likewise. */
#define GPIO_PUPD_NONE      0UL
#define GPIO_PUPD_PULLUP    1UL
#define GPIO_PUPD_PULLDOWN  2UL

/*
 * The GPIO ports only, numbered so that the value IS the AHB4ENR bit.
 * Every other RCU_* the GD32 header has is deliberately absent.
 */
typedef enum {
    RCU_GPIOA = 0,
    RCU_GPIOB = 1,
    RCU_GPIOC = 2,
    RCU_GPIOD = 3,
    RCU_GPIOE = 4,
    RCU_GPIOF = 5,
    RCU_GPIOG = 6,
    RCU_GPIOH = 7,
    RCU_GPIOI = 8,
    RCU_GPIOJ = 9,
    RCU_GPIOK = 10
} rcu_periph_enum;

static inline void rcu_periph_clock_enable(rcu_periph_enum periph)
{
    RCC->AHB4ENR |= (1UL << (uint32_t)periph);

    /*
     * Read it back before returning. A peripheral clock enable is a
     * posted write to a 120 MHz bus from a 480 MHz core, and the next
     * instruction can reach the peripheral before the clock does - which
     * on this part is a bus fault, from the line after the one that
     * looks responsible. Every other driver in this port does the same
     * read, for the same reason.
     */
    (void)RCC->AHB4ENR;
}

static inline FlagStatus gpio_input_bit_get(GPIO_TypeDef *port, uint32_t pin)
{
    return ((port->IDR & pin) != 0U) ? SET : RESET;
}

/*
 * Configure every pin in the mask. Read-modify-write per pin rather than
 * a mask-wide write, because the ports this is called on carry other
 * people's pins: port C here holds the panel's SPI3 and the touch
 * controller's reset, and port A holds SWD. A driver that writes a whole
 * MODER takes the debug port with it.
 */
static inline void gpio_mode_set(GPIO_TypeDef *port, uint32_t mode,
                                 uint32_t pupd, uint32_t pin_mask)
{
    for (uint32_t pin = 0; pin < 16U; pin++) {
        if ((pin_mask & (1UL << pin)) == 0U) { continue; }

        port->MODER = (port->MODER & ~(3UL << (pin * 2U)))
                    | (mode << (pin * 2U));
        port->PUPDR = (port->PUPDR & ~(3UL << (pin * 2U)))
                    | (pupd << (pin * 2U));
    }
}

/*
 * OUTPUT TYPE AND SPEED, AND SETTING A PIN - for demod_am_init(), which
 * switches the speaker amplifier on. Step 8, 21/11/2026.
 *
 * The values are the GD32's, and they happen to be the H7's register
 * encodings as well: OTYPER 0 push-pull / 1 open-drain; OSPEEDR 00 is
 * the slowest edge on both parts (the GD32 calls it 2 MHz, ST "low").
 * Only the slowest speed is defined, because that is all upstream asks
 * for, and a define for a faster one would be a guess about which ST
 * speed the GD32's 25/50 MHz should become.
 */
#define GPIO_OTYPE_PP       0UL
#define GPIO_OTYPE_OD       1UL
#define GPIO_OSPEED_2MHZ    0UL

static inline void gpio_output_options_set(GPIO_TypeDef *port, uint32_t otype,
                                           uint32_t speed, uint32_t pin_mask)
{
    for (uint32_t pin = 0; pin < 16U; pin++) {
        if ((pin_mask & (1UL << pin)) == 0U) { continue; }

        port->OTYPER  = (port->OTYPER & ~(1UL << pin)) | (otype << pin);
        port->OSPEEDR = (port->OSPEEDR & ~(3UL << (pin * 2U)))
                      | (speed << (pin * 2U));
    }
}

/* BSRR, so no read-modify-write and nothing else on the port moves. */
static inline void gpio_bit_set(GPIO_TypeDef *port, uint32_t pin_mask)
{
    port->BSRR = pin_mask;
}

static inline void gpio_bit_reset(GPIO_TypeDef *port, uint32_t pin_mask)
{
    port->BSRR = pin_mask << 16U;
}

#endif /* GD32F4XX_H */
