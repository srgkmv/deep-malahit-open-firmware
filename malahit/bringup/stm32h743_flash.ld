/*
 * The same image, linked to run from flash at 0x08000000.
 *
 * REWRITTEN 20/11/2026, before the first flash write, from
 * stm32h743_ram.ld - the script every test on the radio has actually
 * run - and on purpose it differs from it in ONE way: code, constants
 * and the vector table live in flash, and .data's initial values are
 * copied out of flash by the startup code. Everything that runs in RAM
 * is in the same REGION as in the RAM image - AXI SRAM, never DTCM - so
 * the image that was tested and the image that is flashed differ as
 * little as a flash image can. Not at the same addresses: with the code
 * gone to flash, .data and .bss start at 0x24000000. The crash record
 * at 0x2407FF00 and the stack below it do not move.
 *
 * The previous version of this file dated from the first blink and put
 * .data, .bss and the stack in DTCM: 128 kB, against a .bss of 315 kB
 * today, and a region NO DMA controller can reach - while the SAI's DMA
 * rings are in .bss. It would have failed to link, or, worse, linked
 * with buffers the DMA cannot see. It also said the way back was jumper
 * JP1, which is not fitted on this unit (BOARD.md section 4).
 *
 * THE WAY BACK, which this file cannot provide and must not be written
 * without: src/boot.c, the encoder buttons at power-on into ROM DFU, the
 * very first code of every reset. And the factory image, backed up
 * outside the repository (WORKING.md, "No way back").
 *
 * WHERE IT GOES. Bank 1 from sector 0; the image is ~150 kB, two of
 * bank 1's 128 kB sectors. BANK 2 HOLDS THIS UNIT'S OWN DATA - about
 * 790 kB the factory firmware keeps there, not in its code image - and
 * nothing here may ever reach it. The FLASH region is therefore declared
 * as bank 1 only: an image that grew past 1 MB fails to link instead of
 * landing on top of it.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K     /* bank 1 only */
    AXISRAM (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
}

/*
 * Exactly as in the RAM image: the top 256 bytes of AXI SRAM are the
 * crash record src/fault.h pins at 0x2407FF00, and the stack starts
 * below it.
 */
_estack = ORIGIN(AXISRAM) + LENGTH(AXISRAM) - 256;

_Min_Heap_Size  = 0;
_Min_Stack_Size = 0x2000;   /* 8 kB */

SECTIONS
{
    /* First in flash, where the boot address points; 512-aligned for
     * VTOR, which Reset_Handler sets to this table. */
    .isr_vector :
    {
        . = ALIGN(512);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } >FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
    .ARM :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } >FLASH

    /* Initial values in flash, copied to AXI SRAM by Reset_Handler. */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >AXISRAM AT> FLASH

    /*
     * .tcmram GOES IN .bss HERE, and that is the one place this script
     * has to think rather than copy. Upstream's fft.c puts its work
     * buffers in a section of that name. In the RAM link it was an orphan
     * of LOADED zeros - 20 712 bytes, all zero, checked - which the
     * debugger wrote along with everything else. A flash image has no
     * debugger to write them and the startup only copies .data, so an
     * orphan here would boot full of whatever the RAM held. Inside .bss it
     * is zeroed by the startup, which is what the RAM image had.
     */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.tcmram)
        *(.tcmram*)
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } >AXISRAM

    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } >AXISRAM

    /DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
