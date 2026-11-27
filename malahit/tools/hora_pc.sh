#!/usr/bin/env bash
#
# Set the radio's clock from the PC's - step 68, 28/11/2026.
#
#   tools/hora_pc.sh [elf]
#
# For anyone who cannot hit "On the minute" to the second: the PC's clock,
# kept by NTP, is better than a finger. The radio must be on, with the SWD
# adapter on it (ADAPTER, ft2232d unless said otherwise), and <elf> must
# be the image in its flash (default bringup/build_flash/bringup.elf).
#
# HOW: rtc_desvio.tcl reads the RTC against the PC's clock without halting
# the radio (a few ms); the firmware's radio_hora_pc() is called with that
# difference and moves the RTC by it, taking the zone from the radio's own
# setting. Relative, so the second or so the call takes does not count. It
# is a setting good to 10 ms: it anchors, and the next run an hour or more
# later teaches the clock its crystal (the calibration, CALR) - see
# src/rtc_hw.c. The radio's console says what it did.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELF="${1:-$HERE/../bringup/build_flash/bringup.elf}"
export ADAPTER="${ADAPTER:-ft2232d}"

[ -f "$ELF" ] || { echo "no $ELF - build it, or name the image in flash" >&2; exit 1; }
if command -v timedatectl >/dev/null 2>&1 \
   && ! timedatectl show -p NTPSynchronized --value 2>/dev/null | grep -qx yes; then
    echo "*** the PC's clock is not NTP-synchronised: it may be worse than the radio's ***" >&2
fi

mide() {
    "$HERE/swd.sh" -c init -c "source $HERE/rtc_desvio.tcl" -c exit 2>&1 \
        | awk '/^RTC_MENOS_UTC_MS/ {print $2}'
}
# What is left once the zone (a whole quarter of an hour) is taken away.
error_ms() { awk -v d="$1" 'BEGIN { q = 900000; r = d - q * int((d + (d >= 0 ? q / 2 : -q / 2)) / q); print r }'; }

d=$(mide)
[ -n "$d" ] || { echo "could not read the RTC - is the radio on, the adapter on it?" >&2; exit 1; }
echo "before: the radio's clock $(error_ms "$d") ms off the PC's"

"$HERE/swd-call.sh" "$ELF" radio_hora_pc "$(printf '0x%08x' $((d & 0xFFFFFFFF)))" >/dev/null

sleep 1
d2=$(mide)
[ -n "$d2" ] || { echo "could not read the RTC after" >&2; exit 1; }
echo "after:  the radio's clock $(error_ms "$d2") ms off the PC's"
