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

# -nostdlib: nothing here calls into libc, and console.c exists precisely so
# that nothing has to. Leaving the library out means a stray printf() or
# malloc() fails at link time instead of quietly adding 40 kB and a heap.
LDFLAGS  = $(MCUFLAGS) -T$(LDSCRIPT)
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -nostdlib -nostartfiles
LDFLAGS += -Wl,--print-memory-usage
# A RAM image is by definition both writable and executable, so the linker's
# RWX-segment warning is describing the point of the exercise, not a problem.
LDFLAGS += -Wl,--no-warn-rwx-segments

LDSCRIPT ?= stm32h743_ram.ld

OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.S=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))

.PHONY: all ram flash size clean

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) $(LDSCRIPT)
	$(LD) $(OBJECTS) $(LDFLAGS) -o $@
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
