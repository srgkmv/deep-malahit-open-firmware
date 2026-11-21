# Where to pick this up

Rewritten 19/11/2026, updated through 21/11/2026 - start at "START
HERE". `WORKING.md` is still the file to read first; this one only
says what the next session should do and what it must not trust.

## START HERE — 21/11/2026, night: steps 8, 9 and 28 in git and IN FLASH

**In flash: `93eaba6` since later the same night - see below.** Before
that, `4d9848a` - `build_flash/bringup.bin`, 153 544 bytes,
sha256 `deb595dd…`, SELFTEST 0, clean build; written over DFU without
`:leave`, read back identical byte for byte, left with the 4-byte
`:leave` upload. **Cold boot**: settings from the FRAM (10 MHz, -46 dB,
1 kHz step, ATT 59), touch and add-on board up, upstream's demodulator
playing, screen and audio 2019 ms after SysTick, SAI DMA err 0, no
overrun.

**One thing to watch, not a fault so far:** after the cold boot
`baseband: station` started at 9 and climbed slowly (21 at one minute,
23 at 72 s) where every warm RAM run today read 28, while the spectrum
peak read 94 dB throughout. `station` is a correlation at EXACTLY
+12 kHz over a 42.7 ms block, so it falls off for a carrier only a few
hertz away - 1 ppm of the 22 MHz reference is 10 Hz at 10 MHz - and the
reference was cold. **It settled: 27 by five minutes and flat after**,
DMA err 0 and no overrun throughout, and the owner's taps on Ruido and
Ajustes all landed. The deaf cold start (`station` 3-8, FLAT until a
re-init) looked different. If a cold boot ever reads low and STAYS low,
that is the deaf start again.

Step 9, from `PORTING.md` step 27, in short:

- **Bandas, Modo, Paso** open upstream's own screens (`ui_cfg.c`,
  `ui_grid.c`) at 480 x 320, laid out on the PC first (deepsdr-ui
  `make malahit-menus`, 0 labels overflowing) and then seen ON THE GLASS.
  Choosing tunes / switches mode / sets the step and closes the screen.
  Ruido = upstream's NR on/off, Volumen = small knob back to volume.
  Big knob in a menu = cursor, its press = choose. AGC and BW chips
  cycle; the band pill opens the band screen. Upstream's demodulator
  boots (am.c still in the image: `demod_path 0` for the click A/B).
