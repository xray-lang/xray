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
static void emit_class_create_impl(EmitCtx *ctx, XiValue *v,
                                   XiClassData *cdata, uint8_t dst);

/* Field load: GETPROP or GETFIELD */
XR_FUNC void xi_emit_load_field(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t obj = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    const char *prop = (const char *)v->aux;
    if (prop) {
        int sym = add_symbol(ctx, prop);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_GETPROP, dst, obj, (uint8_t)sym));
        /* Record IC-relevant instruction offset for JIT */
        if (v->id < ctx->reg_map_size)
            ctx->value_pc[v->id] = current_pc(ctx) - 1;
    } else {
        emit_inst(ctx, CREATE_ABC(OP_GETFIELD, dst, obj, (uint8_t)v->aux_int));
    }
}

/* Field store: SETPROP or SETFIELD */
XR_FUNC void xi_emit_store_field(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t obj = reg_of(ctx, v->args[0]);
    uint8_t val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK) return;
    const char *prop = (const char *)v->aux;
    if (prop) {
        int sym = add_symbol(ctx, prop);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_SETPROP, obj, (uint8_t)sym, val));
        if (v->id < ctx->reg_map_size)
            ctx->value_pc[v->id] = current_pc(ctx) - 1;
    } else {
        emit_inst(ctx, CREATE_ABC(OP_SETFIELD, obj, (uint8_t)v->aux_int, val));
    }
}

/* Index get */
XR_FUNC void xi_emit_index_get(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t obj = reg_of(ctx, v->args[0]);
    uint8_t key = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_INDEX_GET, dst, obj, key));
}

/* Index set */
XR_FUNC void xi_emit_index_set(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 3) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t obj = reg_of(ctx, v->args[0]);
    uint8_t key = reg_of(ctx, v->args[1]);
    uint8_t val = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_INDEX_SET, obj, key, val));
}

/* Array creation */
XR_FUNC void xi_emit_array_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    uint8_t cap = 0;
    if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
        cap = (uint8_t)v->args[0]->aux_int;
    }
    emit_inst(ctx, CREATE_ABC(OP_NEWARRAY, dst, cap, 0));
}

/* Map creation */
XR_FUNC void xi_emit_map_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    uint8_t cap = 0;
    if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
        cap = (uint8_t)v->args[0]->aux_int;
    }
    uint8_t c_field = (uint8_t)(v->aux_int & 0xFF);
    emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, cap, c_field));
}

/* Set creation */
XR_FUNC void xi_emit_set_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    uint8_t b_field = (uint8_t)(v->aux_int & 0xFF);
    emit_inst(ctx, CREATE_ABC(OP_NEWSET, dst, b_field, 0));
}

/* Json object creation: build Shape, store in constant pool, emit OP_NEWJSON */
XR_FUNC void xi_emit_json_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    int field_count = (int)v->aux_int;
    const char **field_names = (const char **)v->aux;

    if (!ctx->isolate || field_count <= 0 || !field_names) {
        /* Fallback: zero-field Json (bare OP_NEWMAP) */
        emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, 0, 0));
        return;
    }

    /* Build Shape via symbol table + xr_shape_build_fixed */
    XrSymbolTable *st = (XrSymbolTable *)xr_isolate_get_symbol_table(ctx->isolate);
    XR_DCHECK(st != NULL, "isolate must have a symbol table");

    SymbolId symbols[32];
    int n = field_count > 32 ? 32 : field_count;
    for (int i = 0; i < n; i++) {
        symbols[i] = xr_symbol_register_in_table(st, field_names[i]);
    }

    XrShape *shape = xr_shape_build_fixed(ctx->isolate, symbols, (uint16_t)n);
    if (!shape) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* Store Shape pointer as integer constant in pool */
    int kidx = add_const_int(ctx, (int64_t)(intptr_t)shape);
    if (ctx->status != XI_EMIT_OK) return;

    emit_inst(ctx, CREATE_ABC(OP_NEWJSON, dst, (uint8_t)kidx, 0));
}

