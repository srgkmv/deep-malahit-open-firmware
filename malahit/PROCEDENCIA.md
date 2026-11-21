# Third-party code under `malahit/`

The repository is **AGPL-3.0**, which makes it necessary to know exactly
what inside it is not ours and whether it fits. `../PROCEDENCIA.md` is
upstream's inventory for the DEEPSDR 101 tree; this file is the same thing
for what the Malahit port adds, kept separate so neither has to be read to
understand the other.

Short answer: everything added so far is Apache-2.0, which is one-way
compatible with GPLv3 and therefore with AGPL-3.0. Apache-2.0 code may be
combined into an AGPL-3.0 work; the combined work is AGPL-3.0, and the
Apache notices have to travel with it. They do — see below.

---

## Upstream `User/` and `CMSIS/DSP/`, compiled in place

As of 02/11/2026 the port compiles files out of the DEEPSDR 101 tree
directly — no copy, no fork, objects prefixed `up_` and `dsp_`:

| File | Lines |
|---|---|
| `User/fft.c` | 460 |
| `User/iqbal.c` | 1694 |
| `User/iqancho.c` | 232 |
| `User/agc.c` | 72 |
| `User/audiofil.c` | 151 |
| `User/gfx2.c` | 969 |
| `User/gfx.c` | 385 |
| `User/spectrum.c` | 2078 |
| `User/encoder.c` | 332 |
| `User/spec_rotulos.c` | 846 |
| `User/idioma.c` | 60 |
| `User/fonts_data.c` | glyph tables |
| `User/demod_am.c` | 6992 |
| `User/ui_cfg.c`, `User/ui_grid.c`, `User/ui_det.c` | step 9, 21/11/2026 |
| `User/semibanda.c`, `nco.c`, `nco_tabla.c`, `nb.c`, `anotch.c`, `nr_ss.c`, `sam.c`, `rapido.c`, `bip.c` | step 8, 21/11/2026 |
| `CMSIS/DSP/Source/**` | 9 files |

Those are part of the same AGPL-3.0 work and raise no new licence
question. ARM's CMSIS-DSP sources carry Apache-2.0 headers, same as the
CMSIS-Core headers below.

**Several upstream files have been modified** — it said "one" long after
there were eight, which is the sort of count that goes stale silently —
and each is recorded here so the divergence is never a surprise:

- `User/gfx2.h` — `GFX2_W` and `GFX2_H` wrapped in `#ifndef`, so the
  screen size can be set from the build. Generic, follows the pattern
  `GFX2_BAND_H` already used, and verified not to change the DEEPSDR
  build or the `deepsdr-ui` host build. It belongs upstream.
- `User/gfx.h` — `GFX_SCREEN_WIDTH` and `GFX_SCREEN_HEIGHT` likewise.
- `User/spec_chrome.h`, `User/ui_top.h`, `User/ui_act.h` — the layout
  constants wrapped in `#ifndef` the same way.
- `User/ui_top.c`, `User/ui_act.c`, `User/spec_chrome.c` — about
  thirty-five numeric offsets inside the drawing functions replaced by
  named constants derived from the band heights, each written so that
  the old value falls out at the old band height.
- `User/ui_top.h`, `User/ui_top.c` — **three right-anchored coordinates
  that still assumed an 800 px panel**, found when the screen was first
  put on the radio rather than only rendered:
  `UI_TOP_HORA_RW` was `800 - UI_TOP_HORA_RX`, so the clock corner's
  clipped repaint asked for a window reaching x = 799 on a 480 px
  panel; the clock's touch box was the literal `640 + 150` although the
  two constants it is made of had already been parameterised; and the
  battery pill was placed at `790 - w - 2`, which on this panel put it
  at x = 740 where `gfx2` clipped it away entirely — present in the
  code, absent from the screen, and absent from every render of this
  layout made before now. All three now keep their distance from
  `GFX2_W`, and the battery's four numbers became `HDR_BAT_*`. At 800
  px wide every one of them produces the number it produced before.

