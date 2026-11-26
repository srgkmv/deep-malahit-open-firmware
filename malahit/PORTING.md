# Porting this firmware to the Malahit DSP-1

Status, 01/11/2026: **steps 0 to 6 of §4 are done and verified on
hardware.** Clock tree at 480 MHz from the 22 MHz bypass oscillator,
switched rail, UART7 console, crash reporting, the ILI9488 panel
drawing at 18 bits per pixel, an FT6236 touch controller calibrated
against four targets, and the NAU8822 codec on SAI1 as I²S master.

**The question the whole port turned on is answered: 192 kHz works** —
§3.2. The upstream DSP tree keeps its sample rates and needs no
resampler, which is the difference between a driver swap and retuning
every filter corner in `User/`.

**The radio now receives, and shows what it receives.** A generator at
10 MHz is tuned, filtered, mixed to baseband, digitised, transformed and
drawn on the panel, with the peak where the arithmetic predicts. Every
stage answers to the register that controls it. Nothing is demodulated
yet.

Next is step 7, AM — audio out of the speaker. The first thing to attack
there is not the demodulator but **the mixer's DC offset**, which
measures 15 dB above the wanted carrier and within 8 dB of clipping the
ADC; see `BOARD.md` §6.

- `BOARD.md` is the hardware reference: pin map, rails, clocks, and what
  each peripheral turned out to be.
- `WORKING.md` is how to build, load and debug without losing an
  evening. **Read it first in a new session**; most of it is lessons
  rather than instructions.
- `bringup/` is the code.

The upstream firmware targets the DEEPSDR 101 (GD32F450VET6, 800 × 480
parallel panel, QSD mixer, MS5351 LO, TLV320AIC3204 codec, external SPI NOR
flash). The Malahit DSP-1 is a different board with the same *shape* of
signal chain. See `BOARD.md` for what is actually on it.

---

## 1. Why this is worth trying

Both receivers digitise analog I/Q with an **audio codec at roughly 100–200
kHz**, not with a wideband ADC. That single fact is what makes the port
possible: the whole of `sdr_rx.c → iqbal → demod_am.c → <28 modes>` is the
same architecture on both radios. Roughly 90 % of `User/` — every
demodulator, FT8, HFDL, STANAG, WSPR, AIS, SSTV, WEFAX, RTTY, PSK31, the AGC,
the noise reduction, the filters — is portable C that never touches a
register. Only 31 of the 337 files in `User/` include `gd32f4xx.h` at all.

And the target is a big step up: Cortex-M7 at 480 MHz against M4F at 200 MHz,
2 MB flash against 512 kB, ~1 MB RAM against ~192 kB.

## 2. What has to be replaced

| Upstream file(s) | Lines | Replaced by |
|---|---|---|
| `ms5351.c`, `lo_gen_gd32.c` | 1577 | `msi001.c` — **written and verified**: 24-bit write-only control on PA4/PA5/PA7 (SPI1), 22 MHz reference, Mirics register map: LO PLL, band mode, LNA/mixer/baseband gain, baseband filter width, DC offset |
| `rf_lpf.c` | 110 | **done** — `BPF1`…`BPF6` on PE10–PE15 plus `PREAMP` on PE0; three PE42641 switches, masks measured, `BOARD.md` §6 |
| `aic3204.c` | 1143 | `nau8822.c` — SPI control, not I²C; different register map; built-in speaker amp |
| `gd32_i2s.c` | 1305 | SAI1 on the H743, **master** (the MCU drives MCLK from PE2), with DMA |
| `rm68120_exmc.c`, `lcd_dma.c` | 1401 | ILI9488 over SPI3 + DMA |
| `touch.c`, `touch_calib.c` | ~1000 | capacitive controller on I2C3; no calibration needed |
| `encoder.c` | 241 | two encoders plus buttons |
| `spi_flash.c` | 6088 | the W25Q driver and FAT32 layer are portable; only the SPI backend changes — **but the chip itself is absent on this board**, see §4 |
| `usb_cdc_hw.c`, `usb/GD32F4xx_usb_library/` | — | ST's OTG_FS driver or TinyUSB |
| `dfu_salto.c`, `cargador.c`, `boot/` | — | probably nothing: the H743 has a ROM DFU bootloader on BOOT0, which jumper JP1 already reaches. Dropping the custom bootloader removes the single riskiest subsystem of the port |
| `battery.c`, `rtc_hw.c`, `backlight.c`, `reset_causa.c` | ~935 | H7 ADC / RTC / timer PWM / RCC |

`ipa_blit.c` is a happy exception: the GD32's IPA is the same block as ST's
Chrom-ART, so it becomes DMA2D with little more than a rename.

## 3. The four real problems

### 3.1 The display, and the UI on top of it

> **Answered, 01/11/2026: 18 bits per pixel, 460 800 bytes a frame.**
> The panel accepts COLMOD 0x55 and reads it back as 0x05, then draws
> nothing - the chip has the register bit for 16-bit serial and no
> serial data path behind it. Its own datasheet gives data-format
> figures for 3-bit and 18-bit on the 4-line interface and none for
> 16-bit. Measured: 245 ms a frame at 15 MHz, about 122 ms at 30 MHz,
> both at the full bus rate. See `BOARD.md` §6.
>
> The consequence for the framebuffer is the awkward one. RGB565 would
> have been 307 kB in AXI SRAM and gone to the panel unchanged; instead
> it is either 461 kB held in the panel's own format, which is nearly
> all of AXI SRAM, or RGB565 converted to three bytes on the way out,
> which costs CPU on every pixel. That choice is now a real design
> decision rather than an assumption.
>
> **Answered by measurement, 01/11/2026: no framebuffer is needed.**
> Drawing the spectrum one 320-pixel column at a time into a 960-byte
> buffer costs the same as building a whole 460 kB frame and sending it
> in a single transfer - within a few percent - because the overhead of a
> window set and a RAMWR is a dozen bytes against 960 bytes of pixels.
> The bus is the limit either way, at 129 ms a full frame, and the 460 kB
> stays free for the DSP tree.


800 × 480 → 480 × 320 is 3.4× fewer pixels. The upstream layout is not
parameterised, it is hand-fitted: `main.c` is 39 094 lines and carries
constants like `MAIN_W = 800`, `MENU_AREA_W(800)`, `RTTY_TEXT_COLS = 66`, with
comments doing arithmetic such as "8 buttons of 126 + 5 gaps of 8 + 2 = 800
exactly". This is a redesign of header, spectrum, waterfall, FT8 map, HFDL
list, menus and frequency keypad — not a change of `#define`.

It is also the largest single piece of work in the whole port, and the part
with the least leverage from the hardware being better.

**Mitigation: `deepsdr-ui/` already compiles the real firmware files on a PC
and emits PNGs** (290 test programs). The entire 480 × 320 relayout can be
done and reviewed on a desktop before any of it runs on the radio.

### 3.2 Sample rate

> ### ANSWERED, 01/11/2026: 192 kHz WORKS. The DSP tree keeps its rates.
>
> Measured on the radio, 8 kHz to 384 kHz, SAI1 as I²S master with
> MCLK = 256 × Fs from PE2 and the codec's own PLL bypassed:
>
> | Fs | measured | ADC noise rms | verdict |
> |---|---|---|---|
> | 8 000 | 8 000 | 2 | ok |
> | 48 000 | 47 999 | 3 | ok |
> | 96 000 | 95 999 | 3 | ok |
> | 160 000 | 159 999 | 6 | ok |
> | **192 000** | **191 999** | **7** | **ok** |
> | 240 000 | 239 998 | 10 | ok |
> | 320 000 | 319 997 | 15 | ok |
> | 384 000 | 383 997 | 182 | breaking up |
>
> Zero transmit underruns and zero receive overruns at every rate.
>
> The noise column is what makes this evidence rather than assertion. A
> sigma-delta spreads its quantisation noise across the sampled band, so
> the in-band RMS should grow as √Fs and no faster — and from 8 kHz to
> 320 kHz it does, a 40× rate increase for a 7.5× noise increase against
> √40 = 6.3. A converter that had quietly stopped working would still
> have delivered the right *number* of samples at the right rate; it
> would not have delivered a noise floor that tracks the square root of
> the rate. At 384 kHz the trend breaks outright — rms 182 against 15 one
> step earlier, extremes at −4142 and +9995 — which is a modulator losing
> stability.
>
> So the datasheet's 48 kHz ceiling (IMCLK ≤ 12.288 MHz) is exceeded by
> **four times with margin to spare**: 192 kHz needs IMCLK = 49.152 MHz
> and the part is still clean at 81.92 MHz. The margin is undocumented
> but it is not marginal.
>
> **Consequence: nothing downstream has to be retuned.** The upstream
> 48 / 96 / 192 kHz rates transplant unchanged, the SSB branch still
> decimates by 4 to 12 kHz, and HFDL, STANAG, FT8, WSPR and the rest keep
> the input rate they were written for. No resampler.

Upstream runs 48 / 96 / 192 kHz, and the SSB branch decimates by 4 to
**12 kHz**, which is what HFDL, STANAG, FT8 and the rest of the digital modes
expect at their input. The stock Malahit firmware runs 160 / 80 / 40 kHz;
dividing 160 by 4 gives 20 kHz and every digital mode lands on the wrong rate.

Two ways out, of which the first turned out to work:

1. Find out whether the NAU8822 will run at 192 kHz on *this* board. The
   often-quoted "PLL only locks between 90 and 100 MHz" constraint applies to
   the codec running as clock master; here the **MCU supplies MCLK = 256 × Fs**
   from PE2, so the codec's own PLL is out of the picture. If 192 kHz works,
   the entire DSP chain keeps its rates untouched.
2. Otherwise insert a rational resampler. `hfdl_rational_resampler.c` already
   exists and can be generalised. **Not needed.**

### One thing 22 MHz cannot do: land exactly on either rate

Neither 192 nor 160 kHz is exactly synthesisable from this oscillator, and
no choice of dividers changes that. A PLL output is HSE/M × (N + FRACN/8192)
/ P, and both target MCLKs carry a factor of 5³ in the denominator —
49.152 MHz is 6144/125 MHz, 40.96 MHz is 1024/25. The only place a factor
of five can come from is M or P, and working it through, an exact
49.152 MHz needs M × P = 1375k, which forces N above 3000 against a
hardware maximum of 512.

In practice this does not matter at all. With MCKDIV and P between them
offering thousands of places for the fractional divider to sit, the solver
in `bringup/src/sai1.c` lands within a few hundred parts per **billion**:
192 kHz comes out at 191999.988 Hz. X1 is a commercial oscillator module
whose own tolerance measured 1500 ppm in step 1 — four orders of magnitude
larger. The residual division error is not what will ever be wrong with
this receiver.

### 3.3 Storage

No SPI NOR flash, no SD card. 32 kB of FRAM, and that is all. Upstream keeps
maps, the station database, the aircraft database, screenshots, SSTV captures
and WEFAX images on a FAT32 disk in SPI NOR; `mapas/` alone is 11 MB.

For now: **build without the disk.** Settings go in the FRAM, which suits them
better than flash anyway. Maps, the station database and USB MSC are disabled.

Later: a W25Q128 fits on the existing SPI2 bus beside the FRAM, needing only a
second chip select from one of the free pins on header J6. Upstream already
sizes the chip at boot, so no recompile is needed once it is there.

**Done 26/11/2026 (step 60):** a W25Q64 (8 MB) stacked on the FRAM, its
/CS on PB0 - this unit has no J6. Upstream's driver and formatter, a 7 MB
FAT12 volume, and the USB disk as a mode of the application.

### 3.4 The H7 itself

Things the M4 did not have and that will bite:

- **D-cache against DMA.** Every SAI, SPI and DMA2D buffer needs either an MPU
  region marked non-cacheable or explicit clean/invalidate. This is the
  classic "works, but occasionally garbage" failure.
- **HSE is 22 MHz in bypass mode** (`RCC_CR.HSEBYP`), shared with the tuner.
  No STM32H7 example anywhere assumes 22 MHz; every PLL divider has to be
  recomputed.
- Flash writes are 256 bits wide and sectors are 128 kB.
- Power supply configuration (LDO vs SMPS) must be set before raising the
  clock, or the chip hangs at `PWR_D3CR.VOSRDY`.

## 4. Order of work

Each step should end with something visible on the radio.

0. **Debug path.** *Done* — `tools/swd.sh id` answers over `dapdirect_swd`.
1. **Blink.** *Done* — `bringup/`. HSE bypass at 22 MHz, PLL1 to 480 MHz
   through VOS0, `PC6` for the peripheral rail, `PB8` for the backlight,
   UART7 on PE7/PE8 as console. Measured on the radio: `clock_status` = 0,
   `SYSCLK` = 480 000 000, `RCC_CFGR.SWS` = PLL1, `PWR_D3CR` = `0xE000`
   (VOS0 plus VOSRDY), and SysTick counted 2003 ms across a 2000 ms
   debugger pause — 0.15 % off, which is the oscillator's own tolerance
   and proves the divider chain rather than merely asserting it.

   Runs from RAM (`make ram`), so none of it costs the factory image.

   The console is on UART7, PE7/PE8 — pins 37 and 38 of the package on
   units without the J6 header. A crash is recorded to a fixed address
   and reported in plain text on the next boot, and a crashed run no
   longer has to be power-cycled out of: see `src/fault.c`.
2. **Panel.** ILI9488 over SPI3. Measure the real throughput and settle the
   16-bit-versus-18-bit-per-pixel question.
3. **Touch.** *Done* — FT6236 at 0x38 on I2C3, CIPHER 0x36 and
   FOCALTECH_ID 0x11. Coordinates decoded and mapped to the landscape
   display; see `BOARD.md` §6. `src/i2c3.c` is the bus driver.
4. **Codec.** *Done* — `bringup/src/nau8822.c` and `bringup/src/sai1.c`.
   SAI1 block A is the I²S master transmitter and owns the clocks, block
   B a synchronous slave receiver carrying I/Q; MCLK = 256 × Fs out of
   PE2 from a dedicated fractional PLL2; the NAU8822 runs as a 16-bit
   I²S slave with its own PLL bypassed. **192 kHz confirmed on hardware
   (§3.2), and a 1 kHz tone comes out of the speaker.**

   Three things were known going in and all three mattered. The control
   interface is **write-only** — there is no read-back pin, so every
   register has to be verified by its effect rather than by asking; the
   three effects that worked are in `BOARD.md` §6. SAI1 must be the
   **master**, because MCLK comes from the MCU on PE2, the opposite of
   the DEEPSDR wiring. And the datasheet's 12.288 MHz IMCLK ceiling is
   exceeded four times over with margin left.

   Four things were not known, and each cost real time:

   - **`CKSTR` for the receiver.** RM0433 and ST's own HAL disagree, and
     the wrong choice does not look like a timing error — it looks like
     a railed converter at every sample rate. See `sai1.h`.
   - **The D-cache was on**, left by the factory firmware, so every
     *second* load over SWD ran a mixture of two builds. See
     `WORKING.md`; this one was worth an evening.
   - **The speaker is bridge-tied** across LSPKOUT and RSPKOUT with no
     ground, so R43 `RSUBBYP` is mandatory, not an optimisation.
   - **The ADC high-pass filter defaults to ON**, which would notch out
     the centre of the receiver's passband.

   Still polled, not DMA, deliberately: the D-cache question belongs in
   a step that is about DMA together with the MPU regions that make it
   safe, rather than smuggled into the step that is about the codec. At
   192 kHz a frame is 2500 CPU cycles and four words, so polling is
   nowhere near the limit — but the transmit FIFO is 21 µs deep at that
   rate, which means any blocking console write is an audible gap. That,
   and not CPU load, is what makes DMA necessary. **Done, in step 10 —
   24 console lines, 24 underruns polled and 0 on DMA.**
5. **Tuner.** *Done* — `bringup/src/msi001.c`. Register sequence, 24-bit
   write-only control on SPI1, the synthesiser arithmetic for a 22 MHz
   reference, band selection and the filter bank.

   Verified against a signal generator at 10.000 MHz:

   - tuning computed `INT`/`FRAC`/`AFC` with **0 Hz error** and a 0.33 Hz
     step, and sweeping the tuned frequency ±5 kHz put the response peak
     **exactly** on the predicted tuning. That is the test that cannot be
     passed by accident — a wrong reference, LO divider or FRAC field
     would still receive something, just not there.
   - the band filter masks were predicted from the schematic and then
     confirmed by sweeping all 64 combinations: the two paths that pass
     are the two whose passbands border 10 MHz, 23 dB above the rest.
   - **AM port 2**, not port 1: 33.6 dB better, as the 75 Ω versus 1 kΩ
     input impedances predict.
   - gain reduction tracks measurement within 2.6 dB over its 59 dB range.
   - **image rejection 32.2 dB** uncorrected, and **LO leakage 4 dB above
     the carrier** at DC.

   `BOARD.md` §6 has the tables. The last two numbers are the ones that
   matter downstream: 32 dB is what `iqbal.c` starts from, and the DC
   leakage is large enough to be a headroom problem rather than a
   cosmetic one.
6. **Spectrum.** *Done* — `bringup/src/fft.c`, `bringup/src/spectrum.c`.
   A 1024-point complex FFT of the I/Q pair, Hann windowed, 187.5 Hz a
   bin over the full 192 kHz span, drawn across 480 columns with the
   tuned frequency in the middle.

   **A generator at 10 MHz is visible on the radio's own screen**, with
   its peak where the arithmetic says it should be: tuned 20 kHz low, the
   peak lands at +19 875 Hz, which is bin 106 of 187.5 Hz — the nearest
   bin to +20 000 and therefore exactly right.

   Complex, not real: the receiver is zero-IF, so a carrier 20 kHz above
   the tuning and one 20 kHz below are different signals and must not
   fold onto each other. A real FFT would discard precisely what the
   quadrature front end was built to provide.

   Costs: **129 ms a frame**, 7.7 fps, of which the FFT is 1.7 ms. The
   display is the whole cost and the bus is the limit.

   No libm - this image links `-nostdlib` - so the twiddles and the Hann
   window are built from the half-angle identities, halving down from
   cos(pi) = -1, which reaches every angle a power-of-two FFT needs
   exactly. The one place that went wrong was a log2 polynomial written
   from memory instead of fitted: it was exact at powers of two, so every
   obvious test passed, and 1.5 bits out elsewhere, which made every
   level uniformly 4.5 dB low. Fitted properly it is 0.0006 dB.
7. **AM.** Audio out of the speaker.

   **The graft of upstream code starts here, not at step 8** - the plan
   used to say step 8 and that was wrong, because what step 7 needs is
   already written. Grafted and running on the radio as of 02/11/2026,
   compiled in place out of `../../User` with no copy and no edit:

   | File | Lines | What it brought |
   |---|---|---|
   | `fft.c` | 460 | the complex I/Q transform, fftshift order, DC blanking |
   | `iqbal.c` | 1694 | image/gain/phase corrector, with its own A/B harness |
   | `iqancho.c` | 232 | the frequency-dependent corrector iqbal falls back on |
   | `agc.c` | 72 | |
   | `audiofil.c` | 151 | |

   plus the nine CMSIS-DSP sources upstream had already trimmed to.
   2609 lines of upstream DSP, unmodified, on a different MCU.

   Measured once it was running: image rejection **30 dB with the
   corrector off, 34 dB with it on**, by `iqbal`'s own A/B test; phase
   error 0.69°, gain error 0.36 dB. The 4 dB is modest and it is exactly
   what upstream itself found - `iqancho.h` opens with "Un decibelio",
   the observation that a flat two-number corrector barely moves the
   on-screen mirror when the imbalance is frequency-dependent. The same
   thing is true on this board, which is a useful cross-check on both.

   Two things had to be reconciled, and neither needed an upstream edit:

   - **I/Q order.** `iqbal_bloque()` reads `crudo[2k]` as I; this board
     wires AUDIO_Q to the codec's left channel, so the stream comes out
     Q first. Fixed with the codec's `ADCPHS` bit - one register, no
     per-sample cost - rather than by swapping at every call site. See
     `bringup/src/nau8822.h`.
   - **CMSIS-DSP.** Upstream's copy expects the CMSIS-4 era
     `core_cmInstr.h`/`core_cm4_simd.h`; this port uses a modern CMSIS
     core where those intrinsics live in `cmsis_gcc.h`. Two forwarding
     headers in `compat/` resolve the names without touching upstream,
     and `ARM_MATH_CM7` replaces `ARM_MATH_CM4`.

   `-nostdlib` had to go: the DSP tree is written against libm.

   > ### `demod_am.c` is step 8, not step 7 - measured, 02/11/2026
   >
   > The file itself is portable: 6987 lines whose only hardware call is
   > `gd32_i2s_stream_write_half()`, five times. That made it look like a
   > one-shim graft, and it is not.
   >
   > **Its transitive closure is 106 `.c` files and 72 126 lines**, of
   > which 12 touch GD32 registers - including `lcd_dma.c`,
   > `ipa_blit.c` and `spi_flash.c`. It is the hub of the whole mode
   > tree, and upstream offers no build switch to leave modes out:
   > `FW_EXCLUIR` excludes exactly one GD32 peripheral driver and
   > nothing else, and `SIN_IQ` is the only feature guard in the tree.
   >
   > So grafting it means grafting essentially all of `User/` at once,
   > and that cannot happen before the display layer (§3.1, step 9) and
   > the storage decision (§3.3) are settled, because they are inside
   > the closure. Which is what step 8 always said.
