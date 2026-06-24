/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit_object.c - Bytecode emission for field access, containers,
 *                    closures, upvalues, shared vars, class, alloc, import
 */

#include "xi_emit_internal.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xstruct_layout.h"
#include "../runtime/object/xstring.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/class/xmethod.h"
#include "../runtime/closure/xclosure.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../module/xmodule.h"
#include "../base/xfileio.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/analyzer/xtype_ref_resolve.h"

/* Recursively propagate shared_offset to a proto and all its descendants.
 * When a parent closure emits a child via xi_emit(), the child's sub-protos
 * are created before the parent can set shared_offset on the child.  This
 * recursive walk fixes up grandchild (and deeper) protos that were emitted
 * with the wrong offset. */
static void propagate_shared_offset(XrProto *proto, int offset) {
    proto->shared_offset = offset;
    int n = PROTO_PROTO_COUNT(proto);
    for (int i = 0; i < n; i++) {
        XrProto *child = PROTO_PROTO(proto, i);
        if (child)
            propagate_shared_offset(child, offset);
    }
}

/* Forward declaration for class helpers (defined later in this file) */
static void emit_class_create_impl(EmitCtx *ctx, XiValue *v, XiClassData *cdata, XiEmitReg dst);

/* Field load: GETPROP or GETFIELD */
XR_FUNC void xi_emit_load_field(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg obj = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    const char *prop = (const char *) v->aux;
    if (prop) {
        int sym = add_symbol(ctx, prop);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t sym_arg = 0;
        if (!xi_emit_symbol_index_to_arg(ctx, sym, &sym_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_GETPROP, dst, obj, sym_arg));
    } else {
        uint16_t field_arg = 0;
        if (!xi_emit_index_to_arg(ctx, v->aux_int, XI_EMIT_ERR_INTERNAL, &field_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_GETFIELD, dst, obj, field_arg));
    }
}

/* Field store: SETPROP or SETFIELD */
XR_FUNC void xi_emit_store_field(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg obj = reg_of(ctx, v->args[0]);
    XiEmitReg val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    const char *prop = (const char *) v->aux;
    if (prop) {
        int sym = add_symbol(ctx, prop);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t sym_arg = 0;
        if (!xi_emit_symbol_index_to_arg(ctx, sym, &sym_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_SETPROP, obj, sym_arg, val));
    } else {
        uint16_t field_arg = 0;
        if (!xi_emit_index_to_arg(ctx, v->aux_int, XI_EMIT_ERR_INTERNAL, &field_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_SETFIELD, obj, field_arg, val));
    }
}

/* Scan all transitive uses of a struct value in the function.
 * Stack allocation is safe while the value is only observed through field
 * operations or identity nodes whose result is checked recursively. */
static bool struct_uses_safe_depth(EmitCtx *ctx, XiValue *target, XiValue *origin, int depth) {
    if (depth > 8)
        return false;

    XiFunc *f = ctx->func;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        /* Block control (RETURN / IF condition) */
        if (blk->control == target)
            return false;

        /* Phi nodes — struct in PHI can cross loop iterations */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t k = 0; k < phi->value.nargs; k++) {
                if (phi->value.args[k] == target)
                    return false;
            }
        }

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != target)
                    continue;
                switch ((XiOp) v->op) {
                    case XI_STRUCT_GET:
                        break;
                    case XI_STRUCT_SET:
                        /* Safe only as args[0] (the container receiving the write).
                         * As args[1] (the stored value), this struct escapes into
                         * another struct's field — must be heap-allocated. */
                        if (a != 0)
                            return false;
                        break;
                    case XI_PRINT:
                        break;
                    case XI_RETAIN:
                    case XI_RELEASE:
                        if (a != 0)
                            return false;
                        break;
                    case XI_COPY:
                    case XI_MOVE:
                        if (a != 0)
                            return false;
                        if (xi_emit_trace_struct_origin(v) != origin)
                            return false;
                        if (!struct_uses_safe_depth(ctx, v, origin, depth + 1))
                            return false;
                        break;
                    default:
                        return false;
                }
            }
        }
    }
    return true;
}

/* Returns true if the struct never escapes local value operations and is safe
 * for stack allocation via OP_NEW_STRUCT. */
static bool struct_can_stack_alloc(EmitCtx *ctx, XiValue *target) {
    XiValue *origin = xi_emit_trace_struct_origin(target);
    return origin == target && struct_uses_safe_depth(ctx, target, origin, 0);
}

/* Struct new: decide stack vs heap at emit time.
 * Stack path: OP_NEW_STRUCT (frame struct_area, zero heap allocation).
 * Heap path:  OP_INVOKE(constructor) (normal object allocation). */
XR_FUNC void xi_emit_struct_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XrStructLayout *layout = (XrStructLayout *) v->aux;
    XR_DCHECK(layout != NULL, "XI_STRUCT_NEW: missing struct layout");

    if (struct_can_stack_alloc(ctx, v)) {
        /* Stack path: OP_NEW_STRUCT */
        XiEmitReg cls_reg = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;

        uint16_t slot = 0;
        if (!xi_emit_alloc_struct_area_slot(ctx, layout, &slot))
            return;

        emit_inst(ctx, CREATE_ABC(OP_NEW_STRUCT, dst, cls_reg, slot));
        v->aux_int |= XI_EMIT_STRUCT_PROMOTED_BIT;
    } else {
        /* Heap path: emit OP_INVOKE(constructor, 0 args).
         * OP_INVOKE needs R[dst+1] for receiver. Allocate a fresh
         * scratch register beyond all live values to avoid clobbering. */
        XiEmitReg recv = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;

        /* Use a high scratch register for the call window to avoid
         * interfering with live values in low registers. */
        if (ctx->max_reg + 2 > MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return;
        }
        XiEmitReg base = (XiEmitReg) ctx->max_reg;
        uint32_t call_top = (uint32_t) base + 2;
        if (call_top > ctx->max_reg)
            ctx->max_reg = call_top;

        /* R[base+1] = receiver (class), invoke stores result in R[base] */
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (base + 1), recv, 0));

        int sym = add_symbol(ctx, "constructor");
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t sym_arg = 0;
        if (!xi_emit_symbol_index_to_arg(ctx, sym, &sym_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_INVOKE, base, sym_arg, 0));

        /* Move result from scratch to actual destination */
        if (base != dst)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
    }
}