- **The bench can now see the panel and touch it**: `tools/grab.py`
  (the panel's GRAM over SPI3 MISO, 9 s a screenshot) and
  `swd-call ... ui_tap x y`. Negative swd-call arguments must be given
  as `0xFFFFFFFE`, not `-2` - OpenOCD silently does not set the register.
- Upstream edits: `#ifndef` geometry + named text offsets in
  `ui_grid.h/.c`, `ui_cfg.h/.c`; nine DEEPSDR renders byte-identical
  before/after (`PROCEDENCIA.md`).

**Memory: the SAI rings went to D2 SRAM** (`PORTING.md` step 28): RAM
image 484 -> 353 kB of AXI, nothing slower, DMA errors 0. D2 still has
160 kB free (SRAM2 half, SRAM3) and the DTCM 50 kB. Still cheap:
`ui_top.o` carries its own static copies of font_ui_10/10b (8 kB) -
`src/fonts_menu.c` shows how to drop them.

**Flash is behind:** it holds `41a0523`, before steps 8-9 and the
rings. A RAM image loaded over it inherits that image's DMA writing
into AXI at the OLD ring addresses until our sai1 re-arms - harmless so
far, and gone once the flash carries the D2 layout too (both images
then put the rings at the same D2 address). Writing flash is the
owner's DFU, two button presses.

**Done since (`PORTING.md` step 29) and IN FLASH: `93eaba6`** -
154 856 bytes, sha256 `0e05b56c…`, clean build, written over
DFU, read back identical, left with the 4-byte `:leave`. Cold boot: the
v3 record read (slot 1, seq 74), "Mode: AM, from the FRAM", screen and
audio 2053 ms, DMA err 0. Mode kept in the FRAM (record v3), upstream's
eleven steps generated from its main.c, a band's own step on choosing
it, the band pill following the dial. Firmware older than 93eaba6
cannot read a v3 record and boots on defaults.

**Ajustes works and is kept in the FRAM** (`PORTING.md` steps 30-31,
from RAM): 13 settings on upstream's three pages, the 18 themes
generated from upstream, stored as an extension of the v3 record that
the flash's older firmware does not see - checked on the radio by
loading 93eaba6 over it.

**ui_det.c too** (`PORTING.md` step 32): Silenciador, Reducción
(strength), Volumen and Atenuador on upstream's detail page, squelch and
NR strength in the FRAM. The NR strength had never been set (0 = bypass)
- the Ruido button did nothing until this; 48 now.

**Open in step 9:** smoothing (the third continuous one upstream has -
the port's waterfall/trace smoothing API first); the frequency keypad (`ui_kbd.c`, tap on
the frequency); WFM (upstream's WFM wants the station at 0 Hz, this port
keeps it 12 kHz off the LO).

## 21/11/2026, evening: step 8 started, upstream's modes run

**What is in flash is still `41a0523`** (sha `214e3e8b…`). What is new
was checked from RAM only: `PORTING.md` step 26.

- **The census was the answer, and it was small.** `demod_am.c`'s
  106-file closure is the `#include` one; to LINK it needs ten modules
  and 52 decoder hooks answering "off" - exactly what upstream's host
  bench stubs (`deepsdr-ui`, `make demod`, `sim/demod_stubs.c`). On this
  port's flags: ten files clean, four errors in `demod_am.c` on one
  spot (speaker pin, PB7 there, PE1 here: an `#ifndef` upstream,
  GD32 disassembly identical before and after, `PROCEDENCIA.md`).
- **Memory, settled without the owner's buttons:** upstream's `.tcmram`
  is CPU-only by upstream's own rule (the GD32's TCM has no DMA either),
  so it now lives in the DTCM, NOLOAD, zeroed by the startup (all six
  objects' sections checked to be zeros). RAM image AXI 439 -> 409 kB
  before the graft, **460 / 512 kB with it**, DTCM 32 / 128 kB. The SWD
  RAM loop survives. Next lever when a decoder needs it: the 128 kB of
  SAI rings into D2 SRAM.
- **The graft:** `demod_am.c`, `semibanda.c`, `nco.c`, `nco_tabla.c`,
  `nb.c`, `anotch.c`, `nr_ss.c`, `sam.c`, `rapido.c`, `bip.c` in place;
  `src/demod.c` cuts each 8192-frame half into 16 upstream calls of 512;
  `src/decoders_off.c` the stubs, type-checked against upstream's
  headers. **am.c still boots**; upstream is switched in the running
  radio: `swd-call.sh build/bringup.elf demod_path 1|0`,
  `demod_mode 0..5` (no WFM), `demod_mix <hz>`, `demod_report`.
- **On the radio, by the owner: every mode checked, "seems all fine"**
  - by ear, on the generator; no measurement of the audio yet. So the
  mix sign (+12000 Hz, predicted from the I/Q order) is right: wrong,
  AM would have been silent on the dial.
  `demod_report` in LSB: hook **11.9 ms of 42.7** (am.c's AM: 9.0),
  worst 808 us per call, SAI overruns 0, sink errors 0, silent calls 0.
  Frame 42.7 ms against 47.5 at the panel-DMA step, with MORE in the
  interrupt - the DTCM is the likely reason and was NOT A/B'd.

**Open, for the next session:**

1. Commit (the owner asks for it), then decide whether upstream becomes
   the boot path. Before that, by ear: the **retune click** - am.c has
   the predicted fill (step 25), upstream's path has none, so tuning
   with upstream on may click again.
2. No mode UI yet: the mode is set over SWD only. The mode chip and
   the buttons are step 9 territory.
3. WFM is a separate upstream hook at another block size - not wired.
4. `swd.sh id` HALTS the core (`whoami` starts with `halt`) and does not
   resume. Run on the radio this session, the console was silent after
   it - whether that was the cause was not established. Resume by hand,
   or give `whoami` a `resume`.

## The session before: end of 21/11/2026, afternoon — the plan for step 8

**What is in flash is exactly `41a0523`** (sha `214e3e8b…`, 104 960
bytes), verified by read-back and a cold boot. Nothing is open from
this session. What it did, each with its own section below and its own
step in `PORTING.md`:

| step | what | the number |
|---|---|---|
| 21 | the panel on DMA, blocks chained from SPI3's EOT interrupt | frame ~80 -> ~47 ms |
| 22 | the touch controller left alone 300 ms after its reset | lost 2/20 -> 0/20 |
| 23 | dial, volume, step kept in the FM25W256 FRAM (checked to BE FRAM first; factory content backed up) | cold boot restores them |
| 24 | ATT and preamp kept too; the dial no longer loses 1 Hz a retune | 14 993/29 990 dials off -> 0 |
| 25 | the retune click filled with the modulation's own period instead of faded | -7.0 -> -47.9 dB rms |

**The next session: PORTING.md step 8, the upstream DSP tree** - the
modes. The owner chose it, 21/11/2026. The radio is AM-only today, on
this port's own `am.c` and a scaffold `main.c`. What is known already
(`PORTING.md`, the box under step 7, and §3.3):

- `demod_am.c` is the hub of every mode, and its transitive closure is
  **106 `.c` files, 72 126 lines**; upstream has no switch to leave
  modes out, so it comes as essentially all of `User/` at once. 12 of
  the files touch GD32 registers (`lcd_dma.c` - already replaced by
  `compat/` -, `ipa_blit.c` -> DMA2D, `spi_flash.c`, ...).
- Storage is decided: **build without the disk** - no SPI NOR, no SD on
  this board - so maps, the station database, screenshots and USB mass
  storage are off. Settings already live in the FRAM (`settings.c`).
- Upstream's `main.c` (39 094 lines) replaces the scaffold loop and
  brings the band table, menus, keypad and FT8 screen with it - the rest
  of step 9.

**Start with a census, not with code**: compile that closure against
this port - storage and USB stubbed - and let the compiler and the
linker write the work list (undefined symbols, GD32 registers, missing
headers), sorted by file. That turns 72 000 lines into a number.

**And settle MEMORY before the census can mislead.** Today:

| image | AXI SRAM (512 kB) |
|---|---|
| flash image, `.data` + `.bss` | 335 kB |
| RAM image (code too), `make` | 439 kB |
| RAM image, `SELFTEST=1` | 98 %, 8 kB stack included |

72 000 lines of code will not fit a RAM image that also carries the
code, so **the SWD RAM-load loop - the whole working method of this
port, `WORKING.md` "The loop" - stops working the day step 8 links**,
unless the image is split: code in AXI, data elsewhere. Unused today:
**DTCM 128 kB** (CPU only - right for DSP state, wrong for any DMA
buffer: `spi3.c`'s `dma_reachable()` and `sai1.h` say why), **D2 SRAM1-3
288 kB** (DMA-reachable; needs its RCC clock bits), SRAM4 64 kB in D3.
The other way out - every test a DFU write - costs the owner two button
presses a load; decide with the owner, not by default.

## 21/11/2026, night: the retune click — predicted, not faded — in git and in flash

**In git and in flash.** `build_flash/bringup.bin`, 104 960 bytes,
sha256 `214e3e8b…`, written over DFU, read back identical, left.
`PORTING.md` step 25, `WORKING.md` "The retune click, part two".

The dull click on every retune with a signal on the dial was the 19/11
hold's own hole in the modulation. The window is now filled with the
envelope's last period (`am.c`, `pred_period()`); measured on the
codec's output, A/B interleaved in one run: -7.0 -> -47.9 dB rms re the
tone, >2 kHz -20 -> -50 dB. The owner listened to the RAM image: "Всё
хорошо".

**The 10 MHz digital noise is the owner's, and parked.** The owner, the
same day: the SMA socket sits loose in the home-made aluminium case, not
fixed with its nuts, so its ground does not reach the case - the bond
SIGUIENTE already said makes the noise go. A hardware fix, the owner's
to make, later. Nothing in the firmware to chase until then.

**Open from this:**
- The prediction was measured on a pure 1 kHz tone, its best case. On
  speech and music it was only listened to - no numbers. If a retune on
  the air ever sounds like a stutter, `RETUNE_TEST` 2 and
  `tools/clickaudio.py` on a broadcast station are the instrument.
- `SELFTEST=1` in RAM is at 98 % of AXI SRAM. The next buffer that
  grows wants the 128 kB of DTCM - CPU-only memory, and the envelope
  history is CPU-only - rather than another squeeze.

## 21/11/2026, late: ATT and preamp remembered too, and the dial keeps its hertz — in git and in flash

**In git and in flash.** `build_flash/bringup.bin`, 103 728 bytes,
sha256 `b9f9e4da…`, written over DFU, read back identical, left.
`PORTING.md` step 24.

- **ATT and preamp in the FRAM** (the owner asked): the settings record
  is version 2, 24 bytes - the gain reduction the ATT pill shows and
  the preamp added. When a record has them, the boot restores them and
  `rf_autorange()` does not run; it runs only with nothing to restore.
  Version-1 records still load (dial, volume, step) - checked on the
  radio before the first v2 save.
- **The dial no longer loses a hertz a retune.** `msi001_plan()` now
  rounds `f_actual` instead of truncating it. `MSI_ROUND_TEST`, on the
  radio, arithmetic only: before, 14 993 of 29 990 dials across HF read
  1 Hz under their request and 1 000 detents of +1 kHz from 10 MHz ended
  at 10 999 541; after, 0 of 29 990, and every walk - 1 kHz, 100 Hz,
  10 Hz, up and back - ends exactly where it should.
- **What the bug left in the FRAM is cleaned at load**: a stored dial
  off the 10 Hz grid (the finest step) is rounded onto it, with a line
  on the console. The owner's 9 000 003 came back as 9 000 000.

**Cold boot from flash**: ATT 59 dB, preamp OFF "from the FRAM ...
rf_autorange() not run"; dial 9 000 000 (rounded from 9 000 003),
-29 dB, 10 kHz step; record v2, seq 30. Boot **1 978 ms** - the
autorange's captures were most of the difference from ~2.65 s.

## 21/11/2026, evening: the radio remembers the dial, the volume and the step — in git and in flash

**In git and in flash.** `build_flash/bringup.bin`, 102 912 bytes,
sha256 `b74abb2c…`, written over DFU, read back identical, left. The
first cold boot from it came up on the stored 9 990 093 Hz, -31 dB,
100 Hz (slot 1, seq 10); touch found, no I2C3 line, boot 2 763 ms.
`PORTING.md` step 23, `BOARD.md` "FM25W256 — FRAM", `settings.h`.

- **D11 is FRAM**, identified by behaviour before anything was written
  (the owner asked for that first): no busy bit and no page wrap on a
  96-byte write across a 64-byte page line, no RDID, 32 kB. Test image
  `FRAM_TEST=1|2`, built into a scratch `BUILD_DIR`, never `build/`.
- **Its 32 kB went to `Deep-Malahit/backups/fram_a.bin`/`fram_b.bin`
  first** - it holds the factory firmware's settings (0x0000-0x0CFF,
  0x7000-0x7FFF). Ours are at 0x4000-0x403F; a full read afterwards
  differed from the backup in those 40 bytes only.
- **On the radio:** blank FRAM -> defaults; the owner tuned, changed
  volume and step (each saved a second later, slots alternating, seq
  1..10), held power off, switched on; the next RAM load came up on
  9 990 093 Hz, -31 dB, 100 Hz - the last save, exactly.

**Found on the way - FIXED later the same evening (section above): the
dial lost up to 1 Hz per retune.**
The saves show it: 10 000 000 Hz in 1 kHz steps became 9 990 997, then
in 100 Hz steps 9 990 093. `msi001_plan()` reports `f_actual` by
integer division - truncated - and `dial_tune()` takes the next dial
from `s_tuned_hz`, which is that truncated figure plus the IF. The
synthesiser's step is ~0.33 Hz, so rounding `f_actual` to the nearest
hertz instead (half the divisor added before the division) should make
round dials stay round. One line, but in the tuner's arithmetic: check
`err_hz` and the 10 Hz step across the generator after changing it.

## 21/11/2026, later: the touch at power-on — measured, fixed, in git and in flash

**In git and in flash.** `build_flash/bringup.bin`, 100 576 bytes,
sha256 `c10ada62…`, written over DFU ~12:27, read back identical, left
with the 4-byte `:leave`. `PORTING.md` step 22, `BOARD.md` "Touch —
FT6236".

**Three cold boots after the write, all alike:** no `I2C3 : bus
recovered` line (the AFR-before-MODER fix: the recovery now needs 0
clocks at a power-on), touch found, boot **2 753 ms** each time. Read
over SWD after the last: 1 identify read, SDA never low, 0 recoveries,
1 attempt.

The owner did three more cold boots of `deba396`: boot times 8 479,
2 647 and 2 647 ms. The first of them: `*** no answer at 0x38 ***`,
touch dead for the session, SDA stuck from the 6th read. With the
first one that makes two bad power-ons of four, both stuck at read 5-6.
And `I2C3 : bus recovered, -1 clocks` on all four.

`test_touch_wake()` (`TOUCH_WAKE_TEST=n`, off by default) reproduced it
warm and separated the fixes - 20 resets each, interleaved:

| after the reset | not found | SDA stuck | mean |
|---|---|---|---|
| 120 ms, no recovery (deba396) | 2 / 20 | 3 / 20 | 629 ms |
| 120 ms + recovery in the poll | 0 / 20 | 3 / 20 | 184 ms |
| 300 ms + recovery (the fix) | 0 / 20 | 0 / 20 | 311 ms |

Built for that run with `make BUILD_DIR=<scratch> OPT="-O2 -g3
-DTOUCH_WAKE_TEST=20"` and loaded to RAM - a test image never lands in
`build/` or `build_flash/`.

Three cold boots are three; the bad rate on the old code was two in
four cold and two in twenty warm, so a bad boot from here on would mean
a second cause - read the touch counters over SWD (`s_dbg_*` in
`touch.c`, addresses from `build_flash/bringup.elf`) before reloading.

## 21/11/2026, afternoon: the panel on DMA — frame ~80 -> ~47 ms, in git and in flash

**In git and in flash.** `build_flash/bringup.bin`, 100 392 bytes,
sha256 `6f991a4e…`, written over DFU ~12:13, read back identical byte
for byte, left with the 4-byte `:leave` upload (see below).

The panel's pixels go by DMA now (`PORTING.md` step 21; `WORKING.md`,
"The panel on DMA, and why the queue runs from an interrupt"): a queue
of eight 4-row slots, DMA1 stream 2 on request 62, each next block
started by SPI3's end-of-transfer interrupt at priority 0, above the AM
block interrupt. Measured on one run, the switch flipped over SWD
(`swd-call.sh build/bringup.elf ili9488_dma_enable 0|1`), generator on
the dial: **frame median 79.9 ms polled -> 47.5 ms (21 fps)**, waterfall
48.3 -> 30.2 ms. A first version that started blocks from the display
loop stopped at 65.5 ms, and why it stopped is the useful part: the
8.4 ms AM interrupt idles any bus the CPU has to restart.

Seen on the radio: self-test 256 B in 69 us (68.3 us of bit times),
118 455 transfers, 0 failed, 0 stalls, `dry 0`, no block overrun, the
carrier at full level throughout. FEIF on every transfer - explained
as far as it can be without a test, harmless, `WORKING.md` says why.

**Checked by the owner, same afternoon, from RAM:** the picture is the
same with and without the DMA - and with it **the waterfall is much
smoother; without it there was a flicker, with it it is clean**. The
audio is unchanged. The 10 MHz digital noise was not noticeable (it
had been expected to sound steadier, not louder).

**The first cold boot of this image from flash, ~12:15: it works, and
it took 4.46 s instead of 2.7.** Panel DMA on, probe 70 us, the carrier
at full level, transfers clean. The extra 1.8 s is the TOUCH, not the
panel - read over SWD from the running radio afterwards, without a
halt (`swd.sh -c init -c "mdh <addr> 1"`, addresses from
`build_flash/bringup.elf`):

| `touch.c` / `i2c3.c` | read | meaning |
|---|---|---|
| `s_recover_clocks` | -1 | the recovery before the reset: SDA never came back |
| `s_dbg_after_reset` | 0x03 | both lines high after the reset pulse |
| `s_dbg_sda_low_at` | 5 | SDA found low from the 5th identify read on |
| `s_dbg_rc_at` | 0 | ... and that read had failed |
| `s_dbg_tries` | 66 | the first identify ran all 60 tries; the second, after recovering the bus again, got it on its 6th |

The console showed only `I2C3 : bus recovered, -1 clocks` - the
identify's counters print only when BOTH attempts fail. **This is the
"touch controller silent" seen once before (section below), now at a
real power-on - so by that note's own rule it is real.** Nothing links
it to the DMA that can be seen: another bus (I2C3, PA8/PC9), no shared
pins, and the queue's interrupt only writes CS and DC through BSRR.
One cold boot is one sample; two or three more, watching the boot time
and that console line, say whether it is every cold start or some.
`touch_init()`'s second attempt is what saved this one.

**Not run:** `make SELFTEST=1` builds (93.96 % of AXI SRAM in the RAM image); it
  has not been run.

**Next, in order of what the radio gains:**
1. ~~**The touch at power-on**~~ - measured, fixed and cold-booted, see
   the section above.
2. ~~**Remember the dial and the volume**~~ - done that evening, see the
   top section. (Was:) across power-offs - FM25W256
   FRAM on SPI2, `BOARD.md` section 6, no driver yet. The same FRAM
   could keep the crash record.
3. ~~**The dial's 1 Hz per retune**~~ - fixed, see the top section.
4. The 100 kHz-step test for the 10 MHz digital noise (below).
5. The frame is now near the bus: one waterfall alone is 32.4 ms of
   wire. Going faster means fewer bytes (the waterfall shorter, or
   redrawn less often than the trace) or a faster SPI clock than 30 MHz
   - and 30 MHz is already 1.5 times the datasheet's (ILI9488 17.4.3,
   4-line SPI: `twc` 50 ns minimum, 20 MHz), and a faster clock moves
   the digital-noise comb.

