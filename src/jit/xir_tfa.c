/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_tfa.c - Type Flow Analysis implementation
 *
 * KEY CONCEPT:
 *   Worklist-driven fixed-point iteration over function summaries.
 *   Each function summary maps param types → return type.
 *   When call-site argument types change, callee is re-enqueued.
 *
 * ALGORITHM:
 *   1. Seed: register all functions, set typed params from annotations,
 *      untyped params start as BOTTOM
 *   2. Collect: scan bytecode for CALL instructions, record call sites
 *      with argument types inferred from caller's context
 *   3. Propagate: for each callee on worklist, join all call-site arg
 *      types into param summary. If any param type widens, re-analyze
 *      callee's return type and enqueue callers.
 *   4. Converge: repeat until no changes or max iterations reached
 */

#include "xir_tfa.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype_feedback.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include <stdio.h>
#include <string.h>
#include "../base/xmalloc.h"

/* ========== Initialization ========== */

void tfa_init(TfaState *tfa) {
    XR_DCHECK(tfa != NULL, "tfa_init: NULL tfa");
    memset(tfa, 0, sizeof(TfaState));

    tfa->summary_cap = TFA_INIT_FUNCS;
    tfa->summaries = (TfaSummary *)xr_calloc(tfa->summary_cap, sizeof(TfaSummary));

    tfa->hash_size = TFA_INIT_HASH;
    tfa->hash_mask = tfa->hash_size - 1;
    tfa->hash_table = (uint32_t *)xr_calloc(tfa->hash_size, sizeof(uint32_t));

    tfa->call_cap = TFA_INIT_CALLS;
    tfa->calls = (TfaCallSite *)xr_calloc(tfa->call_cap, sizeof(TfaCallSite));

    tfa->worklist_cap = TFA_INIT_FUNCS;
    tfa->worklist = (TfaSummary **)xr_calloc(tfa->worklist_cap, sizeof(TfaSummary *));

    if (!tfa->summaries || !tfa->hash_table || !tfa->calls || !tfa->worklist) {
        tfa_free(tfa);
        return;
    }

    tfa->worklist_head = 0;
    tfa->worklist_tail = 0;
}

void tfa_free(TfaState *tfa) {
    if (!tfa) return;
    xr_free(tfa->summaries);
    xr_free(tfa->hash_table);
    xr_free(tfa->calls);
    xr_free(tfa->worklist);
    memset(tfa, 0, sizeof(TfaState));
}

/* ========== Growth Helpers ========== */

static bool tfa_grow_summaries(TfaState *tfa) {
    uint32_t old_cap = tfa->summary_cap;
    uint32_t new_cap = old_cap * 2;
    if (!XR_REALLOC(tfa->summaries, new_cap * sizeof(TfaSummary)))
        return false;
    memset(tfa->summaries + old_cap, 0, (new_cap - old_cap) * sizeof(TfaSummary));
    tfa->summary_cap = new_cap;
    return true;
}

static bool tfa_grow_calls(TfaState *tfa) {
    uint32_t old_cap = tfa->call_cap;
    uint32_t new_cap = old_cap * 2;
    if (!XR_REALLOC(tfa->calls, new_cap * sizeof(TfaCallSite)))
        return false;
    memset(tfa->calls + old_cap, 0, (new_cap - old_cap) * sizeof(TfaCallSite));
    tfa->call_cap = new_cap;
    return true;
}

static bool tfa_grow_worklist(TfaState *tfa) {
    uint32_t old_cap = tfa->worklist_cap;
    uint32_t new_cap = old_cap * 2;
    if (!XR_REALLOC(tfa->worklist, new_cap * sizeof(TfaSummary *)))
        return false;
    memset(tfa->worklist + old_cap, 0, (new_cap - old_cap) * sizeof(TfaSummary *));
    tfa->worklist_cap = new_cap;
    return true;
}

