/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_jit.c - JIT compiler state and VM integration
 *
 * KEY CONCEPT:
 *   Manages the JIT compilation pipeline and provides the bridge
 *   between VM's tagged XrValue world and JIT's raw value world.
 *
 * WHY THIS DESIGN:
 *   - C bridge builds raw int64 args array, calls JIT via (coro, args_ptr)
 *   - Unified calling convention: consistent with OSR entry
 *   - Supports mixed i64/f64 parameters and return values
 *   - Compilation is synchronous (on hot path, but fast enough for Tier 1)
 */

#include "xir_jit.h"
#include "xir.h"
#include "../base/xlog.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype_feedback.h"
#include "xir_builder.h"
#include "xir_codegen.h"
#include "xir_target.h"
#include "xir_pass.h"
#include "xir_printer.h"
#include "../runtime/value/xchunk.h"
#include "xir_opcode_support.h"
#include "xir_eligibility.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include "../coro/xchannel.h"
#include "../coro/xdeep_copy.h"
#include "../coro/xcoroutine.h"
#include "../coro/xtask.h"
#include "../runtime/xexec_frame.h"
#include "../runtime/xexec_state.h"
#include "../vm/xvm.h"
#include "../vm/xvm_internal.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xexec_state.h"
#include "../runtime/object/xexception.h"
#include "../runtime/xerror_codes.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xmap.h"
#include "../runtime/object/xjson.h"
#include "../runtime/object/xshape.h"
#include "../runtime/object/xrange.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/class/xclass.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/object/xset.h"
#include "../runtime/object/xstringbuilder.h"
#include "../runtime/object/xiterator.h"
#include "../coro/xdeep_copy.h"
#include "../runtime/class/xenum.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/closure/xcell.h"
#include "../runtime/value/xtype_names.h"
#include "../module/xmodule.h"
#include "../runtime/value/xvalue_print.h"
#include "../runtime/xstrbuf.h"
#include "../coro/xworker.h"
#include "../coro/xcoro_pool.h"
#include "xir_jit_debug.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../base/xmalloc.h"
#include "xir_jit_runtime.h"

// Alias: XR_JIT_RESULT(val) → XR_JIT_VAL(val) from xir_jit_runtime.h
#define XR_JIT_RESULT(val)  XR_JIT_VAL(val)

/* ========== Dominant Shape Discovery ========== */

// Scan all protos in the module tree for OP_NEWJSON instructions.
// If all NEWJSON use the same non-dictionary shape, return it.
// Returns NULL if no NEWJSON found, or shapes are ambiguous.
static struct XrShape *find_dominant_shape(XrProto *root) {
    if (!root) return NULL;

    struct XrShape *found = NULL;
    bool ambiguous = false;

    // BFS over proto tree
    XrProto *stack[256];
    int sp = 0;
    stack[sp++] = root;

    while (sp > 0 && !ambiguous) {
        XrProto *p = stack[--sp];
        int code_len = p->code.count;
        for (int i = 0; i < code_len && !ambiguous; i++) {
            XrInstruction inst = PROTO_CODE(p, i);
            if (GET_OPCODE(inst) == OP_NEWJSON) {
                int bx = GETARG_B(inst);
                XrValue shape_val = PROTO_CONST_FAST(p, bx);
                struct XrShape *shape = (struct XrShape *)(intptr_t)XR_TO_INT(shape_val);
                if (!shape) continue;
                if (!found) {
                    found = shape;
                } else if (found != shape) {
                    ambiguous = true;
                }
            }
        }
        // Push children
        uint32_t nchild = PROTO_PROTO_COUNT(p);
        for (uint32_t c = 0; c < nchild && sp < 256; c++) {
            XrProto *child = PROTO_PROTO(p, c);
            if (child) stack[sp++] = child;
        }
    }

    return ambiguous ? NULL : found;
}

/* ========== JIT State Management ========== */

XirJitState *xir_jit_init(XrayIsolate *isolate, int threshold) {
    XR_DCHECK(threshold >= 0, "xir_jit_init: negative threshold");
    XirJitState *jit = (XirJitState *)xr_calloc(1, sizeof(XirJitState));
    if (!jit) return NULL;

    xir_code_alloc_init(&jit->code_alloc);
    jit->isolate = isolate;
    jit->threshold = threshold > 0 ? threshold : 100;
    jit->compiled_count = 0;
    jit->enabled = true;
    jit->verbose = (threshold == 1);  // --jit-force → auto-enable diagnostics

    // Start background compilation thread (disabled in --jit-force mode
    // which needs synchronous compilation for immediate execution)
    if (threshold > 1) {
        XirCompileQueue *q = (XirCompileQueue *)xr_calloc(1, sizeof(XirCompileQueue));
        if (q) {
            xjit_queue_init(q, jit);
            jit->bg_queue = q;
        }
    }

    return jit;
}

void xir_jit_destroy(XirJitState *jit) {
    if (!jit) return;
    // Shutdown background thread before freeing code allocator
    if (jit->bg_queue) {
        xjit_queue_destroy(jit->bg_queue);
        xr_free(jit->bg_queue);
        jit->bg_queue = NULL;
    }
    xir_code_alloc_destroy(&jit->code_alloc);
    if (jit->tfa) {
        tfa_free(jit->tfa);
        xr_free(jit->tfa);
    }
    xr_free(jit);
}

/* ========== Compilation ========== */

// Recompilation threshold: after this many executions at Tier 1,
// recompile with full optimizations (GVN, LICM, etc.)
#define JIT_RECOMPILE_THRESHOLD 200

/*
 * Build shared_protos mapping from enclosing proto.
 *
 * Scans the enclosing (parent) proto's bytecode for CLOSURE+SETSHARED
 * patterns to establish which shared variable index maps to which child
 * proto. This enables CALL_KNOWN optimization for functions accessed
 * via GETSHARED (e.g., module-level functions called from nested scopes).
 *
 * Returns malloc'd array (caller frees) or NULL if no mapping found.
 */
static XrProto **jit_build_shared_protos(XrProto *proto, int *out_nshared) {
    *out_nshared = 0;
    if (!proto->enclosing) return NULL;

    XrProto *parent = proto->enclosing;
    const uint32_t *bc = PROTO_CODE_BASE(parent);
    int nbc = PROTO_CODE_COUNT(parent);
    if (nbc < 2) return NULL;

    // First pass: find max shared index to size the array
    int max_shared = -1;
    for (int i = 0; i < nbc; i++) {
        if (GET_OPCODE(bc[i]) == OP_SETSHARED) {
            int idx = GETARG_Bx(bc[i]) + parent->shared_offset;
            if (idx > max_shared) max_shared = idx;
        }
    }
    if (max_shared < 0) return NULL;

    int nshared = max_shared + 1;
    XrProto **mapping = (XrProto **)xr_calloc(nshared, sizeof(XrProto *));
    if (!mapping) return NULL;

    // Second pass: match CLOSURE R[x] immediately followed by SETSHARED R[x]
    int nproto = PROTO_PROTO_COUNT(parent);
    for (int i = 0; i + 1 < nbc; i++) {
        if (GET_OPCODE(bc[i]) == OP_CLOSURE &&
            GET_OPCODE(bc[i + 1]) == OP_SETSHARED &&
            GETARG_A(bc[i]) == GETARG_A(bc[i + 1])) {
            int proto_idx = GETARG_Bx(bc[i]);
            int shared_idx = GETARG_Bx(bc[i + 1]) + parent->shared_offset;
            if (proto_idx < nproto && shared_idx >= 0 && shared_idx < nshared) {
                mapping[shared_idx] = PROTO_PROTO(parent, proto_idx);
            }
        }
    }

    *out_nshared = nshared;
    return mapping;
}

bool xir_jit_try_compile(XirJitState *jit, XrProto *proto) {
    if (!jit || !jit->enabled || !proto) return false;

    static bool crash_handler_installed = false;
    if (!crash_handler_installed) {
        jit_debug_install_crash_handler();
        jit_guard_page_init_trampoline();
        crash_handler_installed = true;
    }

    // Determine if this is a recompilation (Tier 1 → Tier 2)
    bool is_recompile = false;
    if (proto->jit_entry) {
        // Already at full optimization — nothing to do
        if (proto->jit_opt_level >= XIR_OPT_FULL) return true;

        // Not enough executions for recompilation yet
        if (atomic_load_explicit(&proto->exec_count, memory_order_relaxed) < JIT_RECOMPILE_THRESHOLD) return true;

        is_recompile = true;
    }

    // Background compilation: enqueue and return immediately.
    // The VM continues interpreting; compiled code is picked up at next call
    // when jit_entry_pending becomes non-NULL.
    if (jit->bg_queue && !is_recompile) {
        // Already queued or compiled by bg thread → skip
        if (atomic_load_explicit(&proto->jit_entry_pending, memory_order_acquire))
            return false;

        /* Build a self-contained task snapshot on the main thread so the bg
         * worker never has to race-read mutable proto fields. Every input
         * the pipeline needs (feedback, shared_protos, shape_hint) is
         * captured here by value. */
        XirBgTask task;
        memset(&task, 0, sizeof(task));
        task.proto        = proto;
        task.is_recompile = is_recompile;

        if (proto->type_feedback) {
            // Copy the feedback struct (POD, ~16 bytes); main thread may
            // keep mutating the original via xfb_record_arg afterwards.
            task.feedback_snapshot = *proto->type_feedback;
            task.has_feedback = true;
        }

        /* shared_protos: enables CALL_KNOWN for module-level fn references
         * via GETSHARED.  Truncate to XJIT_BG_SHARED_CAP (rare overflow
         * simply falls back to CALL_C, still correct). */
        int nshared = 0;
        XrProto **tmp = jit_build_shared_protos(proto, &nshared);
        if (tmp) {
            int n = nshared < XJIT_BG_SHARED_CAP ? nshared : XJIT_BG_SHARED_CAP;
            for (int i = 0; i < n; i++) task.shared_protos[i] = tmp[i];
            task.nshared = n;
            xr_free(tmp);
        }

        /* shape_hint: only bother passing it when the proto has a PTR
         * parameter that could actually benefit. */
        if (jit->dominant_shape) {
            for (int i = 0; i < proto->numparams; i++) {
                uint8_t gc = XR_SLOT_ANY;
                if (proto->param_types && i < proto->param_types_count &&
                    proto->param_types[i])
                    gc = xr_type_to_slot_type(proto->param_types[i]);
                if (gc == XR_SLOT_PTR) {
                    task.shape_hint = (struct XrShape *)jit->dominant_shape;
                    break;
                }
            }
        }

        // Set sentinel (0x1) to prevent duplicate queue entries from OSR triggers.
        // bg thread replaces this with the actual XirBgResult* when done.
        atomic_store_explicit(&proto->jit_entry_pending,
                              (void *)(uintptr_t)1, memory_order_release);
        if (xjit_queue_push(jit->bg_queue, &task)) {
            return false;  // enqueued, VM will interpret
        }
        // Queue full → clear sentinel, return false (NEVER fall through
        // to sync while bg thread may be using code_alloc concurrently)
        atomic_store_explicit(&proto->jit_entry_pending, NULL, memory_order_release);
        return false;
    }

    // Run TFA to infer types for untyped params.
    // Only run once per JIT lifetime, on first compilation of an untyped function.
    if (!jit->tfa_ran && !is_recompile &&
        proto->numparams > 0 && !proto->param_types && !proto->type_feedback) {
        if (!jit->tfa) {
            jit->tfa = (TfaState *)xr_calloc(1, sizeof(TfaState));
        }
        if (jit->tfa) {
            tfa_analyze_module(jit->tfa, proto);
            jit->tfa_ran = true;
        }
    }

    // Discover dominant shape from module (once, after TFA or on first compile)
    if (!jit->dominant_shape) {
        // Walk up to module root via TFA summaries, or scan from this proto
        // For now: scan from the proto being compiled (covers its children)
        // and also scan TFA-registered protos if available
        if (jit->tfa && jit->tfa_ran) {
            for (uint32_t i = 0; i < jit->tfa->nsummary && !jit->dominant_shape; i++) {
                struct XrShape *s = find_dominant_shape(jit->tfa->summaries[i].proto);
                if (s) jit->dominant_shape = s;
            }
        }
        if (!jit->dominant_shape) {
            struct XrShape *s = find_dominant_shape(proto);
            if (s) jit->dominant_shape = s;
        }
    }

    // Check eligibility
    if (!is_jit_eligible(proto, jit->verbose)) return false;

    // Eager-compile child protos referenced by OP_CLOSURE instructions.
    // Ensures callback closures (filter/map/reduce callbacks) have jit_entry
    // set when the parent's builder emits CALL_KNOWN, enabling direct
    // JIT-to-JIT calls instead of expensive VM re-entry via xr_vm_call_closure.
    if (!is_recompile) {
        const uint32_t *bc = PROTO_CODE_BASE(proto);
        int nbc = PROTO_CODE_COUNT(proto);
        for (int i = 0; i < nbc; i++) {
            if ((bc[i] & 0xFF) == OP_CLOSURE) {
                uint16_t bx = GETARG_Bx(bc[i]);
                if (bx < PROTO_PROTO_COUNT(proto)) {
                    XrProto *child = PROTO_PROTO(proto, bx);
                    if (child && !child->jit_entry) {
                        xir_jit_try_compile(jit, child);
                    }
                }
            }
        }
    }

    // Build shared_protos mapping from enclosing proto (enables CALL_KNOWN
    // for functions accessed via GETSHARED, e.g. module-level fn calls)
    int nshared = 0;
    XrProto **shared_protos = jit_build_shared_protos(proto, &nshared);

    // Build XIR from bytecode
    // Use shape-guided build if dominant shape is known and proto has PTR params
    struct XrShape *shape_hint = NULL;
    if (jit->dominant_shape) {
        for (int i = 0; i < proto->numparams; i++) {
            uint8_t gc = XR_SLOT_ANY;
            if (proto->param_types && i < proto->param_types_count && proto->param_types[i])
                gc = xr_type_to_slot_type(proto->param_types[i]);
            if (gc == XR_SLOT_PTR) {
                shape_hint = (struct XrShape *)jit->dominant_shape;
                break;
            }
        }
    }
    XirFunc *func = xir_build_from_proto_jit(proto, shared_protos, nshared,
                                              shape_hint, jit->isolate);
    if (!func) {
        xr_log_warning("jit", "builder failed for %s",
                proto->name ? XR_STRING_CHARS(proto->name) : "?");
        xr_free(shared_protos);
        return false;
    }

    // Guard: reject functions with too many vregs for fixed-size arrays
    if (func->nvreg > 512) {
        xr_log_warning("jit", "too many vregs (%u) for %s, skipping",
                func->nvreg,
                proto->name ? XR_STRING_CHARS(proto->name) : "?");
        xir_func_destroy(func);
        xr_free(shared_protos);
        return false;
    }

    // Select optimization level
    // First compile: Tier 1 (basic) for fast startup
    // Recompile: Tier 2 (full) with GVN, LICM, etc.
    XirOptLevel opt = is_recompile ? XIR_OPT_FULL : XIR_OPT_BASIC;
    xir_run_pipeline_ex(func, opt, proto);

    // Generate ARM64 machine code
    XirCodegenResult res = xir_codegen_arm64(func, &jit->code_alloc);
    if (!res.success) {
        xr_log_warning("jit", "codegen failed for %s: %s",
                proto->name ? XR_STRING_CHARS(proto->name) : "?", res.error ? res.error : "unknown");
        xir_func_destroy(func);
        xr_free(shared_protos);
        return false;
    }

    // Store compiled entry point (old code is abandoned in arena)
    proto->jit_entry = res.code;
    proto->jit_fast_entry = (char *)res.code + res.fast_entry_offset * 4;
    proto->jit_resume_entry = res.resume_entry_offset
        ? (char *)res.code + res.resume_entry_offset * 4 : NULL;
    proto->jit_opt_level = (uint8_t)opt;
    // Transfer stack map table ownership to proto (freed on proto destruction)
    if (proto->stack_map) xr_free(proto->stack_map);
    proto->stack_map = res.stack_map;
    if (!is_recompile) jit->compiled_count++;

    // Promote feedback param types to param_types (freeze at compile time).
    // param_types is the authoritative source for JIT entry type guards.
    // Live type_feedback may be mutated by subsequent calls with different types,
    // so we freeze monomorphic feedback types into param_types here.
    if (proto->numparams > 0 && proto->type_feedback && proto->type_feedback->stable) {
        if (!proto->param_types) {
            proto->param_types = (struct XrType **)xr_calloc(
                proto->numparams, sizeof(struct XrType *));
            if (proto->param_types)
                proto->param_types_count = proto->numparams;
        }
        if (proto->param_types) {
            for (int i = 0; i < proto->numparams && i < 8; i++) {
                if (i < proto->param_types_count && !proto->param_types[i] &&
                    i < XFB_MAX_PARAMS && xfb_is_monomorphic(proto->type_feedback->arg_types[i])) {
                    uint8_t st = xfb_to_slot_type(proto->type_feedback->arg_types[i]);
                    proto->param_types[i] = xr_slot_type_to_type(st);
                }
            }
        }
    }

    if (jit->verbose) {
        xr_log_verbose("jit", "%s %s (O%d, %u bytes, %u vregs, %u blocks)",
                is_recompile ? "recompile" : "compile",
                proto->name ? XR_STRING_CHARS(proto->name) : "?",
                (int)opt, res.code_size, func->nvreg, func->nblk);
    }

    // Store runtime deopt table
    if (res.ndeopt > 0) {
        size_t deopt_size = res.ndeopt * sizeof(XirRtDeoptEntry);
        XirRtDeoptEntry *entries = (XirRtDeoptEntry *)xr_malloc(deopt_size);
        if (entries) {
            xr_free(proto->deopt_table);
            memcpy(entries, res.deopt_entries, deopt_size);
            proto->deopt_table = entries;
            proto->ndeopt = res.ndeopt;
        }
    }

    // Store OSR entry points for loop headers
    if (res.nosr > 0) {
        size_t osr_size = res.nosr * sizeof(XirOsrEntry);
        XirOsrEntry *entries = (XirOsrEntry *)xr_malloc(osr_size);
        if (entries) {
            xr_free(proto->osr_entries);  // free old OSR entries on recompile
            memcpy(entries, res.osr_entries, osr_size);
            proto->osr_entries = entries;
            proto->nosr = res.nosr;
        }
    }

    // NOTE: param_types population from feedback is handled in xir_builder_init
    // (line 250-263) which reads proto->type_feedback->arg_types directly.
    // Do NOT call xr_type_new_int/float/bool here — the type pool may be freed.

    // Promote feedback return type to return_type_info if not set
    if (!proto->return_type_info && proto->type_feedback) {
        uint8_t fb_ret = xfb_to_slot_type(proto->type_feedback->return_type);
        if (fb_ret != XR_SLOT_ANY) {
            proto->return_type_info = xr_slot_type_to_type(fb_ret);
        }
    }

    // Fallback: infer return type by scanning XIR RET blocks.
    // Covers --jit-force (threshold=1) where no type feedback exists yet,
    // but the XIR builder assigned typed vregs to return values.
    if (!proto->return_type_info) {
        uint8_t inferred_slot = XR_SLOT_ANY;
        bool first = true;
        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XirBlock *blk2 = func->blocks[bi];
            if (blk2->jmp.type != XIR_JMP_RET || !xir_ref_is_vreg(blk2->jmp.arg))
                continue;
            uint32_t vi = XIR_REF_INDEX(blk2->jmp.arg);
            if (vi >= func->nvreg) continue;
            uint8_t vtype = func->vregs[vi].rep;
            XirType rct = xir_ref_ctype(func, blk2->jmp.arg);
            uint8_t vtag = type_kind_to_vtag(rct.kind);
            // Only infer F64 and BOOL — these are misclassified by ANY heuristic.
            // I64/PTR are NOT inferred: ANY mode already handles int→I64 correctly,
            // and PTR inference may mis-specialize 'any'-typed functions.
            uint8_t slot;
            if (vtype == XR_REP_F64)
                slot = XR_SLOT_F64;
            else if (vtype == XR_REP_I64 && vtag == VTAG_BOOL)
                slot = XR_SLOT_BOOL;  // vtag BOOL
            else
                slot = XR_SLOT_ANY;
            if (first) {
                inferred_slot = slot;
                first = false;
            } else if (inferred_slot != slot) {
                inferred_slot = XR_SLOT_ANY;  // mixed types → keep ANY
                break;
            }
        }
        if (inferred_slot != XR_SLOT_ANY) {
            proto->return_type_info = xr_slot_type_to_type(inferred_slot);
        }
    }

    const char *fname = proto->name ? XR_STRING_CHARS(proto->name) : "?";

    // Register code region for crash diagnostics (always needed)
    jit_debug_register(fname, res.code, res.code_size, res.fast_entry_offset);

