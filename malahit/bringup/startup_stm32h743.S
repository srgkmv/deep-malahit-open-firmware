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

    /*
     * 1a. Interrupts allowed again.
     *
     *     A reset clears PRIMASK; an image started by a debugger
     *     inherits it from whatever ran before, and fault_record() parks
     *     a crashed run with interrupts disabled on purpose. Inheriting
     *     that means SysTick never ticks, so board_delay_ms() inside
     *     board_power_on() waits forever on a counter nothing
     *     increments - a hang BEFORE console_init(), with no console yet
     *     to report it. Silent, and it cost two evenings.
     *
     *     CONTROL is deliberately NOT touched here. Clearing it looks
     *     like the same kind of tidying and is not: it selects which
     *     stack pointer is live, so getting it wrong relative to the SP
     *     load above crashes before the first C function, and it crashes
     *     while VTOR still points at someone else's vector table, which
     *     turns an ordinary fault into a lockup that only a power cycle
     *     clears. Nothing in this firmware uses the process stack, and
     *     an exception return to Thread mode already leaves SPSEL at 0.
     *     The loader clears CONTROL before it sets the stack pointer,
     *     which is the right place for it - see ram_run in
     *     ../openocd.cfg.
     */
    movs  r0, #0
    msr   primask, r0
    msr   faultmask, r0
    msr   basepri, r0
    isb

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

    /* main() is not supposed to return. If it does, say so properly. */
    movs  r0, #0
    bl    fault_unexpected
    b     .

    .size Reset_Handler, .-Reset_Handler

/*
 * NO bkpt IN ANY OF THIS. 31/10/2026, after it cost most of an evening.
 *
 * The obvious way to write these handlers is `bkpt #0`, so that a halt
 * shows up in the debugger at a known address. It works beautifully while
 * the debugger is attached - and when it is NOT, a bkpt raises a fault of
 * its own. Inside a fault handler that is a double fault, and a double
 * fault LOCKS THE CORE UP.
 *
 * A locked-up core keeps IPSR at HardFault and ignores further debug
 * requests, which means the next attempt to load an image attaches to a
 * machine that is not in a state any image can start from. The symptom is
 * a firmware that appears to crash instantly and for no reason, on every
 * subsequent load, with the evidence from the FIRST crash long gone. We
 * chased exactly that, and the original fault turned out not to exist:
 * the whole cascade came from this instruction.
 *
 * So: handlers record what happened to a fixed address and spin. Spinning
 * is debuggable - attach whenever, read the record - and it cannot make
 * anything worse.
 */
/*
 * .thumb_func BEFORE EVERY ONE OF THESE LABELS, AND IT IS NOT OPTIONAL.
 *
 * A label in a .thumb section is still only a DATA label as far as the
 * assembler is concerned. `.word SomeLabel` then stores an even address,
 * and an even address in a vector table means the core enters the handler
 * with the Thumb bit clear: INVSTATE, escalation to HardFault, and - since
 * the HardFault vector is one of the broken ones - lockup.
 *
 * That is not hypothetical. These five handlers were written without it,
 * and the vector table came out with 0x24000da4 where it needed
 * 0x24000da5. The fault reporter below never recorded anything, because
 * the very act of taking a fault locked the part up. The Reset_Handler
 * above was fine all along - it carries .type %function - and the weak
 * aliases were fine too, because .thumb_set marks its target. Only the
 * handlers written out by hand were wrong, including Default_Handler,
 * which every one of the 150 interrupt vectors points at.
 *
 * Check it, do not trust it:
 *     arm-none-eabi-objdump -s -j .isr_vector build/bringup.elf
 * every word after the first must be ODD.
 */
    .section .text.Default_Handler,"ax",%progbits
    .type    Default_Handler, %function
    .thumb_func
Default_Handler:
    /*
     * Same treatment as a real fault: hand the exception frame to C so it
     * can be recorded AND so the handler can be left by a normal
     * exception return. fault_record() reads which exception this was out
     * of IPSR; it does not need to be told.
     */
    tst   lr, #4
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    mov   r1, lr
    b     fault_record
    .size Default_Handler, .-Default_Handler

/*
 * The real fault handlers. Work out which stack was in use - bit 2 of the
 * EXC_RETURN value in LR says PSP or MSP - and hand that frame to C, which
 * can then read the caller's PC, LR and registers out of it.
 */
    .section .text.HardFault_Handler,"ax",%progbits
    .global HardFault_Handler
    .type    HardFault_Handler, %function
    .thumb_func
HardFault_Handler:
    tst   lr, #4
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    mov   r1, lr
    b     fault_record
    .size HardFault_Handler, .-HardFault_Handler

    .section .text.MemManage_Handler,"ax",%progbits
    .global MemManage_Handler
    .type    MemManage_Handler, %function
    .thumb_func
MemManage_Handler:
    tst   lr, #4
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    mov   r1, lr
    b     fault_record
    .size MemManage_Handler, .-MemManage_Handler

    .section .text.BusFault_Handler,"ax",%progbits
    .global BusFault_Handler
    .type    BusFault_Handler, %function
    .thumb_func
BusFault_Handler:
    tst   lr, #4
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    mov   r1, lr
    b     fault_record
    .size BusFault_Handler, .-BusFault_Handler

    .section .text.UsageFault_Handler,"ax",%progbits
    .global UsageFault_Handler
    .type    UsageFault_Handler, %function
    .thumb_func
UsageFault_Handler:
    tst   lr, #4
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    mov   r1, lr
    b     fault_record
    .size UsageFault_Handler, .-UsageFault_Handler

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
 * Weak aliases for the handlers that do NOT have a real definition above:
 * each points at Default_Handler until a C file defines its own. The four
 * fault handlers are deliberately absent from this list - they are real
 * code in this file and must not be overridable by accident.
 */
    .macro  weak_handler name
    .weak   \name
    .thumb_set \name, Default_Handler
    .endm

    /* NMI is NOT weak-aliased to Default_Handler: on this board it is
     * how the clock security system reports that HSE has stopped, which
     * is a thing that can actually happen here - see clock.c. nmi_css()
     * in clock.c handles it. */
    weak_handler SVC_Handler
    weak_handler DebugMon_Handler
    weak_handler PendSV_Handler
    weak_handler SysTick_Handler
