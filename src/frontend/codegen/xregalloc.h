/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregalloc.h - LIFO register allocator
 *
 * KEY CONCEPT:
 *   Pure LIFO stack-based management. Single freereg integer
 *   tracks stack top. Protect stack for consecutive arguments.
 */

#ifndef XREGALLOC_H
#define XREGALLOC_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../../base/xdefs.h"

#define XREG_MAX 250           // Max register count
#define XREG_PROTECT_STACK 16  // Max protect region nesting depth
#define XREG_SCOPE_MAX 64      // Max scope nesting depth

/* ============================================================
 * Protect Region (for consecutive arguments, etc.)
 * ============================================================ */
typedef struct XProtectRegion {
    int base;              // Protect region start register
    int count;             // Number of protected registers
    const char *reason;    // Reason for protection (debug)
} XProtectRegion;

/* ============================================================
 * Register Allocator (LIFO simplified version)
 * 
 * Core concept:
 *   - freereg is the only state, representing "stack top"
 *   - num_locals marks local variable region, registers in this region cannot be freed
 *   - Allocation: freereg++
 *   - Deallocation: freereg-- (must be stack top)
 * ============================================================ */
typedef struct XRegAlloc {
    /* === Core State === */
    int num_locals;        // Local variable count (fixed region, cannot be freed)
    int freereg;           // First free register (stack top)
    int watermark;         // Historical max usage (for maxstacksize)
    
    /* === Protect Region Stack (for consecutive arguments) === */
    XProtectRegion protect_stack[XREG_PROTECT_STACK];
    int protect_depth;     // Current protect region nesting depth
    
    /* === Scope Tracking === */
    int scope_depth;       // Current scope depth
    int scope_base[XREG_SCOPE_MAX];  // num_locals start value for each scope
    
    /* === Debug === */
    bool enable_debug;
    
} XRegAlloc;

/* ============================================================
 * API: Creation and Destruction
 * ============================================================ */

// Create new register allocator
XR_FUNC XRegAlloc* xreg_new(void);

// Destroy allocator
XR_FUNC void xreg_free(XRegAlloc *ra);

// Reset (when compiling new function)
XR_FUNC void xreg_reset(XRegAlloc *ra);

/* ============================================================
 * API: Register Allocation (LIFO)
 * ============================================================ */

// Allocate local variable register
// - Allocates from freereg, also updates num_locals
// - Local variable registers cannot be freed via xreg_free_temp
XR_FUNC int xreg_alloc_local(XRegAlloc *ra, const char *name);

// Allocate temporary register
// - Simple freereg++ operation
// - O(1) complexity
XR_FUNC int xreg_alloc_temp(XRegAlloc *ra);

// Free temporary register (LIFO)
// - Must free freereg - 1 (stack top)
// - Cannot free registers in local variable region
// - Prints warning when LIFO order is violated (asserts in debug mode)
XR_FUNC void xreg_free_temp(XRegAlloc *ra, int reg);

// Reserve n consecutive registers
// - freereg += n
// - Returns first register number
XR_FUNC int xreg_reserve(XRegAlloc *ra, int n);

/* ============================================================
 * API: Stack Pointer Control (Core)
 * 
 * Key to LIFO model: direct control of freereg
 * - Batch reclaim: xreg_set_freereg(ra, saved_base)
 * - Consecutive allocation: loop xreg_alloc_temp or directly set_freereg
 * ============================================================ */

// Get current freereg (next allocatable position)
XR_FUNC int xreg_get_freereg(XRegAlloc *ra);

// Set freereg (core method for batch reclaim)
XR_FUNC void xreg_set_freereg(XRegAlloc *ra, int freereg);

// Get num_locals (local variable region end position)
XR_FUNC int xreg_get_local_end(XRegAlloc *ra);

// Set num_locals
XR_FUNC void xreg_set_local_end(XRegAlloc *ra, int local_end);

/* ============================================================
 * API: Protect Region Management
 * 
 * For scenarios that need to protect intermediate results (e.g., consecutive argument compilation)
 * ============================================================ */

// Begin protecting a register region
// - Returns protect region ID (for ending protection)
// - During protection, these registers won't be allocated by xreg_alloc_temp
XR_FUNC int xreg_protect_begin(XRegAlloc *ra, int base, int count, const char *reason);

// End protect region
// - Must end in LIFO order
XR_FUNC void xreg_protect_end(XRegAlloc *ra, int protect_id);

// Check if register is in protect region
XR_FUNC bool xreg_is_protected(XRegAlloc *ra, int reg);

/* ============================================================
 * API: Scope Management
 * ============================================================ */

// Enter new scope
// - Records current num_locals
XR_FUNC void xreg_scope_enter(XRegAlloc *ra);

// Exit scope
// - Restores num_locals to value when entered
// - Also sets freereg to num_locals
XR_FUNC void xreg_scope_exit(XRegAlloc *ra);

/* ============================================================
 * API: Query
 * ============================================================ */

// Get local variable count
XR_FUNC int xreg_num_locals(XRegAlloc *ra);

// Get watermark (historical max usage)
XR_FUNC int xreg_watermark(XRegAlloc *ra);

// Check if register is a local variable
XR_FUNC bool xreg_is_local(XRegAlloc *ra, int reg);

/* ============================================================
 * API: Consecutive Allocation
 * ============================================================ */

// Allocate next register (equivalent to xreg_alloc_temp)
XR_FUNC int xreg_alloc_next(XRegAlloc *ra);

// Mark register as local variable
XR_FUNC void xreg_set_local(XRegAlloc *ra, int reg, const char *name);

/* ============================================================
 * API: Debug
 * ============================================================ */

// Print allocator state
XR_FUNC void xreg_dump(XRegAlloc *ra);

// Set debug mode
XR_FUNC void xreg_set_debug(XRegAlloc *ra, bool enable);

/* ============================================================
 * Debug Assertion Macros
 * ============================================================ */

#ifdef XREG_DEBUG_ASSERTS
    #define ASSERT_REG_VALID(reg) do { \
        if ((reg) < 0 || (reg) >= XREG_MAX) { \
            fprintf(stderr, "[XReg] Invalid register: %d\n", (reg)); \
        } \
    } while(0)
    
    #define ASSERT_FREEREG(ra, expected) do { \
        int actual = xreg_get_freereg(ra); \
        if (actual != (expected)) { \
            fprintf(stderr, "[XReg] freereg mismatch: %d != %d\n", actual, (expected)); \
        } \
    } while(0)
    
    #define ASSERT_FREEREG_GE(ra, min_val) do { \
        int actual = xreg_get_freereg(ra); \
        if (actual < (min_val)) { \
            fprintf(stderr, "[XReg] freereg too small: %d < %d\n", actual, (min_val)); \
        } \
    } while(0)
#else
    #define ASSERT_REG_VALID(reg) ((void)0)
    #define ASSERT_FREEREG(ra, expected) ((void)0)
    #define ASSERT_FREEREG_GE(ra, min_val) ((void)0)
#endif

#endif // XREGALLOC_H
