/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_target_arm64.c - ARM64 target description for XIR backend
 *
 * KEY CONCEPT:
 *   Defines the ARM64 register inventory and frame layout as a
 *   static XirTarget instance. This is the single source of truth
 *   for all ARM64-specific register and frame parameters.
 */

#include "xir_target.h"
#include "../base/xchecks.h"
#include "xir_arm64.h"

/* Allocatable GP registers: x1-x15 (caller-saved), x21-x27 (callee-saved).
 * x0 = return value / temp, x16/x17 = scratch, x18 = platform,
 * x19 = coro pointer (reserved), x20 = safepoint stride counter (reserved),
 * x28 = jit_ctx (reserved), x29 = FP, x30 = LR, x31 = SP. */
static const int arm64_gpr_alloc[] = {
    A64_X1, A64_X2, A64_X3, A64_X4, A64_X5, A64_X6, A64_X7,
    A64_X8, A64_X9, A64_X10, A64_X11, A64_X12, A64_X13, A64_X14, A64_X15,
    A64_X21, A64_X22, A64_X23, A64_X24, A64_X25, A64_X26, A64_X27,
};

/* Allocatable FP registers: d0-d15.
 * d0-d7 are caller-saved, d8-d15 are callee-saved. */
static const int arm64_fpr_alloc[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

const XirTarget xir_target_arm64 = {
    .name               = "arm64",

    .ngpr               = 22,       // x1-x15 + x21-x27 (x20=safept, x28=jit_ctx)
    .nfpr               = 16,       // d0-d15

    .gpr_alloc          = arm64_gpr_alloc,
    .fpr_alloc          = arm64_fpr_alloc,

    .ngpr_caller_save   = 15,       // x1-x15 (alloc_regs[0..14])
    .nfpr_caller_save   = 8,        // d0-d7 (alloc_fp_regs[0..7])

    .scratch_gpr        = { A64_X16, A64_X17 },
    .coro_reg           = A64_X19,
    .sp_reg             = A64_SP,
    .fp_reg             = A64_FP,
    .lr_reg             = A64_LR,

    .frame_base         = 176,      // callee-saved(160) + stack_map_ptr(8) + safepoint_id(8) = 176B
    .spill_base         = 176,      // spill slots start after frame metadata
    .max_spill_slots    = 64,

    .max_vregs          = 512,
};

// Global current target pointer (initialized by JIT init)
const XirTarget *xir_current_target = &xir_target_arm64;
