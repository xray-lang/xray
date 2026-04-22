/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_builder_call.c - XIR translation for call/invoke/array operations
 *
 * KEY CONCEPT:
 *   Handles function calls, tail calls, method invocation, builtin
 *   dispatch, array/map creation and access opcodes in the
 *   bytecode-to-XIR translation pipeline.
 */

#include "xir_builder_internal.h"
#include "../base/xchecks.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_lookup.h"
#include "../runtime/class/xinstance.h"
#include "../vm/xic_method.h"

/*
 * Builtin method return type table, indexed by SymbolId.
 * Encoding: (rep + 1) so that 0 = unset = default PTR.
 * XR_REP_I64=0 collides with C zero-init; this encoding avoids that.
 * Only non-PTR primitive returns need explicit entries.
 * Used by INVOKE_METHOD / INVOKE_BUILTIN for precise return rep.
 */
#define BREP(r) ((r) + 1)
static const uint8_t builtin_return_rep[SYMBOL_BUILTIN_COUNT] = {
    // I64 returns: sizes, indices, conversions
    [SYMBOL_LENGTH]         = BREP(XR_REP_I64),
    [SYMBOL_INDEXOF]        = BREP(XR_REP_I64),
    [SYMBOL_LASTINDEXOF]    = BREP(XR_REP_I64),
    [SYMBOL_TOINT]          = BREP(XR_REP_I64),
    [SYMBOL_ORD]            = BREP(XR_REP_I64),
    [SYMBOL_FINDINDEX]      = BREP(XR_REP_I64),
    [SYMBOL_CHAR_LENGTH]    = BREP(XR_REP_I64),
    [SYMBOL_BYTE_LENGTH]    = BREP(XR_REP_I64),
    [SYMBOL_ORDINAL]        = BREP(XR_REP_I64),
    [SYMBOL_CAPACITY]       = BREP(XR_REP_I64),
    // SYMBOL_BYTE_AT returns string (PTR), not I64 — use default
    [SYMBOL_SIGN]           = BREP(XR_REP_I64),
    [SYMBOL_MEMBER_COUNT]   = BREP(XR_REP_I64),
    [SYMBOL_DAYS_IN_MONTH]  = BREP(XR_REP_I64),
    [SYMBOL_YEAR]           = BREP(XR_REP_I64),
    [SYMBOL_MONTH]          = BREP(XR_REP_I64),
    [SYMBOL_DAY]            = BREP(XR_REP_I64),
    [SYMBOL_HOUR]           = BREP(XR_REP_I64),
    [SYMBOL_MINUTE]         = BREP(XR_REP_I64),
    [SYMBOL_SECOND]         = BREP(XR_REP_I64),
    [SYMBOL_MILLISECOND]    = BREP(XR_REP_I64),
    [SYMBOL_WEEKDAY]        = BREP(XR_REP_I64),
    [SYMBOL_YEARDAY]        = BREP(XR_REP_I64),

    // I64 returns: boolean predicates (bool stored as I64 in JIT)
    [SYMBOL_IS_EMPTY]       = BREP(XR_REP_I64),
    [SYMBOL_HAS]            = BREP(XR_REP_I64),
    [SYMBOL_HAS_VALUE_MAP]  = BREP(XR_REP_I64),
    [SYMBOL_CONTAINS]       = BREP(XR_REP_I64),
    [SYMBOL_STARTSWITH]     = BREP(XR_REP_I64),
    [SYMBOL_ENDSWITH]       = BREP(XR_REP_I64),
    [SYMBOL_HASNEXT]        = BREP(XR_REP_I64),
    [SYMBOL_IS_SUBSET]      = BREP(XR_REP_I64),
    [SYMBOL_IS_SUPERSET]    = BREP(XR_REP_I64),
    [SYMBOL_IS_BEFORE]      = BREP(XR_REP_I64),
    [SYMBOL_IS_AFTER]       = BREP(XR_REP_I64),
    [SYMBOL_EQUALS]         = BREP(XR_REP_I64),
    [SYMBOL_IS_LEAP_YEAR]   = BREP(XR_REP_I64),
    [SYMBOL_IS_LETTER]      = BREP(XR_REP_I64),
    [SYMBOL_IS_NUMBER]      = BREP(XR_REP_I64),
    [SYMBOL_IS_ALNUM]       = BREP(XR_REP_I64),
    [SYMBOL_IS_WHITESPACE]  = BREP(XR_REP_I64),
    [SYMBOL_DONE]           = BREP(XR_REP_I64),
    [SYMBOL_CANCELLED]      = BREP(XR_REP_I64),
    [SYMBOL_IS_CLOSED]      = BREP(XR_REP_I64),
    [SYMBOL_ISZERO]         = BREP(XR_REP_I64),
    [SYMBOL_ISNEGATIVE]     = BREP(XR_REP_I64),
    [SYMBOL_ISPOSITIVE]     = BREP(XR_REP_I64),
    [SYMBOL_EVERY]          = BREP(XR_REP_I64),
    [SYMBOL_SOME]           = BREP(XR_REP_I64),
    [SYMBOL_INCLUDES]       = BREP(XR_REP_I64),
    [SYMBOL_TRYSEND]        = BREP(XR_REP_I64),

    // F64 returns: math, conversions, timestamps
    [SYMBOL_FLOOR]          = BREP(XR_REP_F64),
    [SYMBOL_CEIL]           = BREP(XR_REP_F64),
    [SYMBOL_ROUND]          = BREP(XR_REP_F64),
    [SYMBOL_ABS]            = BREP(XR_REP_F64),
    [SYMBOL_SQRT]           = BREP(XR_REP_F64),
    [SYMBOL_POW]            = BREP(XR_REP_F64),
    [SYMBOL_TOFLOAT]        = BREP(XR_REP_F64),
    [SYMBOL_TIMESTAMP]      = BREP(XR_REP_F64),
    [SYMBOL_MAX]            = BREP(XR_REP_F64),
    [SYMBOL_MIN]            = BREP(XR_REP_F64),
};

// Decode builtin_return_rep: 0=unset→PTR, nonzero→rep-1
static inline uint8_t builtin_ret_rep(int method_symbol) {
    if (method_symbol <= 0 || method_symbol >= SYMBOL_BUILTIN_COUNT)
        return XR_REP_PTR;
    uint8_t raw = builtin_return_rep[method_symbol];
    return raw ? (raw - 1) : XR_REP_PTR;
}

/*
 * Encode vtag (XrVRegTag) to 4-bit bitmap nibble for INVOKE arg bitmap.
 * Nibble values correspond to XrValueTag (0-7); 0xF=unknown.
 * Runtime reads this bitmap to reconstruct XrValues without heuristics.
 */
static inline uint8_t tag_to_nibble(uint8_t vtag) {
    switch (vtag) {
    case VTAG_NULL: return 0;   // XR_TAG_NULL
    case VTAG_I64:  return 3;   // XR_TAG_I64
    case VTAG_F64:  return 4;   // XR_TAG_F64
    case VTAG_PTR:  return 5;   // XR_TAG_PTR
    case VTAG_BOOL: return 1;   // XR_TAG_BOOL
    default:        return 0xF; // NUMERIC, CALLEE_SETS, TAGGED → runtime heuristic
    }
}

/*
 * Encode receiver + args xr_tag into a 64-bit bitmap stored in call_args[15].
 * Each slot gets 4 bits (low nibble): slot0 = bits[3:0], slot1 = bits[7:4], ...
 * Tag values 0-7 stored directly; 0xE = BOOL; 0xF = UNKNOWN.
 * Runtime reads this bitmap to reconstruct XrValues without heuristics.
 */
static void builder_emit_invoke_tag_bitmap(XirBuilder *b, XirBlock *blk,
                                           int recv_reg, int first_arg_reg,
                                           int nargs) {
    XR_DCHECK(b != NULL, "emit_invoke_tag_bitmap: NULL builder");
    XR_DCHECK(blk != NULL, "emit_invoke_tag_bitmap: NULL block");
    int64_t bitmap = 0;
    // Slot 0: receiver tag
    uint8_t rtag = builder_slot_xr_tag(b, recv_reg);
    bitmap |= (int64_t)tag_to_nibble(rtag);
    // Slots 1..nargs: argument tags
    for (int i = 0; i < nargs && i < 15; i++) {
        uint8_t atag = builder_slot_xr_tag(b, first_arg_reg + i);
        bitmap |= (int64_t)tag_to_nibble(atag) << ((1 + i) * 4);
    }
    int32_t off15 = JIT_CALL_ARGS_OFFSET + 15 * 8;
    XirRef off_ref = xir_const_i64(b->func, (int64_t)off15);
    XirRef bm_c = xir_const_i64(b->func, bitmap);
    XirRef bm_v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, bm_c);
    xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off_ref, bm_v, XIR_NONE);
    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
}

/*
 * After CALL/INVOKE, if the result slot's declared type is
 * nullable primitive (int?/float?/bool?), load the runtime tag from
 * slot_runtime_tags[slot] (written by call_c_stub from x1, or by the
 * JIT callee's RET epilogue via the CALLEE_SETS chain) so OP_ISNULL
 * can use tag comparison instead of payload==0.
 * Must be called AFTER builder_set_slot (which clears slot_tag_refs).
 */
static void builder_track_call_result_tag(XirBuilder *b, XirBlock *blk,
                                           int slot, XirRef result_ref) {
    XR_DCHECK(b != NULL, "track_call_result_tag: NULL builder");
    XR_DCHECK(blk != NULL, "track_call_result_tag: NULL block");
    if (slot < 0 || slot >= 256) return;
    // Use inst_types[pc] for flow-sensitive result type (nullable check).
    struct XrType *t = builder_inst_xrtype(b, b->cur_pc);
    if (!t || !t->is_nullable) return;
    if (t->kind != XR_KIND_INT && t->kind != XR_KIND_FLOAT &&
        t->kind != XR_KIND_BOOL) return;

    XirRef off = xir_const_i64(b->func,
        (int64_t)(XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + slot));
    XirRef tag_vr = xir_emit_unary(b->func, blk, XIR_LOAD_CORO_BYTE,
                                    XR_REP_I64, off);
    b->slot_tag_refs[slot] = tag_vr;
    b->slot_value_refs[slot] = result_ref;
}