#ifndef NDEBUG
    fprintf(stderr, "[JIT] %s %s (%d params, %u bytes, tier %d%s) entry=%p fast=%p\n",
            is_recompile ? "recompiled" : "compiled",
            fname, proto->numparams, res.code_size, opt,
            proto->type_feedback ? ", profile-guided" : "",
            proto->jit_entry, proto->jit_fast_entry);
    jit_debug_dump(fname, res.code, res.code_size, res.fast_entry_offset);
#endif

    xir_func_destroy(func);
    xr_free(shared_protos);
    return true;
}

/* ========== JIT Call Bridge ========== */

/*
 * INTERPRETER / JIT BOUNDARY PROTOCOL
 *
 * Entry (interpreter → JIT):
 *   - Args are complete XrValue (16B each); JIT extracts 8B payload.
 *   - Float args: IEEE754 bits copied as int64 (not converted).
 *   - Type guard: arg tags must match compiled param_types, else bail out.
 *   - After extraction, JIT operates on raw payloads (int64/double/ptr).
 *
 * Exit (JIT → interpreter):
 *   - Normal return: raw int64 payload reconstructed to XrValue via
 *     jit_value_from_tag() using compiled return_type.
 *   - Deopt return: XIR_DEOPT_MARKER signals deopt; coro->jit_ctx->deopt_regs[]
 *     holds saved register state. Recovery via xir_jit_deopt_recover():
 *     each slot rebuilt using deopt_reconstruct(raw, xir_type, xr_tag).
 *     When xr_tag is known (0-15), it is used directly for precise
 *     reconstruction of null/true/false values that share I64 machine type.
 *
 * Field access:
 *   - LOAD_FIELD: loads 8B payload from XrValue field. Tag is NOT loaded;
 *     codegen relies on vreg.xr_tag (compile-time) or runtime inference.
 *   - STORE_FIELD: writes 8B payload + 4B descriptor (tag+heap_type).
 *     Tag comes from vreg.xr_tag when known, else runtime inference.
 *
 * TAGGED values (xr_tag == UNKNOWN):
 *   - Deopt snapshots carry xr_tag from vreg annotation.
 *   - When xr_tag is UNKNOWN at deopt time, recovery infers from machine
 *     type (I64→6, F64→12, PTR/TAGGED→pointer heuristic).
 *   - Future: TAGGED values will use 16B load (wide vreg) to preserve tag.
 */

// Unified calling convention: x0=coro, x1=pointer to raw int64 args array
typedef XrJitResult (*XirJitFn)(intptr_t, int64_t *);

// Forward declarations (defined in Generic Index Operations section)
static inline XrValue jit_value_from_tag(int64_t raw, uint8_t xr_tag);
static inline uint8_t slot_type_to_xr_tag(uint8_t slot_type);

// Verify that an XrValue tag is compatible with the compiled slot type.
// Profile-based compilation assumes monomorphic types; mismatches cause deopt.
static inline bool jit_tag_matches_slot(uint32_t tag, uint8_t slot_type) {
    if (slot_type == XR_SLOT_ANY) return true;
    if (XR_SLOT_IS_INT(slot_type) || slot_type == XR_SLOT_BOOL) {
        // Accept int, bool, and null (null as 0 handled by JIT null-checks)
        return tag == XR_TAG_I64 || tag == XR_TAG_BOOL || tag == XR_TAG_NULL;
    }
    if (XR_SLOT_IS_FLOAT(slot_type)) {
        return tag == XR_TAG_F64;
    }
    if (slot_type == XR_SLOT_PTR) {
        // Accept both PTR and NULL: nullable parameters (e.g. Json fields)
        // pass null as raw 0 which JIT null-checks handle correctly.
        return tag == XR_TAG_PTR || tag == XR_TAG_NULL;
    }
    return false;
}

int xir_jit_call(void *jit_entry, XrCoroutine *coro,
                  XrValue *args, int nargs,
                  struct XrType *return_type_info, XrValue *result) {
    if (!jit_entry || !result) return XIR_JIT_DEOPT;
    // Reset frame stack: must be empty when entering JIT from interpreter.
    // Prevents stale frame pointers from previous deopt or aborted JIT calls.
    coro->jit_ctx->jit_frame_depth = 0;
    // Set stack map for GC scanning (proto->stack_map populated by codegen)
    {
        XrProto *p = (XrProto *)coro->jit_ctx->call_proto;
        coro->jit_ctx->active_stack_map = p ? p->stack_map : NULL;
    }
    coro->jit_ctx->active_safepoint_id = UINT32_MAX;
    coro->jit_ctx->invoke_deopt_id = UINT32_MAX;  // Safe default: no invoke recovery
    coro->jit_ctx->yield_frame_pushed = false;     // clear stale yield state
    // Derive return slot type from return_type_info for reconstruction
    uint8_t return_type = return_type_info ? xr_type_to_slot_type(return_type_info) : XR_SLOT_ANY;

    // Type guard: verify argument tags match compiled specialization.
    // Derives expected types from param_types (frozen at JIT compile time).
    // For numeric union params with monomorphic type_feedback, use the
    // speculated type (I64/F64) instead of ANY for strict matching.
    uint8_t speculated[8];
    {
        XrProto *proto = (XrProto *)coro->jit_ctx->call_proto;
        for (int i = 0; i < 8; i++) speculated[i] = XR_SLOT_ANY;
        if (proto) {
            for (int i = 0; i < nargs && i < 8; i++) {
                uint8_t gc = (proto->param_types && i < proto->param_types_count && proto->param_types[i])
                    ? xr_type_to_slot_type(proto->param_types[i]) : XR_SLOT_ANY;
                // Speculation: narrow ANY → I64/F64 from feedback for
                // numeric unions (int|float) and nullable primitives (int?/float?/bool?)
                if (gc == XR_SLOT_ANY && proto->param_types &&
                    i < proto->param_types_count && proto->param_types[i] &&
                    proto->type_feedback && proto->type_feedback->stable &&
                    i < XFB_MAX_PARAMS) {
                    XrType *st = proto->param_types[i];
                    bool speculatable =
                        value_tag_to_vtag(xr_type_to_xr_tag(st)) == VTAG_NUMERIC ||
                        (st->is_nullable && (st->kind == XR_KIND_INT ||
                         st->kind == XR_KIND_FLOAT || st->kind == XR_KIND_BOOL));
                    if (speculatable) {
                        uint8_t fb = proto->type_feedback->arg_types[i];
                        if (fb == XFB_TYPE_INT || fb == XFB_TYPE_BOOL)
                            gc = XR_SLOT_I64;
                        else if (fb == XFB_TYPE_FLOAT)
                            gc = XR_SLOT_F64;
                    }
                }
                speculated[i] = gc;
                if (!jit_tag_matches_slot(args[i].tag, gc)) {
                    coro->jit_ctx->deopt_id = UINT32_MAX;
                    return XIR_JIT_DEOPT;  // type mismatch
                }
            }
        }
    }

    // Store param tags for nullable primitive null-check in JIT codegen.
    // Allows JIT to distinguish int(0) (tag=I64) from null (tag=NULL).
    for (int i = 0; i < nargs && i < 8; i++)
        coro->jit_ctx->param_tags[i] = (int64_t)args[i].tag;
    // Missing params (default parameters) must have tag=NULL so
    // EQ(param, null) null-checks work correctly in JIT code.
    {
        XrProto *p = (XrProto *)coro->jit_ctx->call_proto;
        int nparams = p ? p->numparams : 0;
        for (int i = nargs; i < nparams && i < 8; i++)
            coro->jit_ctx->param_tags[i] = 0;  // XR_TAG_NULL
    }

    // Build raw args array: extract payload from XrValue
    // Uses speculated types so float params get IEEE754 bit extraction.
    // Zero-init: missing params (beyond nargs) must be null (raw=0).
    int64_t raw_args[8];
    memset(raw_args, 0, sizeof(raw_args));
    for (int i = 0; i < nargs && i < 8; i++) {
        if (XR_SLOT_IS_FLOAT(speculated[i])) {
            // Store f64 bits as int64 (IEEE754 bit pattern)
            memcpy(&raw_args[i], &args[i].f, sizeof(double));
        } else {
            raw_args[i] = args[i].i;
        }
    }

    XrJitResult jr = ((XirJitFn)jit_entry)((intptr_t)coro, raw_args);
    int64_t ret = jr.payload;

    if (ret == XIR_SUSPEND_MARKER) {
        // JIT suspend: coro blocked on channel/await. resume_entry/proto
        // already set by XIR_SUSPEND codegen BEFORE block_helper.
        // Do NOT write to coro/jit_ctx here — another worker may have
        // already resumed this coro (gopark race). Return thread-local
        // signal so VM can handle without touching shared state.
        *result = xr_null();
        return XIR_JIT_SUSPEND;
    }

    // Invalidate JIT frame state: after return, frame is deallocated.
    // Without this, GC between JIT calls would scan stale frame memory
    // using the still-valid active_safepoint_id + jit_frame_sp.
    // Safe here because SUSPEND case returned above (no jit_ctx access).
    coro->jit_ctx->active_safepoint_id = UINT32_MAX;
    coro->jit_ctx->jit_frame_sp = NULL;

    if (ret == XIR_DEOPT_MARKER) {
        // Deopt triggered: registers saved in coro->jit_ctx->deopt_regs[] by stub.
        // If JIT helper set an exception (e.g. division by zero), propagate
        // to VM exception system so try-catch can handle it properly.
        if (coro->jit_ctx->exception) {
            XrValue exc;
            exc.descriptor = 0;
            exc.tag = XR_TAG_PTR;
            exc.ptr = coro->jit_ctx->exception;
            exc.heap_type = (uint16_t)((XrGCHeader *)exc.ptr)->type;
            coro->jit_ctx->exception = NULL;
            xr_vm_add_stacktrace(coro->isolate, exc);
            xr_vm_throw_exception(coro->isolate, exc);
        }
        // Recovery happens in VM via xir_jit_deopt_recover().
        return XIR_JIT_DEOPT;
    }

    // Use tag from XrJitResult.tag (set by JIT epilogue via x1).
    uint8_t tag = (uint8_t)jr.tag;
    uint16_t ht = 0;

    if (tag == XR_RTAG_UNKNOWN) {
        // Polymorphic return: use return_type_info as hint
        *result = jit_value_from_tag(ret, slot_type_to_xr_tag(return_type));
    } else if (tag == XR_TAG_F64) {
        result->descriptor = 0;
        memcpy(&result->f, &ret, sizeof(double));
        result->tag = XR_TAG_F64;
    } else if (tag == XR_TAG_PTR) {
        // PTR return: validate pointer and fill heap_type safely
        result->descriptor = 0;
        result->i = ret;
        if (ret == 0) {
            result->tag = XR_TAG_NULL;
        } else {
            result->tag = XR_TAG_PTR;
            result->heap_type = ht ? ht : (uint16_t)((XrGCHeader *)(void *)ret)->type;
        }
    } else if (tag == XR_TAG_BOOL ||
               (tag == XR_TAG_I64 && return_type == XR_SLOT_BOOL)) {
        // Bool return: payload is already 0/1, tag is XR_TAG_BOOL.
        result->descriptor = 0;
        result->i = ret;
        result->tag = XR_TAG_BOOL;
    } else {
        result->descriptor = 0;
        result->i = ret;
        result->tag = tag;
        result->heap_type = ht;
    }
    return XIR_JIT_OK;
}

/* ========== JIT Resume After Suspend ========== */

int xir_jit_resume(XrCoroutine *coro, XrValue *result) {
    void *resume_entry = coro->jit_resume_entry;
    if (!resume_entry || !result) return XIR_JIT_DEOPT;

    // Restore JIT scratch context from the proto that was suspended
    XrProto *proto = (XrProto *)coro->jit_resume_proto;
    if (!proto) return XIR_JIT_DEOPT;

    coro->jit_ctx->call_proto = proto;
    coro->jit_ctx->jit_frame_depth = 0;
    coro->jit_ctx->active_stack_map = proto->stack_map;
    coro->jit_ctx->active_safepoint_id = UINT32_MAX;
    coro->jit_ctx->invoke_deopt_id = UINT32_MAX;
    coro->jit_ctx->yield_frame_pushed = false;

    // The await result has already been written into
    // coro->jit_suspend_state.result by the waker (xr_jit_await_block
    // inline-resume path or worker resume path).

    // Call the resume entry stub: same calling convention as XirJitFn
    XrJitResult jr = ((XirJitFn)resume_entry)((intptr_t)coro, NULL);
    int64_t ret = jr.payload;

    if (ret == (int64_t)XIR_SUSPEND_MARKER) {
        // Nested suspend: another channel/await block hit during resume.
        // XIR_SUSPEND codegen already re-populated jit_resume_entry/proto.
        // Do NOT write to coro/jit_ctx — gopark race: another worker may
        // have already woken and resumed this coro.
        // Return XIR_JIT_SUSPEND on the stack so caller does not need to
        // read any shared coro fields to decide the outcome.
        return XIR_JIT_SUSPEND;
    }

    // Safe to access jit_ctx: DEOPT/OK paths mean coro is still ours.
    coro->jit_ctx->active_safepoint_id = UINT32_MAX;
    coro->jit_ctx->jit_frame_sp = NULL;

    // Clear resume state (one-shot)
    coro->jit_resume_entry = NULL;
    coro->jit_resume_proto = NULL;

    if (ret == XIR_DEOPT_MARKER) {
        if (coro->jit_ctx->exception) {
            XrValue exc;
            exc.descriptor = 0;
            exc.tag = XR_TAG_PTR;
            exc.ptr = coro->jit_ctx->exception;
            exc.heap_type = (uint16_t)((XrGCHeader *)exc.ptr)->type;
            coro->jit_ctx->exception = NULL;
            xr_vm_add_stacktrace(coro->isolate, exc);
            xr_vm_throw_exception(coro->isolate, exc);
        }
        return XIR_JIT_DEOPT;
    }

    // Reconstruct return value (same logic as xir_jit_call)
    uint8_t tag = (uint8_t)jr.tag;
    if (tag == XR_TAG_F64) {
        result->descriptor = 0;
        memcpy(&result->f, &ret, sizeof(double));
        result->tag = XR_TAG_F64;
    } else if (tag == XR_TAG_PTR) {
        result->descriptor = 0;
        result->i = ret;
        if (ret == 0) {
            result->tag = XR_TAG_NULL;
        } else {
            result->tag = XR_TAG_PTR;
            result->heap_type = (uint16_t)((XrGCHeader *)(void *)ret)->type;
        }
    } else if (tag == XR_TAG_BOOL) {
        result->descriptor = 0;
        result->i = ret;
        result->tag = XR_TAG_BOOL;
    } else {
        result->descriptor = 0;
        result->i = ret;
        result->tag = tag;
    }
    return XIR_JIT_OK;
}

/* ========== Multi-Return Value Reconstruction ========== */

// Reconstruct extra return values from jit_ctx->ret_vals[] into VM registers.
// Called after xir_jit_call() succeeds when nresults > 1.
// results[0] is already filled by xir_jit_call; this fills results[1..ret_count-1].
void xir_jit_read_multi_ret(XrCoroutine *coro, XrValue *results, int nresults) {
    XrJitScratch *ctx = coro->jit_ctx;
    int ret_count = ctx->ret_count;
    if (ret_count <= 1) return;

    int extra = (ret_count - 1 < nresults - 1) ? ret_count - 1 : nresults - 1;
    for (int i = 0; i < extra && i < 7; i++) {
        int64_t raw = ctx->ret_vals[i];
        int64_t tag = ctx->ret_tags[i];

        results[i + 1].descriptor = 0;
        if (tag == XR_TAG_F64) {
            memcpy(&results[i + 1].f, &raw, sizeof(double));
            results[i + 1].tag = XR_TAG_F64;
        } else if (tag == XR_TAG_PTR) {
            results[i + 1].i = raw;
            if (raw == 0) {
                results[i + 1].tag = XR_TAG_NULL;
            } else {
                results[i + 1].tag = XR_TAG_PTR;
                results[i + 1].heap_type = (uint16_t)((XrGCHeader *)(void *)raw)->type;
            }
        } else if (tag == XR_TAG_BOOL) {
            results[i + 1].i = raw;
            results[i + 1].tag = XR_TAG_BOOL;
        } else if (tag != XR_RTAG_UNKNOWN && tag != XR_RTAG_NUMERIC) {
            results[i + 1].i = raw;
            results[i + 1].tag = tag;
        } else {
            // Unknown/Numeric tag: raw==0 defaults to I64(0), NOT NULL.
            // Null values must carry XR_TAG_NULL from VTAG_NULL.
            results[i + 1].i = raw;
            if (raw != 0 && (raw & 0x7) == 0 && (uint64_t)raw > 0x10000) {
                results[i + 1].tag = XR_TAG_PTR;
                results[i + 1].heap_type = (uint16_t)((XrGCHeader *)(void *)raw)->type;
            } else {
                results[i + 1].tag = XR_TAG_I64;
            }
        }
    }
}

/* ========== Mid-Function Deopt Recovery ========== */

