# Malahit DSP-1 — board reference

Target board for this port. Everything below was read off the official
schematic (`Receiver20.sch`, KiCad 5.0, dated 2019-10-05, released as
`Malahit_schematics_15012020.pdf`, by R6DCY / RX9CIM / R6DAN) and, where
marked **[measured]**, confirmed on a real unit over SWD.

This is *not* the DEEPSDR 101 board. See `../README.md` for the firmware this
port starts from, and `PORTING.md` for what has to change.

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

This matters: `RCC_CR.HSEBYP` must be set before enabling HSE, and every PLL
divider has to be worked out for 22 MHz, not for the 8/25 MHz that most
STM32H7 examples assume.

**LSE = 32.768 kHz** crystal X2 on `PC14`/`PC15` (OSC32_IN/OSC32_OUT), with
C71/C72 = 10 pF. Backup domain is fed from `CPU_VBAT`.

### Sample-rate clocking

`SAI1_CODEC_MCLK` is an **MCU output** (`PE2`), so the STM32 is the clock
master and the NAU8822 is the slave. MCLK must be exactly **256 × Fs**. The
stock firmware runs 160 / 80 / 40 kHz (panorama spans 160 / 80 / 40 kHz);
the DEEPSDR firmware assumes 192 / 96 / 48 kHz. Which of the two this port
ends up using is an open question — see `PORTING.md`.

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

### ROM DFU — jumper JP1

`BOOT0` (pin 94) is held low by R17 (10 kΩ) and pulled high through JP1 +
R15 (470 Ω). Closing JP1 at power-up enters the STM32 system bootloader,
which enumerates as `0483:df11` with `@Internal Flash /0x08000000/16*128Kg`.

**This jumper is the safety net: it is pure hardware, so no firmware we write
can lock us out.** The radio cannot be bricked by a bad flash as long as JP1
is reachable.

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

Input band filters are switched by `BPF1`…`BPF6` (`PE10`–`PE15`) plus the
preamp on `PE0`. The mapping from frequency to filter still has to be
determined — see `PORTING.md`.

### NAU8822 — codec

Nuvoton NAU8822, 32-pin. Control interface strapped to **SPI** (`MODE` pin),
driven from `PB3`/`PB5`/`PB6` — write-only 3-wire, like the tuner. Audio
interface is I²S to SAI1.

Serves two jobs at once: it is the **I/Q ADC** for the receiver (stereo line
in from MSi001) and the **audio DAC** plus speaker amplifier for the output.
That means ADC and DAC share one sample rate.

Datasheet maximum is 48 kHz; the stock firmware runs it at 160 kHz, well out
of spec, with MCLK = 256 × Fs supplied externally by the MCU.

### ILI9488 — display

3.5", 480 × 320, 50-pin FPC. The connector exposes a full DB0–DB17 parallel
bus, but on this board **only the 4-wire SPI pins are used**: SDI, SDO,
WRX/SCL, D/CX, CSX, RESET, with IM0/IM1/IM2 strapped by R23/R24/R25 (10 kΩ).
Series resistors RN1/RN2 (33 Ω) on the signal lines.

Open question to settle early: the ILI9488 is widely documented as accepting
only **18-bit (3 bytes per pixel)** pixel data in SPI mode, not RGB565. If
that holds here, a full frame is 480 × 320 × 3 = 460 800 bytes, which sets
the ceiling on redraw rate. Measure it before designing the UI.

Note that 480 × 320 × 2 = 307 200 bytes, so unlike the DEEPSDR 101 a **full
RGB565 framebuffer fits in AXI SRAM** with room to spare. The band-compositor
approach in `gfx2.c` exists because the GD32 had 192 kB; it is not needed
here.

### Touch

Capacitive, on **I2C3** (`PA8` SCL / `PC9` SDA) with IRQ on `PC8` and reset
on `PC7`. The controller part number is not given on the schematic — read its
I²C address off the bus, or open the unit. Likely an FT6x36 or GT911.

The DEEPSDR 101 uses a resistive XPT2046 over bit-banged SPI, so
`User/touch.c` and `User/touch_calib.c` are replaced wholesale; capacitive
panels need no calibration.

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
