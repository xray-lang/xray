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
 *   Every JIT C helper is declared exactly once in XM_HELPER_DEF.
 *   The macro table generates:
 *     - XmHelperId enum (compile-time IDs)
 *     - xm_helper_info[] metadata array (function pointer, return rep, flags)
 *   Builder and codegen use the metadata to:
 *     - Automatically derive result vtag from ret_rep
 *     - Emit correct tag writeback after CALL_C
 *     - Validate helper signatures at JIT init
 *
 *   Adding a new helper requires ONE edit: add a line to XM_HELPER_DEF.
 *   Forgetting to add it causes a linker error (function not in table).
 *
 * XM_HELPER_DEF(name, nargs, ret_rep, flags)
 *
 *   name:     C function name suffix after "xr_jit_"
 *   nargs:    number of call_arg_pool arguments consumed by this helper
 *             (NOT counting the (coro, extra_arg) ABI params — those are implicit)
 *   ret_rep:  return representation (determines tag writeback strategy):
 *             XR_REP_TAGGED — returns XrJitResult, tag is dynamic (x1)
 *             XR_REP_PTR    — always returns a GC pointer
 *             XR_REP_I64    — always returns int64
 *             XR_REP_F64    — always returns float64
 *             XR_REP_VOID   — no meaningful return value
 *             (XR_REP_I64 is used for bool returns too — codegen emits XR_TAG_BOOL
 *              when the helper's XmType.kind is XM_TK_BOOL)
 *   flags:    bitwise OR of XM_HF_* flags:
 *             XM_HF_GC      — may trigger GC (needs safepoint)
 *             XM_HF_DEOPT   — may request deoptimization
 *             XM_HF_THROW   — may throw exception
 *             XM_HF_SUSPEND — may suspend coroutine (CPS)
 */

#ifndef XM_HELPER_TABLE_H
#define XM_HELPER_TABLE_H

#include "../runtime/value/xtype.h"  // XrRep
#include "../base/xdefs.h"
#include "xm.h"  // XM_TK_*, VTAG_*, XmType

/* ========== Helper Flags ========== */

#define XM_HF_GC (1 << 0)       // may trigger GC
#define XM_HF_DEOPT (1 << 1)    // may request deoptimization
#define XM_HF_THROW (1 << 2)    // may throw exception
#define XM_HF_SUSPEND (1 << 3)  // may suspend coroutine (CPS)

/* ========== The Declaration Table ========== */
/*
 * Format: _(name, nargs, ret_rep, flags)
 *
 * Grouped by category. Keep alphabetical within each group.
 */
#define XM_HELPER_DEF(_)                                                                          \
    /* ---- Call / Invoke ---- */                                                                  \
    _(call_self, 0, XR_REP_TAGGED, XM_HF_GC | XM_HF_DEOPT)                                       \
    _(call_func, 0, XR_REP_TAGGED, XM_HF_GC | XM_HF_DEOPT)                                       \
    _(invoke_method, 0, XR_REP_TAGGED, XM_HF_GC | XM_HF_DEOPT)                                   \
    _(invoke_direct, 0, XR_REP_TAGGED, XM_HF_GC | XM_HF_DEOPT)                                   \
    _(closure_new, 0, XR_REP_PTR, XM_HF_GC)                                                       \
    _(closure_set_upval, 0, XR_REP_VOID, 0)                                                        \
    _(upval_get, 0, XR_REP_TAGGED, 0)                                                              \
    /* ---- Arithmetic (mixed-type fallback) ---- */                                               \
    _(rt_add, 2, XR_REP_TAGGED, XM_HF_GC)                                                         \
    _(rt_sub, 2, XR_REP_TAGGED, 0)                                                                 \
    _(rt_mul, 2, XR_REP_TAGGED, 0)                                                                 \
    _(rt_div, 2, XR_REP_TAGGED, 0)                                                                 \
    _(rt_mod, 2, XR_REP_TAGGED, 0)                                                                 \
    _(rt_eq, 2, XR_REP_I64, 0)    /* returns bool */                                               \
    _(eq_value, 2, XR_REP_I64, 0) /* returns bool */                                               \
    /* ---- Property Access ---- */                                                                \
    _(getprop, 1, XR_REP_TAGGED, XM_HF_GC | XM_HF_DEOPT)                                         \
    _(setprop, 2, XR_REP_VOID, XM_HF_GC)                                                          \
    _(getfield_ic, 0, XR_REP_TAGGED, 0)                                                            \
    _(getbuiltin, 0, XR_REP_TAGGED, 0)                                                             \
    /* ---- Index / Container Access ---- */                                                       \
    _(index_get, 2, XR_REP_TAGGED, XM_HF_GC | XM_HF_DEOPT)                                       \
    _(index_set, 3, XR_REP_VOID, XM_HF_GC)                                                        \
    _(tarray_get, 2, XR_REP_TAGGED, 0)                                                             \
    _(tarray_set, 3, XR_REP_VOID, 0)                                                               \
    _(map_get, 2, XR_REP_TAGGED, XM_HF_GC)                                                        \
    _(map_set, 3, XR_REP_VOID, XM_HF_GC)                                                          \
    _(map_increment, 2, XR_REP_VOID, XM_HF_GC)                                                    \
    /* ---- Shared Variables ---- */                                                               \
    _(get_shared, 0, XR_REP_TAGGED, XM_HF_GC)                                                     \
    _(set_shared, 1, XR_REP_VOID, XM_HF_GC)                                                       \
    /* ---- Exception ---- */                                                                      \
    _(throw, 1, XR_REP_VOID, XM_HF_THROW)                                                         \
    /* ---- Type Operations ---- */                                                                \
    _(is_type, 1, XR_REP_I64, 0)   /* returns bool */                                              \
    _(checktype, 1, XR_REP_I64, 0) /* returns bool */                                              \
    _(typename, 1, XR_REP_PTR, 0)                                                                  \
    _(typeof, 1, XR_REP_I64, 0)                                                                    \
    _(deep_copy, 1, XR_REP_PTR, XM_HF_GC)                                                         \
    /* ---- String Operations ---- */                                                              \
    _(chr, 0, XR_REP_PTR, XM_HF_GC)                                                               \
    _(substring, 0, XR_REP_PTR, XM_HF_GC)                                                         \
    _(str_repeat, 0, XR_REP_PTR, XM_HF_GC)                                                        \
    _(tostring, 1, XR_REP_PTR, XM_HF_GC)                                                          \
    _(strbuf_new, 0, XR_REP_PTR, XM_HF_GC)                                                        \
    _(strbuf_append, 0, XR_REP_VOID, XM_HF_GC)                                                    \
    _(strbuf_finish, 0, XR_REP_PTR, XM_HF_GC)                                                     \
    /* ---- Struct ---- */                                                                         \
    _(new_struct, 0, XR_REP_PTR, XM_HF_GC)                                                        \
    _(struct_get, 0, XR_REP_TAGGED, 0)                                                             \
    _(struct_set, 0, XR_REP_VOID, 0)                                                               \
    _(struct_copy, 0, XR_REP_PTR, XM_HF_GC)                                                       \
    /* ---- Container Construction ---- */                                                         \
    _(rt_array_new, 0, XR_REP_PTR, XM_HF_GC)                                                      \
    _(rt_array_push, 0, XR_REP_VOID, XM_HF_GC)                                                    \
    _(rt_array_len, 0, XR_REP_I64, 0)                                                              \
    _(rt_map_new, 0, XR_REP_PTR, XM_HF_GC)                                                        \
    _(newrange, 0, XR_REP_PTR, XM_HF_GC)                                                          \
    _(range_unpack, 0, XR_REP_VOID, 0)                                                             \
    _(newset, 0, XR_REP_PTR, XM_HF_GC)                                                            \
    _(slice, 0, XR_REP_PTR, XM_HF_GC)                                                             \
    _(bytes_new, 0, XR_REP_PTR, XM_HF_GC)                                                         \
    /* ---- Enum ---- */                                                                           \
    _(enum_access, 0, XR_REP_TAGGED, 0)                                                            \
    _(enum_name, 0, XR_REP_PTR, 0)                                                                 \
    _(enum_convert, 0, XR_REP_TAGGED, 0)                                                           \
    /* ---- IO / Debug ---- */                                                                     \
    _(print, 0, XR_REP_VOID, 0)                                                                    \
    _(dump, 0, XR_REP_VOID, 0)                                                                     \
    _(assert, 1, XR_REP_VOID, XM_HF_THROW)                                                        \
    _(assert_eq, 2, XR_REP_VOID, XM_HF_THROW)                                                     \
    _(assert_ne, 2, XR_REP_VOID, XM_HF_THROW)                                                     \
    /* ---- Channel / Concurrency ---- */                                                          \
    _(chan_new, 0, XR_REP_PTR, XM_HF_GC)                                                          \
    _(chan_close, 0, XR_REP_VOID, 0)                                                               \
    _(chan_is_closed, 0, XR_REP_I64, 0) /* returns bool */                                         \
    _(chan_try_send, 0, XR_REP_I64, 0)  /* returns bool */                                         \
    _(chan_try_recv, 0, XR_REP_TAGGED, 0)                                                          \
    _(chan_send, 0, XR_REP_TAGGED, XM_HF_GC | XM_HF_SUSPEND)                                     \
    _(chan_send_block, 0, XR_REP_VOID, XM_HF_SUSPEND)                                             \
    _(chan_recv, 0, XR_REP_TAGGED, XM_HF_GC | XM_HF_SUSPEND)                                     \
    _(chan_recv_block, 0, XR_REP_VOID, XM_HF_SUSPEND)                                             \
    _(scope_enter, 0, XR_REP_VOID, 0)                                                              \
    _(scope_exit, 0, XR_REP_VOID, 0)                                                               \
    _(spawn_cont, 0, XR_REP_TAGGED, XM_HF_GC | XM_HF_SUSPEND)                                    \
    _(await, 0, XR_REP_TAGGED, XM_HF_SUSPEND)                                                     \
    _(await_block, 0, XR_REP_VOID, XM_HF_SUSPEND)                                                 \
    // ---- end ----

/* ========== Generated Enum ========== */

typedef enum {
#define XM_HELPER_ENUM_(name, nargs, ret_rep, flags) XM_HELPER_##name,
    XM_HELPER_DEF(XM_HELPER_ENUM_)
#undef XM_HELPER_ENUM_
    XM_HELPER__COUNT
} XmHelperId;

/* ========== Metadata Struct ========== */

typedef struct {
    void *func;       // C function pointer
    uint8_t ret_rep;  // XrRep: return representation
    uint8_t nargs;    // number of call_arg_pool arguments
    uint16_t flags;   // XM_HF_*
} XmHelperInfo;

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

// Check if a helper may trigger GC
static inline bool xm_helper_may_gc(XmHelperId id) {
    return id < XM_HELPER__COUNT && (xm_helper_info[id].flags & XM_HF_GC);
}

// Check if a helper may suspend
static inline bool xm_helper_may_suspend(XmHelperId id) {
    return id < XM_HELPER__COUNT && (xm_helper_info[id].flags & XM_HF_SUSPEND);
}

// Check if a helper returns a dynamic-typed value (needs runtime tag)
static inline bool xm_helper_is_tagged(XmHelperId id) {
    return id < XM_HELPER__COUNT && xm_helper_info[id].ret_rep == XR_REP_TAGGED;
}

#endif  // XM_HELPER_TABLE_H