// Reconstruct XrValue from raw int64 payload, XIR type tag, and semantic xr_tag.
// When xr_tag is known (0-15), it is used directly for precise reconstruction.
// When xr_tag is UNKNOWN (0xFF), falls back to XIR machine type inference.
static inline XrValue deopt_reconstruct(int64_t raw, uint8_t xir_type, uint8_t xr_tag) {
    XrValue v;
    v.descriptor = 0;

    // Numeric meta-tag: treat as I64 for deopt (codegen uses F64 internally,
    // but deopt values are raw int64 payloads from the interpreter stack).
    if (xr_tag == XR_RTAG_NUMERIC) {
        xr_tag = XR_RTAG_UNKNOWN;
    }

    // Fast path: precise tag from compile-time annotation.
    // Skip for TAGGED types and for I64 tags where raw looks like a heap ptr.
    // Bytecode registers can be reused across loops for different types
    // (e.g., R[7]=counter in spawn-loop, R[7]=coro in await-loop), so the
    // compile-time tag may not match the runtime value.
    if (xr_tag != XR_RTAG_UNKNOWN && xir_type != XR_REP_TAGGED &&
        !(xr_tag == XR_TAG_I64 && raw != 0 && (raw & 0x7) == 0 &&
          (uint64_t)raw > 0x10000)) {
        v.tag = xr_tag;
        if (xr_tag == XR_TAG_F64) {
            memcpy(&v.f, &raw, sizeof(double));
        } else {
            v.i = raw;
        }
        // Set heap_type for PTR values
        if (xr_tag == XR_TAG_PTR && raw != 0 && (raw & 0x7) == 0) {
            XrGCHeader *gc = (XrGCHeader *)(intptr_t)raw;
            v.heap_type = (uint16_t)gc->type;
        }
        return v;
    }

    // Slow path: infer from machine type
    switch (xir_type) {
    case XR_REP_F64:
        memcpy(&v.f, &raw, sizeof(double));
        v.tag = XR_TAG_F64;
        break;
    case XR_REP_PTR:
        v.i = raw;
        if (raw == 0) {
            v.tag = XR_TAG_NULL;
        } else if ((raw & 0x7) == 0) {
            v.tag = XR_TAG_PTR;
            XrGCHeader *gc = (XrGCHeader *)(intptr_t)raw;
            v.heap_type = (uint16_t)gc->type;
        } else {
            v.tag = XR_TAG_I64;
        }
        break;
    case XR_REP_TAGGED:
    tagged_heuristic:
        // TAGGED rep: raw==0 defaults to I64(0), NOT NULL.
        // Null values should have precise xr_tag=0 from VTAG_NULL,
        // which takes the fast path above. The slow path here is only
        // reached for truly unknown values where int(0) is more likely.
        v.i = raw;
        if (raw != 0 && (raw & 0x7) == 0 && (uint64_t)raw > 0x10000) {
            v.tag = XR_TAG_PTR;
            XrGCHeader *gc = (XrGCHeader *)(intptr_t)raw;
            v.heap_type = (uint16_t)gc->type;
        } else {
            v.tag = XR_TAG_I64;
        }
        break;
    default:
        // I64 slots may hold heap pointers at runtime when the bytecode
        // register is reused for different types across loops (e.g.,
        // R[7]=counter in spawn-loop, R[7]=coro in await-loop).
        // If the raw value looks like a heap pointer (8-byte aligned,
        // above typical integer range), use the tagged heuristic.
        v.i = raw;
        if (raw != 0 && (raw & 0x7) == 0 && (uint64_t)raw > 0x10000) {
            goto tagged_heuristic;
        }
        v.tag = XR_TAG_I64;
        break;
    }
    return v;
}

int32_t xir_jit_deopt_recover(XrCoroutine *coro, XrValue *frame, int maxstack) {
    XrProto *proto = (XrProto *)coro->jit_ctx->call_proto;
    if (!proto || !proto->deopt_table) return -1;

    uint32_t did = coro->jit_ctx->deopt_id;
    // CALL_C invoke recovery: UINT32_MAX signals that the real deopt_id
    // is stored in invoke_deopt_id (avoids deopt_id=0 vs CBNZ conflict).
    if (did == UINT32_MAX) {
        did = coro->jit_ctx->invoke_deopt_id;
    }
    if (did >= proto->ndeopt) return -1;

    XirRtDeoptEntry *entry = &((XirRtDeoptEntry *)proto->deopt_table)[did];

    for (uint16_t i = 0; i < entry->nslots; i++) {
        XirRtDeoptSlot *s = &entry->slots[i];
        int bc = s->bc_slot;
        if (bc < 0 || bc >= maxstack) continue;

        int64_t raw = 0;
        switch (s->loc_kind) {
        case DEOPT_LOC_REG:
            if (s->loc.phys_reg < 29)
                raw = coro->jit_ctx->deopt_regs[s->loc.phys_reg];
            break;
        case DEOPT_LOC_FP_REG:
            if (s->loc.phys_reg < 16)
                raw = coro->jit_ctx->deopt_fp_regs[s->loc.phys_reg];
            break;
        case DEOPT_LOC_SPILL: {
            // Read from saved frame pointer + offset.
            // NOTE: stack frame is destroyed after epilogue; spill data may
            // be stale. This path is rare (most values are in registers).
            int64_t *fp = (int64_t *)coro->jit_ctx->deopt_spill_base;
            if (fp) {
                int16_t off = s->loc.spill_offset;
                raw = *(int64_t *)((char *)fp + off);
            }
            break;
        }
        case DEOPT_LOC_CONST_I64:
            raw = s->loc.const_i64;
            break;
        case DEOPT_LOC_CONST_F64:
            memcpy(&raw, &s->loc.const_f64, sizeof(double));
            break;
        case DEOPT_LOC_CONST_PTR:
            raw = (int64_t)(intptr_t)s->loc.const_ptr;
            break;
        default:
            continue;
        }

        frame[bc] = deopt_reconstruct(raw, s->type, s->xr_tag);
    }

    return (int32_t)entry->bc_pc;
}

/* ========== JIT→JIT Self-Call (CALLSELF) ========== */

// Called from JIT code via CALL_C to perform recursive self-call.
// Args are pre-stored in coro->jit_ctx->call_args by JIT code.
// Proto pointer is in coro->jit_ctx->call_proto (set by JIT prologue-equivalent).
// extra_arg is unused (0).
XrJitResult xr_jit_call_self(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrProto *proto = (XrProto *)coro->jit_ctx->call_proto;
    if (!proto || !proto->jit_entry) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    // Recursive JIT call: same calling convention (coro, args_ptr)
    return ((XirJitFn)proto->jit_entry)((intptr_t)coro, coro->jit_ctx->call_args);
}

/* ========== JIT Yield Frame Pre-push ========== */

/*
 * Read a GP register value from CALL_C stub saved area or JIT frame.
 *
 * During CALL_C, caller-saved regs (x1-x15) are saved to the C stack
 * at safepoint_saved_sp. Callee-saved regs (x20-x27) are on the JIT
 * frame at jit_frame_sp (saved by prologue).
 *
 * CALL_C stub layout (safepoint_saved_sp):
 *   +0: x1,x2  +16: x3,x4  +32: x5,x6  +48: x7,x8
 *   +64: x9,x10  +80: x11,x12  +96: x13,x14  +112: x15,LR
 *
 * JIT frame layout (jit_frame_sp):
 *   +0: FP,LR  +16: x19,x20  +32: x21,x22  +48: x23,x24
 *   +64: x25,x26  +80: x27,x28
 */
static inline int64_t jit_read_saved_gp(XrJitScratch *ctx, uint8_t phys_reg) {
    int64_t *saved_sp = (int64_t *)ctx->safepoint_saved_sp;
    int64_t *frame_sp = (int64_t *)ctx->jit_frame_sp;
    if (phys_reg >= 1 && phys_reg <= 15 && saved_sp)
        return saved_sp[phys_reg - 1];
    if (phys_reg >= 19 && phys_reg <= 27 && frame_sp)
        return frame_sp[2 + (phys_reg - 19)];
    return 0;
}

/*
 * Read an FP register value from CALL_C stub saved area or JIT frame.
 *
 * CALL_C stub: d0-d7 at safepoint_saved_sp + 128 + d*8
 * JIT frame:   d8-d15 at jit_frame_sp + 96 + (d-8)*8
 */
static inline int64_t jit_read_saved_fp(XrJitScratch *ctx, uint8_t phys_reg) {
    int64_t *saved_sp = (int64_t *)ctx->safepoint_saved_sp;
    int64_t *frame_sp = (int64_t *)ctx->jit_frame_sp;
    if (phys_reg <= 7 && saved_sp)
        return saved_sp[16 + phys_reg];
    if (phys_reg >= 8 && phys_reg <= 15 && frame_sp)
        return frame_sp[12 + (phys_reg - 8)];
    return 0;
}

/*
 * Pre-push an interpreter frame and populate the VM stack from JIT
 * register state. Used by JIT yield: after try-mode returns
 * WOULD_BLOCK, this function reconstructs the interpreter state so the
 * yieldable can be called in normal mode with a proper frame for
 * yield_setup_frame to operate on.
 *
 * Returns the bc_pc of the OP_INVOKE instruction, or -1 on failure.
 */
static int32_t jit_prepush_yield_frame(XrCoroutine *coro, uint32_t deopt_id) {
    XrJitScratch *jctx = coro->jit_ctx;
    XrProto *proto = (XrProto *)jctx->call_proto;
    if (!proto || !proto->deopt_table) return -1;
    if (deopt_id >= proto->ndeopt) return -1;

    XirRtDeoptEntry *entry = &((XirRtDeoptEntry *)proto->deopt_table)[deopt_id];
    XrayIsolate *isolate = coro->isolate;
    XrVMContext *vm = &isolate->vm_ctx;
    int32_t base_off = jctx->call_base_offset;
    int maxstack = proto->maxstacksize;

    // Bounds checks
    if (base_off + maxstack > vm->stack_capacity) return -1;
    if (vm->frame_count >= vm->frame_capacity) return -1;

    // Populate VM stack slots from saved register areas
    XrValue *frame_base = vm->stack + base_off;
    for (uint16_t i = 0; i < entry->nslots; i++) {
        XirRtDeoptSlot *s = &entry->slots[i];
        int bc = s->bc_slot;
        if (bc < 0 || bc >= maxstack) continue;

        int64_t raw = 0;
        switch (s->loc_kind) {
        case DEOPT_LOC_REG:
            raw = jit_read_saved_gp(jctx, s->loc.phys_reg);
            break;
        case DEOPT_LOC_FP_REG:
            raw = jit_read_saved_fp(jctx, s->loc.phys_reg);
            break;
        case DEOPT_LOC_SPILL: {
            int64_t *fp = (int64_t *)jctx->jit_frame_sp;
            if (fp) raw = *(int64_t *)((char *)fp + s->loc.spill_offset);
            break;
        }
        case DEOPT_LOC_CONST_I64:
            raw = s->loc.const_i64;
            break;
        case DEOPT_LOC_CONST_F64:
            memcpy(&raw, &s->loc.const_f64, sizeof(double));
            break;
        case DEOPT_LOC_CONST_PTR:
            raw = (int64_t)(intptr_t)s->loc.const_ptr;
            break;
        default:
            continue;
        }
        frame_base[bc] = deopt_reconstruct(raw, s->type, s->xr_tag);
    }

    // Pre-push interpreter frame
    XrBcCallFrame *nf = &vm->frames[vm->frame_count++];
    nf->closure = (XrClosure *)jctx->call_closure;
    nf->pc = PROTO_CODE_BASE(proto) + entry->bc_pc + 1;  // next instruction
    nf->base_offset = base_off;
    nf->result_offset = 0;
    nf->flags = 0;
    nf->call_status = 0;
    nf->u.c.has_cfunc_result = false;
    nf->u.c.cfunc_result = xr_null();

    // Decode result_slot from the OP_INVOKE instruction at bc_pc
    XrInstruction invoke_inst = PROTO_CODE_BASE(proto)[entry->bc_pc];
    nf->u.c.result_slot = (int16_t)GETARG_A(invoke_inst);

    return (int32_t)entry->bc_pc;
}

/* ========== Method Invocation Bridge ========== */

// Called from JIT code via CALL_C for OP_INVOKE.
// Type hints for IC-guided fast dispatch in xr_jit_invoke_method.
// Encoded in bits 8-15 of the encoded parameter.
#define JIT_TYPE_HINT_NONE     0
#define JIT_TYPE_HINT_INT      1
#define JIT_TYPE_HINT_FLOAT    2
#define JIT_TYPE_HINT_BOOL     3
#define JIT_TYPE_HINT_STRING   4
#define JIT_TYPE_HINT_ARRAY    5
#define JIT_TYPE_HINT_MAP      6
#define JIT_TYPE_HINT_SET      7
#define JIT_TYPE_HINT_JSON     8
#define JIT_TYPE_HINT_INSTANCE 9
#define JIT_TYPE_HINT_ITERATOR 10
#define JIT_TYPE_HINT_CLASS    11

// Classify receiver XrValue into JIT_TYPE_HINT_* constant.
// Used on IC miss (no hint or hint mismatch) to determine dispatch target.
static int jit_classify_receiver(XrValue receiver) {
    if (XR_IS_INT(receiver))    return JIT_TYPE_HINT_INT;
    if (XR_IS_FLOAT(receiver))  return JIT_TYPE_HINT_FLOAT;
    if (XR_IS_BOOL(receiver))   return JIT_TYPE_HINT_BOOL;
    if (XR_IS_STRING(receiver)) return JIT_TYPE_HINT_STRING;
    if (XR_IS_ARRAY(receiver))  return JIT_TYPE_HINT_ARRAY;
    if (XR_IS_MAP(receiver))    return JIT_TYPE_HINT_MAP;
    if (XR_IS_SET(receiver))    return JIT_TYPE_HINT_SET;
    if (xr_value_is_json(receiver))     return JIT_TYPE_HINT_JSON;
    if (xr_value_is_instance(receiver)) return JIT_TYPE_HINT_INSTANCE;
    if (XR_IS_ITERATOR(receiver))        return JIT_TYPE_HINT_ITERATOR;
    if (xr_value_is_class(receiver))     return JIT_TYPE_HINT_CLASS;
    return JIT_TYPE_HINT_NONE;
}

// Build call_args array with receiver as this[0] for instance/class method calls.
static inline void jit_build_this_args(XrValue *call_args, XrValue receiver,
                                       XrValue *args, int nargs) {
    call_args[0] = receiver;
    for (int i = 0; i < nargs && i < 8; i++)
        call_args[1 + i] = args[i];
}

/*
 * Decode xr_tag from a 64-bit tag bitmap stored in call_args[15].
 * Each slot occupies 4 bits: slot i is at bits[(i+1)*4-1 : i*4].
 * 0-7: XR_TAG_* directly; 0xF: UNKNOWN.
 */
static inline uint8_t jit_bitmap_tag(int64_t bitmap, int slot) {
    uint8_t nibble = (uint8_t)((bitmap >> (slot * 4)) & 0xF);
    if (nibble <= 7) return nibble;
    return XR_RTAG_UNKNOWN;
}

