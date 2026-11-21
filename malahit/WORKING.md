# Working on this radio

Read this before the first build of a session. It is the practical half
of what `BOARD.md` documents: not what the hardware *is*, but how to get
code onto it without losing an evening. Everything here was paid for.

---

## The loop

```bash
cd malahit/bringup
make                                    # build; SELFTEST=1 adds the tests
ADAPTER=ft2232d ../tools/swd.sh ram build/bringup.elf
```

`ram` loads into AXI SRAM and starts the image **without touching
flash**, so a power cycle undoes everything. Since 20/11/2026 what is in
flash is OUR firmware, not the factory's, and what a RAM load inherits
is its state - see "LOADING OVER SWD RESETS NOTHING", which got two
more entries the day that became true. Updating flash is DFU, not SWD:
"The way back" below.

`make SELFTEST=1` (or `make flash SELFTEST=1`) builds the image with
the 37 s of bring-up tests in the boot; the default boots straight to
the radio. The value is kept in `build*/flags.txt`, so changing it
rebuilds everything and repeating it rebuilds nothing.

**A one-off test image** - `ADDON_COLD_TEST`, `TOUCH_WAKE_TEST`,
`FRAM_TEST`, `MSI_ROUND_TEST`, each 0 by default in `main.c` - goes into
a build directory of its own, so it can never end up in `build/` or
`build_flash/` by accident:

    make BUILD_DIR=/some/scratch/dir OPT="-O2 -g3 -DFRAM_TEST=1"
    ADAPTER=ft2232d ../tools/swd.sh ram /some/scratch/dir/bringup.elf

`flags.txt` does not track these, which is the other reason for the
separate directory. A buffer a test leaves in RAM can be read over SWD
while the radio runs: `swd.sh -c init -c "dump_image f.bin <addr> <n>"`
with the address from `arm-none-eabi-nm` on the same ELF.

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
| `swd-call.sh <elf> <fn> [args]` | call one firmware function on the RUNNING radio, from the display loop, and let it carry on |
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

Three more, 20/11/2026, all new since the image before every load is our
own one with its block interrupt running:

- **The old image's DMA was still writing.** Halting a core halts no
  DMA: SAI1 kept streaming samples into the old receive ring through
  the load and into the new image's first milliseconds - after its
  `.bss` was zeroed. Shown as I/Q pairs in `sai1.c`'s `s_mux_released`
  (a 16-bit mask that read `0x2c8df875`). `core_clean` now resets SAI1
  and every DMA controller through RCC before the load.
- **The core was halted INSIDE the block interrupt**, and the new image
  then ran entirely as that exception. The AM demodulator keeps the
  core in exception 27 about 40 % of the time; `ram_run` writes PC, SP
  and xPSR, but neither the IPSR nor the NVIC's active bit changes, and
  no register write can clear an active bit. Everything at priority 0
  - SysTick, so knobs, buttons, screen - kept working; the block
  interrupt never arrived again: **`irq 0/0`, no audio**, and inherited
  by every load after it until a power cycle. The startup code now
  returns out of any exception it finds itself in (step 0 in
  `startup_stm32h743.S`), `core_clean` switches the interrupts off and
  waits 20 ms before it halts, and `main()` prints a warning if an
  active bit survives anyway. **If a RAM run reports `irq 0/0`, suspect
  this first.**

- **A SysTick left pending by a crashed image ran the NEW image's
  handler before its first instruction** - and switched the radio off.
  `fault_park()` runs with interrupts masked, so SysTick pends and stays
  pending; stopping SysTick does not un-pend it, and `ICPR` covers only
  external interrupts. On `resume` (and on `step`: openocd's "auto"
  masking lets pending interrupts run first) the core took it through
  the new vector table, already loaded, with the new image's `.data`
  and `.bss` still holding the old image's bytes. Its
  `boot_power_tick()` read console text where its statics now live -
  "armed", "held" for ever - and called `boot_standby()`. Every load
  over a parked crash died before printing a byte; read on the radio,
  parked: `ICSR = 0x0440F000`, SysTick pending. `core_clean` now clears
  `PENDSTCLR`/`PENDSVCLR`, and the same load comes up and reports the
  crash. **Halting is not a clean slate either: pending exceptions
  survive it, and run first.**

  The first two were found by the same set of loads, and the second was
  hiding behind the first: the fix for the DMA was tested on runs that had no
  audio, and nobody - me - looked at the `irq` column until a "final"
  check. Read every column of the first status line after a load, not
  just the one the change was about.

Every "the reset value is fine" is really "whatever the previous
firmware left is fine". **Write the registers you depend on.**

### And the mirror image of that, which cost more

The rule above is about registers left unwritten. The DC offset was the
opposite failure and it lasted longer: a register **written**, from a
misread datasheet line, over a reset value that was correct. The MSi001
powers up with its DC trim tracking continuously; `msi001.c` wrote
"static" into it, believing that meant "calibrate once and hold", and
static means *no tracking at all*. The result railed the ADC and was
filed under "the open problem of the RF chain" for weeks.

So the rule has a second half. **Write the registers you depend on —
and know what the reset value was before you overwrite one.** A
peripheral's defaults are chosen by people who know the part; a value
written from a misunderstanding is worse than one not written at all,
because it looks deliberate and nobody questions it again.

And it is not only registers. **A bus has state too**, and the one
outside the chip cannot be written at all — see the next section.

---

## A failed transfer must leave the bus usable

Three days of the touch panel working about half the time, and all
three causes were the same mistake at different levels: **something
was left half-finished and nobody ended it.**

- **The I²C error paths ended the transfer for the master only.**
  Every one of them cleared the sticky flags and returned, with no
  STOP. `i2c3_write_read()` runs its write phase with `AUTOEND` off
  on purpose — the bus has to be held for the repeated start — so a
  failed read left the peripheral mid-transfer.
- **A STOP was still not enough.** A slave caught mid-byte drives SDA
  between clocks and lets go only when it has been clocked to the end
  of that byte. So the abort path now issues the stop and, if SDA is
  still low, clocks the bus free — nine pulses, one per bit plus the
  acknowledge — and stops again.
- **And the peripheral has to be reset before the pins are taken
  back.** `PE = 0` must be held for at least three APB cycles or the
  software reset does not take, and two back-to-back posted writes
  from a 480 MHz core to a 120 MHz bus are not three cycles of
  anything. Read `PE` back until it really is clear.

The symptom is worth memorising because it points the wrong way:
`i2c3_init()` reports a clean bus needing **zero** recovery clocks, and
the diagnostic printed a moment later says `SDA=0` with a scan that
finds nothing. Both measurements are true. They describe different
instants, and the step between them — handing the pins to a peripheral
that was never reset — was the one nobody was looking at.

What made it reproducible was sampling the lines at every step of
`init` and inside the identify loop, which turned "about half the
time" into "reads one to four succeed, read five fails 45 ms into the
part's wake-up, and SDA is low for the other 115". **The fix was
obvious once the question was `which read`, and invisible while the
question was `why sometimes`.**

Note which change exposed it. Making `touch_init()` poll for the
controller's ID instead of waiting a fixed 120 ms was itself a real
fix — the FT6236 ACKs its address at once and answers `0x00` to CIPHER
and VENDOR for ~200 ms more — and it turned one risky read into sixty,
where the first failure poisoned the fifty-nine after it. A latent bug
that costs nothing until something legitimate retries.

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

   **And read it back AGAIN, later.** `SAIEN` on the SAI's receive block
   reads back as set, every time, with a retry counter that never moves
   — and is clear a hundred microseconds later, on three arms in four,
   with every error flag in the block at zero. An enable that takes and
   then quietly un-takes is a previous *disable* that had not finished;
   `sai1_stop()` waits for the bit to read 0 and that is not the same as
   the peripheral having finished with it. The fix is to verify after a
   delay and redo the whole start — and the retry, not the delay, is what
   works: 400 µs of settling on its own still failed. `blocks_enable()`'s
   comment in `sai1.c` has the measurements.

   The symptom is the thing to recognise, because nothing in it points
   at an enable bit: a receiver delivering **digital silence**, with
   `rc 0`, no error flag, a DMA stream that says it is enabled, and the
   best frame rate the radio had ever reported — because a screen with
   nothing on it is quick to draw.

   **It was never a DMA problem**, and that was the second half of the
   lesson. The same hazard had been in the polled `sai1_start()` since
   step 4 and nobody had looked: on the first run after a cold power
   cycle, three polled captures in one run came back with **zero
   frames** — one of them in test 0, before this firmware had armed a
   DMA stream at all. The pattern that triggers it is "stop, then start
   again immediately", which is what `sai1_set_rx_timing()` and
   `sai1_stream()`'s re-alignment both do. Both paths verify now, and
   the relatch counter runs into the tens over a full run.
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

   **And it happened again, 05/11/2026, with `%-10s`** — a
   left-justified table column, which is the most natural thing to write
   in a format string and was not supported either. Same mechanism, same
   damage: the literal text `%-10s` in the column, then every later
   argument on the line read one position out, so the button
   measurement's table reported 604 073 540 samples and a voltage where
   a label belonged. It looked like a fault in the thing being measured.
   `%-` now works. The point is not that `+` and `-` are now handled;
   it is that **this failure mode is silent and produces plausible
   numbers**, so a new conversion in a format string is a change to be
   checked against `console.c` before it is believed.
9. **Is the measurement looking where the signal is?** A single-bin DFT
   is exact arithmetic and a poor instrument for an absolute level:
   the generator and this radio do not share a reference, so a carrier
   nominally 3 kHz off lands some tens of hertz away from that, and a
   23 Hz bin then reads window skirt instead of carrier. It under-read
   by **16 dB**. Ratios between bins survive this; absolute levels do
   not. Use the FFT peak for anything absolute.
10. **Was the constant fitted or remembered?** A log2 polynomial written
   from memory was exact at every power of two - so log2(1), log2(8)
   and log2(0.5) all checked out - and 1.5 bits wrong in between, which
   made every spectrum level uniformly 4.5 dB low. A uniform error is
   the kind that gets absorbed into a calibration constant and never
   found. The replacement is a least-squares fit with a measured
   maximum error.
11. **Is the number in a unit the datasheet also uses?** The DC offset
   was reported as "−7.7 dBFS" for weeks and treated as a hardware
   limit. dBFS cannot be compared with anything Mirics publishes. The
   same measurement in millivolts — one ADC count is 43.16 µV, which is
   one line of the codec datasheet — reads **1122 mV against a
   specified 50 mV**, and a figure twenty-two times the spec is not a
   limitation, it is a bug. Nothing was measured differently; only the
   units changed, and the units were what made it arguable.

   The same test grew a second column for the same reason: **how much
   of the converter is in use.** "−7.7 dBFS" and "99 % of the ADC" are
   the same fact, and only the second one is alarming on sight.
12. **Take it four times before believing it once.** The DC offset read
   +2.4, +0.3, −17.6 and −22.6 dBFS on four runs of identical code — a
   25 dB spread, from pinned to acceptable. One run of that is a
   plausible number that gets written down and believed; four runs is a
   diagnosis, because a quantity with no settled value is being
   produced by something with no settled state, and that is never the
   hardware. **A figure that will not repeat is not a loose measurement
   of something, it is a measurement of nothing.**
13. **Was the constant measured, or measured ONCE?** The display ran at
   a hard-coded 12 dB of gain reduction, which was measured — against
   one generator, at one level, on one afternoon. The generator was
   later unplugged, reconnected by cable at a different level, and the
   same 12 dB put the converter at 100 % for a whole session, drawing a
   spectrum made of intermodulation products. **A constant that was
   right when it was written is the most expensive kind, because
   nobody suspects it: it was measured.** The fix is not a better
   number, it is `rf_autorange()` — ask the hardware, every boot, and
   be right at every level instead of at one.

   And the symptom was "the trace looks like porridge", which is how a
   clipped capture presents when nothing in the firmware is checking.
   `capture()` now reports ADC usage and a clip count, in the per-frame
   line, because a clipped capture is not a loud capture — it is a
   different signal, and no amount of staring at a spectrum
   distinguishes its products from reality.
