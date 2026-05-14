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
#include "../runtime/object/xshape.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xmethod.h"
#include "../runtime/closure/xclosure.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../frontend/parser/xast_nodes.h"
#include "../frontend/analyzer/xtype_ref_resolve.h"

/* Forward declaration for class helpers (defined later in this file) */
static void emit_class_create_impl(EmitCtx *ctx, XiValue *v, XiClassData *cdata, uint8_t dst);

/* Field load: GETPROP or GETFIELD */
XR_FUNC void xi_emit_load_field(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t obj = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    const char *prop = (const char *) v->aux;
    if (prop) {
        int sym = add_symbol(ctx, prop);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_GETPROP, dst, obj, (uint8_t) sym));
        /* Record IC-relevant instruction offset for JIT */
        if (v->id < ctx->reg_map_size)
            ctx->value_pc[v->id] = current_pc(ctx) - 1;
    } else {
        emit_inst(ctx, CREATE_ABC(OP_GETFIELD, dst, obj, (uint8_t) v->aux_int));
    }
}

/* Field store: SETPROP or SETFIELD */
XR_FUNC void xi_emit_store_field(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t obj = reg_of(ctx, v->args[0]);
    uint8_t val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    const char *prop = (const char *) v->aux;
    if (prop) {
        int sym = add_symbol(ctx, prop);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_SETPROP, obj, (uint8_t) sym, val));
        if (v->id < ctx->reg_map_size)
            ctx->value_pc[v->id] = current_pc(ctx) - 1;
    } else {
        emit_inst(ctx, CREATE_ABC(OP_SETFIELD, obj, (uint8_t) v->aux_int, val));
    }
}

/* Scan all uses of XI_STRUCT_NEW value v in the function.
 * Returns true if the struct never escapes local field access —
 * safe for stack allocation via OP_NEW_STRUCT.
 *
 * Safe consumers: XI_STRUCT_GET (args[0]),
 *                 XI_STRUCT_SET (args[0] only — the container),
 *                 XI_PRINT.
 * Unsafe: XI_STRUCT_SET args[1] (stored value escapes into field),
 *         COPY (value semantics require independent copies),
 *         returned, passed as call arg, stored to shared/upval/field,
 *         pushed into container, used in PHI (may cross loop iteration). */
static bool struct_can_stack_alloc(EmitCtx *ctx, XiValue *target) {
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

        /* Value args: only direct field access on this struct is safe.
         * Everything else (COPY, CALL, STORE, RETURN...) escapes.
         *
         * XI_STRUCT_GET: target must be args[0] (the struct being read).
         * XI_STRUCT_SET: target as args[0] is safe (container being written).
         *                target as args[1] is UNSAFE — storing a stack struct_ref
         *                into another struct creates a dangling pointer when the
         *                container is heap-allocated. */
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
                    default:
                        return false;
                }
            }
        }
    }
    return true;
}

/* Mark a XI_STRUCT_NEW value as stack-promoted by setting a flag bit.
 * Emit handlers for XI_STRUCT_GET/SET check this to decide the opcode. */
#define STRUCT_PROMOTED_BIT ((int64_t) 1 << 32)
#define STRUCT_IS_PROMOTED(v) (((v)->aux_int & STRUCT_PROMOTED_BIT) != 0)

/* Trace through COPY chain to find the defining XI_STRUCT_NEW. */
static XiValue *trace_struct_origin(XiValue *v) {
    while (v && v->op == XI_COPY && v->nargs >= 1)
        v = v->args[0];
    return v;
}

/* Struct new: decide stack vs heap at emit time.
 * Stack path: OP_NEW_STRUCT (frame struct_area, zero heap allocation).
 * Heap path:  OP_INVOKE(constructor) (normal object allocation). */