// jit_call_args[0] = receiver (raw XrValue.i), jit_call_args[1..n] = args.
// jit_call_args[15] = tag bitmap (4 bits per slot, set by builder).
// encoded = (method_symbol << 32) | (deopt_id << 16) | (type_hint << 8) | nargs.
// deopt_id: builder-provided deopt snapshot index for yieldable recovery.
// type_hint: IC-derived receiver type (0=none, classify at runtime).
XrJitResult xr_jit_invoke_method(XrCoroutine *coro, int64_t encoded) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate) return XR_JIT_NULL();

    int method_symbol = (int)(encoded >> 32);
    int deopt_id = (int)((encoded >> 16) & 0xFFFF);
    int type_hint = (int)((encoded >> 8) & 0xFF);
    int nargs = (int)(encoded & 0xFF);

    // Read tag bitmap from call_args[15] (written by builder)
    int64_t tag_bitmap = coro->jit_ctx->call_args[15];

    // Reconstruct receiver XrValue using bitmap tag (ground truth from builder).
    // Previously used type_hint for INT/FLOAT/BOOL cases, but type_hint can be
    // wrong when slot_rep doesn't match runtime type (e.g., upvalue-loaded array
    // with I64 rep → type_hint=INT). Bitmap tag is always set from vreg xr_tag
    // and correctly identifies the actual value type.
    uint8_t recv_tag = jit_bitmap_tag(tag_bitmap, 0);
    if (recv_tag == XR_RTAG_UNKNOWN)
        recv_tag = coro->jit_ctx->call_arg_tags[0];
    XrValue receiver = jit_value_from_tag(coro->jit_ctx->call_args[0], recv_tag);
    // Build args array: bitmap tag preferred, fallback to call_arg_tags[]
    // (dynamic-patched from slot_runtime_tags at each call site)
    XrValue args[16];
    for (int i = 0; i < nargs && i < 15; i++) {
        int64_t raw = coro->jit_ctx->call_args[1 + i];
        uint8_t tag = jit_bitmap_tag(tag_bitmap, 1 + i);
        if (tag == XR_RTAG_UNKNOWN)
            tag = coro->jit_ctx->call_arg_tags[1 + i];
        args[i] = jit_value_from_tag(raw, tag);
    }

    // Determine receiver type for dispatch.
    // Always verify hint against actual receiver — static type info can be
    // stale when bytecode registers are reused across different code regions.
    int recv_type = type_hint;
    if (recv_type == JIT_TYPE_HINT_NONE) {
        recv_type = jit_classify_receiver(receiver);
    } else {
        // Validate PTR-based hints against actual GC type
        int actual = jit_classify_receiver(receiver);
        if (actual != recv_type)
            recv_type = actual;
    }

    // Unified dispatch — single switch covers all types
    XrValue result = XR_NULL_VAL;
    switch (recv_type) {
    case JIT_TYPE_HINT_INT:
        result = int_method_call_by_symbol(isolate, XR_TO_INT(receiver),
                                           method_symbol, args, nargs);
        break;
    case JIT_TYPE_HINT_FLOAT:
        result = float_method_call_by_symbol(isolate, XR_TO_FLOAT(receiver),
                                             method_symbol, args, nargs);
        break;
    case JIT_TYPE_HINT_BOOL:
        result = bool_method_call_by_symbol(isolate, XR_TO_BOOL(receiver),
                                            method_symbol);
        break;
    case JIT_TYPE_HINT_STRING: {
        XrString *str = (XrString *)XR_TO_PTR(receiver);
        result = string_method_call_by_symbol(isolate, str, method_symbol,
                                              args, nargs);
        break;
    }
    case JIT_TYPE_HINT_ARRAY: {
        XrArray *arr = XR_TO_ARRAY(receiver);
        result = array_method_call_by_symbol(isolate, arr, method_symbol,
                                             args, nargs);
        break;
    }
    case JIT_TYPE_HINT_MAP: {
        XrMap *map = XR_TO_MAP(receiver);
        result = map_method_call_by_symbol(isolate, map, method_symbol,
                                           args, nargs);
        break;
    }
    case JIT_TYPE_HINT_SET: {
        XrSet *set = XR_TO_SET(receiver);
        result = set_method_call_by_symbol(isolate, set, method_symbol,
                                           args, nargs);
        break;
    }
    case JIT_TYPE_HINT_JSON: {
        XrJson *json = xr_value_to_json(receiver);
        result = json_method_call_by_symbol(isolate, json, method_symbol,
                                            args, nargs);
        break;
    }
    case JIT_TYPE_HINT_ITERATOR: {
        XrIterator *iter = xr_value_to_iterator(receiver);
        if (iter) {
            if (method_symbol == SYMBOL_HASNEXT) {
                result = xr_bool(xr_iterator_has_next(iter));
            } else if (method_symbol == SYMBOL_NEXT) {
                result = xr_iterator_next(iter);
            }
        }
        break;
    }
    case JIT_TYPE_HINT_INSTANCE: {
        XrInstance *inst = xr_value_to_instance(receiver);
        if (!inst) {
            // Fallback: try direct cast if receiver is a valid pointer
            if (XR_IS_PTR(receiver) && receiver.ptr) {
                XrGCHeader *hdr = (XrGCHeader *)receiver.ptr;
                if (hdr->type == XR_TINSTANCE)
                    inst = (XrInstance *)receiver.ptr;
            }
            if (!inst) break;
        }
        XrMethod *method = xr_class_lookup_method(inst->klass, method_symbol);
        if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
            XrValue call_args[9];
            jit_build_this_args(call_args, receiver, args, nargs);
            result = method->as.primitive(isolate, call_args, nargs + 1);
        } else if (method && method->type == XMETHOD_CLOSURE && method->as.closure) {
            XrValue call_args[9];
            jit_build_this_args(call_args, receiver, args, nargs);
            bool saved = xr_isolate_get_suppress_exception_print(isolate);
            xr_isolate_set_suppress_exception_print(isolate, true);
            result = xr_vm_call_closure(isolate, method->as.closure, call_args, nargs + 1);
            xr_isolate_set_suppress_exception_print(isolate, saved);
            if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
                coro->jit_ctx->exception = (void *)coro->vm_ctx.current_exception.ptr;
                coro->vm_ctx.current_exception = XR_NULL_VAL;
                return XR_JIT_NULL();
            }
        }
        break;
    }
    case JIT_TYPE_HINT_CLASS: {
        // Constructor call: new ClassName(args...)
        XrClass *cls = xr_value_to_class(receiver);
        if (cls) {
            XrInstance *inst = xr_instance_new(isolate, cls);
            if (!inst) break;
            XrValue inst_val = xr_value_from_instance(inst);
            XrMethod *ctor = xr_class_lookup_method(cls, method_symbol);
            if (ctor && ctor->type == XMETHOD_CLOSURE && ctor->as.closure) {
                // Call constructor with instance as this
                XrValue call_args[9];
                call_args[0] = inst_val;
                for (int i = 0; i < nargs && i < 8; i++)
                    call_args[1 + i] = args[i];
                bool saved = xr_isolate_get_suppress_exception_print(isolate);
                xr_isolate_set_suppress_exception_print(isolate, true);
                xr_vm_call_closure(isolate, ctor->as.closure, call_args, nargs + 1);
                xr_isolate_set_suppress_exception_print(isolate, saved);
                if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
                    coro->jit_ctx->exception = (void *)coro->vm_ctx.current_exception.ptr;
                    coro->vm_ctx.current_exception = XR_NULL_VAL;
                    return XR_JIT_NULL();
                }
            } else if (ctor && ctor->type == XMETHOD_PRIMITIVE && ctor->as.primitive) {
                result = ctor->as.primitive(isolate, args, nargs);
                break;
            }
            // Return instance (constructor modifies it in-place)
            result = inst_val;
        }
        break;
    }
    default: {
        // Generic PTR fallback: try native type class lookup
        if (XR_IS_PTR(receiver) && receiver.ptr) {
            XrGCHeader *hdr = (XrGCHeader *)receiver.ptr;
            // Module receiver: look up exported function and call it
            if (hdr->type == XR_TMODULE) {
                XrModule *mod = (XrModule *)receiver.ptr;
                XrValue export_val = xr_module_get_sym(mod, method_symbol);
                // Use already-reconstructed args[] (bitmap-tagged, precise)
                if (XR_IS_PTR(export_val) && export_val.ptr) {
                    XrGCHeader *fn_hdr = (XrGCHeader *)export_val.ptr;
                    if (fn_hdr->type == XR_TCFUNCTION) {
                        XrCFunction *cfunc = (XrCFunction *)export_val.ptr;
                        if (cfunc->is_yieldable) {
                            /* Phase 0: try-mode fast path. If IO is ready,
                             * completes inline (zero deopt). */
                            coro->jit_try_mode = true;
                            XrValue try_result;
                            XrCFuncResult status = cfunc->as.yieldable(
                                isolate, args, nargs + 1, &try_result);
                            coro->jit_try_mode = false;

                            if (status == XR_CFUNC_DONE) {
                                return (XrJitResult){ try_result.i, 0 };
                            }

                            /* Phase 2: WOULD_BLOCK — pre-push interpreter
                             * frame and call yieldable in normal mode.
                             * Avoids deopt + interpreter re-execution of
                             * OP_INVOKE. If pre-push fails, fall back to
                             * Phase 0+1 deopt path. */
                            if (jit_prepush_yield_frame(coro, (uint32_t)deopt_id) >= 0) {
                                XrValue normal_result;
                                XrCFuncResult st2 = cfunc->as.yieldable(
                                    isolate, args, nargs + 1, &normal_result);

                                if (st2 == XR_CFUNC_DONE) {
                                    /* IO became ready between try-mode and
                                     * normal call. Pop the pre-pushed frame
                                     * and return result to JIT. */
                                    isolate->vm_ctx.frame_count--;
                                    return (XrJitResult){ normal_result.i, 0 };
                                }
                                /* BLOCKED/YIELD: yield_setup_frame has set up
                                 * continuation on the pre-pushed frame. Signal
                                 * VM to skip deopt recovery and return the
                                 * appropriate result directly. */
                                coro->jit_ctx->yield_frame_pushed = true;
                                coro->jit_ctx->yield_vm_result = (uint8_t)st2;
                            }

                            /* Trigger deopt exit (deopt stub tears down JIT
                             * frame). VM checks yield_frame_pushed to decide
                             * whether to do normal deopt recovery or return
                             * BLOCKED/YIELD immediately. */
                            coro->jit_ctx->invoke_deopt_id = (uint32_t)deopt_id;
                            coro->jit_ctx->deopt_id = UINT32_MAX;
                            return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
                        }
                        result = cfunc->as.func(isolate, args, nargs);
                    } else if (fn_hdr->type == XR_TFUNCTION) {
                        // Script closure: call via VM
                        XrClosure *fn = (XrClosure *)export_val.ptr;
                        bool saved = xr_isolate_get_suppress_exception_print(isolate);
                        xr_isolate_set_suppress_exception_print(isolate, true);
                        result = xr_vm_call_closure(isolate, fn, args, nargs);
                        xr_isolate_set_suppress_exception_print(isolate, saved);
                        if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
                            coro->jit_ctx->exception = (void *)coro->vm_ctx.current_exception.ptr;
                            coro->vm_ctx.current_exception = XR_NULL_VAL;
                            return XR_JIT_NULL();
                        }
                    }
                }
                break;
            }
            XrClass *klass = xr_value_get_class(isolate, receiver);
            if (klass) {
                XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                    XrValue call_args[9];
                    jit_build_this_args(call_args, receiver, args, nargs);
                    result = method->as.primitive(isolate, call_args, nargs + 1);
                }
            }
        }
        break;
    }
    }

    return XR_JIT_RESULT(result);
}

/* ========== Shared Variable Access ========== */

// Load a shared (global) variable.
// extra_arg = absolute shared index (Bx + shared_offset, pre-computed by builder).
XrJitResult xr_jit_get_shared(XrCoroutine *coro, int64_t shared_index) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate) return XR_JIT_NULL();
    XrValue val = xr_shared_array_get(&xr_isolate_get_vm_state(isolate)->shared, (int)shared_index);
    return XR_JIT_RESULT(val);
}

// Store a shared (global) variable.
// extra_arg = (val_bc_slot<<24) | (val_slot_type<<16) | shared_index
// The value to store is pre-written to jit_call_args[0] by JIT code.
XrJitResult xr_jit_set_shared(XrCoroutine *coro, int64_t extra_arg) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate) return XR_JIT_OK();
    int shared_idx = (int)(extra_arg & 0xFFFF);
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                      coro->jit_ctx->call_arg_tags[0]);
    xr_shared_array_set(&xr_isolate_get_vm_state(isolate)->shared, shared_idx, val);
    return XR_JIT_OK();
}

/* ========== JIT Exception Handling ========== */

// Called from JIT code (via CALL_C) when OP_THROW is executed.
// Sets coro->jit_ctx->exception so JIT can branch to catch handler.
// extra_arg = raw exception value (XrValue.i payload, tag=PTR assumed).
XrJitResult xr_jit_throw(XrCoroutine *coro, int64_t exception_raw) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate) {
        coro->jit_ctx->exception = NULL;
        return XR_JIT_OK();
    }

    // Reconstruct XrValue from raw payload
    XrValue exception;
    exception.descriptor = 0;
    exception.tag = XR_TAG_PTR;
    exception.i = exception_raw;

    // Auto-wrap non-exception values
    if (!xr_is_exception(exception)) {
        exception = xr_exception_from_value(isolate, exception);
    }

    // Store exception pointer for JIT catch handler
    coro->jit_ctx->exception = (void *)exception.ptr;
    return XR_JIT_OK();
}

/* ========== Generic Index Operations ========== */

// Convert XR_SLOT_* to xr_tag for callers that use slot_type
// (e.g. uvinfo.slot_type, xr_type_to_slot_type(), return_type from VM).
static inline uint8_t slot_type_to_xr_tag(uint8_t slot_type) {
    switch (slot_type) {
    case XR_SLOT_I64:  return XR_TAG_I64;
    case XR_SLOT_F64:  return XR_TAG_F64;
    case XR_SLOT_PTR:  return XR_TAG_PTR;
    case XR_SLOT_BOOL: return XR_TAG_BOOL;
    default:           return XR_RTAG_UNKNOWN;
    }
}

// Reconstruct XrValue from raw payload + xr_tag.
// When xr_tag is known (0-15), reconstruction is exact with zero heuristics.
// UNKNOWN (0xFF) defaults to I64 — no address-range guessing.
// All CALL_C helpers use call_arg_tags[] for precise tags; invoke_method
// uses call_arg_tags[] fallback when bitmap tag is UNKNOWN.
static inline XrValue jit_value_from_tag(int64_t raw, uint8_t xr_tag) {
    XrValue v;
    v.descriptor = 0;
    switch (xr_tag) {
    case XR_TAG_F64:
        memcpy(&v.f, &raw, sizeof(double));
        v.tag = XR_TAG_F64;
        break;
    case XR_TAG_NULL:
        v.tag = XR_TAG_NULL;
        v.i = 0;
        break;
    case XR_TAG_BOOL:
        v.i = raw;
        v.tag = XR_TAG_BOOL;
        break;
    case XR_TAG_PTR:
        // Trust the type system for well-formed pointers.
        // Minimal safety: reject null and clearly-invalid low addresses.
        // Low addresses (< 4096) are never valid heap pointers on any OS.
        if (raw == 0) {
            v.tag = XR_TAG_NULL;
            v.i = 0;
        } else if ((uint64_t)raw >= 0x1000 && (raw & 0x7) == 0) {
            v.ptr = (void *)raw;
            v.tag = XR_TAG_PTR;
            v.heap_type = (uint16_t)((XrGCHeader *)v.ptr)->type;
        } else {
            // Compile-time type claimed PTR but raw is not a valid pointer.
            // This indicates a type inference gap (e.g., Array<any> element).
            // Treat as integer to avoid crash; the caller's logic handles
            // the mismatch via runtime tag checks.
            v.i = raw;
            v.tag = XR_TAG_I64;
        }
        break;
    case XR_RTAG_NUMERIC:
        // Numeric union (int|float): codegen handles numeric with inline
        // tag-check inside JIT; at CALL_C boundaries we default to I64.
        // FALLTHROUGH
    case XR_RTAG_UNKNOWN:
        // All CALL_C helpers now use call_arg_tags[] with precise tags
        // (populated by codegen: static for known types, dynamic from
        // slot_runtime_tags[bc_slot] for 'any' types).
        // No address-range pointer guessing — default to I64.
        v.i = raw;
        v.tag = XR_TAG_I64;
        break;
    default:
        // I64 and all integer variants (I8..U64)
        v.i = raw;
        v.tag = xr_tag;
        break;
    }
    return v;
}

// Called from JIT via CALL_C for OP_GETPROP (shape-miss or non-Json fallback).
// jit_call_args[0] = obj raw (ptr), extra_arg = global SymbolId
XrJitResult xr_jit_getprop(XrCoroutine *coro, int64_t symbol_id) {
    XrValue obj;
    obj.ptr = (void *)coro->jit_ctx->call_args[0];
    obj.tag = XR_TAG_PTR;

    SymbolId sym = (SymbolId)(uint32_t)symbol_id;

    if (!obj.ptr || (uintptr_t)obj.ptr < 0x1000) return XR_JIT_NULL();
    int heap_type = XR_GC_GET_TYPE((XrGCHeader *)obj.ptr);

    // String properties
    if (heap_type == XR_TSTRING) {
        XrString *str = (XrString *)obj.ptr;
        if (sym == SYMBOL_LENGTH)      return (XrJitResult){ (int64_t)xr_string_char_length(str), XR_TAG_I64 };
        if (sym == SYMBOL_BYTE_LENGTH) return (XrJitResult){ (int64_t)str->length, XR_TAG_I64 };
        if (sym == SYMBOL_IS_EMPTY)    return (XrJitResult){ str->length == 0 ? 1 : 0, XR_TAG_I64 };
    }

    // Array / ArraySlice properties
    if (heap_type == XR_TARRAY || heap_type == XR_TARRAY_SLICE) {
        XrArray *arr = (XrArray *)obj.ptr;
        if (sym == SYMBOL_LENGTH)   return (XrJitResult){ (int64_t)arr->length, XR_TAG_I64 };
        if (sym == SYMBOL_IS_EMPTY) return (XrJitResult){ arr->length == 0 ? 1 : 0, XR_TAG_I64 };
    }

    // Map properties
    if (heap_type == XR_TMAP) {
        XrMap *map = (XrMap *)obj.ptr;
        if (sym == SYMBOL_LENGTH)   return (XrJitResult){ (int64_t)xr_map_size(map), XR_TAG_I64 };
        if (sym == SYMBOL_IS_EMPTY) return (XrJitResult){ xr_map_is_empty(map) ? 1 : 0, XR_TAG_I64 };
    }

    // Set properties
    if (heap_type == XR_TSET) {
        XrSet *set = (XrSet *)obj.ptr;
        if (sym == SYMBOL_LENGTH)   return (XrJitResult){ (int64_t)xr_set_size(set), XR_TAG_I64 };
        if (sym == SYMBOL_IS_EMPTY) return (XrJitResult){ xr_set_is_empty(set) ? 1 : 0, XR_TAG_I64 };
    }

    // Enum value properties
    if (heap_type == XR_TENUM_VALUE) {
        XrEnumValue *ev = (XrEnumValue *)obj.ptr;
        if (sym == SYMBOL_VALUE)   return XR_JIT_RESULT(ev->raw_value);
        if (sym == SYMBOL_NAME)    { XrValue _n = xr_string_value(xr_string_new(coro->isolate, ev->member_name, strlen(ev->member_name))); return XR_JIT_RESULT(_n); }
        if (sym == SYMBOL_ORDINAL) return (XrJitResult){ (int64_t)ev->member_index, XR_TAG_I64 };
    }

    // Json object field access
    if (xr_value_is_json(obj)) {
        XrJson *json = xr_value_to_json(obj);
        XrValue result = xr_json_get(json, sym);
        return XR_JIT_RESULT(result);
    }

    return XR_JIT_NULL();
}

// Called from JIT via CALL_C for OP_SETPROP.
// jit_call_args[0] = obj raw (ptr), jit_call_args[1] = value raw
// extra_arg = (val_bc_slot<<40) | (symbol_id<<8) | val_slot_type
XrJitResult xr_jit_setprop(XrCoroutine *coro, int64_t extra_arg) {
    SymbolId sym = (SymbolId)(uint32_t)((extra_arg >> 8) & 0xFFFFFFFF);

    XrValue obj;
    obj.ptr = (void *)coro->jit_ctx->call_args[0];
    obj.tag = XR_TAG_PTR;

    XrValue value = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                        coro->jit_ctx->call_arg_tags[1]);

    if (xr_value_is_json(obj)) {
        XrJson *json = xr_value_to_json(obj);
        xr_json_set(coro->isolate, json, sym, value);
    }

    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_INDEX_GET.
// jit_call_args[0] = obj raw, jit_call_args[1] = key raw
// extra_arg = (obj_slot_type << 8) | key_slot_type
XrJitResult xr_jit_index_get(XrCoroutine *coro, int64_t extra_arg) {
    XrValue obj_val = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                          coro->jit_ctx->call_arg_tags[0]);
    XrValue key_val = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                          coro->jit_ctx->call_arg_tags[1]);

    XrValue result = xr_null();

    if (XR_IS_PTR(obj_val) && obj_val.ptr) {
        int heap_type = XR_GC_GET_TYPE((XrGCHeader *)obj_val.ptr);

        if (heap_type == XR_TARRAY && XR_IS_INT(key_val)) {
            XrArray *arr = (XrArray *)obj_val.ptr;
            int idx = (int)key_val.i;
            if ((unsigned)idx < (unsigned)arr->length && arr->data) {
                result = (arr->elem_type == XR_ELEM_ANY)
                    ? ((XrValue *)arr->data)[idx]
                    : xr_array_get_element(arr, idx);
            }
        } else if (heap_type == XR_TMAP) {
            XrMap *map = (XrMap *)obj_val.ptr;
            bool found;
            result = xr_map_get(map, key_val, &found);
            if (!found) result = xr_null();
        } else if (heap_type == XR_TSTRING && XR_IS_INT(key_val)) {
            XrString *str = (XrString *)obj_val.ptr;
            size_t idx = (size_t)key_val.i;
            XrString *ch = xr_string_char_at_unicode(coro->isolate, str, idx);
            result = ch ? xr_string_value(ch) : xr_null();
        } else if (heap_type == XR_TJSON && XR_IS_PTR(key_val)) {
            XrJson *json = (XrJson *)obj_val.ptr;
            XrString *key_str = (XrString *)key_val.ptr;
            result = xr_json_get_by_key(coro->isolate, json, key_str->data);
        }
    }

    return XR_JIT_RESULT(result);
}

// Called from JIT via CALL_C for OP_INDEX_SET.
// jit_call_args[0] = obj raw, jit_call_args[1] = key raw, jit_call_args[2] = val raw
// extra_arg = (val_bc_slot<<24) | (obj_st<<16) | (key_st<<8) | val_st
//   val_bc_slot: bc register slot for val — used to resolve CALLEE_SETS via slot_runtime_tags
XrJitResult xr_jit_index_set(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue obj_val = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                          coro->jit_ctx->call_arg_tags[0]);
    XrValue key_val = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                          coro->jit_ctx->call_arg_tags[1]);
    XrValue new_val = jit_value_from_tag(coro->jit_ctx->call_args[2],
                                          coro->jit_ctx->call_arg_tags[2]);

    if (XR_IS_PTR(obj_val) && obj_val.ptr) {
        int heap_type = XR_GC_GET_TYPE((XrGCHeader *)obj_val.ptr);

        if (heap_type == XR_TARRAY && XR_IS_INT(key_val)) {
            XrArray *arr = (XrArray *)obj_val.ptr;
            int idx = (int)key_val.i;
            if ((unsigned)idx < (unsigned)arr->length) {
                xr_array_set_element(arr, idx, new_val);
            }
        } else if (heap_type == XR_TMAP) {
            XrMap *map = (XrMap *)obj_val.ptr;
            xr_map_set(map, key_val, new_val);
        } else if (heap_type == XR_TJSON && XR_IS_PTR(key_val)) {
            XrJson *json = (XrJson *)obj_val.ptr;
            XrString *key_str = (XrString *)key_val.ptr;
            xr_json_set_by_key(coro->isolate, json, key_str->data, new_val);
        }
    }

    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_TARRAY_GET.
// jit_call_args[0] = array ptr (raw), jit_call_args[1] = index (raw i64)
// Returns raw i64 value from typed array (no tag).
XrJitResult xr_jit_tarray_get(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrArray *arr = (XrArray *)coro->jit_ctx->call_args[0];
    int64_t idx = coro->jit_ctx->call_args[1];

    if (!arr || idx < 0 || idx >= arr->length) return XR_JIT_NULL();

    // Typed array stores raw i64 values
    if (arr->elem_type == XR_ELEM_I64) {
        return XR_JIT_INT(((int64_t *)arr->data)[idx]);
    }
    // Regular array stores tagged XrValue - extract i64 payload
    if (arr->elem_type == XR_ELEM_ANY) {
        XrValue *elems = (XrValue *)arr->data;
        return XR_JIT_VAL(elems[idx]);
    }
    return XR_JIT_NULL();
}

