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
#   ./swd.sh fault                  print the last crash record, if any
#   ./swd.sh clean                  silence the core after a crash
#   ./swd.sh flash firmware.elf     DO NOT USE on this board: it relies on
#                                   "reset halt", which does not halt here
#                                   (WORKING.md, "The way back"). Flash is
#                                   written over ROM DFU - see "make flash".
#   ./swd.sh flashsrst image.bin    write a flash image (make flash's
#                                   build_flash/bringup.bin) to bank 1 over
#                                   SWD, CONNECTING UNDER RESET, verify it,
#                                   and release the core. FT2232D only (it
#                                   drives NRST), and only since the owner's
#                                   power change of 22/11/2026 (WORKING.md,
#                                   "Flash over SWD") - before it, a reset
#                                   cut the rail and the connection with it.
#                                   Works with the radio "asleep": the
#                                   firmware then switches itself off again,
#                                   as after a DFU write; a short press on
#                                   the power button starts it.
#                                   The "cannot read IDR" OpenOCD prints
#                                   after the verify is that: the core it
#                                   let go is already asleep.
#   ./swd.sh reset                  reset and run
#   ./swd.sh -c "<openocd cmd>" ... run arbitrary commands, then exit
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Which cable. Default is the ST-LINK; export ADAPTER=ft2232d for the
# FTDI dongle, which is the one that can actually drive reset.
#
#   ADAPTER=ft2232d ./swd.sh id
#   ADAPTER=ft2232d make ram      (the makefile passes it through)
case "${ADAPTER:-stlink}" in
    stlink)   CFG="$HERE/../openocd.cfg" ;;
    ft2232d)  CFG="$HERE/../openocd-ft2232d.cfg" ;;
    *)        echo "unknown ADAPTER: $ADAPTER (stlink | ft2232d)" >&2; exit 1 ;;
esac

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
        # ram_run lives in ../openocd.cfg. It cleans the core by hand
        # instead of resetting it, because NRST is not wired on this
        # board and "reset halt" would silently do nothing.
        run -c "init" -c "ram_run $ELF" -c "exit"
        ;;
    fault)
        run -c "init" -c "fault" -c "exit"
        ;;
    clean)
        run -c "init" -c "core_clean" -c "exit"
        ;;
    flashsrst)
        BIN="${2:?usage: swd.sh flashsrst <image.bin>}"
        [ "${ADAPTER:-stlink}" = ft2232d ] || { echo "flashsrst needs ADAPTER=ft2232d (NRST)" >&2; exit 1; }
        # Bank 1 from 0x08000000, the sectors the image covers and no more:
        # bank 2 (0x08100000) holds this unit's own data and is never erased.
        run -c "reset_config srst_only srst_push_pull connect_assert_srst" \
            -c "init" -c "reset halt" \
            -c "flash write_image erase $BIN 0x08000000 bin" \
            -c "verify_image $BIN 0x08000000 bin" \
            -c "reset run" -c "exit"
        ;;
    flash)
        ELF="${2:?usage: swd.sh flash <firmware.elf>}"
        run -c "init" -c "reset halt" \
            -c "program $ELF verify reset" -c "exit"
        ;;
    *)       run "$@" ;;
esac
