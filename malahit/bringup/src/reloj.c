/*
 * THE CLOCK UPSTREAM'S DECODERS ASK FOR - step 50, 22/11/2026.
 *
 * Upstream's rtc_hw.c reads the GD32's RTC, which its owner sets from GPS,
 * DCF77 or by hand. This port has no settable clock yet (SIGUIENTE.md: the
 * clock from RDS needs one first), and its header shows the time since
 * power-on (ui_fmt_uptime()). So rtc_hw_get() answers the same: hours and
 * minutes since boot - the "hh:mm" JTTY puts on each line then agrees with
 * the clock on the screen, and both say plainly that they are not the time
 * of day. The date stays at zero.
 *
 * Only rtc_hw_get() is here: it is all the decoders grafted so far ask for.
 * The linker will say when another needs more.
 */
#include <string.h>
#include "rtc_hw.h"     /* ../../User - the contract */
#include "board.h"      /* board_millis() */

void rtc_hw_get(rtc_hw_datetime_t *out)
{
    uint32_t s = board_millis() / 1000U;

    if (out == (rtc_hw_datetime_t *)0) { return; }
    memset(out, 0, sizeof *out);
    out->hour   = (uint8_t)((s / 3600U) % 24U);
    out->minute = (uint8_t)((s / 60U) % 60U);
    out->second = (uint8_t)(s % 60U);
}
