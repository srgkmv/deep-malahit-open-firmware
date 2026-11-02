# Working on this radio

Read this before the first build of a session. It is the practical half
of what `BOARD.md` documents: not what the hardware *is*, but how to get
code onto it without losing an evening. Everything here was paid for.

---

## The loop

```bash
cd malahit/bringup
make                                    # build
ADAPTER=ft2232d ../tools/swd.sh ram build/bringup.elf
```

`ram` loads into AXI SRAM and starts the image **without touching
flash**, so the factory firmware is always still there and a power cycle
undoes everything. There is no reason to write flash yet, and one very
good reason not to — see "no way back" below.

Console: UART7 at 115200 on package pins 37 (PE7, RX) and 38 (PE8, TX).
There is no J6 header on this unit; the leads go to the pins.

```bash
stty -F /dev/ttyUSB0 115200 raw -echo -crtscts
cat /dev/ttyUSB0
```

Set `stty` **every time**. The port reverts to its defaults when the
last reader closes, and the next `cat` then reads at the wrong rate —
steady garbage at a steady rate, which looks nothing like the silence it
is often mistaken for.

Other useful commands, all taking `ADAPTER=ft2232d`:

| | |
|---|---|
| `swd.sh id` | chip ID, flash size, unique ID |
| `swd.sh fault` | the last crash record |
| `swd.sh clean` | quieten a crashed core |
| `swd.sh -c init -c halt -c "mdw <addr> 1"` | read anything |

---

## The radio has to be ON

Not in DFU, not asleep — switched on with its own button, factory
firmware running. SWD needs the core awake; it does not need the
bootloader. Loading an image over the running factory firmware is the
normal way to work here.

If `swd.sh` says `Error connecting DP: cannot read IDR`, the radio is
off or asleep. Switch it on. That message is not a dead adapter, and
chasing it as one costs time.

---

## Do not reset

`reset halt` is configured off, on purpose, in both adapter configs.

A reset returns the vector table to `0x08000000`, so control goes back
to the **factory firmware**, which — with no button held — settles into
a low-power state and takes the debug port with it. Every successful
reset therefore ends with a radio that has to be switched on by hand.
The reset works; that is the problem.

Worse, the 22 MHz oscillator is on the switched rail (`BOARD.md` §3), so
a reset also stops the system clock's own source.

`core_clean` in `openocd-helpers.cfg` does the part of a reset that is
actually wanted — SysTick off, NVIC quiet, fault status cleared — and
`ram_run` calls it.

---

## LOADING OVER SWD RESETS NOTHING

This is the single most expensive lesson of the bring-up, and it caused
five separate bugs that each looked like something else:

- **PRIMASK** was inherited from a crashed run, so SysTick never ticked
  and the next image hung in its first delay loop, before the console.
- **The clock tree** was already on PLL1, and `clock_init()` tried to
  stop HSE to reconfigure it — which the hardware refuses while HSE
  feeds the system clock, so it reported a dead oscillator on a part
  running happily at 480 MHz.
- **SPI123SEL** was left on `per_ck` by the factory firmware, so the
  panel bus ran at 64/240 of the expected speed. Every measurement
  scaled perfectly with the divider and was uniformly 3.75× wrong.
- **The factory watchdog** was still ticking, unfed, and reset the part
  mid-draw. PC6 went low with the reset and the radio switched itself
  off.
- **CSSHSEON**, once armed by our own firmware, cannot be cleared except
  by a reset, so the next image's deliberate HSE teardown looked like an
  oscillator failure.
- **The D-cache was on.** The worst of the five, and the only one where
  the inherited state corrupts the image itself rather than a
  peripheral. Its own section is below.

Every "the reset value is fine" is really "whatever the previous
firmware left is fine". **Write the registers you depend on.**

---

## The caches, and why the SECOND load used to fail

A debugger writes AXI SRAM through the AHB access port, which goes
**around** the CPU's caches and invalidates nothing. The factory
firmware leaves the D-cache enabled — `SCB_CCR` measured `0x00070200` on
arrival, bit 16 set — and `SystemInit()` only ever enabled the I-cache,
so DC stayed on. Loading an image therefore left the previous run's
lines, dirty, for the very addresses the loader had just overwritten.

The symptom is worth memorising, because nothing about it points at a
cache:

