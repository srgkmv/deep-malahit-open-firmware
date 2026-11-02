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
#   make flash        build the flash-linked variant and program it
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
DEFS += -DGFX2_W=480 -DGFX2_H=320
DEFS += -DRM68120_EXMC_H
CFLAGS_EXTRA_UPSTREAM = -include rm68120_ili9488.h

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

OPT = -O2 -g3

CFLAGS  = $(MCUFLAGS) $(DEFS) $(INCLUDES) $(OPT)
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
LDFLAGS += -nostdlib -nostartfiles
LDFLAGS += -Wl,--print-memory-usage
# A RAM image is by definition both writable and executable, so the linker's
# RWX-segment warning is describing the point of the exercise, not a problem.
LDFLAGS += -Wl,--no-warn-rwx-segments

# Maths only, and last. See the block above.
LDLIBS   = -lm

LDSCRIPT ?= stm32h743_ram.ld

OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.S=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/up_,$(notdir $(UPSTREAM_SOURCES:.c=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/dsp_,$(notdir $(DSP_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))

.PHONY: all ram flash size clean

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/up_%.o: $(UPSTREAM_DIR)/%.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $(CFLAGS_EXTRA_UPSTREAM) $< -o $@

vpath %.c $(sort $(dir $(DSP_SOURCES)))
$(BUILD_DIR)/dsp_%.o: %.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)
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

flash:
	$(MAKE) LDSCRIPT=stm32h743_flash.ld all
	../tools/swd.sh flash $(BUILD_DIR)/$(TARGET).elf

size: $(BUILD_DIR)/$(TARGET).elf
	$(SIZE) -A $<

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d)
