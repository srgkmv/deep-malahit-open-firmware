#ifndef MALAHIT_COMPAT_CORE_CMINSTR_H
#define MALAHIT_COMPAT_CORE_CMINSTR_H
/*
 * A forwarding shim, so upstream's CMSIS-DSP can be compiled for a
 * Cortex-M7 without editing a single upstream file.
 *
 * CMSIS/DSP/Include/cmsis_compiler.h - which is itself a shim upstream
 * wrote - ends with:
 *
 *     #include "core_cmInstr.h"
 *     #include "core_cm4_simd.h"
 *
 * Those are the CMSIS-4 era headers that carried the hardware
 * intrinsics (__SSAT, __CLZ, __QADD8 ...), and upstream vendors them in
 * CMSIS/Include for its GD32F450. This port uses a modern CMSIS 5/6
 * core instead, where the same intrinsics live in cmsis_gcc.h and the
 * two old headers no longer exist.
 *
 * Pointing the include path at upstream's CMSIS/Include would drag
 * Cortex-M4 core definitions into a Cortex-M7 build, which is the wrong
 * fix for a reason that would not show up until something subtle broke.
 * The instruction set is not the problem - the M7 has the same DSP
 * extension as the M4, so the SIMD intrinsics are identical - the
 * problem is two CMSIS generations defining the same names in one
 * translation unit.
 *
 * So: resolve the name, forward to the modern header, leave upstream
 * alone. The alternative was editing CMSIS/DSP/Include/cmsis_compiler.h,
 * and the whole premise of compiling User/ in place is that it stays
 * byte-identical to the tree it came from.
 */
#include "cmsis_gcc.h"
#endif