XR_FUNC void xi_emit_struct_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XrStructLayout *layout = (XrStructLayout *) v->aux;
    XR_DCHECK(layout != NULL, "XI_STRUCT_NEW: missing struct layout");

    if (struct_can_stack_alloc(ctx, v)) {
        /* Stack path: OP_NEW_STRUCT */
        uint8_t cls_reg = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;

        uint16_t slot = ctx->struct_area_offset;
        uint16_t bytes_needed = (uint16_t) (8 + layout->total_size);
        uint16_t slots_needed = (bytes_needed + 15) / 16;
        ctx->struct_area_offset = (uint16_t) (slot + slots_needed);

        XR_DCHECK(slot <= 255, "XI_STRUCT_NEW: struct_area slot overflow");
        emit_inst(ctx, CREATE_ABC(OP_NEW_STRUCT, dst, cls_reg, (uint8_t) slot));
        v->aux_int |= STRUCT_PROMOTED_BIT;
    } else {
        /* Heap path: emit OP_INVOKE(constructor, 0 args).
         * OP_INVOKE needs R[dst+1] for receiver. Allocate a fresh
         * scratch register beyond all live values to avoid clobbering. */
        uint8_t recv = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;

        /* Use a high scratch register for the call window to avoid
         * interfering with live values in low registers. */
        uint8_t base = ctx->max_reg;
        uint8_t call_top = (uint8_t) (base + 2);
        if (call_top > ctx->max_reg)
            ctx->max_reg = call_top;

        /* R[base+1] = receiver (class), invoke stores result in R[base] */
        emit_inst(ctx, CREATE_ABC(OP_MOVE, base + 1, recv, 0));

        int sym = add_symbol(ctx, "constructor");
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_INVOKE, base, (uint8_t) sym, 0));

        /* Move result from scratch to actual destination */
        if (base != dst)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
    }
}

/* Struct get: read field.
 * Stack-promoted → OP_STRUCT_GET (direct native field read).
 * Heap fallback  → OP_GETPROP   (property lookup by name). */
XR_FUNC void xi_emit_struct_get(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XiValue *origin = trace_struct_origin(v->args[0]);
    bool promoted = (origin && origin->op == XI_STRUCT_NEW && STRUCT_IS_PROMOTED(origin));

    uint8_t obj = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;

    if (promoted) {
        XR_DCHECK(v->aux_int >= 0 && v->aux_int < XR_MAX_STRUCT_FIELDS,
                  "XI_STRUCT_GET: field_idx out of range");
        emit_inst(ctx, CREATE_ABC(OP_STRUCT_GET, dst, obj, (uint8_t) v->aux_int));
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
        emit_inst(ctx, CREATE_ABC(OP_GETPROP, dst, obj, (uint8_t) sym));
        if (v->id < ctx->reg_map_size)
            ctx->value_pc[v->id] = current_pc(ctx) - 1;
    }
}

/* Struct set: write field.
 * Stack-promoted → OP_STRUCT_SET (direct native field write).
 * Heap fallback  → OP_SETPROP   (property store by name). */
XR_FUNC void xi_emit_struct_set(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XiValue *origin = trace_struct_origin(v->args[0]);
    bool promoted = (origin && origin->op == XI_STRUCT_NEW && STRUCT_IS_PROMOTED(origin));

    uint8_t obj = reg_of(ctx, v->args[0]);
    uint8_t val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;

    if (promoted) {
        XR_DCHECK(v->aux_int >= 0 && v->aux_int < XR_MAX_STRUCT_FIELDS,
                  "XI_STRUCT_SET: field_idx out of range");
        emit_inst(ctx, CREATE_ABC(OP_STRUCT_SET, obj, (uint8_t) v->aux_int, val));
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
        emit_inst(ctx, CREATE_ABC(OP_SETPROP, obj, (uint8_t) sym, val));
        if (v->id < ctx->reg_map_size)
            ctx->value_pc[v->id] = current_pc(ctx) - 1;
    }
}

/* Index get */
XR_FUNC void xi_emit_index_get(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t obj = reg_of(ctx, v->args[0]);
    uint8_t key = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_INDEX_GET, dst, obj, key));
}

/* Index set */
XR_FUNC void xi_emit_index_set(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void) dst;
    if (v->nargs < 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t obj = reg_of(ctx, v->args[0]);
    uint8_t key = reg_of(ctx, v->args[1]);
    uint8_t val = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_INDEX_SET, obj, key, val));
}

/* Array creation */
XR_FUNC void xi_emit_array_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    uint8_t cap = 0;
    if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
        cap = (uint8_t) v->args[0]->aux_int;
    }
    /* C = (elem_tid << 2) | storage_mode.
     * elem_tid is only set when the lowerer explicitly encodes it in
     * aux_int (e.g. new Array<T>()).  Array literals like [1,2,3] always
     * create XR_ELEM_ANY arrays because OP_NEWARRAY pushes B elements
     * from registers and typed storage crashes on uninitialized slots. */
    uint8_t c_field = (uint8_t) (v->aux_int & 0xFF);
    emit_inst(ctx, CREATE_ABC(OP_NEWARRAY, dst, cap, c_field));
}