bool xir_translate_call_ops(XirBuilder *b, XirBlock **cur_blk,
                            uint32_t pc, XrInstruction inst, OpCode op) {
    XR_DCHECK(b != NULL, "translate_call_ops: NULL builder");
    XR_DCHECK(cur_blk != NULL, "translate_call_ops: NULL cur_blk");

    // All call ops are GC safepoints — freshly allocated status is invalidated
    for (int _cf = 0; _cf < 256; _cf++) {
        XirVReg *_fv = builder_vreg_for_slot(b, _cf);
        if (_fv) _fv->is_fresh_alloc = false;
    }

    XirBlock *blk = *cur_blk;
    switch (op) {
        /* === Function Call === */
        case OP_CALL: {
            // R[A]...R[A+C-2] = R[A](R[A+1]...R[A+B-1])
            int a = GETARG_A(inst);
            int nargs = GETARG_B(inst);

            if (nargs > (b->aot_mode ? 64 : 15)) {  // AOT: no coro buffer limit
                b->ops_skipped++;
                return true;
            }

            // Check if callee proto is known (from OP_CLOSURE tracking)
            XirVReg *callee_v = builder_vreg_for_slot(b, a);
            XrProto *callee_proto = callee_v ? callee_v->callee_proto : NULL;

            // Collect call arguments: [0]=closure, [1..nargs]=args
            XirRef call_args[16];
            call_args[0] = builder_get_slot(b, blk, a);
            for (int i = 0; i < nargs; i++)
                call_args[1 + i] = builder_get_slot(b, blk, a + 1 + i);
            uint16_t total_ca = (uint16_t)(1 + nargs);

            if (callee_proto) {
                // === CALL_KNOWN path: callee proto known at compile time ===
                uint8_t ret_type = callee_proto->return_type_info
                    ? xr_type_rep(callee_proto->return_type_info)
                    : XR_REP_TAGGED;
                uint8_t ret_vtag = callee_proto->return_type_info
                    ? value_tag_to_vtag(xr_type_to_xr_tag((XrType*)callee_proto->return_type_info))
                    : VTAG_TAGGED;

                // Register-passing fast path for small calls (nargs <= 2)
                if (!b->aot_mode && nargs <= 2) {
                    // Store proto to jit_call_proto (codegen loads from there)
                    XirRef proto_val = xir_const_ptr(b->func, (void*)callee_proto);
                    XirRef proto_store = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                        XR_REP_I64, proto_val);
                    {
                        int32_t poff = JIT_CALL_PROTO_OFFSET;
                        XirRef poff_ref = xir_const_i64(b->func, (int64_t)poff);
                        xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                                     poff_ref, proto_store, XIR_NONE);
                        blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    }
                    XirRef arg0 = (nargs >= 1) ? call_args[1] : XIR_NONE;
                    XirRef arg1 = (nargs >= 2) ? call_args[2] : XIR_NONE;
                    XirRef result = xir_emit(b->func, blk, XIR_CALL_KNOWN_REG, ret_type,
                                             arg0, arg1);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    builder_bind_call_args(b, result, call_args, total_ca);
                    if (xir_ref_is_vreg(result)) {
                        uint32_t vi = XIR_REF_INDEX(result);
                        if (vi < b->func->nvreg) {
                            XirIns *d = b->func->vregs[vi].def;
                            if (d) d->ctype = xir_type_from_vtag(ret_vtag, 0);
                        }
                    }
                    builder_set_slot(b, a, result);
                } else {
                    // Memory-passing path (nargs > 2 or AOT mode)
                    XirRef proto_ref = xir_const_ptr(b->func, (void*)callee_proto);
                    XirRef nargs_ref = xir_const_i64(b->func, (int64_t)nargs);
                    XirRef nargs_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                      XR_REP_I64, nargs_ref);
                    XirRef result = xir_emit(b->func, blk, XIR_CALL_KNOWN, ret_type,
                                             proto_ref, nargs_val);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    builder_bind_call_args(b, result, call_args, total_ca);
                    if (xir_ref_is_vreg(result)) {
                        uint32_t vi = XIR_REF_INDEX(result);
                        if (vi < b->func->nvreg) {
                            XirIns *d = b->func->vregs[vi].def;
                            if (d) d->ctype = xir_type_from_vtag(ret_vtag, 0);
                        }
                    }
                    builder_set_slot(b, a, result);
                }
            } else {
                // === CALL_DIRECT path: callee unknown, use closure indirect ===
                XirRef nargs_ref = xir_const_i64(b->func, (int64_t)nargs);
                XirRef nargs_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                  XR_REP_I64, nargs_ref);
                XirRef fn_ref = xir_const_ptr(b->func, (void*)xr_jit_call_func);
                // Use XR_REP_TAGGED: return type is unknown at compile time.
                // Payload is always in GPR (x0), but value may be int/float/ptr.
                // XR_REP_I64 was wrong — it auto-inferred ctype=XIR_TK_INT,
                // causing RET epilogue to hardcode XR_TAG_I64 for all returns.
                XirRef result = xir_emit(b->func, blk, XIR_CALL_DIRECT,
                                         XR_REP_TAGGED, nargs_val, fn_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, result, call_args, total_ca);
                builder_set_slot(b, a, result);
                // Try compile-time return type from inst_types first.
                // xray is strongly typed — the compiler records call result
                // types in inst_types[pc] when available.
                builder_refine_slot_from_inst_type(b, a);
                // If compile-time type is still unknown (inst_types miss),
                // mark as TAGGED and track runtime tag as fallback.
                if (a < 256 && b->slot_tag[a] == VTAG_TAGGED) {
                    b->slot_rep[a] = XR_REP_TAGGED;
                    // Codegen writes callee's return tag to slot_runtime_tags[a]
                    // after each CALL_DIRECT. Emit a load so downstream ops
                    // can read the precise tag instead of heuristic.
                    XirRef off = xir_const_i64(b->func,
                        (int64_t)(XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + a));
                    XirRef tag_vr = xir_emit_unary(b->func, blk,
                        XIR_LOAD_CORO_BYTE, XR_REP_I64, off);
                    b->slot_tag_refs[a] = tag_vr;
                    b->slot_value_refs[a] = b->slot_map[a];
                }
            }
            // Clear proto tracking for result register (call result is not a closure)
            { XirVReg *_rv = builder_vreg_for_slot(b, a); if (_rv) _rv->callee_proto = NULL; }

            // track return_tag for nullable primitive result slots
            builder_track_call_result_tag(b, blk, a, b->slot_map[a]);

            // Multi-return: read extra results from jit_ctx->ret_vals[]
            {
                int nresults = GETARG_C(inst);
                for (int i = 1; i < nresults && i < 8; i++) {
                    int32_t val_offset = JIT_RET_VALS_OFFSET + (i - 1) * 8;
                    XirRef off_ref = xir_const_i64(b->func, (int64_t)val_offset);
                    XirRef extra = xir_emit_unary(b->func, blk, XIR_LOAD_CORO,
                                                   XR_REP_I64, off_ref);
                    builder_set_slot(b, a + i, extra);
                }
            }

            b->ops_translated++;
            return true;
        }

        /* === Tail Call (treated as CALL + RET for AOT) === */
        case OP_TAILCALL: {
            // R[A](R[A+1]...R[A+B-1]) — tail position call
            // Translate identically to OP_CALL + implicit return
            int a = GETARG_A(inst);
            int nargs = GETARG_B(inst);

            if (nargs > (b->aot_mode ? 64 : 15)) {  // AOT: no coro buffer limit
                b->ops_skipped++;
                return true;
            }

            { XirVReg *_cv2 = builder_vreg_for_slot(b, a);
            XrProto *callee_proto = _cv2 ? _cv2->callee_proto : NULL;

            // Collect call arguments: [0]=closure, [1..nargs]=args
            XirRef call_args[16];
            call_args[0] = builder_get_slot(b, blk, a);
            for (int i = 0; i < nargs; i++)
                call_args[1 + i] = builder_get_slot(b, blk, a + 1 + i);
            uint16_t total_ca = (uint16_t)(1 + nargs);

            if (callee_proto) {
                uint8_t ret_type = callee_proto->return_type_info
                    ? xr_type_rep(callee_proto->return_type_info)
                    : XR_REP_TAGGED;
                uint8_t tc_ret_vtag = callee_proto->return_type_info
                    ? value_tag_to_vtag(xr_type_to_xr_tag((XrType*)callee_proto->return_type_info))
                    : VTAG_TAGGED;
                XirRef proto_ref = xir_const_ptr(b->func, (void*)callee_proto);
                XirRef nargs_ref = xir_const_i64(b->func, (int64_t)nargs);
                XirRef nargs_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                  XR_REP_I64, nargs_ref);
                XirRef result = xir_emit(b->func, blk, XIR_CALL_KNOWN, ret_type,
                                         proto_ref, nargs_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, result, call_args, total_ca);
                if (xir_ref_is_vreg(result)) {
                    uint32_t vi = XIR_REF_INDEX(result);
                    if (vi < b->func->nvreg) {
                        XirIns *d = b->func->vregs[vi].def;
                        if (d) d->ctype = xir_type_from_vtag(tc_ret_vtag, 0);
                        if (b->func->vregs[vi].bc_slot < 0)
                            b->func->vregs[vi].bc_slot = (int16_t)a;
                    }
                }
                xir_block_set_ret(blk, result);
            } else {
                XirRef nargs_ref = xir_const_i64(b->func, (int64_t)nargs);
                XirRef nargs_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                  XR_REP_I64, nargs_ref);
                XirRef fn_ref = xir_const_ptr(b->func, (void*)xr_jit_call_func);
                XirRef result = xir_emit(b->func, blk, XIR_CALL_DIRECT, XR_REP_TAGGED,
                                         nargs_val, fn_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, result, call_args, total_ca);
                if (xir_ref_is_vreg(result)) {
                    uint32_t vi = XIR_REF_INDEX(result);
                    if (vi < b->func->nvreg) {
                        if (b->func->vregs[vi].bc_slot < 0)
                            b->func->vregs[vi].bc_slot = (int16_t)a;
                    }
                }
                xir_block_set_ret(blk, result);
            }
            { XirVReg *_rv2 = builder_vreg_for_slot(b, a); if (_rv2) _rv2->callee_proto = NULL; }
            } // _cv2 block (OP_TAILCALL callee_proto)
            b->ops_translated++;
            return true;
        }

        /* === Recursive Self-Call === */
        case OP_CALLSELF: {
            // R[A]...R[A+C-2] = self(R[A+1]...R[A+B-1])
            int a = GETARG_A(inst);
            int nargs = GETARG_B(inst);

            if (nargs > 15) {
                b->ops_skipped++;
                return true;
            }

            // Determine return XIR type from proto
            uint8_t ret_xir = b->proto->return_type_info
                ? xr_type_rep(b->proto->return_type_info)
                : XR_REP_TAGGED;
            uint8_t cs_ret_vtag = b->proto->return_type_info
                ? value_tag_to_vtag(xr_type_to_xr_tag((XrType*)b->proto->return_type_info))
                : VTAG_TAGGED;
            if (ret_xir == XR_REP_TAGGED) ret_xir = XR_REP_I64;

            // Collect all args
            XirRef cs_args[16];
            for (int i = 0; i < nargs; i++)
                cs_args[i] = builder_get_slot(b, blk, a + 1 + i);

            XirRef result;
            if (nargs <= 5) {
                // Fast path: arg0/arg1 in registers, arg2..4 via call_arg_pool
                XirRef arg0 = (nargs >= 1) ? cs_args[0] : XIR_NONE;
                XirRef arg1 = (nargs >= 2) ? cs_args[1] : XIR_NONE;
                int extra = (nargs > 2) ? (nargs - 2) : 0;
                result = xir_emit(b->func, blk, XIR_CALL_SELF_DIRECT, ret_xir,
                                  arg0, arg1);
                if (extra > 0)
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_EXTRA_ARGS(extra);
            } else {
                // Slow path (nargs >= 6): all args via call_arg_pool
                result = xir_emit(b->func, blk, XIR_CALL_SELF_DIRECT, ret_xir,
                                  XIR_NONE, XIR_NONE);
            }
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, cs_args, (uint16_t)nargs);
            if (xir_ref_is_vreg(result)) {
                uint32_t vi = XIR_REF_INDEX(result);
                if (vi < b->func->nvreg) {
                    XirIns *d = b->func->vregs[vi].def;
                    if (d) d->ctype = xir_type_from_vtag(cs_ret_vtag, 0);
                }
            }
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Exception Handling === */
        case OP_TRY: {
            int catch_offset = GETARG_Bx(inst);
            if (catch_offset <= 0 || (uint32_t)catch_offset >= b->code_count) {
                b->ops_skipped++;
                return true;
            }
            // Next instruction holds finally_offset (OP_NOP placeholder)
            XrInstruction next_inst = PROTO_CODE(b->proto, pc + 1);
            int finally_offset = GETARG_Bx(next_inst);

            // Get or create catch block
            XirBlock *catch_blk = b->pc_to_block[catch_offset];
            if (!catch_blk) {
                catch_blk = xir_func_add_block(b->func, "catch");
                b->pc_to_block[catch_offset] = catch_blk;
            }

            // Get or create finally block (if present)
            XirBlock *finally_blk = NULL;
            if (finally_offset > 0) {
                finally_blk = b->pc_to_block[finally_offset];
                if (!finally_blk) {
                    finally_blk = xir_func_add_block(b->func, "finally");
                    b->pc_to_block[finally_offset] = finally_blk;
                }
            }

            // Push try state with slot_map snapshot
            if (b->try_depth < 8) {
                b->try_stack[b->try_depth].catch_block = catch_blk;
                b->try_stack[b->try_depth].finally_block = finally_blk;
                memcpy(b->try_stack[b->try_depth].saved_slot_map,
                       b->slot_map, sizeof(b->slot_map));
                b->try_depth++;
            }

            // Set exception_handler on current block
            blk->exception_handler = catch_blk;

            b->ops_translated++;
            return true;
        }

        case OP_CATCH: {
            int a = GETARG_A(inst);
            // Load exception value from coro->jit_exception
            XirRef exc = xir_emit_unary(b->func, blk, XIR_CATCH, XR_REP_PTR, XIR_NONE);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_tag_vreg(b, exc, VTAG_PTR, 0);
            builder_set_slot(b, a, exc);
            b->ops_translated++;
            return true;
        }

        case OP_THROW: {
            int a = GETARG_A(inst);
            XirRef val = builder_get_slot(b, blk, a);

            // Call xr_jit_throw(coro, exception_value)
            XirRef fn_ptr = xir_const_ptr(b->func, (void *)xr_jit_throw);
            xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ptr, val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT | XIR_FLAG_MAY_THROW;

            // If inside try block, jump to catch handler
            if (b->try_depth > 0) {
                XirBlock *catch_blk = b->try_stack[b->try_depth - 1].catch_block;
                xir_block_set_jmp(blk, catch_blk);
                xir_block_add_pred(catch_blk, blk, b->func->arena);
            } else {
                // Not in try: return exception marker (deopt to interpreter)
                XirRef marker = xir_const_i64(b->func, (int64_t)0xDEAD0001DEAD0001LL);
                XirRef marker_v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, marker);
                blk->jmp.type = XIR_JMP_RET;
                blk->jmp.arg = marker_v;
            }

            // Start a new block for code after throw (unreachable but needed for builder)
            // Mark it terminated to prevent spurious fall-through edges
            XirBlock *next_blk = xir_func_add_block(b->func, NULL);
            next_blk->jmp.type = XIR_JMP_RET;
            *cur_blk = next_blk;

            b->ops_translated++;
            return true;
        }

        case OP_END_TRY: {
            if (b->try_depth > 0) {
                b->try_depth--;
            }
            // Clear exception_handler on current block
            blk->exception_handler = NULL;
            b->ops_translated++;
            return true;
        }

        case OP_FINALLY:
            // Finally block marker (no special XIR needed)
            b->ops_translated++;
            return true;

        /* === General Method Invocation === */
        case OP_INVOKE: {
            // OP_INVOKE: R[A]=return, R[A+1]=receiver, R[A+2..]=args, B=symbol, C=nargs
            // JIT: delegate to xr_jit_invoke_method C bridge
            // AOT: delegate to xrt_invoke_method_sentinel
            int a = GETARG_A(inst);
            int sym_local = GETARG_B(inst);
            int nargs = GETARG_C(inst);

            // Bounds check to prevent SIGSEGV
            if (sym_local >= PROTO_SYMBOL_COUNT(b->proto)) {
#if XR_DEBUG
                fprintf(stderr, "[JIT] Symbol index %d out of bounds (max %d) in OP_INVOKE\n",
                        sym_local, PROTO_SYMBOL_COUNT(b->proto));
#endif
                b->ops_skipped++;
                return true;
            }

            int method_symbol = PROTO_SYMBOL(b->proto, sym_local);

            if (nargs > 14) {
                b->ops_skipped++;
                return true;
            }

            // --- CHA devirtualization attempt ---
            // When receiver type is statically known (INSTANCE with class_name),
            // and the class is final or has no subclasses, resolve the method at
            // compile time and emit CALL_KNOWN instead of the generic bridge.
            // Works in both JIT and AOT modes when isolate is available.
            // CHA devirt: resolve receiver type to devirtualize method call.
            // Uses builder_find_reg_type which checks param_types then
            // backward-scans inst_types to find the receiver's static type.
            if (b->isolate &&
                method_symbol >= SYMBOL_BUILTIN_COUNT &&
                (a + 1) < 256)
            {
                XrType *recv_type = builder_find_reg_type(b, a + 1);
                if (!recv_type) goto skip_cha_devirt;
                const char *cname = xr_type_get_class_name(recv_type);
                if (cname && recv_type->kind == XR_KIND_INSTANCE) {
                    XrClass *klass = xr_class_lookup_by_name(b->isolate, cname);
                    if (klass &&
                        ((klass->flags & XR_CLASS_FINAL) ||
                         !(klass->flags & XR_CLASS_HAS_SUBCLASSES)))
                    {
                        XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                        if (method && method->type == XMETHOD_CLOSURE &&
                            method->as.closure && method->as.closure->proto)
                        {
                            XrProto *callee_proto = method->as.closure->proto;

                            // Derive return type from callee proto
                            uint8_t ret_type_ck = XR_REP_I64;
                            if (callee_proto->return_type_info)
                                ret_type_ck = xr_type_rep(callee_proto->return_type_info);

                            // Collect args: [0]=closure, [1]=receiver, [2..nargs+1]=args
                            XirRef cha_args[16];
                            XirRef closure_ptr = xir_const_ptr(b->func, (void *)method->as.closure);
                            cha_args[0] = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                          XR_REP_I64, closure_ptr);
                            cha_args[1] = builder_get_slot(b, blk, a + 1);
                            for (int i = 0; i < nargs; i++)
                                cha_args[2 + i] = builder_get_slot(b, blk, a + 2 + i);
                            uint16_t cha_nca = (uint16_t)(2 + nargs);
                            // Emit CALL_KNOWN: callee expects nargs+1 (this + args)
                            XirRef proto_ref = xir_const_ptr(b->func, (void *)callee_proto);
                            XirRef na_ref = xir_const_i64(b->func, (int64_t)(nargs + 1));
                            XirRef na_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                           XR_REP_I64, na_ref);
                            XirRef result_ck = xir_emit(b->func, blk, XIR_CALL_KNOWN,
                                                        ret_type_ck, proto_ref, na_val);
                            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                            builder_bind_call_args(b, result_ck, cha_args, cha_nca);
                            if (ret_type_ck == XR_REP_PTR)
                                builder_tag_vreg(b, result_ck, VTAG_PTR, 0);
                            if (ret_type_ck != XR_REP_PTR && a < 256)
                                b->slot_rep[a] = ret_type_ck;
                            builder_set_slot(b, a, result_ck);
                            builder_track_call_result_tag(b, blk, a, b->slot_map[a]);
                            b->ops_translated++;
                            return true;
                        }
                    }
                }
            }

            skip_cha_devirt: ;

            // --- IC-based speculative devirtualization ---
            // When CHA fails (unknown static type or class has subclasses),
            // use runtime IC profile to speculatively devirtualize.
            // Monomorphic IC (single class seen): emit GUARD_KLASS + CALL_KNOWN.
            // Polymorphic IC with >90% dominant class: same treatment.
            // On guard failure, deopt back to interpreter.
            if (!b->aot_mode && b->isolate &&
                method_symbol >= SYMBOL_BUILTIN_COUNT &&
                (a + 1) < 256 && b->proto->ic_methods)
            {
                size_t ic_index = b->cur_pc;
                XrICMethod *ic = xr_ic_method_table_get(b->proto->ic_methods,
                                                         (int)ic_index);
                if (!ic) goto skip_ic_devirt;

                XrICState ic_state = xr_ic_method_state(ic);
                XrClass *ic_klass = NULL;

                if (ic_state == XR_IC_STATE_MONO) {
                    ic_klass = ic->entries[0].klass;
                } else if (ic_state == XR_IC_STATE_POLY) {
                    double ratio = 0.0;
                    ic_klass = xr_ic_method_dominant_class(ic, &ratio);
                    if (ratio < 0.90) ic_klass = NULL;
                }

                if (!ic_klass) goto skip_ic_devirt;

                XrMethod *ic_method = xr_class_lookup_method(ic_klass, method_symbol);
                if (!ic_method || ic_method->type != XMETHOD_CLOSURE ||
                    !ic_method->as.closure || !ic_method->as.closure->proto)
                    goto skip_ic_devirt;

                XrProto *callee_proto = ic_method->as.closure->proto;

                // Emit GUARD_KLASS: deopt if receiver->klass != expected
                XirRef recv_ref = builder_get_slot(b, blk, a + 1);
                XirRef klass_ref = xir_const_ptr(b->func, (void *)ic_klass);
                xir_emit(b->func, blk, XIR_GUARD_KLASS, XR_REP_I64,
                         recv_ref, klass_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

                // Derive return type from callee proto
                uint8_t ret_type_ic = XR_REP_I64;
                if (callee_proto->return_type_info)
                    ret_type_ic = xr_type_rep(callee_proto->return_type_info);

                // Collect args: [0]=closure, [1]=receiver, [2..nargs+1]=args
                XirRef ic_args[16];
                XirRef cl_ptr = xir_const_ptr(b->func,
                                              (void *)ic_method->as.closure);
                ic_args[0] = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, cl_ptr);
                ic_args[1] = builder_get_slot(b, blk, a + 1);
                for (int i = 0; i < nargs; i++)
                    ic_args[2 + i] = builder_get_slot(b, blk, a + 2 + i);
                uint16_t ic_nca = (uint16_t)(2 + nargs);

                XirRef proto_ref = xir_const_ptr(b->func, (void *)callee_proto);
                XirRef na_ref = xir_const_i64(b->func, (int64_t)(nargs + 1));
                XirRef na_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                               XR_REP_I64, na_ref);
                XirRef result_ic = xir_emit(b->func, blk, XIR_CALL_KNOWN,
                                            ret_type_ic, proto_ref, na_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, result_ic, ic_args, ic_nca);
                if (ret_type_ic == XR_REP_PTR)
                    builder_tag_vreg(b, result_ic, VTAG_PTR, 0);
                if (ret_type_ic != XR_REP_PTR && a < 256)
                    b->slot_rep[a] = ret_type_ic;
                builder_set_slot(b, a, result_ic);
                builder_track_call_result_tag(b, blk, a, b->slot_map[a]);
                b->ops_translated++;
                return true;
            }

            skip_ic_devirt: ;
            // --- Fallback: generic CALL_C bridge ---
            // Collect args: [0]=receiver, [1..nargs]=args
            XirRef inv_args[16];
            inv_args[0] = builder_get_slot(b, blk, a + 1);
            for (int i = 0; i < nargs; i++)
                inv_args[1 + i] = builder_get_slot(b, blk, a + 2 + i);
            uint16_t inv_nca = (uint16_t)(1 + nargs);

            // Store tag bitmap to call_args[15] for precise arg reconstruction
            builder_emit_invoke_tag_bitmap(b, blk, a + 1, a + 2, nargs);

            // Derive receiver type hint for IC-guided dispatch
            int type_hint = builder_derive_type_hint(b, a + 1);

            // Deopt snapshot so yieldable C function encounters can recover
            // to interpreter at this OP_INVOKE instead of re-executing the
            // entire function from scratch.
            int deopt_id = builder_add_deopt_info(b, pc);

            // Encode: method_symbol[63:32] | deopt_id[31:16] | type_hint[15:8] | nargs[7:0]
            int64_t encoded = ((int64_t)method_symbol << 32)
                            | ((int64_t)((deopt_id >= 0 ? deopt_id : 0) & 0xFFFF) << 16)
                            | ((int64_t)(type_hint & 0xFF) << 8)
                            | (int64_t)(nargs & 0xFF);
            void *bridge_fn = b->aot_mode
                ? (void*)xrt_invoke_method_sentinel
                : (void*)xr_jit_invoke_method;
            XirRef fn_ref = xir_const_ptr(b->func, bridge_fn);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            // Table-driven return type for builtin methods
            uint8_t ret_type_inv = builtin_ret_rep(method_symbol);
            // SYMBOL_NEXT return type refinement: use inst_types[pc] for
            // element type when it indicates a scalar (I64/F64).
            if (ret_type_inv == XR_REP_PTR && method_symbol == SYMBOL_NEXT) {
                struct XrType *it = builder_inst_xrtype(b, b->cur_pc);
                if (it) {
                    uint8_t st = value_tag_to_vtag(xr_type_to_xr_tag(it));
                    if (st == VTAG_I64)      ret_type_inv = XR_REP_I64;
                    else if (st == VTAG_F64) ret_type_inv = XR_REP_F64;
                }
            }
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, ret_type_inv,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, inv_args, inv_nca);
            if (ret_type_inv == XR_REP_PTR) {
                // Tag as PTR directly. Do NOT use builder_tag_from_slot here:
                // static type info may reflect a later reuse of this register
                // (e.g., R[4] used for INVOKE result=PTR then ISNULL_SET=bool),
                // causing the INVOKE result to be mis-tagged as bool.
                builder_tag_vreg(b, result, VTAG_PTR, 0);
            }
            if (ret_type_inv != XR_REP_PTR && a < 256)
                b->slot_rep[a] = ret_type_inv;
            builder_set_slot(b, a, result);
            // track return_tag for nullable primitive result slots
            builder_track_call_result_tag(b, blk, a, b->slot_map[a]);
            b->ops_translated++;
            return true;
        }

        /* === Builtin Method Invocation === */
        case OP_INVOKE_BUILTIN: {
            // R[A] = R[A+1]:method_B(R[A+2]..R[A+1+C])
            // A=base, B=method_symbol(local), C=nargs
            int a = GETARG_A(inst);
            int sym_local = GETARG_B(inst);
            int nargs = GETARG_C(inst);

            // Bounds check to prevent SIGSEGV
            if (sym_local >= PROTO_SYMBOL_COUNT(b->proto)) {
#if XR_DEBUG
                fprintf(stderr, "[JIT] Symbol index %d out of bounds (max %d) in OP_INVOKE_BUILTIN\n",
                        sym_local, PROTO_SYMBOL_COUNT(b->proto));
#endif
                b->ops_skipped++;
                return true;
            }

            int method_symbol = PROTO_SYMBOL(b->proto, sym_local);

            if (nargs > 14) {
                b->ops_skipped++;
                return true;
            }

            // Collect args: [0]=receiver, [1..nargs]=args
            XirRef bi_args[16];
            bi_args[0] = builder_get_slot(b, blk, a + 1);
            for (int i = 0; i < nargs; i++)
                bi_args[1 + i] = builder_get_slot(b, blk, a + 2 + i);
            uint16_t bi_nca = (uint16_t)(1 + nargs);

            // Store tag bitmap to call_args[15] for precise arg reconstruction
            builder_emit_invoke_tag_bitmap(b, blk, a + 1, a + 2, nargs);

            // Derive receiver type hint for IC-guided dispatch
            int type_hint = builder_derive_type_hint(b, a + 1);

            // Deopt snapshot for yieldable C function recovery
            int deopt_id_bi = builder_add_deopt_info(b, pc);

            // Encode: method_symbol[63:32] | deopt_id[31:16] | type_hint[15:8] | nargs[7:0]
            int64_t encoded = ((int64_t)method_symbol << 32)
                            | ((int64_t)((deopt_id_bi >= 0 ? deopt_id_bi : 0) & 0xFFFF) << 16)
                            | ((int64_t)(type_hint & 0xFF) << 8)
                            | (int64_t)(nargs & 0xFF);
            void *bridge_fn = b->aot_mode
                ? (void*)xrt_invoke_method_sentinel
                : (void*)xr_jit_invoke_method;
            XirRef fn_ref = xir_const_ptr(b->func, bridge_fn);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            // Table-driven return type for builtin methods
            uint8_t ret_type = builtin_ret_rep(method_symbol);
            // SYMBOL_NEXT return type refinement via inst_types[pc]
            if (ret_type == XR_REP_PTR && method_symbol == SYMBOL_NEXT) {
                struct XrType *it = builder_inst_xrtype(b, b->cur_pc);
                if (it) {
                    uint8_t st = value_tag_to_vtag(xr_type_to_xr_tag(it));
                    if (st == VTAG_I64)      ret_type = XR_REP_I64;
                    else if (st == VTAG_F64) ret_type = XR_REP_F64;
                }
            }
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, ret_type,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, bi_args, bi_nca);
            // Tag as PTR directly (same reason as OP_INVOKE above)
            if (ret_type == XR_REP_PTR)
                builder_tag_vreg(b, result, VTAG_PTR, 0);
            if (ret_type != XR_REP_PTR && a < 256)
                b->slot_rep[a] = ret_type;
            builder_set_slot(b, a, result);
            // track return_tag for nullable primitive result slots
            builder_track_call_result_tag(b, blk, a, b->slot_map[a]);
            b->ops_translated++;
            return true;
        }

        /* === Tail Call Instructions === */
        case OP_INVOKE_TAIL: {
            // Method tail call: in JIT mode, treat as normal invoke (no frame reuse)
            if (b->aot_mode) { b->ops_skipped++; return true; }
            int a = GETARG_A(inst);
            int sym_local = GETARG_B(inst);
            int nargs = GETARG_C(inst);

            // Bounds check to prevent SIGSEGV
            if (sym_local >= PROTO_SYMBOL_COUNT(b->proto)) {
#if XR_DEBUG
                fprintf(stderr, "[JIT] Symbol index %d out of bounds (max %d) in OP_INVOKE_TAIL\n",
                        sym_local, PROTO_SYMBOL_COUNT(b->proto));
#endif
                b->ops_skipped++;
                return true;
            }

            int method_symbol = PROTO_SYMBOL(b->proto, sym_local);
            if (nargs > 14) { b->ops_skipped++; return true; }

            // --- CHA devirtualization for tail calls ---
            if (b->isolate &&
                method_symbol >= SYMBOL_BUILTIN_COUNT &&
                (a + 1) < 256)
            {
                XrType *recv_type_t = builder_find_reg_type(b, a + 1);
                if (!recv_type_t) goto skip_tail_cha;
                const char *cname_t = xr_type_get_class_name(recv_type_t);
                if (cname_t && recv_type_t->kind == XR_KIND_INSTANCE) {
                    XrClass *klass_t = xr_class_lookup_by_name(b->isolate, cname_t);
                    if (klass_t &&
                        ((klass_t->flags & XR_CLASS_FINAL) ||
                         !(klass_t->flags & XR_CLASS_HAS_SUBCLASSES)))
                    {
                        XrMethod *method_t = xr_class_lookup_method(klass_t, method_symbol);
                        if (method_t && method_t->type == XMETHOD_CLOSURE &&
                            method_t->as.closure && method_t->as.closure->proto)
                        {
                            XrProto *callee_proto_t = method_t->as.closure->proto;
                            uint8_t ret_t = XR_REP_I64;
                            if (callee_proto_t->return_type_info)
                                ret_t = xr_type_rep(callee_proto_t->return_type_info);

                            XirRef cha_t_args[16];
                            XirRef cl_ref_t = xir_const_ptr(b->func, (void *)method_t->as.closure);
                            cha_t_args[0] = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                           XR_REP_I64, cl_ref_t);
                            cha_t_args[1] = builder_get_slot(b, blk, a + 1);
                            for (int i = 0; i < nargs; i++)
                                cha_t_args[2 + i] = builder_get_slot(b, blk, a + 2 + i);
                            uint16_t cha_t_nca = (uint16_t)(2 + nargs);

                            XirRef proto_ref_t = xir_const_ptr(b->func, (void *)callee_proto_t);
                            XirRef na_ref_t = xir_const_i64(b->func, (int64_t)(nargs + 1));
                            XirRef na_val_t = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                             XR_REP_I64, na_ref_t);
                            XirRef result_t = xir_emit(b->func, blk, XIR_CALL_KNOWN,
                                                       ret_t, proto_ref_t, na_val_t);
                            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                            builder_bind_call_args(b, result_t, cha_t_args, cha_t_nca);
                            if (ret_t == XR_REP_PTR)
                                builder_tag_vreg(b, result_t, VTAG_PTR, 0);
                            if (ret_t != XR_REP_PTR && a < 256)
                                b->slot_rep[a] = ret_t;
                            builder_set_slot(b, a, result_t);
                            builder_track_call_result_tag(b, blk, a, b->slot_map[a]);
                            b->ops_translated++;
                            return true;
                        }
                    }
                }
            }
            skip_tail_cha: ;

            // --- IC-based speculative devirtualization for tail calls ---
            if (b->isolate &&
                method_symbol >= SYMBOL_BUILTIN_COUNT &&
                (a + 1) < 256 && b->proto->ic_methods)
            {
                size_t ic_idx_t = b->cur_pc;
                XrICMethod *ic_t = xr_ic_method_table_get(b->proto->ic_methods,
                                                           (int)ic_idx_t);
                if (!ic_t) goto skip_tail_ic;

                XrICState ic_st_t = xr_ic_method_state(ic_t);
                XrClass *ic_klass_t = NULL;

                if (ic_st_t == XR_IC_STATE_MONO) {
                    ic_klass_t = ic_t->entries[0].klass;
                } else if (ic_st_t == XR_IC_STATE_POLY) {
                    double ratio_t = 0.0;
                    ic_klass_t = xr_ic_method_dominant_class(ic_t, &ratio_t);
                    if (ratio_t < 0.90) ic_klass_t = NULL;
                }

                if (!ic_klass_t) goto skip_tail_ic;

                XrMethod *ic_m_t = xr_class_lookup_method(ic_klass_t, method_symbol);
                if (!ic_m_t || ic_m_t->type != XMETHOD_CLOSURE ||
                    !ic_m_t->as.closure || !ic_m_t->as.closure->proto)
                    goto skip_tail_ic;

                XrProto *callee_proto_ic = ic_m_t->as.closure->proto;

                XirRef recv_t = builder_get_slot(b, blk, a + 1);
                XirRef klass_ref_t = xir_const_ptr(b->func, (void *)ic_klass_t);
                xir_emit(b->func, blk, XIR_GUARD_KLASS, XR_REP_I64,
                         recv_t, klass_ref_t);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

                uint8_t ret_ic_t = XR_REP_I64;
                if (callee_proto_ic->return_type_info)
                    ret_ic_t = xr_type_rep(callee_proto_ic->return_type_info);

                XirRef ic_t_args[16];
                XirRef cl_ic_t = xir_const_ptr(b->func, (void *)ic_m_t->as.closure);
                ic_t_args[0] = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                              XR_REP_I64, cl_ic_t);
                ic_t_args[1] = builder_get_slot(b, blk, a + 1);
                for (int i = 0; i < nargs; i++)
                    ic_t_args[2 + i] = builder_get_slot(b, blk, a + 2 + i);
                uint16_t ic_t_nca = (uint16_t)(2 + nargs);

                XirRef proto_ic_t = xir_const_ptr(b->func, (void *)callee_proto_ic);
                XirRef na_ic_t = xir_const_i64(b->func, (int64_t)(nargs + 1));
                XirRef na_ic_v = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                XR_REP_I64, na_ic_t);
                XirRef result_ict = xir_emit(b->func, blk, XIR_CALL_KNOWN,
                                             ret_ic_t, proto_ic_t, na_ic_v);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, result_ict, ic_t_args, ic_t_nca);
                if (ret_ic_t == XR_REP_PTR)
                    builder_tag_vreg(b, result_ict, VTAG_PTR, 0);
                if (ret_ic_t != XR_REP_PTR && a < 256)
                    b->slot_rep[a] = ret_ic_t;
                builder_set_slot(b, a, result_ict);
                builder_track_call_result_tag(b, blk, a, b->slot_map[a]);
                b->ops_translated++;
                return true;
            }
            skip_tail_ic: ;

            // --- Fallback: generic CALL_C bridge ---
            // Collect args: [0]=receiver, [1..nargs]=args
            XirRef it_args[16];
            it_args[0] = builder_get_slot(b, blk, a + 1);
            for (int i = 0; i < nargs; i++)
                it_args[1 + i] = builder_get_slot(b, blk, a + 2 + i);
            uint16_t it_nca = (uint16_t)(1 + nargs);

            // Store tag bitmap to call_args[15] for precise arg reconstruction
            builder_emit_invoke_tag_bitmap(b, blk, a + 1, a + 2, nargs);

            // Derive receiver type hint for IC-guided dispatch
            int type_hint_t = builder_derive_type_hint(b, a + 1);
            int64_t encoded = ((int64_t)method_symbol << 32)
                            | ((int64_t)(type_hint_t & 0xFF) << 8)
                            | (int64_t)(nargs & 0xFF);
            XirRef fn_ref = xir_const_ptr(b->func, (void*)xr_jit_invoke_method);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            // Table-driven return type for builtin methods
            uint8_t ret_type_inv = builtin_ret_rep(method_symbol);
            // SYMBOL_NEXT return type refinement via inst_types[pc]
            if (ret_type_inv == XR_REP_PTR && method_symbol == SYMBOL_NEXT) {
                struct XrType *it = builder_inst_xrtype(b, b->cur_pc);
                if (it) {
                    uint8_t st = value_tag_to_vtag(xr_type_to_xr_tag(it));
                    if (st == VTAG_I64)      ret_type_inv = XR_REP_I64;
                    else if (st == VTAG_F64) ret_type_inv = XR_REP_F64;
                }
            }
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, ret_type_inv,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, it_args, it_nca);
            // Tag as PTR directly (same reason as OP_INVOKE above)
            if (ret_type_inv == XR_REP_PTR)
                builder_tag_vreg(b, result, VTAG_PTR, 0);
            if (ret_type_inv != XR_REP_PTR && a < 256)
                b->slot_rep[a] = ret_type_inv;
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Tail Recursion Loop === */
        case OP_LOOP_BACK: {
            // Tail recursion → loop: R[skip..skip+B-1] = R[A+1..A+B], goto entry
            // A=func_reg, B=nargs, C=skip (0=regular, 1=method preserves this)
            int a = GETARG_A(inst);
            int nargs = GETARG_B(inst);
            int skip = GETARG_C(inst);

            // Read new arg values first (avoid aliasing with slot_map updates)
            XirRef new_vals[8];
            for (int i = 0; i < nargs && i < 8; i++) {
                new_vals[i] = builder_get_slot(b, blk, a + 1 + i);
            }
            // Update slot_map for the target param slots (SSA: phi handles merging)
            for (int i = 0; i < nargs && i < 8; i++) {
                int param_idx = skip + i;
                if (param_idx < 256) {
                    builder_set_slot(b, param_idx, new_vals[i]);
                }
            }

            // Safepoint before back-edge (GC + preemption check)
            XirRef none = XIR_NONE;
            xir_emit(b->func, blk, XIR_SAFEPOINT, XR_REP_VOID, none, none);

            // Jump to loop header (orig_entry stored in pc_to_block[0])
            XirBlock *loop_target = b->pc_to_block[0];
            xir_block_set_jmp(blk, loop_target);
            // Defer seal: multiple LOOP_BACK may target same header
            // Seal after all instructions translated
            b->ops_translated++;
            return true;
        }

        /* === Container Creation === */
        case OP_NEWMAP: {
            int a = GETARG_A(inst);
            int cap = GETARG_B(inst);
            XirRef cap_ref = xir_const_i64(b->func, (int64_t)cap);
            XirRef v = xir_emit(b->func, blk, XIR_RT_MAP_NEW, XR_REP_PTR,
                                cap_ref, XIR_NONE);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_tag_vreg(b, v, VTAG_PTR, 0);
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        /* === Array Operations === */
        case OP_NEWARRAY: {
            int a = GETARG_A(inst);
            int cap = GETARG_B(inst);
            XirRef cap_ref = xir_const_i64(b->func, (int64_t)cap);
            XirRef v = xir_emit(b->func, blk, XIR_RT_ARRAY_NEW, XR_REP_PTR,
                                cap_ref, XIR_NONE);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_tag_vreg(b, v, VTAG_PTR, 0);
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        case OP_ARRAY_PUSH: {
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef arr = builder_get_slot(b, blk, a);
            XirRef val = builder_get_slot(b, blk, rb);
            xir_emit(b->func, blk, XIR_RT_ARRAY_PUSH, XR_REP_VOID, arr, val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            b->ops_translated++;
            return true;
        }

        case OP_ARRAY_GET:
        case OP_ARRAY_GET_NOCHECK: {
            // R[A] = R[B]:Array[R[C]]
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, rb);
            XirRef key = builder_get_slot(b, blk, rc);

            // Infer result type from container's element type.
            // Skip inference when obj tag is UNKNOWN (e.g., CTX_LOAD result):
            // slot_xrtype is flow-insensitive and may reflect a different type
            // due to register reuse, causing wrong element rep.
            uint8_t obj_tag_check = builder_slot_xr_tag(b, rb);
            uint8_t res_type = XR_REP_TAGGED;
            if (obj_tag_check != VTAG_TAGGED &&
                rb < 256 && builder_slot_xrtype(b, rb)) {
                uint8_t elem_gc = xr_type_element_gc_tag(builder_slot_xrtype(b, rb));
                res_type = xr_slot_to_rep(elem_gc);
            }

            // Inline path for known-PTR array objects (ANY elem_type)
            if (ref_xir_type(b->func, obj) == XR_REP_PTR) {
                // 1. Load arr->length (int32 sign-extended)
                XirRef len_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_LENGTH_OFFSET);
                XirRef len = xir_emit(b->func, blk, XIR_LOAD32S, XR_REP_I64, obj, len_off);

                // 2. Bounds check
                xir_emit(b->func, blk, XIR_GUARD_BOUNDS, XR_REP_VOID, key, len);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                int did_ag = builder_add_deopt_info(b, pc);
                if (did_ag >= 0) {
                    blk->ins[blk->nins - 1].dst =
                        xir_const_i64(b->func, (int64_t)did_ag);
                }

                // 3. Load arr->data pointer
                XirRef data_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_DATA_OFFSET);
                XirRef addr_base = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, obj, data_off);
                XirRef data_ptr = xir_emit_unary(b->func, blk, XIR_LOAD, XR_REP_PTR, addr_base);

                // 4. Compute element address: data_ptr + idx * 16 + 8 (payload offset)
                XirRef es16 = xir_const_i64(b->func, (int64_t)XIR_XRVALUE_SIZE);
                XirRef es16_v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, es16);
                XirRef byte_off = xir_emit(b->func, blk, XIR_MUL, XR_REP_I64, key, es16_v);
                XirRef elem_base = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, data_ptr, byte_off);
                // Add payload offset (byte 8 within XrValue)
                XirRef pay_off = xir_const_i64(b->func, (int64_t)XIR_XRVALUE_PAYLOAD_OFFSET);
                XirRef pay_off_v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, pay_off);
                XirRef elem_addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, elem_base, pay_off_v);

                // 5. Load payload (int64/f64/ptr — all 8 bytes at payload offset)
                XirRef v;
                if (res_type == XR_REP_F64) {
                    v = xir_emit_unary(b->func, blk, XIR_LOAD, XR_REP_F64, elem_addr);
                } else {
                    v = xir_emit_unary(b->func, blk, XIR_LOAD, res_type, elem_addr);
                }
                builder_set_slot(b, a, v);

                // 6. For polymorphic arrays (Array<any>), load the element's
                // runtime tag so downstream consumers (PRINT, TYPEOF, etc.)
                // can distinguish bool/null/int/ptr correctly.
                if ((res_type == XR_REP_TAGGED || res_type == XR_REP_I64) && a < 256) {
                    // Load tag byte from elem_base + 0 (XIR_XRVALUE_TAG_OFFSET)
                    XirRef tag_byte = xir_emit_unary(b->func, blk, XIR_LOAD8Z,
                                                      XR_REP_I64, elem_base);
                    // Store to slot_runtime_tags[a]
                    XirRef rt_off = xir_const_i64(b->func,
                        (int64_t)(XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + a));
                    xir_emit_raw(b->func, blk, XIR_STORE_CORO_BYTE, XR_REP_VOID,
                                 rt_off, tag_byte, XIR_NONE);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    b->slot_tag_refs[a] = tag_byte;
                    b->slot_value_refs[a] = v;
                }

                b->ops_translated++;
                return true;
            }

            // Fallback: CALL_C for non-array objects (Map, String, Instance, etc.)
            {
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, obj, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                XirRef off1 = xir_const_i64(b->func, (int64_t)(JIT_CALL_ARGS_OFFSET + 8));
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, key, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            uint8_t obj_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
            uint8_t key_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            int64_t encoded = ((int64_t)obj_tag << 8) | (int64_t)key_tag;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_index_get);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            XirRef v = xir_emit(b->func, blk, XIR_CALL_C, res_type, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_set_slot(b, a, v);
            // Read precise runtime tag from slot_runtime_tags[a] (written by call_c_stub)
            if (res_type == XR_REP_TAGGED && a >= 0 && a < 256) {
                XirRef tag_off = xir_const_i64(b->func,
                    (int64_t)(XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + a));
                XirRef tag_vr = xir_emit_unary(b->func, blk, XIR_LOAD_CORO_BYTE,
                                               XR_REP_I64, tag_off);
                b->slot_tag_refs[a] = tag_vr;
                b->slot_value_refs[a] = v;
            }
            b->ops_translated++;
            return true;
        }

        case OP_ARRAY_GETC: {
            // R[A] = R[B]:Array[C] — constant index
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int c = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, rb);
            XirRef key_c = xir_const_i64(b->func, (int64_t)c);
            XirRef key = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, key_c);

            // Infer result type from container's element type.
            uint8_t obj_tag_check = builder_slot_xr_tag(b, rb);
            uint8_t res_type = XR_REP_TAGGED;
            if (obj_tag_check != VTAG_TAGGED &&
                rb < 256 && builder_slot_xrtype(b, rb)) {
                uint8_t elem_gc = xr_type_element_gc_tag(builder_slot_xrtype(b, rb));
                res_type = xr_slot_to_rep(elem_gc);
            }

            // Inline path for known-PTR array objects with known scalar element type.
            // Only inline when res_type is I64 or F64 (precise scalar) AND
            // the source vreg has a known type (not UNKNOWN from MAP_GETK etc).
            // Inline loads only the 8-byte payload, losing the XrValue tag,
            // so integer 0 becomes indistinguishable from null.
            bool src_unknown = false;
            if (rb >= 0 && rb < 256) {
                XirRef ref = b->slot_map[rb];
                if (xir_ref_is_vreg(ref)) {
                    src_unknown = (xir_ref_ctype(b->func, ref).kind == XIR_TK_UNKNOWN);
                }
            }
            if (ref_xir_type(b->func, obj) == XR_REP_PTR && !src_unknown &&
                (res_type == XR_REP_I64 || res_type == XR_REP_F64)) {
                XirRef len_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_LENGTH_OFFSET);
                XirRef len = xir_emit(b->func, blk, XIR_LOAD32S, XR_REP_I64, obj, len_off);

                xir_emit(b->func, blk, XIR_GUARD_BOUNDS, XR_REP_VOID, key, len);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                int did_gc = builder_add_deopt_info(b, pc);
                if (did_gc >= 0) {
                    blk->ins[blk->nins - 1].dst =
                        xir_const_i64(b->func, (int64_t)did_gc);
                }

                XirRef data_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_DATA_OFFSET);
                XirRef addr_base = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, obj, data_off);
                XirRef data_ptr = xir_emit_unary(b->func, blk, XIR_LOAD, XR_REP_PTR, addr_base);

                // Constant byte offset: c * 16 + 8 (payload within XrValue)
                int64_t byte_off_val = (int64_t)c * XIR_XRVALUE_SIZE + XIR_XRVALUE_PAYLOAD_OFFSET;
                XirRef bo_c = xir_const_i64(b->func, byte_off_val);
                XirRef bo_v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, bo_c);
                XirRef elem_addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, data_ptr, bo_v);

                XirRef v;
                if (res_type == XR_REP_F64) {
                    v = xir_emit_unary(b->func, blk, XIR_LOAD, XR_REP_F64, elem_addr);
                } else {
                    v = xir_emit_unary(b->func, blk, XIR_LOAD, res_type, elem_addr);
                }
                builder_set_slot(b, a, v);
                b->ops_translated++;
                return true;
            }

            // Fallback: CALL_C
            {
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, obj, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                XirRef off1 = xir_const_i64(b->func, (int64_t)(JIT_CALL_ARGS_OFFSET + 8));
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, key, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            uint8_t obj_tag = vtag_to_value_tag(ref_vtag(b->func, obj));
            int64_t encoded = ((int64_t)obj_tag << 8) | (int64_t)XR_TAG_I64;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_index_get);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            XirRef v = xir_emit(b->func, blk, XIR_CALL_C, res_type, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_set_slot(b, a, v);
            // Read precise runtime tag from slot_runtime_tags[a] (written by call_c_stub)
            if (res_type == XR_REP_TAGGED && a >= 0 && a < 256) {
                XirRef tag_off = xir_const_i64(b->func,
                    (int64_t)(XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + a));
                XirRef tag_vr = xir_emit_unary(b->func, blk, XIR_LOAD_CORO_BYTE,
                                               XR_REP_I64, tag_off);
                b->slot_tag_refs[a] = tag_vr;
                b->slot_value_refs[a] = v;
            }
            b->ops_translated++;
            return true;
        }

        case OP_ARRAY_SET: {
            // R[A]:Array[R[B]] = R[C] — use CALL_C(xr_jit_index_set)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, a);
            XirRef key = builder_get_slot(b, blk, rb);
            XirRef val = builder_get_slot(b, blk, rc);
            {
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, obj, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                XirRef off1 = xir_const_i64(b->func, (int64_t)(JIT_CALL_ARGS_OFFSET + 8));
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, key, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                XirRef off2 = xir_const_i64(b->func, (int64_t)(JIT_CALL_ARGS_OFFSET + 16));
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off2, val, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            uint8_t key_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            int64_t encoded = ((int64_t)XR_TAG_PTR << 16) | ((int64_t)key_tag << 8) | val_tag;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_index_set);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            xir_emit(b->func, blk, XIR_CALL_C, XR_REP_VOID, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            b->ops_translated++;
            return true;
        }

        case OP_ARRAY_SETC: {
            // R[A]:Array[B] = R[C] — constant index, use CALL_C(xr_jit_index_set)
            int a = GETARG_A(inst);
            int bi = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, a);
            XirRef key_c = xir_const_i64(b->func, (int64_t)bi);
            XirRef key = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, key_c);
            XirRef val = builder_get_slot(b, blk, rc);
            {
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, obj, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                XirRef off1 = xir_const_i64(b->func, (int64_t)(JIT_CALL_ARGS_OFFSET + 8));
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, key, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                XirRef off2 = xir_const_i64(b->func, (int64_t)(JIT_CALL_ARGS_OFFSET + 16));
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off2, val, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            int64_t encoded = ((int64_t)XR_TAG_PTR << 16) | ((int64_t)XR_TAG_I64 << 8) | val_tag;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_index_set);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            xir_emit(b->func, blk, XIR_CALL_C, XR_REP_VOID, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            b->ops_translated++;
            return true;
        }

        case OP_TARRAY_PUSH: {
            // OP_TARRAY_PUSH: R[A]:TypedArray.push(R[B].i) — treat as generic array push
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef arr = builder_get_slot(b, blk, a);
            XirRef val = builder_get_slot(b, blk, rb);
            // When val slot is I64 but stored as TAGGED, unbox to let xcgen emit push_i
            uint8_t vt = ref_xir_type(b->func, val);
            if (vt != XR_REP_I64 && b->slot_rep[rb] == XR_REP_I64) {
                XirRef none = XIR_NONE;
                val = xir_emit(b->func, blk, XIR_UNBOX_I64, XR_REP_I64, val, none);
            } else if (vt != XR_REP_F64 && b->slot_rep[rb] == XR_REP_F64) {
                XirRef none = XIR_NONE;
                val = xir_emit(b->func, blk, XIR_UNBOX_F64, XR_REP_F64, val, none);
            }
            xir_emit(b->func, blk, XIR_RT_ARRAY_PUSH, XR_REP_VOID, arr, val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            b->ops_translated++;
            return true;
        }

        case OP_ARRAY_LEN: {
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef arr = builder_get_slot(b, blk, rb);
            XirRef v = xir_emit(b->func, blk, XIR_RT_ARRAY_LEN, XR_REP_I64,
                                arr, XIR_NONE);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        /* === Print === */
        case OP_PRINT: {
            // R[A] = value, B = add_space, C = bit0:newline | bit1-2:slot_hint
            int a = GETARG_A(inst);
            int add_space = GETARG_B(inst);
            int c_field = GETARG_C(inst);
            int newline = c_field & 1;
            XirRef val = builder_get_slot(b, blk, a);

            if (b->aot_mode) {
                // AOT: use non-void CALL_C so call_arg_pool binding works.
                // STORE_CORO path fails because void dst → no vreg → pool lost.
                // Encoded flags: bit0=newline, bit1=add_space (AOT codegen
                // handles boxing; no slot_enc/val_tag needed).
                int64_t encoded = ((add_space & 1) << 1) | (newline & 1);
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_print);
                XirRef enc_ref = xir_const_i64(b->func, encoded);
                XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                XR_REP_I64, enc_ref);
                XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                         fn_ref, enc_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                XirRef call_args[1] = { val };
                builder_bind_call_args(b, result, call_args, 1);
            } else {
                // JIT: store value to coro call_args buffer, emit void CALL_C
                {
                    XirRef off0 = xir_const_i64(b->func,
                                                (int64_t)JIT_CALL_ARGS_OFFSET);
                    xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                                 off0, val, XIR_NONE);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                }
                // Convert vtag to value_tag for the runtime print helper.
                uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, a));
                int bc_slot_hint = -1;
                if (a >= 0 && a < 256 &&
                    !xir_ref_is_none(b->slot_tag_refs[a])) {
                    val_tag = 0xFF;
                    bc_slot_hint = a;
                }
                int64_t slot_enc = (bc_slot_hint >= 0)
                    ? (int64_t)bc_slot_hint : 0xFF;
                int64_t encoded = (slot_enc << 24)
                                | ((int64_t)val_tag << 8)
                                | ((add_space & 1) << 1) | (newline & 1);
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_print);
                XirRef enc_ref = xir_const_i64(b->func, encoded);
                XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                XR_REP_I64, enc_ref);
                xir_emit(b->func, blk, XIR_CALL_C, XR_REP_VOID,
                         fn_ref, enc_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            b->ops_translated++;
            return true;
        }

        /* === Typed Array Access === */
        case OP_TARRAY_GET: {
            // R[A].i = R[B]:TypedArray[R[C]] (raw i64 output, no tag)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);

            // Build-time check: inline path assumes 8-byte stride (int64_t).
            // Array<any> stores 16-byte XrValue elements → wrong offset.
            // Bail if element type is not I64 (forces interpreter fallback).
            {
                struct XrType *arr_type = builder_slot_xrtype(b, rb);
                uint8_t elem_tag = xr_type_element_gc_tag(arr_type);
                if (elem_tag != XR_SLOT_I64) {
                    b->ops_skipped++;
                    return true;
                }
            }

            XirRef arr = builder_get_slot(b, blk, rb);
            XirRef idx = builder_get_slot(b, blk, rc);

            // Inline path: load length, bounds check, load data, compute addr, load element
            // 1. Load arr->length (int32 sign-extended to int64)
            XirRef len_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_LENGTH_OFFSET);
            XirRef len = xir_emit(b->func, blk, XIR_LOAD32S, XR_REP_I64, arr, len_off);

            // 2. Bounds check: deopt if (unsigned)idx >= (unsigned)len
            xir_emit(b->func, blk, XIR_GUARD_BOUNDS, XR_REP_VOID, idx, len);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            int did1 = builder_add_deopt_info(b, pc);
            if (did1 >= 0) {
                blk->ins[blk->nins - 1].dst =
                    xir_const_i64(b->func, (int64_t)did1);
            }

            // 3. Load arr->data pointer (void* at offset 16)
            XirRef data_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_DATA_OFFSET);
            XirRef addr_base = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, arr, data_off);
            XirRef data_ptr = xir_emit_unary(b->func, blk, XIR_LOAD, XR_REP_PTR, addr_base);

            // 4. Compute element address: data_ptr + idx * 8 (sizeof(int64_t))
            XirRef elem_size = xir_const_i64(b->func, 8);
            XirRef elem_size_v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, elem_size);
            XirRef byte_off = xir_emit(b->func, blk, XIR_MUL, XR_REP_I64, idx, elem_size_v);
            XirRef elem_addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, data_ptr, byte_off);

            // 5. Load element (raw int64, no tag)
            XirRef result = xir_emit_unary(b->func, blk, XIR_LOAD, XR_REP_I64, elem_addr);

            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_TARRAY_SET: {
            // R[A]:TypedArray[R[B]] = R[C].i (raw i64 input, no tag)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef arr = builder_get_slot(b, blk, a);
            XirRef idx = builder_get_slot(b, blk, rb);
            XirRef val = builder_get_slot(b, blk, rc);

            // Inline: load length, bounds check, load data, compute addr, store element
            // 1. Load arr->length
            XirRef len_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_LENGTH_OFFSET);
            XirRef len = xir_emit(b->func, blk, XIR_LOAD32S, XR_REP_I64, arr, len_off);

            // 2. Bounds check: deopt if (unsigned)idx >= (unsigned)len
            xir_emit(b->func, blk, XIR_GUARD_BOUNDS, XR_REP_VOID, idx, len);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            int did_s = builder_add_deopt_info(b, pc);
            if (did_s >= 0) {
                blk->ins[blk->nins - 1].dst =
                    xir_const_i64(b->func, (int64_t)did_s);
            }

            // 3. Load arr->data pointer
            XirRef data_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_DATA_OFFSET);
            XirRef addr_base = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, arr, data_off);
            XirRef data_ptr = xir_emit_unary(b->func, blk, XIR_LOAD, XR_REP_PTR, addr_base);

            // 4. Compute element address: data_ptr + idx * 8
            XirRef es = xir_const_i64(b->func, 8);
            XirRef es_v = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, es);
            XirRef byte_off = xir_emit(b->func, blk, XIR_MUL, XR_REP_I64, idx, es_v);
            XirRef elem_addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, data_ptr, byte_off);

            // 5. Store element (raw int64, no GC barrier for typed arrays)
            xir_emit_void(b->func, blk, XIR_STORE, elem_addr, val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

            b->ops_translated++;
            return true;
        }

        /* === Typed Array Constant Index Access === */
        case OP_TARRAY_GETC: {
            // R[A].i = R[B]:TypedArray[C] (constant index, raw i64 output)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef arr = builder_get_slot(b, blk, rb);
            XirRef idx_c = xir_const_i64(b->func, (int64_t)rc);
            XirRef idx = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, idx_c);

            // Inline: load length, bounds check, load data, load element
            XirRef len_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_LENGTH_OFFSET);
            XirRef len = xir_emit(b->func, blk, XIR_LOAD32S, XR_REP_I64, arr, len_off);

            xir_emit(b->func, blk, XIR_GUARD_BOUNDS, XR_REP_VOID, idx, len);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            int did_c = builder_add_deopt_info(b, pc);
            if (did_c >= 0) {
                blk->ins[blk->nins - 1].dst =
                    xir_const_i64(b->func, (int64_t)did_c);
            }

            XirRef data_off = xir_const_i64(b->func, (int64_t)XIR_ARRAY_DATA_OFFSET);
            XirRef addr_base = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, arr, data_off);
            XirRef data_ptr = xir_emit_unary(b->func, blk, XIR_LOAD, XR_REP_PTR, addr_base);

            // Constant index: compute byte offset at build time
            XirRef byte_off_c = xir_const_i64(b->func, (int64_t)rc * 8);
            XirRef byte_off = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, byte_off_c);
            XirRef elem_addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR, data_ptr, byte_off);

            XirRef result = xir_emit_unary(b->func, blk, XIR_LOAD, XR_REP_I64, elem_addr);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Typeof (returns XrTypeId as int) === */
        case OP_TYPEOF: {
            // R[A] = typeof(R[B]) → returns int (XrTypeId)
            // Always use runtime helper: slot_tag is flow-insensitive and may
            // not reflect the actual runtime type (e.g., parameter declared int
            // but receiving bool). The helper is cheap — just reads the GC type
            // byte for PTR values or checks tag for primitives.
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            {
                XirRef val = builder_get_slot(b, blk, rb);
                {
                    XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                    xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                                 off0, val, XIR_NONE);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                }
                uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_typeof);
                XirRef tag_ref = xir_const_i64(b->func, (int64_t)val_tag);
                XirRef tag_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                XR_REP_I64, tag_ref);
                XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                         fn_ref, tag_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_set_slot(b, a, result);
            }
            b->ops_translated++;
            return true;
        }

        /* === Array Batch Init === */
        case OP_ARRAY_INIT: {
            // R[A][1..B] = R[A+1..A+B] - batch init array elements
            // Use RT_ARRAY_PUSH: newly created arrays have length=0,
            // so index_set would fail bounds check. Push appends correctly.
            int a = GETARG_A(inst);
            int count = GETARG_B(inst);
            XirRef arr = builder_get_slot(b, blk, a);

            for (int j = 1; j <= count; j++) {
                XirRef val = builder_get_slot(b, blk, a + j);
                xir_emit(b->func, blk, XIR_RT_ARRAY_PUSH, XR_REP_VOID, arr, val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            b->ops_translated++;
            return true;
        }

        /* === Map Operations === */
        case OP_MAP_GET: {
            // R[A] = R[B]:Map[R[C]]
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef map = builder_get_slot(b, blk, rb);
            XirRef key = builder_get_slot(b, blk, rc);
            {
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, map, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                int32_t off1v = JIT_CALL_ARGS_OFFSET + 8;
                XirRef off1 = xir_const_i64(b->func, (int64_t)off1v);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, key, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            uint8_t key_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_map_get);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)key_tag);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_MAP_GETK: {
            // R[A] = R[B]:Map[K[C]] — constant key
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int kc = GETARG_C(inst);

            // Bounds check for constant pool
            if (kc >= PROTO_CONST_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            XirRef map = builder_get_slot(b, blk, rb);
            // Load constant key as tagged value (string ptr)
            XrValue kval = PROTO_CONST_FAST(b->proto, kc);
            XirRef key = xir_const_i64(b->func, kval.i);
            XirRef key_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, key);
            {
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, map, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                int32_t off1v = JIT_CALL_ARGS_OFFSET + 8;
                XirRef off1 = xir_const_i64(b->func, (int64_t)off1v);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, key_val, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            // Key is from constant pool: always PTR (string)
            uint8_t key_tag = (uint8_t)XR_TAG_PTR;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_map_get);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)key_tag);
            XirRef enc_val2 = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR, fn_ref, enc_val2);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            // Use compiler type for map value (Map<K,V> → V known at compile time)
            builder_tag_from_slot(b, result, a);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_MAP_SET: {
            // R[A]:Map[R[B]] = R[C]
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef map = builder_get_slot(b, blk, a);
            XirRef key = builder_get_slot(b, blk, rb);
            XirRef val = builder_get_slot(b, blk, rc);
            {
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, map, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                int32_t off1v = JIT_CALL_ARGS_OFFSET + 8;
                XirRef off1 = xir_const_i64(b->func, (int64_t)off1v);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, key, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                int32_t off2v = JIT_CALL_ARGS_OFFSET + 16;
                XirRef off2 = xir_const_i64(b->func, (int64_t)off2v);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off2, val, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            uint8_t key_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            int64_t encoded = ((int64_t)key_tag << 8) | val_tag;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_map_set);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            xir_emit(b->func, blk, XIR_CALL_C, XR_REP_VOID, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            b->ops_translated++;
            return true;
        }

        case OP_MAP_SETK: {
            // R[A]:Map[K[B]] = R[C] — constant key
            int a = GETARG_A(inst);
            int kb = GETARG_B(inst);
            int rc = GETARG_C(inst);

            // Bounds check for constant pool
            if (kb >= PROTO_CONST_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            XirRef map = builder_get_slot(b, blk, a);
            XrValue kval = PROTO_CONST_FAST(b->proto, kb);
            XirRef key = xir_const_i64(b->func, kval.i);
            XirRef key_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, key);
            XirRef val = builder_get_slot(b, blk, rc);
            {
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, map, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                int32_t off1v = JIT_CALL_ARGS_OFFSET + 8;
                XirRef off1 = xir_const_i64(b->func, (int64_t)off1v);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, key_val, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                int32_t off2v = JIT_CALL_ARGS_OFFSET + 16;
                XirRef off2 = xir_const_i64(b->func, (int64_t)off2v);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off2, val, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            uint8_t key_tag = (uint8_t)XR_TAG_PTR; // constant key is always string ptr
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            int64_t encoded = ((int64_t)key_tag << 8) | val_tag;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_map_set);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            xir_emit(b->func, blk, XIR_CALL_C, XR_REP_VOID, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            b->ops_translated++;
            return true;
        }

        /* === Builtin Globals (read-only) === */
        case OP_GETBUILTIN: {
            // R[A] = builtins[Bx] — read-only predefined global
            int a = GETARG_A(inst);
            int bx = GETARG_Bx(inst);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_getbuiltin);
            XirRef idx_ref = xir_const_i64(b->func, (int64_t)bx);
            XirRef idx_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, idx_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, idx_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_MAP_INCREMENT: {
            // R[A]:Map[R[B]]++ — increment or set to 1 if not exists
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef map = builder_get_slot(b, blk, a);
            XirRef key = builder_get_slot(b, blk, rb);
            {
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, map, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                int32_t off1v = JIT_CALL_ARGS_OFFSET + 8;
                XirRef off1 = xir_const_i64(b->func, (int64_t)off1v);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, key, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            uint8_t key_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_map_increment);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)key_tag);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            xir_emit(b->func, blk, XIR_CALL_C, XR_REP_VOID, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            b->ops_translated++;
            return true;
        }

        case OP_MAP_SETKS: {
            // R[A].fields[0..B-1] = R[A+1]..R[A+B] — batch map set
            // Translate as N individual MAP_SETK operations
            int a = GETARG_A(inst);
            int count = GETARG_B(inst);
            XirRef map = builder_get_slot(b, blk, a);
            for (int j = 0; j < count; j++) {
                XirRef val = builder_get_slot(b, blk, a + 1 + j);
                // Store map, key (constant from j-th position), value
                {
                    XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                    xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off0, map, XIR_NONE);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    // Key: j-th constant from constant pool (pairs: key0, key1, ...)
                    // For batch set, keys are consecutive constants starting from proto->symbols
                    // Use index_set as fallback — store index, map, value
                    int32_t off1v = JIT_CALL_ARGS_OFFSET + 8;
                    XirRef idx_ref = xir_const_i64(b->func, (int64_t)j);
                    XirRef idx_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, idx_ref);
                    XirRef off1 = xir_const_i64(b->func, (int64_t)off1v);
                    xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off1, idx_val, XIR_NONE);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    int32_t off2v = JIT_CALL_ARGS_OFFSET + 16;
                    XirRef off2 = xir_const_i64(b->func, (int64_t)off2v);
                    xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID, off2, val, XIR_NONE);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                }
                uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, a + 1 + j));
                int64_t encoded = ((int64_t)XR_TAG_I64 << 8) | val_tag;
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_map_set);
                XirRef enc_ref = xir_const_i64(b->func, encoded);
                XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
                xir_emit(b->func, blk, XIR_CALL_C, XR_REP_VOID, fn_ref, enc_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            b->ops_translated++;
            return true;
        }

        /* === Misc no-op for JIT === */
        case OP_SET_STORAGE_CTX: {
            // Set storage context — no effect in JIT mode
            b->ops_translated++;
            return true;
        }

        case OP_TO_SHARED: {
            // R[A] = to_shared(R[B]) — in JIT, treat as move
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef src = builder_get_slot(b, blk, rb);
            builder_set_slot(b, a, src);
            b->ops_translated++;
            return true;
        }

        case OP_INST_TYPE_ARGS: {
            // R[A]:Instance.gc.extra = packed type args — no-op for JIT
            b->ops_translated++;
            return true;
        }

        case OP_CALL_KEEP: {
            // R[C] = R[A](R[A+1]...R[A+B]); R[A] preserved
            // Same as CALL but result goes to R[C] instead of R[A]
            int a = GETARG_A(inst);
            int nargs = GETARG_B(inst);
            int result_reg = GETARG_C(inst);

            if (nargs > 8) { b->ops_skipped++; return true; }

            // Check if callee proto is known (from OP_CLOSURE tracking)
            { XirVReg *_cv3 = builder_vreg_for_slot(b, a);
            XrProto *callee_proto = _cv3 ? _cv3->callee_proto : NULL;

            // Store closure to jit_call_args[0] (needed for slow path fallback)
            XirRef func_val = builder_get_slot(b, blk, a);
            {
                XirRef off_ref = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                             off_ref, func_val, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }

            // Store args to jit_call_args[1..n]
            XirRef arg_refs[2] = { XIR_NONE, XIR_NONE };
            for (int j = 0; j < nargs; j++) {
                int32_t off = JIT_CALL_ARGS_OFFSET + (1 + j) * 8;
                XirRef off_ref = xir_const_i64(b->func, (int64_t)off);
                XirRef arg = builder_get_slot(b, blk, a + 1 + j);
                if (j < 2) arg_refs[j] = arg;
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                             off_ref, arg, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }

            if (false && callee_proto && !b->aot_mode) {  // TEMP: disable CALL_KNOWN for CALL_KEEP
                // CALL_KNOWN path: direct JIT-to-JIT call.
                // Callee was eager-compiled in xir_jit_try_compile, so
                // jit_entry is set → codegen emits BLR to fast_entry.
                uint8_t ret_type = callee_proto->return_type_info
                    ? xr_type_rep(callee_proto->return_type_info)
                    : XR_REP_TAGGED;

                if (nargs <= 2) {
                    // Register-passing fast path
                    XirRef proto_val = xir_const_ptr(b->func, (void*)callee_proto);
                    XirRef proto_store = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                        XR_REP_I64, proto_val);
                    {
                        XirRef poff = xir_const_i64(b->func, (int64_t)JIT_CALL_PROTO_OFFSET);
                        xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                                     poff, proto_store, XIR_NONE);
                        blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    }
                    XirRef result = xir_emit(b->func, blk, XIR_CALL_KNOWN_REG, ret_type,
                                             arg_refs[0], arg_refs[1]);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    builder_set_slot(b, result_reg, result);
                } else {
                    // Memory-passing path (nargs > 2)
                    XirRef proto_ref = xir_const_ptr(b->func, (void*)callee_proto);
                    XirRef nargs_ref = xir_const_i64(b->func, (int64_t)nargs);
                    XirRef nargs_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                      XR_REP_I64, nargs_ref);
                    XirRef result = xir_emit(b->func, blk, XIR_CALL_KNOWN, ret_type,
                                             proto_ref, nargs_val);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    builder_set_slot(b, result_reg, result);
                }
            } else {
                // Generic path: callee unknown or CALL_KNOWN disabled, use C bridge
                uint8_t call_rep = XR_REP_I64;
                if (callee_proto && callee_proto->return_type_info) {
                    uint8_t rt = xr_type_rep(callee_proto->return_type_info);
                    if (rt == XR_REP_PTR) call_rep = XR_REP_PTR;
                }
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_call_func);
                XirRef nargs_ref = xir_const_i64(b->func, (int64_t)nargs);
                XirRef nargs_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                  XR_REP_I64, nargs_ref);
                XirRef result = xir_emit(b->func, blk, XIR_CALL_C, call_rep,
                                         fn_ref, nargs_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_set_slot(b, result_reg, result);
            }
            // R[A] preserved — don't clear callee_proto for slot a
            { XirVReg *_rv3 = builder_vreg_for_slot(b, result_reg);
              if (_rv3) _rv3->callee_proto = NULL; }
            } // callee_proto block for INVOKE
            b->ops_translated++;
            return true;
        }

        /* === Register Spill/Reload (no-op for JIT — has own register allocator) === */
        case OP_SPILL: {
            // S[A] = R[B] — JIT ignores spill slots, track as move
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef src = builder_get_slot(b, blk, rb);
            builder_set_slot(b, a, src);
            b->ops_translated++;
            return true;
        }

        case OP_RELOAD: {
            // R[A] = S[B] — JIT ignores spill slots, track as move
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef src = builder_get_slot(b, blk, rb);
            builder_set_slot(b, a, src);
            b->ops_translated++;
            return true;
        }

        /* === Call Variants === */
        case OP_CALL_STATIC: {
            // Same as OP_CALL but with known closure type — skip callable check
            // For JIT, translate identically to OP_CALL
            int a = GETARG_A(inst);
            int nargs = GETARG_B(inst);

            // Check if callee has a known proto (for CALL_KNOWN optimization)
            XirVReg *_cv4 = builder_vreg_for_slot(b, a);
            XrProto *callee_proto = _cv4 ? _cv4->callee_proto : NULL;

            if (callee_proto && callee_proto->jit_entry) {
                // Direct JIT→JIT call (same as OP_CALL's CALL_KNOWN path)
                goto handle_call_known;
            }

            // Store closure to jit_call_args[0] (xr_jit_call_func reads it here)
            {
                XirRef closure = builder_get_slot(b, blk, a);
                XirRef off_ref = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                             off_ref, closure, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }

            // Store args to jit_call_args[1..n]
            for (int j = 0; j < nargs && j < 15; j++) {
                int32_t off = JIT_CALL_ARGS_OFFSET + (1 + j) * 8;
                XirRef off_ref = xir_const_i64(b->func, (int64_t)off);
                XirRef arg = builder_get_slot(b, blk, a + 1 + j);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                             off_ref, arg, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }

            // Call xr_jit_call_func(coro, nargs_encoded)
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_call_func);
            int64_t nargs_enc = (int64_t)nargs;
            XirRef nargs_ref = xir_const_i64(b->func, nargs_enc);
            XirRef nargs_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                              XR_REP_I64, nargs_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, nargs_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;

            handle_call_known: (void)0;
            // Fall through to CALL_KNOWN — store closure+args, emit XIR_CALL_KNOWN
            // Store closure to call_args[0] (needed for slow-path fallback)
            {
                XirRef closure_ref = builder_get_slot(b, blk, a);
                XirRef off0 = xir_const_i64(b->func, (int64_t)JIT_CALL_ARGS_OFFSET);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                             off0, closure_ref, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            // Store args to call_args[1..n]
            for (int j = 0; j < nargs && j < 15; j++) {
                int32_t off = JIT_CALL_ARGS_OFFSET + (1 + j) * 8;
                XirRef off_ref = xir_const_i64(b->func, (int64_t)off);
                XirRef arg = builder_get_slot(b, blk, a + 1 + j);
                xir_emit_raw(b->func, blk, XIR_STORE_CORO, XR_REP_VOID,
                             off_ref, arg, XIR_NONE);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            {
                XirRef proto_ref = xir_const_ptr(b->func, (void *)callee_proto);
                XirRef na_ref = xir_const_i64(b->func, (int64_t)nargs);
                XirRef result2 = xir_emit(b->func, blk, XIR_CALL_KNOWN,
                                          XR_REP_I64, proto_ref, na_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_set_slot(b, a, result2);
            }
            b->ops_translated++;
            return true;
        }

        default:
            return false;
    }
}