// Called from JIT via CALL_C for OP_TARRAY_SET.
// jit_call_args[0] = array ptr (raw), jit_call_args[1] = index (raw i64),
// jit_call_args[2] = value (raw i64)
// extra_arg = val_slot_type (for correct tag reconstruction on ELEM_ANY arrays)
XrJitResult xr_jit_tarray_set(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrArray *arr = (XrArray *)coro->jit_ctx->call_args[0];
    int64_t idx = coro->jit_ctx->call_args[1];
    int64_t raw_val = coro->jit_ctx->call_args[2];

    if (!arr || idx < 0 || idx >= arr->length) return XR_JIT_OK();

    // Typed array stores raw i64 values
    if (arr->elem_type == XR_ELEM_I64) {
        ((int64_t *)arr->data)[idx] = raw_val;
    }
    // Regular array stores tagged XrValue — reconstruct with correct tag
    else if (arr->elem_type == XR_ELEM_ANY) {
        XrValue *elems = (XrValue *)arr->data;
        elems[idx] = jit_value_from_tag(raw_val, coro->jit_ctx->call_arg_tags[2]);
    }
    return XR_JIT_OK();
}

/* ========== Map Operations ========== */

// Called from JIT via CALL_C for OP_MAP_GET / OP_MAP_GETK.
// jit_call_args[0] = map/json ptr, jit_call_args[1] = key raw
// extra_arg = key_slot_type
// Handles both XrMap and XrJson objects (VM OP_MAP_GETK supports both).
XrJitResult xr_jit_map_get(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    void *obj = (void *)coro->jit_ctx->call_args[0];
    if (!obj) return XR_JIT_NULL();
    XrGCHeader *hdr = (XrGCHeader *)obj;
    XrValue key = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                      coro->jit_ctx->call_arg_tags[1]);
    XrValue result = xr_null();
    if (hdr->type == XR_TMAP) {
        XrMap *map = (XrMap *)obj;
        bool found;
        result = xr_map_get(map, key, &found);
        if (!found) result = xr_null();
    } else if (hdr->type == XR_TJSON && XR_IS_PTR(key)) {
        XrJson *json = (XrJson *)obj;
        XrString *key_str = (XrString *)key.ptr;
        result = xr_json_get_by_key(coro->isolate, json, key_str->data);
    }
    return XR_JIT_RESULT(result);
}

// Called from JIT via CALL_C for OP_MAP_SET / OP_MAP_SETK.
// jit_call_args[0] = map/json ptr, jit_call_args[1] = key raw, jit_call_args[2] = val raw
// extra_arg = (key_st << 8) | val_st
// Handles both XrMap and XrJson objects (VM OP_MAP_SETK supports both).
XrJitResult xr_jit_map_set(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    void *obj = (void *)coro->jit_ctx->call_args[0];
    if (!obj) return XR_JIT_OK();
    XrGCHeader *hdr = (XrGCHeader *)obj;
    XrValue key = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                      coro->jit_ctx->call_arg_tags[1]);
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[2],
                                      coro->jit_ctx->call_arg_tags[2]);
    if (hdr->type == XR_TMAP) {
        XrMap *map = (XrMap *)obj;
        xr_map_set(map, key, val);
    } else if (hdr->type == XR_TJSON && XR_IS_PTR(key)) {
        XrJson *json = (XrJson *)obj;
        XrString *key_str = (XrString *)key.ptr;
        xr_json_set_by_key(coro->isolate, json, key_str->data, val);
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_MAP_INCREMENT.
// jit_call_args[0] = map ptr, jit_call_args[1] = key raw
// extra_arg = key_slot_type
XrJitResult xr_jit_map_increment(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrMap *map = (XrMap *)coro->jit_ctx->call_args[0];
    if (!map) return XR_JIT_OK();
    XrValue key = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                      coro->jit_ctx->call_arg_tags[1]);
    bool found;
    XrValue cur = xr_map_get(map, key, &found);
    XrValue newval;
    newval.descriptor = 0;
    newval.tag = XR_TAG_I64;
    newval.i = found ? cur.i + 1 : 1;
    xr_map_set(map, key, newval);
    return XR_JIT_OK();
}

/* ========== Builtin Access ========== */

// Called from JIT via CALL_C for OP_GETBUILTIN.
// extra_arg = builtin_index
// Returns raw i64 payload of the builtin XrValue.
XrJitResult xr_jit_getbuiltin(XrCoroutine *coro, int64_t extra_arg) {
    int idx = (int)(extra_arg & 0xFFFF);
    XrayIsolate *isolate = coro->isolate;
    if (idx < xr_isolate_get_vm_state(isolate)->builtin_count) {
        XrValue val = xr_isolate_get_vm_state(isolate)->builtins[idx];
        return XR_JIT_VAL(val);
    }
    return XR_JIT_NULL();
}

/* ========== Range Operations ========== */

// Called from JIT via CALL_C for OP_NEWRANGE.
// jit_call_args[0] = start (i64), jit_call_args[1] = end (i64)
// Returns raw ptr to XrRange.
XrJitResult xr_jit_newrange(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    int64_t start = coro->jit_ctx->call_args[0];
    int64_t end = coro->jit_ctx->call_args[1];
    XrRange *range = xr_range_new(coro, start, end);
    return XR_JIT_PTR(range);
}

// Called from JIT via CALL_C for OP_RANGE_UNPACK.
// jit_call_args[0] = range ptr
// Returns packed: count in return value, step and start stored in jit_call_args[1..2]
XrJitResult xr_jit_range_unpack(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrRange *rng = (XrRange *)coro->jit_ctx->call_args[0];
    if (!rng) return XR_JIT_OK();
    int64_t count = xr_range_length(rng);
    coro->jit_ctx->call_args[1] = rng->step;
    coro->jit_ctx->call_args[2] = rng->start;
    return XR_JIT_INT(count);
}

/* ========== Type Check ========== */

// Called from JIT via CALL_C for OP_IS.
// jit_call_args[0] = value raw
// extra_arg = (xr_tag << 8) | expected_type_id
XrJitResult xr_jit_is_type(XrCoroutine *coro, int64_t extra_arg) {
    int expected_tid = (int)(extra_arg & 0xFF);
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                      coro->jit_ctx->call_arg_tags[0]);
    XrTypeId actual_tid = xr_value_typeid(val);
    return XR_JIT_BOOL((actual_tid == (XrTypeId)expected_tid) ? 1 : 0);
}

// Called from JIT via CALL_C for OP_CHECKTYPE.
// jit_call_args[0] = value raw
// jit_call_args[1] = expected bitmask (int64)
// extra_arg = val_tag
// Returns: 1 if type matches bitmask, 0 if mismatch (triggers deopt)
XrJitResult xr_jit_checktype(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    int64_t mask = coro->jit_ctx->call_args[1];
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                      coro->jit_ctx->call_arg_tags[0]);
    XrTypeId actual_tid = xr_value_typeid(val);
    return XR_JIT_BOOL(((1LL << actual_tid) & mask) ? 1 : 0);
}

/* ========== String Operations ========== */

// Called from JIT via CALL_C for OP_CHR.
// jit_call_args[0] = codepoint (i64)
// Returns raw ptr to XrString (or null i64 if invalid).
XrJitResult xr_jit_chr(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    int64_t cp = coro->jit_ctx->call_args[0];
    if (cp < 0 || cp > 0x10FFFF) return XR_JIT_NULL();
    char buf[4];
    int len = 0;
    if (cp < 0x80) {
        buf[0] = (char)cp; len = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F)); len = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F)); len = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F)); len = 4;
    }
    XrString *s = xr_string_intern(coro->isolate, buf, len, 0);
    return XR_JIT_PTR(s);
}

// Called from JIT via CALL_C for OP_SUBSTRING.
// jit_call_args[0] = string ptr, jit_call_args[1] = start, jit_call_args[2] = end
// Returns raw i64 of result string XrValue.
XrJitResult xr_jit_substring(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrString *str = (XrString *)coro->jit_ctx->call_args[0];
    if (!str) return XR_JIT_NULL();
    int start = (int)coro->jit_ctx->call_args[1];
    int end = (int)coro->jit_ctx->call_args[2];
    int slen = (int)str->length;
    if (start < 0) start = 0;
    if (end > slen) end = slen;
    if (start >= end) return XR_JIT_PTR(xr_string_intern(coro->isolate, "", 0, 0));
    XrString *sub = xr_string_intern(coro->isolate, str->data + start, end - start, 0);
    return XR_JIT_PTR(sub);
}

// Called from JIT via CALL_C for OP_STR_REPEAT.
// jit_call_args[0] = string ptr, jit_call_args[1] = count
// Returns raw i64 of result string XrValue.
XrJitResult xr_jit_str_repeat(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrString *str = (XrString *)coro->jit_ctx->call_args[0];
    if (!str) return XR_JIT_NULL();
    int count = (int)coro->jit_ctx->call_args[1];
    if (count <= 0) return XR_JIT_PTR(xr_string_intern(coro->isolate, "", 0, 0));
    if (count == 1) return XR_JIT_PTR(str);
    int slen = (int)str->length;
    int total = slen * count;
    if (total > 1024 * 1024) return XR_JIT_NULL(); // safety limit
    char *buf = xr_malloc(total);
    if (!buf) return XR_JIT_NULL();
    for (int i = 0; i < count; i++) {
        memcpy(buf + i * slen, str->data, slen);
    }
    XrString *result = xr_string_intern(coro->isolate, buf, total, 0);
    xr_free(buf);
    return XR_JIT_PTR(result);
}

// Called from JIT via CALL_C for OP_TYPENAME.
// jit_call_args[0] = value raw
// extra_arg = (slot_hint << 8) | xr_tag
// Returns raw i64 of result string XrValue.
XrJitResult xr_jit_typename(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                      coro->jit_ctx->call_arg_tags[0]);
    const char *type_name = xr_typeid_name(xr_value_typeid(val));
    size_t len = strlen(type_name);
    XrString *str = xr_string_intern(coro->isolate, type_name, len, 0);
    return XR_JIT_PTR(str);
}

// Called from JIT via CALL_C for OP_DUMP.
// jit_call_args[0] = value raw
// extra_arg = (xr_tag << 8) | depth
XrJitResult xr_jit_dump(XrCoroutine *coro, int64_t extra_arg) {
    int depth = (int)(extra_arg & 0xFF);
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                      coro->jit_ctx->call_arg_tags[0]);
    xr_value_dump(val, depth);
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for XIR_RT_EQ when operands are not both numeric.
// jit_call_args[0] = value A raw, jit_call_args[1] = value B raw.
// extra_arg = (a_xr_tag << 8) | b_xr_tag
// Returns 1 if equal, 0 otherwise.
XrJitResult xr_jit_rt_eq(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    int64_t raw_a = coro->jit_ctx->call_args[0];
    int64_t raw_b = coro->jit_ctx->call_args[1];

    // Fast path 1: identical raw values → equal
    if (raw_a == raw_b) return XR_JIT_BOOL(1);

    uint8_t a_tag = coro->jit_ctx->call_arg_tags[0];
    uint8_t b_tag = coro->jit_ctx->call_arg_tags[1];

    // Fast path 2: both PTR tags, both aligned → compare GC types
    if (a_tag == XR_TAG_PTR && b_tag == XR_TAG_PTR &&
        raw_a != 0 && raw_b != 0 &&
        (raw_a & 0x7) == 0 && (raw_b & 0x7) == 0) {
        XrGCHeader *ga = (XrGCHeader *)(intptr_t)raw_a;
        XrGCHeader *gb = (XrGCHeader *)(intptr_t)raw_b;
        if (ga->type != gb->type) return XR_JIT_BOOL(0);
    }

    XrValue a = jit_value_from_tag(raw_a, a_tag);
    XrValue b = jit_value_from_tag(raw_b, b_tag);
    return XR_JIT_BOOL(vm_values_equal(a, b) ? 1 : 0);
}

// Called from JIT via CALL_C for OP_EQK with non-numeric constants.
// jit_call_args[0] = value A raw, jit_call_args[1] = value B raw (constant).
// extra_arg = (a_slot_type << 8) | b_tag
// Returns 1 if equal, 0 otherwise.
XrJitResult xr_jit_eq_value(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t b_tag = (uint8_t)(extra_arg & 0xFF);
    XrValue a = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                    coro->jit_ctx->call_arg_tags[0]);
    XrValue b;
    b.descriptor = 0;
    b.tag = b_tag;
    b.i = coro->jit_ctx->call_args[1];
    if (b_tag == XR_TAG_PTR && b.i != 0) {
        XrGCHeader *gc = (XrGCHeader *)(intptr_t)b.i;
        b.heap_type = (uint16_t)gc->type;
    }
    return XR_JIT_BOOL(vm_values_equal(a, b) ? 1 : 0);
}

// UPVAL_GET: read upvalue from current closure.
// extra_arg = upvalue index.
// Returns XrJitResult with payload and tag.
XrJitResult xr_jit_upval_get(XrCoroutine *coro, int64_t upval_index) {
    XrClosure *cl = (XrClosure *)coro->jit_ctx->call_closure;
    if (!cl || upval_index < 0 || upval_index >= cl->upval_count) {
        return XR_JIT_NULL();
    }
    XrValue val = cl->upvals[upval_index];
    return XR_JIT_VAL(val);
}

// extra_arg = child proto pointer (cast to int64_t).
// Flat upvalue model: OP_CLOSURE populates closure->upvals[] from proto descriptors.
// Returns raw closure pointer.
// Automatically populates UPVAL_SRC_UPVAL entries from enclosing closure;
// UPVAL_SRC_REG entries must be set via xr_jit_closure_set_upval afterwards.
XrJitResult xr_jit_closure_new(XrCoroutine *coro, int64_t proto_raw) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate) return XR_JIT_NULL();

    XrProto *child = (XrProto *)(uintptr_t)proto_raw;
    if (!child) return XR_JIT_NULL();

    XrClosure *closure = xr_closure_new(isolate, child, coro);
    if (!closure) return XR_JIT_NULL();

    // Populate UPVAL_SRC_UPVAL entries from enclosing closure
    XrClosure *enclosing = (XrClosure *)coro->jit_ctx->call_closure;
    int nuv = DYNARRAY_COUNT(&child->upvalues);
    for (int j = 0; j < nuv; j++) {
        UpvalInfo *uv = &DYNARRAY_GET(&child->upvalues, j, UpvalInfo);
        if (uv->source == UPVAL_SRC_UPVAL && enclosing) {
            int idx = uv->index;
            closure->upvals[j] = (idx < enclosing->upval_count)
                ? enclosing->upvals[idx] : xr_null();
        }
    }

    return XR_JIT_PTR(closure);
}

// Store a register value into closure->upvals[idx].
// call_args[0] = raw closure pointer, call_args[1] = raw payload.
// extra_arg = (upval_index << 8) | tag.
XrJitResult xr_jit_closure_set_upval(XrCoroutine *coro, int64_t encoded) {
    int idx = (int)((uint64_t)encoded >> 8);
    uint8_t tag = (uint8_t)(encoded & 0xFF);
    XrClosure *cl = (XrClosure *)(uintptr_t)coro->jit_ctx->call_args[0];
    if (!cl || idx >= cl->upval_count) return XR_JIT_OK();

    int64_t raw = coro->jit_ctx->call_args[1];
    XrValue val = {0};
    if (tag == XR_TAG_PTR) {
        val = xr_make_ptr_val((void *)(uintptr_t)raw);
    } else {
        val.i = raw;
        val.tag = tag;
    }
    cl->upvals[idx] = val;
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_TOSTRING (JIT mode).
// jit_call_args[0] = raw value to convert.
// extra_arg = xr_tag (precise tag from builder).
// Returns raw ptr to XrString*.
XrJitResult xr_jit_tostring(XrCoroutine *coro, int64_t xr_tag_arg) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate) return XR_JIT_NULL();
    int64_t raw = coro->jit_ctx->call_args[0];
    uint8_t tag = coro->jit_ctx->call_arg_tags[0];
    XrValue val = jit_value_from_tag(raw, tag);
    XrString *str = xr_value_to_string(isolate, val);
    return XR_JIT_PTR(str);
}

// Called from JIT via CALL_C for OP_STRBUF_NEW (JIT mode).
// Returns raw ptr to new StringBuilder.
XrJitResult xr_jit_strbuf_new(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrStringBuilder *sb = xr_stringbuilder_new(coro);
    return XR_JIT_PTR(sb);
}

// Called from JIT via CALL_C for OP_STRBUF_APPEND (JIT mode).
// jit_call_args[0] = sb ptr, jit_call_args[1] = value raw
// extra_arg = (bc_slot_hint << 8) | val_slot_type
XrJitResult xr_jit_strbuf_append(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrStringBuilder *sb = (XrStringBuilder *)coro->jit_ctx->call_args[0];
    if (!sb) return XR_JIT_OK();
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                      coro->jit_ctx->call_arg_tags[1]);
    if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        xr_stringbuilder_append_str(sb, s);
    } else if (XR_IS_INT(val)) {
        xr_stringbuilder_append_int(sb, XR_TO_INT(val));
    } else if (XR_IS_FLOAT(val)) {
        xr_stringbuilder_append_float(sb, XR_TO_FLOAT(val));
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_STRBUF_FINISH (JIT mode).
// jit_call_args[0] = sb ptr
// Returns raw i64 of result string XrValue.
XrJitResult xr_jit_strbuf_finish(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrStringBuilder *sb = (XrStringBuilder *)coro->jit_ctx->call_args[0];
    if (!sb) return XR_JIT_NULL();
    XrString *result = xr_stringbuilder_to_string(sb);
    return XR_JIT_PTR(result);
}

/* ========== Set Operations ========== */

// Called from JIT via CALL_C for OP_NEWSET.
// extra_arg = (elem_tid << 2) | flags
// Returns raw ptr to XrSet.
XrJitResult xr_jit_newset(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrSet *set = xr_set_new(coro);
    if (!set) return XR_JIT_NULL();
    return XR_JIT_PTR(set);
}

/* ========== Field Access ========== */

// Called from JIT via CALL_C for OP_GETFIELD_IC.
// jit_call_args[0] = instance ptr
// extra_arg = field_name constant index (encoded as raw string ptr)
// Returns raw i64 payload of the field value.
XrJitResult xr_jit_getfield_ic(XrCoroutine *coro, int64_t extra_arg) {
    XrInstance *inst = (XrInstance *)coro->jit_ctx->call_args[0];
    if (!inst) {
        return XR_JIT_NULL();
    }
    XrString *field_name = (XrString *)(uintptr_t)extra_arg;
    if (!field_name) {
        return XR_JIT_NULL();
    }
    XrClass *cls = xr_instance_get_class(inst);
    int idx = xr_class_lookup_field_by_name(coro->isolate, cls, field_name->data);
    if (idx < 0) {
        return XR_JIT_NULL();
    }
    XrValue val = inst->fields[idx];
    return XR_JIT_VAL(val);
}

/* ========== Enum Operations ========== */

// Called from JIT via CALL_C for OP_ENUM_ACCESS.
// jit_call_args[0] = enum_type ptr, jit_call_args[1] = member_index
XrJitResult xr_jit_enum_access(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrEnumType *et = (XrEnumType *)coro->jit_ctx->call_args[0];
    int idx = (int)coro->jit_ctx->call_args[1];
    if (!et || idx < 0 || (uint32_t)idx >= et->member_count) {
        return XR_JIT_NULL();
    }
    return XR_JIT_PTR(et->members[idx].instance);
}

// Called from JIT via CALL_C for OP_ENUM_NAME.
// jit_call_args[0] = enum_value ptr
XrJitResult xr_jit_enum_name(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrEnumValue *ev = (XrEnumValue *)coro->jit_ctx->call_args[0];
    if (!ev || !ev->member_name) return XR_JIT_NULL();
    size_t len = strlen(ev->member_name);
    XrString *s = xr_string_intern(coro->isolate, ev->member_name, len, 0);
    return XR_JIT_PTR(s);
}

// Called from JIT via CALL_C for OP_ENUM_CONVERT.
// jit_call_args[0] = enum_type ptr, jit_call_args[1] = value raw
// extra_arg = xr_tag
XrJitResult xr_jit_enum_convert(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t val_tag = (uint8_t)(extra_arg & 0xFF);
    XrEnumType *et = (XrEnumType *)coro->jit_ctx->call_args[0];
    if (!et) {
        return XR_JIT_NULL();
    }
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[1], val_tag);
    XrEnumValue *result = xr_enum_from_value(et, val);
    if (result) {
        return XR_JIT_PTR(result);
    }
    return XR_JIT_NULL();
}

