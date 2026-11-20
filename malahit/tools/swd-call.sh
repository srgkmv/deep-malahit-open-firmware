#!/usr/bin/env bash
#
# Call one firmware function on the RUNNING radio, from the display loop,
# and let it carry on - no reload, no restart.
#
#   ADAPTER=ft2232d tools/swd-call.sh <elf> <function> [r0] [r1] [r2] [r3]
#   ADAPTER=ft2232d tools/swd-call.sh build_flash/bringup.elf msi001_retune 9988000
#
# The ELF must be the image that is running (build_flash/ for what is in
# flash, build/ for a RAM load), or every address below is wrong.
#
# HOW: a hardware breakpoint on every call to encoder_take_delta() in
# main() - the first thing the display loop does each frame. At a call
# site the scratch registers (r0-r3, r12, s0-s15) are dead by the ABI, so
# they can be overwritten: the arguments go in r0-r3, LR is set to the
# call site itself and PC to <function>. It runs with interrupts as they
# were, returns to the call site, and the loop makes its own call as if
# nothing had happened. Tried 21/11/2026 with msi001_retune() and three
# relay pulses: the radio carried on, audio and all.
#
# Written to bisect a cold-start fault that only a re-init cured: call
# the init steps one by one on a radio that has the fault, and watch the
# console's baseband: line after each. See SIGUIENTE.md.
#
# Integers only (decimal or 0x...). Pointers, floats and structs need more
# than four registers' worth of care and are deliberately not supported.
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELF="${1:?usage: swd-call.sh <elf> <function> [r0..r3]}"
FN="${2:?usage: swd-call.sh <elf> <function> [r0..r3]}"
shift 2

addr=$(arm-none-eabi-nm "$ELF" | awk -v f="$FN" '$3==f && ($2=="T" || $2=="t") && !n++ {print "0x"$1}')
[ -n "$addr" ] || { echo "no function '$FN' in $ELF (inlined or garbage-collected?)" >&2; exit 1; }

# main()'s extent from the symbol table, so objdump stops by itself: an
# awk that exits early hands objdump a SIGPIPE, and under pipefail and
# set -e the whole script then ends silently - which it did, once.
read -r mstart msize < <(arm-none-eabi-nm -S "$ELF" | awk '$4=="main"{print $1, $2}')
mend=$(printf '0x%x' $((0x$mstart + 0x$msize)))
sites=$(arm-none-eabi-objdump -d --start-address=0x"$mstart" --stop-address="$mend" "$ELF" \
        | awk '/bl.*<encoder_take_delta>/ {sub(":","",$1); print "0x"$1}')
[ -n "$sites" ] || { echo "no call to encoder_take_delta() in main()" >&2; exit 1; }

args=(-c init)
for s in $sites; do args+=(-c "bp $s 2 hw"); done
args+=(-c "wait_halt 3000" -c 'echo "stopped at [lindex [reg pc] 2]"')
i=0
for a in "$@"; do args+=(-c "reg r$i $a"); i=$((i + 1)); done
args+=(-c 'reg lr [format 0x%08x [expr {[lindex [reg pc] 2] | 1}]]' -c "reg pc $addr")
for s in $sites; do args+=(-c "rbp $s"); done
args+=(-c resume -c exit)

echo "calling $FN at $addr ($*) from the loop, sites: $(echo $sites)"
"$HERE/swd.sh" "${args[@]}" 2>&1 | grep -E '^stopped at|Error' || true
