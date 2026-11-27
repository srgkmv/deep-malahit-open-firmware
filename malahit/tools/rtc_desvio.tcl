# The radio's RTC against the PC's clock, over SWD, WITHOUT halting: the
# RTC's SSR, TR, DR (SSR first - it freezes TR and DR until DR is read)
# bracketed by the PC's microseconds, five times 200 ms apart, averaged.
# Prints "RTC_MENOS_UTC_MS <n>": the RTC's time of day minus the PC's
# UTC, in ms, wrapped to +-12 h - the radio's zone plus the clock's error.
# Step 56's scratch script, kept for tools/hora_pc.sh (step 68).
#
#   ADAPTER=ft2232d tools/swd.sh -c init -c "source tools/rtc_desvio.tcl" -c exit
#
# RTC on the STM32H743: TR 0x58004000, DR 0x58004004, SSR 0x58004028
# (PREDIV_S 255: SSR counts 255..0 within the second).
proc bcd {v} { return [expr {($v >> 4) * 10 + ($v & 15)}] }
set sum 0.0
for {set k 0} {$k < 5} {incr k} {
    set t0  [clock microseconds]
    set ssr [lindex [read_memory 0x58004028 32 1] 0]
    set tr  [lindex [read_memory 0x58004000 32 1] 0]
    set dr  [lindex [read_memory 0x58004004 32 1] 0]
    set t1  [clock microseconds]
    set utc [expr {fmod(($t0 + $t1) / 2.0 / 1e6, 86400.0)}]
    set h [bcd [expr {($tr >> 16) & 0x3f}]]
    set m [bcd [expr {($tr >> 8) & 0x7f}]]
    set s [bcd [expr {$tr & 0x7f}]]
    set rtc [expr {$h * 3600 + $m * 60 + $s + (255 - ($ssr & 0xffff)) / 256.0}]
    set d [expr {$rtc - $utc}]
    if {$d >  43200.0} { set d [expr {$d - 86400.0}] }
    if {$d < -43200.0} { set d [expr {$d + 86400.0}] }
    set sum [expr {$sum + $d}]
    after 200
}
echo [format "RTC_MENOS_UTC_MS %d" [expr {round($sum / 5.0 * 1000.0)}]]
echo [format "RTC date 20%02d-%02d-%02d" [bcd [expr {($dr >> 16) & 0xff}]] \
          [bcd [expr {($dr >> 8) & 0x1f}]] [bcd [expr {$dr & 0x3f}]]]