/* Struct get: read field.
 * Stack-promoted → OP_STRUCT_GET (direct native field read).
 * Heap fallback  → OP_GETPROP   (property lookup by name). */
XR_FUNC void xi_emit_struct_get(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XiValue *origin = xi_emit_trace_struct_origin(v->args[0]);
    bool promoted = (origin && origin->op == XI_STRUCT_NEW && XI_EMIT_STRUCT_IS_PROMOTED(origin));

    XiEmitReg obj = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;

    if (promoted) {
        XR_DCHECK(v->aux_int >= 0 && v->aux_int < XR_MAX_STRUCT_FIELDS,
                  "XI_STRUCT_GET: field_idx out of range");
        uint16_t field_arg = 0;
        if (!xi_emit_index_to_arg(ctx, v->aux_int, XI_EMIT_ERR_INTERNAL, &field_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_STRUCT_GET, dst, obj, field_arg));
    } else {
        /* Heap path: OP_GETPROP with field name from layout */
        XrStructLayout *sl = (XrStructLayout *) v->aux;
        const char *fname = (sl && sl->field_names && v->aux_int < sl->field_count)
                                ? sl->field_names[v->aux_int]
                                : NULL;
        XR_DCHECK(fname != NULL, "XI_STRUCT_GET: missing field name");
        int sym = add_symbol(ctx, fname);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t sym_arg = 0;
        if (!xi_emit_symbol_index_to_arg(ctx, sym, &sym_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_GETPROP, dst, obj, sym_arg));
    }
}

/* Struct set: write field.
 * Stack-promoted → OP_STRUCT_SET (direct native field write).
 * Heap fallback  → OP_SETPROP   (property store by name). */
XR_FUNC void xi_emit_struct_set(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XiValue *origin = xi_emit_trace_struct_origin(v->args[0]);
    bool promoted = (origin && origin->op == XI_STRUCT_NEW && XI_EMIT_STRUCT_IS_PROMOTED(origin));

    XiEmitReg obj = reg_of(ctx, v->args[0]);
    XiEmitReg val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;

    if (promoted) {
        XR_DCHECK(v->aux_int >= 0 && v->aux_int < XR_MAX_STRUCT_FIELDS,
                  "XI_STRUCT_SET: field_idx out of range");
        uint16_t field_arg = 0;
        if (!xi_emit_index_to_arg(ctx, v->aux_int, XI_EMIT_ERR_INTERNAL, &field_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_STRUCT_SET, obj, field_arg, val));
    } else {
        /* Heap path: OP_SETPROP with field name */
        XrStructLayout *sl = (XrStructLayout *) v->aux;
        const char *fname = (sl && sl->field_names && v->aux_int < sl->field_count)
                                ? sl->field_names[v->aux_int]
                                : NULL;
        XR_DCHECK(fname != NULL, "XI_STRUCT_SET: missing field name");
        int sym = add_symbol(ctx, fname);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t sym_arg = 0;
        if (!xi_emit_symbol_index_to_arg(ctx, sym, &sym_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_SETPROP, obj, sym_arg, val));
    }
}

/* Index get */
XR_FUNC void xi_emit_index_get(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg obj = reg_of(ctx, v->args[0]);
    XiEmitReg key = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_INDEX_GET, dst, obj, key));
}

/* Index set */
XR_FUNC void xi_emit_index_set(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg obj = reg_of(ctx, v->args[0]);
    XiEmitReg key = reg_of(ctx, v->args[1]);
    XiEmitReg val = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_INDEX_SET, obj, key, val));
}

/* Array creation */
XR_FUNC void xi_emit_array_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    /* C = (elem_tid << 2) | storage_mode.
     * elem_tid is only set when the lowerer explicitly encodes it in
     * aux_int (e.g. new Array<T>()).  Array literals like [1,2,3] always
     * create XR_ELEM_ANY arrays because OP_NEWARRAY pushes B elements
     * from registers and typed storage crashes on uninitialized slots. */
    uint8_t c_field = (uint8_t) (v->aux_int & 0xFF);
    if (v->nargs >= 1 && v->args[0]->op == XI_CONST && v->args[0]->aux_int >= 0 &&
        (uint64_t) v->args[0]->aux_int <= MAXARG_B) {
        emit_inst(ctx, CREATE_ABC(OP_NEWARRAY, dst, (uint16_t) v->args[0]->aux_int, c_field));
        return;
    }
    if (v->nargs < 1) {
        emit_inst(ctx, CREATE_ABC(OP_NEWARRAY, dst, 0, c_field));
        return;
    }
    XiEmitReg cap = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ARRAY_NEW_CAP, dst, cap, c_field));
}

