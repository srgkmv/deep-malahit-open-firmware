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
   nowhere near the limit — but the transmit FIFO is 83 µs deep, which
   means any blocking console write is an audible gap. That, and not CPU
   load, is what makes DMA necessary next.
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
8. **Graft the upstream DSP tree** mode by mode.
9. **Relayout the UI** at 480 × 320, in the simulator first.

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
