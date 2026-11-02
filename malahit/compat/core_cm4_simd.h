#ifndef MALAHIT_COMPAT_CORE_CM4_SIMD_H
#define MALAHIT_COMPAT_CORE_CM4_SIMD_H
/* See core_cmInstr.h in this directory. cmsis_gcc.h carries the SIMD
 * intrinsics too, guarded by __ARM_FEATURE_DSP, which the Cortex-M7
 * sets exactly as the M4 does. */
#include "cmsis_gcc.h"
#endif