14. **Is the impairment in the signal, or in the instrument?** The AM
   envelope test read 62.5 % depth with a 3rd harmonic at −26.6 dB from
   a generator set to 70 %, which is exactly what a distorting
   generator looks like. It was the measurement: `|I + jQ|` is not
   linear, so the mixer's standing DC offset beats against the
   rotating carrier and manufactures harmonics of the modulation. At
   the 26 % offset this board has, a *mathematically perfect* 70 %
   signal reads 66 % with a 2nd harmonic at −37 dB.

   What caught it was having a host model of the instrument
   (`tools/envdft.py`) that could be fed the impairment and asked
   whether it reproduced the reading. It did. **Before believing a
   measurement accuses the thing being measured, feed the known
   impairments to a model of the measurement and see how much of the
   accusation it can produce on its own.**
15. **Does the row that wins also win on the other column?** The
   speed-up bit gave the lowest offset in the table and six times the
   AC noise, which is the datasheet's own warning about it appearing as
   a number. A table with one column cannot show that. Measure the
   thing you are optimising *and* the thing it is likely to cost.
16. **Is somebody else wired to the same hardware?** Not an inherited
   register and not an inherited board — an inherited **bus master**.
   The factory firmware's own SAI transport was still running: DMAMUX1
   channels 9 and 13, which are DMA2 streams 1 and 5, enabled, pointed
   at its buffers in D2 SRAM, reading and writing the same two SAI
   registers ours do. Halting the core does not stop a DMA controller.

   It cost nothing for as long as the SAI was polled, because `DMAEN`
   was clear in both blocks and a peripheral that makes no requests
   feeds no stream. The moment our firmware set `DMAEN`, two channels
   were listening to each request line, the flag is a level, and a
   transfer by either one clears it — so **the two streams took
   alternate words and our receiver got exactly half the stream**, with
   no overrun and no error anywhere.

   Half a stereo stream read as pairs is consecutive samples of ONE
   channel pretending to be an I/Q pair, which is a real signal at half
   the sample rate: **the spectrum came out at twice the right offset
   with an image almost as strong as the carrier.** Three measurements
   settled it and none of them was the spectrum — the item rate out of
   `NDTR` (191 994 a second against the 384 000 that two slots a frame
   require), the two slot means (identical, on a board whose I and Q
   offsets differ fivefold), and the same block through the polled path
   for comparison. `sai1_dma_start()` now takes the request lines back
   and says which channels it had to take them from.

   `sai1.c`'s `dma_release_sai_requests()` is the scan. The same was
   owed to SPI3_TX, request 62, the day the panel moved to DMA — the
   factory firmware has that one configured too, on DMA1 stream 2 — and
   was paid on that day, 21/11/2026: `spi3.c`'s `dma_release_requests()`.
17. **Did the test that proved it take its own setup away with it?**
   The speaker was silent for three days and the cause was a retired
   test. Step 4 proved a 1 kHz tone comes out of the speaker; that test
   unmuted the codec before playing, because `nau8822_configure()`
   deliberately ends with `nau8822_mute(1)` — drivers enabled, outputs
   quiet, so a bring-up image cannot come up shouting into a small
   speaker. When the sample-rate question was answered the tone test was
   retired to `(void)tone_fill;` — **and the unmute left with it.**
   Nothing in the image has unmuted since 02/11/2026.

   Three things made it survive: nobody listens to a receiver whose
   audio nothing produces yet, every *measurement* kept working because
   measurements are made on the receive side, and the first two attempts
   to check it by ear were reported as "no clicks" — which is what a
   working fix sounds like and also what a muted codec sounds like.

   The lesson is about what a `(void)` cast does not say. Retiring a
   test keeps the code compiling and keeps the FILE honest; what it
   silently discards is everything the test's own setup was doing for
   the rest of the program. A test that configures hardware is not only
   a measurement, and the part that is not a measurement has no other
   owner.

   Worth keeping the shape of the contradiction too, because both sides
   were right: **48 transmit underruns in the very same run prove the
   digital path is alive** — a transmitter only starves if it is
   clocking frames — while the ear heard nothing at all. Counters and
   ears were reporting different halves of the chain, and the mute bit
   sat between them.
18. **An absence is only evidence if somebody was standing there.**
   This was got wrong three times in two days and twice it was written
   down as a conclusion, so it has earned its own line.

   - The audio A/B reported "no clicks" twice. The codec had been muted
     since step 4, and the window was 0.67 s, thirty-four seconds into a
     run nobody was watching.
   - `RENC1` showed no movement across three runs and sixty seconds of
     supposed turning. It was written into `reglas.mk` as a hardware
     fault — PC0 open, PC1 shorted. The fourth run, same configuration,
     counted **151 changes in twenty seconds**. Both encoders work.

   The firmware cannot tell whether a hand was on the knob while it was
   recording, so a window that expires is not a result. Two things fix
   it and they are cheap: **the test waits for the input instead of for
   the clock** — `encoder_take_delta() != 0` starts the timer, not
   `board_millis()` — and it arrives **early**, a second or two after
   the load rather than after a minute of RF tests. Everything in this
   list that needs a person now does both.

   And when a run does come back empty, the honest report is "nothing
   was recorded", not "nothing moved".
19. **Did the measurement actually happen?** Two register settings that
   produce *byte-identical* results have not demonstrated that the
   register does nothing; they have demonstrated that one of the two
   measurements never ran. `sai1_stream()` returns a frame count and
   `main.c` ignored it, so a failed capture was silently re-averaged
   from the previous buffer and printed as a new result. Identical
   numbers out of an analogue measurement should be unbelievable on
   sight. **Check the return value of anything that fills a buffer.**

20. **Did the build actually rebuild?** The encoder was moved from one
   knob to the other by editing two `-D` flags in `reglas.mk`, and
   `make` answered **"nothing to be done"**. `-MMD` cannot catch this:
   the compiler records the headers it opened, and a flag that changes
   what a file compiles to was never a header. The load would then have
   put the OLD image on the radio, and the bench test — a hand on a
   named knob, the one measurement that cannot be repeated cheaply —
   would have measured the PREVIOUS decision while reading as a clean
   result for the new one. Worse than a build error by a wide margin,
   because nothing looks wrong.

   Fixed where it belongs rather than by remembering to `make clean`:
   `reglas.mk` and `GNUmakefile` are now prerequisites of every object
   rule. Editing the file that holds the flags rebuilds everything,
   which is the right trade. **A file that decides how code compiles
   has to be a dependency of the code.**

21. **Did the measurement change what it was measuring?** The ADC's
   channel scan reads all twenty inputs, and to do that it preselected
   all twenty in `PCSEL` — because `PCSEL` was being OR-ed into rather
   than set. The battery channel came back at 912 mV that way and at
   1530 mV with two channels selected, a 40 % difference in a number
   printed in a neat column with units.

   **It was worse than a wrong number: it was a wrong number that held
   still.** The channel being read was an unconnected pad, and the
   saturated `ENC_BUT` channel preselected beside it pulled the reading
   to a steady, plausible 1.53 V — which became "the battery is at
   3.05 V", survived three runs, and reached two documents. With one
   channel preselected per conversion that pad wanders between 0.05 and
   0.1 V, which is what nothing-connected actually looks like. **A
   reading does not become a measurement by being repeatable.**

   What caught it was an outside number: a multimeter on the cell said
   4.15 V and the arithmetic could not be made to agree — not even by
   doubting the reference, since that would have taken a `VREF+` above
   the supply rail. **An external check is worth more than another run
   of the same code**, and it is the only thing that can catch an error
   this shape.

   One conversion now preselects one channel. The general form: **a
   diagnostic that configures the hardware differently from normal
   operation measures the diagnostic** — the same reason the transport
   A/B runs polled and DMA frames inside one loop instead of comparing
   two runs.

22. **Is the test asking somebody who can hear it?** Six tests in this
   image need a person — turn this, press that, listen — and every one
   of them asked over the UART. The person holding the radio cannot see
   the UART. They followed instructions relayed from the other end of a
   debugger for three sessions, with the panel in their hands dark,
   before asking why the screen said nothing.

   **A bench test that needs a human is a user interface**, and it was
   on the wrong device. The panel's bring-up moved from step 6 to before
   the first such test — three self-contained calls — and `ui_prompt()`
   now puts the question on the screen. The console still gets
   everything, because that is where the numbers are read afterwards.

   The second half of the same lesson: a test that waits for a hand must
   not be in the way when the answer is already known. Three of them
   were costing a minute and a half of somebody's attention on every
   load, long after they had answered, and the capture window was ending
   before the radio reached its own main screen. Retired, still
   building, with what each one settled written where they are stood
   aside.

23. **Did you measure through somebody else's configuration?** The
   buttons worked for a day and then reported themselves held down for
   ever, with nothing in this firmware changed. The ladder's line read
   18 900 counts instead of 65 500, the classifier said "both pressed",
   the debounce agreed, and no press ever arrived - because a press is
   reported on RELEASE and the release never came. Every stage was
   correct about a pin connected to nothing.

   `PA1` has a second, analog-only pad with its own ADC channel and an
   analog switch between them, and **`SYSCFG_PMCR` holds that switch**.
   The factory firmware sets it before our image ever runs. It read
   `0x01000000` one day and `0x03000000` the next, after a power cycle
   let the factory firmware start from cold.

   The day the switch was CLOSED, channel 1 was a mirror of channel 17,
   and the test that identified the channel put a finger on the button
   and watched channel 1 move. **The observation was right and the
   conclusion was wrong**: it had identified the mirror. When the switch
   came up open, the mirror went away.

   Two things follow, and the second is the general one:

   - **Do not inherit a register that decides what your measurement
     means.** `adc_init()` now WRITES `PMCR`, opening both switches
     every time, so the nets sit on the pads this board bonds -
     channel 17 and channel 16 - whatever ran before us. Opening is
     also the safe direction: it disconnects, where closing would hang
     an analog line on a GPIO.
   - A measurement taken through someone else's configuration measures
     their configuration too, and **theirs can change when yours did
     not**. That is a different failure from trap 16's "somebody else is
     wired to the same hardware": there the other party was active, here
     they had already left.

---

## Unexplained, and recorded rather than explained away

**A counter that cannot have counted that high — 05/11/2026.**

`sai1_dma_errors()` jumped mid-run from 3 to **4 245 803 899** and then
carried on incrementing normally by one. The counter does nothing but
`++`, so no sequence of events reaches that value: a 32-bit word was
written into it from outside.

What is known:

- It happened in **exactly one build**, for the whole of one run after
  about the eleventh status line. The build before it (1 199 status
  lines) and the build after (856) are clean.
- `.bss` is zeroed correctly — the startup loop covers `[_sbss, _ebss)`
  and the linker script collects `.bss`, `.bss*` and `COMMON`.
- The variable's neighbours in the map are **scalars, not arrays**, so
  there is no obvious buffer to have run past; `s_arm` next door is a
  struct written field by field.
- It is nowhere near the stack: 72 kB of ring buffers sit between it and
  the end of `.bss`.

What is not known: **what wrote it.** Nothing here is a fix, because
nothing here is understood well enough to fix.

What was done instead: the number is **never clamped** — clamping would
hide the only evidence — and `main()` now says so loudly, once, the
first time the count passes a million. A wild write that landed on a
diagnostic counter this time could land on something that matters next
time, and the difference between noticing and not noticing is that one
line.

---

## The way back — and the radio now runs from flash

**There is no JP1 on this board.** The schematic shows a BOOT0 jumper;
it is not fitted, and pin 94's trace disappears into an inner layer with
nothing near it. DFU is reached by holding the encoder buttons and
pressing power — which is a feature of the **factory firmware**, the
very thing a flash write would replace.

So before anything is written to flash there has to be another route
back: SWD that our firmware never disables, or our own button
combination jumping to the ROM bootloader. Cheap to write, and it
belongs before the first flash write rather than after it.

Until then: RAM only, and the radio cannot be bricked.

### What was established, 20/11/2026, on the way to the first flash write

- **The whole flash is backed up**, read twice over SWD and identical,
  with the option registers: `Deep-Malahit/backups/`, outside the repo
  (the factory firmware is not ours to publish). Its README has the
  hashes, the sector map and the restore commands. Bank 1 holds the
  factory code, byte-identical to the RE dump; **bank 2 holds ~790 kB of
  this unit's own data** that the RE dump does not have. Our image goes
  in bank 1 only and bank 2 is never erased.
