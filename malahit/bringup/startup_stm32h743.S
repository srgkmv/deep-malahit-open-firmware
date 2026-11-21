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
     * 0. OUT OF HANDLER MODE, if a debugger started us inside an
     *    exception - found 20/11/2026, and it had silenced the radio.
     *
     *    The block interrupt carries the AM demodulator, ~8 ms of every
     *    21 ms, so a load over SWD halts the old image INSIDE exception
     *    27 (DMA1_Stream0) four times in ten. ram_run then writes PC,
     *    SP and xPSR - and the IPSR does not change, nor does the NVIC's
     *    active bit. The whole new image ran as exception 27: SysTick, at
     *    priority 0, still preempted it, so knobs, buttons and screen all
     *    worked; the block interrupt, at 1 and already "active", could
     *    never be taken again. irq 0/0, no audio, and the next load
     *    inherits it, so it lasts until a power cycle. Seen on the radio:
     *    xPSR 0x290f001b with the PC in gfx2_wf_blit(), IABR0 bit 11 set.
     *
     *    Writing the registers cannot clear an NVIC active bit; only an
     *    exception return can. So make one: a basic frame whose return
     *    address is this very handler, xPSR with only the Thumb bit, and
     *    EXC_RETURN for thread mode on MSP. We land at the top again, in
     *    thread mode, with the bit cleared.
     *
     *    NONBASETHRDENA first, so a return to thread mode with another
     *    exception still nested underneath is not an INVPC fault; that
     *    case (the halt caught SysTick preempting the block interrupt)
     *    is rare and leaves the outer one active - main() reads the
     *    active bits at boot and says so. And FPCCR.LSPACT cleared, because a lazy FP save the
     *    old image's exception had reserved would otherwise land on the
     *    old frame address - in our memory - at the first float.
     *
     *    A real reset arrives with IPSR 0 and skips all of it.
     */
    mrs   r0, ipsr
    cbz   r0, 2f
    ldr   r0, =0xE000EF34          /* FPU->FPCCR */
    ldr   r1, [r0]
    bic   r1, r1, #1               /* LSPACT */
    str   r1, [r0]
    ldr   r0, =0xE000ED14          /* SCB->CCR */
    ldr   r1, [r0]
    orr   r1, r1, #1               /* NONBASETHRDENA */
    str   r1, [r0]
    dsb
    isb
    sub   sp, sp, #32              /* r0 r1 r2 r3 r12 lr pc xpsr */
    movs  r0, #0
    str   r0, [sp, #0]
    str   r0, [sp, #4]
    str   r0, [sp, #8]
    str   r0, [sp, #12]
    str   r0, [sp, #16]
    str   r0, [sp, #20]
    ldr   r0, =Reset_Handler
    bic   r0, r0, #1               /* a stacked PC has bit 0 clear */
    str   r0, [sp, #24]
    ldr   r0, =0x01000000          /* xPSR: Thumb, IPSR 0 */
    str   r0, [sp, #28]
    ldr   r0, =0xFFFFFFF9          /* EXC_RETURN: thread, MSP, no FP */
    bx    r0
2:

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

    /*
     * 1b. THE CACHES, AND THIS HAS TO BE THE FIRST THING AFTER THE STACK.
     *
     *     This image runs from AXI SRAM, and the debugger writes it there
     *     through the AHB access port - which goes round the CPU's caches
     *     and does not invalidate them. The previous run's lines for
     *     those same addresses are therefore still in the I-cache and the
     *     D-cache, dirty, describing a program that no longer exists.
     *
     *     It cost a confusing evening. The FIRST load after a power-on
     *     always worked, because the factory firmware's cached lines
     *     covered flash at 0x08000000 and not SRAM at 0x24000000. The
     *     SECOND load - our image over our image, same addresses - ran a
     *     mixture of the two builds: a PLL solver that reported no
     *     solution existed for arithmetic that is provably correct, and
     *     then an UNALIGNED hard fault whose recorded PC and LR could not
     *     both have come from the same binary. That last detail is the
     *     tell. When the stacked PC and LR disagree about which program
     *     they belong to, the instruction stream is stale, not wrong.
     *
     *     clock.c has always claimed the data cache was off here. It was
     *     not: SystemInit() enabled the I-cache and never touched DC, so
     *     DC was whatever ran before, and the factory firmware leaves it
     *     ON. Measured: SCB_CCR read 0x00070200 on arrival.
     *
     *     INVALIDATE, DO NOT CLEAN. The usual sequence for disabling a
     *     data cache cleans it first, so that dirty lines reach memory.
     *     Here that is precisely the wrong thing: those dirty lines hold
     *     the OLD image and memory holds the NEW one, so cleaning would
     *     write the old program back over the new one. Discarding them is
     *     the whole point, and it is only safe this early - before the
     *     .data copy below has written anything worth keeping.
     *
     *     The D-cache's own stale lines are left sitting in the array
     *     rather than invalidated by a set/way loop: with DC clear they
     *     are never looked up and never evicted. Anything that later
     *     turns the D-cache on must invalidate first - which is what
     *     CMSIS's SCB_EnableDCache() does, so the normal route is safe.
     */
    ldr   r0, =0xE000ED14          /* SCB->CCR */
    ldr   r1, [r0]
    bic   r1, r1, #(1 << 16)       /* DC: data cache off */
    bic   r1, r1, #(1 << 17)       /* IC: instruction cache off */
    str   r1, [r0]
    dsb
    isb

    ldr   r0, =0xE000EF50          /* SCB->ICIALLU */
    movs  r1, #0
    str   r1, [r0]                 /* whole I-cache invalid */
    dsb
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
     * 3a. IS ANYBODY SWITCHING US ON? From flash, a reset with the power
     *     button not held goes straight back to standby (src/boot.c). A
     *     RAM image over SWD returns at once. It touches no global, which
     *     is what makes calling it before .data and .bss legal.
     *
     *     FIRST, and that order was learnt: the DFU check below has to
     *     wait ~20 ms for the peripheral rail before it can read the
     *     ladder, and on the owner's unit the press of the power button
     *     is the only thing powering the MCU until PC6 is up. Asked
     *     second, a quick press could already be over.
     */
    bl    boot_power_check

    /*
     * 3b. THE WAY BACK. Both encoder buttons held at power-on jumps to
     *     the ROM bootloader (src/boot.c), before .data, .bss and the
     *     clocks, so that nothing this firmware does later can stand
     *     between a held button and DFU. Same rules as 3a. Raises PC6
     *     either way - the ladder hangs from that rail. Returns if the
     *     buttons are not held.
     */
    bl    boot_dfu_check

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

    /*
     * 5b. Zero upstream's .tcmram, which lives in the DTCM (both linker
     *     scripts say why). NOLOAD in both images, so nothing else will:
     *     not the debugger, not the copy above. The DTCM is clocked by
     *     the core itself, so it is usable this early, before SystemInit.
     */
    ldr   r2, =_stcmram
    ldr   r4, =_etcmram
    movs  r3, #0
    b     LoopFillZeroTcm

FillZeroTcm:
    str   r3, [r2]
    adds  r2, r2, #4

LoopFillZeroTcm:
    cmp   r2, r4
    bcc   FillZeroTcm

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

    /*
     * External interrupts 0..149. Only the ones this project actually
     * uses are named; the rest are filled in as they are needed.
     *
     * THE FIRST ONE EVER NAMED HERE IS NUMBER 11, DMA1_Stream0, and it
     * is the receive block interrupt that makes audio possible: the
     * demodulator has to run at block rate, and nothing else in this
     * firmware runs at block rate. Until this line the project got by
     * without a single external vector, which is why encoder.c was
     * grafted onto the existing SysTick rather than given one.
     *
     * The counts are written as arithmetic rather than as two magic
     * numbers, so that naming the next one cannot silently shift
     * everything after it - a vector table off by one entry is a jump
     * into whatever follows, with no diagnostic at all.
     */
    .rept 11                        /* 0..10 */
    .word Default_Handler
    .endr
    .word DMA1_Stream0_IRQHandler   /* 11 - SAI1_B receive, the block hook */
    .rept 51 - 11 - 1               /* 12..50 */
    .word Default_Handler
    .endr
    .word SPI3_IRQHandler           /* 51 - the panel's block queue (EOT) */
    .rept 82 - 51 - 1               /* 52..81 */
    .word Default_Handler
    .endr
    .word UART7_IRQHandler          /* 82 - the console's transmit ring */
    .rept 150 - 82 - 1              /* 83..149 */
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

    /* Weak, like the rest: a build without sai1.c still links, and the
     * slot points at Default_Handler, which reports rather than hangs. */
    weak_handler DMA1_Stream0_IRQHandler
    weak_handler SPI3_IRQHandler
    weak_handler UART7_IRQHandler