// Rehash: rebuild hash table with new size (must be power of 2)
static bool tfa_rehash(TfaState *tfa, uint32_t new_size) {
    uint32_t *new_ht = (uint32_t *)xr_calloc(new_size, sizeof(uint32_t));
    if (!new_ht) return false;
    uint32_t new_mask = new_size - 1;

    for (uint32_t i = 0; i < tfa->nsummary; i++) {
        uintptr_t v = (uintptr_t)tfa->summaries[i].proto;
        uint32_t h = (uint32_t)((v >> 4) * 2654435761u) & new_mask;
        for (uint32_t probe = 0; probe < new_size; probe++) {
            uint32_t idx = (h + probe) & new_mask;
            if (new_ht[idx] == 0) {
                new_ht[idx] = i + 1;
                break;
            }
        }
    }

    xr_free(tfa->hash_table);
    tfa->hash_table = new_ht;
    tfa->hash_size = new_size;
    tfa->hash_mask = new_mask;
    return true;
}

/* ========== Hash Helper ========== */

static inline uint32_t tfa_hash_ptr(const TfaState *tfa, const void *ptr) {
    uintptr_t v = (uintptr_t)ptr;
    v = (v >> 4) * 2654435761u;
    return (uint32_t)(v & tfa->hash_mask);
}

/* ========== Summary Management ========== */

TfaSummary *tfa_lookup(TfaState *tfa, XrProto *proto) {
    if (!tfa || !proto || !tfa->hash_table) return NULL;
    uint32_t slot = tfa_hash_ptr(tfa, proto);
    for (uint32_t probe = 0; probe < tfa->hash_size; probe++) {
        uint32_t idx = (slot + probe) & tfa->hash_mask;
        uint32_t entry = tfa->hash_table[idx];
        if (entry == 0) return NULL;
        uint32_t si = entry - 1;
        if (si < tfa->nsummary && tfa->summaries[si].proto == proto)
            return &tfa->summaries[si];
    }
    return NULL;
}

TfaSummary *tfa_register_func(TfaState *tfa, XrProto *proto) {
    if (!tfa || !proto) return NULL;

    // Check if already registered
    TfaSummary *existing = tfa_lookup(tfa, proto);
    if (existing) return existing;

    // Grow summaries if needed
    if (tfa->nsummary >= tfa->summary_cap) {
        if (!tfa_grow_summaries(tfa)) return NULL;
    }

    // Grow worklist to match summary capacity
    if (tfa->worklist_cap < tfa->summary_cap) {
        if (!tfa_grow_worklist(tfa)) return NULL;
    }

    // Rehash if load factor > 0.7
    if (tfa->nsummary * 10 >= tfa->hash_size * 7) {
        if (!tfa_rehash(tfa, tfa->hash_size * 2)) return NULL;
    }

    uint32_t si = tfa->nsummary++;
    TfaSummary *s = &tfa->summaries[si];
    memset(s, 0, sizeof(TfaSummary));
    s->proto = proto;
    s->return_type = NULL;  // BOTTOM

    // Insert into hash table
    uint32_t slot = tfa_hash_ptr(tfa, proto);
    for (uint32_t probe = 0; probe < tfa->hash_size; probe++) {
        uint32_t idx = (slot + probe) & tfa->hash_mask;
        if (tfa->hash_table[idx] == 0) {
            tfa->hash_table[idx] = si + 1;
            break;
        }
    }

    // Seed parameter types from annotations (XrType* directly)
    uint8_t np = (proto->numparams < TFA_MAX_PARAMS)
                 ? (uint8_t)proto->numparams : TFA_MAX_PARAMS;
    s->nparam = np;

    for (uint8_t i = 0; i < np; i++) {
        if (proto->param_types && i < proto->param_types_count && proto->param_types[i]) {
            s->param_types[i] = proto->param_types[i];  // direct canonical pointer
        } else {
            s->param_types[i] = NULL;  // BOTTOM
        }
    }

    // Seed return type
    if (proto->return_type_info) {
        s->return_type = proto->return_type_info;
    }

    return s;
}

/* ========== Worklist ========== */

static void tfa_enqueue(TfaState *tfa, TfaSummary *s) {
    XR_DCHECK(tfa != NULL, "tfa_enqueue: NULL tfa");
    XR_DCHECK(s != NULL, "tfa_enqueue: NULL summary");
    if (s->on_worklist) return;
    if (tfa->worklist_tail >= tfa->worklist_cap) {
        if (!tfa_grow_worklist(tfa)) return;
    }
    s->on_worklist = true;
    tfa->worklist[tfa->worklist_tail++] = s;
}

