/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_eligibility.c - Implementation of is_jit_eligible()
 *
 * The opcode whitelist scan delegates to jit_op_support_table[] in
 * xir_opcode_support.h, which is statically asserted to cover every
 * opcode — adding a new one will refuse to compile rather than silently
 * shipping with a gap.
 */

#include "xir_eligibility.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "xir_opcode_support.h"
#include "xir_target.h"
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

bool is_jit_eligible(struct XrProto *proto, bool verbose) {
    const char *name = (proto && proto->name) ? XR_STRING_CHARS(proto->name) : "?";

    if (!proto)
        return false;

    // Must have bb_leaders for CFG construction (legacy builder path).
    // xi_to_xir builds CFG directly from Xi IR SSA blocks, so bb_leaders
    // is not required when proto carries attached Xi IR.
    if (!proto->bb_leaders && !proto->xi_func) {
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: no bb_leaders\n", name);
        return false;
    }

    // Complexity guard: oversized functions stay in interpreter.
    // Derived from max_vregs (bytecode-to-vreg ratio ~1:0.25, so limit = max_vregs * 4).
    int max_bc = xir_current_target ? xir_current_target->max_vregs * 4 : 2048;
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
        // Backoff retry for mild deopt failures
        uint32_t backoff = proto->deopt_backoff ? proto->deopt_backoff : 10;
        uint32_t current = atomic_load_explicit(&proto->call_count, memory_order_relaxed);
        if (current - proto->deopt_reset_at < backoff) {
            if (verbose)
                fprintf(stderr, "[JIT] skip %s: deopt backoff (%u/%u)\n", name,
                        current - proto->deopt_reset_at, backoff);
            return false;
        }
        // Backoff elapsed: give one retry, double interval for next failure
        atomic_store_explicit(&proto->deopt_count, 0, memory_order_relaxed);
        proto->deopt_reset_at = current;
        proto->deopt_backoff = backoff * 2 < 10000 ? backoff * 2 : 10000;
        if (verbose)
            fprintf(stderr, "[JIT] retry %s after backoff (next=%u)\n", name, proto->deopt_backoff);
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
        XirTypeFeedback *fb = proto->type_feedback;
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

    // Only JIT functions with typed return values that xir_jit_call can reconstruct.
    // Promote feedback return type to return_type_info if not set.
    if (!proto->return_type_info && proto->type_feedback && proto->type_feedback->stable) {
        uint8_t fb_ret = xfb_to_slot_type(proto->type_feedback->return_type);
        if (fb_ret != XR_SLOT_ANY) {
            proto->return_type_info = xr_slot_type_to_type(NULL, fb_ret);
        }
    }
    uint8_t rt =
        proto->return_type_info ? xr_type_to_slot_type(proto->return_type_info) : XR_SLOT_ANY;
    // XR_SLOT_ANY is allowed: void functions and untyped returns
    // xir_jit_call handles ANY return with safe fallback (i64 payload)
    if (rt != XR_SLOT_ANY && rt != XR_SLOT_I64 && rt != XR_SLOT_F64 && rt != XR_SLOT_PTR &&
        rt != XR_SLOT_BOOL) {
        if (verbose)
            fprintf(stderr, "[JIT] skip %s: return type %d (unsupported)\n", name, rt);
        return false;
    }

    /* Whitelist scan — delegate to the single source of truth
     * (jit_op_support_table[] in xir_opcode_support.h).
     * SUPPORTED and DEOPT_FALLBACK opcodes are allowed; only BAIL_OUT
     * or UNIMPLEMENTED causes the whole function to stay in interpreter.
     * DEOPT_FALLBACK opcodes generate unconditional deopt at that PC,
     * allowing hot loops before the opcode to still run JIT-compiled.
     *
     * Skip when xi_func is attached: xi_to_xir works from Xi IR and
     * handles unsupported ops by returning NULL at lowering time. */
    if (proto->xi_func)
        return true;
    int code_len = proto->code.count;
    for (int ci = 0; ci < code_len; ci++) {
        XrInstruction ins = PROTO_CODE(proto, ci);
        OpCode op = GET_OPCODE(ins);
        if ((unsigned) op >= NUM_OPCODES) {
            if (verbose)
                fprintf(stderr, "[JIT] skip %s: invalid opcode %d at pc=%d\n", name, op, ci);
            return false;
        }
        JitOpcodeSupport sup = jit_op_support_table[op];
        if (sup == JIT_OP_SUPPORTED || sup == JIT_OP_DEOPT_FALLBACK)
            continue;
        if (verbose) {
            const char *why = "unsupported";
            switch (sup) {
                case JIT_OP_BAIL_OUT:
                    why = "bail-out";
                    break;
                case JIT_OP_UNIMPLEMENTED:
                    why = "NYI";
                    break;
                default:
                    why = "unknown";
                    break;
            }
            fprintf(stderr, "[JIT] skip %s: %s opcode %d at pc=%d\n", name, why, op, ci);
        }
        return false;
    }

    return true;
}
