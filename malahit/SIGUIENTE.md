# Where to pick this up

Rewritten 19/11/2026, updated 20/11/2026 (late at night). `WORKING.md` is still the file
to read first; this one only says what the next session should do and
what it must not trust.

## 20/11/2026, night: fast boot, buffered console, a crash that can be switched off — none of it in flash yet

**In git:** the fast boot (`SELFTEST`, `PORTING.md` step 18, commit
`77142f4`) and, after it, step 19: the buffered console (frame ~113 ms
-> **~80 ms, 12.5 fps**), the first-capture-after-arm fix, and the crash
path - `fault_park()` flushes the console and polls the power button,
`boot_standby()` clears pending exceptions so it really sleeps, and
`core_clean` clears a pending SysTick. All of it tested on the radio over
RAM; the crash path with a deliberate fault (a throwaway build, not in
the tree) and the owner's hand on the button. **Both ways of switching
off were checked to the end**, the normal 3 s hold and the same hold
after a crash: the backlight goes at 3 s, and after the release the
debug port is dead - the chip really unpowered, not just dark.

**Seen once, not chased:** a RAM load onto the OLD flash image while its
37 s test boot was running left the touch controller silent (`*** no
answer at 0x38 ***`), and the boot spent 6 s scanning the bus for it
(8.3 s instead of 2.5 s). Most likely the load caught the old image
mid-transfer with the FT6236; it did not happen on any load over the new
image. If it shows up after a cold boot of the new image, it is real.

**IN FLASH since 21/11/2026, ~00:15**: `build_flash/bringup.bin`,
`SELFTEST=0`, 96 960 bytes, sha256 `7b870abd…bcbd189`, written over DFU
WITHOUT `:leave`, read back (`-U`, same length) and identical byte for
byte, THEN left with a 4-byte `-s 0x08000000:4:leave -U` (its "get_status"
error is the device leaving, normal). It boots cold in ~2.3 s, touch
works. That write-verify-leave sequence is the one to repeat;
`tools/flash-dfu.sh` still writes with `:leave` and verifies nothing.

**OPEN, AND FIRST: AFTER SOME COLD STARTS THE RADIO RECEIVES NOTHING.**
Two cold starts out of three tonight, one with each firmware: `leak DC`
normal (~1 700), interrupts counting, audio running - and no carrier at
all: `station` 3-8, the peak wandering over noise, SNR 10-12 dB, ADC 7 %.
It is decided in the first second - `rf_autorange()` already sees no
carrier (`28 dB ... ADC at 31%` against `44 dB ... 41%` on a good start;
the boot log is otherwise identical) - and it lasts for minutes. **Any
re-run of the init cures it at once**: a RAM load, or the same flash
image started warm from the debugger.

| cold start, 20-21/11 | preceded by | off for | carrier |
|---|---|---|---|
| old flash image | the radio switching itself off (crash test) | minutes | **none** |
| new flash image | the image's own power-off right after DFU `:leave` | 1-2 min | **none** |
| new flash image | the owner's normal 3 s hold | seconds | yes |
| new flash image, several more | the owner's normal 3 s hold | various | yes, every time |

**A pattern, not a proof:** both deaf starts followed a power-off the
radio did BY ITSELF - the crash test's, and the image's own one straight
after DFU `:leave` - and none of the owner's ordinary 3 s holds has been
followed by one. If it is real, what differs is what the board looked
like at the moment the power went: a crashed or barely-started image
leaves the add-on board and the tuner in states an ordinary run never
does. The cheap test of that is a DFU `:leave` followed by a cold start,
twice - with the console recording from power-on.

**Ruled out:** the flash build (the same image started warm receives);
the add-on board powering up cold (simulated warm, once - but the fault
is intermittent, so once is weak); relay pulses on a warm board (three
injected, no effect). **Not ruled out:** the add-on's 595 losing the
filter code on a freshly powered board (a wrong code is NO signal, which
is what this looks like), the MSi001, the BPF/preamp GPIOs. The old
image also failed although its tests rewrite the tuner and the filter
dozens of times - which points at something written ONCE, early.