- **How the factory firmware does it**, from its own code:
  - DFU at boot (`0x08007a34`): GPIOA clock on, PA1 an input, **read as a
    digital pin**; 0 means jump. "Both" is 0.86 V, below VIL, so it
    always reads 0; the small knob alone (1.05 V) is in the undefined
    band. This is also why it leaves PA1's analog switch closed - trap
    23's inherited `PMCR` was this code's requirement.
  - The jump (`0x0800e2ac`): SysTick off, RCC de-init, all 160 IRQs
    disabled and un-pended, MSP and PC from **`0x1FF09800`** - the
    H743's system bootloader. Not `0x1FF00000`, which is the option-byte
    boot address and was written here before as the jump target.
  - Power off (`0x08009a00`): PE1 (speaker amp) off, wake-up pin
    **WKUP4 = PC13 = SW3** armed on a rising edge, every clock enable
    cleared, `PDDS_D1|D2|D3`, **`DBGMCU_CR = 0`**, SLEEPDEEP, `wfi`. That
    last write is why SWD dies when the radio is "off".
- **Option bytes**: `FLASH_OPTSR_CUR = 0x0BC2AAF8` - RDP level 0, IWDG1
  software-started (no hardware watchdog to race on a cold boot).
- **THERE IS NO "RESET AND CATCH IT" OVER SWD.** Tried twice: NRST with
  `reset halt`, and SYSRESETREQ with `reset_config none`. Both lose the
  debug connection, the vector catch is lost with it, the factory
  firmware runs from its first instruction and switches the radio off.
  `openocd-ft2232d.cfg` claimed `reset halt` worked; it is corrected.

So the way back, once our image is in flash, rests on two things and
both have to be in it from the first write: **the DFU check as the very
first code at boot**, before clocks or anything that can fail; and SWD
while our firmware is running. There is no third: no BOOT0 pads, no
reset-halt — and, it turned out, no SWD while "off" either: keeping
`DBGMCU_CR` set in standby changed nothing, because on this unit "off"
is a power cut (`BOARD.md` section 3). `boot.c` now writes the factory's
`DBGMCU_CR = 0`.

### Flashed, 20/11/2026 — and the round trip proven

Our image is in flash and the radio boots it cold, from the power
button, with no PC attached. Bank 1 sectors 0–1 only; bank 2 untouched
throughout. Every link was checked on the radio:

| step | result |
|---|---|
| backup | three reads — SWD twice, ROM DFU once — identical |
| ours → flash | over the factory's DFU, read back byte for byte |
| our DFU, from flash | enumerates and holds power on this unit |
| ours → ours | rewritten over our own DFU |
| factory restored | all 2 MB identical to the backup, and it ran |
| ours again | over the factory's DFU, read back byte for byte |

**How to update the radio from now on** — no SWD needed:

    cd malahit/bringup && make flash        # builds build_flash/, writes nothing
    # radio: hold BOTH encoder buttons, press power -> 0483:df11
    dfu-util -a 0 -s 0x08000000:leave -D build_flash/bringup.bin

After `:leave` the image starts with the power button not held, so it
switches itself off; that is correct. Switch on with a short press.
Holding the power button for more than three seconds switches off
(`BOOT_POWER_OFF_MS`). The boot goes straight to the radio: screen and
audio 2.5 s after SysTick starts. The bring-up tests - 37 s of them -
run only in a `SELFTEST=1` build (`make flash SELFTEST=1`); see
`main.c`, "SELFTEST", for what had to move out of them first.

### What the first cold boots found — LOADING OVER SWD RESETS NOTHING, three more times

Every one of these hid behind state the factory firmware had left, so
not one RAM test could have shown it. The first flash image reached the
radio carrying all three.

1. **The button ladder hangs from the switched rail.** `PA1` read before
   `PC6` is up has no pull-up and reads 0: the first flashed image went
   into DFU on a plain press of the power button. The factory firmware
   raises `PC6` before its read; that was the one step of its recipe not
   copied.
2. **On this unit the MCU's own supply is latched by `PC6`** (`BOARD.md`
   section 3), so a DFU entered before `PC6` is up dies when the buttons
   are released. The owner caught it before the first write. The RAM
   test had passed because `PC6` was still high from the image before.
3. **`PWR_CR3` must be WRITTEN once after power-on** before the core
   voltage can change; `clock.c` said "deliberately not touched" and
   every image had run on the factory firmware's write. Cold, VOSRDY
   never set: 64 MHz, no sample rate, a dark radio that was in fact
   switched on — which the console said in plain words, and which a
   dark screen alone would never have.

The general form is the one at the top of this file, and it is now
earned in both directions: **write the registers you depend on — and the
pins, and the supplies — because the first boot from flash is the first
time nothing was written for you.**

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

> **Read this first - 20/11/2026.** Most of this section describes the
> factory firmware's watchdog, which is gone from flash. **Our firmware
> starts no watchdog** (nothing writes `0xCCCC`; the option bytes leave
> it to software), so nothing resets a hung radio. What happens now:
>
> - **A crash parks the core** in `fault_park()`: console flushed, record
>   at `0x2407FF00`, interrupts masked, the power button POLLED by
>   `boot_power_park()` - hold 3 s, the backlight goes, it switches off on
>   release. Before 20/11 the button was dead after a crash (it lives in
>   SysTick) and the radio could only be stopped at the battery.
> - **A load over SWD onto a parked crash switched the radio off** until
>   `core_clean` learnt to clear a pending SysTick - "LOADING OVER SWD
>   RESETS NOTHING", the third 20/11 entry. Some of the unexplained
>   power-offs in `SIGUIENTE.md` may have been this; that is not shown.
> - **`boot_standby()` did not sleep when called with a tick pending**:
>   the screen went dark, the MCU stayed powered, spinning through
>   `WFI`. It now clears pending system exceptions before `WFI`.
> - The next load reports the crash (`THE PREVIOUS RUN CRASHED`) only
>   if the radio was NOT switched off in between: the record is in RAM.

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
- **A polled loop on a SAI that is not running**, 05/11/2026. This one
  is worth the space because the "bounded wait" that was supposed to
  prevent it did not. `sai1_play()` polls `FLVL` and gives up after
  `(frames + 64) × 20000` **loop passes** — and a pass reads two APB
  registers at 120 MHz from a 480 MHz core, so the bound is not the
  microseconds it looks like on paper. One call measured about **46
  seconds**; the watchdog can only be stretched to 32.

  The stall itself was a missing restart: `sai1_dma_stop()` clears
  `SAIEN`, and `sai1_play()` — unlike `sai1_stream()`, which re-aligns
  whenever `s_aligned` is clear — went straight into its loop. Polled
  **after** DMA was simply an order nothing had asked for until a
  listening test did.

  Both halves are fixed, and the second one is the general lesson: **a
  guard counted in loop passes is not a time limit.** Every polled loop
  in `sai1.c` now feeds the watchdog from inside itself, so a stalled
  peripheral costs a silence of the wrong length instead of a radio that
  has to be switched on by hand.

  **This lesson was then re-learnt in the ADC driver, 05/11/2026**, by
  the person who wrote it down. `adc_init()`'s calibration wait was
  bounded by an iteration count that looked generous and was not, so the
  first run on the radio reported a calibration that never finished and
  the buttons did not work. The wait is now `board_millis()`-based, and
  **calibration is no longer allowed to be fatal**: it trims a few
  counts of offset against button bands thousands of counts wide, so
  refusing to start the converter over it was the wrong trade twice
  over. Where a wait like this cannot be escaped at all — `ADCAL` is
  cleared by hardware, so a stuck calibration blocks `ADEN` with no bit
  to clear — the way out is a peripheral reset through RCC, which is now
  what happens.

`main()` also brings the console up **first**, before the rail and
before SysTick, which it can because UART7 is on MCU pins on the
always-on rail and `SystemInit()` has already raised PC6 and locked the
PLL. The first line of output now precedes everything that can hang.

---

## State, 05/11/2026 — read this first

Steps 0 to 6 of `PORTING.md` §4 are done and verified on the radio,
step 7 — the graft of upstream code — is well under way, and **the SAI
now runs on DMA** (§4 step 10).

### The SAI is on DMA, both directions

`sai1_dma_start()` arms DMA1 stream 0 for the receiver and stream 1 for
the transmitter, both circular over two halves, and `main()` switches to
it at the boundary between measuring the hardware and running the radio.
Everything before that boundary — every test, every table in `BOARD.md`
§6 — still runs on the polled path, unchanged, so none of those numbers
has moved.

What it bought, measured on the radio by the same code in the same run —
the display loop runs its first eight frames polled on purpose, so the
two transports are compared on the same signal rather than against
yesterday's log — and **taken five times**:

| | polled | DMA |
|---|---|---|
| transmit underruns, 24 console lines | **24, every run** | **0, every run** |
| capture cost a frame | 43 315 µs | **386 µs** |
| frame period, four runs of five | 126.5–127.0 ms (7.9 fps) | **89.4–90.7 ms (11.0 fps)** |
| frame period, the odd one | 146.8 ms (6.8 fps) | 99.9 ms (10.0 fps) |

That one run was 10 % slower in **both** columns — something outside
this change was taking time in it — and the five ratios are 1.40, 1.40,
1.40, 1.47, 1.41. So the honest headline is **a 1.4× frame rate**, and
the absolute figures are worth what the bench is worth that hour.
Ratios between two rows taken a second apart survive almost anything;
absolutes taken an hour apart do not.

One underrun per printed line is not a coincidence: the transmit FIFO is
eight words — 21 µs at 192 kHz — and a console line at 115200 baud is
9 ms. That is the audible click per line, and it is gone. **`dry` stays
at 0 for the whole display loop**, through 16 ms of trace and 40 ms of
waterfall over SPI a frame, which is the thing the step existed to fix:
the panel no longer starves the audio.

**And it has been heard, 05/11/2026** — which is the half of this that
no counter can supply. `test_audio_listen()` runs 1.2 s polled against
1.2 s on DMA, six times, and the owner of the radio reported hearing the
alternation between a tone with clicks and a steady one. The same run
counted **72 underruns polled and 0 on DMA**, and 72 is exactly one per
printed line. Ear and counter agree, on the same signal, in the same
run.

It took three attempts to get that confirmation and the first two were
worth more than the third: see trap 17. The codec had been **muted since
step 4**, so "I heard nothing at all" was a precise measurement and the
counters were simultaneously right about the digital path. The listening
test now owns its own unmute and re-mutes afterwards.

No MPU regions and no cache maintenance, because the D-cache is off (see
"the caches" above). That is the only reason this was a small step, and
anything that later turns the data cache on owes the two rings a
non-cacheable region.

**No interrupt, deliberately.** Which half is finished comes out of
`NDTR`, which is what upstream's own `gd32_i2s_stream_write_half()` does
on the DEEPSDR. An ISR buys nothing until there is a demodulator that
must run once per block — and it will also need a named vector, which
`startup_stm32h743.S` has for nothing (all 150 external vectors point at
`Default_Handler`). That is step 8's work.

Three things cost a load each and all three are in the trap list above:
the factory firmware's own DMA streams still running on our request
lines (trap 16), `SAIEN` not holding (trap 6), and — once the retry for
that existed — **the retry itself exchanging I and Q**, because a failed
attempt had already moved an odd number of words through a stream that
was still running. The unit of retry has to be the whole arm: flush,
stream from the top of the ring, `DMAEN`, `SAIEN`.

How often that retry fires: **three or four times per four arms**, every
run, and into the tens once the polled starts are counted too. It is not
a rare path and it is not decoration. `SAIEN retries` stays at 0 in the
same report, which is the other half of the fact — the write is never
refused, it is accepted and then dropped.

### And the polled path had it all along

Found the morning after, on the first run following a cold power cycle,
and it is the more important half of the bug: **three polled captures
returned zero frames**, the first of them inside test 0, before anything
had armed a DMA stream. `sai1_start()` had never verified `SAIEN`
either. What that cost, in this order of visibility:

- a **row of test 0** missing from the table — the CKSTR/FSOFF sweep
  prints three rows instead of four;
- one frame of the display loop saying `capture failed`;
- and the one that mattered: **test 11's "polled" reference rows were
  the DMA rows averaged a second time.** `capture()` returned 0, `s_rx`
  still held the DMA block, and the two sets of figures agreed to the
  last digit — which is the only reason it was noticed. Four identical
  columns out of an analogue measurement are impossible.

`capture()` had printed `*** capture got 0 of 8192 frames ***` every
single time it happened. The test above it did not check the return
value. **That is the last trap in this list, committed in the code that
exists to catch it**, three days after it was written down.

