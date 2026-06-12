/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_abi.c - AOT function ABI plan
 */

#include "xaot_abi.h"
#include "xaot_abi_gen.h"
#include "../ir/xi_ops_gen.h"
#include "../base/xmalloc.h"
#include <string.h>

static bool func_has_op_class(const XiFunc *func, uint8_t op_class) {
    uint32_t bi;

    if (!func)
        return false;
    for (bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *blk = func->blocks[bi];
        uint32_t vi;
        if (!blk)
            continue;
        for (vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && xi_generated_op_class(v->op) == op_class)
                return true;
        }
    }
    return false;
}

static XaotArgClass arg_class_for_value_rep(XaotValueRep rep) {
    switch (rep.kind) {
        case XAOT_VALUE_VOID:
            return XAOT_ARG_VOID;
        case XAOT_VALUE_SCALAR:
            return XAOT_ARG_SCALAR;
        case XAOT_VALUE_PTR:
            return XAOT_ARG_PTR;
        case XAOT_VALUE_AGGREGATE:
            return XAOT_ARG_AGG_BY_VALUE;
        case XAOT_VALUE_TAGGED:
        default:
            return XAOT_ARG_TAGGED;
    }
}

static XaotAbiSlot tagged_slot(const XrType *type) {
    XaotAbiSlot slot;
    memset(&slot, 0, sizeof(slot));
    slot.cls = XAOT_ARG_TAGGED;
    slot.rep.kind = XAOT_VALUE_TAGGED;
    slot.rep.rep = XAOT_REP_TAGGED;
    slot.rep.type = type;
    slot.rep.c_type = "XrValue";
    slot.c_type = "XrValue";
    return slot;
}

static XaotBoundaryReason tagged_reason_for_func(const XiFunc *func, bool is_module_init) {
    if (is_module_init)
        return XAOT_BOUNDARY_MODULE_INIT;
    if (func && func->ncaptures > 0)
        return XAOT_BOUNDARY_CLOSURE_OBJECT;
    if (func_has_op_class(func, XI_GEN_CLASS_COROUTINE))
        return XAOT_BOUNDARY_CORO_FRAME;
    if (func_has_op_class(func, XI_GEN_CLASS_EXCEPTION))
        return XAOT_BOUNDARY_EXCEPTION_FLOW;
    return XAOT_BOUNDARY_TAGGED_TYPE;
}

XR_FUNC bool xaot_abi_build_func(XaotFuncAbi *abi, const XiFunc *func, bool is_module_init) {
    bool native_abi;
    uint16_t i;

    if (!abi || !func)
        return false;

    memset(abi, 0, sizeof(*abi));
    abi->nparams = func->nparams;
    if (func->nparams > 0) {
        abi->params = (XaotAbiSlot *) xr_calloc(func->nparams, sizeof(XaotAbiSlot));
        if (!abi->params)
            return false;
    }

    native_abi = !is_module_init && func->ncaptures == 0 &&
                 !func_has_op_class(func, XI_GEN_CLASS_COROUTINE) &&
                 !func_has_op_class(func, XI_GEN_CLASS_EXCEPTION) &&
                 xaot_abi_type_can_use_typed_boundary(func->return_type);

    if (!native_abi) {
        abi->kind =
            func_has_op_class(func, XI_GEN_CLASS_COROUTINE) ? XAOT_ABI_CORO : XAOT_ABI_TAGGED;
        abi->boundary_reason = tagged_reason_for_func(func, is_module_init);
        abi->ret = tagged_slot(func->return_type);
        for (i = 0; i < func->nparams; i++) {
            const XiValue *param = func->params ? func->params[i] : NULL;
            abi->params[i] = tagged_slot(param ? param->type : NULL);
        }
        return true;
    }

    abi->kind = XAOT_ABI_NATIVE;
    abi->boundary_reason = XAOT_BOUNDARY_NONE;
    abi->ret.rep = xaot_value_rep_for_type(func->return_type);
    abi->ret.cls = arg_class_for_value_rep(abi->ret.rep);
    abi->ret.c_type = abi->ret.rep.c_type;
    for (i = 0; i < func->nparams; i++) {
        const XiValue *param = func->params ? func->params[i] : NULL;
        abi->params[i].rep = xaot_value_rep_for_type(param ? param->type : NULL);
        abi->params[i].cls = arg_class_for_value_rep(abi->params[i].rep);
        abi->params[i].c_type = abi->params[i].rep.c_type;
    }
    return true;
}

XR_FUNC void xaot_abi_free(XaotFuncAbi *abi) {
    if (!abi)
        return;
    xr_free(abi->params);
    memset(abi, 0, sizeof(*abi));
}

XR_FUNC const char *xaot_abi_kind_name(XaotAbiKind kind) {
    switch (kind) {
        case XAOT_ABI_NATIVE:
            return "native";
        case XAOT_ABI_TAGGED:
            return "tagged";
        case XAOT_ABI_ADAPTER:
            return "adapter";
        case XAOT_ABI_CORO:
            return "coro";
        case XAOT_ABI_RUNTIME_HELPER:
            return "runtime-helper";
        default:
            return "?";
    }
}
