/*
 * Link the bring-up image into AXI SRAM, so it can be loaded and run over
 * SWD WITHOUT ERASING ANYTHING. The factory firmware stays in flash,
 * untouched, and a power cycle brings the radio back exactly as it was.
 *
 * That is the whole point of this script: the first time new clock and
 * GPIO code runs on a radio, it should not also be the first time the
 * factory image is destroyed. Flash comes later, from
 * stm32h743_flash.ld, once there is something worth keeping.
 *
 * Memory on the STM32H743VIT6, for reference:
 *
 *   ITCM        0x00000000   64 kB   CPU-local, zero wait state
 *   DTCM        0x20000000  128 kB   CPU-local, zero wait state, NO DMA
 *   AXI SRAM    0x24000000  512 kB   D1 domain, reachable by DMA2D/MDMA
 *   SRAM1/2/3   0x30000000  288 kB   D2 domain, where the DMA peripherals live
 *   SRAM4       0x38000000   64 kB   D3 domain
 *   Backup SRAM 0x38800000    4 kB
 *
 * Everything goes in AXI SRAM here. 512 kB is far more than a bring-up
 * image needs, and keeping one contiguous region means the loader has a
 * single range to write.
 *
 * NOTE for later: DTCM is the fastest RAM but no DMA controller can reach
 * it. Audio and panel buffers must NOT end up there. That trap is worth
 * remembering now rather than discovering it at the SAI stage.
 */

ENTRY(Reset_Handler)

MEMORY
{
    AXISRAM (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
}

/*
 * The top 256 bytes of AXI SRAM are reserved for the crash record that
 * src/fault.h pins at 0x2407FF00. The stack starts BELOW it and grows
 * down, so a crash note can never be overwritten by the stack that was
 * running when the crash happened - which is precisely when it matters.
 *
 * The address is fixed rather than exported as a symbol on purpose: a
 * record whose address moves with every rebuild is a record the debugger
 * reads at the wrong place, and this project has already been bitten by
 * exactly that.
 */
_estack = ORIGIN(AXISRAM) + LENGTH(AXISRAM) - 256;

_Min_Heap_Size  = 0;
_Min_Stack_Size = 0x2000;   /* 8 kB */

SECTIONS
{
    /*
     * The vector table has to be first: Reset_Handler points VTOR at the
     * start of this region, and the Cortex-M7 requires the table to be
     * aligned to the next power of two above its size. 512 bytes covers
     * the H743's 150 interrupts with room to spare.
     */
    .isr_vector :
    {
        . = ALIGN(512);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >AXISRAM

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
    } >AXISRAM

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >AXISRAM

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >AXISRAM
    .ARM :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } >AXISRAM

    /*
     * In a RAM image the load address and the run address are the same, so
     * the startup code's copy loop has nothing to do. _sidata still has to
     * exist and still has to equal _sdata, so that the same startup file
     * works for both this script and the flash one.
     */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >AXISRAM AT> AXISRAM

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
    } >AXISRAM

    ._user_heap_stack :
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