Fixed in two places, and they are different fixes: `sai1_start()` now
verifies and retries like the DMA arm does, and `test_dma_transport()`
checks what `capture()` returned and prints `--- (no capture)` instead
of a comparison it did not make. After both: **zero failed captures**,
all four rows of test 0, and the two slot columns differing as two
measurements of the same signal a second apart should.

### Taking the request lines back, seen from both sides

Worth knowing before reading that line in a log: the scan reports
`0x2200` — channels 9 and 13 — on **the first load after a power
cycle**, and `0x0000` on every reload after it, because by then this
firmware has already unwired them and the factory image has not run
since. Zero is the right answer for a reload and the wrong one for a
cold boot, so the console says which is which rather than just printing
the number.

What proves the receive path is right, rather than merely moving: the
item rate out of `NDTR` reads **384 003 to 384 007 half-words a second**
against the 384 000 that two slots a frame require, and the
per-slot means match the polled reference channel for channel
(**+1641 to +1675** on slot 0, **−337 to −356** on slot 1 — this board's
I and Q offsets differ by a factor of five, which is what makes that
comparison able to catch a swap at all). The screen agrees: peak at
**+19 875 Hz, 126 dB**, the bin the arithmetic predicts, identical to
the polled frames in the same run.

### The knob tunes the radio

Upstream's own `encoder.c` — 241 lines of quadrature table, debounce and
press classification — grafted whole and compiled in place, of which
four lines touch a register. `compat/gd32f4xx.h` translates those four
onto this chip and declares nothing else, so any other GD32 call a later
graft needs will fail loudly at the name.

**It needed no new interrupt and no new vector.** Upstream asks for
`encoder_tick()` at 1 kHz from `SysTick_Handler`, in those words, and
this board already had exactly that — which matters, because
`startup_stm32h743.S` has a name for none of the 150 external vectors
and adding one is a change to the file every fault handler lives in.
Three GPIO reads and a table lookup a millisecond.

Measured end to end on the radio, 05/11/2026: turning the knob moves the
frequency readout, the ruler and the trace, and **the peak moved from
+19 875 Hz to −58 875 Hz** while the generator stayed where it was —
−78.75 kHz, which is the 79 detents that were turned, at 1 kHz each. The
spectrum follows the tuning exactly.

**The tuning knob is the big one**, `RENC1` on `PC0`/`PC1`. Moving the
driver there also tested the one claim in this step that rested on a
single report — which physical knob is which, read off the order the two
were turned in — and it held: the named knob tuned, the other went
inert. Peak +19 875 → −37 125 Hz **monotonically, no reversal**, inside
seven blocks, then still for 169 console lines while the small knob was
turned. Direction confirmed by the hand, so
`encoder_invertido_pon()` stays at its default; upstream carries it
because the sense differs between units.

Detents are read **once per frame, not once per detent**, which is what
`encoder_take_delta()` is built for: it accumulates quarter-steps in the
1 kHz tick and hands over whole detents with the remainder carried, so a
frame arriving 90 ms late gets the sum of everything the hand did and
loses none of it.

Open, and both are small:

- **The second encoder has no driver.** One knob is wired, the other is
  watched. Upstream has one encoder, so a second instance of its driver
  is new code rather than a graft.

### The buttons, off the ADC

Both encoder buttons sit on **one** analog line through a resistor
ladder, so this needed a converter rather than a pin. Working, measured
end to end 05/11/2026: a short press of the big knob cycles the tuning
step, through **upstream's own debounce and its own short/long
classification** — `-DENC_BTN_READ=adc_enc1_down` hands `encoder.c` a
function instead of a port and a pin, and everything below that reader is
untouched (`PROCEDENCIA.md`).

ADC1, channel 1, 16-bit, one conversion per SysTick with **no wait
anywhere**: a tick starts a conversion and the next tick collects it, by
which time it finished 975 µs ago. Two channels alternate, so each is
fresh within 2 ms against a 20 ms debounce. 32 369 conversions, **zero
timeouts**.

The three press plateaus measured 2.498 V, 1.656 V and 1.423 V where the
schematic's resistor values predict 1.95, 1.05 and 0.86 — and were
written down as "those resistors are not what is fitted". **They are**:
remeasured 20/11/2026 with `adc_init()` writing `PMCR`, the plateaus are
1.960, 1.050 and 0.861 V, within half a per cent of the schematic. The
05/11 figures were taken through the closed `PA1` analog switch the
factory firmware leaves behind — trap 23, which this measurement had
already been the victim of once, and was again for two weeks after the
trap was written, because its thresholds were never retaken. They would
have read the big knob as the small one and the small one as both.

What makes the identification solid either way is that **the third
plateau predicts itself**: "both buttons" is the two branches in
parallel, and that reading follows from the other two with no resistor
value and no reference voltage in the arithmetic — 17 129 counts
predicted against 17 091 measured. It held through the closed switch
too, which is the warning: a self-consistent ladder proves which state is
which, not that nothing is loading it. `BOARD.md` §6 has the table, and
`adc.c` keeps its thresholds as fractions of full scale because the
ladder's top and `VREF+` are the same rail — a sagging rail moves both
and the ratio not at all.

Three things this cost, all recorded as traps: a calibration wait bounded
in loop passes (trap: the watchdog lesson, re-learnt), `%-10s` in a table
(trap 8 again), and a channel scan whose absolute millivolts were wrong
because it preselected all twenty channels at once (trap 21).

### The second knob, and what it was given to do

Both knobs work now, and the second one did not need a second decoder.
**`User/encoder.c` is compiled twice**, the second time with its public
names renamed on the command line (`ENC2_RENAME` in `reglas.mk`).
Upstream keeps its state in file-scope statics, which makes it a
one-knob module; a second translation unit of the same source gets its
own copy of those statics, which is exactly what a second knob needs. So
both run the identical quadrature table, debounce integrator and
short/long classification — already tested on the first knob — and there
is no second decoder to drift out of step. The cost is that
`encoder2_tick()` has no definition to grep for, which `src/enc2.h`
says in as many words.

**What it adjusts is RF gain, and the choice needed stating.** The
obvious jobs for a second control — volume, mode, bandwidth — all sit
downstream of a demodulator, and this image has none: the only sound it
has ever made is a test tone. A control labelled for something that does
not exist is worse than one that does nothing, because it looks like it
works. RF gain does exist, is measured, and is the one setting that
matters while the AGC is a stub: too much and the converter clips, too
little and the signal is in the noise.

It is also **visible without audio**, which is what makes it checkable:
turning the knob swept `msi001_set_gain()` over its whole 0–59 dB range
in 90 detents and took the ADC fill from **10 % to 100 %** — from
underloaded to clipping. Its button toggles the preamp, measured at
+15.1 dB on this board.

**Known missing indicator:** the preamp has none. The ATT chip shows the
baseband reduction in upstream's own three-character notation ("OFF",
"-48"), and folding the preamp into it was rejected — the two are not
interchangeable, one being removed ahead of the mixer and one behind it,
and a chip reading 59 for both "59 at baseband" and "44 with no preamp"
would disagree with the knob in the hand. It goes to the console instead
and is written down here rather than papered over with a symbol this UI
has no convention for.

### The battery, and a stable number that was fiction

The header's battery pill shows a real percentage now. The voltage comes
from **ADC1 channel 16**, the midpoint of two 10 kΩ resistors across the
cell, and it was confirmed the only way that counts: a multimeter on the
cell said **4.15 V**, the converter said **4.17 V** — six parts in a
thousand.

**The rail is measured too, now, and not against anything outside the
chip.** `VREFINT` is a ~1.2 V bandgap that does not follow the supply, so
converting it against the supply measures the supply — every
millisecond, smoothed over about thirty. On this part the internal
channels are on **ADC3**, so `src/adc.c` drives two converters; the
sequence is one function taking an instance rather than two copies.

Both facts it needed are datasheet facts, and there is no datasheet
here, so both were established on the radio. The **channel** by
difference: everything read with the bandgap off and again with it on,
and channel 19 moved by 11 200 counts where nothing else moved by more
than 512. **ST's factory calibration** by plausibility: 24 327 at
`0x1FF1E860`, inside the only window a 16-bit conversion of a
1.18–1.24 V reference against 3.3 V can occupy — and its two neighbours
are the temperature sensor's two points, equally plausible and in the
right order. A wrong base address does not produce three sensible
numbers in a row.

| route to VDDA | result |
|---|---|
| this die's factory value | **3 298 mV** |
| the bandgap's nominal 1 216 mV | 3 274 mV |
| the multimeter, via the cell | 3 280 mV |

Within 0.7 %, and **only the third did not come out of this chip**. That
is the whole value of the meter here: it is the only one of the three
that could catch the other two agreeing on something wrong. The firmware
uses the factory value, which removes the bandgap's part-to-part spread.

What is left is the **divider**, and no reference can fix it: two 10 kΩ
resistors at 1 % allow about 40 mV at a full cell, and nothing inside the
chip can see a ratio error. So it was checked from outside, and the
result is worth more than the number: the radio said **4 163 mV** and the
meter said **4.16 V**.

**Three millivolts apart — and the trim stayed at zero.** The meter reads
to 10 mV, so "4.16" means somewhere in 4 155…4 165, and 4 163 is inside
it. The disagreement is **smaller than the resolution of the instrument
measuring it**, so fitting a constant to it would be fitting the meter's
last digit: it would look like a refinement and would be an arbitrary
offset. The honest reading is not "they agree" but "they do not disagree
down to 10 mV, which is as far as this meter can see".

`test_bat_calib()` prints the table to read the trim off, for the day
there is a meter with millivolt resolution. The trim it would set is
**multiplicative**, where upstream's `battery_calib_pon()` adds
millivolts — and upstream is right for its own board, because there the
error being trimmed is a diode's forward drop, which really is roughly
constant. Same idea, different shape, because the hardware under it is
different.

**The reading before this one was 3.05 V and was not a measurement at
all.** `PCSEL` was being OR-ed instead of set, so the battery's channel
was converted with the saturated `ENC_BUT` channel preselected beside it,
and an unconnected pad was dragged to a steady, plausible 1.53 V. With
one channel preselected per conversion that pad wanders between 0.05 and
0.1 V, which is what nothing-connected looks like. **A number that holds
still is not thereby a measurement** — that one had held still across
three runs and had been written into two documents.

The percentage is the weak part and is labelled as such wherever it
appears: a straight line from 3.3 to 4.2 V, which reads high through most
of a lithium cell's discharge because the real curve is flat there.
Upstream's `battery.c` was opened in the hope of grafting a real curve
and has the same two constants and the same interpolation.

### The die thermometer

Free, once ADC3 was there: same converter, same identification method,
its own enable bit. **Channel 18 moved 9 369 counts when `TSEN` went
on**, the bandgap 11 045 on channel 19. **40.5 °C** on the die, which at
480 MHz is well above the room — a reading near room temperature would
be the suspicious one.

ST's two calibration points are believed for the same kind of reason the
reference's are: 12 375 and 16 418 give **623 mV at 30 °C** and
**2.537 mV/°C**, an offset and a slope, independent of each other, both
landing where a silicon sensor has to be. Two words that were only read,
and no other interpretation of them produces two textbook numbers at
once.

Both calibration points were taken at 3.3 V, so the reading is referred
back to 3.3 V before it is compared with them — otherwise the
thermometer creeps as the battery sags, about half a degree at 3.28 V.

**Two bugs in the test itself, found by reading its own output:**

- **The channel search toggled the wrong bit.** It switched `VREFEN`
  twice and called whatever moved second "the thermometer" — but `TSEN`
  was on throughout, so the sensor's channel read the same in both
  sweeps and the runner-up was an unconnected pad drifting by 604
  counts. It named channel 16 and printed a confident warning that the
  build was wrong. **A difference test identifies only what it actually
  switched.**
- **The slope arithmetic overflowed 32 bits** and printed 0.087 mV/°C —
  in the middle of an argument that the numbers were right. Multiplying
  4 043 by 3 300 000 does not fit in a `uint32_t`.

Neither changed the temperature, which came out at 40.4 °C in the same
run from the right channel. That is the uncomfortable part: **the
headline number was correct while two of the supporting figures were
nonsense**, and a reader skimming for the answer would have carried away
a false "wrong channel" warning as well.

### The demodulator, and the first named interrupt vector