/* Json field init by index: OP_JSON_INIT A B C (A=json, B=field_idx, C=val) */
XR_FUNC void xi_emit_json_init_f(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;  /* dst unused; this is a store op */
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t json_reg = reg_of(ctx, v->args[0]);
    uint8_t val_reg = reg_of(ctx, v->args[1]);
    int field_idx = (int)v->aux_int;
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_INIT, json_reg, (uint8_t)field_idx, val_reg));
}

/* Json field read by index: OP_JSON_GET A B C (A=dst, B=json, C=field_idx) */
XR_FUNC void xi_emit_json_get_f(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t json_reg = reg_of(ctx, v->args[0]);
    int field_idx = (int)v->aux_int;
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_GET, dst, json_reg, (uint8_t)field_idx));
}

/* Json field write by index: OP_JSON_SET A B C (A=json, B=field_idx, C=val) */
XR_FUNC void xi_emit_json_set_f(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;  /* dst unused; this is a store op */
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t json_reg = reg_of(ctx, v->args[0]);
    uint8_t val_reg = reg_of(ctx, v->args[1]);
    int field_idx = (int)v->aux_int;
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_JSON_SET, json_reg, (uint8_t)field_idx, val_reg));
}

/* Range creation */
XR_FUNC void xi_emit_range(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t start = reg_of(ctx, v->args[0]);
    uint8_t end = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_NEWRANGE, dst, start, end));
}

/* Slice: OP_SLICE expects start at R[C], end at R[C+1] (consecutive) */
XR_FUNC void xi_emit_slice(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 3) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src    = reg_of(ctx, v->args[0]);
    uint8_t lo_src = reg_of(ctx, v->args[1]);
    uint8_t hi_src = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK) return;
    if (ctx->next_reg + 2 >= MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS); return;
    }
    uint8_t lo_slot = ctx->next_reg;
    ctx->next_reg += 2;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    emit_inst(ctx, CREATE_ABC(OP_MOVE, lo_slot, lo_src, 0));
    emit_inst(ctx, CREATE_ABC(OP_MOVE, (uint8_t)(lo_slot + 1), hi_src, 0));
    emit_inst(ctx, CREATE_ABC(OP_SLICE, dst, src, lo_slot));
}

/* Closure creation */
XR_FUNC void xi_emit_closure_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    XiFunc *child_func = (XiFunc *)v->aux;
    XR_DCHECK(child_func != NULL, "closure child func must not be NULL");

    XrProto *child_proto = NULL;
    XiEmitStatus child_st = xi_emit(child_func, ctx->isolate, &child_proto);
    if (child_st != XI_EMIT_OK || !child_proto) {
        emit_error(ctx, child_st != XI_EMIT_OK
                   ? child_st : XI_EMIT_ERR_INTERNAL);
        return;
    }
    child_proto->shared_offset = ctx->proto->shared_offset;

    /* Populate upvalue descriptors on child proto from captures */
    for (uint16_t ci = 0; ci < child_func->ncaptures; ci++) {
        XiCapture *cap = &child_func->captures[ci];
        uint8_t uv_index = 0;
        if (cap->source == XI_CAPTURE_SRC_REG) {
            XR_DCHECK(cap->value != NULL,
                      "SRC_REG capture must have parent SSA value");
            uv_index = reg_of(ctx, cap->value);
            if (ctx->status != XI_EMIT_OK) return;
        } else {
            uv_index = cap->index;
        }
        xr_vm_proto_add_upvalue(child_proto, uv_index,
                                 0, 0, 0, cap->source, cap->type);
    }

    /* Cell wrapping for mutable captures (emit once per value) */
    for (uint16_t ci = 0; ci < child_func->ncaptures; ci++) {
        XiCapture *cap = &child_func->captures[ci];
        if (cap->needs_cell && cap->source == XI_CAPTURE_SRC_REG) {
            uint8_t reg = reg_of(ctx, cap->value);
            if (ctx->status != XI_EMIT_OK) return;
            if (!ctx->cell_wrapped[cap->value->id]) {
                emit_inst(ctx, CREATE_ABC(OP_CELL_NEW, reg, 0, 0));
                ctx->cell_wrapped[cap->value->id] = true;
            }
        }
    }

    /* Transfer Xi IR ownership to child proto for JIT direct lowering */
    child_proto->xi_func = child_func;
    uint16_t cidx = (uint16_t)v->aux_int;
    if (cidx < ctx->func->nchildren &&
        ctx->func->children[cidx] == child_func) {
        ctx->func->children[cidx] = NULL;
    }

    int proto_idx = xr_vm_proto_add_proto(ctx->proto, child_proto);
    emit_inst(ctx, CREATE_ABx(OP_CLOSURE, dst, proto_idx));
}

