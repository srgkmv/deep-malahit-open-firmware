#
# Build rules for the Malahit DSP-1 bring-up image.
#
# Named reglas.mk, with GNUmakefile a single include line, to match the
# convention the rest of this repository already uses - see the comment at
# the top of ../../reglas.mk for why that split exists upstream.
#
#   make              build build/bringup.elf, .bin and .hex
#   make ram          build, then load into AXI SRAM and run it over SWD
#                     (does NOT touch flash - the factory firmware survives)
#   make flash        build the flash-linked variant into build_flash/
#                     (builds only - the write is ROM DFU, see the target)
#   make size         section sizes
#   make clean
#
# "make ram" is the one to use while bringing things up. Linking into RAM
# means a wrong clock configuration costs a power cycle, not the only copy
# of the factory image.
#

TARGET    = bringup
BUILD_DIR = build

# --- Toolchain ---
PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
AS      = $(PREFIX)gcc -x assembler-with-cpp
LD      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE    = $(PREFIX)size

# --- MCU ---
#
# -mcpu=cortex-m7 with fpv5-d16, not the fpv4-sp-d16 the upstream GD32
# build uses: this core has a DOUBLE precision FPU. That matters more than
# it looks. Plenty of DSP code says 1.0 or M_PI without an f suffix, and on
# a single precision part every one of those promotes the whole expression
# to software double. Here they stay in hardware.
CPU      = -mcpu=cortex-m7
FPU      = -mfpu=fpv5-d16
FLOAT    = -mfloat-abi=hard
MCUFLAGS = $(CPU) -mthumb $(FPU) $(FLOAT)

DEFS = -DSTM32H743xx

INCLUDES  = -Isrc
INCLUDES += -I../CMSIS/Include
INCLUDES += -I../CMSIS/Device/ST/STM32H7xx/Include

#
# UPSTREAM, COMPILED IN PLACE - NOT COPIED.
#
# This is where the port stops being a parallel firmware and starts
# being a port. The files below are built straight out of ../../User,
# the DEEPSDR 101 tree, with no copy and no fork: a fix upstream is a
# fix here, and a divergence is visible as a diff instead of hiding in
# a duplicate.
#
# Only files that touch no register can be used this way. Of the 163 .c
# files in User/, 31 include gd32f4xx.h; the rest are portable C, which
# is the whole premise of PORTING.md section 1.
#
# fft.c - 460 lines, includes stdint.h and nothing else. It has a
#         complex I/Q path built for exactly this - fft_compute_db_iq(),
#         output in fftshift order with the VFO in the middle - and a
#         hook that iqbal's calibration attaches to. Step 6 of this port
#         wasted effort on a hand-written FFT before anyone grepped
#         User/ for one.
#
UPSTREAM_DIR = ../../User
INCLUDES += -I$(UPSTREAM_DIR)

#
# CMSIS-DSP, out of the upstream tree as well.
#
# Upstream ships a TRIMMED copy - nine source files, exactly the ones
# its DSP chain calls - so this costs almost nothing and unlocks
# demod_am.c, which uses arm_biquad_cascade_df1_f32,
# arm_fir_decimate_f32, arm_fir_interpolate_f32 and arm_cmplx_mag_f32
# heavily.
#
# ../compat is FIRST on the path and holds two forwarding headers that
# let those sources build against a modern CMSIS core; the reasoning is
# written out in compat/core_cmInstr.h.
#
# ARM_MATH_CM7, not CM4: same DSP extension, different core. The M7's
# is what this part has, and claiming the wrong one is the sort of thing
# that works until it picks a different code path.
#
DSP_DIR  = ../../CMSIS/DSP
INCLUDES := -I../compat $(INCLUDES) -I$(DSP_DIR)/Include -I$(DSP_DIR)/PrivateInclude
DEFS += -DARM_MATH_DSP=1 -DARM_MATH_CM7 -DARM_MATH_LOOPUNROLL

#
# THE DISPLAY, AND HOW UPSTREAM'S gfx2 IS POINTED AT THIS PANEL.
#
# GFX2_W/GFX2_H: 480 x 320 instead of the DEEPSDR's 800 x 480. Those two
# were the only thing in gfx2.h that stopped the file compiling for
# another board, and they are now #ifndef-guarded upstream - a generic
# change, the same pattern GFX2_BAND_H already used, and it leaves the
# DEEPSDR build and the deepsdr-ui host build untouched.
#
# RM68120_EXMC_H + -include: substitute the panel header WITHOUT editing
# or copying upstream. Defining the guard makes User/rm68120_exmc.h
# expand to nothing; the -include puts ours in its place. An -I cannot
# do this - for a quoted include the compiler searches the directory of
# the file holding the directive first, and that is User/. The full
# reasoning is in compat/rm68120_ili9488.h.
#
DEFS += -DRM68120_EXMC_H -DLCD_DMA_H
CFLAGS_EXTRA_UPSTREAM  = -include rm68120_ili9488.h
CFLAGS_EXTRA_UPSTREAM += -include lcd_dma_ili9488.h

