/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_helper_table.h - Central declaration table for all JIT C helpers
 *
 * DESIGN:
 *   Every JIT C helper is declared exactly once in xisa/xm/helpers.def.
 *   xisagen generates xm_helpers_gen.h which provides:
 *     - XmHelperId enum (compile-time IDs)
 *     - XmHelperInfo struct
 *     - XM_HELPER_DEF X-macro for fn-ptr table construction
 *   Builder and codegen use the metadata to:
 *     - Automatically derive result vtag from ret_rep
 *     - Emit correct tag writeback after CALL_C
 *     - Validate helper signatures at JIT init
 *
 *   Adding a new helper requires ONE edit: add a line to xisa/xm/helpers.def
 *   then regenerate (cmake --build . --target gen-xm-ops).
 *   Forgetting to add it causes a linker error (function not in table).
 */

#ifndef XM_HELPER_TABLE_H
#define XM_HELPER_TABLE_H

#include "../runtime/value/xtype.h"  // XrRep
#include "../base/xdefs.h"
#include "xm.h"              // XM_TK_*, VTAG_*, XmType
#include "xm_helpers_gen.h"  // XmHelperId, XmHelperInfo, XM_HELPER_DEF, XM_HF_*

// Declared in xm_helper_table.c
extern const XmHelperInfo xm_helper_info[XM_HELPER__COUNT];

/* ========== Convenience Queries ========== */

// Derive compile-time vtag from helper return rep
static inline uint8_t xm_helper_vtag(XmHelperId id) {
    if (id >= XM_HELPER__COUNT)
        return VTAG_TAGGED;
    switch (xm_helper_info[id].ret_rep) {
        case XR_REP_TAGGED:
            return VTAG_TAGGED;
        case XR_REP_PTR:
            return VTAG_PTR;
        case XR_REP_I64:
            return VTAG_I64;
        case XR_REP_F64:
            return VTAG_F64;
        case XR_REP_VOID:
            return VTAG_NULL;
        default:
            return VTAG_TAGGED;
    }
}

// Derive XmTypeKind from helper return rep
static inline uint8_t xm_helper_type_kind(XmHelperId id) {
    if (id >= XM_HELPER__COUNT)
        return XM_TK_TAGGED;
    switch (xm_helper_info[id].ret_rep) {
        case XR_REP_TAGGED:
            return XM_TK_TAGGED;
        case XR_REP_PTR:
            return XM_TK_PTR;
        case XR_REP_I64:
            return XM_TK_INT;
        case XR_REP_F64:
            return XM_TK_FLOAT;
        case XR_REP_VOID:
            return XM_TK_NULL;
        default:
            return XM_TK_TAGGED;
    }
}

// Lookup helper ID by function pointer. Returns XM_HELPER__COUNT if not found.
static inline XmHelperId xm_helper_lookup(void *func_ptr) {
    for (int i = 0; i < XM_HELPER__COUNT; i++) {
        if (xm_helper_info[i].func == func_ptr)
            return (XmHelperId) i;
    }
    return XM_HELPER__COUNT;
}

static inline bool xm_helper_may_gc(XmHelperId id) {
    return id < XM_HELPER__COUNT && (xm_helper_info[id].flags & XM_HF_GC);
}

static inline bool xm_helper_may_deopt(XmHelperId id) {
    return id < XM_HELPER__COUNT && (xm_helper_info[id].flags & XM_HF_DEOPT);
}

static inline bool xm_helper_may_throw(XmHelperId id) {
    return id < XM_HELPER__COUNT && (xm_helper_info[id].flags & XM_HF_THROW);
}

static inline bool xm_helper_may_suspend(XmHelperId id) {
    return id < XM_HELPER__COUNT && (xm_helper_info[id].flags & XM_HF_SUSPEND);
}

static inline bool xm_helper_may_enter_vm(XmHelperId id) {
    return id < XM_HELPER__COUNT && (xm_helper_info[id].flags & XM_HF_ENTER_VM);
}

static inline bool xm_helper_may_run_user_code(XmHelperId id) {
    return id < XM_HELPER__COUNT && (xm_helper_info[id].flags & XM_HF_USER_CODE);
}

static inline bool xm_helper_needs_stackmap(XmHelperId id) {
    return id < XM_HELPER__COUNT && (xm_helper_info[id].flags & XM_HF_STACKMAP);
}

static inline uint8_t xm_helper_pointer_trust(XmHelperId id) {
    if (id >= XM_HELPER__COUNT)
        return XM_HPT_EXTERNAL;
    return xm_helper_info[id].pointer_trust;
}

static inline uint8_t xm_helper_post_call(XmHelperId id) {
    if (id >= XM_HELPER__COUNT)
        return XM_HPC_DEOPT | XM_HPC_THROW | XM_HPC_SUSPEND;
    return xm_helper_info[id].post_call;
}

static inline void *xm_helper_func(XmHelperId id) {
    if (id >= XM_HELPER__COUNT)
        return NULL;
    return xm_helper_info[id].func;
}

static inline bool xm_helper_call_c_needs_deopt_check(const XmFunc *func, const XmIns *ins) {
    if (!func || !ins || !xm_ref_is_const(ins->args[0]))
        return true;
    uint32_t ci = XM_REF_INDEX(ins->args[0]);
    if (ci >= func->nconst)
        return true;
    XmHelperId id = xm_helper_lookup((void *) (uintptr_t) func->consts[ci].val.raw);
    return (xm_helper_post_call(id) & (XM_HPC_DEOPT | XM_HPC_SUSPEND)) != 0;
}

// Check if a helper returns a dynamic-typed value (needs runtime tag)
static inline bool xm_helper_is_tagged(XmHelperId id) {
    return id < XM_HELPER__COUNT && xm_helper_info[id].ret_rep == XR_REP_TAGGED;
}

#endif  // XM_HELPER_TABLE_H