/* Upvalue load */
XR_FUNC void xi_emit_load_upval(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    int upval_idx = (int)v->aux_int;
    emit_inst(ctx, CREATE_ABC(OP_UPVAL_GET, dst, (uint8_t)upval_idx, 0));
    if (upval_idx < (int)ctx->func->ncaptures &&
        ctx->func->captures[upval_idx].needs_cell) {
        emit_inst(ctx, CREATE_ABC(OP_CELL_GET, dst, dst, 0));
    }
}

/* Upvalue store */
XR_FUNC void xi_emit_store_upval(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t val = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    int upval_idx = (int)v->aux_int;
    uint8_t cell_reg = dst;
    emit_inst(ctx, CREATE_ABC(OP_UPVAL_GET, cell_reg,
                               (uint8_t)upval_idx, 0));
    emit_inst(ctx, CREATE_ABC(OP_CELL_SET, cell_reg, val, 0));
}

/* Shared (module-level) variable access */
XR_FUNC void xi_emit_get_shared(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    int shared_idx = (int)v->aux_int;
    emit_inst(ctx, CREATE_ABx(OP_GETSHARED, dst, shared_idx));
}

XR_FUNC void xi_emit_set_shared(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t val = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    int shared_idx = (int)v->aux_int;
    emit_inst(ctx, CREATE_ABx(OP_SETSHARED, val, shared_idx));
}

/* Runtime global variable lookup */
XR_FUNC void xi_emit_get_builtin(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    emit_inst(ctx, CREATE_ABx(OP_GETBUILTIN, dst, (int)v->aux_int));
}

/* Iteration protocol */
XR_FUNC void xi_emit_iter(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t obj = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;

    const char *method =
        v->op == XI_ITER_NEW   ? "iterator" :
        v->op == XI_ITER_VALID ? "hasNext"  : "next";
    int sym = add_symbol(ctx, method);
    if (ctx->status != XI_EMIT_OK) return;

    uint8_t base = dst;
    if (obj != base + 1)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, base + 1, obj, 0));
    emit_inst(ctx, CREATE_ABC(OP_INVOKE, base, (uint8_t)sym, 1));
}

/* Class creation wrapper */
XR_FUNC void xi_emit_class_create(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    XiClassData *cdata = (XiClassData *)v->aux;
    XR_DCHECK(cdata != NULL && cdata->ast != NULL,
              "XI_CLASS_CREATE: missing class data");
    emit_class_create_impl(ctx, v, cdata, dst);
}

/* Module import */
XR_FUNC void xi_emit_import_ref(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    XiImportRef *ref = (XiImportRef *)v->aux;
    if (!ref || !ref->module_path || !ctx->isolate) {
        emit_inst(ctx, CREATE_ABx(OP_LOADNULL, dst, 0));
        return;
    }
    int mod_idx = add_const_string(ctx, ref->module_path);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABx(OP_IMPORT, dst, mod_idx));
    if (ref->member_name) {
        int sym_idx = add_symbol(ctx, ref->member_name);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_GETPROP, dst, dst,
                                  (uint8_t)sym_idx));
    }
}

/* ========== Class Emission Helpers ========== */

