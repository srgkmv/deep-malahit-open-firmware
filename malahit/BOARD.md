# Malahit DSP-1 — board reference

Target board for this port. Everything below was read off the official
schematic (`Receiver20.sch`, KiCad 5.0, dated 2019-10-05, released as
`Malahit_schematics_15012020.pdf`, by R6DCY / RX9CIM / R6DAN) and, where
marked **[measured]**, confirmed on a real unit over SWD.

This is *not* the DEEPSDR 101 board. See `../README.md` for the firmware this
port starts from, and `PORTING.md` for what has to change.

**Datasheets live in `../../docs/`**, outside the repository: MSi001,
NAU8822 (Rev 3.3), ILI9488, FT6236, FM25W256, PE4259, PE42641, BGA614 and
the FT2232D. Every page reference in this file is to the copy there.
Anything quoted from one is quoted rather than remembered — several of
the expensive mistakes in this port came from recalling a register map
instead of opening it.

---

## 1. MCU

| | |
|---|---|
| Part | STM32H743VIT6, LQFP100 |
| Core | Cortex-M7 r1p1, FPU with double precision, 16 kB I-cache + 16 kB D-cache |
| DEV_ID | `0x450` **[measured]** (`DBGMCU_IDCODE` = `0x20036450`) |
| REV_ID | `0x2003` = **revision V** **[measured]** → 480 MHz capable (rev Y would be 400 MHz) |
| Flash | 2048 kB **[measured]** (`0x1FF1E880` = `0x0800`), 2 banks × 8 sectors × 128 kB |
| RAM | 128 kB DTCM + 64 kB ITCM + 512 kB AXI + 128 kB SRAM1 + 128 kB SRAM2 + 32 kB SRAM3 + 64 kB SRAM4 + 4 kB backup |

Compare with the DEEPSDR 101's GD32F450VET6: 512 kB flash, ~192 kB usable
SRAM, Cortex-M4F at 200 MHz, single precision. There is a lot of headroom
here, not a shortage.

### Option bytes **[measured]**

Read with `dfu-util -a 1 -s 0x5200201C:128 -U optbytes.bin`:

```
OPTSR_CUR = 0x0BC2AAF8   RDP = 0xAA  -> level 0, no readout protection
WPSN_CUR1 = 0x000000FF               -> no sector write protection
BOOT_CUR  = 0x1FF00800   BOOT_ADD0 = 0x08000000 (user flash)
                         BOOT_ADD1 = 0x1FF00000 (system bootloader)
```

The part is completely unlocked: it can be read out, erased and reflashed
without restriction.

---

## 2. Clocks

**HSE = 22.000 MHz, in BYPASS mode.** X1 is an oscillator *module*
(EN / V+ / GND / OUT), not a crystal. Its output drives `PH0` (OSC_IN);
`PH1` (OSC_OUT) is explicitly marked no-connect on the schematic. The same
22 MHz also feeds `REF_CLK_MSI001`, so the tuner reference and the MCU
reference are the same oscillator.

> ### X1 IS ON THE SWITCHED RAIL. READ §3 BEFORE WRITING CLOCK CODE.
>
> Pin 4 of the module takes its supply through ferrite bead L27 from the
> net labelled **`3.3V`** — U4's output, the rail gated by `PC6` — and
> **not** from `3.3V_CPU`. After a reset the processor is running and its
> own reference oscillator is switched off.
>
> So firmware must raise `PC6` and let the module settle **before** it
> touches `HSEON`. Get the order wrong and `clock_init()` reports a dead
> oscillator on a board whose oscillator is merely unpowered, which is
> not a diagnosis anybody reaches quickly.
>
> It hides well, too. Reloading an image over SWD does not reset the
> GPIOs, so once any run has set `PC6` every later run inherits a
> powered oscillator and the bug stays invisible. It only appeared here
> the day a working hardware reset did.

This matters: `RCC_CR.HSEBYP` must be set before enabling HSE, and every PLL
divider has to be worked out for 22 MHz, not for the 8/25 MHz that most
STM32H7 examples assume.

**LSE = 32.768 kHz** crystal X2 on `PC14`/`PC15` (OSC32_IN/OSC32_OUT), with
C71/C72 = 10 pF. Backup domain is fed from `CPU_VBAT`.

### Sample-rate clocking

`SAI1_CODEC_MCLK` is an **MCU output** (`PE2`), so the STM32 is the clock
master and the NAU8822 is the slave. MCLK must be exactly **256 × Fs**. The
stock firmware runs 160 / 80 / 40 kHz (panorama spans 160 / 80 / 40 kHz);
the DEEPSDR firmware assumes 192 / 96 / 48 kHz.

**Settled on hardware, 01/11/2026: this board holds 192 / 96 / 48 kHz**,
and stays clean all the way to 320 kHz. So the port keeps the DEEPSDR
rates. `PORTING.md` §3.2 has the measured table.

MCLK comes from **PLL2, dedicated to the audio chain** and retuned for
every sample rate, because the kernel clock has to be an exact multiple
of 256 × Fs and `pll1_q_ck` (240 MHz, the SPI3 clock) is not. With
`NODIV` = 0 and `OSR` = 0 the SAI generates

    sai1_ker_ck = 256 × Fs × MCKDIV,    MCKDIV = 1..63

and derives FS and BCLK from the same chain, which also satisfies the
codec's requirement that FS, BCLK and IMCLK all share one reference.
PLL2's reference is fixed at 11 MHz (`DIVM2` = 2) rather than the 2 MHz
PLL1 has to live with: MCLK jitter lands directly on the I/Q sampling
instants, so on this radio it is the receiver's own noise floor.

**Odd values of `DIVP2` are legal** — confirmed on hardware, with P = 67,
23 and 18 all producing the right measured frame rate. RM0433 forbids
odd `DIVP1` and says nothing of the kind about PLL2 or PLL3, and the
solver uses that freedom.

---

## 3. Power rails — read this before bring-up

Two LD3985M33 regulators, both fed from `BAT`:

| Reg | Output | Enable | Powers |
|---|---|---|---|
| **U3** | `3.3V_CPU` | INH tied to VIN — **always on** | STM32H743 only |
| **U4** | `3.3V` | INH = net `3.3V_ON` | **display, touch, MSi001, NAU8822** |

`3.3V_ON` is driven by **`PC6` (pin 63)** and pulled to ground by R38 (1 kΩ).
JP3 is a hardware jumper that shorts `BAT` onto that net, bypassing the MCU.

**Consequence: after reset, everything except the MCU is unpowered. Firmware
must drive `PC6` high early in boot or the screen, the tuner and the codec
stay dark and nothing on any other bus will answer.**

Verified **[measured]** by halting the core over SWD and doing, by hand:

```
mmw 0x580244E0 0x00000006 0            ; RCC_AHB4ENR: GPIOBEN | GPIOCEN
mmw 0x58020800 0x00001000 0x00002000   ; GPIOC MODER: PC6 = output
mww 0x58020818 0x00000040              ; GPIOC BSRR:  PC6 = 1   -> 3.3V rail on
mmw 0x58020400 0x00010000 0x00020000   ; GPIOB MODER: PB8 = output
mww 0x58020418 0x00000100              ; GPIOB BSRR:  PB8 = 1   -> backlight on
```

Do **not** fit JP3 as a convenience. It keeps the peripheral rail powered
while the radio is "off" (the MCU rail is always live anyway; power-off is
MCU standby plus `PC6` low), which drains the battery and hides a bring-up
step that has to be correct regardless. Bench work is easier on USB power.

> ### THIS UNIT IS MODIFIED, AND THE MCU IS NOT ALWAYS ON
>
> Stated by the owner, 20/11/2026: on this radio **U3's INH (pin 3) is
> not tied to VIN (pin 1)**. It is fed through diodes from **`3.3V_ON`
> (`PC6`)** and from **`WAKEUP`** (the power button), and the button
> itself is powered from **`BAT`**, not from `3.3V_CPU`. Other Malahits
> are as the schematic shows; this one is not.
>
> So here **the MCU is powered only while the power button is held or
> `PC6` is high**: the firmware latches its own supply, and "off" is a
> real power cut, not standby. That explains several things this file
> and `WORKING.md` recorded without explaining:
>
> - every boot reports `last reset: power-on, brown-out` — it is one;
> - **any reset switches the radio off**: the reset returns `PC6` to an
>   input, R38 pulls it low, and U3 goes with the peripheral rail;
> - SWD is dead whenever the radio is "off", whatever `DBGMCU_CR` says —
>   the chip has no power (tried with debug kept on in standby: no
>   difference);
> - **DFU must hold `PC6` high** or it lasts only as long as the thumbs
>   on the buttons. The factory firmware raises it before its jump, and
>   so does `src/boot.c`.
>
> The firmware is written to work on both: it latches `PC6` early, and
> its "off" is standby with every pin floating, which on an unmodified
> board is standby and on this one is the power cut.

Other power control:

| Net | Pin | Function |
|---|---|---|
| `PREAMP` | `PE0` | Q7 (2N7002) → Q3 (IRLML6402) → `5V_PREAMP` |
| `Vspk_ON` | `PE1` | Q8 (2N7002) → Q4 (IRLML6402) → `Vskp_CODEC` (speaker amp supply) |
| `SPI_LCD_BL` | `PB8` | R27/R28 → Q1 (IRLML2502) → panel LED anode. PWM capable: TIM4_CH3 (AF2) or TIM16_CH1 (AF1) — **TIM16_CH1, 20 kHz, active high, since 22/11/2026** (`src/backlight.c`); no spur from it measured at 50 % |

Battery / charger:

| Net | Pin | Function |
|---|---|---|
| `ADC_IN` | `PA0` | `BAT` through R34/R36 = 10 kΩ/10 kΩ divider → **V_bat = 2 × V_adc** |
| `CHRG` | `PB10` | TP4056 charge status, open drain, active low |
| `STBY` | `PB11` | TP4056 standby status, open drain, active low |
| `WAKEUP` | `PC13` | SW3 "STDBY" button, via R39/R44/R45 divider off `3.3V_CPU` |

---

## 4. Debug access

### SWD — connector J2

**There is no Vcc pin on J2, and the pin order is not the usual one:**

| J2 pin | Signal | MCU |
|---|---|---|
| 1 | `CPU_RST` | NRST (pin 14) |
| 2 | `SW_CLK` | PA14 (pin 76) |
| 3 | GND | — |
| 4 | `SWD_IO` | PA13 (pin 72) |

**OpenOCD must use `dapdirect_swd`, not `hla_swd`.** **[measured]** With
`interface/stlink.cfg` (which selects HLA) an ST-LINK/V2 reports
`Error: init mode failed (unable to connect to the target)` even though
`Target voltage` reads correctly. With `interface/stlink-dap.cfg` the same
cable connects first try. The `st-info` / `st-flash` tools from stlink-tools
only speak HLA, so they will not work here either — use OpenOCD.

See `openocd.cfg` next to this file; `tools/swd.sh` wraps it.

### ROM DFU — BY BUTTON, NOT BY JUMPER

The schematic shows `BOOT0` (pin 94) held low by R17 (10 kΩ) and pulled
high through jumper **JP1** + R15 (470 Ω). **On this unit JP1 is not
fitted**, and the schematic is from 2020, so do not assume it is there.

DFU is instead reached the way the factory firmware offers it: **hold the
encoder buttons and press the power button.** The device then enumerates
as `0483:df11` with `@Internal Flash /0x08000000/16*128Kg`.

Three consequences, and the third one matters a great deal.

1. **Any reset leaves DFU.** NRST restarts the factory firmware with no
   buttons held, so it boots normally, and if nobody presses anything it
   settles into a low-power state that takes the debug port with it. The
   symptom is `cannot read IDR` on the next attach, which reads like a
   dead adapter and is a sleeping radio. This is why `openocd.cfg` does
   not reset and why `core_clean` exists.

2. **SWD does not need DFU.** It needs the core awake. Simply switching
   the radio on normally is enough to attach, load a RAM image over the
   running factory firmware and go.

