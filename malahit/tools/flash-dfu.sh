#!/usr/bin/env bash
# Flash a .bin to a Malahit DSP-1 over the STM32 ROM bootloader.
#
# Enter DFU by closing jumper JP1 and powering up. JP1 is wired straight to
# BOOT0 through R15, with R17 holding it low otherwise — it is pure hardware,
# so this route still works no matter how badly a flashed image misbehaves.
# That is the reason this radio cannot be bricked from software.
#
# Usage:  ./flash-dfu.sh firmware.bin
set -euo pipefail

BIN="${1:?usage: flash-dfu.sh <firmware.bin>}"
[ -f "$BIN" ] || { echo "No such file: $BIN" >&2; exit 1; }

SIZE=$(stat -c%s "$BIN")
if [ "$SIZE" -gt $((2048 * 1024)) ]; then
    echo "Image is $SIZE bytes, larger than the 2 MB of flash." >&2
    exit 1
fi

if ! dfu-util -l 2>/dev/null | grep -q '0483:df11'; then
    echo "No STM32 DFU device found." >&2
    echo "Power the radio up with jumper JP1 closed, then plug in USB." >&2
    exit 1
fi

echo "Writing $BIN ($SIZE bytes) to 0x08000000 ..."
dfu-util -a 0 -s 0x08000000:leave -D "$BIN"

echo
echo "Done. Open JP1 and power-cycle to run the new image."
