/*
 * snprintf() FOR UPSTREAM'S ident.c - step 53, 23/11/2026.
 *
 * ident.c is the first upstream file grafted here that formats with
 * snprintf(), and this port links with -nostdlib: newlib's snprintf() would
 * bring its whole stdio behind it - _sbrk, _write, _close and the rest of
 * the system calls this firmware does not have, and some kilobytes of
 * float formatting nobody asks for. So the conversions ident.c uses are
 * here, and only those:
 *
 *     %d  %u  %s  %%   with an optional '0' and a width, as in %02d
 *
 * Anything else is copied as it stands, so a new format that slips in
 * shows on the screen instead of reading past the arguments. Like the C
 * one, it always ends the string and returns how long it wanted to be.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

/* One character out: written while there is room, counted always. A
 * function and not a macro: the macro took its argument only when there
 * was room, so PON(t[--m]) stopped counting down once the string was
 * cut - and looped for ever (found on the host before it reached the
 * radio). */
static void pon(char *buf, size_t n, size_t *k, char c)
{
    if ((*k + 1U) < n) { buf[*k] = c; }
    (*k)++;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    size_t k = 0U;

#define PON(c) pon(buf, n, &k, (c))

    while (*fmt != '\0') {
        char cero = ' ';
        unsigned ancho = 0U;

        if (*fmt != '%') { PON(*fmt); fmt++; continue; }
        fmt++;
        if (*fmt == '%') { PON('%'); fmt++; continue; }
        if (*fmt == '0') { cero = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { ancho = (ancho * 10U) + (unsigned)(*fmt - '0'); fmt++; }
        if (*fmt == 'd' || *fmt == 'u') {
            char t[12];
            unsigned m = 0U, v, i;
            int neg = 0;

            if (*fmt == 'd') {
                int d = va_arg(ap, int);

                neg = (d < 0);
                v = neg ? (0U - (unsigned)d) : (unsigned)d;
            } else {
                v = va_arg(ap, unsigned);
            }
            do { t[m++] = (char)('0' + (v % 10U)); v /= 10U; } while (v != 0U);
            if (neg && cero == '0') { PON('-'); }
            for (i = m + (unsigned)neg; i < ancho; i++) { PON(cero); }
            if (neg && cero != '0') { PON('-'); }
            while (m > 0U) { PON(t[--m]); }
            fmt++;
            continue;
        }
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            unsigned m = 0U, i;

            if (s == (const char *)0) { s = "(null)"; }
            while (s[m] != '\0') { m++; }
            for (i = m; i < ancho; i++) { PON(' '); }
            while (*s != '\0') { PON(*s); s++; }
            fmt++;
            continue;
        }
        PON('%');                       /* not ours: shown as written */
        if (*fmt != '\0') { PON(*fmt); fmt++; }
    }
    if (n > 0U) { buf[(k < n) ? k : (n - 1U)] = '\0'; }
#undef PON
    return (int)k;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