**How to catch it, prepared and tried:** when a cold start comes up
deaf, do NOT reload - that cures it and destroys the evidence. Instead:
1. Snapshot the state (GPIOE/GPIOD/GPIOA, SPI1, the `s_shadow` arrays of
   `msi001.c` and `nau8822.c` - `nm -l` tells the two apart) and compare
   with a good start. The good start's values, read tonight:
   `GPIOE_ODR 00005003`, `GPIOD_ODR 00000603`, msi001 shadow
   `000023e1 0000042c 00019d0d 00000852 00000000 00028fff 0 0`.
2. Then `tools/swd-call.sh build_flash/bringup.elf <step> [args]`, one
   init step at a time, reading the `baseband:` line after each:
   `msi001_retune 9988000`, `msi001_tune 9988000 0`, `addon_set_filter 2`
   (the highpass), `addon_set_att_halfdb 0`, `msi001_set_am_port 1`,
   `msi001_preamp 1`, `msi001_bpf <code>`. The first one after which the
   carrier appears is the step that does not take after power-up. The
   tool calls into the running image and it carries on - tried tonight.

**The OLD flash image did not receive the generator after a power-on,
20/11/2026 late.** `leak DC` normal (~1 680) and the block interrupt
counting, but `station` 4-8, the peak wandering over noise, SNR 10 dB;
a RAM load of the new image a minute later, nothing touched on the
bench, had it at full level (`adc 43%`, peak +12 000 Hz 124 dB), and the
owner heard and saw it. Its boot log was not captured, so its relay and
filter decisions are unknown. The new image did the same - see
the OPEN paragraph above.

**Two things about this bench, also from tonight:**
- **There is no watchdog.** Nothing in our firmware starts the IWDG and
  the option bytes leave it to software. Every note below that says "the
  watchdog resets it after 32 s" is about the factory firmware.
  Whether the radio WANTS one is a decision, not done: on this unit a
  watchdog reset is a power-off.
- **`station` in the `baseband:` line is a ±12 Hz probe.** It read ~20
  instead of ~900 tonight with the carrier at full level and the audio
  normal - most likely the generator a few tens of hertz off 10.000 MHz.
  It is not a reception test; `peak`, `adc` and the ear are.

**Three new RAM-load traps tonight, all fixed, all in `WORKING.md`** under
"LOADING OVER SWD RESETS NOTHING" - the old image's DMA, a halt inside
the block interrupt (`irq 0/0`, no audio), and a SysTick left pending by
a parked crash (the radio switches off on the next load). **If a RAM run
shows `irq 0/0`, or the radio dies on a load, start there.**

**Next, in order of what the radio gains:**

1. **The deaf cold start**, above - with the procedure ready, it needs
   only the next time it happens, and the console recording from power-on.
2. **The panel on SPI DMA** - "THE NEXT THING TO DO" in `WORKING.md`.
   Measured tonight: waterfall 48 ms and trace 17-26 ms of an ~80 ms
   frame - now nearly all of it.
3. **Remember the dial and the volume** across power-offs. Every boot
   starts on 10.000 MHz at -24 dB; there is an FM25W256 FRAM on the
   board (`BOARD.md` section 6) and no driver for it yet. The same FRAM
   could keep the crash record, which a power-off now loses.
4. The 100 kHz-step test for the 10 MHz digital noise (below), when the
   owner has the radio in hand.

## THE RADIO RUNS OUR FIRMWARE FROM FLASH — read this first

Since 20/11/2026 the factory firmware is NOT in flash: ours is, in bank
1 sectors 0–1, and it boots cold from the power button. The factory
image is backed up outside the repo (`Deep-Malahit/backups/`, README
there) and the round trip — factory restored, then ours again — has
been done once, over DFU, and verified. `WORKING.md`, "The way back",
has the table and the update procedure. In short:

