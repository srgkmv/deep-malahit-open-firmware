#include <stdarg.h>

#include "stm32h7xx.h"
#include "console.h"
#include "clock.h"

#define CONSOLE_BAUD    115200U

void console_init(void)
{
    RCC->AHB4ENR  |= RCC_AHB4ENR_GPIOEEN;
    RCC->APB1LENR |= RCC_APB1LENR_UART7EN;
    (void)RCC->APB1LENR;

    /* PE7 and PE8 to alternate function 7. */
    for (uint32_t pin = 7U; pin <= 8U; pin++) {
        GPIOE->MODER = (GPIOE->MODER & ~(3UL << (pin * 2U)))
                     | (2UL << (pin * 2U));            /* 10 = alternate  */
        GPIOE->OSPEEDR |= (3UL << (pin * 2U));         /* very high speed */
        GPIOE->PUPDR = (GPIOE->PUPDR & ~(3UL << (pin * 2U)))
                     | (1UL << (pin * 2U));            /* pull-up: idle   */
        GPIOE->AFR[pin >> 3U] =
              (GPIOE->AFR[pin >> 3U] & ~(0xFUL << ((pin & 7U) * 4U)))
            | (7UL << ((pin & 7U) * 4U));              /* AF7 = UART7     */
    }

    /*
     * UART7's kernel clock selector (RCC_D2CCIP2R, USART234578SEL) is left
     * at its reset value of 000, which is rcc_pclk1. So the divisor comes
     * from the APB1 frequency that clock_init() actually reached - 120 MHz
     * if the PLL came up, 64 MHz if it did not. Deriving it instead of
     * hard-coding is what lets a failed PLL still print the reason it
     * failed, which is the one message that matters most.
     */
    UART7->CR1 = 0U;                                   /* disable to edit */
    UART7->BRR = (clock_apb1_hz + (CONSOLE_BAUD / 2U)) / CONSOLE_BAUD;
    UART7->CR2 = 0U;                                   /* 1 stop bit      */
    UART7->CR3 = 0U;                                   /* no flow control */
    UART7->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;

    while (!(UART7->ISR & USART_ISR_TEACK)) { }
}

void console_putc(char c)
{
    while (!(UART7->ISR & USART_ISR_TXE_TXFNF)) { }
    UART7->TDR = (uint8_t)c;
}

void console_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            console_putc('\r');
        }
        console_putc(*s++);
    }
}

static void put_unsigned(uint32_t v, uint32_t base, int width, char pad)
{
    char buf[11];
    int  n = 0;

    if (v == 0U) {
        buf[n++] = '0';
    }
    while (v != 0U) {
        uint32_t d = v % base;
        buf[n++] = (char)(d < 10U ? ('0' + d) : ('a' + d - 10U));
        v /= base;
    }
    while (n < width) {
        buf[n++] = pad;
    }
    while (n > 0) {
        console_putc(buf[--n]);
    }
}

void console_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') {
                console_putc('\r');
            }
            console_putc(*fmt++);
            continue;
        }
        fmt++;                                  /* skip '%' */

        char pad   = ' ';
        int  width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') {                      /* %lu, %lx: same size here */
            fmt++;
        }

        switch (*fmt++) {
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) {
                console_putc('-');
                put_unsigned((uint32_t)(-v), 10U, width, pad);
            } else {
                put_unsigned((uint32_t)v, 10U, width, pad);
            }
            break;
        }
        case 'u':
            put_unsigned(va_arg(ap, uint32_t), 10U, width, pad);
            break;
        case 'x':
            put_unsigned(va_arg(ap, uint32_t), 16U, width, pad);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            console_puts(s ? s : "(null)");
            break;
        }
        case 'c':
            console_putc((char)va_arg(ap, int));
            break;
        case '%':
            console_putc('%');
            break;
        default:
            /* Unknown conversion: show it rather than silently eating it,
             * so a typo in a format string is visible on the wire. */
            console_putc('%');
            console_putc(*(fmt - 1));
            break;
        }
    }

    va_end(ap);
}