#
# THE LAYOUT, AND WHY IT IS A HEADER AND NOT FOUR -D FLAGS.
#
# GFX2_W, GFX2_H and GFX_SCREEN_* used to be -D here. They are now in
# compat/malahit_layout.h, force-included ahead of everything, along
# with the other sixty-odd constants that place the header, the status
# band, the spectrum, the ruler, the waterfall and the action bar on a
# 480 x 320 panel. Upstream's UI headers guard every one of them with
# #ifndef for exactly this.
#
# THE POINT IS THAT IT IS THE SAME FILE the host render uses -
# deepsdr-ui's `make malahit` target force-includes this very header -
# so the PNG on the PC and the pixels on the panel cannot drift apart.
# A layout constant that exists twice is a layout constant that will
# eventually disagree with itself, and the disagreement shows up as a
# clipped chip on the radio and nowhere else.
#
# It goes in CFLAGS rather than CFLAGS_EXTRA_UPSTREAM because main.c
# needs the same geometry to place what it draws: it is the caller of
# ui_top_draw(), spec_chrome_draw() and spectrum_draw(), and the
# coordinates it passes have to be the ones the UI modules were
# compiled with.
#
CFLAGS_LAYOUT = -include malahit_layout.h

#
# THE TYPE, HALVED, WITHOUT TOUCHING A SINGLE UPSTREAM FILE.
#
# ui_top.c and friends name their faces as bare identifiers -
# &font_ui_14b - so a -D redirects all of them at once. No edit, no
# copy, no indirection layer. These are the same two sets deepsdr-ui's
# Makefile uses, and they have to stay the same two sets or the host
# render stops predicting the panel.
#
# WHY IT WORKS AT ALL, which is not obvious: font_ui_14.h only
# DECLARES `extern const gfx2_font_t font_ui_14`, the data being in
# fonts_data.c. Rewritten to font_ui_8, that declaration lands after
# compat/font_ui_8.h has already defined a static one of that name, and
# C says an extern declaration following a visible internal-linkage
# declaration keeps the internal linkage. So the header resolves to the
# small face and nothing is defined twice.
#
# TWO SETS, AND THEREFORE A RULE OF ITS OWN FOR ui_top.c. The remap is
# per translation unit. ui_top.c is the only file that needs the 8 px
# face - six status chips in a 480 px row - while the ruler and the
# action bar read fine at 11 and shrinking them would only make them
# worse. ui_top.c also carries the frequency readout, which is the one
# thing on this screen read from across a room: font_num_44 at 46 px
# does not fit a 320-row panel, so it goes to font_num_31.
#
# AND fonts_data.c MUST NOT GET EITHER SET. It is what DEFINES these
# symbols; remapped, the definition of font_ui_14b would be renamed to
# font_ui_11 and the link would fail on a duplicate.
#
MALAHIT_FONTS = -Dfont_ui_18b=font_ui_11 \
                -Dfont_ui_14b=font_ui_11 \
                -Dfont_ui_14=font_ui_11 \
                -Dfont_num_20=font_ui_11

# 10 px and not 8 since 03/11/2026: the panel decided it, not the
# render. See the same block in deepsdr-ui/Makefile - these two have to
# say the same thing or the PNG stops predicting the glass.
MALAHIT_FONTS_TOP = -include font_num_31.h -include font_ui_10.h \
                    -include font_ui_10b.h \
                    -Dfont_num_44=font_num_31 \
                    -Dfont_ui_18b=font_ui_11 \
                    -Dfont_ui_14b=font_ui_10b \
                    -Dfont_ui_14=font_ui_10 \
                    -Dfont_num_20=font_ui_11

# THE MENUS' SET: ui_top's faces, but referred to rather than included.
# The same renames as MALAHIT_FONTS_TOP minus the -include lines, so
# each upstream `extern` of font_ui_14 and friends becomes an extern of
# font_ui_10 and friends, defined once in src/fonts_menu.c. With the
# -includes, every menu object carried its own static copy of the
# glyphs - 8 kB each.
MALAHIT_FONTS_MENU = -Dfont_ui_18b=font_ui_11 \
                     -Dfont_ui_14b=font_ui_10b \
                     -Dfont_ui_14=font_ui_10 \
                     -Dfont_num_20=font_ui_11 \
                     -Dfont_num_44=font_num_31