## 21/11/2026: in flash and verified — fast boot, buffered console, a crash that can be switched off, no more deaf cold starts

**In git and in flash:** the fast boot (`SELFTEST`, `PORTING.md` step
18), step 20 - the deaf cold start, below - and step 19: the buffered console (frame ~113 ms
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

**How the flash is written now** (first on 21/11/2026, ~00:15; the
current image is in the next section): over DFU WITHOUT `:leave`, read back (`-U`, same length) and identical byte for
byte, THEN left with a 4-byte `-s 0x08000000:4:leave -U` (its "get_status"
error is the device leaving, normal). It boots cold in ~2.7 s (2.3 s
before the add-on board's 200 ms), touch works. That write-verify-leave sequence is the one to repeat;
`tools/flash-dfu.sh` still writes with `:leave` and verifies nothing.

**FOUND AND FIXED 21/11/2026, IN FLASH AND VERIFIED: the deaf cold
start.** Some cold starts came up with no carrier at all (`leak DC`
normal, interrupts counting, `station` 3-8, ADC ~7 %) until anything
re-ran the init. Caught live the morning after - the owner switched on
cold, it was deaf, and nobody reloaded:

1. A snapshot of GPIO, SPI1 and the clocks matched a good start exactly
   (PA15 differed: it is the panel's CS, caught mid-transfer).
2. ONE call over SWD, `tools/swd-call.sh build_flash/bringup.elf
   addon_set_filter 2`, and the carrier was back - console, screen and
   the owner's ear. The add-on board's 74HC595 had lost the filter code.
3. `test_addon_cold()` (`main.c`, `ADDON_COLD_TEST`) then powered the
   add-on board down and up 96 times with the MCU warm. The relay is
   innocent; it is TIME: writes 10-20 ms after `Vext_ON` are lost every
   time, from 25 ms never. `BOARD.md` section 7 has the table.

`addon_init()` waited 10 ms; it now waits `ADDON_RAIL_MS` = 200 ms when
it finds the board unpowered. The boot's real writes had landed at
~15-25 ms - pushed there by console lines - which is why it came and
went, and why a cold morning found it.

**Verified from flash, 21/11/2026 ~11:20**: `build_flash/bringup.bin`,
96 968 bytes, sha256 `5b7cb243…`, written, read back identical, left.
Four cold starts in the console log (the owner counted about five, with
pauses between them), the first straight after DFU `:leave` - the very
case that had come up deaf the night before - and every one with the
carrier at full level: autorange 44 dB at 41-44 % ADC, peak +12 000 Hz
124-125 dB. Boot 2.68-2.70 s, the 0.2 s of the new wait included. A
deaf start from here on would mean a second cause. The old flash
image's deafness, which this does not obviously explain (its tests
rewrote the filter seconds after power-up), is moot with that image
gone.

The same log is a reminder about `baseband:`'s verdict: two of the four
starts read `station` ~200-260, under its 400 threshold, so no
`<- carrier on the dial`, with the carrier at full level and heard. A
±12 Hz probe follows the generator's drift; `peak`, `adc` and the
autorange are the reception test.

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

1. ~~**The panel on SPI DMA**~~ - done the same afternoon, see the
   section above.
2. **Remember the dial and the volume** across power-offs. Every boot
   starts on 10.000 MHz at -24 dB; there is an FM25W256 FRAM on the
   board (`BOARD.md` section 6) and no driver for it yet. The same FRAM
   could keep the crash record, which a power-off now loses.
3. The 100 kHz-step test for the 10 MHz digital noise (below), when the
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
  the modulation, rather than fading it, is the step after that. (DONE
  21/11/2026 - the top of this file.)
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