8. **Graft the upstream DSP tree** mode by mode - which in practice
   means `demod_am.c` and the 106 files behind it, so it wants the
   display layer in place first. See the box under step 7.

   > **Measured 21/11/2026: not the 106 files.** That is the closure
   > of `#include`. What `demod_am.c` needs to LINK is ten modules,
   > with the decoders it feeds answering "off" - `src/decoders_off.c`,
   > 52 names. So the modes come first and each decoder is its own
   > later graft: delete its stubs, add its `.c`, let the linker speak.
   > Step 26 below.
9. **Relayout the UI** at 480 × 320, in the simulator first.

   *Started, 02/11/2026, and it began with the part nobody budgeted
   for being small.* `User/gfx2.c` — 969 lines, the band compositor and
   the whole drawing API — now runs on the Malahit's ILI9488 at
   480 × 320, drawing gradients, rounded rectangles, antialiased text
   from upstream's own faces, and lines. A full screen takes 160 ms, of
   which 138 ms is SPI.

   **It needed three functions.** PORTING.md budgeted 1401 lines to
   replace the display layer; most of that is `rm68120_exmc.c` and
   `lcd_dma.c`, the panel driver, which this board already has in
   `ili9488.c`. What `gfx2` is actually coupled to is
   `rm68120_set_window()`, `rm68120_dato()` and `rm68120_ciclo()` —
   because `gfx2` was already decoupled. It is the same file
   `deepsdr-ui` compiles, byte for byte, against `lcd_sim.c` to draw
   the UI on a PC, and that seam is what the port inherited.

   The two mismatches are in `compat/rm68120_ili9488.h`: RGB565 against
   18-bit, and a memory-mapped store per pixel against SPI. The second
   is the one that matters — a transaction per pixel would be 153 600
   of them — so `rm68120_dato()` converts into a staging buffer and the
   window goes out in one transfer.

   **`User/spectrum.c` followed, and draws the panadapter.** 2078 lines
   of heat-map palettes, partial redraw, per-column ceilings and band
   labels, with `gfx.c`, `idioma.c` and `spec_rotulos.c` behind it — all
   compiled in place. 68 ms for a 480 × 240 panel, about 14 frames a
   second, against 129 ms for the stand-in it replaced, because upstream
   redraws only what changed.

   `SPEC_MAX_W = 800` needed no change: it sizes arrays and 480 fits
   inside it. What did bite was `SPEC_MAX_H = 280` — `spectrum_draw()`
   opens with `if (h > SPEC_MAX_H) return;`, so asking for all 320 rows
   drew nothing and said nothing. **Timing the call is what found it:**
   a draw that reports 0 µs has not drawn, and without that instrument
   the search would have gone into the SPI layer, which was working.
   240 rows is the right answer anyway; a real layout wants a header and
   a waterfall.

   Two seams were left by `main.c` rather than by hardware, and both had
   to be filled here: `g_pal`, the theme pointer the whole drawing tree
   reads, which upstream defines on line 7484 of its `main.c`; and a
   peak search, which the real UI does from its own cursor.

   **The stand-in is gone.** `panbringup.c` has been deleted; the only
   thing kept from it is the peak search, now in `iqmeas.c`. With that,
   upstream code in the image passes ours: **10 403 lines against 7381**,
   plus nine CMSIS-DSP sources.

   **The 480 × 320 relayout is done, and it was done on a PC** — which
   is what §3.1 proposed and what made it cheap. `make malahit` in
   `deepsdr-ui/` renders the whole main screen from the real firmware
   files at the port's geometry, so every iteration was a PNG and not a
   trip to the bench.

   The geometry lives in `compat/malahit_layout.h`, used by the host
   render and the firmware build alike so the two cannot drift:

   | | upstream 800×480 | here 480×320 |
   |---|---|---|
   | header | 64 | 34 |
   | status | 40 | 16 |
   | spectrum | 240 | 142 (trace 120) |
   | waterfall | 72 | 92 |
   | actions | 50 | 30 |

   The waterfall ends up **larger than upstream's** on a panel a third
   shorter, because the chrome was halved and the type with it.

   Three things had to be understood rather than guessed:

   - **`gfx2_text()` lays out a LINE BOX, not a glyph.** `font_ui_11` is
     11 px tall but its line box is 14, and two of them are 28 — which
     is why the action bar needed 30 rows and not 26. The buttons were
     not badly centred, the text did not fit.
   - **Scaling offsets by the band height is wrong when the type does
     not scale with it.** The faces stop at `font_ui_11`, so a chip
     still needs room for its text whatever the band does. Chips are
     sized from the face instead.
   - **Generated faces where none existed.** Nothing sat between
     `font_num_20` at 21 px and `font_num_44` at 46 px, so the frequency
     readout was either a label or did not fit. `font_num_31` was
     generated with upstream's own tool. It fits a 34-row header only
     because the face is digits-only and its descent is empty.

   **And it is on the radio, 03/11/2026.** `ui_top.c`, `ui_act.c` and
   `spec_chrome.c` are compiled in place alongside the rest, the
   layout header is force-included into the firmware build exactly as
   it is into the host render, and `main.c` fills the three state
   structs from the receiver and draws. The trace sits inside the
   layout, the waterfall scrolls in a 92 × 440 ring, and the whole
   touch map answers: six action buttons, four status chips, the
   frequency readout, the band pill, MEM and the clock, every one of
   them confirmed under a finger on the glass.

   Frame cost on the radio: trace 31 ms, waterfall 40 ms, DSP 0.6 ms,
   about 7 fps. The waterfall is the expensive half — 92 × 440 pixels
   is 121 kB of SPI per frame — and the answer to it is DMA, which is
   the same conclusion the SAI reached.

   **Three bugs that only hardware could find**, and all three are
   worth the space because none of them could have come out of a PNG:

   - **Three right-anchored coordinates in `ui_top` still assumed an
     800 px panel** - the clock corner's clipped repaint window, the
     clock's touch box, and the battery pill. The battery was being
     drawn at x = 740 and clipped away in silence, so it was missing
     from every render of this layout and looked like a design choice.
     All three now hold their distance from `GFX2_W`; see
     `PROCEDENCIA.md`.
   - **Nothing cleared the panel.** `ili9488_init()` resets the ILI9488
     and sends its init sequence but never touches GRAM, and no UI
     module covers the whole screen, so the four gaps between the
     modules showed uninitialised panel memory: dotted lines above and
     below the trace and a bright stripe down the right of both the
     spectrum and the waterfall. **One artefact crossing two modules
     that share no drawing code is the tell** - it cannot be a bug in
     either of them. The host render is structurally unable to show
     this, because `sim/principal.c` clears the simulated panel first.
     It is the first thing in this port the PNG could not predict.
   - **A touch report sampled at 7 fps.** Everything above the action
     bar reported hits to the console and showed nothing on the panel,
     because that path waited for the FT6236's event code 0, "contact
     down", which the controller sends exactly once per finger. At one
     poll per 140 ms frame, a tap is usually only ever seen as event 2.
     The six buttons worked throughout only because their branch never
     looked at the event code. It is an edge detected here now.

   What remains of §3.1 is `main.c`'s 39 094 lines, which carry the
   screens this render does not: menus, the band list, the keypad, the
   FT8 map. Those are `ui_digi.c` and friends, already separate modules,
   and the same harness renders them.

10. **The SAI on DMA.** *Done, 05/11/2026* — `sai1_dma_start()` and the
   rest of the DMA section of `bringup/src/sai1.c`. DMA1 stream 0 for
   the receiver and stream 1 for the transmitter, both circular over two
   halves, 16-bit items in direct mode, the rings in AXI SRAM where DMA1
   can reach them.

   **It is not a performance step and it was never about CPU load.** The
   transmit FIFO is eight words — 21 µs at 192 kHz — and a console line
   at 115200 baud takes 9 ms. 24 console lines produced **24 underruns
   polled and 0 on DMA**, which is the audible click per printed line
   that step 4 predicted and could not fix. `dry` then stays at 0 for
   the whole display loop, through 16 ms of trace and 40 ms of waterfall
   over SPI a frame.

   Two things came free. The capture went from 43 315 µs of polled
   waiting to a **386 µs** memcpy out of the finished half, which took
   the frame period from **127 ms to 91 ms (7.9 to 11.0 fps)**, measured
   by the same code in the same run. And the receiver stopped having
   gaps: it now runs continuously, which is what a demodulator needs.

   **No MPU regions, no cache maintenance.** The D-cache is off in this
   firmware (`clock.c`, and `startup_stm32h743.S` before the `.data`
   copy), so there is nothing stale either way. That is the whole reason
   the step was small, and it is the debt the day anything turns the
   data cache on. §3.4's "works, except sometimes" is still waiting
   there.

   **No interrupt either**, which is upstream's own mechanism: which
   half is finished comes out of `NDTR`, exactly as
   `gd32_i2s_stream_write_half()` does it on the DEEPSDR. An ISR is
   step 8's business, when there is a demodulator that has to run once
   per block — and it will need a named vector, which
   `startup_stm32h743.S` currently has for nothing.

   Three bugs, one per load, and every one of them is a thing a DMA
   driver on a radio that boots somebody else's firmware first has to
   survive:

   - **The factory firmware's own transport was still running.** DMAMUX1
     channels 9 and 13 — DMA2 streams 1 and 5 — enabled, pointed at its
     buffers in D2 SRAM, on the same two SAI request lines. Halting the
     core does not stop a DMA controller. Two channels on one request
     line take alternate words, so the receiver got **exactly half** the
     stream with no overrun and no error, and consecutive samples of one
     channel read as an I/Q pair put the spectrum at twice the right
     offset with an image almost as strong as the carrier.
   - **`SAIEN` did not hold.** It reads back as set and is clear a
     hundred microseconds later, on three arms in four, with every error
     flag at zero — a previous disable that had not finished. The
     symptom was a receiver delivering digital silence with `rc 0` and
     the best frame rate the radio had ever reported.
   - **And the retry for that exchanged I and Q**, because a failed
     attempt had already moved an odd number of words through a stream
     that was still running. The unit of retry has to be the whole arm.

   And then a fourth, the morning after, which turned out to be the
   oldest of them: **the polled `sai1_start()` had the `SAIEN` hazard
   too**, and had had it since step 4. On the first run after a cold
   power cycle three polled captures returned zero frames, one of them
   in test 0 before any DMA stream existed. It had been costing a row of
   test 0's table and the occasional frame, and — because
   `test_dma_transport()` did not check what `capture()` returned — it
   silently turned that test's polled reference into the DMA block
   averaged twice. The warning had been printed every time.

   `WORKING.md`'s trap list has all four, with the measurements that
   identified them rather than the theories that did not.

   **And it has been heard**, which is the half of this step that no
   counter can supply: 1.2 s polled against 1.2 s on DMA, six times, and
   the owner of the radio reported the alternation between a tone with
   clicks and a steady one. The same run counted 72 underruns polled and
   0 on DMA — one per printed line. Getting that confirmation took three
   attempts, and the first two found a better bug than the third: the
   codec had been **muted since step 4**, because the tone test that
   proved the speaker works was retired and took its own unmute with it.
   `test_audio_listen()` owns the unmute now.

11. **The encoders.** *Done, 05/11/2026* — `User/encoder.c` grafted
   whole, 241 lines, compiled in place through `compat/gd32f4xx.h`.
   Turning the knob retunes the receiver: the readout, the ruler and the
   trace all move, and the peak travelled from +19 875 Hz to −58 875 Hz
   against a generator that did not move — the 79 detents that were
   turned, at 1 kHz each.

   **The tuning knob is the big one, `RENC1` on `PC0`/`PC1`**, which is
   where the user wants it. That also settled the one thing in this step
   still resting on a single report: which physical knob is which was
   read off the ORDER the two were turned in, and the move to `RENC1`
   tested it from the other side — the named knob tuned, the other went
   inert. The peak ran +19 875 → −37 125 Hz **monotonically, with no
   reversal**, inside seven blocks, and then held still for 169 console
   lines while the small knob was turned. Direction confirmed by the
   hand; upstream's `encoder_invertido_pon()` is there if another unit
   disagrees, which upstream found that they do.

   **No new interrupt and no new vector**, because upstream asks to be
   sampled at 1 kHz from `SysTick_Handler` and this board already had
   one. The graft needed a stand-in for four GPIO calls and `#ifndef`
   around upstream's six pin defines (`PROCEDENCIA.md`).

   Two things the schematic had to settle and no probe could:

   - **The push-buttons are not GPIOs.** `ENC_BUT` is a resistor ladder
     shared by both buttons, with one of the two presses inside the
     input buffer's undefined band, so they need the ADC — step 12,
     which also brought the tuning step with it. The voltages quoted
     here at the time, 1.95 V and 1.05 V, were the schematic's
     arithmetic and turned out not to be this board's; step 12 has the
     measured ones.
   - **There are no external pulls on A/B**, so the MCU's internal
     pull-ups are the circuit rather than a precaution.

   And one thing that was got wrong three times before it was got right:
   `RENC1` was recorded as a hardware fault after three runs with no
   movement, and then moved 151 times in twenty seconds on the fourth.
   An absence is only evidence if somebody was turning the knob while it
   was being recorded — `WORKING.md`'s trap 18, which this step and the
   listening test wrote between them.

12. **The buttons, on the ADC.** *Done, 05/11/2026* — `src/adc.c`, ADC1
   channel 1, and a short press of the big knob now cycles the tuning
   step. Both encoder buttons share **one** analog line through a
   resistor ladder, so this is the one control on the board that no GPIO
   driver could ever have read.

   **Upstream's debounce was kept, and that was the point.** A new
   `ENC_BTN_READ` hook names a function the board supplies, so
   `encoder.c`'s integrator, hold timer, short/long classification on
   release and "pressed and turned" consumption all keep working for a
   button that is a voltage. See `PROCEDENCIA.md`; with no `-D` the
   DEEPSDR compiles the GPIO reader it always had, verified by building
   its firmware.

   **No interrupt and no wait.** One conversion per SysTick, collected
   by the following tick — the result has been sitting in `DR` for
   975 µs by then — with the two channels alternating. 32 369
   conversions and zero timeouts across a run.

   The first measurement (05/11) read 2.498, 1.656 and 1.423 V against
   1.95, 1.05 and 0.86 predicted from the 47k/68k/22k on page 3, and
   concluded the resistors were not what is fitted. **Wrong: it was
   measured through the closed `PA1` analog switch** (trap 23).
   Remeasured 20/11 with `PMCR` written: 1.960, 1.050 and 0.861 V, the
   schematic to half a per cent, and the thresholds moved with them.
   **"Both pressed" predicts itself** from the other two plateaus with
   no resistor value and no reference voltage in the arithmetic —
   17 129 counts against 17 091 measured. `BOARD.md` §6.

