/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfusion.h - Instruction fusion optimizer
 *
 * KEY CONCEPT:
 *   Pattern recognition and optimization for instruction sequences.
 */

#ifndef XFUSION_H
#define XFUSION_H

#include "../../runtime/value/xchunk.h"
#include <stdbool.h>
#include "../../base/xdefs.h"

/* ========== Instruction Fusion Optimizer ========== */

// Perform instruction fusion optimization on XrProto
//
// Optimizations include:
//   1. LOADK constant optimization: LOADK K(0/1/-1) => LOADI 0/1/-1
//   2. Arithmetic immediate optimization: ADD R, K(n) => ADDI R, n (if n is small int)
//   3. Compare immediate optimization: LOADK + LT/LE => LTI/LEI
//
// Parameters:
//   proto - function prototype to optimize
//
// Returns:
//   number of optimizations performed
XR_FUNC int xr_fusion_optimize(XrProto *proto);

/* ========== Sub-optimization Functions ========== */

// LOADK constant optimization
//
// Replace LOADK instructions loading common constants with more efficient LOADI
//
// Examples:
//   LOADK R1, K(0)   => LOADI R1, 0
//   LOADK R1, K(1)   => LOADI R1, 1
//   LOADK R1, K(-1)  => LOADI R1, -1
XR_FUNC int xr_fusion_loadk_const(XrProto *proto);

// Arithmetic immediate optimization
//
// Optimize LOADK + arithmetic operation to immediate operation
//
// Examples:
//   LOADK R2, K(5)
//   ADD R1, R1, R2   => ADDI R1, R1, 5 (if 5 is in range)
XR_FUNC int xr_fusion_arith_imm(XrProto *proto);

// Compare constant optimization
//
// Optimize LOADK in comparison operations to immediate comparison
//
// Examples:
//   LOADK R2, K(0)
//   LT R1, R2        => LTI R1, 0
XR_FUNC int xr_fusion_cmp_const(XrProto *proto);

/* ========== Helper Functions ========== */

// Check if constant value fits in immediate
// Returns true and sets *imm if can be represented as immediate
XR_FUNC bool xr_fusion_is_small_int(XrValue value, int *imm);

/* ========== Optimization Statistics ========== */

typedef struct FusionStats {
    int loadk_to_loadi;  // LOADK to LOADI optimizations
    int arith_to_imm;    // Arithmetic to immediate optimizations
    int cmp_to_imm;      // Compare to immediate optimizations
    int total_fusions;   // Total fusion count
} FusionStats;

extern FusionStats g_fusion_stats;

XR_FUNC void xr_fusion_reset_stats(void);
XR_FUNC void xr_fusion_print_stats(void);

#endif  // XFUSION_H
