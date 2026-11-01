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

Every "the reset value is fine" is really "whatever the previous
firmware left is fine". **Write the registers you depend on.**

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

## State, 01/11/2026

Done and verified on hardware: clock tree at 480 MHz from the 22 MHz
bypass oscillator, switched rail, console, crash reporting, the ILI9488
panel at 18 bits per pixel, and an FT6236 touch controller calibrated
against four targets.

Next: the NAU8822 codec and SAI1, and the question the whole port turns
on — whether it will hold 192 kHz, or whether everything downstream has
to live with 160 and a resampler. `PORTING.md` §3.2 has the detail.
