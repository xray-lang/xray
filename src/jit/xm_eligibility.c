/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_eligibility.c - Implementation of is_jit_eligible()
 *
 * Determines whether a proto is eligible for JIT compilation based
 * on heuristics (code size, deopt count, feedback, etc.).
 */

#include "xm_eligibility.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "xm_target.h"
#include "../ir/xi.h"
#include "../base/xmalloc.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_feedback.h"
#include "../runtime/object/xstring.h"

/* ========== Helpers ========== */

/*
 * Return true if |st| names a primitive slot kind the JIT knows how to
 * carry end-to-end (register / spill layout, deopt reconstruction, etc.).
 */
static bool check_slot_type_eligible(uint8_t st) {
    return st == XR_SLOT_I64 || st == XR_SLOT_F64 || st == XR_SLOT_BOOL || st == XR_SLOT_PTR;
}

static bool type_is_jit_seed_type(const XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type))
        return false;
    if (type->kind == XR_KIND_CHANNEL)
        return true;
    uint8_t st = xr_type_to_slot_type((XrType *) type);
    return st == XR_SLOT_I64 || st == XR_SLOT_F64 || st == XR_SLOT_BOOL;
}

static void seed_proto_signature_from_xi(struct XrProto *proto) {
    if (!proto || !proto->xi_func)
        return;

    const XiFunc *func = (const XiFunc *) proto->xi_func;
    uint16_t nparams = 0;
    if (proto->numparams > 0 && proto->numparams <= 8) {
        nparams = func->nparams;
        if (nparams > (uint16_t) proto->numparams)
            nparams = (uint16_t) proto->numparams;
    }

    bool needs_param_array = false;
    for (uint16_t i = 0; i < nparams; i++) {
        const XiValue *param = func->params ? func->params[i] : NULL;
        if (param && type_is_jit_seed_type(param->type) &&
            (!proto->param_types || i >= proto->param_types_count || !proto->param_types[i])) {
            needs_param_array = true;
            break;
        }
    }

    if (needs_param_array && !proto->param_types) {
        proto->param_types =
            (struct XrType **) xr_calloc((size_t) proto->numparams, sizeof(struct XrType *));
        if (proto->param_types)
            proto->param_types_count = (uint8_t) proto->numparams;
    }

    if (proto->param_types) {
        for (uint16_t i = 0; i < nparams && i < proto->param_types_count; i++) {
            const XiValue *param = func->params ? func->params[i] : NULL;
            if (!proto->param_types[i] && param && type_is_jit_seed_type(param->type))
                proto->param_types[i] = param->type;
        }
    }

    if (!proto->return_type_info && type_is_jit_seed_type(func->return_type))
        proto->return_type_info = func->return_type;
}

static const XrType *proto_static_type_for_value(const struct XrProto *proto,
                                                 const XiValue *value) {
    if (!value)
        return NULL;
    if (value->type && value->type->kind != XR_KIND_UNKNOWN)
        return value->type;
    if (!proto || value->op != XI_PARAM || value->aux_int < 0)
        return NULL;
    uint32_t param_index = (uint32_t) value->aux_int;
    if (!proto->param_types || param_index >= proto->param_types_count)
        return NULL;
    return proto->param_types[param_index];
}

static bool type_contains_channel(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_CHANNEL)
        return true;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (type_contains_channel(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

static bool channel_method_may_suspend(int method_sym) {
    switch (method_sym) {
        case SYMBOL_SEND:
        case SYMBOL_RECV:
        case SYMBOL_SENDTIMEOUT:
        case SYMBOL_RECVTIMEOUT:
            return true;
        default:
            return false;
    }
}

static bool xi_value_uses_suspend_channel_helper(const struct XrProto *proto,
                                                 const XiValue *value) {
    if (!value)
        return false;

    switch (value->op) {
        case XI_GO:
        case XI_AWAIT:
        case XI_SELECT_BLOCK:
        case XI_YIELD:
            return true;
        case XI_CALL_METHOD:
            if (value->nargs < 1 || (value->aux_int & 1) != 0)
                return false;
            if (!channel_method_may_suspend((int) (value->aux_int >> 1)))
                return false;
            return type_contains_channel(proto_static_type_for_value(proto, value->args[0]));
        default:
            return false;
    }
}

static bool xi_func_uses_suspend_channel_helpers(const struct XrProto *proto, const XiFunc *func) {
    if (!func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            if (xi_value_uses_suspend_channel_helper(proto, block->values[vi]))
                return true;
        }
    }
    return false;
}

static XiFunc *resolve_shared_slot_callee(const XiFunc *caller, int64_t slot) {
    if (slot < 0)
        return NULL;
    for (const XiFunc *f = caller; f; f = f->parent_func) {
        if (!f->shared_slot_funcs || slot >= (int64_t) f->shared_slot_func_count)
            continue;
        XiFunc *callee = f->shared_slot_funcs[slot];
        if (callee)
            return callee;
    }
    return NULL;
}

static XiFunc *resolve_known_xi_callee(const XiFunc *caller, const XiValue *callee) {
    while (callee && callee->op == XI_COPY && callee->nargs >= 1)
        callee = callee->args[0];
    if (!callee)
        return NULL;
    if (callee->op == XI_CLOSURE_NEW && callee->aux)
        return (XiFunc *) callee->aux;
    if (callee->op == XI_GET_SHARED)
        return resolve_shared_slot_callee(caller, callee->aux_int);
    return NULL;
}

static bool xi_value_may_suspend_when_called(const XiValue *value) {
    if (!value)
        return false;
    if (xi_value_uses_suspend_channel_helper(NULL, value))
        return true;
    return value->op == XI_CHAN_SEND || value->op == XI_CHAN_RECV;
}

static bool xi_func_may_suspend_when_called(const XiFunc *func, int depth) {
    if (!func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (xi_value_may_suspend_when_called(value))
                return true;
            if (depth <= 0 || !value || value->op != XI_CALL || value->nargs < 1)
                continue;
            XiFunc *callee = resolve_known_xi_callee(func, value->args[0]);
            if (callee && xi_func_may_suspend_when_called(callee, depth - 1))
                return true;
        }
    }
    return false;
}