- The **first** load after a power-on always worked. The factory
  firmware's cached lines cover flash at `0x08000000`, not SRAM at
  `0x24000000`, so there was nothing stale to hit.
- The **second** load — our image over our image, same addresses — ran a
  mixture of two builds. A PLL solver reported that no solution existed
  for arithmetic that is provably correct on a host. Then an UNALIGNED
  hard fault, whose recorded PC and LR **could not both have come from
  the same binary**.

That last detail is the tell, and it generalises: when a stacked PC and
LR disagree about which program they belong to, the instruction stream
is stale, not wrong. Stop debugging the program.

Fixed in two places, because neither is sufficient alone:

- `core_clean` in `openocd-helpers.cfg` clears IC and DC in `SCB_CCR`
  and invalidates the I-cache **before** `load_image` writes a byte.
  Only the loader can do this: by the time the firmware's reset handler
  runs, the reset handler is the thing that might have come from a stale
  line.
- `Reset_Handler` in `startup_stm32h743.S` clears both caches and
  invalidates the I-cache as its first act after setting the stack, so
  the firmware does not depend on which loader started it.

Both **invalidate and never clean**. The usual sequence for disabling a
data cache cleans it first so dirty lines reach memory; here those dirty
lines hold the *old* image and memory holds the *new* one, so cleaning
would write the old program back over the new one. Discarding them is
the entire point, and it is only safe before the `.data` copy.

The D-cache's own stale lines are left sitting in the array rather than
walked by a set/way loop: with DC clear they are never looked up and
never evicted. Anything that later turns the D-cache on must invalidate
first — which CMSIS's `SCB_EnableDCache()` does.

---

## When something looks impossible

In rough order of how often it has been the answer:

1. **Is the radio on?** See above.
2. **Did `stty` run?** See above.
3. **Read the phase marker.** `main.c` keeps `g_phase` at a fixed
   address, bumped at each startup stage, and inside the I2C scan it
   carries the address being probed. Find it with
   `arm-none-eabi-nm build/bringup.elf | grep g_phase`. The console has
   produced zero bytes while the firmware was demonstrably executing;
   when the evidence and the machine disagree, the machine wins.
4. **Read the crash record.** `swd.sh fault`, or the next boot prints it
   with CFSR decoded in words.
5. **Check the vector table** if anything was touched in
   `startup_stm32h743.S`:
   `arm-none-eabi-objdump -s -j .isr_vector build/bringup.elf` — every
   word after the first must be **odd**. A label in a `.thumb` section
   is still a data label; without `.thumb_func` the assembler stores an
   even address and every fault locks the core.
6. **Read the register back.** Hardware clears bits behind your back:
   a mode fault cleared `CFG2.MASTER` on SPI3, and the symptom was a
   transfer that started, accepted its byte and never completed.
7. **Is this the second load?** If the first load after a power cycle
   works and later ones do not, it is the caches — see above. Try a
   power cycle before believing anything a repeat load tells you.
8. **Is the format string lying to you?** `console_printf()` is a few
   dozen lines in `console.c` and supports a small set of conversions.
   An unsupported one — `%+3d` was the one that bit — falls through to
   a default case that prints the characters and **does not consume the
   argument**, so every later argument on that line shifts by one. The
   visible result was a neat column of decibel figures that were
   actually a different variable, ending in a `%s` reading a pointer
   out of an integer and printing heap bytes. `%+d` now works; the
   lesson is that a format string is code.

   Same family: `%u` takes a 32-bit argument, so printing a `uint64_t`
   with it shows the low word. That produced a magnitude and a dBFS
   figure for the same quantity that disagreed by 29 dB, on adjacent
   lines.
10. **Is the measurement looking where the signal is?** A single-bin DFT
   is exact arithmetic and a poor instrument for an absolute level:
   the generator and this radio do not share a reference, so a carrier
   nominally 3 kHz off lands some tens of hertz away from that, and a
   23 Hz bin then reads window skirt instead of carrier. It under-read
   by **16 dB**. Ratios between bins survive this; absolute levels do
   not. Use the FFT peak for anything absolute.
