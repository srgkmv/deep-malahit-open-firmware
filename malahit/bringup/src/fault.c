#include "stm32h7xx.h"
#include "fault.h"
#include "console.h"
#include "boot.h"         /* boot_power_park() */

/*
 * Everything here runs with the machine already broken, so it does as
 * little as possible and assumes nothing. No printf from inside the
 * handler: the UART may not be initialised, the clock may be wrong, and a
 * handler that hangs trying to report a fault is worse than one that says
 * nothing. The record goes to memory; the printing happens on the next
 * run, from fault_report_previous(), where the machine works.
 */

/*
 * Where a crashed run goes to wait. Thread mode, interrupts off, doing
 * nothing - see the long note in fault_record() for why it is reached by
 * returning from the handler instead of by looping inside it.
 */
static void fault_park(void)
{
    /*
     * Keep feeding the watchdog while parked.
     *
     * A crashed run that stops feeding it gets reset a few seconds
     * later, the reset drops PC6, the switched rail dies and the radio
     * switches itself off - taking with it the crash record, the
     * debug port and any chance of looking at what happened. The whole
     * point of parking instead of spinning in the handler was to leave
     * the machine inspectable; letting the watchdog undo that would be
     * silly.
     *
     * Written straight to the register rather than through
     * board_watchdog_feed(), because this runs after a fault and
     * should not depend on anything that might be what broke.
     *
     * The console's ring goes out first: from the display loop on,
     * output is buffered, and the bytes still queued are the last
     * thing the run said before it crashed. console_flush() polls with
     * interrupts left masked and is bounded by the ring's size.
     */
    console_flush();

    /*
     * AND THE POWER BUTTON STILL WORKS. Interrupts are masked here, so
     * boot_power_tick() - which lives in SysTick - is dead, and nothing
     * starts the watchdog in this firmware: until 20/11/2026 a crash
     * left a frozen radio that could not be switched off at all.
     * boot_power_park() polls the button, still feeds the watchdog in
     * case anything ever starts it, and switches off on a 3 s hold.
     * The core stays halt-able throughout - that was the point of
     * parking.
     */
    boot_power_park();
}

void fault_record(uint32_t *frame, uint32_t exc_return)
{
    volatile fault_record_t *r = FAULT_REC;

    uint32_t prev = (r->magic == FAULT_MAGIC) ? r->count : 0U;

    r->magic      = FAULT_MAGIC;
    r->ipsr       = __get_IPSR();
    r->exc_return = exc_return;

    r->r0  = frame[0];
    r->r1  = frame[1];
    r->r2  = frame[2];
    r->r3  = frame[3];
    r->r12 = frame[4];
    r->lr  = frame[5];
    r->pc  = frame[6];
    r->xpsr = frame[7];

    r->cfsr  = SCB->CFSR;
    r->hfsr  = SCB->HFSR;
    r->mmfar = SCB->MMFAR;
    r->bfar  = SCB->BFAR;
    r->count = prev + 1U;

    __DSB();

    /*
     * DO NOT SPIN HERE, AND DO NOT BREAKPOINT. RETURN.
     *
     * The breakpoint half is explained in startup_stm32h743.S: with no
     * debugger attached it double-faults and locks the core.
     *
     * Spinning inside the handler looks safe and is not, for a reason
     * that took a while to see. HardFault has no "active" bit in SHCSR -
     * unlike MemManage, BusFault, SysTick and the rest, there is nothing
     * to write a zero into. While a HardFault is active the core will not
     * run anything else, and the ONLY things that clear that state are an
     * exception return and a reset. A debugger can halt the core, load a
     * fresh image, set SP and PC and resume, and the core will simply sit
     * there executing nothing - which is exactly the symptom we chased,
     * repeatedly, mistaking it for a locked-up part.
     *
     * On this board that matters more than it would elsewhere: NRST is
     * not wired to the adapter, and a reset would hand control back to
     * the factory firmware in flash anyway, which then sleeps and takes
     * the debug port with it. So "just reset it" costs a power cycle,
     * every single crash.
     *
     * Returning normally from the handler is what deactivates HardFault.
     * So: rewrite the stacked PC to point at fault_park(), force the
     * stacked xPSR to a clean Thumb state, and return. The core lands in
     * Thread mode in a quiet loop, fully halt-able and fully restartable,
     * and the next ram_run starts a new image without anyone touching the
     * radio.
     *
     * Interrupts off first, so whatever was firing cannot fire again into
     * a machine that has already given up.
     */
    __disable_irq();

    frame[6] = (uint32_t)(void *)fault_park & ~1UL;  /* stacked PC   */
    frame[7] = 0x01000000UL;                         /* stacked xPSR */

    __DSB();
    __ISB();
}

void fault_unexpected(uint32_t ipsr)
{
    volatile fault_record_t *r = FAULT_REC;

    uint32_t prev = (r->magic == FAULT_MAGIC) ? r->count : 0U;

    r->magic      = FAULT_MAGIC;
    r->ipsr       = ipsr;
    r->exc_return = 0U;
    r->r0 = r->r1 = r->r2 = r->r3 = r->r12 = 0U;
    r->lr = r->pc = r->xpsr = 0U;
    r->cfsr  = SCB->CFSR;
    r->hfsr  = SCB->HFSR;
    r->mmfar = 0U;
    r->bfar  = 0U;
    r->count = prev + 1U;

    __DSB();

    for (;;) { }
}

