#!/usr/bin/env python3
"""
A screenshot of the Malahit's panel, read back off the glass - 21/11/2026.

    ADAPTER=ft2232d tools/grab.py bringup/build/bringup.elf out.png

The ELF must be the image that is running. Over SWD, in ONE OpenOCD
session: for each strip of GRAB_ROWS rows, call panel_grab(y0, rows)
from the display loop the way swd-call.sh calls anything (breakpoint on
the loop's call to encoder_take_delta(), arguments in r0/r1, LR back to
the call site), wait for g_grab_seq to move, dump g_grab. Then the
strips are put together here.

THE PHASE IS FOUND, NOT ASSUMED. RAMRD over the 4-wire bus may lead with
a dummy - a byte, or on some ILI9488 commands a single clock - and this
panel already disagreed once with what drivers assume (BOARD.md: no
dummy byte on register reads). In 18-bit mode every colour byte has its
two low bits zero, so the bit offset is the one at which the fewest
bytes have them set. A whole-byte offset cannot be seen that way - it
rotates R, G and B - and is --byte. It is ONE, settled on the first
capture, 21/11/2026: read with none, every pixel came out (B, R, G) -
the amber pills teal, the blue waterfall dark red - against the host
render of the same screen. So RAMRD has the dummy byte that register
reads on this panel do not.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
W, H, ROWS = 480, 320, 32


def nm(elf):
    out = subprocess.run(["arm-none-eabi-nm", "-S", elf], check=True,
                         capture_output=True, text=True).stdout
    sym = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            sym[p[3]] = (int(p[0], 16), int(p[1], 16))
        elif len(p) == 3:
            sym.setdefault(p[2], (int(p[0], 16), 0))
    return sym


def call_sites(elf, sym):
    start, size = sym["main"]
    out = subprocess.run(["arm-none-eabi-objdump", "-d",
                          "--start-address=0x%x" % start,
                          "--stop-address=0x%x" % (start + size), elf],
                         check=True, capture_output=True, text=True).stdout
    sites = []
    for line in out.splitlines():
        if re.search(r"bl.*<encoder_take_delta>", line):
            sites.append(int(line.split(":")[0], 16))
    if not sites:
        sys.exit("no call to encoder_take_delta() in main()")
    return sites


def tcl(sym, sites, tmp):
    fn = sym["panel_grab"][0]
    buf = sym["g_grab"][0]
    seq = sym["g_grab_seq"][0]
    lines = ["init"]
    for i, y0 in enumerate(range(0, H, ROWS)):
        rows = min(ROWS, H - y0)
        n = rows * W * 3 + 4
        lines += ["bp 0x%x 2 hw" % s for s in sites]
        lines += [
            "wait_halt 3000",
            "set old [read_memory 0x%x 32 1]" % seq,
            "reg r0 %d" % y0,
            "reg r1 %d" % rows,
            "reg lr [format 0x%08x [expr {[lindex [reg pc] 2] | 1}]]",
            "reg pc 0x%x" % fn,
        ]
        lines += ["rbp 0x%x" % s for s in sites]
        lines += [
            "resume",
            "set t 0",
            "while {[read_memory 0x%x 32 1] == $old && $t < 300} "
            "{ sleep 10; incr t }" % seq,
            "if {$t >= 300} { echo \"strip %d: no answer\" }" % i,
            "dump_image %s/strip%02d.bin 0x%x %d" % (tmp, i, buf, n),
        ]
    lines.append("exit")
    path = os.path.join(tmp, "grab.tcl")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path


def bits_of(data):
    v = int.from_bytes(data, "big")
    return v, len(data) * 8


def realign(data, bit_off):
    """The stream shifted left by bit_off bits, as bytes."""
    if bit_off == 0:
        return data
    v, nbits = bits_of(data)
    v = (v << bit_off) & ((1 << nbits) - 1)
    return v.to_bytes(len(data), "big")


def best_bit_offset(data):
    scores = []
    for b in range(8):
        d = realign(data, b)
        scores.append((sum(1 for x in d if x & 3), b))
    scores.sort()
    return scores[0][1], scores


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("out")
    ap.add_argument("--byte", type=int, default=1,
                    help="whole bytes of dummy before the first pixel")
    ap.add_argument("--reuse", action="store_true",
                    help="assemble the strips already in --keep, no capture")
    ap.add_argument("--keep", help="keep the raw strips in this directory")
    a = ap.parse_args()

    sym = nm(a.elf)
    for need in ("panel_grab", "g_grab", "g_grab_seq", "main"):
        if need not in sym:
            sys.exit("%s not in %s - is this the running image?" % (need, a.elf))
    sites = call_sites(a.elf, sym)

    tmp = a.keep or tempfile.mkdtemp(prefix="grab")
    os.makedirs(tmp, exist_ok=True)
    if not a.reuse:
        script = tcl(sym, sites, tmp)
        r = subprocess.run([os.path.join(HERE, "swd.sh"), "-f", script],
                           capture_output=True, text=True,
                           env=dict(os.environ))
        for line in (r.stdout + r.stderr).splitlines():
            if "no answer" in line or "Error" in line:
                print(line)

    raw = bytearray()
    for i, y0 in enumerate(range(0, H, ROWS)):
        rows = min(ROWS, H - y0)
        p = os.path.join(tmp, "strip%02d.bin" % i)
        if not os.path.exists(p):
            sys.exit("strip %d missing - see the OpenOCD output above" % i)
        d = open(p, "rb").read()
        off, scores = best_bit_offset(d[:4096])
        d = realign(d, off)[a.byte:a.byte + rows * W * 3]
        if i == 0:
            print("bit offset %d (bytes with low bits set, by offset: %s)"
                  % (off, " ".join("%d:%d" % (b, s) for s, b in sorted(
                      scores, key=lambda t: t[1]))))
        raw += d

    from PIL import Image
    img = Image.frombytes("RGB", (W, H), bytes(raw))
    img.save(a.out)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