/* Array spread append: R[A]:Array.push(R[B]).  In-place store, no dst. */
XR_FUNC void xi_emit_array_push(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg arr = reg_of(ctx, v->args[0]);
    XiEmitReg val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ARRAY_PUSH, arr, val, 0));
}

/* Array spread splice: R[A]:Array.extend(R[B]:Array).  In-place store, no dst. */
XR_FUNC void xi_emit_array_extend(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg arr = reg_of(ctx, v->args[0]);
    XiEmitReg src = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ARRAY_EXTEND, arr, src, 0));
}

/* Tuple creation: N elements in args[0..N-1].  OP_NEWTUPLE scoops
 * elements from R[base+1..base+N] in order, so we materialize the
 * window in a fresh scratch range above every source register.
 * Going through scratch (rather than moving directly into
 * R[dst+1..dst+N]) sidesteps the parallel-move hazard when an arg's
 * home register overlaps the target window — e.g. arg0 at R[k+1] and
 * arg1 at R[k] would have one move clobbering the other's source.
 * Tuples are immutable, so this single construction is the only
 * writer of element slots. */
XR_FUNC void xi_emit_tuple_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    uint16_t n = v->nargs;
    if (n + 1 > MAX_REGS - ctx->next_reg) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg = (base + 1 + n);
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    for (uint16_t a = 0; a < n; a++) {
        XiEmitReg src = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (base + 1 + a);
        if (src != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, src, 0));
    }
    emit_inst(ctx, CREATE_ABC(OP_NEWTUPLE, base, (uint8_t) n, 0));
    if (dst != base)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
}

/* Tuple field load: args[0] = tuple, aux_int = zero-based index. */
XR_FUNC void xi_emit_tuple_get(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg tup = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint8_t idx = (uint8_t) (v->aux_int & 0xFF);
    emit_inst(ctx, CREATE_ABC(OP_TUPLE_GET, dst, tup, idx));
}

/* Map creation */
XR_FUNC void xi_emit_map_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    uint16_t cap = 0;
    if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
        if (!xi_emit_index_to_arg(ctx, v->args[0]->aux_int, XI_EMIT_ERR_INTERNAL, &cap))
            return;
    }
    /* C field pre-encoded by lowerer: (key_kind<<7)|(value_tid<<2)|flags */
    uint8_t c_field = (uint8_t) (v->aux_int & 0xFF);
    emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, cap, c_field));
}

/* Set creation */
XR_FUNC void xi_emit_set_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    /* B field pre-encoded by lowerer: (elem_tid<<2)|flags */
    uint8_t b_field = (uint8_t) (v->aux_int & 0xFF);
    emit_inst(ctx, CREATE_ABC(OP_NEWSET, dst, b_field, 0));
}

/* Json object creation: build Shape, store in constant pool, emit OP_NEWJSON */
XR_FUNC void xi_emit_json_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    int field_count = xi_json_field_count(v);
    uint8_t storage_mode = xi_json_storage_mode(v);
    const char **field_names = (const char **) v->aux;
    if (field_count < 0 || field_count > UINT16_MAX) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    if (!ctx->isolate) {
        /* No isolate: cannot build Shape, fall back to Map */
        emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, 0, 0));
        return;
    }
    /* Build dynamic-layout class chain (jsonRoot -> field1 -> ... -> fieldN).
     * Empty {} just uses the root. The leaf class becomes the constant the
     * VM uses to allocate the Json instance. */
    int n = field_count > 0 ? field_count : 0;
    XrClass *cls = xr_class_build_json_chain(ctx->isolate, field_names, n, false);
    if (!cls) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* Store class pointer as integer constant in pool */
    int kidx = add_const_int(ctx, (int64_t) (intptr_t) cls);
    if (ctx->status != XI_EMIT_OK)
        return;

    uint16_t karg;
    if (!xi_emit_const_index_to_c(ctx, kidx, &karg))
        return;

    emit_inst(ctx, CREATE_ABC(OP_NEWJSON, dst, karg, storage_mode));
}

/* Json field init by index: OP_JSON_INIT A B C (A=json, B=field_idx, C=val) */
XR_FUNC void xi_emit_json_init_f(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst; /* dst unused; this is a store op */
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg json_reg = reg_of(ctx, v->args[0]);
    XiEmitReg val_reg = reg_of(ctx, v->args[1]);
    int field_idx = (int) v->aux_int;
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t field_arg = 0;
    if (!xi_emit_index_to_arg(ctx, field_idx, XI_EMIT_ERR_INTERNAL, &field_arg))
        return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_INIT, json_reg, field_arg, val_reg));
}

/* Json field read by index: OP_JSON_GET A B C (A=dst, B=json, C=field_idx) */
XR_FUNC void xi_emit_json_get_f(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg json_reg = reg_of(ctx, v->args[0]);
    int field_idx = (int) v->aux_int;
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t field_arg = 0;
    if (!xi_emit_index_to_arg(ctx, field_idx, XI_EMIT_ERR_INTERNAL, &field_arg))
        return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_GET, dst, json_reg, field_arg));
}

/* Json field write by index: OP_JSON_SET A B C (A=json, B=field_idx, C=val) */
XR_FUNC void xi_emit_json_set_f(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst; /* dst unused; this is a store op */
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg json_reg = reg_of(ctx, v->args[0]);
    XiEmitReg val_reg = reg_of(ctx, v->args[1]);
    int field_idx = (int) v->aux_int;
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t field_arg = 0;
    if (!xi_emit_index_to_arg(ctx, field_idx, XI_EMIT_ERR_INTERNAL, &field_arg))
        return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_SET, json_reg, field_arg, val_reg));
}

