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

/* ========== Public API ========== */

void xm_eligibility_prepare(struct XrProto *proto) {
    if (!proto)
        return;

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
