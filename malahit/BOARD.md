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

Other power control:

| Net | Pin | Function |
|---|---|---|
| `PREAMP` | `PE0` | Q7 (2N7002) → Q3 (IRLML6402) → `5V_PREAMP` |
| `Vspk_ON` | `PE1` | Q8 (2N7002) → Q4 (IRLML6402) → `Vskp_CODEC` (speaker amp supply) |
| `SPI_LCD_BL` | `PB8` | R27/R28 → Q1 (IRLML2502) → panel LED anode. PWM capable: TIM4_CH3 (AF2) or TIM16_CH1 (AF1) |

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
     does: a button combination at boot that jumps to `0x1FF00000`.

   Item three is cheap to write and should exist before the first flash
   write, not after it.

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

Confirmed by sweeping all 64 combinations against the 10 MHz generator.
Exactly two paths pass strongly and they are the two whose passbands
border 10 MHz — 0x14 (the 0–12 filter) and 0x0C (the 12–30 filter) — at
about **23 dB** above the other 60 combinations. `BPF1`/`BPF2` are
don't-care on HF, so each real path appears four times in the sweep.

### Front end gains **[measured]**

At 10 MHz, against the same generator, levels taken from the **FFT peak**:

| Change | Carrier level | peak at |
|---|---|---|
| AM **port 2** (75 Ω), preamp off, 0 dB GR | −18.4 dBFS | +3000 Hz |
| AM **port 1** (1 kΩ), preamp off, 0 dB GR | −43.1 dBFS | −937 Hz |
| AM port 2, **preamp on** | −3.7 dBFS | +3000 Hz |
| AM port 2, 20 dB gain reduction | −37.7 dBFS | +3000 Hz |
| AM port 2, 40 dB gain reduction | −58.0 dBFS | +3000 Hz |
| AM port 2, 59 dB gain reduction | −75.3 dBFS | +71625 Hz |

**Use AM port 2.** It is at least 24.7 dB better here, which is what the
input impedances predict — port 1 is 1 kΩ for a ferrite rod, port 2 is
75 Ω for an antenna, and this board feeds the tuner from the filter bank.
"At least", because on port 1 the carrier is no longer the strongest
thing in the band: the peak column shows the search landing on something
else at −937 Hz, which is the measurement saying so rather than
pretending. The same happens at 59 dB of gain reduction.

The preamp (BGA614 on `PE0`) is worth **+14.7 dB**. Programmed gain
reduction tracks measurement within about 0.7 dB at 20 dB and 0.4 dB at
40 dB, which is the evidence that register 1 is being written correctly.

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

- **image rejection 22.7 to 32.2 dB** uncorrected, across runs
- **DC / LO leakage −7.7 dBFS, which is 15 dB *above* the carrier** and
  only 8 dB from clipping the ADC

The image figure is what `iqbal.c` exists to improve on. **The DC figure
is the open problem of the RF chain**: with the preamp on, the mixer's own
offset is within 8 dB of full scale, so most of the ADC's range is being
spent digitising the receiver's own error rather than signal. That is a
sensitivity loss, not a cosmetic one, and it is the first thing to attack
in step 7.

More precisely: the mixer's own offset is the largest single
thing in the band and it eats ADC headroom that the wanted signal could
have used. `src/msi001.c` currently selects **static** DC calibration,
which recalibrates on every synthesiser programming and then holds — the
right choice for a receiver, since the periodic and continuous modes keep
digging a hole at DC, and on a zero-IF receiver DC is the tuned frequency
itself. If the leakage turns out to drift, register 6's timers and the
periodic modes are the answer, but that should be a response to measured
drift rather than a precaution.

Measuring either number needs care: with the preamp off the image sits in
the noise, and the first version of this test reported it as anything
between 8 and 31 dB on consecutive captures.

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

### Display throughput **[measured]**

A full 480 × 320 frame at 18 bpp is 460 800 bytes and takes **129 ms** at
SPI3 = 30 MHz — 7.7 frames a second — of which the 1024-point complex FFT
is 1.7 ms. The display is the entire cost of a panorama on this radio and
the FFT is a rounding error against it.

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

### Controls

Two rotary encoders — `RENC1` on `PC0`/`PC1`, `RENC2` on `PC2_C`/`PC3_C` —
each with a push-button, plus `ENC_BUT` on `PA1` and the standby button SW3
on `PC13`. All encoder lines have 1 nF to ground (C61/C62/C66/C67).

The DEEPSDR 101 has one encoder. The second one is free UI budget.

### USB

OTG_FS on `PA11`/`PA12`, ID on `PA10`, VBUS sense on `PA9` through JP2.
Micro-B connector J3, 33 Ω series (RN4). Also the charge input for the
TP4056.