11. **Was the constant fitted or remembered?** A log2 polynomial written
   from memory was exact at every power of two - so log2(1), log2(8)
   and log2(0.5) all checked out - and 1.5 bits wrong in between, which
   made every spectrum level uniformly 4.5 dB low. A uniform error is
   the kind that gets absorbed into a calibration constant and never
   found. The replacement is a least-squares fit with a measured
   maximum error.
9. **Did the measurement actually happen?** Two register settings that
   produce *byte-identical* results have not demonstrated that the
   register does nothing; they have demonstrated that one of the two
   measurements never ran. `sai1_stream()` returns a frame count and
   `main.c` ignored it, so a failed capture was silently re-averaged
   from the previous buffer and printed as a new result. Identical
   numbers out of an analogue measurement should be unbelievable on
   sight. **Check the return value of anything that fills a buffer.**

---

## No way back, yet

**There is no JP1 on this board.** The schematic shows a BOOT0 jumper;
it is not fitted, and pin 94's trace disappears into an inner layer with
nothing near it. DFU is reached by holding the encoder buttons and
pressing power — which is a feature of the **factory firmware**, the
very thing a flash write would replace.

So before anything is written to flash there has to be another route
back: SWD that our firmware never disables, or our own button
combination jumping to `0x1FF00000`. Cheap to write, and it belongs
before the first flash write rather than after it.

Until then: RAM only, and the radio cannot be bricked.

---

## Adapters

`ADAPTER=ft2232d` selects a generic FT2232D dongle, `stlink` (the
default) an ST-LINK/V2.

The FT2232D is the better one here: it demonstrably drives NRST, which
the ST-LINK could never be shown to do. Wiring and the two config traps
are in `openocd-ft2232d.cfg`. Neither adapter is currently used for
resets, for the reasons above, so the choice barely matters day to day.

OpenOCD must use `dapdirect_swd`, never `hla_swd`, with either cable.
That also rules out `st-info` and `st-flash`, which only speak HLA.

---

## If the radio switches itself off

Two different things look the same from the bench, and both end with
`Error connecting DP: cannot read IDR`, because PC6 going low takes the
22 MHz oscillator's supply with it and the debug port dies too.

- **A reset.** Any reset returns PC6 to an input, R38 pulls `3.3V_ON`
  low, and the radio is off. The usual cause is the factory watchdog,
  which cannot be stopped — only stretched to about 32 seconds by
  `board_watchdog_tame()` — so a firmware that hangs for half a minute
  switches the radio off rather than sitting there looking broken.
- **A hang before the console.** `board_delay_ms()` used to spin forever
  if SysTick was not ticking, and its first call is inside
  `board_power_on()`, so the failure produced not one byte of
  explanation before the watchdog finished the job. It now feeds the
  watchdog from inside the loop and has a cycle-count backstop: a dead
  SysTick costs a delay of the wrong length instead of a dark radio.

`main()` also brings the console up **first**, before the rail and
before SysTick, which it can because UART7 is on MCU pins on the
always-on rail and `SystemInit()` has already raised PC6 and locked the
PLL. The first line of output now precedes everything that can hang.

---

## State, 02/11/2026 — read this first

Steps 0 to 6 of `PORTING.md` §4 are done and verified on the radio, and
step 7 — the graft of upstream code — is well under way. Everything
below is committed; the working tree is clean.

### What runs on the radio

Clock tree at 480 MHz from the 22 MHz bypass oscillator, switched rail,
UART7 console, crash reporting, ILI9488 at 18 bpp, FT6236 touch,
NAU8822 codec with SAI1 as I²S master, MSi001 tuner, and a spectrum
drawn by **upstream's own `spectrum.c`**.

With a generator at 10.000 MHz on the bench: tuning error 0 Hz, the
peak lands in the bin the arithmetic predicts, image rejection 30 dB
with `iqbal` off and 34 dB on by its own A/B test.

**192 kHz works** — the question the whole port turned on. Clean from
8 kHz to 320 kHz, breaking up only at 384, against a datasheet ceiling
of 48. So the upstream DSP tree keeps its rates and needs no resampler.
`PORTING.md` §3.2 has the table.

### What is grafted

Eleven files out of `../User` and nine CMSIS-DSP sources, **compiled in
place — no copy, no fork**: `fft.c`, `iqbal.c`, `iqancho.c`, `agc.c`,
`audiofil.c`, `gfx2.c`, `gfx.c`, `spectrum.c`, `spec_rotulos.c`,
`idioma.c`, `fonts_data.c`. Upstream code in the image outweighs ours.