**AM by envelope, from the antenna to the speaker.** The chain is
upstream's own, described in `demod_am.h` and followed step for step:
channel low-pass on I and Q, `|z|` by `arm_cmplx_mag_f32`, DC blocker,
audio band-pass, AGC, stereo int16 out.

**What is NOT grafted, and why that is a departure.** This port's rule
is that upstream code is compiled in place. `User/demod_am.c` is **6 987
lines** and includes **thirty** other modules — RDS, SAM, spectral noise
reduction, HFDL, RTTY, CW, NAVTEX, PSK31, WEFAX, SSTV, Feld-Hell, FT8,
WSPR, AIS, ALE, JTTY, STANAG, APRS, DSC. It is not a demodulator, it is
the whole decoder suite with an AM path through it, wired into a main
loop, a settings store and a mode machine this board does not have yet.

So `src/am.c` is ours — but only the arithmetic. **The two pieces with
judgement in them stay upstream's**: `agc.c` and `audiofil.c`, compiled
in place, already in this build. A channel filter, a magnitude and a DC
blocker have one correct answer each; an AGC's feel and a filter's
corners do not.

**The block interrupt is the first named external vector in this
project.** `startup_stm32h743.S` had a name for none of the 150, which
is why the encoder was grafted onto the existing SysTick rather than
given one. Audio leaves no choice: it must be produced at block rate,
and nothing else in this firmware runs faster than eleven frames a
second. DMA1 Stream 0, priority 1 so SysTick still preempts it, and the
counts in the vector table written as arithmetic (`11`, then
`150 - 11 - 1`) because a table off by one entry is a jump into whatever
follows with no diagnostic at all.

It also retires a known hazard: `sai1_tx_due()` asks "is a transmit half
free" from whatever loop is running, and a producer more than one half
late simply misses it. A hook driven by the DMA cannot be late.

**The transmit ring grew to match the receive ring** — 8 192 frames a
half, 42.7 ms — so one receive half in is one transmit half out, and the
hook is handed both. That removed the 32 kB staging buffer a
demodulator would otherwise need on a part already at 89 % of its RAM,
and removed "which half is safe to write" from every future caller.

### Low IF, and the bug report that diagnosed itself

The first working audio was distorted, and **the user's experiment
identified it exactly**: with the generator's modulation switched OFF, a
tone remained, and its pitch tracked the detuning.

A true envelope detector cannot do that. `|z|` of an offset carrier is
constant — the offset rotates the phasor and the magnitude does not
move. So something else was sitting at 0 Hz and beating against the
carrier: the oscillator's own leakage. **At zero IF the carrier and the
leakage are both at 0 Hz, and in AM the carrier IS the signal**, so no
filter can separate them. Upstream names this in its own header as the
smear of distortion that always sits on the centre line, and calls it
the architecture rather than the filter chain.

The cure is low IF, and the user put the general form better than the
first attempt here did: **the tuning knob should drive the NCO and the
oscillator should be parked off to one side.** Step one of that is in:
the tuner is parked a quarter of the sample rate below the dial, so the
station arrives at +48 kHz and the leakage stays at 0, and `am.c` rotates
by exactly a quarter turn a sample to bring it back.

> **THE FIRST VERSION OF THIS SHIPPED WITHOUT THE MIXER IN IT**, and the
> way that happened is worth more than the fix. A scripted multi-part
> edit failed on its last substitution and therefore wrote **nothing** —
> but two smaller edits applied by hand afterwards did land, creating the
> mixer's phase variable and its reset. The file ended up with all of the
> mixer's scaffolding and none of the mixer. **It compiled perfectly**,
> because code without a mixer is entirely valid code.
>
> Then it was believed. The user reported silence with the modulation
> switched off, and that was taken as proof that low IF was working —
> but at exact tuning the beat frequency is zero, so silence is what
> zero IF gives too. **The observation never distinguished the two**,
> and a whole explanation was built on it, including a paragraph in
> this file.
>
> What actually settled it was `am_probe()`: correlate a raw block
> against DC, (-j)^n and (+j)^n and print all three. It came back
> **DC 1712, (-j)^n 1, (+j)^n 715** — the station nowhere near where the
> chain was looking, the leakage twice its size, and the rotation that
> brings it to zero being the opposite one from the derivation.
>
> Two lessons, and they are different. **Verify that the code you
> intended is in the file, not that the build succeeded** — a missing
> transformation compiles. And **an observation that both hypotheses
> predict is not evidence**: ask what the other one would have looked
> like before calling it confirmation.

**At exactly a quarter rate the mixer costs nothing**: multiplying by
(-j)^n is 1, -j, -1, +j — swaps and sign changes, no table, no
multiplies, and **no phase error at all**, where an arbitrary-frequency
oscillator would bring spurs of its own. It is also what upstream
settled on after backing out its first attempt at Fs/8 with a sine
table.

Consequences, both recorded rather than papered over:

- **The spectrum's centre is now the oscillator, not the station.** The
  ruler is centred on the tuner and `demod_px` marks the demodulation
  point a quarter of the trace to the right — which is upstream's own
  value, and the field was already there with a comment explaining that
  the DEEPSDR uses it and this radio did not. Now it does.
- **`iq_peak`'s figure stopped meaning anything.** It blanks the middle
  ±2 250 Hz, which was enough when the spike and the carrier coincided;
  with them 48 kHz apart the search lands on the spike's shoulder and
  reports +3 000 Hz. The number is not wrong, it is answering a question
  nobody is asking any more.

### The watchdog now feeds from the tick, and that is a policy change

The radio switched itself off during this step. It is a failure with a
signature: the independent watchdog is not fed for 32 seconds, resets
the part, the reset returns `PC6` to an input, `R38` pulls the
peripheral rail down, and the radio goes dark. From the bench it looks
like a hardware fault, and it costs somebody a trip to the power switch.

**A watchdog exists to recover from a hang. On this board it does not
recover** — it adds a second failure to the first, and the recovery is a
human hand. So `SysTick_Handler` feeds it now, and a stalled main loop
is reported instead of being fatal: `board_loop_alive()` stamps the
loop, `board_loop_stale_ms()` says how long ago, and the status line
carries it. If SysTick itself stops, nothing feeds the watchdog and it
still does its job — which is the one failure it can genuinely catch
here.

That change also settled the shutdown's cause, by eliminating one:
**the demodulator is not starving anything.** Measured with the core's
own cycle counter, the block hook costs **3.6 million cycles of a 20.5
million budget - 17.5 % of the CPU**, and the main loop reports itself
70 ms stale, which is one frame. Whatever switched the radio off, it was
not the audio path's load.

> The first version of that measurement reported **4 294 734 thousand
> cycles**, which is a small negative number wearing an unsigned face.
> It came from `board_cycles()`, which assembles SysTick's millisecond
> count with its countdown register and can return the two inconsistent
> when read from inside an interrupt. `DWT->CYCCNT` has none of that -
> a free-running counter of core clocks, monotonic, wrapping every 8.9
> seconds against a 42.7 ms budget.
>
> Even then the figure stayed wrong, because a **worst-case latches**:
> one early block had been measured across the counter's own
> initialisation and the maximum never came back down. Printing the
> LAST block beside the worst is what showed it - two numbers where one
> had been enough to mislead.

### What the bench cannot settle

The carrier sits a repeatable **2 250 Hz** from where the dial says, every
boot — 225 ppm at 10 MHz. It cannot be attributed from inside the radio:
the sample rate comes from the same 22 MHz reference, so a reference
error moves the recovered audio by the same 225 ppm and nothing internal
disagrees with anything else.

The only outside reference is the bench generator, and **the user says it
is not trustworthy** — an untrimmed VCXO. So the error is most plausibly
the generator's, the board's dial is not adjusted, and this stays
**unattributed on purpose**. Writing a correction factor fitted to an
instrument known to be wrong would be worse than the error.

### Back to zero IF, and what the factory firmware actually does

> **Superseded before it ever ran — see "Forward again, to a fixed
> 12 kHz" below.** Kept because the reasoning in it is the instructive
> part: every step follows, and the premise was a misreading.

The low-IF arrangement built over the previous sessions has been
**removed**: no digital oscillator, no offset, no window for the dial to
wander in, no cropped panadapter. The tuner is programmed straight to
the dial and the whole 192 kHz is shown, centred on it.

What changed the design was not a measurement here but a **reverse
engineering report on the stock firmware**,
`~/Projects/Radio/MalahitDSP/Firmware/Malahit_FW_1_10d_RE_report.v1.md`.
Three things in it overturn what had been assumed:

- **It runs zero IF** (MSi001 Reg0 `FIL_MODE=11`), not low IF.
- **Its sample rate is 160 kHz**, so its 160 kHz span is the whole
  baseband. The arithmetic that had been offered here — 192 − 160 = 32 =
  two 16 kHz offsets — was numerology and wrong. Its 160/80/40 presets
  are **FFT sizes** (2048/1024/512 bins), not decimation.
- **The oscillator's leakage is removed as the DC term it is**, by a
  blocker at the front of the DSP chain (report 2.4), not by standing
  far away from it. That is what makes zero IF liveable, and it is what
  this firmware now does too.

**Why the stock firmware can retune on every click without the AGC
restarting**, which is the question that started this:

- Its AGC **smooths the envelope** — `(1-a)*prev + a*blend` with attack
  and decay from the menu (report 6.1.4). Upstream's `agc.c`, which this
  port uses, has an **instantaneous attack**: "el pico salta a la
  muestra". One transient latches the peak and the release then takes
  half a second. That is the whole of the one-second dropout.
- A **DC blocker and a noise blanker** sit ahead of its detector, so a
  step from the PLL never reaches the level loop as a spike.
- It writes the tuner **only when a value actually changed**, with the
  encoder accumulating in the ISR and the main loop applying once.

Until this radio's AGC smooths rather than latches, `am_resettle()`
swallows two blocks and fades back in. That is a workaround for an
attack shape, and it is worth replacing with the real thing.

### Forward again, to a fixed 12 kHz — 19/11/2026

The zero-IF build was replaced **before it was ever loaded**, because a
model of it showed it could not demodulate a correctly tuned station.

`am.c` removes the leakage by subtracting a one-pole DC estimate from I
and Q. At zero IF **the AM carrier is that DC term too**, and no
estimate can take one and leave the other. The file's own comment said
so — *"It cannot be done at ZERO offset, where the carrier IS the DC
term"* — and survived the change that made it true.
`tools/ifmodel.py --zero-if` models the chain (70 % AM, leakage 2.4×
the carrier as `am_probe()` measured):

| carrier − LO | 1 kHz tone | 2nd harmonic |
|---|---|---|
| 0 Hz | **gone** — the rectified audio is all that is left | — |
| 0.5 Hz | −16.7 dB | **+7.1 dB above the tone** |
| 1 Hz | −8.2 dB | −4.4 dB |
| 10 Hz | 0 dB | −41.7 dB |
| 2 250 Hz (this bench) | 0 dB | −54.6 dB |

**The bench could not have caught it.** The generator sits a repeatable
2 250 Hz off the dial, where the model is clean. On a real station, with
a 10 Hz step and a synthesiser resolving 0.33 Hz, the carrier lands
within a hertz of the oscillator all the time.

**And the report never said what it was read as saying.**
`FIL_MODE = 11` is the MSi001's own output mode — baseband I/Q out of
the chip rather than its 450 kHz or 2 MHz IF — not where the firmware
puts the station. The stock DSP chain runs a **shift stage after its DC
blocker** (report §2.4 item 7, §4.3), and this file already recorded,
from the stock waterfall, the leakage "a few kilohertz off the tuned
station". Both pointed at an offset; the comment in `main.c` cited §2.4
and §5.3 for the opposite.

What runs now:

- **The tuner is parked `AM_IF_HZ` = 12 kHz below the dial**, on every
  detent — no window, no re-park, so none of the old clicking. `am.h`
  holds the constant, because `main.c`, the panadapter and `am.c` must
  all agree on it.
- **`am.c` subtracts the leakage at DC, then rotates by +12 kHz** with a
  16-entry table that repeats exactly — Fs/16, no phase accumulator.
  The sign is the one `am_probe()` measured for the Fs/4 mixer: station
  above the oscillator is on the negative side of `z = I + jQ`.
- **The panadapter shows bins [64, 512)**, 168 of the 192 kHz, centred
  on the dial; three `_Static_assert`s in `main.c` hold the window
  centred and the offset a whole number of bins. The leakage line sits
  12 kHz left of centre, as on the stock waterfall — and upstream's DC
  blanking in `fft.c` now blanks the leakage instead of the station.
