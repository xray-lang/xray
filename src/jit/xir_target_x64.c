/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_target_x64.c - x86-64 target description for XIR backend
 *
 * KEY CONCEPT:
 *   Defines the x86-64 register inventory and frame layout as a
 *   static XirTarget instance. This is the single source of truth
 *   for all x86-64-specific register and frame parameters.
 *
 * CALLING CONVENTION (System V AMD64 ABI):
 *   Integer args:  rdi, rsi, rdx, rcx, r8, r9
 *   FP args:       xmm0-xmm7
 *   Return:        rax (+ rdx for 128-bit)
 *   Caller-saved:  rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11
 *   Callee-saved:  rbx, rbp, r12, r13, r14, r15
 *   Stack pointer: rsp (16-byte aligned before CALL)
 *   Red zone:      128 bytes below rsp (leaf functions only)
 *
 * JIT REGISTER ASSIGNMENT:
 *   r15 = coroutine pointer (callee-saved, reserved)
 *   rbp = frame pointer (callee-saved, reserved)
 *   rsp = stack pointer (reserved)
 *   r11 = scratch (caller-saved, not allocatable)
 *   Remaining 13 GP regs are allocatable.
 */

#ifdef __x86_64__

#include "xir_target.h"
#include "../base/xchecks.h"
#include "xir_x64.h"

/* Allocatable GP registers.
 * Caller-saved (8): rax, rcx, rdx, rsi, rdi, r8, r9, r10  (r11=scratch excluded)
 * Callee-saved (3): rbx, r12, r13  (r14=jit_ctx, r15=coro, rbp=FP excluded)
 * Total: 11 allocatable */
static const int x64_gpr_alloc[] = {
    /* Caller-saved (first 8 entries) */
    X64_RAX, X64_RCX, X64_RDX, X64_RSI, X64_RDI, X64_R8, X64_R9, X64_R10,
    /* Callee-saved (next 3 entries) */
    X64_RBX, X64_R12, X64_R13,
    /* Note: r11=scratch, r14=jit_ctx, r15=coro, rbp=FP → not allocatable */
};

/* Allocatable FP registers: xmm0-xmm14 (xmm15 reserved as scratch).
 * System V ABI: all xmm regs are caller-saved. xmm15 is held back so the
 * codegen has a guaranteed FP scratch for two-operand fixups, fp/gp moves
 * and similar contract repairs without disturbing register-allocator state. */
static const int x64_fpr_alloc[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};

/*
 * Frame layout (x86-64):
 *
 *   [higher addresses]
 *     return address      (pushed by CALL)      +8
 *     saved rbp           (PUSH rbp)            +0  ← rbp points here
 *     saved rbx           -8
 *     saved r12           -16
 *     saved r13           -24
 *     saved r14           -32
 *     saved r15 (coro)    -40
 *     stack_map_ptr       -48
 *     safepoint_id        -56
 *     --- spill slots start here ---            -64
 *
 *   frame_base = 56 bytes (6 callee-saved regs × 8 + stack_map_ptr(8) + safepoint_id(8))
 *   Wait: 5 callee-saved (rbx,r12,r13,r14,r15) + rbp = 6 × 8 = 48
 *         + stack_map_ptr(8) + safepoint_id(8) = 64
 *   spill_base = 64
 */
const XirTarget xir_target_x64 = {
    .name = "x86-64",

    .ngpr = 11,  // 8 caller-saved + 3 callee-saved (minus r11,r14,r15,rbp,rsp)
    .nfpr = 15,  // xmm0-xmm14 (xmm15 reserved as scratch)

    .gpr_alloc = x64_gpr_alloc,
    .fpr_alloc = x64_fpr_alloc,

    .ngpr_caller_save = 8,  // rax,rcx,rdx,rsi,rdi,r8,r9,r10
    .nfpr_caller_save =
        15,  // System V: all xmm are caller-saved (xmm15 is scratch, not allocatable)

    .scratch_gpr = {X64_R11, -1},  // r11 = scratch; no second scratch
    .coro_reg = X64_R15,
    .sp_reg = X64_RSP,
    .fp_reg = X64_RBP,
    .lr_reg = -1,  // x86-64 has no link register (uses stack)

    .frame_base = 64,       // callee-saved(48) + stack_map_ptr(8) + safepoint_id(8)
    .spill_base = 64,       // spill slots start after frame metadata
    .max_spill_slots = 32,  // match ARM64 limit (uint32_t spill_bitmap)

    .max_vregs = 4096,
};

// Global current target pointer — set to this platform's target
const XirTarget *xir_current_target = &xir_target_x64;

#endif  // __x86_64__