Four upstream files carry deliberate, generic edits — constants made
configurable and literals made named — all recorded in
`PROCEDENCIA.md`: `gfx2.h`, `gfx.h`, `spec_chrome.*`, `ui_top.*`,
`ui_act.*`.

### THE NEXT THING TO DO

**Put the 480 × 320 UI on the panel.** The layout is finished and
verified, but *only on the PC*: `make malahit` in `deepsdr-ui/` renders
the whole main screen and it is right, while the firmware still draws
the spectrum directly and never calls `ui_top.c` or `ui_act.c`.

So: add `../../User/ui_top.c` and `../../User/ui_act.c` to
`UPSTREAM_SOURCES` in `bringup/reglas.mk`, force-include
`malahit_layout.h` and the font remap the way `deepsdr-ui/Makefile`
already does, and call `ui_top_draw()` / `ui_act_draw()` from `main.c`.

Two things that will bite:

- **`ui_top.c` needs its own font remap**, different from everything
  else: `font_ui_14`/`font_ui_14b` → `font_ui_8`/`font_ui_8b` so six
  status chips fit a 480 px row, while the ruler and the buttons stay
  at 11. The remap is per translation unit, so `ui_top.c` must be
  compiled by a rule of its own — `deepsdr-ui/Makefile` has the
  pattern, including why `fonts_data.c` must *not* get the remap.
- **`main.c` will have to supply more seams.** `g_pal` was one;
  `ui_top_state_t` and `ui_act_state_t` have to be filled from real
  receiver state, and upstream fills them in its own 39 094-line
  `main.c`, which is not coming.

After that: the other screens — menus, band list, keypad, FT8 map —
live in `ui_digi.c` (1817 lines) and `ui_kbd.c`, already separate
modules, and the same host harness renders them.

### Still open

- **The mixer DC offset is the worst number on the board**: −7.7 dBFS
  with the preamp on, which is 15 dB *above* the wanted carrier and
  8 dB from clipping the ADC. Most of the converter's range is spent
  on the receiver's own error. `BOARD.md` §6 has the `DCCAL` options;
  this is the first thing to attack before AM audio.
- **`demod_am.c` is step 8, not step 7.** The file is portable, but its
  transitive closure is 106 `.c` files and 72 126 lines, and upstream
  has no switch to leave modes out. It wants the display layer in place
  first — see the box under step 7 in `PORTING.md`.
- **SAI runs polled, not DMA.** The transmit FIFO is 83 µs deep, so any
  blocking console write is an audible gap — one click per printed
  line, measured. That, and not CPU load, is what makes DMA necessary.

### The bench, as it stands

- Generator at **10.000 MHz**, ≤ −40 dBm, on the antenna input.
- The generator is an **OSA103, which is also an FTDI device**:
  `0403:6001`, serial `AH7J76J7`, against the programmer's `0403:6010`
  with no serial. `openocd-ft2232d.cfg` pins the product ID so the two
  cannot be confused. The OSA103 creates no `ttyUSB`.
- Console on `/dev/ttyUSB0` (a Prolific adapter). The capture that
  works is a plain `cat`, started *before* the load:

      stty -F /dev/ttyUSB0 115200 raw -echo -crtscts
      ( timeout 50 cat /dev/ttyUSB0 > log & )
      sleep 1
      ADAPTER=ft2232d ../tools/swd.sh ram build/bringup.elf

  Holding the port open on a spare fd and reading from that instead
  produced nothing at all; do not be clever here.

### Regression checks to run before committing anything in `User/`

    cd deepsdr-ui && python3 tools/copias_check.py ..   # copies in step
    make after                                          # host bank builds
    cd ../malahit/bringup && make                       # firmware builds

and, for any change to the drawing code, prove it changes nothing
upstream by rendering the DEEPSDR's own 800 × 480 screen before and
after and comparing the PPM **byte for byte**:

    cd deepsdr-ui
    make ref800-guarda        # before touching anything
    ...                       # the change
    make ref800-comprueba     # after

It never reported a difference across the whole UI refactor, which is
the point: it is what makes "this changes nothing upstream" a fact
rather than a hope.