- **Update**: `make flash` in `bringup/` (builds only), then switch on
  with BOTH encoder buttons held and
  `dfu-util -a 0 -s 0x08000000:leave -D build_flash/bringup.bin`.
- **Power**: short press on; hold more than 3 s off. After a DFU
  `:leave` it switches itself off — correct.
- **RAM loads still work** for development: switch on, then
  `swd.sh ram build/bringup.elf` as before. What they inherit is now
  OUR firmware's state, not the factory's — a different set of
  assumptions; keep that in mind before trusting one again.
- **THIS UNIT IS MODIFIED** (`BOARD.md` section 3): the MCU's own supply
  is latched by PC6 or the power button. Any reset is a power cut;
  "off" is no power at all; SWD is dead when off.
- **The boot is the bring-up image** in what is in flash today: 37 s
  of self-tests before the main screen. The fast boot is built and
  waiting to be written - see the section above.
- `make flash` writes nothing and never will; `swd.sh flash` uses
  `reset halt`, which does not work on this board — do not use it.

## The tuning arrangement changed again, and this one is modelled

The zero-IF build the previous session left untested was **replaced
before it was ever loaded**. At zero IF the AM carrier and the
oscillator's leakage are the same DC term, and `am.c`'s DC subtraction
removed both: `tools/ifmodel.py --zero-if` puts the 2nd harmonic 7 dB
above the tone with the carrier 0.5 Hz off, and the tone gone at 0 Hz.
The bench generator sits 2 250 Hz off the dial, so no bench test could
have shown it. `WORKING.md`, "Forward again, to a fixed 12 kHz", has the
whole story, including what the RE report does and does not say.

What is in the tree now:

- **The tuner is parked 12 kHz below the dial** (`AM_IF_HZ` in `am.h`),
  on every detent. No window, no re-park.
- **`am.c` subtracts the leakage at DC, then rotates by +12 kHz** with an
  exact 16-entry table. The sign is the one `am_probe()` measured for
  the old Fs/4 mixer, not a new derivation.
- **The panadapter shows 168 kHz centred on the dial** (bins [64, 512));
  the leakage line is 12 kHz left of centre.
- **The console's `baseband:` line** reads `leak DC / station / mirror`
  and flags it when the mirror is the loudest.

`python3 tools/ifmodel.py` models the new chain: clean from 0 to 100 Hz
of carrier error.

**Run on the radio twice, 19/11/2026, generator off:** boots, `Tuning :
dial 10000000 Hz, tuner 9988000 Hz`, 572 and 456 frames in the display
loop, no fault, stayed on both times. `leak DC` steady at 1 650-1 780
(the old figure was 1 712).

**Something is on 10.000 MHz.** `station` beat `mirror` in 52 of 65
lines on the first run and 46 of 52 on the second (medians 78/35 and
171/47, peaks to 625, three `<- carrier on the dial` verdicts) - a weak,
fading signal within about ±12 Hz of the dial. It was first written down
as noise, from glancing at a few lines; counting them said otherwise.

**It is the radio's own interference, and it settled the sign.** The
owner, same evening: nothing on the antenna; the stock firmware shows a
leak at 10 MHz too; in the headphones it is bursts of pulses, plainly
digital, most likely the panel or the touch controller; it fades
smoothly when the dial is moved off 10 MHz.

- **The rotation's sign is CONFIRMED on hardware**, without a
  generator, by three things that do not share an assumption: the
  stock firmware puts the leak at 10 MHz; `am_probe()` puts it on the
  station side; and the audio path plays it with the dial on 10 MHz.
  With the sign wrong the dial at 10 MHz would play 9.976 MHz, the
  10 MHz leak would sit 24 kHz outside the 4 kHz channel, and it would
  only be heard with the dial at 10.024 MHz.
- **It says NOTHING about the dial's absolute accuracy.** It is made on
  this board from this board's clocks, so it lands where the board's
  own reference puts it. The 2 250 Hz and `sai1.h`'s "X1 ... measured
  1500 ppm in step 1" (repeated in `PORTING.md`) stay open - and they
  do disagree with each other: 1 500 ppm is 15 kHz at 10 MHz.
