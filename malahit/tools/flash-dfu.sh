#!/usr/bin/env bash
# Flash a .bin to a Malahit DSP-1 over the STM32 ROM bootloader.
#
# Enter DFU the way this unit offers it: HOLD THE ENCODER BUTTONS AND
# PRESS THE POWER BUTTON. There is no JP1 on this board - the schematic
# shows the jumper, but it is not fitted - so BOOT0 cannot be reached
# and the button combination is implemented by the FACTORY FIRMWARE.
#
# Which means this route is NOT a hardware safety net. It lives in the
# very image that flashing replaces. Read BOARD.md section 4 before
# writing anything to flash: there has to be another way back first.
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
    echo "Hold the encoder buttons and press power to enter DFU." >&2
    exit 1
fi

echo "Writing $BIN ($SIZE bytes) to 0x08000000 ..."
dfu-util -a 0 -s 0x08000000:leave -D "$BIN"

echo
echo "Done. Power-cycle to run the new image."