static bool xi_func_calls_suspendable_known_callee(const XiFunc *func) {
    if (!func)
        return false;
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        const XiBlock *block = func->blocks[bi];
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            const XiValue *value = block->values[vi];
            if (!value || value->op != XI_CALL || value->nargs < 1)
                continue;
            XiFunc *callee = resolve_known_xi_callee(func, value->args[0]);
            if (callee && xi_func_may_suspend_when_called(callee, 4))
                return true;
        }
    }
    return false;
}

/* ========== Public API ========== */

void xm_eligibility_prepare(struct XrProto *proto) {
    if (!proto)
        return;

    seed_proto_signature_from_xi(proto);

    /* Deopt backoff state machine: reset deopt_count when the backoff
     * period has elapsed, giving the function another JIT attempt.
     * This mutates proto fields and MUST run on the main thread only. */
    uint32_t dc = atomic_load_explicit(&proto->deopt_count, memory_order_relaxed);
    if (dc > 3 && dc < 5) {
        uint32_t backoff = proto->deopt_backoff ? proto->deopt_backoff : 10;
        uint32_t current = atomic_load_explicit(&proto->call_count, memory_order_relaxed);
        if (current - proto->deopt_reset_at >= backoff) {
            atomic_store_explicit(&proto->deopt_count, 0, memory_order_relaxed);
            proto->deopt_reset_at = current;
            proto->deopt_backoff = backoff * 2 < 10000 ? backoff * 2 : 10000;
        }
    }

    /* Promote feedback return type to return_type_info if not set.
     * This write is only safe on the main thread. */
    if (!proto->return_type_info && proto->type_feedback && proto->type_feedback->stable) {
        uint8_t fb_ret = xfb_to_slot_type(proto->type_feedback->return_type);
        if (fb_ret != XR_SLOT_ANY) {
            proto->return_type_info = xr_slot_type_to_type(NULL, fb_ret);
        }
    }
}