/* Json merge: OP_JSON_MERGE A B (A=dst Json, B=src Json).  In-place store. */
XR_FUNC void xi_emit_json_merge(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg dst_reg = reg_of(ctx, v->args[0]);
    XiEmitReg src_reg = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_MERGE, dst_reg, src_reg, 0));
}

/* Typed JSON decode: OP_JSON_DECODE A B C
 * A=dst (result: T? sealed Json or null)
 * B=data register (string to parse)
 * C=Shape constant index (built from field names)
 *
 * Reuses the same Shape-building logic as xi_emit_json_new. */
XR_FUNC void xi_emit_json_decode(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    int n = (int) v->aux_int;
    const char **field_names = (const char **) v->aux;
    XR_DCHECK(n > 0 && field_names != NULL, "json_decode: no field info");
    if (n < 0 || n > UINT16_MAX) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XiEmitReg data_reg = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;

    /* Build sealed class chain: typed JSON decode produces a fixed-shape
     * sealed Json with exactly the declared fields. */
    bool sealed = (v->type && v->type->kind == XR_KIND_JSON && v->type->object.is_sealed);
    XrClass *cls = xr_class_build_json_chain(ctx->isolate, field_names, n, sealed);
    if (!cls) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    int kidx = add_const_int(ctx, (int64_t) (intptr_t) cls);
    if (ctx->status != XI_EMIT_OK)
        return;

    uint16_t karg;
    if (!xi_emit_const_index_to_c(ctx, kidx, &karg))
        return;

    emit_inst(ctx, CREATE_ABC(OP_JSON_DECODE, dst, data_reg, karg));
}

/* Range creation */
XR_FUNC void xi_emit_range(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg start = reg_of(ctx, v->args[0]);
    XiEmitReg end = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(v->aux_int ? OP_NEWRANGE_INCLUSIVE : OP_NEWRANGE, dst, start, end));
}

/* Slice: OP_SLICE expects start at R[C], end at R[C+1] (consecutive) */
XR_FUNC void xi_emit_slice(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    XiEmitReg lo_src = reg_of(ctx, v->args[1]);
    XiEmitReg hi_src = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (ctx->next_reg + 2 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg lo_slot = (XiEmitReg) ctx->next_reg;
    ctx->next_reg += 2;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    emit_inst(ctx, CREATE_ABC(OP_MOVE, lo_slot, lo_src, 0));
    emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (lo_slot + 1), hi_src, 0));
    emit_inst(ctx, CREATE_ABC(OP_SLICE, dst, src, lo_slot));
}

/* Closure creation */
XR_FUNC void xi_emit_closure_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    XiFunc *child_func = (XiFunc *) v->aux;
    XR_DCHECK(child_func != NULL, "closure child func must not be NULL");

    XrProto *child_proto = NULL;
    XiEmitStatus child_st = xi_emit(child_func, ctx->isolate, &child_proto);
    if (child_st != XI_EMIT_OK || !child_proto) {
        emit_error(ctx, child_st != XI_EMIT_OK ? child_st : XI_EMIT_ERR_INTERNAL);
        return;
    }
    /* Propagate shared_offset to child and all its descendants */
    propagate_shared_offset(child_proto, ctx->proto->shared_offset);

    /* Populate upvalue descriptors on child proto from captures */
    for (uint16_t ci = 0; ci < child_func->ncaptures; ci++) {
        XiCapture *cap = &child_func->captures[ci];
        uint16_t uv_index = 0;
        if (cap->source == XI_CAPTURE_SRC_REG) {
            /* Use CLOSURE_NEW's arg (kept current by optimization passes)
             * instead of cap->value which may point to an eliminated PHI. */
            XiValue *cap_val = (ci < v->nargs && v->args[ci]) ? v->args[ci] : cap->value;
            XR_DCHECK(cap_val != NULL, "SRC_REG capture must have parent SSA value");
            uv_index = reg_of(ctx, cap_val);
            if (ctx->status != XI_EMIT_OK)
                return;
        } else {
            uv_index = cap->index;
        }
        xr_vm_proto_add_upvalue(child_proto, uv_index, 0, 0, 0, cap->source, cap->type);
    }

    /* Cell wrapping for mutable captures (emit once per value).
     * Skip if cell was already created at the variable's first definition
     * (cell_created[var_id] is set by the early CELL_NEW path in emit_value). */
    for (uint16_t ci = 0; ci < child_func->ncaptures; ci++) {
        XiCapture *cap = &child_func->captures[ci];
        if (cap->needs_cell && cap->source == XI_CAPTURE_SRC_REG) {
            XiValue *cap_val = (ci < v->nargs && v->args[ci]) ? v->args[ci] : cap->value;
            XiEmitReg reg = reg_of(ctx, cap_val);
            if (ctx->status != XI_EMIT_OK)
                return;
            bool already = ctx->cell_wrapped[cap_val->id];
            if (!already && xi_emit_var_id_in_state(ctx, cap_val->var_id))
                already = ctx->cell_created[cap_val->var_id];
            if (!already) {
                emit_inst(ctx, CREATE_ABC(OP_CELL_NEW, reg, 0, 0));
                ctx->cell_wrapped[cap_val->id] = true;
                if (xi_emit_var_id_in_state(ctx, cap_val->var_id)) {
                    ctx->cell_side_reg[cap_val->var_id] = reg;
                    ctx->cell_created[cap_val->var_id] = true;
                }
            }
        }
    }

    /* Transfer Xi IR ownership to child proto for AOT direct lowering */
    xi_emit_attach_ir(child_proto, child_func);
    uint16_t cidx = (uint16_t) v->aux_int;
    if (cidx < ctx->func->nchildren && ctx->func->children[cidx] == child_func) {
        ctx->func->children[cidx] = NULL;
    }

    int proto_idx = xr_vm_proto_add_proto(ctx->proto, child_proto);
    if (xi_emit_var_has_side_cell(ctx, v->var_id)) {
        /* The destination register is cell-wrapped (hoisted function).
         * Emit closure to a temp register, then store into the cell. */
        if (ctx->next_reg >= MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return;
        }
        XiEmitReg tmp = (XiEmitReg) ctx->next_reg++;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
        emit_inst(ctx, CREATE_ABx(OP_CLOSURE, tmp, proto_idx));
        emit_inst(ctx, CREATE_ABC(OP_CELL_SET, dst, tmp, 0));
    } else {
        emit_inst(ctx, CREATE_ABx(OP_CLOSURE, dst, proto_idx));
    }
}