- It is also the zero-IF failure case in real life - a carrier within
  hertz of the dial - and it demodulates. "Audible" is all that says;
  a pulse train is no test of distortion.

**How it gets in - measured by the owner, 19/11/2026: bonding the
antenna socket's ground to the case makes the digital noise go away.**
So it arrives through the connector's ground, common-mode, rather than
as a field on the input. A proper bond is a hardware job and the owner
is designing one ("not a snotty wire"); until it exists, the 10 MHz
burst noise is a property of the bench, not of the firmware.

**Where it comes from - a guess with arithmetic, not a measurement.**
The panel's SPI runs at 30 MHz with 3 bytes a pixel: 30 MHz / 24 bits
is a 1.25 MHz pixel rate, and its 8th harmonic is exactly 10.000 MHz.
The trace and the waterfall go out for 38 + 41 ms of every 120 ms
frame, which would be the bursts. The touch controller's I²C is the
other candidate, though a weak one: it runs at **100 kHz** (`i2c3.c`,
standard mode on purpose), so 10 MHz would be its 100th harmonic, from
slow open-drain edges. **The test that separates them** is the knob,
100 kHz step: a 1.25 MHz comb is also at 8.75 and 11.25 MHz and not at
10.1; a 100 kHz comb is at 10.1 and not at 11.25. If it is the SPI comb
it is in every 1.25 MHz across HF, and that is worth knowing before
anybody calls a band deaf.

### What to check the first time the generator is connected

Item 1 is already settled by the 10 MHz leak (above); it stays here as
the check to repeat if `am.c`'s rotation or the slot order is ever
touched.

1. `baseband:` - the line ends `<- carrier on the dial` when `station`
   is over 400 and four times `mirror`. **The leakage (`leak DC`, ~1 700)
   is normally the largest of the three and that is fine** - at the
   bench level the carrier read ~715. If the line says
   `MIRROR LOUDEST`, the rotation turns the wrong way: flip the sign of
   `k_rot_s` in `am.c` (and nothing else - the probe uses the same
   table, so it flips with it). The 400 floor was measured once, on
   noise; if a real carrier reads below it there is no verdict, and
   the two numbers have to be compared by eye.
2. The carrier sits under the centre mark of the trace, the leakage
   line 12 kHz to its left, and the ruler reads the dial frequency in
   the middle.
3. **The test the bench could not do before:** set the 10 Hz step and
   tune across the carrier in single detents, through the point where
   the readout matches the generator. The tone must stay clean at
   every step. The generator's 2 250 Hz error means "the readout
   matches" is not "zero beat"; tune until the console's peak sits at
   `+12000 Hz` from the tuner.

## Noticed on the first run, not chased

**The frame is 120 ms (8 fps), and the trace alone is 38 ms.** WORKING.md
records 91 ms and 16 ms from 05/11. Not attributed: nothing between
then and now ran on the radio, so there is no baseline to compare with.
One piece of arithmetic worth having: a full repaint of the 440 x 120
trace at 18 bpp is 158 kB, which at 30 MHz SPI is 42 ms - so 38 ms is
nearly the whole trace going out.

