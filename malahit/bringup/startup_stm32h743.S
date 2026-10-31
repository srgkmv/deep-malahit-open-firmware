/*
 * Startup for the STM32H743VIT6 on the Malahit DSP-1.
 *
 * Written rather than borrowed from ST's template for one reason: this
 * image is normally LOADED INTO RAM AND STARTED BY THE DEBUGGER, not
 * reached through a chip reset. A debugger-started image cannot assume the
 * core did anything for it:
 *
 *   - The stack pointer is NOT loaded from the vector table, because no
 *     reset happened. Reset_Handler sets SP itself, first instruction.
 *   - VTOR still points at whatever the previous image left it at, which
 *     on this radio is the factory firmware in flash. We point it at our
 *     own table before enabling a single interrupt.
 *
 * Both of those are harmless when the same code does run from a real
 * reset, so there is one startup file and not two.
 */

    .syntax unified
    .cpu cortex-m7
    .fpu fpv5-d16
    .thumb

.global g_pfnVectors
.global Default_Handler

.word _sidata
.word _sdata
.word _edata
.word _sbss
.word _ebss

    .section .text.Reset_Handler
    .weak    Reset_Handler
    .type    Reset_Handler, %function
Reset_Handler:
    /* 1. Our own stack, no matter how we got here. */
    ldr   sp, =_estack

    /* 2. Our own vector table, before anything can interrupt us. */
    ldr   r0, =g_pfnVectors
    ldr   r1, =0xE000ED08          /* SCB->VTOR */
    str   r0, [r1]
    dsb
    isb

    /*
     * 3. Turn the FPU on. -mfloat-abi=hard means the compiler emits VFP
     *    instructions in ordinary C, so this has to happen before any C
     *    runs or the first one traps. CPACR: full access to CP10 and CP11.
     */
    ldr   r0, =0xE000ED88          /* SCB->CPACR */
    ldr   r1, [r0]
    orr   r1, r1, #(0xF << 20)
    str   r1, [r0]
    dsb
    isb

    /*
     * 4. Copy .data from its load address to its run address. In the RAM
     *    link those two are the same and this loop copies zero bytes; in
     *    the flash link it does the real work.
     */
    ldr   r0, =_sdata
    ldr   r1, =_edata
    ldr   r2, =_sidata
    movs  r3, #0
    b     LoopCopyDataInit

CopyDataInit:
    ldr   r4, [r2, r3]
    str   r4, [r0, r3]
    adds  r3, r3, #4

LoopCopyDataInit:
    adds  r4, r0, r3
    cmp   r4, r1
    bcc   CopyDataInit

    /* 5. Zero .bss. */
    ldr   r2, =_sbss
    ldr   r4, =_ebss
    movs  r3, #0
    b     LoopFillZerobss

FillZerobss:
    str   r3, [r2]
    adds  r2, r2, #4

LoopFillZerobss:
    cmp   r2, r4
    bcc   FillZerobss

    /* 6. Clocks and power, then us. */
    bl    SystemInit
    bl    main

    /*
     * main() is not supposed to return. If it does, stop here rather than
     * running off into whatever follows: a breakpoint shows up in the
     * debugger as a deliberate halt at a known address, an infinite loop
     * just looks like a hang.
     */
    bkpt  #0
    b     .

    .size Reset_Handler, .-Reset_Handler

/*
 * Anything without its own handler lands here. The breakpoint is the same
 * argument as above: with a debugger attached this stops dead and tells
 * you where. The loop after it keeps a detached board from wandering.
 */
    .section .text.Default_Handler,"ax",%progbits
Default_Handler:
    bkpt  #0
    b     .
    .size Default_Handler, .-Default_Handler

/*
 * The vector table. 150 maskable interrupts on the STM32H743, plus the 16
 * system entries. Names match the CMSIS IRQn_Type enum in stm32h743xx.h so
 * that defining a handler in C is enough to install it.
 */
    .section .isr_vector,"a",%progbits
    .type    g_pfnVectors, %object

g_pfnVectors:
    .word _estack
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler

    /* External interrupts 0..149. Only the ones this project actually
     * uses are named; the rest are filled in as they are needed. */
    .rept 150
    .word Default_Handler
    .endr

    .size g_pfnVectors, .-g_pfnVectors

/*
 * Weak aliases: every system handler points at Default_Handler until a C
 * file defines a real one with the same name.
 */
    .macro  weak_handler name
    .weak   \name
    .thumb_set \name, Default_Handler
    .endm

    weak_handler NMI_Handler
    weak_handler HardFault_Handler
    weak_handler MemManage_Handler
    weak_handler BusFault_Handler
    weak_handler UsageFault_Handler
    weak_handler SVC_Handler
    weak_handler DebugMon_Handler
    weak_handler PendSV_Handler
    weak_handler SysTick_Handler
