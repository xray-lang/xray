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
 * TWO ABIs SUPPORTED:
 *
 *   System V AMD64 (Linux/macOS):
 *     Args: rdi, rsi, rdx, rcx, r8, r9 | Return: rax+rdx
 *     Caller-saved: rax,rcx,rdx,rsi,rdi,r8-r11,xmm0-xmm15
 *     Callee-saved: rbx,rbp,r12-r15
 *
 *   Win64 (Windows):
 *     Args: rcx, rdx, r8, r9 | Return: rax
 *     Caller-saved: rax,rcx,rdx,r8-r11,xmm0-xmm5
 *     Callee-saved: rbx,rdi,rsi,rbp,r12-r15,xmm6-xmm15
 *     32-byte shadow space required before every CALL
 *
 * JIT REGISTER ASSIGNMENT (both ABIs):
 *   r15 = coroutine pointer (callee-saved, reserved)
 *   r14 = jit_ctx pointer  (callee-saved, reserved)
 *   rbp = frame pointer    (callee-saved, reserved)
 *   rsp = stack pointer    (reserved)
 *   r11 = scratch           (caller-saved, not allocatable)
 */

#if defined(__x86_64__) || defined(_M_X64)

#include "xir_target.h"
#include "../base/xchecks.h"
#include "xir_x64.h"
#include "xir_codegen_x64_internal.h"  /* X64_NGPR_CALLER_SAVE etc. */

#ifdef _WIN32
/* Win64: caller-saved (6) then callee-saved (5).
 * Caller-saved: rax, rcx, rdx, r8, r9, r10   (r11=scratch excluded)
 * Callee-saved: rbx, rdi, rsi, r12, r13       (r14=jit_ctx, r15=coro, rbp=FP excluded)
 * Total: 11 allocatable */
static const int x64_gpr_alloc[] = {
    /* Caller-saved (first 6 entries) */
    X64_RAX, X64_RCX, X64_RDX, X64_R8, X64_R9, X64_R10,
    /* Callee-saved (next 5 entries) */
    X64_RBX, X64_RDI, X64_RSI, X64_R12, X64_R13,
};
#else
/* System V: caller-saved (8) then callee-saved (3).
 * Caller-saved: rax, rcx, rdx, rsi, rdi, r8, r9, r10   (r11=scratch excluded)
 * Callee-saved: rbx, r12, r13                            (r14/r15/rbp excluded)
 * Total: 11 allocatable */
static const int x64_gpr_alloc[] = {
    /* Caller-saved (first 8 entries) */
    X64_RAX, X64_RCX, X64_RDX, X64_RSI, X64_RDI, X64_R8, X64_R9, X64_R10,
    /* Callee-saved (next 3 entries) */
    X64_RBX, X64_R12, X64_R13,
};
#endif

/* Allocatable FP registers: xmm0-xmm14 (xmm15 reserved as scratch).
 * System V: all xmm caller-saved.
 * Win64:    xmm0-xmm5 caller-saved, xmm6-xmm14 callee-saved.
 * xmm15 is held back as FP scratch for two-operand fixups. */
static const int x64_fpr_alloc[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};

const XirTarget xir_target_x64 = {
    .name = "x86-64",

    .ngpr = 11,  /* X64_NGPR_CALLER_SAVE + X64_NGPR_CALLEE_SAVE_ALLOC */
    .nfpr = 15,  /* xmm0-xmm14 */

    .gpr_alloc = x64_gpr_alloc,
    .fpr_alloc = x64_fpr_alloc,

    .ngpr_caller_save = X64_NGPR_CALLER_SAVE,
    .nfpr_caller_save = X64_NFPR_CALLER_SAVE,

    .scratch_gpr = {X64_R11, -1},
    .coro_reg = X64_R15,
    .sp_reg = X64_RSP,
    .fp_reg = X64_RBP,
    .lr_reg = -1,  /* x86-64 uses stack for return address */

    .frame_base = X64_JIT_FRAME_BASE,
    .spill_base = X64_SPILL_BASE,
    .max_spill_slots = 32,

    .max_vregs = 4096,
};

// Global current target pointer — set to this platform's target
const XirTarget *xir_current_target = &xir_target_x64;

#endif  // __x86_64__ || _M_X64
