#!/usr/bin/env bash
# Hold the adapter's reset line asserted for 20 seconds so it can be
# measured, because asking OpenOCD whether it worked does not answer the
# question.
#
# WHY THIS EXISTS. On this board NRST is wired from J2 pin 1 to the
# adapter and the wire has been continuity-checked, yet "reset halt" times
# out and an assert/deassert cycle leaves VTOR untouched - proof that the
# target never saw a reset. Two explanations fit equally well from the PC
# side:
#
#   1. The adapter's reset output is dead or not connected internally.
#      Common on ST-LINK/V2 clones, where the pin exists on the header
#      and goes nowhere.
#   2. OpenOCD's stlink-dap backend does not drive reset at all. This
#      board needs dapdirect_swd - the hla_swd transport cannot talk to
#      the part - and srst support differs between the two backends.
#
# A multimeter settles it in one measurement, and nothing else will.
#
# RUN IT, then with the probes on J2 pin 1 (NRST) and any ground:
#
#   reads ~0 V while held   -> the adapter drives reset; the problem is
#                              elsewhere, probably reset timing
#   stays at ~3.3 V         -> the adapter is not driving it. Either its
#                              reset pin is dead, or this OpenOCD backend
#                              cannot drive it. An FTDI-based adapter
#                              (FT2232) drives reset as a plain GPIO and
#                              sidesteps the whole question.
#
# Measure at the adapter's own reset pin too: 0 V there and 3.3 V at J2
# would mean a broken lead rather than a broken adapter.
set -euo pipefail

HOLD="${1:-20}"

echo "Asserting reset for $HOLD s. Measure J2 pin 1 against ground now."
echo

openocd \
    -f interface/stlink-dap.cfg \
    -c "transport select dapdirect_swd; adapter speed 1800" \
    -f target/stm32h7x.cfg \
    -c "reset_config srst_only srst_nogate" \
    -c "init" \
    -c "adapter assert srst" \
    -c "echo {>>> RESET ASSERTED - measure now <<<}" \
    -c "sleep ${HOLD}000" \
    -c "adapter deassert srst" \
    -c "echo {>>> released <<<}" \
    -c "exit"
