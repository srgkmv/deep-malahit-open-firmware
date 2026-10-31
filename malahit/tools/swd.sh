#!/usr/bin/env bash
# Talk to a Malahit DSP-1 over SWD with the right transport.
#
# OpenOCD's default stlink.cfg uses the HLA transport, which does not work
# with this board — see the comment at the top of ../openocd.cfg. This wrapper
# always uses the config next to it, which selects dapdirect_swd.
#
#   ./swd.sh                        open an interactive session (telnet :4444,
#                                   gdb :3333), Ctrl-C to quit
#   ./swd.sh id                     print chip ID, flash size, unique ID
#   ./swd.sh rail                   switch the peripheral 3.3V rail on
#   ./swd.sh light                  rail + backlight on
#   ./swd.sh ram firmware.elf       load into AXI SRAM and run it, WITHOUT
#                                   touching flash — the factory firmware
#                                   survives and a power cycle restores it
#   ./swd.sh flash firmware.elf     program and verify over SWD
#   ./swd.sh reset                  reset and run
#   ./swd.sh -c "<openocd cmd>" ... run arbitrary commands, then exit
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG="$HERE/../openocd.cfg"

run() { openocd -f "$CFG" "$@"; }

case "${1-}" in
    "")      run ;;
    id)      run -c "init" -c "whoami" -c "exit" ;;
    rail)    run -c "init" -c "rail_on" -c "exit" ;;
    light)   run -c "init" -c "rail_on" -c "backlight_on" -c "exit" ;;
    dark)    run -c "init" -c "backlight_off" -c "exit" ;;
    reset)   run -c "init" -c "reset run" -c "exit" ;;
    ram)
        ELF="${2:?usage: swd.sh ram <firmware.elf>}"
        # reset halt first, so the image starts from a chip in its reset
        # state rather than from whatever the factory firmware had already
        # configured. The halt catches the core at the reset vector, before
        # a single instruction of the flash image has run.
        #
        # Then start it by hand: no reset follows the load, so the core
        # never pops SP and PC off the vector table itself. Both come out
        # of our table at 0x24000000, and xPSR gets its Thumb bit set
        # explicitly, which is the one thing that turns a silent HardFault
        # into a working jump.
        run -c "init" -c "reset halt" \
            -c "load_image $ELF" \
            -c "reg sp [mrw 0x24000000]" \
            -c "reg pc [mrw 0x24000004]" \
            -c "reg xPSR 0x01000000" \
            -c "resume" -c "exit"
        ;;
    flash)
        ELF="${2:?usage: swd.sh flash <firmware.elf>}"
        run -c "init" -c "reset halt" \
            -c "program $ELF verify reset" -c "exit"
        ;;
    *)       run "$@" ;;
esac
