/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_target.h - Target-independent backend interface
 *
 * KEY CONCEPT:
 *   Abstracts target-specific details (registers, frame layout, code emission)
 *   behind a uniform interface. The register allocator, spill logic, and
 *   optimization passes operate through this interface, enabling multiple
 *   backend targets (ARM64, x86-64, etc.) to share the same pipeline.
 *
 * WHY THIS DESIGN:
 *   - Inspired by QBE's Target struct: function pointers for target-specific
 *     phases (isel, emit, abi), with register descriptions as plain data.
 *   - Keeps the C codebase simple (no inheritance, just a struct + callbacks).
 *   - Register allocator only needs register counts and classes, not encodings.
 *
 * RELATED MODULES:
 *   - xm_codegen.c: uses XmTarget for register allocation and code emission
 *   - xm_arm64.h: ARM64 instruction encoding (target-specific)
 */

#ifndef XM_TARGET_H
#define XM_TARGET_H

#include <stdint.h>
#include <stdbool.h>

/* ========== Target Description ========== */

typedef struct XmTarget {
    const char *name;  // "arm64", "x86_64"

    // --- Register inventory ---
    int ngpr;  // number of allocatable GP registers
    int nfpr;  // number of allocatable FP registers

    /* Allocatable register arrays (indexed 0..ngpr-1 / 0..nfpr-1).
     * Values are target-specific register enum values. */
    const int *gpr_alloc;  // GP allocatable registers
    const int *fpr_alloc;  // FP allocatable registers

    // Caller-saved register count (first N in alloc arrays are caller-saved)
    int ngpr_caller_save;  // e.g. 15 for ARM64 (x1-x15)
    int nfpr_caller_save;  // e.g. 8 for ARM64 (d0-d7)

    // Special-purpose registers (target-specific enum values)
    int scratch_gpr[2];  // scratch registers (not allocatable)
    int coro_reg;        // register holding coroutine pointer
    int sp_reg;          // stack pointer register
    int fp_reg;          // frame pointer register
    int lr_reg;          // link register (-1 if not applicable)

    // --- Frame layout ---
    int frame_base;       // size of callee-saved area (bytes)
    int spill_base;       // offset where spill slots start (bytes)
    int max_spill_slots;  // maximum number of spill slots

    // --- Limits ---
    int max_vregs;  // maximum virtual registers supported
} XmTarget;

/* ========== Global Target Instance ========== */

/*
 * The active target is set once at JIT initialization.
 * All target-dependent code reads from this pointer.
 */
extern const XmTarget *xm_current_target;

// Built-in target definitions
extern const XmTarget xm_target_arm64;
extern const XmTarget xm_target_x64;

#endif  // XM_TARGET_H