- **`am_probe()` uses the demodulator's own table** and the console line
  reads `leak DC / station / mirror`. With a carrier on the dial,
  `station` must clearly beat `mirror` — not the leakage, which at the
  bench level (~1 700) is bigger than the carrier (~715). The line says
  `<- carrier on the dial` or `MIRROR LOUDEST` only when one side is
  over 400 and four times the other; the empty mirror side read at most
  118. **The first version compared the mirror with the leakage too,
  and could therefore never have fired** — found on the radio's first
  run, by reading the numbers rather than the flag.

**And then the numbers were misread too, the same day.** The first run
was written down as "noise, nothing on the dial" because `station` and
`mirror` swapped places from one line to the next. Counted instead of
glanced at, `station` won **52 lines of 65**, and on the second run
**46 of 52**, peaking at 625 — a weak, fading signal within about
±12 Hz of 10.000 MHz (the probe's 42.7 ms window is that narrow). Trap
12's lesson from the other side: four looks at a number that varies is
not a distribution, and a distribution was one line of Python away.
It is **the radio's own digital interference** — nothing on the
antenna, the stock firmware shows it at 10 MHz too, and in the
headphones it is bursts of pulses that fade as the dial moves off.
That **confirmed the rotation's sign on hardware without a generator**:
with the sign wrong, the dial at 10 MHz plays 9.976 MHz and the leak
sits 24 kHz outside the channel. It confirms nothing about the dial's
absolute accuracy, because a spur made from the board's own clocks
lands wherever the board's own reference puts it. `SIGUIENTE.md` has
the candidate sources and the knob test that separates them.

The same model on the new chain: tone flat and harmonics at the
numerical floor from 0 Hz to 100 Hz of carrier error, and the probe
formulas return `station 1.00, mirror 0.0000` on the measured
convention.

Two lessons, and the second is the expensive one:

- **A comment that states a precondition is a test nobody runs.** The
  sentence that described exactly this failure sat four lines from the
  code that broke it.
- **Before believing a source says X, find the sentence that says X.**
  "The stock firmware runs zero IF" was a true statement about a chip
  register, carried into a false one about a design. The bench was
  never going to arbitrate: both readings predicted a clean tone at
  2 250 Hz off.

### The retune transient, measured — 19/11/2026

The plan was to change the AGC: upstream's `agc.c` attacks
instantaneously, a retune's transient latches its peak, and
`am_resettle()` muted two blocks and faded back in to hide the "second
of silence" that followed. **Nobody had ever measured that transient.**
So before touching the AGC, `am.c` grew a trace (`am_trace_*`): per
2.67 ms chunk, the carrier's level, the audio peak into the AGC, the
AGC's peak follower and the output peak, two seconds around a retune.
`main.c`'s `RETUNE_TEST` moves the dial by itself, so the test needs no
hand and repeats exactly. Generator at 10.000 MHz, AM 70 % at 1 kHz.

| move | mute | dip below the final gain | > 1 dB under it for |
|---|---|---|---|
| ±1 kHz, 4 runs | none | 0.7 / 1.7 / 1.8 / 2.1 dB | 0 / 64 / 85 / 98 ms |
| onto the station from 20 kHz, 2 runs | none | 2.8 / 3.3 dB | 128 / 152 ms |
| ±1 kHz, 2 runs | `am_resettle(2)` | 85 ms silence, then 6.4 / 6.5 dB | 397 / 402 ms |
| onto the station | `am_resettle(2)` | 85 ms silence, then 5.9 dB | 365 ms |

**The workaround was the worst thing in the table.** `am_resettle()`
zeroed the DC blocker with the filters, so when the audio came back the
carrier hit the blocker as a step from nothing: +6.3 dB of transient,
which upstream's AGC then latched exactly as it was feared to latch the
retune. A retune inside the channel, left alone, costs about 2 dB for a
tenth of a second. Tuning onto a station costs 3 dB for 150 ms, and that
one is the DC blocker meeting a carrier that just appeared — inherent
to an envelope detector, not to the AGC.

So **the knob no longer mutes**, and **upstream's AGC is unchanged**.
The "second of silence" did not reproduce on this build; whatever
produced it belonged to an arrangement that no longer exists, and the
conclusion drawn from it — "the attack must be smoothed" — was never
tested against the thing it described.

Two things about the instrument, both found by reading its output:

- **It marked the retune after making it**, and the hook runs a whole
  42.7 ms block per interrupt, so a block of the new frequency — or of
  the mute — could already be recorded before the mark. The first run
  reported "gain before +49.5 dB" for the muted moves: a muted AGC at
  its floor, averaged into the baseline. Mark first, then move.
- **"Dip below the starting gain" is the wrong metric** once the move
  changes what is on the dial: tuning off a station legitimately ends
  30 dB higher. The dip is measured against where the gain ends up.

And one fact the trace gave for free: **the generator's carrier sat
within ±12 Hz of the dial.** At 10.000 MHz the probe read `station 889,
mirror 5`; at 10.001 MHz, `station` read 0.35 of that — the upper
sideband of 70 % AM, exactly. The "repeatable 2 250 Hz" this file
records did not reproduce today, and **the owner had not touched the
generator** — so whatever it was, it was on the radio's side, most
likely in the arithmetic of an arrangement that no longer exists. Not
verified.

### The retune click, found and repaired — 19/11/2026

With the mute gone the owner heard the dropout go and **a click stay**.
Same method again: look before fixing.

**A fine trace first** (`am_fine_*`, one point per 32 samples) showed
the leakage innocent — the 0 Hz term moved by 7–26 counts in 1 700 —
and the carrier collapsing by 11–16 dB with a phase jump of 75–190°,
right at the synthesiser write. **Then the raw samples** (`am_raw_*`,
2 048 frames around four writes), because a 32-sample mean cannot show
a shape: **13–50 samples after the write the station's I/Q collapses
into noise for 40–60 samples (0.2–0.3 ms) and comes back with another
phase** — the MSi001 relocking. Through the channel filter that is the
envelope falling to nearly nothing; through the DC blocker and the
band-pass, a spike as big as the programme; and upstream's AGC then
latches the spike, which was the 2–4 dB dip in the retune table.

The repair was designed on those four captures, offline, with the
chain's real coefficients (`tools/clickmodel.py` — upstream's
`audiofil.c` compiled on the host to print them):

- **Bridging the glitch in I/Q** (Hermite in amplitude and phase) was
  the best when the glitch's edges were found exactly and the worst
  when they were not — the PLL rings in phase after the main collapse,
  and a detector that catches the ringing bridges too much. Rejected as
  fragile.