- `User/encoder.c` — the three pin pairs (`ENC_A_PORT`/`PIN`,
  `ENC_B_*`, `ENC_BTN_*`) wrapped in `#ifndef`, so a second board can
  say where its encoder is wired; plus `ENC_BTN_PRESSED`, which says
  **which level is a press**, because that is a property of the board
  and not of the decoder — the DEEPSDR's switch goes to Vcc and this
  one's goes to ground. The two levels used to be spelt out inline, in a
  comparison against `SET` and a hard-coded pull-down; they are now one
  `btn_read()` that returns "pressed" and a `ENC_BTN_PUPD` that asks for
  the pull which holds the line at its IDLE level. Everything else in
  the file — the quadrature table, the debounce integrator, the hold
  timer, the short/long classification — was already written in terms of
  "the debounced state is a press" rather than a voltage, which is why
  this change reaches three lines and not thirty.

- `User/encoder.c`, second edit — **`ENC_BTN_READ`, for a button that is
  not a pin at all.** Defined, it names a function the board supplies and
  that function becomes the whole of the reading: no port, no pin, no
  pull, and `ENC_BTN_PRESSED` stops meaning anything. The Malahit is why.
  Both of its encoder buttons sit on **one** analog line through a
  resistor ladder, so one of the two presses puts it at 1.95 V — on a
  3.3 V input that is neither a low nor a high but the undefined band in
  between, and no pull or polarity can rescue it. It takes an ADC and two
  thresholds.

  The guard is in `encoder_init()` as well, and that one is not
  tidiness: `ENC_BTN_PORT`/`PIN` still carry their upstream defaults, so
  an unguarded `gpio_mode_set()` would configure whatever they name —
  on this board `PC9`, the touch controller's I²C data line. An encoder
  driver writing a pull-up onto a bus is a fault nobody would look for
  there. Verified by preprocessing the file with this port's flags: two
  `gpio_mode_set()` calls survive, for bits 0 and 1 of port C, and none
  for the button.

  Everything below the reader is untouched, which is the point of doing
  it here: the debounce integrator, the hold timer, the short/long
  classification on release and the "pressed and turned" consumption all
  keep working for a board whose button is a voltage. A second copy of
  any of them would have been the alternative.

- `User/demod_am.c` — `SPK_EN_PORT`/`SPK_EN_PIN`, the speaker
  amplifier's enable that `demod_am_init()` drives high, wrapped in
  `#ifndef` (21/11/2026, step 8). Upstream's PB7 is not connected on
  this board; ours is PE1, set from `bringup/reglas.mk`. The same
  pattern as the encoder's pins, for the same reason: a driver that
  writes a pin the board does not own is a fault nobody looks for. With
  no `-D` the DEEPSDR keeps PB7 — **verified by compiling `demod_am.o`
  with upstream's own GD32 command line before and after: identical
  disassembly, identical section sizes.**

- `User/ui_grid.h`, `User/ui_grid.c`, `User/ui_cfg.h`, `User/ui_cfg.c`
  — step 9, 21/11/2026: the menu geometry wrapped in `#ifndef` (the
  zone, the cells, the gaps, the category column), and the literal
  offsets inside the drawing - where each line of text sits in its cell,
  the radii, the accent bar - turned into named `#ifndef` constants whose
  defaults are the old literals, one by one. **Verified by rendering
  nine DEEPSDR screens before and after - bands, settings, detail,
  config, the four mode pages and the main screen - and comparing the
  PPMs byte for byte: identical.** `deepsdr-ui/gfx2/` copies synced,
  `copias_check.py` clean.

- `User/ui_det.h`, `User/ui_det.c` — step 9, 21/11/2026: the detail
  page's geometry wrapped in `#ifndef`, its literals (touch margins, the
  value's hole and lines, the sign bars, radii, foot margins) turned into
  named `#ifndef` constants with the old values, and its two `800`s and
  one `400` into `GFX2_W` and `GFX2_W / 2` - the same numbers at 800.
  **Verified: `detalle` byte-identical to its reference, and `detreal`
  (with and without the preset row) byte-identical to a build of HEAD's
  ui_det.c.** `deepsdr-ui/gfx2/` copies synced.