/* ========== Bytes Operations ========== */

// Called from JIT via CALL_C for OP_BYTES_NEW.
// jit_call_args[0..nargs-1] = byte values
// extra_arg = nargs
XrJitResult xr_jit_bytes_new(XrCoroutine *coro, int64_t extra_arg) {
    int nargs = (int)(extra_arg & 0xFF);
    XrArray *arr = xr_array_new(coro);
    if (!arr) return XR_JIT_NULL();
    for (int i = 0; i < nargs && i < 8; i++) {
        xr_array_push(arr, XR_FROM_INT(coro->jit_ctx->call_args[i]));
    }
    return XR_JIT_PTR(arr);
}

/* ========== Struct Native Storage ========== */

// OP_NEW_STRUCT: allocate in per-frame struct_area (zero heap allocation).
// call_args[0] = class ptr (XrClass*), extra_arg = slot_offset (C operand)
// Returns raw struct_ptr (into struct_area).
// Precondition: struct_area already allocated by VM startfunc before JIT entry.
XrJitResult xr_jit_new_struct(XrCoroutine *coro, int64_t extra_arg) {
    int slot_offset = (int)extra_arg;
    XrClass *cls = (XrClass *)(uintptr_t)coro->jit_ctx->call_args[0];
    XrStructLayout *layout = cls->struct_layout;
    XR_DCHECK(layout != NULL, "xr_jit_new_struct: struct_layout required");
    int frame_idx = coro->vm_ctx.frame_count - 1;
    XR_DCHECK(coro->vm_ctx.struct_areas && coro->vm_ctx.struct_areas[frame_idx],
              "xr_jit_new_struct: struct_area not allocated");
    uint8_t *struct_ptr = coro->vm_ctx.struct_areas[frame_idx] + slot_offset * 16;
    *(XrClass**)struct_ptr = cls;
    memset(struct_ptr + 8, 0, layout->total_size);
    return XR_JIT_PTR(struct_ptr);
}

// OP_STRUCT_GET: read native field from stack-allocated struct, box to XrValue.
// call_args[0] = struct_ptr (raw), extra_arg = field_idx
// Returns XrValue.i (raw payload of boxed result).
XrJitResult xr_jit_struct_get(XrCoroutine *coro, int64_t extra_arg) {
    int field_idx = (int)extra_arg;
    uint8_t *struct_ptr = (uint8_t *)(uintptr_t)coro->jit_ctx->call_args[0];
    XrClass *cls = *(XrClass**)struct_ptr;
    XrStructLayout *layout = cls->struct_layout;
    XrStructFieldLayout *field = &layout->fields[field_idx];
    uint8_t *fp = struct_ptr + 8 + field->offset;
    XrValue result;
    switch (field->native_type) {
        case XR_NATIVE_I64:  result = XR_FROM_INT(*(int64_t*)fp); break;
        case XR_NATIVE_F64:  result = XR_FROM_FLOAT(*(double*)fp); break;
        case XR_NATIVE_BOOL: result.descriptor = 0; result.i = *(uint8_t*)fp ? 1 : 0; result.tag = XR_TAG_BOOL; break;
        case XR_NATIVE_I32:  result = XR_FROM_INT((int64_t)*(int32_t*)fp); break;
        case XR_NATIVE_U32:  result = XR_FROM_INT((int64_t)*(uint32_t*)fp); break;
        case XR_NATIVE_I16:  result = XR_FROM_INT((int64_t)*(int16_t*)fp); break;
        case XR_NATIVE_U16:  result = XR_FROM_INT((int64_t)*(uint16_t*)fp); break;
        case XR_NATIVE_I8:   result = XR_FROM_INT((int64_t)*(int8_t*)fp); break;
        case XR_NATIVE_U8:   result = XR_FROM_INT((int64_t)*(uint8_t*)fp); break;
        case XR_NATIVE_F32:  result = XR_FROM_FLOAT((double)*(float*)fp); break;
        case XR_NATIVE_STRING: {
            XrString *s = *(XrString**)fp;
            result = s ? XR_FROM_STR(s) : xr_null();
            break;
        }
        case XR_NATIVE_STRUCT: result = xr_struct_ref(fp, field->sub_layout_id); break;
        case XR_NATIVE_ARRAY:
            result = xr_array_ref(fp, field->elem_native_type, field->elem_count);
            break;
        default: result = xr_null(); break;
    }
    return XR_JIT_VAL(result);
}

// OP_STRUCT_SET: unbox XrValue and write native field to stack-allocated struct.
// call_args[0] = struct_ptr (raw), call_args[1] = value raw (i64 bits)
// extra_arg = field_idx | (xr_tag << 8)
XrJitResult xr_jit_struct_set(XrCoroutine *coro, int64_t extra_arg) {
    int field_idx = (int)(extra_arg & 0xFF);
    int val_tag = (int)((extra_arg >> 8) & 0xFF);
    uint8_t *struct_ptr = (uint8_t *)(uintptr_t)coro->jit_ctx->call_args[0];
    int64_t raw_val = coro->jit_ctx->call_args[1];
    XrClass *cls = *(XrClass**)struct_ptr;
    XrStructLayout *layout = cls->struct_layout;
    XrStructFieldLayout *field = &layout->fields[field_idx];
    uint8_t *fp = struct_ptr + 8 + field->offset;

    switch (field->native_type) {
        case XR_NATIVE_I64:
        case XR_NATIVE_I32: case XR_NATIVE_U32:
        case XR_NATIVE_I16: case XR_NATIVE_U16:
        case XR_NATIVE_I8:  case XR_NATIVE_U8: {
            int64_t ival = raw_val;
            switch (field->native_type) {
                case XR_NATIVE_I64:  *(int64_t*)fp  = ival; break;
                case XR_NATIVE_I32:  *(int32_t*)fp  = (int32_t)ival; break;
                case XR_NATIVE_U32:  *(uint32_t*)fp = (uint32_t)ival; break;
                case XR_NATIVE_I16:  *(int16_t*)fp  = (int16_t)ival; break;
                case XR_NATIVE_U16:  *(uint16_t*)fp = (uint16_t)ival; break;
                case XR_NATIVE_I8:   *(int8_t*)fp   = (int8_t)ival; break;
                case XR_NATIVE_U8:   *(uint8_t*)fp  = (uint8_t)ival; break;
                default: break;
            }
            break;
        }
        case XR_NATIVE_F64: case XR_NATIVE_F32: {
            double dval;
            if (val_tag == XR_TAG_F64) {
                memcpy(&dval, &raw_val, sizeof(double));
            } else {
                dval = (double)raw_val; // int → float conversion
            }
            if (field->native_type == XR_NATIVE_F64)
                *(double*)fp = dval;
            else
                *(float*)fp = (float)dval;
            break;
        }
        case XR_NATIVE_BOOL:
            *(uint8_t*)fp = raw_val ? 1 : 0;
            break;
        case XR_NATIVE_STRING: {
            *(XrString**)fp = (XrString*)(uintptr_t)raw_val;
            break;
        }
        case XR_NATIVE_STRUCT: {
            uint8_t *src_ptr = (uint8_t*)(uintptr_t)raw_val;
            memcpy(fp, src_ptr, field->size);
            break;
        }
        default:
            break;
    }
    return XR_JIT_OK();
}

// OP_STRUCT_COPY: memcpy struct to new struct_area slot.
// call_args[0] = src struct_ptr (raw), extra_arg = dst_slot_offset (C operand)
// Returns raw dst struct_ptr.
XrJitResult xr_jit_struct_copy(XrCoroutine *coro, int64_t extra_arg) {
    int slot_offset = (int)extra_arg;
    uint8_t *src_ptr = (uint8_t *)(uintptr_t)coro->jit_ctx->call_args[0];
    XrClass *cls = *(XrClass**)src_ptr;
    XrStructLayout *layout = cls->struct_layout;
    int frame_idx = coro->vm_ctx.frame_count - 1;
    uint8_t *dst_ptr = coro->vm_ctx.struct_areas[frame_idx] + slot_offset * 16;
    memcpy(dst_ptr, src_ptr, 8 + layout->total_size);
    return XR_JIT_PTR(dst_ptr);
}

/* ========== Copy/Misc Operations ========== */

// Called from JIT via CALL_C for OP_COPY.
// jit_call_args[0] = value raw
// extra_arg = xr_tag
// Returns raw i64 of deep-copied XrValue.
XrJitResult xr_jit_deep_copy(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t val_tag = (uint8_t)(extra_arg & 0xFF);

    // Fast path: flat_copyable struct (skip CopyContext + type dispatch)
    if (val_tag == XR_TAG_PTR || val_tag == XR_RTAG_UNKNOWN) {
        void *raw = (void *)(uintptr_t)coro->jit_ctx->call_args[0];
        if (raw) {
            XrGCHeader *hdr = (XrGCHeader *)raw;
            if (hdr->type == XR_TINSTANCE) {
                XrInstance *inst = (XrInstance *)raw;
                XrClass *cls = inst->klass;
                if ((cls->flags & (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) ==
                    (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) {
                    uint32_t fc = xr_class_instance_field_count(cls);
                    size_t sz = sizeof(XrInstance) + sizeof(XrValue) * fc;
                    XrGC *gc = xr_isolate_get_gc(coro->isolate);
                    XrInstance *dst = (XrInstance *)xr_gc_alloc(gc, sz, XR_TINSTANCE);
                    if (dst) {
                        dst->klass = cls;
                        dst->gc.extra = (dst->gc.extra & 0x01) |
                                        (inst->gc.extra & ~0x01);
                        if (fc > 0)
                            memcpy(dst->fields, inst->fields,
                                   sizeof(XrValue) * fc);
                        return XR_JIT_PTR(dst);
                    }
                }
            }
        }
    }

    // Generic path
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0], val_tag);
    XrValue result = xr_deep_copy(coro->isolate, val, NULL);
    return XR_JIT_VAL(result);
}

/* ========== Slice Operations ========== */

// Called from JIT via CALL_C for OP_SLICE.
// jit_call_args[0] = source ptr, jit_call_args[1] = start, jit_call_args[2] = end
// Returns raw i64 of result XrValue.
XrJitResult xr_jit_slice(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    void *src = (void *)coro->jit_ctx->call_args[0];
    int32_t start = (int32_t)coro->jit_ctx->call_args[1];
    int32_t end = (int32_t)coro->jit_ctx->call_args[2];
    if (!src) return XR_JIT_NULL();
    XrArray *arr = (XrArray *)src;
    XrArray *slice = xr_array_slice(coro, arr, start, end);
    return slice ? XR_JIT_PTR(slice) : XR_JIT_NULL();
}

/* ========== Assert Operations ========== */

// Called from JIT via CALL_C for OP_ASSERT.
// jit_call_args[0] = condition raw (truthy/falsy)
// extra_arg = (negate << 16) | (loc_const_idx & 0xFFFF)
// Returns 0 on success, sets jit_exception on failure.
XrJitResult xr_jit_assert(XrCoroutine *coro, int64_t extra_arg) {
    int negate = (int)((extra_arg >> 16) & 0x1);
    int64_t cond_raw = coro->jit_ctx->call_args[0];
    // Simple truthy check: non-zero = truthy
    bool truthy = (cond_raw != 0);
    bool failed = negate ? truthy : !truthy;
    if (failed) {
        // Set a generic assertion exception
        const char *fn_name = negate ? "assert_false" : "assert";
        fprintf(stderr, "\nASSERTION FAILED: %s()\n\n", fn_name);
        return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_ASSERT_EQ.
// jit_call_args[0] = actual raw, jit_call_args[1] = expected raw
// extra_arg = (a_bc_slot << 24) | (b_bc_slot << 16) | (actual_st << 8) | expected_st
XrJitResult xr_jit_assert_eq(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t a_bc = (uint8_t)((extra_arg >> 24) & 0xFF);
    uint8_t b_bc = (uint8_t)((extra_arg >> 16) & 0xFF);
    uint8_t a_tag = coro->jit_ctx->call_arg_tags[0];
    uint8_t b_tag = coro->jit_ctx->call_arg_tags[1];
    XrValue actual = jit_value_from_tag(coro->jit_ctx->call_args[0], a_tag);
    XrValue expect = jit_value_from_tag(coro->jit_ctx->call_args[1], b_tag);
    if (!xr_value_deep_eq(actual, expect)) {
        fprintf(stderr, "\nASSERTION FAILED (JIT): assert_eq() values are not equal\n");
        fprintf(stderr, "  extra_arg=0x%llx a_bc=%d b_bc=%d a_tag=%d b_tag=%d\n",
                (unsigned long long)extra_arg, a_bc, b_bc, a_tag, b_tag);
        fprintf(stderr, "  raw_a=0x%llx raw_b=0x%llx\n",
                (unsigned long long)coro->jit_ctx->call_args[0],
                (unsigned long long)coro->jit_ctx->call_args[1]);
        fprintf(stderr, "  actual: tag=%d i=%lld  expect: tag=%d i=%lld\n",
                actual.tag, (long long)actual.i, expect.tag, (long long)expect.i);
        if (a_bc != 0xFF)
            fprintf(stderr, "  slot_runtime_tags[%d]=%d\n", a_bc,
                    coro->jit_ctx->slot_runtime_tags[a_bc]);
        return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_ASSERT_NE.
// jit_call_args[0] = actual raw, jit_call_args[1] = expected raw
// extra_arg = (a_bc_slot << 24) | (b_bc_slot << 16) | (actual_st << 8) | expected_st
XrJitResult xr_jit_assert_ne(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t a_bc = (uint8_t)((extra_arg >> 24) & 0xFF);
    uint8_t b_bc = (uint8_t)((extra_arg >> 16) & 0xFF);
    uint8_t a_tag = coro->jit_ctx->call_arg_tags[0];
    uint8_t b_tag = coro->jit_ctx->call_arg_tags[1];
    XrValue actual = jit_value_from_tag(coro->jit_ctx->call_args[0], a_tag);
    XrValue expect = jit_value_from_tag(coro->jit_ctx->call_args[1], b_tag);
    if (xr_value_deep_eq(actual, expect)) {
        fprintf(stderr, "\nASSERTION FAILED: assert_ne()\n\n");
        return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
    }
    return XR_JIT_OK();
}

/* ========== Coroutine Runtime Helpers ========== */

// Called from JIT via CALL_C for OP_CHAN_NEW.
// extra_arg = buffer_size
// Returns raw channel pointer (tagged XrValue).
XrJitResult xr_jit_chan_new(XrCoroutine *coro, int64_t extra_arg) {
    uint32_t buffer_size = (uint32_t)(extra_arg & 0xFFFFFFFF);
    XrayIsolate *isolate = coro->isolate;
    if (!isolate) return XR_JIT_NULL();
    XrChannel *ch = xr_channel_new(isolate, buffer_size);
    if (!ch) return XR_JIT_NULL();
    XrValue v = xr_value_from_channel(ch);
    return XR_JIT_VAL(v);
}

// Called from JIT via CALL_C for OP_CHAN_CLOSE.
// jit_call_args[0] = channel raw pointer
// Returns 0.
XrJitResult xr_jit_chan_close(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) return XR_JIT_OK();
    XrChannel *ch = xr_value_to_channel(ch_val);
    xr_channel_close(ch);
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_CHAN_IS_CLOSED.
// jit_call_args[0] = channel raw pointer
// Returns 1 if closed, 0 otherwise.
XrJitResult xr_jit_chan_is_closed(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) return XR_JIT_BOOL(1);
    XrChannel *ch = xr_value_to_channel(ch_val);
    return XR_JIT_BOOL(xr_channel_is_closed(ch) ? 1 : 0);
}

// Called from JIT via CALL_C for OP_CHAN_TRY_SEND.
// jit_call_args[0] = channel raw, jit_call_args[1] = value raw
// extra_arg = slot_type of value
// Returns 1 on success, 0 on failure.
XrJitResult xr_jit_chan_try_send(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) return XR_JIT_BOOL(0);
    XrChannel *ch = xr_value_to_channel(ch_val);

    XrValue send_v = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                         coro->jit_ctx->call_arg_tags[1]);

    // Deep copy mutable values for buffer safety
    if (XR_IS_PTR(send_v) && xr_value_needs_copy(send_v)) {
        XrayIsolate *isolate = coro->isolate;
        send_v = xr_deep_copy(isolate, send_v, xr_isolate_get_gc(isolate));
    }

    bool success = xr_channel_try_send(ch, send_v);
    if (success) {
        xr_runtime_wake_channel(coro->isolate, ch, false);
    }
    return XR_JIT_BOOL(success ? 1 : 0);
}

// Called from JIT via CALL_C for OP_CHAN_TRY_RECV.
// jit_call_args[0] = channel raw
// Returns received value raw (or 0 for null). Stores ok flag in jit_call_args[1].
XrJitResult xr_jit_chan_try_recv(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        coro->jit_ctx->call_args[1] = 0;  // ok = false
        return XR_JIT_NULL();
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    bool ok;
    XrValue value = xr_channel_try_recv(ch, &ok);

    // Unbuffered rendezvous: try to wake sender
    if (!ok) {
        XrCoroutine *sender = xr_runtime_wake_channel(coro->isolate, ch, true);
        if (sender) {
            value = sender->send_value;
            ok = true;
        }
    }

    if (ok) {
        xr_runtime_wake_channel(coro->isolate, ch, true);
        coro->jit_ctx->call_args[1] = 1;  // ok = true
        return XR_JIT_VAL(value);
    }
    coro->jit_ctx->call_args[1] = 0;  // ok = false
    return XR_JIT_NULL();
}


/* ========== Blocking Channel Send/Recv (JIT CPS) ========== */

// Fast path for OP_CHAN_SEND: try non-blocking send.
// jit_call_args[0] = channel raw, jit_call_args[1] = value raw
// extra_arg = slot_type of value
// Returns XR_JIT_NULL on success, DEOPT_MARKER if needs blocking.
XrJitResult xr_jit_chan_send(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    if (xr_channel_is_closed(ch)) {
        return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
    }

    XrValue send_v = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                         coro->jit_ctx->call_arg_tags[1]);

    // Deep copy mutable values for buffer safety
    if (XR_IS_PTR(send_v) && xr_value_needs_copy(send_v)) {
        XrayIsolate *isolate = coro->isolate;
        send_v = xr_deep_copy(isolate, send_v, xr_isolate_get_gc(isolate));
    }

    // Store prepared value for block helper
    coro->jit_ctx->call_args[1] = send_v.i;
    coro->jit_ctx->call_args[2] = (int64_t)send_v.tag;

    bool ok = xr_channel_try_send(ch, send_v);
    if (ok) {
        xr_runtime_wake_channel(coro->isolate, ch, false);
        return XR_JIT_NULL();
    }
    // Need blocking → XIR_SUSPEND will save regs then call block helper
    return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
}