3. **The route into the bootloader is a feature of the firmware we
   intend to replace.** Erase flash and the button combination goes with
   it. Before anything is written to flash, there has to be another way
   back:
   - the `JP1`/R15 pads next to pin 94, if they exist unpopulated on
     this board — worth looking for with a meter, because shorting
     BOOT0 high is the one recovery that no firmware can take away;
   - or SWD, which works as long as our own firmware never disables the
     debug port and never sleeps without a way to wake;
   - or our firmware implementing the same courtesy the factory one
     does: a button combination at boot that jumps to the bootloader.

   Item three is cheap to write and should exist before the first flash
   write, not after it.

**Done, 20/11/2026, and proven both ways on the radio.** `src/boot.c` is
the factory recipe: at every reset, before `.data`, `.bss` or a clock,
`PC6` up, ~20 ms for the rail, `PA1` read as a digital pin, and if it is
low, quiesce and jump to **`0x1FF09800`** — the H743's bootloader. Not
`0x1FF00000`, which this file used to give: that is the option-byte boot
address, and the factory code's literal says `0x1FF09800`. The jump
target is where the bootloader's own vector table starts.

Measured, so the next person does not have to:

- **The ladder hangs from the switched rail.** With `PC6` low `PA1` has
  no pull-up and reads 0 — "both buttons held" on every cold boot. The
  first flash image did exactly that. Raise `PC6` and wait before
  reading.