static TfaSummary *tfa_dequeue(TfaState *tfa) {
    if (tfa->worklist_head >= tfa->worklist_tail) return NULL;
    TfaSummary *s = tfa->worklist[tfa->worklist_head++];
    s->on_worklist = false;
    return s;
}

/* ========== Call Sites ========== */

void tfa_add_call(TfaState *tfa, TfaSummary *caller, TfaSummary *callee,
                  XrType *const *arg_types, uint8_t nargs) {
    if (!tfa || !caller || !callee) return;
    if (tfa->ncall >= tfa->call_cap) {
        if (!tfa_grow_calls(tfa)) return;
    }

    TfaCallSite *cs = &tfa->calls[tfa->ncall++];
    cs->caller = caller;
    cs->callee = callee;
    cs->nargs = (nargs < TFA_MAX_PARAMS) ? nargs : TFA_MAX_PARAMS;
    memcpy(cs->arg_types, arg_types, cs->nargs * sizeof(XrType *));
    cs->next = callee->call_sites;
    callee->call_sites = cs;
}

/* ========== Return Type Inference ========== */

/*
 * Infer return type from bytecode by scanning for OP_RET instructions
 * and examining the types of their operands. This is a lightweight
 * approximation — we look at the proto's return_type_info first,
 * then fall back to examining the last few instructions.
 */
static XrType *tfa_infer_return_type(TfaSummary *s) {
    XrProto *proto = s->proto;

    // If already annotated with full type info, use it
    if (proto->return_type_info) {
        return proto->return_type_info;
    }

    // If type feedback available, use it
    if (proto->type_feedback) {
        uint8_t fb_tag = proto->type_feedback->return_type;
        if (fb_tag != 0) {
            if (fb_tag <= 8) return xr_type_new_int();
            if (fb_tag == 9 || fb_tag == 10) return xr_type_new_float();
            return xr_type_new_unknown();
        }
    }

    // If all params are numeric and function is small, likely returns numeric
    bool all_numeric = true;
    bool any_float = false;
    for (uint8_t i = 0; i < s->nparam; i++) {
        XrType *pt = s->param_types[i];
        if (!pt || pt == xr_type_new_unknown()) {
            all_numeric = false;
            break;
        }
        if (pt->kind == XR_KIND_FLOAT) any_float = true;
        else if (pt->kind != XR_KIND_INT && pt->kind != XR_KIND_BOOL) {
            all_numeric = false;
            break;
        }
    }

    if (all_numeric && s->nparam > 0) {
        return any_float ? xr_type_new_float() : xr_type_new_int();
    }

    return xr_type_new_unknown();
}

/* ========== Fixed-Point Solver ========== */

void tfa_solve(TfaState *tfa) {
    if (!tfa || tfa->nsummary == 0) return;

    // Phase 1: Enqueue all functions with unresolved types
    for (uint32_t i = 0; i < tfa->nsummary; i++) {
        TfaSummary *s = &tfa->summaries[i];
        bool has_bottom = false;
        for (uint8_t p = 0; p < s->nparam; p++) {
            if (s->param_types[p] == NULL) {  // NULL = BOTTOM
                has_bottom = true;
                break;
            }
        }
        if (has_bottom || s->return_type == NULL) {
            tfa_enqueue(tfa, s);
        }
    }

    // Phase 2: Fixed-point iteration
    uint32_t iter = 0;
    while (tfa->worklist_head < tfa->worklist_tail && iter < TFA_MAX_ITERATIONS) {
        iter++;

        // Reset worklist for next round
        uint32_t round_end = tfa->worklist_tail;

        while (tfa->worklist_head < round_end) {
            TfaSummary *s = tfa_dequeue(tfa);
            if (!s) break;

            bool changed = false;

            // Join argument types from all call sites targeting this function
            for (TfaCallSite *cs = s->call_sites; cs; cs = cs->next) {
                uint8_t n = (cs->nargs < s->nparam) ? cs->nargs : s->nparam;
                for (uint8_t p = 0; p < n; p++) {
                    XrType *old = s->param_types[p];
                    XrType *joined = tfa_join(old, cs->arg_types[p]);
                    if (joined != old) {
                        s->param_types[p] = joined;
                        changed = true;
                    }
                }
            }

            // Re-infer return type based on updated params
            XrType *new_ret = tfa_infer_return_type(s);
            if (new_ret != s->return_type) {
                s->return_type = new_ret;
                changed = true;
            }

            if (changed) {
                tfa->types_refined++;
                s->stable = false;

                /* Enqueue all callers of this function (their arg types may
                 * now flow through to other callees) */
                for (TfaCallSite *cs = s->call_sites; cs; cs = cs->next) {
                    if (cs->caller != s)
                        tfa_enqueue(tfa, cs->caller);
                }
            } else {
                s->stable = true;
            }

            s->iteration = iter;
        }
    }

    tfa->iterations = iter;
}