**It depends on the picture, and that is upstream's design.** An
earlier version of this note said `spectrum_draw()` pushes the whole
rectangle "whatever the signal"; it does not. It works out each
column's CEILING - the highest row the trace, fill or peak reaches -
and skips the bands of rows above it, which are only the unchanged
background and grid (`User/spectrum.c`, "LAS BANDAS QUE NO HACE FALTA
..."). Measured 20/11/2026 on the same image: **7.7 ms** with the preamp
off and the ADC at 6 %, against **38 ms** with it on and a noisy floor
at 58 %. A high noise floor fills the trace and costs a full repaint;
16 ms on 05/11 is a picture in between. Not a regression, and the
waterfall's 41 ms - which has no such skipping - is the bigger item.
Either way it is the SPI DMA's job (WORKING.md, "THE NEXT THING TO
DO").

## The radio switching itself off: what was ruled out, and what the modification explains

**Half-explained, 20/11/2026**: on this unit any reset is a power cut —
the reset drops PC6 and PC6 is what keeps the MCU powered (`BOARD.md`
section 3). So "it switched itself off" means "something reset it", and
the candidates are the ones below: a fault (the handler spins, the
watchdog bites 32 s later), the watchdog, a brown-out. What reset it is
still not known. The notes below are from 19/11 and still stand:

The radio was **on** at the start of this session. Read before
concluding anything from an empty log:

- **A debugger halt does not cause it.** `DBGMCU_APB4FZ1` reads
  `0x000C0000`: both independent watchdogs freeze while the core is
  halted. (`swd.sh id` halts the core - `whoami` starts with `halt` -
  and that is safe.)
- **Nothing in this firmware drives `PC6` low.** The GD32 shim only
  touches masked pins; test 12b restores `GPIOC->MODER` with PC6 still
  an output.
- **No crash record** at `0x2407FF00`, and `RCC_RSR` read `0x00FE0000`
  - the cold-power-on pattern, no `IWDG1RSTF`. The factory firmware's
  standby/wake cycle runs between our image dying and the next read, so
  this cannot name the cause after the fact.
- **The fault handler spins forever and blocks SysTick**, so any crash
  ends as a watchdog reset 32 s later - which, on this board, is a
  power-off. That fits the symptom and is not proven.

What would settle it is a breadcrumb that survives standby: an RTC
backup register or backup SRAM, written by our firmware once a second
with the phase marker and uptime, read over SWD after the next
unexplained power-off. **Neither region is known to be free.** A literal
search of `dsp1_dump.bin` found the RTC base `0x58004000` next to
`PWR_CR1` three times (`0x201e8`, `0x39274`, `0x3934c` - the backup
domain unlocked, then the RTC set up), and two word-aligned literals
into backup SRAM, `0x38800298` and `0x38800518`. Backup registers are
reached as base + offset, so the absence of a `0x580040xx` literal
proves nothing. Disassemble those three sites before writing a byte
there - the factory firmware may keep its clock, or a DFU flag, in
them.

## The button thresholds: done, 20/11/2026

Remeasured with `PMCR` written, off the display loop's `buttons:` line:
big knob 38 924, small 20 855, both 17 091 - **the schematic's
47k/68k/22k to half a per cent**. The 05/11 table that said "the
resistors are not what is fitted" was measured through the closed `PA1`
switch; with its thresholds the big knob read as the small one and the
small one as both. New midpoints in `adc.c`, and verified on the radio
the same day: classes 1, 2, 3 for big, small, both; short big cycles the
step, short small swaps volume and RF gain, long small toggles the
preamp - by the log and by the owner's eye. `BOARD.md` §6 has the table.

A press of BOTH is delivered to both knobs at once (upstream's encoder.c
runs per knob), so it cycles the step and swaps the small knob's job
together, and a long one toggles the preamp. Nothing is bound to "both"
as a gesture of its own; if something ever is, it has to swallow the two
single presses it currently produces.

## Still not finished

**`test_adc(1)` hangs.** Not diagnosed, worked around. It printed its
first heartbeat and then neither looped nor returned. Given the list
above, "hung" may have been a power-off.

## What NOT to trust

- Any conclusion drawn from a log that ends mid-line.
- The "Back to zero IF" section of `WORKING.md`. It is kept for its
  reasoning and marked superseded; its premise was a misreading.
- `ticks 0` was real, but the reasoning built on top of it ("the boot
  is fifteen times too slow") was speculation.
- The claim that the watchdog being fed proves SysTick is running.
  Several polled loops feed it themselves.

## The AGC: measured, and left alone

The plan was to smooth upstream's instant attack so `am_resettle()` could
go. **Measured first** (`WORKING.md`, "The retune transient, measured"),
and the premise did not hold: a retune inside the channel costs ~2 dB
for under 100 ms with no mute at all, and **the mute made it worse** -
85 ms of silence and then a 6.5 dB dip for 400 ms, because it reset the
DC blocker and the carrier came back as a step from zero.

So, as of 19/11/2026:

- **The mute is gone, by ear too.** The owner: "the second of quiet
  after a retune has gone - much better than it was." `am_resettle()`,
  the fade and `s_settle` are deleted.
- **The click that remained is repaired** - `WORKING.md`, "The retune
  click, found and repaired". The MSi001 relocks for 0.2-0.3 ms after
  every synthesiser write; `am_retune_note()` pins the write to the
  sample and the hook holds the carrier level across it, fading only
  the modulation (2.7 ms in all). Measured on the radio: the click's
  energy above 2 kHz down to -22 dB re the tone on four moves of four,
  and the AGC dip on ±1 kHz gone (0.0-0.3 dB). **Heard by the owner,
  same evening:** on the generator's steady tone the clicks are still
  audible but no longer sharp - duller; tuned off the signal, on noise
  alone, no retune click is heard at all. "Tolerable, it will do." What
  is left on the tone is the planned 2 ms gap in the modulation, not
  the spike. If it ever needs to go further, the window is three
  constants in `am.h` (`AM_HOLD_*`), and `tools/clickmodel.py` scores
  any change on the four captured glitches before it goes near the
  radio: shorter windows trade gap for click (the model: [16, 224) with
  a 96-sample ramp gives -16 dB). Filling the gap with a prediction of
  the modulation, rather than fading it, is the step after that.
- **Still there: tuning ONTO a carrier**, 2 dB for 90 ms. That is the DC
  blocker meeting a carrier that just appeared, not the synthesiser,
  and the click repair does not touch it. Seeding the blocker with the
  new carrier level after the hold would - but `am_resettle()` died of
  resetting that blocker to the wrong value, so measure first.
- **Upstream's `agc.c` is untouched**, and should stay that way unless
  something new is measured. Changing it would change the DEEPSDR too,
  and `deepsdr-ui/sim/agc_test.c` compares it sample by sample with a
  textbook AGC.
- **`RETUNE_TEST` in `am.h`** reruns the measurement: 1 for the table,
  2 for the fine and raw dumps (`tools/clickdump.py <log> --png`,
  `tools/clickmodel.py <log>`). It takes the dial for a minute after
  boot. Off by default, and at 0 its buffers give back ~35 kB.

What could still be worth doing, and is NOT measured yet: the instant
attack also latches on **impulsive noise** - the 10 MHz bursts above
are exactly that - and ducks the audio after each one. The stock
firmware has a noise blanker ahead of its detector for this (RE report
2.4 item 4). If that is audible once the antenna ground is bonded, a
blanker is the answer, not the AGC's attack.

## The generator, today

**Its carrier sat within ±12 Hz of the dial at 10.000 MHz** (`station
889, mirror 5`; at 10.001 MHz the probe saw the upper sideband at 0.35
of it, exactly 70 % AM). The "repeatable 2 250 Hz" in `WORKING.md` did
not reproduce, and **the owner had not touched the generator** - so it
was on the radio's side, most likely the arithmetic of the old
arrangement. Not verified. It also makes `sai1.h`'s "X1 ... 1500 ppm"
harder to believe: 1 500 ppm is 15 kHz at 10 MHz, unless the tuner has
a reference of its own.

## In git

Everything up to the click repair is committed on branch `malahit`
(19/11/2026, the commit after `cda75be`) - the SAI on DMA, both knobs,
the ADC, the add-on board, AM, the 12 kHz IF and the retune work. Not
pushed. Before committing anything in `User/`, the checks in
`WORKING.md` ("Regression checks"); for `User/encoder.c` the commit
also compared the DEEPSDR's `encoder.o` disassembly before and after -
identical - which is the stronger check for a file both boards compile.