/* Upvalue load */
XR_FUNC void xi_emit_load_upval(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    int upval_idx = (int) v->aux_int;
    emit_inst(ctx, CREATE_ABC(OP_UPVAL_GET, dst, (uint16_t) upval_idx, 0));
    if (upval_idx < (int) ctx->func->ncaptures && ctx->func->captures[upval_idx].needs_cell) {
        emit_inst(ctx, CREATE_ABC(OP_CELL_GET, dst, dst, 0));
    }
}

/* Upvalue store */
XR_FUNC void xi_emit_store_upval(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg val = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int upval_idx = (int) v->aux_int;
    XiEmitReg cell_reg = dst;
    emit_inst(ctx, CREATE_ABC(OP_UPVAL_GET, cell_reg, (uint16_t) upval_idx, 0));
    emit_inst(ctx, CREATE_ABC(OP_CELL_SET, cell_reg, val, 0));
}

/* Shared (module-level) variable access */
XR_FUNC void xi_emit_get_shared(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    int shared_idx = (int) v->aux_int;
    emit_inst(ctx, CREATE_ABx(OP_GETSHARED, dst, shared_idx));
}

XR_FUNC void xi_emit_set_shared(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg val = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int shared_idx = (int) v->aux_int;
    emit_inst(ctx, CREATE_ABx(OP_SETSHARED, val, shared_idx));
}

/* Name-keyed top-level globals.  v->aux is a const char* with the
 * binding's source name; the emitter interns it into the proto
 * constant pool and emits OP_GETGLOBAL / OP_SETGLOBAL.  Used in REPL
 * mode where every cross-input top-level binding goes through the
 * runtime globals dict instead of an integer-indexed shared array. */
XR_FUNC void xi_emit_get_global(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    const char *name = (const char *) v->aux;
    if (!name) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    int kx = add_const_string(ctx, name);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABx(OP_GETGLOBAL, dst, kx));
}

XR_FUNC void xi_emit_set_global(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    const char *name = (const char *) v->aux;
    if (!name) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg val = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int kx = add_const_string(ctx, name);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABx(OP_SETGLOBAL, val, kx));
}

/* Runtime global variable lookup */
XR_FUNC void xi_emit_get_builtin(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_inst(ctx, CREATE_ABx(OP_GETBUILTIN, dst, (int) v->aux_int));
}

/* Iteration protocol */
XR_FUNC void xi_emit_iter(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg obj = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;

    const char *method = v->op == XI_ITER_NEW     ? "iterator"
                         : v->op == XI_ITER_VALID ? "hasNext"
                                                  : "next";
    int sym = add_symbol(ctx, method);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t sym_arg = 0;
    if (!xi_emit_symbol_index_to_arg(ctx, sym, &sym_arg))
        return;

    XiEmitReg base = dst;
    if (obj != base + 1)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, base + 1, obj, 0));
    emit_inst(ctx, CREATE_ABC(OP_INVOKE, base, sym_arg, 1));
}

/* Class creation wrapper */
XR_FUNC void xi_emit_class_create(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    XiClassData *cdata = (XiClassData *) v->aux;
    XR_DCHECK(cdata != NULL && cdata->ast != NULL, "XI_CLASS_CREATE: missing class data");
    emit_class_create_impl(ctx, v, cdata, dst);
}

/* Try to resolve an import reference at emit time by looking up the
 * pre-loaded module_table on the registry.  For selective imports,
 * fills both resolved_mod_index and resolved_shared_slot.  For
 * whole-module imports, fills only resolved_mod_index. */