- **Holding the carrier level** around the write and fading only the
  modulation — 1 ms down, 1.7 ms out, 1 ms back — needs no detection,
  only the write's time, which `am_retune_note()` pins to the sample
  from the DMA position. −20 dB on all four in the model, and a causal
  version (carrier level frozen from the DC blocker's own state) scored
  identically. That is what runs.

On the radio, same four moves, same metric (the click's energy above
2 kHz, where a smooth dip in a 1 kHz tone puts almost nothing and a
spike most of itself — `tools/clickdump.py`):

| | no repair (8 moves) | repaired (4 moves) |
|---|---|---|
| click above 2 kHz, re the tone | −1.9 … −23.6 dB, mostly −3…−8 | **−21.7 … −22.1 dB** |
| AGC dip on ±1 kHz | 1.7–4.4 dB, up to 220 ms | **0.0–0.3 dB** |
| onto the station from 20 kHz | 2.8–3.3 dB, 128–152 ms | 2.0–2.2 dB, 90 ms |

**And by ear**, the owner's: on the generator's steady tone the clicks
are still there but no longer sharp — duller; on noise alone, off the
signal, no retune click at all. "Tolerable." That is the table in other
words: the spike is gone, and what a steady tone still reveals is the
2 ms the modulation is faded for.

The fade starts **before** the write — a hook that processes whole
blocks after the fact can do that, if the write is at least 192 samples
into its half — so `dial_tune()` waits (at most 1.3 ms) until the DMA is
far enough in. And it writes the band filters only when they change,
like the stock firmware: the add-on board's 595 is bit-banged on lines
that run to a board in the RF path.

Three things about the instruments, all found by reading their output
rather than their verdicts:

- **A metric that scores three repairs identically to the last digit
  is measuring itself.** The audio filters need tens of milliseconds to
  settle and the capture had 1.3 ms before the write; every peak was
  the start-up transient. The model now gives them a 40 ms lead-in, and
  it is exact rather than invented: 1 kHz modulation is periodic in
  192 samples at 192 kHz.
- **A glitch detector on |z| catches the modulation's troughs** — 70 %
  AM takes the envelope to 0.3 of the carrier by itself. The
  sample-to-sample change of the complex envelope is the better
  detector, and even it catches the PLL's ringing.
- **The peak residual from the tone is the wrong metric for a repair
  that fades**: it scores the planned 2 ms gap as loudly as the spike
  it replaced. Above 2 kHz is where a click lives and a fade does not.

`RETUNE_TEST` (now in `am.h`) brings all of it back: 1 for the table,
2 for the fine and raw dumps. At 0 the three buffers shrink to one entry
each and give back about 35 kB.

### What runs on the radio

Clock tree at 480 MHz from the 22 MHz bypass oscillator, switched rail,
UART7 console, crash reporting, ILI9488 at 18 bpp, FT6236 touch,
NAU8822 codec with SAI1 as I²S master, MSi001 tuner, and **the whole
480 × 320 main screen drawn by upstream's own UI** — `ui_top.c` for the
header and status band, `spec_chrome.c` for the gutter and ruler,
`spectrum.c` for the trace, `ui_act.c` for the buttons, with a
scrolling waterfall.

Every touch target answers, confirmed under a finger: six action
buttons, the AGC/ATT/NCO/JMP chips, the frequency readout, the band
pill, MEM and the clock. Nothing is wired behind them yet, so touching
one reports itself and says nothing changed.

**The controls do something now.** The big knob tunes, a short press of
its button cycles the step, the pill names the step, and the battery
pill shows a real voltage as a percentage. The panel also comes up
**before** anything that needs a hand, so a test that wants a finger
asks on the screen the finger is near rather than over the UART.

Frame cost: capture 0.4 ms, DSP 0.6 ms, trace 16 ms, waterfall 40 ms,
and a measured **91 ms a frame — 11 fps**. The waterfall blit is now
the largest single item by a factor of two, and the next frame rate
comes out of the SPI, not the SAI.

With a generator at 10.000 MHz on the bench: tuning error 0 Hz, the
peak lands in the bin the arithmetic predicts, image rejection 30 dB
with `iqbal` off and 34 dB on by its own A/B test. Those were taken
before the DC fix below and the absolute levels around them need a
retake; the ratios do not.

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

### The DC offset is fixed, and it was ours

The worst number on the board was a register this firmware wrote on a
misreading. `msi001.c` selected `DCCAL = 000`, believing static mode
"calibrates once and then holds"; table 20 defines 000 as **no DC
tracking**, so selecting it switched the trim off. The part's own
power-on default is continuous, which means **the tuner arrived better
configured than our driver left it**.

**What settled it was running the same test four times.** Identical
code, same bench, nothing on the antenna:

| | run 1 | run 2 | run 3 | run 4 |
|---|---|---|---|---|
| `DCCAL` = 000, as we wrote it | **+2.4** | **+0.3** | **−17.6** | **−22.6** dBFS |
| periodic1, now the default | −25.5 | −26.1 | −25.8 | −25.1 dBFS |

Static spans **25 dB** across four runs, because it holds whatever the
trim was left holding and that depends on the history of the part. The
+2.4 dBFS end is a **railed converter** — ±1410 mV is the rail, and both
channels reached it. And −7.7 dBFS, the figure this file carried for
weeks as a hardware limit, sits in the middle of that spread: it was
never a measurement of the board, just one sample of something with no
settled value.

Now: 26 dB of ADC headroom recovered, repeatable to a tenth of a dB,
and the screen's working point (12 dB GR, preamp on) uses 18 % of the
range instead of all of it.

`test_dc_offset()` in `main.c` is the measurement, it needs no
generator, and `BOARD.md` §6 has the full table including why not
continuous, why not the speed-up bit, and the one place static hold is
still the right answer.

### THERE IS AN ADD-ON BOARD, AND WE DO NOT DRIVE IT

**This unit has a `MalahitAddonBoard` between the antenna and the main
PCB** — a High-Z input, a PE4312 step attenuator (0–31.5 dB), a second
four-filter bank, and a 74HC595 driving all of it. BOARD.md §7 has the
full control map, read out of the KiCad netlist at
`~/Projects/Radio/MalahitDSP/MalahitAddonBoard/`.

**Every RF measurement in these files was taken through it, and our
firmware has never written a bit to it.** So it has been sitting in
whatever state the factory firmware last left — and the stock
firmware's own display reads `ATT 30` on this bench. If that is this
board's attenuator, thirty decibels of the signal path were
unaccounted for in everything measured so far.

This is the "loading over SWD resets nothing" lesson one level further
out: not an inherited register, an inherited **board**. And the relay
is **latching**, so its position survives a power cycle — there is no
default to fall back on.

**The pins are now known** — J2 goes to **J7**, the 10-pin `OPTIONAL`
header, and all six signals are identified: `Vext_ON` = PD9, the relay
coils = PD8 and PD11, `595LE` = PD10, `595DIN` = PD12, `595CLK` = PD13.
BOARD.md §7 has the evidence.

Two independent sources agreed, and neither needed a single write to
the hardware:

- **The live registers.** Our firmware never writes port D above bit 1,
  so the configuration the factory firmware left was still sitting in
  `GPIOD->MODER` and `ODR` and could just be read over SWD. That is the
  "loading over SWD resets nothing" rule being *useful* for once,
  instead of costing a day.
- **The factory image.** `~/Projects/Radio/MalahitDSP/Firmware/dsp1_dump.bin`
  is plaintext Thumb, and both the latching-relay pulse pair and the
  74HC595 shift-out loop are plain to read in it. The loop also settles
  that the byte goes **MSB first**, which had been an assumption.

Worth remembering as a method: **when a peripheral's wiring is
undocumented, the firmware that already drives it is a schematic.**

**Step 9a is done:** `src/addon.c` drives the relay, and **PD8 selects
the 50 Ω input, PD11 the High-Z one**. That was settled by letting the
stock firmware pick the position by name and then asking which of our
coils reproduced it — the as-found level landed 0 dB from PD8 and 3 dB
from PD11. BOARD.md §7 has it.

Also now confirmed by the owner rather than inferred: **the factory
firmware leaves the add-on board's attenuator at 30 dB.** Thirty
decibels that no level measured before this was accounting for.

Two things that step taught, both about the test and not the radio:

- **A threshold is a prediction.** The first version expected tens of
  decibels between High-Z and 50 Ω and reported "the relay is not
  switching" for a relay that was audibly clicking. A High-Z input
  barely loads a 50 Ω generator, so it sees the full open-circuit
  voltage — 6 dB more than matched — which the follower and the
  transformer then mostly give back. A few decibels was always the
  right answer, and nobody had checked the prediction against the
  circuit before using it to judge the hardware.
- **Autoranging cancels the thing you are measuring.** `rf_autorange()`
  holds the converter near half scale, so a level comparison must fix
  the gain first or it compares nothing at all.

**Step 9b is done too:** the 74HC595, the filter bank and the PE4312
attenuator are all driven from `src/addon.c`, and `main()` now puts the
board into a **known state before any RF measurement**. The filter bank
measures 37 to 67 dB of rejection at 10 MHz; BOARD.md §7 has the truth
table and the figures.

It cost four runs it should not have, and all four mistakes are worth
keeping:

- **The drawings disagreed and I believed the wrong one.** The port
  order came from the KiCad rebuild; the vendor's PDF has U4's four
  ports in a different order. Fourteen of the sixteen codes connect
  the input of one filter to the output of another, and the four I
  picked were four of those fourteen. **The caveat that the rebuild
  must be checked against the vendor PDF is in BOARD.md, written two
  hours earlier by the same hand that then did not check.** Writing a
  caveat down is not applying it.
- **The board LATCHES, so a bad write poisons every later run.** The
  add-on board keeps its own power; its shift register and its
  attenuator hold their state across a firmware reload. One wrong
  filter code and every subsequent run measured a dead receiver —
  including the earlier tests, which had nothing to do with it.
- **I misattributed the damage, twice.** 31 dB of loss looked exactly
  like the attenuator's 31.5 dB maximum, so I rewrote the latch
  handling and then the whole attenuator driver chasing it. The
  evidence against that theory was in the same log all along: **the
  relay's 3 dB difference had vanished too**, and a relay cannot stop
  switching a signal that is absent.
- **A number that coincides is not a diagnosis.** "31 dB ≈ 31.5 dB"
  was a coincidence of two numbers, and it bought two runs of
  confident work in the wrong direction.

And the thing that actually worked: **when the drawings disagree, sweep
every combination.** Four bits is sixteen codes and a minute of bench
time, and the result is immune to both drawings. §6 settled the main
board's bank the same way with all 64.

**Step 9c is done:** the PE4312 attenuates by what it is told, **within
0.3 dB over its whole 31.5 dB range**, and returns exactly to the
reference at zero. The sweep walks the six individual bits rather than
round decibels, which tests the bit order, the step size and the latch
at once — a reversed bit order would run the column backwards, and a
whole-decibel register would double every entry.

One more instrument bug, found the same way as the others: the first
run read a tidy **1 dB short at every setting**, which looks like a
real property of the part. It was `(int32_t)pk.db` truncating both the
reference and the reading. **An instrument quantised to 1 dB cannot
check a part specified in half-decibel steps**, and that should have
been settled before the measurement rather than after it.

### The add-on board is fully driven now

Relay, filter bank and attenuator, all in `src/addon.c`, all verified
on the radio, and `main()` puts the board into a known state before any
RF measurement. The thing that made this step cost four unnecessary
runs is worth stating once more, because it applies to the whole board
and not just to one bug:

**It latches, and it keeps its own power.** The shift register and the
attenuator both survive a firmware reload over SWD. So a wrong value
written once is still there on the next run, and on the one after that
— poisoning tests that have nothing to do with it and looking exactly
like a bench fault.

### The front-end table is retaken

Done 04/11/2026, on a path that is known end to end for the first time:
50 Ω input, add-on attenuator set by us, filter chosen by frequency,
and **the bench level written down** — 100 mV through a 40 dB pad,
−47 dBm at the antenna. `BOARD.md` §6 has it.

- **Port 2 beats port 1 by 48.7 dB**, reproducible to 0.2 dB. The old
  table could only say "at least 24.7", because on port 1 the carrier
  had sunk under the noise and the peak search was reporting something
  else.
- **The preamp is worth 15.1 dB**, against the old 14.7 — the one
  figure in the old table that survived, because a ratio between two
  rows taken a second apart survives almost anything.
- **Gain reduction tracks to 0.1 dB above about 32 dB and compresses
  below it.** Where the control is trustworthy is a more useful fact
  than an average error, and it is the boundary an AGC has to respect.

Two mistakes in the retake, both about how a measurement is presented
rather than taken:

- **Zero gain reduction cannot be a reference row, at any input level.**
  §8b already said why — 77 dB of baseband gain fills the converter
  with its own noise — and the first retake tried anyway, wound the
  add-on attenuator to its limit looking for a level that does not
  exist, and reported the loudest *noise* bin to a tenth of a decibel.
  `addon_report_tail()` now flags any peak that is not where the
  carrier must be, and no derived figure is printed unless both its
  rows passed.
- **A constant offset across every row is the signature of ONE bad
  row.** Measured cumulatively against a compressed reference, the gain
  sweep read a flat −9 dB error everywhere and looked like a gain
  control broken throughout. Consecutive steps showed three of four
  within 0.1 dB. Same numbers, opposite conclusion.

### The panel on DMA, and why the queue runs from an interrupt — 21/11/2026

**What runs:** every pixel block gfx2 and gfx hand over is converted
into one of eight 4-row slots (`ili9488_q_slot()`), queued
(`ili9488_q_push()`), and sent by DMA1 stream 2 - request 62, SPI3_TX -
under its own window and RAMWR. SPI3's end-of-transfer interrupt
(vector 51, priority 0) raises CS, sends the next block's eleven bytes
of window commands polled, and starts its DMA. The display loop only
converts and queues; it waits when all eight slots are taken. Anything
else that touches the panel - a fill, a register read, the init - waits
for the queue to empty first (`bus_idle()` in `ili9488.c`), so nothing
can land among a block's pixels.

**Why from an interrupt, measured in order on one run each:**

| | frame | waterfall |
|---|---|---|
| polled | 79.9 ms | 48.3 ms |
| DMA, the display loop starts each block | 65.5 ms | 39.5 ms |
| DMA, the EOT interrupt starts each block | 47.5 ms | 30.2 ms |

The waterfall's bytes need 32.4 ms. The polled loop lost the rest to
interrupts - the SPI FIFO is 16 bytes, 4 us, and the AM block interrupt
holds the core 8.4 ms (`hook` on the status line, ~4 000k cycles). The
first DMA version won back the CPU's conversion time but not that: a
block is 2.7 ms on the wire, so whatever block was going out when the
demodulator arrived finished, and the bus then stood idle until the
display loop got the core back. The arithmetic matched to the
millisecond (29.7 ms of wire + 8.4 ms + a little = 39.5). Only something
that outranks the demodulator, with more than 8.4 ms of blocks already
converted, keeps the bus going through it: eight full slots are 12.3 ms.

**The switch, for any A/B:** `swd-call.sh build/bringup.elf
ili9488_dma_enable 0` (or 1) on the running radio. The status line's
`lcd dma q8 0/0/1` reads: transport, deepest queue this frame (8 = the
loop ran ahead of the wire, the healthy case), transfers failed, queue
stalls, DMA error flags seen. Once a second, `lcd:` counts transfers.

**What `wf` and `trace` mean now:** time to QUEUE, not to send. They
fell to 30 and ~5-9 ms partly because the last ~12 ms of each drain
under whatever comes next. `frame` is the honest number.

**FEIF is set on every transfer** - 118 455 of 118 455 - and nothing is
wrong: all of them ended by EOT, the self-test's timing is the wire's,
and FEIF stops nothing. The likely reason, not proven: in direct mode
the stream preloads ONE byte, and an empty 16-byte SPI FIFO asks for
sixteen back to back at the start of every block - an underrun by
definition, and harmless, because an SPI master just waits for data
(the polled path paused for milliseconds at a time for weeks without a
wrong pixel). The test, if it ever matters: FIFO mode (`DMDIS`) in
`spi3_dma_init()`, and watch the `FEIF on` count stop.

**Two things changed on the way that are not about the panel:**
- **`DWT->CYCCNT` is no longer zeroed** by `ili9488_fill_screen()` or
  `am_init()`. It is shared - am.c, sai1.c, boot.c and now the DMA
  timeout all time intervals on it - and a reset in the middle of one
  makes it four billion cycles long. Everything uses differences.
- **A complete window arms nothing** in `rm68120_ili9488.c`, and the
  window's last pixel queues its last block. Before, the tail of a
  window taller than the buffer sat staged until the next
  `set_window()`; pixels written past a window were sent as rows below
  it, and are now dropped (nothing upstream writes any).

**What it does to the 10 MHz digital noise is not known.** The SPI is
now busy ~95 % of the frame instead of in bursts of 38 + 41 ms of every
120 ms. If the panel's 1.25 MHz pixel comb is the source (SIGUIENTE.md,
"Where it comes from"), each burst is the same as before and there is
less silence between them - steadier, present more of the time, no
louder while it lasts. Listen with the generator off.

### THE NEXT THING TO DO

- ~~**An encoder driver.**~~ Done, 05/11/2026 — see above. What is left
  of it is the buttons, which need the ADC, and the second encoder.
- ~~**The panel on DMA.**~~ Done, 21/11/2026 — the frame from ~80 ms
  to ~47 ms. "The panel on DMA, and why the queue runs from an
  interrupt" below, and `PORTING.md` step 21.
- **SAI on DMA is done** (above), and the one thing it left undone is
  the interrupt: `sai1_tx_due()` is a polled stand-in for the
  half-transfer interrupt and can miss a whole half if the producer is
  more than one half late. Nothing is late yet, because nothing
  produces audio.

After that: the other screens — menus, band list, keypad, FT8 map —
live in `ui_digi.c` (1817 lines) and `ui_kbd.c`, already separate
modules, and the same host harness renders them.

### Still open