DSP_SOURCES  = $(wildcard $(DSP_DIR)/Source/FilteringFunctions/*.c)
DSP_SOURCES += $(wildcard $(DSP_DIR)/Source/ComplexMathFunctions/*.c)

# iqbal.c - 1694 lines, no register anywhere. The image-rejection and
#           gain/phase corrector, which is the right tool for what step 5
#           measured on this board: 23-32 dB of raw image rejection and
#           an LO leakage 15 dB above the wanted carrier. It carries its
#           own A/B harness, so it can report the rejection with itself
#           off and then on.
UPSTREAM_SOURCES = $(UPSTREAM_DIR)/fft.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/iqbal.c
# iqancho.c - 232 lines, math.h and nothing else. iqbal.c calls into it,
#           so the graft is not optional - but it is also the piece that
#           matters most here. iqbal corrects with TWO numbers for the
#           whole band, a gain and a phase, which is exact only if the
#           sole difference between I and Q is an angle. A DELAY between
#           the branches, or antialias filters that are not identical,
#           makes the imbalance a function of frequency: half a sample
#           period is 0.9 degrees at 1 kHz and 43 degrees at 48. This
#           corrects it with a filter instead of a constant.
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/iqancho.c
# agc.c, audiofil.c - 72 and 151 lines, no register. The AGC and the
#           audio filter the AM path feeds.
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/agc.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/audiofil.c
# gfx2.c  - 969 lines, the band compositor and the whole drawing API
#           (fills, rounded rects, lines, text). It is already decoupled
#           from the panel - deepsdr-ui compiles THIS FILE, byte for
#           byte, against a simulated LCD to render the UI on a PC - and
#           it needs exactly three functions from a display driver.
#           src/rm68120_ili9488.c provides them over SPI.
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/gfx2.c
# fonts_data.c - upstream's glyph data, seven faces. Pure const tables.
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/fonts_data.c
# gfx.c, spectrum.c, idioma.c, spec_rotulos.c - the panadapter itself.
# spectrum.c is 2078 lines of heat-map palettes, partial redraw,
# per-column ceilings and band labels; gfx.c is the older drawing layer
# it still uses for blits. Both reach the panel through the two
# substituted headers above.
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/gfx.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/idioma.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/spec_rotulos.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/spectrum.c
#
# THE SCREEN AROUND THE SPECTRUM - step 7's last three files.
#
# spec_chrome.c  the dB gutter, the frequency ruler and the colour
#                legend: everything around the trace that changes a few
#                times a minute instead of thirty times a second, which
#                is the reason it is its own module and its own repaint.
# ui_top.c       the header - frequency, mode, band, clock, battery -
#                and the status band with the S-meter and the chips.
# ui_act.c       the six buttons along the bottom, and the hit test for
#                them, which comes out of the same geometry that drew
#                them rather than a second table.
#
# All three are pure gfx2 and were already parameterised on GFX2_W;
# what they were NOT parameterised on is the vertical budget, and that
# is what compat/malahit_layout.h supplies. The edits that made the
# remaining literals overridable are generic, are upstream's own
# #ifndef pattern, and are recorded in PROCEDENCIA.md.
#
#
# encoder.c - 241 lines of quadrature decoding, button debouncing and
#           short/long press classification, of which four lines touch a
#           register. It wants encoder_tick() at 1 kHz and this board
#           already has a 1 kHz SysTick with a handler, so the graft
#           needs no new interrupt and no new vector - which matters,
#           because startup_stm32h743.S has a name for none of the 150
#           external ones.
#
#           THE PINS COME FROM HERE. Upstream's six defines are #ifndef
#           now (PROCEDENCIA.md), so this is where this board's wiring
#           lives: BOARD.md section 5 has RENC1 on PC0/PC1 and the
#           encoder button on PA1.
#
#           ENC_BTN_PRESSED says which level is a press, and this is a
#           PREDICTION until test_encoder() has run with a finger on the
#           button. The DEEPSDR's switch goes to Vcc and wants 1; the
#           encoder's own A/B lines on this board are pulled UP by the
#           factory firmware, which means its contacts go to ground, so a
#           button wired the same way wants 0. The test probes the pin
#           with both pulls and says which it is; a wrong guess here
#           costs a button that never reports, not a confused one.
#
#           compat/gd32f4xx.h is what makes it compile: the four GPIO
#           calls it needs, onto this chip's registers, and nothing else.
#
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/encoder.c
#           RENC1 BY CHOICE: the user wants the BIG knob to tune, and
#           BOARD.md section 6 has the big one as RENC1. Which encoder
#           the driver gets is a UI decision, not a capability - both
#           work and both give a textbook Gray sequence, 00, 10, 11, 01.
#           See test_encoder_lines().
#
#           NEITHER IS DEAD, though the first version of this comment
#           said RENC1 was, as a measurement, and was wrong. It showed
#           no movement across three runs and sixty seconds of supposed
#           turning, which was read as a fault; the fourth run, with the
#           same configuration, recorded 151 changes on it. The absences
#           were windows nobody was standing in, the same mistake the
#           audio test had already cost twice.
#
#           AND THE KNOB-TO-PIN MAPPING WAS THE WEAK LINK, so moving
#           here was also the test of it. "Small on top = RENC2, big
#           below = RENC1" came from the ORDER the two knobs were turned
#           in against the order the pins moved - one report, with the
#           hand out of reach of the oscilloscope. Confirmed from the
#           other side on 05/11/2026: with these pins the BIG knob tunes
#           and the small one does nothing, peak +19875 -> -37125 Hz
#           monotonically and then still for 169 console lines while the
#           small one was turned. Mapping and request agree.
#
#           PC0/PC1 also need nothing from SYSCFG_PMCR. PC2_C/PC3_C are
#           dual-pad pins whose analog switches decide whether they
#           reach the digital port at all; these two are ordinary.
#
#           THE BUTTON IS NOT A GPIO ON THIS BOARD, so it does not
#           arrive as a port and a pin. ENC_BUT is a resistor ladder:
#           47k to 3.3 V, 68k to ground for ENC1 and 22k for ENC2, which
#           is 1.95 V and 1.05 V - one of them inside the input buffer's
#           undefined band, where no pull and no polarity can help. Two
#           buttons on one pin can only be told apart by measuring the
#           voltage, which is why the factory firmware keeps PA1 in
#           analog mode with its switch open.
#
#           ENC_BTN_READ names the function that answers instead, and
#           upstream keeps everything else: the debounce integrator, the
#           hold timer, the short/long classification on release and the
#           "pressed and turned" consumption are all written in terms of
#           "the debounced state is a press" and do not care where the
#           press came from. src/adc.c supplies the answer from ADC1,
#           sampled a millisecond at a time from SysTick.
#
#           The guard matters in encoder_init() too, and not for
#           tidiness: ENC_BTN_PORT and ENC_BTN_PIN keep their upstream
#           defaults, so an unguarded gpio_mode_set() would configure
#           PC9 - the touch controller's I2C data line. See
#           PROCEDENCIA.md for both edits.
#
#           The PC4 placeholder this replaced was the honest version of
#           not having an ADC: a pin BOARD.md lists as NOT CONNECTED,
#           with a press defined as the level its pull-up cannot produce,
#           so the driver reported an idle button for ever instead of
#           inventing presses. Not PC13, which was the first choice and a
#           bad one: that is the standby button, and a real button in
#           that slot would have reported real presses belonging to
#           something else.
DEFS += -DENC_A_PORT=GPIOC -DENC_A_PIN=GPIO_PIN_0
DEFS += -DENC_B_PORT=GPIOC -DENC_B_PIN=GPIO_PIN_1
DEFS += -DENC_BTN_READ=adc_enc1_down

#
# THE SECOND KNOB IS THE SAME FILE, COMPILED TWICE.
#
# This board has two encoders and upstream's encoder.c has file-scope
# state, so it is a one-knob module: a second instance needs a second
# copy of that state. The choices were to write a second decoder, to
# restructure upstream's into something instance-based, or this.
#
# This is the one that costs nothing and copies nothing. A second
# translation unit of the SAME source gets its own statics for free -
# that is what a static is - and the public names are renamed on the
# command line so the two do not collide. The second knob therefore
# gets the identical quadrature table, the identical debounce
# integrator and the identical short/long classification, already
# tested on the first knob, with no new decoding code to be wrong in a
# different way.
#
# What it costs is this comment: `encoder2_tick()` has no definition
# anywhere to grep for, which is a surprise worth warning about once.
# src/enc2.h declares the renamed names so the rest of the port can see
# them normally.
#
ENC2_RENAME  = -Dencoder_init=encoder2_init
ENC2_RENAME += -Dencoder_tick=encoder2_tick
ENC2_RENAME += -Dencoder_take_delta=encoder2_take_delta
ENC2_RENAME += -Dencoder_take_press=encoder2_take_press
ENC2_RENAME += -Dencoder_take_long_press=encoder2_take_long_press
ENC2_RENAME += -Dencoder_button_down=encoder2_button_down
ENC2_RENAME += -Dencoder_consume_press=encoder2_consume_press
ENC2_RENAME += -Dencoder_invertido_pon=encoder2_invertido_pon
ENC2_RENAME += -Dencoder_invertido=encoder2_invertido

UPSTREAM_SOURCES += $(UPSTREAM_DIR)/spec_chrome.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ui_top.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ui_act.c

#
# STEP 9: THE MENU SCREENS - 21/11/2026.
#
# ui_cfg.c   a column of categories and a grid of cells: upstream's mode
#            and band screens (and its settings, later)
# ui_grid.c  a paged grid with a title: the step screen
#
# Both laid out by compat/malahit_layout.h - their geometry and the
# offsets inside their cells are #ifndef upstream now (PROCEDENCIA.md) -
# and both drawn in ui_top's type, MALAHIT_FONTS_TOP: they sit between
# the header and the action bar and should read like them. deepsdr-ui
# `make malahit-menus` renders them on a PC with exactly these flags.
# src/menus.c is the controller, the part of upstream's main.c these
# three screens need.
#
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ui_cfg.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ui_grid.c
# ui_det.c  one continuous setting, "-" and "+" around its value: the
#           squelch, the NR strength (step 9, 21/11/2026)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ui_det.c
# ui_kbd.c  the frequency keypad, a tap on the frequency (step 9,
#           21/11/2026)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ui_kbd.c
# spec_snap.c  a tap on the spectrum lands on the signal near it (a pure
#              function, upstream's sim/snaptest.c tests it)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/spec_snap.c
# banco.c  the memory channels: four banks of fifteen, upstream's table
#          and its MEM.CSV, kept in the FRAM by src/memfram.c (22/11/2026)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/banco.c
# rds.c, rds_pi.c  RDS on WFM: the decoder upstream's WFM hook feeds, and
#                  its table of networks by PI code (22/11/2026)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/rds.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/rds_pi.c
# cw.c  the Morse decoder: a Goertzel on the USB audio demod_am.c already
#       hands it at 12 kHz, and a dot-length estimator (22/11/2026, the
#       first of the text decoders - PORTING.md step 42)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/cw.c
# rtty_scope.c  the tone scope's audio spectrum, fed from demod_am.c's USB
#               branch while a text decoder is on (step 42)
# ui_digi.c     upstream's text panel, its geometry in malahit_layout.h
#               (UDG_*), drawn in the menus' faces (step 42)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/rtty_scope.c
# rtty.c, rtty_auto.c  the RTTY decoder (Goertzel pair, bit sync, ITA2)
#                      and its automatic shift/baud/polarity (step 43)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/rtty.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/rtty_auto.c
# psk31.c  PSK31: a phasor at the tone, differential detection, Varicode
#          (step 44)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/psk31.c
# navtex.c  NAVTEX, SITOR-B: two tone filters a bit long, a Gardner bit
#           clock, CCIR 476's four-of-seven and the repeat 350 ms later
#           (step 45)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/navtex.c
# dsc.c  HF DSC, ITU-R M.493: the same tone filters and bit clock as NAVTEX,
#        ten-bit characters, DX/RX two pairs apart, the ECC; its state in
#        the borrowed RAM (src/prestamo.c, not upstream's hfdl_ram.c)
#        (step 46)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/dsc.c
# wefax.c    radiofax: an FM discriminator on the tone (disc_fm.c), the
#            300/450 Hz start and stop signals, the phasing pulse and the
#            line clock; its eight-line ring in the borrowed RAM (step 47)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/wefax.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/disc_fm.c
# sstv.c     slow-scan TV: the VIS header, the line sync, the mode table
#            computed from its timings; its output queue in the borrowed RAM
#            (step 48)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/sstv.c
# hell.c     Feld-Hell: the envelope cut into 245 points a second, columns
#            of 14; its state in the borrowed RAM (step 49)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/hell.c
# jtty_*.c   WSJT-X's JTTY: 4-GFSK frames of 34 bits, tail-biting
#            convolutional code, atoms joined into messages by frequency;
#            dxcc.c for the country of a callsign; the Viterbi from the
#            main loop (jtty_modo_poll()) (step 50)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/jtty_rx.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/jtty_fec.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/jtty_msg.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/jtty_modo.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/dxcc.c
# ale_*.c    ALE 2G (MIL-STD-188-141): 8-FSK at 125 Bd, eight Goertzel
#            on the 12 kHz sideband, the 2-of-3 vote, Golay (24,12),
#            words joined into addresses in a table of stations; the
#            table in the borrowed RAM (step 51)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ale_rx.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ale_golay.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ale_modo.c
# stanag_*.c STANAG: the detector that names a waveform by the period it
#            repeats at (stanag_det.c), the 4285/4481 demodulator that
#            reads text (stanag_rx.c), the 4415 at 75 bps (s4415.c), and
#            the mode behind one button (stanag_modo.c); 32-39 kB of the
#            borrowed RAM, never two at once (step 52)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/stanag_det.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/stanag_rx.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/s4415.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/stanag_modo.c
# hfdl_viterbi.c  HFDL's K=7 rate-1/2 Viterbi, the same code the 4285 and
#            the 4415 use; HFDL itself is not grafted yet (step 52)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/hfdl_viterbi.c
# ident.c   IDENT, "what signal is this?": STANAG's detector's five
#            measures (width, a lone carrier, two tones and their shift,
#            baud, the frame period) plus the on/off cadence, against a
#            table of signatures; the detector's 32 kB in the borrowed RAM
#            (step 53)
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ident.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/ui_digi.c
#
# THE BAND TABLE IS UPSTREAM'S, GENERATED, NOT COPIED: k_band_presets[]
# out of User/main.c by the script the host render uses for the same
# purpose (deepsdr-ui/tools/bandas_dump.py), into the build directory.
# And with it upstream's step list, TUNE_STEPS_LISTA, which main.c uses
# as its step table and menus.c for the step screen.
# A band added upstream reaches this radio at the next build, and the
# PNG and the panel list the same bands.
#
BANDAS_TABLA = $(BUILD_DIR)/bandas_tabla.h
BANDAS_DUMP  = ../../deepsdr-ui/tools/bandas_dump.py
#
# AND THE THEMES, the same way: k_temas[] - the UI palette and the
# waterfall palette each theme pairs - out of upstream's main.c by the
# script the host render uses (deepsdr-ui/tools/gen_temas.py), for the
# settings screen's Tema.
#
TEMAS_TABLA  = $(BUILD_DIR)/temas_tabla.h
TEMAS_GEN    = ../../deepsdr-ui/tools/gen_temas.py
# (The rule that makes it is further down, beside up_encoder2.o, and for
# the same reason: up here it was the first target, so `make` built the
# table and stopped.)

#
# STEP 8: UPSTREAM'S DEMODULATOR - 21/11/2026.
#
# demod_am.c's #include closure is 106 files; what it needs to LINK is
# these ten and the decoders switched off (src/decoders_off.c). Same
# set as upstream's host bench, deepsdr-ui `make demod` (DEMOD_REALES),
# and found again here by compiling it on this port's flags. agc.c and
# audiofil.c, the other two of the bench's set, are already above.
#
#   demod_am.c   AM, SAM, USB, LSB, NFM and WFM (its own hook, step 35)
#   semibanda.c  the half-band pair: 192 -> 96 kHz in, 96 -> 192 out
#   nco.c, nco_tabla.c   the digital mix, for any offset that is not Fs/4
#   nb.c         noise blanker        anotch.c  automatic notch
#   nr_ss.c      spectral-subtraction noise reduction
#   sam.c        synchronous AM's carrier loop
#   rapido.c     the fast atan2 of the FM discriminator
#   bip.c        the notification beep, mixed in at the end of the block
#
# src/demod.c is the plumbing, and the A/B switch against am.c.
#
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/demod_am.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/semibanda.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/nco.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/nco_tabla.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/nb.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/anotch.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/nr_ss.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/sam.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/rapido.c
UPSTREAM_SOURCES += $(UPSTREAM_DIR)/bip.c
#
# THE SPEAKER AMPLIFIER. demod_am_init() switches it on, and upstream's
# pin is PB7 - not connected on this board (BOARD.md, pin 93). Ours is
# PE1, Vspk_ON, which board.c already drives high; the #ifndef that
# lets a board say so is the upstream edit in PROCEDENCIA.md.
#
DEFS += -DSPK_EN_PORT=GPIOE -DSPK_EN_PIN=GPIO_PIN_1
#
# __CORTEX_M, SAID ONCE AND THE SAME WAY core_cm7.h SAYS IT. Upstream's
# trimmed CMSIS-DSP defines it as 0x04 when arm_math.h comes before the
# device header - demod_am.c does that, through sam.h - and core_cm7.h
# then redefines it to (7U): a warning on every build. Defined here with
# core_cm7.h's exact tokens, the DSP header's #ifndef skips and the
# redefinition is an identical one, which C allows silently. Nothing in
# the DSP sources selects code on it beyond ">= 3", true either way.
#
DEFS += '-D__CORTEX_M=(7U)'
#
# HFDL_DIAG: hfdl_modo.h tests it with #if and upstream never defines it,
# which is 0 by the rules of C and a warning by this build's -Wundef.
# Said explicitly, with the value upstream already gets.
#
DEFS += -DHFDL_DIAG=0

#
# NOT added to C_SOURCES, and that is deliberate. Objects here are named
# by $(notdir), and the upstream tree has its own main.c - 39 094 lines
# of it - so putting User/ on the vpath made "build/main.o" ambiguous
# and make chose theirs. It failed loudly on a missing gd32f4xx.h, which
# was lucky; a quieter collision would have linked the wrong file.
#
# So upstream objects get their own prefix and their own rule below.
# build/up_fft.o could not be mistaken for anything of ours.
#
C_SOURCES = $(wildcard src/*.c)
ASM_SOURCES = startup_stm32h743.S

#
# SELFTEST: whether the boot runs the bring-up tests - main.c, under
# "SELFTEST", has what they are and what had to move out of them first.
# 0 boots straight to the radio, 2.5 s; 1 runs every test, 37 s of them:
#
#   make flash SELFTEST=1
#
# Set from the command line, so it is the case the paragraph above
# BUILD_RULES below does NOT cover: no file changes. FLAGS_STAMP does.
#
SELFTEST ?= 0
DEFS += -DSELFTEST=$(SELFTEST)

OPT = -O2 -g3

CFLAGS  = $(MCUFLAGS) $(DEFS) $(INCLUDES) $(OPT)
CFLAGS += $(CFLAGS_LAYOUT)
CFLAGS += -Wall -Wextra -Wshadow -Wundef
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fno-common
CFLAGS += -std=c11
# Dependency tracking, so editing a header actually rebuilds what uses it.
CFLAGS += -MMD -MP

ASFLAGS = $(MCUFLAGS) $(OPT) -Wall

#
# -nostdlib USED TO SAY "nothing here calls into libc", AND THAT STOPPED
# BEING TRUE AT STEP 7.
#
# The upstream DSP tree is written against libm. iqbal.c alone calls
# sqrtf, atan2f, atanf, sinf, cosf, log10f and exp; demod_am.c adds more.
# There is no version of grafting that code which does not link a maths
# library, so the honest move is to link one and say which.
#
# Still -nostartfiles: the entry point, the vector table and the .data
# copy are ours, in startup_stm32h743.S, and newlib's crt0 would fight
# all three. -lm comes AFTER the objects so the linker resolves it last.
#
# What is deliberately NOT here is full libc. console.c exists precisely
# so that nothing needs printf, and leaving libc out means a stray
# printf() or malloc() still fails at link time instead of quietly
# adding 40 kB and a heap.
#
LDFLAGS  = $(MCUFLAGS) -T$(LDSCRIPT)
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref
LDFLAGS += -Wl,--gc-sections
# FM stereo (src/estereo.c, 22/11/2026): upstream hands the WFM multiplex
# to rds_feed() and to nothing else, so the call is wrapped - demod.c's
# __wrap_rds_feed() gives it to the stereo decoder too, then to rds.c.
# Not one line of demod_am.c changes for it.
LDFLAGS += -Wl,--wrap=rds_feed
# And psk31_process(), for demod.c's recorder of the decoder's audio (the
# bench's: demod_graba).
LDFLAGS += -Wl,--wrap=psk31_process
# and STANAG's, for the same recorder (step 52)
LDFLAGS += -Wl,--wrap=stanag_modo_mete
LDFLAGS += -nostdlib -nostartfiles
LDFLAGS += -Wl,--print-memory-usage
# A RAM image is by definition both writable and executable, so the linker's
# RWX-segment warning is describing the point of the exercise, not a problem.
LDFLAGS += -Wl,--no-warn-rwx-segments

# Maths only, and last. See the block above.
LDLIBS   = -lm

LDSCRIPT ?= stm32h743_ram.ld

OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
OBJECTS += $(BUILD_DIR)/up_encoder2.o   # the second knob; rule further down
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.S=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/up_,$(notdir $(UPSTREAM_SOURCES:.c=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/dsp_,$(notdir $(DSP_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))

.PHONY: all ram flash size clean FORCE

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex

#
# THIS FILE IS A PREREQUISITE OF EVERY OBJECT, and it has to be said
# explicitly because -MMD cannot say it: the compiler lists the headers
# it opened, and a -D that changes what a file COMPILES TO was never a
# header. So a wiring change here - the encoder's pins are the live
# example - left every .o untouched and `make` answered "nothing to be
# done", which is the worst possible answer: the next load puts the
# OLD image on the radio and the bench test then measures the previous
# decision. Caught once; it must not be available to catch twice.
#
# The cost is that editing this file rebuilds everything. That is the
# right trade for a file that holds the flags.
#
# AND A FLAG GIVEN ON THE COMMAND LINE - `make SELFTEST=1` - changes no
# file at all, so it is the same trap with nothing left to catch it.
# FLAGS_STAMP holds the values in a file of their own, rewritten only
# when they differ from the last build in this directory: changing one
# rebuilds everything, and repeating one rebuilds nothing.
#
FLAGS_STAMP = $(BUILD_DIR)/flags.txt
BUILD_RULES = reglas.mk GNUmakefile $(FLAGS_STAMP)

$(FLAGS_STAMP): FORCE | $(BUILD_DIR)
	@echo 'SELFTEST=$(SELFTEST)' | cmp -s - $@ \
	    || echo 'SELFTEST=$(SELFTEST)' > $@

$(BUILD_DIR)/%.o: %.c $(BUILD_RULES) | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

#
# THE FONT REMAP IS PER FILE, so it is a target-specific variable and
# not a flag on the rule. Two files are the exceptions, and both would
# fail loudly rather than quietly, which is the only reason this is
# comfortable: fonts_data.c would fail at link time on a duplicate
# symbol, and ui_top.c would simply be drawn at the wrong size, which
# the host render shows side by side.
#
CFLAGS_FONTS = $(MALAHIT_FONTS)
$(BUILD_DIR)/up_fonts_data.o: CFLAGS_FONTS =
$(BUILD_DIR)/up_ui_top.o:     CFLAGS_FONTS = $(MALAHIT_FONTS_TOP)
$(BUILD_DIR)/up_ui_cfg.o:     CFLAGS_FONTS = $(MALAHIT_FONTS_MENU)
$(BUILD_DIR)/up_ui_grid.o:    CFLAGS_FONTS = $(MALAHIT_FONTS_MENU)
$(BUILD_DIR)/up_ui_det.o:     CFLAGS_FONTS = $(MALAHIT_FONTS_MENU)
$(BUILD_DIR)/up_ui_kbd.o:     CFLAGS_FONTS = $(MALAHIT_FONTS_MENU)
$(BUILD_DIR)/up_ui_digi.o:    CFLAGS_FONTS = $(MALAHIT_FONTS_MENU)

# ale_modo.c asserts that its table fits ft8_shared_ram_t - the waterfall
# memory upstream lends it - so it includes ft8_shared_ram.h, which reaches
# Ft8Lib's headers. Only a sizeof: nothing of FT8 is linked (step 51).
$(BUILD_DIR)/up_ale_modo.o: CFLAGS_EXTRA_UPSTREAM += -I$(UPSTREAM_DIR)/../Ft8Lib
# ALE's AFC (User/ale_rx.c, 23/11/2026): off by default there, because the
# DEEPSDR's flash has no room for it; on here.
$(BUILD_DIR)/up_ale_rx.o: CFLAGS_EXTRA_UPSTREAM += -DALE_CAF=1

$(BUILD_DIR)/up_%.o: $(UPSTREAM_DIR)/%.c $(BUILD_RULES) | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $(CFLAGS_EXTRA_UPSTREAM) $(CFLAGS_FONTS) $< -o $@

vpath %.c $(sort $(dir $(DSP_SOURCES)))
$(BUILD_DIR)/dsp_%.o: %.c $(BUILD_RULES) | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

#
# THE SECOND KNOB'S OBJECT, and it lives DOWN HERE for a reason that
# cost a confused minute: the first target in a makefile is the default
# goal, and this rule sitting up with the ENC2_RENAME flags made
# `make` build one object file and stop, reporting it as up to date.
# Nothing failed and nothing was linked. Same family as the -D that
# did not trigger a rebuild (WORKING.md trap 20): the build system
# quietly did something other than what was asked.
#
$(BUILD_DIR)/up_encoder2.o: $(UPSTREAM_DIR)/encoder.c $(BUILD_RULES) | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $(CFLAGS_EXTRA_UPSTREAM) $(ENC2_RENAME) \
	      -UENC_A_PIN -UENC_B_PIN -UENC_BTN_READ \
	      -DENC_A_PORT=GPIOC -DENC_A_PIN=GPIO_PIN_2 \
	      -DENC_B_PORT=GPIOC -DENC_B_PIN=GPIO_PIN_3 \
	      -DENC_BTN_READ=adc_enc2_down \
	      $< -o $@

# The band table, generated - see BANDAS_TABLA above.
$(BANDAS_TABLA): $(UPSTREAM_DIR)/main.c $(BANDAS_DUMP) | $(BUILD_DIR)
	python3 $(BANDAS_DUMP) $(UPSTREAM_DIR)/main.c $@
$(BUILD_DIR)/menus.o: $(BANDAS_TABLA)
$(BUILD_DIR)/menus.o: CFLAGS += -I$(BUILD_DIR)
$(BUILD_DIR)/main.o: $(BANDAS_TABLA)
$(BUILD_DIR)/main.o: CFLAGS += -I$(BUILD_DIR)
$(TEMAS_TABLA): $(UPSTREAM_DIR)/main.c $(TEMAS_GEN) | $(BUILD_DIR)
	python3 $(TEMAS_GEN) ../.. $@
$(BUILD_DIR)/menus.o: $(TEMAS_TABLA)

$(BUILD_DIR)/%.o: %.S $(BUILD_RULES) | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) $(LDSCRIPT)
	$(LD) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@
	$(SIZE) $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(OBJCOPY) -O binary -S $< $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR):
	mkdir -p $@

ram: all
	../tools/swd.sh ram $(BUILD_DIR)/$(TARGET).elf

# THE FLASH IMAGE, BUILT ONLY - NOTHING IS WRITTEN BY MAKE.
#
# Into build_flash/, so the RAM image in build/ survives. It used to
# build into build/ and program over SWD with "reset halt", which on this
# board does not halt (WORKING.md, "No way back") - a flash write through
# it would have run on a core nobody had stopped.
#
# The write itself is ROM DFU, the path proven on this radio: switch on
# with both encoder buttons held, then the dfu-util line this prints. It
# erases only the sectors the image covers; bank 2, this unit's own data,
# is never touched.
flash:
	$(MAKE) LDSCRIPT=stm32h743_flash.ld BUILD_DIR=build_flash all
	@echo
	@echo "  flash image: build_flash/$(TARGET).bin - write it over ROM DFU with"
	@echo "    dfu-util -a 0 -s 0x08000000:leave -D build_flash/$(TARGET).bin"

size: $(BUILD_DIR)/$(TARGET).elf
	$(SIZE) -A $<

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d)
