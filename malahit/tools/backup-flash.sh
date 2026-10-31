#!/usr/bin/env bash
# Read the whole 2 MB of internal flash out of a Malahit DSP-1 and save it,
# named by date and by the chip's own unique ID, so two radios never collide.
#
# Works over DFU (jumper JP1 closed at power-up). The part ships with RDP
# level 0, so there is nothing to defeat.
#
# Usage:  ./backup-flash.sh [output-directory]
set -euo pipefail

OUTDIR="${1:-.}"
mkdir -p "$OUTDIR"

if ! dfu-util -l 2>/dev/null | grep -q '0483:df11'; then
    echo "No STM32 DFU device found." >&2
    echo "Power the radio up with jumper JP1 closed, then plug in USB." >&2
    exit 1
fi

SERIAL=$(dfu-util -l 2>/dev/null | sed -n 's/.*serial="\([^"]*\)".*/\1/p' | head -1)
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$OUTDIR/malahit-stock-${SERIAL:-unknown}-$STAMP.bin"

echo "Reading 2048 kB from 0x08000000 ..."
dfu-util -a 0 -s 0x08000000:2097152 -U "$OUT"

echo
echo "Saved: $OUT"
sha256sum "$OUT"
echo
echo "Keep this file. It is the only way back to the factory firmware."