/* Map creation */
XR_FUNC void xi_emit_map_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    uint8_t cap = 0;
    if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
        cap = (uint8_t) v->args[0]->aux_int;
    }
    /* C field pre-encoded by lowerer: (key_kind<<7)|(value_tid<<2)|flags */
    uint8_t c_field = (uint8_t) (v->aux_int & 0xFF);
    emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, cap, c_field));
}

/* Set creation */
XR_FUNC void xi_emit_set_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    /* B field pre-encoded by lowerer: (elem_tid<<2)|flags */
    uint8_t b_field = (uint8_t) (v->aux_int & 0xFF);
    emit_inst(ctx, CREATE_ABC(OP_NEWSET, dst, b_field, 0));
}

/* Json object creation: build Shape, store in constant pool, emit OP_NEWJSON */
XR_FUNC void xi_emit_json_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    int field_count = (int) v->aux_int;
    const char **field_names = (const char **) v->aux;

    if (!ctx->isolate) {
        /* No isolate: cannot build Shape, fall back to Map */
        emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, 0, 0));
        return;
    }
    if (field_count <= 0 || !field_names) {
        /* Empty Json object {} — create root Shape with no fields.
         * xr_shape_new requires capacity>=1, so pass 1 as min capacity. */
        XrShape *shape = xr_shape_new(ctx->isolate, 1);
        if (!shape) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        int kidx = add_const_int(ctx, (int64_t) (intptr_t) shape);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_NEWJSON, dst, (uint8_t) kidx, 0));
        return;
    }

    /* Build Shape via symbol table + xr_shape_build_fixed */
    XrSymbolTable *st = (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->isolate);
    XR_DCHECK(st != NULL, "isolate must have a symbol table");

    SymbolId symbols[32];
    int n = field_count > 32 ? 32 : field_count;
    for (int i = 0; i < n; i++) {
        symbols[i] = xr_symbol_register_in_table(st, field_names[i]);
    }

    XrShape *shape = xr_shape_build_fixed(ctx->isolate, symbols, (uint16_t) n);
    if (!shape) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* Store Shape pointer as integer constant in pool */
    int kidx = add_const_int(ctx, (int64_t) (intptr_t) shape);
    if (ctx->status != XI_EMIT_OK)
        return;

    emit_inst(ctx, CREATE_ABC(OP_NEWJSON, dst, (uint8_t) kidx, 0));
}

/* Json field init by index: OP_JSON_INIT A B C (A=json, B=field_idx, C=val) */
XR_FUNC void xi_emit_json_init_f(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void) dst; /* dst unused; this is a store op */
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t json_reg = reg_of(ctx, v->args[0]);
    uint8_t val_reg = reg_of(ctx, v->args[1]);
    int field_idx = (int) v->aux_int;
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_INIT, json_reg, (uint8_t) field_idx, val_reg));
}

/* Json field read by index: OP_JSON_GET A B C (A=dst, B=json, C=field_idx) */
XR_FUNC void xi_emit_json_get_f(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t json_reg = reg_of(ctx, v->args[0]);
    int field_idx = (int) v->aux_int;
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_GET, dst, json_reg, (uint8_t) field_idx));
}

/* Json field write by index: OP_JSON_SET A B C (A=json, B=field_idx, C=val) */
XR_FUNC void xi_emit_json_set_f(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void) dst; /* dst unused; this is a store op */
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t json_reg = reg_of(ctx, v->args[0]);
    uint8_t val_reg = reg_of(ctx, v->args[1]);
    int field_idx = (int) v->aux_int;
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_SET, json_reg, (uint8_t) field_idx, val_reg));
}

/* Typed JSON decode: OP_JSON_DECODE A B C
 * A=dst (result: T? sealed Json or null)
 * B=data register (string to parse)
 * C=Shape constant index (built from field names)
 *
 * Reuses the same Shape-building logic as xi_emit_json_new. */
