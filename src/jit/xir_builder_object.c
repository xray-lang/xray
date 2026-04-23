/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_builder_object.c - XIR translation for object/property/JSON operations
 *
 * KEY CONCEPT:
 *   Handles field access, property get/set, JSON operations, map/set
 *   operations, index access, compound assignment, and type conversion
 *   opcodes in the bytecode-to-XIR translation pipeline.
 */

#include "xir_builder_internal.h"
#include "../base/xchecks.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_lookup.h"

bool xir_translate_object_ops(XirBuilder *b, XirBlock **cur_blk,
                              uint32_t pc, XrInstruction inst, OpCode op) {
    XR_DCHECK(b != NULL, "translate_object_ops: NULL builder");
    XR_DCHECK(cur_blk != NULL, "translate_object_ops: NULL cur_blk");
    XirBlock *blk = *cur_blk;
    switch (op) {
        case OP_GETFIELD: {
            // R[A] = R[B]:Instance.fields[C]
            // Instance layout: XrGCHeader(16) + klass*(8) + XrValue fields[]
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int c = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, rb);
            int32_t byte_offset = XR_INSTANCE_FIELDS_OFFSET + c * XR_XRVALUE_SIZE;
            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);

            // Use inst_types[pc] for flow-sensitive field result type
            uint8_t field_rep = XR_REP_I64;
            uint8_t field_tag = VTAG_TAGGED;
            {
                struct XrType *ft = builder_inst_xrtype(b, b->cur_pc);
                if (ft) {
                    field_rep = xr_type_rep(ft);
                    uint8_t vtag = value_tag_to_vtag(xr_type_to_xr_tag(ft));
                    if (vtag != VTAG_TAGGED) field_tag = vtag;
                }
            }
            // For TAGGED rep, codegen already loads runtime tag from XrValue
            // (see XIR_LOAD_FIELD codegen: ldrb tag when UNKNOWN+I64)
            if (field_rep == XR_REP_TAGGED) field_rep = XR_REP_I64;

            XirRef v = xir_emit(b->func, blk, XIR_LOAD_FIELD, field_rep, obj, off_ref);
            builder_tag_vreg(b, v, field_tag, 0);
            builder_set_slot(b, a, v);
            if (a < 256 && field_rep != XR_REP_TAGGED)
                b->slot_rep[a] = field_rep;
            b->ops_translated++;
            return true;
        }

        case OP_SETFIELD: {
            // R[A]:Instance.fields[B] = R[C]
            // Instance layout: XrGCHeader(16) + klass*(8) + XrValue fields[]
            int a = GETARG_A(inst);
            int field_b = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, a);
            XirRef val = builder_get_slot(b, blk, rc);
            int32_t byte_offset = XR_INSTANCE_FIELDS_OFFSET + field_b * XR_XRVALUE_SIZE;
            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
            uint8_t sf_tag = builder_ref_sf_tag(b, val, rc);
            xir_emit_raw(b->func, blk, XIR_STORE_FIELD, sf_tag, off_ref, obj, val);
            b->ops_translated++;
            return true;
        }

        /* === Json Object Operations === */
        case OP_NEWJSON: {
            // R[A] = new Json(K[B]:Shape), C=storage_mode
            // Only support normal mode (C=0) in JIT
            int a = GETARG_A(inst);
            int bx = GETARG_B(inst);
            int storage_mode = GETARG_C(inst);
            if (storage_mode != 0) {
                b->ops_skipped++;
                return true;
            }
            // Bounds check for constant pool
            if (bx >= PROTO_CONST_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            // Extract shape pointer from constant pool at compile time
            XrValue shape_val = PROTO_CONST_FAST(b->proto, bx);
            void *shape_ptr = (void*)(intptr_t)XR_TO_INT(shape_val);

            // Optimization: use _noinit version if all fields are set before any
            // GC-triggering operation (CALLSELF, CALL, CALL_C).
            // Scan forward to check if JSON_INIT/JSON_INIT_N/JSON_INIT_I cover all
            // fields before any call instruction.
            struct XrShape *shape = (struct XrShape *)shape_ptr;
            int field_count = shape->in_object_capacity;
            bool safe_noinit = false;
            if (field_count > 0 && field_count <= 32) {
                uint32_t init_mask = 0;
                uint32_t all_fields = (field_count >= 32) ? 0xFFFFFFFFu : ((1u << field_count) - 1);
                safe_noinit = true;
                for (uint32_t scan = pc + 1; scan < b->code_count; scan++) {
                    XrInstruction si = PROTO_CODE(b->proto, scan);
                    int sop = GET_OPCODE(si);
                    if (sop == OP_JSON_INIT || sop == OP_JSON_INIT_N || sop == OP_JSON_INIT_I) {
                        int sa = GETARG_A(si);
                        int sc = GETARG_C(si);
                        if (sa == a && sc < field_count)
                            init_mask |= (1u << sc);
                        if (init_mask == all_fields) break;
                    } else if (sop == OP_CALLSELF || sop == OP_CALL || sop == OP_RETURN1 ||
                               sop == OP_RETURN0 || sop == OP_JMP) {
                        // Hit a potential GC point or control flow before all fields set
                        if (init_mask != all_fields)
                            safe_noinit = false;
                        break;
                    }
                }
                if (init_mask != all_fields) safe_noinit = false;
            }

            // Inline allocation via XIR_ALLOC (bump-pointer fast path in codegen)
            // gc_extra encodes shape_id for GC header
            uint16_t gc_extra = (uint16_t)(shape->id << XR_SHAPE_ID_SHIFT);
            int64_t packed = ((int64_t)gc_extra << 8) | (int64_t)XR_TJSON;
            // XrJson layout: XrGCHeader(16) + overflow*(8) + fields[capacity]
            int32_t alloc_size = XGC_HEADER_SIZE + 8 + field_count * XR_XRVALUE_SIZE;
            XirRef pack_ref = xir_const_i64(b->func, packed);
            XirRef size_ref = xir_const_i64(b->func, (int64_t)alloc_size);
            XirRef v = xir_emit(b->func, blk, XIR_ALLOC, XR_REP_PTR,
                                pack_ref, size_ref);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_tag_vreg(b, v, VTAG_PTR, 0);

            // Zero-initialize overflow pointer and fields
            {
                XirRef zero = xir_const_i64(b->func, 0);
                XirRef zero_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                 XR_REP_I64, zero);
                // Always zero overflow pointer at offset 16 (right after GC header)
                XirRef ov_off = xir_const_i64(b->func, (int64_t)XGC_HEADER_SIZE);
                xir_emit_raw(b->func, blk, XIR_STORE_FIELD,
                             0 /* null/ptr */, ov_off, v, zero_val);
                // Zero-initialize fields (unless safe_noinit — caller sets all)
                if (!safe_noinit) {
                    for (int fi = 0; fi < field_count && fi < 32; fi++) {
                        int32_t foff = XR_JSON_FIELDS_OFFSET + fi * XR_XRVALUE_SIZE;
                        XirRef off_ref = xir_const_i64(b->func, (int64_t)foff);
                        xir_emit_raw(b->func, blk, XIR_STORE_FIELD,
                                     0 /* XR_TAG_NULL */, off_ref, v, zero_val);
                    }
                }
            }

            builder_set_slot(b, a, v);
            // Record shape for GETPROP optimization (stored in vreg)
            { XirVReg *_sv = builder_vreg_for_slot(b, a); if (_sv) _sv->shape_hint = shape; }
            // Mark as freshly allocated (stored in vreg)
            { XirVReg *_fv = builder_vreg_for_slot(b, a); if (_fv) _fv->is_fresh_alloc = true; }
            b->ops_translated++;
            return true;
        }

        case OP_GETPROP: {
            // R[A] = R[B].Symbol[C]  — dynamic property access
            // C is a local symbol index into proto->symbols[].
            // Fast path: if we know the shape of R[B] at compile time (from
            // OP_NEWJSON tracking), look up field index from shape directly and
            // emit LOAD_FIELD — identical to OP_JSON_GET but without the known
            // static field index in the opcode.
            // Slow path: CALL_C(xr_jit_getprop) for unknown shapes / non-Json.
            int a  = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int c  = GETARG_C(inst);

            // Bounds check to prevent SIGSEGV
            if (c >= PROTO_SYMBOL_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            // Resolve compile-time global symbol id
            SymbolId sym = PROTO_SYMBOL(b->proto, c);

            { XirVReg *_srb = builder_vreg_for_slot(b, rb);
            struct XrShape *known_shape = _srb ? _srb->shape_hint : NULL;
            if (known_shape && known_shape->symbol_to_index &&
                (SymbolId)sym >= known_shape->min_symbol &&
                (SymbolId)sym <= known_shape->max_symbol) {
                int field_idx = known_shape->symbol_to_index[sym - known_shape->min_symbol];
                if (field_idx >= 0) {
                    XirRef obj = builder_get_slot(b, blk, rb);
                    builder_emit_shape_guard(b, blk, obj, known_shape, pc);
                    int32_t byte_offset = XR_JSON_FIELDS_OFFSET + field_idx * XR_XRVALUE_SIZE;
                    XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
                    XirRef v = xir_emit(b->func, blk, XIR_LOAD_FIELD, XR_REP_I64, obj, off_ref);
                    builder_tag_vreg(b, v, VTAG_TAGGED, 0);
                    builder_set_slot(b, a, v);
                    { XirVReg *_sa = builder_vreg_for_slot(b, a); if (_sa) _sa->shape_hint = NULL; }
                    b->ops_translated++;
                    return true;
                }
            }

            // IC-guided fast path: if VM collected a monomorphic Json Shape IC
            // for this bytecode offset, emit GUARD_SHAPE + LOAD_FIELD.
            if (b->proto->ic_fields) {
                XrICField *ic = xr_ic_field_table_get(b->proto->ic_fields, (int)pc);
                if (ic && ic->json_shape_id != 0 && ic->cached_symbol == (int)sym) {
                    struct XrShape *ic_shape = xr_shape_get_by_id(b->isolate, ic->json_shape_id);
                    if (ic_shape && ic_shape->symbol_to_index &&
                        (SymbolId)sym >= ic_shape->min_symbol &&
                        (SymbolId)sym <= ic_shape->max_symbol) {
                        int field_idx = ic_shape->symbol_to_index[sym - ic_shape->min_symbol];
                        if (field_idx >= 0) {
                            XirRef obj = builder_get_slot(b, blk, rb);
                            // Emit shape guard: deopt if runtime shape != IC shape
                            XirRef shape_id_ref = xir_const_i64(b->func,
                                (int64_t)(uint32_t)ic_shape->id);
                            xir_emit(b->func, blk, XIR_GUARD_SHAPE, XR_REP_VOID,
                                     obj, shape_id_ref);
                            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                            int did = builder_add_deopt_info(b, pc);
                            if (did >= 0) {
                                blk->ins[blk->nins - 1].dst =
                                    xir_const_i64(b->func, (int64_t)did);
                            }
                            // Direct field access using IC-cached offset
                            int32_t byte_offset = XR_JSON_FIELDS_OFFSET +
                                                  field_idx * XR_XRVALUE_SIZE;
                            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
                            XirRef v = xir_emit(b->func, blk, XIR_LOAD_FIELD,
                                                XR_REP_I64, obj, off_ref);
                            builder_tag_from_slot(b, v, a);
                            builder_set_slot(b, a, v);
                            { XirVReg *_sa2 = builder_vreg_for_slot(b, a); if (_sa2) _sa2->shape_hint = NULL; }
                            b->ops_translated++;
                            return true;
                        }
                    }
                }
            }
            } // known_shape block

            // AOT class field resolution: if receiver is a known class
            // instance, resolve symbol → field offset for direct access.
            if (b->aot_mode && b->isolate && rb < 256) {
                XrType *recv_type = builder_find_reg_type(b, rb);
                // Fallback: if receiver type unknown but 'this' (param 0) has
                // a known class type, try that class's fields for the symbol.
                if (!recv_type || recv_type->kind != XR_KIND_INSTANCE) {
                    XrType *this_type = builder_find_reg_type(b, 0);
                    if (this_type && this_type->kind == XR_KIND_INSTANCE)
                        recv_type = this_type;
                }
                const char *cname = recv_type ? xr_type_get_class_name(recv_type) : NULL;
                if (cname && recv_type->kind == XR_KIND_INSTANCE) {
                    XrClass *klass = xr_class_lookup_by_name(b->isolate, cname);
                    if (klass) {
                        int fi = xr_class_lookup_field(klass, (int)sym);
                        if (fi >= 0 && fi < klass->field_count) {
                            // Use same offset formula as OP_GETFIELD for
                            // consistency with AOT codegen adj_offset logic.
                            int32_t byte_off = XR_INSTANCE_FIELDS_OFFSET
                                             + fi * XR_XRVALUE_SIZE;
                            XirRef obj2 = builder_get_slot(b, blk, rb);
                            XirRef off2 = xir_const_i64(b->func, (int64_t)byte_off);
                            // Determine load rep from field type
                            uint8_t frep = XR_REP_I64;
                            const char *ftype = klass->fields[fi].type_name;
                            if (ftype && (strcmp(ftype, "float") == 0 ||
                                          strcmp(ftype, "double") == 0 ||
                                          strcmp(ftype, "f64") == 0))
                                frep = XR_REP_F64;
                            XirRef fv = xir_emit(b->func, blk, XIR_LOAD_FIELD,
                                                 frep, obj2, off2);
                            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                            if (frep == XR_REP_F64)
                                builder_tag_vreg(b, fv, VTAG_F64, 0);
                            else
                                builder_tag_vreg(b, fv, VTAG_TAGGED, 0);
                            builder_set_slot(b, a, fv);
                            b->ops_translated++;
                            return true;
                        }
                    }
                }
            }

            // Slow path: call runtime helper.
            XirRef obj = builder_get_slot(b, blk, rb);
            XirRef fn_ref   = xir_const_ptr(b->func, (void *)xr_jit_getprop);
            XirRef sym_ref  = xir_const_i64(b->func, (int64_t)(uint32_t)sym);
            XirRef sym_val  = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, sym_ref);
            // AOT: getprop returns XrtValue (struct), not raw i64
            uint8_t gp_rep = b->aot_mode ? XR_REP_TAGGED : XR_REP_I64;
            XirRef v = xir_emit(b->func, blk, XIR_CALL_C_LEAF, gp_rep, fn_ref, sym_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, v, &obj, 1);
            // xr_jit_getprop returns polymorphic values (int for .length,
            // ptr for Json fields). Do NOT use builder_tag_from_slot here.
            builder_tag_vreg(b, v, VTAG_TAGGED, 0);
            builder_set_slot(b, a, v);
            { XirVReg *_sa3 = builder_vreg_for_slot(b, a); if (_sa3) _sa3->shape_hint = NULL; }
            b->ops_translated++;
            return true;
        }

        case OP_SETPROP: {
            // R[A].Symbol[B] = R[C] — dynamic property write
            int a  = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);

            // Bounds check to prevent SIGSEGV
            if (rb >= PROTO_SYMBOL_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            SymbolId sym = PROTO_SYMBOL(b->proto, rb);

            // Fast path: known shape → direct STORE_FIELD
            { XirVReg *_sa_sp2 = builder_vreg_for_slot(b, a);
            struct XrShape *known_shape = _sa_sp2 ? _sa_sp2->shape_hint : NULL;
            if (known_shape && known_shape->symbol_to_index &&
                (SymbolId)sym >= known_shape->min_symbol &&
                (SymbolId)sym <= known_shape->max_symbol) {
                int field_idx = known_shape->symbol_to_index[sym - known_shape->min_symbol];
                if (field_idx >= 0) {
                    XirRef obj = builder_get_slot(b, blk, a);
                    builder_emit_shape_guard(b, blk, obj, known_shape, pc);
                    XirRef val = builder_get_slot(b, blk, rc);
                    int32_t byte_offset = XR_JSON_FIELDS_OFFSET + field_idx * XR_XRVALUE_SIZE;
                    XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
                    uint8_t sf_tag = builder_ref_sf_tag(b, val, rc);
                    xir_emit_raw(b->func, blk, XIR_STORE_FIELD, sf_tag, off_ref, obj, val);
                    b->ops_translated++;
                    return true;
                }
            }

            // IC-guided fast path: monomorphic Json Shape IC → GUARD_SHAPE + STORE_FIELD
            if (b->proto->ic_fields) {
                XrICField *ic = xr_ic_field_table_get(b->proto->ic_fields, (int)pc);
                if (ic && ic->json_shape_id != 0 && ic->cached_symbol == (int)sym) {
                    struct XrShape *ic_shape = xr_shape_get_by_id(b->isolate, ic->json_shape_id);
                    if (ic_shape && ic_shape->symbol_to_index &&
                        (SymbolId)sym >= ic_shape->min_symbol &&
                        (SymbolId)sym <= ic_shape->max_symbol) {
                        int field_idx = ic_shape->symbol_to_index[sym - ic_shape->min_symbol];
                        if (field_idx >= 0) {
                            XirRef obj = builder_get_slot(b, blk, a);
                            // Emit shape guard
                            XirRef shape_id_ref = xir_const_i64(b->func,
                                (int64_t)(uint32_t)ic_shape->id);
                            xir_emit(b->func, blk, XIR_GUARD_SHAPE, XR_REP_VOID,
                                     obj, shape_id_ref);
                            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                            int did = builder_add_deopt_info(b, pc);
                            if (did >= 0) {
                                blk->ins[blk->nins - 1].dst =
                                    xir_const_i64(b->func, (int64_t)did);
                            }
                            // Direct field write using IC-cached offset
                            XirRef val = builder_get_slot(b, blk, rc);
                            int32_t byte_offset = XR_JSON_FIELDS_OFFSET +
                                                  field_idx * XR_XRVALUE_SIZE;
                            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
                            uint8_t sf_tag = builder_ref_sf_tag(b, val, rc);
                            xir_emit_raw(b->func, blk, XIR_STORE_FIELD, sf_tag,
                                         off_ref, obj, val);
                            b->ops_translated++;
                            return true;
                        }
                    }
                }
            }

            // Slow path: call xr_jit_setprop runtime helper
            XirRef sp_args[2];
            sp_args[0] = builder_get_slot(b, blk, a);
            sp_args[1] = builder_get_slot(b, blk, rc);
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            uint8_t val_bc_slot_sp = (rc >= 0 && rc < 256) ? (uint8_t)rc : 0xFF;
            int64_t encoded = ((int64_t)val_bc_slot_sp << 40) | ((int64_t)(uint32_t)sym << 8) | val_tag;
            XirRef fn_ref   = xir_const_ptr(b->func, (void *)xr_jit_setprop);
            XirRef enc_ref  = xir_const_i64(b->func, encoded);
            XirRef enc_val  = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            XirRef sp_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, sp_result, sp_args, 2);
            b->ops_translated++;
            return true;
            } // _sa_sp2 block (OP_SETPROP)
        }

        case OP_JSON_SETK: {
            // R[A].symbol[B] = R[C] — Json field write by symbol
            // Same as SETPROP but compiler knows it's Json
            int a  = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);

            // Bounds check to prevent SIGSEGV
            if (rb >= PROTO_SYMBOL_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            SymbolId sym = PROTO_SYMBOL(b->proto, rb);

            { XirVReg *_sa_sp = builder_vreg_for_slot(b, a);
            struct XrShape *known_shape = _sa_sp ? _sa_sp->shape_hint : NULL;
            if (known_shape && known_shape->symbol_to_index &&
                (SymbolId)sym >= known_shape->min_symbol &&
                (SymbolId)sym <= known_shape->max_symbol) {
                int field_idx = known_shape->symbol_to_index[sym - known_shape->min_symbol];
                if (field_idx >= 0) {
                    XirRef obj = builder_get_slot(b, blk, a);
                    builder_emit_shape_guard(b, blk, obj, known_shape, pc);
                    XirRef val_ref = builder_get_slot(b, blk, rc);
                    int32_t byte_offset = XR_JSON_FIELDS_OFFSET + field_idx * XR_XRVALUE_SIZE;
                    XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
                    uint8_t sf_tag = builder_ref_sf_tag(b, val_ref, rc);
                    xir_emit_raw(b->func, blk, XIR_STORE_FIELD, sf_tag, off_ref, obj, val_ref);
                    b->ops_translated++;
                    return true;
                }
            }

            // IC-guided fast path for JSON_SETK
            if (b->proto->ic_fields) {
                XrICField *ic = xr_ic_field_table_get(b->proto->ic_fields, (int)pc);
                if (ic && ic->json_shape_id != 0 && ic->cached_symbol == (int)sym) {
                    struct XrShape *ic_shape = xr_shape_get_by_id(b->isolate, ic->json_shape_id);
                    if (ic_shape && ic_shape->symbol_to_index &&
                        (SymbolId)sym >= ic_shape->min_symbol &&
                        (SymbolId)sym <= ic_shape->max_symbol) {
                        int field_idx = ic_shape->symbol_to_index[sym - ic_shape->min_symbol];
                        if (field_idx >= 0) {
                            XirRef obj = builder_get_slot(b, blk, a);
                            XirRef shape_id_ref = xir_const_i64(b->func,
                                (int64_t)(uint32_t)ic_shape->id);
                            xir_emit(b->func, blk, XIR_GUARD_SHAPE, XR_REP_VOID,
                                     obj, shape_id_ref);
                            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                            int did = builder_add_deopt_info(b, pc);
                            if (did >= 0) {
                                blk->ins[blk->nins - 1].dst =
                                    xir_const_i64(b->func, (int64_t)did);
                            }
                            XirRef val = builder_get_slot(b, blk, rc);
                            int32_t byte_offset = XR_JSON_FIELDS_OFFSET +
                                                  field_idx * XR_XRVALUE_SIZE;
                            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
                            uint8_t sf_tag = builder_ref_sf_tag(b, val, rc);
                            xir_emit_raw(b->func, blk, XIR_STORE_FIELD, sf_tag,
                                         off_ref, obj, val);
                            b->ops_translated++;
                            return true;
                        }
                    }
                }
            }

            } // known_shape block (SETPROP)
            // Slow path: same as SETPROP
            XirRef jsk_args[2];
            jsk_args[0] = builder_get_slot(b, blk, a);
            jsk_args[1] = builder_get_slot(b, blk, rc);
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            uint8_t val_bc_slot_sp = (rc >= 0 && rc < 256) ? (uint8_t)rc : 0xFF;
            int64_t encoded = ((int64_t)val_bc_slot_sp << 40) | ((int64_t)(uint32_t)sym << 8) | val_tag;
            XirRef fn_ref   = xir_const_ptr(b->func, (void *)xr_jit_setprop);
            XirRef enc_ref  = xir_const_i64(b->func, encoded);
            XirRef enc_val  = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
            XirRef jsk_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, jsk_result, jsk_args, 2);
            b->ops_translated++;
            return true;
        }

        case OP_JSON_GETK: {
            // R[A] = R[B].symbol[C] — Json field read by symbol (compiler knows it's Json)
            // Same logic as OP_GETPROP: fast path with known shape, slow path via CALL_C.
            int a  = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int c  = GETARG_C(inst);

            // Bounds check to prevent SIGSEGV
            if (c >= PROTO_SYMBOL_COUNT(b->proto)) {
                b->ops_skipped++;
                return true;
            }

            SymbolId sym = PROTO_SYMBOL(b->proto, c);

            { XirVReg *_srb2 = builder_vreg_for_slot(b, rb);
            struct XrShape *known_shape = _srb2 ? _srb2->shape_hint : NULL;
            if (known_shape && known_shape->symbol_to_index &&
                (SymbolId)sym >= known_shape->min_symbol &&
                (SymbolId)sym <= known_shape->max_symbol) {
                int field_idx = known_shape->symbol_to_index[sym - known_shape->min_symbol];
                if (field_idx >= 0) {
                    XirRef obj = builder_get_slot(b, blk, rb);
                    builder_emit_shape_guard(b, blk, obj, known_shape, pc);
                    int32_t byte_offset = XR_JSON_FIELDS_OFFSET + field_idx * XR_XRVALUE_SIZE;
                    XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
                    XirRef v = xir_emit(b->func, blk, XIR_LOAD_FIELD, XR_REP_I64, obj, off_ref);
                    builder_tag_from_slot(b, v, a);
                    builder_set_slot(b, a, v);
                    { XirVReg *_sa4 = builder_vreg_for_slot(b, a); if (_sa4) _sa4->shape_hint = NULL; }
                    b->ops_translated++;
                    return true;
                }
            }

            // IC-guided fast path for JSON_GETK
            if (b->proto->ic_fields) {
                XrICField *ic = xr_ic_field_table_get(b->proto->ic_fields, (int)pc);
                if (ic && ic->json_shape_id != 0 && ic->cached_symbol == (int)sym) {
                    struct XrShape *ic_shape = xr_shape_get_by_id(b->isolate, ic->json_shape_id);
                    if (ic_shape && ic_shape->symbol_to_index &&
                        (SymbolId)sym >= ic_shape->min_symbol &&
                        (SymbolId)sym <= ic_shape->max_symbol) {
                        int field_idx = ic_shape->symbol_to_index[sym - ic_shape->min_symbol];
                        if (field_idx >= 0) {
                            XirRef obj = builder_get_slot(b, blk, rb);
                            XirRef shape_id_ref = xir_const_i64(b->func,
                                (int64_t)(uint32_t)ic_shape->id);
                            xir_emit(b->func, blk, XIR_GUARD_SHAPE, XR_REP_VOID,
                                     obj, shape_id_ref);
                            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                            int did = builder_add_deopt_info(b, pc);
                            if (did >= 0) {
                                blk->ins[blk->nins - 1].dst =
                                    xir_const_i64(b->func, (int64_t)did);
                            }
                            int32_t byte_offset = XR_JSON_FIELDS_OFFSET +
                                                  field_idx * XR_XRVALUE_SIZE;
                            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
                            XirRef v = xir_emit(b->func, blk, XIR_LOAD_FIELD,
                                                XR_REP_I64, obj, off_ref);
                            builder_tag_from_slot(b, v, a);
                            builder_set_slot(b, a, v);
                            { XirVReg *_sa5 = builder_vreg_for_slot(b, a); if (_sa5) _sa5->shape_hint = NULL; }
                            b->ops_translated++;
                            return true;
                        }
                    }
                }
            }
            } // known_shape block (JSON_GETK)

            // Slow path: call xr_jit_getprop runtime helper
            XirRef obj = builder_get_slot(b, blk, rb);
            XirRef fn_ref   = xir_const_ptr(b->func, (void *)xr_jit_getprop);
            XirRef sym_ref  = xir_const_i64(b->func, (int64_t)(uint32_t)sym);
            XirRef sym_val  = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, sym_ref);
            XirRef v = xir_emit(b->func, blk, XIR_CALL_C_LEAF, XR_REP_I64, fn_ref, sym_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, v, &obj, 1);
            builder_tag_from_slot(b, v, a);
            builder_set_slot(b, a, v);
            { XirVReg *_sa6 = builder_vreg_for_slot(b, a); if (_sa6) _sa6->shape_hint = NULL; }
            b->ops_translated++;
            return true;
        }

        case OP_JSON_GET: {
            // R[A] = R[B].fields[C] (Json field read by index)
            // Json layout: XrGCHeader(16) + overflow*(8) + XrValue fields[]
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int c = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, rb);
            int32_t byte_offset = XR_JSON_FIELDS_OFFSET + c * XR_XRVALUE_SIZE;
            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);

            // Use inst_types[pc] for flow-sensitive JSON field rep.
            // PTR rep is critical: it makes GC safepoint bitmap track this
            // register, preventing GC from collecting reachable objects
            // when the only live reference is in a JIT register.
            uint8_t field_rep = XR_REP_I64;
            {
                struct XrType *ft = builder_inst_xrtype(b, b->cur_pc);
                uint8_t r = ft ? xr_type_rep(ft) : XR_REP_I64;
                if (r == XR_REP_PTR) field_rep = XR_REP_PTR;
                else if (r == XR_REP_F64) field_rep = XR_REP_F64;
                // TAGGED → I64: codegen loads runtime tag from XrValue
            }

            XirRef v = xir_emit(b->func, blk, XIR_LOAD_FIELD, field_rep, obj, off_ref);
            builder_tag_from_slot(b, v, a);
            builder_set_slot(b, a, v);
            b->ops_translated++;
            return true;
        }

        case OP_JSON_SET: {
            // R[A].fields[B] = R[C] (Json field write by index)
            // Json layout: XrGCHeader(16) + XrValue fields[]
            int a = GETARG_A(inst);
            int field_b = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, a);
            XirRef val = builder_get_slot(b, blk, rc);
            int32_t byte_offset = XR_JSON_FIELDS_OFFSET + field_b * XR_XRVALUE_SIZE;
            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
            uint8_t sf_tag = builder_ref_sf_tag(b, val, rc);
            xir_emit_raw(b->func, blk, XIR_STORE_FIELD, sf_tag, off_ref, obj, val);
            b->ops_translated++;
            return true;
        }

        case OP_JSON_INIT: {
            // R[A].fields[B] = R[C] (same as JSON_SET, used during init)
            int a = GETARG_A(inst);
            int field_b = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, a);
            XirRef val = builder_get_slot(b, blk, rc);
            int32_t byte_offset = XR_JSON_FIELDS_OFFSET + field_b * XR_XRVALUE_SIZE;
            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
            uint8_t sf_tag = builder_ref_sf_tag(b, val, rc);
            xir_emit_raw(b->func, blk, XIR_STORE_FIELD, sf_tag, off_ref, obj, val);
            // Skip write barrier if container was just allocated (still WHITE)
            { XirVReg *_fv2 = builder_vreg_for_slot(b, a);
            if (_fv2 && _fv2->is_fresh_alloc)
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_NO_BARRIER;
            } // is_fresh_alloc block
            b->ops_translated++;
            return true;
        }

        case OP_JSON_INIT_I: {
            // R[A].fields[B] = sC (immediate integer)
            int a = GETARG_A(inst);
            int field_b = GETARG_B(inst);
            int sc = GETARG_sC(inst);
            XirRef obj = builder_get_slot(b, blk, a);
            XirRef c = xir_const_i64(b->func, (int64_t)sc);
            XirRef val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c);
            int32_t byte_offset = XR_JSON_FIELDS_OFFSET + field_b * XR_XRVALUE_SIZE;
            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
            xir_emit_raw(b->func, blk, XIR_STORE_FIELD, XR_TAG_I64, off_ref, obj, val);
            b->ops_translated++;
            return true;
        }

        case OP_JSON_INIT_N: {
            // R[A].fields[B] = null (tag=0, payload=0)
            int a = GETARG_A(inst);
            int field_b = GETARG_B(inst);
            XirRef obj = builder_get_slot(b, blk, a);
            XirRef c = xir_const_i64(b->func, 0);
            XirRef val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c);
            int32_t byte_offset = XR_JSON_FIELDS_OFFSET + field_b * XR_XRVALUE_SIZE;
            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
            xir_emit_raw(b->func, blk, XIR_STORE_FIELD, 0 /* XR_TAG_NULL */, off_ref, obj, val);
            b->ops_translated++;
            return true;
        }

        /* === Typed Field Access (same layout as Json: GCHeader(16) + XrValue fields[]) === */
        case OP_TFIELD_GET: {
            // R[A] = R[B]:Json.fields[C] (typed field read)
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int c = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, rb);
            int32_t byte_offset = XR_JSON_FIELDS_OFFSET + c * XR_XRVALUE_SIZE;
            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
            // Use F64 type if the compiler recorded this field as float
            uint8_t ftype = (c < 64 && (b->proto->tfield_float_bitmap >> c) & 1)
                            ? XR_REP_F64 : XR_REP_I64;
            XirRef v = xir_emit(b->func, blk, XIR_LOAD_FIELD, ftype, obj, off_ref);
            builder_set_slot(b, a, v);
            if (a < 256) b->slot_rep[a] = ftype;
            b->ops_translated++;
            return true;
        }

        case OP_TFIELD_SET: {
            // R[A]:Json.fields[B] = R[C] (typed field write)
            int a = GETARG_A(inst);
            int field_b = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, a);
            XirRef val = builder_get_slot(b, blk, rc);
            int32_t byte_offset = XR_JSON_FIELDS_OFFSET + field_b * XR_XRVALUE_SIZE;
            XirRef off_ref = xir_const_i64(b->func, (int64_t)byte_offset);
            uint8_t sf_tag = builder_ref_sf_tag(b, val, rc);
            xir_emit_raw(b->func, blk, XIR_STORE_FIELD, sf_tag, off_ref, obj, val);
            b->ops_translated++;
            return true;
        }

        /* === Generic Index Operations === */
        case OP_INDEX_GET: {
            // R[A] = R[B][R[C]] — runtime dispatch via C helper
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, rb);
            XirRef key = builder_get_slot(b, blk, rc);

            // Fast path: struct inline fixed array ([N]T)
            // When rb is known to hold a fixed-array base pointer (from STRUCT_GET),
            // emit inline element load: base + index * elem_size → typed load
            { XirVReg *_av_rb = builder_vreg_for_slot(b, rb);
            if (_av_rb && _av_rb->array_etype != 0xFF) {
                uint8_t etype = _av_rb->array_etype;
                uint8_t es = xr_native_type_size(etype);
                int load_op = -1;
                uint8_t rep = XR_REP_I64;
                switch (etype) {
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
                    default: break;
                }
                if (load_op >= 0) {
                    // addr = base + key * elem_size
                    XirRef es_c = xir_const_i64(b->func, (int64_t)es);
                    XirRef es_v = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                 XR_REP_I64, es_c);
                    XirRef byte_off = xir_emit(b->func, blk, XIR_MUL, XR_REP_I64,
                                               key, es_v);
                    XirRef addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR,
                                           obj, byte_off);
                    XirRef v = xir_emit(b->func, blk, load_op, rep, addr, XIR_NONE);
                    if (rep == XR_REP_I64)
                        builder_tag_vreg(b, v, VTAG_I64, 0);
                    else if (rep == XR_REP_F64)
                        builder_tag_vreg(b, v, VTAG_F64, 0);
                    builder_set_slot(b, a, v);
                    if (a < 256) b->slot_rep[a] = rep;
                    { XirVReg *_av_a = builder_vreg_for_slot(b, a);
                      if (_av_a) { _av_a->array_etype = 0xFF; _av_a->array_ecount = 0; } }
                    b->ops_translated++;
                    return true;
                }
            }
            } // _av_rb block (INDEX_GET)

            // Generic path: CALL_C runtime helper
            XirRef ig_args[2];
            ig_args[0] = obj;
            ig_args[1] = key;

            // Encode tags: (obj_tag << 8) | key_tag
            // Use vreg vtag/rep (flow-sensitive) to avoid register-reuse
            // type confusion (e.g. int index tagged as struct_ref).
            // When vtag is TAGGED, fall back to rep-derived tag to prevent
            // integer 0 from being misinterpreted as NULL via UNKNOWN path.
            uint8_t obj_vtag = ref_vtag(b->func, obj);
            if (obj_vtag == VTAG_TAGGED) {
                uint8_t obj_rep = ref_xir_type(b->func, obj);
                if (obj_rep == XR_REP_PTR) obj_vtag = VTAG_PTR;
                else if (obj_rep == XR_REP_I64) obj_vtag = VTAG_I64;
                else if (obj_rep == XR_REP_F64) obj_vtag = VTAG_F64;
            }
            uint8_t key_vtag = ref_vtag(b->func, key);
            if (key_vtag == VTAG_TAGGED) {
                uint8_t key_rep = ref_xir_type(b->func, key);
                if (key_rep == XR_REP_I64) key_vtag = VTAG_I64;
                else if (key_rep == XR_REP_F64) key_vtag = VTAG_F64;
                else if (key_rep == XR_REP_PTR) key_vtag = VTAG_PTR;
            }
            uint8_t key_tag = vtag_to_value_tag(key_vtag);
            int64_t encoded = ((int64_t)vtag_to_value_tag(obj_vtag) << 8) | (int64_t)key_tag;

            // Infer result type from container element type.
            // When obj tag is UNKNOWN (e.g., CTX_LOAD or CALL_DIRECT result), skip all
            // flow-insensitive inference: slot_xrtype and dst static types may
            // reflect a different register reuse, causing wrong result rep.
            bool obj_unreliable = (obj_vtag == VTAG_TAGGED);
            uint8_t res_type = XR_REP_PTR;
            bool elem_type_known = false; // true only when container has a concrete element type
            if (!obj_unreliable) {
                if (rb < 256 && builder_slot_xrtype(b, rb)) {
                    uint8_t elem_gc = xr_type_element_gc_tag(builder_slot_xrtype(b, rb));
                    uint8_t elem_xir = xr_slot_to_rep(elem_gc);
                    if (elem_xir != XR_REP_TAGGED) {
                        res_type = elem_xir;
                        elem_type_known = true;
                    } else {
                        // Array<any> or unknown element type: result is fully
                        // dynamic.  Use TAGGED so downstream uses return_tag.
                        res_type = XR_REP_TAGGED;
                    }
                }
                /* Do NOT override a container-derived PTR result type with the
                 * destination slot's declared type.  The slot annotation may be
                 * i64 (type inference gap) while the actual element is a heap
                 * object (e.g. Array<Array<int>> element is Array, not int).
                 * Downgrading PTR→I64 here causes jit_value_from_tag to treat
                 * the pointer as an integer, breaking downstream INDEX_GET. */
            } else {
                // Unknown container: result could be any type.
                // Use TAGGED so downstream sees full XrValue semantics.
                res_type = XR_REP_TAGGED;
            }

            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_index_get);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, res_type,
                                     fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, ig_args, 2);
            if (obj_unreliable || res_type == XR_REP_TAGGED) {
                // Runtime helper sets return_tag, but a subsequent function call
                // before RET may overwrite it (e.g. predicate after INDEX_GET).
                // Use UNKNOWN so RET writes UNKNOWN and xir_jit_call uses
                // return_type_info/heuristic for safe reconstruction.
                builder_tag_vreg(b, result, VTAG_TAGGED, 0);
            } else {
                builder_tag_from_slot(b, result, a);
            }
            builder_set_slot(b, a, result);
            b->ops_translated++;
            return true;
        }

        case OP_INDEX_SET: {
            // R[A][R[B]] = R[C] — runtime dispatch via C helper
            int a = GETARG_A(inst);
            int rb = GETARG_B(inst);
            int rc = GETARG_C(inst);
            XirRef obj = builder_get_slot(b, blk, a);
            XirRef key = builder_get_slot(b, blk, rb);
            XirRef val = builder_get_slot(b, blk, rc);

            // Fast path: struct inline fixed array ([N]T)
            { XirVReg *_av_a2 = builder_vreg_for_slot(b, a);
            if (_av_a2 && _av_a2->array_etype != 0xFF) {
                uint8_t etype = _av_a2->array_etype;
                uint8_t es = xr_native_type_size(etype);
                int store_op = -1;
                switch (etype) {
                    case XR_NATIVE_I64: case XR_NATIVE_U64: case XR_NATIVE_F64:
                        store_op = XIR_STORE; break;
                    case XR_NATIVE_BOOL: case XR_NATIVE_I8: case XR_NATIVE_U8:
                        store_op = XIR_STORE8; break;
                    case XR_NATIVE_I16: case XR_NATIVE_U16:
                        store_op = XIR_STORE16; break;
                    case XR_NATIVE_I32: case XR_NATIVE_U32:
                        store_op = XIR_STORE32; break;
                    case XR_NATIVE_F32:
                        store_op = XIR_STORE_F32; break;
                    default: break;
                }
                if (store_op >= 0) {
                    XirRef es_c = xir_const_i64(b->func, (int64_t)es);
                    XirRef es_v = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                                 XR_REP_I64, es_c);
                    XirRef byte_off = xir_emit(b->func, blk, XIR_MUL, XR_REP_I64,
                                               key, es_v);
                    XirRef addr = xir_emit(b->func, blk, XIR_ADD, XR_REP_PTR,
                                           obj, byte_off);
                    xir_emit_void(b->func, blk, store_op, addr, val);
                    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                    b->ops_translated++;
                    return true;
                }
            }

            } // known_shape block (INDEX_SET)
            // Generic path: CALL_C runtime helper
            XirRef is_args[3];
            is_args[0] = obj;
            is_args[1] = key;
            is_args[2] = val;

            // Encode slot types: bits[31:24]=val_bc_slot, [23:16]=obj_st, [15:8]=key_st, [7:0]=val_st
            // val_bc_slot allows runtime to read slot_runtime_tags[bc_slot] when val_st is imprecise.
            uint8_t obj_tag = vtag_to_value_tag(builder_slot_xr_tag(b, a));
            uint8_t key_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rb));
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, rc));
            uint8_t val_bc_slot = (rc >= 0 && rc < 256) ? (uint8_t)rc : 0xFF;
            int64_t encoded = ((int64_t)val_bc_slot << 24) | ((int64_t)obj_tag << 16)
                            | ((int64_t)key_tag << 8) | (int64_t)val_tag;

            XirRef fn_ref = xir_const_ptr(b->func, (void *)xr_jit_index_set);
            XirRef enc_ref = xir_const_i64(b->func, encoded);
            XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, enc_ref);
            XirRef is_result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, enc_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, is_result, is_args, 3);
            b->ops_translated++;
            return true;
        }

        /* === Type Conversion === */
        case OP_TOINT: {
            // R[A] = int(R[B])
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            uint8_t type = b->slot_rep[ra_b];
            if (type == XR_REP_I64) {
                builder_set_slot(b, a, vb);  // already int, no-op
            } else if (type == XR_REP_F64) {
                XirRef r = xir_emit_unary(b->func, blk, XIR_F2I, XR_REP_I64, vb);
                builder_tag_vreg(b, r, VTAG_I64, 0);
                builder_set_slot(b, a, r);
            } else {
                // bool/ptr → int: treat raw bits as int (0 or 1 for bool, ptr addr)
                builder_set_slot(b, a, vb);
            }
            b->ops_translated++;
            return true;
        }

        case OP_TOFLOAT: {
            // R[A] = float(R[B])
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            uint8_t type = b->slot_rep[ra_b];
            if (type == XR_REP_F64) {
                builder_set_slot(b, a, vb);  // already float, no-op
            } else if (type == XR_REP_I64) {
                XirRef r = xir_emit_unary(b->func, blk, XIR_I2F, XR_REP_F64, vb);
                builder_tag_vreg(b, r, VTAG_F64, 0);
                builder_set_slot(b, a, r);
            } else {
                // bool → float: 0→0.0, 1→1.0 (via int→float)
                XirRef r = xir_emit_unary(b->func, blk, XIR_I2F, XR_REP_F64, vb);
                builder_tag_vreg(b, r, VTAG_F64, 0);
                builder_set_slot(b, a, r);
            }
            b->ops_translated++;
            return true;
        }

        case OP_TOSTRING: {
            // R[A] = string(R[B]), slot_hint C: 1=i64, 2=f64, 0=generic
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            int slot_hint = GETARG_C(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);

            XirRef fn_ref;
            XirRef arg_val;
            if (b->aot_mode) {
                // AOT: use sentinel with negative encoded value
                fn_ref = xir_const_ptr(b->func, (void *)xrt_invoke_method_sentinel);
                int64_t enc2 = -(int64_t)(slot_hint + 1);
                XirRef enc2_const = xir_const_i64(b->func, enc2);
                arg_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                         XR_REP_I64, enc2_const);
            } else {
                // JIT: use xr_jit_tostring C bridge
                // Pass precise xr_tag instead of slot_hint for exact reconstruction
                fn_ref = xir_const_ptr(b->func, (void *)xr_jit_tostring);
                uint8_t ts_tag = (slot_hint == 1) ? XR_TAG_I64 :
                                 (slot_hint == 2) ? XR_TAG_F64 :
                                 vtag_to_value_tag(builder_slot_xr_tag(b, ra_b));
                XirRef hint_const = xir_const_i64(b->func, (int64_t)ts_tag);
                arg_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                         XR_REP_I64, hint_const);
            }
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR,
                                     fn_ref, arg_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &vb, 1);
            builder_tag_vreg(b, result, VTAG_PTR, 0);
            builder_set_slot(b, a, result);
            b->slot_rep[a] = XR_REP_PTR;
            b->ops_translated++;
            return true;
        }

        case OP_NEWSTRINGBUILDER: {
            // R[A] = new StringBuilder, B=storage_mode
            int a = GETARG_A(inst);
            int storage_mode = GETARG_B(inst);
            if (storage_mode != 0) { b->ops_skipped++; return true; }

            XirRef fn_ref = xir_const_ptr(b->func,
                b->aot_mode ? (void*)xrt_strbuf_new_sentinel : (void*)xr_jit_strbuf_new);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR, fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_set_slot(b, a, result);
            b->slot_rep[a] = XR_REP_PTR;
            b->ops_translated++;
            return true;
        }

        case OP_STRBUF_APPEND: {
            // sb(R[A]).append(R[B])
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);

            XirRef sba_args[2];
            sba_args[0] = builder_get_slot(b, blk, a);
            sba_args[1] = builder_get_slot(b, blk, ra_b);

            if (b->aot_mode) {
                XirRef fn_ref = xir_const_ptr(b->func, (void*)xrt_strbuf_append_sentinel);
                XirRef zero = xir_const_i64(b->func, 0);
                XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
                XirRef sba_res = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR, fn_ref, zval);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, sba_res, sba_args, 2);
            } else {
                uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, ra_b));
                uint8_t bc_slot_hint = 0xFF;
                if (ra_b >= 0 && ra_b < 256 && !xir_ref_is_none(b->slot_tag_refs[ra_b])) {
                    val_tag = 0xFF;
                    bc_slot_hint = (uint8_t)ra_b;
                }
                XirRef fn_ref = xir_const_ptr(b->func, (void*)xr_jit_strbuf_append);
                int64_t enc_raw = ((int64_t)bc_slot_hint << 8) | val_tag;
                XirRef enc_ref = xir_const_i64(b->func, enc_raw);
                XirRef enc_val = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, enc_ref);
                XirRef sba_res2 = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, enc_val);
                blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
                builder_bind_call_args(b, sba_res2, sba_args, 2);
            }
            b->ops_translated++;
            return true;
        }

        case OP_STRBUF_FINISH: {
            // R[A] = sb(R[A]).toString()
            int a = GETARG_A(inst);

            XirRef sb_ref = builder_get_slot(b, blk, a);
            XirRef fn_ref = xir_const_ptr(b->func,
                b->aot_mode ? (void*)xrt_strbuf_finish_sentinel : (void*)xr_jit_strbuf_finish);
            XirRef zero = xir_const_i64(b->func, 0);
            XirRef zval = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, zero);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_PTR, fn_ref, zval);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, result, &sb_ref, 1);
            builder_set_slot(b, a, result);
            b->slot_rep[a] = XR_REP_PTR;
            b->ops_translated++;
            return true;
        }

        case OP_TOBOOL: {
            // R[A] = bool(R[B]) — truthy: non-zero/non-null
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            uint8_t type = b->slot_rep[ra_b];
            if (type == XR_REP_F64) {
                // f64: compare with 0.0
                XirRef c0 = xir_const_f64(b->func, 0.0);
                XirRef zero = xir_emit_unary(b->func, blk, XIR_CONST_F64, XR_REP_F64, c0);
                XirRef r = xir_emit(b->func, blk, XIR_FNE, XR_REP_I64, vb, zero);
                builder_set_slot(b, a, r);
            } else {
                // i64/bool/ptr: non-zero is truthy
                XirRef c0 = xir_const_i64(b->func, 0);
                XirRef zero = xir_emit_unary(b->func, blk, XIR_CONST_I64, XR_REP_I64, c0);
                XirRef r = xir_emit(b->func, blk, XIR_NE, XR_REP_I64, vb, zero);
                builder_set_slot(b, a, r);
            }
            b->ops_translated++;
            return true;
        }

        /* === Strict Comparison (reference equality) === */
        case OP_CMP_EQ_STRICT: {
            // R[A] = (R[B] === R[C]) — pointer/bitwise equality
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            int ra_c = GETARG_C(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef vc = builder_get_slot(b, blk, ra_c);
            XirRef r = xir_emit(b->func, blk, XIR_EQ, XR_REP_I64, vb, vc);
            builder_set_slot(b, a, r);
            b->ops_translated++;
            return true;
        }

        case OP_CMP_NE_STRICT: {
            // R[A] = (R[B] !== R[C]) — pointer/bitwise inequality
            int a = GETARG_A(inst);
            int ra_b = GETARG_B(inst);
            int ra_c = GETARG_C(inst);
            XirRef vb = builder_get_slot(b, blk, ra_b);
            XirRef vc = builder_get_slot(b, blk, ra_c);
            XirRef r = xir_emit(b->func, blk, XIR_NE, XR_REP_I64, vb, vc);
            builder_set_slot(b, a, r);
            b->ops_translated++;
            return true;
        }

        /* === Shared Variable Access === */
        case OP_GETSHARED: {
            // R[A] = shared_array[Bx + shared_offset]
            int a = GETARG_A(inst);
            int bx = GETARG_Bx(inst);
            int shared_index = bx + b->proto->shared_offset;
            XirRef fn_ref = xir_const_ptr(b->func, (void*)xr_jit_get_shared);
            XirRef idx_ref = xir_const_i64(b->func, (int64_t)shared_index);
            XirRef idx_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, idx_ref);
            XirRef result = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64,
                                     fn_ref, idx_val);
            // Must mark as side-effect to prevent CSE from merging two loads from
            // the same shared index into one — shared vars can change between loads
            // (e.g., via concurrent write or between SPAWN_CONT boundaries).
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_tag_from_slot(b, result, a);
            builder_set_slot(b, a, result);
            // Track proto from shared_protos mapping (for AOT CALL_KNOWN)
            if (b->shared_protos && shared_index >= 0 &&
                shared_index < b->nshared_protos &&
                b->shared_protos[shared_index] != NULL) {
                { XirVReg *_pv = builder_vreg_for_slot(b, a); if (_pv) _pv->callee_proto = b->shared_protos[shared_index]; }
            }
            b->ops_translated++;
            return true;
        }

        case OP_SETSHARED: {
            // shared_array[Bx + shared_offset] = R[A]
            int a = GETARG_A(inst);
            int bx = GETARG_Bx(inst);
            int shared_index = bx + b->proto->shared_offset;

            XirRef val = builder_get_slot(b, blk, a);
            XirRef fn_ref = xir_const_ptr(b->func, (void*)xr_jit_set_shared);
            // Encode: (val_bc_slot<<24) | (val_slot_type<<16) | shared_index
            uint8_t val_tag = vtag_to_value_tag(builder_slot_xr_tag(b, a));
            uint8_t val_bc_slot_ss = (a >= 0 && a < 256) ? (uint8_t)a : 0xFF;
            int64_t encoded_idx = ((int64_t)val_bc_slot_ss << 24) | ((int64_t)val_tag << 16) | (shared_index & 0xFFFF);
            XirRef idx_ref = xir_const_i64(b->func, encoded_idx);
            XirRef idx_val = xir_emit_unary(b->func, blk, XIR_CONST_I64,
                                            XR_REP_I64, idx_ref);
            XirRef ss_res = xir_emit(b->func, blk, XIR_CALL_C, XR_REP_I64, fn_ref, idx_val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            builder_bind_call_args(b, ss_res, &val, 1);
            b->ops_translated++;
            return true;
        }

        default:
            return false;
    }
}
