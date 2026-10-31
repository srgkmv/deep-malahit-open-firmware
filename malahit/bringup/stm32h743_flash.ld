/*
 * The same image, linked to run from flash at 0x08000000.
 *
 * Use this only once a build has already proved itself from RAM. Writing
 * it replaces the factory firmware; getting back needs the backup and
 * jumper JP1. Nothing is lost permanently - JP1 reaches the ROM bootloader
 * no matter what is in flash - but there is no reason to spend the factory
 * image on a test that stm32h743_ram.ld can run for free.
 *
 * Flash geometry on the STM32H743VIT6: 2 MB, two banks of 1 MB, eight
 * 128 kB sectors each. 128 kB is also the smallest thing that can be
 * erased, which is worth remembering before planning to keep settings in
 * flash - this board has 32 kB of FRAM precisely so that it does not have
 * to.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
    DTCM    (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
    AXISRAM (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
}

/*
 * Stack in DTCM. It is the fastest RAM on the part and the CPU is the only
 * thing that ever touches a stack, so DTCM's one limitation - no DMA
 * controller can reach it - costs nothing here. Buffers are a different
 * story: anything a peripheral reads or writes must live in AXISRAM or the
 * D2 SRAMs instead.
 */
_estack = ORIGIN(DTCM) + LENGTH(DTCM);

/*
 * The crash record at 0x2407FF00 (src/fault.h) is in AXI SRAM, not DTCM,
 * so the stack above does not collide with it. Nothing is allocated into
 * AXISRAM by this script; the region is declared so that the address is
 * documented rather than merely assumed.
 */

_Min_Heap_Size  = 0;
_Min_Stack_Size = 0x2000;

SECTIONS
{
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

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >DTCM AT> FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } >DTCM

    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } >DTCM

    /DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