// Block helper for channel send (called from XIR_SUSPEND after regs saved).
// Follows Go gopark pattern: regs already saved → call xr_channel_send
// which sets BLOCKED under lock.
// Returns 0 if blocked, 1 if send succeeded during retry.
XrJitResult xr_jit_chan_send_block(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        coro->jit_suspend_state.result = xr_null().i;
        coro->jit_suspend_state.result_tag = XR_TAG_NULL;
        return (XrJitResult){ 1, 0 };  // not blocked, handle error inline
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    uint8_t val_tag = (uint8_t)(coro->jit_ctx->call_args[2] & 0xFF);
    XrValue send_v = jit_value_from_tag(coro->jit_ctx->call_args[1], val_tag);

    coro->send_value = send_v;
    XrChanResult cr = xr_channel_send(ch, send_v, coro);
    if (cr == XR_CHAN_OK) {
        coro->jit_suspend_state.result = xr_null().i;
        coro->jit_suspend_state.result_tag = XR_TAG_NULL;
        return (XrJitResult){ 1, 0 };  // succeeded during retry
    }
    if (cr == XR_CHAN_BLOCK) {
        // BLOCKED already set by xr_channel_send under lock (gopark pattern).
        // Do NOT touch flags after this point — coro may already be woken
        // by another worker. VM bytecode path also doesn't set wait_reason.
        return (XrJitResult){ 0, 0 };  // blocked
    }
    // Closed or error: store null result, continue inline
    coro->jit_suspend_state.result = xr_null().i;
    coro->jit_suspend_state.result_tag = XR_TAG_NULL;
    return XR_JIT_OK();
}

// Fast path for OP_CHAN_RECV: try non-blocking recv.
// jit_call_args[0] = channel raw
// Returns XrJitResult with value on success, DEOPT_MARKER if needs blocking.
// Stores ok flag in jit_call_args[1].
XrJitResult xr_jit_chan_recv(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        coro->jit_ctx->call_args[1] = 0;
        return XR_JIT_NULL();
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    bool ok;
    XrValue value = xr_channel_try_recv(ch, &ok);
    if (ok) {
        // Deep copy to receiver's heap
        if (XR_IS_PTR(value) && xr_value_needs_copy(value)) {
            value = xr_deep_copy_to_coro(coro->isolate, value, coro);
        }
        coro->jit_ctx->call_args[1] = 1;
        return XR_JIT_RESULT(value);
    }

    if (xr_channel_is_closed(ch)) {
        coro->jit_ctx->call_args[1] = 0;
        return XR_JIT_NULL();
    }

    // Need blocking → XIR_SUSPEND will save regs then call block helper
    return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
}

// Block helper for channel recv (called from XIR_SUSPEND after regs saved).
// Sets recv_slot to &suspend_regs[23] so sender writes directly there.
// Returns 0 if blocked, 1 if recv succeeded during retry.
XrJitResult xr_jit_chan_recv_block(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        coro->jit_suspend_state.result = xr_null().i;
        coro->jit_suspend_state.result_tag = XR_TAG_NULL;
        return XR_JIT_OK();
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    // recv_slot must point to persistent memory — sender writes *recv_slot
    // after dequeuing this coro from recvq.  Use coro's bytecode stack[0]
    // which is always allocated and persistent (JIT doesn't use it).
    coro->recv_slot = &coro->vm_ctx.stack[0];

    XrValue out;
    XrChanResult cr = xr_channel_recv(ch, &out, coro);
    if (cr == XR_CHAN_OK) {
        // Deep copy to receiver's heap
        if (XR_IS_PTR(out) && xr_value_needs_copy(out)) {
            out = xr_deep_copy_to_coro(coro->isolate, out, coro);
        }
        coro->jit_suspend_state.result = out.i;
        coro->jit_suspend_state.result_tag = out.tag;
        return (XrJitResult){ 1, 0 };  // succeeded during retry
    }
    if (cr == XR_CHAN_CLOSED) {
        coro->jit_suspend_state.result = xr_null().i;
        coro->jit_suspend_state.result_tag = XR_TAG_NULL;
        return (XrJitResult){ 1, 0 };
    }
    if (cr == XR_CHAN_BLOCK) {
        // BLOCKED already set by xr_channel_recv under lock (gopark pattern).
        // recv_slot → stack[0] is persistent; sender will write there.
        // Worker resume path copies stack[0] → jit_suspend_state.result before
        // calling xir_jit_resume.
        // Do NOT touch flags — coro may already be woken by another worker.
        return (XrJitResult){ 0, 0 };  // blocked
    }
    coro->jit_suspend_state.result = xr_null().i;
    coro->jit_suspend_state.result_tag = XR_TAG_NULL;
    return XR_JIT_OK();
}

/* ========== Method Invocation ========== */

// Called from JIT via CALL_C for OP_INVOKE_DIRECT.
// jit_call_args[0] = receiver (raw value, tagged or ptr depending on slot type)
// jit_call_args[1..nargs] = method arguments
// extra_arg = (recv_xr_tag << 24) | (method_idx << 8) | nargs
// Resolves method closure from instance's class, then calls via VM interpreter.
XrJitResult xr_jit_invoke_direct(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t recv_tag = (uint8_t)((extra_arg >> 24) & 0xFF);
    int method_idx = (int)((extra_arg >> 8) & 0xFFFF);
    int nargs = (int)(extra_arg & 0xFF);

    XrValue recv_val = jit_value_from_tag(coro->jit_ctx->call_args[0], recv_tag);
    if (!XR_IS_PTR(recv_val) || !recv_val.ptr) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
    XrInstance *inst = xr_value_to_instance(recv_val);
    if (!inst) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    XrClass *cls = xr_instance_get_class(inst);
    if (!cls || method_idx >= cls->method_count) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    XrClosure *closure = cls->methods[method_idx].as.closure;
    if (!closure || !closure->proto) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    XrProto *proto = closure->proto;

    // Build args array: [0]=this(receiver), [1..nargs]=method args
    // Must use xr_value_from_instance so heap_type is set correctly;
    // the VM interpreter's xr_value_to_instance checks heap_type.
    XrValue args[16];
    args[0] = xr_value_from_instance(inst);
    for (int i = 0; i < nargs && i < 15; i++) {
        int64_t raw = coro->jit_ctx->call_args[1 + i];
        uint8_t tag = XR_TAG_I64;
        if (proto->param_types && (i + 1) < proto->param_types_count && proto->param_types[i + 1])
            tag = slot_type_to_xr_tag(xr_type_to_slot_type(proto->param_types[i + 1]));
        args[1 + i] = jit_value_from_tag(raw, tag);
    }

    XrayIsolate *isolate = coro->isolate;
    if (!isolate) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    bool saved_suppress = xr_isolate_get_suppress_exception_print(isolate);
    xr_isolate_set_suppress_exception_print(isolate, true);
    XrValue result = xr_vm_call_closure(isolate, closure, args, nargs + 1);
    xr_isolate_set_suppress_exception_print(isolate, saved_suppress);

    if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
        coro->jit_ctx->exception = (void *)coro->vm_ctx.current_exception.ptr;
        coro->vm_ctx.current_exception = XR_NULL_VAL;
        return XR_JIT_NULL();
    }

    {
        uint8_t rst = proto->return_type_info
            ? xr_type_to_slot_type(proto->return_type_info) : XR_SLOT_ANY;
        if (XR_SLOT_IS_FLOAT(rst)) {
            int64_t raw;
            memcpy(&raw, &result.f, sizeof(int64_t));
            return (XrJitResult){ raw, XR_TAG_F64 };
        }
    }
    return XR_JIT_RESULT(result);
}

/* ========== JIT→Any Function Call (OP_CALL) ========== */

// Called from JIT code via CALL_C for general function calls.
// jit_call_args[0] = raw closure pointer, jit_call_args[1..n] = raw arg values.
// extra_arg = nargs (low 8 bits).
// Tries JIT path first, falls back to VM interpreter.
XrJitResult xr_jit_call_func(XrCoroutine *coro, int64_t nargs_encoded) {
    int nargs = (int)(nargs_encoded & 0xFF);
    int64_t raw_closure = coro->jit_ctx->call_args[0];
    // Detect poison values from uninitialized JIT registers (OSR edge case)
    if (raw_closure == 0 || ((uint64_t)raw_closure >> 48) != 0)
        return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    // Class call: allocate instance + call constructor inline
    XrGCHeader *callee_gc = (XrGCHeader *)raw_closure;
    if (XR_GC_GET_TYPE(callee_gc) == XR_TCLASS) {
        XrClass *klass = (XrClass *)raw_closure;
        XrayIsolate *isolate = coro->isolate;
        if (!isolate) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

        // Allocate instance on coroutine heap (normal storage mode)
        XrInstance *instance = xr_instance_new(isolate, klass);
        if (!instance) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

        // Look up constructor method (cached symbol for speed)
        static int ctor_sym = -1;
        if (ctor_sym < 0) {
            XrSymbolTable *st = (XrSymbolTable *)isolate->symbol_table;
            ctor_sym = (int)xr_symbol_lookup_in_table(st, XR_KEYWORD_CONSTRUCTOR);
        }
        XrMethod *ctor = (ctor_sym > 0) ? xr_class_lookup_method(klass, ctor_sym) : NULL;

        if (ctor && ctor->type == XMETHOD_CLOSURE) {
            // User constructor: shift args right, insert instance as arg[0]
            for (int i = nargs; i > 0; i--)
                coro->jit_ctx->call_args[1 + i] = coro->jit_ctx->call_args[i];
            coro->jit_ctx->call_args[1] = (int64_t)(uintptr_t)instance;
            coro->jit_ctx->call_args[0] = (int64_t)(uintptr_t)ctor->as.closure;

            // Recursive call to handle constructor (JIT fast path + VM fallback)
            xr_jit_call_func(coro, (nargs + 1) & 0xFF);
        } else if (ctor && ctor->type == XMETHOD_PRIMITIVE) {
            // Native constructor: box args and call C function
            XrValue args[16];
            for (int i = 0; i < nargs && i < 15; i++)
                args[i] = jit_value_from_tag(coro->jit_ctx->call_args[1 + i], XR_TAG_I64);
            ctor->as.primitive(isolate, args, nargs);
        }

        // Return instance pointer regardless of constructor outcome
        return (XrJitResult){ (int64_t)(uintptr_t)instance, XR_TAG_PTR };
    }

    XrClosure *closure = (XrClosure *)raw_closure;
    if (!closure->proto) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    XrProto *proto = closure->proto;

    // On-demand JIT compilation for uncompiled callees.
    // Callbacks in JIT-compiled loops (filter/map/reduce) are small closures
    // that never reach the VM hot threshold. Compile them here on first call
    // to avoid expensive xr_vm_call_closure VM re-entry on every iteration.
    if (!proto->jit_entry && proto->deopt_count <= 3 && proto->bb_leaders
        && coro->isolate) {
        XirJitState *jit = coro->isolate->vm.jit;
        if (jit && jit->enabled) {
            // Synthesize or fix param_types for untyped params (default to i64).
            // Caller passes raw i64 values; this matches the VM fallback's
            // default tag (XR_TAG_I64) at line ~2340 below.
            // TFA may have created param_types but left params as NULL (ANY),
            // which is_jit_eligible rejects. Fill in missing param types.
            if (proto->numparams > 0 && !proto->type_feedback) {
                if (!proto->param_types) {
                    proto->param_types = (struct XrType **)xr_calloc(
                        proto->numparams, sizeof(struct XrType *));
                    if (proto->param_types)
                        proto->param_types_count = proto->numparams;
                }
                if (proto->param_types) {
                    for (int i = 0; i < proto->numparams; i++) {
                        if (i < proto->param_types_count && !proto->param_types[i])
                            proto->param_types[i] = xr_slot_type_to_type(XR_SLOT_I64);
                    }
                }
            }
            bool ok = xir_jit_try_compile(jit, proto);
            if (!ok)
                proto->deopt_count = 4;  // prevent retry on failure
        }
    }

    // JIT fast path: callee compiled (either pre-compiled or just-now compiled)
    if (proto->jit_entry) {
        void *saved_proto = coro->jit_ctx->call_proto;
        void *saved_closure = coro->jit_ctx->call_closure;
        coro->jit_ctx->call_proto = proto;
        coro->jit_ctx->call_closure = closure;
        XrJitResult ret = ((XirJitFn)proto->jit_entry)(
            (intptr_t)coro, &coro->jit_ctx->call_args[1]);
        coro->jit_ctx->call_proto = saved_proto;
        coro->jit_ctx->call_closure = saved_closure;
        if (ret.payload != XIR_DEOPT_MARKER) return ret;
        // Callee deoptimized — clear stale deopt_id so the outer JIT
        // caller's post-CALL_C CBNZ check doesn't see it and mistakenly
        // deopt the outer function with the callee's deopt_id.
        coro->jit_ctx->deopt_id = 0;
        // Check if callee threw (jit_exception set by xr_jit_throw)
        if (coro->jit_ctx->exception) return XR_JIT_NULL();
    }

    // VM fallback: BOX args and call via interpreter
    XrayIsolate *isolate = coro->isolate;
    if (!isolate) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    XrValue args[16];
    for (int i = 0; i < nargs && i < 15; i++) {
        int64_t raw = coro->jit_ctx->call_args[1 + i];
        uint8_t tag = XR_TAG_I64;
        if (proto->param_types && i < proto->param_types_count && proto->param_types[i])
            tag = slot_type_to_xr_tag(xr_type_to_slot_type(proto->param_types[i]));
        args[i] = jit_value_from_tag(raw, tag);
    }

    // Suppress VM uncaught-exception print (JIT caller handles exceptions)
    bool saved_suppress = xr_isolate_get_suppress_exception_print(isolate);
    xr_isolate_set_suppress_exception_print(isolate, true);
    XrValue result = xr_vm_call_closure(isolate, closure, args, nargs);
    xr_isolate_set_suppress_exception_print(isolate, saved_suppress);

    // Check if callee threw an exception (propagate to JIT catch handler)
    if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
        coro->jit_ctx->exception = (void *)coro->vm_ctx.current_exception.ptr;
        coro->vm_ctx.current_exception = XR_NULL_VAL;
        return XR_JIT_NULL();
    }

    // UNBOX result based on return type
    {
        uint8_t rst = proto->return_type_info
            ? xr_type_to_slot_type(proto->return_type_info) : XR_SLOT_ANY;
        if (XR_SLOT_IS_FLOAT(rst)) {
            int64_t raw;
            memcpy(&raw, &result.f, sizeof(int64_t));
            return (XrJitResult){ raw, XR_TAG_F64 };
        }
    }
    return XR_JIT_RESULT(result);
}

/* ========== OSR Entry Bridge ========== */

// OSR calling convention: same as normal — x0=coro, x1=pointer to int64_t values[]
bool xir_jit_osr_enter(void *osr_entry, XrCoroutine *coro,
                        int64_t *values, uint8_t return_type,
                        XrValue *result) {
    if (!osr_entry || !result) return false;

    XrJitResult jr = ((XirJitFn)osr_entry)((intptr_t)coro, values);

    if (jr.payload == XIR_DEOPT_MARKER) {
        return false;
    }

    uint8_t tag = (uint8_t)jr.tag;
    if (tag == XR_RTAG_UNKNOWN) {
        *result = jit_value_from_tag(jr.payload, slot_type_to_xr_tag(return_type));
    } else {
        *result = jit_value_from_tag(jr.payload, tag);
    }
    return true;
}

/* ========== Background Result Installation ========== */

/*
 * Install a completed background compilation result into proto fields.
 * Called from both OP_CALL and OSR paths when jit_entry_pending is ready.
 * Safe to call from any worker thread — proto fields are only written once
 * (bg thread never writes proto fields directly; only jit_entry_pending).
 *
 * Uses CAS on jit_entry_pending to ensure exactly-once installation
 * when multiple coroutines race to install the same result.
 */
void xir_jit_install_bg_result(XrProto *proto) {
    void *pending = atomic_load_explicit(&proto->jit_entry_pending,
                                          memory_order_acquire);
    if (!pending || (uintptr_t)pending <= 1) return;

    // CAS to claim installation (prevent double-install from racing workers)
    if (!atomic_compare_exchange_strong_explicit(
            &proto->jit_entry_pending, &pending, NULL,
            memory_order_acq_rel, memory_order_acquire)) {
        // Another thread installed it — jit_entry should be set now
        return;
    }

    XirBgResult *bgr = (XirBgResult *)pending;
    if (proto->stack_map) xr_free(proto->stack_map);
    proto->stack_map = bgr->stack_map;
    if (proto->deopt_table) xr_free(proto->deopt_table);
    proto->deopt_table = bgr->deopt_table;
    proto->ndeopt = bgr->ndeopt;
    if (proto->osr_entries) xr_free(proto->osr_entries);
    proto->osr_entries = bgr->osr_entries;
    proto->nosr = bgr->nosr;
    proto->jit_opt_level = bgr->opt_level;
    proto->jit_fast_entry = bgr->fast_entry;
    proto->jit_resume_entry = bgr->resume_entry;
    // jit_entry must be written LAST — release fence ensures all metadata
    // stores above are visible to other cores before they observe jit_entry.
    // On ARM64 (weak memory order), plain stores can be reordered without this.
    atomic_thread_fence(memory_order_release);
    proto->jit_entry = bgr->code;
    xr_free(bgr);
}

/* ========== OSR Trigger (called from VM at loop back-edges) ========== */