static void explain_cfsr(uint32_t cfsr)
{
    /* Usage fault, CFSR[31:16] */
    if (cfsr & SCB_CFSR_UNDEFINSTR_Msk) {
        console_printf("    UNDEFINSTR  undefined instruction\n");
    }
    if (cfsr & SCB_CFSR_INVSTATE_Msk) {
        console_printf("    INVSTATE    bad execution state - a branch to an\n"
                       "                address with the Thumb bit clear, or\n"
                       "                an exception return that does not\n"
                       "                match the active exception\n");
    }
    if (cfsr & SCB_CFSR_INVPC_Msk) {
        console_printf("    INVPC       bad EXC_RETURN value\n");
    }
    if (cfsr & SCB_CFSR_NOCP_Msk) {
        console_printf("    NOCP        coprocessor disabled - the FPU was\n"
                       "                used before CPACR enabled it\n");
    }
    if (cfsr & SCB_CFSR_UNALIGNED_Msk) {
        console_printf("    UNALIGNED   unaligned access\n");
    }
    if (cfsr & SCB_CFSR_DIVBYZERO_Msk) {
        console_printf("    DIVBYZERO   integer divide by zero\n");
    }

    /* Bus fault, CFSR[15:8] */
    if (cfsr & SCB_CFSR_IBUSERR_Msk) {
        console_printf("    IBUSERR     bus error fetching an instruction\n");
    }
    if (cfsr & SCB_CFSR_PRECISERR_Msk) {
        console_printf("    PRECISERR   bus error on a data access, BFAR valid\n");
    }
    if (cfsr & SCB_CFSR_IMPRECISERR_Msk) {
        console_printf("    IMPRECISERR bus error, address unknown - usually a\n"
                       "                write that retired late\n");
    }

    /* Memory management fault, CFSR[7:0] */
    if (cfsr & SCB_CFSR_IACCVIOL_Msk) {
        console_printf("    IACCVIOL    MPU: instruction fetch not allowed\n");
    }
    if (cfsr & SCB_CFSR_DACCVIOL_Msk) {
        console_printf("    DACCVIOL    MPU: data access not allowed, MMFAR valid\n");
    }
}

static const char *exception_name(uint32_t ipsr)
{
    switch (ipsr) {
    case 0U:  return "thread mode - main() returned";
    case 3U:  return "HardFault";
    case 4U:  return "MemManage";
    case 5U:  return "BusFault";
    case 6U:  return "UsageFault";
    case 15U: return "SysTick";
    default:  return (ipsr >= 16U) ? "an unhandled peripheral interrupt"
                                   : "an unhandled system exception";
    }
}

void fault_report_previous(void)
{
    volatile fault_record_t *r = FAULT_REC;

    if (r->magic != FAULT_MAGIC) {
        return;
    }

    console_printf("\n*** THE PREVIOUS RUN CRASHED (fault #%u) ***\n",
                   r->count);
    console_printf("  exception : %u, %s\n",
                   r->ipsr, exception_name(r->ipsr));

    if (r->ipsr >= 16U) {
        console_printf("  IRQ       : %u\n", r->ipsr - 16U);
    }

    if (r->exc_return != 0U) {
        console_printf("  PC        : 0x%08x   <- the faulting instruction\n",
                       r->pc);
        console_printf("  LR        : 0x%08x   <- who called it\n", r->lr);
        console_printf("  xPSR      : 0x%08x\n", r->xpsr);
        console_printf("  r0-r3,r12 : %08x %08x %08x %08x %08x\n",
                       r->r0, r->r1, r->r2, r->r3, r->r12);
        console_printf("  EXC_RETURN: 0x%08x (%s stack%s)\n", r->exc_return,
                       (r->exc_return & 4U) ? "process" : "main",
                       (r->exc_return & 0x10U) ? "" : ", FPU context pushed");
    }

    console_printf("  CFSR      : 0x%08x\n", r->cfsr);
    explain_cfsr(r->cfsr);
    console_printf("  HFSR      : 0x%08x%s\n", r->hfsr,
                   (r->hfsr & SCB_HFSR_FORCED_Msk)
                       ? "  (FORCED: escalated from the fault above)" : "");

    if (r->cfsr & SCB_CFSR_MMARVALID_Msk) {
        console_printf("  MMFAR     : 0x%08x\n", r->mmfar);
    }
    if (r->cfsr & SCB_CFSR_BFARVALID_Msk) {
        console_printf("  BFAR      : 0x%08x\n", r->bfar);
    }

    console_printf("  Find the PC with:  arm-none-eabi-addr2line -e "
                   "build/bringup.elf 0x%08x\n\n", r->pc);

    /* Consumed. Clear it so the next boot does not report it again. */
    r->magic = 0U;
}