/* ========== Apply Results ========== */

void tfa_apply_results(TfaState *tfa) {
    if (!tfa) return;

    for (uint32_t i = 0; i < tfa->nsummary; i++) {
        TfaSummary *s = &tfa->summaries[i];
        XrProto *proto = s->proto;

        // Apply param types: directly assign XrType* to proto->param_types
        if (proto->numparams > 0) {
            bool any_refined = false;
            uint8_t np = (proto->numparams < TFA_MAX_PARAMS)
                         ? (uint8_t)proto->numparams : TFA_MAX_PARAMS;

            for (uint8_t p = 0; p < np; p++) {
                XrType *inferred = s->param_types[p];
                if (inferred && inferred != xr_type_new_unknown()) {
                    any_refined = true;
                    break;
                }
            }

            if (any_refined) {
                if (!proto->param_types) {
                    proto->param_types = (struct XrType **)xr_calloc(
                        proto->numparams, sizeof(struct XrType *));
                    if (proto->param_types)
                        proto->param_types_count = proto->numparams;
                }
                if (proto->param_types) {
                    for (uint8_t p = 0; p < np && p < proto->param_types_count; p++) {
                        if (proto->param_types[p] == NULL) {
                            XrType *inferred = s->param_types[p];
                            if (inferred && inferred != xr_type_new_unknown())
                                proto->param_types[p] = inferred;
                        }
                    }
                }
            }
        }

        // Apply return type
        if (s->return_type && s->return_type != xr_type_new_unknown()) {
            if (!proto->return_type_info) {
                proto->return_type_info = s->return_type;
            }
        }
    }
}

/* ========== Bytecode Scanning ========== */

/*
 * Scan a function's bytecode to collect call sites.
 * Tracks OP_CLOSURE → register mapping, then at OP_CALL/OP_CALLSELF
 * records which callee is invoked and with what argument types.
 *
 * Argument type inference is approximate:
 *   - If arg register was loaded from a typed local → use its slot_type
 *   - If arg register was loaded from OP_LOADI/OP_LOADF → use I64/F64
 *   - Otherwise → ANY
 */
#define TFA_REG_TRACK_MAX 256