int xir_jit_osr_trigger(XirJitState *jit, XrProto *proto, XrCoroutine *coro,
                         uint32_t bc_pc, XrValue *base, int maxstack,
                         uint8_t return_type, XrValue *result) {
    if (!jit || !jit->enabled || !proto || !result) return XIR_JIT_DEOPT;

    // Step 1: ensure compiled code is available
    if (!proto->jit_entry) {
        // Check if background compilation finished
        if (jit->bg_queue) {
            void *pending = atomic_load_explicit(
                &proto->jit_entry_pending, memory_order_acquire);
            if (pending && (uintptr_t)pending > 1) {
                xir_jit_install_bg_result(proto);
            }
        }
        // Still no jit_entry → try sync compile
        if (!proto->jit_entry) {
            if (!xir_jit_try_compile(jit, proto))
                return XIR_JIT_DEOPT;
        }
    }

    // Step 2: find OSR entry matching this loop header
    if (!proto->osr_entries || proto->nosr == 0) return XIR_JIT_DEOPT;

    XirOsrEntry *entries = (XirOsrEntry *)proto->osr_entries;
    XirOsrEntry *match = NULL;
    for (uint32_t i = 0; i < proto->nosr; i++) {
        if (entries[i].bc_offset == bc_pc) {
            match = &entries[i];
            break;
        }
    }
    if (!match) {
        return XIR_JIT_DEOPT;
    }

    // Step 3: build values array from interpreter registers
    // values[slot] = raw payload of R(slot)
    int nslots = maxstack < 256 ? maxstack : 256;
    int64_t values[256];
    for (int i = 0; i < nslots; i++) {
        values[i] = base[i].i;
    }

    // Step 4: compute absolute OSR entry address
    void *osr_entry = (uint8_t *)proto->jit_entry + match->entry_offset;

    // Step 5: set closure pointer for upvalue access
    // The closure is in the current call frame
    coro->jit_ctx->call_proto = proto;

    // Step 6: enter JIT at loop header
    int saved_id = coro->id;  // save before JIT call (coro may be resumed by another worker)
    XrJitResult osr_jr = ((XirJitFn)osr_entry)((intptr_t)coro, values);

    if (osr_jr.payload == XIR_DEOPT_MARKER) {
        // JIT ran but deoptimized mid-function. It may have produced
        // side effects (spawned coroutines, pushed to arrays, etc.).
        // Recover interpreter state from deopt info so the interpreter
        // continues from the deopt point, not from the OSR entry.
        int32_t deopt_pc = xir_jit_deopt_recover(coro, base, maxstack);
        if (deopt_pc >= 0) {
            coro->jit_ctx->osr_deopt_pc = deopt_pc;
        } else {
            coro->jit_ctx->osr_deopt_pc = -1;
        }
        return XIR_JIT_DEOPT;
    }

    if (osr_jr.payload == XIR_SUSPEND_MARKER) {
        // JIT suspended at channel/await blocking point during OSR.
        // resume_entry/proto already set by XIR_SUSPEND codegen.
        // Return XIR_JIT_SUSPEND on the stack — no racy side-channel needed.
        *result = xr_null();
        return XIR_JIT_SUSPEND;
    }

    uint8_t osr_tag = (uint8_t)osr_jr.tag;
    if (osr_tag == XR_RTAG_UNKNOWN) {
        *result = jit_value_from_tag(osr_jr.payload, slot_type_to_xr_tag(return_type));
    } else {
        *result = jit_value_from_tag(osr_jr.payload, osr_tag);
    }
    return XIR_JIT_OK;
}

/* ========== Print Helper ========== */

// Called from JIT codegen for XIR_RT_ARRAY_NEW.
// extra_arg = capacity
// Returns raw ptr to XrArray.
XrJitResult xr_jit_rt_array_new(XrCoroutine *coro, int64_t capacity) {
    XrArray *arr = xr_array_with_capacity(coro, (int)capacity);
    return XR_JIT_PTR(arr);
}

// Called from JIT codegen for XIR_RT_ARRAY_PUSH.
// call_args[0] = array ptr, call_args[1] = value raw payload
// extra_arg = val_slot_type
XrJitResult xr_jit_rt_array_push(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrArray *arr = (XrArray *)(uintptr_t)coro->jit_ctx->call_args[0];
    if (!arr) return XR_JIT_OK();
    // Guard: type mismatch can pass non-array objects here
    if (XR_GC_GET_TYPE((XrGCHeader *)arr) != XR_TARRAY) return XR_JIT_OK();
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                      coro->jit_ctx->call_arg_tags[1]);
    xr_array_push(arr, val);
    return XR_JIT_OK();
}

// Called from JIT codegen for XIR_RT_ARRAY_LEN.
// call_args[0] = array ptr
// Returns array length as i64.
XrJitResult xr_jit_rt_array_len(XrCoroutine *coro, int64_t unused) {
    (void)unused;
    XrArray *arr = (XrArray *)(uintptr_t)coro->jit_ctx->call_args[0];
    if (!arr) return XR_JIT_INT(0);
    return XR_JIT_INT((int64_t)arr->length);
}

// Called from JIT codegen for XIR_RT_MAP_NEW.
// extra_arg = capacity (unused, always creates default-sized map)
// Returns raw ptr to XrMap.
XrJitResult xr_jit_rt_map_new(XrCoroutine *coro, int64_t capacity) {
    (void)capacity;
    XrMap *map = xr_map_new(coro);
    return XR_JIT_PTR(map);
}

// Called from JIT via CALL_C for polymorphic OP_ADD.
// call_args[0] = lhs raw payload, call_args[1] = rhs raw payload
// Handles: int+int, float+float, mixed numeric, string concat.
XrJitResult xr_jit_rt_add(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                     coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                     coro->jit_ctx->call_arg_tags[1]);

    // Integer addition
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XrValue r; r.descriptor = 0; r.tag = XR_TAG_I64;
        r.i = (int64_t)((uint64_t)XR_TO_INT(vb) + (uint64_t)XR_TO_INT(vc));
        return XR_JIT_RESULT(r);
    }
    // Float promotion
    {
        double nb = 0, nc = 0;
        if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
            XrValue r; r.descriptor = 0; r.tag = XR_TAG_F64;
            r.f = nb + nc;
            int64_t raw; memcpy(&raw, &r.f, sizeof(double));
            return (XrJitResult){ raw, XR_TAG_F64 };
        }
    }
    // String concatenation
    XrayIsolate *isolate = coro->isolate;
    XrString *str_b = xr_value_to_string(isolate, vb);
    XrString *str_c = xr_value_to_string(isolate, vc);
    XrStrBuf *sb = xr_strbuf_tmp(isolate);
    xr_strbuf_append_str(sb, str_b);
    xr_strbuf_append_str(sb, str_c);
    XrValue result = xr_string_value(xr_strbuf_to_string(sb));
    return XR_JIT_RESULT(result);
}

// call_args[0] = lhs raw payload, call_args[1] = rhs raw payload
// Handles: int-int, mixed numeric (float promotion).
XrJitResult xr_jit_rt_sub(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                     coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                     coro->jit_ctx->call_arg_tags[1]);
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XrValue r; r.descriptor = 0; r.tag = XR_TAG_I64;
        r.i = (int64_t)((uint64_t)XR_TO_INT(vb) - (uint64_t)XR_TO_INT(vc));
        return XR_JIT_RESULT(r);
    }
    double nb = 0, nc = 0;
    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
        XrValue r; r.descriptor = 0; r.tag = XR_TAG_F64;
        r.f = nb - nc;
        int64_t raw; memcpy(&raw, &r.f, sizeof(double));
        return (XrJitResult){ raw, XR_TAG_F64 };
    }
    return XR_JIT_NULL();
}

XrJitResult xr_jit_rt_mul(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                     coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                     coro->jit_ctx->call_arg_tags[1]);
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XrValue r; r.descriptor = 0; r.tag = XR_TAG_I64;
        r.i = (int64_t)((uint64_t)XR_TO_INT(vb) * (uint64_t)XR_TO_INT(vc));
        return XR_JIT_RESULT(r);
    }
    double nb = 0, nc = 0;
    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
        XrValue r; r.descriptor = 0; r.tag = XR_TAG_F64;
        r.f = nb * nc;
        int64_t raw; memcpy(&raw, &r.f, sizeof(double));
        return (XrJitResult){ raw, XR_TAG_F64 };
    }
    return XR_JIT_NULL();
}

XrJitResult xr_jit_rt_div(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                     coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                     coro->jit_ctx->call_arg_tags[1]);
    // Division always produces float
    double nb = 0, nc = 0;
    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
        if (nc == 0.0) {
            XrValue exc = xr_exception_newf(coro->isolate,
                XR_ERR_DIV_BY_ZERO, "division by zero");
            coro->jit_ctx->exception = (void *)exc.ptr;
            return XR_JIT_NULL();
        }
        XrValue r; r.descriptor = 0; r.tag = XR_TAG_F64;
        r.f = nb / nc;
        int64_t raw; memcpy(&raw, &r.f, sizeof(double));
        return (XrJitResult){ raw, XR_TAG_F64 };
    }
    XrValue exc = xr_exception_newf(coro->isolate,
        XR_ERR_DIV_BY_ZERO, "division by zero");
    coro->jit_ctx->exception = (void *)exc.ptr;
    return XR_JIT_NULL();
}

XrJitResult xr_jit_rt_mod(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0],
                                     coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1],
                                     coro->jit_ctx->call_arg_tags[1]);
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        int64_t b = XR_TO_INT(vb), c = XR_TO_INT(vc);
        if (c == 0) {
            XrValue exc = xr_exception_newf(coro->isolate,
                XR_ERR_DIV_BY_ZERO, "division by zero");
            coro->jit_ctx->exception = (void *)exc.ptr;
            return XR_JIT_NULL();
        }
        XrValue r; r.descriptor = 0; r.tag = XR_TAG_I64;
        r.i = b % c;
        return XR_JIT_RESULT(r);
    }
    double nb = 0, nc = 0;
    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
        if (nc == 0.0) {
            XrValue exc = xr_exception_newf(coro->isolate,
                XR_ERR_DIV_BY_ZERO, "division by zero");
            coro->jit_ctx->exception = (void *)exc.ptr;
            return XR_JIT_NULL();
        }
        XrValue r; r.descriptor = 0; r.tag = XR_TAG_F64;
        r.f = fmod(nb, nc);
        int64_t raw; memcpy(&raw, &r.f, sizeof(double));
        return (XrJitResult){ raw, XR_TAG_F64 };
    }
    XrValue exc2 = xr_exception_newf(coro->isolate,
        XR_ERR_DIV_BY_ZERO, "division by zero");
    coro->jit_ctx->exception = (void *)exc2.ptr;
    return XR_JIT_NULL();
}

// Called from JIT via CALL_C for OP_TYPEOF.
// jit_call_args[0] = value raw payload
// extra_arg = xr_tag of the value
XrJitResult xr_jit_typeof(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    int64_t raw = coro->jit_ctx->call_args[0];
    XrValue val = jit_value_from_tag(raw, coro->jit_ctx->call_arg_tags[0]);
    return XR_JIT_INT((int64_t)xr_value_typeid(val));
}

// Called from JIT via CALL_C for OP_PRINT.
// jit_call_args[0] = value raw payload
// extra_arg encoding: [15:8]=val_xr_tag, bit1=add_space, bit0=newline
XrJitResult xr_jit_print(XrCoroutine *coro, int64_t extra_arg) {
    int newline   = (int)(extra_arg & 1);
    int add_space = (int)((extra_arg >> 1) & 1);

    int64_t raw = coro->jit_ctx->call_args[0];
    XrValue val = jit_value_from_tag(raw, coro->jit_ctx->call_arg_tags[0]);

    if (add_space) printf(" ");

    XrayIsolate *isolate = coro->isolate;
    if (isolate) {
        XrString *s = xr_value_to_string(isolate, val);
        if (s) printf("%s", s->data);
    }
    if (newline) printf("\n");
    return XR_JIT_OK();
}

/* ========== Structured Concurrency Helpers ========== */

// Called from JIT via CALL_C for OP_SCOPE_ENTER.
// Creates a new scope context and pushes it onto the coro's scope stack.
// Returns 0.
XrJitResult xr_jit_scope_enter(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    if (!coro) return XR_JIT_OK();

    atomic_store(&coro->wait_count, 0);
    atomic_store(&coro->any_done, false);

    XrScopeContext *scope = (XrScopeContext *)xr_malloc(sizeof(XrScopeContext));
    if (scope) {
        atomic_store(&scope->count, 0);
        scope->parent = coro->current_scope;
        coro->current_scope = scope;
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_SCOPE_EXIT.
// If all children done (wait_count==0): unlinks and frees scope, returns 0.
// If children still running: returns XIR_DEOPT_MARKER to deopt back to interpreter.
XrJitResult xr_jit_scope_exit(XrCoroutine *coro, int64_t extra_arg) {
    (void)extra_arg;
    if (!coro) return XR_JIT_OK();

    XrScopeContext *scope = coro->current_scope;
    if (!scope) return XR_JIT_OK();

    if (atomic_load(&coro->wait_count) > 0) {
        // Children still running — deopt to interpreter for blocking wait
        return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
    }

    // All children done — safe to unlink and free scope
    coro->current_scope = scope->parent;
    xr_free(scope);
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_SPAWN_CONT.
// jit_call_args[0] = closure raw (XrValue tagged as PTR)
// jit_call_args[1..nargs] = argument values (raw payloads)
// extra_arg = nargs | (fire_and_forget << 7)
// Creates the coroutine, sets up scope, schedules via xr_runtime_spawn.
// Returns the coroutine XrValue raw payload (for R[A]).
// Note: skips continuation stealing for simplicity — coro scheduled normally.
XrJitResult xr_jit_spawn_cont(XrCoroutine *coro, int64_t extra_arg) {
    int c_raw = (int)(extra_arg & 0xFF);
    bool fire_and_forget = (c_raw & 0x80) != 0;
    int nargs = c_raw & 0x7F;

    XrValue fn_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_closure(fn_val)) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    struct XrClosure *closure = xr_value_to_closure(fn_val);
    XrProto *proto = closure->proto;

    if (!proto->is_coro_safe) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
    if (nargs != proto->numparams) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    // Build args from call_args[1..nargs].
    // Primary tag source: call_arg_tags[] (precise compile-time tags from codegen).
    // Fallback: proto->param_types for truly unknown (any) types.
    XrValue args[16];
    for (int i = 0; i < nargs && i < 16; i++) {
        int64_t raw = coro->jit_ctx->call_args[1 + i];
        uint8_t tag = coro->jit_ctx->call_arg_tags[1 + i];
        if (tag == XR_RTAG_UNKNOWN &&
            proto->param_types && i < proto->param_types_count && proto->param_types[i])
            tag = slot_type_to_xr_tag(xr_type_to_slot_type(proto->param_types[i]));
        args[i] = jit_value_from_tag(raw, tag);
    }

    XrayIsolate *isolate = coro->isolate;
    if (!isolate) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    XrCoroutine *child = xr_coro_create(isolate, closure, args, nargs,
                                         NULL, NULL, 0);
    if (!child) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    // Create Task handle (mirrors vm_spawn)
    XrTask *task = xr_task_create(coro, child);
    if (!task) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };

    if (fire_and_forget)
        child->gc_flags |= XR_CORO_GC_RECYCLABLE;

    // Setup scope tracking (waiter on task, not coro)
    XrScopeContext *scope = coro->current_scope;
    if (scope) {
        child->parent_scope = scope;
        atomic_fetch_add_explicit(&scope->count, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&coro->wait_count, 1, memory_order_relaxed);
        task->waiter = coro;
        task->waiter_index = -2;  // scope mode
    }

    // Schedule the coroutine (no continuation stealing in JIT path)
    XrRuntime *runtime = (XrRuntime *)xr_isolate_get_vm_state(isolate)->runtime;
    if (!runtime) return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
    xr_runtime_spawn(runtime, child);

    XrValue result = xr_value_from_task(task);
    return XR_JIT_VAL(result);
}

// Called from JIT via CALL_C for OP_AWAIT.
// jit_call_args[0] = coro raw (XrValue tagged as PTR)
// extra_arg = discard_result flag
// Fast path: if Task is done, return result immediately.
// Slow path: return XIR_DEOPT_MARKER to trigger deopt to interpreter.
// All await coordination lives on XrTask; raw coro await no longer supported.
XrJitResult xr_jit_await(XrCoroutine *coro, int64_t extra_arg) {
    int discard_result = (int)(extra_arg & 0xFF);

    int64_t raw0 = coro->jit_ctx->call_args[0];
    XrValue val = jit_value_from_tag(raw0, XR_TAG_PTR);

    // Task fast path: return result if already done
    if (xr_value_is_task(val)) {
        XrTask *task = xr_value_to_task(val);
        uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
        if (tstate == XR_TASK_COMPLETED) {
            XrValue res = discard_result ? xr_null()
                : xr_deep_copy_to_coro(coro->isolate, task->result, coro);
            return XR_JIT_RESULT(res);
        }
        if (tstate == XR_TASK_FAILED || tstate == XR_TASK_CANCELLED) {
            return XR_JIT_NULL();
        }
    }

    // Not done or not a Task → deopt to interpreter for blocking wait
    return (XrJitResult){ XIR_DEOPT_MARKER, 0 };
}

/*
 * JIT suspend helper for AWAIT blocking path.
 *
 * Called from XIR_SUSPEND codegen AFTER live registers are saved to
 * coro->jit_suspend_state. Performs the CAS coordination on Task to
 * register this coro as waiter.
 *
 * The Task ptr was stored in jit_call_args[0] by the preceding CALL_C
 * (xr_jit_await) which returned DEOPT_MARKER to signal "not done".
 * extra_arg encodes: bits[0:7] = discard_result.
 *
 * Returns:
 *   0 → blocked (JIT returns SUSPEND_MARKER, worker yields coro)
 *   1 → race: task completed during CAS, result in jit_suspend_state.result
 *       (JIT reloads regs and continues inline)
 */
XrJitResult xr_jit_await_block(XrCoroutine *coro, int64_t extra_arg) {
    int discard_result = (int)(extra_arg & 0xFF);

    /* Retrieve Task pointer from the preceding xr_jit_await call.
     * The fast-path helper stored it in call_args[0]. */
    int64_t raw0 = coro->jit_ctx->call_args[0];
    XrValue val = jit_value_from_tag(raw0, XR_TAG_PTR);
    if (!xr_value_is_task(val)) return (XrJitResult){ 0, 0 }; // safety fallback
    XrTask *task = xr_value_to_task(val);

    // Re-check: task may have completed between fast-path and here
    uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
    if (tstate == XR_TASK_COMPLETED || tstate == XR_TASK_FAILED ||
        tstate == XR_TASK_CANCELLED) {
        // Race win: result ready — store into suspend_regs[23] for inline resume
        XrValue res = xr_null();
        if (tstate == XR_TASK_COMPLETED && !discard_result) {
            res = xr_deep_copy_to_coro(coro->isolate, task->result, coro);
        }
        coro->jit_suspend_state.result = res.i;
        coro->jit_suspend_state.result_tag = res.tag;
        return (XrJitResult){ 1, 0 }; // not blocked — JIT continues
    }

    // CAS: NONE → WAITING (mirror vm_await logic in xvm_cold_paths.c:2439)
    __atomic_store_n(&task->waiter_index, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&task->waiter, coro, __ATOMIC_RELEASE);

    int expected = XR_AWAIT_NONE;
    if (atomic_compare_exchange_strong_explicit(&task->await_state, &expected,
                                     XR_AWAIT_WAITING,
                                     memory_order_acq_rel, memory_order_acquire)) {
        /* Successfully registered as waiter — coro will be woken by
         * xr_task_wake_waiter when executor completes. */
        atomic_store_explicit(&coro->await_task, task, memory_order_release);
        uint32_t old_flags = xr_coro_flags_load(coro);
        uint32_t new_flags = xr_coro_set_wait_reason_flags(old_flags,
                              XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
        atomic_store_explicit(&coro->flags, new_flags, memory_order_release);
        return (XrJitResult){ 0, 0 }; // blocked — JIT saves resume info and returns SUSPEND_MARKER
    }

    if (expected == XR_AWAIT_RESOLVED) {
        /* Race: executor completed between our state check and CAS.
         * Result already in task->result. */
        __atomic_store_n(&task->waiter, NULL, __ATOMIC_RELAXED);
        XrValue res = xr_null();
        if (!discard_result) {
            res = xr_deep_copy_to_coro(coro->isolate, task->result, coro);
        }
        coro->jit_suspend_state.result = res.i;
        coro->jit_suspend_state.result_tag = res.tag;
        atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
        return (XrJitResult){ 1, 0 }; // not blocked — JIT continues
    }

    // XR_AWAIT_WAITING: concurrent await on same task (overwrite waiter)
    atomic_store_explicit(&coro->await_task, task, memory_order_release);
    uint32_t old_flags2 = xr_coro_flags_load(coro);
    uint32_t new_flags2 = xr_coro_set_wait_reason_flags(old_flags2,
                          XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
    atomic_store_explicit(&coro->flags, new_flags2, memory_order_release);
    return (XrJitResult){ 0, 0 }; // blocked
}