- **No `JP1` pads, no reset-and-halt over SWD** (`WORKING.md`, "No way
  back"): ROM DFU on the buttons is the way back, plus SWD while our
  firmware is running.
- **The round trip works**: our image written over the factory DFU,
  rewritten over our own DFU, the factory image restored from the backup
  (all 2 MB identical afterwards, and it ran), and ours written back over
  the factory DFU again. `dfu-util` needs no root on this PC.

### Debug UART — header J6

`UART7` is brought out on the reserved-pin header: `PE7` = UART7_RX (AF7),
`PE8` = UART7_TX (AF7). This is the natural console for `debug_uart.c`.

J6 pinout, from the schematic netlist — **verify against the real board before
soldering**, the pin numbering was read from extracted text, not from the
drawing:

| J6 | Net | J6 | Net |
|---|---|---|---|
| 1 | `PE9` | 2 | `PE8` UART7_TX |
| 3 | `PE7` UART7_RX | 4 | `PB2` |
| 5 | `PB1` | 6 | `PB0` |
| 7 | `PD8` | 8 | `PD9` |
| 9 | `PD10` | 10 | `PD11` |
| 11 | `PD12` I2C4_SCL | 12 | `PD13` I2C4_SDA |
| 13 | `PD14` | 14 | `PD15` |

No ground pin is shown on J6 — take GND from J2 pin 3 or any ground pad.

These same free pins are where an external SPI NOR flash chip select would
go, if one is added later (see `PORTING.md`).

---

## 5. Pin map

Net names are quoted exactly as the schematic spells them. **Watch out: some
net names do not match the STM32H743 alternate-function reality** — see the
notes under the table.

### Port A

| Pin | MCU | Net | Function |
|---|---|---|---|
| 22 | PA0 | `ADC_IN` | battery sense, 2:1 divider off `BAT` |
| 23 | PA1 | `ENC_BUT` | encoder push-button (SW1) |
| 24 | PA2 | — | not connected |
| 25 | PA3 | — | not connected |
| 28 | PA4 | `SPI2_MSI_CS` | MSi001 chip select |
| 29 | PA5 | `SPI2_MSI_SCK` | MSi001 clock |
| 30 | PA6 | — | not connected |
| 31 | PA7 | `SPI2_MSI_MOSI` | MSi001 data |
| 67 | PA8 | `I2C3_TOUCH_SCL` | I2C3_SCL, AF4 |
| 68 | PA9 | `OTG_FS_VBUS` | USB VBUS sense (via JP2) |
| 69 | PA10 | `OTG_FS_ID` | USB ID |
| 70 | PA11 | `OTG_FS_D_N` | USB D−, AF10 |
| 71 | PA12 | `OTG_FS_D_P` | USB D+, AF10 |
| 72 | PA13 | `SWD_IO` | SWDIO |
| 76 | PA14 | `SW_CLK` | SWCLK |
| 77 | PA15 | `SPI3_LCD_CS` | panel CS — SPI3_NSS (AF6) or plain GPIO |

> **The `SPI2_MSI_*` names are wrong for this MCU.** On the STM32H743,
> `PA4`/`PA5`/`PA7` are **SPI1** (NSS/SCK/MOSI, AF5) or **SPI6** (AF8).
> They are not SPI2 pins. Since SPI6 is taken by the codec, the MSi001 is
> either on **SPI1** or bit-banged. The MSi001 control interface is a
> write-only 3-wire shift register, so bit-banging is entirely reasonable
> and avoids the question.

### Port B

| Pin | MCU | Net | Function |
|---|---|---|---|
| 34 | PB0 | `RESERVED_PB0` | free, on J6 |
| 35 | PB1 | `RESERVED_PB1` | free, on J6 |
| 36 | PB2 | `RESERVED_PB2` | free, on J6 |
| 89 | PB3 | `SPI6_CODEC_SCK` | NAU8822 control clock — SPI6_SCK, AF8 |
| 90 | PB4 | — | not connected |
| 91 | PB5 | `SPI6_CODEC_MOSI` | NAU8822 control data — SPI6_MOSI, AF8 |
| 92 | PB6 | `SPI6_CODEC_CS` | NAU8822 CSB — **plain GPIO**, PB6 is not an SPI6 NSS pin |
| 93 | PB7 | — | not connected |
| 95 | PB8 | `SPI_LCD_BL` | backlight gate (not an SPI signal despite the name) |
| 46 | PB10 | `CHRG` | TP4056 charging, active low |
| 47 | PB11 | `STBY` | TP4056 standby, active low |
| 51 | PB12 | `SPI2_FRAM_CS` | SPI2_NSS, AF5 |
| 52 | PB13 | `SPI2_FRAM_SCK` | SPI2_SCK, AF5 |
| 53 | PB14 | `SPI2_FRAM_MISO` | SPI2_MISO, AF5 |
| 54 | PB15 | `SPI2_FRAM_MOSI` | SPI2_MOSI, AF5 |

> The FRAM really is on hardware SPI2 — those four names are correct. It is
> only the MSi001 group that is mislabelled.

### Port C

| Pin | MCU | Net | Function |
|---|---|---|---|
| 15 | PC0 | `RENC1_A` | encoder 1 phase A |
| 16 | PC1 | `RENC1_B` | encoder 1 phase B |
| 17 | PC2_C | `RENC2_A` | encoder 2 phase A |
| 18 | PC3_C | `RENC2_B` | encoder 2 phase B |
| 32 | PC4 | — | not connected |
| 33 | PC5 | — | not connected |
| 63 | PC6 | `3.3V_ON` | **peripheral rail enable — see §3** |
| 64 | PC7 | `I2C3_TOUCH_RST` | touch reset |
| 65 | PC8 | `I2C3_TOUCH_IRQ` | touch interrupt |
| 66 | PC9 | `I2C3_TOUCH_SDA` | I2C3_SDA, AF4 |
| 78 | PC10 | `SPI3_LCD_SCK` | SPI3_SCK, AF6 |
| 79 | PC11 | `SPI3_LCD_MISO` | SPI3_MISO, AF6 |
| 80 | PC12 | `SPI3_LCD_MOSI` | SPI3_MOSI, AF6 |
| 7 | PC13 | `WAKEUP` | SW3 standby button |
| 8 | PC14 | — | OSC32_IN |
| 9 | PC15 | — | OSC32_OUT |

> `PC2_C` / `PC3_C` are the analog-switch versions of PC2/PC3 on the H743.
> For digital use they behave as normal GPIOs, but check `SYSCFG_PMCR`
> (`PC2SO`/`PC3SO`) if the encoder reads oddly.

### Port D

| Pin | MCU | Net | Function |
|---|---|---|---|
| 81 | PD0 | `SPI3_LCD_DC` | panel data/command |
| 82 | PD1 | `SPI3_LCD_RST` | panel reset |
| 83 | PD2 | — | to J7 (optional connector) |
| 84 | PD3 | — | to J7 (optional connector) |
| 85–88 | PD4–PD7 | — | not connected |
| 55–62 | PD8–PD15 | `RESERVED_PD8`…`PD15` | free, on J6. PD12/PD13 are I2C4_SCL/SDA |

### Port E

| Pin | MCU | Net | Function |
|---|---|---|---|
| 97 | PE0 | `PREAMP` | preamp supply enable |
| 98 | PE1 | `Vspk_ON` | speaker amp supply enable |
| 1 | PE2 | `SAI1_CODEC_MCLK` | SAI1_CK1 (MCLK out), AF6 |
| 2 | PE3 | `SAI1_CODEC_ADC_DATA` | SAI1_SD_B (RX), AF6 |
| 3 | PE4 | `SAI1_CODEC_FS` | SAI1_FS_A, AF6 |
| 4 | PE5 | `SAI1_CODEC_BCLK` | SAI1_SCK_A, AF6 |
| 5 | PE6 | `SAI1_CODEC_DAC_DATA` | SAI1_SD_A (TX), AF6 |
| 37 | PE7 | `RESERVED_PE7_UART7_RX` | UART7_RX, AF7 — on J6 |
| 38 | PE8 | `RESERVED_PE8_UART7_TX` | UART7_TX, AF7 — on J6 |
| 39 | PE9 | `RESERVED_PE9` | free, on J6 |
| 40–45 | PE10–PE15 | `BPF1`…`BPF6` | input band-filter selection |

> The SAI mapping is a textbook full-duplex pair: block **A** is the master
> transmitter (FS + SCK + SD_A to the codec DAC), block **B** is the
> synchronous slave receiver (SD_B from the codec ADC, carrying I/Q).
>
> **`CKSTR` must be SET on the receiving block** — measured, because the
> documentation is not reliable here. RM0433 describes `CKSTR` = 0 as
> sampling received data on the rising edge, which is what I²S wants;
> ST's own HAL sets `CKSTR` for every receiver, which says the opposite.
> On this board, clear is wrong and set is right.
>
> Getting it wrong does not look like a timing error, which is why it
> cost a cycle to find. Sampling on the same edge the codec changes its
> data returns the PREVIOUS bit, so every sample arrives shifted one
> place and the sign bit is whatever the last bit of the previous slot
> happened to be. A quiet input reading −310 then comes back alternating
> between +32613 and −155, and the summary statistics — mean 16384, rms
> 16384 — are arithmetically indistinguishable from an overloaded input.
> It sends you looking in the analogue half of the board.
>
> What settles it is the raw words. `7f65 ff62 ff68 ff64 7f65` — only
> bit 15 moving, everything below it near-constant — is a framing
> offset and cannot be a signal. `bringup/src/main.c` now tries all four
> combinations at startup and picks the one that reads near zero.

### Port H

| Pin | MCU | Net | Function |
|---|---|---|---|
| 12 | PH0 | `REF_CLK_MSI001` | OSC_IN ← 22 MHz oscillator (bypass mode) |
| 13 | PH1 | — | not connected |

---

## 6. Peripherals

### MSi001 — tuner

Mirics MSi001: integrated LNA, quadrature mixer and LO PLL, analog I/Q
baseband out. Control is a write-only 3-wire serial interface on
`PA4`/`PA5`/`PA7`. Reference clock 22 MHz, shared with the MCU.

I/Q baseband goes to the codec's microphone inputs:
`AUDIO_I_P`/`AUDIO_I_N` → `RMICP`/`RMICN`, `AUDIO_Q_P`/`AUDIO_Q_N` →
`LMICP`/`LMICN`.

Known hardware limit, not a firmware one: the PLL cannot cover roughly
**263–390 MHz**.

Control format, datasheet §5: **24-bit words, MSB first, clocked in on the
rising edge of CLK, and latched on the RISING EDGE OF EN** — so EN is an
ordinary active-low chip select and its rising edge *is* the write. The
**address is the last four bits sent**, i.e. the bottom of the word, with
data from bit 4 up. Nine-plus-twenty-bit fields and an address at the
bottom mean a byte-oriented driver cannot talk to this part at all;
`src/msi001.c` uses SPI1 with a native 24-bit frame.

**Register 1 carries the DC offset trim as well as the gain**, and the
two interact: the calibration is only valid for the gain it was taken
at. The driver selects `DCCAL` = periodic1 and recalibrates after a gain
change; the long version, and the measurement that forced it, is under
"The DC offset" below.

The bit layout was checked against both worked examples in datasheet
table 16 before any of it ran — THRESH = 312 decodes from its register
word, and INT = 32 with FRAC = 51 gives exactly 1672.5 MHz for a 26 MHz
part. Also, in one line of body text outside the register table:
**register 5 bits 19 and 21 must be set high**, which the table itself
contradicts for bit 19. The worked example settles it; both are set.

### Synthesiser, with a 22 MHz reference **[measured]**

`XTAL_SEL` = 001, and that choice also fixes the AM up-convert first IF at
**132 MHz** (table 23 — each supported crystal has its own value).

    FLO = (4 × FREF1 / LODIV) × (INT + (FRAC × 4096 + AFC) / (THRESH × 4096))

with `THRESH` pinned at 4095, giving a tuning step of **0.33 Hz** in AM
mode. LO dividers: AM 16, VHF 32, B3 16, B4/5 4, L 2. In the AM modes the
LO must clear the first IF too, so `FLO = Fc + 132 MHz`; everywhere else
it sits on the carrier.

Verified on the bench against a generator at 10.000 MHz: tuning to
9 997 000 Hz computed `INT` = 25, `FRAC` = 3348, `AFC` = 905, reported 0 Hz
error, and sweeping the tuned frequency ±5 kHz in 500 Hz steps put the
response peak **exactly** on the predicted tuning with about 20 dB of
discrimination against neighbouring steps.

### Band filters **[measured]**

Three PE42641 SP4T switches, controlled by `BPF1`…`BPF6` (`PE10`–`PE15`),
plus the preamp on `PE0`. Truth table: `RF1` = V2:0 V1:0, `RF2` = 1,0,
`RF3` = 0,1, `RF4` = 1,1. Each switch is drawn in the same style, with
`V2` taking the lower-numbered line of its pair:

| Switch | V2 | V1 |
|---|---|---|
| input (RF_IN → filter) | `BPF3` | `BPF4` |
| output (filter → tuner) | `BPF5` | `BPF6` |
| VHF sub-band | `BPF1` | `BPF2` |

**The two switches do not use the same code for the same band**, because
each filter is wired to a different port at each end. This is the trap in
this bank: selecting a band by writing the same two bits to both ends
connects the input of one filter to the output of another, and the result
is not a weak signal but no signal.

| Band | input port | output port | mask (BPF1 = bit 0) |
|---|---|---|---|
| 0–12 MHz | RF2 (1,0) | RF2 (1,0) | **0x14** |
| 12–30 MHz | RF4 (1,1) | RF1 (0,0) | **0x0C** |
| 30–60 MHz | RF1 (0,0) | RF4 (1,1) | **0x30** |
| VHF | RF3 (0,1) | RF3 (0,1) | **0x28** + sub-band |

VHF sub-band switch: VHF1 = RF1 (0x00), B4/5 = RF2 (0x01), L band = RF3
(0x02), VHF2 = RF4 (0x03).

**Which filter for which frequency** (21/11/2026, from the schematic and
the factory firmware, not swept on the bench): the schematic labels the
two VHF filters "VHF1 50MHz - 120MHz" and "VHF2 120MHz - 250MHz", VHF2
feeding the MSi001's B3 input; the factory firmware switches the
MSi001's path at 60 / 120 / 250 MHz (AM / VHF / B3, RE report 5.4). So
VHF1 = **0x28** with the VHF path for 60-120 MHz, VHF2 = **0x2B** with
B3 for 120-250. `msi001_band_for()` and `msi001_bpf_for()` use exactly
these edges, both on the tuner's frequency. Register 0 read back from
the shadow on the radio: VHF_MODE at 100 MHz, B3_MODE at 145.

Confirmed by sweeping all 64 combinations against the 10 MHz generator.
Exactly two paths pass strongly and they are the two whose passbands
border 10 MHz — 0x14 (the 0–12 filter) and 0x0C (the 12–30 filter) — at
about **23 dB** above the other 60 combinations. `BPF1`/`BPF2` are
don't-care on HF, so each real path appears four times in the sweep.

### Front end gains **[measured]**

**Retaken 04/11/2026 on a known path.** Every earlier version of this
table went through the add-on board in an inherited state — 30 dB of
attenuation the firmware did not know about, and an unrecorded filter —
so those numbers are void and these replace them.

**The bench, written down this time:** generator at 10.000 MHz, AM 70 %
at 1000 Hz, **100 mV through a 40 dB pad = −47 dBm** at the antenna.
Add-on board: 50 Ω input, 4.5 MHz highpass, attenuator stated per
block. Levels from the FFT peak, in upstream's uncalibrated dB — every
figure below is a *difference* between two rows, so the scale cancels.

**Working point A**, add-on attenuator 0 dB, gain reduction 48 dB
chosen by `rf_autorange()` on the hottest row:

| | peak | ADC | snr |
|---|---|---|---|
| AM **port 2** (75 Ω), preamp ON | 126.5 dB | 49 % | 71 dB |
| AM **port 1** (1 kΩ), preamp ON | 77.8 dB | 5 % | 35 dB |
| AM port 2, preamp OFF | 111.4 dB | 12 % | 64 dB |

- **Port 2 is +48.7 dB better than port 1** — reproducible to 0.2 dB
  over three runs. The old table could only say "at least 24.7 dB",
  because on port 1 the carrier had sunk below the noise and the peak
  search was reporting something else. With a clean path it is visible
  and the figure is a real one.
- **The preamp (BGA614 on `PE0`) is worth +15.1 dB**, which agrees with
  the old +14.7 dB — a ratio between two rows taken a second apart
  survives almost anything, which is why it was the one number in the
  old table that was right.

**Working point B** for the gain control, preamp OFF and the add-on
attenuator at 31.5 dB, so that fifty decibels of reduction fit under
the register's 59 dB limit:

| GR | peak | ADC | step |
|---|---|---|---|
| 12 dB | 107.4 dB | 17 % | — |
| 22 dB | 104.7 dB | 10 % | 2.7 dB ← compressing |
| 32 dB | 96.6 dB | 6 % | 8.1 dB ← compressing |
| 42 dB | 86.5 dB | 5 % | **10.1 dB** |
| 52 dB | 76.6 dB | 5 % | **9.9 dB** |

**Gain reduction tracks to 0.1 dB above about 32 dB, and the front end
compresses below it** at this input level. That is a more useful fact
than the old "within 0.7 dB" — it says *where* the control is
trustworthy, and the boundary is the thing an AGC has to respect.

> ### Step by step, not against a single reference
>
> Measured cumulatively against the 12 dB row, the same data read as a
> flat −9 dB error at 20, 30 and 40 dB and looked like a gain control
> broken everywhere. **A constant offset across every row is the
> signature of one bad row** — and the bad row was the reference, which
> was compressed.
>
> Consecutive steps put it where it belongs: three of the four are
> within 0.1 dB and the two at the bottom are the front end running out
> of linearity. Same numbers, opposite conclusion.

> ### Zero gain reduction cannot be a reference row, at any input level
>
> The first retake tried, wound the add-on attenuator to its 31.5 dB
> limit looking for a level where the preamp-on row would stop
> clipping, never found one, and reported the loudest **noise** bin to
> a tenth of a decibel for two rows. Every figure derived from them was
> wrong and none of it looked wrong.
>
> §8b already had the reason: at 0 dB GR the baseband has 77 dB of gain
> and fills the converter with its own amplified noise whatever the
> signal does. That operating point cannot be reached by attenuating
> the input.
>
> The old table had the same flaw. It did not show, because 30 dB of
> add-on attenuation nobody knew about was holding it in the linear
> region by accident.
>
> `addon_report_tail()` now flags any row whose peak is not within
> three bins of where the carrier must be, and no derived figure is
> printed unless both of its rows passed that check.

> ### Do not measure an absolute level in a single DFT bin
>
> These figures were first taken with `iq_measure()`, which evaluates one
> bin at exactly Fs/64 — arithmetically perfect and the wrong instrument.
> **The generator and this radio do not share a reference**, so the
> carrier arrives at 3000 Hz plus whatever the two oscillators disagree
> by. That bin is 192000/8192 = 23.4 Hz wide, so an offset of a hundred
> hertz puts the carrier several bins away and the measurement reads
> mostly window skirt.
>
> It under-read by **16 dB**: −20.6 dBFS against the FFT's −3.7 dBFS for
> the same signal. Everything *relative* in the table above survived,
> because the offset is common to all the rows and cancels in a ratio;
> the absolute numbers did not, and are corrected here.
>
> Image rejection is a ratio of two bins equally far off-tune, so it
> survives too — but it still has to be measured with the carrier clear
> of the noise floor.

Note that all three gain controls are **reductions**: zero means maximum
sensitivity, the opposite of what most of the upstream AGC code assumes.

### I/Q quality **[measured]**

With the carrier well clear of the noise floor (preamp on):

- **image rejection 22.7 to 32.2 dB** uncorrected, across runs, 34 dB
  with `iqbal.c` on by its own A/B test
- **DC / LO leakage −25.6 dBFS**, with 18 % of the converter in use —
  see the next section for what that figure used to be

The image figure is what `iqbal.c` exists to improve on. Measuring it
needs care: with the preamp off the image sits in the noise, and the
first version of that test reported it as anything between 8 and 31 dB on
consecutive captures.

### The DC offset, and why it is in millivolts **[measured]**

This was the worst number on the board and it was a firmware fault, not
a hardware one. It is written out at length because the symptom pointed
nowhere near the cause, and because the two numbers that mattered were
in different units from the ones being printed.

**What it looked like.** −7.7 dBFS of DC, 15 dB *above* the wanted
carrier and 8 dB from clipping — recorded here for weeks as "the open
problem of the RF chain", a hardware limitation to be worked around.

**What it was.** `src/msi001.c` wrote `DCCAL = 000` into register 1, on
the argument that static mode "calibrates once on synthesiser
programming and then holds", which avoids the spectral hole the tracking
modes dig at DC — and DC on a zero-IF receiver is the tuned frequency
itself. The argument was wrong at its first step. Table 20 defines 000
as **"No DC tracking"**, and section 5.4 is explicit that static mode
*holds* whatever the last calibration left. Selecting it is how the trim
is switched off.

The part arrives with `DCCAL = 101`, continuous (table 28's reset
column). **So the tuner's own power-on default was better than what this
firmware chose**, and the whole problem was a register written on a
misreading.

**In millivolts, which is the only unit that could settle it.** One ADC
count is 43.16 µV — the NAU8822's full scale is 1.0 Vrms at 0 dB PGA
with VDDA = 3.3 V, and this board runs the PGA at 0 dB with no boost.
Datasheet table 13 gives the MSi001's own residual offset as 50 mV in
continuous mode and 5 mV with the speed-up bit. At 0 dB of baseband gain
reduction, measured at the codec's input:

| mode | I | Q | ADC used |
|---|---|---|---|
| tuner untouched, reset default | +29 mV | −138 mV | — |
| **`DCCAL` = 000, what this firmware wrote** | **+1122 mV** | **−778 mV** | **99 %** |
| 1 shot, short track then hold | +199 mV | +168 mV | 39 % |
| **periodic1, register 6 reset timers** | **+69 mV** | **−13 mV** | **60 %** |
| continuous | +73 mV | −16 mV | 67 % |
| continuous + `DCCAL_SPEEDUP` | +33 mV | −13 mV | 100 % ← noise |

The second row is not an offset, it is a **railed converter**: ±1410 mV
is the rail. Every level measured through it was a measurement of
nothing, which is how step 5's image rejection came out as −4.8 dB on
the run that found this.

> ### The number that mattered was the spread, not the value
>
> That second row is unrepeatable, and *that* is the diagnosis. Four
> runs of identical code, same configuration, same bench, nothing
> connected to the antenna:
>
> | | run 1 | run 2 | run 3 | run 4 |
> |---|---|---|---|---|
> | `DCCAL` = 000, static | **+2.4** | **+0.3** | **−17.6** | **−22.6** dBFS |
> | periodic1 | −25.5 | −26.1 | −25.8 | −25.1 dBFS |
> | periodic1, 12 dB GR, preamp on | — | −25.5 | −25.6 | −25.5 dBFS |
>
> Static spans **25 dB** across four runs; periodic1 spans one, and the
> working configuration spans a tenth. Static mode holds whatever the
> trim happened to be left holding, so its result depends on the
> history of the part — what the factory firmware did, what the previous
> load did, how long ago. There is no value to measure.
>
> And −7.7 dBFS, the figure carried in this file for weeks as a property
> of the hardware, sits in the middle of that spread. It was never a
> measurement of the board. It was one sample of a quantity with no
> settled value, which is exactly the kind of number that survives
> being written down: it is plausible, it is reproducible to within a
> few dB on any given afternoon, and it is about the firmware.
>
> **A figure that will not repeat is not a loose measurement of
> something, it is a measurement of nothing** — and the way to find out
> is to take it four times before believing it once.

**Why not continuous, which the part itself defaults to.** periodic1
with register 6's reset timers tracks for **2.5 µs every 5.12 ms** — one
part in 2048, repeating at 195 Hz — and measured the same offset as
continuous to within 4 mV and the same AC noise to within 0.4 %. The
original worry about a permanent hole at the tuned frequency was the
right worry. It just has a cheaper answer than switching the trim off.

The speed-up row is the one to read twice: it wins on offset and
measured **six times the AC noise**, exactly as section 5.4 warns
("significant loss of energy around DC ... not intended to be used
during active reception"). A mode that wins on the number being
optimised and loses on the signal is not a win, and only a column for
each makes that visible.

**Static hold still has one use, and it is a good one.** Above about
20 dB of baseband gain reduction the offset is inside the trim's range,
and a calibration burst followed by a hold measured **+10 mV and
−6 mV** — better than continuous by a factor of seven and better than
the datasheet's own speed-up figure. Held for 20 s it moved by 2 mV,
so **the hold circuit does not measurably leak**; that was the one thing
that would have justified a periodic refresh, and it does not. Static is
not the default only because the default has to be safe at full gain.

**The calibration follows the gain**, which is section 5.4's other
point: "as ... the baseband gain settings changes then the effective DC
offset will also change". At 20 dB GR, the same gain measured −127 mV
and −232 mV with a stale calibration and +12 mV and +23 mV with a fresh
one — a factor of ten for a 2 ms burst. The datasheet's own power-up
flow (figure 5) writes register 2, the calibration trigger, *before*
register 1, which carries the gain, so following it literally calibrates
for a gain the part no longer has. `msi001_set_gain()` now recalibrates
behind itself whenever the selected mode is one that holds.

The whole measurement is `test_dc_offset()` in `bringup/src/main.c`, and
it needs no generator: a standing offset is the receiver's own error and
is cleanest measured with the input quiet.

### The whole receive path, checked against a number set outside it

**[measured]** Generator at 10.000 MHz, AM 70 % at 1000 Hz, 100 mV
through a 40 dB pad — about −47 dBm at the antenna. `test_am_envelope()`
in `bringup/src/main.c`:

| | measured | set |
|---|---|---|
| modulation depth | **66.4 %** | 70 % |
| 2nd harmonic of the envelope | −43.9 dB | — |
| 3rd harmonic | −49.7 dB | — |
| ADC used | 51 %, no clipping | — |

The depth is within **0.46 dB** of what the generator was told to
produce, and the envelope's harmonic distortion is 44 dB down. One
number, set outside the radio and nowhere in our code, reproduced
through the filter bank, the mixer, the baseband filter, the codec,
the SAI and the sample framing. It is the first end-to-end check of the
receive path that does not depend on a calibration of ours.

The residual 0.46 dB is **not** the receiver. I/Q imbalance was the
obvious suspect and `tools/envdft.py` rules it out: imbalance leaves the
recovered depth at exactly 70.0 %, because its wobble is at twice the
carrier rate — tens of kilohertz from 1000 Hz — and a coherent DFT at
1000 Hz never sees it. A 5 % relative error on a generator's modulation
setting is ordinary.

> ### The first version of this measurement accused the generator
>
> It read **62.5 % depth with the 3rd harmonic at −26.6 dB**, which is
> exactly what a distorting generator looks like. It was the
> instrument. `|I + jQ|` is not linear, so the mixer's standing DC
> offset beats against the rotating carrier, flattens the modulation
> and manufactures harmonics of it.
>
> With the offset this board actually has — +1705 and −332 counts
> against an envelope of 6371, so 26 % — `envdft.py` predicts **66.0 %
> and −37.0 dB from a mathematically perfect 70 % signal.** Almost the
> whole "distortion" was produced by measuring it.
>
> Subtracting the mean of I and Q first fixes it, and the fix is worth
> noting twice: it is the same correction upstream applies in
> `demod_am.c` as `s_rf_dc_i`/`s_rf_dc_q` before the NCO mix. Arriving
> at it independently, from the other end, is some evidence it belongs
> there.

### Upstream fixes this too, at the other level, and both are needed

Worth knowing before step 8, because it would be easy to find upstream's
version of this and conclude one of the two is redundant. They are not
the same fix and neither can do the other's job.

Upstream's receiver never puts a wanted signal at analogue DC.
`NCO_OFF_MIN_HZ` in `User/nco.h` keeps the station **6 to 44 kHz above
the LO, never below and never through the middle**, and the long comment
above the define in `User/main.c` says why: the LO leakage self-mixes to
0 Hz *always*, so a dial that walked across the centre would play a
carrier that is not on the air.

Six kilohertz of distance turned out not to be enough, and the way they
found out is worth reading. The original comment claimed the ±4 kHz
channel filter put the leakage "outside with margin" — true for AM and
SSB on the day it was written. Then the wide filters arrived and nobody
re-checked the number. Measured on the coefficients in `demod_am.c`,
with the offset at its 6 kHz minimum:

| channel filter | leakage rejected by |
|---|---|
| `CHF` — AM and SSB, −3 dB at 4 kHz | −14.5 dB |
| `NFM_CHF` — −3 dB at 6.25 kHz | −2.3 dB |
| `AIS_CHF` — −3 dB at 9 kHz | −0.1 dB |

A user reported "artefactos en los pasos pegados a la frecuencia
intermedia cero" and the `sim/rf_dc.c` bank put a figure on it: with a
weak station in NFM 6 kHz from the LO, **45.7 % of what came out of the
speaker was not the station.**

Their fix is `s_rf_dc_i`/`s_rf_dc_q` in `demod_am.c` — a running mean of
I and Q, subtracted before the NCO mix — and their own comment notes
that widening the 6 kHz hole is the obvious wrong answer, since it costs
dial travel and the DC is still there.

**Why this port still wants the hardware trim.** Subtracting a mean in
software removes the leakage from the *audio* and cannot recover a
single bit of converter range: by the time firmware sees a sample, the
headroom is already spent. That is the whole argument above, and
upstream's comment is quiet about headroom because software has nothing
to say about it. Conversely the hardware trim leaves 73 mV and −15 mV
of residual, which is still a DC term that self-mixes into NFM and AIS
audio exactly as measured. So:

- **the hardware trim buys headroom**, 26 dB of it, and nothing else can
- **upstream's subtraction buys audio**, and nothing else can

The port gets the second one free when `demod_am.c` is grafted. Neither
makes the other unnecessary, and `demod_am_get_rf_dc()` is the accessor
that will let the two be compared on this board.

> **The front-end table above has been retaken** on a known path, with
> the DC offset fixed and the add-on board under control. Its figures
> are the current ones.

### NAU8822 — codec

Nuvoton NAU8822, 32-pin. Control interface strapped to **SPI** (`MODE` pin),
driven from `PB3`/`PB5`/`PB6` — write-only 3-wire, like the tuner. Audio
interface is I²S to SAI1.

Serves two jobs at once: it is the **I/Q ADC** for the receiver (stereo line
in from MSi001) and the **audio DAC** plus speaker amplifier for the output.
That means ADC and DAC share one sample rate.

The control format, from §9.7 of the datasheet: **16 bits a transaction,
7 bits of register address then 9 bits of data**, MSB first, sampled on
the rising edge of SCLK — and **latched into the register on the RISING
EDGE OF CSB**. So CS framing is not housekeeping here, it is the write;
a CS that rises one bit early latches a 15-bit word, which the part
reads as a different register entirely. `TSCK` is 80 ns minimum (12.5
MHz); `src/nau8822.c` runs SPI6 at 1.875 MHz, which is as fast as a few
dozen configuration words ever need to be.

Nine data bits is also why a byte-oriented SPI driver cannot talk to
this part at all. SPI6 is configured for 16-bit frames.

**Outputs, from the schematic:**

| Codec pin | Goes to |
|---|---|
| `LSPKOUT` / `RSPKOUT` | **LS1, the internal speaker, BRIDGE TIED** |
| `LHP` / `RHP` | J5, the 3.5 mm jack, through 100 µF and a 10 µH choke |
| `AUXOUT1` / `AUXOUT2` | not connected |
| `LLIN`/`GPIO2`, `RLIN`/`GPIO3`, `LAUXIN`, `RAUXIN` | not connected |

> **The outputs come up MUTED, and that is deliberate.**
> `nau8822_configure()` sets the volumes, enables the drivers and then
> calls `nau8822_mute(1)`: a bring-up image must not come up shouting a
> full-scale tone into a small speaker. Nothing else in the firmware
> unmutes, so **anything that expects to hear something must unmute
> itself** — `test_audio_listen()` does, at −18 dB, and re-mutes
> afterwards. That gap cost three days of a silent speaker when step 4's
> tone test was retired and took its unmute with it; `WORKING.md`'s
> trap 17 has the story.
>
> `Vspk_ON` on `PE1` gates the amplifier's supply and **this firmware
> never writes it** — measured on 05/11/2026 as an output already driven
> high by the factory firmware. The audio works by inheritance, which is
> not the same as working.

**LS1 has no ground connection.** It sits between `LSPKOUT` and
`RSPKOUT`, so the two amplifiers must drive it in antiphase: register 43
`RSUBBYP` routes the right speaker amplifier through the submixer, whose
documented job is to invert RMIX for exactly this. That is worth 6 dB,
and it is why a radio this small is as loud as it is. Wire it in phase
by mistake and the speaker still works, very quietly, driven only by the
mismatch between the two amplifiers.

`VDDSPK` is **`Vskp_CODEC`, which is BAT itself** — the speaker supply
comes straight off the battery through Q4, so it is 4.2 V on a charger
and 3.0 V on a flat cell. It therefore spans the 3.6 V threshold that
decides whether the datasheet's 1.5× output boost (`SPKBST`) is allowed.
A setting that depends on the state of charge is a setting that will be
wrong one day, so the boost stays off and the loudness comes from the
bridge tie instead.

**There is no headphone detect.** J5 has only tip, ring and sleeve, and
the codec's jack-sense inputs are GPIO2 and GPIO3, both left
unconnected. Speaker-versus-headphone has to be a menu item, not a
sensor.

`RN5`/`RN6` are 33 Ω series networks on all the control and I²S lines.

### The ADC high-pass filter must be OFF, and this is not an audio decision

R14 resets to `0x100`, which enables a first-order 3.7 Hz filter to block
DC — correct for a microphone and actively wrong here. The MSi001
delivers quadrature baseband, so **zero Hz at the ADC is the centre of
the receiver's passband**, the tuned frequency itself. Leaving the
default would carve a notch out of the middle of every signal the radio
is pointed at.

The consequence runs the other way too, and is useful: with the filter
off the ADC output carries a real DC offset, which is what makes the
write-only control interface verifiable at all (see below).

### Verifying a write-only control interface **[measured]**

There is no read-back pin, so no register can be confirmed by asking.
Three effects were used instead, and all three answered:

- **R14[0] `LADCPL`** inverts the sign of the left ADC output. With the
  high-pass filter off the channel carries a standing offset, so one bit
  flips the sign of a large number. Measured: L mean −313 became +16322
  under the earlier framing fault, and tracks cleanly now.
- **R15/R16 ADC digital volume = 0** produced **exactly zero** on both
  channels. That is the single most useful measurement of the whole
  step: it proves the samples arriving over I²S really are the
  converter's output, which rules out every "the codec is not running"
  explanation in one line.
- **R45/R46 PGA gain** moves the whole chain.

Quiet-input noise floor at 192 kHz, inputs AC-coupled from an
unconfigured MSi001: **L mean −313 rms 7, R mean −44 rms 12** counts of
16-bit full scale.

**Variant: this unit has the NAU8822L**, not the NAU8822A that most Chinese
clones carry. The original Malahit used the L; field reports blame the
clones' "cracking" audio on the A. Worth recording, and worth less than it
looks — see below.

### Clocking, and why the L/A difference does not matter here

From the datasheet (Rev 3.3), the rules are short and strict:

- `IMCLK` is **always** exactly 256 × the converter sample rate.
- "IMCLK should not exceed 12.288 MHz under any condition" — which puts the
  datasheet ceiling at **48 kHz**.
- `IMCLK` comes out of the Master Clock Prescaler, whose input is **either
  the external MCLK pin or the internal PLL**, selected by `CLKM` in
  register R6 (0 = MCLK pin 11, 1 = PLL), with `MCLKSEL` = 000 giving
  divide-by-1.

On this board MCLK is driven by the MCU from `PE2`. So the right
configuration is `CLKM` = 0, `MCLKSEL` = 000, and `IMCLK` = MCLK straight
through. **The internal PLL is not used at all.**

That is what settles the variant question. The documented difference
between the A and the L is an extra PLL register bit — the L can divide the
PLL output by 2 where the A only offers 4. With the PLL bypassed, that bit
is never programmed and the two parts behave identically for our purposes.

What does matter is that either part has to run far outside its datasheet:
160 kHz needs IMCLK = 40.96 MHz, 192 kHz needs 49.152 MHz, against a stated
maximum of 12.288 MHz. The stock firmware does exactly this and works, so
the margin is real — but it is undocumented margin, and whether this part
reaches 192 kHz rather than 160 kHz is a question for the bench, not for
the datasheet. It is step 4 of `PORTING.md`.

One more constraint from the same section, easy to miss and expensive to
discover: FS, BCLK and IMCLK **must all derive from one reference**, or
audio quality degrades. Since the MCU already supplies MCLK, SAI1 must be
the I²S master and generate BCLK and FS from the same clock tree — which it
does, and which is the opposite of how the DEEPSDR 101 is wired, where the
AIC3204 is master and the GD32 is the slave.

### What the factory firmware leaves in the DMA controllers **[measured]**

Read over SWD on 04/11/2026, with our own image running and halted — so
this is what the factory firmware set up and then never got the chance to
tear down, and **it is still running**. Halting the core does not stop a
DMA controller; it is a separate bus master.

`RCC_AHB1ENR` = `0x08000023`: both DMA1 and DMA2 are clocked.

| DMAMUX1 ch | request | stream | CR | pointed at |
|---|---|---|---|---|
| 0 | 88 SAI1_B | DMA1 S0 | ours | — |
| 1 | 87 SAI1_A | DMA1 S1 | ours | — |
| 2 | 62 SPI3_TX | DMA1 S2 | `0x00030440`, EN=0 | the panel, idle |
| **9** | **87 SAI1_A** | **DMA2 S1** | **`0x000e2d41`, EN=1** | `0x30023de0` / `0x30025de0` |
| **13** | **88 SAI1_B** | **DMA2 S5** | **`0x00062d11`, EN=1** | `0x30027de0` / `0x30029de0` |

Both live streams are circular, double-buffered, half-word, and wired to
`SAI1_Block_A->DR` (`0x40015820`) and `SAI1_Block_B->DR` (`0x40015840`).
That is the stock firmware's audio transport, and it has been running
through every run of this bring-up.

`0x2200` is what `sai1_dma_start()`'s scan reports on **the first load
after a power cycle** — channels 9 and 13 — and `0x0000` on every reload
after it, because by then this firmware has unwired them and the factory
image has not run since. Both were measured; zero is the right answer
for a reload and the wrong one for a cold boot.

Two consequences, one harmless and one not:

- **It writes into D2 SRAM at `0x30000000`.** This image lives entirely
  in AXI SRAM at `0x24000000`, so nothing of ours has ever been
  corrupted by it. That is luck, not design, and it is a reason to keep
  the linker script where it is.
- **It shares the request line.** Harmless while the SAI is polled,
  because `DMAEN` is clear in both blocks and a peripheral that makes no
  requests feeds no stream — and then it cost exactly half the receive
  stream the moment our firmware set `DMAEN`. `sai1_dma_start()` now
  scans all sixteen multiplexer channels and takes back any that names
  one of its requests. `WORKING.md`'s trap 16 has the symptom.

Since 21/11/2026 channel 2 / DMA1 stream 2 is **ours**, for the panel
(`spi3.c`), and `spi3_dma_init()` does the same scan for request 62.
The boot line `Panel DMA` says which channels it had to take back; on
every load so far, none - the factory's stream 2 was idle (EN=0) and
ours now sits on the same channel.

### Display throughput **[measured]**

A full 480 × 320 frame at 18 bpp is 460 800 bytes and takes **129 ms** at
SPI3 = 30 MHz — 7.7 frames a second — of which the 1024-point complex FFT
is 1.7 ms. The display is the entire cost of a panorama on this radio and
the FFT is a rounding error against it.

**With the radio running, 21/11/2026** - generator on the dial, AM
demodulating, the full UI - one frame of waterfall (440 x 92, 121 kB,
32.4 ms of wire) and trace:

| transport | frame, median | p10 - p90 | waterfall |
|---|---|---|---|
| polled | 79.9 ms | 71.6 - 80.6 ms | 48.3 ms |
| DMA, each block started by the display loop | 65.5 ms | 59.3 - 68.5 ms | 39.5 ms |
| DMA, blocks chained by SPI3's EOT interrupt | **47.5 ms** | 44.4 - 60.9 ms | 30.2 ms |

The polled figure is not the wire's: the AM block interrupt takes the
core for 8.4 ms every 42.7 ms, and a polled SPI stops with it. The DMA
probe at boot - 256 bytes with CS high - takes 69 us against 68.3 us
of bit times, so the bus itself runs without gaps. `PORTING.md` step 21.

Measured both ways: drawing column by column into a 960-byte buffer costs
the same as building a whole 460 kB frame and sending it in one transfer,
within a few percent, because the per-transfer overhead of a window set
and a RAMWR is a dozen bytes against 960 bytes of pixels. **So the
framebuffer is not needed** — which answers the question `PORTING.md`
§3.1 raised about spending 460 kB of AXI SRAM on one.

### ILI9488 — display

3.5", 480 × 320, 50-pin FPC. The connector exposes a full DB0–DB17 parallel
bus, but on this board **only the 4-wire SPI pins are used**: SDI, SDO,
WRX/SCL, D/CX, CSX, RESET, with IM0/IM1/IM2 strapped by R23/R24/R25 (10 kΩ).
Series resistors RN1/RN2 (33 Ω) on the signal lines.

**Settled, on hardware and in the datasheet: 18 bits per pixel, three
bytes, and 16-bit is not available over this bus.**

The panel will *accept* the format change - write COLMOD (0x3A) = 0x55
and RDDCOLMOD (0x0C) reads back 0x05, so the register takes it - and then
pixel writes do nothing at all. Not garbage, not a black screen: the
previous picture simply stays. Confirmed by watching red and blue
alternate with a 16-bit green phase between them that never appeared.

The datasheet explains why, and contradicts itself on the way. Section
4.7.2, the 4-line serial interface, lists 3, 16 and 18 bits per pixel as
available formats - but its own data-format subsections are 4.7.2.1
(3-bit) and 4.7.2.2 (18-bit) and nothing else. There is no "SPI data for
16-bit/pixel" figure for this interface, while section 4.7.3 does have
one for the 8-bit parallel bus. The chip has a COLMOD register bit for
16-bit serial and no serial data path behind it.

So the folklore is right, the bullet list is wrong, and the figures are
the authority. **A frame is 480 x 320 x 3 = 460 800 bytes.**

Measured throughput, SPI3 on pll1_q_ck:

| SCK | full screen | frames/s |
|---|---|---|
| 15 MHz | 245 ms | 4 |
| 30 MHz | ~122 ms | 8 |

Both at the full bus rate, so the bus is the limit and DMA will free the
CPU without making frames arrive faster. The clock ceiling is the panel's
own write cycle, not the MCU's.

READING THE PANEL BACK WORKS, and is worth more than it looks: it is how
the format question was settled without anyone squinting at a screen.
MISO is on PC11. One correction to the obvious implementation - **there
is no dummy byte before the answer** for RDDCOLMOD; the data is the first
byte returned. Many ILI9488 drivers skip a leading byte and so read the
one after the answer, which for a register whose values are small looks
exactly like a plausible zero.

Note that 480 × 320 × 2 = 307 200 bytes, so unlike the DEEPSDR 101 a **full
RGB565 framebuffer fits in AXI SRAM** with room to spare. The band-compositor
approach in `gfx2.c` exists because the GD32 had 192 kB; it is not needed
here.

### Touch — FT6236, address 0x38

Capacitive, on **I2C3** (`PA8` SCL / `PC9` SDA) with IRQ on `PC8` and
reset on `PC7`. The schematic never names the controller; the bus does.

Confirmed on hardware by scanning 0x08..0x77 — exactly one device
answers:

```
bus idle levels: SCL=1 SDA=1
  device at 0x38
  0x38 chip id 0x36, vendor 0x11   <- CIPHER 0x36, FOCALTECH_ID 0x11
```

That is an **FT6236** (the family runs FT6206/FT6236/FT6336/FT6436,
distinguished by CIPHER at 0xA3). Register map in
`docs/FT6236-FT6336-FT6436L-FT6436_Datasheet.pdf`; the useful part is
the first fifteen bytes:

| Reg | Meaning |
|---|---|
| 0x00 | DEV_MODE |
| 0x01 | GEST_ID |
| 0x02 | TD_STATUS, `[3:0]` = points down |
| 0x03 | P1_XH — `[7:6]` event, `[3:0]` X high nibble |
| 0x04 | P1_XL |
| 0x05 | P1_YH — `[7:4]` touch id, `[3:0]` Y high nibble |
| 0x06 | P1_YL |
| 0x07 | P1_WEIGHT — reads 0 on this part |
| 0x08 | P1_MISC — area, reads 0 |
| 0x09–0x0E | the same six for a second finger |
| 0xA3 | CIPHER = 0x36 |
| 0xA4 | G_MODE — see below |
| 0xA8 | FOCALTECH_ID = 0x11 |

Event is 00 press, 01 lift, 10 contact, 11 none.

**Do not speak to it for 300 ms after its reset** - the datasheet's
`Trsi`, "time of starting to report point after resetting", and on
this unit a measured fact, 21/11/2026. Released from reset, it ACKs its
address almost at once, answers zeros for ~170 ms, and the read that
lands on the moment its I2C really comes alive - the 5th of a 10 ms
poll begun 120 ms after the reset - can leave it holding SDA mid-byte.
Warm, 20 resets per variant, interleaved:

| after the reset | not found | SDA stuck | mean |
|---|---|---|---|
| 120 ms, poll, no recovery | 2 / 20 | 3 / 20 | 629 ms (max 3 660) |
| 120 ms, poll, bus recovered when SDA sticks | 0 / 20 | 3 / 20 | 184 ms |
| 300 ms, then the same poll | 0 / 20 | 0 / 20 | 311 ms |

Cold, the same afternoon, on the 120 ms code: two power-ons of four
had SDA stuck from the 5th/6th read, and one never found the part.
`touch.c` waits `FT_WAKE_MS` = 300 now, and keeps the recovery.

**`PA8`/`PC9` are MCO1/MCO2 at AF0**, which is what `AFR` reads on a
cold chip. Switch `MODER` to alternate function before `AFR` holds 4
and both lines are pulled low for a few bus cycles - a START and a
STOP on the touch bus at every power-on. `i2c3.c` writes `AFR` first.

**Set G_MODE (0xA4) to 0x00.** It powers up at 0x01, "interrupt trigger
mode", where INT gives a pulse per report instead of sitting low while a
finger is down. Code that samples the pin level misses the pulses
between samples and concludes the panel is dead. 0x00 is level mode.

**Coordinates come in the panel's own orientation**, 320 across by 480
down, portrait, while the display is driven landscape. Measured over 910
reported points: `tx` spans 0..314 and `ty` spans 10..479, so the full
native range with no scaling needed. The transform is a swap and one
mirror:

```c
screen_x = ty;                 /* 0..479 */
screen_y = ILI9488_H - 1 - tx; /* 0..319 */
```

**Calibrated on this radio**, four targets inset 40 px from the edges:

| screen | raw |
|---|---|
| (40, 40) | (265, 46) |
| (440, 40) | (265, 442) |
| (440, 280) | (37, 440) |
| (40, 280) | (38, 45) |

Opposite corners agree on `tx` to within one count and on `ty` to within
two, so the four readings form a clean rectangle in raw space: the panel
is linear and the numbers are worth trusting. Averaging each edge gives
`ty` = 45 at x = 40 and 441 at x = 440, `tx` = 265 at y = 40 and 37 at
y = 280, hence

```c
#define TOUCH_TY_REF    45
#define TOUCH_TX_REF   265
#define TOUCH_SCALE_X  1034     /* 1.0101, x1024 */
#define TOUCH_SCALE_Y (-1077)   /* -1.0526, x1024 */

screen_x = (((ty - TOUCH_TY_REF) * TOUCH_SCALE_X) >> 10) + 40;
screen_y = (((tx - TOUCH_TX_REF) * TOUCH_SCALE_Y) >> 10) + 40;
```

Both scales are within a few percent of unity, which is the sanity
check — a controller bonded to this display should report over much the
same span as the display. The small residual before calibration, a
cursor sitting slightly right of the finger, was the glass offset, and
these constants absorb it. `src/touch.c` carries them.

**One oddity, unexplained.** `PC8` reads LOW continuously, with or
without a finger. Nothing depends on it — the driver polls TD_STATUS on
a timer and believes the register — but an interrupt-driven driver
cannot be written until this is understood. Suspect the line's polarity
or a missing pull-up.

### FM25W256 — FRAM

256 kbit = **32 kB** ferroelectric RAM on hardware SPI2 (`PB12`–`PB15`).
Byte-writable, no erase cycle, essentially unlimited endurance.

This is the *only* non-volatile storage besides the MCU's own flash. There is
no SPI NOR flash and no SD card on the DSP-1. The DEEPSDR firmware expects a
FAT32 disk on SPI NOR for maps, the station database, the aircraft database
and screenshots (`mapas/` alone is 11 MB). See `PORTING.md` for how that is
handled.

FRAM is a better home for settings than flash — no wear levelling, no erase
blocks, no write-cycle budget.

**It is FRAM — checked on this unit, 21/11/2026 [measured].** D11 on the
schematic; /WP and /HOLD tied to 3.3V, CS pulled up by R37 (10k). The
FM25W256 has no ID command, so it was identified by behaviour, against
the part most likely to sit in the same SO-8 instead, an SPI EEPROM
(`main.c`, `test_fram()`, `FRAM_TEST` 1 or 2, at 3.75 MHz):

| test | an SPI EEPROM | this part |
|---|---|---|
| RDSR | bit 0 = WIP | `0x00` |
| RDID (0x9F) | none | none - all `ff` (MB85RS256 and FM25V02 would answer) |
| sequential read past 0x7FFF | - | wraps to 0x0000: 32 kB |
| 96-byte write across the 64-byte page line at 0x3FC0, read at once | busy ~5 ms, then wrapped inside the page | 96/96 at once, never busy, nothing outside changed |

FM25W256 or its 3 V sibling FM25L256 - nothing electrical tells those
two apart, and nothing here depends on which.

**What is in it.** The factory firmware's settings - e.g. `68 9a 98 00`
at 0x007C, 10 001 000 Hz - in **0x0000-0x0CFF and 0x7000-0x7FFF**; the
rest, 0x0D00-0x6FFF, read blank (`ff`). The whole 32 kB was dumped over
SWD twice, identical, CRC-32 `0x6dee620e` as the firmware computed it,
into `Deep-Malahit/backups/fram_a.bin` and `fram_b.bin` - outside this
repository, like the flash backup. **Our settings live at 0x4000-0x403F**
(`settings.h`), two 32-byte slots; a later full read differed from the
backup in exactly those 40 bytes and nowhere else. (Two 20-byte records
then; version 2 of the record, the same evening, is 24 bytes - the same
two slots.)

### Controls **[measured]**

Two rotary encoders — `RENC1` on `PC0`/`PC1`, `RENC2` on `PC2_C`/`PC3_C` —
each with a push-button, plus `ENC_BUT` on `PA1` and the standby button SW3
on `PC13`. All encoder lines have 1 nF to ground (C61/C62/C66/C67).

The DEEPSDR 101 has one encoder. The second one is free UI budget.

**Both work, confirmed on the radio 05/11/2026.** Each common terminal
goes to ground, A and B carry nothing but their 1 nF cap, and there is
**no external pull anywhere** — so the MCU's internal pull-ups are not
optional, they are the circuit. With them on, a line floats high when
its contact is open and both lines read 0 at a detent, which is an
EC11's resting state and is easy to mistake for a short. Turning
produces the textbook cycle:

    00 -> 10 -> 11 -> 01 -> 00

four quarter-steps a detent, which is what upstream's
`QUARTER_STEPS_PER_DETENT = 4` expects. 297 line changes in twenty
seconds of ordinary turning.

**Which knob is which:** the small one on top is `RENC2`, the big one
below it is `RENC1`. First read off the order the two were turned in
against the order the pins moved — one report, and the weakest thing in
the section. Then confirmed from the other side, which is why it is
`[measured]`: with the driver moved to `PC0`/`PC1`, turning the **big**
knob tuned the receiver and turning the small one did nothing. The peak
ran +19 875 → −37 125 Hz monotonically, with no reversal, and then held
for 169 console lines while the small knob was being turned.

The driver is on `RENC1`, the big knob, because that is the one a hand
reaches for to tune.

#### The push-buttons are a RESISTOR LADDER, not GPIOs **[measured]**

This is the part no pin map can carry, and it is on page 3 of
`Malahit_schematics_15012020.pdf`: `ENC_BUT` is one analog line shared by
both buttons.

**The voltages are measured, and they are what the schematic's resistor
values produce** (20/11/2026, with `adc_init()` writing `SYSCFG_PMCR` so
`PA1`'s analog switch is open; 16-bit conversions on `ADC1_INP1`, read off
the display loop's `buttons:` line):

| | `ENC_BUT` measured | counts | what 47k/68k/22k predicts |
|---|---|---|---|
| idle | 3.300 V | 65535 | 65535 |
| **big knob** (ENC1) | **1.960 V** | 38887–38944 | 38751 (+0.4 %) |
| **small knob** (ENC2) | **1.050 V** | 20824–20898 | 20895 (−0.2 %) |
| both | **0.861 V** | 17064–17120 | 17122 (−0.2 %) |

So R18/R19 **are** the 68 kΩ and 22 kΩ on page 3, and the big knob is the
68 kΩ branch.

> **This table replaces one from 05/11/2026 that said the opposite** —
> 2.498 / 1.656 / 1.423 V, every press high, "R18/R19 are not what is
> fitted". It was measured through the **closed** `PA1` analog switch
> the factory firmware leaves behind (see below and `WORKING.md` trap 23):
> the switch hung the digital pad on the ladder and its load moved every
> plateau. The resistors were right; the configuration was somebody
> else's. The thresholds set from it would have taken the big knob for
> the small one and the small one for both.

> **Why these three plateaus are certainly the three button states.** The
> third one predicts itself: if "both" is the two branches in parallel,
> its reading follows from the other two with **no resistor value and no
> reference voltage anywhere in the arithmetic** — the scale cancels. That
> gives 17129 counts against 17091 measured, 0.2 %. It held through the
> closed switch too, which is why the old table was believed: a
> self-consistent ladder proves which state is which, not that nothing is
> loading it.
>
> **Which knob is which** was settled by the hand: big pressed first,
> small second, three times over, then both together; the plateaus
> arrived in that order.

Each class holds to within about 60 counts, against a narrowest gap of
3764 (small to both), so the classification has well over an order of
magnitude of margin. `src/adc.c` puts its thresholds at the midpoints
with a dead band, and keeps them as **fractions of full scale**: the
ladder's top goes to the same 3.3 V rail that feeds `VREF+`, so a sagging
rail moves the ladder and the full scale together and the ratio does not
move at all.

On a 3.3 V input `VIL` is 0.99 V and `VIH` is 2.31 V, so **both presses,
1.96 V and 1.05 V, sit in the undefined band** — two buttons on one pin
can only be told apart by measuring, and this one cannot be read as a
GPIO at all. The first attempt here did read it as a GPIO, got the idle
level, and announced it as a press.

`SYSCFG_PMCR`, as the factory firmware leaves it, is **`0x01000000`**
(measured 05/11/2026, and printed by `test_adc()` on every run): `PA0SO`
set, so the battery pad's analog switch is **open**; `PA1SO` **clear**,
so `ENC_BUT`'s switch is **CLOSED** and that pad reaches the digital port
`PA1` as well as the converter. `PC2SO`/`PC3SO` clear too, which is why
`PC2`/`PC3` work as ordinary GPIOs for the second encoder.

> **This contradicts an earlier note here**, which recorded `0x03000000`
> and both switches open, and the earlier one is the one to distrust: it
> was read once, in passing, while the subject was the encoder's digital
> lines. The value above is printed by a test whose whole job is this
> register.
>
> It does not change what the firmware does — nothing here writes `PMCR`,
> and a closed switch leaves the ladder undisturbed because `PA1`'s reset
> state is analog with no pull. What it changes is the explanation: with
> the switch closed there IS something on `PA1` to read as a GPIO. It
> would read the ladder through the input buffer, which for one of the two
> presses sits in the undefined band — so the pin is still unreadable as a
> digital input, for a different reason than the one written down before.

#### Which ADC channel, settled by measurement **[measured]**

No STM32H743 datasheet is in `../../docs/`, and this file's rule is that
a pin map gets quoted rather than remembered. So the channels were
identified on the radio by `test_adc_pins()`, which reads the four
candidates with `PCSEL` narrowed to **one at a time** and prints them
(05/11/2026):

| channel | counts | behaviour | verdict |
|---|---|---|---|
| 0 | 1 568 | wanders 927–1 984 | pad connected to nothing |
| 1 | 65 534 | steady, and followed the finger | **a mirror — see below** |
| **16** | **41 460** | steady 41 439–41 500 | **`ADC_IN`** |
| 17 | 5 528 → 28 400 | wandered over 23 000 counts | **`ENC_BUT`** |

> **The `ENC_BUT` row was wrong for a day, and the way it was wrong is
> worth keeping.** `PA1` has a second, analog-only pad with its own
> channel and an analog switch between the two, and `SYSCFG_PMCR` holds
> that switch — set by the **factory firmware**, before our image runs.
> On the day of the table above it was CLOSED, so channel 1 was a mirror
> of channel 17 and the finger confirmed the mirror. A power cycle later
> it came up open, channel 1 went floating, and every button in the radio
> reported itself held down for ever.
>
> `adc_init()` now **writes** `PMCR` rather than reading it, opening both
> switches every time, so both nets sit on the pads this board bonds:
> **`ENC_BUT` on channel 17, `ADC_IN` on channel 16**. The asymmetry
> noted below — one net apparently on a `_C` channel and one on a plain
> one — was the clue, and it was shrugged at.

**Channel 16 is the battery, confirmed against a multimeter** rather than
against a pin table: the meter read **4.15 V** at the cell, the schematic
makes `ADC_IN` the midpoint of two 10 kΩ resistors across it, and 41 460
counts doubled is **4.17 V** — six parts in a thousand.

The asymmetry — one net on a `_C` channel and the other on a plain one —
is not explained here and does not need to be. It would take the
package's own pin map to say which pad each pin bonds, and both channels
are confirmed by physical evidence, which outranks a pin map.

> **An earlier note here had the battery on channel 0, reading 3.05 V at
> the cell.** That was not a battery reading at all. `PCSEL` was being
> OR-ed rather than set, so the floating channel 0 was being read with
> the saturated `ENC_BUT` channel preselected alongside it and was
> dragged to a steady, plausible, entirely fictional 1.53 V. With one
> channel preselected at a time it wanders between 0.05 and 0.1 V, which
> is what an unconnected pad should look like. **A number that is stable
> is not thereby a measurement.**

#### The battery **[measured]**

`ADC_IN` is `ADC1_INP16`, the midpoint of R34/R36 (10 kΩ each) from
ground to `BAT`, with C78 100 nF across the lower half. **No diode
anywhere in that path** — the DEEPSDR's battery sense has one and this
board does not, so nothing has to be added back.

    V_cell = 2 x V_pin

`VREF+` is **measured, not assumed** — see the next section. The firmware
converts the part's own bandgap every millisecond and divides, so the
rail is followed rather than described.

#### The rail, measured against the chip's own bandgap **[measured]**

`VREFINT` is a ~1.2 V reference that does **not** move with the supply,
so converting it *against* the supply measures the supply:

    VDDA = 3300 mV x VREFIN_CAL / raw

On this part the internal channels are on **ADC3**, which is why there
are two converters in `src/adc.c`. Both facts needed to use it are
datasheet facts and there is no datasheet in `../../docs/`, so both were
established on the radio (05/11/2026):

- **The channel is `ADC3_INP19`.** Every ADC3 channel was read with the
  bandgap switched off and again with it on. Channel 19 moved by
  **11 200 counts** (13 148 → 24 348); no other channel moved by more
  than 512.
- **ST's factory calibration is at `0x1FF1E860`** and reads **24 327**,
  which is inside the only window a 16-bit conversion of a 1.18–1.24 V
  reference against 3.3 V can occupy. The two neighbours are plausible
  too — `0x1FF1E820` = 12 375 and `0x1FF1E840` = 16 418, the temperature
  sensor's two calibration points, the second higher than the first as
  it must be. A wrong base address does not produce three sensible
  numbers in a row.

| route to VDDA | result |
|---|---|
| this die's factory value | **3 298 mV** |
| the bandgap's nominal 1 216 mV | 3 274 mV |
| the multimeter, via the cell | 3 280 mV |

Within 0.7 % of each other, and **only the third did not come out of
this chip** — which is the only one that could have caught the other two
agreeing on something wrong. The firmware uses the factory value: it
removes the bandgap's part-to-part spread, which the nominal cannot.

> **What is left is the divider, and the reference cannot fix it** — two
> 10 kΩ resistors at 1 % allow about 40 mV at a full cell, and a ratio
> error is invisible from inside the chip. Checked from outside
> 05/11/2026: the radio said **4 163 mV**, the meter **4.16 V**.
>
> **No trim was applied, and that is the result.** The meter reads to
> 10 mV, so its answer covers 4 155–4 165 and the radio's number is
> inside it. The disagreement is smaller than the resolution of the
> instrument measuring it, so a constant fitted to it would be fitting
> the meter's last digit. `ADC_BAT_TRIM_PPT` stays 0 and
> `test_bat_calib()` prints the table to read it off, for the day a
> millivolt-resolution meter is at hand.

#### The die thermometer **[measured]**

Same converter, same method, its own enable bit: **`ADC3_INP18`** moved
by **9 369 counts** when `TSEN` was switched on, against 11 045 for the
bandgap on channel 19. Each was found by toggling *its own* bit — a
difference test identifies only what it actually switched, and the first
version of this test switched `VREFEN` twice and then named the
runner-up, which was simply an unconnected pad drifting.

ST measures the sensor at 30 °C and 110 °C, both at 3.3 V, and leaves
the two readings in system memory. They are believed because the pair
only makes sense one way:

| | counts | at 3.3 V |
|---|---|---|
| `TS_CAL1` (`0x1FF1E820`) | 12 375 | 623 mV at 30 °C |
| `TS_CAL2` (`0x1FF1E840`) | 16 418 | 827 mV at 110 °C |
| slope | 50.5 counts/°C | **2.537 mV/°C** |

A silicon sensor of this kind sits near 620 mV at 30 °C and climbs about
2.5 mV/°C. **Both land where they must — an offset and a slope,
independent of each other — out of two words that were only read.** No
other reading of them (12 bits, a different pair of temperatures, a
different reference) produces two textbook figures at once.

    T = 30 + (raw x VDDA / 3300 - TS_CAL1) x 80 / (TS_CAL2 - TS_CAL1)

The `VDDA / 3300` is not decoration: both calibration points were taken
at 3.3 V, so a reading against another rail has to be referred back
before it is compared with them. Skip it and the thermometer drifts as
the battery sags — about half a degree at 3.28 V.

**Measured: 40.5 °C.** That is the **die**, not the room: at 480 MHz
this part runs well above ambient, so a reading near the room
temperature would be the suspicious one.

The percentage in the header pill is a **straight line from 3.3 V to
4.2 V**, which is the weakest number on that screen. A lithium cell's
real curve is flat through the middle of its charge, so a line reads high
for most of a discharge and then falls off quickly, and a cell on charge
sits above its resting voltage besides. Upstream's `battery.c` does
exactly the same thing — it was read in the hope of grafting a real
curve, and it has two constants and an interpolation.

### USB

OTG_FS on `PA11`/`PA12`, ID on `PA10`, VBUS sense on `PA9` through JP2.
Micro-B connector J3, 33 Ω series (RN4). Also the charge input for the
TP4056.

---

## 7. THE ADD-ON BOARD — it is in the signal path and we do not drive it

**This unit has the optional add-on board fitted between the antenna and
the main PCB.** Two sources, and they are not interchangeable:

- `docs/Receiver20_Optional_PCB_EN.pdf` — **the vendor's own schematic**,
  and the authority for the connector mapping and the fitting notes.
- `~/Projects/Radio/MalahitDSP/MalahitAddonBoard/` — a KiCad rebuild of
  the same board. Good for connectivity, and its netlist is where the
  tables below come from — but its `J2` is drawn with the **main
  board's** 8-pin numbering, not the add-on's 10-pin one, and its 2021
  netlist predates the `J6` its own 2023 schematic shows. Check it
  against the vendor PDF before trusting a pin number from it.

> ### Read this before trusting any level measured so far
>
> Every RF measurement in this file was taken **through this board**,
> and our firmware has never written a single bit to it. So it has been
> sitting in whatever state the factory firmware last left — which
> includes **up to 31.5 dB of attenuation** and a choice of four
> filters, one of which would reject the 10 MHz test signal almost
> entirely.
>
> The stock firmware's display says `ATT 30` on this bench, and the
> radio's owner confirms that **the factory firmware leaves this
> board's attenuator at 30 dB**. So thirty decibels of the signal path
> were unaccounted for in everything above — not "if", measured.
>
> It is the "loading over SWD resets nothing" lesson one level further
> out: not an inherited *register*, an inherited *board*.
>
> And the relay is **latching** (HFD2/003-S-L2, two coils), so its
> position survives a power cycle. There is no default state to fall
> back on, for it or for the shift register's output latch.

### What is on it

- **High-Z input** — 2SK544 JFET follower into a TC4-1+ 4:1 transformer,
  selected by the latching relay K1 against the direct 50 Ω path.
- **PE4312 digital step attenuator** (DD1), 6 bits, 0.5 dB steps,
  0–31.5 dB.
- **A second filter bank** — two PE42641 SP4T switches (U3 in, U4 out)
  around four filters: a 0.5–1.5 MHz bandpass, a 1.5–4.5 MHz bandpass,
  a 4.5 MHz highpass and a 500 kHz lowpass.
- **74HC595 shift register** (U2) driving all of it.
- Its own LDO (LD3985M33, `3v3sw`) enabled by `Vext_ON`.

### The control interface — the two sides of the harness

Six signals, and that is the whole interface. **The two ends are
numbered differently and it is easy to describe the wrong one** —
`docs/Receiver20_Optional_PCB_EN.pdf` is the vendor's own drawing and
carries the mapping as a legend, "Connecting pins: Add. board – Main
board". Its note says the link is made "by soldering pieces of any
wire" between the two, so there is no cable to inspect either.

| add-on J2 (10 pins) | signal | vendor's main-board pin (8 pins) |
|---|---|---|
| 1 | `+BAT` | +BAT |
| 2 | `Vext_ON` — enables the LDO: no power, no board | 1 |
| 3 | `REL_R_ON` — relay RESET coil (latching) | 2 |
| 4, 5 | not connected | NC |
| 6 | `REL_S_ON` — relay SET coil (latching) | 3 |
| 7 | `595DIN` — 74HC595 SER | 6 |
| 8 | `595LE` — 74HC595 RCLK, 47 kΩ pull-down | 4 |
| 9 | `595CLK` — 74HC595 SRCLK | 5 |
| 10 | `GND` | GND |

**The vendor's main-board side is an 8-pin connector and this radio's
is the 10-pin `J7`**, so the legend's right-hand column does not apply
here directly: J7 carries `BAT`, then `PD15` down to `PD8`, then `GND`.
Wiring J2 straight through to J7 pin-for-pin is therefore **wrong**,
and the live registers prove it — that mapping would put `Vext_ON` on
`PD15`, which is configured as an *input*.

What this unit actually uses is in "The MCU pins" below, taken from the
firmware that drives it.

`OE` is tied to ground, so the outputs are always enabled. `SRCLR` is
held by an RC from `3v3sw` (47 kΩ, 1 µF ≈ 47 ms), which clears the
**shift register** at power-up — but *not* the output latch, so the
outputs come up undefined until something clocks RCLK.

### The shift-register byte

Shifting MSB first, the **first bit sent ends up at QH**:

| bit | pin | signal |
|---|---|---|
| 7 | QH | `ATT_DATA` |
| 6 | QG | `ATT_CLK` |
| 5 | QF | `ATT_LE` |
| 4 | QE | `BPFE12` — U3 (input switch) V2 |
| 3 | QD | `BPFE11` — U3 V1 |
| 2 | QC | `BPFE22` — U4 (output switch) V2 |
| 1 | QB | `BPFE21` — U4 V1 |
| 0 | QA | not connected |

**The attenuator is programmed THROUGH the shift register.** Its
`DATA`, `CLK` and `LE` are three of the 595's outputs, so each bit sent
to the PE4312 costs three whole 595 writes — set data, raise clock,
lower clock. Around 160 clock edges for one attenuation change, which
is slow and completely acceptable. The filter bits must be held
unchanged throughout, or the band switches while the attenuator is
being written.

### The filter switches **[measured]** — the same trap as the main board

The two PE42641s put the same filter on **different port numbers**, so
`BPFE1x` and `BPFE2x` need **different** values. Exactly as in §6, and
with the same consequence: writing the same code to both ends joins the
input of one filter to the output of another, and the result is not a
weak signal but **no** signal.

| filter | U3 (in) | U4 (out) | byte |
|---|---|---|---|
| 1.5–4.5 MHz | RF1 (0,0) | RF3 (0,1) | `0x22` |
| 0.5–1.5 MHz | RF2 (1,0) | RF4 (1,1) | `0x36` |
| **4.5 MHz highpass** | RF3 (0,1) | RF1 (0,0) | **`0x28`** |
| 500 kHz lowpass | RF4 (1,1) | RF2 (1,0) | `0x3c` |

(Bytes include `ATT_LE` = `0x20`, which is the attenuator lines' resting
value — see below.)

**Established by sweeping all sixteen codes**, not by reading a
drawing, because the two drawings disagree about U4's port order and
the vendor's turned out to be the right one. Against the 10 MHz
carrier, exactly one code passed:

| | peak | snr |
|---|---|---|
| E12=0 E11=1 E22=0 E21=0 | **136 dB** | **56 dB** |
| E12=0 E11=0 E22=0 E21=1 | 99 dB | 23 dB |
| the other fourteen | 84–87 dB, peak wandering | 8–11 dB |

The second row is the confirmation worth having: a 10 MHz carrier
37 dB down through a **1.5–4.5 MHz bandpass** is a real filter's
stopband, and it lands on the one code the same truth table predicts
for that filter. Two independent rows, one table.

With the gain set once on the passing filter so that nothing clips —
a rejection measured against a clipped reference is not a rejection —
the bank measures:

| filter | peak | rejection at 10 MHz |
|---|---|---|
| 4.5 MHz highpass | 126 dB, snr 70 dB | — passes |
| 1.5–4.5 MHz | 89 dB | **37 dB** |
| 500 kHz lowpass | 72 dB | **54 dB** |
| 0.5–1.5 MHz | 59 dB, into the noise | **67 dB** |

The ordering is the physical one: the adjacent band rejects least, the
far one most.

> ### This file asserted the opposite, on the rebuild's authority
>
> The first driver took the port order from the KiCad rebuild, whose
> U4 lists its four ports differently from the vendor's schematic, and
> this section confidently said the trap was *not* here. The four codes
> it produced — `0x00`, `0x14`, `0x0A`, `0x1E` — are four of the
> fourteen that connect nothing.
>
> The warning at the top of this section, that the rebuild must be
> checked against the vendor PDF before a pin number is taken from it,
> was written two hours earlier **by the same hand that then did not
> check**. Writing a caveat down is not the same as applying it.
>
> Sixteen combinations is a minute of bench time and settles the
> question against both drawings at once. §6 reached the main board's
> bank the same way, by sweeping all 64.

### The attenuator

`P/S` (pin 13) is tied to `3v3sw`, and the six parallel control pins are
all commoned through a single 10 kΩ to ground — which can only mean
**serial mode**, since in parallel mode those pins could only ever
select 0 dB or 31.5 dB together. *Inferred from the wiring; the PE4312
datasheet is not in `docs/` and should confirm it before the driver is
believed.* `PUP1`/`PUP2` are pulled low through 10 kΩ, which sets the
power-up attenuation state.

`ATT_DATA` reaches the part through a 10 kΩ series resistor (R24),
which is unusual on a logic line and slows the edge; at the speeds a
bit-bang through a shift register runs, it does not matter.

The programming sequence, instruction for instruction from the factory
image:

    ATT_LE = 0                      open the latch
    six times:
        ATT_DATA = next bit, MSB first
        ATT_CLK  = 0
        ATT_CLK  = 1                rising edge clocks it in
    ATT_DATA = 0
    ATT_LE   = 1                    rising edge latches, AND STAYS HIGH
    ATT_CLK  = 0

Each of those lines is a whole 595 write, so one attenuation change
costs about twenty-two of them — a fraction of a millisecond.

**The register is in half-decibels and the factory passes whole ones**,
doubling on the way in (`lsls r5, r0, #1`), which is how both the step
size and the bit order are known rather than guessed.

> **`ATT_LE` rests HIGH**, and that is not obvious. The first version
> of this driver forced it low on every filter write, reasoning that a
> low latch line cannot produce a latch edge — true, and beside the
> point, because it took the part out of the state it is left sitting
> in. **Idle a line where the firmware that works idles it**, and
> change only the bits being asked for.

### And it attenuates by what it is told **[measured]**

`test_addon_atten()` walks the **six individual bits** rather than
round decibels, because each bit has twice the weight of the last and
that makes four separate claims testable at once — bit order, step
size, which edge latches, and whether the register counts decibels or
halves:

| code | asked | measured | error |
|---|---|---|---|
| 1 | 0.5 dB | 0.4 dB | +0.1 |
| 2 | 1.0 dB | 1.0 dB | 0.0 |
| 4 | 2.0 dB | 2.0 dB | 0.0 |
| 8 | 4.0 dB | 3.8 dB | +0.2 |
| 16 | 8.0 dB | 7.9 dB | +0.1 |
| 32 | 16.0 dB | 15.7 dB | +0.3 |
| 63 | 31.5 dB | 31.2 dB | +0.3 |
| 0 | 0.0 dB | **0.0 dB** | exact return |

**Within 0.3 dB over the whole 31.5 dB range.** The doubling down the
measured column confirms the bit order; the fact that code 2 is 1 dB
and code 32 is 16 dB confirms half-decibel steps; and the last row
returning exactly to the reference is what distinguishes a part being
programmed from one drifting.

> ### A 1 dB instrument cannot check a 0.5 dB part
>
> The first run of this sweep read a tidy, systematic **1 dB short** at
> every setting above 2 dB. A systematic error of that size and shape
> looks like a real property of the part, and it was entirely in the
> measurement: `(int32_t)pk.db` truncated both the reference and the
> reading, so their difference lost up to a decibel.
>
> Reporting in tenths made the same sweep accurate to 0.3 dB. That the
> instrument's resolution has to be finer than the thing it is checking
> should have been settled before the measurement rather than after it.

### The MCU pins **[measured, and read out of the factory firmware]**

J2 goes to **J7** on the main board, the 10-pin header the schematic
labels `OPTIONAL`: pin 1 is `BAT`, pins 2–9 are `PD15` down to `PD8`,
pin 10 is `GND`. (The 2×7 header carrying `PB0`–`PB2`, `PE7`–`PE9` and
`PD8`–`PD15` is **J6**, which is the debug-UART one and is not fitted
on this unit.)

The hand-soldered harness is **not** pin-for-pin, so which J7 pin
carries which signal had to be measured rather than read off:

| signal | pin | J7 | how it was established |
|---|---|---|---|
| `Vext_ON` | **PD9** | 8 | the only remaining driven output that is HIGH, and the LDO's INH is active-high with a 10 kΩ pull-down |
| `REL_?_ON` | **PD8** | 9 | factory firmware, 10 ms pulse pair |
| `REL_?_ON` | **PD11** | 6 | factory firmware, 10 ms pulse pair |
| `595LE` | **PD10** | 7 | factory firmware, held low across the shift and raised at the end |
| `595DIN` | **PD12** | 5 | factory firmware, set per bit from the MSB |
| `595CLK` | **PD13** | 4 | factory firmware, toggled twice per bit |

> **The two HIGH pins are exactly the two that should be high**, which
> is the check that makes the rest of this credible rather than
> plausible. `Vext_ON` must be high or the board has no power at all,
> and the shift routine *ends* by raising `595LE` — so PD9 and PD10
> reading 1, and every other one of them reading 0, is what this
> assignment predicts and not something it was fitted to.

`PD14` is a seventh driven output on the same header and is **not** one
of these six; it is touched by a different routine and nothing here
knows what it does. `PD15` is an input.

Two sources, and they agree. First, the live registers: our firmware
never writes port D above bit 1, so **the configuration the factory
firmware left was still in `GPIOD->MODER` and `ODR` and could simply be
read over SWD** — PD8–PD14 outputs, PD15 input, PD9 and PD10 high. That
is the "loading over SWD resets nothing" rule finally being *useful*.

Second, the factory image itself (`Firmware/dsp1_dump.bin`, plaintext
Thumb). The relay pair is a textbook latching drive — set one line,
clear the other, delay 10, clear the first — and the shift-out is
unmistakable:

    bic  #0x400     PD10 = 0            LE low for the whole shift
    movs r5, #8
    loop:
      tst  #0x80    test the MSB        MSB FIRST
      orr/bic #0x1000   PD12 = that bit DATA
      lsls r3, #1
      bic  #0x2000  PD13 = 0            CLK low
      orr  #0x2000  PD13 = 1            CLK high, shifts
      subs r5, #1; bne loop
    bic  #0x1000    PD12 = 0
    orr  #0x400     PD10 = 1            LE high, latches

That also **confirms MSB-first**, so the byte layout above — first bit
sent lands in QH — is the factory's own convention and not an
assumption.

### The relay is ours **[measured]**

`test_addon_relay()` drives it, `src/addon.c` is the driver, and the
first thing it established is audible: **the relay clicks.** PD8 and
PD11 are the two coils, one 10 ms pulse each, and it latches.

Through a 50 Ω cable from the generator, over two separate loads:

| position | peak | ADC | floor | snr |
|---|---|---|---|---|
| as found (factory's own) | 123 dB | 48 % | 82 | 41 dB |
| after PD8 | 123–124 dB | 49–51 % | 82 | 41 dB |
| after PD11 | 120 dB | 31–37 % | 82 | 37 dB |

So the factory firmware leaves it in **PD8's position**, and the two
positions differ by a repeatable **3–4 dB** with an identical noise
floor.

> ### Three decibels is the right answer, and the first test called it a failure
>
> That test asked for tens of decibels and reported "the relay is not
> switching" for a relay that was audibly switching. Two mistakes, both
> about the circuit rather than the code:
>
> - **R19 and R20, the 470 kΩ pair, are a static drain to ground**, not
>   940 kΩ in series with the signal. Nothing puts that in the path.
> - **A High-Z input hardly loads a 50 Ω generator**, so it sees the
>   full open-circuit voltage — 6 dB more than the matched case — and
>   the source follower and the TC4-1+ give some of that back. A few
>   decibels between the two paths is what the physics predicts.
>
> The lesson is not "use a smaller threshold". It is that a threshold
> is a prediction, and this one was never checked against the circuit
> before being used to judge the hardware.

### And PD8 is the 50 Ω input **[measured]**

Settled without a whip, by letting the firmware that has the *label*
choose the position and then asking which of our coils reproduces it:
the stock UI was set to **50 Ω**, this image was loaded, and the level
was read **before either coil was touched**.

| | peak | ADC |
|---|---|---|
| **as found, untouched** | **123 dB** | **52 %** |
| after PD8 | 124 dB | 51–54 % |
| after PD11 | 120 dB | 32–37 % |

Zero decibels from PD8 and three from PD11, and the ADC column says the
same thing more plainly. So **PD8 selects 50 Ω and PD11 selects
High-Z**, and `addon.c` names them `addon_input_50r()` and
`addon_input_highz()` rather than A and B.

Two details that make this an experiment rather than a reading:

- **The gain is fixed at 16 dB for all three**, not autoranged.
  `rf_autorange()` exists to hold the converter near half scale, so
  letting it run between the readings would have quietly cancelled the
  very difference being measured.
- **All three are in one run**, so nothing about the bench can drift
  between the numbers being compared.

> The first version of the decision rule demanded the nearer candidate
> be two whole decibels nearer, got 1 dB against 3 dB, and printed
> "cannot tell" on the run that answered the question. A fixed margin
> between two distances is the wrong shape of test when the quantity
> being resolved is itself only three decibels; the natural dividing
> line is **half the gap between the two candidates**, which is what it
> uses now.

### It needs 20-25 ms after Vext_ON before the 595 takes a write **[measured]**

Writes to the 74HC595 sooner than that after `Vext_ON` rises are simply
lost - no error, nothing to read back - and the filter switches sit on
whatever they powered up with, which is NO signal. Swept 21/11/2026 by
`test_addon_cold()` in `main.c`, MCU warm, the board powered down and up
each cycle, boot order (attenuator, filter, relay pulse):

| wait after Vext_ON | 10 | 15 | 20 | 25 | 30 | 50 | 100 | 200 ms |
|---|---|---|---|---|---|---|---|---|
| deaf | 6/6 | 6/6 | 6/6 | 0/6 | 0/6 | 0/6 | 0/6 | 0/6 |

Not the relay: the same writes with no relay pulse at all were deaf 12
in 12. The LD3985 itself is up in well under a millisecond, so something
else on the board holds it off - an RC power-on reset on the 595 would
do it; not checked against the schematic. `addon_init()` now waits
`ADDON_RAIL_MS` = 200 ms when it finds the board unpowered. At 10 ms it
made some cold starts deaf, intermittently, because the boot's console
lines happened to push the writes to 15-25 ms, right on the threshold.

### What is still open
- **What PD14 does.** It is driven, it is on this header, and it is not
  one of the six.
- **The RF topology through J6/J3 on the add-on board.** The 2021
  netlist predates the `J6` that the 2023 schematic shows, so the
  through-path is read off the drawing and not yet confirmed from a
  netlist. The vendor's own note is the practical statement of it:
  *"The output of the additional board is connected to the input of the
  UFL–UFL receiver board with a pigtail"*, and the standard SMA–UFL
  pigtail has to be dismantled to fit it. So the add-on board is in
  **series** ahead of the receiver, not a branch — there is no bypass.
- **The PE4312 `P/S` polarity**, inferred from the wiring above. The
  datasheet is not in `docs/`.

None of these blocks writing the driver; all four want checking before
any level measured through this board is quoted again.