static bool try_emit_time_resolve(EmitCtx *ctx, XiImportRef *ref) {
    XR_DCHECK(ctx != NULL && ref != NULL, "try_emit_time_resolve: NULL arg");
    if (!ctx->isolate)
        return false;

    XrModuleRegistry *mreg = (XrModuleRegistry *) xr_isolate_get_module_registry(ctx->isolate);
    if (!mreg || !mreg->module_table || mreg->module_table_count <= 0)
        return false;

    /* Resolve module_path to an absolute path via the resolver */
    char *abs_path = xr_module_resolve_path(ctx->isolate, ref->module_path);
    if (!abs_path)
        return false;

    /* Normalize to realpath for cache matching */
    char *real = xr_realpath(abs_path);
    if (real) {
        xr_free(abs_path);
        abs_path = real;
    }

    /* Find this module in module_table by matching path */
    int target_topo = -1;
    for (int ti = 0; ti < mreg->module_table_count; ti++) {
        XrModule *m = mreg->module_table[ti];
        if (m && m->path && strcmp(m->path, abs_path) == 0) {
            target_topo = ti;
            break;
        }
    }
    xr_free(abs_path);
    if (target_topo < 0 || (uint64_t) target_topo > MAXARG_B)
        return false;

    ref->resolved_mod_index = target_topo;

    /* Whole-module import: no slot needed */
    if (!ref->member_name)
        return true;

    /* Selective import: find export slot by name */
    XrModule *target = mreg->module_table[target_topo];
    XR_DCHECK(target != NULL, "try_emit_time_resolve: NULL target module");
    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->isolate);
    SymbolId sym = xr_symbol_lookup_in_table(sym_table, ref->member_name);
    if (sym < 0)
        return false;

    /* Dense index lookup via sparse table */
    if (target->symbol_to_index && sym >= target->min_symbol && sym <= target->max_symbol) {
        int32_t slot = target->symbol_to_index[sym - target->min_symbol];
        if (slot >= 0 && (uint64_t) slot <= MAXARG_C) {
            ref->resolved_shared_slot = slot;
            return true;
        }
    }
    /* Fallback: linear scan */
    for (uint16_t ei = 0; ei < target->export_count; ei++) {
        if (target->export_symbols[ei] == sym) {
            ref->resolved_shared_slot = (int) ei;
            return true;
        }
    }
    /* Module found but member not found — keep mod_index set for
     * LOAD_MODULE fallback, but don't claim full resolution. */
    return false;
}

/* Module import emission.
 * Selective imports → OP_LOAD_MODULE_SLOT (single indexed load).
 * Whole-module imports → OP_LOAD_MODULE (module object by topo index).
 * Unresolved imports → LOADNULL (should not happen with graph). */
XR_FUNC void xi_emit_import_ref(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    XiImportRef *ref = (XiImportRef *) v->aux;
    if (!ref || !ref->module_path || !ctx->isolate) {
        emit_inst(ctx, CREATE_ABx(OP_LOADNULL, dst, 0));
        return;
    }

    /* Try emit-time resolution if not already resolved */
    if (ref->resolved_mod_index < 0)
        try_emit_time_resolve(ctx, ref);

    /* Selective import: OP_LOAD_MODULE_SLOT */
    if (ref->resolved_mod_index >= 0 && ref->resolved_shared_slot >= 0 && ref->member_name) {
        uint16_t mod_arg = 0;
        uint16_t slot_arg = 0;
        if (!xi_emit_index_to_arg(ctx, ref->resolved_mod_index, XI_EMIT_ERR_TOO_MANY_CONSTS,
                                  &mod_arg) ||
            !xi_emit_index_to_arg(ctx, ref->resolved_shared_slot, XI_EMIT_ERR_TOO_MANY_CONSTS,
                                  &slot_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_LOAD_MODULE_SLOT, dst, mod_arg, slot_arg));
        return;
    }

    /* Whole-module import: OP_LOAD_MODULE */
    if (ref->resolved_mod_index >= 0 && !ref->member_name) {
        emit_inst(ctx, CREATE_ABx(OP_LOAD_MODULE, dst, ref->resolved_mod_index));
        return;
    }

    /* Fallback: OP_IMPORT for stdlib/native modules not in the graph,
     * or when module_table is unavailable (REPL). */
    int mod_idx = add_const_string(ctx, ref->module_path);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABx(OP_IMPORT, dst, mod_idx));
    if (ref->member_name) {
        int sym_idx = add_symbol(ctx, ref->member_name);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t sym_arg = 0;
        if (!xi_emit_symbol_index_to_arg(ctx, sym_idx, &sym_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_GETPROP, dst, dst, sym_arg));
    }
}

/* ========== Class Emission Helpers ========== */

/* Convert an AST literal to XrValue for field default values. */
static XrValue ast_field_default_to_value(EmitCtx *ctx, AstNode *init) {
    if (!init)
        return xr_null();
    if (init->type == AST_LITERAL_INT)
        return xr_int(init->as.literal.raw_value.int_val);
    if (init->type == AST_LITERAL_FLOAT)
        return xr_float(init->as.literal.raw_value.float_val);
    if (init->type == AST_LITERAL_TRUE)
        return xr_bool(true);
    if (init->type == AST_LITERAL_FALSE)
        return xr_bool(false);
    if (init->type == AST_LITERAL_CHAR)
        return xr_char(init->as.literal.raw_value.char_val);
    if (init->type == AST_LITERAL_STRING && ctx->isolate) {
        const char *s = init->as.literal.raw_value.string_val;
        if (s) {
            XrString *xs = xr_compile_time_intern(ctx->isolate, s, strlen(s));
            if (xs)
                return xr_string_value(xs);
        }
    }
    return xr_null();
}