13. **The battery.** *Done, 05/11/2026* — the header pill shows a real
   percentage, from **ADC1 channel 16**: the midpoint of R34/R36, two
   10 kΩ resistors across the cell, with no diode in the path (the
   DEEPSDR's sense has one; this board does not).

   **Confirmed against a multimeter, which is what made it a
   measurement**: the meter read 4.15 V at the cell and the converter
   4.17 V. The same point fixes `VREF+` at 3 280 mV rather than the
   nominal 3 300 — one point from one meter, but the error it removes is
   systematic, which is the kind that hides. That constant is now only
   the fallback and the outside check; see below for what replaced it.

   It also overturned the previous step's battery figure. 3.05 V had
   been recorded from channel 0, and that channel is an unconnected pad:
   `PCSEL` was being OR-ed rather than set, so the pad was being read
   with the saturated `ENC_BUT` channel preselected beside it and sat at
   a steady, plausible, entirely fictional 1.53 V. **The stability was
   the trap** — see `WORKING.md` trap 21, and note that nothing inside
   the firmware could have caught it. The meter did.

   **The rail no longer rests on that meter reading.** `VREFINT` on
   **ADC3** — a bandgap that does not follow the supply — is converted
   every millisecond, so VDDA is measured rather than named. Both facts
   it needed came from the radio, not from a datasheet this tree does
   not have: the channel by difference (19 moved by 11 200 counts with
   the bandgap switched on, nothing else by more than 512), and ST's
   factory calibration by plausibility (24 327, inside the only window
   such a conversion can occupy, with the temperature sensor's two
   points beside it in the right order).

   Three routes to the rail — factory value 3 298 mV, nominal 1 216 mV
   giving 3 274, the meter 3 280 — agree within 0.7 %. Only the meter
   did not come out of this chip, which is exactly what makes it worth
   keeping as the fallback constant.

   Driving two converters cost no second copy of the sequence: it is one
   function taking an ADC instance.

   **The divider was then checked from outside, and the answer was to
   change nothing.** With the rail measured, the only error left is the
   ratio of two 1 % resistors — about 40 mV at a full cell, and
   invisible from inside the chip. The radio said 4 163 mV and a meter
   said 4.16 V. Three millivolts apart, and the trim stayed at zero:
   the meter reads to 10 mV, so its answer covers 4 155–4 165 and ours
   is inside it. **A disagreement smaller than the resolution of the
   instrument measuring it is not a disagreement**, and a constant
   fitted to it would have been the meter's last digit written into the
   firmware as if it were a measurement.

   **The die thermometer came with it**, on the same converter and
   found the same way — channel 18 moved 9 369 counts when `TSEN` went
   on. 40.5 °C, which at 480 MHz is well above the room. ST's two
   calibration points are believed because they give an offset and a
   slope independently — 623 mV at 30 °C and 2.537 mV/°C — and both land
   where a silicon sensor has to be.

   The percentage itself is a straight line from 3.3 to 4.2 V and is
   labelled as the weak part wherever it appears. Upstream's
   `battery.c` was opened in the hope of grafting a real discharge curve
   and contains the same two constants and the same interpolation.

   Left open: `ADC1_INP17` disagrees with what `PA1SO` says the analog
   switch is doing. It costs nothing, because both channels in use are
   confirmed by physical evidence.

14. **The panel comes up before the tests that need a hand.** *Done,
   05/11/2026* — and it is here as a step because it is a lesson rather
   than a tidy-up. Six tests in this image need somebody to turn a knob,
   press a button or listen, and all six asked over the **UART**, which
   the person holding the radio cannot see. They had been following
   instructions relayed from the other end of a debugger while the panel
   in their hands stayed dark, and finally asked why the screen was
   silent.

   **A bench test that needs a human is a user interface, and it was on
   the wrong device.** The panel's three bring-up calls moved from step 6
   to just before the first such test, and `ui_prompt()` puts the
   question on the screen in four lines. The console still gets
   everything, because that is where the measurements are read
   afterwards.

15. **The second knob.** *Done, 05/11/2026* — the small one on top now
   adjusts RF gain and its button toggles the preamp.

   **No second decoder was written.** `User/encoder.c` keeps its state
   in file-scope statics, so it is a one-knob module; it is compiled a
   **second time** with its public names renamed on the command line,
   and a second translation unit of the same source gets its own
   statics. Both knobs therefore run the one tested decoder. The cost
   is that `encoder2_tick()` has no definition to grep for, which
   `src/enc2.h` states plainly.

   **Why RF gain and not volume.** Volume, mode and bandwidth are all
   downstream of a demodulator and this image has none — the only sound
   it has ever made is a test tone. A control labelled for something
   that does not exist looks like it works, which is worse than one
   that plainly does nothing. RF gain is real, measured, and the one
   setting that matters while the AGC is a stub.

   Verified by sweeping it: 90 detents over the whole 0–59 dB range took
   the ADC fill from **10 % to 100 %**, underloaded to clipping. The
   preamp is worth a measured +15.1 dB (§4) and has **no on-screen
   indicator yet**, which is recorded rather than hidden behind an
   invented symbol.

16. **The demodulator, the block interrupt, and low IF.** *05/11/2026* —
   AM by envelope, from the antenna to the speaker, audible on the
   bench.

   **The first named external vector in this project.** Audio has to be
   produced at block rate and nothing else here runs faster than eleven
   frames a second, so DMA1 Stream 0 got a handler — priority 1, below
   SysTick, with the vector table's counts written as arithmetic. It
   also retires `sai1_tx_due()`'s known hazard: a hook driven by the DMA
   cannot be late. The transmit ring grew to match the receive ring so
   the hook can be handed both halves, which removed a 32 kB staging
   buffer.

   **`demod_am.c` was NOT grafted**, and that is a departure from this
   port's rule worth stating: it is 6 987 lines and pulls in thirty
   modules — the entire decoder suite. `src/am.c` is ours, but only the
   arithmetic; the AGC and the audio filter's design stay upstream's,
   compiled in place.

   **The first audio was distorted, and the user's experiment diagnosed
   it**: with the modulation switched off a tone remained, its pitch
   tracking the detuning. An envelope detector cannot do that — `|z|` of
   an offset carrier is constant — so something else was at 0 Hz,
   beating against the carrier. It was the oscillator's leakage, which
   at zero IF shares 0 Hz with a carrier that, in AM, *is* the signal.

   The cure is low IF: the tuner is parked a quarter of the sample rate
   below the dial and `am.c` mixes back by exactly Fs/4 — 1, -j, -1,
   +j, so swaps and sign changes, no table, no multiplies, no phase
   error. The spectrum's centre is now the oscillator and `demod_px`
   marks the listening point a quarter of the trace to the right, which
   is upstream's own value in a field that was already there.

   Two unit errors were found by reading the AGC's code rather than its
   comment: it works in int16 scale, not normalised, and its "release"
   is a per-sample MULTIPLIER where the header's prose reads as a
   subtraction. Neither is visible to a compiler, and the first produced
   a radio that counted blocks, reported no underruns and played
   silence.

   **Superseded 19/11/2026: the offset is now a fixed 12 kHz (Fs/16)**,
   not Fs/4 — the tuner follows the dial on every detent, a 16-entry
   table rotates the station back, and the panadapter shows 168 kHz
   centred on the dial. A zero-IF version in between was modelled and
   dropped before it ran: it subtracted the carrier with the leakage.
   The retune mute is gone and the click it left is repaired in the
   envelope. `WORKING.md`, "Forward again, to a fixed 12 kHz" and the two
   retune sections after it.

17. **Flash, and the way back.** *Done, 20/11/2026* — the radio boots
   our firmware cold from flash, with no PC attached. `src/boot.c` is
   the factory firmware's own recipe, read out of its code: both
   encoder buttons at power-on jump to ROM DFU (`0x1FF09800`), the
   power button held for three seconds switches off, and a reset with
   nobody pressing it goes straight back to sleep. Written over DFU into
   bank 1 sectors 0–1 only; the factory image is backed up outside the
   repo and the round trip — factory restored, then ours again — is
   done and verified. Three cold-boot bugs that no RAM test could show
   are in `WORKING.md`, and the unit turned out to be modified so the
   MCU latches its own supply (`BOARD.md` section 3).
18. **A boot for a radio, not for a bench.** *20/11/2026* — `SELFTEST`
   in `main.c`, 0 by default: screen and audio **2.5 s** after SysTick
   starts, against **37 s** with the bring-up tests (`make flash
   SELFTEST=1` puts them back; both figures off the boot's own
   `Boot      :` line). The image also shrinks from 134 kB to 96 kB,
   because the tests drop out of it - one flash sector instead of two.

   Skipping them was trap 17 three times over: three settings the radio
   ran on were outputs of tests. The receive clock edge (`CKSTR=1`) was
   chosen by `test_rx_timing()`, and `sai1.c`'s default was the wrong
   one; the add-on relay was chosen by measuring a generator, a coin
   toss on an antenna; the add-on filter was whatever the last test
   left. All three are now set by the startup itself, after the tests,
   so what the radio runs on no longer depends on which tests ran.
   Verified on the radio both ways: carrier on the dial on every
   `baseband:` line, no underruns, and the full path unchanged.

   The day also found two ways a RAM load corrupted the image it
   started - the old image's DMA, and a halt inside the block interrupt
   that left the new image with no audio. `WORKING.md`, "LOADING OVER
   SWD RESETS NOTHING".
19. **The console stops costing the frame, and a crash can be switched
   off.** *20/11/2026* — from the display loop on, console output goes
   into an 8 kB ring that UART7's interrupt sends (`console_async()`).
   The ~300-character status line had cost ~26 ms of every ~113 ms
   frame, the CPU waiting on the wire; the frame is now **~80 ms, 12.5
   fps**, with nothing dropped (`con` on the line counts writes that
   found the ring full: 0). The boot stays polled, because that is where
   runs hang, and `fault_park()` flushes the ring before it parks -
   checked with a deliberate crash, whose last line came out whole.

   Going faster uncovered a hidden bug: the first capture after the DMA
   is armed copied a half that had not been filled yet - one block of
   zeros - hidden until now by the ~50 ms the polled console took to
   print the arm report. `sai1_rx_half()` waits for the first half,
   once per arm.

   And chasing a power-off after a crash found two real faults and one
   loader trap: after a crash **the power button did nothing** (it lives
   in SysTick, masked while parked, and no watchdog runs), so
   `fault_park()` now polls it - 3 s held switches off, checked on the
   radio; `boot_standby()` **did not sleep** with a tick pending, a dark
   radio still drawing power - it now clears pending exceptions first;
   and a load over a parked crash ran the new image's SysTick handler on
   the old image's memory, which switched the radio off - `core_clean`
   now clears it. `WORKING.md` has all three.
20. **The deaf cold start, found and fixed.** *21/11/2026* — some cold
   starts came up with no carrier at all until anything re-ran the init.
   Caught live without a reload, and cured by one call over SWD
   (`tools/swd-call.sh`, `addon_set_filter`): the add-on board's 74HC595
   had lost its filter code. A test that power-cycles the add-on board
   96 times with the MCU warm (`test_addon_cold()`) put the cause in
   numbers: writes 10-20 ms after `Vext_ON` are lost every time, from
   25 ms never - the relay pulse was innocent. `addon_init()` waited
   10 ms; it waits 200 ms now. The boot's writes had sat at ~15-25 ms,
   on the threshold, which is why the fault came and went. `BOARD.md`
   section 7 has the table.

   The method is worth more than the fix: **when an intermittent fault
   shows itself, keep the radio in that state** - a snapshot to compare,
   then one init step at a time called into the running image - instead
   of reloading, which cures it and destroys the evidence.
21. **The panel on DMA.** *Done, 21/11/2026, in flash* — the
   frame went from **~80 ms (12.5 fps) to ~47 ms (21 fps)**, median, same
   signal, same picture, both measured in one run with the switch
   (`ili9488_dma_enable`) flipped over SWD. Pixel blocks go to SPI3 by
   DMA1 stream 2 (request 62, `spi3.c`) from a queue of eight 4-row
   slots (`ili9488.c`, "THE BLOCK QUEUE"), and SPI3's end-of-transfer
   interrupt starts each next block.

   **What it bought is not overlap but the bus time interrupts used to
   steal.** The polled loop stopped whenever the core was taken; the
   SPI FIFO holds 4 us, the AM block interrupt runs 8.4 ms. The
   waterfall measured 48 ms for the 32 ms its 121 kB need. A first
   version that started each block from the display loop, double
   buffered, got it to 39.5 ms and no further - the bus still stood
   idle for the rest of every 8.4 ms interrupt once the block in flight
   had gone. Starting blocks from an interrupt that outranks the
   demodulator, with 12 ms of blocks queued, got it to 30 ms. Same
   lesson as step 10's: on this radio a transfer that needs the CPU
   between pieces is a transfer the demodulator can stop.

   Fail-safe by construction: a 256-byte self-test with CS high (the
   panel not listening) has to pass before the queue is used, a queue
   that stops moving for 20 ms is torn down and the DMA stays off for
   the run, and every other panel function waits for the queue to
   empty before it touches the bus. 118 455 transfers on the radio, no
   failure, no stall, no audio underrun.
22. **The touch at power-on.** *Done, 21/11/2026, in flash* — four
   cold boots of step 21's image: two found the touch controller at
   once, one on the second attempt (+1.8 s), one never (touch dead
   until the next power-on). Both bad ones had SDA stuck from the 5th read of an
   identify poll that began 120 ms after the reset - inside the
   FT6236's wake-up, which its datasheet puts at 300 ms (`Trsi`).
   `test_touch_wake()` reproduced it warm, 2 lost of 20, and measured
   the two fixes apart: a bus recovery inside the poll cures the
   symptom (0 lost, still 3 stuck), waiting the datasheet's 300 ms
   removes it (0 stuck). Both are in: `FT_WAKE_MS` and the recovery,
   ~135 ms more boot. And `gpio_af_od()` wrote `AFR` last, so every
   power-on pulsed SCL and SDA low through MCO1/MCO2 at AF0 - the
   "bus recovered, -1 clocks" printed on four cold boots of four.
   Three cold boots of the fix: no such line, touch found on the first
   read, boot 2 753 ms each.
23. **The radio remembers the dial, the volume and the step.**
   *Done, 21/11/2026, in flash* — in the FM25W256 FRAM on SPI2 (`fram.c`,
   `settings.c`), at 0x4000, clear of the factory firmware's two areas.
   Two slots with a sequence number and a CRC-32: a save goes to the
   older one, a load takes the newest that checks, so a write cut by
   the power leaves the previous record whole. Saved one second after
   the last change; the 3 s hold that switches the radio off gives a
   last change time to land. The RF gain is deliberately not kept -
   `rf_autorange()` measures it at every boot.

   Before a byte was written the part was identified by behaviour, not
   trusted from the schematic: no busy bit and no page wrap on a write
   across a 64-byte page line, which rules out the SPI EEPROM that fits
   the same footprint (`BOARD.md`, FM25W256). And its 32 kB - the
   factory's settings in it - went to `backups/` first.

   Checked on the radio: blank FRAM boots on defaults; the owner tuned,
   changed volume and step, switched off and on; the next image came up
   on 9 990 093 Hz, -31 dB, 100 Hz, exactly the last save - and so did
   the first cold boot of the flashed image.
24. **ATT and preamp remembered, and the dial keeps its hertz.** *Done,
   21/11/2026, in flash* — the settings record grew to version 2 with
   the ATT pill's gain reduction and the preamp; restored, they replace
   `rf_autorange()` at boot (1.98 s instead of ~2.65), and version-1
   records still load. And the dial no longer walks down: `msi001_plan()`
   truncated `f_actual`, the synthesiser lands within a sixth of a hertz
   either side of a request, and `dial_tune()` built the next dial on
   the truncated figure - half of all dials read 1 Hz low, 1 000 detents
   of 1 kHz lost 459 Hz. Rounded, none do (`MSI_ROUND_TEST`, ~30 000
   dials and three walks, on the radio). A stored dial the bug left off
   the 10 Hz grid is rounded onto it at load.
25. **The retune click, predicted instead of faded.** *Done,
   21/11/2026, in flash* — the 19/11 repair held the carrier and faded
   the modulation for 3.7 ms around every synthesiser write; on a tone
   that hole was the dull click the owner still heard. The window is now
   filled with the envelope's own last period, found by autocorrelation
   in the 21 ms before it, and falls back to the hold only where there
   is no period. On the radio, the codec's own output, A/B in one run:
   residual from the tone -7.0 dB rms with the hold, -47.9 dB
   predicted; by ear, gone. `WORKING.md`, "The retune click, part two".
26. **Step 8 begins: upstream's demodulator on the radio.** *21/11/2026,
   from RAM, checked by the owner* — `demod_am.c` and nine modules
   compiled in place, beside `am.c`, switched in the running radio
   (`swd-call ... demod_path 1|0`, `demod_mode`, `src/demod.h`). The
   census first: the 106-file figure is the `#include` closure; the
   LINK closure is ten modules and 52 decoder hooks, the same set
   upstream's own host bench stubs (`deepsdr-ui`, `make demod`). Four
   compile errors on one spot - the speaker pin, an `#ifndef` upstream,
   `PROCEDENCIA.md`. AM, SAM, USB, LSB and NFM all checked by ear by the
   owner, "seems all fine"; in LSB the block interrupt costs 11.9 ms of 42.7 (am.c's AM:
   9.0), worst 808 us per 512-frame call, no overrun, no lost audio.
   And the memory: upstream's `.tcmram` - CPU-only on the GD32 too -
   now lives in the DTCM, so the RAM image is 460 kB of 512 with
   upstream in it and the SWD loop survives step 8.
27. **Step 9: the action bar works - mode, band and step screens.**
   *21/11/2026, from RAM, checked on the radio through its own panel* -
   upstream's two menu engines, `ui_cfg.c` (category column + grid: the
   mode and band screens) and `ui_grid.c` (paged grid: the step screen),
   compiled in place at 480 x 320. Their geometry and the text offsets
   inside their cells are `#ifndef` upstream now, with the old values as
   defaults - nine DEEPSDR renders byte-identical before and after
   (`PROCEDENCIA.md`) - and `compat/malahit_layout.h` gives this panel's.
   Laid out on the PC first: deepsdr-ui `make malahit-menus` renders the
   three screens over the main one and measures every label against its
   cell (0 overflows). `src/menus.c` is the controller - the part of
   upstream's `main.c` these screens need; the band table is upstream's,
   generated from its `main.c` at build time by the host's own script.
   Bandas, Modo and Paso open their screens (and close them, as
   upstream's do); Ruido toggles upstream's NR; Volumen gives the small
   knob back to the volume. In a menu the big knob moves upstream's
   cursor (across families on the band screen) and its press chooses.
   The status band's AGC chip steps OFF/SLW/MED/FST and the BW chip
   upstream's three quick filters, both reading the demodulator's real
   values; the band pill opens the band screen. Upstream's demodulator
   is now what boots.
28. **The SAI rings in D2 SRAM.** *21/11/2026, from RAM* - the two
   64 kB rings out of AXI SRAM into SRAM1 (`.d2sram`, NOLOAD, same
   address in both images; `sai1_dma_start()` switches the SRAMs'
   clocks on - they reset off, and the power-off turns them off again -
   and zeroes the rings as it always did). The RAM image goes from 484
   to 353 kB of AXI, the flash image's RAM from 331 to 200. Nothing got
   slower: the block hook 11.8 ms in LSB (11.9 before), worst upstream
   call 750 us, the loop's copy of a half 414 us (406), frame 42.7 ms,
   no overrun. And the SAI DMA error count reads 0 where it had read 1
   on every run since the panel DMA - not explained, recorded.
29. **The radio remembers its mode; upstream's eleven steps; the band
   pill follows the dial.** *21/11/2026, from RAM* - settings record
   version 3: the demodulator's mode in the byte that was reserved, and
   step_idx now indexes upstream's TUNE_STEPS_LISTA (10 Hz ... 1 MHz,
   with 500 Hz, 9 kHz, 12,5 and 25 kHz) instead of five decades. The
   list is GENERATED, not copied - `bandas_dump.py` now emits it beside
   the band table, with each band's edges and its own step - so a band
   chosen from the menu brings its step, as upstream's does. A v2 record
   loads with its step mapped through its hertz (1 kHz stayed 1 kHz on
   the radio). The header's band pill is upstream's banda_indice() rule
   - the narrowest band whose edges hold the dial, or "Bandas". Checked:
   41 m + USB saved, reloaded, came back USB on 7.300 with a 5 kHz step.
   Firmware older than this rejects a v3 record and boots on defaults.
30. **Ajustes: upstream's settings screen, with what this port can
   honour.** *21/11/2026, from RAM, seen on the glass* - upstream's
   k_ajustes[] shape (name, page, id; one function for the value, one
   for the tap) in `src/menus.c`, on upstream's pages and names, values
   in upstream's words: Radio - AGC, Ancho, Notch (off + four strengths),
   Reducción, Blanker (off + three), Preamplif., Atenuador; Pantalla -
   Tema, Paleta, Estilo, Volumen; Espectro - Puente, Traza. A tap or the
   knob's press steps an enumeration and the screen stays open; ATT and
   Volumen hand the small knob over. The THEMES are upstream's k_temas[]
   - 18 of them, each a UI palette and a waterfall palette - generated
   from its main.c by deepsdr-ui's `gen_temas.py`, as the bands are;
   choosing one repaints the whole radio (light ones included, checked
   on the panel). The host render measures all 18 theme names and every
   waterfall palette against the cell: 0 overflow. Not kept in the FRAM
   yet; the continuous ones upstream has (squelch, NR strength,
   smoothing) wait for ui_det.c.
31. **Ajustes kept in the FRAM - without breaking the older firmware.**
   *21/11/2026, from RAM* - theme, waterfall palette, style, bridge,
   white trace, AGC, each family's quick filter, notch (on + strength),
   blanker, NR: an EXTENSION in the slot's last 8 bytes, not a record
   version 4. The base stays version 3 with its own CRC, so firmware
   that knows only version 3 reads it as before and never looks further;
   the extension has its own check (16 bits of a CRC-32 over the base
   AND itself), so one left under a base an older image has rewritten is
   ignored. Proven on the radio both ways: the flash's own commit
   (93eaba6), built in a worktree and loaded over this FRAM, came up on
   the stored dial and mode; the new image after it came back with its
   theme, AGC, filter and notch. Applied in two halves - the display
   before the first paint, the demodulator after demod_am_init() (which
   resets the notch and the NR) - and then the baseline is taken, so a
   boot does not save back what it read.
32. **ui_det.c: the continuous settings, and an NR that now does
   something.** *21/11/2026, from RAM* - upstream's detail page ("-" and
   "+" either side of the value, the knob turning it, "Volver" or the
   knob's press back to the grid) at 480 x 320: its geometry and its
   literals `#ifndef` upstream, DEEPSDR renders byte-identical
   (`detalle`, and `detreal` with and without the preset row against
   HEAD's ui_det.c), the value in font_num_31 (font_num_44's own
   character set). Four settings open it: Silenciador (new - upstream's
   -10..60 dB in 2 dB), Reducción (the strength; the Ruido button stays
   on/off), Volumen and Atenuador - those two now through one setter in
   main.c that the small knob uses too. **Found on the way: the port
   never set the NR strength**, and nr_ss_init() leaves 0, which nr_ss.c
   documents as a transparent bypass - so the Ruido button had switched
   on an NR that did nothing. It boots with 48 now (upstream's 50 on the
   16-step grid it is stored on). Squelch and strength are in the FRAM:
   the base record's two spare bytes, trusted only under a flag bit in
   the extension (an extension written before them covers them as
   zeros, which is how 0 came back as a "stored" strength in the first
   try). The extension's version stays 1 - 705749d accepts only 1 - and
   that was checked on the radio: 705749d loaded over the new record
   came up with its theme; the new image after it, with squelch 4 dB
   and NR 80.
   Checked with nobody at the glass: `ui_tap` injects a finger over SWD
   and **`tools/grab.py` reads the panel's own GRAM back** (RAMRD over
   SPI3's MISO, one dummy byte, 9 s a screen) - every screen above was
   seen as the panel holds it, not as a PNG predicts it.
