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
