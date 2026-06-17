/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_abi.h - AOT function ABI plan
 */

#ifndef XAOT_ABI_H
#define XAOT_ABI_H

#include "xaot_boundary.h"
#include "xaot_rep.h"
#include "../ir/xi.h"
#include "../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct XaotBundle XaotBundle;

typedef enum XaotAbiKind {
    XAOT_ABI_NATIVE = 0,
    XAOT_ABI_TAGGED,
    XAOT_ABI_ADAPTER,
    XAOT_ABI_CORO,
    XAOT_ABI_RUNTIME_HELPER,
} XaotAbiKind;

typedef enum XaotArgClass {
    XAOT_ARG_VOID = 0,
    XAOT_ARG_SCALAR,
    XAOT_ARG_PTR,
    XAOT_ARG_AGG_BY_VALUE,
    XAOT_ARG_AGG_BY_REF,
    XAOT_ARG_TAGGED,
    XAOT_ARG_AOT_CTX,
} XaotArgClass;

typedef struct XaotAbiSlot {
    XaotArgClass cls;
    XaotValueRep rep;
    const char *c_type;
    const char *c_name;
    uint32_t flags;
} XaotAbiSlot;

typedef struct XaotFuncAbi {
    XaotAbiKind kind;
    XaotAbiSlot ret;
    XaotAbiSlot *params;
    uint16_t nparams;
    XaotBoundaryReason boundary_reason;
    const char *c_symbol;
    const char *boxed_symbol;
} XaotFuncAbi;

XR_FUNC bool xaot_abi_build_func(XaotFuncAbi *abi, const XaotBundle *bundle, const XiFunc *func,
                                 bool is_module_init);
XR_FUNC void xaot_abi_free(XaotFuncAbi *abi);
XR_FUNC const char *xaot_abi_kind_name(XaotAbiKind kind);

/* Effective value rep a call site sees for an ABI slot.  A tagged-class
 * slot transports XrValue no matter which typed rep was recorded next to
 * it, so every boundary decision (prepare, verify, emit) must go through
 * this one resolver or the three drift apart. */
XR_FUNC XaotValueRep xaot_abi_slot_value_rep(const XaotAbiSlot *slot);

#endif  // XAOT_ABI_H
