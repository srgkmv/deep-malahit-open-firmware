#!/usr/bin/env python3
"""
The faces this port generates for itself, into malahit/compat/.

WHY A SEPARATE SCRIPT AND NOT A LINE IN gen_assets.py'S TABLE.

deepsdr-ui/tools/gen_assets.py regenerates the DEEPSDR's own seven
faces into deepsdr-ui/gfx2/ and then runs split_fonts over them. Those
are upstream's assets, this port does not touch them, and the whole
point of the arrangement is that `make ref800-comprueba` can prove it
did not. Adding Malahit faces to that table would mean running a tool
that rewrites nine upstream files every time this port wants an eighth
face.

So the RENDERER is reused - rendering a face is not something to have
two copies of - and only the table and the output directory are ours.
The generated headers say gen_assets.py in their banner because that
is still what produced the bytes.

    python3 malahit/tools/gen_fuentes.py

The faces, and why each exists:

  font_ui_8 / 8b    the first try at the status chips. Six of them have
                    to fit a 480 px row, and at 11 px they did not.
  font_ui_10 / 10b  what replaced them, 03/11/2026: read on the real
                    panel, 8 px is legible in a PNG at desk distance
                    and marginal on the glass. Two pixels back.
  font_num_31       the frequency readout. Nothing sat between
                    font_num_20 at 21 px, which makes a receiver's main
                    readout look like a label, and font_num_44 at 46,
                    which does not fit a 320-row panel.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
UI   = os.path.join(HERE, "..", "..", "deepsdr-ui", "tools")
OUT  = os.path.normpath(os.path.join(HERE, "..", "compat"))

#
# gen_assets.py IS LOADED, NOT IMPORTED, and the tail is cut off first.
#
# Its last two statements are `import split_fonts` and
# `split_fonts.split(OUT)` at MODULE level, so a plain `import
# gen_assets` would rewrite upstream's font headers as a side effect of
# asking for a function. Everything from the `if __name__` guard
# onwards is dropped, which leaves the renderer and the charsets and
# runs nothing.
#
src = open(os.path.join(UI, "gen_assets.py")).read()
cut = src.index('if __name__ == "__main__":')
ns = {"__file__": os.path.join(UI, "gen_assets.py"), "__name__": "gen_assets"}
exec(compile(src[:cut], "gen_assets.py", "exec"), ns)      # noqa: S102

#
# The DejaVu paths in gen_assets.py are Debian's. This port is built on
# Arch, where the same files live under /usr/share/fonts/TTF. Resolved
# here rather than edited there, for the same reason as everything
# else in this file.
#
CANDIDATES = ["/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/TTF"]


def dejavu(name):
    for d in CANDIDATES:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    sys.exit("no encuentro %s en %s" % (name, " ni ".join(CANDIDATES)))


ASCII   = ns["ASCII"]
SPANISH = ns["SPANISH"]
DIGITS  = ns["DIGITS"]

C  = dejavu("DejaVuSansCondensed.ttf")
CB = dejavu("DejaVuSansCondensed-Bold.ttf")
MB = dejavu("DejaVuSansMono-Bold.ttf")

FUENTES = [
    ("font_ui_8",   C,   8, ASCII + SPANISH,
     "Chapas de estado del Malahit DSP-1: un tercio mas pequena que "
     "font_ui_11, para que quepan seis en una fila de 480 px."),
    ("font_ui_8b",  CB,  8, ASCII + SPANISH,
     "La misma en negrita, para la chapa activa."),
    ("font_ui_10",  C,  10, ASCII + SPANISH,
     "Chapas de estado del Malahit DSP-1: dos pixeles mas que la de 8, "
     "que en el panel de verdad se leia justa."),
    ("font_ui_10b", CB, 10, ASCII + SPANISH,
     "Chapas de estado del Malahit DSP-1, negrita, 10 px."),
    ("font_num_31", MB, 31, DIGITS,
     "Lector de frecuencia del Malahit DSP-1: cifras TABULARES a 31 px. "
     "38 resulto excesivo en un panel de 320 filas."),
]

if __name__ == "__main__":
    ns["OUT"] = OUT
    print("Fuentes propias del puerto, en %s:" % OUT)
    total = 0
    for args in FUENTES:
        total += ns["render_font"](*args)
    print("  %-14s %6d B  (%.1f KB)" % ("TOTAL", total, total / 1024))
    print()
    print("Son cabeceras con los datos DENTRO (static const), a proposito:")
    print("solo las incluye ui_top.c, asi que no hay copias que separar.")
