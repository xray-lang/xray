/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_target_riscv64.c - RISC-V 64 target description for Xm backend
 *
 * KEY CONCEPT:
 *   Defines the RV64GD register inventory and frame layout as a
 *   static XmTarget instance. LP64D ABI (standard Linux RISC-V).
 *
 * REGISTER ALLOCATION:
 *   Allocatable GPRs (20 total):
 *     Caller-saved (12): a0-a7 (x10-x17), t0-t3 (x5-x7,x28)
 *     Callee-saved (8):  s1 (x9), s2-s8 (x18-x24)
 *   Reserved:
 *     x0=zero, x1=ra, x2=sp, x3=gp, x4=tp, s0/x8=FP,
 *     s10/x26=jit_ctx, s11/x27=coro, t5/x30=scratch2, t6/x31=scratch
 *   Not allocatable (caller-saved, reserved as scratch):
 *     t4 (x29) — available as extra temp if needed
 *
 *   Allocatable FPRs (20 total):
 *     Caller-saved (12): fa0-fa7 (f10-f17), ft0-ft3 (f0-f3)
 *     Callee-saved (8):  fs0-fs1 (f8-f9), fs2-fs7 (f18-f23)
 *   Reserved:
 *     ft11 (f31) — FP scratch register
 */

#ifdef __riscv

#include "xm_target.h"
#include "../base/xchecks.h"
#include "xm_riscv64.h"

/* Allocatable GPRs: caller-saved first, then callee-saved.
 * t0-t3 + a0-a7 (caller-saved), s1, s2-s8 (callee-saved). */
static const int rv64_gpr_alloc[] = {
    /* Caller-saved (first 12 entries) */
    RV64_A0,
    RV64_A1,
    RV64_A2,
    RV64_A3,
    RV64_A4,
    RV64_A5,
    RV64_A6,
    RV64_A7,
    RV64_T0,
    RV64_T1,
    RV64_T2,
    RV64_T3,
    /* Callee-saved (next 8 entries) */
    RV64_S1,
    RV64_S2,
    RV64_S3,
    RV64_S4,
    RV64_S5,
    RV64_S6,
    RV64_S7,
    RV64_S8,
};

/* Allocatable FPRs: caller-saved first, then callee-saved.
 * ft0-ft3 + fa0-fa7 (caller-saved), fs0-fs1, fs2-fs7 (callee-saved). */
static const int rv64_fpr_alloc[] = {
    /* Caller-saved (first 12 entries) */
    RV64_FA0,
    RV64_FA1,
    RV64_FA2,
    RV64_FA3,
    RV64_FA4,
    RV64_FA5,
    RV64_FA6,
    RV64_FA7,
    RV64_FT0,
    RV64_FT1,
    RV64_FT2,
    RV64_FT3,
    /* Callee-saved (next 8 entries) */
    RV64_FS0,
    RV64_FS1,
    RV64_FS2,
    RV64_FS3,
    RV64_FS4,
    RV64_FS5,
    RV64_FS6,
    RV64_FS7,
};

const XmTarget xm_target_riscv64 = {
    .name = "riscv64",

    .ngpr = 20, /* a0-a7 + t0-t3 + s1 + s2-s8 */
    .nfpr = 20, /* fa0-fa7 + ft0-ft3 + fs0-fs1 + fs2-fs7 */

    .gpr_alloc = rv64_gpr_alloc,
    .fpr_alloc = rv64_fpr_alloc,

    .ngpr_caller_save = 12, /* a0-a7 + t0-t3 (alloc_regs[0..11]) */
    .nfpr_caller_save = 12, /* fa0-fa7 + ft0-ft3 (alloc_fp_regs[0..11]) */

    .scratch_gpr = {RV64_T6, RV64_T5},
    .coro_reg = RV64_S11, /* s11 = coroutine pointer */
    .sp_reg = RV64_SP,
    .fp_reg = RV64_FP, /* s0/x8 = frame pointer */
    .lr_reg = RV64_RA, /* x1 = return address */

    /* Frame layout (preliminary, will be refined during codegen bringup):
     * Stack map metadata: 16B
     * GPR callee-saved: ra + fp + s1-s9 + s10 + s11 = 13 * 8 = 104B
     * FPR callee-saved: fs0-fs7 = 8 * 8 = 64B
     * Total frame_base = 184B, rounded to 192B */
    .frame_base = 192,
    .spill_base = 192,
    .max_spill_slots = 32,

    .max_vregs = 4096,
};

/* Global current target pointer — set to this platform's target */
const XmTarget *xm_current_target = &xm_target_riscv64;

#endif /* __riscv */
