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

/*
 * THE TRANSMIT RING - output that does not stop the radio.
 *
 * Polled, every byte waits for the line: 87 us at 115 200 baud. The
 * display loop's status line is ~300 characters, so it cost ~26 ms of
 * every ~113 ms frame - measured 20/11/2026, the single largest item
 * after the waterfall. The UART is only ~25 % busy at that rate, so
 * the bytes fit on the wire in the background; the CPU was simply
 * waiting for them.
 *
 * So from console_async(1) on - the start of the display loop - bytes
 * go into this ring and UART7's interrupt sends them. Nothing is ever
 * dropped: a full ring makes the writer wait, as before, pumping bytes
 * out itself so that it does not depend on the interrupt to make
 * progress.
 *
 * THE BOOT STAYS POLLED, and that is the trade this console has always
 * made: a buffered byte is not on the wire, and a run that hangs with
 * bytes in the ring loses them - the last lines, which are the ones
 * that say where it stopped. The boot is where runs hang, and it is a
 * second of output. In the loop the cases are covered as far as they
 * can be: the interrupt is at priority 0, so a block interrupt that
 * hangs still lets it drain; fault_park() flushes before it parks.
 * Only a hang with interrupts masked keeps what is queued - at the
 * loop's rate, about one line.
 *
 * Polled whenever buffering cannot work or should not be trusted: before
 * console_async(1), inside any exception, with interrupts masked. And a
 * polled byte sends everything queued first, so the order on the wire
 * is always the order of the calls.
 */
#define TX_RING     8192U                   /* a power of two */

static char              s_tx[TX_RING];
static volatile uint32_t s_head;    /* next free byte; the writer's only  */
static volatile uint32_t s_tail;    /* next to send; changed only by the
                                     * interrupt or with it masked        */
static volatile uint32_t s_async;
static volatile uint32_t s_stalls;  /* writes that found the ring full    */

static void tx_poll(char c)
{
    while (!(UART7->ISR & USART_ISR_TXE_TXFNF)) { }
    UART7->TDR = (uint8_t)c;
}

/*
 * One queued byte onto the wire, if there is one and the transmitter
 * has room. With interrupts masked so it cannot race UART7_IRQHandler
 * on s_tail - and restored as they were, so a caller that already had
 * them off (fault_park) keeps them off.
 */
static void tx_pump_one(void)
{
    uint32_t pm = __get_PRIMASK();

    __disable_irq();
    if (s_tail != s_head && (UART7->ISR & USART_ISR_TXE_TXFNF)) {
        UART7->TDR = (uint8_t)s_tx[s_tail & (TX_RING - 1U)];
        s_tail = s_tail + 1U;
    }
    __set_PRIMASK(pm);
}

void console_flush(void)
{
    /* At most a ring's worth of PROGRESS, whatever the indices say - so
     * a ring corrupted by whatever just crashed cannot keep fault_park()
     * here for ever. A transmitter that never empties can, exactly as
     * the polled console always could. */
    for (uint32_t sent = 0U; s_tail != s_head && sent <= TX_RING; ) {
        uint32_t t = s_tail;

        tx_pump_one();
        if (s_tail != t) { sent++; }
    }
}

void console_async(int on)
{
    if (on) {
        NVIC_SetPriority(UART7_IRQn, 0);
        NVIC_ClearPendingIRQ(UART7_IRQn);
        NVIC_EnableIRQ(UART7_IRQn);
        s_async = 1U;
    } else {
        s_async = 0U;
        console_flush();
        UART7->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
        NVIC_DisableIRQ(UART7_IRQn);
    }
}

uint32_t console_stalls(void) { return s_stalls; }

void UART7_IRQHandler(void)
{
    while (s_tail != s_head && (UART7->ISR & USART_ISR_TXE_TXFNF)) {
        UART7->TDR = (uint8_t)s_tx[s_tail & (TX_RING - 1U)];
        s_tail = s_tail + 1U;
    }
    if (s_tail == s_head) {
        UART7->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
    }
}

