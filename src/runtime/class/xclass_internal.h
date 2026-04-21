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
 *     1. The full XrItableEntry layout (xclass.h only knows it as an
 *        opaque forward-declared struct).
 *     2. Build-time helpers -- xr_class_build_itable,
 *        xr_class_compute_operator_flags, xr_symbol_to_op_flag --
 *        that the builder calls during finalize and that nothing
 *        outside the class module has any business invoking.
 *     3. Abstract-class internals (add/inherit/is-abstract-method
 *        plus the free hook) that used to sit in xclass.h with no
 *        external caller. Lifting them out keeps the public header
 *        focused on the "class as a value" contract.
 *
 * WHY THIS DESIGN:
 *   Public consumers (vm/, jit/, frontend/, api/) only need
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

/* ========== ITable Entry ========== */

// Interface table entry for interface method dispatch.
//
// method_symbol_to_index[] is a per-entry reverse map from method
// symbol -> index into methods[]. It is allocated by
// xr_class_build_itable and sized by the highest symbol the entry
// references (method_map_capacity). A NULL map means "no symbol
// index available" (e.g. allocation pressure); callers fall back to
// the methods[] array walk on that path.
struct XrItableEntry {
    struct XrClass *interface;
    XrMethod **methods;
    uint16_t method_count;
    int *method_symbol_to_index;
    int method_map_capacity;
};

/* ========== Build-Time Helpers ========== */

// Compute operator overload flags for a class. Called once from
// xr_class_builder_finalize after methods[] has been populated.
XR_FUNC void xr_class_compute_operator_flags(XrClass *cls);

// Map an operator method symbol to its XR_OP_*_FLAG bit, or 0 if the
// symbol is not an operator. Internal-only; callers outside the class
// module go through XCLASS_HAS_OP (public, flag-level).
XR_FUNC uint32_t xr_symbol_to_op_flag(int symbol);

// Rebuild the class's itable from its interfaces[] array. Idempotent:
// frees any previously-allocated itable state before repopulating.
// Returns 0 on success, -1 on allocation failure (cls->itable is
// restored to NULL in that case so the class stays in a well-defined
// "no itable" state rather than half-built).
XR_FUNC int xr_class_build_itable(XrClass *cls);

/* ========== Abstract Class Internals ========== */

// Mark `symbol` as an abstract method on cls (de-duplicated).
XR_FUNC void xr_class_add_abstract_method(XrClass *cls, int method_symbol);

// Copy parent's abstract method list into child, filtering out any
// symbol child already implements concretely.
XR_FUNC void xr_class_inherit_abstract_methods(XrClass *child, XrClass *parent);

// Return true iff `method_symbol` is one of cls's abstract methods.
XR_FUNC bool xr_class_is_abstract_method(XrClass *cls, int method_symbol);

/* ========== Cleanup ========== */

// Explicit class teardown. Classes are normally GC-managed, but
// xr_class_builder_finalize uses this to unwind half-built classes
// on allocation failure.
XR_FUNC void xr_class_free(XrClass *cls);

#endif // XCLASS_INTERNAL_H