/* Populate instance and static fields on the descriptor from AST. */
static bool emit_class_collect_fields_impl(EmitCtx *ctx, ClassDeclNode *cd,
                                           XrClassDescriptor *desc) {
    /* Instance fields */
    uint32_t fc = 0;
    for (int i = 0; i < cd->field_count; i++)
        if (cd->fields[i]->type == AST_FIELD_DECL && !cd->fields[i]->as.field_decl.is_static)
            fc++;
    if (fc > 0) {
        desc->instance_fields =
            (XrFieldDescriptorEntry *) xr_calloc(fc, sizeof(XrFieldDescriptorEntry));
        if (!desc->instance_fields)
            return false;
        desc->instance_field_count = fc;
        uint32_t idx = 0;
        for (int i = 0; i < cd->field_count; i++) {
            if (cd->fields[i]->type != AST_FIELD_DECL)
                continue;
            FieldDeclNode *f = &cd->fields[i]->as.field_decl;
            if (f->is_static)
                continue;
            desc->instance_fields[idx].name = strdup(f->name);
            desc->instance_fields[idx].type_name =
                f->field_type ? xr_type_to_string(xr_tref_resolve(ctx->isolate, f->field_type))
                              : NULL;
            desc->instance_fields[idx].default_value =
                ast_field_default_to_value(ctx, f->initializer);
            if (f->is_private)
                desc->instance_fields[idx].flags |= XR_FIELD_PRIVATE;
            if (f->is_final)
                desc->instance_fields[idx].flags |= XR_FIELD_FINAL;
            idx++;
        }
    }
    /* Static fields */
    uint32_t sfc = 0;
    for (int i = 0; i < cd->field_count; i++)
        if (cd->fields[i]->type == AST_FIELD_DECL && cd->fields[i]->as.field_decl.is_static)
            sfc++;
    if (sfc > 0) {
        desc->static_fields =
            (XrFieldDescriptorEntry *) xr_calloc(sfc, sizeof(XrFieldDescriptorEntry));
        if (!desc->static_fields)
            return false;
        desc->static_field_count = sfc;
        uint32_t idx = 0;
        for (int i = 0; i < cd->field_count; i++) {
            if (cd->fields[i]->type != AST_FIELD_DECL)
                continue;
            FieldDeclNode *f = &cd->fields[i]->as.field_decl;
            if (!f->is_static)
                continue;
            desc->static_fields[idx].name = strdup(f->name);
            desc->static_fields[idx].flags = XR_FIELD_STATIC;
            desc->static_fields[idx].type_name =
                f->field_type ? xr_type_to_string(xr_tref_resolve(ctx->isolate, f->field_type))
                              : NULL;
            desc->static_fields[idx].default_value =
                ast_field_default_to_value(ctx, f->initializer);
            idx++;
        }
    }
    return true;
}

/* Emit a child XiFunc as a sub-proto and return the proto index. */
static int emit_method_proto_impl(EmitCtx *ctx, uint16_t child_func_idx) {
    XR_DCHECK(child_func_idx < ctx->func->nchildren, "child index out of bounds");
    XiFunc *child = ctx->func->children[child_func_idx];
    XrProto *child_proto = NULL;
    XiEmitStatus cst = xi_emit(child, ctx->isolate, &child_proto);
    if (cst != XI_EMIT_OK || !child_proto)
        return -1;
    /* Propagate shared_offset to child and all its descendants */
    propagate_shared_offset(child_proto, ctx->proto->shared_offset);

    for (uint16_t ui = 0; ui < child->ncaptures; ui++) {
        XiCapture *cap = &child->captures[ui];
        uint16_t uv_idx = 0;
        if (cap->source == XI_CAPTURE_SRC_REG) {
            XR_DCHECK(cap->value != NULL, "SRC_REG capture must have parent SSA value");
            uv_idx = reg_of(ctx, cap->value);
            if (ctx->status != XI_EMIT_OK)
                return -1;
        } else {
            uv_idx = cap->index;
        }
        xr_vm_proto_add_upvalue(child_proto, uv_idx, 0, 0, 0, cap->source, cap->type);
    }

    xi_emit_attach_ir(child_proto, child);
    ctx->func->children[child_func_idx] = NULL;

    return xr_vm_proto_add_proto(ctx->proto, child_proto);
}

/* Build XrClassDescriptor from AST, emit child method protos,
 * and generate OP_CLASS_CREATE_FROM_DESCRIPTOR. */
