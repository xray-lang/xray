/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_internal.h - Class module internals
 *
 * KEY CONCEPT:
 *   Everything in this header is private to src/runtime/class/.
 *   It sits one level below the public xclass.h and exposes:
 *     1. Build-time helpers -- xr_class_compute_operator_flags,
 *        xr_symbol_to_op_flag --
 *        that the builder calls during finalize and that nothing
 *        outside the class module has any business invoking.
 *     2. The free hook used by builder rollback paths.
 *
 * WHY THIS DESIGN:
 *   Public consumers (vm/, frontend/, api/) only need
 *   xclass.h: it carries the inline instanceof, public API
 *   signatures, operator flags, and the opaque XrClass struct they
 *   embed into values. Moving the above into xclass_internal.h
 *   narrows the public surface without forcing xclass.h to turn
 *   XrClass itself into an opaque type (which would cost inline
 *   instanceof its O(1) array probe).
 */

#ifndef XCLASS_INTERNAL_H
#define XCLASS_INTERNAL_H

#include "xclass.h"

/* ========== Build-Time Helpers ========== */

// Compute operator overload flags for a class. Called once from
// xr_class_builder_finalize after methods[] has been populated.
XR_FUNC void xr_class_compute_operator_flags(XrClass *cls);

// Map an operator method symbol to its XR_OP_*_FLAG bit, or 0 if the
// symbol is not an operator. Internal-only; callers outside the class
// module go through XCLASS_HAS_OP (public, flag-level).
XR_FUNC uint32_t xr_symbol_to_op_flag(int symbol);

/* ========== Cleanup ========== */

// Explicit class teardown. Classes are normally GC-managed, but
// xr_class_builder_finalize uses this to unwind half-built classes
// on allocation failure.
XR_FUNC void xr_class_free(XrClass *cls);

#endif  // XCLASS_INTERNAL_H
