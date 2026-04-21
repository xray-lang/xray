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
#include "xir_jit_internal.h"

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
    // Both first-compile and recompile (Tier1→Tier2) use this path so
    // that all bg tasks share the same XirBgTask snapshot construction.
    if (jit->bg_queue) {
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
    // Per-module: only analyze each module root once; newly loaded modules
    // (with a different root proto) trigger fresh analysis automatically.
    if (!is_recompile &&
        proto->numparams > 0 && !proto->param_types && !proto->type_feedback) {
        if (!jit->tfa) {
            jit->tfa = (TfaState *)xr_calloc(1, sizeof(TfaState));
        }
        if (jit->tfa) {
            XrProto *root = tfa_find_root(proto);
            if (!tfa_is_module_analyzed(jit->tfa, root)) {
                tfa_analyze_module(jit->tfa, proto);
            }
        }
    }

    // Discover dominant shape from module (once, after TFA or on first compile)
    if (!jit->dominant_shape) {
        // Walk up to module root via TFA summaries, or scan from this proto
        // For now: scan from the proto being compiled (covers its children)
        // and also scan TFA-registered protos if available
        if (jit->tfa && jit->tfa->n_analyzed_roots > 0) {
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
                    proto->param_types[i] = xr_slot_type_to_type(NULL, st);
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
            proto->return_type_info = xr_slot_type_to_type(NULL, fb_ret);
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
            proto->return_type_info = xr_slot_type_to_type(NULL, inferred_slot);
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

    // Lazy-allocate jit_suspend on first JIT entry (saves 320B per non-JIT coro)
    if (!coro->jit_suspend) {
        coro->jit_suspend = xr_calloc(1, sizeof(XrJitSuspendState));
        if (!coro->jit_suspend) return XIR_JIT_DEOPT;
    }

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
    // jit_suspend must exist if we were suspended (allocated in xir_jit_call)
    XR_DCHECK(coro->jit_suspend != NULL, "xir_jit_resume: NULL jit_suspend");

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
    // coro->jit_suspend.result by the waker (xr_jit_await_block
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