static void emit_class_create_impl(EmitCtx *ctx, XiValue *v, XiClassData *cdata, XiEmitReg dst) {
    (void) v;
    ClassDeclNode *cd = &cdata->ast->as.class_decl;

    XrClassDescriptor *desc = (XrClassDescriptor *) xr_calloc(1, sizeof(XrClassDescriptor));
    if (!desc) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    desc->class_name = strdup(cd->name);
    desc->super_name = (cd->super_name && !cd->super_module) ? strdup(cd->super_name) : NULL;
    desc->generic_origin_name =
        cdata->generic_origin_name ? strdup(cdata->generic_origin_name) : NULL;
    desc->display_name = cdata->display_name ? strdup(cdata->display_name) : NULL;
    desc->is_monomorphized = cdata->is_monomorphized;
    desc->mono_type_arg_names = NULL;
    desc->mono_type_arg_count = 0;
    if (cdata->mono_type_arg_count > 0 && cdata->mono_type_arg_names) {
        const char **names =
            (const char **) xr_calloc(cdata->mono_type_arg_count, sizeof(const char *));
        if (names) {
            for (int i = 0; i < cdata->mono_type_arg_count; i++)
                names[i] =
                    cdata->mono_type_arg_names[i] ? strdup(cdata->mono_type_arg_names[i]) : NULL;
            desc->mono_type_arg_names = names;
            desc->mono_type_arg_count = cdata->mono_type_arg_count;
        }
    }
    desc->super_global_index = -1;
    desc->descriptor_version = XR_CLASS_DESCRIPTOR_VERSION;
    desc->clinit_proto_index = -1;
    if (cd->is_abstract)
        desc->flags |= XR_CLASS_ABSTRACT;
    if (cd->is_final)
        desc->flags |= XR_CLASS_FINAL;
    if (cdata->is_monomorphized)
        desc->flags |= XR_CLASS_MONOMORPHIZED;
    if (cdata->is_cycle_candidate)
        desc->flags |= XR_CLASS_CYCLE_CANDIDATE;

    if (!emit_class_collect_fields_impl(ctx, cd, desc)) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* Propagate native struct layout and VALUE_TYPE flag for struct classes */
    if (cdata->struct_layout) {
        desc->struct_layout = cdata->struct_layout;
        desc->flags |= XR_CLASS_VALUE_TYPE;
    }

    /* Instance methods */
    if (cdata->ninst > 0) {
        desc->instance_methods =
            (XrMethodDescriptorEntry *) xr_calloc(cdata->ninst, sizeof(XrMethodDescriptorEntry));
        if (!desc->instance_methods) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        desc->instance_method_count = cdata->ninst;
        uint16_t mi = 0;
        for (int i = 0; i < cd->method_count && mi < cdata->ninst; i++) {
            if (cd->methods[i]->type != AST_METHOD_DECL)
                continue;
            MethodDeclNode *m = &cd->methods[i]->as.method_decl;
            if (m->is_static || m->is_static_constructor)
                continue;
            desc->instance_methods[mi].name = strdup(m->name);
            desc->instance_methods[mi].param_count = m->param_count;
            if (m->is_constructor || strcmp(m->name, "constructor") == 0 ||
                strcmp(m->name, "init") == 0)
                desc->instance_methods[mi].flags |= XMETHOD_FLAG_CONSTRUCTOR;
            if (m->is_private)
                desc->instance_methods[mi].flags |= XMETHOD_FLAG_PRIVATE;
            if (m->is_abstract)
                desc->instance_methods[mi].flags |= XMETHOD_FLAG_ABSTRACT;
            if (m->is_final)
                desc->instance_methods[mi].flags |= XMETHOD_FLAG_FINAL;
            if (m->is_operator) {
                desc->instance_methods[mi].is_operator = true;
                desc->instance_methods[mi].op_type = m->op_type;
            }
            XR_DCHECK(cdata->child_idx != NULL, "child_idx must be set");
            int pi = emit_method_proto_impl(ctx, cdata->child_idx[mi]);
            if (pi < 0) {
                emit_error(ctx, XI_EMIT_ERR_INTERNAL);
                return;
            }
            desc->instance_methods[mi].closure_index = (uint32_t) pi;
            mi++;
        }
        if (mi < cdata->ninst) {
            desc->instance_methods[mi].name = strdup("constructor");
            desc->instance_methods[mi].param_count = 0;
            desc->instance_methods[mi].flags |= XMETHOD_FLAG_CONSTRUCTOR;
            XR_DCHECK(cdata->child_idx != NULL, "child_idx must be set");
            int pi = emit_method_proto_impl(ctx, cdata->child_idx[mi]);
            if (pi < 0) {
                emit_error(ctx, XI_EMIT_ERR_INTERNAL);
                return;
            }
            desc->instance_methods[mi].closure_index = (uint32_t) pi;
        }
    }

    /* Static methods */
    if (cdata->nstat > 0) {
        desc->static_methods =
            (XrMethodDescriptorEntry *) xr_calloc(cdata->nstat, sizeof(XrMethodDescriptorEntry));
        if (!desc->static_methods) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        desc->static_method_count = cdata->nstat;
        uint16_t mi = 0, off = cdata->ninst;
        for (int i = 0; i < cd->method_count && mi < cdata->nstat; i++) {
            if (cd->methods[i]->type != AST_METHOD_DECL)
                continue;
            MethodDeclNode *m = &cd->methods[i]->as.method_decl;
            if (!m->is_static || m->is_static_constructor)
                continue;
            desc->static_methods[mi].name = strdup(m->name);
            desc->static_methods[mi].param_count = m->param_count;
            desc->static_methods[mi].flags = XMETHOD_FLAG_STATIC;
            XR_DCHECK(cdata->child_idx != NULL, "child_idx must be set");
            int pi = emit_method_proto_impl(ctx, cdata->child_idx[off + mi]);
            if (pi < 0) {
                emit_error(ctx, XI_EMIT_ERR_INTERNAL);
                return;
            }
            desc->static_methods[mi].closure_index = (uint32_t) pi;
            mi++;
        }
    }

    /* Add descriptor to constant pool and emit bytecode */
    XrValue desc_val = XR_FROM_PTR(desc);
    int desc_idx = xr_vm_proto_add_constant(ctx->proto, desc_val);
    if (desc_idx < 0 || (uint64_t) desc_idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
        return;
    }
    /* Compile static constructor (<clinit>) if present */
    if (cdata->clinit_child_idx >= 0) {
        int clinit_pi = emit_method_proto_impl(ctx, cdata->clinit_child_idx);
        if (clinit_pi >= 0)
            desc->clinit_proto_index = clinit_pi;
    }

    /* If the lowerer resolved a super class, emit it into R[A] so the
     * VM uses the scope-resolved class instead of a name-based registry
     * lookup that may find a same-named builtin. */
    if (v->nargs >= 1 && v->args[0]) {
        XiEmitReg super_reg = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        if (super_reg != dst)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, super_reg, 0));
    } else {
        emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
    }
    emit_inst(ctx, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, dst, desc_idx));

    /* Emit OP_CLINIT_CALL to run static field initializers */
    if (cdata->clinit_child_idx >= 0) {
        emit_inst(ctx, CREATE_ABx(OP_CLINIT_CALL, dst, desc_idx));
    }
}
