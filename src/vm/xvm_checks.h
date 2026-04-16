/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_checks.h - VM-specific defensive assertion macros
 *
 * KEY CONCEPT:
 *   Debug-mode assertions for VM execution engine.
 *   Zero overhead in release builds.
 */

#ifndef XVM_CHECKS_H
#define XVM_CHECKS_H

#include "../base/xchecks.h"

/* ========== Stack Bounds Checks ========== */

// Verify base + offset is within valid range
#define VM_DCHECK_STACK_BOUNDS(base, offset, stack, capacity) \
    XR_DCHECK((base) + (offset) >= (stack) && \
              (base) + (offset) < (stack) + (capacity), \
              "VM stack access out of bounds")

// Prevent base_offset - 1 from producing negative index
#define VM_DCHECK_BASE_OFFSET(offset) \
    XR_DCHECK_GE(offset, 0, "base_offset must be non-negative")

// base_offset must be > 0 to safely write to base_offset - 1
#define VM_DCHECK_RETURN_SLOT(base_offset) \
    XR_DCHECK_GT(base_offset, 0, "return slot requires base_offset > 0")

/* ========== Frame Bounds Checks ========== */

#define VM_DCHECK_FRAME_COUNT(count, max) \
    XR_DCHECK((count) > 0 && (count) <= (max), \
              "frame_count out of valid range")

#define VM_DCHECK_FRAME_ACCESS(index, count) \
    XR_DCHECK_LT(index, count, "frame index out of bounds")

/* ========== Pointer Validity Checks ========== */

#define VM_DCHECK_PTR(ptr, name) \
    XR_DCHECK((ptr) != NULL, name " must not be NULL")

#define VM_DCHECK_CLOSURE(cl) \
    XR_DCHECK((cl) != NULL && (cl)->proto != NULL, \
              "closure and proto must be valid")

/* ========== Constant Access Checks ========== */

#define VM_DCHECK_CONST_INDEX(idx, count) \
    XR_DCHECK_LT(idx, count, "constant index out of bounds")

/* ========== Register Access Checks ========== */

#define VM_DCHECK_REG(reg, max_stack) \
    XR_DCHECK_LT(reg, max_stack, "register index out of bounds")

#endif // XVM_CHECKS_H