/* Convert an AST literal to XrValue for field default values. */
static XrValue ast_field_default_to_value(EmitCtx *ctx, AstNode *init) {
    if (!init) return xr_null();
    if (init->type == AST_LITERAL_INT)
        return xr_int(init->as.literal.raw_value.int_val);
    if (init->type == AST_LITERAL_FLOAT)
        return xr_float(init->as.literal.raw_value.float_val);
    if (init->type == AST_LITERAL_TRUE)  return xr_bool(true);
    if (init->type == AST_LITERAL_FALSE) return xr_bool(false);
    if (init->type == AST_LITERAL_STRING && ctx->isolate) {
        const char *s = init->as.literal.raw_value.string_val;
        if (s) {
            XrString *xs = xr_compile_time_intern(ctx->isolate, s, strlen(s));
            if (xs) return xr_string_value(xs);
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
        if (cd->fields[i]->type == AST_FIELD_DECL
            && !cd->fields[i]->as.field_decl.is_static)
            fc++;
    if (fc > 0) {
        desc->instance_fields = (XrFieldDescriptorEntry *)xr_calloc(
            fc, sizeof(XrFieldDescriptorEntry));
        if (!desc->instance_fields) return false;
        desc->instance_field_count = fc;
        uint32_t idx = 0;
        for (int i = 0; i < cd->field_count; i++) {
            if (cd->fields[i]->type != AST_FIELD_DECL) continue;
            FieldDeclNode *f = &cd->fields[i]->as.field_decl;
            if (f->is_static) continue;
            desc->instance_fields[idx].name = strdup(f->name);
            desc->instance_fields[idx].type_name =
                f->field_type ? xr_type_to_string(
                    xr_tref_resolve(ctx->isolate, f->field_type)) : NULL;
            desc->instance_fields[idx].default_value =
                ast_field_default_to_value(ctx, f->initializer);
            if (f->is_private) desc->instance_fields[idx].flags |= XR_FIELD_PRIVATE;
            if (f->is_final)   desc->instance_fields[idx].flags |= XR_FIELD_FINAL;
            idx++;
        }
    }
    /* Static fields */
    uint32_t sfc = 0;
    for (int i = 0; i < cd->field_count; i++)
        if (cd->fields[i]->type == AST_FIELD_DECL
            && cd->fields[i]->as.field_decl.is_static)
            sfc++;
    if (sfc > 0) {
        desc->static_fields = (XrFieldDescriptorEntry *)xr_calloc(
            sfc, sizeof(XrFieldDescriptorEntry));
        if (!desc->static_fields) return false;
        desc->static_field_count = sfc;
        uint32_t idx = 0;
        for (int i = 0; i < cd->field_count; i++) {
            if (cd->fields[i]->type != AST_FIELD_DECL) continue;
            FieldDeclNode *f = &cd->fields[i]->as.field_decl;
            if (!f->is_static) continue;
            desc->static_fields[idx].name = strdup(f->name);
            desc->static_fields[idx].flags = XR_FIELD_STATIC;
            desc->static_fields[idx].default_value = xr_null();
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
    if (cst != XI_EMIT_OK || !child_proto) return -1;
    child_proto->shared_offset = ctx->proto->shared_offset;

    for (uint16_t ui = 0; ui < child->ncaptures; ui++) {
        XiCapture *cap = &child->captures[ui];
        uint8_t uv_idx = 0;
        if (cap->source == XI_CAPTURE_SRC_REG) {
            XR_DCHECK(cap->value != NULL,
                      "SRC_REG capture must have parent SSA value");
            uv_idx = reg_of(ctx, cap->value);
            if (ctx->status != XI_EMIT_OK) return -1;
        } else {
            uv_idx = cap->index;
        }
        xr_vm_proto_add_upvalue(child_proto, uv_idx,
                                 0, 0, 0, cap->source, cap->type);
    }

    child_proto->xi_func = child;
    ctx->func->children[child_func_idx] = NULL;

    return xr_vm_proto_add_proto(ctx->proto, child_proto);
}

/* Build XrClassDescriptor from AST, emit child method protos,
 * and generate OP_CLASS_CREATE_FROM_DESCRIPTOR. */
static void emit_class_create_impl(EmitCtx *ctx, XiValue *v,
                                   XiClassData *cdata, uint8_t dst) {
    (void)v;
    ClassDeclNode *cd = &cdata->ast->as.class_decl;

    XrClassDescriptor *desc = (XrClassDescriptor *)xr_calloc(
        1, sizeof(XrClassDescriptor));
    if (!desc) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

    desc->class_name = strdup(cd->name);
    desc->super_name = (cd->super_name && !cd->super_module)
                       ? strdup(cd->super_name) : NULL;
    desc->super_global_index = -1;
    desc->descriptor_version = XR_CLASS_DESCRIPTOR_VERSION;
    desc->clinit_proto_index = -1;
    if (cd->is_abstract) desc->flags |= XR_CLASS_ABSTRACT;
    if (cd->is_final)    desc->flags |= XR_CLASS_FINAL;

    if (!emit_class_collect_fields_impl(ctx, cd, desc)) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* Instance methods */
    if (cdata->ninst > 0) {
        desc->instance_methods = (XrMethodDescriptorEntry *)xr_calloc(
            cdata->ninst, sizeof(XrMethodDescriptorEntry));
        if (!desc->instance_methods) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        desc->instance_method_count = cdata->ninst;
        uint16_t mi = 0;
        for (int i = 0; i < cd->method_count && mi < cdata->ninst; i++) {
            if (cd->methods[i]->type != AST_METHOD_DECL) continue;
            MethodDeclNode *m = &cd->methods[i]->as.method_decl;
            if (m->is_static || m->is_static_constructor) continue;
            desc->instance_methods[mi].name = strdup(m->name);
            desc->instance_methods[mi].param_count = m->param_count;
            if (m->is_constructor || strcmp(m->name, "constructor") == 0
                || strcmp(m->name, "init") == 0)
                desc->instance_methods[mi].flags |= XMETHOD_FLAG_CONSTRUCTOR;
            if (m->is_private)  desc->instance_methods[mi].flags |= XMETHOD_FLAG_PRIVATE;
            if (m->is_abstract) desc->instance_methods[mi].flags |= XMETHOD_FLAG_ABSTRACT;
            if (m->is_final)    desc->instance_methods[mi].flags |= XMETHOD_FLAG_FINAL;
            XR_DCHECK(cdata->child_idx != NULL, "child_idx must be set");
            int pi = emit_method_proto_impl(ctx, cdata->child_idx[mi]);
            if (pi < 0) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            desc->instance_methods[mi].closure_index = (uint32_t)pi;
            mi++;
        }
    }

    /* Static methods */
    if (cdata->nstat > 0) {
        desc->static_methods = (XrMethodDescriptorEntry *)xr_calloc(
            cdata->nstat, sizeof(XrMethodDescriptorEntry));
        if (!desc->static_methods) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        desc->static_method_count = cdata->nstat;
        uint16_t mi = 0, off = cdata->ninst;
        for (int i = 0; i < cd->method_count && mi < cdata->nstat; i++) {
            if (cd->methods[i]->type != AST_METHOD_DECL) continue;
            MethodDeclNode *m = &cd->methods[i]->as.method_decl;
            if (!m->is_static || m->is_static_constructor) continue;
            desc->static_methods[mi].name = strdup(m->name);
            desc->static_methods[mi].param_count = m->param_count;
            desc->static_methods[mi].flags = XMETHOD_FLAG_STATIC;
            XR_DCHECK(cdata->child_idx != NULL, "child_idx must be set");
            int pi = emit_method_proto_impl(ctx, cdata->child_idx[off + mi]);
            if (pi < 0) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            desc->static_methods[mi].closure_index = (uint32_t)pi;
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
    emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
    emit_inst(ctx, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, dst, desc_idx));
}
