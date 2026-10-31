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
    flash)
        ELF="${2:?usage: swd.sh flash <firmware.elf>}"
        run -c "init" -c "reset halt" \
            -c "program $ELF verify reset" -c "exit"
        ;;
    *)       run "$@" ;;
esac