`User/encoder.c` is also **compiled twice**, the second time with its
public names renamed through `-D` so the board's second encoder gets its
own copy of the file's statics. That is not a modification — the source
is identical and unmodified in both objects — but it is worth recording
next to them, because `encoder2_tick()` exists with no definition to
find. See `ENC2_RENAME` in `bringup/reglas.mk` and `src/enc2.h`.

All of them are generic: a constant becoming configurable, or a literal
becoming a named expression. None adds anything Malahit-specific, and
with no `-D` the DEEPSDR gets what it always got — **verified by
rendering its 800 x 480 main screen before and after and comparing the
PPM byte for byte**, after every step.

### Fonts generated for this port

`compat/font_ui_8.h`, `compat/font_ui_8b.h` and `compat/font_num_31.h`
were produced by upstream's own `deepsdr-ui/tools/gen_assets.py` from
DejaVu, the same source and the same 4 bpp format as the faces it ships.
11.6 kB in total. They are new files; no upstream font was regenerated
or modified.

Licence unchanged: DejaVu is under the Bitstream Vera / DejaVu terms,
which permit embedding and redistribution, and that is already why
upstream uses it.

Everything else upstream is byte-identical. Two mechanisms keep it that
way and both are documented where they are used:

- `-DRM68120_EXMC_H -include rm68120_ili9488.h` substitutes the panel
  header without touching `gfx2.c`, which an `-I` cannot do — see
  `compat/rm68120_ili9488.h`.
- `compat/core_cmInstr.h` and `compat/core_cm4_simd.h` forward the
  CMSIS-4 era intrinsic headers that upstream's CMSIS-DSP expects onto
  a modern CMSIS core.

---

## `malahit/CMSIS/Include/`

ARM's CMSIS-Core headers for the Cortex-M7, copied unmodified from
STM32Cube_FW_H7 V1.13.0:

`core_cm7.h`, `cmsis_gcc.h`, `cmsis_compiler.h`, `cmsis_version.h`,
`mpu_armv7.h`, `cachel1_armv7.h`

- Copyright (c) 2009-2021 Arm Limited.
- **SPDX-License-Identifier: Apache-2.0**, stated in each file's own header.
- `LICENSE.txt` in that directory is the full Apache-2.0 text, carried
  across with the files.

Not modified. If any of them ever needs a change, say so here and mark the
file, as Apache-2.0 section 4(b) requires.

## `malahit/CMSIS/Device/ST/STM32H7xx/Include/`

ST's device headers — the register map — copied unmodified from the same
package:

`stm32h7xx.h`, `stm32h743xx.h`, `system_stm32h7xx.h`

- Copyright (c) 2019 STMicroelectronics.
- `LICENSE.txt` in that directory: *"If you received this software component
  outside of a package or without applicable license terms, the terms of the
  Apache-2.0 license shall apply."* Which is this case exactly — these three
  files are here without the surrounding Cube package.

Not modified.

`system_stm32h7xx.c` is deliberately **not** included. Its `SystemInit()`
assumes a clock tree this board does not have (HSE at 22 MHz in bypass, a
PLL nobody else configures this way) and would have to be rewritten line by
line. `bringup/src/clock.c` provides its own `SystemInit()` and the one
variable, `SystemCoreClock`, that the CMSIS headers declare.

---

## What is ours

Everything under `malahit/bringup/` — the startup file, both linker
scripts, `clock.c`, `board.c`, `console.c`, `main.c`, `reglas.mk` — plus
`BOARD.md`, `PORTING.md`, `openocd.cfg` and `tools/`. Written for this
port, AGPL-3.0 like the rest of the repository.

`startup_stm32h743.S` deserves a note because startup files are usually
copied: this one is not. ST's template is written for an image that a chip
reset starts, and the bring-up image is normally started by a debugger
after being loaded into RAM, which means the core has not popped SP off the
vector table and VTOR still points at the factory firmware. Both are
handled explicitly in the first few instructions. The file's own header
explains it.
