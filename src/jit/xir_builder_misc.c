/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_builder_misc.c - XIR translation for misc/string/concurrency operations
 *
 * KEY CONCEPT:
 *   Handles type names, print, type checks, string operations,
 *   assertions, enum access, channel/scope/await operations,
 *   and other miscellaneous opcodes in the bytecode-to-XIR
 *   translation pipeline.
 */

#include "xir_builder_internal.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xstruct_layout.h"

/*
 * CPS suspend pattern helper: shared by OP_AWAIT, OP_CHAN_SEND, OP_CHAN_RECV.
 *
 * Emits the deopt-marker compare, creates suspend_blk + cont_blk, emits
 * XIR_SUSPEND, and writes suspend_result to result_slot.
 *
 * Caller must:
 *   1. Emit fast-path CALL_C and write fast-path slot values BEFORE calling.
 *   2. After return, optionally emit extra instructions in *out_suspend_blk.
 *   3. Call builder_finalize_cps_suspend() to wire up the CFG.
 */
static XirRef builder_emit_cps_suspend(
    XirBuilder *b, XirBlock *blk, uint32_t pc,
    int result_slot, XirRef fast_result,
    XirRef suspend_arg0, XirRef suspend_arg1,
    void *block_helper,
    XirBlock **out_suspend_blk, XirBlock **out_cont_blk)
{
    // Deopt marker compare
    XirRef marker = xir_const_i64(b->func, (int64_t)0xDEAD0001DEAD0001LL);
    XirRef marker_v = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                     XR_REP_I64, marker);
    XirRef cmp = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64,
                           fast_result, marker_v);

    // Create suspend + continuation blocks
    XirBlock *suspend_blk = xir_func_add_block(b->func, NULL);
    XirBlock *cont_blk = b->pc_to_block[pc + 1];
    if (!cont_blk) cont_blk = xir_func_add_block(b->func, NULL);
    xir_block_set_br(blk, cmp, suspend_blk, cont_blk);
    xir_block_add_pred(suspend_blk, blk, b->func->arena);
    xir_block_add_pred(cont_blk, blk, b->func->arena);

    // Grow block_defs if needed
    uint32_t max_id = suspend_blk->id > cont_blk->id
                    ? suspend_blk->id : cont_blk->id;
    if (max_id >= b->block_defs_size) {
        uint32_t new_size = max_id + 8;
        XR_REALLOC_OR_ABORT(b->block_defs,
                            new_size * sizeof(BraunBlockDef),
                            "xir_builder_misc block_defs grow");
        memset(&b->block_defs[b->block_defs_size], 0,
            (new_size - b->block_defs_size) * sizeof(BraunBlockDef));
        b->block_defs_size = new_size;
    }
    b->block_defs[suspend_blk->id].sealed = true;

    // Emit XIR_SUSPEND with block helper in func metadata
    uint32_t suspend_id = b->func->nsuspend++;
    b->func->suspend_block_helpers[suspend_id] = block_helper;
    XirRef suspend_result = xir_emit(b->func, suspend_blk, XIR_SUSPEND,
                                     XR_REP_I64, suspend_arg0, suspend_arg1);
    suspend_blk->ins[suspend_blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

    // Store suspend_id in vreg metadata and set bc_slot for runtime_tags
    if (xir_ref_is_vreg(suspend_result)) {
        uint32_t vi = XIR_REF_INDEX(suspend_result);
        if (vi < b->func->nvreg) {
            b->func->vregs[vi].call_arg_start = (int16_t)suspend_id;
            if (b->func->vregs[vi].bc_slot < 0)
                b->func->vregs[vi].bc_slot = (int16_t)result_slot;
        }
    }

    // Write suspend_result to result slot
    builder_set_slot_in_block(b, suspend_blk->id, result_slot, suspend_result);

    *out_suspend_blk = suspend_blk;
    *out_cont_blk = cont_blk;
    return suspend_result;
}

// Finalize CPS suspend: connect suspend_blk → cont_blk, seal, advance.
static void builder_finalize_cps_suspend(
    XirBuilder *b, XirBlock *suspend_blk, XirBlock *cont_blk,
    XirBlock **cur_blk)
{
    xir_block_set_jmp(suspend_blk, cont_blk);
    xir_block_add_pred(cont_blk, suspend_blk, b->func->arena);
    b->block_defs[cont_blk->id].sealed = true;
    *cur_blk = cont_blk;
    b->cur_blk_id = cont_blk->id;
}