- **The audio outputs come up MUTED and nothing but the listening test
  unmutes them.** `nau8822_configure()` ends with `nau8822_mute(1)` on
  purpose and the default should stay; what does not exist is anybody to
  turn it off — no volume control, no mute policy, nothing that knows
  whether a human wants to hear the radio. It is step 8's to own,
  together with the audio it will finally have to play, and until then
  `test_audio_listen()` is the only thing in the image that makes a
  sound. See trap 17 for how three days of silence hid in that gap.
- **`Vspk_ON` on PE1 is never written by this firmware.** The speaker
  amplifier's supply is whatever the factory firmware left, and it has
  been left ON — measured 05/11/2026, `MODER` an output and `ODR` bit 1
  set. So the audio worked today by inheritance. That is the "loading
  over SWD resets nothing" rule in the analogue domain, and the rule's
  own answer applies: write the pin we depend on. `board.c` is where it
  belongs.

- **`demod_am.c` is step 8, not step 7.** The file is portable, but its
  transitive closure is 106 `.c` files and 72 126 lines, and upstream
  has no switch to leave modes out. It wants the display layer in place
  first — see the box under step 7 in `PORTING.md`.
- ~~**SAI runs polled, not DMA.**~~ Done, 05/11/2026 — and the "one
  click per printed line" guess turned out to be exactly right: 24
  console lines produced 24 underruns on the polled transport and 0 on
  DMA. See the state section above.
- **periodic1's 195 Hz comb has not been looked for**, and upstream's
  architecture says it probably does not matter. The trim tracks for
  2.5 µs every 5.12 ms, repeating at 195 Hz, which is squarely in the
  audio band — but it repeats at the *analogue* DC, and upstream never
  puts a wanted signal there. `NCO_OFF_MIN_HZ` keeps the station
  **6 to 44 kHz above the LO, never below it and never through the
  middle** (`User/main.c`, the long block above the define), because the
  LO leakage self-mixes to 0 Hz and would otherwise be heard as a
  carrier that is not on the air. So the comb lands at least 6 kHz from
  the demodulated audio and the AM channel filter is another 14.5 dB on
  top of that. It still wants an FFT with a quiet input before being
  called harmless; `msi001_set_dc_timers()` is there if it is not.
- **Image rejection still wanders, 26 to 41 dB between runs**, and the
  known-good path did not settle it — so it is not the add-on board, not
  the attenuator and not the filter bank. Everything else on this bench
  now repeats to a tenth of a decibel, which makes this the one figure
  left that behaves like trap 12: a quantity with no settled value is
  being produced by something with no settled state. `iqbal` adapts, so
  the first question is whether the spread is its convergence rather
  than the hardware's balance — which means logging the coefficients
  alongside the rejection, not just measuring it again. It may be the
  same asymmetry as the next bullet.
- **The I and Q offsets differ by a factor of five**, consistently,
  across every mode and every run — I is the worse channel. The trim
  handles each separately so it does not matter for headroom, but it is
  an asymmetry in the analogue path that nothing explains yet, and it
  may be the same asymmetry the image rejection is measuring.
- **The battery pill is new and unreviewed.** It had never appeared on
  this layout — drawn off-screen and clipped away, see `PROCEDENCIA.md`
  — so it was placed beside the clock without anyone having had the
  chance to dislike it. Four constants in `compat/malahit_layout.h`
  (`HDR_BAT_*`) move it, and it still reads 0 %. That is no longer for
  want of an ADC — PA0 is read every other millisecond and the voltage
  reaches the console — but because a **percentage** needs the cell's
  discharge curve and a reference voltage that has been measured rather
  than assumed. A straight line between two guesses would look like a
  measurement, which is worse than an empty pill.

### The bench, as it stands

- Generator at **10.000 MHz**, **AM 70 % at 1000 Hz**, by **coaxial
  cable** to the antenna input, through **two attenuators, 40 dB and
  10 dB in series**. `lsusb` is how to tell it is connected at all: no
  `0403:6001` means no carrier, and the spectrum then wanders over the
  noise floor exactly as a broken tuner would.

  **Write the amplitude and the pads down, every time.** The old
  front-end table says "≤ −40 dBm", which is not a level, and that is
  the single reason it cannot be compared with anything measured since.
  Into 50 Ω, RMS: 1.5 mV is −43.5 dBm, 20 mV is −20.5 dBm, 500 mV is
  +7 dBm; each 10 dB of pad subtracts 10.

  | generator | pads | at the antenna | |
  |---|---|---|---|
  | 1.5 mV | none | −43.5 dBm | plateau 39 dB, generator-limited |
  | 20 mV | 50 dB | −71 dBm | plateau 23 dB, far too weak |
  | **500 mV** | **50 dB** | **−43 dBm** | **the target** |
  | 150 mV | 40 dB | −43.5 dBm | equally good if 500 mV clips |

  The generator wants to run **hard** and the pad wants to be **large**:
  the receiver only sees the ratio of the two, while the generator's own
  noise depends only on how hard its DAC is working. With 70 % AM the
  envelope peaks at **1.7× the carrier**, so an amplitude set to the
  generator's maximum will clip it — and clipped modulation looks
  exactly like the mess this whole exercise was chasing.

### There is a WALL at the top, and it is not the receiver's input

"Generator hard, pad large" has a limit that has nothing to do with
levels or ratios. **At 500 mV the generator radiates straight into this
unshielded board**, and then the attenuators, the antenna connector, the
filter bank and the mixer are all bypassed — so no setting of the
receiver can help. The symptom is spectacular and points nowhere:

    rf_autorange: still 99% at 60 dB GR - the input is too strong

Sixty decibels of **baseband** gain reduction, and the converter still
full. That is impossible for anything arriving through the mixer, and
that impossibility is the clue.

**The `input PGA muted` row of test 8 is what proves it**, and it was
written to answer a completely different question (is the DC offset the
codec's?). Muting the codec's input PGA disconnects the tuner from the
converter, so anything still present is arriving *after* the input
stage:

| generator | pads | `input PGA muted` |
|---|---|---|
| 1.5 mV | none | `ac 11, used 0 %` |
| 20 mV | 50 dB | `ac 11, used 0 %` |
| **500 mV** | **50 dB** | **`ac 18438, used 99 %`** |

Identical receiver level in rows one and three, and one of them has the
bench shouting into the board. **Keep that row in mind as the pickup
check**: `ac 11, used 0 %` means the bench is clean, and anything else
means stop measuring and find the leak.

So the usable window has a wall at each end, and they are different
kinds of wall:

- **below**, the generator's own DAC noise, which gets worse as its
  amplitude falls — measured at 39 dB down at 1.5 mV of output;
- **above**, radiated pickup into the board, somewhere between 20 and
  500 mV of generator output.

Which puts the working point at **100–150 mV with the 40 dB pad** —
−43 dBm at the antenna, the generator 36–40 dB above its minimum so its
quantisation noise is ~76 dB down and invisible, and a radiating field
10–14 dB below the one that broke it.

**That is the setting now in use, and it works.** 100 mV through the
40 dB pad puts `input PGA muted` back at `ac 11, used 0 %`,
`rf_autorange()` picks 16 dB, the ADC sits at 51 % with no clipping,
and the AM depth comes back within 0.46 dB of what the generator was
told to produce — see BOARD.md §6.

### The stock firmware is a second instrument; use it

The factory firmware runs on the same hardware and sees the same
signal, so loading it is a cheap way to ask whether a number is ours or
the board's. On this signal it reports **SNR 46 dB** and chooses
**ATT 30** — the same order as `rf_autorange()`'s own choice, so our
gain decisions are not eccentric.

Its SNR is not directly comparable to the `snr` in our per-frame line
(it measures in the channel bandwidth; ours is the peak over the median
across all 192 kHz), but it is close to the ~50 dB that the two-level
analysis above predicted for a receiver-limited bench.

**And it shows the same artefact at the centre of its spectrum**, with
the LO leakage as a separate line in its waterfall a few kilohertz off
the tuned station. That settles whose it is: the leakage belongs to
this hardware, which is why upstream parks the station 6 kHz off the LO
and subtracts the DC in `demod_am.c` rather than pretending otherwise.

### The generator's own noise floor is 39 dB, and that is a ceiling

Worth knowing before trusting any ratio measured on this bench. At its
minimum output the OSA103's DAC is working in a handful of least
significant bits, and its quantisation noise is broadband.

`test_signal_quality()` measures it without touching the hardware, by
walking our own gain and watching the peak-to-median ratio — **a number
that changes when we change our gain is ours; a number that does not is
in the signal**:

| preamp | GR | ADC | peak | floor | snr |
|---|---|---|---|---|---|
| ON | 0 | 100 % | 128 | 104 | **24** ← clipping |
| ON | 6 | 100 % | 126 | 97 | **29** ← clipping |
| ON | 12 | 100 % | 124 | 90 | **34** ← clipping |
| ON | 20 | 52 % | 120 | 81 | **39** |
| ON | 30 | 17 % | 111 | 71 | **39** |
| ON | 40 | 8 % | 100 | 61 | **39** |

**Clipping cost up to 15 dB and was ours.** A clipped carrier is not a
loud carrier, it is a *different signal* — flat-topping makes
intermodulation products at frequencies nothing is transmitting. The
ratio climbs 24 → 39 dB as the gain comes off. That part the gain-walk
settles on its own.

### But one level cannot tell you WHOSE noise the plateau is

The plateau then stops dead at 39 dB and three more gain settings
cannot move it. It is tempting — and it was the first conclusion
written here — to call that the generator. **The gain-walk cannot show
that**, and the reason is worth keeping:

- noise added *after* our gain would give a plateau that **worsens**
  with more gain reduction;
- noise arriving *with the signal* — the generator's — gives a **flat**
  plateau, because our gain scales both;
- the receiver's own **front-end** noise also arrives before the gain,
  so it gives a flat plateau too.

The last two are indistinguishable by anything we do to our own gain.
Separating them takes **two generator levels**, because an external
attenuator scales the generator's signal and the generator's noise
equally — so the carrier-to-generator-noise ratio arriving here does
not depend on the attenuator at all, only on how hard the generator's
DAC is working.

| at the antenna | plateau | implied noise |
|---|---|---|
| −43.5 dBm (1.5 mV, no pad) | 39 dB | −82.5 dBm |
| −71 dBm (20 mV, 50 dB pad) | 23 dB | −94 dBm |

The weak point has to be receiver-limited — nothing else puts the floor
*lower* when the generator is working *harder*. So **this receiver's own
floor is about −94 dBm** in this measurement bandwidth, which at
−43.5 dBm would allow 50 dB. We measured 39. The missing 11 dB is the
generator's own noise at 1.5 mV of output, and raising it to 500 mV is
+50 dB of its DAC's signal-to-noise, which puts its contribution ~89 dB
down and out of sight.

**Prediction, to be checked:** at −43 dBm with the generator run hard,
the plateau should read about **50 dB and be limited by this receiver** —
which is the number the bench exists to measure.

### Setting the bench level: measure it, do not derive it

There is an optimum and it has a wall on each side. Too much level and
the converter clips; too little and the carrier sinks towards our own
floor, the LO leakage at the centre of the screen stops being 30 dB
down, and `rf_autorange()` correctly responds by using *less* gain
reduction, which amplifies our own noise along with everything else.

The first attempt at this derived a target from one premise — "enough
attenuation that 0 dB GR with the preamp on does not clip" — and the
premise was wrong: at 0 dB GR the baseband has 77 dB of gain and fills
the converter with **amplified thermal noise** whatever the signal does.
That operating point cannot be reached by attenuating, at any level, and
aiming for it over-attenuated by 27 dB.

So the procedure is: set a level, run `test_signal_quality()`, read the
plateau. **The right level is the one with the highest plateau**, and
the test takes a minute.
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

- **ONE reader. Not two.** Two `cat`s on the same port each get a
  random share of the bytes, and the log looks exactly like a baud
  rate error — readable words with characters missing — which sends
  you to `stty` and the cable. It happens by leaving a long capture
  running and starting another; `swd.sh` will happily reload
  underneath it. `pkill -f "cat /dev/ttyUSB0"` is not the answer
  either: that pattern matches the shell running it, so it kills
  itself. Anchor it (`-f '^timeout [0-9]+ cat /dev/ttyUSB0$'`,
  `-x -f 'cat /dev/ttyUSB0'`) or keep the loader in a script that
  does it once and correctly.

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