XR_FUNC void xi_emit_json_decode(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    int n = (int) v->aux_int;
    const char **field_names = (const char **) v->aux;
    XR_DCHECK(n > 0 && field_names != NULL, "json_decode: no field info");

    uint8_t data_reg = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;

    /* Build Shape from field names (identical to json_new) */
    XrSymbolTable *st = (XrSymbolTable *) xr_isolate_get_symbol_table(ctx->isolate);
    XR_DCHECK(st != NULL, "json_decode: no symbol table");

    SymbolId symbols[256];
    XR_DCHECK(n <= 256, "json_decode: too many fields");
    for (int i = 0; i < n; i++) {
        symbols[i] = xr_symbol_register_in_table(st, field_names[i]);
    }

    XrShape *shape = xr_shape_build_fixed(ctx->isolate, symbols, (uint16_t) n);
    if (!shape) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* Propagate sealed flag from compile-time type */
    if (v->type && v->type->kind == XR_KIND_JSON && v->type->object.is_sealed) {
        shape->is_sealed = true;
    }

    int kidx = add_const_int(ctx, (int64_t) (intptr_t) shape);
    if (ctx->status != XI_EMIT_OK)
        return;

    emit_inst(ctx, CREATE_ABC(OP_JSON_DECODE, dst, data_reg, (uint8_t) kidx));
}

/* Range creation */
XR_FUNC void xi_emit_range(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t start = reg_of(ctx, v->args[0]);
    uint8_t end = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_NEWRANGE, dst, start, end));
}

/* Slice: OP_SLICE expects start at R[C], end at R[C+1] (consecutive) */
XR_FUNC void xi_emit_slice(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t src = reg_of(ctx, v->args[0]);
    uint8_t lo_src = reg_of(ctx, v->args[1]);
    uint8_t hi_src = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (ctx->next_reg + 2 >= MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    uint8_t lo_slot = ctx->next_reg;
    ctx->next_reg += 2;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    emit_inst(ctx, CREATE_ABC(OP_MOVE, lo_slot, lo_src, 0));
    emit_inst(ctx, CREATE_ABC(OP_MOVE, (uint8_t) (lo_slot + 1), hi_src, 0));
    emit_inst(ctx, CREATE_ABC(OP_SLICE, dst, src, lo_slot));
}

/* Closure creation */
XR_FUNC void xi_emit_closure_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    XiFunc *child_func = (XiFunc *) v->aux;
    XR_DCHECK(child_func != NULL, "closure child func must not be NULL");

    XrProto *child_proto = NULL;
    XiEmitStatus child_st = xi_emit(child_func, ctx->isolate, &child_proto);
    if (child_st != XI_EMIT_OK || !child_proto) {
        emit_error(ctx, child_st != XI_EMIT_OK ? child_st : XI_EMIT_ERR_INTERNAL);
        return;
    }
    child_proto->shared_offset = ctx->proto->shared_offset;

    /* Populate upvalue descriptors on child proto from captures */
    for (uint16_t ci = 0; ci < child_func->ncaptures; ci++) {
        XiCapture *cap = &child_func->captures[ci];
        uint8_t uv_index = 0;
        if (cap->source == XI_CAPTURE_SRC_REG) {
            XR_DCHECK(cap->value != NULL, "SRC_REG capture must have parent SSA value");
            uv_index = reg_of(ctx, cap->value);
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
            uint8_t reg = reg_of(ctx, cap->value);
            if (ctx->status != XI_EMIT_OK)
                return;
            bool already = ctx->cell_wrapped[cap->value->id];
            if (!already && cap->value->var_id != 0xFF)
                already = ctx->cell_created[cap->value->var_id];
            if (!already) {
                emit_inst(ctx, CREATE_ABC(OP_CELL_NEW, reg, 0, 0));
                ctx->cell_wrapped[cap->value->id] = true;
                if (cap->value->var_id != 0xFF) {
                    ctx->cell_side_reg[cap->value->var_id] = reg;
                    ctx->cell_created[cap->value->var_id] = true;
                }
            }
        }
    }

    /* Transfer Xi IR ownership to child proto for JIT direct lowering */
    child_proto->xi_func = child_func;
    uint16_t cidx = (uint16_t) v->aux_int;
    if (cidx < ctx->func->nchildren && ctx->func->children[cidx] == child_func) {
        ctx->func->children[cidx] = NULL;
    }

    int proto_idx = xr_vm_proto_add_proto(ctx->proto, child_proto);
    if (v->var_id != 0xFF && ctx->cell_side_reg[v->var_id] != NO_REG) {
        /* The destination register is cell-wrapped (hoisted function).
         * Emit closure to a temp register, then store into the cell. */
        uint8_t tmp = ctx->next_reg++;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
        emit_inst(ctx, CREATE_ABx(OP_CLOSURE, tmp, proto_idx));
        emit_inst(ctx, CREATE_ABC(OP_CELL_SET, dst, tmp, 0));
    } else {
        emit_inst(ctx, CREATE_ABx(OP_CLOSURE, dst, proto_idx));
    }
}