bool xir_translate_misc_ops(XirBuilder *b, XirBlock **cur_blk,
                            uint32_t pc, XrInstruction inst, OpCode op) {
    XR_DCHECK(b != NULL, "translate_misc_ops: NULL builder");
    XR_DCHECK(cur_blk != NULL, "translate_misc_ops: NULL cur_blk");
    XirBlock *blk = *cur_blk;
    switch (op) {
        /* === Type Name === */
        case OP_TYPENAME: {
            // R[A] = typename(R[B]) — returns type name as string
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int slot_hint = GETARG_C(inst);
            XirRef val = builder_get_slot(b, blk, rb);
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
            int64_t encoded = ((int64_t)slot_hint << 8) | val_tag;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_typename);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &val, 1);
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Debug Dump === */
        case OP_DUMP: {
            // dump(R[A], depth=B) — debug output (no return value)
            int a = GETARG_A(inst);
            int depth = GETARG_B(inst);
            XirRef val = builder_get_slot(b, blk, a);
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, a));
            int64_t encoded = ((int64_t)val_tag << 8) | (depth & 0xFF);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_dump);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef dump_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, dump_result, &val, 1);
            b->ops_translated++;
            return true;
        }

        /* === Range Operations === */
        case OP_NEWRANGE: {
            // R[A] = Range(R[B], R[C])
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef rng_args[2];
            rng_args[0] = builder_get_slot(b, blk, rb);
            rng_args[1] = builder_get_slot(b, blk, rc);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_newrange);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR,
                                     fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, rng_args, 2);
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_RANGE_UNPACK: {
            // R[A] = count, R[A+1] = step, R[A+2] = start (from Range in R[B])
            // Range layout: GCHeader(16) + start(8) + end(8) + step(8)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef range = builder_get_slot(b, blk, rb);

            // Read start, end, step via LOAD_FIELD
            XirRef start_off = xir_const_i64(b->func, 16); // offsetof(XrRange, start)
            XirRef start_val = xir_emit(b->func, blk, XIR_LOAD_FIELD, XR_REP_I64,
                                        range, start_off);
            XirRef end_off = xir_const_i64(b->func, 24);   // offsetof(XrRange, end)
            XirRef end_val = xir_emit(b->func, blk, XIR_LOAD_FIELD, XR_REP_I64,
                                      range, end_off);
            XirRef step_off = xir_const_i64(b->func, 32);  // offsetof(XrRange, step)
            XirRef step_val = xir_emit(b->func, blk, XIR_LOAD_FIELD, XR_REP_I64,
                                       range, step_off);

            // count = (end - start) / step + 1
            XirRef diff = xir_emit(b->func, blk, XIR_SUB, XR_REP_I64, end_val, start_val);
            XirRef count = xir_emit(b->func, blk, XIR_DIV, XR_REP_I64, diff, step_val);
            XirRef one = xir_const_i64(b->func, 1);
            XirRef one_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, one);
            XirRef total = xir_emit(b->func, blk, XIR_ADD, XR_REP_I64, count, one_val);
            builder_tag_vreg(b, start_val, VTAG_I64, 0);
            builder_tag_vreg(b, step_val, VTAG_I64, 0);
            builder_tag_vreg(b, total, VTAG_I64, 0);

            builder_set_slot(b, a, total);       // count
            builder_set_slot(b, a + 1, step_val); // step
            builder_set_slot(b, a + 2, start_val); // index (start)
            b->ops_translated++;
            return true;
        }

        /* === Type Check === */
        case OP_IS: {
            // R[A] = (R[B] is Type[C]) — runtime type check
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int kc = GETARG_C(inst);
            XirRef val = builder_get_slot(b, blk, rb);
            // Bounds check for constant pool
            if (kc >= PROTO_CONST_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            // Get expected type id from constant pool
            XrValue type_val = PROTO_CONST_FAST(b->proto, kc);
            int expected_tid = XR_IS_INT(type_val) ? (int)XR_TO_INT(type_val) : 0;
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
            int64_t encoded = ((int64_t)val_tag << 8) | (expected_tid & 0xFF);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_is_type);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &val, 1);
            builder_set_slot(b, a, result);
            builder_tag_vreg(b, result, VTAG_BOOL, 0);
            b->ops_translated++;
            return true;
        }

        case OP_CHECKTYPE: {
            // CHECKTYPE A B: assert R[A] matches type bitmask K(B).
            // Deopt on mismatch — interpreter re-executes and throws TypeError.
            int a = GETARG_A(inst);
            int kb = GETARG_B(inst);
            if (kb >= PROTO_CONST_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }
            XrValue mask_val = PROTO_CONST_FAST(b->proto, kb);
            if (!XR_IS_INT(mask_val)) {
                b->ops_skipped++;
                return true;
            }
            int64_t mask = XR_TO_INT(mask_val);

            // Compile-time fold: if tag is known, check statically
            uint8_t vtag = builder_slot_xr_tag(b, a);
            int known_tid = -1;
            switch (vtag) {
            case VTAG_I64:  known_tid = 8;  break;  // XR_TID_INT
            case VTAG_F64:  known_tid = 11; break;  // XR_TID_FLOAT
            case VTAG_BOOL: known_tid = 1;  break;  // XR_TID_BOOL
            default: break;
            }

            if (known_tid >= 0) {
                if ((1LL << known_tid) & mask) {
                    // Check passes at compile time — elide entirely
                    b->ops_translated++;
                    return true;
                }
                // Check fails at compile time — unconditional deopt
                int did = builder_add_deopt_info(b, pc);
                XirRef did_ref = xir_const_i64(b->func, (int64_t)(did >= 0 ? did : 0xFFFF));
                xir_emit_unary(b->func, blk, XIR_DEOPT, XR_REP_VOID, XIR_NONE);
                blk->ins[blk->nins - 1].dst = did_ref;
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                b->ops_translated++;
                return true;
            }

            // Runtime check via C helper (TAGGED / PTR values)
            XirRef val = builder_get_slot(b, blk, a);
            uint8_t val_xr_tag = vtag_to_value_tag(vtag);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_checktype);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)val_xr_tag);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            // Pass value and bitmask as call_args[0] and call_args[1]
            XirRef mask_ref = xir_const_i64(b->func, mask);
            XirRef mask_r = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                           XR_REP_I64, mask_ref);
            XirRef ct_args[2] = { val, mask_r };
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, ct_args, 2);

            // Guard: deopt if result == 0 (type mismatch)
            xir_emit(b->func, blk, XIR_GUARD_NONNULL, XR_REP_VOID,
                     result, XIR_NONE);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            int did = builder_add_deopt_info(b, pc);
            if (did >= 0) {
                XirRef did_ref = xir_const_i64(b->func, (int64_t)did);
                blk->ins[blk->nins - 1].dst = did_ref;
            }
            b->ops_translated++;
            return true;
        }

        /* === String Operations === */
        case OP_CHR: {
            // R[A] = chr(R[B]) — codepoint to string
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef cp = builder_get_slot(b, blk, rb);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_chr);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &cp, 1);
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_SUBSTRING: {
            // R[A] = R[B][R[C]:R[D]] — substring (A=dest, B=str, C=start, sC=end via next inst)
            // Encoding: A=dest, B=source string, sC=end_offset
            // Actually: R[A] = R[B].substring(sB, sC)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef ss_args[3];
            ss_args[0] = builder_get_slot(b, blk, rb);
            ss_args[1] = builder_get_slot(b, blk, a + 1);  // start index
            ss_args[2] = builder_get_slot(b, blk, rc);      // end index
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_substring);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, ss_args, 3);
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_STR_REPEAT: {
            // R[A] = R[B] * R[C] — string repeat
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef sr_args[2];
            sr_args[0] = builder_get_slot(b, blk, rb);
            sr_args[1] = builder_get_slot(b, blk, rc);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_str_repeat);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, sr_args, 2);
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Set Operations === */
        case OP_NEWSET: {
            // R[A] = new Set (empty, normal mode)
            int a = GETARG_A(inst);
            int b_arg = GETARG_B(inst);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_newset);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)b_arg);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Field Access with IC === */
        case OP_GETFIELD_IC: {
            // R[A] = R[B].K[C] — instance field access (IC optimization)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int kc = GETARG_C(inst);

            // Bounds check for constant pool
            if (kc >= PROTO_CONST_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            // Compile-time devirtualization: if receiver type is a known class
            // instance, resolve field index now and emit direct LOAD_FIELD.
            XrValue fname_val = PROTO_CONST_FAST(b->proto, kc);
            if (rb < 256 && builder_slot_xrtype(b, rb) &&
                builder_slot_xrtype(b, rb)->kind == XR_KIND_INSTANCE &&
                builder_slot_xrtype(b, rb)->instance.class_ref &&
                XR_IS_STRING(fname_val)) {
                XrClassInfo *ci = builder_slot_xrtype(b, rb)->instance.class_ref;
                const char *fname = XR_TO_STRING(fname_val)->data;
                int field_idx = -1;
                for (int fi = 0; fi < ci->field_count; fi++) {
                    if (ci->fields[fi] && ci->fields[fi]->name &&
                        strcmp(ci->fields[fi]->name, fname) == 0) {
                        field_idx = fi;
                        break;
                    }
                }
                if (field_idx >= 0) {
                    XirRef obj = builder_get_slot(b, blk, rb);
                    int32_t byte_offset = XR_INSTANCE_FIELDS_OFFSET +
                                          field_idx * XR_XRVALUE_SIZE;
                    XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
                    XirRef v = xir_emit(b->func, blk, XIR_LOAD_FIELD,
                                        XR_REP_I64, obj, off_ref);
                    builder_tag_from_slot(b, v, a);
                    builder_set_slot(b, a, v);
                    b->ops_translated++;
                    return true;
                }
            }

            // Slow path: C bridge for unknown receiver type or field miss
            XirRef obj = builder_get_slot(b, blk, rb);
            int64_t fname_ptr = XR_IS_STRING(fname_val) ?
                (int64_t)(uintptr_t)XR_TO_STRING(fname_val) : 0;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_getfield_ic);
            XirRef enc_ref = xir_const_i64(b->func, fname_ptr);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &obj, 1);
            builder_tag_vreg(b, result, VTAG_TAGGED, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Method Invocation === */
        case OP_INVOKE_DIRECT: {
            // R[A] = R[A+1].methods[B](R[A+2]..R[A+1+nargs])
            // A=base, B=method_idx, C=nargs|tail_flag
            int a = GETARG_A(inst);
            int method_idx = GETARG_B(inst);
            int c_raw = GETARG_C(inst);
            int nargs = c_raw & 0x7F;

            // Collect args: [0]=receiver, [1..nargs]=args
            XirRef id_args[16];
            id_args[0] = builder_get_slot(b, blk, a + 1);
            for (int j = 0; j < nargs && j < 15; j++)
                id_args[1 + j] = builder_get_slot(b, blk, a + 2 + j);
            uint16_t id_nca = (uint16_t)(1 + (nargs < 15 ? nargs : 15));

            uint8_t recv_tag = vtag_to_value_tag(builder_slot_xr_tag(b, a + 1));
            int64_t encoded = ((int64_t)recv_tag << 24) | ((int64_t)method_idx << 8) | (nargs & 0xFF);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_invoke_direct);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, id_args, id_nca);
            builder_tag_vreg(b, result, VTAG_TAGGED, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Slice === */
        case OP_SLICE: {
            // R[A] = R[B][R[C]:R[C+1]]
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef sl_args[3];
            sl_args[0] = builder_get_slot(b, blk, rb);
            sl_args[1] = builder_get_slot(b, blk, rc);
            sl_args[2] = builder_get_slot(b, blk, rc + 1);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_slice);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR,
                                     fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, sl_args, 3);
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Assertions === */
        case OP_ASSERT: {
            // if !R[A] then assert error; B=loc_idx, C=negate
            int ra = GETARG_A(inst);
            int rc = GETARG_C(inst);
            XirRef cond = builder_get_slot(b, blk, ra);
            int64_t encoded = ((int64_t)rc << 16);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_assert);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef assert_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, assert_result, &cond, 1);
            b->ops_translated++;
            return true;
        }

        case OP_ASSERT_EQ: {
            // if R[A] != R[B] then assert error; C=loc_idx
            int ra = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef aeq_args[2];
            aeq_args[0] = builder_get_slot(b, blk, ra);
            aeq_args[1] = builder_get_slot(b, blk, rb);
            uint8_t a_tag = vtag_to_value_tag(builder_slot_xr_tag(b, ra));
            uint8_t b_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
            uint8_t a_bc = 0xFF, b_bc = 0xFF;
            if (ra >= 0 && ra < 256 && !xir_ref_is_none(b->slot_tag_refs[ra])) {
                a_tag = 0xFF; a_bc = (uint8_t)ra;
            }
            if (rb >= 0 && rb < 256 && !xir_ref_is_none(b->slot_tag_refs[rb])) {
                b_tag = 0xFF; b_bc = (uint8_t)rb;
            }
            int64_t encoded = ((int64_t)a_bc << 24) | ((int64_t)b_bc << 16)
                            | ((int64_t)a_tag << 8) | b_tag;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_assert_eq);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef aeq_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, aeq_result, aeq_args, 2);
            b->ops_translated++;
            return true;
        }

        case OP_ASSERT_NE: {
            // if R[A] == R[B] then assert error; C=loc_idx
            int ra = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef ane_args[2];
            ane_args[0] = builder_get_slot(b, blk, ra);
            ane_args[1] = builder_get_slot(b, blk, rb);
            uint8_t a_tag = vtag_to_value_tag(builder_slot_xr_tag(b, ra));
            uint8_t b_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
            uint8_t a_bc = 0xFF, b_bc = 0xFF;
            if (ra >= 0 && ra < 256 && !xir_ref_is_none(b->slot_tag_refs[ra])) {
                a_tag = 0xFF; a_bc = (uint8_t)ra;
            }
            if (rb >= 0 && rb < 256 && !xir_ref_is_none(b->slot_tag_refs[rb])) {
                b_tag = 0xFF; b_bc = (uint8_t)rb;
            }
            int64_t encoded = ((int64_t)a_bc << 24) | ((int64_t)b_bc << 16)
                            | ((int64_t)a_tag << 8) | b_tag;
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_assert_ne);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef ane_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, ane_result, ane_args, 2);
            b->ops_translated++;
            return true;
        }

        /* === Enum Operations === */
        case OP_ENUM_ACCESS: {
            // R[A] = R[B].members[R[C]]
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef ea_args[2];
            ea_args[0] = builder_get_slot(b, blk, rb);
            ea_args[1] = builder_get_slot(b, blk, rc);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_enum_access);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, ea_args, 2);
            builder_tag_vreg(b, result, VTAG_TAGGED, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_ENUM_NAME: {
            // R[A] = R[B].name — enum value member name
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef ev = builder_get_slot(b, blk, rb);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_enum_name);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &ev, 1);
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_ENUM_CONVERT: {
            // R[A] = enum_from_value(R[B], R[C])
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef ec_args[2];
            ec_args[0] = builder_get_slot(b, blk, rb);
            ec_args[1] = builder_get_slot(b, blk, rc);
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_enum_convert);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)val_tag);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, ec_args, 2);
            builder_tag_vreg(b, result, VTAG_TAGGED, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Bytes Operations === */
        case OP_BYTES_NEW: {
            // R[A] = Bytes(R[A+1..A+B])
            int a = GETARG_A(inst);
            int nargs = GETARG_B(inst);
            XirRef bn_args[8];
            int bn_count = (nargs < 8) ? nargs : 8;
            for (int j = 0; j < bn_count; j++)
                bn_args[j] = builder_get_slot(b, blk, a + 1 + j);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_bytes_new);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)nargs);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, bn_args, (uint16_t)bn_count);
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        /* === Deep Copy === */
        case OP_COPY: {
            // R[A] = deep_copy(R[B])
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef val = builder_get_slot(b, blk, rb);

            // Try inline copy for flat-copyable structs
            int field_count = 0;
            struct XrType *st = builder_slot_xrtype(b, rb);
            if (st && (st->kind == XR_KIND_CLASS || st->kind == XR_KIND_INSTANCE)
                && st->instance.class_ref) {
                field_count = st->instance.class_ref->field_count;
            }

            if (field_count > 0 && field_count <= 32) {
                // Inline struct copy: XIR_ALLOC + field-by-field copy
                // XrInstance layout: XrGCHeader(16) + klass*(8) + XrValue fields[]
                int32_t alloc_size = XR_INSTANCE_FIELDS_OFFSET +
                                     field_count * XR_XRVALUE_SIZE;
                int64_t packed = (int64_t)XR_TINSTANCE;
                XirRef pack_ref = xir_const_i64(b->func, packed);
                XirRef size_ref = xir_const_i64(b->func, (int64_t)alloc_size);
                XirRef dst = xir_emit(b->func, blk, XIR_ALLOC, XR_REP_PTR,
                                      pack_ref, size_ref);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

                // Copy klass pointer (raw ptr at GC header size offset)
                // Must use ADD+LOAD/STORE instead of LOAD_FIELD/STORE_FIELD
                // because klass is a plain pointer, not an XrValue
                XirRef klass_off = xir_const_i64(b->func, (int64_t)XGC_HEADER_SIZE);
                XirRef klass_src_addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR,
                                                 val, klass_off);
                XirRef klass_val = xir_emit(b->func, blk, XIR_LOAD, XR_REP_PTR,
                                            klass_src_addr, XIR_NONE);
                XirRef klass_dst_addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR,
                                                 dst, klass_off);
                xir_emit_raw(b->func, blk, XIR_STORE, XR_REP_VOID,
                             XIR_NONE, klass_dst_addr, klass_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

                // Copy each instance field
                for (int fi = 0; fi < field_count; fi++) {
                    int32_t foff = XR_INSTANCE_FIELDS_OFFSET +
                                   fi * XR_XRVALUE_SIZE;
                    XirRef off_ref = xir_const_i64(b->func, (int64_t)foff);
                    XirRef fval = xir_emit(b->func, blk, XIR_LOAD_FIELD,
                                           XR_REP_I64, val, off_ref);
                    xir_emit_raw(b->func, blk, XIR_STORE_FIELD,
                                 0, off_ref, dst, fval);
                }

                builder_set_slot(b, a, dst);
                builder_tag_vreg(b, dst, VTAG_PTR, XR_TINSTANCE);
            } else {
                // Fallback: delegate to C runtime
                uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
                XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_deep_copy);
                XirRef enc_ref = xir_const_i64(b->func, (int64_t)val_tag);
                XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                XR_REP_I64, enc_ref);
                XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                         fn_ref, enc_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, result, &val, 1);
                builder_tag_vreg(b, result, VTAG_PTR, 0);
                builder_set_slot(b, a, result);
            }
            b->slot_rep[a] = XR_REP_PTR;
            b->ops_translated++;
            return true;
        }

        /* === StringBuilder === */
        case OP_STRBUF_NEW: {
            // R[A] = new StringBuilder
            int a = GETARG_A(inst);
            XirRef fn_ref = xir_const_ptr(b->func,
                b->aot_mode ? (void*)xrt_strbuf_new_sentinel : (void*)xr_jit_strbuf_new);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR, fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->slot_rep[a] = XR_REP_PTR;
            b->ops_translated++;
            return true;
        }

        /* === Deopt-to-VM opcodes (rare in hot loops) === */
        case OP_IMPORT:
        case OP_DEFER:
        case OP_ABSTRACT_ERROR:
        case OP_GETSUPER:
        case OP_SUPERINVOKE:
        case OP_CLASS_CREATE_FROM_DESCRIPTOR:
        case OP_CLINIT_CALL:
        /* === Channel (non-blocking) === */
        case OP_CHAN_NEW: {
            // R[A] = Channel(Bx) — create channel with buffer
            int a = GETARG_A(inst);
            int buffer_size = GETARG_Bx(inst);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_chan_new);
            XirRef bs_ref = xir_const_i64(b->func, (int64_t)buffer_size);
            XirRef bs_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                           XR_REP_I64, bs_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, bs_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_CHAN_CLOSE: {
            // R[A].close()
            int a = GETARG_A(inst);
            XirRef ch = builder_get_slot(b, blk, a);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_chan_close);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zero_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, zero);
            XirRef cc_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, zero_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, cc_result, &ch, 1);
            b->ops_translated++;
            return true;
        }

        case OP_CHAN_IS_CLOSED: {
            // R[A] = R[B].isClosed()
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef ch = builder_get_slot(b, blk, rb);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_chan_is_closed);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zero_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, zero_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &ch, 1);
            builder_set_slot(b, a, result);
            // isClosed returns bool (0/1) — tag for correct print dispatch
            builder_tag_vreg(b, result, VTAG_BOOL, 0);
            b->ops_translated++;
            return true;
        }

        case OP_CHAN_TRY_SEND: {
            // R[A] = R[B].trySend(R[C]) → bool
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef ts_args[2];
            ts_args[0] = builder_get_slot(b, blk, rb);
            ts_args[1] = builder_get_slot(b, blk, rc);
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_chan_try_send);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)val_tag);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, ts_args, 2);
            builder_set_slot(b, a, result);
            // trySend returns bool (0/1) — tag it for correct print dispatch
            builder_tag_vreg(b, result, VTAG_BOOL, 0);
            b->ops_translated++;
            return true;
        }

        /* === Channel non-blocking receive (multi-return) === */
        case OP_CHAN_TRY_RECV: {
            // R[A] = value, R[A+1] = ok (bool)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef ch = builder_get_slot(b, blk, rb);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_chan_try_recv);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zero_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, zero_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &ch, 1);
            // R[A] = received value (tagged)
            builder_set_slot(b, a, result);
            b->slot_rep[a] = XR_REP_TAGGED;
            // R[A+1] = ok flag: load from jit_call_args[1]
            {
                XirRef off1 = xir_const_i64(b->func, (int64_t)(JIT_CALL_ARGS_OFFSET + 8));
                XirRef ok = xir_emit_unary(b->func, blk, XIR_LOAD_CORO,
                                           XR_REP_I64, off1);
                builder_set_slot(b, a + 1, ok);
                b->slot_rep[a + 1] = XR_REP_I64;
                // ok flag is bool (0/1) — tag for correct print dispatch
                builder_tag_vreg(b, ok, VTAG_BOOL, 0);
            }
            b->ops_translated++;
            return true;
        }

        /* === Structured concurrency: scope enter === */
        case OP_SCOPE_ENTER: {
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_scope_enter);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zero_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, zero);
            xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, zero_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            b->ops_translated++;
            return true;
        }

        /* === Structured concurrency: scope exit (may deopt if blocking) === */
        case OP_SCOPE_EXIT: {
            b->func->has_coro_deopt = true;
            // Add deopt info before the call (in case helper returns deopt marker)
            int deopt_id = builder_add_deopt_info(b, pc);
            (void)deopt_id;

            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_scope_exit);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zero_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, zero_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;

            // Check if helper returned deopt marker → return it to trigger deopt
            XirRef marker = xir_const_i64(b->func, (int64_t)0xDEAD0001DEAD0001LL);
            XirRef marker_v = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, marker);
            XirRef cmp = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64,
                                  result, marker_v);

            // Split: deopt_block returns marker, cont_blk proceeds.
            // cont_blk is the pre-existing block at pc+1 (created during
            // block scanning), so Braun SSA handles it naturally.
            XirBlock *deopt_blk = xir_func_add_block(b->func, NULL);
            XirBlock *cont_blk = b->pc_to_block[pc + 1];
            if (!cont_blk) {
                cont_blk = xir_func_add_block(b->func, NULL);
            }
            xir_block_set_br(blk, cmp, deopt_blk, cont_blk);
            xir_block_add_pred(deopt_blk, blk, b->func->arena);
            xir_block_add_pred(cont_blk, blk, b->func->arena);

            // Deopt block: keep live vregs alive then deopt
            {
                int max_s = b->proto->maxstacksize;
                if (max_s > 256) max_s = 256;
                int arg_idx = 0;
                for (int i = 0; i < max_s && arg_idx < 14; i++) {
                    XirRef ref = b->slot_map[i];
                    if (ref == XIR_NONE || !xir_ref_is_vreg(ref)) continue;
                    XirRef off = xir_const_i64(b->func,
                        (int64_t)(JIT_CALL_ARGS_OFFSET + arg_idx * 8));
                    xir_emit_raw(b->func, deopt_blk, XIR_STORE_CORO,
                                 XR_REP_VOID, off, ref, XIR_NONE);
                    deopt_blk->ins[deopt_blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    arg_idx++;
                }
            }
            if (deopt_id >= 0) {
                XirRef did_ref = xir_const_i64(b->func, (int64_t)deopt_id);
                xir_emit_unary(b->func, deopt_blk, XIR_DEOPT, XR_REP_VOID, XIR_NONE);
                deopt_blk->ins[deopt_blk->nins - 1].dst = did_ref;
                deopt_blk->ins[deopt_blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            deopt_blk->jmp.type = XIR_JMP_RET;
            deopt_blk->jmp.arg = marker_v;

            // Continue in cont_blk (pre-existing block at pc+1).
            // Mark sealed and update cur_blk_id for braun_write_var.
            if (cont_blk->id < b->block_defs_size)
                b->block_defs[cont_blk->id].sealed = true;
            *cur_blk = cont_blk;
            b->cur_blk_id = cont_blk->id;
            b->ops_translated++;
            return true;
        }

        /* === Spawn with continuation (structured concurrency) === */
        case OP_SPAWN_CONT: {
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int c_raw = GETARG_C(inst);
            int nargs = c_raw & 0x7F;

            // Collect args: [0]=closure, [1..nargs]=args
            XirRef sp_args[16];
            sp_args[0] = builder_get_slot(b, blk, rb);
            int sp_count = 1;
            for (int i = 0; i < nargs && i < 15; i++)
                sp_args[sp_count++] = builder_get_slot(b, blk, rb + 1 + i);

            // Encode arg xr_tags into high bits of extra_arg.
            // bits[7:0] = c_raw (nargs + fire_and_forget flag)
            // bits[8+i*8 : 15+i*8] = arg[i] xr_tag (up to 7 args at bits 8..55)
            // Runtime uses these for precise value reconstruction, avoiding the
            // UNKNOWN heuristic which breaks for pointers above the 8GB threshold.
            int64_t extra_enc = (int64_t)c_raw;
            for (int i = 0; i < nargs && i < 7; i++) {
                uint8_t arg_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb + 1 + i));
                extra_enc |= ((int64_t)arg_tag << (8 + i * 8));
            }
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_spawn_cont);
            XirRef enc_ref = xir_const_i64(b->func, extra_enc);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, sp_args, (uint16_t)sp_count);
            // R[A] = coro value (tagged pointer — set xr_tag=UNKNOWN so deopt
            // recovery uses raw-value heuristic instead of assuming I64)
            builder_set_slot(b, a, result);
            b->slot_rep[a] = XR_REP_TAGGED;
            {
                // Result type is unknown (TAGGED) — ctype defaults to UNKNOWN from xir_emit
            }
            b->ops_translated++;
            return true;
        }

        /* === Await coroutine (fast path or CPS suspend) === */
        case OP_AWAIT: {
            b->func->has_coro_deopt = true;
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int discard_result = GETARG_C(inst);
            XirRef coro_ref = builder_get_slot(b, blk, rb);

            // Fast path: xr_jit_await returns result if task done, DEOPT_MARKER if not
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_await);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)discard_result);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef fast_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, fast_result, &coro_ref, 1);

            builder_set_slot(b, a, fast_result);

            XirBlock *suspend_blk, *cont_blk;
            builder_emit_cps_suspend(b, blk, pc, a, fast_result,
                                     coro_ref, enc_val, NULL,
                                     &suspend_blk, &cont_blk);
            builder_finalize_cps_suspend(b, suspend_blk, cont_blk, cur_blk);
            b->slot_rep[a] = XR_REP_TAGGED;
            b->ops_translated++;
            return true;
        }

        /* === Blocking channel send (JIT CPS via XIR_SUSPEND) === */
        case OP_CHAN_SEND: {
            b->func->has_coro_deopt = true;
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef cs_args[2];
            cs_args[0] = builder_get_slot(b, blk, rb);  // channel
            cs_args[1] = builder_get_slot(b, blk, rc);  // value
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));

            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_chan_send);
            XirRef enc_ref = xir_const_i64(b->func, (int64_t)val_tag);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef zero_ref = xir_const_i64(b->func, 0);
            XirRef zero_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, zero_ref);
            XirRef fast_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                          fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, fast_result, cs_args, 2);

            builder_set_slot(b, a, fast_result);

            XirBlock *suspend_blk, *cont_blk;
            builder_emit_cps_suspend(b, blk, pc, a, fast_result,
                                     cs_args[0], zero_val,
                                     (void *)xr_jit_chan_send_block,
                                     &suspend_blk, &cont_blk);
            builder_finalize_cps_suspend(b, suspend_blk, cont_blk, cur_blk);
            b->slot_rep[a] = XR_REP_TAGGED;
            b->ops_translated++;
            return true;
        }

        /* === Blocking channel recv (JIT CPS via XIR_SUSPEND) === */
        case OP_CHAN_RECV: {
            b->func->has_coro_deopt = true;
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            XirRef ch_ref = builder_get_slot(b, blk, rb);

            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_chan_recv);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zero_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, zero);
            XirRef fast_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                          fn_ref, zero_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, fast_result, &ch_ref, 1);

            // Fast path writes: R[A] = value, R[A+1] = ok flag from call_args[1]
            builder_set_slot(b, a, fast_result);
            {
                XirRef off1 = xir_const_i64(b->func, (int64_t)(JIT_CALL_ARGS_OFFSET + 8));
                XirRef ok = xir_emit_unary(b->func, blk, XIR_LOAD_CORO,
                                           XR_REP_I64, off1);
                builder_set_slot_in_block(b, blk->id, a + 1, ok);
                builder_tag_vreg(b, ok, VTAG_BOOL, 0);
            }

            XirBlock *suspend_blk, *cont_blk;
            builder_emit_cps_suspend(b, blk, pc, a, fast_result,
                                     ch_ref, zero_val,
                                     (void *)xr_jit_chan_recv_block,
                                     &suspend_blk, &cont_blk);

            // Blocked→resumed path: ok is always true (sender wrote value)
            {
                XirRef one_c = xir_const_i64(b->func, 1);
                XirRef one_v = xir_emit_unary(b->func, suspend_blk, XIR_CONST_I64,
                                              XR_REP_I64, one_c);
                builder_set_slot_in_block(b, suspend_blk->id, a + 1, one_v);
            }

            builder_finalize_cps_suspend(b, suspend_blk, cont_blk, cur_blk);
            b->slot_rep[a] = XR_REP_TAGGED;
            b->slot_rep[a + 1] = XR_REP_I64;
            b->ops_translated++;
            return true;
        }

        /* === go closure(args...) — fire-and-forget coroutine === */
        case OP_GO: {
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int nargs = GETARG_C(inst);

            // Collect args: [0]=closure, [1..nargs]=args
            XirRef go_args[16];
            go_args[0] = builder_get_slot(b, blk, rb);
            int go_count = 1;
            for (int i = 0; i < nargs && i < 15; i++)
                go_args[go_count++] = builder_get_slot(b, blk, rb + 1 + i);

            // Scan ahead for optional NOP annotations (name, priority)
            int priority = 1;
            int next_pc = pc + 1;
            int code_len = b->proto->code.count;
            while (next_pc < code_len) {
                XrInstruction ni = PROTO_CODE(b->proto, next_pc);
                if (GET_OPCODE(ni) != OP_NOP) break;
                int na = GETARG_A(ni);
                if (na == 1) { next_pc++; continue; }  // name NOP — skip
                if (na == 2) { priority = GETARG_Bx(ni); next_pc++; continue; }
                break;
            }

            // Encode: bits[0:7]=nargs, bits[8:15]=priority
            int64_t extra_enc = (int64_t)(nargs & 0xFF) | ((int64_t)(priority & 0xFF) << 8);

            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_go);
            XirRef enc_ref = xir_const_i64(b->func, extra_enc);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, go_args, (uint16_t)go_count);

            builder_set_slot(b, a, result);
            b->slot_rep[a] = XR_REP_TAGGED;
            b->ops_translated++;
            return true;
        }

        /* === go receiver.method(args...) — deopt to interpreter === */
        case OP_GO_INVOKE: {
            b->func->has_coro_deopt = true;
            int deopt_id = builder_add_deopt_info(b, pc);

            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_go_invoke);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zero_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, zero_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            (void)deopt_id;
            (void)result;
            b->ops_translated++;
            return true;
        }

        /* === DEOPT_FALLBACK opcodes: emit unconditional deopt so hot loops
         * before these instructions can still run JIT-compiled. === */
        case OP_AWAIT_ALL:
        case OP_AWAIT_ANY:
        case OP_AWAIT_TIMEOUT:
        case OP_CHAN_RECV_TIMEOUT:
        case OP_CHAN_SEND_TIMEOUT:
        case OP_SELECT_START:
        case OP_SELECT_CASE:
        case OP_SELECT_BLOCK:
        case OP_SELECT_END: {
            b->func->has_coro_deopt = true;
            int deopt_id = builder_add_deopt_info(b, pc);
            if (deopt_id >= 0) {
                XirRef did_ref = xir_const_i64(b->func, (int64_t)deopt_id);
                xir_emit_unary(b->func, blk, XIR_DEOPT, XR_REP_VOID, XIR_NONE);
                blk->ins[blk->nins - 1].dst = did_ref;
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            }
            // Return DEOPT_MARKER to trigger deopt recovery
            XirRef marker = xir_const_i64(b->func, (int64_t)XIR_DEOPT_MARKER);
            XirRef marker_v = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                             XR_REP_I64, marker);
            blk->jmp.type = XIR_JMP_RET;
            blk->jmp.arg = marker_v;
            b->ops_translated++;
            return true;
        }

        // Coroutine/Channel (require VM scheduler — still BAIL_OUT)
        case OP_CHAN_NEW_NAMED:
        case OP_YIELD:
        case OP_SLEEP:
        case OP_CORO_CTRL:
        case OP_SET_PRIORITY:
        case OP_CANCELLED:
        case OP_TIME_AFTER:
        case OP_LOCK_THREAD:
        case OP_UNLOCK_THREAD:
        // Coroutine local storage
        case OP_GET_LOCAL:
        case OP_SET_LOCAL:
        // Misc
        case OP_REGEX_COMPILE:
            b->ops_translated++;
            return true;

        /* === Export (no-op for JIT — module-level only) === */
        case OP_EXPORT:
        case OP_EXPORT_ALL:
            b->ops_translated++;
            return true;

        /* === Struct Native Storage === */
        case OP_NEW_STRUCT: {
            // R[A] = new struct in struct_area, class from R[B], slot offset = C
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int slot_offset = GETARG_C(inst);
            XirRef class_ref = builder_get_slot(b, blk, rb);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_new_struct);
            XirRef extra = xir_const_i64(b->func, (int64_t)slot_offset);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR,
                                     fn_ref, extra);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &class_ref, 1);
            builder_set_slot(b, a, result);
            b->slot_rep[a] = XR_REP_PTR;
            // Phase 2: track struct layout for downstream GET/SET inlining
            { XirVReg *_sv = builder_vreg_for_slot(b, a);
              if (_sv) {
                _sv->struct_idx = (int16_t)slot_offset;
                if (slot_offset >= 0 && slot_offset < b->proto->struct_layout_count)
                    _sv->layout = b->proto->struct_layouts[slot_offset];
              }
            }
            b->ops_translated++;
            return true;
        }

        case OP_STRUCT_GET: {
            // R[A] = struct(R[B]).field[C] — read native field
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int field_idx = GETARG_C(inst);

            // Phase 2: inline load when layout known at compile time
            XirVReg *_sv_rb = builder_vreg_for_slot(b, rb);
            int sidx = _sv_rb ? (int)_sv_rb->struct_idx : -1;
            XrStructLayout *layout = NULL;
            if (sidx >= 0 && sidx < b->proto->struct_layout_count)
                layout = b->proto->struct_layouts[sidx];
            // Fallback: layout propagated via vreg
            if (!layout && _sv_rb)
                layout = _sv_rb->layout;

            if (layout && field_idx < layout->field_count) {
                XrStructFieldLayout *fl = &layout->fields[field_idx];
                uint8_t nt = fl->native_type;
                // Pick the load instruction based on native type
                int load_op = -1;
                uint8_t rep = XR_REP_I64;
                switch (nt) {
                    case XR_NATIVE_I64: case XR_NATIVE_U64:
                        load_op = XIR_LOAD; rep = XR_REP_I64; break;
                    case XR_NATIVE_F64:
                        load_op = XIR_LOAD; rep = XR_REP_F64; break;
                    case XR_NATIVE_BOOL: case XR_NATIVE_U8:
                        load_op = XIR_LOAD8Z; break;
                    case XR_NATIVE_I8:
                        load_op = XIR_LOAD8S; break;
                    case XR_NATIVE_U16:
                        load_op = XIR_LOAD16Z; break;
                    case XR_NATIVE_I16:
                        load_op = XIR_LOAD16S; break;
                    case XR_NATIVE_U32:
                        load_op = XIR_LOAD32Z; break;
                    case XR_NATIVE_I32:
                        load_op = XIR_LOAD32S; break;
                    case XR_NATIVE_F32:
                        load_op = XIR_LOAD_F32; rep = XR_REP_F64; break;
                    case XR_NATIVE_STRING:
                        load_op = XIR_LOAD; rep = XR_REP_PTR; break;
                    case XR_NATIVE_ARRAY: {
                        // Inline base pointer for fixed array: addr = struct_ptr + 8 + offset
                        XirRef struct_ref = builder_get_slot(b, blk, rb);
                        int32_t byte_off = 8 + fl->offset;
                        XirRef off_c = xir_const_i64(b->func, (int64_t)byte_off);
                        XirRef base = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR,
                                               struct_ref, off_c);
                        builder_set_slot(b, a, base);
                        if (a < 256) b->slot_rep[a] = XR_REP_PTR;
                        { XirVReg *_av = builder_vreg_for_slot(b, a);
                          if (_av) { _av->array_etype = fl->elem_native_type;
                                     _av->array_ecount = fl->elem_count; } }
                        b->ops_translated++;
                        return true;
                    }
                    default: break; // XR_NATIVE_STRUCT → fallback
                }
                if (load_op >= 0) {
                    XirRef struct_ref = builder_get_slot(b, blk, rb);
                    int32_t byte_off = 8 + fl->offset;
                    XirRef off_c = xir_const_i64(b->func, (int64_t)byte_off);
                    XirRef addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR,
                                           struct_ref, off_c);
                    XirRef v = xir_emit(b->func, blk, load_op, rep,
                                        addr, XIR_NONE);
                    builder_set_slot(b, a, v);
                    if (a < 256) b->slot_rep[a] = rep;
                    b->ops_translated++;
                    return true;
                }
            }

            // Fallback: CALL_C runtime helper
            XirRef struct_ref = builder_get_slot(b, blk, rb);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_struct_get);
            XirRef extra = xir_const_i64(b->func, (int64_t)field_idx);
            XirRef v = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                fn_ref, extra);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, v, &struct_ref, 1);
            builder_tag_from_slot(b, v, a);
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        case OP_STRUCT_SET: {
            // struct(R[A]).field[B] = R[C] — write native field
            int a = GETARG_A(inst);
            int field_idx = GETARG_B(inst);
            int rc = GETARG_C(inst);

            // Phase 2: inline store when layout known at compile time
            XirVReg *_sv_a = builder_vreg_for_slot(b, a);
            int sidx = _sv_a ? (int)_sv_a->struct_idx : -1;
            XrStructLayout *layout = NULL;
            if (sidx >= 0 && sidx < b->proto->struct_layout_count)
                layout = b->proto->struct_layouts[sidx];
            // Fallback: layout propagated via vreg
            if (!layout && _sv_a)
                layout = _sv_a->layout;

            if (layout && field_idx < layout->field_count) {
                XrStructFieldLayout *fl = &layout->fields[field_idx];
                uint8_t nt = fl->native_type;
                // Pick the store instruction based on native type
                int store_op = -1;
                switch (nt) {
                    case XR_NATIVE_I64: case XR_NATIVE_U64: case XR_NATIVE_F64:
                    case XR_NATIVE_STRING:
                        store_op = XIR_STORE; break;
                    case XR_NATIVE_BOOL: case XR_NATIVE_I8: case XR_NATIVE_U8:
                        store_op = XIR_STORE8; break;
                    case XR_NATIVE_I16: case XR_NATIVE_U16:
                        store_op = XIR_STORE16; break;
                    case XR_NATIVE_I32: case XR_NATIVE_U32:
                        store_op = XIR_STORE32; break;
                    case XR_NATIVE_F32:
                        store_op = XIR_STORE_F32; break;
                    default: break; // XR_NATIVE_STRUCT etc → fallback
                }
                if (store_op >= 0) {
                    XirRef struct_ref = builder_get_slot(b, blk, a);
                    XirRef val = builder_get_slot(b, blk, rc);
                    int32_t byte_off = 8 + fl->offset;
                    XirRef off_c = xir_const_i64(b->func, (int64_t)byte_off);
                    XirRef addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR,
                                           struct_ref, off_c);
                    xir_emit_void(b->func, blk, store_op, addr, val);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    b->ops_translated++;
                    return true;
                }
            }

            // Fallback: CALL_C runtime helper
            XirRef ss_fa[2];
            ss_fa[0] = builder_get_slot(b, blk, a);
            ss_fa[1] = builder_get_slot(b, blk, rc);
            uint8_t val_tag = builder_slot_xr_tag(b, rc);
            int64_t packed = (int64_t)field_idx | ((int64_t)val_tag << 8);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_struct_set);
            XirRef extra = xir_const_i64(b->func, packed);
            XirRef ss_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, extra);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, ss_result, ss_fa, 2);
            b->ops_translated++;
            return true;
        }

        case OP_STRUCT_COPY: {
            // R[A] = deep copy of struct R[B], placed at struct_area slot C
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int slot_offset = GETARG_C(inst);
            XirRef src = builder_get_slot(b, blk, rb);
            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_struct_copy);
            XirRef extra = xir_const_i64(b->func, (int64_t)slot_offset);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR,
                                     fn_ref, extra);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &src, 1);
            builder_set_slot(b, a, result);
            b->slot_rep[a] = XR_REP_PTR;
            // Phase 2: propagate source struct's layout to the copy
            { XirVReg *_sv_rb2 = builder_vreg_for_slot(b, rb);
              XirVReg *_sv_a2 = builder_vreg_for_slot(b, a);
              if (_sv_rb2 && _sv_a2 && _sv_rb2->struct_idx >= 0)
                  _sv_a2->struct_idx = _sv_rb2->struct_idx; }
            b->ops_translated++;
            return true;
        }

        /* === No-op === */
        case OP_NOP:
            b->ops_translated++;
            return true;

        default:
            return false;
    }
}
