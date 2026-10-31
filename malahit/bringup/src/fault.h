#ifndef MALAHIT_FAULT_H
#define MALAHIT_FAULT_H

#include <stdint.h>

/*
 * WHERE A CRASH LEAVES ITS NOTE.
 *
 * A fixed address, not a linker symbol, and that is the whole point. The
 * first time this project went looking for a crash the record was an
 * ordinary global; a rebuild moved it, the debugger kept reading the old
 * address, and what came back was a piece of a format string being read as
 * a counter. An hour went into trusting that number.
 *
 * 0x2407FF00 is the last 256 bytes of AXI SRAM. Both linker scripts put
 * _estack below it, so the stack can never reach it, and nothing else is
 * allocated there. It will still be at this address after any rebuild, in
 * either the RAM or the flash link, which means:
 *
 *     ../tools/swd.sh fault
 *
 * always works and never needs to be told where to look.
 *
 * It also survives a warm reset - SRAM keeps its contents - so a crash can
 * be read out AFTER resetting the part to get the debugger talking again.
 * That matters on this board, where a locked-up core may be reachable no
 * other way.
 */
#define FAULT_RECORD_ADDR   0x2407FF00UL
#define FAULT_MAGIC         0x4D414C46UL   /* "MALF" */

typedef struct {
    uint32_t magic;      /* FAULT_MAGIC once a fault has been recorded */
    uint32_t ipsr;       /* which exception: 3 = HardFault, 4 = MemManage,
                          * 5 = BusFault, 6 = UsageFault, >=16 = IRQ n-16 */
    uint32_t exc_return; /* LR on entry: says which stack, and FPU state   */

    /* The frame the core pushed. r0..r3 and r12 are the caller's. */
    uint32_t r0, r1, r2, r3, r12;
    uint32_t lr;         /* the faulting function's return address */
    uint32_t pc;         /* THE FAULTING INSTRUCTION - read this first */
    uint32_t xpsr;

    uint32_t cfsr;       /* configurable fault status: what kind */
    uint32_t hfsr;       /* hard fault status: usually just FORCED */
    uint32_t mmfar;      /* address that faulted, if MMARVALID */
    uint32_t bfar;       /* address that faulted, if BFARVALID */
    uint32_t count;      /* how many faults since power-up */
} fault_record_t;

#define FAULT_REC  ((volatile fault_record_t *)FAULT_RECORD_ADDR)

/* Called from the assembly fault handlers. Never returns. */
void fault_record(uint32_t *frame, uint32_t exc_return);

/* An exception with no handler, or main() returning. Never returns. */
void fault_unexpected(uint32_t ipsr);

/*
 * If a record is present, print it and clear it. Called from main() at
 * startup, so a crash reports itself on the next run rather than having to
 * be dug out by hand.
 */
void fault_report_previous(void);

#endif /* MALAHIT_FAULT_H */