/* Upvalue load */
XR_FUNC void xi_emit_load_upval(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    int upval_idx = (int) v->aux_int;
    emit_inst(ctx, CREATE_ABC(OP_UPVAL_GET, dst, (uint8_t) upval_idx, 0));
    if (upval_idx < (int) ctx->func->ncaptures && ctx->func->captures[upval_idx].needs_cell) {
        emit_inst(ctx, CREATE_ABC(OP_CELL_GET, dst, dst, 0));
    }
}

/* Upvalue store */
XR_FUNC void xi_emit_store_upval(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t val = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int upval_idx = (int) v->aux_int;
    uint8_t cell_reg = dst;
    emit_inst(ctx, CREATE_ABC(OP_UPVAL_GET, cell_reg, (uint8_t) upval_idx, 0));
    emit_inst(ctx, CREATE_ABC(OP_CELL_SET, cell_reg, val, 0));
}

/* Shared (module-level) variable access */
XR_FUNC void xi_emit_get_shared(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    int shared_idx = (int) v->aux_int;
    emit_inst(ctx, CREATE_ABx(OP_GETSHARED, dst, shared_idx));
}

XR_FUNC void xi_emit_set_shared(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t val = reg_of(ctx, v->args[0]);
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
XR_FUNC void xi_emit_get_global(EmitCtx *ctx, XiValue *v, uint8_t dst) {
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

XR_FUNC void xi_emit_set_global(EmitCtx *ctx, XiValue *v, uint8_t dst) {
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
    uint8_t val = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int kx = add_const_string(ctx, name);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABx(OP_SETGLOBAL, val, kx));
}

/* Runtime global variable lookup */
XR_FUNC void xi_emit_get_builtin(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    emit_inst(ctx, CREATE_ABx(OP_GETBUILTIN, dst, (int) v->aux_int));
}

/* Iteration protocol */
XR_FUNC void xi_emit_iter(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint8_t obj = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;

    const char *method = v->op == XI_ITER_NEW     ? "iterator"
                         : v->op == XI_ITER_VALID ? "hasNext"
                                                  : "next";
    int sym = add_symbol(ctx, method);
    if (ctx->status != XI_EMIT_OK)
        return;

    uint8_t base = dst;
    if (obj != base + 1)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, base + 1, obj, 0));
    emit_inst(ctx, CREATE_ABC(OP_INVOKE, base, (uint8_t) sym, 1));
}

/* Class creation wrapper */
XR_FUNC void xi_emit_class_create(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    XiClassData *cdata = (XiClassData *) v->aux;
    XR_DCHECK(cdata != NULL && cdata->ast != NULL, "XI_CLASS_CREATE: missing class data");
    emit_class_create_impl(ctx, v, cdata, dst);
}

/* Module import */
XR_FUNC void xi_emit_import_ref(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    XiImportRef *ref = (XiImportRef *) v->aux;
    if (!ref || !ref->module_path || !ctx->isolate) {
        emit_inst(ctx, CREATE_ABx(OP_LOADNULL, dst, 0));
        return;
    }
    int mod_idx = add_const_string(ctx, ref->module_path);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABx(OP_IMPORT, dst, mod_idx));
    if (ref->member_name) {
        int sym_idx = add_symbol(ctx, ref->member_name);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_GETPROP, dst, dst, (uint8_t) sym_idx));
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
    child_proto->shared_offset = ctx->proto->shared_offset;

    for (uint16_t ui = 0; ui < child->ncaptures; ui++) {
        XiCapture *cap = &child->captures[ui];
        uint8_t uv_idx = 0;
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

    child_proto->xi_func = child;
    ctx->func->children[child_func_idx] = NULL;

    return xr_vm_proto_add_proto(ctx->proto, child_proto);
}

/* Build XrClassDescriptor from AST, emit child method protos,
 * and generate OP_CLASS_CREATE_FROM_DESCRIPTOR. */
static void emit_class_create_impl(EmitCtx *ctx, XiValue *v, XiClassData *cdata, uint8_t dst) {
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
    if (desc_idx < 0 || desc_idx > MAXARG_Bx) {
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
        uint8_t super_reg = reg_of(ctx, v->args[0]);
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