static void tfa_scan_proto(TfaState *tfa, TfaSummary *caller, XrProto *proto) {
    if (!proto) return;

    uint32_t ncode = PROTO_CODE_COUNT(proto);
    if (ncode == 0) return;

    const XrInstruction *code = PROTO_CODE_BASE(proto);

    // Track which register holds which child proto (from OP_CLOSURE)
    XrProto *reg_proto[TFA_REG_TRACK_MAX];
    // Track inferred XrType* per register (NULL = unknown/any)
    XrType *reg_type[TFA_REG_TRACK_MAX];
    memset(reg_proto, 0, sizeof(reg_proto));
    for (int r = 0; r < TFA_REG_TRACK_MAX; r++)
        reg_type[r] = xr_type_new_unknown();

    // Seed param register types from caller summary
    if (caller) {
        for (uint32_t i = 0; i < caller->nparam && i < TFA_REG_TRACK_MAX; i++) {
            if (caller->param_types[i])
                reg_type[i] = caller->param_types[i];
        }
    }

    XrType *t_int   = xr_type_new_int();
    XrType *t_float = xr_type_new_float();
    XrType *t_bool  = xr_type_new_bool();
    XrType *t_top   = xr_type_new_unknown();
    XrType *t_str   = xr_type_new_string();

    for (uint32_t pc = 0; pc < ncode; pc++) {
        XrInstruction ins = code[pc];
        OpCode op = GET_OPCODE(ins);
        uint32_t a = GETARG_A(ins);

        switch (op) {
        case OP_CLOSURE: {
            uint16_t bx = GETARG_Bx(ins);
            if (a < TFA_REG_TRACK_MAX && bx < PROTO_PROTO_COUNT(proto)) {
                reg_proto[a] = PROTO_PROTO(proto, bx);
            }
            break;
        }

        case OP_LOADI:
            if (a < TFA_REG_TRACK_MAX) reg_type[a] = t_int;
            break;
        case OP_LOADF:
            if (a < TFA_REG_TRACK_MAX) reg_type[a] = t_float;
            break;
        case OP_LOADTRUE:
        case OP_LOADFALSE:
            if (a < TFA_REG_TRACK_MAX) reg_type[a] = t_bool;
            break;
        case OP_LOADNULL:
            if (a < TFA_REG_TRACK_MAX) reg_type[a] = t_top;
            break;
        case OP_LOADK: {
            if (a < TFA_REG_TRACK_MAX) {
                uint16_t bx = GETARG_Bx(ins);
                if (bx < PROTO_CONST_COUNT(proto)) {
                    XrValue kv = PROTO_CONST_FAST(proto, bx);
                    uint32_t tag = kv.tag;
                    if (tag <= 8)       reg_type[a] = t_int;
                    else if (tag <= 12) reg_type[a] = t_float;
                    else                reg_type[a] = t_str;
                } else {
                    reg_type[a] = t_top;
                }
            }
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_MOD: case OP_BAND: case OP_BOR: case OP_BXOR:
        case OP_SHL: case OP_SHR:
            if (a < TFA_REG_TRACK_MAX) {
                uint32_t b = GETARG_B(ins);
                uint32_t c = GETARG_C(ins);
                XrType *bt = (b < TFA_REG_TRACK_MAX) ? reg_type[b] : t_top;
                XrType *ct = (c < TFA_REG_TRACK_MAX) ? reg_type[c] : t_top;
                if (bt->kind == XR_KIND_INT && ct->kind == XR_KIND_INT) {
                    reg_type[a] = t_int;
                } else if (bt->kind == XR_KIND_FLOAT || ct->kind == XR_KIND_FLOAT) {
                    reg_type[a] = t_float;
                } else {
                    reg_type[a] = t_top;
                }
            }
            break;

        case OP_ADDI: case OP_SUBI:
            if (a < TFA_REG_TRACK_MAX) {
                uint32_t b = GETARG_B(ins);
                XrType *bt = (b < TFA_REG_TRACK_MAX) ? reg_type[b] : t_top;
                reg_type[a] = (bt->kind == XR_KIND_FLOAT) ? t_float : t_int;
            }
            break;

        case OP_MOVE:
            if (a < TFA_REG_TRACK_MAX) {
                uint32_t b = GETARG_B(ins);
                reg_type[a] = (b < TFA_REG_TRACK_MAX) ? reg_type[b] : t_top;
                reg_proto[a] = (b < TFA_REG_TRACK_MAX) ? reg_proto[b] : NULL;
            }
            break;

        case OP_CALL:
        case OP_CALL_KEEP: {
            uint32_t b = GETARG_B(ins);
            int nargs = (b > 0) ? (b - 1) : 0;

            XrProto *callee_proto = (a < TFA_REG_TRACK_MAX) ? reg_proto[a] : NULL;
            if (callee_proto) {
                TfaSummary *callee = tfa_lookup(tfa, callee_proto);
                if (!callee) callee = tfa_register_func(tfa, callee_proto);
                if (callee && caller) {
                    XrType *arg_types[TFA_MAX_PARAMS];
                    int n = (nargs < TFA_MAX_PARAMS) ? nargs : TFA_MAX_PARAMS;
                    for (int i = 0; i < n; i++) {
                        uint32_t ri = a + 1 + i;
                        arg_types[i] = (ri < TFA_REG_TRACK_MAX) ? reg_type[ri] : t_top;
                    }
                    tfa_add_call(tfa, caller, callee, arg_types, n);
                }
                XrType *ret = callee ? callee->return_type : NULL;
                reg_type[a] = ret ? ret : t_top;
                reg_proto[a] = NULL;
            }
            break;
        }

        case OP_CALLSELF: {
            uint32_t b = GETARG_B(ins);
            int nargs = (b > 0) ? (b - 1) : 0;

            if (caller) {
                XrType *arg_types[TFA_MAX_PARAMS];
                int n = (nargs < TFA_MAX_PARAMS) ? nargs : TFA_MAX_PARAMS;
                for (int i = 0; i < n; i++) {
                    uint32_t ri = a + 1 + i;
                    arg_types[i] = (ri < TFA_REG_TRACK_MAX) ? reg_type[ri] : t_top;
                }
                tfa_add_call(tfa, caller, caller, arg_types, n);
            }

            if (a < TFA_REG_TRACK_MAX) {
                XrType *ret = caller ? caller->return_type : NULL;
                reg_type[a] = ret ? ret : t_top;
            }
            break;
        }

        case OP_CMP_EQ: case OP_CMP_NE:
        case OP_CMP_EQ_STRICT: case OP_CMP_NE_STRICT:
        case OP_CMP_LT: case OP_CMP_LE:
        case OP_IS: case OP_ISNULL_SET:
            if (a < TFA_REG_TRACK_MAX) reg_type[a] = t_bool;
            break;

        case OP_NOT:
            if (a < TFA_REG_TRACK_MAX) reg_type[a] = t_bool;
            break;

        case OP_UNM:
        case OP_BNOT: {
            if (a < TFA_REG_TRACK_MAX) {
                uint32_t b = GETARG_B(ins);
                XrType *bt = (b < TFA_REG_TRACK_MAX) ? reg_type[b] : t_top;
                reg_type[a] = (bt->kind == XR_KIND_FLOAT) ? t_float : t_int;
            }
            break;
        }

        case OP_ADDK: case OP_SUBK: case OP_MULK: case OP_DIVK: case OP_MODK:
            if (a < TFA_REG_TRACK_MAX) {
                uint32_t b = GETARG_B(ins);
                XrType *bt = (b < TFA_REG_TRACK_MAX) ? reg_type[b] : t_top;
                reg_type[a] = (bt->kind == XR_KIND_FLOAT) ? t_float : t_int;
            }
            break;

        case OP_MULI:
            if (a < TFA_REG_TRACK_MAX) {
                uint32_t b = GETARG_B(ins);
                XrType *bt = (b < TFA_REG_TRACK_MAX) ? reg_type[b] : t_top;
                reg_type[a] = (bt->kind == XR_KIND_FLOAT) ? t_float : t_int;
            }
            break;

        case OP_STRBUF_FINISH:
            if (a < TFA_REG_TRACK_MAX) reg_type[a] = t_str;
            break;

        case OP_NEWJSON: case OP_NEWARRAY: case OP_NEWMAP: case OP_NEWSET:
            if (a < TFA_REG_TRACK_MAX) reg_type[a] = t_top;
            break;

        case OP_GETSHARED:
        case OP_GETBUILTIN:
            if (a < TFA_REG_TRACK_MAX) reg_type[a] = t_top;
            break;

        case OP_CALL_STATIC: {
            uint32_t b = GETARG_B(ins);
            int nargs = (b > 0) ? (b - 1) : 0;
            XrProto *callee_proto = (a < TFA_REG_TRACK_MAX) ? reg_proto[a] : NULL;
            if (callee_proto && caller) {
                TfaSummary *callee = tfa_lookup(tfa, callee_proto);
                if (!callee) callee = tfa_register_func(tfa, callee_proto);
                if (callee) {
                    XrType *arg_types[TFA_MAX_PARAMS];
                    int n = (nargs < TFA_MAX_PARAMS) ? nargs : TFA_MAX_PARAMS;
                    for (int i = 0; i < n; i++) {
                        uint32_t ri = a + 1 + i;
                        arg_types[i] = (ri < TFA_REG_TRACK_MAX) ? reg_type[ri] : t_top;
                    }
                    tfa_add_call(tfa, caller, callee, arg_types, n);
                }
            }
            if (a < TFA_REG_TRACK_MAX) {
                TfaSummary *callee = callee_proto ? tfa_lookup(tfa, callee_proto) : NULL;
                XrType *ret = callee ? callee->return_type : NULL;
                reg_type[a] = ret ? ret : t_top;
                reg_proto[a] = NULL;
            }
            break;
        }

        default:
            break;
        }
    }
}

/* ========== Module-Level Analysis Entry Point ========== */

void tfa_analyze_module(TfaState *tfa, XrProto *main_proto) {
    if (!tfa || !main_proto) return;

    tfa_init(tfa);

    // Phase 1: Register all protos recursively
    // Use a dynamically allocated stack to traverse the proto tree
    uint32_t stack_cap = 64;
    XrProto **stack = (XrProto **)xr_malloc(stack_cap * sizeof(XrProto *));
    if (!stack) return;
    uint32_t sp = 0;
    stack[sp++] = main_proto;

    while (sp > 0) {
        XrProto *p = stack[--sp];
        TfaSummary *s = tfa_register_func(tfa, p);
        if (!s) continue;

        // Push child protos
        uint32_t nchild = PROTO_PROTO_COUNT(p);
        for (uint32_t i = 0; i < nchild; i++) {
            XrProto *child = PROTO_PROTO(p, i);
            if (child && !tfa_lookup(tfa, child)) {
                if (sp >= stack_cap) {
                    uint32_t new_cap = stack_cap * 2;
                    if (!XR_REALLOC(stack, new_cap * sizeof(XrProto *)))
                        break;
                    stack_cap = new_cap;
                }
                stack[sp++] = child;
            }
        }
    }
    xr_free(stack);

    // Phase 2: Scan bytecode for call sites
    for (uint32_t i = 0; i < tfa->nsummary; i++) {
        TfaSummary *s = &tfa->summaries[i];
        tfa_scan_proto(tfa, s, s->proto);
    }

    // Phase 3: Fixed-point solve
    tfa_solve(tfa);

    // Phase 4: Apply inferred types to protos
    tfa_apply_results(tfa);
}

/* ========== Debug Dump ========== */

static const char *xrtype_name(XrType *t) {
    if (!t) return "⊥";
    switch (t->kind) {
        case XR_KIND_UNKNOWN:       return "unknown";
        case XR_KIND_INT:       return "int";
        case XR_KIND_FLOAT:     return "float";
        case XR_KIND_BOOL:      return "bool";
        case XR_KIND_STRING:    return "string";
        case XR_KIND_ARRAY:     return "Array";
        case XR_KIND_MAP:       return "Map";
        case XR_KIND_INSTANCE:  return "instance";
        case XR_KIND_FUNCTION:  return "function";
        case XR_KIND_VOID:      return "void";
        default:                return "?";
    }
}

void tfa_dump_stats(TfaState *tfa) {
    if (!tfa) return;

    fprintf(stderr, "[TFA] functions: %u, call sites: %u, "
            "iterations: %u, types refined: %u\n",
            tfa->nsummary, tfa->ncall,
            tfa->iterations, tfa->types_refined);

    for (uint32_t i = 0; i < tfa->nsummary; i++) {
        TfaSummary *s = &tfa->summaries[i];
        XrProto *proto = s->proto;
        const char *name = proto->name ? XR_STRING_CHARS(proto->name) : "?";

        fprintf(stderr, "  %s(", name);
        for (uint8_t p = 0; p < s->nparam; p++) {
            if (p > 0) fprintf(stderr, ", ");
            fprintf(stderr, "%s", xrtype_name(s->param_types[p]));
        }
        fprintf(stderr, ") -> %s", xrtype_name(s->return_type));
        if (s->stable) fprintf(stderr, " [stable]");
        fprintf(stderr, "\n");
    }
}