bool is_jit_eligible(struct XrProto *proto, bool verbose) {
    const char *name = (proto && proto->name) ? XR_STRING_CHARS(proto->name) : "?";

    if (!proto)
        return false;

    // Must have bb_leaders for CFG construction (legacy builder path).
    // xi_to_xm builds CFG directly from Xi IR SSA blocks, so bb_leaders
    // is not required when proto carries attached Xi IR.
    if (!proto->bb_leaders && !proto->xi_func) {
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: no bb_leaders\n", name);
        return false;
    }

    // Complexity guard: oversized functions stay in interpreter.
    // Derived from max_vregs (bytecode-to-vreg ratio ~1:0.25, so limit = max_vregs * 4).
    int max_bc = xm_current_target ? xm_current_target->max_vregs * 4 : 2048;
    if (proto->code.count > max_bc) {
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: too many bytecodes (%d, limit %d)\n", name,
                    proto->code.count, max_bc);
        return false;
    }

    // No vararg functions
    if (proto->is_vararg) {
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: vararg function\n", name);
        return false;
    }

    /* The current coroutine/channel JIT path lowers blocking channel operations
     * through suspend-bridged helpers.  Microbenchmarks show that this partial
     * path can be slower than the VM's direct channel bytecodes, so keep those
     * functions on the VM until direct channel lowering is ready. */
    if (proto->xi_func &&
        xi_func_uses_suspend_channel_helpers(proto, (const XiFunc *) proto->xi_func)) {
        atomic_store_explicit(&proto->jit_static_blocked, 1, memory_order_relaxed);
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: suspend channel helper path\n", name);
        return false;
    }

    if (proto->xi_func && xi_func_calls_suspendable_known_callee((const XiFunc *) proto->xi_func)) {
        atomic_store_explicit(&proto->jit_static_blocked, 1, memory_order_relaxed);
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: calls suspendable callee\n", name);
        return false;
    }

    // Max 16 upvalues (closures supported)
    if (PROTO_UPVAL_COUNT(proto) > 16) {
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: too many upvalues (%d)\n", name,
                    (int) PROTO_UPVAL_COUNT(proto));
        return false;
    }

    // Max 8 params
    if (proto->numparams > 8) {
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: too many params (%d)\n", name, proto->numparams);
        return false;
    }

    // Adaptive deopt policy:
    //   deopt_count < 5  → normal (keep current JIT code)
    //   deopt_count >= 5  → conservative recompile (no type speculation)
    //   deopt_count >= 20 → permanently disable JIT for this proto
    uint32_t dc = atomic_load_explicit(&proto->deopt_count, memory_order_relaxed);
    if (dc >= 20) {
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: permanently disabled (deopt_count=%u)\n", name, dc);
        return false;
    }
    if (dc > 3 && dc < 5) {
        /* Backoff window: xm_eligibility_prepare() resets deopt_count when
         * the backoff period elapses.  If we still see dc in (3,5) here,
         * the backoff has not yet elapsed — skip. */
        uint32_t backoff = proto->deopt_backoff ? proto->deopt_backoff : 10;
        uint32_t current = atomic_load_explicit(&proto->call_count, memory_order_relaxed);
        if (current - proto->deopt_reset_at < backoff) {
            if (verbose)
                fprintf(stderr, "[JIT] skip %s: deopt backoff (%u/%u)\n", name,
                        current - proto->deopt_reset_at, backoff);
            return false;
        }
        /* Backoff elapsed but prepare() hasn't run yet (bg re-check path).
         * Allow compilation — the reset will happen on the next main-thread
         * trigger. */
    }
    // deopt_count >= 5: eligible, but caller should use conservative mode

    // Source 1: param_types (authoritative per-parameter types)
    if (proto->param_types) {
        for (int i = 0; i < proto->numparams; i++) {
            uint8_t gc = (i < proto->param_types_count && proto->param_types[i])
                             ? xr_type_to_slot_type(proto->param_types[i])
                             : XR_SLOT_ANY;
            if (!check_slot_type_eligible(gc)) {
                if (verbose)
                    fprintf(stderr, "[JIT] skip %s: param %d has ineligible slot_type %d\n", name,
                            i, gc);
                return false;
            }
        }
    }
    // Source 2: runtime profile feedback (stable monomorphic types)
    else if (proto->type_feedback && proto->type_feedback->stable) {
        XmTypeFeedback *fb = proto->type_feedback;
        for (int i = 0; i < proto->numparams; i++) {
            if (!xfb_is_monomorphic(fb->arg_types[i])) {
                if (verbose)
                    fprintf(stderr, "[JIT] skip %s: param %d not monomorphic in feedback\n", name,
                            i);
                return false;
            }
            uint8_t st = xfb_to_slot_type(fb->arg_types[i]);
            if (!check_slot_type_eligible(st)) {
                if (verbose)
                    fprintf(stderr, "[JIT] skip %s: param %d feedback type %d ineligible\n", name,
                            i, st);
                return false;
            }
        }
    }
    // No type info at all — not eligible (unless zero params)
    else {
        if (proto->numparams > 0) {
            if (verbose)
                fprintf(stderr,
                        "[JIT] skip %s: %d params but no type info "
                        "(no param_types, no feedback)\n",
                        name, proto->numparams);
            return false;
        }
    }

    /* Return type eligibility: xm_eligibility_prepare() promotes feedback
     * return type on the main thread; here we only read. */
    uint8_t rt =
        proto->return_type_info ? xr_type_to_slot_type(proto->return_type_info) : XR_SLOT_ANY;
    // XR_SLOT_ANY is allowed: void functions and untyped returns
    // xm_jit_call handles ANY return with safe fallback (i64 payload)
    if (rt != XR_SLOT_ANY && rt != XR_SLOT_I64 && rt != XR_SLOT_F64 && rt != XR_SLOT_PTR &&
        rt != XR_SLOT_BOOL) {
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: return type %d (unsupported)\n", name, rt);
        return false;
    }

    /* xi_to_xm handles unsupported ops by returning NULL at lowering
     * time — no bytecode opcode whitelist needed. */
    return true;
}