void console_putc(char c)
{
    if (!s_async || __get_IPSR() != 0U || __get_PRIMASK() != 0U) {
        if (s_tail != s_head) { console_flush(); }
        tx_poll(c);
        return;
    }

    if (s_head - s_tail >= TX_RING) {
        s_stalls++;
        while (s_head - s_tail >= TX_RING) { tx_pump_one(); }
    }

    s_tx[s_head & (TX_RING - 1U)] = c;
    __DMB();                    /* the byte before the index that shows it */
    s_head = s_head + 1U;

    /* The interrupt switches itself off when the ring runs dry; this
     * switches it back on. A race between the two costs one spurious
     * interrupt that finds nothing to send, never a lost byte. */
    UART7->CR1 |= USART_CR1_TXEIE_TXFNFIE;
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

static int  s_left;     /* '-' seen: pad on the right instead */

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

    if (s_left) {
        int digits = n;

        while (n > 0) { console_putc(buf[--n]); }
        while (digits < width) { console_putc(' '); digits++; }
        return;
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
        int  plus  = 0;

        /*
         * '+' - always show a sign.
         *
         * This exists because its absence did real damage. An unknown
         * flag falls through to the default case below, which prints
         * the two characters and MOVES ON WITHOUT CONSUMING THE
         * ARGUMENT - so one "%+3d" in a format string shifts every
         * later argument in that line by one. The visible result was a
         * column of plausible-looking decibel figures that were
         * actually the wrong variable, followed by a %s reading a
         * pointer from an integer and printing heap bytes.
         *
         * Numbers that are meaningful with a sign are exactly the ones
         * a measurement prints - frequency offsets, errors, image
         * ratios - so the flag belongs here rather than being worked
         * around at every call site.
         */
        if (*fmt == '+') {
            plus = 1;
            fmt++;
        }
        /*
         * '-' - pad on the right, and it is here for exactly the reason
         * the comment above gives for '+', having cost the same kind of
         * damage one step further on. A table of measurements printed
         * with "%-10s" produced the literal text "%-10s" in the column
         * and then read every later argument in that line one position
         * out: sample counts in the thousands of millions, a voltage
         * where a label belonged, and one number that was the NEXT
         * row's threshold. It looked like an instrumentation bug in the
         * thing being measured.
         *
         * Left-justified columns are what a table wants, so a format
         * string that asks for one should get it rather than quietly
         * poisoning the rest of the line.
         */
        s_left = 0;
        if (*fmt == '-') {
            s_left = 1;
            fmt++;
        }
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
            /*
             * THE SIGN BELONGS AGAINST THE NUMBER, which is not what
             * this did. It printed the sign and then asked
             * put_unsigned() to right-justify the digits inside the
             * remaining width, so "%+4d" of -4 came out as "-  4" -
             * the sign stranded at the far left of the column with
             * three spaces between it and its own digits. In a table
             * of trim values that is not a typo, it is a different
             * number to the eye.
             *
             * Correct is: work out how wide the whole thing is, pad to
             * the column first, then sign, then digits. Zero padding is
             * the one exception and goes AFTER the sign - "-0042", not
             * "00-42".
             */
            int32_t  v    = va_arg(ap, int32_t);
            uint32_t mag  = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
            int      sign = (v < 0) || plus;
            int      n    = sign;
            uint32_t t    = mag;

            do { n++; t /= 10U; } while (t != 0U);

            if (!s_left && pad != '0') {
                while (n < width) { console_putc(' '); n++; }
            }

            if (v < 0)   { console_putc('-'); }
            else if (plus) { console_putc('+'); }

            if (!s_left && pad == '0') {
                while (n < width) { console_putc('0'); n++; }
            }

            {
                int left = s_left;

                s_left = 0;                 /* the digits never pad */
                put_unsigned(mag, 10U, 0, ' ');
                s_left = left;
            }

            if (s_left) { while (n < width) { console_putc(' '); n++; } }
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
            const char *t = s ? s : "(null)";
            int         n = 0;

            while (t[n] != '\0') { n++; }

            int padn = width - n;

            if (!s_left) { while (padn-- > 0) { console_putc(' '); } }
            console_puts(t);
            if (s_left)  { while (padn-- > 0) { console_putc(' '); } }
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