33. **The frequency keypad: a tap on the frequency.** *21/11/2026, from
   RAM, seen on the glass* - upstream's `ui_kbd.c` at 480 x 320: its
   geometry and literals `#ifndef` upstream, proven neutral three ways
   (`PROCEDENCIA.md`: the DEEPSDR keypad renders byte-identical, the
   host and the Cortex-M4 disassembly identical). Its columns are
   ui_grid's, as upstream's are: 4 x 114 + 3 x 4 in the menu area, a
   34-row strip over keys of 114 x 46 (17 x 7 mm); the digits' INK
   centred in the key (12 rows above, 11 below) and the typed number's
   beside the title and hint, measured on the render rather than
   computed from the line box - the first try, from the box, sat them
   2-3 px low. deepsdr-ui `make malahit-menus` renders it, measures
   every label, the longest entry (nine digits and the comma) against
   the hint beside it, and the hit map: 0 failures. `src/menus.c` runs
   it with upstream's entry functions as they are (one comma, Borrar
   back past it, nothing typed is nothing done, nine digits times a
   million in 64 bits); `radio_dial_go()` clamps to the dial as
   upstream's clamps to its own. As upstream: the knob does nothing on
   it, a second tap on the frequency starts the entry afresh, and it
   takes the place of any other screen (the bar's buttons go dark).
   Checked over SWD with `ui_tap` and `grab.py`: 7,155 MHz (with a
   Borrar on the way) tuned 7 155 000 Hz and the pill said 40 m; 165 MHz
   stopped at 30 MHz with "el borde del dial" on the console; MHz on an
   empty entry and after a lone comma did nothing; Vaciar, Volver
   (no retune), over Paso, and Bandas from it all as they should;
   10000 kHz put the radio back on 10 MHz. FRAM format unchanged.
34. **Suavizado, and a settings record of its own: version 4.**
   *21/11/2026, from RAM, seen on the glass* - upstream's third
   continuous setting: an exponential average of the TRACE between
   frames (`s_db_traza`, upstream's name), the waterfall and the S-meter
   left raw, as upstream's are; 0-95 % of weight on the history in 5 %
   steps, 25 % by default (upstream's since 30/10/2026). On the Espectro
   page before Puente, as upstream has it, opening the detail page; the
   number alone in the big digits because neither font_num_31 nor
   upstream's font_num_44 has a '%' (upstream's own page draws "25 "),
   the unit in the line under it. **Measured on the radio**: eight
   dumps of `s_db` and `s_db_traza` over SWD at 40 %, the noise floor's
   frame-to-frame spread 4.97 dB raw and 3.10 dB on the trace - 0.62,
   where an EMA on white noise predicts 0.65 - and the mean level 0.4 dB
   apart.
   **The FRAM record is version 4 now, and older firmware cannot read
   it** - the owner allowed exactly that the same night. The 32-byte
   slot had three bits left that older images ignore, for a setting of
   twenty values. Version 4 is 64-byte slots at 0x4100, a byte a value,
   27 bytes of room, CRC-32; the old slots at 0x4000 are only READ, so
   the first boot of this image came back on the v3 record (seq 150,
   "suavizado is the default") and its first save wrote v4 (seq 151).
   Reloaded: "record version 4, at 0x4100", suavizado 40 % from the
   FRAM; set back to 25. An image before this one boots on defaults
   over a v4 record.
35. **VHF, and WFM with the tuner on the dial.** *21/11/2026, from RAM;
   the paths checked on the radio, and WFM heard by the owner on an
   antenna: "plays fine", in mono - upstream's WFM is mono by design
   (demod_am.h: no room in +/-96 kHz for the 19 kHz pilot and the 38 kHz
   L-R), so no stereo is expected* - the owner's choice of the two ways:
   as upstream does it.
   - **The dial goes to 180 MHz**, upstream's own top (RADIO_TUNE_* in
     `src/menus.h`, which the band screen reads too: FM OIRT, FM, Aérea,
     Meteo, Marina, VHF alta and APRS appear). `msi001_retune()` now
     switches the MSi001's path when a frequency crosses an edge - the
     fast path kept the boot's AM mode, so a VHF frequency was planned
     as a 132 MHz up-conversion. **The edges are the factory firmware's
     and the board's, not the chip's**: AM below 60, VHF 60-120, B3
     120-250 (RE report 5.4), the schematic's filters being "VHF1 50MHz
     - 120MHz" and "VHF2 120MHz - 250MHz"; this port had 150/260 and no
     VHF2 at all. VHF2 is mask 0x2B (BOARD.md's sub-band switch on RF4).
     The filter is chosen by the TUNER's frequency now, as the path is,
     so the two cannot sit on opposite sides of an edge.
   - **WFM**: upstream's `demod_wfm_process_raw()` - its own 192 kHz,
     512-frame hook, exactly this port's chunk, so `demod_block()` calls
     it instead of `demod_am_process_raw()`; upstream's mode-switch order
     without the codec-rate half this board does not need (rf_doble off,
     decimators forgotten, the WFM entry reset or short mute, mix 0). A
     mode change now raises a flag the block interrupt answers with
     silence, where before upstream's switch ran with blocks going
     through it (upstream takes its hook off). **The tuner goes ON the
     dial in WFM** (`s_lo_off` 0, 12 kHz in every other mode), so does
     the spectrum's window (bins 32..479 instead of 64..511), and the
     boot puts it there before its first tune when the FRAM says WFM.
     The BW chip in WFM is upstream's audio low-pass, 15/8/4 kHz, and
     the FRAM keeps it (record v4, byte 27, 0 = not stored).
   - **Checked on the radio**, over SWD: 100 MHz in AM - plan 100 000 000
     Hz exact, LODIV 32, VCO 3.2 GHz, register 0 with VHF_MODE and
     RF_SYNTH, filter 0x28; WFM there - "tuner on the dial", the leakage
     under the centre mark, the hook 6.7 ms of 42.7, no overrun, no
     sink error, no silent block; boot straight into WFM from the FRAM;
     the BW chip round 15 -> 8 -> 4 -> 15 k; 145 MHz - B3, LODIV 16,
     filter 0x2B; back to 10 MHz AM - the generator's carrier at 94 dB,
     as before any of it. Then the owner put an antenna on it and
     listened: WFM plays fine, mono.
36. **English - upstream's two languages.** *21/11/2026, from RAM, seen
   on the glass* - the owner asked how to switch the language and found
   no switch: upstream's idioma.c was in the image all along (ui_top.c
   and spectrum.c call its tr()), but its switch lives on a settings
   page the port had not taken, and every string the port draws itself
   was a Spanish literal. Now: **Idioma / Language** on upstream's own
   page for it, Equipo / Device, a tap toggling it and repainting
   everything as a theme does; every string in `src/menus.c` and the
   ones `src/main.c` draws (the bar, the AGC names, the band pill) as
   T(es, en) / tr(), with UPSTREAM'S English wherever upstream has the
   string - k_ajustes[], k_modos[], k_act_slots[], k_kbd_freq_rot[] -
   and the band, family, step and theme tables' own English columns
   (gen_temas.py now emits the themes' - PROCEDENCIA.md). The port's own
   few written in the same style. Kept in the FRAM, record v4 byte 28.
   Measured on the PC first, every English string against the hole the
   Spanish one is measured in: one did not fit, upstream's "CB and MW in
   the Americas" for the 10 kHz step, 136 px in 114, said "CB, MW
   Americas" here as AM's "Envolvente" is. Then on the glass: the main
   screen, Mode, Bands, Settings, the keypad, all in English, nothing
   clipped; and a cold boot came back in English from the FRAM. Numbers
   keep upstream's format in both ("104.300,00"), as upstream decided.
   Found on the way: a full repaint draws the action bar as `s_act`
   holds it, and the names were set only when the bar was refreshed -
   the first try left the bar in Spanish under an English header.
37. **Touching the spectrum tunes.** *22/11/2026, from RAM, on the air* -
   upstream's spec_tap_tune_to_x() and spec_drag_tune_apply() on this
   port's window, with upstream's `spec_snap.c` compiled in place (a
   pure function; its host bench `snaptest` passes). A TAP lands on the
   signal near the finger - the carrier, or the voice's edge in USB/LSB -
   or, with nothing there, on the step grid; a DRAG pans, the content
   following the finger, in whole steps with the remainder carried and
   the first step onto the grid (upstream's tune_mueve_pasos()). A press
   becomes a drag past 6 px, upstream's threshold. The zone is the trace
   AND the waterfall (upstream's is the trace only, but the waterfall
   here is 92 of the 233 rows). A pixel is a frequency without asking
   where the tuner is: the window is centred on the dial in every mode.
   `swd-call ... ui_swipe x0 x1 y` drags a finger from the bench.
   **On the air, 31 m at night, the carrier positions read from the
   trace over SWD first**: a tap 3 px beside the carrier at +4.5 kHz
   tuned 9 604 566 Hz - the station, not the 5 kHz grid; a tap on empty
   spectrum, 9 665 000 - the grid, as computed; a 100 px drag left
   9 700 000 (+38.2 kHz, seven 5 kHz steps), and back right on the
   waterfall 9 665 000. Found on the way, from step 36 and IN FLASH: the
   console printed garbage for the action bar's button name (a T() pair
   handed to %s; console_printf() has no format attribute, so nothing
   warned) - console only, fixed here.
38. **The memory channels, MEM.CSV in the FRAM.** *22/11/2026, from RAM,
   on the radio* - upstream's banco.c compiled in place, unmodified: four
   banks of fifteen, each memory the frequency, mode, step and filter.
   banco.c reaches its disk through two calls, and `src/memfram.c`
   answers them from the FRAM for MEM.CSV alone - two 3 kB slots at
   0x4200, header and CRC-32, newest valid wins, written data first and
   header last and read back: so the file in the FRAM is UPSTREAM'S own
   CSV, written and parsed by upstream's code. Upstream's screen and
   gestures in `src/menus.c`: the MEM chip in the header opens and closes
   it; banks in the column (upstream's HF / Aircraft / Marine /
   Repeaters, both languages); a tap recalls and closes, a tap on an
   empty cell gives the "no" pip and stays, TWO SECONDS on a cell save
   with the "done" pip; the knob runs across cells and banks, its press
   recalls. Upstream's 16th cell, SCAN, is not here yet. Host render
   `memorias`, 0 overflows; `swd-call ... ui_hold x y ms` holds a finger
   from the bench. **Checked on the radio**: a hold saved 104.3 MHz WFM
   (FRAM slot 0, seq 1), another 9.6 MHz AM (slot 1, seq 2); the CSV in
   RAM read back over SWD is upstream's format line for line; a tap on 01
   from AM put the radio back in WFM with the tuner on the dial and the
   100 kHz step; reloaded, "Memories: 2 in use, MEM.CSV from the FRAM",
   and both recall.
39. **Brightness.** *22/11/2026, from RAM, on the radio* - upstream's
   backlight API (User/backlight.h: percent, a 10 % floor, 20 kHz) on this
   board's pin: `src/backlight.c`, TIM16_CH1 on PB8 (AF1), ACTIVE HIGH (Q1
   is an N-channel gate; upstream's panel lights on a LOW pin). TIM16 is
   reset and every register written. At 100 % CCR1 > ARR and the pin
   never switches - the default is exactly the backlight this board had.
   Brillo / Brightness on Pantalla after Volumen, upstream's place, a
   detail page in upstream's knob range (20-100 % in 5s); in the FRAM,
   record v4 byte 29 (0 = not stored, 100 %). `board_backlight()` goes
   through it, and **the power-off in boot.c puts PB8 back to an output
   before driving it low** - BSRR does nothing to a pin in alternate
   function, so the backlight would no longer have gone dark at once
   under a held power button (confirmed by the owner's hand, 22/11/2026,
   with the brightness by eye). On the radio: TIM16 CR1 0x81, ARR 11999, CCR1 12000
   at 100 % and 6000 at 50 %, BDTR MOE, PB8 AF1; 70 % saved, reloaded,
   came back 8400. **And no PWM spur**: 16 spectra at 50 % against 16 at
   100 % and 16 at 50 % again, A/B in one run over swd-call, at a quiet
   151 MHz - the k x 20 kHz bins within +/-2 dB, the same spread as 50 %
   against itself, the floor's median 62.1 / 62.3 / 62.2 dB.
40. **RDS, and room for it in the header.** *22/11/2026, from RAM, on
   the air* - upstream's header already has a strip for RDS (ui_top.c:
   the station's name over a scrolling line, between the band pill and
   MEM), which on this 480 px header had 0 px. Room made for it, on the
   owner's lines: **the clock and the battery stacked** in one 32 px
   column (clock 1..14, battery 18..31, as upstream stacks them), **MEM
   pulled up against it** and narrowed to its text plus 4 px a side, and
   **the mode and band pills' side air cut to 5 px** from upstream's
   13/11 - those two numbers, the strip's left margin and its two lines'
   offsets were literals in ui_top.c/h and are `#ifndef` now, with the
   old values (PROCEDENCIA.md: the DEEPSDR's 800 x 480 screen identical
   to the pixel, ui_top's disassembly identical on the host and for the
   Cortex-M4). Then the owner's last pixels: the frequency from x = 3
   (its ink at 6, over the S-meter bar's edge), 4 px from "kHz" to the
   mode chip instead of 10, MEM 2 px closer to the battery - three more
   upstream literals `#ifndef` (HDR_FREQ_X, HDR_UNIT_GAP, HDR_FREQ_HIT_M),
   proven the same way. The strip measures **109 px at the widest
   header** (104.300,00 WFM FM) and 146 on HF (deepsdr-ui `cab_wfm`,
   `cab_hf`). On the way: the frequency's touch zone ran 4 px into the
   mode chip (upstream's 12 px after "kHz" against this port's 34 px
   gap); it ends at the chip now - checked, x 219 opens the keypad and
   223 does not. And the bring-up's touch report in the knob pill
   ("Touched" / "Unassigned", left standing after a tap on anything
   unwired) is gone: everything there is wired; the console still logs.
   RDS itself is upstream's: rds.c and rds_pi.c in place of their stubs,
   fed by upstream's WFM hook; upstream's rds_marq_sync()/poll()/
   reinicia() and its PTY table in main.c - name from the PI table, else
   the PS, else the PI in hex; traffic, programme type and radiotext
   scrolling 2 px a frame. On in WFM, off elsewhere, from zero on every
   retune. Not taken: the clock from RDS (this clock is an uptime count).
   **On the air at 104.3 MHz**: the name built two characters at a time
   (the station runs a dynamic PS - "Tiffany Aris - Find Me"); the
   radiotext read over SWD "Pop music   Teddy Swims - Mr. Know It All";
   the strip scrolls on the glass; to AM it empties, back to WFM it
   starts again; a step away and back, from zero. The WFM hook with RDS:
   13.4 ms of 42.7 (6.7 without), no overrun.
41. **FM stereo - this port's own; upstream's WFM is mono by design.**
   *22/11/2026, from RAM, on the air* - `src/estereo.c`, built round
   upstream's mono path rather than beside it: upstream's output is M =
   (L+R)/2 with everything upstream does to it (squelch, de-emphasis,
   the 15/8/4 kHz low-pass, AGC, mutes, pips); this decodes S = (L-R)/2
   from the same multiplex through the SAME DC blocker, de-emphasis and
   low-pass (upstream's constants, copied), and writes L = M + gS, R =
   M - gS, g being the gain upstream's AGC put on M in that block -
   regressed from upstream's output on this file's own copy of M (the
   residual on air -45 dB). A squelch or a mute zeroes M and so g. The
   multiplex comes from where upstream hands it to RDS: rds_feed() is
   linked with `--wrap` and demod.c's wrapper feeds both - not a line of
   demod_am.c changed. The 38 kHz reference is the pilot's phase
   doubled, the pilot found through a 200 Hz low-pass after a 19 kHz
   oscillator; stereo needs a pilot of 2.5 kHz deviation or more and
   blends from mono to stereo between 12 and 24 dB of pilot SNR.
   **The bench first** (deepsdr-ui `make wfmestereo`: synthetic BS.450
   stereo through upstream's real demod_wfm_process_raw()): L 1 kHz, R
   3 kHz - separation 65-75 dB, channels not swapped; a mono station
   stays mono; a weak one blends. It found two things: upstream's DC
   blocker (15 Hz) acts on the multiplex before the tap, so M had it and
   S did not - 0.9 degrees at 1 kHz held the separation at 40 dB until S
   got the same blocker; and a block-averaged pilot let the programme
   in (a noiseless pilot read 29 dB), so it is low-passed instead (92
   dB), and a 1 kHz tone's 19th harmonic read as a "pilot" until the
   deviation threshold. **On the air at 104.3 MHz**: pilot 6.1 kHz
   deviation, SNR 30-33 dB, at 19000.0-19000.3 Hz, blend 100 %.
   **Cost**: 139 + 78 us a call, 3.5 ms of the 42.7 ms block, after
   moving the hot buffers to the DTCM (the D-cache is off) and writing
   the SAI ring once a frame; the frame A/B in one run, 79.4 ms with
   stereo and 71.7 without, at a station whose spectrum keeps the trace
   busy. Mono costs nothing. **"WFM ST"** in the mode chip while the
   blend is at 50 % (off below 40), the owner's choice; Settings > Radio
   > Stereo, Auto / Mono, in the FRAM (record v4 byte 30). And by ear,
   the owner: "the sound is quite satisfactory".
42. **CW - the first text decoder, and the screen every text decoder
   will use.** *22/11/2026, from RAM, against a HackRF on the bench* -
   the owner's order is simple to hard, one decoder at a time.
   **The decoder** is upstream's cw.c, its stubs gone from
   decoders_off.c; demod_am.c already feeds it from the USB branch. CW is
   a mode ENTRY, as upstream's table has it (USB + the decoder), numbered
   after upstream's demodulators: `RADIO_MODE_CW` = 6 (menus.h), so the
   FRAM byte and the memory's `modo` carry it unchanged; a firmware that
   does not know 6 boots on AM. The decoder's switch runs under demod.c's
   mode-change flag (`demod_cw()`), since turning it on clears state the
   block interrupt is using. The mode screen has upstream's second family,
   **Digital**, with CW ("Telegrafía Morse" fits: `make malahit-menus`).
   In CW the BW chip is upstream's CW band-pass (1,0 / 0,5 / 0,25 k,
   record v4 byte 31 in tens of hertz), and a tap on a signal puts the
   dial a pitch below it, upstream's cw_signo_vfo().
   **The bench was a HackRF**, through the 10 + 40 dB pads, amp off, I/Q
   files written by a script with its own Morse table and band-limited
   noise (SNR in 500 Hz, upstream's definition). The two references
   differ by ~9 ppm at 14 MHz (a 575 Hz tone for a 700 Hz pitch), which
   the decoder's probes follow. **On the air**: 12-35 wpm; SNR 5 dB
   clean and 0 dB readable from a fresh start - as good as the host bench.
   **And a real bug in upstream's cw.c, found there**: after a strong
   station, one 5 dB weaker (after the AGC) put its key-down inside the
   two-means tracker's dead zone, which moves neither mean - the high
   mean could never come down, and the decoder printed "E T E E E", or at
   8 dB nothing, for good. Reproduced on the host with cw.c itself; fixed
   in User/cw.c (`CW_KM_MESETA`): a run of a dot or more clearly above
   the noise that never reaches the upper third is a weaker key, and
   three of them in a row let the high mean decay towards the low one and
   close the gate (its history too). Not by time: that first version
   forgot in every PAUSE as well, and on the air the station answering
   three seconds later came in as one long noise "key" and was lost. Two
   new cases in upstream's `make cw` (a strong station then a weaker one;
   a station then only noise, raised 0/10/20 dB) - the unfixed file fails
   the first; the speed x SNR sweep within a point of before, "only noise
   for a minute" still 0 characters, and the +400 Hz interference case
   now decodes at 8-11 % error where it went silent (that silence was the
   same deadlock). On the air, a station, 3 or 8 s of silence and a
   weaker one answering: both decoded, the second losing its first one
   or two letters.
   **The screen is upstream's**, the owner's choice: in a text mode the
   spectrum and the waterfall give their place to the **tone scope**
   (rtty_scope.c's audio spectrum, 0-6 kHz across the width, the pitch
   and the locked tone as lines) over **ui_digi.c's text panel**, eight
   rows in the menus' faces, its bottom where the waterfall's was
   (`src/digi.c`: upstream's rtty_text_push(), digi_sync() and
   rtty_scope_draw(), cut to CW). ui_digi.c's geometry is `#ifndef` now,
   Malahit values in malahit_layout.h (the header's 800 px partition
   scaled and still exact, as its asserts demand); the DEEPSDR's panel
   renders (`out/digi`, `digi_ft8`) identical byte for byte. The chip
   reads "20 wpm 14 dB" - the speed and the margin between the two means
   - with the lock dot; Clear clears. Checked on the glass over SWD
   (grab.py): text wrapping across rows, Clear, USB back to the spectrum
   and the waterfall, Digital > CW from the mode screen by taps. Cost:
   the frame 40.6 ms (24 fps), AXI 427 of 512 kB.
43. **RTTY, on the same screen.** *22/11/2026, from RAM, against the
   HackRF* - upstream's rtty.c and rtty_auto.c (its automatic shift,
   speed and polarity), stubs gone. Upstream's two entries, numbered on:
   **RTTY-L** (7, LSB, config.h's mark 2525 / space 2075) and **RTTY-U**
   (8, USB, the two swapped - USB hears the same radio tones mirrored),
   switched under demod.c's flag (`demod_rtty()`, and `demod_quieto()` for
   any decoder change from the display loop). One deviation: the shift
   the person chose survives entering the mode (upstream sets config.h's
   pair and drops it). src/digi.c's RTTY header is upstream's - Borrar |
   shift | speed | Auto, the bar cut to 100 of 174 px to make room
   (`DIGI_RTTY_BARRA_W`, upstream's 170 of 300), no chip - and the scope
   shows mark and space; each button acts on its press (upstream's 170/
   425/450/850 Hz and 45.45/50/75/100 Bd round, Auto on/off). CW got
   upstream's second button, **Aprende / Learn**. In the FRAM, record v4
   bytes 32-35: the shift in 5 Hz, the speed's index, Auto, Learn.
   `tools/hackrf_gen.py rtty` (ITA2 from ITU-T S.2 by hand, continuous-
   phase FSK, 1.5 stop bits, the shift re-sent after a space).
   **On the air at 14.085 MHz** (dial 14.0873 LSB, the HackRF 125 Hz up
   for the two references): 50 Bd / 450 Hz clean at SNR 5 and 10, at 3
   one figures-shift lost ("1234567890" as "QWERTYUIOP"); 45.45 Bd / 170
   Hz set by hand, clean at 12 but for a few letters. Noise prints
   characters before and after a transmission - upstream's RTTY has no
   squelch. **Four things about upstream's RTTY the bench showed, and the
   owner had three of them fixed in User/** (reproduced first on the host
   with the new `sim/rtty_auto_aire.c` - any f32 file, any starting point -
   on hackrf_gen.py's `--audio` files: a normal and a reversed station at
   50/450, 45.45/170 starting from 50/450, a minute of noise):
   1. **RTTY-L's polarity was the reverse of the convention** (mark the
      HIGHER radio frequency): config.h said mark 2525 / space 2075 under
      two comments of its own saying the opposite ("marca por debajo de
      espacio", "marca + 450"). Now 2075 / 2525. A normal station decodes
      at once; a reversed one still gets flipped by rtty.c's detector -
      but that flip no longer writes the person's setting
      (`rtty_set_station_inverted()`): it is the station's (`s_inv_auto`),
      thrown away on entering the mode and when rtty_auto.c has seen no
      tone pair for three windows (`rtty_olvida_vuelta()`), so the NEXT
      station starts on the convention. On the DEEPSDR a stored "inverted"
      that the old detector wrote is now the wrong way round - the
      detector flips it back per station.
   2. **Auto no longer touches anything without a signal**: a tone pair
      (dos_picos()) in one of the last three windows, or nothing moves. A
      minute of noise used to walk the speed from 50 to 45.45; now nothing,
      and a speed set by hand stays.
   3. **Auto reaches 45.45/170 from 50/450** - it never did. Three causes:
      the tones and the speed were only looked at when the decoder was
      UNHEALTHY, and a wrong shift or a 10 % wrong speed still closes the
      frames (now, healthy or not, a measured pair or a measured speed
      that points elsewhere two windows running is applied); "coherent"
      was "within 8 %", which took a 46.3 Bd measurement for 50 (now the
      nearest in the list must be the one set); and a failed speed went to
      the NEXT in the list rather than to the one measured, with the
      search starting from index 0 whatever was set. And the run lengths
      measured with the tones in the wrong place - the decision flickering,
      "112 Bd" - are thrown away when the tones move, as upstream already
      did on a speed change. Host: 45.45/170 at SNR 15 decodes at 40 s,
      clean after; the reversed station flips in two windows; the normal
      one never flips; noise changes nothing.
   4. deepsdr-ui's `make rttyaire` rule lacked rtty_auto.c and did not
      link - fixed in local.mk on the way. `make rttysolo` (upstream's own
      auto bench) needs a DWD recording this clone does not have.
   **On the air, after the fixes**: from a stored 100 Bd a normal 50/450
   station pulled it to 50 and the tones 62 Hz down onto the signal (the
   two references), 97 % frames; a reversed one flipped and decoded; 45.45/
   170 found from 50/450 and decoded repeat after repeat; 25-30 s of
   silence before and after moved nothing. The console logs each decision
   ("rtty now: mark ... Bd ... flipped by the detector").
   `digi_report` (swd-call) prints RTTY's settings and the scope's two
   peaks - how the 50 Hz left between the two references was measured.
   CW re-checked on the air after the change. AXI 439 of 512 kB.
44. **PSK31.** *22/11/2026, from RAM, against the HackRF* - upstream's
   psk31.c (differential BPSK, a Gardner symbol clock, Varicode, a gate
   at quality 78), stub gone; entry 9 (USB), "Charla en fase" in Digital;
   `demod_psk31()` under the flag. The panel is upstream's: Borrar and
   the chip, which for PSK31 is the FREQUENCY ERROR ("+0,7 Hz") with the
   quality as the dot. Two things are this port's: the scope shows a line
   at the decoder's centre (upstream's scope_rayas() has no PSK31 branch
   and drew RTTY's two), and **a tap on the scope tunes**: the centre goes
   to the power-weighted centre of the two bins either side of the
   strongest within +-60 Hz of the finger - the CENTROID, because idling
   PSK31 is two lines at +-15.6 Hz and the strongest bin is a sideband,
   twice the decoder's +-7.8 Hz away. Upstream tunes PSK31 with the dial
   only. `tools/hackrf_gen.py psk31` (Varicode written by hand, then found
   identical to both upstream copies; cos^2 pulses two symbols long; idle
   preamble, carrier postamble); upstream's psk31.c decodes its `--audio`
   files on the host down to 0 dB in 500 Hz.
   **On the air at 14.071 MHz** (dial 14.070 USB, 1000 Hz): the first run
   printed steady nonsense at quality 96 - the signal was at 987 Hz, the
   two references having drifted another 13 Hz, and outside +-7.8 Hz
   PSK31's phase turns pass for data (upstream's own warning). Found with
   a recorder of what psk31_process() is fed (demod.c, `demod_graba`,
   `--wrap=psk31_process`, 2.5 s in D2 SRAM, built only with
   `-DDEMOD_GRABA=1`): the host decoded the radio's audio the same way at
   1000 Hz and correctly at 987. With a tap: SNR 3 and 0 dB clean, -3 dB
   but for the last two letters; three repeats in a row; error held at
   +1 Hz by its tracking. AXI 444.7 of 512 kB.
45. **NAVTEX, and three fixes to upstream's in User/.** *22/11/2026,
   from RAM, against the HackRF at 4209.5 kHz* - upstream's navtex.c
   (SITOR-B: two tone filters a bit long, a Gardner bit clock, CCIR 476's
   four-of-seven, the repeat 350 ms later), stub gone; entry 10 (USB),
   "Avisos marítimos" in Digital; `demod_navtex()` under the flag. The
   panel is upstream's: Borrar and the chip, which says the phase -
   "buscando", "bits", "SITOR", then the last header ("FA01") - with the
   character sync as the dot. The scope draws B and Y 85 Hz either side of
   where the decoder listens NOW (its setting plus what it measured);
   upstream's scope_rayas() has no NAVTEX branch and drew RTTY's lines.
   `tools/hackrf_gen.py navtex` (CCIR 476 in transmission order, B the
   higher frequency; the FEC frame of M.476/M.625 - DX on the evens, RX
   five positions later, alpha/RQ phasing, alpha at the end) and
   `deepsdr-ui/sim/navtex_aire.c` (`make out/navtexaire`: upstream's
   navtex.c, with the scope, on any `--audio` file). The letters were
   checked against upstream's table, not written from the recommendation -
   only the control signals were; upstream's table is the one proven on
   the air (an Oostende recording). A real station on 518/490 kHz, below
   the HackRF, is the independent test still to do.
   **The owner chose to have three things fixed in User/**, each shown
   first on the radio and reproduced on the host:
   1. **The centre of the tones**, the decisive one. Upstream measures it
      by counting zero crossings after a search window without a lock -
      which measures the centre of the NOISE as soon as the signal does
      not swamp it (a 4 kHz sideband holds 9 dB more noise than 500 Hz).
      On the radio: with no signal the centre wandered to 2295-2357 Hz;
      SNR 15 dB (in 500 Hz) pulled it back to 1741 and decoded; SNR 8
      left it at 2020-2100 and nothing came out. On the host, tones at
      1100 Hz with SNR 10 were "measured" at 2245. Now navtex_espectro(),
      fed from the main loop with the tuning scope's frames (rtty_scope.c,
      23.4 Hz a bin), averages ~1.4 s and looks for the PAIR: each lobe
      (+-45 Hz at c-85 and c+85) against the floor on its own outer side
      (so the SSB filter's edge cannot fake one), the worse of the two
      ratios, 2.0 to pass; the position from the energy of both lobes,
      continuous across bins, refined by a parabola; three searches
      within 15 Hz before a move, none while locked, none in noise -
      without a pair the centre stays where it was. The zero crossings
      stay for whoever gives no spectrum (the recordings bench). Host:
      the centre within 5 Hz from 1100 to 2300 Hz down to SNR 3, the
      text whole down to SNR 3 at every centre (the crossings: nothing
      below SNR 8 except where the noise's centre happened to be);
      shaped noise (300-1800/2300/4000 Hz) scores 1.3-1.55 at most over
      5 minutes. Three things the bench taught on the way: the first frame
      seen is stale (a tone from before stayed in the average ten seconds);
      the exponential average must start as a true mean; and "worse lobe
      against its floor" says WHETHER, not WHERE - its maximum is flat
      over +-35 Hz, and on the radio the centre jumped 1674/1707 and each
      jump reset the bit clock (now a small correction, under 60 Hz,
      keeps the loops).
   2. **A tap on the scope sets the centre and it holds** until the mode
      is entered again: navtex_set_centro_hz() said "the operator rules"
      and gave the operator one search window, 2.2 s. The tap goes to
      the pair the same search finds within +-150 Hz of the finger
      (navtex_centro_cerca(), score 1.6 or the finger itself).
   3. **A bit slip no longer ruins the rest of the message.** The lock
      watchdog lets go below 37 % valid codes; shifted one bit, text
      still gives 45-70 %, and the decoder sat on the wrong offset to
      the end printing garbage - on the radio, 2 emissions of 10 locked
      at quality 57-65 %. Comparing the two copies does not see it
      either: two 7-bit words one bit apart cannot both have four ones,
      so shifted copies that are both valid always agree. What sees it
      is the search's own count, kept going while locked: if another
      offset gets PAR_MIN pairs and 4 more pairs AND 4 more valid codes
      than the one set, in a 224-bit window, the decoder moves there and
      measures the parity again (the rewind keeps what came between).
      Host: a bit lost at character 60 of 151 - the end of the message
      back after ~25 characters; no move at all in clean runs, SNR 15
      to 1, or two minutes of noise. On the radio, after: 10 messages of
      10 clean at SNR 15 and 8, gaps and no gaps.
   And upstream's three NAVTEX band rows put the dial ON the published
   frequency - the centre of the emission - which in USB leaves the tones
   at +-85 Hz of audio: nothing came in through them (the same mistake
   upstream fixed for its fax rows on 21/10). Now 516.3, 488.3 and
   4207.8 kHz, labelled with the dial, as the fax rows are.
   **On the air after the fixes** (dial 4207.8 USB, the HackRF and the
   radio 26-29 Hz apart here): noise - the centre stays at 1700, score
   1.1-1.2; SNR 8 - found at 1672, both repeats clean; SNR 3 - header
   and text but for a lost figures shift; SNR 0 - nothing (upstream's
   decoder needs ~2 dB); the station 400 Hz up - 2072, 500 Hz down -
   1182, clean. Upstream's bench (`make navtex2`) has two new sections,
   8 (the centre in the spectrum, noise alone, the operator's centre)
   and 9 (a bit lost), all green; the DEEPSDR's firmware builds and its
   29 firmware checks are green. Left as upstream has them: a few seconds
   of garbage after each emission ends (the lock is let go by the
   validity watchdog), the chip keeping the last header after the lock
   is lost (upstream's "old but true"), and a band button setting USB,
   which switches the decoder off. AXI 454.4 of 512 kB.
46. **DSC, four fixes to upstream's, and a fifth band family.**
   *22/11/2026, from RAM, against the HackRF at 8414.5 kHz* - upstream's
   dsc.c (HF DSC, ITU-R M.493: NAVTEX's tone filters and bit clock,
   ten-bit characters, DX/RX two pairs apart, the ECC), stub gone; entry
   11 (USB), "Llamada marítima" in Digital; `demod_dsc()` under the flag,
   saying so on the console if the decoder refuses to start. Its state
   lives in "the borrowed RAM" (User/hfdl_ram.h): upstream lends the
   DEEPSDR's waterfall pixels, which this port's main.c keeps writing in
   every mode, so `src/prestamo.c` lends 8 kB of D2 SRAM instead - the
   same three calls; WSPR and HFDL will need it grown to 66 kB, and AXI
   is untouched by it (tools/prestamo_check.py skips it as it skips
   hfdl_ram.c: it defines the lock). The panel gets each call whole when
   dsc.c counts it, as upstream's dsc_poll_main() does; the chip, which
   upstream does not have for DSC, says searching / sync / call, the lock
   as the dot; the scope draws B and Y round the centre.
   `tools/hackrf_gen.py dsc` writes calls from the recommendation - ten-
   bit characters, dot pattern (`--dots`, 200 or 20), phasing 125 /
   111..104, format specifier twice, the ECC with it once - individual,
   all-ships or a distress alert; upstream's decoder read the first one
   whole, ECC included. `deepsdr-ui/sim/dsc_radio.c` (`make
   out/dscradio`) runs dsc.c the way the radio does, with the scope and
   dsc_poll() every 15 blocks - upstream's dscaire has neither, and they
   are what moves the centre.
   **On the host, as on the radio, upstream lost calls**: SNR 5-20 dB in
   500 Hz, centres 1100/1700/2300 Hz, 48 of 72; with a 20-bit dot pattern,
   what many HF calls send, half. Four causes, each found by tracing, and
   the owner had all four fixed in User/dsc.c:
   1. The centre of a single scope frame jitters (1676 / 1723 Hz for
      tones at 1700) by more than the 25 Hz that moves it, and every move
      threw the character-phase search away - it locked only in the gap
      between two jumps. A move under 60 Hz now retunes the tones and
      keeps the search.
   2. The search's counts (punt[]) were never cleared while hunting, so
      minutes of noise filled them with chance sync characters until one
      phase was one short of four; then anything locked it on the WRONG
      phase, the call came in as garbage and the decoder filled its
      message for 8 s until it overflowed. Counts now live in 160-bit
      (1.6 s) windows; the phasing gives 14 sync characters in 1.4 s.
   3. The bit clock's integrator, with upstream's high gain, reaches its
      +-2 % clamp in a few dozen bits of noise: a call came in with the
      clock 1.6 % slow and slipped a bit right after the lock (the
      phasing's 109 read as 54). The clamp is NAVTEX's +-0.5 %, and the
      integrator is cleared at the lock and with each window.
   4. A peak had to be 4x the frame's mean, which noise alone gives five
      bins a frame: the centre moved 502 times in two minutes of noise,
      and the scope's lines with it. Now 8x: 20 times, calls the same.
   Host after: 71 of 72 at SNR 5-20 (36 of 36 short-dot calls at SNR 10,
   34 at 5 against 32 and 30 halfway); `make dsc` (upstream's dsctest) has
   a new section 6, audio through scope and dsc_poll() with 40 s of noise
   first and four short-dot calls - upstream's file gives 2, this one 4.
   The real recordings upstream checked dsc.c against are not in this
   clone: the +-0.5 % clamp is the one change that could matter to a
   recording with a wrong sample rate.
   **On the air** (dial 8412.8 USB): 20 calls of 20 with the check right,
   SNR 10 and 5, long and short dots, the station 400 Hz up; before the
   clock fix, 2 of 4 short-dot calls at SNR 5. The search finds 1664-
   1699 Hz for a station the references put at ~1655.
   **The owner asked for the six GMDSS distress and safety DSC channels**
   (2187.5, 4207.5, 6312, 8414.5, 12577, 16804.5 kHz) in upstream's band
   table, dial 1.7 kHz under as NAVTEX's. A family holds 16 cells and
   Utility had 11, so - the owner's choice - a fifth family, **Marítimo /
   Maritime**, takes the three NAVTEX rows and the six DSC rows (Utility
   keeps 8): BAND_FAM_MAR in User/main.c, on both radios; the port reads
   the families from the generated table and needed nothing. The DEEPSDR
   builds, its 29 firmware checks and its band-screen bench are green.
   Left as upstream has it: dsc.c's zero-crossing centre (upstream's own
   note says it does not work for DSC) moves the centre to the noise's
   ~2300 Hz until the first pair is found; and the search still moves it
   now and then in noise, ~10 times a minute. AXI 462.2 of 512 kB, D2
   136 of 288.
47. **WEFAX, and the third mode family, Images.** *22/11/2026, from RAM,
   against the HackRF at 7880 kHz (band FAX 8, dial 7878.1)* - upstream's
   wefax.c and disc_fm.c, stub gone; entry 12 (USB), "Mapas del tiempo"
   in a new family on the mode screen, **Imágenes / Images** (menus.c
   FAM_IMG); `demod_wefax()` under the flag. Its eight-line ring (6 kB)
   lives in the borrowed RAM with DSC's state, so the mode change now
   switches every decoder OFF before it switches one on - DSC to WEFAX
   would otherwise have had both on the same 8 kB for a moment.
   **The screen is upstream's way** (fax_panel_linea()/fax_hdr_draw()),
   the owner's choice: the chart is kept nowhere but the panel's own
   memory, a line at a time in a ring the height of the area (211 rows
   under the header) with a red line on the row that comes next; a menu
   wipes it and the next row starts at the top (upstream keeps its charts
   on a USB stick, which this port does not have). The header is
   ui_digi's: **Sync** (wefax_sincroniza() + wefax_fuerza_imagen(), for a
   chart caught half-way), where upstream has its save button the **line
   rate** (60/90/120/240 lpm, upstream's Ajustes setting; record v4 byte
   36), and the chip with the phase and the slant correction ("Receiving
   +12"). **The scale is this port's, also the owner's choice:
   proportional** - an IOC 576 chart has square pixels, 1809 a line, so a
   screen row is 1809/480 = 3.77 lines averaged; upstream draws a line a
   row, 2.4 times taller than wide. `src/digi.c` (digi_fax_poll() from the
   main loop takes the lines out even under a menu; gfx_blit() a row).
   `tools/hackrf_gen.py wefax` (start signal - black and white alternating
   at 300 Hz, 5 s -, phasing with a 5 % white pulse, a test chart with a
   grid, circles, a grey wedge and R1TEST, the stop signal; `--ppm` for a
   transmitter running fast) and `deepsdr-ui/sim/wefax_aire.c` (`make
   out/wefaxaire`: upstream's wefax.c on an --audio file, the lines to a
   PGM) - upstream's decoder drew the chart straight and aligned.
   **On the air**: SNR 25 - straight, the circles round, the wedge's ten
   greys, 12 ppm measured (the two references); SNR 15 with the
   transmitter 80 ppm fast - 98 ppm measured, straight, a little grain.
   Sync, the line-rate button round and back to 120 (saved each time) and
   a menu over a chart: as above. Not done: wfx_recoloca.c, upstream's
   re-centring of a finished chart, which works on the saved file. AXI
   474.5 of 512 kB: the RAM image has 37 kB left.
48. **SSTV and SSTV-FM, as upstream has them.** *22/11/2026, from RAM,
   against the HackRF at 14.230 MHz and 145.5 MHz* - the owner asked for
   SSTV and HELL "as they are". Upstream's sstv.c, stub gone; entries 13
   (USB) and 14 (NFM: the same decoder from demod_am.c's FM branch) in
   Images - SSTV-FM's description shortened, upstream's is 96 px in a 90
   px cell. Its output queue (~13 kB) is in the borrowed RAM, now 16 kB.
   The screen is upstream's way, as WEFAX's: each line to the panel as it
   comes, nothing kept, a menu wipes it; 320 x 256 does not fit the 211
   rows, so the picture is 263 x 211, centred (proportional, as the fax).
   The header: Empezar / Start, and where upstream has its save button a
   mode choice - Auto, then each mode - that applies at once, as
   upstream's mode grid does. `tools/hackrf_gen.py sstv` (Martin 1 and
   Scottie 1 from the mode specifications, the VIS header, a test picture
   with colour bars) and `--fm DEV` for any of its signals as narrow FM;
   `deepsdr-ui/sim/sstv_imagen.c` writes what sstv.c decodes to a PPM.
   **On the air**: Martin 1 at SNR 40 - the bars, the ramp, a round
   circle, two or three streaked lines; Scottie 1 at SNR 25 - not
   started; at 35 - received, but its lines jitter (77 syncs thrown
   away); SSTV-FM at 145.5 MHz, Martin 1 as NFM +-3 kHz - received, a few
   streaks. **Every picture was found by its line rhythm, none by its VIS
   header**, and that is upstream's discriminator (disc_fm.c, shared with
   WEFAX): it moves the audio down by 1900 Hz and smooths with two 12-
   sample moving averages, whose response falls to zero 1000 Hz from the
   centre - the VIS bits' 1100 Hz is 25 dB down, 1300 Hz 12 dB, the 1200
   Hz sync 17 dB. On the host the header decodes clean and at SNR 40, not
   at 30 (the 1100 Hz bits read 1205); on the radio the two references'
   ~70 Hz pushes the tones further down that slope. Left as it is, the
   owner's word; the fix, if wanted, is a filter that passes 1100-2300 Hz
   for SSTV without touching what WEFAX's chart needs.
49. **Feld-Hell, as upstream has it.** *22/11/2026, from RAM, against the
   HackRF at 14.064 MHz* - upstream's hell.c, stub gone; entry 15 (USB),
   "Texto dibujado" in Images; state in the borrowed RAM; started from
   zero on every entry, as upstream. The screen is upstream's: each column
   of 14 points twice, one copy over the other (Hell has no vertical sync:
   with two copies a whole letter is always there), 2 px wide, in strips
   across the width - six of 240 columns here - the next strip cleared as
   the writing reaches it; Borrar clears. `tools/hackrf_gen.py hell` (122.5
   Bd, 7 x 14, columns bottom to top, a 5 x 7 font of its own with the
   rows doubled) and `deepsdr-ui/sim/hell_aire.c`. **On the air**: "CQ CQ
   DE R1TEST R1TEST K" at SNR 15, read in the strips. AXI 491.7 of 512
   kB.
50. **JTTY, WSJT-X's teletype.** *22/11/2026, from RAM, against the
   HackRF at 14.0915 MHz* - upstream's jtty_rx.c, jtty_fec.c, jtty_msg.c,
   jtty_modo.c and dxcc.c (the country of a callsign), stub gone; entry 16
   (USB), "Teletipo digital" in Digital. 4-GFSK frames of 34 bits at 31.25
   Bd, a tail-biting convolutional code whose Viterbi runs from the main
   loop (jtty_modo_poll()), atoms joined into messages by frequency. Its
   spectrum ring and lines are 52.4 kB of borrowed RAM (jtty_rx_ram_t alone
   49 504 bytes here): the pool in D2 is 56 kB now - 48 was refused on the
   radio, and demod.c said so. Three things upstream's code asks of the
   radio, given by this port: `waterfall_prestada()` (src/prestamo.c - the
   pool's lock), `rtc_hw_get()` (src/reloj.c - the time since power-on,
   which is what the header shows too: this port has no settable clock
   yet, so JTTY's "hh:mm" is uptime), and `g_msticks`, upstream's
   millisecond count, now the name of board.c's own counter.
   **The panel is upstream's FT8-style table** over the whole area -
   Time, Offset, Qual., Message, Country, zebra rows, the newest last,
   Borrar - without its frequency button and Map (no JTTY frequency list,
   no maps here). The column positions were 800-px numbers in User/
   ui_digi.c: now `#ifndef UDG_COLS_JTTY` there with upstream's values,
   and compat/malahit_layout.h's { 4, 38, 90, 120, -476 } (the country
   right-aligned) - the DEEPSDR's panel renders and its 800 x 480 screen
   byte-identical, the gfx2 copy in step, the DEEPSDR builds, 29 checks
   green. `tools/hackrf_gen.py jtty` sends the six normative messages with
   WSJT-X's OWN data symbols (from the vectors deepsdr-ui/sim/jtty_oro.h
   generates out of tbcc.f90 - an encoder independent of upstream's), the
   13-symbol sync, 4-GFSK BT 1.0; `deepsdr-ui/sim/jtty_radio.c` runs the
   whole mode on any --audio file. Host: all six at SNR 0, none at -10.
   **On the air**: the six at SNR 0 dB in 500 Hz - "CQ K1ABC CQ" with
   "USA", HELLO, FN42, 599 123, AGN?, 599 CA - offset -93 Hz (the two
   references at 14 MHz); a station 200 Hz up read at +109. Also the
   SWD-entry keep-alive in demod.c is two asm statements now (a fifteenth
   register operand did not fit). AXI 506.3 of 512 kB, D2 184 of 288: the
   RAM image has 5.7 kB left - from the next decoder on, flash over SWD
   (WORKING.md, "Flash over SWD").
51. **ALE, the soundings of HF.** *22/11/2026, from RAM, against the
   HackRF at 7102 and 14109 kHz* - upstream's ale_rx.c, ale_golay.c and
   ale_modo.c (2G ALE, MIL-STD-188-141 appendix A: 8-FSK at 125 Bd, eight
   Goertzel on the 12 kHz sideband, the 2-of-3 vote over the three copies
   of each 49-bit word, two extended Golay (24,12), words joined into
   addresses by their preambles), stub gone; entry 17 (USB), "Sondeos de
   HF" in Digital; `demod_ale()` under the flag. Its table of stations
   (1.1 kB) in the borrowed RAM; ale_modo.c includes ft8_shared_ram.h for
   a sizeof, so that one file gets `-I Ft8Lib` (nothing of FT8 links).
   **The screen is upstream's table** over the whole area, as JTTY's -
   Callsign, Type, kHz, Qual., N, in the order heard, Borrar - with its
   bar: the quality of the last word (unanimous votes of 48) while locked,
   else "searching for a word" and, once something has been heard, "last
   one N min" (upstream's clock, kept in src/digi.c as upstream keeps it
   in its main.c). The columns through a new `#ifndef UDG_COLS_ALE` in
   User/ui_digi.c, { 4, 168, 240, 312, -476 } in malahit_layout.h -
   fifteen 'W's, the longest address, 150 px in 164; the DEEPSDR's
   ui_digi.o, its digi screens and its 800 x 480 screen byte-identical,
   the gfx2 copy in step, the DEEPSDR builds and its 29 checks are green;
   `make malahit-menus` renders the ALE screen (`malahit_ale*.png`) and
   measures every field against its column. Two things of this port's:
   upstream's bar said "calidad" in English too, here "quality"; and it
   repainted the bar only when the quality moved, so an unlock kept
   "calidad 48/48" up to a minute - here the bar and chip repaint when
   their text or the dot change, and the table when one of its rows
   does (a station closed by silence did not repaint upstream's until
   the next word).
   **Upstream's poll counts passes, and this port's loop is not
   upstream's**: ale_modo.c closes the address it is joining after 200
   polls with no word, 3.2 s in upstream's 16 ms loop; this port's loop
   on the decoders' screen runs at whatever its drawing costs (4 ms
   passes in the console), and under ~2 ms a pass "TIS EA4" would be
   noted alone before its "DATA XYZ" came 392 ms later. So
   `digi_ale_poll()` calls ale_modo_poll() once per 16 ms of the clock,
   however many passes that is - upstream's timing on any loop.
   `tools/hackrf_gen.py ale` writes soundings from the standard's own
   description, not from ale_*.c (tribit-to-tone table, word layout,
   Golay by polynomial division - its 4096 codewords weigh {0:1, 8:759,
   12:2576, 16:759, 24:1} - B's check bits inverted, the interleave, the
   stuff bit, the bit-level triple copy): `--call`, `--to`, `--twas`,
   `--words`, `--sounds`, `--offset-hz`. `deepsdr-ui/sim/ale_radio.c`
   (`make out/aleradio`) runs the mode on a file with the radio's 16 ms
   poll. What the two could share by mistake (the Golay's bit order, the
   place of its overall parity) only a real station settles.
   **Host, as upstream has it**: every word at SNR 2 dB in 500 Hz, most
   at 0; but the tones' bins are fixed and hold +-80 Hz of mistuning at
   SNR 10 (+-50 at SNR 4, +-25 at 2), nothing at 90-100. **On the air**
   at 7102 kHz, SNR 10: 36 words of 36, quality 48; at 14109 kHz the two
   references are ~80 Hz apart (JTTY measured -93): every word, but at
   quality ~40; the HackRF 60 Hz up, 48; 60 Hz down, nothing - a radio
   ~6 ppm off would hear little ALE from 14 MHz up.
   **So, the owner's word (23/11/2026), an AFC - in User/ale_rx.c, under
   `ALE_CAF`, OFF by default** (the DEEPSDR's flash is full: with it on,
   1116 bytes over; off, the DEEPSDR builds as before, +46 bytes, its 29
   checks green), ON in this port's reglas.mk and in deepsdr-ui's ALE
   benches. It moves the eight Goertzel, not the radio:
   - **tracking**: on each clear symbol two Goertzel at +-62.5 Hz of the
     winning tone - for a rectangular window the ratio of their
     magnitudes gives the error exactly within half a bin; each tone keeps
     its own mean error and the AFC moves 8 % of the MEDIAN of them a
     symbol (a carrier at -6 dB next to one tone pulled the plain mean
     onto the carrier: +51 Hz for a station at +120);
   - **search**: beyond ~75 Hz the symbols are not clear - upstream's
     symbol-phase election goes to windows straddling two symbols, whose
     half-length tones have twice the lobe - so each symbol also measures
     one of four other grids (+-62.5, +-125 Hz), each at its OWN best of
     the four phases (measured at the current grid's phase, all four came
     out with the same energy); the energy of each tone in each grid,
     averaged ~100 symbols; a grid "has signal" when six of its eight
     tones are 30 % over the floor (the poorest grid) AND within a fifth
     of its strongest tone (a strong carrier lights every bin with its
     sidelobes, but its own bin is 20-50 times the rest); another grid
     with signal and 25 % more energy than this one for 16 symbols in a
     row takes the AFC;
   - **edges**: a grid 250 Hz off holds seven of the eight tones; the
     eighth falls 250 Hz below the first or above the last, where an
     aligned grid sees only noise - averaged there, the AFC jumps 250 Hz.
   It holds its value without a signal (it is mostly this radio's own
   error on this frequency) and goes to zero when the dial's kHz change
   (ale_modo_frec_pon()); the bar shows it ("  AFC -81"), the console
   too. **Host, with it**: all of +-400 Hz at SNR 10 (26-36 words of
   36), 18-36 at SNR 4, the middle and parts of the rest at SNR 2; thirty
   minutes of noise, carriers at SNR 40 (1110, 1690, 1750, 2000 Hz),
   RTTY, PSK31, CW, JTTY: the AFC stays at 0. Upstream's bench `make
   aleaire` has a section 7 (+-150 and +-300 Hz, noise, a carrier);
   `ALE_TRAZA=1 out/aleradio` prints every move. Cost on the radio: the
   worst upstream call 932 us with ALE, 759 in USB, of 2667 - no SAI
   overrun. **On the air, with it**, 14109 kHz, the HackRF uncorrected:
   the AFC finds -81 Hz, every word at quality 48; with the HackRF a
   further -150, +150, -250 Hz: -231, +66, -333, 30-34 words of 36 each,
   the AFC moving up to 400 Hz from the last value; SNR 4 from zero: -72
   then -81, 24 of 36; TO MIAMI + TIS EA4XYZ at +150 Hz, SNR 8: 36 of 36.
   Before the AFC, with the HackRF 60 Hz up: a 15-character TWAS address
   after a TO MIAMI (the '@' fill stripped), and SNR 4 and 2 - every
   word, quality 45 and 37. Borrar, a menu over the screen during a
   sounding, JTTY <-> ALE <-> USB: as they should. AXI 518.2 of 524.3 kB
   in the RAM image (6.1 kB left); the flash image 292 752 bytes.
52. **STANAG, and four fixes to upstream's 4285 acquisition.**
   *23/11/2026, from flash, against the HackRF at 14109 kHz* - upstream's
   stanag_det.c, stanag_rx.c, s4415.c and stanag_modo.c, with HFDL's
   hfdl_viterbi.c (the same K=7 code), stub gone; entry 18 (USB), "Módem
   OTAN" / "NATO modem" in Digital (upstream's "Módem naval OTAN" is 92 px
   in a 90 px cell). Its three jobs behind one waveform button share 32-39
   kB of the borrowed RAM. **The screen is upstream's** over the whole
   area: Borrar; the waveform round (Auto / S4285 / S4529 / S4481 / S4538 /
   S4415); with the detector, its zebra table Period / Signal / Quality
   (columns through a new `#ifndef UDG_COLS_IDENT` in User/ui_digi.c, {
   4, 72, 340 } here; IDENT shares them) whose first row is upstream's loop
   instrument; with the 4285, its text and speed / interleave / format;
   with the 4415, text and format. The loop instrument's three numbers
   (`ui_bucle_peor_us()` and its two sisters) are measured in this port's
   main.c - the worst pass, cw_poll() and the decoders' drawing - and
   cleared on entering the mode. Three things of this port's: upstream
   repaints the panel four times a second, ~90 ms each on this SPI panel,
   so here each beat goes through `ui_digi_draw_cambios()` (only what
   changed); upstream's bar of the interleaver's filling shares its slot
   with the waveform button and is left out (the filling is in the chip
   and the last row); and the speed button skips what does not fit -
   upstream's round stuck at 1200 for good with the long interleave,
   because 2400/long does not fit the 32 kB and stanag_modo_vel_pon()
   keeps the speed it had.
   **The bench**: `deepsdr-ui/out/stanaggenera` writes a 4285 emission
   from upstream's own sim/stanag_tx.h (the inverse of stanag_rx.c,
   validated round trip against the air recording, which this clone does
   not have), `tools/hackrf_gen.py audio --in f.f32 --tone 1800` sends any
   12 kHz audio file as USB, and `deepsdr-ui/out/stanagradio f.f32 S4285
   600 S` runs the mode the radio's way (`STANAG_TRAZA=1`: its state each
   second). The identifier named the 4285 on the air at once - "106,67 ms
   S4285 / S4481 PSK 100%".
   **The 4285 read nothing on the air** - 18 % of frames good, probes at
   50 %, the text never - though the decoder's input, recorded on the
   radio (`DEMOD_GRABA=1`, now also wrapping stanag_modo_mete()), matched
   the transmitted waveform at 0.996 shifted -84 Hz. The difference was
   the decoder's own state: started with the signal, upstream's demod read
   266 frames of 266; started after some seconds of noise, as a radio
   always is, it was a lottery - 3 s of noise, 95 of 259; 20 s, 39 of 238;
   25 s, none. Four causes in upstream's stanag_rx.c, each found by
   tracing on the host, all four fixed there (the owner's yes, 23/11/2026):
   1. **The preamble's echo.** Its 80 symbols are a period-31 m-sequence
      repeated, so the template 31 symbols off matches 49 of 80: 0.56 when
      the true peak is 0.91, almost twice the 0.30 threshold. The sweep
      could win the echo, the +-4-sample refinement cannot leave it, and
      tracking holds it forever (0.56 passes). `quita_alias()` checks the
      positions 155 samples either way after every acquisition, climbing
      while it improves (the sweep sometimes stops two echoes away), and
      refines time and carrier again where it lands.
   2. **Half the frame was never searched.** Acquisition starts with 1744
      samples ahead of the reading point, and each position had to have a
      whole frame written behind it (cabe_barrer()): only the first
      460-700 positions of the 1280, and always the same frame phases,
      since the reading point moves a frame at a time; the "other half"
      the sweep alternated to was almost never searchable. Now
      `cabe_correlar()` lets every position whose 400 correlation samples
      are written be searched - a whole frame - with two carriers a turn
      instead of four (the same cost), the halves gone; a winner without
      its whole frame yet waits, as upstream already handles.
   3. **A lock that gives no probes was kept.** Eight frames in a row with
      the probes under SONDEOS_MIN (0.85 s) now drop the lock and the
      carrier.
   4. **The fast relock believed the old carrier.** A weak lock made on
      noise or across the emission's start could leave the carrier 50 Hz
      off, and the fine residual (atan2/31, univocal to +-38.7 Hz) reads
      50 as -27 and pushes further out; the fast relock kept finding that
      carrier. `busca_rapida()` now refines the carrier +-50 Hz as the full
      sweep does.
   After the four, on the host: noise 0-50 s before the signal, offsets
   -82 / 0 / +60 Hz - 30 cases of 30 locking, 90-100 % of frames good
   (most 98-100 %); text at 600 S down to SNR 6, at 1200 S and 300 L.
   Upstream's benches that can run here (stanagdet, stanagmodo,
   stanaglatido, stanagaire, stanagfft, stanagfase, stanagtabla) green;
   the ones that want the air recordings (stanagrx, stanagbusca, s4415,
   stanagmulti) cannot run in this clone - so the fixes are proven on
   synthetic signals and the HackRF only. The DEEPSDR builds (+520 bytes)
   and its 29 checks are green; its screens byte-identical.
   **On the air, from flash**, the radio listening 20 s first: 600 S - the
   text, 227 frames good of 234, offset -81 Hz; 1200 S - 255 of 256 and
   the text; 300 L - 342 of 342 and the text. The 4415 has no generator
   here and is untested on the radio. Flash image 333 472 bytes.
53. **IDENT, "what signal is this?".** *23/11/2026, from flash, against the
   HackRF at 14109 kHz* - upstream's ident.c on STANAG's detector (already
   grafted), stub gone; entry 19 (USB), "¿Qué señal es?" / "What signal
   is this?" in Digital (90 px of the 90). Nothing of upstream changed.
   The screen is upstream's: rows over the whole area without columns -
   the names that fit, their notes, the five numbers measured, the
   cadence - Borrar, the chip "measuring" / "listening", the dot when a
   width is measured; repainted through cambios() four times a second.
   Every signature's name and note measured in font_ui_10b: the widest
   371 px of 472. ident.c is the first grafted file that formats with
   snprintf(), and newlib's would drag its stdio and the system calls
   this firmware has not: `src/snprintf.c` has the conversions ident.c
   uses (%d %u %s %% and a zero-padded width), checked against glibc on
   all of them, truncation included - its first version looped for ever
   when a string was cut (a macro that took its argument only when there
   was room), found on the host. `deepsdr-ui/sim/ident_radio.c` (`make
   out/identradio`, `IDENT_TRAZA=1`) runs the mode on a file.
   **Host, hackrf_gen.py's signals**: amateur RTTY 170/45.45, FSK 850/50
   (naval), NAVTEX or SITOR-B, ALE 2G, STANAG 4285, a lone carrier - each
   named; PSK31 read as a carrier or CW, CW as "nothing I know" (upstream's
   table and measures, left as they are); JTTY has no signature.
   Upstream's identtest and identancho green (detvel wants recordings).
   **On the air**: RTTY "Amateur RTTY", ALE "ALE 2G", STANAG "STANAG 4285
   or 4481 PSK ... put it in STANAG"; NAVTEX between "NAVTEX or SITOR-B
   (tones unclear)" and "matches no signature". The empty band after:
   "no measurable structure ... could be HFDL" - upstream's answer to
   wideband noise. Flash image 348 600 bytes.
54. **APRS and PKT300 - AX.25 packet.** *23/11/2026, from flash, against
   the HackRF at 144.800 MHz and 14.109 MHz* - upstream's ax25.c, stub
   gone; one decoder, two entries in Digital as upstream: 20 APRS
   ("Paquete AX.25", NFM, 1200 Bd AFSK 1200/2200 Hz from the FM
   discriminator at the RF path's rate, demod_am_get_active_fs_hz()) and
   21 PKT300 ("Paquete de HF", LSB, 300 Bd FSK 1600/1800 Hz from the 12
   kHz sideband). Nothing of upstream changed. `demod_ax25(1|2|0)`. The
   frames go to the decoders' text panel as upstream's ax25_poll() does
   (and to the console as "ax25: ..."); the scope's two lines are the
   decoder's own mark and space (upstream's scope_rayas() branch); the
   chip, which upstream has none of here, the frames with their check
   right ("4 ok"), the dot ax25.c's "flags seen lately". The Digital
   family holds twelve now; AIS, FT8, WSPR and HFDL make it sixteen, its
   ceiling.
   `tools/hackrf_gen.py ax25` writes UI frames from the AX.25 2.2 and
   Bell 202 texts, not from ax25.c: addresses shifted and SSID-terminated,
   control 0x03 / PID 0xF0, the FCS (CRC-16/X.25: "123456789" gives
   0x906E), bit stuffing, NRZI, continuous-phase AFSK; `--hf` for PKT300,
   `--fm DEV` for APRS on NFM, and `--lsb` now also mirrors the I/Q for an
   LSB dial (the HackRF at dial - tone - 200 kHz).
   `deepsdr-ui/sim/ax25_radio.c` (`make out/ax25radio`) runs ax25.c on a
   file. Upstream's ax25test green. Host: both clean 4 of 4, the mirrored
   PKT300 too; PKT300 holds +-40 Hz of mistuning at SNR 20, some frames at
   +-60, none at +-80.
   **On the air**: APRS at 144.800 MHz, NFM +-3 kHz - 4 frames of 4,
   "R1TEST-9>APRS,WIDE1-1:!5545.00N/03737.00E-Malahit test 1..4". PKT300
   at 14.109 MHz LSB, SNR 20 - nothing with the HackRF where the dial
   says (the two references ~81 Hz apart there, past the +-40), 4 of 4
   with it 81 Hz up. Upstream has no AFC for it: the scope's lines are
   for tuning by hand.
   **So, the owner's word (24/11/2026), an AFC for PKT300 - in User/
   ax25.c under `AX25_CAF`, off by default** (the DEEPSDR builds the same
   size without it, its 29 checks and ax25test green), on in this port's
   reglas.mk; not for APRS, whose FM discriminator already takes the
   tuning error. It moves the two NCOs, not the radio:
   - **the measure**: when one tone dominates (four times the other's
     power, and the pair not faded under a tenth of the level), its
     correlator - the tone brought to zero and averaged a bit - turns at
     the error's rate; the cross product of two successive sums gives the
     turn without atan2. Far off, the tone moving towards the other stops
     dominating, the one moving away dominates more: one is always there;
   - **when it is believed**: three HDLC flags in a row (the TXDELAY) or a
     frame that passes its check - but past ~80 Hz the HDLC sees no flags,
     so also the scope's spectrum, `ax25_espectro()` from the main loop as
     navtex_espectro(): the mean level within +-400 Hz of the centre
     against 600-1100 Hz from it, averaged ~16 frames, over 1.6, the frame
     itself over 1.2, the centre's +-150 Hz hump too, and 300 Hz at least
     of bins over twice the outside. Each condition came from a failure:
     one frame alone passes 1.5 one time in ten in white noise (14 062
     frames measured) and ten minutes of noise walked the AFC 41 Hz; a
     PSK31 at the centre took it 107 Hz; a fading emission's last sums
     read +156 Hz from 1e-7 of energy. The spectrum only says "there is
     signal" - with the TXDELAY one tone is on seven eighths of the time
     and the spectrum's centre of mass sits 75 Hz off. The loop decides,
     the block interrupt applies (s_caf_pide): no race on the sums.
   Half the error a step, 30 Hz a step at most, 250 Hz from the tones at
   most; it holds without signal. The chip says it ("4 ok -81 Hz"), the
   scope's lines move with it, the console logs each 10 Hz.
   **Host**: offsets -200..+200 Hz - every frame at SNR 20, the decoder's
   own 2-4 of 6 at SNR 10, the AFC within ~10 Hz of the truth; ten
   minutes of noise, a carrier, PSK31, CW - 0 Hz; RTTY 170 and NAVTEX
   ~15 Hz; an 850 Hz FSK still drags it (+143), and it comes back from
   there (capture +-200). **On the air**: the HackRF where the dial says
   - 4 of 4, the AFC at +85 (the references' 81); 100 Hz up - 4 of 4,
   -25; 100 Hz down - 4 of 4, +169..+193. APRS after it: 4 of 4, twice.
   Flash image 353 904 bytes.
55. **AIS - the ships.** *24/11/2026, from flash, against the HackRF at
   161.975 MHz* - upstream's ais_rx.c, ais_msg.c and ais_modo.c, stub
   gone; Digital 22 "AIS" ("Barcos"), NFM, from the FM discriminator at the
   RF path's rate (96 kHz, 48 with RATE), raw, before audio filter and
   squelch, as upstream's demod_am.c hands it. `demod_ais(1|0)` (the rate
   told, ais_modo_start() takes the borrowed RAM). The channel filter goes
   wide, as SSTV-FM's - and that one had been lost since step 48: texto_modo()
   now sets it for both (SSTV-FM's not retried on the air). Entering AIS
   tunes channel B if the dial is on 162.025, A otherwise. The screen is
   upstream's table of ships over the whole area, one zebra row per MMSI
   (MMSI, name, position, speed, course, frames), the channel in the title;
   button 4 shows the dial ("161.975") and swaps A/B; the bar says the ships
   ("1 ship"), the chip good/seen frames. Columns in malahit_layout.h
   (`UDG_COLS_AIS`, `#ifndef` in ui_digi.c, the DEEPSDR's unchanged). The
   console logs each changed row ("ais: 0 273123450|MALAHIT TEST|...").
   `tools/hackrf_gen.py ais` writes messages 1 and 5 from ITU-R M.1371, not
   from ais_rx.c: six-bit text, octets LSB first on the air, CRC-16/X.25,
   stuffing, NRZI, 24 training bits, GMSK BT 0.4 +-2400 Hz.
   `deepsdr-ui/sim/ais_radio.c` (`make out/aisradio`) runs the mode on a
   discriminator file.
   **Two upstream changes in User/ais_rx.c** (their own commit):
   - **the octets' bit order.** The frames from the generator passed the CRC
     8 times of 8 and left no ship: MMSI 547234053 for 273123450. M.1371
     packs the fields MSB first into octets and each octet goes out as in
     HDLC, LSB first; gr-ais reverses each octet after the CRC
     (reverse_bit_order()). Upstream's bench (sim/ais_aire.c) was written
     with ais_rx.c's own convention, so both agreed; it now writes the air
     order and its checks are green. Only a real ship settles it;
   - **`AIS_CAF`, off by default** (the DEEPSDR builds the same; 1 kB of SRAM
     it would need), on in reglas.mk: the per-burst DC. On the radio the
     discriminator gives FM noise between bursts, and upstream's 32-bit DC
     follower walks with it: a burst starts ~0.07 rad/sample off, while the
     GMSK preamble, after the one-bit average, stands 0.044 from zero. And
     the bit clock, chasing noise crossings in the pause, starts at a random
     phase and cannot lock in the preamble's 12 transitions at 1/16. So: a
     carrier gate, open when the discriminator is *smooth* - sum of
     (x[n]-x[n-1])^2 over 12 bits under 0.20 of the window's variance
     (scale-free: the radio's discriminator is radians x 110000, upstream's
     bench is in deviations; plain x^2 cannot separate noise from signal in
     a +-9 kHz channel); over 0.50 it is sure noise, and a window holding an
     exact zero is "nothing" (the empty window at start, digital silence).
     After 16 bits of sure noise, the opening sets the DC to the window's
     mean - 12 preamble bits, ++-- in NRZI, zero over any 4 bits - then it
     follows the window mean at 128 bits, only while open. Not remembered
     from burst to burst: two ships at +-500 Hz gave 7 of 40 that way. The
     clock is not corrected on sure noise, and its first 24 bits after an
     opening go at 1/8.
   **Host** (`aisradio`, RF noise in 14 kHz, a +-9 kHz channel, bursts at a
   random bit phase; frames of 20 for one ship, of 40 for two):

   |                               | 25 dB   | 20 dB   | 15 dB |
   |-------------------------------|---------|---------|-------|
   | one ship, on frequency        | 13 → 19 | 11 → 19 | 1 → 2 |
   | one ship, +930 Hz             |  6 → 17 |  4 → 17 | 0 → 3 |
   | one ship, +1200 Hz            |  0 → 19 |  0 → 18 | 0 → 2 |
   | two ships, -500/+500 Hz       | 20 → 38 |  8 → 31 | 0 → 1 |
   | two ships, +700/+1500 Hz      |  8 → 31 |  3 → 27 | 0 → 1 |
   | one ship, 48 kHz, +930 Hz     |  1 → 13 |  0 → 13 | 0 → 0 |

   Upstream's aisaire with AIS_CAF=1: all green, the offset holds to 3.0
   deviations (0.8 before). Under ~15 dB almost nothing either way: that is
   the one-bit average on the discriminator, short for GMSK - other work.
   **On the air** (8 frames a go, the HackRF and the radio ~0.9 kHz apart
   at 162 MHz): before the gate, -x 20 none, -x 35 5, -x 45 6; with it, -x
   20 none, -x 25 3 and 4, -x 30 8 and 8, -x 35 8, -x 45 6; at -x 30 with
   the HackRF moved +1 / -1 / +2 / -2 kHz: 6, 8, 7, 8. The ship's row
   complete (name from message 5). DEEPSDR: make all, its 29 checks green.
   Flash image 361 936 bytes.
56. **The clock - the RTC, by hand, and by radio.** *24/11/2026, from
   flash* - FT8 and WSPR need the time of day to the second (15 s slots,
   even minutes), and until now `rtc_hw_get()` (src/reloj.c, gone) gave the
   uptime. `src/rtc_hw.c` is upstream's contract (User/rtc_hw.h) on the
   H743: the RTC on the 32.768 kHz crystal X2 in the backup domain, which
   CPU_VBAT keeps from the battery while the radio is off (the owner).
   Upstream's decisions kept: a running calendar is not touched at start
   (INITS, the GD32's YCM), a dead one is seeded 2026-01-01 00:00:00 on
   purpose, "ever set" is a magic in backup register 0 and the epoch of
   the last setting in 1, an impossible date is refused, prescalers 128 x
   256 for the 1/256 s shift. One difference: the crystal is not waited
   for - rtc_hw_init() switches it on (drive medium-high) and returns,
   rtc_hw_poll() from the main loop finishes when LSERDY comes or gives up
   at 5 s, so a dead crystal costs no boot time; until then
   rtc_hw_lxtal_failed() says "do not believe this clock", as upstream's
   callers expect. upstream's rtc_cal.c (the calendar's arithmetic)
   compiled as it is. The time kept is upstream's: LOCAL.
   The header's corner shows the RTC's hh:mm (the uptime only while the
   crystal starts). A tap on it opens upstream's keypad of the time
   (src/menus.c, its KBD_HORA): HHMM with a dash for each digit not yet
   typed, "Set the clock" and "On the minute" - the second says the tap
   is the minute's edge, and from then the keypad's unit slot ticks the
   seconds. The date is not the keypad's, as upstream's.
   **On the radio**: first start, the crystal ran (RCC_BDCR 0x810B: LSE
   on and ready, LSEDRV 01, RTCSEL LSE, RTCEN) and the calendar was seeded
   and counted at 1 Hz; typed 2016 and "On the minute" at the PC's
   20:16:00 (EET, NTP) - the RTC 0.18 s behind the PC (the tap's way
   through SWD), backup registers "RTC1" and the epoch. Off with the
   button, a minute or two, on: "Clock : RTC kept by VBAT, 2026-01-01
   20:22:07 local, set 367 s ago", the time right (the owner). Boot still
   2362 ms.
   **A day later** (25/11/2026, 22.9 h after "On the minute"): 3.96 s
   fast - **+50 ppm**, 4.3 s a day; C71/C72 are not this crystal's loads.
   So the smooth calibration (CALR, 0.954 ppm steps, in the backup
   domain): set by hand to -50.5 ppm (CALM 53) from that measurement, and
   learnt from then on - `rtc_hw_pon_anclado()` reads how far the RTC
   walked since the last setting that knew the second (a station, the
   keypad's On the minute; backup registers 2 and 3) and, when that was
   long enough ago for the two settings' errors to be under ~5 ppm
   ((err1 + err2) x 200 s: 11 h for two stations, 22 h with a hand),
   moves CALR by it. A walk over 2 minutes teaches nothing.
   **The radio clock** - upstream's "Hora por radio", Settings > Digital
   (a fifth page, as upstream's): `src/hora.c` is upstream's hora_*(), the
   sweep, GRID_HUSO and rds_poll() from its main.c on this port's calls,
   in menus.c's MENU_HORA; upstream's dcf77.c, hora_rbu.c, hora_wwv.c,
   hora_comun.c and husos.c compiled as they are (their stubs gone);
   upstream's ui_hora.c with its literals made `#ifndef` (upstream's
   values; the DEEPSDR's object identical byte for byte) and this
   screen's in malahit_layout.h. The station button cycles DCF77, MSF,
   ALS162, RBU, WWV 10/15, BPM, RDS and the sweep; the zone button opens
   upstream's fifty cities with their summer rules, plus "Measure" against
   a station that sends UTC; the zone goes to the FRAM (v4 bytes 37-38).
   Leaving the screen gives back the mode, filter and dial; the FRAM is
   not written while it is up.
   What this port does differently, each from a measurement:
   - **each decoder's latency is taken off.** Upstream counts the
     "atraso" from when it saw DCF_LISTO. Measured on upstream's own
     benches (their generators, the radio's rates), from the minute's true
     edge to the decoder's new minute: DCF77 +29 ms, MSF +421, RBU +190,
     WWV +1107, BPM +1086 (the rise of second 01's subcarrier closes the
     minute), ALS162 -898 (it knows the minute in second 59). WWV
     uncorrected set the RTC 1.41 s late;
   - **the second's fraction is written** by the RTC's shift (1/256 s),
     not lost to a whole-second write;
   - **RDS only fixes a plainly wrong clock.** Riga's stations against the
     PC's NTP: LR-1 (90.7) 5.75 s late, SWH ROCK (89.2) 2.1 s late, MIX FM
     (102.7) and LR-2 (91.5) no clock-time at all - the standard says
     +-0.1 s. So RDS sets the clock when it was never set or is over 15 s
     off, else at most the date, and never teaches the crystal; upstream's
     anchors to every group.
   `tools/hackrf_gen.py wwv` (NIST SP 432's time code as AM, minutes long,
   written in one-second pieces) and `rds` (wide FM: pilot, RDS 0A with
   the PS and 4A placed to END on each minute's edge, IEC 62106's blocks
   and CRC; a 1 kHz tone, because upstream's WFM takes an unswept carrier
   for its own LO's leakage).
   **On the air**: WWV 10 MHz from the HackRF, zone Helsinki +2:00:
   "Time received and confirmed 19:17 25/11/26", Apply - with the
   latencies, **RTC - PC = -0.04 s** (without, -1.41). RDS from the
   HackRF set the clock from a clean 4A group (its 2.5 kHz subcarrier
   decoded at only 25-35 % of blocks on this bench - through 40 dB of pad
   the HackRF is weaker than the radio's own LO leakage; the host decodes
   the same file at 100 %); on an antenna, LR-1 then SWH set it 5.75 and
   2.1 s late, which is what changed the RDS rule; after it, LR-1's -5.6 s
   was left alone ("se deja"). DCF77, MSF and RBU are under the dial's
   100 kHz floor (RADIO_TUNE_MIN_HZ): they tune 100 kHz for now - whether
   the MSi001 and the add-on board hear 60-78 kHz is for a night with an
   antenna.
   **Found on the way, not this step's**: FM on the antenna crackles today
   on many stations, step 41's image too (so not a regression): the
   front end is overloaded - 20 dB of the add-on board's PE4312, ahead of
   the MSi001, takes the pilot's SNR from ~0 to 12 dB. And one HardFault,
   at 103.1 MHz while a scan drove the dial over SWD every 3 s: inside an
   interrupt, and fault.c's return into fault_park() faulted again 394
   million times, each pass writing over the record - which now keeps the
   first fault. Not reproduced at 103.1 since.
   Flash image 398 472 bytes.
57. **FT8, and the QTH screen.** *25/11/2026, from flash, against the
   HackRF at 14.074 MHz* - upstream's ft8_decimator.c, ft8_fft.c,
   ft8_tablas.c, ft8_waterfall_adapter.c, ft8_decoder.c, ft8_snr.c,
   ft8_lista.c and ft8_modo.c, and ft8_lib's decoder as upstream trimmed
   it (Ft8Lib/ft8, objects `ftx_*`), stub gone; Digital "FT8" ("Digital de
   HF"), USB, after DSC as upstream's. `demod_ft8(1|0)`; `ft8_modo_tick()`
   from cw_poll(). The slot comes from the RTC (step 56).
   **Its memory is upstream's union itself**: FT8's files name the members
   of `g_ft8_shared_ram` directly, so src/prestamo.c now DEFINES that union
   and lends all of it to the other borrowers (hfdl_ram_todo()); it is
   wiped when taken (FT8's lines live in it and a JTTY table left behind
   would show as decodes). It moved from D2 to AXI SRAM (no RAM image to
   fit any more), 175 kB with this port's flags, below.
   **The screen** is upstream's: the table over the whole area (Time Hz dB
   DT Message km Country, `UDG_COLS_FT8` in malahit_layout.h, measured by
   `make malahit-menus`), CQs in their colour; the second header layout:
   Clear, the CQ filter, the frequency (upstream's nine FT8 dials, a tap
   goes to the next), the slot's bar with the second, the candidates and
   the mean DT; a tap on the bar squares the slot; a finger scrolls the 48
   kept (ft8_lista.c). Upstream's Map is left out. The console: each
   decode, and once a slot its numbers (`ft8 slot N: ...`).
   **What this radio needed, each measured** (host: `deepsdr-ui
   out/ft8radio`, upstream's whole mode on a 12 kHz file, with the decode's
   duration modelled; signals: `tools/hackrf_gen.py ft8`, an encoder
   written from the protocol - pack77, CRC-14, the generator matrix that
   ft8_lib's decoder does NOT use, checked against its parity checks, Gray,
   GFSK BT 2 - whose messages upstream's decoder reads back):
   - **the level** - the sideband reaches FT8 at sigma ~3 LSB (ATT 46),
     upstream's benches at 1000. Upstream's decimator AGC resets to unity
     at every slot edge and takes ~10 s to come down to that, two thirds
     of every slot on the bottom of the spectrogram; on the host, 18 of 20
     decodes at this radio's level, none at half of it.
     `__wrap_ft8_modo_come()` (src/demod.c) lifts the block to RMS 1000
     with a gain that follows slowly (30 s), x115-560 seen;
   - **FREQ_OSR=2** (upstream: 1, for the DEEPSDR's RAM; `#ifndef` now) -
     a signal half a bin (3.125 Hz) off read 0 of 20 at -12 dB, 20 with 2;
     on the radio, -12 dB 10 -> 20 of 20, -15 dB 7 -> 20;
   - **a byte a cell** (`FT8_ADAPTER_MAG_BITS=8`, `FTX_MAG_4BITS=0`,
     upstream's other branch) - the candidate search is one call at the
     slot's edge over the whole spectrogram, and with 4-bit cells and
     FREQ_OSR=2 it took 650 ms (PC sampled over SWD: ft8_sync_score(), a
     nibble and four float operations per read, D-cache off); it is ~300
     ms now, and more sensitive (-15 dB a quarter bin off, 2 -> 11 of 20);
   - **eight hand-off buffers** (`FT8_DECI_BUFS`, `#ifndef` now) - two
     give between 160 and 320 ms, and that search plus a frame still lost a
     sub-block now and then; eight give 1.1 s. `lost 0` over 17 slots.
   On the radio, five messages a slot from the HackRF: -8 dB 25 of 25, -12
   dB 15-20 of 20, -15 dB 20 of 20 (WSJT-X's SNR in 2500 Hz; upstream's
   estimate reads ~3 dB high); the decode 0.5-0.9 s, the worst main-loop
   pass 301 ms.
   **Left as upstream has it**: TIME_OSR=1 - a signal a fraction of a
   symbol off the 160 ms grid loses some dB; the capture starts at a main
   loop pass (10-60 ms of jitter here), so near -12 dB slots alternate on
   the host until the auto-squaring settles, and on the air every station
   brings its own DT. TIME_OSR=2 doubles the search again and the
   spectrogram (~95 kB more) - a decision for after the live antenna.
   And "short" slots every time (92 of 93 blocks: upstream's 120 ms margin
   against this loop), harmless since upstream decodes short slots.
   **The QTH screen** - upstream's ui_qth.c (its geometry `#ifndef` now,
   this port's in malahit_layout.h; its font_ui_18b kept for the letters)
   and its controller (src/qth.c, out of upstream's main.c): six boxes,
   each only its alphabet, the coordinates under them, -, +, Apply, Exit;
   Settings > Device > QTH, "not set" until set. This port adds the knob:
   turning moves the chosen box, the press goes to the next. In the FRAM,
   v4 bytes 39-44. Upstream boots on IN80dk; here none, so the km column
   stays empty until the owner sets one.
   **Found on the way**: the HackRF and the radio are 719 Hz apart at 14
   MHz today (51 ppm; ~6 ppm in steps 50-55) - nothing in the tuning moved;
   for the live pass, against a known station (upstream has "Calibrar
   PPM", not ported).
   Flash image 439 160 bytes.
58. **WSPR.** *25-26/11/2026, from flash, against the HackRF at 14.0956
   MHz* - upstream's wspr_rx.c, wspr_fano.c, wspr_msg.c and wspr_modo.c,
   stub gone; Digital "WSPR" ("Balizas de HF"), USB. `demod_wspr(1|0)`
   (wspr_modo_start() takes the borrowed RAM: its 46 kB spectrogram, the
   receiver's state, the lines); upstream's wspr_modo_poll() from cw_poll().
   A window opens at each odd minute's second 59 (the RTC, step 56) and
   closes 114.7 s later; then the sync search and the stack decoder, one
   beacon a cycle, as upstream decided. Its spectrogram is normalised row by
   row in floats, so the radio's low sideband - FT8's trouble - does not
   reach it, and nothing of upstream changes but the columns.
   **The screen** is upstream's (digi.c): the beacons over the whole area,
   newest last, zebra rows (Time Callsign Loc. dBm Offset Qual.,
   `UDG_COLS_WSPR`, `#ifndef` now in ui_digi.c); the bar the cycle -
   "starts in 1:47" / "listening 63 %" and the last attempt's "q 92" (88
   accepts); the dial button upstream's ten WSPR frequencies ("14.095,6"),
   a tap during a window noted and applied when it closes ("then 20 m" in
   the chip); the chip the beacons heard. Upstream's bar is Spanish in
   both languages; here tr() gives it English. The console: each window
   opened and closed, with its q and the beacon.
   `tools/hackrf_gen.py wspr` writes beacons from G4JNT's description, not
   from wspr_msg.c: the 28-bit call, the 22-bit locator and power, the K=32
   convolutional code, the bit-reversal interleave, the sync vector (written
   from memory and found equal, all 162 bits, to upstream's k_wspr_sincro),
   continuous-phase 4-FSK from 1 s past an even minute; `--snr` per cycle.
   `deepsdr-ui/sim/wspr_radio.c` (`out/wsprradio`) runs the mode on a file.
   Host: every beacon read back, q 99-100; -24 dB 8 of 8, -26 dB 6 of 8,
   -28 dB none (WSJT-X: about -28).
   **On the radio**: the four test beacons decoded at -20 dB, q 97-99, and
   -24 dB once of twice; -26 dB not. **But the bench is the limit, not the
   radio**: the HackRF drifts while it warms - the first two to four
   cycles of every transmission fail at noise-level q (74-80), the rest
   decode (7 of 7 warm cycles at -20 dB), and even warm it moves ~0.5 Hz in
   a 110 s beacon against tones 1.46 Hz apart. FT8, 12.6 s long, never
   noticed.
   **Measured on the way - the radio's own frequency**: the CPU's cycle
   counter against the PC's NTP time over 598 s (one OpenOCD session,
   CYCCNT unwrapped every 2 s): **-11.0 ppm**, g_msticks the same. X1 is
   also the MSi001's reference (BOARD.md), so the radio tunes 11 ppm low:
   on the air a station appears 155 Hz high at 14 MHz - a WSPR beacon at
   1500 Hz lands at 1655, outside upstream's 1400-1600 window. The HackRF
   is then ~-62 ppm (the 719-751 Hz of step 57). Not corrected here: for
   the live pass, upstream's "Calibrar PPM" (SAM lock on a known station)
   or a calibration of X1 against the RTC, which step 56 already
   disciplines to NTP. (A first reading of -388 ppm was swd-call and
   grab.py halting the core: SysTick stops with it.)
   **Left as upstream has it**: the decode at a window's end is one call,
   1.38 s on this radio when there is no beacon (PC sampled: the stack
   decoder, wspr_fano_ex(), scanning its stack in uncached AXI to its
   200 000-step limit); the capture is closed by then, so nothing is lost,
   but the panel and the knob stop for that long every two minutes. The
   D-cache over AXI would cure this and FT8's search alike - a decision
   about the DMA buffers, not this step's.
   Flash image 448 352 bytes.
59. **HFDL - the aircraft.** *26/11/2026, from flash, against the HackRF at
   8942 kHz* - upstream's twenty-three hfdl_*.c (hfdl_viterbi.c was in
   since STANAG), stubs gone; Digital "HFDL" ("Datos de aviones"), USB -
   upstream keeps it in a family of its own, "Aire"; here it is Digital's
   sixteenth cell. `demod_hfdl(1|0)`; upstream's hfdl_modo_poll() from
   cw_poll(). Its input is upstream's own hook in demod_am.c, the I/Q pair at
   12 kHz after the decimators, the subcarrier at +1440 Hz - nothing to wire.
   **Its RAM**: upstream's User/hfdl_ram.c is now compiled as it is: it
   lends the union through waterfall_presta(), which src/prestamo.c has
   provided since step 57, and the port's copies of hfdl_ram_coge(),
   _suelta() and _todo() are gone. The union is 175 kB here (FT8's flags),
   against upstream's 66-70 kB, so **every "giro" fits**, the 1800 bps
   double slot (giro 7, 76 kB) too, which the DEEPSDR cannot decode at all.
   The gate before HFDL's AGC learns a new floor when the gain moves, as
   upstream asks after its PGA: radio_att_set() and radio_preamp_toggle()
   call hfdl_modo_nivel_cambiado(). The aircraft models come from
   ICAO24.BIN on upstream's external flash; this radio has none, so
   `icao24_db_lookup()` says "not found" (src/decoders_off.c) and the Model
   column shows "-" - the database could live in the H743's bank 2.
   **The screen** is upstream's (digi.c): the aircraft over the whole area,
   newest last, zebra rows (ICAO or a ground station, Model, Flight,
   Position, the last event, N - `UDG_COLS_HFDL`, `#ifndef` now in
   ui_digi.c), the station last heard in the title; the bar the phase
   (searching / half a signal / almost / LOCKED) or the funnel
   "A1>A2>M1 U ud/pos"; the chip "signal% right/seen" and upstream's P/Z/X
   count of frames that did not fit; the fourth button the channel,
   upstream's 29 (Bahrain, Shannon, Reykjavik, Canarias), the next up on a
   tap, lit when on one. Left out: upstream's third button (a grid of the
   channels with their frames), and its fifth (Map, or "Bytes", the raw
   frames: with a fifth button the chip has 86 px, and "99% 999/999 P999"
   needs 129). The console: the counts and the newest row as they change.
   **The signal**: `deepsdr-ui/sim/hfdl_gen.c` (`out/hfdlgen`) - upstream's
   own burst, as sim/hfdl_extremo.c builds it (448 carrier symbols, A, A,
   the giro's M1, M2, nine training windows, the self-test's message with
   its CRC in data segments, training between), RRC beta 0.35 at 12 kHz on
   the 1440 Hz subcarrier; `tools/hackrf_gen.py hfdl --in b.c32 --snr N`
   takes it to the HackRF. NOT an independent transmitter - the decoder's
   author wrote both; what it tests is this radio's chain, and the
   protocol's reading is for the live pass. `sim/hfdl_radio.c`
   (`out/hfdlradio`) runs the mode on a .c32. Host: giros 0, 3 and 7 two of
   two, giro 7 only with the port's union (the FT8 flags).
   **On the radio**, four bursts each of giros 0, 3 and 7: SNR 15 dB in 3
   kHz 12 of 12 frames right, giro 7 included; SNR 8 dB 9 of 12 (the host
   12 of 12 from the same int8 file - the HackRF's drift and the radio's own
   noise at the threshold). The audio hook (demod_report): 12.1 ms of each
   42.7 ms in USB, 14.7 in HFDL on noise, 15.8 worst during bursts, no SAI
   overrun; the main loop's worst decoders' pass 83 ms (the Viterbi).
   Flash image 477 232 bytes.
60. **The SPI flash, and the USB disk.** *26/11/2026, from flash.* The
   owner soldered a Winbond W25Q64CV (8 MB, a part salvaged off a PC
   board - it held a Phoenix BIOS image, NVRAM store "EVSA" at 0x701000)
   stacked on the FRAM D11, every pin in parallel but /CS, which goes to
   PB0 (LQFP 34) with its own 10k pull-up - measured over SWD: PB0 reads
   1 against the H7's internal pull-down. Before a line was written the
   chip was read over SWD by driving SPI2 by hand on the running radio
   (halted, both selects checked high first): JEDEC EF 40 17, SR1 00 (no
   block protection), SFDP present, serial ca62a031 43603035.
   **The driver is upstream's whole User/spi_flash.c** - commands, erase
   and program, the FAT12 volume, the async save - compiled in place; its
   bit-banged GD32 bus is swapped under an `#ifndef SPI_FLASH_TRANSPORTE`
   (upstream edit; the DEEPSDR's object identical) for `src/spi_nor.c`:
   SPI2 at fram_init()'s 15 MHz, ENDLESS runs (TSIZE 0) from select to
   deselect, CSUSP/SUSP to end one - the sequence checked over SWD first.
   fram.c owns the bus and now drives PB0 high too, before the bus first
   moves. 691 kB/s reading (the wire's ceiling is 1830: a byte's register
   polling costs more than its clocks - enough for files and for USB full
   speed; 30 MHz for the NOR alone is the obvious next step if a use needs
   it). The boot log says JEDEC, size, serial, SR1/SR2 (block protection
   loudly), the volume's state, and runs upstream's repair of a write a
   power cut interrupted. `banco.c`'s two disk calls go on to MEM.CSV in
   the FRAM (src/memfram.c), renamed for those two objects
   (`MEMFRAM_RENAME`): the owner's channels stay where they are.
   **The chip, tested whole** (`nor_prueba`, over SWD): erase 8 MB in 14.4
   s (128 blocks of 64 kB), blank check, a pattern hashed from each word's
   address programmed into all 32 768 pages (48 s) and read back: 0 bad
   bytes; erased again, blank. Then upstream's formatter (`formato.c`,
   `nor_formatea`): 7 MB FAT12, 14 336 blocks of 512, 4 kB clusters, two
   FATs, writable; `formato_comprueba()` wrote PRUEBA.TXT through the FAT
   and read it back. Free 7148 kB. The top megabyte is upstream's high
   zone (BD.BIN, the map), still empty.
   **The USB disk** - upstream keeps it in its BOOTLOADER (boot/boot_main.c,
   boot/disco_msc.c); this radio's bootloader is ST's ROM, so here it is a
   mode of the application (`src/usbdisco.c`). GigaDevice's USB library as
   upstream carries it, in place (objects gdusb_*): the STM32H743's OTG_FS
   is the same Synopsys DWC2 core as the GD32's USBFS. Three upstream
   `#ifndef`s, each defaulting to the GD32 (all four DEEPSDR binaries
   identical, the application and three bootloaders): the core's base
   address (drv_usb_regs.h), the chip's serial number's (usbd_enum.h -
   the GD32's address is a bus fault on the H7), and turning the PHY on
   (drv_usb_core.c: in the H7's GCCFG bits 18/19 are charger detection,
   which takes D+/D- over, and 21 SENSES VBUS where the GD32's ignores it;
   here PWRDWN alone and the B-session forced valid, as ST's driver does
   without VBUS sensing - VBUS reaches PA9 only through JP2).
   `compat/usb_malahit.h`, forced into those objects, says the H7's
   values. The hardware: PLL3 (free) 22/11 x 240 / 10 = 48 MHz, the USB
   supply detector, PA11/PA12 AF10, OTG_FS_IRQ the LOWEST priority (the
   class reads and writes the flash inside it, ~70 ms for a 4 kB write;
   audio, SysTick, the panel and the console preempt it). The interrupt
   shares SPI2 with the FRAM: fram.c masks it from select to deselect of
   every transaction started from thread mode (`fram_bus_irq()`), and
   spi_nor.c does the same. usbd_init()'s waits have no way out, so
   usbdisco_abre() starts nothing unless PLL3 locked and the USB supply
   said ready.
   **The first run did not enumerate**, and the registers said why:
   DCTL 0x00000002 (soft disconnect still set), DCFG's speed 00, GINTSTS
   "mode mismatch". GigaDevice's usbd_init() forces device mode and writes
   the device registers at once; the DWC2 core takes up to 25 ms to switch,
   drops what comes before, and the H7's core leaves reset as a HOST (PA10,
   ID, is not its input here). Set by hand over SWD - DCFG |= 3, SDIS
   cleared - the PC enumerated "DEEPSDR Radio Disk", 14 336 blocks, and
   mounted it. A fourth upstream `#ifdef`, `USB_MODO_ESPERA` after
   usb_curmode_set() in usbd_core.c (DEEPSDR's four binaries identical
   again); the port waits for GINTSTS.CMOD = 0, as ST's driver does: **7
   ms** on this chip. The boot of the fixed image enumerated by itself.
   **On the PC** (Linux, vfat): PRUEBA.TXT as the radio wrote it; 256 kB
   of random data written in 2.7 s with sync (~94 kB/s: an erase and
   sixteen pages per 4 kB), read back past the page cache (O_DIRECT) in
   0.84 s (~300 kB/s), the same SHA-256 - and again after a power cycle of
   the radio. The class: 228 reads, 3 writes, 0 failures.
   **Switched from the radio**: Settings > Device > "USB disk" (this
   port's own cell): a tap on, a tap off; the value Off / Waiting / PC
   (configured by a host). Not kept in the FRAM - every power-on starts
   with it off. Eject on the PC before switching it off.
   **BD.BIN** (upstream's databases, 528 384 bytes, the repo root's) by
   upstream's path: copied to the USB disk from the PC (5 s, read back
   past the cache), and loaded AT BOOT by `src/bd.c` - upstream main.c's
   datos_arranca() / datos_al_arrancar() and the opening after them,
   rewritten with the same calls into spi_flash.c and zona_alta.c in the
   same order (upstream's main.c is not compiled here): the checks, the
   copy into the high zone at 0x701000, the directory and every section's
   sum, then BD.BIN deleted from the disk. At boot because icao24_db.c
   remembers a failed open until the next one. The panel says "Databases -
   Copying NN %" through main.c's ui_prompt(). On the radio: copied in 6.0
   s, checked, deleted, 8.2 s in all; three sections - aircraft models
   294 909 bytes (57 769 aircraft, 252 types), stations 2204 frequencies /
   13 369 entries, valid until 2027-03-28. `User/zona_alta.c`,
   `emisoras.c` and `icao24_db.c` compiled in place (icao24_db's stub in
   decoders_off.c gone). Checked over SWD: icao24_db_lookup(0x45C86A) =
   "A320 Airbus A320-214", 0xA64A73 = "E75L Embraer ERJ 170-20" - the same
   as BD.BIN parsed on the PC - and an absent address empty. The stations
   are open but nothing draws them yet: upstream labels the spectrum from
   its main.c (35 calls into emisoras.c), a UI step of its own.
   Flash image 521 720 bytes.

Steps 1–7 are a new, small board-support tree. Only at step 8 does the bulk of
the upstream code arrive — which means a long stretch of visible progress
before the hard part, rather than months of nothing.

## 5. What the Malahit gains

- MSi001 reaches far beyond the DEEPSDR's 180 MHz ceiling (with a hardware
  gap at roughly 263–390 MHz that no firmware can close).
- Room for longer FFTs, a wider FT8 search window, more waterfall history.
- A second encoder.
- A capacitive touch panel instead of a resistive one.

## 6. Prior art

[Cerk-sdr](https://github.com/TheHuziek/Cerk-sdr) — an open firmware for this
same radio, written in Rust. Early (a handful of commits), but its TODO list
is exactly the three drivers in §2: ILI9488, MSi001, NAU8822. Worth reading
for register sequences even though this port is C.

## 7. Licence

Upstream is **AGPL-3.0**, and this port is a derivative work, so it stays
AGPL-3.0 and its source stays published. `../PROCEDENCIA.md` is upstream's
file-by-file inventory of third-party code (ft8_lib is MIT, the RM68120 panel
table comes from Espressif); anything this port adds should be recorded the
same way.
